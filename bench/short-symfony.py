#!/usr/bin/env python3
"""Bounded five-mode comparison of the existing Symfony demo in production."""
import argparse
import csv
import datetime
import hashlib
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import statistics
import subprocess
import threading
import time

from run import sample

ROOT = Path(__file__).resolve().parent.parent
APP = Path('/home/m/symfony_demo')
MODES = ['h2o-classic', 'frankenphp-classic', 'h2o-worker', 'frankenphp-worker', 'php-fpm-classic']
WORKLOADS = {'homepage': '/en/', 'blog': '/en/blog/'}
WRK = ROOT / 'upstream/wrk/wrk'
BINARIES = {'h2o': ROOT / 'build/release/h2o-php', 'stock': ROOT / 'build/stock/h2o',
            'frankenphp': Path('/usr/bin/frankenphp'), 'fpm': Path('/usr/sbin/php-fpm')}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def request(port, path):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
    try:
        connection.request('GET', path, headers={'Host': 'benchmark.local'})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


class Server:
    def __init__(self, mode, out):
        self.mode = mode
        self.folder = out / mode
        self.folder.mkdir(exist_ok=True)
        self.processes = []
        self.logs = []
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            self.port = listener.getsockname()[1]
        self.env = dict(os.environ, APP_ENV='prod', APP_DEBUG='0', FRANKENPHP_LOOP_MAX='0',
                        DATABASE_URL=f'sqlite:///{APP}/var/data.db', GOMAXPROCS='24',
                        XDG_CONFIG_HOME=str(out / 'caddy-config'), XDG_DATA_HOME=str(out / 'caddy-data'))
        self.env.pop('FRANKENPHP_WORKER', None)
        self.env.pop('APP_RUNTIME_MODE', None)
        sessions = self.folder / 'sessions'
        sessions.mkdir(exist_ok=True)
        ini = {'opcache.enable': '1', 'opcache.enable_cli': '1', 'opcache.jit': 'off',
               'opcache.jit_buffer_size': '0', 'opcache.validate_timestamps': '0',
               'output_buffering': '4096', 'display_errors': '0', 'session.save_path': str(sessions)}
        if mode.startswith('h2o'):
            worker = mode.endswith('worker')
            self.config = self.folder / 'server.yml'
            self.config.write_text(f'''listen:
  host: 127.0.0.1
  port: {self.port}
num-threads: 4
php.runtime:
  threads: {1 if worker else 4}
  max-threads: {5 if worker else 4}
  max-requests: 0
  shutdown-timeout: 3s
php.ini:
''' + ''.join(f'  {key}: "{value}"\n' for key, value in ini.items()) + f'''php.root: {APP}/public
php.env:
  APP_ENV: prod
  APP_DEBUG: "0"
  FRANKENPHP_LOOP_MAX: "0"
hosts:
  benchmark.local:
    paths:
      /__benchinfo:
        php.root: {out}
        php.index: runtime-info.php
      /__metrics:
        php.metrics: ON
      /:
''' + (f'''        php.worker:
          file: {APP}/public/index.php
          num: 4
          max-requests: 0
''' if worker else '        file.dir: ' + str(APP / 'public') + '\n'))
            self.commands = [[str(BINARIES['h2o']), '-c', str(self.config)]]
        elif mode.startswith('frankenphp'):
            worker = mode.endswith('worker')
            self.config = self.folder / 'Caddyfile'
            self.config.write_text('''{
    admin off
    persist_config off
    auto_https off
    frankenphp {
''' + ('' if worker else '        num_threads 4\n') + f'        max_threads {5 if worker else 4}\n        max_requests 0\n' +
                ''.join(f'        php_ini {key} {value}\n' for key, value in ini.items()) + f'''    }}
}}
http://:{self.port} {{
    bind 127.0.0.1
    handle /__benchinfo {{
        root * {out}
        rewrite * /runtime-info.php
        php_server
    }}
    handle {{
        root * {APP}/public
        php_server {{
''' + (f'''            worker {{
                file {APP}/public/index.php
                num 4
            }}
''' if worker else '') + '''        }
    }
}
''')
            self.commands = [[str(BINARIES['frankenphp']), 'run', '--config', str(self.config), '--adapter', 'caddyfile']]
        else:
            sock = self.folder / 'fpm.sock'
            fpm_config = self.folder / 'php-fpm.conf'
            fpm_config.write_text(f'''[global]
daemonize = no
error_log = {self.folder}/fpm-error.log
pid = {self.folder}/fpm.pid
[benchmark]
listen = {sock}
pm = static
pm.max_children = 4
pm.max_requests = 0
catch_workers_output = yes
clear_env = no
chdir = {APP}
env[APP_ENV] = prod
env[APP_DEBUG] = 0
env[FRANKENPHP_LOOP_MAX] = 0
''' + ''.join(f'php_admin_value[{key}] = {value}\n' for key, value in ini.items()))
            self.config = self.folder / 'server.yml'
            def route(path, script):
                return f'''      {path}:
        setenv:
          SCRIPT_FILENAME: {script}
          SCRIPT_NAME: /index.php
          APP_ENV: prod
          APP_DEBUG: "0"
        fastcgi.document_root: {APP}/public
        fastcgi.timeout.keepalive: 0
        fastcgi.connect:
          port: {sock}
          type: unix
'''
            self.config.write_text(f'''listen:
  host: 127.0.0.1
  port: {self.port}
num-threads: 4
hosts:
  benchmark.local:
    paths:
''' + route('/__benchinfo', out / 'runtime-info.php') + route('/', APP / 'public/index.php'))
            self.commands = [[str(BINARIES['fpm']), '-F', '-y', str(fpm_config)],
                             [str(BINARIES['stock']), '-c', str(self.config)]]

    def start(self):
        for i, command in enumerate(self.commands):
            log = (self.folder / f'server-{i}.log').open('w')
            self.logs.append(log)
            self.processes.append(subprocess.Popen(command, cwd=APP, env=self.env, stdout=log,
                                                    stderr=subprocess.STDOUT, start_new_session=True))
            if self.mode == 'php-fpm-classic' and i == 0:
                end = time.monotonic() + 5
                while not (self.folder / 'fpm.sock').exists() and time.monotonic() < end:
                    time.sleep(.05)
        end = time.monotonic() + 15
        while time.monotonic() < end:
            if any(process.poll() is not None for process in self.processes):
                raise RuntimeError(f'{self.mode} exited; see {self.folder}')
            try:
                status, headers, body = request(self.port, '/__benchinfo')
                if status == 200:
                    return json.loads(body)
            except (OSError, ValueError, http.client.HTTPException):
                pass
            time.sleep(.05)
        raise RuntimeError(f'{self.mode} failed readiness; see {self.folder}')

    def stop(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
        for log in self.logs:
            log.close()


def measure(server, path, connections, duration, dest, deadline):
    if time.time() + duration + 12 >= deadline:
        raise TimeoutError('Stopping before the overall benchmark deadline')
    command = [str(WRK), '-t', str(min(connections, 4)), '-c', str(connections), '-d', f'{duration}s',
               '--timeout', '5s', '--latency', '-s', str(ROOT / 'bench/latency.lua'),
               '-H', 'Host: benchmark.local', f'http://127.0.0.1:{server.port}{path}']
    stop = threading.Event()
    monitor = threading.Thread(target=sample, args=([p.pid for p in server.processes], stop, dest.with_suffix('.csv')))
    monitor.start()
    begin = time.monotonic()
    try:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=min(duration + 8, deadline - time.time()))
    finally:
        elapsed = time.monotonic() - begin
        stop.set()
        monitor.join(timeout=2)
    dest.write_text('$ ' + ' '.join(command) + '\n' + result.stdout)
    if result.returncode:
        raise RuntimeError(f'wrk failed: {dest}')
    stats = next(json.loads(line.split(' ', 1)[1]) for line in result.stdout.splitlines()
                 if line.startswith('H2O_PHP_METRICS '))
    stats['wall_seconds'] = elapsed
    stats['timing_valid'] = abs(stats['duration_us'] / 1e6 - duration) < .5 and abs(elapsed - duration) < 1
    if not stats['requests'] or not stats['timing_valid']:
        raise RuntimeError(f'Invalid load-generator duration or no responses: {dest}')
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--deadline-utc', required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    deadline = datetime.datetime.fromisoformat(args.deadline_utc).timestamp()
    runtime_info = (ROOT / 'examples/bench/runtime-info.php').read_text().replace(
        "'php' => PHP_VERSION", "'app_env' => getenv('APP_ENV'), 'app_debug' => getenv('APP_DEBUG'), 'php' => PHP_VERSION")
    (out / 'runtime-info.php').write_text(runtime_info)
    metadata = {'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'app': str(APP),
                'app_env': 'prod', 'app_debug': '0', 'php_slots': 4, 'connections': [1, 8, 32, 128],
                'warmup_seconds': 2, 'repeat': 3, 'duration_seconds': 3, 'workloads': WORKLOADS,
                'php_fpm_frontend': 'stock H2O, Unix socket, FastCGI keepalive disabled',
                'server_environment': {'APP_ENV': 'prod', 'APP_DEBUG': '0', 'FRANKENPHP_LOOP_MAX': '0', 'GOMAXPROCS': '24'},
                'binaries': {name: {'path': str(path), 'sha256': sha(path)} for name, path in BINARIES.items()},
                'wrk_sha256': sha(WRK), 'source_sha256': {str(p.relative_to(ROOT)): sha(p) for p in (ROOT / 'src').rglob('*.c')},
                'application_files': {str(p.relative_to(APP)): sha(p) for p in [APP / 'composer.lock', APP / 'public/index.php', APP / 'src/Runtime/FrankenPhpRuntime.php', APP / 'src/Runtime/FrankenPhpWorkerRunner.php']}}
    for name, command in {'app_revision': ['git', '-C', str(APP), 'rev-parse', 'HEAD'],
                          'native_revision': ['git', 'rev-parse', 'HEAD'], 'machine': ['uname', '-a'],
                          'frankenphp_version': [str(BINARIES['frankenphp']), 'version'],
                          'php_fpm_version': [str(BINARIES['fpm']), '-v'],
                          'native_version': [str(BINARIES['h2o']), '--version']}.items():
        metadata[name] = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip()
    def save():
        (out / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    save()
    # Verify every configuration and real HTML response before timed load.
    metadata['preflight'] = {}
    for mode in MODES:
        server = Server(mode, out)
        try:
            info = server.start()
            assert info['app_env'] == 'prod' and info['app_debug'] == '0', info
            assert info['opcache_enabled'], info
            checks = {}
            for workload, path in WORKLOADS.items():
                status, headers, body = request(server.port, path)
                (server.folder / f'preflight-{workload}.html').write_bytes(body)
                assert status == 200 and b'<html' in body.lower() and len(body) > 1000, (mode, path, status, body[:300])
                assert 'x-debug-token' not in {key.lower() for key in headers}, headers
                checks[workload] = {'status': status, 'bytes': len(body), 'sha256': hashlib.sha256(body).hexdigest(), 'headers': headers}
            metadata['preflight'][mode] = {'runtime': info, 'responses': checks, 'commands': server.commands}
            print(json.dumps({'preflight': mode, 'php': info['php'], 'zts': info['zts'], 'responses': {k:v['bytes'] for k,v in checks.items()}}), flush=True)
            save()
        finally:
            server.stop()
    rows = []
    for mode in MODES:
        server = Server(mode, out)
        try:
            server.start()
            for workload, path in WORKLOADS.items():
                measure(server, path, 8, 2, server.folder / f'{workload}-warmup.txt', deadline)
                for connections in [1, 8, 32, 128]:
                    for repetition in range(1, 4):
                        raw = server.folder / f'{workload}-c{connections}-r{repetition}.txt'
                        row = measure(server, path, connections, 3, raw, deadline)
                        row.update(mode=mode, workload=workload, connections=connections, repetition=repetition, raw=str(raw.relative_to(ROOT)))
                        rows.append(row)
                        (out / 'runs.json').write_text(json.dumps(rows, indent=2) + '\n')
                    selected = rows[-3:]
                    print(json.dumps({'mode': mode, 'workload': workload, 'connections': connections,
                                      'median_rps': round(statistics.median(r['rps'] for r in selected)),
                                      'errors': sum(sum(r['errors_' + e] for e in ['connect','read','write','status','timeout']) for r in selected)}), flush=True)
        finally:
            server.stop()
    summary = []
    for mode in MODES:
        for workload in WORKLOADS:
            for connections in [1, 8, 32, 128]:
                selected = [r for r in rows if r['mode'] == mode and r['workload'] == workload and r['connections'] == connections]
                row = {'mode': mode, 'workload': workload, 'connections': connections,
                       **{'median_' + key: statistics.median(r[key] for r in selected) for key in ['rps', 'p50_ms', 'p95_ms', 'p99_ms']},
                       'errors': sum(sum(r['errors_' + e] for e in ['connect','read','write','status']) for r in selected),
                       'timeouts': sum(r['errors_timeout'] for r in selected)}
                summary.append(row)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    with (out / 'summary.csv').open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    metadata['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    metadata['measured_runs'] = len(rows)
    metadata['notes'] = ['Same-host HTTP/1.1 keepalive; no TLS or compression; no wrk coordinated-omission correction.',
                         'Four active PHP slots; worker configurations additionally have one idle classic diagnostic slot.',
                         'PHP-FPM uses its normal NTS build; embedded servers use ZTS, all PHP 8.5.12.',
                         'Existing app production cache warmed by HTTP, no cache clearing or source edits.',
                         'Summed process RSS counts shared pages more than once. Short runs are a local comparison.']
    save()
    print(json.dumps({'complete': str(out), 'runs': len(rows)}), flush=True)


if __name__ == '__main__':
    main()
