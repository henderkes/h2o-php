"""HTTP pool regression checks: python3 tests/test_worker_pool.py [h2o-php binary]."""

from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()
# Keep omitted worker-count checks independent of host affinity/cgroup policy.
DEFAULT_CPUS = 2
DEFAULT_WORKERS = 2 * DEFAULT_CPUS


def wait_until(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("Timed out waiting for pool state")


class WorkerServer:
    def __init__(self, num=3, *, scalar=False, bootstrap_gate=False,
                 fail_once=False, max_requests=0, max_wait="2s"):
        self.temp = tempfile.TemporaryDirectory(prefix="h2o-php-pool-")
        self.base = Path(self.temp.name)
        self.default_count = num is None
        self.num = DEFAULT_WORKERS if self.default_count else num
        self.process = None
        self.log = None
        self.worker = self.base / "worker.php"
        if bootstrap_gate:
            (self.base / "bootstrap-gate").touch()
        self.worker.write_text("""<?php
$id = bin2hex(random_bytes(8));
file_put_contents(__DIR__.'/boots', $id."\\n", FILE_APPEND | LOCK_EX);
FAIL_ONCE
while (is_file(__DIR__.'/bootstrap-gate')) {
    usleep(1000);
    clearstatcache(true, __DIR__.'/bootstrap-gate');
}
$count = 0;
while (frankenphp_handle_request(function () use ($id, &$count) {
    if (isset($_GET['hold'])) {
        $gate = __DIR__.'/release-'.$_GET['hold'];
        touch(__DIR__.'/entered-'.$_GET['hold']);
        $deadline = microtime(true) + 8;
        do {
            clearstatcache(true, $gate);
            if (is_file($gate)) break;
            if (microtime(true) >= $deadline) throw new RuntimeException('gate timed out');
            usleep(1000);
        } while (true);
    }
    header('Content-Type: application/json');
    echo json_encode(['id' => $id, 'count' => ++$count]);
}));
""".replace("FAIL_ONCE", """
if ($once = @fopen(__DIR__.'/failed-once', 'x')) {
    fclose($once);
    throw new RuntimeException('intentional bootstrap failure');
}
""" if fail_once else ""))
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        if scalar:
            worker_config = f"        php.worker: {self.worker}\n"
        else:
            worker_config = f"        php.worker:\n          file: {self.worker}\n"
            if num is not None:
                worker_config += f"          num: {num}\n"
            worker_config += "          max-consecutive-failures: 1\n"
        self.config = self.base / "h2o.yml"
        self.config.write_text(f"""listen:
  host: 127.0.0.1
  port: {self.port}
num-threads: 2
php.runtime:
  threads: 1
  max-threads: {self.num + 1}
  max-requests: {max_requests}
  max-wait-time: {max_wait}
  request-timeout: 10s
  shutdown-timeout: 2s
php.ini:
  opcache.enable: "1"
  opcache.jit: "off"
hosts:
  pool.test:
    php.root: {self.base}
    paths:
      /metrics:
        php.metrics: ON
      /restart:
        php.restart: ON
      /:
{worker_config}""")

    def start(self, wait=True):
        self.log = (self.base / "server.log").open("w+")
        self.process = subprocess.Popen(
            [str(BINARY), "-c", str(self.config)],
            stdout=self.log, stderr=subprocess.STDOUT,
            env={**os.environ, **({"GOMAXPROCS": str(DEFAULT_CPUS)} if self.default_count else {})},
        )
        if wait:
            self.wait_ready()
        return self

    def wait_ready(self):
        def ready():
            if self.process.poll() is not None:
                raise AssertionError(self.log_text())
            return "ready to serve requests" in self.log_text()
        try:
            wait_until(ready)
        except AssertionError as error:
            raise AssertionError(f"{error}\n{self.log_text()}") from error

    def log_text(self):
        return (self.base / "server.log").read_text()

    def boots(self):
        path = self.base / "boots"
        return path.read_text().splitlines() if path.exists() else []

    def request(self, path="/", method="GET"):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            connection.request(method, path, headers={"Host": "pool.test"})
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def json(self, path="/"):
        status, body = self.request(path)
        assert status == 200, (status, body, self.log_text())
        return json.loads(body)

    def metrics(self):
        return self.json("/metrics")

    def entered(self, name):
        wait_until(lambda: (self.base / f"entered-{name}").exists())

    def release(self, name):
        (self.base / f"release-{name}").touch()

    def all_idle(self):
        wait_until(lambda: self.metrics()["ready_workers"] == self.num)

    def stop(self):
        if self.process is not None:
            (self.base / "bootstrap-gate").unlink(missing_ok=True)
            for marker in self.base.glob("entered-*"):
                self.release(marker.name.removeprefix("entered-"))
            if self.process.poll() is None:
                self.process.terminate()
            try:
                self.process.wait(timeout=6)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError(f"Pool shutdown hung:\n{self.log_text()}")
            if self.process.returncode != 0:
                raise AssertionError(f"Server exited {self.process.returncode}:\n{self.log_text()}")
        if self.log is not None:
            self.log.close()
        self.process = self.log = None
        self.temp.cleanup()


class WorkerPoolTest(unittest.TestCase):
    def server(self, *args, **kwargs):
        server = WorkerServer(*args, **kwargs)
        self.addCleanup(server.stop)
        return server.start()

    def test_default_count_is_twice_cpu_budget_with_scalar_and_mapping(self):
        for scalar in (True, False):
            with self.subTest(scalar=scalar):
                server = self.server(num=None, scalar=scalar)
                self.assertEqual(len(server.boots()), DEFAULT_WORKERS)
                self.assertEqual(server.metrics()["ready_workers"], DEFAULT_WORKERS)
                self.assertEqual(server.metrics()["threads"], DEFAULT_WORKERS + 1)
                server.stop()

    def test_eager_bootstrap_then_prefer_first_available_worker(self):
        server = WorkerServer(num=4, bootstrap_gate=True)
        self.addCleanup(server.stop)
        server.start(wait=False)
        wait_until(lambda: len(server.boots()) == 4)
        self.assertNotIn("ready to serve requests", server.log_text())
        (server.base / "bootstrap-gate").unlink()
        server.wait_ready()
        self.assertEqual(server.metrics()["ready_workers"], 4)
        responses = [server.json() for _ in range(16)]
        self.assertEqual(len({response["id"] for response in responses}), 1)
        self.assertEqual([response["count"] for response in responses], list(range(1, 17)))
        self.assertEqual(len(server.boots()), 4)

    def test_busy_workers_queue_and_dispatch_to_the_first_free_worker(self):
        server = self.server(num=2)
        with ThreadPoolExecutor(max_workers=3) as pool:
            first = pool.submit(server.json, "/?hold=first")
            server.entered("first")
            second = pool.submit(server.json, "/?hold=second")
            server.entered("second")
            queued = pool.submit(server.json)
            try:
                wait_until(lambda: server.metrics()["queued_requests"] == 1)
                self.assertEqual(server.metrics()["busy_workers"], 2)
                self.assertEqual(len(server.boots()), 2)
                server.release("second")
                second_response = second.result(timeout=3)
                queued_response = queued.result(timeout=3)
                self.assertEqual(queued_response["id"], second_response["id"])
                self.assertFalse(first.done())
            finally:
                server.release("first")
                server.release("second")
            first_response = first.result(timeout=3)
        self.assertNotEqual(first_response["id"], second_response["id"])
        server.all_idle()
        self.assertEqual(server.json()["id"], first_response["id"])

    def test_burst_wakes_every_available_worker(self):
        server = self.server(num=4)
        with ThreadPoolExecutor(max_workers=4) as pool:
            requests = [pool.submit(server.json, f"/?hold={i}") for i in range(4)]
            try:
                for i in range(4):
                    server.entered(str(i))
                self.assertEqual(server.metrics()["busy_workers"], 4)
            finally:
                for i in range(4):
                    server.release(str(i))
            responses = [request.result(timeout=3) for request in requests]
        self.assertEqual(len({response["id"] for response in responses}), 4)
        self.assertEqual(len(server.boots()), 4)

    def test_queue_deadline_expires_while_worker_is_busy(self):
        server = self.server(num=1, max_wait="100ms")
        with ThreadPoolExecutor(max_workers=1) as pool:
            busy = pool.submit(server.json, "/?hold=busy")
            try:
                server.entered("busy")
                self.assertEqual(server.request()[0], 503)
                self.assertFalse(busy.done())
            finally:
                server.release("busy")
            busy.result(timeout=3)
        self.assertEqual(server.json()["count"], 2)

    def test_restart_wakes_idle_workers_and_waits_for_busy_callback(self):
        server = self.server(num=2)
        original = set(server.boots())
        with ThreadPoolExecutor(max_workers=1) as pool:
            busy = pool.submit(server.json, "/?hold=busy")
            try:
                server.entered("busy")
                self.assertEqual(server.request("/restart", method="POST")[0], 202)
                wait_until(lambda: len(server.boots()) == 3)
                self.assertNotIn(server.json()["id"], original)
                self.assertFalse(busy.done())
            finally:
                server.release("busy")
            busy.result(timeout=3)
        wait_until(lambda: len(server.boots()) == 4)
        server.all_idle()
        self.assertEqual(server.metrics()["threads"], 3)

    def test_single_bootstrap_failure_recovers_before_ready(self):
        server = self.server(num=2, fail_once=True)
        self.assertEqual(server.metrics()["worker_startup_failures"], 1)
        self.assertEqual(server.metrics()["failed_workers"], 0)
        self.assertEqual(server.metrics()["ready_workers"], 2)
        self.assertEqual(len(server.boots()), 3)
        self.assertEqual(server.request()[0], 200)
        self.assertEqual(server.request("/restart", method="POST")[0], 202)
        server.all_idle()
        self.assertEqual(server.metrics()["failed_workers"], 0)
        self.assertEqual(server.request()[0], 200)

    def test_recycling_preserves_dispatch_and_inventory(self):
        server = self.server(num=2, max_requests=1)
        responses = [server.json() for _ in range(6)]
        self.assertEqual([response["count"] for response in responses], [1] * 6)
        self.assertEqual(len({response["id"] for response in responses}), 6)
        server.all_idle()
        self.assertEqual(server.metrics()["threads"], 3)
        self.assertEqual(server.metrics()["active_worker_scripts"], 2)


if __name__ == "__main__":
    unittest.main()
