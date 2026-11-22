find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(PSIZAM_WAT2WASM wat2wasm REQUIRED)

set(_parity_wat "${CMAKE_CURRENT_BINARY_DIR}/float-parity.wat")
set(_parity_wasm "${CMAKE_CURRENT_BINARY_DIR}/float-parity.wasm")
set(_parity_names "${CMAKE_CURRENT_BINARY_DIR}/float_parity_ops.hpp")
add_custom_command(OUTPUT "${_parity_wat}" "${_parity_names}"
   COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/gen_float_parity.py"
           "${_parity_wat}" "${_parity_names}"
   DEPENDS gen_float_parity.py VERBATIM)
add_custom_command(OUTPUT "${_parity_wasm}"
   COMMAND "${PSIZAM_WAT2WASM}" --enable-relaxed-simd "${_parity_wat}" -o "${_parity_wasm}"
   DEPENDS "${_parity_wat}" VERBATIM)

add_executable(psizam_wasmtime_backend_tests
   wasmtime_backend_tests.cpp
   "${_parity_wasm}" "${_parity_names}")
target_include_directories(psizam_wasmtime_backend_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(psizam_wasmtime_backend_tests PRIVATE
   psizam psizam-exec psizam-wasmtime Catch2::Catch2)
target_compile_definitions(psizam_wasmtime_backend_tests PRIVATE CATCH_CONFIG_NO_POSIX_SIGNALS
   FLOAT_PARITY_WASM="${_parity_wasm}")
add_test(NAME execution.wasmtime-backend COMMAND psizam_wasmtime_backend_tests)
set_tests_properties(execution.wasmtime-backend PROPERTIES LABELS "execution;quick;wasmtime" TIMEOUT 300)

# Runs module files on psizam's JIT2 and on wasmtime and compares every call.
# Not a registered test: it needs a corpus (for example from `wasm-tools smith`).
add_executable(psizam_wasmtime_differential_fuzz differential_fuzz.cpp)
target_link_libraries(psizam_wasmtime_differential_fuzz PRIVATE psizam psizam-exec psizam-wasmtime)
