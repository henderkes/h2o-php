"""Tight input-copy capacity: partial PUT reads, pipelining and early finish."""
import time
import unittest

import test_http1_body_reuse as reuse


class HTTP1TightPipelineTest(reuse.HTTP1BodyReuseTest):
    body_limit = 1048576
    global_limit = 1048576 + 16384

    def setUp(self):
        super().setUp()
        (self.base / 'partial.php').write_text(r'''<?php
$stream = fopen('php://input', 'rb');
$first = fread($stream, 113);
if (strlen($first) !== 113) throw new RuntimeException('short prefix');
$token = $_GET['token'];
file_put_contents(__DIR__.'/entered-'.$token, 'ready');
while (!file_exists(__DIR__.'/release-'.$token)) { clearstatcache(); usleep(1000); }
if (isset($_GET['finish'])) { header('Content-Length: 2'); echo 'ok'; return; }
$hash = hash_init('sha256'); hash_update($hash, $first); $length = strlen($first);
while (!feof($stream)) {
    $part = fread($stream, 113);
    if ($part === false) throw new RuntimeException('body read failed');
    $length += strlen($part); hash_update($hash, $part);
}
$extra = fread($stream, 1);
$result = json_encode(['id' => $token, 'length' => $length,
    'sha256' => hash_final($hash), 'eof' => feof($stream), 'extra_length' => strlen($extra)]);
header('Content-Length: '.strlen($result)); echo $result;
''')
        time.sleep(3.11)
        self.quiescent()

    def test_partial_put_tail_then_pipeline_and_early_finish(self):
        before = self.quiescent()
        for finish in (False, True):
            token = 'finish' if finish else 'partial'
            gate = self.base / ('release-' + token)
            try:
                with self.raw_connection(False) as connection:
                    path = '/partial.php?token=' + token + ('&finish=1' if finish else '')
                    connection.sendall((f'PUT {path} HTTP/1.1\r\nHost: reuse.test\r\n'
                        'Content-Type: application/octet-stream\r\nContent-Length: 1048576\r\n\r\n').encode()
                        + reuse.LARGE[:786432])
                    marker = self.base / ('entered-' + token)
                    deadline = time.monotonic() + 2
                    while not marker.exists() or self.metrics()['buffered_bytes'] < 262144:
                        self.assertIsNone(self.process.poll(), self.log_path.read_text())
                        self.assertLess(time.monotonic(), deadline, self.metrics())
                        time.sleep(.001)
                    connection.sendall(reuse.LARGE[786432:] +
                        b'GET /read.php?id=next HTTP/1.1\r\nHost: reuse.test\r\nConnection: close\r\n\r\n')
                    gate.touch()
                    with connection.makefile('rb') as stream:
                        body = self.read_fixed_response(stream)
                        if finish:
                            self.assertEqual(body, b'ok')
                        else:
                            self.verify(body, token, reuse.LARGE)
                        self.verify(self.read_fixed_response(stream), 'next', b'')
                        self.assertEqual(stream.read(1), b'')
            finally:
                gate.touch()
            self.quiescent()
        after = self.quiescent()
        self.assertEqual({key: after[key] - before[key] for key in ('completed', 'failed', 'timed_out')},
                         {'completed': 4, 'failed': 0, 'timed_out': 0})


def load_tests(loader, tests, pattern):
    return unittest.TestSuite([HTTP1TightPipelineTest('test_partial_put_tail_then_pipeline_and_early_finish')])


if __name__ == '__main__':
    unittest.main()
