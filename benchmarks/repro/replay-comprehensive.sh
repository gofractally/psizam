#!/bin/sh
# Build all recorded source cohorts before starting any timed experiment.
set -eu
if [ "$#" -ne 8 ]; then
  echo "Usage: $0 KIT_ROOT BUILD_ROOT NEW_WORK LLVM_PREFIX WASMTIME_C_API WASI_SDK WASMTIME REPORT_OUT" >&2
  exit 2
fi
bench_kit=$(cd "$1" && pwd)
mkdir -p "$2"
bench_build=$(cd "$2" && pwd)
bench_work=$3
bench_llvm=$(cd "$4" && pwd)
bench_api=$(cd "$5" && pwd)
bench_sdk=$(cd "$6" && pwd)
bench_wt=$7
bench_report=$8
if [ -e "$bench_work" ] || [ -e "$bench_build/replay-source" ]; then
  echo 'Use new work and build directories; existing evidence is never overwritten.' >&2
  exit 2
fi
case "$("$bench_wt" --version)" in
  'wasmtime 48.0.2 '*) ;;
  *) echo 'This experiment requires Wasmtime 48.0.2.' >&2; exit 2 ;;
esac
case "$(uname -m)" in
  arm64|aarch64) bench_arch=aarch64 ;;
  x86_64) bench_arch=x86_64 ;;
  *) echo 'Unsupported host architecture.' >&2; exit 2 ;;
esac
python3 "$bench_kit/experiment-source/verify_package.py" "$bench_kit"
bench_service=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["directory"])' "$bench_kit/evidence/service-cohort.json")
bench_writer=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["jit1_provenance"]["directory"])' "$bench_kit/evidence/artifact-sizes.json")
mkdir "$bench_build/replay-source"
cp -R "$bench_kit/source" "$bench_build/replay-source/compute"
cp -R "$bench_kit/evidence/$bench_service/source" "$bench_build/replay-source/calls"
cp -R "$bench_kit/evidence/$bench_writer/source" "$bench_build/replay-source/writer"
for bench_cohort in compute calls writer; do
  bench_source="$bench_build/replay-source/$bench_cohort"
  cp "$bench_kit"/experiment-source/* "$bench_source/psizam/benchmarks/repro/"
  sh "$bench_source/psizam/benchmarks/repro/build.sh" \
    "$bench_source/psizam" "$bench_source/psio" "$bench_build/$bench_cohort" \
    "$bench_llvm" "$bench_api" "$bench_sdk"
done
bench_scripts="$bench_build/replay-source/compute/psizam/benchmarks/repro"
python3 "$bench_scripts/run.py" prepare --work "$bench_work" \
  --wasi-sdk "$bench_sdk" --native-cc "$bench_llvm/bin/clang" \
  --engine-bin "$bench_build/compute/engine/bin" --runner "$bench_build/compute/runner/repro-psizam" \
  --wasmtime "$bench_wt" --psio-source "$bench_build/replay-source/compute/psio" \
  --source-cache "$bench_kit/evidence/sources"
python3 "$bench_scripts/resources.py" --work "$bench_work" --threads 1,18 --samples 5 --compile-samples 3
python3 "$bench_scripts/clang_resources.py" prepare --work "$bench_work" --fixtures "$bench_kit/evidence/compiler-fixtures"
python3 "$bench_scripts/clang_resources.py" compile --work "$bench_work"
python3 "$bench_scripts/clang_resources.py" measure --work "$bench_work"
python3 "$bench_scripts/boundary.py" --work "$bench_work" --runner "$bench_build/compute/runner/repro-boundary" --samples 5
for bench_policy in none gas; do
  bench_gas=off
  if [ "$bench_policy" = gas ]; then bench_gas=on; fi
  "$bench_build/calls/engine/bin/pzam-compile" --target="$bench_arch" --backend=llvm --opt-level=2 \
    --gas-metering="$bench_gas" "$bench_build/calls/calls/calls.wasm" \
    -o "$bench_build/calls/calls/call-$bench_policy.pzam"
done
cp "$bench_build/calls/calls/call-none.pzam" "$bench_build/calls/calls/call-timeout.pzam"
python3 - "$bench_scripts" "$bench_work" "$bench_build/replay-source/calls" <<'PY'
import sys
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0,sys.argv[1])
from run import save,source_identity
from resources import freeze_source
work=Path(sys.argv[2]);source=Path(sys.argv[3]);directory=work/'public-release';directory.mkdir()
config={repo:source_identity(source/repo) for repo in ['psizam','psio']}
digest=freeze_source(SimpleNamespace(work=directory,config=config))
save(work/'service-cohort.json',dict(directory=directory.name,commit=config['psizam']['commit'],source_manifest_sha256=digest))
PY
python3 "$bench_build/replay-source/calls/psizam/benchmarks/call-boundary/run.py" \
  --binary "$bench_build/calls/calls/call-boundary" --fixture "$bench_build/calls/calls/calls.wasm" \
  --aot-dir "$bench_build/calls/calls" --out "$bench_work/public-release/service-calls" --samples 5
python3 "$bench_build/replay-source/calls/psizam/benchmarks/call-boundary/run_runtime.py" \
  --binary "$bench_build/calls/calls/call-runtime" \
  --fixtures "$bench_build/calls/engine/examples/composition" --out "$bench_work/public-release/runtime-calls" --samples 5
python3 "$bench_scripts/run.py" prepare --work "$bench_work/coremark" \
  --wasi-sdk "$bench_sdk" --native-cc "$bench_llvm/bin/clang" \
  --engine-bin "$bench_work/tools" --runner "$bench_work/tools/repro-psizam" \
  --wasmtime "$bench_work/tools/wasmtime" --psio-source "$bench_build/replay-source/compute/psio" \
  --source-cache "$bench_kit/evidence/sources"
python3 "$bench_scripts/resources.py" --work "$bench_work/coremark" --only coremark --threads 1 --samples 3 --compile-samples 3
python3 "$bench_scripts/native_coremark.py" --work "$bench_work/coremark" --samples 3
python3 "$bench_scripts/artifact_sizes.py" --work "$bench_work" \
  --probe "$bench_build/writer/runner/repro-code-size" --readobj "$bench_llvm/bin/llvm-readobj" \
  --jit1-compiler "$bench_build/writer/engine/bin/pzam-compile" \
  --jit1-source "$bench_build/replay-source/writer/psizam" --psio-source "$bench_build/replay-source/writer/psio"
# These are original-host acceptance records, never evidence of new-host testing.
cp -R "$bench_kit/evidence/validation-evidence" "$bench_work/validation-evidence"
cp -R "$bench_kit/evidence/release-validation" "$bench_work/release-validation"
python3 "$bench_scripts/report.py" --work "$bench_work" --out "$bench_report" --comprehensive
