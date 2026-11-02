The `openssl/engine-upstream-3.5.8.h` file is the unmodified official OpenSSL **3.5.8** header:
https://raw.githubusercontent.com/openssl/openssl/openssl-3.5.8/include/openssl/engine.h

The installed `openssl-devel-3.5.8-2.el10_2.alma.1.x86_64` package in the build
environment supplied a zero-byte `/usr/include/openssl/engine.h`. Its matching
system libcrypto still exports functional ENGINE API required by H2O neverbleed,
despite setting `OPENSSL_NO_ENGINE` in its development configuration. A small
wrapper restores these declarations. CMake executes ENGINE_new/ENGINE_free
against the installed libcrypto to verify this capability before using it.
CMake uses this header only when the system declaration check fails and the
system OpenSSL version is exactly 3.5.8. TLS remains linked to system OpenSSL.
The header is licensed under Apache-2.0, as stated in its copyright notice.
