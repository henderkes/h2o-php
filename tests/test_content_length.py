"""CGI framing metadata: python3 tests/test_content_length.py [h2o-php binary]."""

import base64
import hashlib
import http.client
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()
PAYLOAD = bytes(range(256)) * 2 + b"Z"
CASES = [("positive", PAYLOAD, ["513"]), ("zero", b"", ["0"]),
         ("leading-zero", PAYLOAD, ["000513"]), ("absent", None, []),
         ("duplicate", PAYLOAD, ["00513", "00513"]), ("absent-again", None, [])]


def port(udp=False):
    for _ in range(30):
        with socket.socket() as tcp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as datagram:
            tcp.bind(("127.0.0.1", 0))
            result = tcp.getsockname()[1]
            try:
                if udp:
                    datagram.bind(("127.0.0.1", result))
                return result
            except OSError:
                pass
    raise AssertionError("could not find a private TCP/UDP port")


class ContentLengthTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="h2o-php-content-length-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.port = port()
        self.process = self.log = None
        self.addCleanup(self.stop)
        (self.base / "inspect.inc.php").write_text(r'''<?php
function inspect_request($boot = null, $count = null) {
    $body = file_get_contents('php://input');
    $headers = [];
    foreach (frankenphp_request_headers() as $key => $value) $headers[strtolower($key)] = $value;
    header('Content-Type: application/json');
    echo json_encode(['content_length_exists' => array_key_exists('CONTENT_LENGTH', $_SERVER),
        'content_length' => $_SERVER['CONTENT_LENGTH'] ?? null,
        'http_content_length_exists' => array_key_exists('HTTP_CONTENT_LENGTH', $_SERVER),
        'http_content_length' => $_SERVER['HTTP_CONTENT_LENGTH'] ?? null,
        'headers' => (object)$headers, 'body' => base64_encode($body), 'length' => strlen($body),
        'sha256' => hash('sha256', $body), 'boot' => $boot, 'count' => $count]);
}
''')
        (self.base / "inspect.php").write_text("<?php require __DIR__.'/inspect.inc.php'; inspect_request();\n")
        (self.base / "worker.php").write_text(r'''<?php
require __DIR__.'/inspect.inc.php';
$boot = bin2hex(random_bytes(8)); $count = 0;
while (frankenphp_handle_request(function () use ($boot, &$count) { inspect_request($boot, ++$count); }));
''')

    def start(self, *, tls=False, upstream=None):
        listeners = f"listen:\n  - host: 127.0.0.1\n    port: {self.port}\n"
        if tls:
            self.tls_port = port(udp=True)
            certificate, key = self.base / "certificate.pem", self.base / "key.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                            "-noenc", "-days", "1", "-subj", "/CN=length.test", "-keyout", str(key),
                            "-out", str(certificate)], capture_output=True, check=True, timeout=10)
            key.chmod(0o600)
            for protocol in ("tcp", "quic"):
                listeners += (f"  - host: 127.0.0.1\n    port: {self.tls_port}\n    type: {protocol}\n"
                              f"    ssl:\n      certificate-file: {certificate}\n      key-file: {key}\n"
                              "      minimum-version: TLSv1.3\n      ocsp-update-interval: 0\n")
        proxy = f"      /proxy:\n        proxy.reverse.url: http://127.0.0.1:{upstream}/\n" if upstream else ""
        self.config = self.base / "server.yml"
        self.config.write_text(listeners + f"""num-threads: 2
num-quic-threads: 1
php.runtime:
  threads: 1
  max-threads: 2
  collect-metrics: OFF
  request-timeout: 5s
  shutdown-timeout: 2s
php.root: {self.base}
hosts:
  length.test:
    paths:
      /metrics:
        php.metrics: ON
      /worker:
        php.worker:
          file: {self.base / 'worker.php'}
          num: 1
{proxy}      /:
        php_server: ON
""")
        self.log_path = self.base / "server.log"
        self.log = self.log_path.open("w")
        self.process = subprocess.Popen([str(BINARY), "-c", str(self.config)], stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.assertIsNone(self.process.poll(), self.log_path.read_text())
            try:
                metrics = self.quiescent()
                if metrics["ready_workers"] == 1:
                    return
            except (OSError, http.client.HTTPException):
                pass
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

    def quiescent(self):
        deadline = time.monotonic() + 3
        while True:
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
            try:
                connection.request("GET", "/metrics", headers={"Host": "length.test"})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                values = json.loads(response.read())
            finally:
                connection.close()
            if all(values[key] == 0 for key in ("active_php_threads", "busy_workers", "queued_requests", "buffered_bytes")):
                return values
            self.assertLess(time.monotonic(), deadline, values)
            time.sleep(.01)

    def verify(self, raw, body, lengths):
        result = json.loads(raw)
        expected = lengths[0] if lengths else ""
        payload = body or b""
        self.assertIs(result["content_length_exists"], True)
        self.assertEqual(result["content_length"], expected)
        self.assertIs(result["http_content_length_exists"], bool(lengths))
        self.assertEqual(result["http_content_length"], expected if lengths else None)
        self.assertEqual(result["headers"].get("content-length"), expected if lengths else None)
        self.assertEqual(base64.b64decode(result["body"]), payload)
        self.assertEqual(result["length"], len(payload))
        self.assertEqual(result["sha256"], hashlib.sha256(payload).hexdigest())
        return result

    def send_h1(self, connection, path, body, lengths, *, chunked=False):
        connection.putrequest("GET" if body is None else "POST", path, skip_host=True, skip_accept_encoding=True)
        connection.putheader("Host", "length.test")
        if body is not None:
            connection.putheader("Content-Type", "application/octet-stream")
        for value in lengths:
            connection.putheader("Content-Length", value)
        if chunked:
            connection.putheader("Transfer-Encoding", "chunked")
        connection.endheaders()
        if chunked:
            connection.send(f"{len(body):x}\r\n".encode() + body + b"\r\n0\r\nX-Trailer: done\r\n\r\n")
        elif body:
            connection.send(body)
        response = connection.getresponse()
        raw = response.read()
        self.assertEqual(response.status, 200, raw)
        self.assertFalse(response.will_close)
        return raw

    def assert_accounting(self, before, count):
        after = self.quiescent()
        self.assertEqual({key: after[key] - before[key] for key in ("completed", "failed", "timed_out")},
                         {"completed": count, "failed": 0, "timed_out": 0})

    def test_h1_original_values_absence_chunking_and_reuse(self):
        self.start()
        before = self.quiescent()
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        first_socket = None
        worker_values = []
        try:
            for path in ("/inspect.php", "/worker"):
                for name, body, lengths in CASES + [("chunked", PAYLOAD, [])]:
                    with self.subTest(path=path, case=name):
                        raw = self.send_h1(connection, path, body, lengths, chunked=name == "chunked")
                        if first_socket is None:
                            first_socket = connection.sock
                        self.assertIs(connection.sock, first_socket, "H1 connection reopened")
                        result = self.verify(raw, body, lengths)
                        if path == "/worker":
                            worker_values.append(result)
            self.assertEqual(len({value["boot"] for value in worker_values}), 1)
            self.assertEqual([value["count"] for value in worker_values], list(range(1, len(worker_values) + 1)))
        finally:
            connection.close()
        self.assert_accounting(before, 14)

    def test_conflicting_duplicate_framing_still_rejects_pipeline(self):
        self.start()
        before = self.quiescent()
        wire = (b"POST /inspect.php HTTP/1.1\r\nHost: length.test\r\nContent-Length: 513\r\n"
                b"Content-Length: 514\r\n\r\n" + PAYLOAD + b"GET /inspect.php HTTP/1.1\r\nHost: length.test\r\n\r\n")
        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as connection:
            connection.sendall(wire)
            response = bytearray()
            while True:
                piece = connection.recv(65536)
                if not piece:
                    break
                response.extend(piece)
        self.assertTrue(response.startswith(b"HTTP/1.1 400 "), response)
        self.assertEqual(response.count(b"HTTP/1.1 "), 1)
        self.assert_accounting(before, 0)

    def test_h2_h3_original_values_and_persistent_reuse(self):
        if not shutil.which("curl"):
            self.skipTest("curl with H2/H3 is required")
        version = subprocess.check_output(["curl", "--version"], text=True)
        if "HTTP2" not in version or "HTTP3" not in version:
            self.skipTest("curl with H2/H3 is required")
        self.start(tls=True)
        before = self.quiescent()
        for protocol, flag in (("2", "--http2"), ("3", "--http3-only")):
            command = ["curl"]
            outputs = []
            for path in ("/inspect.php", "/worker"):
                for name, body, lengths in CASES:
                    if outputs:
                        command += ["--next"]
                    output = self.base / f"h{protocol}-{path.strip('/').replace('.', '-')}-{name}.json"
                    command += ["--noproxy", "*", "--insecure", flag, "--connect-timeout", "3", "--max-time", "5",
                                "--resolve", f"length.test:{self.tls_port}:127.0.0.1", "--header", "Accept-Encoding: identity"]
                    for value in lengths:
                        command += ["--header", "Content-Length: " + value]
                    if body is not None:
                        request_body = self.base / f"request-{name}.bin"
                        request_body.write_bytes(body)
                        command += ["--data-binary", "@" + str(request_body), "--header", "Content-Type: application/octet-stream"]
                    command += ["--output", str(output), "--write-out", "%{json}\n", f"https://length.test:{self.tls_port}{path}"]
                    outputs.append((output, body, lengths, path))
            result = subprocess.run(command, capture_output=True, text=True, timeout=70)
            self.assertEqual(result.returncode, 0, result.stderr)
            metadata = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual(len(metadata), len(outputs))
            self.assertEqual([row["num_connects"] for row in metadata], [1] + [0] * (len(outputs) - 1), metadata)
            for row, (output, body, lengths, path) in zip(metadata, outputs):
                with self.subTest(protocol=protocol, file=output.name):
                    self.assertEqual(row["http_version"], protocol)
                    self.assertEqual(row["response_code"], 200)
                    self.verify(output.read_bytes(), body, lengths)
        self.assert_accounting(before, 24)

    def test_proxy_still_emits_one_framing_field(self):
        class Capture(BaseHTTPRequestHandler):
            def do_POST(self):
                lengths = self.headers.get_all("Content-Length", [])
                body = self.rfile.read(int(lengths[0]) if lengths else 0)
                output = json.dumps({"content_lengths": lengths, "transfer_encoding": self.headers.get_all("Transfer-Encoding", []),
                                     "body": base64.b64encode(body).decode()}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(output)))
                self.end_headers()
                self.wfile.write(output)

            def log_message(self, *_):
                pass

        upstream = HTTPServer(("127.0.0.1", 0), Capture)
        thread = threading.Thread(target=upstream.serve_forever, kwargs={"poll_interval": .01}, daemon=True)
        thread.start()
        try:
            self.start(upstream=upstream.server_port)
            before = self.quiescent()
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
            try:
                for name, body, lengths in (CASES[0], CASES[1], CASES[2], CASES[4]):
                    with self.subTest(case=name):
                        result = json.loads(self.send_h1(connection, "/proxy/inspect", body, lengths))
                        self.assertEqual(result["content_lengths"], [str(len(body))])
                        self.assertEqual(result["transfer_encoding"], [])
                        self.assertEqual(base64.b64decode(result["body"]), body)
            finally:
                connection.close()
            self.assert_accounting(before, 0)
        finally:
            upstream.shutdown()
            upstream.server_close()
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())


if __name__ == "__main__":
    unittest.main()
