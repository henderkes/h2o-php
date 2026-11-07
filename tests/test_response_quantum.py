"""Response credits, concurrent writers and gzip: python3 tests/test_response_quantum.py [binary]."""
from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
import http.client
import itertools
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else 'build/release/h2o-php').resolve()
LARGE = bytes(range(256)) * 4096
SMALL = b'\x00small-after-output\xff\n'
SEGMENTS = (b'A' * 131071, b'B' * 196613, b'C' * 262151)
PREFIX, SUFFIX = b'parallel-start\n', b'parallel-end\n'
PARALLEL_BODIES = {PREFIX + b''.join(parts) + SUFFIX for parts in itertools.permutations(SEGMENTS)}
TEXT = b'Response quantum filter row 0123456789 abcdef\n' * 32768


class ResponseQuantumTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='h2o-php-response-quantum-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.process = self.log = None
        self.addCleanup(self.stop)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); self.port = listener.getsockname()[1]
        self.config = self.base / 'server.yml'
        self.log_path = self.base / 'server.log'
        (self.base / 'large.php').write_text(r'''<?php
$pattern = ''; for ($i = 0; $i < 256; ++$i) $pattern .= chr($i);
$body = str_repeat($pattern, 4096);
header('Content-Type: application/octet-stream'); header('Content-Length: '.strlen($body));
echo $body; flush();
''')
        (self.base / 'huge.php').write_text(r'''<?php
header('Content-Type: application/octet-stream'); header('Content-Length: 8388608');
echo str_repeat('H', 8388608); flush();
''')
        (self.base / 'small.php').write_text(r'''<?php
$body = "\x00small-after-output\xff\n";
header('Content-Type: application/octet-stream'); header('Content-Length: '.strlen($body)); echo $body;
''')
        (self.base / 'empty.php').write_text("<?php header('Content-Length: 0');\n")
        (self.base / 'compressed').mkdir()
        (self.base / 'compressed/index.php').write_text(r'''<?php
$body = str_repeat("Response quantum filter row 0123456789 abcdef\n", 32768);
header('Content-Type: text/plain'); header('Content-Length: '.strlen($body)); echo $body; flush();
''')
        (self.base / 'write-before-read.php').write_text(r'''<?php
$key = $_GET['key'] ?? '';
if (!preg_match('/^[a-f0-9]{32}$/D', $key)) throw new RuntimeException('bad private input gate key');
touch(__DIR__.'/'.$key.'-entered'); $end = microtime(true) + 5;
do {
    clearstatcache(true, __DIR__.'/'.$key.'-release');
    if (is_file(__DIR__.'/'.$key.'-release')) break;
    if (microtime(true) >= $end) throw new RuntimeException('private input gate timed out');
    usleep(1000);
} while (true);
header('Content-Type: application/octet-stream');
echo str_repeat('R', 65536); flush();
$input = file_get_contents('php://input');
echo json_encode(['length' => strlen($input), 'sha256' => hash('sha256', $input)], JSON_THROW_ON_ERROR)."\n";
''')
        (self.base / 'parallel.php').write_text(r'''<?php
$key = $_GET['key'] ?? '';
if (!preg_match('/^[a-f0-9]{32}$/D', $key)) throw new RuntimeException('bad private writer key');
$gate = __DIR__.'/'.$key; $runtimes = []; $futures = [];
header('Content-Type: application/octet-stream');
header('Content-Length: '.(strlen("parallel-start\n") + 131071 + 196613 + 262151 + strlen("parallel-end\n")));
echo "parallel-start\n"; flush();
try {
    foreach ([['A', 131071], ['B', 196613]] as [$letter, $length]) {
        $runtime = new parallel\Runtime(); $runtimes[] = $runtime;
        $futures[] = $runtime->run(static function (string $gate, string $letter, int $length): array {
            touch($gate.'-'.$letter); $end = microtime(true) + 5;
            do {
                clearstatcache(true, $gate.'-release');
                if (is_file($gate.'-release')) break;
                if (microtime(true) >= $end) throw new RuntimeException('bounded writer gate timed out');
                usleep(1000);
            } while (true);
            $body = str_repeat($letter, $length); echo $body; flush();
            return ['length' => strlen($body), 'sha256' => hash('sha256', $body)];
        }, [$gate, $letter, $length]);
    }
    $end = microtime(true) + 5;
    do {
        clearstatcache(); if (is_file($gate.'-A') && is_file($gate.'-B')) break;
        if (microtime(true) >= $end) throw new RuntimeException('both parallel writers did not enter');
        usleep(1000);
    } while (true);
    touch($gate.'-release'); echo str_repeat('C', 262151); flush();
    foreach ($futures as $index => $future) {
        $letter = $index ? 'B' : 'A'; $length = $index ? 196613 : 131071;
        if ($future->value() !== ['length' => $length, 'sha256' => hash('sha256', str_repeat($letter, $length))])
            throw new RuntimeException('wrong parallel writer value');
    }
    foreach ($runtimes as $runtime) $runtime->close(); $runtimes = [];
    echo "parallel-end\n";
} finally {
    touch($gate.'-release');
    foreach ($runtimes as $runtime) try { $runtime->kill(); } catch (Throwable $ignored) {}
}
''')

    def start(self, budget):
        self.budget = budget
        self.config.write_text(f'''listen:
  host: 127.0.0.1
  port: {self.port}
num-threads: 2
php.runtime:
  threads: 4
  max-threads: 4
  collect-metrics: OFF
  request-timeout: 8s
  shutdown-timeout: 2s
  max-buffered-body: 4096
  max-buffered-response: 131072
  max-buffered-global: {budget}
php.root: {self.base}
php.ini:
  output_buffering: "0"
  zlib.output_compression: "0"
  ignore_user_abort: "0"
  display_errors: "0"
hosts:
  quantum.test:
    paths:
      /metrics:
        php.metrics: ON
      /compressed/:
        php.root: {self.base / 'compressed'}
        compress: ON
        php_server: ON
      /:
        php_server: ON
''')
        effective = subprocess.run([str(BINARY), 'effective-config', '-c', str(self.config)], capture_output=True, text=True, timeout=10)
        self.assertEqual(effective.returncode, 0, effective.stderr)
        config = json.loads(effective.stdout)
        self.assertFalse(config['php_engine_started'])
        self.assertEqual(config['runtime']['max_buffered_global'], budget)
        self.assertEqual(config['runtime']['max_buffered_response'], 131072)
        self.assertFalse(config['runtime']['collect_metrics'])
        self.log = self.log_path.open('w')
        self.process = subprocess.Popen([str(BINARY), '-c', str(self.config)], stdout=self.log, stderr=subprocess.STDOUT)
        end = time.monotonic() + 8
        while time.monotonic() < end:
            self.assertIsNone(self.process.poll(), self.log_path.read_text())
            try:
                self.quiescent(); return
            except (OSError, http.client.HTTPException): time.sleep(.01)
        self.fail('response test server did not start:\n' + self.log_path.read_text())

    def metrics(self):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=3)
        try:
            connection.request('GET', '/metrics', headers={'Host': 'quantum.test'})
            response = connection.getresponse(); body = response.read()
            self.assertEqual(response.status, 200, body)
            value = json.loads(body)
            self.assertGreaterEqual(value['buffered_bytes'], 0)
            self.assertLessEqual(value['buffered_bytes'], self.budget, value)
            return value
        finally: connection.close()

    def quiescent(self):
        end = time.monotonic() + 4
        while True:
            value = self.metrics()
            if all(value[key] == 0 for key in ('active_php_threads', 'busy_workers', 'queued_requests', 'buffered_bytes', 'auxiliary_threads')):
                return value
            self.assertLess(time.monotonic(), end, value); time.sleep(.01)

    def useful_connection(self, barrier=None):
        if barrier: barrier.wait(timeout=4)
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=14)
        first = None
        try:
            paths = [('/large.php', LARGE), ('/small.php', SMALL), ('/parallel.php?key=' + uuid.uuid4().hex, None),
                     ('/empty.php', b''), ('/small.php', SMALL)]
            for path, expected in paths:
                connection.request('GET', path, headers={'Host': 'quantum.test', 'Accept-Encoding': 'identity'})
                if first is None: first = connection.sock
                self.assertIs(connection.sock, first, 'client reopened a connection')
                response = connection.getresponse(); body = response.read()
                self.assertEqual(response.status, 200, (path, body[:1000], self.log_path.read_text()))
                self.assertFalse(response.will_close)
                self.assertEqual(response.getheader('Content-Length'), str(len(body)))
                self.assertIsNone(response.getheader('Content-Encoding'))
                if expected is None:
                    self.assertTrue(body in PARALLEL_BODIES, ('wrong parallel framing/ownership', len(body), hashlib.sha256(body).hexdigest()))
                else:
                    self.assertEqual(body, expected)
        finally: connection.close()

    def concurrent_wave(self, count, callback):
        barrier = threading.Barrier(count)
        with ThreadPoolExecutor(max_workers=count) as pool:
            futures = [pool.submit(callback, barrier) for _ in range(count)]
            while any(not future.done() for future in futures):
                self.metrics(); time.sleep(.002)
            for future in futures: future.result()
        self.quiescent()

    def cancelled_credit(self):
        connection = socket.socket(); connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        connection.settimeout(3); connection.connect(('127.0.0.1', self.port))
        try:
            connection.sendall(b'GET /huge.php HTTP/1.1\r\nHost: quantum.test\r\nAccept-Encoding: identity\r\n\r\n')
            end = time.monotonic() + 3
            while True:
                values = self.metrics()
                if values['active_php_threads'] == 1 and values['buffered_bytes'] > 0: break
                self.assertLess(time.monotonic(), end, values); time.sleep(.005)
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        finally: connection.close()
        self.quiescent()

    def budget_case(self, budget):
        self.start(budget); before = self.quiescent()
        self.concurrent_wave(1, self.useful_connection)
        self.concurrent_wave(8, self.useful_connection)
        after_useful = self.quiescent()
        self.assertEqual({key: after_useful[key] - before[key] for key in ('completed', 'failed', 'timed_out')},
                         {'completed': 45, 'failed': 0, 'timed_out': 0})
        self.cancelled_credit()
        after_cancel = self.quiescent()
        self.assertEqual({key: after_cancel[key] - after_useful[key] for key in ('completed', 'failed', 'timed_out')},
                         {'completed': 1, 'failed': 1, 'timed_out': 0})
        self.useful_connection(); after = self.quiescent()
        self.assertEqual({key: after[key] - before[key] for key in ('completed', 'failed', 'timed_out')},
                         {'completed': 51, 'failed': 1, 'timed_out': 0})
        self.assertEqual(after['threads'], 4)

    def test_minimum_shared_budget_large_writes_parallel_ownership_and_credit_recovery(self):
        self.budget_case(16384)

    def test_intermediate_shared_budget_large_writes_parallel_ownership_and_credit_recovery(self):
        self.budget_case(40960)

    def gzip_connection(self, barrier=None):
        if barrier: barrier.wait(timeout=4)
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=14); first = None
        try:
            for path, encoding, expected in [('/compressed/index.php', 'gzip', TEXT), ('/small.php', 'identity', SMALL), ('/compressed/index.php', 'gzip', TEXT)]:
                connection.request('GET', path, headers={'Host': 'quantum.test', 'Accept-Encoding': encoding})
                if first is None: first = connection.sock
                self.assertIs(connection.sock, first)
                response = connection.getresponse(); body = response.read()
                self.assertEqual(response.status, 200, (body[:1000], self.log_path.read_text()))
                self.assertFalse(response.will_close)
                if encoding == 'gzip':
                    self.assertEqual(response.getheader('Content-Encoding'), 'gzip')
                    self.assertEqual(body[:2], b'\x1f\x8b')
                    self.assertEqual(gzip.decompress(body), expected)
                else:
                    self.assertIsNone(response.getheader('Content-Encoding')); self.assertEqual(body, expected)
        finally: connection.close()

    def test_large_php_vectors_through_gzip_filter_and_reused_connection(self):
        self.start(40960); before = self.quiescent()
        self.concurrent_wave(1, self.gzip_connection)
        self.concurrent_wave(8, self.gzip_connection)
        after = self.quiescent()
        self.assertEqual({key: after[key] - before[key] for key in ('completed', 'failed', 'timed_out')},
                         {'completed': 27, 'failed': 0, 'timed_out': 0})

    def test_output_progress_with_held_unread_request_body(self):
        self.start(24576); before = self.quiescent(); key = uuid.uuid4().hex
        payload = bytes(range(256)) * 16
        def request():
            connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=12)
            try:
                # PUT keeps PHP's ordinary eager POST parser out of this
                # explicit write-before-read application contract.
                connection.request('PUT', '/write-before-read.php?key=' + key, body=payload,
                    headers={'Host': 'quantum.test', 'Content-Type': 'application/octet-stream'})
                response = connection.getresponse(); body = response.read()
                self.assertEqual(response.status, 200, (body[:1000], self.log_path.read_text()))
                expected = b'R' * 65536 + json.dumps({'length': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()},
                    separators=(',', ':')).encode() + b'\n'
                self.assertEqual(body, expected)
            finally: connection.close()
        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(request)
            try:
                end = time.monotonic() + 3
                while True:
                    if future.done(): future.result(); self.fail('request returned before releasing its input gate')
                    values = self.metrics()
                    if (self.base / (key + '-entered')).exists() and values['active_php_threads'] == 1 and values['buffered_bytes'] == len(payload): break
                    self.assertLess(time.monotonic(), end, ('request body was not demonstrably retained', values))
                    time.sleep(.005)
                (self.base / (key + '-release')).touch()
                future.result(timeout=10)
            finally: (self.base / (key + '-release')).touch()
        after = self.quiescent()
        self.assertEqual({name: after[name] - before[name] for name in ('completed', 'failed', 'timed_out')},
                         {'completed': 1, 'failed': 0, 'timed_out': 0})

    def partial_input_progress(self, budget):
        self.start(budget); before = self.quiescent()
        for preload in (4096, 3073):
            with self.subTest(budget=budget, preload=preload):
                key = uuid.uuid4().hex
                connection = socket.create_connection(('127.0.0.1', self.port), timeout=12)
                stream = connection.makefile('rb')
                def exact(length):
                    value = stream.read(length)
                    self.assertEqual(len(value), length, self.log_path.read_text())
                    return value
                def headers():
                    status = stream.readline()
                    self.assertTrue(status.startswith(b'HTTP/1.1 200 '), (status, self.log_path.read_text()))
                    values = {}
                    while True:
                        line = stream.readline()
                        self.assertTrue(line, 'EOF in response headers')
                        if line == b'\r\n': return values
                        name, value = line.rstrip(b'\r\n').split(b':', 1)
                        values[name.lower()] = value.strip()
                def chunk():
                    line = stream.readline()
                    self.assertTrue(line, ('EOF before output chunk', self.log_path.read_text()))
                    length = int(line.split(b';', 1)[0], 16)
                    if not length:
                        self.assertEqual(stream.readline(), b'\r\n')
                        return b''
                    value = exact(length)
                    self.assertEqual(exact(2), b'\r\n')
                    return value
                try:
                    connection.sendall((
                        'PUT /write-before-read.php?key=' + key + ' HTTP/1.1\r\n'
                        'Host: quantum.test\r\nContent-Type: application/octet-stream\r\n'
                        'Content-Length: ' + str(len(LARGE)) + '\r\n\r\n').encode() + LARGE[:preload])
                    end = time.monotonic() + 3
                    while True:
                        values = self.metrics()
                        if (self.base / (key + '-entered')).exists() and values['buffered_bytes'] == preload:
                            break
                        self.assertLess(time.monotonic(), end, ('partial input was not retained', values))
                        time.sleep(.005)
                    (self.base / (key + '-release')).touch()
                    self.assertEqual(headers().get(b'transfer-encoding'), b'chunked')
                    prefix = bytearray()
                    while len(prefix) < 65536:
                        value = chunk(); self.assertTrue(value)
                        prefix.extend(value)
                    self.assertEqual(prefix, b'R' * 65536)
                    # The application must emit all 64 KiB before this upload
                    # continues. Queue the next request at the exact body edge.
                    connection.sendall(LARGE[preload:] +
                        b'GET /small.php HTTP/1.1\r\nHost: quantum.test\r\n\r\n')
                    tail = bytearray()
                    while True:
                        value = chunk()
                        if not value: break
                        tail.extend(value)
                    self.assertEqual(json.loads(tail), {'length': len(LARGE), 'sha256': hashlib.sha256(LARGE).hexdigest()})
                    small_headers = headers()
                    self.assertEqual(small_headers.get(b'content-length'), str(len(SMALL)).encode())
                    self.assertEqual(exact(len(SMALL)), SMALL)
                finally:
                    (self.base / (key + '-release')).touch()
                    stream.close(); connection.close()
                self.quiescent()
        after = self.quiescent()
        self.assertEqual({name: after[name] - before[name] for name in ('completed', 'failed', 'timed_out')},
                         {'completed': 4, 'failed': 0, 'timed_out': 0})

    def test_minimum_budget_output_before_partial_input_with_pipelined_followup(self):
        self.partial_input_progress(16384)

    def test_intermediate_budget_output_before_partial_input_with_pipelined_followup(self):
        self.partial_input_progress(24576)

    def stop(self):
        if self.process is not None:
            if self.process.poll() is None: self.process.terminate()
            try: self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait(timeout=2)
                self.fail('response quantum shutdown hung:\n' + self.log_path.read_text())
            self.assertEqual(self.process.returncode, 0, self.log_path.read_text()); self.process = None
        if self.log is not None: self.log.close(); self.log = None


if __name__ == '__main__':
    unittest.main()
