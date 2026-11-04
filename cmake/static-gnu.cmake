# GNU dependency profile. This prefers static third-party libraries; the ELF
# manifest determines whether libc/compiler support remain dynamic.
set(CMAKE_SYSTEM_NAME Linux)
if(NOT DEFINED H2O_PHP_STATIC_DEPS)
  message(FATAL_ERROR "Set H2O_PHP_STATIC_DEPS to the pinned static dependency prefix")
endif()
if(NOT DEFINED H2O_PHP_STATIC_PHP)
  message(FATAL_ERROR "Set H2O_PHP_STATIC_PHP to the isolated PHP install prefix")
endif()
find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
find_program(CMAKE_AR NAMES llvm-ar REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib REQUIRED)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES H2O_PHP_STATIC_DEPS H2O_PHP_STATIC_PHP)
set(CMAKE_PREFIX_PATH "${H2O_PHP_STATIC_PHP};${H2O_PHP_STATIC_DEPS}")
set(CMAKE_FIND_LIBRARY_SUFFIXES .a .so)
set(PHPZTS_LINK_STATIC ON CACHE BOOL "Static PHP embed archive")
set(PHPZTS_CONFIG "${H2O_PHP_STATIC_PHP}/bin/php-config" CACHE FILEPATH "Matching static PHP config")
set(PHPZTS_LIBRARY "${H2O_PHP_STATIC_PHP}/lib/libphp.a" CACHE FILEPATH "Matching static PHP archive")
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(OPENSSL_ROOT_DIR "${H2O_PHP_STATIC_DEPS}" CACHE PATH "Matching OpenSSL headers and archives")
set(OPENSSL_INCLUDE_DIR "${H2O_PHP_STATIC_DEPS}/include" CACHE PATH "Matching OpenSSL headers")
set(OPENSSL_SSL_LIBRARY "${H2O_PHP_STATIC_DEPS}/lib/libssl.a" CACHE FILEPATH "Static SSL")
set(OPENSSL_CRYPTO_LIBRARY "${H2O_PHP_STATIC_DEPS}/lib/libcrypto.a" CACHE FILEPATH "Static crypto")
set(ZLIB_INCLUDE_DIR "${H2O_PHP_STATIC_DEPS}/include" CACHE PATH "Matching zlib headers")
set(ZLIB_LIBRARY "${H2O_PHP_STATIC_DEPS}/lib/libz.a" CACHE FILEPATH "Static zlib")
set(CMAKE_C_FLAGS_INIT "-O2 -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_INIT "-O2 -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -Wl,--gc-sections -L${H2O_PHP_STATIC_PHP}/lib -L${H2O_PHP_STATIC_DEPS}/lib")
# io_uring is optional and its system library may be shared. Keep the first
# GNU distribution profile bounded to audited cached archives.
set(WITH_IO_URING OFF CACHE BOOL "Optional external io_uring dependency")
set(WITH_MRUBY OFF CACHE BOOL "Optional external mruby dependency")

# GNU single-executable profile uses the pinned embedded trust fallback.
# Explicit SSL_CERT_FILE/SSL_CERT_DIR retain precedence in the runtime.
set(H2O_PHP_PREFER_EMBEDDED_CA ON CACHE BOOL "Prefer the reviewed embedded CA asset")
