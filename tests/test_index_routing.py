"""HTTP regression checks: python3 tests/test_index_routing.py [h2o-php binary]."""

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


class IndexRoutingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="h2o-php-index-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.base = Path(cls.temp.name)
        cls.root = cls.base / "public"
        cls.custom = cls.base / "custom"
        cls.static = cls.base / "static"
        for directory in (cls.root, cls.custom, cls.static):
            directory.mkdir()
        for path, entry in (
            (cls.root / "index.php", "root"),
            (cls.root / "admin" / "index.php", "directory"),
            (cls.root / "direct.php", "direct"),
            (cls.custom / "app.php", "custom"),
            (cls.custom / "admin" / "app.php", "custom-directory"),
            (cls.base / "outside" / "index.php", "outside"),
        ):
            path.parent.mkdir(exist_ok=True)
            path.write_text(
                "<?php header('Content-Type: application/json'); echo json_encode(["
                f"'entry' => '{entry}', "
                "'script' => $_SERVER['SCRIPT_NAME'], "
                "'path_info' => $_SERVER['PATH_INFO'] ?? '', "
                "'uri' => $_SERVER['REQUEST_URI'], "
                "'root' => $_SERVER['DOCUMENT_ROOT'], 'query' => $_GET]);"
            )
        (cls.root / "asset.css").write_text("body { color: green; }\n")
        (cls.static / "index.html").write_text("static index\n")
        cls.worker = cls.base / "worker.php"
        cls.worker.write_text(
            "<?php $count = 0; while (frankenphp_handle_request(function () use (&$count) {"
            "header('Content-Type: application/json'); "
            "echo json_encode(['entry' => 'worker', 'count' => ++$count]); }));"
        )
        (cls.root / "escape").symlink_to(cls.base / "outside", target_is_directory=True)
        (cls.root / "escape.php").symlink_to(cls.base / "outside" / "index.php")
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
  max-threads: 2
hosts:
  default.test:
    php.root: {cls.root}
    paths:
      /mounted: {{}}
      /off:
        php.index: OFF
        file.dir: {cls.root}
      /static:
        file.file: {cls.root / 'asset.css'}
      /proxy:
        proxy.reverse.url: http://127.0.0.1:{cls.port}/static
      /metrics:
        php.metrics: ON
      /:
        file.dir: {cls.root}
  custom.test:
    php.root: {cls.custom}
    php.index: app.php
    paths:
      /off:
        php.index: OFF
      /default:
        php.root: {cls.root}
        php.index: index.php
      /: {{}}
  disabled.test:
    php.root: {cls.root}
    php.index: OFF
    paths:
      /enabled:
        php.index: index.php
      /: {{}}
  static.test:
    php.root: {cls.static}
    paths:
      /:
        file.dir: {cls.static}
  extension.test:
    php.root: {cls.root}
    paths:
      /:
        file.dir: {cls.root}
        file.index: [index.php, index.html]
        file.custom-handler:
          extension: .php
  minimal.test:
    php.root: {cls.root}
    paths:
      /: {{}}
  fallback.test:
    php.root: {cls.root}
    paths: {{}}
  worker.test:
    php.root: {cls.root}
    paths:
      /worker:
        php.worker:
          file: {cls.worker}
          num: 1
      /: {{}}
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
    def request(cls, path, host="default.test"):
        connection = http.client.HTTPConnection("127.0.0.1", cls.port, timeout=5)
        try:
            connection.request("GET", path, headers={"Host": host})
            response = connection.getresponse()
            return response.status, response.getheader("Content-Type", ""), response.read()
        finally:
            connection.close()

    def assert_script(self, path, entry, script, host="default.test", path_info=""):
        status, content_type, body = self.request(path, host)
        self.assertEqual(status, 200, body)
        self.assertIn("application/json", content_type)
        result = json.loads(body)
        self.assertEqual(result["entry"], entry)
        self.assertEqual(result["script"], script)
        self.assertEqual(result["path_info"], path_info)
        self.assertEqual(result["uri"], path)
        return result

    def test_default_index_and_framework_route(self):
        self.assert_script("/", "root", "/index.php")
        result = self.assert_script("/login?next=home", "root", "/index.php")
        self.assertEqual(result["query"], {"next": "home"})
        self.assertEqual(result["root"], str(self.root))

    def test_directory_index_takes_precedence_over_root_index(self):
        self.assert_script("/admin/", "directory", "/admin/index.php")
        self.assert_script("/admin", "directory", "/admin/index.php")

    def test_explicit_php_and_path_info(self):
        self.assert_script("/direct.php", "direct", "/direct.php")
        self.assert_script("/direct.php/more?x=1", "direct", "/direct.php", path_info="/more")

    def test_mounted_application(self):
        self.assert_script("/mounted/", "root", "/mounted/index.php")
        self.assert_script("/mounted/login", "root", "/mounted/index.php")
        self.assert_script("/mounted/admin/", "directory", "/mounted/admin/index.php")

    def test_existing_static_file(self):
        status, _, body = self.request("/asset.css")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"body { color: green; }\n")

    def test_application_root_enables_php_and_assets_by_default(self):
        for host in ("minimal.test", "fallback.test"):
            with self.subTest(host=host):
                self.assert_script("/login", "root", "/index.php", host)
                status, _, body = self.request("/asset.css", host)
                self.assertEqual(status, 200)
                self.assertEqual(body, b"body { color: green; }\n")

    def test_explicit_routes_keep_their_handlers(self):
        for path in ("/static/login", "/proxy/login"):
            with self.subTest(path=path):
                status, _, body = self.request(path)
                self.assertEqual(status, 200)
                self.assertEqual(body, b"body { color: green; }\n")
        status, _, body = self.request("/metrics")
        self.assertEqual(status, 200)
        self.assertNotIn("entry", json.loads(body))

    def test_worker_file_selects_persistent_execution(self):
        for count in (1, 2):
            status, _, body = self.request("/worker/login", "worker.test")
            self.assertEqual(status, 200)
            self.assertEqual(json.loads(body), {"entry": "worker", "count": count})
        self.assert_script("/login", "root", "/index.php", "worker.test")

    def test_html_index_when_no_php_index_exists(self):
        status, _, body = self.request("/", "static.test")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"static index\n")

    def test_index_off_keeps_direct_scripts(self):
        self.assertEqual(self.request("/off/login")[0], 404)
        self.assert_script("/off/direct.php", "direct", "/off/direct.php")
        self.assert_script("/login", "root", "/index.php")

    def test_custom_index_and_scope_overrides(self):
        self.assert_script("/", "custom", "/app.php", "custom.test")
        self.assert_script("/admin/", "custom-directory", "/admin/app.php", "custom.test")
        self.assertEqual(self.request("/off/login", "custom.test")[0], 404)
        self.assert_script("/default/login", "root", "/default/index.php", "custom.test")
        self.assert_script("/login", "custom", "/app.php", "custom.test")
        self.assertEqual(self.request("/login", "disabled.test")[0], 404)
        self.assert_script("/enabled/login", "root", "/enabled/index.php", "disabled.test")

    def test_symlinks_cannot_execute_scripts_outside_root(self):
        self.assertEqual(self.request("/escape.php")[0], 404)
        self.assert_script("/escape/", "root", "/index.php")

    def test_h2o_extension_handler_retains_its_own_index_routing(self):
        self.assert_script("/", "root", "/index.php", "extension.test")
        self.assert_script("/direct.php/more", "direct", "/direct.php", "extension.test", "/more")
        self.assertEqual(self.request("/login", "extension.test")[0], 404)

    def test_invalid_index_configuration_is_rejected(self):
        for value in ("../outside/index.php", "/absolute.php", "nested/../outside.php", '""'):
            with self.subTest(value=value):
                config = self.base / "invalid.yml"
                config.write_text(f"php.index: {value}\n")
                result = subprocess.run(
                    [str(BINARY), "-t", "-c", str(config)], capture_output=True, text=True
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("index must be a relative script path within root", result.stderr)


if __name__ == "__main__":
    unittest.main()
