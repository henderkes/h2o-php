"""Active supervisor log backpressure: python3 tests/test_master_log_backpressure.py [binary].

Linux x86-64 /proc syscall evidence is part of the finite setup oracle. The ten
declared cells never release a blocked reader to obtain a successful deadline
verdict. H2O_PHP_TEST_OUTPUT optionally retains all raw evidence in a fresh
subdirectory; otherwise the same evidence lives in a private temporary directory.
"""
import array
import fcntl
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest
import uuid


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/release/h2o-php").resolve()
HOST = "master-log.test"
HEALTH = b"master-log-static-health\x00\xff\n"
LOG_PAYLOAD_MAX = 4 * 1024 * 1024
CAPTURE_MAX = 8 * 1024 * 1024
SHUTDOWN_BOUND = 1.5
CELLS = [
    ("configured", sink, action, False)
    for sink in ("fifo", "unix") for action in ("stop", "signal", "reload")
] + [
    ("inherited", sink, "flags", nonblocking)
    for sink in ("fifo", "unix") for nonblocking in (False, True)
]

LOG_PHP = r'''<?php
$token = $_GET['token'] ?? '';
if (!preg_match('/^[a-f0-9]{32}$/D', $token)) throw new RuntimeException('bad test token');
function publish(string $path, array $value): void {
    $temp = $path.'.tmp';
    if (file_put_contents($temp, json_encode($value, JSON_THROW_ON_ERROR)) === false || !rename($temp, $path))
        throw new RuntimeException('cannot publish test marker');
}
$prefix = __DIR__.'/log-'.$token;
$owner = ['token' => $token, 'pid' => getmypid(),
    'tid' => (int)strtok(file_get_contents('/proc/thread-self/stat'), ' '),
    'monotonic_ns' => hrtime(true), 'fd2_target' => readlink('/proc/self/fd/2'),
    'fd2_info' => file_get_contents('/proc/self/fdinfo/2')];
publish($prefix.'.entered.json', $owner);
// JSON overhead plus the fixed fields remains below the declared 4 MiB bound.
$padding = ($_GET['burst'] ?? '') === '1' ? str_repeat('L', 4193280) : '';
frankenphp_log('master-log:'.$token, 2, ['probe' => 'master-log-test', 'token' => $token, 'padding' => $padding]);
publish($prefix.'.returned.json', $owner + ['returned_monotonic_ns' => hrtime(true)]);
$body = 'log-ok:'.$token."\n";
header('Content-Type: text/plain'); header('Content-Length: '.strlen($body)); echo $body;
'''
WORKER_PHP = r'''<?php
$boot = bin2hex(random_bytes(16));
while (frankenphp_handle_request(function () use ($boot) {
    header('Content-Type: application/json');
    echo json_encode(['boot' => $boot, 'version' => $_SERVER['VERSION'], 'pid' => getmypid()], JSON_THROW_ON_ERROR);
}));
'''


def need(value, message):
    if not value:
        raise AssertionError(message)


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def until(predicate, seconds, message):
    end = time.monotonic() + seconds
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() >= end:
            raise AssertionError(message)
        time.sleep(.005)


def process_info(pid, expected=None, thread=None):
    """Only inspect owned identities; no descendant waitpid or /proc FD opens."""
    value = {"pid": pid, "monotonic_ns": time.monotonic_ns(), "exists": False, "errors": []}
    root = Path(f"/proc/{pid}")
    try:
        raw = (root / "stat").read_text(); fields = raw.rsplit(")", 1)[1].split()
        value.update(exists=True, stat=raw, state=fields[0], ppid=int(fields[1]),
                     pgrp=int(fields[2]), session=int(fields[3]), birth=int(fields[19]))
        value["same_birth"] = expected is None or value["birth"] == expected["birth"]
        if not value["same_birth"]:
            return value
        value["exe"] = os.readlink(root / "exe")
        value["cmdline"] = (root / "cmdline").read_bytes().replace(b"\0", b"\n").decode(errors="replace")
        value["children"] = [int(x) for x in (root / "task" / str(pid) / "children").read_text().split()]
        value["fds"] = {}
        for fd in (1, 2):
            link = root / "fd" / str(fd); status = link.stat()
            info = (root / "fdinfo" / str(fd)).read_text()
            flags = int(next(line.split()[1] for line in info.splitlines() if line.startswith("flags:")), 8)
            value["fds"][str(fd)] = {"target": os.readlink(link), "device": status.st_dev,
                "inode": status.st_ino, "mode": status.st_mode, "fdinfo": info, "flags": flags,
                "status_flags": flags & ~os.O_CLOEXEC}
        if thread is not None:
            task = root / "task" / str(thread)
            value["thread"] = {"tid": thread}
            for name in ("status", "syscall", "wchan"):
                value["thread"][name] = (task / name).read_text()
    except FileNotFoundError:
        # A process may disappear after its stat read; keep all partial evidence.
        value["disappeared_during_read"] = True
    except (OSError, ValueError, StopIteration) as error:
        value["errors"].append(f"{type(error).__name__}: {error}")
    return value


def same_live(expected):
    try:
        fields = Path(f"/proc/{expected['pid']}/stat").read_text().rsplit(")", 1)[1].split()
        return int(fields[19]) == expected["birth"]
    except FileNotFoundError:
        return False


def identity(value):
    return {key: value[key] for key in ("pid", "birth", "ppid", "pgrp", "session")}


class Sink:
    """One bounded draining reader, with an explicit no-more-reads pause ACK."""
    def __init__(self, directory, control, kind, inherited, nonblocking):
        self.directory, self.kind = directory, kind
        self.path = control / ("errors.fifo" if kind == "fifo" else "errors.sock")
        self.listener = self.connection = self.writer_socket = None
        self.reader_fd = self.filler_fd = self.writer_fd = None
        self.capacity = None; self.peer = None; self.accepted = 0; self.eof = False
        self.condition = threading.Condition()
        self.want_pause = self.paused = self.stopping = False
        self.error = None; self.events = []; self.total = 0
        self.thread = self.raw = None
        self.raw_path = directory / "sink.raw"; self.raw = self.raw_path.open("xb", buffering=0)
        if kind == "fifo":
            os.mkfifo(self.path, 0o600)
            self.reader_fd = os.open(self.path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            self.filler_fd = os.open(self.path, os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            self.capacity = fcntl.fcntl(self.filler_fd, fcntl.F_GETPIPE_SZ)
            need(0 < self.capacity <= 1048576, "FIFO capacity exceeds finite filler bound")
            if inherited:
                flags = os.O_WRONLY | os.O_APPEND | os.O_CLOEXEC | (os.O_NONBLOCK if nonblocking else 0)
                self.writer_fd = os.open(self.path, flags)
        elif inherited:
            self.connection, self.writer_socket = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
            self.connection.setblocking(False)
            self.writer_socket.setblocking(not nonblocking)
            self.writer_fd = self.writer_socket.fileno()
            self.accepted = 1
            self.peer = {"source": "controller-created socketpair, exact writer inherited by Popen",
                         "credentials": list(struct.unpack("3i", self.connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)))}
        else:
            self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.listener.bind(str(self.path)); self.listener.listen(2); self.listener.setblocking(False)
        self.initial_writer = self.writer_info()
        self.thread = threading.Thread(target=self.run, name="master-log-reader", daemon=True)
        self.thread.start()

    def writer_info(self):
        if self.writer_fd is None:
            return None
        value = os.fstat(self.writer_fd)
        return {"fd": self.writer_fd, "device": value.st_dev, "inode": value.st_ino,
                "flags": fcntl.fcntl(self.writer_fd, fcntl.F_GETFL)}

    def check(self):
        need(self.error is None, "log reader failed: " + str(self.error))

    def pending(self):
        fd = self.reader_fd if self.kind == "fifo" else (None if self.connection is None else self.connection.fileno())
        if fd is None:
            return 0
        value = array.array("i", [0]); fcntl.ioctl(fd, termios.FIONREAD, value, True)
        return value[0]

    def state(self):
        with self.condition:
            return {"kind": self.kind, "path": str(self.path), "paused": self.paused,
                "want_pause": self.want_pause, "pending": self.pending(), "capacity": self.capacity,
                "bytes_read": self.total, "accepted": self.accepted, "peer": self.peer,
                "error": self.error, "events": list(self.events), "monotonic_ns": time.monotonic_ns()}

    def run(self):
        try:
            while True:
                with self.condition:
                    if self.want_pause:
                        self.paused = True; self.events.append({"event": "pause-ack", "monotonic_ns": time.monotonic_ns()})
                        self.condition.notify_all()
                        self.condition.wait_for(lambda: not self.want_pause or self.stopping)
                        self.paused = False; self.events.append({"event": "resume-ack", "monotonic_ns": time.monotonic_ns()})
                        self.condition.notify_all()
                    if self.stopping:
                        return
                if self.listener is not None and select.select([self.listener], [], [], .005)[0]:
                    connection, _ = self.listener.accept(); connection.setblocking(False)
                    if self.connection is not None:
                        connection.close()
                        raise AssertionError("unexpected second active Unix log connection")
                    with self.condition:
                        self.connection = connection; self.accepted += 1
                        self.peer = {"credentials": list(struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))),
                            "reader_inode": os.fstat(connection.fileno()).st_ino}
                        self.condition.notify_all()
                fd = self.reader_fd if self.kind == "fifo" else self.connection
                if fd is None or self.eof:
                    time.sleep(.005); continue
                if not select.select([fd], [], [], .005)[0]:
                    continue
                try:
                    data = os.read(fd if isinstance(fd, int) else fd.fileno(), 65536)
                except BlockingIOError:
                    continue
                if not data:
                    # A FIFO keeps its independent filler open; Unix EOF is final.
                    self.eof = self.kind == "unix"; continue
                need(self.total + len(data) <= CAPTURE_MAX, "finite raw log capture limit exceeded")
                self.raw.write(data)
                with self.condition:
                    self.total += len(data); self.condition.notify_all()
        except BaseException as error:
            with self.condition:
                self.error = f"{type(error).__name__}: {error}"; self.condition.notify_all()

    def pause(self):
        until(lambda: self.pending() == 0, 1, "reader did not drain before pause")
        with self.condition:
            self.want_pause = True; self.condition.notify_all()
            need(self.condition.wait_for(lambda: self.paused or self.error, timeout=1), "reader pause ACK timed out")
        self.check(); save(self.directory / "reader-pause.json", self.state())
        if self.kind == "fifo":
            written = 0; calls = 0; eagain = False
            end = time.monotonic() + 1
            while time.monotonic() < end and calls <= self.capacity + 1:
                room = max(0, self.capacity - self.pending())
                try:
                    written += os.write(self.filler_fd, b"\n" * max(1, min(4096, room))); calls += 1
                except BlockingIOError:
                    eagain = True; break
            value = {"written": written, "calls": calls, "EAGAIN": eagain, "state": self.state()}
            save(self.directory / "fifo-full.json", value)
            need(eagain and self.pending() == self.capacity, "full FIFO/EAGAIN not established")

    def nonblocking_socket_full(self):
        """Only the retained identical writer; PHP already attempted the large write."""
        need(self.kind == "unix" and self.writer_socket is not None, "no identical Unix writer retained")
        written = 0; eagain = False
        for _ in range(4097):
            try:
                count = self.writer_socket.send(b"\n", socket.MSG_DONTWAIT | socket.MSG_NOSIGNAL)
                need(count == 1, "short one-byte saturation check")
                written += count
            except BlockingIOError:
                eagain = True; break
        value = {"EAGAIN": eagain, "extra_newline_bytes": written, "state": self.state(),
                 "retained_writer": self.writer_info(), "per_call_flags_only": True}
        save(self.directory / "unix-nonblocking-full.json", value)
        need(eagain and self.pending() > 0, "same Unix writer full/EAGAIN not established")

    def resume(self):
        with self.condition:
            self.events.append({"event": "release-call", "monotonic_ns": time.monotonic_ns()})
            self.want_pause = False; self.condition.notify_all()
            need(self.condition.wait_for(lambda: not self.paused or self.error, timeout=1), "reader release ACK timed out")
        self.check()

    def close(self):
        errors = []
        try:
            self.resume()
            until(lambda: self.pending() == 0 or self.error, 1, "reader cleanup drain timed out")
        except BaseException as error:
            errors.append(f"reader release/drain: {type(error).__name__}: {error}")
        with self.condition:
            self.want_pause = False; self.stopping = True; self.condition.notify_all()
        if self.thread is not None:
            self.thread.join(timeout=1)
            if self.thread.is_alive():
                errors.append("reader join timed out")
        save(self.directory / "reader-final.json", self.state())
        if self.error:
            errors.append(self.error)
        if self.raw is not None:
            self.raw.close()
        for value in (self.listener, self.connection, self.writer_socket):
            if value is not None:
                value.close()
        for fd in (self.reader_fd, self.filler_fd, self.writer_fd if self.writer_socket is None else None):
            if fd is not None:
                os.close(fd)
        return errors


class Request:
    def __init__(self, case, path, label):
        self.case, self.path, self.label = case, path, label
        self.done = threading.Event(); self.connection = None; self.result = {"path": path, "start_ns": time.monotonic_ns()}
        self.thread = threading.Thread(target=self.run, name="master-log-http", daemon=True)
        self.thread.start()

    def run(self):
        try:
            self.connection = http.client.HTTPConnection("127.0.0.1", self.case.port, timeout=15)
            self.connection.request("GET", self.path, headers={"Host": HOST, "Connection": "close"})
            response = self.connection.getresponse(); body = response.read(1048577)
            need(len(body) <= 1048576, "HTTP response exceeded finite bound")
            self.result.update(status=response.status, headers=response.getheaders(), body_hex=body.hex())
            (self.case.directory / (self.label + ".response.raw")).write_bytes(body)
        except BaseException as error:
            self.result["error"] = f"{type(error).__name__}: {error}"
        finally:
            self.result["end_ns"] = time.monotonic_ns()
            if self.connection is not None:
                self.connection.close()
            save(self.case.directory / (self.label + ".json"), self.result); self.done.set()

    def close(self):
        if self.connection is not None and self.connection.sock is not None:
            try:
                self.connection.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.thread.join(timeout=3)
        return [] if not self.thread.is_alive() else ["HTTP callback join timed out"]


class Case:
    def __init__(self, directory, cell):
        self.directory, self.cell = directory, cell
        self.mode, self.kind, self.action, self.nonblocking = cell
        self.short = self.sink = self.original_file = None
        self.process = self.master = self.old_worker = None; self.generations = []; self.requests = []; self.clis = []
        self.cli_started = {}; self.flags_baseline = None; self.events = []; self.http_count = 0; self.release_verdict_saved = False

    def prepare(self):
        self.directory.mkdir(exist_ok=False)
        self.short = tempfile.TemporaryDirectory(prefix="hp-master-log-")
        self.control_root = Path(self.short.name); self.address = self.control_root / "control.sock"
        self.pid_path = self.control_root / "master.pid"
        self.status_path = Path(str(self.pid_path) + ".status")
        # Assign before initialization so partial resource setup is still owned.
        self.sink = Sink.__new__(Sink)
        self.sink.__init__(self.directory, self.control_root, self.kind, self.mode == "inherited", self.nonblocking)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0)); self.port = listener.getsockname()[1]
        self.worker = self.directory / "worker.php"; self.worker.write_text(WORKER_PHP)
        (self.directory / "active-log.php").write_text(LOG_PHP); (self.directory / "health.bin").write_bytes(HEALTH)
        self.original = self.directory / "original-stdio.raw"; self.original_file = self.original.open("xb", buffering=0)
        self.healthy = self.directory / "healthy.log"
        self.config = self.directory / "config.yml"
        self.initial_log = self.sink.path if self.mode == "configured" else None
        self.config.write_text(self.yaml("v1", self.initial_log))
        save(self.directory / "cell-plan.json", {"cell": self.cell, "binary": str(BINARY), "master_cap": 1,
            "shutdown_timeout_ms": 200, "shutdown_bound_seconds": SHUTDOWN_BOUND, "reload_bound_seconds": 3,
            "log_payload_max_bytes": LOG_PAYLOAD_MAX, "normal_callbacks": 1, "backpressure_callbacks": 1,
            "max_HTTP_requests": 12, "automatic_retry": False, "control": str(self.address),
            "owned_status_paths": [str(self.status_path)], "scope": "Finite functional regression; no performance measurement"})

    def yaml(self, version, log_path):
        quote = lambda value: json.dumps(str(value))
        return (f"error-log: {quote(log_path)}\n" if log_path is not None else "") + f"""pid-file: {quote(self.pid_path)}
listen:
  host: 127.0.0.1
  port: {self.port}
num-threads: 1
php.runtime:
  threads: 1
  max-threads: 2
  startup-timeout: 2s
  shutdown-timeout: 200ms
  request-timeout: 20s
php.root: {quote(self.directory)}
php.log: stderr
php.ini:
  display_errors: "0"
  output_buffering: "0"
hosts:
  {HOST}:
    php.env:
      VERSION: {version}
    paths:
      /worker:
        php.worker:
          file: {quote(self.worker)}
          num: 1
      /:
        php_server: ON
"""

    def event(self, name, **value):
        self.events.append({"event": name, "monotonic_ns": time.monotonic_ns(), **value})
        save(self.directory / "timeline.json", self.events)

    def logs(self):
        self.sink.check()
        return self.sink.raw_path.read_bytes() if self.mode == "inherited" or self.initial_log is not None else self.original.read_bytes()

    def start(self):
        writer = self.sink.writer_fd if self.mode == "inherited" else self.original_file
        command = [str(BINARY), "run", "--config", str(self.config), "--address", "unix/" + str(self.address)]
        save(self.directory / "master-command.json", {"command": command, "controller_pid": os.getpid(),
            "retained_writer_before": self.sink.writer_info()})
        self.process = subprocess.Popen(command, stdout=writer, stderr=writer, start_new_session=True)
        value = process_info(self.process.pid); self.master = identity(value)
        need(value["ppid"] == os.getpid() and value["pgrp"] == value["session"] == value["pid"], "master ownership not established")
        self.event("master-start", master=self.master)
        pattern = rb"h2o\.master: generation ([0-9]+) pid ([0-9]+) ready\n"
        def ready():
            need(self.process.poll() is None, "master exited before readiness")
            return re.search(pattern, self.logs())
        match = until(ready, 3, "master ready diagnostic missing")
        self.old_worker = self.remember_worker(int(match.group(2)), int(match.group(1)))
        starting = f"h2o.master: starting generation {self.old_worker['generation']} pid {self.old_worker['pid']}\n".encode()
        published = f"h2o.master: generation {self.old_worker['generation']} pid {self.old_worker['pid']} ready\n".encode()
        raw = self.logs()
        need(raw.count(starting) == raw.count(published) == 1, "normal starting/ready diagnostic records changed")
        save(self.directory / "normal-master-startup.json", {"starting_hex": starting.hex(), "ready_hex": published.hex(),
             "starting_count": raw.count(starting), "ready_count": raw.count(published), "monotonic_ns": time.monotonic_ns()})
        if self.kind == "unix":
            need(self.sink.accepted == 1, "active Unix reader was not established")
            if self.mode == "configured":
                need(self.sink.peer["credentials"][0] == self.master["pid"], "accepted Unix peer is not the owned master")
        self.fetch("/health.bin", "initial-health", HEALTH)
        self.initial_worker = self.worker_response("v1", "initial-worker")
        need(self.initial_worker["pid"] == self.old_worker["pid"], "worker response PID differs from ready generation")
        self.normal_token = uuid.uuid4().hex
        self.fetch("/active-log.php?token=" + self.normal_token, "normal-log", ("log-ok:" + self.normal_token + "\n").encode())
        def normal_record():
            matches = []
            for line in self.logs().splitlines():
                try:
                    value = json.loads(line)
                except (ValueError, UnicodeError):
                    continue
                if isinstance(value, dict) and value.get("message") == "master-log:" + self.normal_token:
                    matches.append(value)
            if matches:
                need(len(matches) == 1, "duplicate normal diagnostic")
                row = matches[0]
                need(row.get("component") == "php.application" and row.get("level") == 2 and row.get("request_id", 0) > 0,
                     "normal diagnostic metadata changed")
                need(row.get("context") == {"probe": "master-log-test", "token": self.normal_token, "padding": ""},
                     "normal diagnostic context changed")
                return row
            return None
        save(self.directory / "normal-record.json", until(normal_record, 1, "normal diagnostic not delivered while draining"))
        owned_status = self.status_file()
        save(self.directory / "owned-status-file.json", owned_status)
        need(owned_status["exists"] and not owned_status["errors"] and
             stat.S_ISREG(owned_status["mode"]) and owned_status["uid"] == os.getuid() and
             owned_status["contents"] == f"{self.old_worker['generation']}:{self.old_worker['pid']}\n",
             "implicit owned pid-file.status identity/current generation not established")
        self.flags_baseline = self.flag_snapshot("normal-before-backpressure")
        self.prove_writer(self.old_worker)

    def remember_worker(self, pid, generation):
        row = process_info(pid)
        need(row.get("exists") and not row.get("errors") and row.get("ppid") == self.master["pid"] and
             row.get("pgrp") == row.get("session") == self.master["pid"] and row.get("exe") == str(BINARY),
             "generation identity not owned")
        result = next((item for item in self.generations if item["pid"] == pid), None)
        if result is not None:
            need(result["birth"] == row["birth"] and result["generation"] in (None, generation), "generation PID was reused")
            result["generation"] = generation
        else:
            result = identity(row); result["generation"] = generation
            self.generations.append(result)
        save(self.directory / "owned-generations.json", self.generations)
        return result

    def prove_writer(self, worker):
        master = process_info(self.master["pid"], self.master)
        child = process_info(worker["pid"], worker)
        first, second = master.get("fds", {}).get("2"), child.get("fds", {}).get("2")
        need(first and second and (first["device"], first["inode"]) == (second["device"], second["inode"]),
             "master/worker do not share the exact live log writer inode")
        need(stat.S_ISFIFO(first["mode"]) if self.kind == "fifo" else stat.S_ISSOCK(first["mode"]), "wrong live log writer type")
        need(bool(first["status_flags"] & os.O_NONBLOCK) == self.nonblocking and
             bool(second["status_flags"] & os.O_NONBLOCK) == self.nonblocking,
             "initial master/worker writer blocking mode changed")
        if self.mode == "inherited":
            retained = self.sink.writer_info()
            need((retained["device"], retained["inode"]) == (first["device"], first["inode"]), "retained OFD inode differs")
        save(self.directory / "live-writer-proof.json", {"master": master, "worker": child,
             "retained": self.sink.writer_info(), "reader": self.sink.state(),
             "inheritance": "Popen inherited writer or master-opened writer inherited through its fork/exec; no /proc FD reopen"})

    def fetch(self, path, label, expected=None):
        self.http_count += 1; need(self.http_count <= 12, "HTTP request budget exceeded")
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        value = {"path": path, "start_ns": time.monotonic_ns()}
        try:
            connection.request("GET", path, headers={"Host": HOST, "Connection": "close"})
            response = connection.getresponse(); body = response.read(1048577)
            value.update(status=response.status, headers=response.getheaders(), body_hex=body.hex())
            (self.directory / (label + ".response.raw")).write_bytes(body)
            need(len(body) <= 1048576 and response.status == 200, f"bad {label} response: {value}")
            if expected is not None:
                need(body == expected, f"wrong exact {label} response")
            return body
        finally:
            connection.close(); value["end_ns"] = time.monotonic_ns(); save(self.directory / (label + ".json"), value)

    def worker_response(self, version, label):
        value = json.loads(self.fetch("/worker", label))
        need(value.get("version") == version and re.fullmatch("[a-f0-9]{32}", value.get("boot", "")), "wrong worker state")
        return value

    def flag_snapshot(self, label, require_original_target=False):
        rows = {"master": process_info(self.master["pid"], self.master)}
        if self.old_worker is not None:
            rows["original_worker"] = process_info(self.old_worker["pid"], self.old_worker)
        value = {"monotonic_ns": time.monotonic_ns(), "retained": self.sink.writer_info(), "owned": rows}
        save(self.directory / (label + "-flags.json"), value)
        if self.flags_baseline is not None:
            if self.sink.initial_writer is not None:
                need(value["retained"] == self.sink.initial_writer, "retained shared OFD status flags changed")
            for role, row in rows.items():
                base = self.flags_baseline["owned"][role]
                if not row.get("exists") or not row.get("same_birth") or row.get("disappeared_during_read"):
                    continue
                need(not row["errors"], f"cannot inspect original {role} flags: {row['errors']}")
                for fd in ("1", "2"):
                    current, previous = row["fds"][fd], base["fds"][fd]
                    if require_original_target:
                        need((current["device"], current["inode"]) == (previous["device"], previous["inode"]),
                             f"{role} FD{fd} target changed before a successful reload")
                    if (current["device"], current["inode"]) == (previous["device"], previous["inode"]):
                        need(current["status_flags"] == previous["status_flags"], f"{role} FD{fd} shared status flags changed")
        return value

    def saturate(self):
        self.sink.pause(); self.burst_token = uuid.uuid4().hex
        self.http_count += 1; need(self.http_count <= 12, "HTTP request budget exceeded")
        self.burst = Request(self, "/active-log.php?burst=1&token=" + self.burst_token, "backpressure-request")
        self.requests.append(self.burst)
        entered_path = self.directory / ("log-" + self.burst_token + ".entered.json")
        end = time.monotonic() + 2; samples = []; proved_at = None
        while time.monotonic() < end:
            if entered_path.exists():
                entered = json.loads(entered_path.read_text())
                need(entered["pid"] == self.old_worker["pid"], "logging callback is not the owned generation")
                sample = process_info(entered["pid"], self.old_worker, entered["tid"])
                state = self.sink.state(); returned = (self.directory / ("log-" + self.burst_token + ".returned.json")).exists()
                row = {"owned": sample, "reader": state, "entered": entered, "returned_marker": returned}
                samples.append(row); save(self.directory / "backpressure-samples.json", samples)
                if self.nonblocking:
                    if self.burst.done.is_set():
                        if self.kind == "unix":
                            self.sink.nonblocking_socket_full()
                        need(self.sink.pending() > 0, "nonblocking full sink unexpectedly empty")
                        save(self.directory / "backpressure-proof.json", {"nonblocking_control": True, "sample": row,
                             "callback_outcome": self.burst.result, "reader": self.sink.state()})
                        break
                else:
                    task = sample.get("thread", {}); call = task.get("syscall", "").split()
                    # Exact syscall ABI is explicitly scoped by the test's platform gate.
                    blocked = (sample.get("exists") and sample.get("same_birth") and not sample["errors"] and
                        sample.get("state") != "Z" and len(call) >= 4 and call[0] == "1" and
                        int(call[1], 0) == 2 and int(call[3], 0) > 0 and
                        "Tgid:\t" + str(entered["pid"]) + "\n" in task.get("status", "") and
                        state["paused"] and state["want_pause"] and not returned and not self.burst.done.is_set())
                    if blocked:
                        fd = sample["fds"]["2"]
                        baseline = self.flags_baseline["owned"]["original_worker"]["fds"]["2"]
                        need((fd["device"], fd["inode"]) == (baseline["device"], baseline["inode"]), "blocked write moved to another sink")
                        need(entered["fd2_target"] == fd["target"], "callback's actual inherited log FD differs")
                        if self.kind == "fifo":
                            need(state["pending"] == state["capacity"], "FIFO stopped being full")
                        if proved_at is not None and time.monotonic() - proved_at >= .02:
                            save(self.directory / "backpressure-proof.json", {"nonblocking_control": False, "samples": samples[-2:],
                                 "first_proof_ns": int(proved_at * 1e9), "reader": state})
                            break
                        if proved_at is None:
                            proved_at = time.monotonic()
                    else:
                        proved_at = None
            self.sink.check(); time.sleep(.005)
        else:
            raise AssertionError("finite exact active-log backpressure proof not established")
        self.flag_snapshot("saturated-before-main-diagnostic", require_original_target=True)
        self.event("active-sink-full", reader=self.sink.state())

    def command(self, action, label, config=None):
        started = time.monotonic_ns()
        command = [str(BINARY), action, "--address", "unix/" + str(self.address)]
        if config is not None:
            command += ["--config", str(config)]
        stdout = (self.directory / (label + ".stdout")).open("xb", buffering=0)
        stderr = (self.directory / (label + ".stderr")).open("xb", buffering=0)
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, start_new_session=True)
        self.clis.append((process, stdout, stderr))
        self.cli_started[process.pid] = started
        save(self.directory / (label + "-command.json"), {"command": command, "pid": process.pid,
             "start_call_ns": started, "start_return_ns": time.monotonic_ns()})
        return process

    def wait_cli(self, process, label):
        deadline = self.cli_started[process.pid] + 3000000000
        code = process.wait(timeout=max(0, (deadline - time.monotonic_ns()) / 1e9))
        observed = time.monotonic_ns()
        save(self.directory / (label + "-completion.json"), {"returncode": code,
            "observed_ns": observed, "deadline_ns": deadline, "start_ns": self.cli_started[process.pid]})
        need(observed <= deadline, label + " exceeded the external 3 s command bound")
        return code, observed

    def refresh_children(self):
        if self.master is None or not same_live(self.master):
            return
        row = process_info(self.master["pid"], self.master)
        for pid in row.get("children", []):
            if any(item["pid"] == pid for item in self.generations):
                continue
            child = process_info(pid)
            if (child.get("exists") and not child.get("errors") and child.get("ppid") == self.master["pid"] and
                child.get("pgrp") == child.get("session") == self.master["pid"] and child.get("exe") == str(BINARY) and
                "-m\nworker\n" in child.get("cmdline", "")):
                remembered = identity(child); remembered["generation"] = None
                self.generations.append(remembered)
        save(self.directory / "owned-generations.json", self.generations)

    def status_file(self):
        value = {"path": str(self.status_path), "exists": False, "errors": [], "monotonic_ns": time.monotonic_ns()}
        try:
            with self.status_path.open("rb") as stream:
                info = os.fstat(stream.fileno()); raw = stream.read(65537)
            value.update(exists=True, device=info.st_dev, inode=info.st_ino, mode=info.st_mode,
                         uid=info.st_uid, size=info.st_size, contents=raw.decode("ascii"))
            need(len(raw) <= 65536, "status file exceeded finite raw capture bound")
        except FileNotFoundError:
            pass
        except (OSError, UnicodeError, AssertionError) as error:
            value["errors"].append(f"{type(error).__name__}: {error}")
        return value

    def snapshot(self):
        self.refresh_children()
        return {"monotonic_ns": time.monotonic_ns(), "reader": self.sink.state(),
            "master": process_info(self.master["pid"], self.master, self.master["pid"]),
            "generations": [process_info(row["pid"], row) for row in self.generations],
            "master_returncode": self.process.poll(), "retained_writer": self.sink.writer_info(),
            "implicit_owned_status_file": self.status_file()}

    def pre_release(self, label):
        value = self.snapshot(); save(self.directory / (label + "-BEFORE-reader-release.json"), value)
        need(value["reader"]["paused"] and value["reader"]["want_pause"], "reader released before strict verdict")
        self.release_verdict_saved = True
        return value

    def stop(self, by_signal=False, paused=True):
        start = time.monotonic(); self.event("shutdown-call", by_signal=by_signal)
        process = None
        if by_signal:
            need(same_live(self.master), "master identity changed before SIGTERM")
            self.process.send_signal(signal.SIGTERM)
        else:
            process = self.command("stop", "STOP")
        end = start + SHUTDOWN_BOUND; polls = []
        while time.monotonic() < end:
            row = {"monotonic_ns": time.monotonic_ns(), "master_returncode": self.process.poll(),
                "CLI_returncode": None if process is None else process.poll(),
                "same_birth_generations_present": [item["pid"] for item in self.generations if same_live(item)]}
            polls.append(row)
            if row["master_returncode"] is not None and not row["same_birth_generations_present"] and (process is None or row["CLI_returncode"] is not None):
                break
            time.sleep(.005)
        terminal = (self.process.poll() == 0 and not any(same_live(item) for item in self.generations) and
                    (process is None or process.poll() == 0))
        observed_done = time.monotonic_ns()
        elapsed = (observed_done - int(start * 1e9)) / 1e9
        save(self.directory / "shutdown-polls.json", polls)
        value = self.pre_release("shutdown-verdict") if paused else self.snapshot()
        save(self.directory / "shutdown-verdict.json", {"before_release": paused, "observation": value,
            "elapsed_seconds_at_observation": elapsed, "bound_seconds": SHUTDOWN_BOUND,
            "CLI_returncode": None if process is None else process.poll()})
        self.flag_snapshot("after-shutdown-main-diagnostic")
        need(elapsed <= SHUTDOWN_BOUND and terminal, "master/generation shutdown exceeded external bound before release")
        need(self.process.poll() == 0, "stop ACK did not produce normal master exit before release")
        need(process is None or process.poll() == 0, "stop command did not acknowledge status0")
        need(not any(same_live(row) for row in self.generations), "owned generation survived shutdown")
        need(not self.address.exists() and not self.pid_path.exists() and not self.status_path.exists(),
             "owned control/pid/status path survived shutdown")
        self.event("shutdown-complete", by_signal=by_signal)

    def reload(self, flags_control=False):
        if not flags_control:
            rejected_path = self.directory / "bad.yml"
            rejected_path.write_text(self.yaml("bad", self.directory / "missing-parent" / "error.log"))
            rejected = self.command("reload", "reject-log", rejected_path)
            try:
                code, _ = self.wait_cli(rejected, "reject-log")
            finally:
                save(self.directory / "reject-log-observation.json", self.snapshot())
            need(code != 0 and b"cannot prepare replacement log" in (self.directory / "reject-log.stderr").read_bytes(),
                 "bad replacement log did not fail explicitly")
            self.flag_snapshot("after-rejected-main-diagnostic", require_original_target=True)
            need(same_live(self.old_worker), "rejected log committed a different generation")
            children = process_info(self.master["pid"], self.master).get("children", [])
            need(children == [self.old_worker["pid"]], "rejected log changed current generation")
            self.fetch("/health.bin", "health-after-rejection", HEALTH)
            self.pre_release("rejected-log-verdict")
        new_path = self.directory / "v2.yml"; new_path.write_text(self.yaml("v2", self.healthy))
        changing = self.command("reload", "healthy-reload", new_path)
        try:
            code, committed_ns = self.wait_cli(changing, "healthy-reload")
        finally:
            save(self.directory / "healthy-reload-observation.json", self.snapshot())
        need(code == 0 and b"configuration loaded" in (self.directory / "healthy-reload.stdout").read_bytes(), "healthy replacement log did not commit")
        until(lambda: not same_live(self.old_worker), max(0, (committed_ns + int(SHUTDOWN_BOUND * 1e9) - time.monotonic_ns()) / 1e9),
              "retiring generation survived drain deadline")
        need(time.monotonic_ns() <= committed_ns + int(SHUTDOWN_BOUND * 1e9), "retiring exit not observed by commit-relative bound")
        state = self.worker_response("v2", "v2-worker")
        need(state["boot"] != self.initial_worker["boot"] and state["pid"] != self.old_worker["pid"], "worker was not replaced")
        current = self.remember_worker(state["pid"], self.old_worker["generation"] + 1)
        self.flag_snapshot("after-reload-main-diagnostics")
        self.pre_release("healthy-reload-verdict")
        def normal_new_records():
            raw = self.healthy.read_bytes() if self.healthy.exists() else b""
            ready = f"h2o.master: generation {current['generation']} pid {current['pid']} ready\n".encode()
            reap = f"h2o.master: retiring generation {self.old_worker['generation']} pid {self.old_worker['pid']} exited status ".encode()
            return ready in raw and re.search(re.escape(reap) + rb"[0-9]+\n", raw)
        until(normal_new_records, 1, "normal ready/reap records missing from draining replacement log")
        if flags_control:
            self.sink.resume(); until(lambda: self.sink.pending() == 0, 1, "old inherited sink did not drain")
            restored_path = self.directory / "v3.yml"; restored_path.write_text(self.yaml("v3", None))
            restored = self.command("reload", "restore-inherited", restored_path)
            restored_code, restored_ns = self.wait_cli(restored, "restore-inherited")
            need(restored_code == 0, "restoring inherited output failed")
            until(lambda: not same_live(current), max(0, (restored_ns + int(SHUTDOWN_BOUND * 1e9) - time.monotonic_ns()) / 1e9),
                  "second generation was not reaped")
            need(time.monotonic_ns() <= restored_ns + int(SHUTDOWN_BOUND * 1e9), "restored generation drain observation exceeded bound")
            state3 = self.worker_response("v3", "v3-worker")
            need(state3["boot"] != state["boot"] and state3["pid"] != state["pid"], "restored worker was not replaced")
            third = self.remember_worker(state3["pid"], current["generation"] + 1)
            expected = f"h2o.master: generation {third['generation']} pid {third['pid']} ready\n".encode()
            until(lambda: expected in self.sink.raw_path.read_bytes(), 1, "ordinary diagnostic did not resume on inherited sink")
            self.flag_snapshot("after-inherited-restore")
            self.stop(paused=False)
            until(lambda: b"h2o.master: draining worker generations\n" in self.sink.raw_path.read_bytes(), 1,
                  "normal drain diagnostic missing on restored draining sink")
        else:
            self.stop()
            until(lambda: b"h2o.master: draining worker generations\n" in self.healthy.read_bytes(), 1,
                  "normal drain diagnostic missing from healthy replacement log")

    def close(self):
        errors = []
        # Save the pre-release evidence first, even on an exception in setup.
        try:
            save(self.directory / "final-BEFORE-cleanup-release.json", self.snapshot() if self.master else
                 {"reader": None if self.sink is None else self.sink.state()})
        except BaseException as error:
            errors.append(f"pre-release snapshot: {type(error).__name__}: {error}")
        finally:
            # Release is independent of snapshot/recording success and always
            # precedes target waits and HTTP-thread joins.
            try:
                if self.sink is not None:
                    self.sink.resume()
            except BaseException as error:
                errors.append(f"cleanup reader release: {type(error).__name__}: {error}")
        if self.process is not None:
            if self.process.poll() is None or any(same_live(row) for row in self.generations):
                try:
                    if self.master and (same_live(self.master) or any(same_live(row) for row in self.generations)):
                        os.killpg(self.master["pid"], signal.SIGKILL)
                    elif self.master is None and self.process.poll() is None:
                        # Unreaped direct Popen child plus start_new_session owns
                        # this group even if its first /proc read failed.
                        os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                errors.append("owned master cleanup wait timed out")
            for row in self.generations:
                if same_live(row):
                    try:
                        os.kill(row["pid"], signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            try:
                until(lambda: not any(same_live(row) for row in self.generations), 3, "owned generation cleanup timed out")
            except BaseException as error:
                errors.append(str(error))
        for request in self.requests:
            errors.extend(request.close())
        for process, stdout, stderr in self.clis:
            if process.poll() is None:
                process.kill()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                errors.append("owned CLI cleanup wait timed out")
            stdout.close(); stderr.close()
        try:
            if self.sink is not None:
                errors.extend(self.sink.close())
        except BaseException as error:
            errors.append(f"sink cleanup: {type(error).__name__}: {error}")
        if self.original_file is not None:
            self.original_file.close()
        if self.short is not None:
            self.short.cleanup()
        save(self.directory / "cleanup.json", {"errors": errors, "monotonic_ns": time.monotonic_ns()})
        return errors


@unittest.skipUnless(sys.platform == "linux" and os.uname().machine == "x86_64", "exact /proc write syscall oracle requires Linux x86-64")
class MasterLogBackpressureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        persistent = os.environ.get("H2O_PHP_TEST_OUTPUT")
        cls.temporary = None
        if persistent:
            cls.base = Path(persistent).resolve() / ("master-log-backpressure-" + uuid.uuid4().hex)
            cls.base.mkdir(parents=True, exist_ok=False)
        else:
            cls.temporary = tempfile.TemporaryDirectory(prefix="h2o-php-master-log-")
            cls.base = Path(cls.temporary.name)
        save(cls.base / "matrix-plan.json", {"schema": "master-log-backpressure-regression-v1", "cells": CELLS,
            "fresh_master_cap": 10, "automatic_retry": False, "binary": str(BINARY),
            "binary_sha256": hashlib.sha256(BINARY.read_bytes()).hexdigest(), "controller_pid": os.getpid(),
            "kernel": os.uname().release, "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip()})

    @classmethod
    def tearDownClass(cls):
        if cls.temporary is not None:
            cls.temporary.cleanup()

    def run_cell(self, cell):
        label = "-".join(str(part).lower() for part in cell)
        case = None; error = None; cleanup = []
        directory = self.base / label
        try:
            case = Case(directory, cell); case.prepare(); case.start(); case.saturate()
            if case.action in ("stop", "signal"):
                case.stop(by_signal=case.action == "signal")
            else:
                case.reload(flags_control=case.action == "flags")
        except BaseException as caught:
            error = caught
        finally:
            if case is not None:
                # Failure is committed before cleanup is allowed to drain the log.
                try:
                    save(directory / "verdict-BEFORE-cleanup.json", {"passed": error is None,
                        "error": None if error is None else f"{type(error).__name__}: {error}",
                        "reader": None if case.sink is None else case.sink.state(), "strict_pre_release_verdict_saved": case.release_verdict_saved,
                        "HTTP_requests": case.http_count, "monotonic_ns": time.monotonic_ns()})
                except BaseException as record_error:
                    if error is None:
                        error = record_error
                finally:
                    cleanup = case.close()
            if directory.exists():
                save(directory / "result.json", {"passed": error is None and not cleanup,
                    "error": None if error is None else f"{type(error).__name__}: {error}", "cleanup_errors": cleanup,
                    "reader_release_never_used_to_change_deadline_verdict": True})
        if error is not None:
            raise error
        self.assertEqual(cleanup, [], f"owned cleanup failed; evidence: {directory}")

    def test_active_full_fifo_and_unix_logs_do_not_stall_stop_or_signal(self):
        for cell in CELLS[:6]:
            if cell[2] == "reload":
                continue
            with self.subTest(cell=cell):
                self.run_cell(cell)

    def test_active_full_logs_allow_rejected_and_healthy_replacement(self):
        for cell in CELLS[:6]:
            if cell[2] != "reload":
                continue
            with self.subTest(cell=cell):
                self.run_cell(cell)

    def test_shared_inherited_writer_flags_and_output_restoration(self):
        for cell in CELLS[6:]:
            with self.subTest(cell=cell):
                self.run_cell(cell)


if __name__ == "__main__":
    unittest.main()
