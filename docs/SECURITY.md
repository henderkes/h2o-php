# Security and reliability boundaries

This is a new native implementation. Its actual validation evidence is recorded separately; it is not a claim of independent audit or production readiness.

PHP runs in the H2O process with installed native extensions. ZTS permits concurrent engine contexts, not process isolation. A faulty native extension can crash or corrupt the process. H2O master/worker supervision or a service manager should restart failed processes. Run mutually untrusted applications in separate H2O processes and OS identities. PHP INI and extension loading are process-wide. OPcache/preload state is shared.

Configure `php.root` explicitly for every classic handler (inheritance is supported). Script paths resolve through `realpath`, must remain within that root, be regular readable files and end in `.php`. Encoded traversal and NULL paths pass through H2O normalization and native confinement checks. A symlink outside the root is rejected. Deployment directories should not be writable by request-serving users: resolving a path and subsequently opening it in PHP cannot eliminate a malicious filesystem replacement race. Worker scripts are trusted administrator-selected absolute paths and may live outside the public document root.

Request metadata is copied to a heap context with references held by the network request, scheduler, and completion messages. Network structures are only touched on their owning H2O loop. Input/output queues have per-request and process-wide budgets; queue admission has process and per-application limits. Overload returns 503. Queued requests expire with 504; request I/O waits have deadlines. Configured buffer sizes bound native queue memory, not the PHP heap, OPcache, kernel socket buffers or H2O protocol buffers. Use PHP `memory_limit`, H2O body/connection limits and service memory limits as well.

Use `max-request-entity-size` (an existing H2O setting), PHP `post_max_size`/`upload_max_filesize`, and native request deadlines for upload policy. Native request bodies stream, but PHP's form/multipart handlers may consume input before executing userland. Unknown media types and `php://input` can be read incrementally. Slow readers/writers block PHP execution threads while network loops continue.

Shutdown stops admission and rejects queued work, allows active work to finish, then interrupts cooperative PHP after the drain deadline. Blocked bridge readers/writers wake on cancellation. The control pthread joins execution pthreads before shutting down the engine/TSRM. Opcode interrupts cannot forcibly terminate an indefinitely blocked native extension. Configure an external process shutdown deadline; forcibly cancelling a pthread inside Zend is unsafe and is not used.

`putenv/getenv` overrides are thread-local and do not mutate the OS process environment. Worker `$_ENV` persists intentionally, matching FrankenPHP. Application-level state and resources intentionally persist; frameworks/applications must reset their own request state. Other process-wide APIs or native extensions (for example locale changes) are not made independent security boundaries by this sandbox.

Metrics and administrative worker-restart routes accept loopback peers only; restart requires POST. Do not proxy these routes publicly. Loopback restriction does not authenticate local users; use an isolated administrative listener and existing H2O access controls where needed. Structured application logs preserve message, severity and JSON context; avoid logging request secrets.

Informational responses require H2O `send-informational: all` for HTTP/1 clients (the upstream default excludes HTTP/1). Duplicate Set-Cookie values are preserved and hop-by-hop headers are removed. PHP/H2O header validation still applies.

Mercure is excluded and no placeholder publishing function exists.
