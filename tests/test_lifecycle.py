"""Lifecycle integration checks: python3 tests/test_lifecycle.py [binary]."""

from concurrent.futures import ThreadPoolExecutor
import http.client
import hashlib
import json
import os
from pathlib import Path
import socket
import signal
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()


def port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for(predicate, timeout=8):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("condition timed out")


class LifecycleTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="h2o-php-lifecycle-")
        self.base = Path(self.temp.name)
        self.address = self.base / "control.sock"
        self.config = self.base / "config.yml"
        self.port = port()
        self.process = None
        self.log = (self.base / "server.log").open("w+")
        self.worker = self.base / "worker.php"
        self.worker.write_text("""<?php
$boot = bin2hex(random_bytes(8));
while (frankenphp_handle_request(function () use ($boot) {
    if (isset($_GET['hold'])) {
        touch(__DIR__.'/entered');
        while (!is_file(__DIR__.'/release')) {
            usleep(1000); clearstatcache(true, __DIR__.'/release');
        }
    }
    header('Content-Type: application/json');
    echo json_encode(['boot' => $boot, 'version' => $_SERVER['VERSION']]);
}));
""")
        self.config.write_text(self.yaml("v1"))
        self.addCleanup(self.cleanup)

    def yaml(self, version, ports=None, worker=None):
        listeners = "".join(f"  - host: 127.0.0.1\n    port: {p}\n" for p in (ports or [self.port]))
        return f"""listen:
{listeners}num-threads: 1
php.runtime:
  threads: 1
  max-threads: 2
  startup-timeout: 2s
  shutdown-timeout: 4s
hosts:
  lifecycle.test:
    php.root: {self.base}
    php.env:
      VERSION: {version}
    paths:
      /:
        php.worker:
          file: {worker or self.worker}
          num: 1
          max-consecutive-failures: 1
"""

    def command(self, command, *arguments):
        return subprocess.run([str(BINARY), command, "--address", f"unix/{self.address}", *arguments],
                              capture_output=True, text=True, timeout=15)

    def start(self):
        self.log.seek(0)
        self.log.truncate()
        self.log.flush()
        self.process = subprocess.Popen([str(BINARY), "run", "--config", str(self.config),
                                         "--address", f"unix/{self.address}"],
                                        stdout=self.log, stderr=subprocess.STDOUT)

        def ready():
            if self.process.poll() is not None:
                self.fail((self.base / "server.log").read_text())
            return " ready\n" in (self.base / "server.log").read_text()
        wait_for(ready)

    def fetch(self, path="/", selected_port=None):
        connection = http.client.HTTPConnection("127.0.0.1", selected_port or self.port, timeout=10)
        try:
            connection.request("GET", path, headers={"Host": "lifecycle.test"})
            response = connection.getresponse()
            body = response.read()
            self.assertEqual(response.status, 200, body)
            return json.loads(body)
        finally:
            connection.close()

    def reload(self, yaml, *flags):
        self.config.write_text(yaml)
        return self.command("reload", "--config", str(self.config), *flags)

    def cleanup(self):
        (self.base / "release").touch()
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.log.close()
        self.temp.cleanup()

    def control_client(self):
        client = socket.socket(socket.AF_UNIX)
        client.settimeout(3)
        client.connect(str(self.address))
        self.addCleanup(client.close)
        return client

    def control_frame(self, command=1, body=b"", force=0, magic=0x48325031):
        return struct.pack("=IIII", magic, command, force, len(body)) + body

    def control_response(self, client):
        def read_exact(length):
            result = bytearray()
            while len(result) < length:
                part = client.recv(length - len(result))
                self.assertTrue(part, "control response ended early")
                result.extend(part)
            return bytes(result)
        status, length = struct.unpack("=iI", read_exact(8))
        self.assertLessEqual(length, 8192)
        return status, read_exact(length)

    def test_partial_control_clients_do_not_delay_signal_or_stop(self):
        self.config.write_text(self.yaml("v1").replace("shutdown-timeout: 4s", "shutdown-timeout: 200ms"))
        for shutdown in ("signal", "stop"):
            with self.subTest(shutdown=shutdown):
                self.start()
                partial_header = self.control_client()
                partial_header.sendall(b"H")
                partial_body = self.control_client()
                partial_body.sendall(struct.pack("=IIII", 0x48325031, 1, 0, 4096) + b"#")
                # Give the server a chance to accept both incomplete frames.
                time.sleep(0.05)
                started = time.monotonic()
                if shutdown == "signal":
                    self.process.terminate()
                else:
                    stopped = self.command("stop")
                    self.assertEqual(stopped.returncode, 0, stopped.stderr)
                self.assertEqual(self.process.wait(timeout=2), 0)
                self.assertLess(time.monotonic() - started, 1.5)
                self.assertEqual(partial_header.recv(1), b"")
                self.assertEqual(partial_body.recv(1), b"")
                partial_header.close()
                partial_body.close()
                self.assertFalse(self.address.exists())

    def test_fragmented_control_frame_and_other_clients_progress(self):
        self.start()
        stalled = [self.control_client() for _ in range(6)]
        for client in stalled:
            client.sendall(b"H")
        frame = self.control_frame(body=self.yaml("v2").encode())
        client = self.control_client()
        for offset in range(0, len(frame), 7):
            client.sendall(frame[offset:offset + 7])
            time.sleep(0.001)
        # A complete frame remains valid when the peer closes its write side.
        client.shutdown(socket.SHUT_WR)
        status, message = self.control_response(client)
        self.assertEqual(status, 0, message)
        self.assertEqual(self.fetch()["version"], "v2")
        self.assertEqual(self.command("reload", "-c", str(self.config)).returncode, 0)

    def test_control_malformed_frames_eof_and_buffer_limit(self):
        self.start()
        before = self.fetch()
        frames = (
            self.control_frame(magic=0),
            self.control_frame(command=3),
            self.control_frame(force=2),
            self.control_frame(command=2, body=b"x"),
            struct.pack("=IIII", 0x48325031, 1, 0, 16 * 1024 * 1024 + 1),
        )
        for frame in frames:
            with self.subTest(frame=frame):
                client = self.control_client()
                client.sendall(frame)
                self.assertEqual(self.control_response(client)[0], 64)
                client.close()
        for frame in (b"H", struct.pack("=IIII", 0x48325031, 1, 0, 100) + b"x"):
            client = self.control_client()
            client.sendall(frame)
            client.shutdown(socket.SHUT_WR)
            self.assertEqual(client.recv(1), b"")
            client.close()
        reserved = [self.control_client() for _ in range(2)]
        for client in reserved:
            client.sendall(struct.pack("=IIII", 0x48325031, 1, 0, 16 * 1024 * 1024))
        # Ordered complete headers reserve the aggregate input budget, while
        # their unfinished bodies must not prevent an immediate rejection.
        time.sleep(0.05)
        excess = self.control_client()
        excess.sendall(struct.pack("=IIII", 0x48325031, 1, 0, 1))
        status, message = self.control_response(excess)
        self.assertEqual(status, 75, message)
        self.assertIn(b"memory limit", message)
        for client in reserved:
            client.close()
        self.assertEqual(self.fetch(), before)
        result = self.command("reload", "-c", str(self.config), "--force")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_control_receive_deadline_is_absolute(self):
        self.start()
        client = self.control_client()
        client.settimeout(7)
        started = time.monotonic()
        client.sendall(b"H")
        time.sleep(4)
        client.sendall(b"2")
        self.assertEqual(client.recv(1), b"")
        elapsed = time.monotonic() - started
        self.assertGreaterEqual(elapsed, 4.5)
        self.assertLess(elapsed, 6)
        self.assertEqual(self.command("stop").returncode, 0)
        self.assertEqual(self.process.wait(timeout=2), 0)

    def test_blocked_configuration_validation_does_not_delay_shutdown(self):
        fifo = self.base / "include.fifo"
        os.mkfifo(fifo)
        for shutdown in ("signal", "stop"):
            with self.subTest(shutdown=shutdown):
                self.config.write_text(self.yaml("v1").replace("shutdown-timeout: 4s", "shutdown-timeout: 200ms"))
                self.start()
                writer = None
                with ThreadPoolExecutor(max_workers=2) as pool:
                    reloading = pool.submit(self.reload, f"!file {fifo}\n")

                    def probe_is_reading():
                        nonlocal writer
                        try:
                            writer = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                            return True
                        except OSError:
                            return False

                    try:
                        wait_for(probe_is_reading)
                        self.assertFalse(reloading.done())
                        self.assertEqual(self.fetch()["version"], "v1")
                        started = time.monotonic()
                        if shutdown == "signal":
                            self.process.terminate()
                        else:
                            stopped = pool.submit(self.command, "stop").result(timeout=2)
                            self.assertEqual(stopped.returncode, 0, stopped.stderr)
                        self.assertEqual(self.process.wait(timeout=2), 0)
                        self.assertLess(time.monotonic() - started, 1.5)
                        self.assertNotEqual(reloading.result(timeout=2).returncode, 0)
                    finally:
                        if writer is not None:
                            os.close(writer)
                self.assertFalse(self.address.exists())

    def test_queued_reload_progresses_after_blocked_validation_finishes(self):
        fifo = self.base / "include.fifo"
        os.mkfifo(fifo)
        self.start()
        with ThreadPoolExecutor(max_workers=2) as pool:
            first = pool.submit(self.reload, f"!file {fifo}\n")
            writer = None

            def probe_is_reading():
                nonlocal writer
                try:
                    writer = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                    return True
                except OSError:
                    return False

            try:
                wait_for(probe_is_reading)
                alternate = self.base / "next.yml"
                alternate.write_text(self.yaml("v3"))
                queued = pool.submit(self.command, "reload", "-c", str(alternate))
                self.assertEqual(self.fetch()["version"], "v1")
                os.write(writer, self.yaml("v2").encode())
            finally:
                if writer is not None:
                    os.close(writer)
            applied = first.result(timeout=5)
            self.assertEqual(applied.returncode, 0, applied.stderr)
            applied = queued.result(timeout=5)
            self.assertEqual(applied.returncode, 0, applied.stderr)
            self.assertEqual(self.fetch()["version"], "v3")

    def test_reload_applies_supplied_file_and_skips_identical_unless_forced(self):
        self.start()
        first = self.fetch()
        same = self.reload(self.yaml("v1"))
        self.assertEqual(same.returncode, 0, same.stderr)
        self.assertIn("unchanged", same.stdout)
        self.assertEqual(self.fetch()["boot"], first["boot"])
        forced = self.command("reload", "-c", str(self.config), "--force")
        self.assertEqual(forced.returncode, 0, forced.stderr)
        forced_boot = self.fetch()["boot"]
        self.assertNotEqual(forced_boot, first["boot"])
        wait_for(lambda: "retiring generation" in (self.base / "server.log").read_text())
        # A different source filename is submitted, not the startup path re-read.
        alternate = self.base / "alternate.yml"
        alternate.write_text(self.yaml("v2"))
        applied = self.command("reload", "--config", str(alternate))
        self.assertEqual(applied.returncode, 0, applied.stderr)
        self.assertEqual(self.fetch()["version"], "v2")
        stopped = self.command("stop")
        self.assertEqual(stopped.returncode, 0, stopped.stderr)
        self.assertEqual(self.process.wait(timeout=8), 0)
        self.assertFalse(self.address.exists())

    def test_invalid_config_and_failed_php_startup_preserve_active_worker(self):
        self.start()
        before = self.fetch()
        for bad in ("invalid: [", self.yaml("bad", worker=self.base / "missing.php"),
                    self.yaml("bad").replace("max-threads: 2", "max-threads: 1")):
            with self.subTest(config=bad):
                result = self.reload(bad)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.fetch(), before)
        broken = self.base / "broken.php"
        broken.write_text("<?php throw new RuntimeException('intentional lifecycle failure');")
        result = self.reload(self.yaml("bad", worker=broken))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.fetch(), before)
        self.assertEqual(self.reload(self.yaml("v2")).returncode, 0)
        self.assertEqual(self.fetch()["version"], "v2")

    def test_changed_listeners_and_bind_failure_rollback(self):
        self.start()
        other = port()
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", 0))
            occupied.listen()
            bad = self.reload(self.yaml("bad", [self.port, occupied.getsockname()[1]]))
            self.assertNotEqual(bad.returncode, 0)
            self.assertEqual(self.fetch()["version"], "v1")
        added = self.reload(self.yaml("v2", [self.port, other]))
        self.assertEqual(added.returncode, 0, added.stderr)
        self.assertEqual(self.fetch()["version"], "v2")
        self.assertEqual(self.fetch(selected_port=other)["version"], "v2")
        wait_for(lambda: "retiring generation" in (self.base / "server.log").read_text())
        removed = self.reload(self.yaml("v3", [other]))
        self.assertEqual(removed.returncode, 0, removed.stderr)
        self.assertEqual(self.fetch(selected_port=other)["version"], "v3")

    def test_reload_and_stop_drain_requests(self):
        self.start()
        with ThreadPoolExecutor(max_workers=1) as pool:
            held = pool.submit(self.fetch, "/?hold=1")
            wait_for(lambda: (self.base / "entered").exists())
            result = self.reload(self.yaml("v2"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(held.done())
            self.assertEqual(self.fetch()["version"], "v2")
            (self.base / "release").touch()
            self.assertEqual(held.result(timeout=5)["version"], "v1")
        wait_for(lambda: "retiring generation" in (self.base / "server.log").read_text())
        (self.base / "release").unlink()
        (self.base / "entered").unlink()
        with ThreadPoolExecutor(max_workers=1) as pool:
            held = pool.submit(self.fetch, "/?hold=1")
            wait_for(lambda: (self.base / "entered").exists())
            self.assertEqual(self.command("stop").returncode, 0)
            self.assertIsNone(self.process.poll())
            (self.base / "release").touch()
            self.assertEqual(held.result(timeout=5)["version"], "v2")
        self.assertEqual(self.process.wait(timeout=8), 0)

    def test_startup_failure_and_duplicate_instance_return_errors(self):
        self.start()
        duplicate = self.command("run", "--config", str(self.config))
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertEqual(self.fetch()["version"], "v1")
        self.assertTrue(self.address.exists())
        self.assertEqual(self.command("stop").returncode, 0)
        self.process.wait(timeout=8)
        self.config.write_text("invalid: [")
        failed = self.command("run", "--config", str(self.config))
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse(self.address.exists())
        self.assertNotEqual(self.command("stop").returncode, 0)

    def test_startup_timeout_preserves_active_config_and_allows_retry(self):
        self.start()
        before = self.fetch()
        blocked = self.base / "blocked.php"
        blocked.write_text("<?php while (true) { usleep(1000); }")
        result = self.reload(self.yaml("bad", worker=blocked).replace("startup-timeout: 2s", "startup-timeout: 150ms"))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.fetch(), before)
        result = self.reload(self.yaml("v2"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.fetch()["version"], "v2")

    def test_reload_queued_while_previous_generation_drains(self):
        self.start()
        with ThreadPoolExecutor(max_workers=2) as pool:
            held = pool.submit(self.fetch, "/?hold=1")
            wait_for(lambda: (self.base / "entered").exists())
            self.assertEqual(self.reload(self.yaml("v2")).returncode, 0)
            self.config.write_text(self.yaml("v3"))
            queued = pool.submit(self.command, "reload", "--config", str(self.config))
            time.sleep(0.1)
            self.assertFalse(queued.done())
            self.assertEqual(self.fetch()["version"], "v2")
            (self.base / "release").touch()
            self.assertEqual(held.result(timeout=5)["version"], "v1")
            result = queued.result(timeout=8)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.fetch()["version"], "v3")

    def test_resolved_includes_change_and_cosmetic_edits_are_noops(self):
        included = self.base / "included.yml"
        included.write_text(self.yaml("v1"))
        self.config.write_text(f"!file {included}\n")
        self.start()
        first = self.fetch()
        result = self.reload(f"# comment\n!file {included}\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.fetch(), first)
        included.write_text(self.yaml("v2"))
        result = self.command("reload", "-c", str(self.config))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.fetch()["version"], "v2")

    def test_unix_listener_survives_failed_and_successful_reloads(self):
        app_socket = self.base / "application.sock"

        def unix_config(version, worker=None):
            yaml = self.yaml(version, worker=worker)
            start = yaml.index("num-threads:")
            return f"listen:\n  type: unix\n  port: {app_socket}\n" + yaml[start:]

        def fetch_unix():
            connection = http.client.HTTPConnection("localhost", timeout=5)
            connection.sock = socket.socket(socket.AF_UNIX)
            connection.sock.connect(str(app_socket))
            try:
                connection.request("GET", "/", headers={"Host": "lifecycle.test"})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                return json.loads(response.read())
            finally:
                connection.close()

        self.config.write_text(unix_config("v1"))
        self.start()
        first = fetch_unix()
        broken = self.base / "broken.php"
        broken.write_text("<?php throw new RuntimeException('intentional lifecycle failure');")
        result = self.reload(unix_config("bad", broken))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(app_socket.exists())
        self.assertEqual(fetch_unix(), first)
        result = self.reload(unix_config("v2"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(app_socket.exists())
        self.assertEqual(fetch_unix()["version"], "v2")
        self.assertEqual(self.command("stop").returncode, 0)
        self.process.wait(timeout=8)
        self.assertFalse(app_socket.exists())

    def test_classic_stream_upload_and_static_across_reload(self):
        script = self.base / "index.php"
        script.write_text("""<?php
if (isset($_GET['stream'])) {
    echo str_repeat('a', 32768); flush(); touch(__DIR__.'/entered');
    while (!is_file(__DIR__.'/release')) {
        usleep(1000); clearstatcache(true, __DIR__.'/release');
    }
    echo str_repeat('b', 32768);
} elseif ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $input = file_get_contents('php://input');
    echo strlen($input).':'.hash('sha256', $input);
} else { echo $_SERVER['VERSION']; }
""")
        asset = b"static bytes\n" * 2048
        (self.base / "asset.txt").write_bytes(asset)

        def classic(version):
            yaml = self.yaml(version)
            return yaml[:yaml.index("        php.worker:")] + "        php_server: ON\n"

        def request(path, body=None):
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
            try:
                connection.request("POST" if body is not None else "GET", path, body,
                                   {"Host": "lifecycle.test"})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                return response.read()
            finally:
                connection.close()

        self.config.write_text(classic("v1"))
        self.start()
        with ThreadPoolExecutor(max_workers=1) as pool:
            stream = pool.submit(request, "/?stream=1")
            wait_for(lambda: (self.base / "entered").exists())
            result = self.reload(classic("v2"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(request("/"), b"v2")
            self.assertEqual(request("/asset.txt"), asset)
            payload = b"upload bytes" * 87382
            self.assertEqual(request("/", payload),
                             f"{len(payload)}:{hashlib.sha256(payload).hexdigest()}".encode())
            (self.base / "release").touch()
            self.assertEqual(stream.result(timeout=5), b"a" * 32768 + b"b" * 32768)

    def test_sigint_drains_and_legacy_master_still_reloads(self):
        self.process = subprocess.Popen([str(BINARY), "-m", "master", "-c", str(self.config)],
                                        stdout=self.log, stderr=subprocess.STDOUT)
        wait_for(lambda: " ready\n" in (self.base / "server.log").read_text())
        self.assertEqual(self.fetch()["version"], "v1")
        self.config.write_text(self.yaml("v2"))
        self.process.send_signal(signal.SIGHUP)
        wait_for(lambda: "generation 2 pid" in (self.base / "server.log").read_text()
                 and " ready\n" in (self.base / "server.log").read_text().split("generation 2 pid", 1)[1])
        self.assertEqual(self.fetch()["version"], "v2")
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=8), 0)
        self.start()
        self.process.send_signal(signal.SIGINT)
        self.assertEqual(self.process.wait(timeout=8), 0)
        self.assertFalse(self.address.exists())

    def test_stale_control_socket_and_initial_php_failure(self):
        with socket.socket(socket.AF_UNIX) as stale:
            stale.bind(str(self.address))
        self.start()
        self.assertEqual(self.address.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.fetch()["version"], "v1")
        self.assertEqual(self.command("stop").returncode, 0)
        self.process.wait(timeout=8)
        broken = self.base / "broken.php"
        broken.write_text("<?php throw new RuntimeException('intentional lifecycle failure');")
        self.config.write_text(self.yaml("bad", worker=broken))
        result = self.command("run", "--config", str(self.config))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.address.exists())

    def test_reload_refuses_non_socket_control_path(self):
        self.address.write_text("sentinel")
        result = self.command("reload", "--config", str(self.config))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.address.read_text(), "sentinel")

    def test_stop_cancels_replacement_startup(self):
        self.start()
        blocked = self.base / "blocked.php"
        blocked.write_text("<?php touch(__DIR__.'/booting'); while (true) { usleep(1000); }")
        self.config.write_text(self.yaml("bad", worker=blocked))
        with ThreadPoolExecutor(max_workers=1) as pool:
            reloading = pool.submit(self.command, "reload", "-c", str(self.config))
            wait_for(lambda: (self.base / "booting").exists())
            self.assertEqual(self.command("stop").returncode, 0)
            self.assertNotEqual(reloading.result(timeout=8).returncode, 0)
        self.assertEqual(self.process.wait(timeout=8), 0)
        self.assertFalse(self.address.exists())

    def test_waiting_fifo_error_log_does_not_delay_shutdown(self):
        fifo = self.base / "errors.fifo"
        os.mkfifo(fifo)
        for shutdown in ("signal", "stop"):
            with self.subTest(shutdown=shutdown):
                self.config.write_text(self.yaml("v1").replace("shutdown-timeout: 4s", "shutdown-timeout: 200ms"))
                self.start()
                before = self.fetch()
                with ThreadPoolExecutor(max_workers=2) as pool:
                    reloading = pool.submit(self.reload, f"error-log: {fifo}\n" + self.yaml("v2"))
                    wait_for(lambda: "waiting for replacement log" in (self.base / "server.log").read_text())
                    self.assertFalse(reloading.done())
                    self.assertEqual(self.fetch(), before)
                    started = time.monotonic()
                    if shutdown == "signal":
                        self.process.terminate()
                    else:
                        stopped = pool.submit(self.command, "stop").result(timeout=2)
                        self.assertEqual(stopped.returncode, 0, stopped.stderr)
                    self.assertEqual(self.process.wait(timeout=2), 0)
                    self.assertLess(time.monotonic() - started, 1.5)
                    self.assertNotEqual(reloading.result(timeout=2).returncode, 0)
                self.assertFalse(self.address.exists())

    def test_fifo_error_log_timeout_and_delayed_reader_success(self):
        fifo = self.base / "errors.fifo"
        os.mkfifo(fifo)
        self.start()
        before = self.fetch()
        started = time.monotonic()
        failed = self.reload(f"error-log: {fifo}\n" + self.yaml("bad").replace("startup-timeout: 2s", "startup-timeout: 150ms"))
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("timed out", failed.stderr.lower())
        self.assertLess(time.monotonic() - started, 1)
        self.assertEqual(self.fetch(), before)
        logs = bytearray()
        stop = threading.Event()
        marker = "waiting for replacement log"
        count = (self.base / "server.log").read_text().count(marker)
        with ThreadPoolExecutor(max_workers=2) as pool:
            reloading = pool.submit(self.reload, f"error-log: {fifo}\n" + self.yaml("v2"))
            wait_for(lambda: (self.base / "server.log").read_text().count(marker) > count)
            self.assertFalse(reloading.done())
            reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)

            def drain():
                while not stop.is_set():
                    try:
                        data = os.read(reader, 65536)
                        if data:
                            logs.extend(data)
                    except BlockingIOError:
                        pass
                    time.sleep(0.005)

            draining = pool.submit(drain)
            try:
                result = reloading.result(timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.fetch()["version"], "v2")
                wait_for(lambda: b" ready\n" in logs)
                self.assertEqual(self.command("stop").returncode, 0)
                self.assertEqual(self.process.wait(timeout=2), 0)
            finally:
                stop.set()
                draining.result(timeout=2)
                os.close(reader)

    def test_reload_queued_behind_waiting_log_sleeps_then_progresses(self):
        self.start()
        stats = Path(f"/proc/{self.process.pid}/stat")
        if not stats.exists():
            self.skipTest("process CPU accounting requires Linux procfs")
        fifo = self.base / "errors.fifo"
        os.mkfifo(fifo)
        ticks = os.sysconf("SC_CLK_TCK")

        def cpu_seconds():
            fields = stats.read_text().rsplit(")", 1)[1].split()
            return (int(fields[11]) + int(fields[12])) / ticks

        stop = threading.Event()
        with ThreadPoolExecutor(max_workers=3) as pool:
            first = pool.submit(self.reload, f"error-log: {fifo}\n" + self.yaml("v2"))
            wait_for(lambda: "waiting for replacement log" in (self.base / "server.log").read_text())
            alternate = self.base / "next.yml"
            alternate.write_text(self.yaml("v3"))
            queued = pool.submit(self.command, "reload", "-c", str(alternate))
            time.sleep(0.05)
            self.assertFalse(queued.done())
            before = cpu_seconds()
            started = time.monotonic()
            time.sleep(0.3)
            elapsed = time.monotonic() - started
            # Pending log setup must sleep while a later reload is queued.
            # Allow three accounting ticks and substantial scheduling noise.
            self.assertLess(cpu_seconds() - before, elapsed / 2 + 3 / ticks)
            reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)

            def drain():
                while not stop.is_set():
                    try:
                        os.read(reader, 65536)
                    except BlockingIOError:
                        pass
                    time.sleep(0.005)

            draining = pool.submit(drain)
            try:
                result = first.result(timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = queued.result(timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.fetch()["version"], "v3")
            finally:
                stop.set()
                draining.result(timeout=2)
                os.close(reader)

    def test_unix_error_log_connects_and_streams(self):
        path = self.base / "errors.sock"
        self.start()
        logs = bytearray()
        stop = threading.Event()
        with socket.socket(socket.AF_UNIX) as listener, socket.socket(socket.AF_UNIX) as queued:
            listener.bind(str(path))
            listener.listen(0)
            listener.settimeout(3)
            queued.connect(str(path))
            with ThreadPoolExecutor(max_workers=2) as pool:
                reloading = pool.submit(self.reload, f"error-log: {path}\n" + self.yaml("v2"))
                wait_for(lambda: "waiting for replacement log" in (self.base / "server.log").read_text())
                self.assertFalse(reloading.done())
                first, _ = listener.accept()
                first.close()
                queued.close()
                connection, _ = listener.accept()
                connection.settimeout(0.1)

                def drain():
                    while not stop.is_set():
                        try:
                            data = connection.recv(65536)
                            if not data:
                                return
                            logs.extend(data)
                        except socket.timeout:
                            pass

                draining = pool.submit(drain)
                try:
                    result = reloading.result(timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(self.fetch()["version"], "v2")
                    wait_for(lambda: b" ready\n" in logs)
                    self.assertEqual(self.command("stop").returncode, 0)
                    self.assertEqual(self.process.wait(timeout=2), 0)
                finally:
                    stop.set()
                    draining.result(timeout=2)
                    connection.close()

    def test_full_unix_log_backlog_does_not_block_control(self):
        path = self.base / "errors.sock"
        self.start()
        before = self.fetch()
        with socket.socket(socket.AF_UNIX) as listener, socket.socket(socket.AF_UNIX) as queued:
            listener.bind(str(path))
            listener.listen(0)
            queued.connect(str(path))
            with ThreadPoolExecutor(max_workers=2) as pool:
                reloading = pool.submit(self.reload, f"error-log: {path}\n" + self.yaml("v2"))
                wait_for(lambda: "waiting for replacement log" in (self.base / "server.log").read_text())
                self.assertFalse(reloading.done())
                self.assertEqual(self.fetch(), before)
                stopped = pool.submit(self.command, "stop").result(timeout=2)
                self.assertEqual(stopped.returncode, 0, stopped.stderr)
                self.assertEqual(self.process.wait(timeout=2), 0)
                self.assertNotEqual(reloading.result(timeout=2).returncode, 0)

    def test_error_log_changes_and_failed_log_open(self):
        self.start()
        errors = self.base / "errors.log"
        result = self.reload(f"error-log: {errors}\n" + self.yaml("v2"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.fetch()["version"], "v2")
        wait_for(lambda: " ready\n" in errors.read_text())
        result = self.reload(self.yaml("v3"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.fetch()["version"], "v3")
        wait_for(lambda: "generation 3 pid" in (self.base / "server.log").read_text())
        result = self.reload(f"error-log: {self.base / 'missing' / 'errors.log'}\n" + self.yaml("bad"))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.fetch()["version"], "v3")

    def test_process_settings_and_acme_fail_explicitly(self):
        self.start()
        before = self.fetch()
        result = self.reload(f"pid-file: {self.base / 'new.pid'}\n" + self.yaml("bad"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires a restart", result.stderr)
        self.assertEqual(self.fetch(), before)
        result = self.reload("acme: {}\n" + self.yaml("bad"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ACME loader requires -m master", result.stderr)
        self.assertEqual(self.fetch(), before)

    def test_tls_certificate_replacement_with_forced_reload(self):
        certificate, key = self.base / "cert.pem", self.base / "key.pem"

        def issue(name):
            subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                            "-noenc", "-keyout", str(key), "-out", str(certificate), "-days", "1",
                            "-subj", f"/CN={name}"], check=True, capture_output=True)
            return ssl.PEM_cert_to_DER_cert(certificate.read_text())

        def fetch_tls():
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=context, timeout=5)
            try:
                connection.connect()
                actual_cert = connection.sock.getpeercert(binary_form=True)
                connection.request("GET", "/", headers={"Host": "lifecycle.test"})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                return actual_cert, json.loads(response.read())
            finally:
                connection.close()

        first = issue("first.test")
        yaml = self.yaml("v1").replace(f"    port: {self.port}\n", f"""    port: {self.port}
    ssl:
      certificate-file: {certificate}
      key-file: {key}
      ocsp-update-interval: 0
      neverbleed: ON
""")
        self.config.write_text(yaml)
        self.start()
        actual, before = fetch_tls()
        self.assertEqual(actual, first)

        def fetch_http2(path):
            result = subprocess.run(["curl", "--silent", "--show-error", "--fail", "--http2", "--insecure",
                                     "--noproxy", "*", "-H", "Host: lifecycle.test", "--write-out", "\n%{http_version}",
                                     f"https://127.0.0.1:{self.port}{path}"],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            body, protocol = result.stdout.rsplit("\n", 1)
            self.assertEqual(protocol, "2")
            return json.loads(body)

        with ThreadPoolExecutor(max_workers=1) as pool:
            held = pool.submit(fetch_http2, "/?hold=1")
            wait_for(lambda: (self.base / "entered").exists())
            second = issue("second.test")
            self.assertNotEqual(first, second)
            result = self.command("reload", "-c", str(self.config), "--force")
            self.assertEqual(result.returncode, 0, result.stderr)
            actual, after = fetch_tls()
            self.assertEqual(actual, second)
            self.assertEqual(after["version"], "v1")
            self.assertNotEqual(after["boot"], before["boot"])
            self.assertEqual(fetch_http2("/")["boot"], after["boot"])
            self.assertFalse(held.done())
            (self.base / "release").touch()
            self.assertEqual(held.result(timeout=5), before)

    def test_parallel_tasks_before_and_after_reload(self):
        self.worker.write_text("""<?php
while (frankenphp_handle_request(function () {
    $future = parallel\\run(static function () {
        return hash('sha256', 'parallel lifecycle');
    });
    header('Content-Type: application/json');
    echo json_encode(['version' => $_SERVER['VERSION'], 'digest' => $future->value()]);
}));
""")
        self.start()
        expected = hashlib.sha256(b"parallel lifecycle").hexdigest()
        self.assertEqual(self.fetch(), {"version": "v1", "digest": expected})
        result = self.reload(self.yaml("v2"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.fetch(), {"version": "v2", "digest": expected})


if __name__ == "__main__":
    unittest.main()
