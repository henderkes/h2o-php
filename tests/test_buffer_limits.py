"""Global bridge budget checks: python3 tests/test_buffer_limits.py [binary]."""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()
EXPECTED = b"".join(bytes([index]) * 8192 for index in range(64))
EXPECTED_SHA256 = hashlib.sha256(EXPECTED).hexdigest()


class GlobalBufferLimitTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="h2o-php-global-budget-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.config = self.base / "server.yml"
        self.log_path = self.base / "server.log"
        self.process = None
        self.log = None
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        (self.base / "stream.php").write_text("""<?php
header('Content-Type: application/octet-stream');
header('Content-Length: 524288');
for ($i = 0; $i < 64; ++$i) {
    echo str_repeat(chr($i), 8192);
    flush();
}
""")
        self.addCleanup(self.stop)

    def yaml(self, budget, spelling="runtime", response_limit=4096):
        runtime = "  max-buffered-global: " + str(budget) + "\n" if spelling == "runtime" else ""
        if spelling == "underscore":
            runtime = "  max_buffered_global: " + str(budget) + "\n"
        scalar = "php.max-buffered-global: " + str(budget) + "\n" if spelling == "scalar" else ""
        return f"""listen:
  host: 127.0.0.1
  port: {self.port}
num-threads: 2
php.runtime:
  threads: 2
  max-threads: 2
  request-timeout: 5s
  shutdown-timeout: 2s
  max-buffered-body: 4096
  max-buffered-response: {response_limit}
{runtime}{scalar}php.root: {self.base}
php.ini:
  output_buffering: "0"
hosts:
  budget.test:
    paths:
      /metrics:
        php.metrics: ON
      /:
        php_server: ON
"""

    def command(self, *arguments):
        return subprocess.run([str(BINARY), *arguments], capture_output=True, text=True, timeout=10)

    def test_too_small_global_budgets_fail_both_validation_commands(self):
        for spelling in ("runtime", "underscore", "scalar"):
            for budget in (4096, 8192, 16383):
                self.config.write_text(self.yaml(budget, spelling))
                for command in (("-t",), ("effective-config",)):
                    with self.subTest(spelling=spelling, budget=budget, command=command):
                        result = self.command(*command, "-c", str(self.config))
                        self.assertNotEqual(result.returncode, 0, result.stdout)
                        self.assertIn("max-buffered-global", result.stderr)
                        self.assertIn("at least 16384 bytes", result.stderr)

    def test_minimum_global_and_existing_per_request_minima_validate(self):
        for spelling in ("runtime", "underscore", "scalar"):
            with self.subTest(spelling=spelling):
                self.config.write_text(self.yaml(16384, spelling))
                result = self.command("-t", "-c", str(self.config))
                self.assertEqual(result.returncode, 0, result.stderr)
                result = self.command("effective-config", "-c", str(self.config))
                self.assertEqual(result.returncode, 0, result.stderr)
                values = json.loads(result.stdout)
                self.assertFalse(values["php_engine_started"])
                runtime = values["runtime"]
                self.assertEqual(runtime["max_buffered_global"], 16384)
                self.assertEqual(runtime["max_buffered_body"], 4096)
                self.assertEqual(runtime["max_buffered_response"], 4096)

    def metrics(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request("GET", "/metrics", headers={"Host": "budget.test"})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            return json.loads(response.read())
        finally:
            connection.close()

    def quiescent(self):
        deadline = time.monotonic() + 3
        while True:
            values = self.metrics()
            if all(values[key] == 0 for key in ("active_php_threads", "busy_workers", "buffered_bytes", "queued_requests")):
                return values
            self.assertLess(time.monotonic(), deadline, values)
            time.sleep(0.01)

    def start(self):
        # A response may queue more than the entire shared budget, so progress
        # depends on releasing global credit after owning-loop sends complete.
        self.config.write_text(self.yaml(16384, response_limit=32768))
        self.log = self.log_path.open("w")
        self.process = subprocess.Popen([str(BINARY), "-c", str(self.config)],
                                        stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.assertIsNone(self.process.poll(), self.log_path.read_text())
            try:
                self.quiescent()
                return
            except (OSError, http.client.HTTPException):
                time.sleep(0.01)
        self.fail("server did not start:\n" + self.log_path.read_text())

    def streams_on_one_connection(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        first_socket = None
        try:
            for _ in range(2):
                connection.request("GET", "/stream.php", headers={"Host": "budget.test", "Accept-Encoding": "identity"})
                if first_socket is None:
                    first_socket = connection.sock
                self.assertIs(connection.sock, first_socket, "client reopened its connection")
                response = connection.getresponse()
                payload = response.read()
                self.assertEqual(response.status, 200)
                self.assertFalse(response.will_close)
                self.assertEqual(len(payload), len(EXPECTED))
                self.assertEqual(hashlib.sha256(payload).hexdigest(), EXPECTED_SHA256)
                self.assertEqual(payload, EXPECTED)
        finally:
            connection.close()

    def test_minimum_budget_streams_exact_bytes_with_one_and_eight_connections(self):
        self.start()
        before = self.quiescent()
        for connections in (1, 8):
            with self.subTest(connections=connections):
                with ThreadPoolExecutor(max_workers=connections) as pool:
                    list(pool.map(lambda _: self.streams_on_one_connection(), range(connections)))
                self.quiescent()
        after = self.quiescent()
        self.assertEqual(after["completed"] - before["completed"], 18)
        self.assertEqual(after["failed"] - before["failed"], 0)
        self.assertEqual(after["timed_out"] - before["timed_out"], 0)
        self.assertEqual(after["threads"], 2)

    def stop(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
                self.fail("minimum-budget shutdown hung:\n" + self.log_path.read_text())
            self.assertEqual(self.process.returncode, 0, self.log_path.read_text())
            self.process = None
        if self.log is not None:
            self.log.close()
            self.log = None


if __name__ == "__main__":
    unittest.main()
