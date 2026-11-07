"""PCNTL context ownership: python3 tests/test_pcntl_ownership.py [h2o-php binary]."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import sys
import time
import unittest
import uuid

from test_worker_pool import WorkerServer, wait_until


COMMON = r'''<?php
function key_name(): string {
    $key = $_GET['key'] ?? '';
    if (!preg_match('/^[a-f0-9]+$/D', $key)) throw new RuntimeException('bad private key');
    return __DIR__.'/'.$key;
}
function gate(string $path, float $seconds = 5): bool {
    $until = microtime(true) + $seconds;
    do { clearstatcache(true, $path); if (is_file($path)) return true; usleep(1000); } while (microtime(true) < $until);
    return false;
}
function emit_json(array $value): void { header('Content-Type: application/json'); echo json_encode($value, JSON_THROW_ON_ERROR); }
'''
OWNER = r'''<?php
require __DIR__.'/common.php';
while (frankenphp_handle_request(function () {
    $base = key_name(); $selector = $_GET['selector'] ?? 'any'; $probe = $_GET['probe'] ?? '';
    $pid = pcntl_fork();
    if ($pid < 0) throw new RuntimeException('fork failed');
    if ($pid === 0) {
        if ($selector === 'group' && !posix_setpgid(0, 0)) exit(97);
        file_put_contents($base.'.child-ready', (string)getmypid());
        exit(gate($base.'.child-release') ? 23 : 98);
    }
    if (!gate($base.'.child-ready')) throw new RuntimeException('child readiness');
    $observed = null;
    if ($probe === 'clone') {
        $status = 123; $value = pcntl_waitpid($pid, $status, WNOHANG | 0x80000000);
        $observed = ['result' => $value, 'errno' => pcntl_get_last_error(), 'expected_errno' => PCNTL_ECHILD, 'status' => $status];
    }
    file_put_contents($base.'.ready', (string)$pid);
    if (!gate($base.'.owner-reap')) throw new RuntimeException('owner reap gate');
    if ($probe === 'cached-invalid') {
        $info = []; pcntl_waitid(P_PID, $pid, $info, WEXITED | WNOWAIT);
        $until = microtime(true) + 1;
        do { clearstatcache(true, '/proc/'.$pid); $cached = !is_dir('/proc/'.$pid); if ($cached) break; usleep(1000); } while (microtime(true) < $until);
        $status = 123; $value = pcntl_waitpid($pid, $status, WNOHANG | 0x10000);
        $observed = ['result' => $value, 'errno' => pcntl_get_last_error(), 'expected_errno' => PCNTL_EINVAL,
            'status' => $status, 'broker_cached' => $cached];
    } elseif ($probe === 'negative-pgid') {
        $info = ['untouched' => true]; $value = pcntl_waitid(P_PGID, -$pid, $info, WEXITED | WNOHANG);
        $observed = ['result' => $value, 'errno' => pcntl_get_last_error(), 'expected_errno' => PCNTL_EINVAL, 'info' => $info];
    }
    $chosen = match ($selector) { 'exact' => $pid, 'current' => 0, 'group' => -$pid, default => -1 };
    $until = microtime(true) + 2; $status = 0;
    do { $got = pcntl_waitpid($chosen, $status, WNOHANG); if ($got !== 0) break; usleep(1000); } while (microtime(true) < $until);
    emit_json(['pid' => $pid, 'got' => $got, 'exited' => $got > 0 && pcntl_wifexited($status),
        'exit_status' => $got > 0 && pcntl_wifexited($status) ? pcntl_wexitstatus($status) : null, 'probe' => $observed]);
}));
'''
FOREIGN = r'''<?php
require __DIR__.'/common.php';
$base = key_name(); $pid = (int)file_get_contents($base.'.ready'); $method = $_GET['method'];
$alias = $_GET['alias'] ?? ''; $blocking = ($_GET['blocking'] ?? '') === '1';
$target = $alias === 'max' ? PHP_INT_MAX : ($alias === 'wrapped' ? $pid + 0x100000000 : $pid);
$bad = ($_GET['bad'] ?? '') === '1'; $status = 123; $info = [];
file_put_contents($base.'.foreign-entered', 'entered');
if ($method === 'waitpid') {
    $result = pcntl_waitpid($target, $status, ($blocking ? 0 : WNOHANG) | ($bad ? 0x10000 : 0));
} else {
    $kind = $alias === 'all-kind' ? P_ALL + 0x100000000 : P_PID;
    $result = pcntl_waitid($kind, $target, $info, WEXITED | ($blocking ? 0 : WNOHANG) | ($bad ? 0x10000 : 0));
}
emit_json(['result' => $result, 'errno' => pcntl_get_last_error(), 'expected_errno' => $bad ? PCNTL_EINVAL : PCNTL_ECHILD,
    'status' => $status, 'info' => $info]);
'''
PROC = r'''<?php
require __DIR__.'/common.php';
$base = key_name(); $method = $_GET['method']; $pipes = [];
$process = proc_open([PYTHON_BINARY, __DIR__.'/proc-child.py', $base],
    [0 => ['pipe','r'], 1 => ['pipe','w'], 2 => ['pipe','w']], $pipes);
if (!is_resource($process)) throw new RuntimeException('proc_open failed');
fclose($pipes[0]); if (!gate($base.'.proc-child-ready')) throw new RuntimeException('proc child readiness');
$state = proc_get_status($process); $pid = $state['pid'];
file_put_contents($base.'.ready', (string)$pid);
$status = 0; $info = []; $got = 0; $exit = null;
if ($method === 'waitpid') { $got = pcntl_waitpid($pid, $status, 0); if ($got > 0 && pcntl_wifexited($status)) $exit = pcntl_wexitstatus($status); }
elseif ($method === 'waitid') { $ok = pcntl_waitid(P_PID, $pid, $info, WEXITED); $got = $ok ? ($info['pid'] ?? 0) : 0; if ($got) $exit = $info['status']; }
$output = stream_get_contents($pipes[1]); $errors = stream_get_contents($pipes[2]);
fclose($pipes[1]); fclose($pipes[2]); $closed = proc_close($process);
emit_json(['pid' => $pid, 'got' => $got, 'exit_status' => $exit, 'proc_close' => $closed, 'stdout' => $output, 'stderr' => $errors]);
'''


class PCNTLOwnershipTest(unittest.TestCase):
    def setUp(self):
        self.server = WorkerServer(num=1)
        self.addCleanup(self.server.stop)
        self.base = self.server.base
        (self.base / 'common.php').write_text(COMMON)
        self.server.worker.write_text(OWNER)
        (self.base / 'foreign.php').write_text(FOREIGN)
        python_literal = "'" + sys.executable.replace('\\', '\\\\').replace("'", "\\'") + "'"
        (self.base / 'proc.php').write_text(PROC.replace('PYTHON_BINARY', python_literal))
        (self.base / 'proc-child.py').write_text("""from pathlib import Path
import os, sys, time
base = sys.argv[1]
Path(base+'.proc-child-ready').write_text(str(os.getpid()))
end = time.monotonic()+5
while not Path(base+'.child-release').exists() and time.monotonic()<end:
    time.sleep(.001)
raise SystemExit(19 if Path(base+'.child-release').exists() else 96)
""")
        config = self.server.config.read_text().replace('      /:\n        php.worker:', '      /owner:\n        php.worker:')
        self.server.config.write_text(config + '      /:\n        php_server: ON\n')
        self.server.start()

    def paths(self):
        key = uuid.uuid4().hex
        return key, self.base / key

    @staticmethod
    def touch(base, suffix):
        Path(str(base) + suffix).touch()

    def assert_owner(self, value):
        self.assertEqual(value['got'], value['pid'], value)
        self.assertTrue(value['exited'], value)
        self.assertEqual(value['exit_status'], 23, value)

    def foreign_case(self, method, *, blocking=False, alias='', bad=False, probe=''):
        key, base = self.paths()
        with ThreadPoolExecutor(max_workers=2) as pool:
            owner = pool.submit(self.server.json, f'/owner?key={key}&probe={probe}')
            try:
                wait_until(lambda: Path(str(base) + '.ready').exists())
                foreign = pool.submit(self.server.json,
                    f'/foreign.php?key={key}&method={method}&blocking={int(blocking)}&alias={alias}&bad={int(bad)}')
                wait_until(lambda: Path(str(base) + '.foreign-entered').exists())
                until = time.monotonic() + .25
                while not foreign.done() and time.monotonic() < until:
                    time.sleep(.002)
                returned_alive = foreign.done()
                self.touch(base, '.child-release')
                other = foreign.result(timeout=3)
                self.touch(base, '.owner-reap')
                own = owner.result(timeout=4)
            finally:
                self.touch(base, '.child-release')
                self.touch(base, '.owner-reap')
        self.assertTrue(returned_alive, (method, other, own))
        if method == 'waitpid':
            self.assertEqual(other['result'], -1, other)
            self.assertEqual(other['status'], 123, other)
        else:
            self.assertIs(other['result'], False, other)
        self.assertEqual(other['errno'], other['expected_errno'], other)
        self.assert_owner(own)
        if probe:
            observed = own['probe']
            self.assertEqual(observed['result'], -1, observed)
            self.assertEqual(observed['errno'], observed['expected_errno'], observed)
            self.assertEqual(observed['status'], 123, observed)

    def own_case(self, selector='any', probe=''):
        key, base = self.paths()
        with ThreadPoolExecutor(max_workers=1) as pool:
            owner = pool.submit(self.server.json, f'/owner?key={key}&selector={selector}&probe={probe}')
            try:
                wait_until(lambda: Path(str(base) + '.ready').exists())
                self.touch(base, '.child-release')
                self.touch(base, '.owner-reap')
                value = owner.result(timeout=4)
            finally:
                self.touch(base, '.child-release')
                self.touch(base, '.owner-reap')
        self.assert_owner(value)
        return value

    def test_known_foreign_positive_waits_and_wide_aliases(self):
        for method in ['waitpid', 'waitid']:
            for blocking in [False, True]:
                with self.subTest(method=method, blocking=blocking):
                    self.foreign_case(method, blocking=blocking)
            with self.subTest(method=method, bad_flags=True):
                self.foreign_case(method, bad=True)
        if sys.maxsize > 0xffffffff:
            for method, alias in [('waitpid', 'max'), ('waitpid', 'wrapped'), ('waitid', 'wrapped'), ('waitid', 'all-kind')]:
                with self.subTest(method=method, alias=alias):
                    self.foreign_case(method, alias=alias)

    def test_selection_and_cached_validation_keep_owner_record(self):
        self.foreign_case('waitpid', probe='clone')
        cached = self.own_case(probe='cached-invalid')['probe']
        self.assertTrue(cached['broker_cached'], cached)
        self.assertEqual(cached['result'], -1, cached)
        self.assertEqual(cached['errno'], cached['expected_errno'], cached)
        self.assertEqual(cached['status'], 123, cached)
        invalid_group = self.own_case(probe='negative-pgid')['probe']
        self.assertIs(invalid_group['result'], False, invalid_group)
        self.assertEqual(invalid_group['errno'], invalid_group['expected_errno'], invalid_group)
        self.assertEqual(invalid_group['info'], {'untouched': True}, invalid_group)

    def test_own_selectors_and_untracked_proc_open_fallback(self):
        for selector in ['exact', 'any', 'current', 'group']:
            with self.subTest(selector=selector):
                self.own_case(selector)
        for method in ['waitpid', 'waitid', 'close']:
            with self.subTest(untracked=method):
                key, base = self.paths()
                with ThreadPoolExecutor(max_workers=1) as pool:
                    pending = pool.submit(self.server.json, f'/proc.php?key={key}&method={method}')
                    try:
                        wait_until(lambda: Path(str(base) + '.ready').exists())
                        self.touch(base, '.child-release')
                        value = pending.result(timeout=4)
                    finally:
                        self.touch(base, '.child-release')
                if method == 'close':
                    self.assertEqual(value['proc_close'], 19, value)
                else:
                    self.assertEqual(value['got'], value['pid'], value)
                    self.assertEqual(value['exit_status'], 19, value)
                    # Explicit PCNTL wait consumes status outside proc_open's cache.
                    self.assertEqual(value['proc_close'], -1, value)
                self.assertEqual(value['stdout'], '', value)


if __name__ == '__main__':
    unittest.main()
