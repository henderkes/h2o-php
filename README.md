# h2o-php

Embeded PHP inside H2O's real standalone server, implemented in C11.

Do not use this. It's a slop project to check how much overhead and go-runtime-handling-headaches can be avoided by... not using Go.

Performance: h2o is around 20-35% faster than Caddy in all relevsnt metrics. Request overhead is also lower here. On a small hello world test this is ~25% faster, on the symfony demo homepage it's 18.5k (h2o-php) vs 16.5k (frankenphp) requests per second. On actual php work the Go overhead doesn't matter.

Build requires php-config-zts on PATH:
`cmake --preset release`
`cmake --build --preset release`

Mirrors FrankenPHP's SAPI.

More examples are in [examples/frameworks](examples/frameworks/README.md).
