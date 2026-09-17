find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
find_program(PSIZAM_WAT2WASM wat2wasm REQUIRED)
set(PSIZAM_VALIDATION_COMPILE_JOBS 4 CACHE STRING "Compiler workers for validation artifacts (not benchmarks)")

add_executable(psizam_execution_driver execution_driver.cpp)
target_link_libraries(psizam_execution_driver PRIVATE psizam psizam-exec)
if(PSIZAM_ENABLE_LLVM)
   target_link_libraries(psizam_execution_driver PRIVATE psizam-llvm)
   add_executable(psizam_llvm_gas_tests llvm_gas.cpp)
   target_link_libraries(psizam_llvm_gas_tests PRIVATE psizam-llvm psizam-exec Catch2::Catch2)
   target_compile_definitions(psizam_llvm_gas_tests PRIVATE CATCH_CONFIG_NO_POSIX_SIGNALS
      LLVM_GAS_WASM="${CMAKE_CURRENT_BINARY_DIR}/llvm-gas.wasm")
   add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/llvm-gas.wasm"
      COMMAND "${PSIZAM_WAT2WASM}" "${CMAKE_CURRENT_SOURCE_DIR}/llvm-gas.wat"
         -o "${CMAKE_CURRENT_BINARY_DIR}/llvm-gas.wasm"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/llvm-gas.wat" VERBATIM)
   add_custom_target(psizam_llvm_gas_fixture DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/llvm-gas.wasm")
   add_dependencies(psizam_llvm_gas_tests psizam_llvm_gas_fixture)
   add_dependencies(psizam_execution_driver psizam_llvm_gas_tests)
   add_test(NAME execution.llvm-gas COMMAND psizam_llvm_gas_tests)
   set_tests_properties(execution.llvm-gas PROPERTIES LABELS "execution;quick;resource" TIMEOUT 60)
endif()

add_executable(psizam_regalloc_tests regalloc.cpp)
target_link_libraries(psizam_regalloc_tests PRIVATE psizam psizam-exec)
add_test(NAME execution.regalloc COMMAND psizam_regalloc_tests)
set_tests_properties(execution.regalloc PROPERTIES LABELS "execution;quick" TIMEOUT 30)

set(_fixtures integer normal-return scalar-f32 scalar-f64 scalar-conversions simd-f32 simd-f64 simd-mixed-call simd-relaxed-fma)
set(_wasms)
foreach(_fixture IN LISTS _fixtures)
   set(_wasm "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm")
   add_custom_command(OUTPUT "${_wasm}"
      COMMAND "${PSIZAM_WAT2WASM}" --enable-tail-call --enable-relaxed-simd "${CMAKE_CURRENT_SOURCE_DIR}/${_fixture}.wat" -o "${_wasm}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_fixture}.wat" VERBATIM)
   list(APPEND _wasms "${_wasm}")
endforeach()
add_custom_target(psizam_execution_fixtures ALL DEPENDS ${_wasms})
add_dependencies(psizam_execution_driver psizam_execution_fixtures)

set(_common "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/execute.py")
add_test(NAME execution.harness COMMAND "${Python3_EXECUTABLE}"
   "${CMAKE_CURRENT_SOURCE_DIR}/test_validation.py")
set_tests_properties(execution.harness PROPERTIES LABELS "execution;quick" TIMEOUT 15)
set(_execution_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64|x86_64)$")
   set(_execution_arch "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(_execution_arch MATCHES "^(arm64|aarch64)$")
   set(_execution_arch aarch64)
elseif(_execution_arch MATCHES "^(x86_64|AMD64)$")
   set(_execution_arch x86_64)
endif()
set(_tools --arch "${_execution_arch}" --driver $<TARGET_FILE:psizam_execution_driver>
   --compiler $<TARGET_FILE:pzam-compile> --runner $<TARGET_FILE:pzam-run>
   --compile-jobs "${PSIZAM_VALIDATION_COMPILE_JOBS}")
set(_modes interpreter-softfloat jit1-fast jit1-softfloat jit2-fast jit2-softfloat jit1-cli jit1-api jit2-cli jit2-api)
if(PSIZAM_ENABLE_LLVM)
   list(APPEND _modes llvm-fast llvm-o2-cli llvm-softfloat-cli llvm-o2-api)
endif()
foreach(_mode IN LISTS _modes)
   foreach(_fixture IN LISTS _fixtures)
      set(_name "execution.${_mode}.${_fixture}")
      set(_result_args)
      if(_fixture STREQUAL "normal-return")
         set(_result_args --return-marker "normal return")
      endif()
      add_test(NAME "${_name}" COMMAND ${_common} small ${_tools}
         --mode "${_mode}" ${_result_args} --wasm "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm"
         --work "${CMAKE_CURRENT_BINARY_DIR}/results/${_name}")
      set_tests_properties("${_name}" PROPERTIES LABELS "execution;quick" TIMEOUT 240)
   endforeach()
endforeach()

# Large modules exercise distant trap handlers, not just small spec modules.
set(_far_wasms)
foreach(_fixture far-stack far-div32 far-div64)
   list(APPEND _far_wasms "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm")
endforeach()
add_custom_command(OUTPUT ${_far_wasms}
   COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate-far-traps.py"
      "${CMAKE_CURRENT_BINARY_DIR}" "${PSIZAM_WAT2WASM}"
   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate-far-traps.py" VERBATIM)
add_custom_target(psizam_far_trap_fixtures DEPENDS ${_far_wasms})
add_dependencies(psizam_execution_driver psizam_far_trap_fixtures)
foreach(_mode jit1-fast jit1-softfloat jit2-fast jit2-softfloat jit1-cli jit1-api jit2-cli jit2-api)
   foreach(_fixture far-stack far-div32 far-div64)
      set(_name "execution.${_mode}.${_fixture}")
      if(_fixture STREQUAL "far-stack")
         set(_trap "stack overflow")
      elseif(_execution_arch STREQUAL "x86_64")
         set(_trap "integer divide by zero")
      else()
         set(_trap "floating point error")
      endif()
      add_test(NAME "${_name}" COMMAND ${_common} small ${_tools}
         --mode "${_mode}" --wasm "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm"
         --trap "${_trap}" --work "${CMAKE_CURRENT_BINARY_DIR}/results/${_name}")
      set_tests_properties("${_name}" PROPERTIES LABELS "execution;quick" TIMEOUT 240)
   endforeach()
endforeach()

if(PSIZAM_ENABLE_COMPILER_TESTS)
   if(NOT IS_DIRECTORY "${PSIZAM_COMPILER_TEST_DATA}")
      message(FATAL_ERROR "PSIZAM_ENABLE_COMPILER_TESTS requires PSIZAM_COMPILER_TEST_DATA; see docs/validation.md")
   endif()
   add_test(NAME compiler.fixtures COMMAND ${_common} fixtures --data "${PSIZAM_COMPILER_TEST_DATA}"
      --work "${CMAKE_CURRENT_BINARY_DIR}/results/compiler.fixtures")
   set_tests_properties(compiler.fixtures PROPERTIES LABELS "execution;compiler" TIMEOUT 30
      FIXTURES_SETUP compiler_data)
   set(_compiler_modes jit1-fast jit1-softfloat jit2-fast jit2-softfloat jit1-cli jit1-api jit2-cli jit2-api)
   if(PSIZAM_ENABLE_LLVM)
      list(APPEND _compiler_modes llvm-o2-cli)
   endif()
   foreach(_mode IN LISTS _compiler_modes)
      set(_artifact "${CMAKE_CURRENT_BINARY_DIR}/clang-${_mode}.pzam")
      set(_required compiler_data)
      if(_mode MATCHES "-(cli|api)$")
         set(_setup "compiler.prepare.${_mode}")
         add_test(NAME "${_setup}" COMMAND ${_common} compile-clang ${_tools} --mode "${_mode}"
            --data "${PSIZAM_COMPILER_TEST_DATA}" --artifact "${_artifact}"
            --work "${CMAKE_CURRENT_BINARY_DIR}/results/${_setup}")
         set_tests_properties("${_setup}" PROPERTIES LABELS "execution;compiler" TIMEOUT 3660
            FIXTURES_REQUIRED compiler_data FIXTURES_SETUP "clang_${_mode}")
         list(APPEND _required "clang_${_mode}")
      endif()
      foreach(_input SipHash.cpp APInt.cpp)
         set(_name "compiler.${_mode}.${_input}")
         add_test(NAME "${_name}" COMMAND ${_common} clang ${_tools} --mode "${_mode}"
            --input "${_input}" --data "${PSIZAM_COMPILER_TEST_DATA}" --artifact "${_artifact}"
            --work "${CMAKE_CURRENT_BINARY_DIR}/results/${_name}")
         set_tests_properties("${_name}" PROPERTIES LABELS "execution;compiler" TIMEOUT 240
            FIXTURES_REQUIRED "${_required}")
      endforeach()
   endforeach()
endif()

add_custom_target(check-execution
   COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --no-tests=error -L execution
   DEPENDS psizam_execution_driver psizam_regalloc_tests pzam-compile pzam-run
   WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" USES_TERMINAL)

# Exercise exactly the resource controls used by the benchmark, including
# nontermination before _start and genuine absence of gas accounting.
add_executable(psizam_resource_runner ../../benchmarks/repro/psizam_runner.cpp)
target_link_libraries(psizam_resource_runner PRIVATE psizam psizam-exec)
if(PSIZAM_ENABLE_LLVM)
   target_link_libraries(psizam_resource_runner PRIVATE psizam-llvm)
endif()
add_test(NAME execution.resource-limits
   COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/resource_limits.py"
      --runner $<TARGET_FILE:psizam_resource_runner> --compiler $<TARGET_FILE:pzam-compile>
      --pzam-run $<TARGET_FILE:pzam-run> --wat2wasm "${PSIZAM_WAT2WASM}"
      --arch "${_execution_arch}" --llvm "${PSIZAM_ENABLE_LLVM}")
set_tests_properties(execution.resource-limits PROPERTIES LABELS "execution;quick;resource" TIMEOUT 180)
add_dependencies(check-execution psizam_resource_runner)

add_dependencies(psizam_execution_driver psizam_resource_runner psizam_pzam_compile_tests)

if(PSIZAM_ENABLE_INSTALL)
   add_test(NAME execution.installed-runtime
      COMMAND "${CMAKE_COMMAND}" "-DBUILD=${CMAKE_BINARY_DIR}"
         "-DSOURCE=${PSIZAM_SOURCE_DIR}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/installed-runtime"
         "-DCC=${CMAKE_C_COMPILER}" "-DCXX=${CMAKE_CXX_COMPILER}"
         "-DLLVM_DIR=${LLVM_DIR}" "-DHAVE_LLVM=${PSIZAM_ENABLE_LLVM}"
         "-DWAT2WASM=${PSIZAM_WAT2WASM}"
         "-DCOMPILER=$<TARGET_FILE:pzam-compile>" "-DARCH=${_execution_arch}"
         -P "${CMAKE_CURRENT_SOURCE_DIR}/installed_runtime.cmake")
   set_tests_properties(execution.installed-runtime PROPERTIES
      LABELS "execution;release;runtime" TIMEOUT 300 RUN_SERIAL TRUE)
endif()

add_test(NAME execution.jit1-saved-roundtrip
   COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/jit1_saved.py"
      --compiler $<TARGET_FILE:pzam-compile> --runner $<TARGET_FILE:pzam-run>
      --fixtures "${CMAKE_CURRENT_BINARY_DIR}" --arch "${_execution_arch}")
set_tests_properties(execution.jit1-saved-roundtrip PROPERTIES
   LABELS "execution;quick;pzam" TIMEOUT 180)
