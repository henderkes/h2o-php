# Discover the installed embed-capable ZTS PHP ABI; never build a replacement PHP.
include(FindPackageHandleStandardArgs)
include(CheckCSourceCompiles)
include(CheckCSourceRuns)
include(CMakePushCheckState)

find_program(PHPZTS_CONFIG NAMES php-config-zts DOC "php-config for the installed ZTS PHP")
if(PHPZTS_CONFIG)
  foreach(_option version includes ldflags libs prefix extension-dir lib-dir lib-embed)
    string(REPLACE "-" "_" _key "${_option}")
    string(TOUPPER "${_key}" _key)
    execute_process(COMMAND "${PHPZTS_CONFIG}" "--${_option}"
      OUTPUT_VARIABLE PHPZTS_${_key} OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _php_config_result)
    if(NOT _php_config_result EQUAL 0 AND NOT _option MATCHES "^(lib-dir|lib-embed)$")
      message(FATAL_ERROR "${PHPZTS_CONFIG} --${_option} failed")
    endif()
  endforeach()
  separate_arguments(_php_include_flags UNIX_COMMAND "${PHPZTS_INCLUDES}")
  set(PHPZTS_INCLUDE_DIRS)
  foreach(_flag IN LISTS _php_include_flags)
    if(_flag MATCHES "^-I(.+)")
      list(APPEND PHPZTS_INCLUDE_DIRS "${CMAKE_MATCH_1}")
    else()
      message(FATAL_ERROR "Unexpected PHP include flag: ${_flag}")
    endif()
  endforeach()
  foreach(_php_include IN LISTS PHPZTS_INCLUDE_DIRS)
    if(EXISTS "${_php_include}/Zend/zend_modules.h")
      file(STRINGS "${_php_include}/Zend/zend_modules.h" _php_api_line REGEX "^#define ZEND_MODULE_API_NO [0-9]+")
      string(REGEX MATCH "[0-9]+" PHPZTS_API "${_php_api_line}")
      break()
    endif()
  endforeach()
  get_filename_component(_php_embed_name "${PHPZTS_LIB_EMBED}" NAME_WE)
  string(REGEX REPLACE "^lib" "" _php_embed_name "${_php_embed_name}")
  find_library(PHPZTS_LIBRARY NAMES php-zts "${_php_embed_name}"
    HINTS "${PHPZTS_LIB_DIR}" "${PHPZTS_PREFIX}/lib64" "${PHPZTS_PREFIX}/lib"
    DOC "Actual embed-capable libphp-zts shared library")
  separate_arguments(PHPZTS_DEPENDENCY_FLAGS UNIX_COMMAND "${PHPZTS_LDFLAGS} ${PHPZTS_LIBS}")
endif()

if(PHPZTS_INCLUDE_DIRS AND PHPZTS_LIBRARY)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_INCLUDES ${PHPZTS_INCLUDE_DIRS})
  set(CMAKE_REQUIRED_LIBRARIES "${PHPZTS_LIBRARY}" ${PHPZTS_DEPENDENCY_FLAGS})
  check_c_source_compiles("#include <php.h>
#include <SAPI.h>
#include <php_main.h>
#include <TSRM.h>
#ifndef ZTS
#error This PHP was built without ZTS
#endif
#if PHP_VERSION_ID < 80500 || PHP_VERSION_ID >= 80600
#error The worker lifecycle port requires matching PHP 8.5 headers and runtime
#endif
int main(void) {
  void *volatile p[] = {(void *)php_module_startup, (void *)php_request_startup,
                       (void *)sapi_startup, (void *)tsrm_startup};
  return p[0] == 0;
}" PHPZTS_ABI_LINKS)
  if(PHPZTS_ABI_LINKS AND PHPZTS_VERSION VERSION_GREATER_EQUAL "8.5" AND NOT CMAKE_CROSSCOMPILING)
    check_c_source_runs("#include <php.h>
#include <php_main.h>
int main(void) { return php_version_id() != PHP_VERSION_ID; }
" PHPZTS_RUNTIME_VERSION_MATCHES)
    if(NOT PHPZTS_RUNTIME_VERSION_MATCHES)
      message(FATAL_ERROR "The libphp runtime version differs from the installed PHP headers, or its loader dependencies cannot be resolved")
    endif()
  endif()
  cmake_pop_check_state()
endif()

find_package_handle_standard_args(PHPZTS REQUIRED_VARS
  PHPZTS_CONFIG PHPZTS_LIBRARY PHPZTS_INCLUDE_DIRS PHPZTS_ABI_LINKS
  VERSION_VAR PHPZTS_VERSION
  FAIL_MESSAGE "An installed embed-capable ZTS PHP 8.5 runtime is required. Set PHPZTS_CONFIG and PHPZTS_LIBRARY to matching headers/library. Inspect CMakeConfigureLog.yaml for ABI errors.")

if(PHPZTS_FOUND AND NOT TARGET PHPZTS::PHPZTS)
  add_library(PHPZTS::PHPZTS SHARED IMPORTED)
  set_target_properties(PHPZTS::PHPZTS PROPERTIES
    IMPORTED_LOCATION "${PHPZTS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PHPZTS_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${PHPZTS_DEPENDENCY_FLAGS}")
  message(STATUS "Embedding PHP ${PHPZTS_VERSION}: ${PHPZTS_LIBRARY}")
  message(STATUS "PHP extensions: ${PHPZTS_EXTENSION_DIR}")
endif()
mark_as_advanced(PHPZTS_LIBRARY PHPZTS_ABI_LINKS)
