"""PHP upgrade/build-cache regression: python3 tests/test_php_build.py."""

import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PHPBuildTest(unittest.TestCase):
    def test_php_upgrade_refreshes_cached_abi_checks(self):
        php_config = shutil.which("php-config-zts")
        if not php_config:
            self.skipTest("php-config-zts is required")
        includes = shlex.split(subprocess.check_output([php_config, "--includes"], text=True))
        headers = next(Path(flag[2:]) for flag in includes
                       if flag.startswith("-I") and (Path(flag[2:]) / "main/php_version.h").exists())
        with tempfile.TemporaryDirectory(prefix="h2o-php-build-test-") as directory:
            base = Path(directory)
            copied = base / "headers"
            shutil.copytree(headers, copied)
            copied_flags = [flag.replace(str(headers), str(copied), 1) for flag in includes]
            config = base / "php-config-zts"
            config.write_text("#!/bin/sh\n"
                              'if [ "$1" = "--includes" ]; then\n'
                              f"  printf '%s\\n' {shlex.quote(shlex.join(copied_flags))}\n"
                              "else\n"
                              f'  exec {shlex.quote(php_config)} "$@"\n'
                              "fi\n")
            config.chmod(0o755)
            build = base / "build"

            def configure():
                return subprocess.run(
                    ["cmake", "-S", str(ROOT), "-B", str(build),
                     f"-DPHPZTS_CONFIG={config}", "-DCMAKE_BUILD_TYPE=Release",
                     "-DCMAKE_C_COMPILER=/usr/bin/gcc", "-DCMAKE_CXX_COMPILER=/usr/bin/g++"],
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)

            def check_build():
                return subprocess.run(
                    ["cmake", "--build", str(build), "--target", "h2o-php-php-abi-check"],
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)

            initial = configure()
            self.assertEqual(initial.returncode, 0, initial.stdout)
            initial_build = check_build()
            self.assertEqual(initial_build.returncode, 0, initial_build.stdout)
            stamp = (build / "phpzts-abi.txt").read_text()
            version_header = copied / "main/php_version.h"
            original = version_header.read_text()
            version_id = int(re.search(r"#define PHP_VERSION_ID (\d+)", original)[1])

            # Package files can be older than the objects already in the build.
            changed = re.sub(r'(#define PHP_VERSION "[^"]+)(")', r'\1-build-test\2', original)
            self.assertNotEqual(changed, original)
            version_header.write_text(changed)
            os.utime(version_header, (1, 1))
            stale = check_build()
            self.assertNotEqual(stale.returncode, 0, stale.stdout)
            self.assertIn("changed since configuration", stale.stdout)

            refreshed = configure()
            self.assertEqual(refreshed.returncode, 0, refreshed.stdout)
            self.assertIn("Performing Test PHPZTS_ABI_LINKS", refreshed.stdout)
            self.assertIn("Performing Test PHPZTS_RUNTIME_VERSION_MATCHES", refreshed.stdout)
            self.assertNotEqual((build / "phpzts-abi.txt").read_text(), stamp)
            rebuilt = check_build()
            self.assertEqual(rebuilt.returncode, 0, rebuilt.stdout)

            # A previous successful runtime check must not mask a new mismatch.
            version_header.write_text(original.replace(f"PHP_VERSION_ID {version_id}",
                                                       f"PHP_VERSION_ID {version_id + 1}"))
            mismatch = configure()
            self.assertNotEqual(mismatch.returncode, 0, mismatch.stdout)
            self.assertIn("runtime version differs", mismatch.stdout)

            version_header.write_text(original.replace(f"PHP_VERSION_ID {version_id}",
                                                       "PHP_VERSION_ID 80700"))
            unsupported = configure()
            self.assertNotEqual(unsupported.returncode, 0, unsupported.stdout)
            self.assertIn("PHPZTS_ABI_LINKS - Failed", unsupported.stdout)


if __name__ == "__main__":
    unittest.main()
