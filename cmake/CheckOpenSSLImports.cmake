# The embed PHP library may export a bundled OpenSSL 3. Linking successfully
# therefore does not prove that removed APIs came from the selected OpenSSL 4.
execute_process(COMMAND "${NM}" -D --undefined-only "${BINARY}"
  RESULT_VARIABLE result OUTPUT_VARIABLE imports ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot inspect OpenSSL imports: ${error}")
endif()
if(imports MATCHES "[ \t]ENGINE_[A-Za-z0-9_]+")
  message(FATAL_ERROR "OpenSSL 4 build imports removed ENGINE APIs; refusing a binary that could bind them from libphp")
endif()
