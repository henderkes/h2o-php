#!/usr/bin/env python3
"""Direct real-HTTP benchmark runner; no synthetic performance results.

Server is started separately. Each workload records warmup, three measured runs at
1/8/32/128 connections, raw wrk output, and Linux process CPU/RSS samples.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parent.parent

def sample(pids, stop, dest):
    hz = os.sysconf('SC_CLK_TCK')
    previous_cpu = {}
    accumulated_cpu = 0
    with dest.open('w') as out:
        out.write('unix_seconds,cpu_seconds,rss_kib,threads\n')
        while not stop.is_set():
            try:
                pending = list(pids)
                visited = set()
                rss = threads = 0
                while pending:
                    pid = pending.pop()
                    if pid in visited:
                        continue
                    visited.add(pid)
                    try:
                        pending.extend(map(int, Path(f'/proc/{pid}/task/{pid}/children').read_text().split()))
                        stat = Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1].split()
                        status = Path(f'/proc/{pid}/status').read_text().splitlines()
                        values = dict(line.split(':', 1) for line in status if ':' in line)
                        cpu = (int(stat[11])+int(stat[12]))/hz
                        accumulated_cpu += max(0, cpu - previous_cpu.get(pid, 0))
                        previous_cpu[pid] = cpu
                        rss += int(values.get('VmRSS', '0 kB').split()[0])
                        threads += int(values.get('Threads', '0'))
                    except (FileNotFoundError, ProcessLookupError):
                        if pid in pids:
                            raise
                out.write(f'{time.time():.6f},{accumulated_cpu:.6f},{rss},{threads}\n')
                out.flush()
            except (FileNotFoundError, ProcessLookupError):
                out.write(f'{time.time():.6f},PROCESS_EXITED,,\n')
                return
            stop.wait(1)

def invoke(args, target, connections, duration, dest, post_size=0):
    script = 'post.lua' if post_size else ('mixed.lua' if target == '/__mixed__' else 'latency.lua')
    cmd = [args.wrk, '-t', str(min(connections, args.threads)), '-c', str(connections),
           '-d', f'{duration}s', '--timeout', '10s', '--latency', '-s',
           str(ROOT / 'bench' / script)]
    for header in args.header:
        cmd.extend(['-H', header])
    cmd.append(args.base_url.rstrip('/') + target)
    env = dict(os.environ, BENCH_LATENCY_LUA=str(ROOT / 'bench/latency.lua'), BENCH_BODY_SIZE=str(post_size))
    def metrics():
        if not args.metrics_path:
            return None
        try:
            with urllib.request.urlopen(args.base_url.rstrip('/') + args.metrics_path, timeout=3) as response:
                return json.loads(response.read())
        except Exception as error:
            return {'collection_error': str(error)}
    before_metrics = metrics()
    stop = threading.Event()
    monitor = None
    if args.pid:
        monitor = threading.Thread(target=sample, args=([args.pid] + args.extra_pid, stop, dest.with_suffix('.csv')), daemon=True)
        monitor.start()
    begin = time.time()
    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    stop.set()
    if monitor:
        monitor.join()
    dest.write_text('$ ' + ' '.join(cmd) + '\n' + result.stdout)
    row = {'target': target, 'connections': connections, 'duration_seconds': time.time()-begin,
           'exit_code': result.returncode, 'body_size': post_size, 'raw': str(dest.relative_to(ROOT))}
    if args.metrics_path:
        row['metrics_before'] = before_metrics
        row['metrics_after'] = metrics()
    for line in result.stdout.splitlines():
        if line.startswith('H2O_PHP_METRICS '):
            row.update(json.loads(line.split(' ', 1)[1]))
    return row

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-url', default='http://127.0.0.1:8080')
    p.add_argument('--wrk', default='wrk', help='Load-generator executable (default: supplied wrk)')
    p.add_argument('--pid', type=int, help='Actual server worker process PID, used for CPU/RSS samples')
    p.add_argument('--extra-pid', type=int, action='append', default=[], help='Additional server process tree, e.g. PHP-FPM master')
    p.add_argument('--binary', help='Measured server executable; record SHA256 and dynamic dependencies')
    p.add_argument('--config', help='Measured server configuration; retain exact contents and SHA256')
    p.add_argument('--metrics-path', help='Optional local native runtime metrics endpoint')
    p.add_argument('--runtime-info-path', help='Optional PHP runtime-info fixture, retained with measurements')
    p.add_argument('--header', action='append', default=[], help='Additional HTTP header, e.g. Host: laravel.local')
    p.add_argument('--label', required=True)
    p.add_argument('--duration', type=int, default=10)
    p.add_argument('--warmup', type=int, default=3)
    p.add_argument('--repeat', type=int, default=3)
    p.add_argument('--threads', type=int, default=4)
    p.add_argument('--connections', default='1,8,32,128')
    p.add_argument('--workloads', default='minimal,json,worker,post-small,post-large,stream,static,mixed')
    p.add_argument('--stability', type=int, default=0, help='Sustained worker duration in seconds; final gate requires >=600')
    args = p.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    out = ROOT / 'bench/raw' / f'{stamp}-{args.label}'
    out.mkdir(parents=True)
    metadata = vars(args) | {'utc': stamp}
    generator = Path(shutil.which(args.wrk) or args.wrk).resolve()
    metadata['load_generator'] = {'path': str(generator), 'sha256': hashlib.sha256(generator.read_bytes()).hexdigest(),
                                'version': subprocess.run([str(generator),'--version'],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout}
    for name, cmd in {'git_commit': ['git','rev-parse','HEAD'], 'git_status': ['git','status','--short'], 'machine': ['uname','-a'],
                      'compiler': ['/usr/bin/gcc','--version'], 'php': ['php-config-zts','--version'],
                      'h2o': ['git','-C','upstream/h2o','rev-parse','HEAD']}.items():
        metadata[name] = subprocess.run(cmd,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout.strip()
    for name, value in [('binary', args.binary), ('config', args.config)]:
        if value:
            path = Path(value).resolve()
            metadata[name + '_path'] = str(path)
            metadata[name + '_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
            if name == 'config':
                (out/'server-config.txt').write_bytes(path.read_bytes())
            else:
                metadata['ldd'] = subprocess.run(['ldd', str(path)],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout
                version_argument = 'version' if path.name == 'frankenphp' else '--version'
                metadata['binary_version'] = subprocess.run([str(path), version_argument],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout
                version_match = re.search(r'h2o-php runtime revision: ([0-9a-f]+)', metadata['binary_version'])
                if version_match:
                    metadata['tested_revision'] = version_match.group(1)
    if args.binary:
        binary_folder = Path(args.binary).resolve().parent
        for flags in [binary_folder/'CMakeFiles/h2o.dir/flags.make', binary_folder/'h2o/CMakeFiles/h2o.dir/flags.make']:
            if flags.exists():
                metadata['measured_build_flags'] = flags.read_text()
                break
    metadata['source_sha256'] = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted((ROOT/'src').rglob('*')) if path.is_file()}
    metadata['fixture_sha256'] = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted((ROOT/'examples/bench').rglob('*')) if path.is_file()}
    if args.runtime_info_path:
        try:
            with urllib.request.urlopen(args.base_url.rstrip('/') + args.runtime_info_path, timeout=5) as response:
                metadata['http_runtime_info'] = json.loads(response.read())
        except Exception as error:
            metadata['http_runtime_info_error'] = str(error)
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
    workloads = {'minimal':('/minimal.php',0), 'json':('/json.php',0), 'worker':('/worker',0),
                 'post-small':('/body.php',1024), 'post-large':('/body.php',1024*1024),
                 'stream':('/stream.php',0), 'static':('/static.txt',0), 'mixed':('/__mixed__',0),
                 'symfony':('/?name=benchmark',0), 'laravel':('/hello/native',0)}
    rows = []
    for name in args.workloads.split(','):
        path, size = workloads[name]
        invoke(args,path,8,args.warmup,out/f'{name}-warmup.txt',size)
        for con in map(int,args.connections.split(',')):
            for run in range(1,args.repeat+1):
                row = invoke(args,path,con,args.duration,out/f'{name}-c{con}-r{run}.txt',size)
                row.update(workload=name, run=run)
                rows.append(row)
                (out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
                print(json.dumps(row),flush=True)
    if args.stability:
        row = invoke(args,'/worker',32,args.stability,out/'stability.txt')
        row['workload'] = 'stability'
        rows.append(row)
        (out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
        print(json.dumps(row),flush=True)

if __name__ == '__main__':
    main()
