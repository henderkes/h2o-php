"""CGI authority metadata: python3 tests/test_authority.py [h2o-php binary]."""

import http.client
import json
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()


def port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class AuthorityTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="h2o-php-authority-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.port = port()
        self.tls_port = port()
        while self.tls_port == self.port:
            self.tls_port = port()
        self.process = self.log = None
        self.addCleanup(self.stop)
        (self.base / "index.php").write_text("""<?php
header('Content-Type: application/json');
echo json_encode(['name' => $_SERVER['SERVER_NAME'], 'port' => $_SERVER['SERVER_PORT'],
    'host' => $_SERVER['HTTP_HOST'], 'scheme' => $_SERVER['REQUEST_SCHEME'],
    'route' => $_SERVER['QUALIFICATION_AUTHORITY_ROUTE'], 'remote_addr' => $_SERVER['REMOTE_ADDR'],
    'remote_port' => $_SERVER['REMOTE_PORT']]);
""")
        certificate, key = self.base / "certificate.pem", self.base / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                        "-noenc", "-days", "1", "-subj", "/CN=authority.test", "-keyout", str(key),
                        "-out", str(certificate)], capture_output=True, check=True, timeout=10)
        key.chmod(0o600)
        source = f"""listen:
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
  threads: 1
  max-threads: 1
  collect-metrics: OFF
  request-timeout: 3s
  shutdown-timeout: 2s
php.root: {self.base}
hosts:
"""
        # H2O checks hosts in configured order and matches explicit port
        # bindings separately; keep those routing semantics unchanged.
        for host, route in (("fixture.test", "fixture"), ("exact.app.test", "exact"),
                            ("[::1]", "ipv6"), ("*.app.test", "wildcard")):
            for suffix in ("", f":{self.port}", f":{self.tls_port}"):
                source += f"""  {json.dumps(host + suffix)}:
    php.env:
      QUALIFICATION_AUTHORITY_ROUTE: {route}
    paths:
      /metrics:
        php.metrics: ON
      /:
        php_server: ON
"""
        self.config = self.base / "server.yml"
        self.config.write_text(source)
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

    def quiescent(self):
        deadline = time.monotonic() + 3
        while True:
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
            try:
                connection.request("GET", "/metrics", headers={"Host": "fixture.test"})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                values = json.loads(response.read())
            finally:
                connection.close()
            if all(values[key] == 0 for key in ("active_php_threads", "busy_workers", "queued_requests", "buffered_bytes")):
                return values
            self.assertLess(time.monotonic(), deadline, values)
            time.sleep(.01)

    def authorities(self, tls):
        listener = self.tls_port if tls else self.port
        default = "443" if tls else "80"
        padded = str(listener).zfill(8)
        return [("a.app.test", "a.app.test", default, "wildcard"),
                (f"b.app.test:{listener}", "b.app.test", str(listener), "wildcard"),
                ("A.App.Test", "A.App.Test", default, "wildcard"),
                ("exact.app.test", "exact.app.test", default, "exact"),
                (f"EXACT.APP.TEST:{padded}", "EXACT.APP.TEST", padded, "exact"),
                (f"[::1]:{listener}", "::1", str(listener), "ipv6"),
                ("[::1]", "[::1]", default, "ipv6"),
                (f"[::1]:{padded}", "::1", padded, "ipv6")]

    def verify_authorities(self, tls):
        before = self.quiescent()
        if tls:
            context = ssl._create_unverified_context()
            connection = http.client.HTTPSConnection("127.0.0.1", self.tls_port, context=context, timeout=5)
        else:
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        first_socket = None
        cases = self.authorities(tls)
        try:
            for authority, name, server_port, route in cases:
                with self.subTest(tls=tls, authority=authority):
                    connection.request("GET", "/index.php", headers={"Host": authority})
                    if first_socket is None:
                        first_socket = connection.sock
                    self.assertIs(connection.sock, first_socket, "connection reopened between authorities")
                    client_port = connection.sock.getsockname()[1]
                    response = connection.getresponse()
                    raw = response.read()
                    self.assertEqual(response.status, 200, raw)
                    self.assertFalse(response.will_close)
                    self.assertEqual(json.loads(raw), {"name": name, "port": server_port,
                        "host": authority, "scheme": "https" if tls else "http", "route": route,
                        "remote_addr": "127.0.0.1", "remote_port": str(client_port)})
        finally:
            connection.close()
        after = self.quiescent()
        self.assertEqual({key: after[key] - before[key] for key in ("completed", "failed", "timed_out")},
                         {"completed": len(cases), "failed": 0, "timed_out": 0})

    def test_plaintext_authorities_match_request_spelling(self):
        self.verify_authorities(False)

    def test_tls_authorities_use_https_default_and_lexical_port(self):
        self.verify_authorities(True)


if __name__ == "__main__":
    unittest.main()
