# Direct runtime verification

These are direct checks against the real executable and temporary diagnostic fixtures. No formal unit/integration test framework was introduced. Runtime binaries link the installed PHP8.5.12 ZTS library directly.

## Completed observations before benchmark freeze

- Real H2O HTTP responses report PHP SAPI `frankenphp`, native extension present and required aliases/reflection signatures. Mercure function absent.
- Classic request body, query, cookie, headers, status and environment work.32 simultaneous putenv/getenv probes did not leak values, and a following classic request had no old override.
- Persistent worker requests reused an identical bootstrap token with increasing counts. Query and custom `$_SERVER` state reset. Eight direct native requests demonstrated count1–4 then a new token/count1–4 after configured recycling;80 concurrent HTTP worker requests shared one token with unique counts and reset request state.
- Worker `$_ENV` persists intentionally. Sessions without cookies get independent IDs with old-state count0; custom session-handler lifecycle is checked separately below.
- Structured `frankenphp_log` output preserves nested JSON fields and severity; public aliases and duplicate Set-Cookie headers work. Fatal errors and exit do not prevent subsequent classic requests.
- A4MiB POST returned the correct SHA256. URLencoded, chunked and multipart requests produced expected PHP values. An incremental POST fixture produced its first HTTP response after8192 bytes, before the remaining2MiB upload was sent. This validates bidirectional bridge streaming rather than mandatory full-body buffering for unknown media types.
- Four flushes across400ms produced the first response bytes at about3ms. Early `frankenphp_finish_request` delivered the response in about2ms while PHP continued approximately400ms; repeated finish returned false and a post-response side effect completed.
- HTTP/2 cleartext body upload works through H2O.103 uses H2O informational responses with `send-informational: all`.
- H2O static responses preserve range and ETag behavior. Classic `file.custom-handler`, `.php/PATH_INFO`, mounted roots, front controllers, multiple worker applications and virtual hosts were exercised. A real H2O reverse-proxy route forwarded successfully.
- Six standalone examples (`classic`, `front-controller`, `worker`, `multiple-workers`, `mixed`, `virtual-hosts`) passed the real YAML loader, served expected HTTP traffic and exited with status0 after graceful stop.
- OPcache reports enabled, cached scripts and hits. A new-ZTS-thread OPcache directive initialization defect in installed PHP was found and corrected through public Zend INI metadata; after correction `used_memory=9194168`, `free_memory=125023560`, `wasted_memory=0` and JSON encoding succeeded.

The SAPI-level harness evidence was initially captured in `/tmp/hp-smoke.out` and `/tmp/hp-smoke.err`; representative durable raw HTTP/memory/profiling evidence is placed under `bench/validation` as checks finish. Temporary diagnostics are not a reproducible automated test suite.

## Real frameworks

Symfony dependencies were installed in `examples/frameworks/symfony`; locked `symfony/runtime`, `http-kernel` and `routing` versions are8.1.10. The real Symfony runtime selected its unmodified `FrankenPhpWorkerRunner`. Two requests returned bootstrap token `c9ed0b521b46e012` with queries `first=1` and `second=2`, proving application reuse and request query reset. The application returns a real Symfony HttpKernel.

A real Laravel application and Octane dependency were installed under `build/frameworks/laravel`. The original `vendor/laravel/octane/bin/frankenphp-worker.php` was executed unchanged, with `APP_BASE_PATH` and array session/cache environment. Responses included `{"framework":"Laravel Octane","path":"/","query":{"first":"1"}}` and `{"hello":"native","framework":"Laravel Octane"}` for `/hello/native`. The Caddy-launching Octane Artisan command is not the H2O launcher; the existing PHP worker script is compatible.

Both frameworks ran concurrently in one H2O process (PID34177 initially) with two network threads, a classic pool and separate single-thread worker groups on loopback ports18084/18085. Validated configurations are `examples/symfony.yml`, `examples/laravel-octane.yml` and `examples/frameworks-combined.yml`.

## Acceptance still requiring recorded evidence

Final benchmark/stability results, allocator diagnostics, cooperative CPU timeout recovery, worker error recovery, TLS and file-watch/admin restart checks are recorded as they finish. Untested protocol/extension combinations are not claimed as validated. HTTP/3 remains built through H2O/quicly but requires an available QUIC client for direct validation.
