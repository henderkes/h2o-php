# Framework applications

Run these commands from the h2o-php checkout root. The Symfony example uses [henderkes/symfony-frankenphp-docker-demo](https://github.com/henderkes/symfony-frankenphp-docker-demo), cloned alongside h2o-php as `../symfony-demo` (`~/Projects/symfony-demo` in this checkout). H2O serves the application's `public/` directory, including static assets, and runs `public/index.php` as a persistent worker using its own `App\Runtime\FrankenPhpRuntime`.

The PHP ZTS installation needs `ctype`, `iconv`, `pcntl`, `posix`, `sysvsem`, `shmop` and `pdo_sqlite`. For a fresh checkout, clone the demo and select its bundled SQLite database in an untracked local environment file:

```sh
git clone https://github.com/henderkes/symfony-frankenphp-docker-demo.git ../symfony-demo
cat > ../symfony-demo/.env.local <<'EOF'
APP_ENV=prod
APP_DEBUG=0
DATABASE_URL="sqlite:///%kernel.project_dir%/data/database.sqlite"
EOF
```

The upstream demo declares `ext-pdo_pgsql` as mandatory. For this SQLite setup, replace that requirement with `ext-pdo_sqlite` and refresh the lock metadata without changing the locked package versions:

```sh
php-zts "$(command -v composer)" remove --working-dir=../symfony-demo --no-update ext-pdo_pgsql
php-zts "$(command -v composer)" require --working-dir=../symfony-demo --no-update 'ext-pdo_sqlite:*'
php-zts "$(command -v composer)" update --working-dir=../symfony-demo --lock --no-install --no-scripts --no-interaction
```

Install dependencies with the same PHP ZTS installation as H2O, build the production assets, and start H2O:

```sh
APP_ENV=prod APP_DEBUG=0 php-zts "$(command -v composer)" install --working-dir=../symfony-demo --no-dev --optimize-autoloader --no-interaction
php-zts ../symfony-demo/bin/console asset-map:compile --env=prod --no-debug
./build/release/h2o-php run -c examples/symfony.yml
```

The standalone configuration listens at `http://localhost/` on port 80. Linux builds automatically try to grant `CAP_NET_BIND_SERVICE` so it can run as your own user. If the build cannot obtain permission, run its printed `sudo setcap` command or authenticate with `sudo -v` and build again. The bundled SQLite database supports the blog and login pages. The application's PostgreSQL-specific stats endpoints require PostgreSQL.

This checkout's production demo disables application logging: `config/bundles.php` enables MonologBundle only in `dev` and `test`, and `config/packages/monolog.yaml` contains only those environments. `config/services_prod.yaml` supplies `Psr\Log\NullLogger`; `config/packages/prod/logging.yaml` sets `framework.php_errors.log: false`. Apply the same configuration to a fresh clone to avoid Monolog record creation, buffering and output. The H2O example passes `APP_RUNTIME_OPTIONS: '{"error_handler":false}'` to skip Symfony's bootstrapping error logger. PHP's reporting mask excludes deprecations while other PHP errors are logged, and error display stays disabled. H2O diagnostics and request errors remain enabled on stderr. Rebuild Symfony's production cache after changing its service configuration and reload H2O to replace its persistent workers.

Laravel validation used a local application under ignored `build/frameworks/laravel`, generated using Composer. Its unmodified `vendor/laravel/octane/bin/frankenphp-worker.php` is the configured worker. Recreate with:

```sh
composer create-project laravel/laravel:13.11.0 build/frameworks/laravel --no-dev --no-interaction
cp examples/frameworks/laravel/composer.json examples/frameworks/laravel/composer.lock build/frameworks/laravel/
composer install --working-dir=build/frameworks/laravel --no-dev --no-interaction
cp examples/frameworks/laravel/routes/web.php build/frameworks/laravel/routes/web.php
php-zts build/frameworks/laravel/artisan vendor:publish --provider='Laravel\Octane\OctaneServiceProvider' --tag=octane-config
```

Export the absolute application directory before starting Laravel; the YAML reads it with `!env APP_BASE_PATH`:

```sh
export APP_BASE_PATH="$PWD/build/frameworks/laravel"
./build/release/h2o-php run -c examples/laravel-octane.yml
```

For a dependency-free database/session demonstration use `SESSION_DRIVER: array` and `CACHE_STORE: array`. The H2O runtime replaces Octane's server transport; Octane's Caddy-launching Artisan command is not used to start H2O. The actual Octane worker script runs without modifications.

`examples/symfony.yml`, `examples/laravel-octane.yml` and `examples/frameworks-combined.yml` are native H2O YAML configurations. Their application roots are relative to the h2o-php checkout root, and worker filenames are relative to `php.root`. The combined example requires both applications and the same `APP_BASE_PATH` export. The `/tmp/h2o-php-config-*` filenames in `run` diagnostics refer to private validation snapshots; they do not change the working directory used to resolve application paths.

The Laravel scaffold used for validation is 13.11.0; tracked Composer metadata locks framework 13.39.0 and Octane 2.22.0. The checked route fixture reproduces the benchmark JSON endpoints. No generated application key, `.env`, vendor tree, database or formal framework test suite is included in this repository.
