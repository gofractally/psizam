# ── psizam: WebAssembly virtual machine library ────────────────────────────────
# Works both as a standalone project and as a subdirectory of a parent project.

# Standalone guard: only set project-level settings when building psizam alone
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
   cmake_minimum_required(VERSION 3.20)
   project(psizam VERSION 1.0.0 LANGUAGES C CXX)
   set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
   set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
   set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
   set(CMAKE_CXX_STANDARD 23)
   set(CMAKE_CXX_EXTENSIONS ON)
   set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
   set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
   find_package(Threads REQUIRED)

   find_program(CCACHE_PROGRAM ccache)
   if(NOT CCACHE_PROGRAM)
      find_program(CCACHE_PROGRAM sccache)
   endif()
   if(CCACHE_PROGRAM)
      message(STATUS "Using compiler cache: ${CCACHE_PROGRAM}")
      set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
      set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
   endif()
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
   set(PSIZAM_IS_TOPLEVEL ON)
else()
   set(PSIZAM_IS_TOPLEVEL OFF)
endif()

# ── Options ────────────────────────────────────────────────────────────────────
include(CMakeDependentOption)

option(PSIZAM_ENABLE_SOFTFLOAT
   "Enable deterministic software floating point operations (required for runtime fp_mode selection; OFF pins the build to fp_mode::fast)" ON)

if(NOT PSIZAM_ENABLE_SOFTFLOAT)
   message(WARNING
      "PSIZAM_ENABLE_SOFTFLOAT=OFF disables softfloat. The interpreter requires "
      "softfloat symbols to compile (runtime fp_mode dispatches through them), "
      "so this configuration does not currently build. Leave ON unless you are "
      "also removing the interpreter from the build.")
endif()
option(PSIZAM_FULL_DEBUG
   "Enable stack dumping and instruction tracing" OFF)
option(PSIZAM_ENABLE_INSTALL
   "Enable install rules for psizam" ${PSIZAM_IS_TOPLEVEL})
option(PSIZAM_ENABLE_TOOLS
   "Build psizam CLI tools" ${PSIZAM_IS_TOPLEVEL})
option(PSIZAM_ENABLE_TESTS
   "Build psizam unit tests and spec tests" OFF)
option(PSIZAM_ENABLE_COMPILER_TESTS
   "Require pinned Clang workloads in the execution validation matrix" OFF)
set(PSIZAM_COMPILER_TEST_DATA "" CACHE PATH "Directory with pinned clang.wasm and frozen LLVM translation units")
if(PSIZAM_ENABLE_COMPILER_TESTS AND NOT PSIZAM_ENABLE_TESTS)
   message(FATAL_ERROR "PSIZAM_ENABLE_COMPILER_TESTS requires PSIZAM_ENABLE_TESTS")
endif()
option(PSIZAM_ENABLE_BENCHMARKS
   "Build comparative benchmark suite" OFF)
option(PSIZAM_ENABLE_EXAMPLES
   "Build psizam examples (requires wasi-sdk)" OFF)

cmake_dependent_option(PSIZAM_ENABLE_SPEC_TESTS
   "Enable wasm spec tests" ON "PSIZAM_ENABLE_TESTS" ON)
cmake_dependent_option(PSIZAM_ENABLE_FUZZ_TESTS
   "Enable fuzz testing" OFF "PSIZAM_ENABLE_TESTS" ON)
option(PSIZAM_ENABLE_WASM3
   "Build wasm3 reference interpreter for differential fuzzing" OFF)

# Dependencies are explicit and available without a surrounding psiserve tree.
set(PSIZAM_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(PSIZAM_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
include(cmake/PsizamDependencies.cmake)
include(cmake/PsiWasmModule.cmake)

# ── Build utilities (sanitizers, profiling) ────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/modules")
include(PsiZamBuildUtils)

# ── Softfloat dependency ──────────────────────────────────────────────────────
if(PSIZAM_ENABLE_SOFTFLOAT AND NOT TARGET softfloat)
   add_subdirectory(external)
endif()

# ── The psizam INTERFACE library ──────────────────────────────────────────────
add_library(psizam INTERFACE)
add_library(psizam::psizam ALIAS psizam)

target_include_directories(psizam INTERFACE
   $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
   $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/softfloat/source/include>
   $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/ucc/include>
   $<INSTALL_INTERFACE:include>
)

target_compile_options(psizam INTERFACE
   $<$<COMPILE_LANGUAGE:CXX>:-Wno-register>
   $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fbracket-depth=1024>
)

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
   target_link_libraries(psizam INTERFACE psio)
else()
   target_link_libraries(psizam INTERFACE Threads::Threads psio)
endif()

# ucc::typed_int is vendored independently of the database.

if(PSIZAM_ENABLE_SOFTFLOAT)
   target_compile_definitions(psizam INTERFACE PSIZAM_SOFTFLOAT)
   target_link_libraries(psizam INTERFACE softfloat)
endif()

if(PSIZAM_FULL_DEBUG)
   target_compile_definitions(psizam INTERFACE PSIZAM_FULL_DEBUG)
endif()

# ── Per-mode per-arch compile targets (INTERFACE, header-only JIT backends) ───
# These targets document the compile dependency for each backend × architecture.
# JIT backends are entirely header-only so no source files are compiled.

add_library(psizam-jit1-x86 INTERFACE)
target_link_libraries(psizam-jit1-x86 INTERFACE psizam)

add_library(psizam-jit1-arm INTERFACE)
target_link_libraries(psizam-jit1-arm INTERFACE psizam)

add_library(psizam-jit2-x86 INTERFACE)
target_link_libraries(psizam-jit2-x86 INTERFACE psizam)

add_library(psizam-jit2-arm INTERFACE)
target_link_libraries(psizam-jit2-arm INTERFACE psizam)

# ── Execution library (native only) ──────────────────────────────────────────
# Replaces psizam-runtime. Includes LLVM runtime helpers unconditionally so
# .pzam files compiled with LLVM can be executed without LLVM installed.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_library(psizam-exec STATIC
      src/runtime_helpers.cpp
      src/runtime.cpp
      src/call_on_stack.cpp
      src/llvm_runtime_helpers.cpp
      src/eh_runtime.cpp
   )
   target_link_libraries(psizam-exec PUBLIC psizam)
endif()

# ── Debug library (native only, always built) ────────────────────────────────
# DWARF parser, ELF synthesizer, and GDB/LLDB JIT registrar. Consumers opt
# in at the template level by instantiating backend<..., debug_instr_map>.
# Unused when no consumer selects debug_instr_map — linker DCE strips it.
# No CMake gate: host release builds retain the ability to symbolize WASM
# traces purely by template selection at the call site.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_library(psizam-debug STATIC
      src/debug/dwarf.cpp
   )
   target_link_libraries(psizam-debug PUBLIC psizam)
endif()

# ── LLVM Compile Backend (opt-in) ────────────────────────────────────────────
option(PSIZAM_ENABLE_LLVM "Build LLVM AOT/JIT backend" OFF)

if(PSIZAM_ENABLE_LLVM)
   # PSIZAM_LLVM_TARGETS: semicolon-separated list, e.g. "X86;AArch64" (default: both)
   if(NOT DEFINED PSIZAM_LLVM_TARGETS)
      set(PSIZAM_LLVM_TARGETS "X86;AArch64")
   endif()

   if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
      # WASI: use pre-built LLVM WASI libraries
      if(NOT LLVM_WASI_PREFIX)
         message(FATAL_ERROR "PSIZAM_ENABLE_LLVM=ON for WASI requires -DLLVM_WASI_PREFIX=<path>")
      endif()
      cmake_path(ABSOLUTE_PATH LLVM_WASI_PREFIX BASE_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE _LLVM_WASI_ABS)
      set(LLVM_DIR "${_LLVM_WASI_ABS}/lib/cmake/llvm")
      find_package(LLVM REQUIRED CONFIG NO_CMAKE_FIND_ROOT_PATH)
      message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (WASI) at ${LLVM_DIR}")
   else()
      find_package(LLVM REQUIRED CONFIG)
      message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
   endif()

   # Compile the AOT implementation once. Architecture facade targets share the
   # implementation and its selected LLVM libraries, avoiding duplicate symbols
   # whose target-initialization behavior depended on static-library link order.
   set(_LLVM_AOT_COMPONENTS core support instcombine scalaropts object)
   set(_LLVM_AOT_DEFINITIONS)
   foreach(_arch IN LISTS PSIZAM_LLVM_TARGETS)
      if(_arch STREQUAL "X86")
         list(APPEND _LLVM_AOT_COMPONENTS x86codegen x86asmparser x86desc x86info)
         list(APPEND _LLVM_AOT_DEFINITIONS PSIZAM_LLVM_TARGET_X86)
      elseif(_arch STREQUAL "AArch64")
         list(APPEND _LLVM_AOT_COMPONENTS aarch64codegen aarch64asmparser aarch64desc aarch64info)
         list(APPEND _LLVM_AOT_DEFINITIONS PSIZAM_LLVM_TARGET_AARCH64)
      else()
         message(FATAL_ERROR "Unsupported PSIZAM_LLVM_TARGETS entry: ${_arch}")
      endif()
   endforeach()
   llvm_map_components_to_libnames(_LLVM_AOT_LIBS ${_LLVM_AOT_COMPONENTS})
   add_library(psizam-llvm-aot STATIC src/llvm_ir_translator.cpp src/llvm_aot_compiler.cpp)
   target_link_libraries(psizam-llvm-aot PUBLIC psizam ${_LLVM_AOT_LIBS})
   target_include_directories(psizam-llvm-aot PUBLIC ${LLVM_INCLUDE_DIRS})
   target_compile_definitions(psizam-llvm-aot PUBLIC PSIZAM_ENABLE_LLVM_BACKEND
      PRIVATE ${_LLVM_AOT_DEFINITIONS})
   if(NOT LLVM_ENABLE_RTTI)
      target_compile_options(psizam-llvm-aot PRIVATE -fno-rtti)
   endif()
   if("X86" IN_LIST PSIZAM_LLVM_TARGETS)
      add_library(psizam-llvm-x86 INTERFACE)
      target_link_libraries(psizam-llvm-x86 INTERFACE psizam-llvm-aot)
   endif()
   if("AArch64" IN_LIST PSIZAM_LLVM_TARGETS)
      add_library(psizam-llvm-arm INTERFACE)
      target_link_libraries(psizam-llvm-arm INTERFACE psizam-llvm-aot)
   endif()

   # ── LLVM JIT runtime (native only) ────────────────────────────────────────
   # ORC JIT compiler + llvm_compile entry point — only needed for native
   # live-JIT, not for loading pre-compiled .pzam files.
   if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
      llvm_map_components_to_libnames(_LLVM_JIT_LIBS core support orcjit native)

      add_library(psizam-llvm-jit STATIC
         src/llvm_jit_compiler.cpp
         src/llvm_compile.cpp
         src/runtime_llvm.cpp
      )
      # llvm_compile.cpp uses llvm_ir_translator (in per-arch AOT targets).
      # Link the host-platform AOT target so the translator symbols resolve.
      if(TARGET psizam-llvm-x86 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
         target_link_libraries(psizam-llvm-jit PUBLIC psizam-llvm-x86)
      elseif(TARGET psizam-llvm-arm)
         target_link_libraries(psizam-llvm-jit PUBLIC psizam-llvm-arm)
      endif()
      target_link_libraries(psizam-llvm-jit PUBLIC psizam psizam-exec ${_LLVM_JIT_LIBS})
      target_include_directories(psizam-llvm-jit PUBLIC ${LLVM_INCLUDE_DIRS})
      target_compile_definitions(psizam-llvm-jit PUBLIC PSIZAM_ENABLE_LLVM_BACKEND)
      if(NOT LLVM_ENABLE_RTTI)
         target_compile_options(psizam-llvm-jit PRIVATE -fno-rtti)
      endif()
   endif()

   # ── psizam-llvm umbrella target ───────────────────────────────────────────
   # Links all enabled LLVM compile targets + JIT for backward compatibility
   # with existing consumers that just say target_link_libraries(... psizam-llvm).
   add_library(psizam-llvm INTERFACE)
   target_compile_definitions(psizam-llvm INTERFACE PSIZAM_ENABLE_LLVM_BACKEND)
   if(TARGET psizam-llvm-x86)
      target_link_libraries(psizam-llvm INTERFACE psizam-llvm-x86)
   endif()
   if(TARGET psizam-llvm-arm)
      target_link_libraries(psizam-llvm INTERFACE psizam-llvm-arm)
   endif()
   if(TARGET psizam-llvm-jit)
      target_link_libraries(psizam-llvm INTERFACE psizam-llvm-jit)
   endif()
endif()

# ── Tools (after LLVM so pzam-compile can link psizam-llvm) ───────────────────
if(PSIZAM_ENABLE_TOOLS OR (PSIZAM_ENABLE_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI"))
   add_subdirectory(tools)
endif()

# ── Benchmarks ─────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_BENCHMARKS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(benchmarks)
endif()

# ── Examples ───────────────────────────────────────────────────────────────────
# Tests depend on composition example WASM fixtures, so force examples on.
if(PSIZAM_ENABLE_TESTS AND NOT PSIZAM_ENABLE_EXAMPLES)
   set(PSIZAM_ENABLE_EXAMPLES ON)
endif()
if(PSIZAM_ENABLE_EXAMPLES AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(examples)
endif()

# ── Tests ──────────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   include(CTest)
   set(CATCH_BUILD_TESTING OFF CACHE BOOL "disable building Catch2 tests")
   set(CATCH_INSTALL_DOCS OFF CACHE BOOL "disable installing Catch2 docs")
   set(CATCH_INSTALL_HELPERS OFF CACHE BOOL "disable installing Catch2 helpers")
   add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/Catch2)
   include(external/Catch2/contrib/Catch.cmake)
   add_subdirectory(tests)
   if(PSIZAM_ENABLE_SPEC_TESTS)
      file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/wasms)
   endif()
endif()

# ── Install ────────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_INSTALL)
   include(GNUInstallDirs)
   include(CMakePackageConfigHelpers)
   set(_psizam_export_targets psizam psizam-jit1-x86 psizam-jit1-arm
      psizam-jit2-x86 psizam-jit2-arm wasi-cpp)
   foreach(_target psizam-exec psizam-debug softfloat)
      if(TARGET ${_target})
         list(APPEND _psizam_export_targets ${_target})
      endif()
   endforeach()
   install(TARGETS ${_psizam_export_targets} EXPORT psizamTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
   if(PSIZAM_ENABLE_LLVM)
      set(_psizam_llvm_exports psizam-llvm psizam-llvm-aot)
      foreach(_target psizam-llvm-x86 psizam-llvm-arm psizam-llvm-jit)
         if(TARGET ${_target})
            list(APPEND _psizam_llvm_exports ${_target})
         endif()
      endforeach()
      install(TARGETS ${_psizam_llvm_exports} EXPORT psizamLLVMTargets
         ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
      install(EXPORT psizamLLVMTargets NAMESPACE psizam::
         DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psizam)
   endif()
   install(DIRECTORY include/psizam DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
   install(DIRECTORY external/ucc/include/ucc
      external/wasi/include/wasi DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
   install(DIRECTORY external/softfloat/source/include/
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/psizam/external/softfloat)
   install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/psizam)
   foreach(_tool pzam-compile pzam-run psizam-wasi)
      if(TARGET ${_tool})
         install(TARGETS ${_tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
      endif()
   endforeach()
   configure_package_config_file(cmake/psizamConfig.cmake.in
      ${CMAKE_CURRENT_BINARY_DIR}/psizamConfig.cmake
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psizam)
   write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/psizamConfigVersion.cmake
      VERSION 1.0.0 COMPATIBILITY SameMajorVersion)
   install(EXPORT psizamTargets NAMESPACE psizam::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psizam)
   install(FILES ${CMAKE_CURRENT_BINARY_DIR}/psizamConfig.cmake
      ${CMAKE_CURRENT_BINARY_DIR}/psizamConfigVersion.cmake
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psizam)
endif()
