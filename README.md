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
./build/release/h2o-php -m worker -c examples/worker.yml
```

Run the commands above from the checkout root. `examples/classic.yml` and `examples/worker.yml` use paths relative to that root; adapt application paths in the framework examples. Build discovery uses matching `php-config-zts`, headers and the actual PHP library.

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

More examples are in [examples/frameworks](examples/frameworks/README.md).
