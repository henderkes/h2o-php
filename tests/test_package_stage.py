"""GNU stage nonreplacement: python3 tests/test_package_stage.py [unused binary].

Every case uses --manifest-only with a private dummy non-ELF input and {} PHP
metadata. No compilation or PHP executable is used. H2O_PHP_TEST_OUTPUT retains
raw artifacts; without it successful temporary cases are removed and failures
are retained under the printed private /tmp path.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import uuid


# The serial project driver supplies its selected binary to every test except
# test_php_build.py. Accept that conventional argument without executing it.
if len(sys.argv) > 1 and not sys.argv[1].startswith('-'):
    sys.argv.pop(1)
ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/package-gnu.sh'
DUMMY = b'not-an-ELF-package-stage-fixture\x00\xff\n'
OLD_BINARY = b'preserve-this-earlier-executable\x00\n'
OLD_MANIFEST = b'{"artifact":"preserve-earlier-manifest"}\n'


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def fingerprint(path):
    """No symlink traversal; retain bytes plus identity of every owned entry."""
    rows = {}

    def visit(value, label):
        info = value.lstat()
        row = {'device': info.st_dev, 'inode': info.st_ino, 'mode': info.st_mode,
               'uid': info.st_uid, 'gid': info.st_gid, 'size': info.st_size,
               'mtime_ns': info.st_mtime_ns}
        if stat.S_ISLNK(info.st_mode):
            row['target'] = os.readlink(value)
        elif stat.S_ISREG(info.st_mode):
            body = value.read_bytes()
            row['sha256'] = hashlib.sha256(body).hexdigest(); row['body_hex'] = body.hex()
        elif stat.S_ISDIR(info.st_mode):
            row['children'] = sorted(item.name for item in value.iterdir())
        rows[label] = row
        if stat.S_ISDIR(info.st_mode):
            for child in sorted(value.iterdir()):
                visit(child, label + '/' + child.name)
    visit(path, '.')
    return rows


class PackageStageTest(unittest.TestCase):
    def setUp(self):
        persistent = os.environ.get('H2O_PHP_TEST_OUTPUT')
        self.persistent = persistent is not None
        self.completed = False
        if persistent:
            self.base = Path(persistent).resolve() / (self._testMethodName + '-' + uuid.uuid4().hex)
            self.base.mkdir(parents=True, exist_ok=False)
        else:
            self.base = Path(tempfile.mkdtemp(prefix='h2o-php-package-stage-'))
        self.addCleanup(self.cleanup)
        self.work = self.base / 'work'
        self.binary = self.work / 'h2o-gnu/h2o-php'
        self.binary.parent.mkdir(parents=True)
        self.binary.write_bytes(DUMMY)
        metadata = self.work / 'php-static-gnu/result.json'
        metadata.parent.mkdir(); metadata.write_text('{}\n')
        self.work_before = fingerprint(self.work)
        write_json(self.base / 'fixture.json', {'work': str(self.work), 'input': str(self.binary),
            'dummy_sha256': hashlib.sha256(DUMMY).hexdigest(), 'scope': 'Manifest-only file preservation; no build/PHP'})

    def cleanup(self):
        if self.completed and not self.persistent:
            shutil.rmtree(self.base)
        elif not self.completed:
            print('Retained package-stage failure evidence: ' + str(self.base), file=sys.stderr)

    def run_manifest(self, requested_stage):
        command = [str(SCRIPT), '--manifest-only', '--work', str(self.work), '--stage', str(requested_stage)]
        started = time.monotonic_ns()
        write_json(self.base / 'command.json', {'command': command, 'cwd': str(ROOT), 'start_ns': started})
        try:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=10,
                env={**os.environ, 'LC_ALL': 'C', 'PYTHONDONTWRITEBYTECODE': '1'})
        except subprocess.TimeoutExpired as error:
            (self.base / 'stdout.raw').write_bytes(error.stdout or b'')
            (self.base / 'stderr.raw').write_bytes(error.stderr or b'')
            write_json(self.base / 'result.json', {'passed': False, 'error': 'packager timeout',
                'start_ns': started, 'end_ns': time.monotonic_ns()})
            raise
        (self.base / 'stdout.raw').write_bytes(result.stdout)
        (self.base / 'stderr.raw').write_bytes(result.stderr)
        write_json(self.base / 'process-result.json', {'returncode': result.returncode,
            'start_ns': started, 'end_ns': time.monotonic_ns()})
        return result

    def populated(self, path):
        path.mkdir()
        (path / 'h2o-php').write_bytes(OLD_BINARY)
        (path / 'manifest.json').write_bytes(OLD_MANIFEST)

    def unchanged_refusal(self, requested_stage, extra_entries=()):
        watched = [requested_stage, *extra_entries]
        before = {str(path): fingerprint(path) for path in watched}
        write_json(self.base / 'before.json', before)
        result = self.run_manifest(requested_stage)
        after = {str(path): fingerprint(path) for path in watched}
        write_json(self.base / 'after.json', after)
        self.assertNotEqual(result.returncode, 0, 'existing stage must be refused')
        self.assertIn(b'Refusing existing stage', result.stderr)
        # The message must name the requested entry, not its symlink target.
        self.assertIn(str(requested_stage).encode(), result.stderr)
        self.assertEqual(after, before, 'existing stage entry or linked target changed')
        self.assertEqual(fingerprint(self.work), self.work_before, 'refusal reached work/build materialization')
        self.assertEqual(result.stdout, b'', 'existing-stage refusal should precede packaging work')

    def finish(self):
        write_json(self.base / 'result.json', {'passed': True, 'scope': 'Dummy manifest-only filesystem control',
            'builds_or_PHP_executions': 0})
        self.completed = True

    def test_populated_stage_preserves_binary_and_manifest(self):
        stage = self.base / 'stage'; self.populated(stage)
        self.unchanged_refusal(stage)
        self.finish()

    def test_empty_existing_directory_is_refused(self):
        stage = self.base / 'stage'; stage.mkdir()
        self.unchanged_refusal(stage)
        self.finish()

    def test_existing_regular_file_is_refused(self):
        stage = self.base / 'stage'; stage.write_bytes(b'preserve-stage-file\n')
        self.unchanged_refusal(stage)
        self.finish()

    def test_live_final_symlink_and_target_are_preserved(self):
        target = self.base / 'old-artifact'; self.populated(target)
        stage = self.base / 'requested-link'; stage.symlink_to(target, target_is_directory=True)
        self.unchanged_refusal(stage, [target])
        self.assertTrue(stage.is_symlink())
        self.assertEqual(os.readlink(stage), str(target))
        self.finish()

    def test_dangling_final_symlink_does_not_create_target(self):
        target = self.base / 'must-remain-absent'
        stage = self.base / 'requested-link'; stage.symlink_to(target, target_is_directory=True)
        self.assertFalse(os.path.lexists(target))
        self.unchanged_refusal(stage)
        self.assertTrue(stage.is_symlink())
        self.assertEqual(os.readlink(stage), str(target))
        self.assertFalse(os.path.lexists(target), 'resolving a dangling stage link created its target')
        self.finish()

    @unittest.skipUnless(shutil.which('readelf'), 'fresh non-ELF validation control requires readelf')
    def test_fresh_invalid_elf_leaves_only_new_incomplete_stage(self):
        other = self.base / 'untouched-artifact'; self.populated(other)
        before = fingerprint(other); write_json(self.base / 'other-before.json', before)
        stage = self.base / 'new-stage'
        self.assertFalse(os.path.lexists(stage))
        result = self.run_manifest(stage)
        after = fingerprint(other); write_json(self.base / 'other-after.json', after)
        self.assertNotEqual(result.returncode, 0, 'dummy non-ELF input cannot produce a completed package')
        self.assertNotIn(b'Refusing existing stage', result.stderr)
        self.assertIn(b'readelf', result.stderr)
        self.assertEqual((stage / 'h2o-php').read_bytes(), DUMMY)
        self.assertFalse((stage / 'manifest.json').exists(), 'failed validation claimed a completed package')
        self.assertEqual(after, before, 'fresh-stage failure modified an unrelated artifact')
        self.assertEqual(fingerprint(self.work), self.work_before, 'manifest-only validation modified build inputs')
        self.finish()


if __name__ == '__main__':
    unittest.main()
