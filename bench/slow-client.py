#!/usr/bin/env python3
"""Throttle a real streaming response while measuring unrelated worker requests."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import socket
import statistics
import subprocess
import threading
import time
import urllib.request

p = argparse.ArgumentParser()
p.add_argument('--port', type=int, default=18080)
p.add_argument('--output', default='bench/raw/native-slow-client.json')
p.add_argument('--binary', help='Exact measured server executable')
args = p.parse_args()
base = f'http://127.0.0.1:{args.port}'
stop = threading.Event()
samples = []; latencies = []; errors = []
def background():
    while not stop.is_set():
        try:
            begin = time.monotonic()
            with urllib.request.urlopen(base+'/worker', timeout=3) as r:
                json.load(r)
            latencies.append((time.monotonic()-begin)*1000)
            with urllib.request.urlopen(base+'/metrics', timeout=3) as r:
                samples.append(json.load(r))
        except Exception as error:
            errors.append(str(error))
        stop.wait(.05)

thread = threading.Thread(target=background)
thread.start()
connection = http.client.HTTPConnection('127.0.0.1', args.port, timeout=30)
connection.connect()
connection.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
begin = time.monotonic()
connection.request('GET', '/slow.php')
response = connection.getresponse()
first_header = time.monotonic()-begin
received = 0; digest = hashlib.sha256(); first_byte = None
try:
    while True:
        chunk = response.read(65536)
        if not chunk:
            break
        if first_byte is None:
            first_byte = time.monotonic()-begin
        digest.update(chunk); received += len(chunk)
        time.sleep(.0625)  # At most 1MiB/s at the application reader.
finally:
    elapsed = time.monotonic()-begin
    connection.close(); stop.set(); thread.join()
ordered = sorted(latencies)
def percentile(q):
    return ordered[min(len(ordered)-1, int((len(ordered)-1)*q))] if ordered else None
expected = 16*1024*1024
result = {'status': response.status, 'bytes': received, 'expected_bytes': expected,
          'sha256': digest.hexdigest(), 'correct': received == expected and digest.hexdigest() == hashlib.sha256(b'r'*expected).hexdigest(),
          'header_seconds': first_header, 'first_body_seconds': first_byte, 'elapsed_seconds': elapsed,
          'reader_limit_bytes_per_second': 1048576, 'socket_receive_buffer_requested': 65536,
          'concurrent_worker_requests': len(latencies), 'worker_p50_ms': percentile(.5), 'worker_p95_ms': percentile(.95), 'worker_p99_ms': percentile(.99),
          'worker_errors': errors, 'peak_native_buffered_bytes': max((m.get('buffered_bytes',0) for m in samples),default=0),
          'metrics_samples': samples}
if args.binary:
    binary = Path(args.binary).resolve()
    result['binary'] = str(binary)
    result['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
    result['binary_version'] = subprocess.run([str(binary),'--version'],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout
Path(args.output).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='metrics_samples'}),flush=True)
if not result['correct'] or errors:
    raise SystemExit(1)
