# psizam wasmtime backend.
#
# wasmtime/shim is a Rust static library exposing a C ABI over wasmtime. The
# wasmtime version is pinned exactly in shim/Cargo.toml (and Cargo.lock): it is
# part of what the engine computes. Any Rust toolchain new enough builds it.

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
   message(FATAL_ERROR "PSIZAM_ENABLE_WASMTIME is a native-only backend")
endif()

find_program(PSIZAM_CARGO cargo REQUIRED)
set(PSIZAM_WASMTIME_BUILD_ID "$ENV{PSIZAM_WASMTIME_BUILD_ID}" CACHE STRING
    "Exact Wasmtime source receipt for development path dependencies")

set(_shim_source "${CMAKE_CURRENT_SOURCE_DIR}/shim")
set(_shim_target "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(_shim_library "${_shim_target}/release/libpsizam_wasmtime_shim.a")
set(_shim_env "CARGO_TARGET_DIR=${_shim_target}")
if(PSIZAM_WASMTIME_BUILD_ID)
   list(APPEND _shim_env "PSIZAM_WASMTIME_BUILD_ID=${PSIZAM_WASMTIME_BUILD_ID}")
endif()

add_custom_command(
   OUTPUT "${_shim_library}"
   COMMAND "${CMAKE_COMMAND}" -E env ${_shim_env}
           "${PSIZAM_CARGO}" build --release --locked
   WORKING_DIRECTORY "${_shim_source}"
   DEPENDS "${_shim_source}/Cargo.toml" "${_shim_source}/Cargo.lock"
           "${_shim_source}/build.rs" "${_shim_source}/src/lib.rs"
           "${_shim_source}/src/stack_meter.rs"
   COMMENT "Building the wasmtime shim with cargo"
   USES_TERMINAL VERBATIM)
add_custom_target(psizam-wasmtime-shim-build DEPENDS "${_shim_library}")

add_library(psizam-wasmtime-shim STATIC IMPORTED GLOBAL)
set_target_properties(psizam-wasmtime-shim PROPERTIES IMPORTED_LOCATION "${_shim_library}")
add_dependencies(psizam-wasmtime-shim psizam-wasmtime-shim-build)

add_library(psizam-wasmtime INTERFACE)
# Relative to this file, so another project can add this directory directly.
target_include_directories(psizam-wasmtime INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_features(psizam-wasmtime INTERFACE cxx_std_20)
target_compile_definitions(psizam-wasmtime INTERFACE PSIZAM_ENABLE_WASMTIME_BACKEND)
target_link_libraries(psizam-wasmtime INTERFACE psizam-wasmtime-shim)
# System libraries the Rust standard library needs (rustc --print native-static-libs).
if(APPLE)
   target_link_libraries(psizam-wasmtime INTERFACE iconv)
else()
   find_package(Threads REQUIRED)
   target_link_libraries(psizam-wasmtime INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

if(PSIZAM_ENABLE_TESTS)
   add_subdirectory(tests)
endif()
