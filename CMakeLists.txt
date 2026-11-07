cmake_minimum_required(VERSION 3.20)
project(h2o_php VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Threads REQUIRED)
find_package(PHPZTS REQUIRED)
find_package(OpenSSL REQUIRED)
include(EmbedAssets)
hp_embed_assets("${CMAKE_CURRENT_SOURCE_DIR}/assets/ca-bundle.pem"
  "${CMAKE_CURRENT_BINARY_DIR}/h2o-php-embedded-assets.h")
option(H2O_PHP_PREFER_EMBEDDED_CA "Use the pinned bundled CA data before platform defaults (explicit user overrides still win)" OFF)
set(_cli_default OFF)
if(PHPZTS_VERSION VERSION_GREATER_EQUAL "8.6")
  set(_cli_default ON)
endif()
option(H2O_PHP_ENABLE_CLI "Enable the real PHP CLI SAPI (PHP 8.6+)" ${_cli_default})
if(H2O_PHP_ENABLE_CLI)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_INCLUDES ${PHPZTS_INCLUDE_DIRS})
  set(CMAKE_REQUIRED_LIBRARIES PHPZTS::PHPZTS)
  check_c_source_compiles("#include <php.h>
#include <php_main.h>
#include <sapi/cli/cli.h>
#if PHP_VERSION_ID < 80600
#error PHP CLI embedding requires PHP 8.6 or later
#endif
int main(void) { int (*volatile cli)(int,char**) = do_php_cli;
  void *volatile modules = (void *)&php_register_internal_extensions_func;
  return cli == 0 || modules == 0; }" H2O_PHP_CLI_LINKS)
  cmake_pop_check_state()
  if(NOT H2O_PHP_CLI_LINKS)
    message(FATAL_ERROR "CLI requested, but matching PHP 8.6 CLI headers/exported do_php_cli and module registration hook are unavailable")
  endif()
endif()

# The upstream standalone server remains the sole network/server implementation.
option(WITH_MRUBY "Build H2O mruby support" OFF)
option(WITH_DTRACE "Build H2O DTrace probes" OFF)
option(WITHOUT_LIBS "Skip installation of standalone libh2o" ON)
option(DISABLE_LIBUV "Use H2O's native evloop" ON)
option(WITH_CCACHE "Use compiler cache" OFF)
add_subdirectory(upstream/h2o h2o)

# Alma/RHEL's OpenSSL 3.5.8 package can ship an empty engine.h despite
# retaining the ENGINE ABI in libcrypto. Use the exact upstream header only
# when that installed header fails the declaration check.
include(CheckCSourceCompiles)
include(CheckCSourceRuns)
if(OPENSSL_VERSION VERSION_GREATER_EQUAL "4.0")
  # Never resolve removed ENGINE symbols from libphp's bundled older libcrypto.
  # OpenSSL 4 uses our neverbleed provider; validate against libcrypto alone.
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
  set(CMAKE_REQUIRED_LIBRARIES OpenSSL::Crypto)
  check_c_source_runs("#include <openssl/provider.h>
#include <openssl/opensslv.h>
#include <openssl/crypto.h>
int main(void) {
  if (OPENSSL_version_major() != OPENSSL_VERSION_MAJOR) return 1;
  OSSL_PROVIDER *p = OSSL_PROVIDER_load(NULL, \"default\");
  if (!p) return 2;
  OSSL_PROVIDER_unload(p); return 0;
}" H2O_PHP_OPENSSL_PROVIDER_WORKS)
  cmake_pop_check_state()
  if(NOT H2O_PHP_OPENSSL_PROVIDER_WORKS)
    message(FATAL_ERROR "OpenSSL provider headers/runtime do not match or the default provider cannot load")
  endif()
  message(STATUS "Using provider-based neverbleed with OpenSSL ${OPENSSL_VERSION}")
else()
  set(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
  check_c_source_compiles("#include <openssl/engine.h>
  int main(void) { return sizeof(&ENGINE_by_id) == 0; }" H2O_PHP_OPENSSL_ENGINE_DECLARED)
  unset(CMAKE_REQUIRED_INCLUDES)
  if(NOT H2O_PHP_OPENSSL_ENGINE_DECLARED)
    if(OPENSSL_VERSION VERSION_EQUAL "3.5.8")
      set(CMAKE_REQUIRED_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compat" ${OPENSSL_INCLUDE_DIR})
      set(CMAKE_REQUIRED_LIBRARIES OpenSSL::Crypto)
      check_c_source_runs("#include <openssl/engine.h>
  int main(void) { ENGINE *e = ENGINE_new(); if (!e) return 1; ENGINE_free(e); return 0; }" H2O_PHP_OPENSSL_ENGINE_ABI_WORKS)
      unset(CMAKE_REQUIRED_INCLUDES)
      unset(CMAKE_REQUIRED_LIBRARIES)
      if(NOT H2O_PHP_OPENSSL_ENGINE_ABI_WORKS)
        message(FATAL_ERROR "Installed libcrypto does not provide functional ENGINE ABI required by H2O neverbleed")
      endif()
      message(WARNING "Installed OpenSSL engine.h lacks ENGINE declarations. Using bundled official OpenSSL 3.5.8 header; shared system libcrypto remains linked.")
      target_include_directories(h2o BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compat")
    else()
      message(FATAL_ERROR "H2O neverbleed requires OpenSSL ENGINE declarations. Repair the OpenSSL development headers for ${OPENSSL_VERSION}.")
    endif()
  endif()
endif()

file(GLOB_RECURSE H2O_PHP_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/h2o/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/config/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/sapi/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/php/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/observability/*.c")
target_sources(h2o PRIVATE ${H2O_PHP_SOURCES})
foreach(source IN LISTS H2O_PHP_SOURCES)
  if(source MATCHES "/src/sapi/")
    set_property(SOURCE "${source}" TARGET_DIRECTORY h2o APPEND PROPERTY
      OBJECT_DEPENDS "${PHPZTS_ABI_STAMP}")
  endif()
endforeach()
target_include_directories(h2o PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(h2o PRIVATE H2O_WITH_PHP=1 _GNU_SOURCE)
target_compile_definitions(h2o PRIVATE H2O_PHP_CLI_AVAILABLE=$<BOOL:${H2O_PHP_ENABLE_CLI}>)
target_compile_definitions(h2o PRIVATE H2O_PHP_PREFER_EMBEDDED_CA=$<BOOL:${H2O_PHP_PREFER_EMBEDDED_CA}>)
target_link_libraries(h2o PHPZTS::PHPZTS Threads::Threads)
set_target_properties(h2o PROPERTIES OUTPUT_NAME h2o-php
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(OPENSSL_VERSION VERSION_GREATER_EQUAL "4.0" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_custom_target(h2o-php-openssl-import-check
    COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DBINARY=$<TARGET_FILE:h2o>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckOpenSSLImports.cmake"
    DEPENDS h2o VERBATIM)
endif()
add_custom_target(native-server DEPENDS h2o)
if(TARGET h2o-php-openssl-import-check)
  add_dependencies(native-server h2o-php-openssl-import-check)
endif()

add_custom_target(h2o-php-php-abi-check
  COMMAND "${CMAKE_COMMAND}"
    "-DPHPZTS_ABI_FILES=${PHPZTS_ABI_FILES}"
    "-DPHPZTS_EXPECTED_FINGERPRINT=${PHPZTS_FINGERPRINT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPHPZTS.cmake"
  VERBATIM)
add_dependencies(h2o h2o-php-php-abi-check)

# Refresh the version when sources or HEAD change, even without reconfiguring.
# The generator writes only changed content, so an unchanged build does not
# recompile main.c. A content digest also distinguishes uncommitted candidates.
set(H2O_PHP_IDENTITY_HEADER "${CMAKE_CURRENT_BINARY_DIR}/h2o-php-build-identity.h")
add_custom_target(h2o-php-build-identity
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT_HEADER=${H2O_PHP_IDENTITY_HEADER}"
    "-DPHP_VERSION=${PHPZTS_VERSION}"
    "-DPHP_ABI=${PHPZTS_API}"
    "-DBUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DC_COMPILER=${CMAKE_C_COMPILER}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildIdentity.cmake"
  BYPRODUCTS "${H2O_PHP_IDENTITY_HEADER}" "${CMAKE_CURRENT_BINARY_DIR}/h2o-php-build-identity.txt"
  VERBATIM)
add_dependencies(h2o h2o-php-build-identity)
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upstream/h2o/src/main.c"
  TARGET_DIRECTORY h2o APPEND PROPERTY COMPILE_OPTIONS
  "-include" "${H2O_PHP_IDENTITY_HEADER}")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/h2o-php-build-identity.txt"
  DESTINATION share/doc/h2o-php)
install(PROGRAMS scripts/check-install.sh DESTINATION share/h2o-php)
install(FILES examples/h2o-php.service DESTINATION share/doc/h2o-php)

install(FILES upstream/h2o/LICENSE DESTINATION share/licenses/h2o-php RENAME H2O-MIT.txt)
install(FILES src/php/LICENSE.frankenphp DESTINATION share/licenses/h2o-php RENAME FrankenPHP-MIT.txt)
install(FILES cmake/compat/LICENSE.OpenSSL DESTINATION share/licenses/h2o-php RENAME OpenSSL-Apache-2.0.txt)
install(FILES assets/LICENSE.MPL-2.0 DESTINATION share/licenses/h2o-php RENAME Mozilla-CA-MPL-2.0.txt)
install(FILES assets/README.md DESTINATION share/licenses/h2o-php RENAME Mozilla-CA-PROVENANCE.md)
install(FILES upstream/php-src/LICENSE DESTINATION share/licenses/h2o-php RENAME PHP-3.01.txt)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/php-src-8.6/LICENSE")
  install(FILES upstream/php-src-8.6/LICENSE DESTINATION share/licenses/h2o-php RENAME PHP-8.6-BSD-3-Clause.txt)
endif()
file(GLOB_RECURSE H2O_DEPENDENCY_NOTICES
  "${CMAKE_CURRENT_SOURCE_DIR}/upstream/h2o/deps/*LICENSE*"
  "${CMAKE_CURRENT_SOURCE_DIR}/upstream/h2o/deps/*COPYING*")
foreach(notice IN LISTS H2O_DEPENDENCY_NOTICES)
  get_filename_component(notice_directory "${notice}" DIRECTORY)
  file(RELATIVE_PATH notice_directory "${CMAKE_CURRENT_SOURCE_DIR}/upstream/h2o/deps" "${notice_directory}")
  install(FILES "${notice}" DESTINATION "share/licenses/h2o-php/H2O-dependencies/${notice_directory}")
endforeach()
