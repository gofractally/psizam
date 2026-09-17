cmake_minimum_required(VERSION 3.20)
project(psizam_repro_bench LANGUAGES C CXX)
find_package(psizam COMPONENTS LLVM CONFIG REQUIRED)
add_executable(repro-psizam psizam_runner.cpp)
target_link_libraries(repro-psizam PRIVATE psizam::psizam-exec psizam::psizam-llvm)
if(WASMTIME_C_API)
  add_executable(repro-boundary boundary.cpp)
  target_include_directories(repro-boundary PRIVATE "${WASMTIME_C_API}/include")
  target_link_libraries(repro-boundary PRIVATE psizam::psizam-exec psizam::psizam-llvm
    "${WASMTIME_C_API}/lib/libwasmtime.a")
endif()
if(APPLE)
  execute_process(COMMAND brew --prefix llvm OUTPUT_VARIABLE llvm_prefix
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  target_link_options(repro-psizam PRIVATE "-L${llvm_prefix}/lib/c++"
    "-Wl,-rpath,${llvm_prefix}/lib/c++")
  if(TARGET repro-boundary)
    target_link_options(repro-boundary PRIVATE "-L${llvm_prefix}/lib/c++"
      "-Wl,-rpath,${llvm_prefix}/lib/c++")
  endif()
endif()
