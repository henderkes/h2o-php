#!/usr/bin/env python3
"""Compare the example Symfony worker configs using real, matching HTTP pages.

Starts only one benchmark server at a time on a temporary IPv4 loopback port.
Keeps PHP tuning unless --match-production or --tracing-jit is supplied. Changes
listen addresses, paths, and Caddy's administration/storage settings. Does not stop
an existing server.
"""
import argparse
import csv
import datetime
import hashlib
import http.client
import json
import os
from pathlib import Path
import random
import re
import shutil
import signal
import socket
import statistics
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT.parent / 'symfony-demo'
ROUTES = {
    'homepage': '/en/',
    'blog': '/en/blog/',
    'post': '/en/blog/posts/lorem-ipsum-dolor-sit-amet-consectetur-adipiscing-elit',
}
BINARIES = {'h2o': ROOT / 'build/release/h2o-php',
            'frankenphp': Path(shutil.which('frankenphp') or '/usr/bin/frankenphp')}
CONFIGS = {'h2o': ROOT / 'examples/symfony.yml',
           'frankenphp': ROOT / 'examples/symfony.Caddyfile'}
PRODUCTION_INI = {'error_reporting': '6143', 'log_errors': '1', 'display_errors': '0',
                  'opcache.enable': '1', 'opcache.validate_timestamps': '0'}
PRODUCTION_ENV = {'APP_ENV': 'prod', 'APP_DEBUG': '0',
                  'APP_RUNTIME_OPTIONS': '{"error_handler":false}'}
JIT_INI = {'opcache.jit': 'tracing', 'opcache.jit_buffer_size': '64M'}
RUNTIME_PATH = '/__bench_runtime'
RUNTIME_PROBE = '''<?php
$status = opcache_get_status(false);
header('Content-Type: application/json');
echo json_encode([
    'php' => PHP_VERSION, 'sapi' => PHP_SAPI, 'pid' => getmypid(),
    'jit_ini' => ini_get('opcache.jit'),
    'jit_buffer_size_ini' => ini_get('opcache.jit_buffer_size'),
    'opcache_enabled' => $status['opcache_enabled'] ?? false,
    'jit' => $status['jit'] ?? null,
    'opcache_statistics' => $status['opcache_statistics'] ?? null,
], JSON_THROW_ON_ERROR);
'''


def capture(command):
    return subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=30).stdout.strip()


def request(port, path):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
    try:
        connection.request('GET', path, headers={'Host': 'localhost'})
        response = connection.getresponse()
        body = response.read()
        title = re.search(rb'<title>(.*?)</title>', body, re.S)
        headings = re.findall(rb'<h1\b[^>]*>(.*?)</h1>', body, re.S)
        return {'status': response.status, 'bytes': len(body),
                'headers': dict(response.getheaders()),
                'title': title.group(1).decode() if title else None,
                'h1': [re.sub(r'<[^>]+>', '', h.decode()).strip() for h in headings],
                'sha256': hashlib.sha256(body).hexdigest()}, body
    finally:
        connection.close()


def process_sample(pid):
    pending, visited = [pid], set()
    result = dict(cpu_seconds=0, rss_kib=0, swap_kib=0, threads=0)
    while pending:
        current = pending.pop()
        if current in visited:
            continue
        visited.add(current)
        try:
            folder = Path(f'/proc/{current}')
            pending.extend(map(int, (folder / f'task/{current}/children').read_text().split()))
            stat = (folder / 'stat').read_text().split(') ', 1)[1].split()
            fields = dict(line.split(':', 1) for line in (folder / 'status').read_text().splitlines() if ':' in line)
            result['cpu_seconds'] += (int(stat[11]) + int(stat[12])) / os.sysconf('SC_CLK_TCK')
            result['rss_kib'] += int(fields.get('VmRSS', '0 kB').split()[0])
            result['swap_kib'] += int(fields.get('VmSwap', '0 kB').split()[0])
            result['threads'] += int(fields.get('Threads', '0'))
        except (FileNotFoundError, ProcessLookupError):
            pass
    return result


def monitor(pid, stop, path):
    with path.open('w') as output:
        fields = ['unix_seconds', 'cpu_seconds', 'rss_kib', 'swap_kib', 'threads']
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        while not stop.is_set():
            writer.writerow(dict(unix_seconds=time.time(), **process_sample(pid)))
            output.flush()
            stop.wait(1)


class Server:
    def __init__(self, engine, folder, source, match_production=False, tracing_jit=False):
        self.engine, self.folder = engine, folder
        folder.mkdir(parents=True)
        self.process = None
        self.log = None
        self.control = None
        if tracing_jit:
            (folder / 'runtime-info.php').write_text(RUNTIME_PROBE)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            self.port = listener.getsockname()[1]
        if engine == 'h2o':
            if match_production:
                for key, value in PRODUCTION_INI.items():
                    if not re.search(r'^  ' + re.escape(key) + r": '" + value + "'", source, re.M):
                        raise ValueError(f'H2O production INI changed: {key}; review matching overrides')
                for key, value in PRODUCTION_ENV.items():
                    if value not in source:
                        raise ValueError(f'H2O production environment changed: {key}')
            if tracing_jit:
                if source.count('php.ini:\n') != 1 or re.search(r'^\s+opcache\.jit(?:_buffer_size)?:', source, re.M):
                    raise ValueError('Review existing H2O JIT settings before overriding')
                source = source.replace('php.ini:\n', 'php.ini:\n' + ''.join(
                    f"  {key}: '{value}'\n" for key, value in JIT_INI.items()), 1)
                source = source.replace('    paths:\n', f'''    paths:
      {RUNTIME_PATH}:
        php.root: {folder}
        php.index: runtime-info.php
''', 1)
            config, count = re.subn(r'(?m)^  host: localhost$', '  host: 127.0.0.1', source, count=1)
            config, ports = re.subn(r'(?m)^  port: 80$', f'  port: {self.port}', config, count=1)
            if count != 1 or ports != 1:
                raise ValueError('Unexpected H2O listen configuration')
            config = config.replace('../symfony-demo/', str(APP) + '/')
            self.config = folder / 'server.yml'
            self.control = tempfile.TemporaryDirectory(prefix='hp-bench-')
            self.command = [str(BINARIES[engine]), 'run', '-c', str(self.config),
                            '--address', self.control.name + '/control.sock']
        else:
            config, count = re.subn(r'(?m)^localhost:80\s*\{',
                                    f'http://localhost:{self.port} {{\n    bind 127.0.0.1', source, count=1)
            if count != 1:
                raise ValueError('Unexpected Caddy listen configuration')
            if tracing_jit:
                config, count = re.subn(r'(?m)^    route \{', f'''    route {RUNTIME_PATH} {{
        root * {folder}
        rewrite * /runtime-info.php
        php {{
            root {folder}
        }}
    }}

    route {{''', config, count=1)
                if count != 1:
                    raise ValueError('Unexpected Caddy routes for runtime probe')
            ini = (PRODUCTION_INI if match_production else {}) | (JIT_INI if tracing_jit else {})
            php = ('    frankenphp {\n' + ''.join(f'        php_ini {key} "{value}"\n'
                   for key, value in ini.items()) + '    }\n') if ini else ''
            config = '{\n    admin off\n    persist_config off\n    auto_https off\n' + php + '}\n' + config + '\n'
            self.config = folder / 'Caddyfile'
            self.command = [str(BINARIES[engine]), 'run', '--config', str(self.config), '--adapter', 'caddyfile']
        self.config.write_text(config)
        self.env = dict(os.environ, XDG_CONFIG_HOME=str(folder / 'xdg-config'),
                        XDG_DATA_HOME=str(folder / 'xdg-data'))
        if match_production:
            self.env.update(PRODUCTION_ENV)
        for key in ('FRANKENPHP_WORKER', 'APP_RUNTIME_MODE'):
            self.env.pop(key, None)

    def start(self):
        self.log = (self.folder / 'server.log').open('w')
        self.process = subprocess.Popen(self.command, cwd=ROOT, env=self.env, stdout=self.log,
                                        stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f'{self.engine} exited; see {self.folder}/server.log')
            try:
                info, _ = request(self.port, ROUTES['homepage'])
                if info['status'] == 200 and info['bytes'] > 1000:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(.1)
        raise RuntimeError(f'{self.engine} not ready; see {self.folder}/server.log')

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait()
        if self.log:
            self.log.close()
        if self.control:
            self.control.cleanup()

    def verify_jit(self, phase):
        info, body = request(self.port, RUNTIME_PATH)
        if info['status'] != 200:
            raise RuntimeError(f'Runtime probe failed: {self.engine} {info}')
        runtime = json.loads(body)
        (self.folder / f'runtime-{phase}.json').write_text(json.dumps(runtime, indent=2) + '\n')
        jit = runtime.get('jit') or {}
        if not (runtime.get('jit_ini') == 'tracing' and jit.get('enabled') and jit.get('on') and jit.get('kind') == 5):
            raise RuntimeError(f'Tracing JIT is not active: {self.engine} {runtime}')
        used = jit['buffer_size'] - jit['buffer_free']
        print(f'JIT {self.engine} phase={phase} enabled={jit["enabled"]} on={jit["on"]} used={used} bytes', flush=True)


def measure(server, workload, connections, duration, threads, wrk, label):
    dest = server.folder / f'{label}-{workload}-c{connections}.txt'
    command = [wrk, '-t', str(min(threads, connections)), '-c', str(connections), '-d', f'{duration}s',
               '--timeout', '10s', '--latency', '-s', str(ROOT / 'bench/latency.lua'),
               '-H', 'Host: localhost', f'http://127.0.0.1:{server.port}{ROUTES[workload]}']
    stop = threading.Event()
    watcher = threading.Thread(target=monitor, args=(server.process.pid, stop, dest.with_suffix('.csv')))
    before = process_sample(server.process.pid)
    begin = time.monotonic()
    watcher.start()
    try:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=duration + 20)
    finally:
        elapsed = time.monotonic() - begin
        stop.set()
        watcher.join(timeout=3)
    after = process_sample(server.process.pid)
    dest.write_text('$ ' + ' '.join(command) + '\n' + result.stdout)
    if result.returncode or server.process.poll() is not None:
        raise RuntimeError(f'Benchmark failed: {dest}')
    metrics = next((json.loads(line.split(' ', 1)[1]) for line in result.stdout.splitlines()
                    if line.startswith('H2O_PHP_METRICS ')), None)
    if not metrics or any(metrics[key] for key in metrics if key.startswith('errors_')):
        raise RuntimeError(f'Missing metrics or HTTP/socket errors: {dest}')
    return dict(engine=server.engine, workload=workload, path=ROUTES[workload], connections=connections,
                elapsed_seconds=elapsed, server_cpu_seconds=after['cpu_seconds'] - before['cpu_seconds'],
                server_rss_kib=after['rss_kib'], server_swap_kib=after['swap_kib'],
                raw=str(dest.relative_to(ROOT)), **metrics)


def aggregate(rows, out):
    summary = []
    for workload in ROUTES:
        for connections in sorted({row['connections'] for row in rows}):
            item = dict(workload=workload, connections=connections)
            for engine in BINARIES:
                group = [r for r in rows if r['workload'] == workload and r['connections'] == connections and r['engine'] == engine]
                if not group:
                    break
                for field in ('rps', 'p50_ms', 'p95_ms', 'p99_ms', 'server_rss_kib', 'server_swap_kib'):
                    item[f'{engine}_{field}'] = statistics.median(r[field] for r in group)
                item[f'{engine}_rps_min'] = min(r['rps'] for r in group)
                item[f'{engine}_rps_max'] = max(r['rps'] for r in group)
                item[f'{engine}_samples'] = len(group)
            else:
                item['h2o_rps_ratio'] = item['h2o_rps'] / item['frankenphp_rps']
                item['h2o_rps_change_pct'] = (item['h2o_rps_ratio'] - 1) * 100
                summary.append(item)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    if summary:
        with (out / 'summary.csv').open('w') as output:
            writer = csv.DictWriter(output, fieldnames=list(summary[0]))
            writer.writeheader()
            writer.writerows(summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--connections', default='1,8,32,100,256')
    parser.add_argument('--duration', type=int, default=10)
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--warmup', type=int, default=3)
    parser.add_argument('--threads', type=int, default=8)
    parser.add_argument('--wrk', default=shutil.which('wrk') or 'wrk')
    parser.add_argument('--match-production', action='store_true',
                        help='Match Caddy PHP INI/environment to the H2O example without changing either source file')
    parser.add_argument('--tracing-jit', action='store_true',
                        help='Enable tracing JIT with a 64 MiB buffer on both servers and verify it over HTTP')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    connections = list(map(int, args.connections.split(',')))
    if min(connections + [args.duration, args.repeat, args.warmup, args.threads]) < 1:
        parser.error('Benchmark sizes must be positive')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    suffix = '-tracing-jit' if args.tracing_jit else ''
    out = (args.output or ROOT / 'bench/raw' / f'{stamp}-symfony-comparison{suffix}').resolve()
    out.mkdir(parents=True)
    sources = {engine: path.read_text() for engine, path in CONFIGS.items()}
    metadata = dict(utc=stamp, arguments={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                    routes=ROUTES, host_header='localhost', address='127.0.0.1',
                    notes=[('Production PHP settings matched to H2O on temporary Caddy config.' if args.match_production else
                            'PHP settings kept as configured; not an isolated engine-only comparison.'),
                           'One measured server at a time; client and server share this machine.',
                           'RSS sums process RSS, which can include shared pages more than once.'],
                    cpu=capture(['lscpu']), memory_start=Path('/proc/meminfo').read_text(),
                    processes_start=capture(['ps', '-eo', 'pid,ppid,comm,pcpu,rss', '--sort=-pcpu']),
                    vmstat_start=Path('/proc/vmstat').read_text(),
                    git_commit=capture(['git', 'rev-parse', 'HEAD']), git_status=capture(['git', 'status', '--short']),
                    app_commit=capture(['git', '-C', str(APP), 'rev-parse', 'HEAD']),
                    wrk_version=capture([args.wrk, '--version']),
                    configs={engine: dict(path=str(CONFIGS[engine]), contents=source,
                             sha256=hashlib.sha256(source.encode()).hexdigest()) for engine, source in sources.items()},
                    binaries={engine: dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                              version=capture([str(path), 'version' if engine == 'frankenphp' else '--version']))
                              for engine, path in BINARIES.items()},
                    ini_files={str(p): p.read_text() for p in [Path('/etc/php-zts/php.ini'),
                               *Path('/etc/php-zts/conf.d').glob('*.ini')] if p.is_file()},
                    inherited_environment={k: os.environ[k] for k in ('GOMAXPROCS', 'APP_ENV', 'APP_DEBUG',
                                           'APP_RUNTIME_OPTIONS', 'PHP_INI_SCAN_DIR') if k in os.environ})
    if args.match_production:
        metadata['production_ini'] = PRODUCTION_INI
        metadata['production_env'] = PRODUCTION_ENV
    if args.tracing_jit:
        metadata['jit_ini'] = JIT_INI
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    rows, preflight, reference = [], [], {}
    print(f'OUTPUT {out}', flush=True)
    for repeat in range(1, args.repeat + 1):
        engines = list(BINARIES) if repeat % 2 else list(reversed(BINARIES))
        cases = [(name, c) for name in ROUTES for c in connections]
        random.Random(7300 + repeat).shuffle(cases)
        for engine in engines:
            print(f'START round={repeat} engine={engine}', flush=True)
            server = Server(engine, out / f'r{repeat}-{engine}', sources[engine], args.match_production, args.tracing_jit)
            try:
                server.start()
                if args.tracing_jit:
                    server.verify_jit('before')
                for name, path in ROUTES.items():
                    info, body = request(server.port, path)
                    if info['status'] != 200 or info['bytes'] < 1000 or not info['title']:
                        raise RuntimeError(f'Invalid application response: {engine} {name} {info}')
                    if name == 'homepage' and not re.search(rb'Worker Mode:\s*1\s*<br', body):
                        raise RuntimeError(f'{engine} homepage does not report worker mode')
                    signature = (info['title'], info['h1'])
                    if name in reference and signature != reference[name]:
                        raise RuntimeError(f'Application content mismatch: {engine} {name} {signature} != {reference[name]}')
                    reference[name] = signature
                    preflight.append(dict(engine=engine, repeat=repeat, workload=name, path=path, **info))
                    (server.folder / f'{name}.html').write_bytes(body)
                    measure(server, name, 100, args.warmup, args.threads, args.wrk, 'warmup')
                if args.tracing_jit:
                    server.verify_jit('warmed')
                (out / 'preflight.json').write_text(json.dumps(preflight, indent=2) + '\n')
                for name, c in cases:
                    row = measure(server, name, c, args.duration, args.threads, args.wrk, 'measured')
                    row['repeat'] = repeat
                    rows.append(row)
                    (out / 'runs.json').write_text(json.dumps(rows, indent=2) + '\n')
                    aggregate(rows, out)
                    print(f"RESULT round={repeat} {engine:10} {name:8} c={c:3} rps={row['rps']:.1f} p95={row['p95_ms']:.3f}ms swap={row['server_swap_kib']}KiB", flush=True)
                if args.tracing_jit:
                    server.verify_jit('after')
            finally:
                server.stop()
    metadata['memory_end'] = Path('/proc/meminfo').read_text()
    metadata['vmstat_end'] = Path('/proc/vmstat').read_text()
    metadata['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'COMPLETE {out}/summary.csv', flush=True)


if __name__ == '__main__':
    main()
