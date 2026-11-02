# Upstream and ABI compatibility (working engineering record)

## Pinned sources

| Source | Revision | Selection |
| --- | --- | --- |
| H2O | `7bbb3e39bf03027344f36b4650e5383c3b6b1014` | Current 2026-11-02 history, including proxy request-stream error handling and bundled picotls/quicly updates; standalone implementation retained. |
| FrankenPHP | `4384c506d219cd2f8750edfceae57d3db8eb54f3` | Actual C SAPI, function stubs/arginfo, environment sandbox, session and persistent request lifecycle reference. |
| PHP source | `136e049ac10b032c83018e65b63b6df858943750` | Commit underlying annotated tag `php-8.5.12`, matching installed release. |

Sources: [H2O](https://github.com/h2o/h2o/tree/7bbb3e39bf03027344f36b4650e5383c3b6b1014), [FrankenPHP](https://github.com/php/frankenphp/tree/4384c506d219cd2f8750edfceae57d3db8eb54f3), [PHP](https://github.com/php/php-src/tree/136e049ac10b032c83018e65b63b6df858943750).

Installed runtime: PHP **8.5.12**, ZTS enabled, debug disabled, PHP/Zend module ABI **20250925**, Zend extension ABI **420250925**. `php-config-zts` is `/usr/bin/php-config-zts`. Headers are `/usr/include/php-zts`. Library `/usr/lib64/libphp-zts.so` resolves to `libphp-zts-85.so` (ELF SONAME). `--libs` is empty and `--lib-dir` reports `/usr/lib` despite the actual multilib directory. Discovery must link the actual library explicitly and validate headers/runtime versions. Exported `php_module_startup`, `php_request_startup`, `sapi_startup`, `php_tsrm_startup`, `tsrm_startup` and embed symbols were checked with `nm -D`; ELF dependencies and SONAME checked with `readelf` and `ldd`. Installed shared extensions live in `/usr/lib64/php-zts/modules`; OPcache is available. PHP is not rebuilt.

## H2O integration

`src/main.c` registers the native configurator with existing configurators, initializes PHP after standalone process setup/drop-privileges and before starting networking threads, and hooks drain/shutdown before upstream process exit. The standalone CMake target remains the server; the integration adds native C sources and `libphp-zts.so`. TLS, HTTP/1, HTTP/2, QUIC/HTTP/3, file serving, proxies, filters and process management remain upstream implementations.

Inspection covered `include/h2o.h`, `multithread.h`, `configurator.h`, standalone `main.c` and CMake, file/custom-handler configuration, FastCGI/proxy handlers and core request/generator code. Each handler has per-event-loop completion receivers. `on_req` copies metadata and only the owning loop calls `h2o_start_response`, `h2o_send`, `h2o_send_informational` and input `proceed_req`. PHP body reads and writes use bounded native queues. Request streaming uses `supports_request_streaming` and `write_req.cb`. H2O's initial routing decision affects extension-handler streaming and is tracked explicitly until verified.

## PHP API and worker semantics

The SAPI name and native module identity are `frankenphp`. Required functions use native Zend registration and generated-style arginfo matching pinned upstream signatures; no PHP polyfills. Required aliases and log constants are implemented in the extension. Mercure functions are deliberately absent.

The worker PHP script is bootstrapped with one full PHP request and remains on its execution pthread. `frankenphp_handle_request` performs specialized partial request activation/deactivation adapted from the pinned C source; it must not perform full PHP request shutdown/startup per callback. Persistent globals, objects, classes, statics and runtime changes remain; request superglobals (except `$_ENV`), output, headers, body resources, sessions and selected extension state reset according to upstream behavior. Sandboxed `putenv/getenv` use thread-local layered maps rather than changing the process environment. Worker `$_ENV` persistence is intentional upstream compatibility.

Worker readiness occurs at `frankenphp_handle_request`; per-group queues dispatch only when a ready worker takes work. Thread-local request counts and generation changes trigger graceful script recycling. Failed bootstrap uses capped exponential backoff and a consecutive failure limit. Watch paths trigger generation changes. Exact framework behavior, alias reflection and lifecycle edge cases require direct HTTP validation; implementation presence is not proof of compatibility.

## Configuration correspondence

`php.runtime` maps native process-wide classic threads, total maximum thread budget, waiting limits, buffer limits and deadlines. `php.worker` declares application-specific fixed execution threads, script, environment, watch paths and recycle/failure limits. `php.root`, `php.env`, `php.front-controller`, `php.request-timeout` inherit via H2O config scopes; classic/worker handlers attach at path or extension levels. `php.ini` is global because a shared PHP engine is not a per-host isolation boundary. Unknown settings must be rejected. Dynamic autoscaling and idle shrinking are not claimed by fixed thread counts.

## Technical boundaries

- PHP 8.5 release ZTS ABI is the supported lifecycle target. Other minors need an explicit lifecycle review rather than bypassing checks.
- ZTS threads share a process and engine/extensions; virtual hosts do not create a security boundary. Deploy untrusted applications in separate H2O processes.
- Native extension crashes terminate the H2O process. Zend bailout only recovers supported PHP termination/error paths.
- Opcode interrupts can bound cooperative PHP execution. A native extension blocked indefinitely cannot safely be killed as a pthread; external process supervision remains necessary.
- Linux is the implementation/validation platform. Other operating systems and all protocol/extension combinations remain unverified until exercised.
- OPcache availability, shared-extension loading, preloading/restart semantics and framework execution are tracked by actual runtime checks; no blanket extension compatibility claim.
- Mercure publishing, hubs, subscriptions, configuration, benchmarks and `mercure_publish()` are **explicitly excluded**. No dummy function is registered.

See `PROGRESS.md` and `BENCHMARKS.md` for completed validation and measured results.
