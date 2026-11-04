#!/usr/bin/env python3
"""Measure H2O loop counts sequentially with the PHP execution budget fixed."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
base_config = (ROOT/'bench/h2o.yml').read_text()
output = ROOT/'build/loop-tuning'
output.mkdir(parents=True, exist_ok=True)
for loops, buffering in [(2,0), (2,4096), (4,4096), (8,4096)]:
    config = output/f'h2o-net{loops}-ob{buffering}.yml'
    text = re.sub(r'^num-threads: \d+$', f'num-threads: {loops}', base_config, flags=re.MULTILINE)
    config.write_text(re.sub(r'^  output_buffering: \d+$', f'  output_buffering: {buffering}', text, flags=re.MULTILINE))
    log_path = ROOT/f'bench/raw/native-net{loops}-ob{buffering}-server.log'
    with log_path.open('w') as log:
        server = subprocess.Popen([str(ROOT/'build/release/h2o-php'), '-c', str(config)], stdout=log, stderr=log, cwd=ROOT)
        try:
            for attempt in range(100):
                try:
                    with urllib.request.urlopen('http://127.0.0.1:18080/metrics', timeout=.2) as response:
                        ready = json.load(response).get('ready_workers', 0)
                    if ready == 4:
                        break
                except Exception:
                    if server.poll() is not None:
                        raise RuntimeError(f'H2O exited: {log_path}')
                time.sleep(.1)
            else:
                raise RuntimeError('H2O workers failed readiness')
            print(f'Tuning {loops} H2O loops, PHP output_buffering={buffering}, server PID {server.pid}', flush=True)
            subprocess.run([sys.executable, 'bench/run.py', '--base-url', 'http://127.0.0.1:18080',
                '--pid', str(server.pid), '--binary', 'build/release/h2o-php', '--config', str(config),
                '--metrics-path', '/metrics', '--runtime-info-path', '/runtime-info.php',
                '--label', f'native-loop{loops}-ob{buffering}-tune', '--duration', '3',
                '--warmup', '1', '--repeat', '3', '--connections', '128', '--workloads', 'minimal,worker'],
                cwd=ROOT, check=True)
        finally:
            if server.poll() is None:
                server.send_signal(signal.SIGTERM)
                try:
                    server.wait(timeout=35)
                except subprocess.TimeoutExpired:
                    server.kill(); server.wait()
                    raise RuntimeError(f'H2O {loops}-loop tuning server did not shut down')
