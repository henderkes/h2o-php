"""H1 response leases: python3 tests/test_http1_body_lease.py --bin build/release/h2o-php.

H2O_PHP_BINARY also selects the server; --baseline optionally compares invalid
Content-Length responses with another binary. No private artifacts are needed.
"""
from concurrent.futures import ThreadPoolExecutor
import argparse
import contextlib
import gzip
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import time
import unittest
import uuid

# The existing standalone helpers consume a positional binary on import.
_saved_argv = sys.argv
sys.argv = sys.argv[:1]
try:
    from test_worker_pool import WorkerServer
    from test_http1_body_reuse import free_port
finally:
    sys.argv = _saved_argv

BINARY = Path(os.environ.get('H2O_PHP_BINARY', 'build/release/h2o-php')).resolve()
BASELINE = None
CANONICAL = b''.join(bytes([i]) * 65536 for i in range(32))
PREFIX, TAIL = b'owned-prefix\x00\xff\n', b'small-tail\x00\xff\n'
SMALL = b'small-followup\x00\xff\n!'
HUGE = b'H' * 8388608
TEXT = b'compressible text row 0123456789 abcdef\\n' * (2048 * 32)
ACTIVE_KEYS = ('active_php_threads', 'busy_workers', 'queued_requests', 'buffered_bytes', 'auxiliary_threads')


def wait_for(predicate, seconds=5, label='condition'):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.005)
    raise AssertionError('timed out waiting for ' + label)


def digest_equal(actual, expected):
    assert len(actual) == len(expected), (len(actual), len(expected))
    assert hashlib.sha256(actual).digest() == hashlib.sha256(expected).digest(), (
        hashlib.sha256(actual).hexdigest(), hashlib.sha256(expected).hexdigest())


class WireReader:
    """Strict response decoder retaining chunk boundaries and trailing bytes."""
    def __init__(self, connection):
        self.socket = connection
        self.stream = connection.makefile('rb')

    def headers(self):
        line = self.stream.readline(8192)
        assert line.startswith(b'HTTP/1.'), line
        status = int(line.split(b' ', 2)[1])
        headers, size = {}, len(line)
        while True:
            line = self.stream.readline(8192)
            size += len(line)
            assert line and size <= 65536, ('invalid headers', line, size)
            if line == b'\r\n':
                break
            key, value = line.split(b':', 1)
            key = key.lower()
            assert key not in headers, ('duplicate header', key)
            headers[key] = value.strip()
        return status, headers

    def exact(self, length):
        body = self.stream.read(length)
        assert len(body) == length, ('early EOF', len(body), length)
        return body

    def chunk(self):
        line = self.stream.readline(8192)
        assert line.endswith(b'\r\n'), ('invalid chunk size', line)
        size = int(line.strip(), 16)
        assert size <= len(HUGE), ('implausible chunk size', size)
        if size == 0:
            assert self.stream.readline(8192) == b'\r\n', 'unexpected trailer or missing terminator'
            return b''
        body = self.exact(size)
        assert self.exact(2) == b'\r\n', 'invalid chunk delimiter'
        return body

    def body(self, status, headers, method='GET'):
        if method == 'HEAD' or status in (204, 304):
            return b'', []
        if headers.get(b'transfer-encoding') == b'chunked':
            chunks = []
            while True:
                piece = self.chunk()
                if not piece:
                    return b''.join(chunks), [len(part) for part in chunks]
                chunks.append(piece)
        assert b'content-length' in headers, headers
        return self.exact(int(headers[b'content-length'])), []

    def response(self, method='GET'):
        status, headers = self.headers()
        body, widths = self.body(status, headers, method)
        return status, headers, body, widths

    def close(self):
        self.stream.close()


PHP_STREAM = r'''<?php
function lease_wire_stream(): void {
    $mode = $_GET['mode'] ?? 'canonical';
    $token = $_GET['token'] ?? '';
    if ($token !== '' && !preg_match('/^[a-f0-9]{32}$/D', $token)) throw new RuntimeException('invalid private token');
    $marker = __DIR__.'/mark-'.$token;
    $canonicalLength = 32 * 65536;
    header('Content-Type: application/octet-stream');
    header('X-Generation: '.($_SERVER['LEASE_GENERATION'] ?? 'v1'));
    if (isset($_GET['status'])) http_response_code((int)$_GET['status']);
    if ($mode === 'small') { header('Content-Length: 18'); echo "small-followup\x00\xff\n!"; return; }
    if ($mode === 'huge') {
        if ($token !== '') touch($marker.'-entered');
        header('Content-Length: 8388608');
        for ($i = 0; $i < 128; ++$i) {
            $buffer = str_repeat('H', 65536); echo $buffer;
            $buffer[0] = 'X'; unset($buffer);
        }
        flush();
        if ($token !== '') touch($marker.'-done');
        return;
    }
    if ($mode === 'unread') {
        echo str_repeat('R', 65536); flush();
        $input = file_get_contents('php://input');
        echo json_encode(['length' => strlen($input), 'sha256' => hash('sha256', $input)], JSON_THROW_ON_ERROR)."\n";
        return;
    }
    if ($mode === 'text') {
        header('Content-Type: text/plain');
        for ($i = 0; $i < 32; ++$i) { echo str_repeat('compressible text row 0123456789 abcdef\n', 2048); }
        return;
    }
    $prefix = $mode === 'prefix' ? "owned-prefix\x00\xff\n" : '';
    $tail = $mode === 'tail' ? "small-tail\x00\xff\n" : '';
    if (isset($_GET['cl'])) header('Content-Length: '.($canonicalLength + strlen($prefix) + strlen($tail) + (int)$_GET['cl']));
    if ($prefix !== '') { echo $prefix; flush(); }
    for ($i = 0; $i < 32; ++$i) {
        $buffer = str_repeat(chr($i), 65536);
        echo $buffer;
        // Reuse the exact PHP variable immediately after the write returns.
        $buffer[0] = chr(255 - $i); $buffer = str_repeat(chr(255 - $i), 65536);
    }
    if ($tail !== '') echo $tail;
    flush();
}
'''

PHP_FORK = r'''<?php
$token = $_GET['token'] ?? '';
if (!preg_match('/^[a-f0-9]{32}$/D', $token)) throw new RuntimeException('invalid private token');
header('Content-Type: application/octet-stream');
for ($i = 0; $i < 16; ++$i) {
    $prefix = str_repeat('F', 65536); echo $prefix;
    $prefix[0] = 'X'; unset($prefix);
}
flush();
$before = [];
foreach (glob('/proc/self/fd/[0-9]*') as $path) {
    $link = @readlink($path);
    if ($link !== false && (str_starts_with($link, 'socket:[') || $link === 'anon_inode:[eventfd]')) $before[(int)basename($path)] = $link;
}
$pid = pcntl_fork();
if ($pid < 0) throw new RuntimeException('fork failed');
if ($pid === 0) {
    $retained = [];
    foreach ($before as $fd => $link) if (@readlink('/proc/self/fd/'.$fd) === $link) $retained[$fd] = $link;
    file_put_contents(__DIR__.'/mark-'.$token.'-child.json', json_encode(['pid' => getmypid(), 'retained' => (object)$retained], JSON_THROW_ON_ERROR));
    exit(23);
}
$status = 0; $got = pcntl_waitpid($pid, $status);
$child = json_decode(file_get_contents(__DIR__.'/mark-'.$token.'-child.json'), true, 512, JSON_THROW_ON_ERROR);
echo json_encode(['before_managed' => $before, 'pid' => $pid, 'got' => $got,
    'exited' => $got > 0 && pcntl_wifexited($status), 'exit' => $got > 0 && pcntl_wifexited($status) ? pcntl_wexitstatus($status) : null,
    'child' => $child], JSON_THROW_ON_ERROR);
'''


class Server(WorkerServer):
    def __init__(self, *, binary=None, budget=None, tls=False, master=False, timeout='8s'):
        super().__init__(num=1)
        self.binary = Path(binary or BINARY).resolve()
        self.tls_port = free_port()
        while self.port == self.tls_port:
            self.tls_port = free_port()
        self.budget, self.tls, self.master, self.timeout = budget, tls, master, timeout
        self.generation = 'v1'
        self.address = self.base / 'control.sock'
        self.log_path = self.base / 'server.log'
        self.connections = []
        self.fd_baseline = None
        (self.base / 'stream.inc.php').write_text(PHP_STREAM)
        (self.base / 'stream.php').write_text("<?php require __DIR__.'/stream.inc.php'; lease_wire_stream();\n")
        self.worker.write_text("<?php require __DIR__.'/stream.inc.php'; while (frankenphp_handle_request(function () { lease_wire_stream(); }));\n")
        (self.base / 'fork.php').write_text(PHP_FORK)
        (self.base / 'capabilities.php').write_text("<?php header('Content-Type: application/json'); echo json_encode(['zts' => PHP_ZTS, 'pcntl' => function_exists('pcntl_fork')]);")

    def yaml(self):
        listeners = f'listen:\n  - host: 127.0.0.1\n    port: {self.port}\n'
        if self.tls:
            listeners += f'''  - host: 127.0.0.1
    port: {self.tls_port}
    ssl:
      certificate-file: {self.base / 'certificate.pem'}
      key-file: {self.base / 'key.pem'}
      minimum-version: TLSv1.3
      ocsp-update-interval: 0
'''
        budget = '' if self.budget is None else f'  max-buffered-global: {self.budget}\n'
        return listeners + f'''num-threads: 2
php.runtime:
  threads: 3
  max-threads: 4
  collect-metrics: OFF
  request-timeout: {self.timeout}
  shutdown-timeout: 4s
  max-buffered-body: 4096
  max-buffered-response: 131072
{budget}php.root: {self.base}
php.request-body-timeout: 5s
php.ini:
  opcache.enable: "0"
  output_buffering: "0"
  zlib.output_compression: "0"
  ignore_user_abort: "0"
  display_errors: "0"
hosts:
  lease.test:
    php.env:
      LEASE_GENERATION: {self.generation}
    paths:
      /metrics:
        php.metrics: ON
      /worker:
        php.worker:
          file: {self.base / 'worker.php'}
          num: 1
      /gzip/:
        php.root: {self.base / 'gzip'}
        compress: ON
        php_server: ON
      /:
        php_server: ON
'''

    def start(self):
        if self.tls:
            certificate, key = self.base / 'certificate.pem', self.base / 'key.pem'
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
                '-noenc', '-days', '1', '-subj', '/CN=lease.test', '-keyout', str(key), '-out', str(certificate)],
                check=True, capture_output=True, timeout=10)
            key.chmod(0o600)
        (self.base / 'gzip').mkdir()
        (self.base / 'gzip/index.php').write_text("<?php require __DIR__.'/../stream.inc.php'; lease_wire_stream();\n")
        self.config.write_text(self.yaml())
        self.log = self.log_path.open('w')
        command = [str(self.binary), '-c', str(self.config)]
        if self.master:
            command = [str(self.binary), 'run', '--config', str(self.config), '--address', 'unix/' + str(self.address)]
        self.process = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT, env=os.environ.copy(), start_new_session=True)
        def ready():
            assert self.process.poll() is None, self.log_path.read_text()
            try:
                values = self.metrics()
                return values.get('ready_workers') == 1
            except (OSError, http.client.HTTPException):
                return False
        wait_for(ready, 10, 'server readiness')
        self.quiescent()
        status, _, body = self.fetch('/capabilities.php')
        assert status == 200, body
        capabilities = json.loads(body)
        assert capabilities['zts'] == 1, capabilities
        self.capabilities = capabilities
        self.quiescent()
        if not self.master and Path('/proc/self/fd').exists():
            self.fd_baseline = self.fd_snapshot()
        return self

    def fd_snapshot(self):
        snapshot = {}
        for path in Path('/proc/' + str(self.process.pid) + '/fd').iterdir():
            with contextlib.suppress(FileNotFoundError):
                snapshot[path.name] = os.readlink(path)
        return snapshot

    def assert_fd_return(self):
        if self.master or self.fd_baseline is None:
            return
        expected = sum(value == 'anon_inode:[eventfd]' for value in self.fd_baseline.values())
        def returned():
            current = self.fd_snapshot()
            return current if sum(value == 'anon_inode:[eventfd]' for value in current.values()) == expected else False
        wait_for(returned, 3, 'lease eventfd return to startup baseline')

    def fetch(self, path='/stream.php?mode=small', *, tls=False, method='GET', headers=None):
        connection = (http.client.HTTPSConnection('127.0.0.1', self.tls_port, timeout=15,
            context=ssl._create_unverified_context()) if tls else http.client.HTTPConnection('127.0.0.1', self.port, timeout=15))
        try:
            connection.request(method, path, headers={'Host': 'lease.test', 'Accept-Encoding': 'identity', **(headers or {})})
            response = connection.getresponse()
            return response.status, {k.lower(): v for k, v in response.getheaders()}, response.read()
        finally:
            connection.close()

    def metrics(self):
        status, _, body = self.fetch('/metrics')
        assert status == 200, body
        values = json.loads(body)
        assert values['buffered_bytes'] >= 0, values
        if self.budget is not None:
            assert values['buffered_bytes'] <= self.budget, values
        return values

    def quiescent(self):
        def idle():
            values = self.metrics()
            return values if all(values.get(key, 0) == 0 for key in ACTIVE_KEYS) else False
        values = wait_for(idle, 5, 'zero retained request/response ownership')
        self.assert_fd_return()
        return values

    def raw(self, *, slow=False):
        connection = socket.socket()
        if slow:
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32768)
        connection.settimeout(15)
        connection.connect(('127.0.0.1', self.port))
        self.connections.append(connection)
        return connection

    def request(self, connection, path, *, method='GET', close=False):
        connection.sendall(f'{method} {path} HTTP/1.1\r\nHost: lease.test\r\nAccept-Encoding: identity\r\n'.encode()
            + (b'Connection: close\r\n' if close else b'') + b'\r\n')

    def active_huge(self):
        token = uuid.uuid4().hex
        connection = self.raw(slow=True)
        self.request(connection, '/stream.php?mode=huge&token=' + token)
        wait_for(lambda: (self.base / ('mark-' + token + '-entered')).exists(), 3, 'huge writer entry')
        wait_for(lambda: self.metrics()['active_php_threads'] > 0, 3, 'active slow writer')
        time.sleep(.08)  # Let an 8 MiB write fill the bounded client receive window.
        assert not (self.base / ('mark-' + token + '-done')).exists(), 'writer completed before slow-reader exercise'
        return connection, token

    def command(self, action, *extra):
        return subprocess.run([str(self.binary), action, '--address', 'unix/' + str(self.address), *extra],
            capture_output=True, text=True, timeout=12, env=os.environ.copy())

    def stop(self):
        for connection in self.connections:
            with contextlib.suppress(OSError):
                connection.close()
        super().stop()


class LeaseWireTests(unittest.TestCase):
    def server(self, **settings):
        server = Server(**settings)
        self.addCleanup(server.stop)
        return server.start()

    def verify(self, response, expected, *, status=200, chunked=None):
        code, headers, body, widths = response
        self.assertEqual(code, status)
        digest_equal(body, expected)
        if chunked is not None:
            self.assertEqual(headers.get(b'transfer-encoding') == b'chunked', chunked)
        return headers, widths

    def response_then_followup(self, server, path, expected, **kwargs):
        connection = server.raw()
        reader = WireReader(connection)
        try:
            server.request(connection, path)
            result = reader.response()
            self.verify(result, expected, **kwargs)
            server.request(connection, '/stream.php?mode=small')
            self.verify(reader.response(), SMALL, chunked=False)
            return result
        finally:
            reader.close()
            connection.close()
            server.quiescent()

    def test_01_worker_canonical_32x64k_and_caller_buffer_reuse(self):
        server = self.server()
        self.response_then_followup(server, '/worker?mode=canonical', CANONICAL, chunked=True)

    def test_02_small_owned_prefix_then_large(self):
        self.response_then_followup(self.server(), '/stream.php?mode=prefix', PREFIX + CANONICAL, chunked=True)

    def test_03_large_then_small_tail(self):
        self.response_then_followup(self.server(), '/worker?mode=tail', CANONICAL + TAIL, chunked=True)

    def test_04_explicit_content_length_and_reuse(self):
        result = self.response_then_followup(self.server(), '/worker?cl=0', CANONICAL, chunked=False)
        self.assertEqual(result[1][b'content-length'], str(len(CANONICAL)).encode())

    def test_05_content_length_mismatches_close_and_optional_baseline(self):
        observed = {}
        binaries = [('candidate', BINARY)]
        if BASELINE is not None:
            binaries.insert(0, ('baseline', BASELINE))
        for label, binary in binaries:
            server = self.server(binary=binary)
            rows = []
            for delta in (-37, 37):
                connection = server.raw()
                server.request(connection, '/worker?cl=' + str(delta), close=True)
                wire = bytearray()
                while True:
                    part = connection.recv(65536)
                    if not part:
                        break
                    wire.extend(part)
                connection.close()
                head, body = bytes(wire).split(b'\r\n\r\n', 1)
                headers = dict(line.split(b': ', 1) for line in head.split(b'\r\n')[1:])
                rows.append({'status': head.split(b'\r\n')[0].decode(), 'length': len(body),
                    'sha256': hashlib.sha256(body).hexdigest(),
                    'content_length': next((v.decode() for k, v in headers.items() if k.lower() == b'content-length'), None),
                    'transfer_encoding': next((v.decode() for k, v in headers.items() if k.lower() == b'transfer-encoding'), None)})
                self.assertTrue(head.startswith(b'HTTP/1.1 200 '), head)
                self.assertIn(b'connection: close', head.lower())
                self.assertEqual(rows[-1]['content_length'], str(len(CANONICAL) + delta))
                self.assertIsNone(rows[-1]['transfer_encoding'])
                digest_equal(body, CANONICAL)
            observed[label] = rows
            server.quiescent()
            server.stop()
        if BASELINE is not None:
            self.assertEqual(observed['candidate'], observed['baseline'])

    def test_06_head_204_304_leave_next_response_clean(self):
        server = self.server()
        connection = server.raw()
        reader = WireReader(connection)
        try:
            for method, status in (('HEAD', 200), ('GET', 204), ('GET', 304)):
                server.request(connection, '/worker?status=' + str(status), method=method)
                self.verify(reader.response(method), b'', status=status)
                server.request(connection, '/stream.php?mode=small')
                self.verify(reader.response(), SMALL)
        finally:
            reader.close(); connection.close()
        server.quiescent()

    def test_07_gzip_filter_owned_fallback(self):
        server = self.server()
        status, headers, body = server.fetch('/gzip/?mode=text', headers={'Accept-Encoding': 'gzip'})
        self.assertEqual(status, 200)
        self.assertEqual(headers.get('content-encoding'), 'gzip')
        digest_equal(gzip.decompress(body), TEXT)
        server.quiescent()

    def test_08_tls_owned_fallback(self):
        server = self.server(tls=True)
        status, headers, body = server.fetch('/worker', tls=True)
        self.assertEqual(status, 200)
        self.assertNotIn('content-encoding', headers)
        digest_equal(body, CANONICAL)
        server.quiescent()

    def test_09_h2_owned_fallback(self):
        if shutil.which('curl') is None or 'HTTP2' not in subprocess.run(['curl', '--version'], capture_output=True, text=True, check=True).stdout:
            self.skipTest('curl with HTTP/2 support is required')
        server = self.server(tls=True)
        output = server.base / 'h2-body.bin'
        result = subprocess.run(['curl', '--noproxy', '*', '--insecure', '--http2', '--connect-timeout', '3',
            '--max-time', '12', '--resolve', f'lease.test:{server.tls_port}:127.0.0.1', '--header', 'Accept-Encoding: identity',
            '--output', str(output), '--write-out', '%{json}', f'https://lease.test:{server.tls_port}/worker'],
            capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        metadata = json.loads(result.stdout)
        self.assertEqual(metadata['http_version'], '2')
        self.assertEqual(metadata['response_code'], 200)
        digest_equal(output.read_bytes(), CANONICAL)
        server.quiescent()

    def test_10_two_pipelined_requests_while_large_first_active(self):
        server = self.server()
        connection, token = server.active_huge()
        server.request(connection, '/stream.php?mode=small')
        reader = WireReader(connection)
        try:
            self.verify(reader.response(), HUGE)
            self.verify(reader.response(), SMALL)
        finally:
            reader.close(); connection.close()
        server.quiescent()

    def test_11_active_slow_reader_complete_then_rst_and_recovery(self):
        server = self.server()
        for cancel in (False, True):
            with self.subTest(rst=cancel):
                connection, token = server.active_huge()
                if cancel:
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                    connection.close()
                else:
                    reader = WireReader(connection)
                    try:
                        self.verify(reader.response(), HUGE)
                    finally:
                        reader.close(); connection.close()
                server.quiescent()
                status, _, body = server.fetch()
                self.assertEqual(status, 200); self.assertEqual(body, SMALL)
                server.quiescent()

    def test_12_active_lease_fork_child_closes_managed_fds_parent_usable(self):
        server = self.server()
        if not server.capabilities['pcntl'] or not Path('/proc/self/fd').exists():
            self.skipTest('Linux /proc and the PHP pcntl extension are required')
        token = uuid.uuid4().hex
        connection = server.raw()
        server.request(connection, '/fork.php?token=' + token)
        reader = WireReader(connection)
        try:
            status, headers, body, widths = reader.response()
            self.assertEqual(status, 200)
            self.assertEqual(body[:1048576], b'F' * 1048576)
            result = json.loads(body[1048576:])
            self.assertGreater(len(result['before_managed']), 0)
            self.assertEqual(result['got'], result['pid'])
            self.assertTrue(result['exited']); self.assertEqual(result['exit'], 23)
            self.assertFalse(result['child']['retained'])
            server.request(connection, '/stream.php?mode=small')
            self.verify(reader.response(), SMALL)
        finally:
            reader.close(); connection.close()
        server.quiescent()
        server.assert_fd_return()

    def test_13_active_lease_reload_and_graceful_stop_drain(self):
        server = self.server(master=True)
        connection, token = server.active_huge()
        server.generation = 'v2'; server.config.write_text(server.yaml())
        result = server.command('reload', '--config', str(server.config))
        self.assertEqual(result.returncode, 0, result.stderr)
        status, headers, body = server.fetch()
        self.assertEqual((status, headers['x-generation'], body), (200, 'v2', SMALL))
        reader = WireReader(connection)
        try:
            response = reader.response()
            self.verify(response, HUGE)
            self.assertEqual(response[1][b'x-generation'], b'v1')
        finally:
            reader.close(); connection.close()
        server.quiescent()
        connection, token = server.active_huge()
        result = server.command('stop')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNone(server.process.poll())
        reader = WireReader(connection)
        try:
            self.verify(reader.response(), HUGE)
        finally:
            reader.close(); connection.close()
        self.assertEqual(server.process.wait(timeout=8), 0)

    def test_14_active_writer_request_deadline_then_recovery(self):
        server = self.server(timeout='400ms')
        before = server.quiescent()
        connection, token = server.active_huge()
        time.sleep(.55)
        # Read the finite close-delimited remainder after request cancellation.
        wire = bytearray()
        while True:
            part = connection.recv(65536)
            if not part:
                break
            wire.extend(part)
        connection.close()
        self.assertLess(len(wire), len(HUGE))
        after = server.quiescent()
        self.assertGreater(after['timed_out'], before['timed_out'])
        status, _, body = server.fetch()
        self.assertEqual(status, 200); self.assertEqual(body, SMALL)
        server.quiescent()

    def test_15_16k_24k_budget_and_unread_input_owned_progress(self):
        input_body = bytes(range(256)) * 4096
        for budget in (16384, 24576):
            with self.subTest(budget=budget):
                server = self.server(budget=budget)
                self.response_then_followup(server, '/worker', CANONICAL, chunked=True)
                connection = server.raw()
                reader = WireReader(connection)
                # Both budgets must make output progress with unread input.
                # Feed the remaining request only after reading its output.
                initial = 4096
                connection.sendall(b'PUT /stream.php?mode=unread HTTP/1.1\r\nHost: lease.test\r\n'
                    b'Content-Type: application/octet-stream\r\nContent-Length: 1048576\r\n\r\n' + input_body[:initial])
                try:
                    status, headers = reader.headers()
                    self.assertEqual(status, 200)
                    self.assertEqual(headers.get(b'transfer-encoding'), b'chunked')
                    prefix = bytearray()
                    while len(prefix) < 65536:
                        prefix.extend(reader.chunk())
                    self.assertEqual(prefix, b'R' * 65536)
                    with ThreadPoolExecutor(max_workers=1) as pool:
                        sending = pool.submit(connection.sendall, input_body[initial:])
                        tail = bytearray()
                        while True:
                            piece = reader.chunk()
                            if not piece:
                                break
                            tail.extend(piece)
                        sending.result(timeout=5)
                    self.assertEqual(json.loads(tail), {'length': len(input_body), 'sha256': hashlib.sha256(input_body).hexdigest()})
                finally:
                    reader.close(); connection.close()
                server.quiescent(); server.stop()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--bin', type=Path, default=BINARY)
    parser.add_argument('--baseline', type=Path)
    options, remaining = parser.parse_known_args()
    BINARY = options.bin.resolve()
    BASELINE = options.baseline.resolve() if options.baseline else None
    unittest.main(argv=[sys.argv[0], *remaining])
