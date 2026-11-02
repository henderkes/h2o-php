# Building and installing

This project adds C sources to the **actual H2O standalone executable target**.
It uses H2O's native event loop and installed `libphp-zts.so`; it does not build
PHP or introduce a separate HTTP server.

Requirements: C11 compiler, CMake >=3.20, pthreads, installed PHP 8.5 ZTS embed
headers and library discoverable by `php-config-zts`, OpenSSL development files,
zlib, and H2O's bundled dependencies. Optional compression libraries are detected
by H2O. The current validated PHP ABI is PHP 8.5.12, API 20250925, release ZTS.
Use matching PHP extensions from `php-config-zts --extension-dir`.

After obtaining the pinned upstream checkouts recorded in
`UPSTREAM_COMPATIBILITY.md`, configure and build:

```sh
./scripts/fetch-upstream.sh
cmake --preset release -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++
cmake --build --preset release
./build/release/h2o-php --version
ldd ./build/release/h2o-php
```

The debug build is separate:

```sh
cmake --preset debug -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++
cmake --build --preset debug
```

The fetch script pins H2O, the FrankenPHP reference source, and the PHP source
matching installed 8.5.12. It applies `patches/h2o-native-php.patch` idempotently
and refuses to replace modified checkouts. The PHP and FrankenPHP sources are
references; the build links the installed PHP library.

For a user-owned installation:

```sh
cmake --preset release -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build --preset release
cmake --install build/release
./install/bin/h2o-php --version
```

For a system installation, choose a prefix at configure time and run the install
step with the privileges needed for that prefix. H2O helpers, including
`start_server`, are installed under `share/h2o` and must accompany the executable.
Do not initialize embedded PHP before forking; the standalone lifecycle hooks
initialize it after H2O's process setup.

Override discovery for nonstandard installations:

```sh
cmake --preset release \
  -DPHPZTS_CONFIG=/opt/php/bin/php-config-zts \
  -DPHPZTS_LIBRARY=/opt/php/lib/libphp-zts.so
```

CMake rejects missing ZTS headers and unresolved embed startup symbols. For PHP
8.5+, it also executes `php_version_id()` to verify the linked runtime matches the
headers. `php-config-zts --libs` may be empty; the actual PHP shared library is
always linked explicitly. The installed loader must resolve its SONAME. Verify
`ldd` after installation; use the system loader configuration or an appropriate
RPATH for a custom prefix.

The build environment's AlmaLinux OpenSSL 3.5.8 package contains an empty ENGINE
header. A conditional fallback uses the unmodified official 3.5.8 header in
`cmake/compat`; this does not replace libcrypto or disable H2O TLS features.

Native extensions and application code share the H2O process. Use separate H2O
processes and Unix accounts for applications requiring a security boundary.
