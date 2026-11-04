"""PHP ABI/lifecycle checks: python3 tests/test_php_compatibility.py [h2o-php binary]."""

import http.client
import json
import os
import pwd
import time
import unittest

from test_worker_pool import WorkerServer, wait_until


class PHPCompatibilityTest(unittest.TestCase):
    def prepare_server(self, source, ini=""):
        server = WorkerServer(num=1)
        self.addCleanup(server.stop)
        server.worker.write_text(source)
        server.config.write_text(server.config.read_text().replace(
            '  opcache.jit: "off"\n', '  opcache.jit: "off"\n' + ini))
        return server

    def test_worker_output_handler_and_request_cleanup(self):
        server = self.prepare_server("""<?php
$boot = bin2hex(random_bytes(8));
while (frankenphp_handle_request(function () use ($boot) {
    session_start();
    $old = $_SESSION['count'] ?? 0;
    $_SESSION['count'] = $old + 1;
    if (isset($_GET['callback'])) {
        header_register_callback(function () { header('X-Callback: first'); });
    }
    header('Content-Type: application/json');
    echo json_encode(['boot' => $boot, 'user' => get_current_user(),
        'get' => $_GET, 'post' => $_POST, 'input' => file_get_contents('php://input'),
        'session' => $old, 'handler' => ob_get_status()['name']]);
}));
""", '  output_handler: "ob_gzhandler"\n')
        server.config.write_text(server.config.read_text().replace(
            'php.ini:\n', f'php.ini:\n  session.save_path: "{server.base}"\n'))
        server.start()

        def fetch(path, method="GET", body=None, headers=None):
            connection = http.client.HTTPConnection("127.0.0.1", server.port, timeout=5)
            try:
                connection.request(method, path, body=body,
                                   headers={"Host": "pool.test", **(headers or {})})
                response = connection.getresponse()
                self.assertEqual(response.status, 200, server.log_text())
                return json.loads(response.read()), {name.lower(): value for name, value in response.getheaders()}
            finally:
                connection.close()

        first, headers = fetch("/?callback=1", "POST", "value=first",
                               {"Content-Type": "application/x-www-form-urlencoded"})
        cookie = headers["set-cookie"].split(";", 1)[0]
        second, second_headers = fetch("/?next=2", headers={"Cookie": cookie})
        fresh, _ = fetch("/")
        self.assertEqual(first["post"], {"value": "first"})
        self.assertEqual(first["input"], "value=first")
        self.assertEqual(first["get"], {"callback": "1"})
        self.assertEqual(second["get"], {"next": "2"})
        self.assertEqual(headers["x-callback"], "first")
        self.assertNotIn("x-callback", second_headers)
        self.assertEqual([r["session"] for r in (first, second, fresh)], [0, 1, 0])
        self.assertEqual(len({r["boot"] for r in (first, second, fresh)}), 1)
        for response in (first, second, fresh):
            self.assertEqual(response["handler"], "ob_gzhandler")
            self.assertEqual(response["user"], pwd.getpwuid(server.worker.stat().st_uid).pw_name)
        for response in (second, fresh):
            self.assertEqual(response["post"], [])
            self.assertEqual(response["input"], "")

    def test_watch_invalidates_opcache_with_same_second_edits(self):
        for overlap in (False, True):
            with self.subTest(overlapping_roots=overlap):
                server = self.prepare_server("""<?php
$value = require __DIR__.'/code/value.php';
$id = bin2hex(random_bytes(8));
file_put_contents(__DIR__.'/boots', $id."\\n", FILE_APPEND | LOCK_EX);
while (frankenphp_handle_request(function () use ($value, $id) {
    header('Content-Type: application/json');
    echo json_encode(['value' => $value, 'id' => $id,
        'cached' => opcache_is_script_cached(__DIR__.'/code/value.php')]);
}));
""", '  opcache.validate_timestamps: "0"\n  opcache.file_update_protection: "0"\n')
                watched = server.base / "code"
                watched.mkdir()
                included = watched / "value.php"
                included.write_text("<?php return 'v1';\n")
                second = int(time.time()) - 5
                timestamp = second * 1_000_000_000
                os.utime(included, ns=(timestamp, timestamp))
                watch_paths = [watched, included] if overlap else [included]
                watch_config = "          watch:\n" + "".join(
                    f"            - {path}\n" for path in watch_paths)
                server.config.write_text(server.config.read_text() + watch_config)
                server.start()
                first = server.json()
                self.assertEqual(first["value"], "v1")
                self.assertTrue(first["cached"])
                included.write_text("<?php return 'v2';\n")
                os.utime(included, ns=(timestamp + 100, timestamp + 100))
                wait_until(lambda: len(server.boots()) >= 2)
                updated = server.json()
                self.assertEqual(updated["value"], "v2")
                self.assertTrue(updated["cached"])
                self.assertNotEqual(updated["id"], first["id"])
                server.stop()


if __name__ == "__main__":
    unittest.main()
