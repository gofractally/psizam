#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?usage: verify-independent.sh BUILD_DIRECTORY PSIO_INSTALL_PREFIX}
psio_prefix=${2:?psio install prefix is required}
mkdir -p "$work"
work=$(cd "$work" && pwd)
cmake -S "$root" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$psio_prefix" -DCMAKE_INSTALL_PREFIX="$work/install" \
  -DPSIZAM_ENABLE_TESTS=ON -DPSIZAM_ENABLE_SPEC_TESTS=OFF -DPSIZAM_ENABLE_TOOLS=ON
cmake --build "$work/build" --target unit_tests runtime_parity_tests psizam_compliance_tests \
  pzam-run pzam-compile psizam-wasi psizam-debug psizam_execution_driver psizam_regalloc_tests --parallel "${PSIZAM_BUILD_JOBS:-4}"
"$work/build/bin/unit_tests"
"$work/build/bin/runtime_parity_tests"
"$work/build/bin/psizam_compliance_tests"
ctest --test-dir "$work/build" --output-on-failure --no-tests=error -L quick
cmake --install "$work/build"
cmake -S "$root/examples/consumer" -B "$work/installed-consumer" -G Ninja \
  -DCMAKE_PREFIX_PATH="$work/install;$psio_prefix"
cmake --build "$work/installed-consumer"
ctest --test-dir "$work/installed-consumer" --output-on-failure
cmake -S "$root/examples/consumer" -B "$work/source-consumer" -G Ninja \
  -DPSIZAM_SOURCE_DIR="$root" -DCMAKE_PREFIX_PATH="$psio_prefix"
cmake --build "$work/source-consumer"
ctest --test-dir "$work/source-consumer" --output-on-failure
