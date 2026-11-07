# Standalone bridge ownership checks

Run these from the repository root. They require a C11 compiler and pthreads,
and do not require a built PHP server.

```sh
cc -std=gnu11 -O2 -pthread -I include tests/test_response_slab.c src/bridge/bridge.c -o /tmp/h2o-php-test-response-slab
/tmp/h2o-php-test-response-slab
cc -std=gnu11 -O2 -pthread -I include tests/test_input_recycle.c -o /tmp/h2o-php-test-input-recycle
/tmp/h2o-php-test-input-recycle
cc -std=c11 -D_GNU_SOURCE -pthread -I include tests/test_finish_notification.c src/bridge/bridge.c -o /tmp/h2o-php-test-finish-notification
/tmp/h2o-php-test-finish-notification
cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -I include tests/test_response_lease.c src/bridge/bridge.c -o /tmp/h2o-php-test-response-lease
/tmp/h2o-php-test-response-lease
```

The response test checks partial slab ownership, retained budget credit,
cancellation cleanup and output progress with unread input. The input test
checks independent copies, partial reads and EOF, capacity reuse and bounds,
input/output pressure eviction, exact-length admission, cancellation and
shutdown cleanup. It includes the bridge implementation to inspect idle
capacity without adding a production diagnostics API.

The finish test checks one terminal publication after the loop consumes its
message, concurrent finishes, pre-set cancellation/timeout state, input/output
waiters, and preserved header, first-byte, informational and explicit-flush
notifications.

The lease test uses a synchronous transport stub to check full borrowed-call
and metadata credit, caller reuse, irreversible late grants, the API-entry
lifetime gap, auxiliary output order during active writes and pending
handshakes, cancellation/deadline wakes, terminal failures without duplicate
fallback, refused-before-I/O fallback, concurrent finish, graceful shutdown
yield, redundant-flush suppression, and owned progress with 16/24 KiB limits.
Native H2O/socket and end-to-end server tests remain separate checks.

# HTTP/1 response lease wire checks

Run the 15 black-box cases against a built server from the repository root:

```sh
python3 tests/test_http1_body_lease.py --bin build/release/h2o-php
```

`H2O_PHP_BINARY` also selects the server. The suite creates its PHP fixtures in
temporary directories, binds fresh local ports, and stops every server it owns.
Use the PHP INI environment appropriate to the binary. TLS cases need `openssl`;
the HTTP/2 and fork cases skip when curl HTTP/2 support or Linux/PHP pcntl support
is unavailable. No private build or baseline binary is required. An optional
`--baseline /path/to/h2o-php` adds a comparison of invalid Content-Length response
semantics against another build.

The cases check exact body hashes while reusing caller buffers, chunked and
explicit-length output, small/large transitions, HEAD/204/304, gzip/TLS/HTTP/2
fallback, pipelining behind a blocked large writer, slow reads and reset recovery,
fork descriptor cleanup with parent connection reuse, reload/stop draining,
request deadlines, and owned output progress with unread input at 16/24 KiB.
Large plain-H1 streams use 64 KiB writes and the default global budget; tight
budget cases retain the original 4 KiB input preload before reading any output.

The native H2O/socket cases are part of `t-00unit-evloop.t`:

```sh
cmake --build <build-dir> --target t-00unit-evloop.t
(cd upstream/h2o && /absolute/path/to/build-dir/h2o/t-00unit-evloop.t)
```
