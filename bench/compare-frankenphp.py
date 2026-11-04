#!/usr/bin/env python3
"""Tune Caddy Go scheduling, then measure the installed FrankenPHP sequentially."""
import json
import math
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
records = []
def launch(procs, label, measured):
    env = dict(os.environ, BENCH_ROOT=str(ROOT/'examples/bench'), GOMAXPROCS=str(procs),
               XDG_CONFIG_HOME=str(ROOT/'build/caddy-config'), XDG_DATA_HOME=str(ROOT/'build/caddy-data'))
    log_path = ROOT/f'bench/raw/{label}-server.log'
    with log_path.open('w') as log:
        server = subprocess.Popen(['frankenphp','run','--config','bench/FrankenPHP.Caddyfile','--adapter','caddyfile'],
                                  cwd=ROOT,env=env,stdout=log,stderr=log)
        try:
            for attempt in range(100):
                try:
                    with urllib.request.urlopen('http://127.0.0.1:18081/runtime-info.php',timeout=.3) as r:
                        info = json.load(r)
                    break
                except Exception:
                    if server.poll() is not None:
                        raise RuntimeError(f'FrankenPHP exited; see {log_path}')
                time.sleep(.1)
            else:
                raise RuntimeError('FrankenPHP baseline failed readiness')
            print(json.dumps({'label':label,'pid':server.pid,'GOMAXPROCS':procs,'runtime_info':info}),flush=True)
            before = set((ROOT/'bench/raw').glob(f'*-{label}'))
            cmd = [sys.executable,'bench/run.py','--wrk','upstream/wrk/wrk','--base-url','http://127.0.0.1:18081',
                   '--pid',str(server.pid),'--binary','/usr/bin/frankenphp','--config','bench/FrankenPHP.Caddyfile',
                   '--runtime-info-path','/runtime-info.php','--label',label,'--duration','5' if measured else '3',
                   '--warmup','3' if measured else '1','--repeat','3' if measured else '1',
                   '--connections','1,8,32,128' if measured else '128',
                   '--workloads','minimal,json,worker' if measured else 'minimal,worker']
            subprocess.run(cmd,cwd=ROOT,env=env,check=True)
            folders = set((ROOT/'bench/raw').glob(f'*-{label}'))-before
            folder = next(iter(folders))
            metadata_path = folder/'metadata.json'
            metadata = json.loads(metadata_path.read_text())
            metadata['server_environment'] = {k:env[k] for k in ['GOMAXPROCS','BENCH_ROOT','XDG_CONFIG_HOME','XDG_DATA_HOME']}
            metadata['frankenphp_version'] = subprocess.run(['frankenphp','version'],text=True,stdout=subprocess.PIPE).stdout
            with urllib.request.urlopen('http://127.0.0.1:18081/runtime-info.php',timeout=5) as response:
                metadata['http_runtime_info_after'] = json.load(response)
            metadata_path.write_text(json.dumps(metadata,indent=2)+'\n')
            rows = json.loads((folder/'summary.json').read_text())
            rates = {name:statistics.median(r['rps'] for r in rows if r['workload']==name) for name in ['minimal','worker']}
            return {'GOMAXPROCS':procs,'directory':str(folder.relative_to(ROOT)),'rates':rates,
                    'geometric_mean_rps':math.sqrt(rates['minimal']*rates['worker'])}
        finally:
            if server.poll() is None:
                server.send_signal(signal.SIGTERM)
                try:
                    server.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    server.kill();server.wait()
                    raise RuntimeError('FrankenPHP baseline did not stop gracefully')

for procs in [4,8,24]:
    records.append(launch(procs,f'frankenphp-go{procs}-tune',False))
best = max(records,key=lambda r:r['geometric_mean_rps'])['GOMAXPROCS']
result = {'tuning':records,'selected_GOMAXPROCS':best}
(ROOT/'bench/raw/frankenphp-final-selection.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'selected_GOMAXPROCS':best}),flush=True)
result['final'] = launch(best,'frankenphp-final',True)
(ROOT/'bench/raw/frankenphp-final-selection.json').write_text(json.dumps(result,indent=2)+'\n')
