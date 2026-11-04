# Framework applications

Symfony dependencies are locked in `symfony/composer.lock`. Install with:

```sh
composer install --working-dir=examples/frameworks/symfony --no-dev
```

`index.php` returns a real Symfony HttpKernel; Symfony's installed, unmodified `FrankenPhpWorkerRunner` selects itself through `FRANKENPHP_WORKER`. The HTTP response carries an application bootstrap token to demonstrate persistence. No framework runner code is patched.

Laravel validation used a local application under ignored `build/frameworks/laravel`, generated using Composer. Its unmodified `vendor/laravel/octane/bin/frankenphp-worker.php` is the configured worker. Recreate with:

```sh
composer create-project laravel/laravel:13.11.0 build/frameworks/laravel --no-dev --no-interaction
cp examples/frameworks/laravel/composer.json examples/frameworks/laravel/composer.lock build/frameworks/laravel/
composer install --working-dir=build/frameworks/laravel --no-dev --no-interaction
cp examples/frameworks/laravel/routes/web.php build/frameworks/laravel/routes/web.php
php-zts build/frameworks/laravel/artisan vendor:publish --provider='Laravel\Octane\OctaneServiceProvider' --tag=octane-config
```

Set worker environment `APP_BASE_PATH` to the application directory. For a dependency-free database/session demonstration use `SESSION_DRIVER: array` and `CACHE_STORE: array`. The H2O runtime replaces Octane's server transport; Octane's Caddy-launching Artisan command is not used to start H2O. The actual Octane worker script runs without modifications.

`examples/symfony.yml` and `examples/laravel-octane.yml` are native H2O YAML configurations. Adapt their application paths for your checkout.

The Laravel scaffold used for validation is 13.11.0; tracked Composer metadata locks framework 13.39.0 and Octane 2.22.0. The checked route fixture reproduces the benchmark JSON endpoints. No generated application key, `.env`, vendor tree, database or formal framework test suite is included in this repository.
