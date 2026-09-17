# Builds a downstream project using only the installed package and public API.
function(checked)
   execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
      OUTPUT_VARIABLE output ERROR_VARIABLE error)
   if(NOT result EQUAL 0)
      message(FATAL_ERROR "Command failed: ${ARGV}\n${output}\n${error}")
   endif()
endfunction()
file(MAKE_DIRECTORY "${WORK}")
checked("${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${WORK}/install")
checked("${WAT2WASM}" "${SOURCE}/tests/installed-runtime/smoke.wat"
   -o "${WORK}/smoke.wasm")
set(variants OFF)
if(HAVE_LLVM)
   list(APPEND variants ON)
endif()
foreach(variant IN LISTS variants)
   set(consumer "${WORK}/consumer-${variant}")
   checked("${CMAKE_COMMAND}" -S "${SOURCE}/tests/installed-runtime" -B "${consumer}"
      -G Ninja "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_C_COMPILER=${CC}"
      "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_PREFIX_PATH=${WORK}/install"
      "-DLLVM_DIR=${LLVM_DIR}" "-DTEST_LLVM=${variant}")
   checked("${CMAKE_COMMAND}" --build "${consumer}" -j 2)
   checked("${consumer}/runtime-consumer" "${WORK}/smoke.wasm")
endforeach()
message(STATUS "Installed runtime acceptance passed: ${variants}")
