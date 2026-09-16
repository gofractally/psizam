# Same WASI ABI, sysroot and build flags as stage 0; only compiler execution
# changes. Native CMake/Ninja/tablegen/ar remain build orchestration tools.
include(${CMAKE_CURRENT_LIST_DIR}/wasi-llvm-toolchain.cmake)
if(NOT DEFINED ENV{PSIZAM_BOOTSTRAP_DRIVER_DIR})
   message(FATAL_ERROR "PSIZAM_BOOTSTRAP_DRIVER_DIR must contain clang and clang++ wrappers")
endif()
set(CMAKE_C_COMPILER "$ENV{PSIZAM_BOOTSTRAP_DRIVER_DIR}/clang")
set(CMAKE_CXX_COMPILER "$ENV{PSIZAM_BOOTSTRAP_DRIVER_DIR}/clang++")
