"""Linux PIDFD ownership/cache/descriptor regression controls; root alone runs workloads."""
from concurrent.futures import ThreadPoolExecutor
import array
import errno
import json
import os
from pathlib import Path
import socket
import sys
import time
import unittest
import uuid

from test_worker_pool import BINARY, WorkerServer, wait_until

# Standalone copy of the reviewed private SCM_RIGHTS fixture protocol.
HELPER = r'''<?php
$settings = json_decode(file_get_contents(__DIR__.'/pidfd-settings.json'), true, 512, JSON_THROW_ON_ERROR);
function marker(string $token, string $suffix): string {
    if (!preg_match('/^[a-f0-9]{32}$/D', $token)) throw new RuntimeException('invalid private token');
    return __DIR__.'/pidfd-'.$token.'-'.$suffix;
}
function publish(string $token, string $suffix, mixed $value): void {
    $path = marker($token, $suffix); $tmp = $path.'.tmp.'.getmypid();
    file_put_contents($tmp, json_encode($value, JSON_THROW_ON_ERROR)); rename($tmp, $path);
}
function wait_marker(string $token, string $suffix, float $seconds = 8): void {
    $path = marker($token, $suffix); $end = microtime(true) + $seconds;
    do {
        clearstatcache(true, $path); if (is_file($path)) return;
        usleep(1000);
    } while (microtime(true) < $end);
    throw new RuntimeException('bounded private gate timed out: '.$suffix);
}
function capabilities(): array {
    $libraries = [];
    foreach (explode("\n", file_get_contents('/proc/self/maps')) as $line)
        if (preg_match('~(/[^ ]*libphp[^ ]*\.so[^ ]*)$~', $line, $m)) $libraries[] = $m[1];
    $libraries = array_values(array_unique($libraries)); sort($libraries);
    return ['php' => PHP_VERSION, 'zts' => PHP_ZTS, 'int_size' => PHP_INT_SIZE,
        'sapi' => PHP_SAPI, 'pcntl_version' => phpversion('pcntl'), 'sockets_version' => phpversion('sockets'),
        'linux' => PHP_OS_FAMILY === 'Linux', 'waitid' => function_exists('pcntl_waitid'),
        'recvmsg' => function_exists('socket_recvmsg'), 'scm_rights' => defined('SCM_RIGHTS'),
        'P_PIDFD_defined' => defined('P_PIDFD'), 'kind' => defined('P_PIDFD') ? constant('P_PIDFD') : 3,
        'kind_source' => defined('P_PIDFD') ? 'P_PIDFD constant' : 'explicit Linux UAPI effective kind 3; native capability control required',
        'php_libraries' => $libraries,
        'library_sha256' => array_map(static fn($p) => hash_file('sha256', $p), $libraries)];
}
function own_child(string $token, string $mode): array {
    global $settings;
    $process = null; $pipes = []; $pid = 0;
    try {
        if ($mode === 'proc') {
            $process = proc_open([$settings['php_binary'], __DIR__.'/pidfd-child.php', $token],
                [0 => ['pipe','r'], 1 => ['pipe','w'], 2 => ['pipe','w']], $pipes);
            if (!is_resource($process)) throw new RuntimeException('proc_open failed');
            fclose($pipes[0]);
            $state = proc_get_status($process); $pid = $state['pid'];
            if (!$state['running']) throw new RuntimeException('proc child was not alive');
        } else {
            $pid = pcntl_fork();
            if ($pid < 0) throw new RuntimeException('private fork failed');
            if ($pid === 0) { private_child($token); exit(99); }
        }
        wait_marker($token, 'child-ready.json');
        $child = json_decode(file_get_contents(marker($token, 'child-ready.json')), true, 512, JSON_THROW_ON_ERROR);
        if ($child['pid'] !== $pid || $child['parent'] !== getmypid()) throw new RuntimeException('private child identity mismatch');
        publish($token, 'owner-ready.json', $child + ['mode' => $mode]);
        return [$child, $process, $pipes];
    } catch (Throwable $error) {
        if ($pid > 0) cleanup_owned($token, $pid, $process, $pipes);
        throw $error;
    }
}
function private_child(string $token): void {
    publish($token, 'child-ready.json', ['pid' => getmypid(), 'parent' => posix_getppid()]);
    try { wait_marker($token, 'child-release'); $exit = 23; }
    catch (Throwable $e) { $exit = 98; }
    publish($token, 'child-exiting.json', ['pid' => getmypid(), 'exit' => $exit]);
    exit($exit);
}
function matching_pidfds(int $pid): array {
    $matches = [];
    foreach (glob('/proc/self/fdinfo/[0-9]*') as $path) {
        $raw = @file_get_contents($path);
        if ($raw !== false && preg_match('/^Pid:\s+(-?\d+)$/m', $raw, $m) && (int)$m[1] === $pid) {
            $fd = (int)basename($path); $link = @readlink('/proc/self/fd/'.$fd);
            if ($link !== false && str_contains($link, '[pidfd]')) $matches[$fd] = ['fd' => $fd, 'fdinfo' => $raw, 'link' => $link];
        }
    }
    return $matches;
}
function receive_pidfd(string $token, int $pid): array {
    global $settings;
    $path = $settings['socket_directory'].'/'.$token.'.sock';
    $listener = socket_create(AF_UNIX, SOCK_STREAM, 0);
    if (!$listener || !socket_bind($listener, $path) || !socket_listen($listener, 1)) throw new RuntimeException('private Unix listener failed');
    socket_set_nonblock($listener); $connection = null;
    try {
        $before = matching_pidfds($pid);
        publish($token, 'receiver-ready.json', ['socket' => $path, 'target_pid' => $pid, 'receiver_pid' => getmypid(), 'before' => $before]);
        $end = microtime(true) + 4;
        do { $connection = @socket_accept($listener); if ($connection) break; usleep(1000); } while (microtime(true) < $end);
        if (!$connection) throw new RuntimeException('bounded private Unix accept timed out');
        socket_set_block($connection); socket_set_option($connection, SOL_SOCKET, SO_RCVTIMEO, ['sec' => 2, 'usec' => 0]);
        $message = ['buffer_size' => 1, 'controllen' => socket_cmsg_space(SOL_SOCKET, SCM_RIGHTS, 1)];
        $n = socket_recvmsg($connection, $message, defined('MSG_CMSG_CLOEXEC') ? MSG_CMSG_CLOEXEC : 0);
        if ($n !== 1 || implode('', $message['iov'] ?? []) !== 'P') throw new RuntimeException('invalid private PIDFD frame');
        $rights = [];
        foreach ($message['control'] ?? [] as $control) {
            if ($control['level'] !== SOL_SOCKET || $control['type'] !== SCM_RIGHTS) throw new RuntimeException('unexpected ancillary control');
            foreach ($control['data'] as $resource) $rights[] = $resource;
        }
        if (count($rights) !== 1) throw new RuntimeException('expected exactly one received descriptor');
        $matches = array_diff_key(matching_pidfds($pid), $before);
        if (count($matches) !== 1) throw new RuntimeException('could not uniquely discover real received pidfd through fdinfo');
        $identity = array_values($matches)[0];
        $identity['resource_type'] = is_resource($rights[0]) ? get_resource_type($rights[0]) : get_debug_type($rights[0]);
        publish($token, 'descriptor.json', $identity);
        return [$identity['fd'], $rights, $identity];
    } finally {
        if ($connection) socket_close($connection);
        socket_close($listener); @unlink($path);
    }
}
function pidfd_wait(int $fd, int $flags): array {
    $info = []; $usage = []; $kind = defined('P_PIDFD') ? constant('P_PIDFD') : 3; $start = microtime(true);
    $error = null;
    try { $result = pcntl_waitid($kind, $fd, $info, $flags, $usage); }
    catch (Throwable $exception) {
        $result = null; $error = ['class' => get_class($exception), 'message' => $exception->getMessage(), 'code' => $exception->getCode()];
    }
    $returned = hrtime(true);
    return ['result' => $result, 'info' => $info, 'pid' => $info['pid'] ?? 0,
        'exit_status' => ($info['pid'] ?? 0) ? ($info['status'] ?? null) : null,
        'code' => $info['code'] ?? null, 'errno' => pcntl_get_last_error(), 'ECHILD' => PCNTL_ECHILD,
        'CLD_EXITED' => CLD_EXITED, 'flags' => $flags, 'elapsed_seconds' => microtime(true) - $start,
        'usage' => $usage, 'effective_kind' => $kind, 'exception' => $error, 'returned_monotonic_ns' => $returned,
        'kind_source' => defined('P_PIDFD') ? 'P_PIDFD constant' : 'explicit Linux UAPI effective kind 3; native capability control required'];
}
function wait_owned_pid(int $pid, float $seconds = 2): array {
    $end = microtime(true) + $seconds;
    do {
        $status = 0; $result = pcntl_waitpid($pid, $status, WNOHANG); $errno = pcntl_get_last_error();
        if ($result !== 0) break;
        usleep(1000);
    } while (microtime(true) < $end);
    return ['result' => $result, 'errno' => $errno, 'ECHILD' => PCNTL_ECHILD,
        'exit_status' => $result > 0 && pcntl_wifexited($status) ? pcntl_wexitstatus($status) : null,
        'exited' => $result > 0 && pcntl_wifexited($status), 'raw_status' => $status];
}
function cleanup_owned(string $token, int $pid, mixed &$process, array &$pipes): void {
    // Cleanup observations are separate; never replace the original wait result.
    $released = touch(marker($token, 'child-release')); touch(marker($token, 'owner-reap'));
    $wait = wait_owned_pid($pid, 4); $proc = null;
    if (is_resource($process) && $wait['result'] !== 0) {
        $io = [];
        foreach ($pipes as $key => $pipe) if (is_resource($pipe)) {
            stream_set_blocking($pipe, false); $io[$key] = stream_get_contents($pipe); fclose($pipe);
        }
        $proc = ['close' => proc_close($process), 'remaining_io' => $io]; $process = null; $pipes = [];
    }
    publish($token, 'cleanup.json', ['release_written' => $released, 'wait' => $wait, 'proc' => $proc,
        'reaped_or_already_consumed' => $wait['result'] === $pid || ($wait['result'] === -1 && $wait['errno'] === $wait['ECHILD'])]);
}
function owned_case(string $token, string $mode): array {
    $child = null; $process = null; $pipes = []; $rights = [];
    try {
        [$child, $process, $pipes] = own_child($token, $mode);
        [$fd, $rights, $descriptor] = receive_pidfd($token, $child['pid']);
        $live = pidfd_wait($fd, WEXITED | WNOHANG | WNOWAIT);
        publish($token, 'live-observed.json', $live);
        publish($token, 'peek-entered', true);
        $peek = pidfd_wait($fd, WEXITED | WNOWAIT);
        $reap = pidfd_wait($fd, WEXITED);
        $after = wait_owned_pid($child['pid']);
        $proc = null;
        if ($process) {
            $stdout = stream_get_contents($pipes[1]); $stderr = stream_get_contents($pipes[2]);
            fclose($pipes[1]); fclose($pipes[2]);
            $proc = ['close' => proc_close($process), 'stdout' => $stdout, 'stderr' => $stderr];
            $process = null; $pipes = [];
        }
        return ['mode' => $mode, 'parent' => getmypid(), 'child' => $child, 'descriptor' => $descriptor,
            'live' => $live, 'peek' => $peek, 'reap' => $reap, 'after' => $after, 'proc' => $proc];
    } finally {
        unset($rights); // Keep the received descriptor alive through all wait calls.
        if ($child) cleanup_owned($token, $child['pid'], $process, $pipes);
    }
}
'''

WORKER = r'''<?php
require __DIR__.'/pidfd-helper.php';
while (frankenphp_handle_request(function () {
    $token = $_GET['token']; $mode = $_GET['mode'] ?? 'fork';
    if ($mode === 'foreign-owner') {
        $child = null; $process = null; $pipes = [];
        try {
            [$child, $process, $pipes] = own_child($token, 'fork'); wait_marker($token, 'owner-reap');
            $value = ['parent' => getmypid(), 'child' => $child, 'wait' => wait_owned_pid($child['pid'])];
        } finally { if ($child) cleanup_owned($token, $child['pid'], $process, $pipes); }
    } else $value = owned_case($token, $mode);
    header('Content-Type: application/json'); echo json_encode($value, JSON_THROW_ON_ERROR);
}));
'''

FOREIGN = r'''<?php
require __DIR__.'/pidfd-helper.php';
$token = $_GET['token']; $blocking = ($_GET['blocking'] ?? '') === '1';
$child = json_decode(file_get_contents(marker($token, 'owner-ready.json')), true, 512, JSON_THROW_ON_ERROR);
[$fd, $rights, $descriptor] = receive_pidfd($token, $child['pid']);
publish($token, 'foreign-entered', true);
$wait = pidfd_wait($fd, WEXITED | ($blocking ? 0 : WNOHANG));
clearstatcache(true, marker($token, 'child-release')); clearstatcache(true, marker($token, 'child-exiting.json'));
publish($token, 'foreign-returned.json', ['wait' => $wait,
    'child_release_observed' => is_file(marker($token, 'child-release')),
    'child_exit_observed' => is_file(marker($token, 'child-exiting.json')), 'observed_monotonic_ns' => hrtime(true)]);
unset($rights);
header('Content-Type: application/json');
echo json_encode(['parent' => getmypid(), 'target_pid' => $child['pid'], 'descriptor' => $descriptor,
    'blocking' => $blocking, 'wait' => $wait], JSON_THROW_ON_ERROR);
'''

EDGE = r'''
function fdinfo(int $fd): array {
    $raw = file_get_contents('/proc/self/fdinfo/'.$fd);
    preg_match('/^Pid:\s+(-?\d+)$/m', $raw, $match);
    return ['raw' => $raw, 'pid' => isset($match[1]) ? (int)$match[1] : null];
}
function real_fds(): array {
    $paths = [];
    foreach (glob('/proc/self/fdinfo/[0-9]*') as $path)
        if (@readlink('/proc/self/fd/'.basename($path)) !== false) $paths[] = $path;
    return $paths;
}
function edge_wait(int $fd, int $flags, int $kind = 3): array {
    global $settings;
    $info = ['untouched' => 123]; $usage = [];
    $result = pcntl_waitid($kind, $fd, $info, $flags, $usage);
    return ['result' => $result, 'info' => $info, 'usage' => $usage, 'errno' => pcntl_get_last_error(),
        'ECHILD' => $settings['errno']['ECHILD'], 'EINVAL' => $settings['errno']['EINVAL'],
        'EBADF' => $settings['errno']['EBADF'], 'EAGAIN' => $settings['errno']['EAGAIN']];
}
function edge_case(string $token, string $mode): array {
    $child = null; $process = null; $pipes = []; $rights = []; $keepRights = []; $pair = []; $replacements = [];
    try {
        [$child, $process, $pipes] = own_child($token, str_starts_with($mode, 'proc') ? 'proc' : 'fork');
        [$fd, $rights, $descriptor] = receive_pidfd($token, $child['pid']);
        $live = edge_wait($fd, WEXITED | WNOHANG | WNOWAIT);
        $badFlags = edge_wait($fd, WEXITED | 0x10000);
        $missingEvent = edge_wait($fd, WNOHANG);
        $negative = edge_wait(-1, WEXITED | WNOHANG);
        $nonblock = str_contains($mode, 'nonblock') ? edge_wait($fd, WEXITED | WNOWAIT) : null;
        $wide = PHP_INT_SIZE > 4 ? [
            edge_wait($fd + 0x100000000, WEXITED | WNOHANG),
            edge_wait($fd, WEXITED | WNOHANG, 3 + 0x100000000),
            edge_wait($fd, WEXITED | WNOHANG | 0x100000000)] : [];
        publish($token, 'live-observed.json', true);
        wait_marker($token, 'child-exiting.json');
        $until = microtime(true) + 2; $identity = fdinfo($fd);
        if ($mode === 'cached') {
            do { $identity = fdinfo($fd); if ($identity['pid'] === -1) break; usleep(1000); } while (microtime(true) < $until);
            if ($identity['pid'] !== -1) throw new RuntimeException('broker did not cache private child');
        } else {
            do { $ready = edge_wait($fd, WEXITED | WNOHANG | WNOWAIT); if (($ready['info']['pid'] ?? 0) === $child['pid']) break; usleep(1000); } while (microtime(true) < $until);
        }
        $stopOnly = edge_wait($fd, WSTOPPED | WNOHANG);
        $continueOnly = edge_wait($fd, WCONTINUED | WNOHANG | WNOWAIT);
        $cloneOnly = edge_wait($fd, WEXITED | WNOHANG | 0x80000000);
        // Keep another reference, then close/reuse the original number before
        // querying the still-cached status through the genuine PIDFD again.
        // Duplicate through a private SCM_RIGHTS pair. php://fd is restricted
        // to CLI SAPIs and must not be used as a server-fixture shortcut.
        if (!socket_create_pair(AF_UNIX, SOCK_STREAM, 0, $pair)) throw new RuntimeException('private duplicate socketpair failed');
        $allBefore = real_fds();
        $send = ['iov' => ['D'], 'control' => [['level' => SOL_SOCKET, 'type' => SCM_RIGHTS, 'data' => [$rights[0]]]]];
        if (socket_sendmsg($pair[0], $send) !== 1) throw new RuntimeException('private duplicate send failed');
        unset($send);
        $keepMessage = ['buffer_size' => 1, 'controllen' => socket_cmsg_space(SOL_SOCKET, SCM_RIGHTS, 1)];
        if (socket_recvmsg($pair[1], $keepMessage) !== 1) throw new RuntimeException('private duplicate receive failed');
        $keepRights = $keepMessage['control'][0]['data']; unset($keepMessage);
        $newPaths = array_values(array_diff(real_fds(), $allBefore));
        $candidates = [];
        foreach ($newPaths as $path) {
            $candidate = (int)basename($path);
            if (@readlink('/proc/self/fd/'.$candidate) === $descriptor['link']) $candidates[] = $candidate;
        }
        if (count($candidates) !== 1) throw new RuntimeException('keeper FD discovery failed');
        $keptFd = $candidates[0]; foreach ($pair as $socket) socket_close($socket); $pair = []; unset($rights);
        $closed = edge_wait($fd, WEXITED | WNOHANG);
        $closedBad = edge_wait($fd, 0);
        $reused = false;
        for ($i = 0; $i < 16; ++$i) {
            $replacements[] = fopen('/dev/null', 'r');
            if (@readlink('/proc/self/fd/'.$fd) === '/dev/null') { $reused = true; break; }
        }
        if (!$reused) throw new RuntimeException('original descriptor was not reused as intended');
        $nonPidfd = edge_wait($fd, WEXITED | WNOHANG);
        $nonPidfdBad = edge_wait($fd, 0);
        $peek = edge_wait($keptFd, WEXITED | WNOWAIT);
        $peekAgain = edge_wait($keptFd, WEXITED | WNOWAIT);
        $reap = edge_wait($keptFd, WEXITED);
        $after = edge_wait($keptFd, WEXITED | WNOHANG);
        return compact('mode', 'child', 'descriptor', 'identity', 'live', 'badFlags', 'missingEvent', 'negative', 'nonblock',
            'wide', 'stopOnly', 'continueOnly', 'cloneOnly', 'closed', 'closedBad', 'reused', 'nonPidfd', 'nonPidfdBad',
            'peek', 'peekAgain', 'reap', 'after');
    } finally {
        unset($rights);
        unset($keepRights);
        foreach ($pair as $socket) socket_close($socket);
        foreach ($replacements as $replacement) if (is_resource($replacement)) fclose($replacement);
        if ($child) cleanup_owned($token, $child['pid'], $process, $pipes);
    }
}
function cached_foreign(string $token): array {
    $child = json_decode(file_get_contents(marker($token, 'owner-ready.json')), true, 512, JSON_THROW_ON_ERROR);
    [$fd, $rights, $descriptor] = receive_pidfd($token, $child['pid']);
    publish($token, 'cache-receiver-ready', true); wait_marker($token, 'child-exiting.json');
    $end = microtime(true) + 2;
    do { $identity = fdinfo($fd); if ($identity['pid'] === -1) break; usleep(1000); } while (microtime(true) < $end);
    if ($identity['pid'] !== -1) throw new RuntimeException('foreign child did not become broker-cached');
    $wait = edge_wait($fd, WEXITED | WNOWAIT); unset($rights);
    return compact('child', 'descriptor', 'identity', 'wait');
}
function fork_in_task(): array {
    $runtime = new parallel\Runtime();
    $future = $runtime->run(static function (): array {
        $forked = pcntl_fork();
        return ['forked' => $forked, 'errno' => $forked < 0 ? pcntl_get_last_error() : 0];
    });
    $result = $future->value(); $runtime->close();
    return $result;
}
'''

@unittest.skipUnless(sys.platform == 'linux' and hasattr(os, 'pidfd_open'), 'Linux pidfd_open required')
class PCNTLPidfdTest(unittest.TestCase):
    def setUp(self):
        self.server = WorkerServer(num=1)
        self.addCleanup(self.server.stop)
        self.base = self.server.base
        (self.base / 'pidfd-helper.php').write_text(HELPER + EDGE)
        (self.base / 'pidfd-settings.json').write_text(json.dumps({'socket_directory': str(self.base), 'php_binary': str(BINARY),
            'errno': {key: getattr(errno, key) for key in ['ECHILD', 'EINVAL', 'EBADF', 'EAGAIN']}}))
        self.server.worker.write_text(WORKER)
        (self.base / 'foreign.php').write_text(FOREIGN)
        (self.base / 'edge.php').write_text("<?php require __DIR__.'/pidfd-helper.php'; header('Content-Type: application/json'); echo json_encode(edge_case($_GET['token'], $_GET['mode']), JSON_THROW_ON_ERROR);\n")
        (self.base / 'cached-foreign.php').write_text("<?php require __DIR__.'/pidfd-helper.php'; header('Content-Type: application/json'); echo json_encode(cached_foreign($_GET['token']), JSON_THROW_ON_ERROR);\n")
        (self.base / 'fork-in-task.php').write_text("<?php require __DIR__.'/pidfd-helper.php'; header('Content-Type: application/json'); echo json_encode(fork_in_task(), JSON_THROW_ON_ERROR);\n")
        (self.base / 'pidfd-child.php').write_text("<?php require __DIR__.'/pidfd-helper.php'; private_child($argv[1]);\n")
        config = self.server.config.read_text().replace('      /:\n        php.worker:', '      /worker:\n        php.worker:')
        self.server.config.write_text(config + '      /:\n        php_server: ON\n')
        self.server.start()

    def marker(self, token, suffix):
        return self.base / f'pidfd-{token}-{suffix}'

    def wait(self, token, suffix, future=None):
        path = self.marker(token, suffix)
        def ready():
            if path.exists():
                return True
            if future is not None and future.done():
                future.result()  # Surface the retained HTTP/PHP error immediately.
                raise AssertionError(f'PHP endpoint returned before marker {suffix}')
            return False
        wait_until(ready, timeout=4)
        return json.loads(path.read_text())

    def release(self, token):
        self.marker(token, 'child-release').touch()
        self.marker(token, 'owner-reap').touch()

    def transfer(self, token, *, nonblock=False, future=None):
        receiver = self.wait(token, 'receiver-ready.json', future)
        child = self.wait(token, 'owner-ready.json', future)
        self.assertEqual(receiver['target_pid'], child['pid'])
        self.assertEqual(receiver['receiver_pid'], self.server.process.pid)
        fd = os.pidfd_open(child['pid'], os.O_NONBLOCK if nonblock else 0)
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as channel:
                channel.settimeout(2); channel.connect(receiver['socket'])
                self.assertEqual(channel.sendmsg([b'P'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))]), 1)
        finally:
            os.close(fd)
        return child

    def assert_denied(self, row, errno):
        self.assertIs(row['result'], False, row)
        self.assertEqual(row['errno'], row[errno], row)

    def assert_exit23(self, row, child):
        self.assertIs(row['result'], True, row)
        self.assertEqual(row['info']['pid'], child['pid'], row)
        self.assertEqual(row['info']['status'], 23, row)

    def finish_metrics(self, before, completed):
        def clean():
            after = self.server.metrics()
            return all(after[key] == 0 for key in ['active_php_threads', 'busy_workers', 'queued_requests', 'buffered_bytes', 'auxiliary_threads'])
        wait_until(clean)
        after = self.server.metrics()
        self.assertEqual(after['completed'] - before['completed'], completed, (before, after))
        self.assertEqual(after['failed'], before['failed'], (before, after))
        self.assertEqual(after['timed_out'], before['timed_out'], (before, after))

    def test_known_foreign_pidfd_live_and_blocking_keep_owner_status(self):
        before = self.server.metrics()
        for blocking in [False, True]:
            with self.subTest(blocking=blocking):
                token = uuid.uuid4().hex
                with ThreadPoolExecutor(max_workers=2) as pool:
                    owner = pool.submit(self.server.json, f'/worker?token={token}&mode=foreign-owner')
                    try:
                        self.wait(token, 'owner-ready.json', owner)
                        foreign = pool.submit(self.server.json, f'/foreign.php?token={token}&blocking={int(blocking)}')
                        self.transfer(token, future=foreign)
                        self.wait(token, 'foreign-entered', foreign)
                        # Allow a finite observation then release regardless of a
                        # detected defect. HTTP scheduling is not a wait verdict.
                        until = time.monotonic() + .25
                        while not self.marker(token, 'foreign-returned.json').exists() and time.monotonic() < until: time.sleep(.002)
                        self.marker(token, 'child-release').touch()
                        other = foreign.result(timeout=4)
                        self.marker(token, 'owner-reap').touch()
                        own = owner.result(timeout=4)
                    finally:
                        self.release(token)
                self.assert_denied(other['wait'], 'ECHILD')
                self.assertEqual(own['wait']['result'], own['child']['pid'], own)
                self.assertEqual(own['wait']['exit_status'], 23, own)
                self.assertTrue(self.wait(token, 'cleanup.json')['reaped_or_already_consumed'])
        self.finish_metrics(before, 4)

    def edge_case(self, mode):
        token = uuid.uuid4().hex
        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(self.server.json, f'/edge.php?token={token}&mode={mode}')
            try:
                self.transfer(token, nonblock='nonblock' in mode, future=future)
                self.wait(token, 'live-observed.json', future)
                self.marker(token, 'child-release').touch()
                value = future.result(timeout=6)
            finally:
                self.release(token)
        self.assertTrue(self.wait(token, 'cleanup.json')['reaped_or_already_consumed'])
        return value

    def assert_edges(self, value):
        self.assertIs(value['live']['result'], True, value)
        self.assertEqual(value['live']['info']['pid'], 0, value)
        for name in ['badFlags', 'missingEvent', 'negative', 'closedBad', 'nonPidfdBad']:
            self.assert_denied(value[name], 'EINVAL')
        for name in ['stopOnly', 'continueOnly', 'cloneOnly', 'after']:
            self.assert_denied(value[name], 'ECHILD')
        for name in ['closed', 'nonPidfd']:
            self.assert_denied(value[name], 'EBADF')
        self.assertTrue(value['reused'])
        for name in ['peek', 'peekAgain', 'reap']:
            self.assert_exit23(value[name], value['child'])
        for row in value['wide']:
            self.assertIs(row['result'], True, row)
            self.assertEqual(row['info']['pid'], 0, row)

    def test_broker_cached_pidfd_lexical_identity_flags_and_reused_fd(self):
        before = self.server.metrics()
        value = self.edge_case('cached')
        self.assertEqual(value['identity']['pid'], -1, value)
        self.assert_edges(value)
        token = uuid.uuid4().hex
        with ThreadPoolExecutor(max_workers=2) as pool:
            owner = pool.submit(self.server.json, f'/worker?token={token}&mode=foreign-owner')
            try:
                self.wait(token, 'owner-ready.json', owner)
                foreign = pool.submit(self.server.json, f'/cached-foreign.php?token={token}')
                self.transfer(token, future=foreign); self.wait(token, 'cache-receiver-ready', foreign)
                self.marker(token, 'child-release').touch()
                other = foreign.result(timeout=4)
                self.marker(token, 'owner-reap').touch()
                own = owner.result(timeout=4)
            finally:
                self.release(token)
        self.assertEqual(other['identity']['pid'], -1, other)
        self.assert_denied(other['wait'], 'ECHILD')
        self.assertEqual(own['wait']['result'], own['child']['pid'], own)
        self.assertEqual(own['wait']['exit_status'], 23, own)
        self.finish_metrics(before, 3)

    def test_nonblocking_tracked_and_untracked_pidfds(self):
        before = self.server.metrics()
        for mode in ['fork-nonblock', 'proc-nonblock']:
            with self.subTest(mode=mode):
                value = self.edge_case(mode)
                self.assert_denied(value['nonblock'], 'EAGAIN')
                self.assert_edges(value)
        self.finish_metrics(before, 2)

    def test_fork_inside_parallel_task_is_refused(self):
        before = self.server.metrics()
        value = self.server.json('/fork-in-task.php')
        self.assertEqual(value['forked'], -1, value)
        self.assertEqual(value['errno'], errno.EPERM, value)
        self.finish_metrics(before, 1)


if __name__ == '__main__':
    unittest.main()

