include_guard(GLOBAL)
find_package(Boost REQUIRED)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
  find_package(Threads REQUIRED)
endif()
if(NOT TARGET psio)
  if(FETCHCONTENT_SOURCE_DIR_PSIO)
    # Explicit source dependency for co-development; no parent-tree assumptions.
    add_subdirectory("${FETCHCONTENT_SOURCE_DIR_PSIO}" "${PSIZAM_BINARY_DIR}/external/psio")
  else()
    find_package(psio 0.1 CONFIG REQUIRED)
    add_library(psio ALIAS psio::psio)
  endif()
endif()
if(NOT TARGET psio1)
  add_subdirectory(${PSIZAM_SOURCE_DIR}/external/psio1 ${PSIZAM_BINARY_DIR}/external/psio1)
endif()
if(NOT TARGET wasi-cpp)
  add_library(wasi-cpp INTERFACE)
  target_include_directories(wasi-cpp INTERFACE
    $<BUILD_INTERFACE:${PSIZAM_SOURCE_DIR}/external/wasi/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(wasi-cpp INTERFACE psio1)
  target_compile_features(wasi-cpp INTERFACE cxx_std_23)
endif()
# Homebrew LLVM's headers and runtime must agree when LLVM is linked on macOS.
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  execute_process(COMMAND brew --prefix llvm OUTPUT_VARIABLE _psizam_llvm
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(EXISTS "${_psizam_llvm}/lib/c++/libc++.dylib")
    add_link_options(-stdlib=libc++ "-L${_psizam_llvm}/lib/c++"
      "-Wl,-rpath,${_psizam_llvm}/lib/c++")
  endif()
endif()
