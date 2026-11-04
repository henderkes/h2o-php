#!/usr/bin/env python3
"""Summarize retained measurements without inventing absent observations."""
import argparse
import csv
import json
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent.parent

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directories', nargs='+')
    parser.add_argument('--scheduler', action='store_true', help='Show native scheduler counter deltas')
    args = parser.parse_args()
    if args.scheduler:
        print('| Server / workload | Connections | Mean queue wait us | Mean PHP execution us | Worker restarts | Runtime failures | Runtime timeouts | End buffered bytes |')
        print('|---|---:|---:|---:|---:|---:|---:|---:|')
    else:
        print('| Server / workload | Connections | Median RPS | p50 ms | p95 ms | p99 ms | Errors / timeouts | CPU % | Peak RSS MiB |')
        print('|---|---:|---:|---:|---:|---:|---:|---:|---:|')
    for directory in args.directories:
        folder = Path(directory)
        metadata = json.loads((folder/'metadata.json').read_text())
        rows = json.loads((folder/'summary.json').read_text())
        groups = {}
        for row in rows:
            groups.setdefault((row['workload'], row['connections']), []).append(row)
        for (workload, connections), values in groups.items():
            def median(field):
                numbers = [row[field] for row in values if field in row]
                return statistics.median(numbers) if numbers else float('nan')
            if args.scheduler:
                queue = []; execution = []; restarts = failures = timeouts = end_buffered = 0
                for row in values:
                    before = row.get('metrics_before', {}); after = row.get('metrics_after', {})
                    completed = after.get('completed',0) - before.get('completed',0)
                    if completed > 0:
                        queue.append((after.get('queue_wait_ns_total',0) - before.get('queue_wait_ns_total',0)) / completed / 1000)
                        execution.append((after.get('execution_ns_total',0) - before.get('execution_ns_total',0)) / completed / 1000)
                    restarts += after.get('worker_restarts',0) - before.get('worker_restarts',0)
                    failures += after.get('failed',0) - before.get('failed',0)
                    timeouts += after.get('timed_out',0) - before.get('timed_out',0)
                    end_buffered = max(end_buffered,after.get('buffered_bytes',0))
                if queue:
                    print(f'| {metadata["label"]} / {workload} | {connections} | {statistics.median(queue):.2f} | {statistics.median(execution):.2f} | {restarts} | {failures} | {timeouts} | {end_buffered} |')
                continue
            cpu_values = []
            rss_values = []
            for row in values:
                samples = ROOT / Path(row['raw']).with_suffix('.csv')
                if not samples.exists():
                    continue
                with samples.open() as source:
                    samples_rows = list(csv.DictReader(source))
                try:
                    delta = float(samples_rows[-1]['unix_seconds'])-float(samples_rows[0]['unix_seconds'])
                    cpu_values.append((float(samples_rows[-1]['cpu_seconds'])-float(samples_rows[0]['cpu_seconds'])) / delta * 100)
                    rss_values.extend(int(sample['rss_kib'])/1024 for sample in samples_rows)
                except (ValueError, IndexError, ZeroDivisionError):
                    pass
            errors = sum(sum(row.get('errors_'+kind, 0) for kind in ['connect','read','write','status']) for row in values)
            timeouts = sum(row.get('errors_timeout', 0) for row in values)
            cpu = f'{statistics.median(cpu_values):.1f}' if cpu_values else 'unavailable'
            rss = f'{max(rss_values):.1f}' if rss_values else 'unavailable'
            print(f'| {metadata["label"]} / {workload} | {connections} | {median("rps"):.0f} | {median("p50_ms"):.3f} | {median("p95_ms"):.3f} | {median("p99_ms"):.3f} | {errors} / {timeouts} | {cpu} | {rss} |')

if __name__ == '__main__':
    main()
