"""H1 body ownership/reuse: python3 tests/test_http1_body_reuse.py [binary]."""

import hashlib
import http.client
import json
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()
LARGE = bytes(range(256)) * 4096
SMALL = b"small-after-large\x00\xff"
ASSET = b"static-after-body\x00\xff\n" * 128


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def chunked(body):
    widths = (1, 17, 4097, 65536, 16381)
    wire, offset, index = [], 0, 0
    while offset < len(body):
        piece = body[offset:offset + widths[index % len(widths)]]
        wire.append(f"{len(piece):x};part={index}\r\n".encode() + piece + b"\r\n")
        offset += len(piece)
        index += 1
    wire.append(b"0\r\nX-Body-Digest: " + hashlib.sha256(body).hexdigest().encode() + b"\r\n\r\n")
    return b"".join(wire)


class HTTP1BodyReuseTest(unittest.TestCase):
    body_limit = 16384
    global_limit = 1048576

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="h2o-php-h1-reuse-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.port, self.tls_port = free_port(), free_port()
        while self.port == self.tls_port:
            self.tls_port = free_port()
        self.process = self.log = None
        self.addCleanup(self.stop)
        certificate, key = self.base / "certificate.pem", self.base / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                        "-noenc", "-days", "1", "-subj", "/CN=reuse.test", "-keyout", str(key),
                        "-out", str(certificate)], check=True, capture_output=True, timeout=10)
        key.chmod(0o600)
        (self.base / "read.php").write_text(r'''<?php
$input = fopen('php://input', 'rb');
$hash = hash_init('sha256'); $length = 0;
while (!feof($input)) {
    $piece = fread($input, 113);
    if ($piece === false) throw new RuntimeException('input read failed');
    if ($piece !== '') {
        $length += strlen($piece); hash_update($hash, $piece);
    }
}
$extra = fread($input, 1);
$body = json_encode(['id' => $_GET['id'] ?? '', 'length' => $length,
    'sha256' => hash_final($hash), 'eof' => feof($input), 'extra_length' => strlen($extra)], JSON_THROW_ON_ERROR);
header('Content-Type: application/json');
header('Content-Length: '.strlen($body));
echo $body;
''')
        (self.base / "asset.bin").write_bytes(ASSET)
        self.config = self.base / "server.yml"
        self.config.write_text(f"""listen:
  - host: 127.0.0.1
    port: {self.port}
  - host: 127.0.0.1
    port: {self.tls_port}
    ssl:
      certificate-file: {certificate}
      key-file: {key}
      minimum-version: TLSv1.3
      ocsp-update-interval: 0
num-threads: 2
php.runtime:
  threads: 2
  max-threads: 2
  collect-metrics: OFF
  request-timeout: 5s
  shutdown-timeout: 2s
  max-buffered-body: {self.body_limit}
  max-buffered-response: 16384
  max-buffered-global: {self.global_limit}
php.root: {self.base}
php.request-body-timeout: 2s
hosts:
  reuse.test:
    paths:
      /metrics:
        php.metrics: ON
      /:
        php_server: ON
""")
        self.log_path = self.base / "server.log"
        self.log = self.log_path.open("w")
        self.process = subprocess.Popen([str(BINARY), "-c", str(self.config)], stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.assertIsNone(self.process.poll(), self.log_path.read_text())
            try:
                self.quiescent()
                return
            except (OSError, http.client.HTTPException):
                time.sleep(.01)
        self.fail("server did not start:\n" + self.log_path.read_text())

    def stop(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
                self.fail("server shutdown hung:\n" + self.log_path.read_text())
            self.assertEqual(self.process.returncode, 0, self.log_path.read_text())
            self.process = None
        if self.log is not None:
            self.log.close()
            self.log = None

    def metrics(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request("GET", "/metrics", headers={"Host": "reuse.test"})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            return json.loads(response.read())
        finally:
            connection.close()

    def quiescent(self):
        deadline = time.monotonic() + 3
        while True:
            values = self.metrics()
            if all(values[key] == 0 for key in ("active_php_threads", "busy_workers", "queued_requests", "buffered_bytes")):
                return values
            self.assertLess(time.monotonic(), deadline, values)
            time.sleep(.005)

    def connection(self, tls):
        if tls:
            return http.client.HTTPSConnection("127.0.0.1", self.tls_port,
                context=ssl._create_unverified_context(), timeout=8)
        return http.client.HTTPConnection("127.0.0.1", self.port, timeout=8)

    def raw_connection(self, tls):
        connection = socket.create_connection(("127.0.0.1", self.tls_port if tls else self.port), timeout=5)
        if tls:
            connection = ssl._create_unverified_context().wrap_socket(connection, server_hostname="reuse.test")
        return connection

    def verify(self, raw, label, body):
        self.assertEqual(json.loads(raw), {'id': label, 'length': len(body),
            'sha256': hashlib.sha256(body).hexdigest(), 'eof': True, 'extra_length': 0})

    def send(self, connection, label, body, *, use_chunks=False, fragmented=False):
        connection.putrequest("POST", "/read.php?id=" + label, skip_host=True, skip_accept_encoding=True)
        connection.putheader("Host", "reuse.test")
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Transfer-Encoding" if use_chunks else "Content-Length", "chunked" if use_chunks else str(len(body)))
        connection.endheaders()
        wire = chunked(body) if use_chunks else body
        if fragmented:
            offset, index = 0, 0
            widths = (1, 17, 4097, 65536, 16381)
            while offset < len(wire):
                piece = wire[offset:offset + widths[index % len(widths)]]
                connection.send(piece)
                offset += len(piece)
                index += 1
                if index <= 3:
                    time.sleep(.002)
        elif wire:
            connection.send(wire)
        response = connection.getresponse()
        raw = response.read()
        self.assertEqual(response.status, 200, raw)
        self.assertFalse(response.will_close)
        self.verify(raw, label, body)

    def test_large_small_repeated_fragmented_and_chunked_reuse_plain_and_tls(self):
        before = self.quiescent()
        for tls in (False, True):
            connection = self.connection(tls)
            first_socket = None
            try:
                for label, body, chunks, fragments in (
                    ('large-1', LARGE, False, False), ('small', SMALL, False, False),
                    ('empty', b'', False, False), ('large-fragments', LARGE[::-1], False, True),
                    ('chunked-large', LARGE, True, True), ('small-after-chunks', SMALL[::-1], False, False),
                    ('large-2', LARGE, False, False)):
                    with self.subTest(tls=tls, request=label):
                        self.send(connection, label, body, use_chunks=chunks, fragmented=fragments)
                        if first_socket is None:
                            first_socket = connection.sock
                        self.assertIs(connection.sock, first_socket, "connection reopened during body reuse")
                        self.quiescent()
            finally:
                connection.close()
        after = self.quiescent()
        self.assertEqual({key: after[key] - before[key] for key in ('completed', 'failed', 'timed_out')},
                         {'completed': 14, 'failed': 0, 'timed_out': 0})

    def read_fixed_response(self, stream):
        status = stream.readline(8192)
        self.assertTrue(status.startswith(b'HTTP/1.1 200 '), status)
        headers, size = {}, len(status)
        while True:
            line = stream.readline(8192)
            size += len(line)
            self.assertTrue(line)
            self.assertLessEqual(size, 65536)
            if line == b'\r\n':
                break
            key, value = line.split(b':', 1)
            key = key.lower()
            self.assertNotIn(key, headers, 'unexpected duplicate response header')
            headers[key] = value.strip()
        self.assertNotIn(b'transfer-encoding', headers)
        length = int(headers[b'content-length'])
        self.assertLessEqual(length, 65536)
        body = stream.read(length)
        self.assertEqual(len(body), length)
        return body

    def test_pipelined_body_trailers_and_next_request_bytes_plain_and_tls(self):
        before = self.quiescent()
        for tls in (False, True):
            fixed = (b'POST /read.php?id=pipeline-large HTTP/1.1\r\nHost: reuse.test\r\n'
                     b'Content-Type: application/octet-stream\r\nContent-Length: ' + str(len(LARGE)).encode() + b'\r\n\r\n' + LARGE)
            small = (b'POST /read.php?id=pipeline-small HTTP/1.1\r\nHost: reuse.test\r\n'
                     b'Content-Length: ' + str(len(SMALL)).encode() + b'\r\n\r\n' + SMALL)
            chunks = (b'POST /read.php?id=pipeline-chunks HTTP/1.1\r\nHost: reuse.test\r\n'
                      b'Transfer-Encoding: chunked\r\nContent-Type: application/octet-stream\r\n\r\n' + chunked(LARGE[:8193]))
            tail = (b'GET /read.php?id=pipeline-empty HTTP/1.1\r\nHost: reuse.test\r\n\r\n'
                    b'GET /asset.bin HTTP/1.1\r\nHost: reuse.test\r\nConnection: close\r\n\r\n')
            with self.raw_connection(tls) as connection:
                connection.sendall(fixed + small + chunks + tail)
                with connection.makefile('rb') as stream:
                    for label, body in (('pipeline-large', LARGE), ('pipeline-small', SMALL),
                                        ('pipeline-chunks', LARGE[:8193]), ('pipeline-empty', b'')):
                        self.verify(self.read_fixed_response(stream), label, body)
                    self.assertEqual(self.read_fixed_response(stream), ASSET)
                    self.assertEqual(stream.read(1), b'', 'final pipelined close did not complete')
        after = self.quiescent()
        self.assertEqual({key: after[key] - before[key] for key in ('completed', 'failed', 'timed_out')},
                         {'completed': 8, 'failed': 0, 'timed_out': 0})

    def test_eof_acknowledgement_keeps_pipelined_headers_until_response(self):
        # A complete streamed body must stop H1 reads while its response is
        # pending. Otherwise the next request can overwrite the active request
        # or feed a stale previous-header length to picohttpparser.
        held = self.base / 'held'
        release = self.base / 'release'
        (self.base / 'held.php').write_text(r"""<?php
$body = file_get_contents('php://input');
file_put_contents(__DIR__.'/held', 'ready');
while (!file_exists(__DIR__.'/release')) { clearstatcache(); usleep(1000); }
$result = json_encode(['id' => 'held', 'length' => strlen($body),
    'sha256' => hash('sha256', $body), 'eof' => true, 'extra_length' => 0]);
header('Content-Length: '.strlen($result)); echo $result;
""")
        for tls in (False, True):
            with self.subTest(tls=tls):
                held.unlink(missing_ok=True)
                release.unlink(missing_ok=True)
                try:
                    with self.raw_connection(tls) as connection:
                        # Keep the initial body incomplete to require streaming
                        # even when the application body budget exceeds 1 MiB.
                        connection.sendall(b'POST /held.php HTTP/1.1\r\nHost: reuse.test\r\n'
                            b'Content-Type: application/octet-stream\r\nContent-Length: 1048576\r\n\r\n' + LARGE[:16384])
                        deadline = time.monotonic() + 2
                        while self.metrics()['active_php_threads'] != 1:
                            self.assertIsNone(self.process.poll(), self.log_path.read_text())
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(.001)
                        connection.sendall(LARGE[16384:])
                        while not held.exists():
                            self.assertIsNone(self.process.poll(), self.log_path.read_text())
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(.001)
                        connection.sendall(b'GET /read.php?id=after-held HTTP/1.1\r\nHost: reuse.test\r\n'
                            b'Connection: close\r\n\r\n')
                        connection.settimeout(.05)
                        with self.assertRaises(socket.timeout, msg=
                                'server processed pipelined headers before the active response completed'):
                            connection.recv(1)
                        connection.settimeout(5)
                        release.touch()
                        with connection.makefile('rb') as stream:
                            self.verify(self.read_fixed_response(stream), 'held', LARGE)
                            self.verify(self.read_fixed_response(stream), 'after-held', b'')
                            self.assertEqual(stream.read(1), b'')
                finally:
                    release.touch()
                self.quiescent()

    def test_wire_size_read_budget_boundaries_then_pipelined_request_plain_and_tls(self):
        # Include the request headers in each boundary: a 1 MiB body alone
        # exceeds the native event loop's 1 MiB read budget once framed.
        for tls in (False, True):
            for wire_size in (8191, 8192, 8193, 1048575, 1048576, 1048577):
                with self.subTest(tls=tls, wire_size=wire_size):
                    label = f'wire-{wire_size}'
                    length = wire_size
                    while True:
                        headers = (f'POST /read.php?id={label} HTTP/1.1\r\nHost: reuse.test\r\n'
                            f'Content-Type: application/octet-stream\r\nContent-Length: {length}\r\n\r\n').encode()
                        adjusted = wire_size - len(headers)
                        if adjusted == length:
                            break
                        length = adjusted
                    body = (LARGE + SMALL)[:length]
                    self.assertEqual(len(headers) + len(body), wire_size)
                    tail = (b'GET /read.php?id=after-boundary HTTP/1.1\r\nHost: reuse.test\r\n'
                            b'Connection: close\r\n\r\n')
                    with self.raw_connection(tls) as connection:
                        connection.sendall(headers + body + tail)
                        with connection.makefile('rb') as stream:
                            self.verify(self.read_fixed_response(stream), label, body)
                            self.verify(self.read_fixed_response(stream), 'after-boundary', b'')
                            self.assertEqual(stream.read(1), b'')
                    self.quiescent()

    def test_expect_continue_large_body_then_empty_reuse_plain_and_tls(self):
        for tls in (False, True):
            with self.raw_connection(tls) as connection:
                with connection.makefile('rb') as stream:
                    connection.sendall(b'POST /read.php?id=continue-large HTTP/1.1\r\nHost: reuse.test\r\n'
                        b'Content-Type: application/octet-stream\r\nExpect: 100-continue\r\n'
                        b'Content-Length: 1048576\r\n\r\n')
                    self.assertEqual(stream.readline(), b'HTTP/1.1 100 Continue\r\n')
                    self.assertEqual(stream.readline(), b'\r\n')
                    connection.sendall(LARGE)
                    self.verify(self.read_fixed_response(stream), 'continue-large', LARGE)
                    connection.sendall(b'GET /read.php?id=continue-empty HTTP/1.1\r\nHost: reuse.test\r\n'
                        b'Connection: close\r\n\r\n')
                    self.verify(self.read_fixed_response(stream), 'continue-empty', b'')
                    self.assertEqual(stream.read(1), b'')
            self.quiescent()

    def test_cancelled_large_body_releases_credit_and_next_connection_recovers(self):
        for tls in (False, True):
            connection = self.raw_connection(tls)
            try:
                connection.sendall(b'POST /read.php?id=cancel HTTP/1.1\r\nHost: reuse.test\r\n'
                    b'Content-Type: application/octet-stream\r\nContent-Length: 1048576\r\n\r\n' + LARGE[:32768])
                # With the default enable_post_data_reading=1, PHP eagerly
                # reads POST input during request startup, before this script
                # can run. Prove native PHP admission without requiring a
                # userland marker that cannot precede the incomplete body's EOF.
                deadline = time.monotonic() + 1
                while True:
                    self.assertIsNone(self.process.poll(), self.log_path.read_text())
                    values = self.metrics()
                    if values['active_php_threads'] == 1 and values['queued_requests'] == 0:
                        break
                    self.assertLess(time.monotonic(), deadline, ('partial request was not admitted to PHP', values))
                    time.sleep(.002)
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            finally:
                connection.close()
            idle = self.quiescent()
            self.assertEqual(idle['buffered_bytes'], 0)
            recovery = self.connection(tls)
            try:
                self.send(recovery, 'recovery-large', LARGE)
                same_socket = recovery.sock
                self.send(recovery, 'recovery-small', SMALL)
                self.assertIs(recovery.sock, same_socket)
            finally:
                recovery.close()
            after = self.quiescent()
            self.assertEqual({key: after[key] - idle[key] for key in ('completed', 'failed', 'timed_out')},
                             {'completed': 2, 'failed': 0, 'timed_out': 0})


class HTTP1LargeBodyBudgetTest(HTTP1BodyReuseTest):
    body_limit = 8388608
    global_limit = 33554432


if __name__ == '__main__':
    unittest.main()
