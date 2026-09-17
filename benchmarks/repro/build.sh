#!/bin/sh
# Build engine and benchmark runners from the supplied independent source trees.
# Usage: build.sh PSIZAM_SOURCE PSIO_SOURCE BUILD_ROOT LLVM_PREFIX WASMTIME_C_API WASI_SDK
set -eu
if [ "$#" -ne 6 ]; then
  echo "Usage: $0 PSIZAM_SOURCE PSIO_SOURCE BUILD_ROOT LLVM_PREFIX WASMTIME_C_API WASI_SDK" >&2
  exit 2
fi
bench_psizam=$(cd "$1" && pwd)
bench_psio=$(cd "$2" && pwd)
mkdir -p "$3"
bench_build=$(cd "$3" && pwd)
bench_llvm=$(cd "$4" && pwd)
bench_llvm_cmake=${BENCH_LLVM_DIR:-$bench_llvm/lib/cmake/llvm}
bench_llvm_targets=${BENCH_LLVM_TARGETS:-X86;AArch64}
bench_wasmtime=$(cd "$5" && pwd)
bench_sdk=$(cd "$6" && pwd)
cmake -S "$bench_psizam" -B "$bench_build/engine" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$bench_llvm/bin/clang" \
  -DCMAKE_CXX_COMPILER="$bench_llvm/bin/clang++" \
  -DFETCHCONTENT_SOURCE_DIR_PSIO="$bench_psio" \
  -DPSIZAM_ENABLE_LLVM=ON -DLLVM_DIR="$bench_llvm_cmake" \
  -DPSIZAM_LLVM_TARGETS="$bench_llvm_targets" \
  -DPSIZAM_ENABLE_TOOLS=ON -DPSIZAM_ENABLE_TESTS=OFF \
  -DPSIZAM_ENABLE_EXAMPLES=ON -DPSIZAM_WASI_SDK="$bench_sdk" -DPSIZAM_ENABLE_INSTALL=ON \
  -DCMAKE_INSTALL_PREFIX="$bench_build/install"
cmake --build "$bench_build/engine" --target pzam-compile pzam-run psizam-wasi psizam-debug composition_provider_guest composition_consumer_guest -j2
cmake --install "$bench_build/engine"
cmake -S "$bench_psizam/benchmarks/repro" -B "$bench_build/runner" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$bench_llvm/bin/clang" \
  -DCMAKE_CXX_COMPILER="$bench_llvm/bin/clang++" \
  -DCMAKE_PREFIX_PATH="$bench_build/install" \
  -DLLVM_DIR="$bench_llvm_cmake" -DWASMTIME_C_API="$bench_wasmtime"
cmake --build "$bench_build/runner" -j2
python3 -m unittest discover -s "$bench_psizam/benchmarks/repro" -p 'test_*.py'

cmake -S "$bench_psizam/benchmarks/call-boundary" -B "$bench_build/calls" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$bench_llvm/bin/clang" \
  -DCMAKE_CXX_COMPILER="$bench_llvm/bin/clang++" \
  -DFETCHCONTENT_SOURCE_DIR_PSIO="$bench_psio" \
  -DPSIZAM_ENABLE_LLVM=ON -DLLVM_DIR="$bench_llvm_cmake" \
  -DPSIZAM_LLVM_TARGETS="$bench_llvm_targets" \
  -DPSIZAM_ENABLE_TESTS=OFF -DPSIZAM_ENABLE_EXAMPLES=OFF \
  -DWASMTIME_C_API="$bench_wasmtime"
cmake --build "$bench_build/calls" --target call-boundary call-runtime -j2
