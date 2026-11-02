cmake_minimum_required(VERSION 3.20)
project(h2o_php VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Threads REQUIRED)
find_package(PHPZTS REQUIRED)
find_package(OpenSSL REQUIRED)

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

file(GLOB_RECURSE H2O_PHP_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/h2o/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/config/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/sapi/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/php/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/observability/*.c")
target_sources(h2o PRIVATE ${H2O_PHP_SOURCES})
target_include_directories(h2o PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(h2o PRIVATE H2O_WITH_PHP=1 _GNU_SOURCE)
target_link_libraries(h2o PHPZTS::PHPZTS Threads::Threads)
set_target_properties(h2o PROPERTIES OUTPUT_NAME h2o-php
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_custom_target(native-server DEPENDS h2o)

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/upstream/h2o"
  OUTPUT_VARIABLE H2O_UPSTREAM_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE H2O_PHP_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE)
target_compile_definitions(h2o PRIVATE
  H2O_PHP_H2O_REVISION="${H2O_UPSTREAM_REVISION}"
  H2O_PHP_REVISION="${H2O_PHP_REVISION}"
  H2O_PHP_PHP_VERSION="${PHPZTS_VERSION}"
  H2O_PHP_PHP_ABI=${PHPZTS_API})
