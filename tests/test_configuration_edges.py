"""Cold YAML regressions: python3 tests/test_configuration_edges.py [binary]."""

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()
MAX_SIZE = (1 << (struct.calcsize("P") * 8)) - 1
FIELDS = ("max-buffered-global", "max-buffered-body", "max-buffered-response")
SPELLINGS = ("mapping", "underscore", "scalar")
COMMAND_TIMEOUT = 8


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


class ConfigurationEdgesTest(unittest.TestCase):
    def setUp(self):
        output = os.environ.get("H2O_PHP_TEST_OUTPUT")
        if output:
            self.base = Path(output).resolve() / self._testMethodName
            self.base.mkdir(parents=True, exist_ok=False)
        else:
            temporary = tempfile.TemporaryDirectory(prefix="h2o-php-config-edges-")
            self.addCleanup(temporary.cleanup)
            self.base = Path(temporary.name)
        self.app = self.base / "app"
        self.cwd = self.base / "unrelated-cwd"
        self.app.mkdir()
        self.cwd.mkdir()
        self.marker = self.app / "PHP-STARTED"
        script = "<?php file_put_contents(__DIR__.'/PHP-STARTED', 'unexpected'); "
        (self.app / "index.php").write_text(script + "echo 'ok';\n")
        for name in ("worker.php", "global.php", "extension.php"):
            (self.app / name).write_text(script + "while (frankenphp_handle_request(static function () { echo 'ok'; }));\n")
        self.records = []
        self.environment = {**os.environ, "GOMAXPROCS": "2"}

    def yaml(self, runtime, *, before="", after="", root_early=True, static=False):
        root = f"php.root: {self.app}\n"
        prefix = "listen: {host: 127.0.0.1, port: 0}\nnum-threads: 1\n"
        handler = f"file.dir: {self.app}" if static else "php_server: ON"
        return (prefix + (root if root_early else "") + before + "php.runtime:\n" + runtime
                + after + ("" if root_early else root)
                + "hosts:\n  config.test:\n    paths:\n      /:\n        " + handler + "\n")

    def buffer_yaml(self, field, value, spelling):
        runtime = "  threads: 1\n  max-threads: 1\n"
        key = field.replace("-", "_") if spelling == "underscore" else field
        encoded = json.dumps(value)
        if spelling == "scalar":
            return self.yaml(runtime, after=f"php.{key}: {encoded}\n")
        return self.yaml(runtime + f"  {key}: {encoded}\n")

    def check(self, tag, yaml, *, valid=True, error=None, runtime=None, workers=None,
              environment=None, extension=None):
        """Keep both cold results before assertions, including expected failures."""
        for command in ("-t", "effective-config"):
            with self.subTest(case=tag, command=command):
                directory = self.base / f"{len(self.records):03d}-{tag}-{command.lstrip('-')}"
                directory.mkdir()
                config = directory / "config.yml"
                config.write_text(yaml)
                argv = [str(BINARY), command, "-c", str(config)]
                write_json(directory / "command.json", {"argv": argv, "cwd": str(self.cwd),
                    "timeout_seconds": COMMAND_TIMEOUT, "scope": "cold configuration only"})
                try:
                    result = subprocess.run(argv, cwd=self.cwd, env=self.environment,
                        capture_output=True, timeout=COMMAND_TIMEOUT)
                except subprocess.TimeoutExpired as failure:
                    record = {"case": tag, "command": command, "argv": argv, "timed_out": True,
                        "stdout": (failure.stdout or b"").decode(errors="replace"),
                        "stderr": (failure.stderr or b"").decode(errors="replace")}
                    write_json(directory / "result.json", record)
                    self.records.append(record)
                    write_json(self.base / "results.json", self.records)
                    raise
                record = {"case": tag, "command": command, "argv": argv,
                    "returncode": result.returncode, "stdout": result.stdout.decode(errors="replace"),
                    "stderr": result.stderr.decode(errors="replace"), "timed_out": False}
                (directory / "stdout.txt").write_bytes(result.stdout)
                (directory / "stderr.txt").write_bytes(result.stderr)
                write_json(directory / "result.json", record)
                self.records.append(record)
                write_json(self.base / "results.json", self.records)
                self.assertFalse(self.marker.exists(), "cold validation executed application PHP")
                if not valid:
                    self.assertNotEqual(result.returncode, 0, record)
                    if error:
                        self.assertIn(error, record["stderr"], record)
                    continue
                self.assertEqual(result.returncode, 0, record)
                if command != "effective-config":
                    continue
                effective = json.loads(result.stdout)
                self.assertIs(effective["php_engine_started"], False)
                for name, expected in (runtime or {}).items():
                    self.assertEqual(effective["runtime"][name], expected, record)
                if workers is not None:
                    pools = {Path(row["file"]).name: row for row in effective["worker_pools"]}
                    self.assertEqual(len(pools), len(effective["worker_pools"]))
                    self.assertEqual(set(pools), set(workers), record)
                    for name, maximum in workers.items():
                        self.assertEqual(pools[name]["max_requests"], maximum, record)
                        self.assertEqual(pools[name]["root"], str(self.app.resolve()), record)
                        for key, value in (environment or {}).items():
                            self.assertEqual(pools[name]["env"][key], value, record)
                if extension:
                    routes = [route for host in effective["hosts"] for path in host["paths"]
                              for route in path["php_extension_handlers"]]
                    self.assertTrue(routes, record)
                    files = {Path(worker["file"]).name for route in routes for worker in route["workers"]}
                    self.assertIn(extension, files, record)

    def test_hidden_negative_budgets_rejected_across_aliases(self):
        for field, spelling in zip(FIELDS, SPELLINGS):
            for label, whitespace in (("space", " "), ("tab", "\t"), ("newline", "\n")):
                self.check(f"{field}-{spelling}-{label}", self.buffer_yaml(field, whitespace + "-1", spelling),
                    valid=False, error="expected nonnegative integer")

    def test_plus_leading_zeros_and_maximum_buffers_remain_legal(self):
        for field, spelling in zip(FIELDS, SPELLINGS):
            for label, value, expected in (("plus", "+00016384", 16384), ("zeros", "00016384", 16384),
                                            ("maximum", str(MAX_SIZE), MAX_SIZE)):
                self.check(f"{field}-{spelling}-{label}", self.buffer_yaml(field, value, spelling),
                    runtime={field.replace("-", "_"): expected}, workers={})

    def test_unsigned_overflow_and_invalid_grammar_rejected(self):
        for field, spelling in zip(FIELDS, SPELLINGS):
            self.check(f"{field}-overflow", self.buffer_yaml(field, str(1 << 64), spelling),
                valid=False, error="expected nonnegative integer")
        for number, value in enumerate(("+", "++1", "1 0", " 16384", "16384 ", "-1")):
            self.check(f"grammar-{number}", self.buffer_yaml(FIELDS[0], value, "mapping"),
                valid=False, error="expected nonnegative integer")

    def test_global_worker_default_and_explicit_limits_ignore_alias_order(self):
        for location in ("before", "after", "mapping"):
            for explicit in (None, 0, 7):
                runtime = ("  threads: 1\n  max-threads: 2\n  workers:\n"
                           "    - file: worker.php\n      num: 1\n")
                if explicit is not None:
                    runtime += f"      max-requests: {explicit}\n"
                before = "php.max-requests: 2\n" if location == "before" else ""
                after = "php.max-requests: 2\n" if location == "after" else ""
                if location == "mapping":
                    runtime += "  max-requests: 2\n"
                self.check(f"worker-{location}-{explicit}", self.yaml(runtime, before=before, after=after),
                    runtime={"max_requests": 2}, workers={"worker.php": 2 if explicit is None else explicit})

    def test_global_extension_worker_sees_immediate_runtime_before_nested_entry(self):
        for global_worker, custom_first in ((False, True), (True, True), (True, False)):
            extension = "extension.php" if global_worker else "worker.php"
            custom = ("file.custom-handler:\n  extension: .php\n"
                      f"  php.worker: {{file: {self.app / extension}, num: 1}}\n")
            runtime = f"  threads: 1\n  max-threads: {3 if global_worker else 2}\n"
            workers = {extension: 2}
            if global_worker:
                runtime += "  worker: {file: global.php, num: 1}\n"
                workers["global.php"] = 2
            runtime += "  max-requests: 2\n"
            self.check(f"extension-global-{global_worker}-first-{custom_first}",
                self.yaml(runtime, before=custom if custom_first else "", after="" if custom_first else custom, static=True),
                runtime={"max_requests": 2}, workers=workers, extension=extension)

    def test_global_worker_uses_final_root_and_environment(self):
        runtime = ("  threads: 1\n  max-threads: 2\n"
                   "  workers: [{file: worker.php, num: 1}]\n  max-requests: 2\n")
        self.check("late-root-and-env", self.yaml(runtime, root_early=False,
            after="php.env: {QUAL_LATE: final}\n"), runtime={"max_requests": 2},
            workers={"worker.php": 2}, environment={"QUAL_LATE": "final"})

    def test_duplicate_runtime_mappings_rejected(self):
        for number, blocks in enumerate((("{}", "{threads: 1, max-threads: 1}"),
                                         ("{threads: 1, max-threads: 1}", "{}"),
                                         ("{workers: []}", "{}"))):
            yaml = ("listen: {host: 127.0.0.1, port: 0}\nnum-threads: 1\n"
                    f"php.root: {self.app}\nphp.runtime: {blocks[0]}\nphp.runtime: {blocks[1]}\n"
                    "hosts:\n  config.test:\n    paths:\n      /: {php_server: ON}\n")
            self.check(f"duplicate-runtime-{number}", yaml, valid=False, error="duplicate php.runtime mapping")

    def test_root_exit_validates_pending_workers_without_nested_entry(self):
        yaml = ("listen: {host: 127.0.0.1, port: 0}\nnum-threads: 1\n"
                f"php.root: {self.app}\nphp.runtime:\n  threads: 1\n  max-threads: 2\n"
                "  workers: [{file: missing.php, num: 1}]\n")
        self.check("root-exit-invalid-worker", yaml, valid=False, error="worker file must resolve to a regular script")


if __name__ == "__main__":
    unittest.main()
