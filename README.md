# h2o-php

Native PHP ZTS inside H2O's real standalone server, implemented in C11 with dedicated PHP pthreads, streaming request/response bridges and persistent FrankenPHP-compatible workers. H2O retains its TLS, HTTP protocols, file serving, proxies, filters and virtual hosts. Mercure and Vulcain are intentionally excluded.

The current PHP profile is **8.6.0RC4 ZTS**, Zend module API **20260924**. The normal build links the installed `libphp-zts.so`.

Building requires CMake 3.20+, a C/C++ compiler, pkg-config, OpenSSL and zlib development files, and an embed-capable ZTS PHP 8.5 or 8.6 installation. The native PHP CLI requires PHP 8.6 with its CLI headers and exported entry point. Set `PHPZTS_CONFIG` and `PHPZTS_LIBRARY` as CMake options if automatic discovery does not find your installation.

```sh
./scripts/fetch-upstream.sh
cmake --preset release
cmake --build --preset release
./build/release/h2o-php --version
./build/release/h2o-php -t -c examples/worker.yml
./build/release/h2o-php run --config examples/worker.yml
```

Run all bundled non-framework YAML examples from the checkout root; their application paths are relative to that checkout (worker filenames may be relative to `php.root`). Adapt application paths in the framework examples. Build discovery uses matching `php-config-zts`, headers and the actual PHP library.

`run` stays in the foreground. Reload its configuration or stop it from another terminal:

```sh
./build/release/h2o-php reload --config examples/worker.yml
./build/release/h2o-php stop
```

Reload submits the supplied H2O YAML and waits for the replacement server to report ready. The previous server finishes its in-flight requests; validation, listener-bind and startup failures leave the active configuration running. Unchanged resolved configuration is skipped; use `reload --force` to restart it anyway, for example after replacing certificates. Included YAML and `!env` values are resolved before comparison and retained in the submitted snapshot. Reloads are serialized while older requests drain, within the configured shutdown deadline.

On Linux, the supervisor continues handling stop and reload operations when a pipe or socket log is full. Supervisor diagnostics use best effort delivery and may be dropped or truncated when a sink is full or unavailable.

These commands use a local Unix control socket, defaulting to `$XDG_RUNTIME_DIR/h2o-php.sock` when `XDG_RUNTIME_DIR` is set to an absolute path and `/tmp/h2o-php-<uid>.sock` otherwise, with access restricted to the owning user: clients verify both that the path is a socket owned by them and, after connecting, the peer's `SO_PEERCRED` user id before sending anything. For multiple instances, pass the same `--address unix//absolute/path.sock` to each instance's `run`, `reload` and `stop` commands. `reload --config` may name a different file from the startup configuration. This implements the foreground run/reload/stop workflow using H2O's native serving generations; it does not implement the Caddy HTTP admin API or Caddyfile/JSON configuration. Use systemd or another service manager for process crash recovery.

The older H2O invocation (`h2o-php -c FILE`, including explicit `-m worker`) still runs the serving process directly and does not provide this control socket. Existing master/daemon invocations remain available for compatibility. The supplied systemd service uses `run` and `reload` with an explicit control socket in its runtime directory.

`pid-file` is a process setting and requires a restart to change. The native H2O ACME loader remains tied to legacy master mode; `run` rejects `acme` configuration explicitly. TLS with externally managed certificates works with `run`; use a forced reload after replacing certificate files.

OpenSSL 4 uses a provider-based neverbleed integration. Private keys remain in neverbleed's helper process, with RSA/ECDSA signing and RSA decryption delegated over its existing transport. The build rejects removed ENGINE imports so they cannot accidentally bind to an older OpenSSL bundled in PHP. TLS 1.2, TLS 1.3, HTTP/2 and forced certificate reloads are checked with `neverbleed: ON`; no TLS isolation setting needs to be disabled. OpenSSL 3 retains the existing ENGINE integration.

A PHP site with static assets uses one handler:

```yaml
listen: 8080
php.runtime:
  num_threads: 2
  max_threads: 8
hosts:
  "localhost:8080":
    php.root: /srv/app/public
    paths:
      "/":
        php_server: ON
```

For a Symfony/Octane front controller that supports worker mode, use `php_server: {worker: {file: index.php, num: 1}}` on that path. Existing assets, favicons and robots.txt remain served by H2O. Direct `php` and `php.worker` handlers are available for routes that should always enter PHP.

`php.runtime.collect-metrics` accepts `ON` (the default) or `OFF`; `collect_metrics` is an alias inside `php.runtime`. With `OFF`, optional PHP metric collection stops and a `php.metrics: {format: prometheus}` route returns HTTP 200 with an empty exposition and no `frankenphp_*` families. The `php.metrics: ON` JSON diagnostics still report request, queue and buffer accounting. Admission limits, worker lifecycle, PCNTL and OPcache behavior are unchanged. The scalar aliases are `php.collect-metrics` and `php.collect_metrics`.

Persistent scripts use the native API and keep their application objects between callbacks:

```php
<?php
$app = new Application();
while (frankenphp_handle_request(static function () use ($app): void {
    $app->handle();
})) {}
```

The HTTP SAPI identifies as `frankenphp`; the real CLI identifies as `cli` and loads `frankenphp-cli`:

```sh
./build/release/h2o-php php-cli -r 'echo PHP_SAPI, "\n";'
./build/release/h2o-php php-cli /srv/app/bin/console --help
```

Build the shared `parallel` extension with `scripts/build-parallel.sh` and enable its resulting module in PHP INI.

The HTTP SAPI requires that patched module when `parallel` is enabled. A distribution's unpatched `parallel` module is rejected at startup. For this checkout, a compatible module and private scan directory were built under `build/extensions/parallel` and `build/php-ini`; `source build/php-env.sh` selects them for the shell without changing system PHP. Use the same PHP profile for `run` and its replacement generations.

More examples are in [examples/frameworks](examples/frameworks/README.md).
