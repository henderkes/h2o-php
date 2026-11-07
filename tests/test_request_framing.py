"""HTTP request framing regression: python3 tests/test_request_framing.py [h2o-php binary]."""

import http.client
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()


class RequestFramingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="h2o-php-framing-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.base = Path(cls.temp.name)
        cls.root = cls.base / "public"
        cls.root.mkdir()
        (cls.root / "index.php").write_text(
            "<?php header('Content-Type: text/plain'); "
            "echo $_SERVER['REQUEST_METHOD'], ' ', $_SERVER['REQUEST_URI'], ' ', $_SERVER['HTTP_HOST'];"
        )
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            cls.port = listener.getsockname()[1]
        cls.config = cls.base / "h2o.yml"
        cls.config.write_text(
            f"""listen:
  host: 127.0.0.1
  port: {cls.port}
num-threads: 1
php.runtime:
  threads: 1
  max-threads: 1
hosts:
  default.test:
    php.root: {cls.root}
    paths:
      /:
        php_server: ON
"""
        )
        cls.log = (cls.base / "server.log").open("w+")
        cls.addClassCleanup(cls.log.close)
        cls.server = subprocess.Popen(
            [str(BINARY), "-c", str(cls.config)],
            stdout=cls.log,
            stderr=subprocess.STDOUT,
        )
        cls.addClassCleanup(cls.stop_server)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if cls.server.poll() is not None:
                break
            try:
                status, _, _ = cls.request("/")
                if status == 200:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.05)
        cls.log.seek(0)
        raise RuntimeError(f"Server did not start:\n{cls.log.read()}")

    @classmethod
    def stop_server(cls):
        if cls.server.poll() is None:
            cls.server.terminate()
            try:
                cls.server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.server.kill()
                cls.server.wait()

    @classmethod
    def request(cls, path):
        connection = http.client.HTTPConnection("127.0.0.1", cls.port, timeout=5)
        try:
            connection.request("GET", path, headers={"Host": "default.test"})
            response = connection.getresponse()
            return response.status, response.getheader("Content-Type"), response.read()
        finally:
            connection.close()

    @classmethod
    def raw(cls, payload, timeout=5):
        """Send raw bytes; return everything the server sends back before closing."""
        with socket.create_connection(("127.0.0.1", cls.port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(payload)
            chunks = []
            while True:
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    break
                chunks.append(chunk)
        return b"".join(chunks)

    def test_conflicting_duplicate_content_length_is_rejected(self):
        body = b"hello"
        payload = (
            b"POST / HTTP/1.1\r\n"
            b"Host: default.test\r\n"
            b"Content-Length: 5\r\n"
            b"Content-Length: 200\r\n"
            b"Content-Type: text/plain\r\n"
            b"\r\n" + body +
            b"GET / HTTP/1.1\r\n"
            b"Host: default.test\r\n"
            b"\r\n"
        )
        response = self.raw(payload)
        self.assertIn(b" 400 ", response)
        self.assertNotIn(b" 200 ", response)
        self.assertEqual(response.count(b"HTTP/1.1 "), 1)

    def test_identical_duplicate_content_length_is_accepted(self):
        body = b"hello"
        payload = (
            b"POST / HTTP/1.1\r\n"
            b"Host: default.test\r\n"
            b"Content-Length: 5\r\n"
            b"Content-Length: 5\r\n"
            b"Content-Type: text/plain\r\n"
            b"\r\n" + body
        )
        response = self.raw(payload)
        self.assertIn(b" 200 ", response)
        self.assertIn(b"POST / default.test", response)

    def test_duplicate_host_is_rejected(self):
        payload = (
            b"GET / HTTP/1.1\r\n"
            b"Host: default.test\r\n"
            b"Host: other.test\r\n"
            b"\r\n"
        )
        response = self.raw(payload)
        self.assertIn(b" 400 ", response)
        self.assertNotIn(b" 200 ", response)

    def test_ordinary_request_still_works(self):
        status, _, body = self.request("/")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"GET / default.test")


if __name__ == "__main__":
    unittest.main()
