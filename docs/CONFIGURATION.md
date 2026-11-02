# Native H2O PHP YAML

H2O's ordinary configuration loader handles these directives alongside static files, reverse proxies, logging and filters. Run `h2o-php -t -c CONFIG` to validate. Working example configurations under `examples/` use absolute local paths, which must be adjusted for another installation.

## Process-wide runtime

```yaml
php.runtime:
  threads: 4
  max-threads: 16
  queue-limit: 1024
  max-wait-time: 10s
  request-timeout: 30s
  shutdown-timeout: 30s
  max-requests: 10000
  max-buffered-body: 1048576
  max-buffered-response: 262144
  max-buffered-global: 268435456
php.ini:
  opcache.enable: "1"
  opcache.jit: "off"
  upload_max_filesize: "16M"
  post_max_size: "16M"
```

`threads` is a fixed classic pool. Worker threads are additional; their sum with classic threads must fit `max-threads`. Limits are process-wide, not per virtual host. There is no dynamic autoscaling or idle shrinking; unsupported directives are rejected. Flat aliases exist for `php.threads`, `php.max-threads`, `php.max-wait-time`, `php.max-requests`, `php.queue-limit`, `php.max-buffered-body`, `php.max-buffered-response`, `php.max-buffered-global` and `php.shutdown-timeout`. Durations accept ms, s or m; sizes are bytes. Buffer limits must be at least4096 bytes; global budget must be at least16384 bytes.

The request admission budget covers all waiting applications. Each classic/worker application gets at most `max(1,queue-limit/application-count)` waiting slots so a saturated app leaves capacity for others. Excess admission returns503. A wait exceeding `max-wait-time` returns504. Request deadlines include queue and execution time. Native byte budgets exclude PHP/H2O/kernel memory. `max-requests` recycles classic TSRM contexts and supplies the default worker script request limit.

`php.ini` is a mapping of PHP directives applied after installed php.ini/conf.d. Quoted/newline-containing INI values are rejected by this YAML adapter. Extension loading and preload apply to the process-wide PHP engine; use separate processes for incompatible INI/extension requirements.

## Routing and inheritance

`php.root`, `php.front-controller`, `php.split-path`, `php.resolve-root-symlink`, `php.env`, and `php.request-timeout` inherit from global to host, path and extension scopes. Child environments override matching inherited keys. INI/runtime settings are global only. PHP handlers attach at path or `file.custom-handler` extension scope.

Every classic route requires `php.root`. Resolved PHP scripts must remain within this canonical root and end in `.php`. `php.resolve-root-symlink` must be ON; unresolved roots are rejected. `php.split-path` defaults ON and recognizes `.php/PATH_INFO`. `php.front-controller` is a relative PHP file under the root, used for application routes; real static files can fall through to H2O.

```yaml
php.root: /srv/app/public
hosts:
  "localhost:8080":
    paths:
      "/":
        file.dir: /srv/app/public
        file.index: [index.php, index.html]
        file.custom-handler:
          extension: .php
          php.classic: ON
```

For front-controller routing put `php.front-controller: index.php`, `php.classic: ON`, then `file.dir` on the path, as demonstrated by `examples/front-controller.yml` (order follows H2O's deferred handler registration).

## Persistent applications

```yaml
php.worker:
  file: /srv/app/worker.php
  name: application
  num: 4
  max-requests: 10000
  max-consecutive-failures: 10
  env:
    APP_ENV: production
  watch:
    - /srv/app/src
```

`file` must resolve to a regular script and is an administrator-selected bootstrap path. `num` fixes the application's thread count. Worker environment is available at bootstrap; route `php.env` and H2O request environment apply to callbacks. `FRANKENPHP_WORKER=1` is provided. Requests enter the group's queue and are only executed when a worker reaches `frankenphp_handle_request`.

Workers recycle after their request count, script termination, watch generation change, OPcache reset or administrative restart. Bootstrap failures use exponential delays capped at6.4 seconds and stop after the configured consecutive limit. A failed group with no live threads returns503. Watch paths are recursively polled once per second (no symlink recursion, depth32); watch enables OPcache timestamp validation and `revalidate_freq=0`. A manually requested restart reexecutes scripts; when OPcache timestamps are disabled, call `opcache_reset()` or use an appropriate invalidation/deployment policy for code replacement.

`php.metrics: ON` creates a loopback-only JSON status path. `php.restart: ON` creates a loopback-only administrative endpoint accepting POST and restarting all worker groups gracefully. Keep these paths off public reverse-proxy routes and apply OS/H2O administrative access controls. `send-informational: all` enables103 for HTTP/1 as well as HTTP/2/3.
