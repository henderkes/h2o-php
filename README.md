# h2o-php

Native PHP 8.5 ZTS execution inside the real H2O standalone server, with classic request threads and persistent FrankenPHP-compatible worker scripts. Implemented in C11 with pthreads, H2O event-loop completion messages and bounded streaming queues. No PHP-FPM is used for embedded execution. Mercure is intentionally excluded.

This is a newly implemented runtime. Consult the validation and benchmark records before deployment; it has not undergone an independent production security audit.

```sh
./scripts/fetch-upstream.sh
cmake --preset release
cmake --build --preset release
./build/release/h2o-php --version
./build/release/h2o-php -t -c examples/worker.yml
./build/release/h2o-php -m worker -c examples/worker.yml
```

The checked YAML files use paths for this workspace; replace `/home/m/h2o-php` with the checkout path when installing elsewhere. `php-config-zts`, installed matching PHP headers and the existing shared ZTS PHP library are required. The build checks the actual ABI and library version. H2O itself still handles TLS, HTTP protocols, static assets, filters, proxies and virtual hosts.

Persistent scripts use the original native API:

```php
<?php
$boot = new Application();
while (frankenphp_handle_request(static function () use ($boot): void {
    $boot->handle();
})) {}
```

The script boots once per worker generation; specialized partial PHP request cleanup preserves its objects and execution stack between callbacks. `$_ENV` persists as in FrankenPHP. The native `frankenphp` extension registers the required API aliases, reflection metadata, structured logging and log constants. `php_sapi_name()` returns `frankenphp`.

Documentation:

- [Build, installation and service](docs/INSTALL.md)
- [Configuration reference](docs/CONFIGURATION.md)
- [Architecture and ownership](docs/ARCHITECTURE.md)
- [Pinned dependencies and compatibility](docs/UPSTREAM_COMPATIBILITY.md)
- [Runtime verification](docs/VALIDATION.md)
- [Measured benchmarks](docs/BENCHMARKS.md)
- [Security and reliability](docs/SECURITY.md)
- [Engineering progress](docs/PROGRESS.md)

Real Symfony and Laravel Octane worker examples are described in [framework validation](examples/frameworks/README.md). Standalone classic, front-controller, worker, multiple-application, mixed static/worker and virtual-host configurations are under `examples/`.
