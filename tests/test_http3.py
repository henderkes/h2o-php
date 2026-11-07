"""QUIC listener ownership regression: python3 tests/test_http3.py [binary]."""

import hashlib
import http.client
import os
from pathlib import Path
import re
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()
PAYLOAD = bytes(range(256)) * 256
ASSET = b"exact HTTP/3 static bytes\n" * 2048


def shared_port():
    for _ in range(30):
        with socket.socket() as tcp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            tcp.bind(("127.0.0.1", 0))
            selected = tcp.getsockname()[1]
            try:
                udp.bind(("127.0.0.1", selected))
                return selected
            except OSError:
                pass
    raise AssertionError("could not find a free TCP/UDP port")


class HTTP3ListenerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        version = subprocess.check_output(["curl", "--version"], text=True)
        if not re.search(r"^Features:.*\bHTTP3\b", version, re.MULTILINE):
            raise unittest.SkipTest("curl with HTTP/3 support is required")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="h2o-php-http3-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.certificate = self.base / "certificate.pem"
        self.key = self.base / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                        "-noenc", "-days", "1", "-subj", "/CN=h3.test", "-addext",
                        "subjectAltName=DNS:h3.test,IP:127.0.0.1", "-out", str(self.certificate),
                        "-keyout", str(self.key)], check=True, capture_output=True, timeout=10)
        self.key.chmod(0o600)
        (self.base / "index.php").write_text("""<?php
$input = file_get_contents('php://input');
header('Content-Type: text/plain');
echo 'h3:'.strlen($input).':'.hash('sha256', $input);
""")
        (self.base / "asset.bin").write_bytes(ASSET)
        (self.base / "upload.bin").write_bytes(PAYLOAD)

    def config(self, network_threads, quic_threads):
        ssl_mapping = f"""    ssl:
      certificate-file: {self.certificate}
      key-file: {self.key}
      minimum-version: TLSv1.3
      ocsp-update-interval: 0
      neverbleed: OFF
"""
        return f"""listen:
  - host: 127.0.0.1
    port: {self.port}
""" + ssl_mapping + f"""  - host: 127.0.0.1
    port: {self.port}
    type: quic
""" + ssl_mapping + f"""num-threads: {network_threads}
num-quic-threads: {quic_threads}
php.runtime:
  threads: 1
  max-threads: 1
  startup-timeout: 2s
  shutdown-timeout: 2s
php.root: {self.base}
hosts:
  h3.test:
    paths:
      /:
        php_server: ON
"""

    def wait_ready(self, process, log):
        context = ssl.create_default_context(cafile=str(self.certificate))
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.assertIsNone(process.poll(), log.read_text())
            connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=context, timeout=1)
            try:
                connection.request("GET", "/asset.bin", headers={"Host": "h3.test"})
                response = connection.getresponse()
                if response.status == 200 and response.read() == ASSET:
                    return
            except (OSError, http.client.HTTPException):
                pass
            finally:
                connection.close()
            time.sleep(0.01)
        self.fail("server did not become ready:\n" + log.read_text())

    def udp_endpoints(self, pid):
        """Count owned kernel UDP endpoints, not duplicate fd wrappers."""
        owned = set()
        for path in (Path("/proc") / str(pid) / "fd").iterdir():
            try:
                target = os.readlink(path)
            except FileNotFoundError:
                continue
            match = re.fullmatch(r"socket:\[(\d+)\]", target)
            if match:
                owned.add(match[1])
        endpoints = set()
        for line in (Path("/proc") / str(pid) / "net/udp").read_text().splitlines()[1:]:
            fields = line.split()
            if int(fields[1].rsplit(":", 1)[1], 16) == self.port and fields[9] in owned:
                endpoints.add(fields[9])
        return endpoints

    def h3_request(self, path, *, upload=False):
        output = self.base / "response.bin"
        command = ["curl", "--silent", "--show-error", "--fail-with-body", "--http3-only",
                   "--noproxy", "*", "--connect-timeout", "2", "--max-time", "5",
                   "--cacert", str(self.certificate), "--resolve", f"h3.test:{self.port}:127.0.0.1",
                   "--header", "Accept-Encoding: identity", "--header", "Expect:",
                   "--output", str(output), "--write-out", "%{http_version}"]
        if upload:
            command += ["--data-binary", "@" + str(self.base / "upload.bin"),
                        "--header", "Content-Type: application/octet-stream"]
        command.append(f"https://h3.test:{self.port}{path}")
        response = subprocess.run(command, capture_output=True, text=True, timeout=7)
        self.assertEqual(response.returncode, 0, response.stderr)
        self.assertEqual(response.stdout, "3")
        return output.read_bytes()

    def test_partial_and_full_quic_thread_budgets_across_invocations(self):
        # Direct worker mode has one reuseport endpoint per active QUIC thread.
        # The native supervisors inherit one shared UDP endpoint; inactive
        # network threads must not create another endpoint in either path.
        for mode, network, quic in (("direct", 4, 1), ("direct", 4, 4), ("direct", 1, 1),
                                    ("run", 4, 1), ("master", 4, 1)):
            with self.subTest(mode=mode, network_threads=network, quic_threads=quic):
                self.port = shared_port()
                config = self.base / "server.yml"
                config.write_text(self.config(network, quic))
                address = self.base / "control.sock"
                command = [str(BINARY), "-c", str(config)]
                if mode == "run":
                    command = [str(BINARY), "run", "-c", str(config), "--address", f"unix/{address}"]
                elif mode == "master":
                    command = [str(BINARY), "-m", "master", "-c", str(config)]
                log = self.base / "server.log"
                with log.open("w") as output:
                    process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
                    try:
                        self.wait_ready(process, log)
                        serving_pid = process.pid
                        if mode != "direct":
                            deadline = time.monotonic() + 5
                            ready = None
                            while time.monotonic() < deadline:
                                ready = re.search(r"generation 1 pid (\d+) ready", log.read_text())
                                if ready:
                                    break
                                self.assertIsNone(process.poll(), log.read_text())
                                time.sleep(0.01)
                            self.assertIsNotNone(ready, log.read_text())
                            serving_pid = int(ready.group(1))
                        if sys.platform.startswith("linux"):
                            expected = quic if mode == "direct" else 1
                            self.assertEqual(len(self.udp_endpoints(serving_pid)), expected,
                                             "each bound UDP endpoint needs an active QUIC reader")
                        for _ in range(4):
                            # Each curl invocation creates a new QUIC flow. A
                            # favorable reuseport hash cannot hide a dead socket.
                            self.assertEqual(self.h3_request("/asset.bin"), ASSET)
                            self.assertEqual(self.h3_request("/index.php", upload=True),
                                             f"h3:{len(PAYLOAD)}:{hashlib.sha256(PAYLOAD).hexdigest()}".encode())
                    finally:
                        if process.poll() is None:
                            process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=2)
                            self.fail("HTTP/3 server required forced shutdown:\n" + log.read_text())
                        self.assertEqual(process.returncode, 0, log.read_text())
                if mode == "run":
                    self.assertFalse(address.exists())


if __name__ == "__main__":
    unittest.main()
