# Framework applications

Symfony dependencies are locked in `symfony/composer.lock`. Install with:

```sh
composer install --working-dir=examples/frameworks/symfony --no-dev
```

`index.php` returns a real Symfony HttpKernel; Symfony's installed, unmodified `FrankenPhpWorkerRunner` selects itself through `FRANKENPHP_WORKER`. The HTTP response carries an application bootstrap token to demonstrate persistence. No framework runner code is patched.

Laravel validation used a local application under ignored `build/frameworks/laravel`, generated using Composer. Its unmodified `vendor/laravel/octane/bin/frankenphp-worker.php` is the configured worker. Recreate with:

```sh
composer create-project laravel/laravel build/frameworks/laravel --no-dev --no-interaction
composer require laravel/octane --working-dir=build/frameworks/laravel --no-interaction
php-zts build/frameworks/laravel/artisan octane:install --server=frankenphp --no-interaction
```

Set worker environment `APP_BASE_PATH` to the application directory. For a dependency-free database/session demonstration use `SESSION_DRIVER: array` and `CACHE_STORE: array`. The H2O runtime replaces Octane's server transport; Octane's Caddy-launching Artisan command is not used to start H2O. The actual Octane worker script runs without modifications.

`examples/symfony.yml` and `examples/laravel-octane.yml` are validated native H2O YAML configurations. Install paths must be adapted outside this workspace. See `docs/VALIDATION.md` for exact installed versions and actual responses.
