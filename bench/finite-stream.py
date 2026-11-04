#!/usr/bin/env python3
"""Check finite concurrent streaming responses and HTTP/1 connection reuse."""
import argparse
import concurrent.futures
import hashlib
import http.client
import json
from pathlib import Path
import subprocess
import threading
import time
import urllib.request

p = argparse.ArgumentParser()
p.add_argument('--port', type=int, default=18080)
p.add_argument('--connections', type=int, default=128)
p.add_argument('--requests-per-connection', type=int, default=3)
p.add_argument('--binary', required=True)
p.add_argument('--output', default='bench/raw/native-finite-stream.json')
args = p.parse_args()
expected_size = 2*1024*1024
expected_hash = hashlib.sha256(b's'*expected_size).hexdigest()
barrier = threading.Barrier(args.connections)
def metrics():
    with urllib.request.urlopen(f'http://127.0.0.1:{args.port}/metrics',timeout=5) as response:
        return json.load(response)
def client(index):
    connection = http.client.HTTPConnection('127.0.0.1',args.port,timeout=30)
    results = []; errors = []; ports = []
    try:
        barrier.wait(timeout=30)
        for sequence in range(args.requests_per_connection):
            begin = time.monotonic()
            connection.request('GET','/stream.php')
            ports.append(connection.sock.getsockname()[1])
            response = connection.getresponse()
            digest = hashlib.sha256(); size = 0
            while True:
                data = response.read(65536)
                if not data:
                    break
                size += len(data); digest.update(data)
            correct = response.status == 200 and size == expected_size and digest.hexdigest() == expected_hash
            results.append({'sequence':sequence,'status':response.status,'bytes':size,'sha256':digest.hexdigest(),
                            'seconds':time.monotonic()-begin,'correct':correct})
            if not correct:
                errors.append('Response status, length or SHA256 mismatch')
    except Exception as error:
        errors.append(f'{type(error).__name__}: {error}')
    finally:
        connection.close()
    return {'client':index,'local_ports':ports,'connection_reused':len(ports)==args.requests_per_connection and len(set(ports))==1,
            'responses':results,'errors':errors}

before = metrics(); begin = time.monotonic()
with concurrent.futures.ThreadPoolExecutor(max_workers=args.connections) as pool:
    clients = list(pool.map(client,range(args.connections)))
binary = Path(args.binary).resolve()
result = {'binary':str(binary),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
          'binary_version':subprocess.run([str(binary),'--version'],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout,
          'connections':args.connections,'requests_per_connection':args.requests_per_connection,
          'expected_responses':args.connections*args.requests_per_connection,
          'complete_responses':sum(len(c['responses']) for c in clients),
          'correct_responses':sum(r['correct'] for c in clients for r in c['responses']),
          'connections_reused':sum(c['connection_reused'] for c in clients),
          'errors':[e for c in clients for e in c['errors']], 'elapsed_seconds':time.monotonic()-begin,
          'metrics_before':before,'metrics_after':metrics(),'clients':clients}
Path(args.output).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k not in ['clients','binary_version']}),flush=True)
if result['errors'] or result['correct_responses'] != result['expected_responses']:
    raise SystemExit(1)
