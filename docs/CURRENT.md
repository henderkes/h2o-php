# Current progress

Updated 2026-11-04, 19:43 Asia/Bangkok. Work stopped at the user's request for PC shutdown. All owned servers, clients, profilers and Vibe workers have exited. The persistent engineering goal is **paused and incomplete** for the shutdown; the full performance acceptance gates have not passed. Resume it explicitly when ready to continue.

## What is on disk

| Item | Identity / status |
|---|---|
| Current source fingerprint | `a1b4c70f7a2af1248ec40d4b26dc3f22342f6e5c58c2ece962577907310beb37` |
| HEAD at shutdown audit | `c457cd103e57957b06a03563d612bfc97b7dab18` |
| User release | `build/release/h2o-php`, SHA256 `cea556f08d6844582c52a56e7e81c155ca7c58ddf8844aac705cf818ac12eb91` |
| Source identity embedded in that release | `e886516ce282110ae075966cdea7113767ed3101d8d1a0ecfed5d310048246a0`, revision `17d1370ab008d2a0ca71414f6ff7aa9351ee2b8b+dirty` |
| Installed FrankenPHP reference | `/usr/bin/frankenphp`, SHA256 `a83acfc081e9d0311e8ebcf968c89b5165c8aafcff815132b51ccf4e88ebd37d` |
| Installed embedded PHP | `/usr/lib64/libphp-zts.so`, SHA256 `218816a04e62dd5a7dad2b55e771a1defbf0b98c6291a397c7427c1fc7b181e2`; PHP 8.6.0RC4 ZTS, ABI 20260924 |

**The release does not contain the entire current source.** Later `worker-exit-log-20261104` and `protocol-metadata-20261104` binaries and previous benchmark/report directories were deleted by the user. They were not restored. Earlier qualifications do not qualify the present release or a future rebuild.

The release retains the user's intentional `WITH_IO_URING=ON`. Relevant cached options: Release, shared installed PHP, native CLI ON, libuv disabled, fusion/KTLS/Brotli/Zstd ON, prefer embedded CA OFF. Preserve those settings. The source fingerprint alone does not encode them; the copied CMake cache records the options.

Current native source still includes the worker-exit-status/warning correctness fix from earlier work. No native source was changed and no server binary was compiled or replaced in the final resumed segment. Source, tests, examples and local application settings were preserved. Four example/test files and HEAD changed concurrently; their before/after identities were recorded without overwriting them. The working tree was clean at the audit immediately before adding these two documents.

## Fresh checks on the actual release

- Existing PHP compatibility checks: **2 passed**, covering output handlers, sessions, request-state cleanup and same-second OPcache watcher invalidation.
- Existing worker checks: **5 behavioral cases passed**. Three cases used older expectations and failed: omitted count 24 versus actual 48; parking after one bootstrap failure versus actual recovery; queue expiry 504 versus actual 503. Four direct private-fixture checks subsequently passed with explicit correct headroom, exact startup-failure counters and queue-expiry behavior. Original failure logs remain. User tests were not rewritten to make them pass.
- Existing routing checks: **13 behavioral cases passed**. The configuration-error case failed against its older assertion about a required `.php` suffix/error text. The test file subsequently changed outside this work; that newer version has not been run. Do not call the whole suite green.
- Exact upload/stream checks at **one and eight reused HTTP/1 connections**: native **36** correct responses, reference **36** correct responses. Uploads were 1 MiB with exact returned length/SHA256; streams were exactly 2 MiB. Native additionally passed **4** exact 32 KiB static responses. Native cumulative completed delta was 36, failed/timeout deltas zero, queue/buffer counters quiescent; both engines exited normally.
- Both engines used the same private application, PHP version, loaded extension set including PCNTL/parallel, OPcache enabled, four regular plus four persistent workers, cap eight and the same CPU affinity. The complete INI comparison had one textual difference: native `opcache.jit="off"`, reference `""`; complete effective JIT equivalence still needs an explicit check before a performance comparison.
- Actual io_uring trace: **4 successful ring setups**, **2 successful enter submissions** and **2 pipe-to-socket splice calls** on the private `file.io_uring: ON` static route. The default static route used **2 sendfile calls**. This establishes runtime use for that route, not a speed benefit or universal PHP network-I/O use. No user setting was disabled.

The first reference transfer attempt stopped before transfer traffic because external `/proc/PID/maps` access was denied. The successful retry read the reference's own maps from its private PHP probe. No privilege/security-policy change was made.

No fresh throughput comparison, paired instruction measurement, optimization candidate or performance acceptance result was produced in this final segment.

## Where the evidence is

Local raw records are under [bench/qualification-resume-20261104-1934](../bench/qualification-resume-20261104-1934/): `functional/` contains outputs, executed private YAML/Caddyfiles, probes, syscall traces and original failures; `identity/` contains hashes, build options, state snapshots and the shutdown process audit. This directory is ignored by Git; keep it on disk if the raw evidence is wanted. These are local records, not a portable reproduction of deleted historical evidence.

Two independent Vibe workers finished normally. Both exports verify API model **`mistral-large-4-0`**, **`thinking="high"`**, provider **Mistral**, tools disabled and nested delegation disabled. Their prompts, tracked handles, logs, exports and reports are in that directory. Maximum was one orchestrator plus two workers, within four available slots. The dedicated profile did not change the user's Vibe defaults. Worker conclusions were independently checked and were not treated as acceptance results.

## Constraints carried forward

Keep native standalone H2O, installed PHP ZTS, C11/pthreads, genuine persistent PHP workers, native YAML, PCNTL/parallel and H2O protocols/TLS/static/proxy/filters. Keep Zend bailout boundaries in C. Mercure/Vulcain remain excluded. Do not touch the real application database or its vendor/cache directories.

Historical conclusions retained from the session, with raw historical files now absent: candidate29 was rejected; the header-flush fix was correctness work with a sub-threshold instruction change; upload/static tails and several operating points remained unqualified. The corrected initial request-mutex observer measured only **0.2876%**, below its **1%** selection threshold: the free-outside-lock patch remains unjustified. The later dispatch observation did not select a pending-queue change. Do not revive rejected caches, leases, allocator tweaks or combined candidates from old patches. Missing old evidence must remain a limitation, not be reconstructed as a new measurement.

Next work is in [TODO.md](TODO.md).
