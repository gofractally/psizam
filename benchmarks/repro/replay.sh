#!/bin/sh
# Run the complete measured suite, serially, from an extracted reproduction kit.
# Dependencies and runners must already be built using build.sh.
set -eu
if [ "$#" -ne 7 ]; then
  echo "Usage: $0 KIT_ROOT BUILD_ROOT NEW_WORK WASI_SDK NATIVE_CLANG WASMTIME REPORT_OUT" >&2
  exit 2
fi
bench_kit=$(cd "$1" && pwd)
bench_build=$(cd "$2" && pwd)
bench_work=$3
bench_sdk=$4
bench_cc=$5
bench_wt=$6
bench_report=$7
bench_scripts="$bench_kit/source/psizam/benchmarks/repro"
case "$("$bench_wt" --version)" in
  'wasmtime 48.0.2 '*) ;;
  *) echo 'This baseline requires Wasmtime 48.0.2.' >&2; exit 2 ;;
esac
python3 "$bench_scripts/run.py" prepare --work "$bench_work" \
  --wasi-sdk "$bench_sdk" --native-cc "$bench_cc" \
  --engine-bin "$bench_build/engine/bin" --runner "$bench_build/runner/repro-psizam" \
  --wasmtime "$bench_wt" --psio-source "$bench_kit/source/psio" \
  --source-cache "$bench_kit/evidence/sources"
# Every execution policy is compiled and validated independently. Add coremark to
# --only for sustained CRC-validated runs (both seed sets, at least 10s each).
python3 "$bench_scripts/resources.py" --work "$bench_work" --threads 1,18 --samples 5 --compile-samples 3
python3 "$bench_scripts/report.py" --work "$bench_work" --out "$bench_report"
