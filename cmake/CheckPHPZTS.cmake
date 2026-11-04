# Used at configure time and before each build. Package upgrades can install
# backdated headers, so Make's timestamp dependencies alone cannot detect them.
function(phpzts_fingerprint output)
  set(inputs "")
  foreach(path IN LISTS PHPZTS_ABI_FILES)
    if(NOT EXISTS "${path}")
      message(FATAL_ERROR "PHP ZTS dependency disappeared: ${path}. Rerun CMake for this build directory.")
    endif()
    file(REAL_PATH "${path}" resolved)
    file(SHA256 "${path}" digest)
    string(APPEND inputs "${path}:${resolved}:${digest}\n")
  endforeach()
  string(SHA256 fingerprint "${inputs}")
  set(${output} "${fingerprint}" PARENT_SCOPE)
endfunction()

if(DEFINED PHPZTS_EXPECTED_FINGERPRINT)
  phpzts_fingerprint(current)
  if(NOT current STREQUAL PHPZTS_EXPECTED_FINGERPRINT)
    message(FATAL_ERROR
      "The installed PHP ZTS headers/library changed since configuration. "
      "Rerun CMake for this build directory (e.g. cmake --preset release) before building.")
  endif()
endif()
