# Queue deadline functional check

One worker was occupied for600ms. Exactly one other request waited behind it with `max-wait-time: 100ms`.

| Case | Busy request | Waiting request | Waiting elapsed |
| --- | --- | --- | --- |
| Before correction |200 |504 |556.74ms |
| After, worker/release |200 |504 |101.57ms |
| After, classic/debug |200 |504 |101.33ms |

The old controller waited for a one-second housekeeping tick or worker availability. It now waits on a separate monotonic condition for the earliest queued deadline. Earlier submissions wake that condition. Queue expiry avoids file-watch scans and unrelated worker broadcasts. Active cancellation and watch housekeeping retain their separate cadence; shutdown also checks its absolute drain deadline promptly.

A separate check sent one CPU-bound PHP request, then stopped the server with a200ms drain budget. It exited0 after272.38ms including transport/engine teardown, with both H2O loops drained and zero pending completions. Exact executable hashes are recorded per result because concurrent routing work also rebuilt the workspace binaries. This change owns only `src/runtime/runtime.c`.

Reproduction files are preserved here; copy them into `/tmp/hp-queue-deadline` before running:

```sh
python3 /tmp/hp-queue-deadline/check.py after
HP_DIAGNOSTIC_BINARY="$PWD/build/debug/h2o-php" \
  python3 /tmp/hp-queue-deadline/check.py after-classic classic
python3 /tmp/hp-queue-deadline/shutdown.py
```

These are bounded functional checks, with two requests per queue case and one for shutdown. No benchmark or load generator ran.
