"""Capability setup checks: python3 tests/test_build_capabilities.py."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "cmake/SetCapabilities.cmake"


class BuildCapabilitiesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="h2o-php-capabilities-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.binary = self.base / "path with spaces" / "h2o-php"
        self.binary.parent.mkdir()
        self.binary.touch()
        self.calls = self.base / "calls.jsonl"
        self.helpers = {}
        for name in ("getcap", "setcap", "sudo"):
            helper = self.base / name
            helper.write_text("#!/usr/bin/python3\n"
                              "import json, os, sys\n"
                              "from pathlib import Path\n"
                              "name = Path(sys.argv[0]).name\n"
                              "with open(os.environ['CAPABILITY_TEST_CALLS'], 'a') as log:\n"
                              "    log.write(json.dumps([name, *sys.argv[1:]]) + '\\n')\n"
                              "if name == 'getcap':\n"
                              "    if os.environ.get('CAPABILITY_TEST_PRESENT') == '1':\n"
                              "        print(sys.argv[1] + ' cap_net_bind_service=ep')\n"
                              "    sys.exit(0)\n"
                              "sys.exit(int(os.environ.get('CAPABILITY_TEST_' + name.upper(), '0')))\n")
            helper.chmod(0o755)
            self.helpers[name] = helper

    def run_setup(self, **environment):
        return subprocess.run(
            ["cmake", f"-DBINARY={self.binary}",
             *[f"-D{name.upper()}={helper}" for name, helper in self.helpers.items()],
             "-P", str(SCRIPT)],
            env=dict(os.environ, CAPABILITY_TEST_CALLS=str(self.calls), **environment),
            capture_output=True, text=True, timeout=10)

    def recorded(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def test_existing_capability_does_not_invoke_setcap_or_sudo(self):
        result = self.run_setup(CAPABILITY_TEST_PRESENT="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded(), [["getcap", str(self.binary)]])

    def test_direct_success_does_not_invoke_sudo(self):
        result = self.run_setup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded()[-1], ["setcap", "cap_net_bind_service=+ep", str(self.binary)])
        self.assertNotIn("sudo", [call[0] for call in self.recorded()])

    def test_permission_failure_uses_noninteractive_sudo(self):
        result = self.run_setup(CAPABILITY_TEST_SETCAP="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded()[-1],
                         ["sudo", "-n", str(self.helpers['setcap']), "cap_net_bind_service=+ep", str(self.binary)])
        self.assertIn("enabled CAP_NET_BIND_SERVICE", result.stdout)

    def test_unavailable_privilege_keeps_build_successful_and_explains_remedy(self):
        result = self.run_setup(CAPABILITY_TEST_SETCAP="1", CAPABILITY_TEST_SUDO="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("could not grant", result.stdout)
        self.assertIn(str(self.binary), result.stdout)

    def test_missing_tools_keep_build_successful(self):
        self.helpers['setcap'] = self.base / "missing-setcap"
        result = self.run_setup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("setcap unavailable", result.stdout)
        self.assertFalse(self.calls.exists())


if __name__ == "__main__":
    unittest.main()
