# Benchmark record

The final benchmark gate is **pending**. No native performance or stability
claims are made until the integrated executable has been measured.

Machine inventory and exact dependency/build information are recorded in
[`bench/environment.json`](../bench/environment.json). Initial inventory:

| Component | Observed value |
|---|---|
| CPU | Intel Core i7-13700, 24 logical CPUs visible |
| Memory | 24,029 MiB RAM; WSL2 virtual machine |
| Kernel | Linux 6.18.33.1-microsoft-standard-WSL2 x86_64 |
| Compiler selected | GCC 14.3.1, `/usr/bin/gcc` |
| CMake | 3.31.8 |
| Embedded PHP | 8.5.12 ZTS, release ABI 20250925 |
| Native H2O revision | `7bbb3e39bf03027344f36b4650e5383c3b6b1014` |
| Available FrankenPHP | 1.13.4, PHP 8.5.12 ZTS, Caddy 2.11.7 |
| Available PHP-FPM | 8.5.12 ZTS |
| Load generator | `wrk`, epoll build |

`bench/run.py` sends real HTTP traffic through wrk. It records warmup output,
three measured runs for each workload and 1, 8, 32, 128 connections, exact wrk
commands, p50/p95/p99, request errors/timeouts, and per-second server CPU/RSS.
It records the tested commit in every output directory. The load generator and
server share this host; results are local measurements, not independent hardware
capacity claims. There is no coordinated omission correction in wrk.

Fixtures in `examples/bench` cover minimal PHP, JSON allocations/hash computation,
a worker with persistent initialized state, 1KiB/1MiB POST, a 2MiB incremental
response, and H2O static routing. The worker fixture is unchanged between the
native runtime and FrankenPHP. OPcache must be enabled and JIT disabled for
comparable runs. Framework measurements require actual installed dependencies;
unavailable frameworks must be reported explicitly.

Planned command after server launch (PID is the actual server process):

```sh
python3 bench/run.py --base-url http://127.0.0.1:18080 \
  --pid SERVER_PID --label native --duration 10 --repeat 3 --stability 600
```

The stability duration is at least ten minutes. Worker recycling, response
correctness, and process liveness must also be inspected; a high requests/second
value alone cannot satisfy the gate. Raw outputs belong under `bench/raw`.

The FrankenPHP comparison configuration is `bench/FrankenPHP.Caddyfile`, using
four classic and four persistent worker threads. Set `BENCH_ROOT` to the absolute
`examples/bench` path before launching. Run comparisons separately from native
measurements to avoid competing servers consuming CPU.
