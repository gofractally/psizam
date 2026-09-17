# Caller defines call_test_target and call_test_wasm. Compile with the actual CLI.
if(PSIZAM_ENABLE_LLVM AND TARGET pzam-compile)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_call_arch aarch64)
  else()
    set(_call_arch x86_64)
  endif()
  foreach(_call_policy none gas)
    if(_call_policy STREQUAL gas)
      set(_call_meter on)
    else()
      set(_call_meter off)
    endif()
    set(_call_pzam "${CMAKE_CURRENT_BINARY_DIR}/call-${_call_policy}.pzam")
    add_custom_command(OUTPUT "${_call_pzam}"
      COMMAND $<TARGET_FILE:pzam-compile> --target=${_call_arch} --backend=llvm --opt-level=2
        --gas-metering=${_call_meter} -j1 -o "${_call_pzam}" "${call_test_wasm}"
      DEPENDS pzam-compile "${call_test_wasm}" VERBATIM)
    add_custom_target(${call_test_target}_${_call_policy}_aot DEPENDS "${_call_pzam}")
    add_dependencies(${call_test_target} ${call_test_target}_${_call_policy}_aot)
    add_test(NAME calls.llvm-aot.${_call_policy} COMMAND ${call_test_target}
      llvm-aot "${_call_pzam}" ${_call_policy} check)
    set_tests_properties(calls.llvm-aot.${_call_policy} PROPERTIES LABELS "execution;quick;calls" TIMEOUT 60)
  endforeach()
endif()
