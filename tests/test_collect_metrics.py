"""Optional collector checks: python3 tests/test_collect_metrics.py [binary]."""

from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import json
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()
BODY = bytes(range(256)) * 257
DIGEST = hashlib.sha256(BODY).hexdigest()
QUIESCENT = ("active_php_threads", "busy_workers", "queued_requests", "buffered_bytes")


class MetricsServer:
    def __init__(self, test, policy=None, *, workers=True, queue_limit=0, global_budget=16384):
        self.test = test
        self.temp = tempfile.TemporaryDirectory(prefix="h2o-php-collect-metrics-")
        test.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.config = self.base / "server.yml"
        self.log_path = self.base / "server.log"
        self.process = self.log = None
        self.workers = workers
        self.enabled = policy != "OFF"
        test.addCleanup(self.stop)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        (self.base / "worker.php").write_text(r'''<?php
$boot = bin2hex(random_bytes(8));
$count = 0;
file_put_contents(__DIR__.'/boots', $boot."\n", FILE_APPEND | LOCK_EX);
while (frankenphp_handle_request(function () use ($boot, &$count) {
    if (isset($_GET['hold'])) {
        $tag = $_GET['hold'];
        touch(__DIR__.'/entered-'.$tag);
        $deadline = microtime(true) + 8;
        do {
            clearstatcache(true, __DIR__.'/release');
            if (is_file(__DIR__.'/release')) break;
            if (microtime(true) >= $deadline) throw new RuntimeException('hold timed out');
            usleep(1000);
        } while (true);
    }
    header('Content-Type: application/json');
    echo json_encode(['kind' => 'worker', 'sha256' => hash('sha256', file_get_contents('php://input')),
        'boot' => $boot, 'count' => ++$count, 'cached' => opcache_is_script_cached(__FILE__)]);
}));
''')
        (self.base / "classic.php").write_text(r'''<?php
$body = file_get_contents('php://input');
header('Content-Type: application/octet-stream');
header('Content-Length: '.strlen($body));
header('X-OPcache-Cached: '.(opcache_is_script_cached(__FILE__) ? '1' : '0'));
echo strrev($body);
''')
        (self.base / "reset.php").write_text(r'''<?php
header('Content-Type: application/json');
$accepted = opcache_reset();
$status = opcache_get_status(false);
echo json_encode(['accepted' => $accepted, 'pending' => $status['restart_pending']]);
''')
        policy_line = "" if policy is None else f"  collect-metrics: {policy}\n"
        worker_route = "" if not workers else f"""      /worker:
        php.worker:
          file: {self.base / 'worker.php'}
          num: 2
"""
        self.config.write_text(f"""listen:
  host: 127.0.0.1
  port: {self.port}
num-threads: 2
php.runtime:
  threads: 2
  max-threads: {4 if workers else 2}
  queue-limit: {queue_limit}
  request-timeout: 10s
  shutdown-timeout: 2s
  max-buffered-body: 4096
  max-buffered-response: 4096
  max-buffered-global: {global_budget}
{policy_line}php.ini:
  opcache.enable: "1"
  opcache.file_update_protection: "0"
  opcache.validate_timestamps: "0"
  opcache.jit: "off"
  opcache.jit_buffer_size: "0"
hosts:
  metrics.test:
    php.root: {self.base}
    paths:
      /metrics:
        php.metrics: ON
      /prometheus:
        php.metrics: {{format: prometheus}}
      /restart:
        php.restart: ON
{worker_route}      /:
        php_server: ON
""")

    def command(self, *arguments):
        return subprocess.run([str(BINARY), *arguments, "-c", str(self.config)],
                              capture_output=True, text=True, timeout=10)

    def start(self):
        self.log = self.log_path.open("w")
        self.process = subprocess.Popen([str(BINARY), "-c", str(self.config)],
                                        stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.test.assertIsNone(self.process.poll(), self.log_path.read_text())
            try:
                metrics = self.quiescent()
                if metrics["ready_workers"] == (2 if self.workers else 0):
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.01)
        self.test.fail("server did not start:\n" + self.log_path.read_text())

    def request(self, path, method="GET", body=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            connection.request(method, path, body=body, headers={"Host": "metrics.test", "Accept-Encoding": "identity"})
            response = connection.getresponse()
            return response.status, {name.lower(): value for name, value in response.getheaders()}, response.read()
        finally:
            connection.close()

    def metrics(self):
        status, _, body = self.request("/metrics")
        self.test.assertEqual(status, 200)
        return json.loads(body)

    def quiescent(self):
        deadline = time.monotonic() + 3
        while True:
            metrics = self.metrics()
            if all(metrics[key] == 0 for key in QUIESCENT):
                return metrics
            self.test.assertLess(time.monotonic(), deadline, metrics)
            time.sleep(0.01)

    def prometheus(self):
        status, headers, body = self.request("/prometheus")
        self.test.assertEqual(status, 200, body)
        self.test.assertTrue(headers["content-type"].startswith("text/plain; version=0.0.4"))
        text = body.decode()
        if not self.enabled:
            self.test.assertNotIn("frankenphp_", text)
            self.test.assertEqual(text, "")
        values = defaultdict(float)
        for line in text.splitlines():
            if not line or line.startswith("#"):
                continue
            match = re.fullmatch(r'(frankenphp_[a-z_]+)(?:\{[^\n]*\})? ([^ ]+)', line)
            self.test.assertIsNotNone(match, line)
            values[match[1]] += float(match[2])
        return values

    def boots(self):
        path = self.base / "boots"
        return path.read_text().splitlines() if path.exists() else []

    def stop(self):
        if self.process is not None:
            (self.base / "release").touch()
            if self.process.poll() is None:
                self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
                self.test.fail("shutdown hung:\n" + self.log_path.read_text())
            self.test.assertEqual(self.process.returncode, 0, self.log_path.read_text())
            self.process = None
        if self.log is not None:
            self.log.close()
            self.log = None


class CollectMetricsTest(unittest.TestCase):
    def test_default_boolean_values_and_aliases_are_effective(self):
        server = MetricsServer(self, workers=False)
        original = server.config.read_text()
        cases = [(None, None, True), ("collect-metrics", "ON", True), ("collect-metrics", "OFF", False),
                 ("collect_metrics", "true", True), ("collect_metrics", "false", False),
                 ("php.collect-metrics", "1", True), ("php.collect-metrics", "0", False),
                 ("php.collect_metrics", "ON", True), ("php.collect_metrics", "OFF", False)]
        for key, value, enabled in cases:
            with self.subTest(key=key, value=value):
                addition = "" if key is None else (f"{key}: {value}\n" if key.startswith("php.") else f"  {key}: {value}\n")
                server.config.write_text(original.replace("php.ini:\n", addition + "php.ini:\n"))
                result = server.command("-t")
                self.assertEqual(result.returncode, 0, result.stderr)
                result = server.command("effective-config")
                self.assertEqual(result.returncode, 0, result.stderr)
                effective = json.loads(result.stdout)
                self.assertFalse(effective["php_engine_started"])
                self.assertIs(effective["runtime"]["collect_metrics"], enabled)

    def test_invalid_values_and_conflicting_aliases_are_rejected(self):
        server = MetricsServer(self, workers=False)
        original = server.config.read_text()
        additions = [f"  collect-metrics: {value}\n" for value in ('maybe', '2', '""', '[ON]', '{enabled: ON}')]
        additions += ["  collect-metrics: ON\n  collect_metrics: OFF\n",
                      "  collect-metrics: ON\nphp.collect-metrics: OFF\n",
                      "php.collect-metrics: ON\nphp.collect_metrics: ON\n"]
        for addition in additions:
            for command in ("-t", "effective-config"):
                with self.subTest(addition=addition, command=command):
                    server.config.write_text(original.replace("php.ini:\n", addition + "php.ini:\n"))
                    result = server.command(command)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn("collect", result.stderr)

    def exact_reused_requests(self, policy):
        # This is a successful-concurrency check, with credit for all eight
        # input/output streams. The dedicated limit tests use 16 KiB; eight
        # 4 KiB input buffers can legitimately exhaust that smaller budget.
        server = MetricsServer(self, policy, global_budget=1048576)
        server.start()
        before = server.quiescent()
        prom_before = server.prometheus()

        def exchange(_):
            connection = http.client.HTTPConnection("127.0.0.1", server.port, timeout=10)
            first_socket = None
            workers = []
            try:
                for path in ("/classic.php", "/worker", "/classic.php", "/worker"):
                    connection.request("POST", path, BODY, {"Host": "metrics.test", "Accept-Encoding": "identity"})
                    if first_socket is None:
                        first_socket = connection.sock
                    self.assertIs(connection.sock, first_socket, "client reopened its connection")
                    response = connection.getresponse()
                    raw = response.read()
                    self.assertEqual(response.status, 200, raw[:1000])
                    self.assertFalse(response.will_close)
                    if path == "/classic.php":
                        self.assertEqual(raw, BODY[::-1])
                        self.assertEqual(response.getheader("X-OPcache-Cached"), "1")
                    else:
                        value = json.loads(raw)
                        expected = {"kind": "worker", "sha256": DIGEST, "boot": value["boot"],
                                    "count": value["count"], "cached": True}
                        self.assertRegex(value["boot"], r"^[a-f0-9]{16}$")
                        self.assertGreater(value["count"], 0)
                        self.assertEqual(raw, json.dumps(expected, separators=(",", ":")).encode())
                        workers.append(value)
            finally:
                connection.close()
            return workers

        results = []
        for connections in (1, 8):
            with ThreadPoolExecutor(max_workers=connections) as pool:
                for group in pool.map(exchange, range(connections)):
                    results.extend(group)
            after = server.quiescent()
            self.assertEqual(after["threads"], 4)
            self.assertEqual(after["ready_workers"], 2)
        counts = defaultdict(list)
        for value in results:
            self.assertIn(value["boot"], server.boots())
            counts[value["boot"]].append(value["count"])
        self.assertEqual(sum(map(len, counts.values())), 18)
        self.assertLessEqual(len(counts), 2)
        for values in counts.values():
            self.assertEqual(sorted(values), list(range(1, len(values) + 1)))
        self.assertTrue(any(len(values) > 1 for values in counts.values()))
        self.assertEqual(after["completed"] - before["completed"], 36)
        self.assertEqual(after["failed"] - before["failed"], 0)
        self.assertEqual(after["timed_out"] - before["timed_out"], 0)
        prom_after = server.prometheus()
        if server.enabled:
            self.assertEqual(prom_after["frankenphp_total_threads"], 4)
            self.assertEqual(prom_after["frankenphp_worker_request_count"] - prom_before["frankenphp_worker_request_count"], 18)
            self.assertGreater(prom_after["frankenphp_worker_request_time"], prom_before["frankenphp_worker_request_time"])
            self.assertEqual(prom_after["frankenphp_busy_workers"], 0)
            self.assertEqual(prom_after["frankenphp_queue_depth"], 0)
            self.assertEqual(prom_after["frankenphp_worker_queue_depth"], 0)

    def test_default_collection_keeps_existing_request_families(self):
        self.exact_reused_requests(None)

    def test_explicit_on_keeps_existing_request_families(self):
        self.exact_reused_requests("ON")

    def test_off_keeps_exact_requests_and_native_accounting(self):
        self.exact_reused_requests("OFF")

    def test_off_preserves_queue_admission_and_worker_restart(self):
        server = MetricsServer(self, "OFF", queue_limit=1)
        server.start()
        before = server.quiescent()

        def wait_for(predicate):
            deadline = time.monotonic() + 3
            while not predicate():
                self.assertLess(time.monotonic(), deadline, server.metrics())
                time.sleep(0.005)

        with ThreadPoolExecutor(max_workers=3) as pool:
            first = pool.submit(server.request, "/worker?hold=first")
            try:
                wait_for(lambda: (server.base / "entered-first").exists())
                second = pool.submit(server.request, "/worker?hold=second")
                wait_for(lambda: all((server.base / f"entered-{tag}").exists() for tag in ("first", "second")))
                during = server.metrics()
                self.assertEqual(during["active_php_threads"], 2)
                self.assertEqual(during["busy_workers"], 2)
                third = pool.submit(server.request, "/worker")
                wait_for(lambda: server.metrics()["queued_requests"] == 1)
                status, _, _ = server.request("/worker")
                self.assertEqual(status, 503)
                server.prometheus()
            finally:
                (server.base / "release").touch()
            for future in (first, second, third):
                self.assertEqual(future.result(timeout=5)[0], 200)
        after = server.quiescent()
        self.assertEqual(after["completed"] - before["completed"], 3)
        self.assertEqual(after["failed"] - before["failed"], 0)
        previous_boots = set(server.boots())
        status, _, _ = server.request("/restart", "POST")
        self.assertEqual(status, 202)
        wait_for(lambda: len(server.boots()) == 4 and server.metrics()["ready_workers"] == 2)
        status, _, raw = server.request("/worker")
        self.assertEqual(status, 200)
        self.assertNotIn(json.loads(raw)["boot"], previous_boots)
        final = server.quiescent()
        self.assertEqual(final["completed"] - after["completed"], 1)
        self.assertGreaterEqual(final["worker_restarts"] - after["worker_restarts"], 2)
        server.prometheus()

    def test_opcache_cache_and_reset_scheduling_with_collection_on_and_off(self):
        for policy in ("ON", "OFF"):
            with self.subTest(policy=policy):
                server = MetricsServer(self, policy, workers=False)
                server.start()
                before = server.quiescent()
                status, headers, raw = server.request("/classic.php", "POST", BODY)
                self.assertEqual((status, raw), (200, BODY[::-1]))
                self.assertEqual(headers["x-opcache-cached"], "1")
                prom_before = server.prometheus()
                status, _, raw = server.request("/reset.php")
                self.assertEqual(status, 200)
                self.assertEqual(json.loads(raw), {"accepted": True, "pending": True})
                after = server.quiescent()
                self.assertEqual(after["completed"] - before["completed"], 2)
                prom_after = server.prometheus()
                if server.enabled:
                    self.assertEqual(prom_after["frankenphp_opcache_restarts"] - prom_before["frankenphp_opcache_restarts"], 1)
                self.assertIn('"component":"php.opcache"', server.log_path.read_text())
                self.assertIn('"reason":"manual"', server.log_path.read_text())
                server.stop()


if __name__ == "__main__":
    unittest.main()
