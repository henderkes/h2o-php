# File capabilities require CAP_SETFCAP; linking libcap cannot grant them.
# Never prompt for authentication or fail an otherwise successful build.
if(NOT DEFINED BINARY OR NOT EXISTS "${BINARY}")
  message(FATAL_ERROR "SetCapabilities needs an existing BINARY")
endif()
if(NOT SETCAP OR NOT EXISTS "${SETCAP}")
  message(STATUS "h2o-php: setcap unavailable; install libcap tools to enable ports 80/443 without root")
  return()
endif()

if(GETCAP AND EXISTS "${GETCAP}")
  execute_process(COMMAND "${GETCAP}" "${BINARY}"
    OUTPUT_VARIABLE capabilities ERROR_QUIET TIMEOUT 5)
  if(capabilities MATCHES "[ \t]cap_net_bind_service=ep([ \t\r\n]|$)")
    return()
  endif()
endif()

execute_process(COMMAND "${SETCAP}" cap_net_bind_service=+ep "${BINARY}"
  RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET TIMEOUT 5)
if(NOT result STREQUAL "0" AND SUDO AND EXISTS "${SUDO}")
  execute_process(COMMAND "${SUDO}" -n "${SETCAP}" cap_net_bind_service=+ep "${BINARY}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET TIMEOUT 5)
endif()
if(result STREQUAL "0")
  message(STATUS "h2o-php: enabled CAP_NET_BIND_SERVICE for ports 80/443")
else()
  message(STATUS "h2o-php: could not grant CAP_NET_BIND_SERVICE automatically; run: sudo \"${SETCAP}\" cap_net_bind_service=+ep \"${BINARY}\"")
endif()
