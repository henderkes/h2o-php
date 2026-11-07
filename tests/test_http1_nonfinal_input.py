"""Nonfinal H1 ownership: partial PHP reads, suffixes, cancellation and pressure."""
import os
from pathlib import Path
import socket
import struct
import time
import unittest

import test_http1_body_reuse as reuse


class NonfinalInputTest(reuse.HTTP1BodyReuseTest):
    body_limit = 1048576
    global_limit = 8388608

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
        # Match the existing qualification fixture's PHP source-age contract.
        time.sleep(3.11)
        self.quiescent()
        time.sleep(.02)
        self.fd_baseline = len(self.fd_snapshot())

    def fd_snapshot(self):
        result = {}
        for path in Path(f'/proc/{self.process.pid}/fd').iterdir():
            try:
                result[path.name] = os.readlink(path)
            except FileNotFoundError:
                pass
        return result

    def drained(self):
        self.assertEqual(self.quiescent()['buffered_bytes'], 0)
        deadline = time.monotonic() + 3
        while len(self.fd_snapshot()) > self.fd_baseline:
            self.assertIsNone(self.process.poll(), self.log_path.read_text())
            self.assertLess(time.monotonic(), deadline, self.fd_snapshot())
            time.sleep(.005)

    def begin_partial(self, connection, token, *, finish=False):
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
        # PHP consumed only 113 bytes while the upload is still incomplete.
        return self.base / ('release-' + token)

    def test_nonfinal_partial_reads_then_pipeline_and_early_finish(self):
        for finish in (False, True):
            token = 'finish' if finish else 'partial'
            gate = self.base / ('release-' + token)
            try:
                with self.raw_connection(False) as connection:
                    self.begin_partial(connection, token, finish=finish)
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
            self.drained()

    def test_small_get_then_nonfinal_upload_and_pipeline(self):
        token = 'after-small-get'
        gate = self.base / ('release-' + token)
        before = self.quiescent()
        try:
            with self.raw_connection(False) as connection, connection.makefile('rb') as stream:
                connection.sendall(b'GET /read.php?id=small HTTP/1.1\r\nHost: reuse.test\r\n\r\n')
                self.verify(self.read_fixed_response(stream), 'small', b'')
                self.begin_partial(connection, token)
                connection.sendall(reuse.LARGE[786432:] +
                    b'GET /read.php?id=next HTTP/1.1\r\nHost: reuse.test\r\nConnection: close\r\n\r\n')
                gate.touch()
                self.verify(self.read_fixed_response(stream), token, reuse.LARGE)
                self.verify(self.read_fixed_response(stream), 'next', b'')
                self.assertEqual(stream.read(1), b'')
        finally:
            gate.touch()
        self.drained()
        after = self.quiescent()
        self.assertEqual({key: after[key] - before[key] for key in ('completed', 'failed', 'timed_out')},
                         {'completed': 3, 'failed': 0, 'timed_out': 0})

    def test_nonfinal_partial_cancel_then_credit_and_fd_drain(self):
        for index in range(3):
            token = f'cancel-{index}'
            gate = self.base / ('release-' + token)
            try:
                with self.raw_connection(False) as connection:
                    self.begin_partial(connection, token)
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            finally:
                gate.touch()
            self.drained()
        connection = self.connection(False)
        try:
            self.send(connection, 'recovery', reuse.LARGE)
        finally:
            connection.close()
        self.drained()


class NonfinalInputTightBudgetTest(NonfinalInputTest):
    # Match the existing ownership regression's tight global budget. The C
    # unit separately proves exact-length fallback at 1 MiB + one quantum.
    global_limit = 2097152


def load_tests(loader, tests, pattern):
    names = ('test_nonfinal_partial_reads_then_pipeline_and_early_finish',
             'test_nonfinal_partial_cancel_then_credit_and_fd_drain',
             'test_small_get_then_nonfinal_upload_and_pipeline')
    return unittest.TestSuite(cls(name) for cls in (NonfinalInputTest, NonfinalInputTightBudgetTest)
                              for name in names)


if __name__ == '__main__':
    unittest.main()
