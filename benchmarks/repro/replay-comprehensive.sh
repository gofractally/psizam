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
if [ -d "$bench_kit/evidence/compiler-jobs-native" ]; then
  if [ -z "${BENCH_NATIVE_CLANG:-}" ]; then
    echo 'This kit includes paired native/compiler jobs. Set BENCH_NATIVE_CLANG to native Clang 22.1.2.' >&2
    exit 2
  fi
  case "$("$BENCH_NATIVE_CLANG" --version)" in
    *22.1.2*) ;;
    *) echo 'BENCH_NATIVE_CLANG must be version 22.1.2.' >&2; exit 2 ;;
  esac
  python3 - "$BENCH_NATIVE_CLANG" "$bench_kit/evidence/compiler-jobs-native/config.json" "${BENCH_NATIVE_TOOLCHAIN:-}" <<'PY'
import hashlib,json,sys
from pathlib import Path
with Path(sys.argv[1]).open('rb') as f:actual=hashlib.file_digest(f,'sha256').hexdigest()
expected=json.load(open(sys.argv[2]))['native']['sha256']
if actual!=expected and not sys.argv[3]:
    raise SystemExit('Different native compiler build: set BENCH_NATIVE_TOOLCHAIN to its acquisition/provenance JSON.')
PY
fi
bench_threads=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(",".join(map(str,sorted({int(v.split("/j")[1].split("/")[0]) for v in d["matrix"] if v.startswith(("llvm-o2-pzam/","cranelift-aot/"))}))))' "$bench_kit/evidence/resources.json")
bench_clang_threads=$(python3 -c 'import json,sys; print(",".join(map(str,json.load(open(sys.argv[1]))["threads"])))' "$bench_kit/evidence/clang-resource-config.json")
bench_run() {
  if [ -n "${BENCH_CPU:-}" ]; then
    python3 "$bench_kit/orchestration/run-stage.py" "$@"
  else
    python3 "$@"
  fi
}
bench_call_run() {
  if [ -n "${BENCH_CPU:-}" ]; then
    taskset -c "$BENCH_CPU" python3 "$@"
  else
    python3 "$@"
  fi
}
if [ -n "${BENCH_CPU:-}" ] && [ ! -f "$bench_kit/orchestration/run-stage.py" ]; then
  echo 'BENCH_CPU requires the Linux affinity adapter included in the VPS kit.' >&2
  exit 2
fi
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
bench_run "$bench_scripts/run.py" prepare --work "$bench_work" \
  --wasi-sdk "$bench_sdk" --native-cc "$bench_llvm/bin/clang" \
  --engine-bin "$bench_build/compute/engine/bin" --runner "$bench_build/compute/runner/repro-psizam" \
  --wasmtime "$bench_wt" --psio-source "$bench_build/replay-source/compute/psio" \
  --source-cache "$bench_kit/evidence/sources"
if [ -n "${BENCH_CPU:-}" ]; then
  python3 - "$bench_work/config.json" "$BENCH_CPU" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]);config=json.loads(p.read_text())
cpu=sys.argv[2]
config['execution_cpu_affinity']=cpu
config['placement_notes']='Linux runtime subprocesses pinned to CPU '+cpu+'; compiler subprocesses retain the available CPU set. Physical-core isolation, frequency and temperature are not controlled.'
for line in Path('/proc/cpuinfo').read_text().splitlines():
    if line.startswith('model name'):
        config['hardware']={'machdep.cpu.brand_string':line.split(':',1)[1].strip()};break
p.write_text(json.dumps(config,indent=2)+'\n')
PY
fi
bench_run "$bench_scripts/resources.py" --work "$bench_work" --threads "$bench_threads" --samples 5 --compile-samples 3
bench_run "$bench_scripts/clang_resources.py" prepare --work "$bench_work" --fixtures "$bench_kit/evidence/compiler-fixtures" --threads "$bench_clang_threads"
bench_run "$bench_scripts/clang_resources.py" compile --work "$bench_work"
if [ -d "$bench_kit/evidence/compiler-jobs-native" ]; then
  python3 - "$bench_kit/evidence/compiler-jobs-native/config.json" "$bench_build/native-clang-acquisition.json" <<'PY'
import json,sys
config=json.load(open(sys.argv[1]))
with open(sys.argv[2],'w') as f:json.dump(config['native']['acquisition'],f,indent=2)
PY
  if [ -n "${BENCH_NATIVE_TOOLCHAIN:-}" ]; then
    cp "$BENCH_NATIVE_TOOLCHAIN" "$bench_build/native-clang-acquisition.json"
  fi
  set --
  if [ -n "${BENCH_CPU:-}" ]; then set -- --cpu "$BENCH_CPU"; fi
  python3 "$bench_scripts/compiler_jobs.py" prepare --work "$bench_work" \
    --native-cc "$BENCH_NATIVE_CLANG" --native-toolchain "$bench_build/native-clang-acquisition.json" \
    --extra-fixture "$bench_kit/evidence/compiler-jobs-native/inputs/LLParser.cpp.ii" \
    --fixture-metadata "$bench_kit/evidence/compiler-jobs-native/fixture-metadata.json" "$@"
  python3 - "$bench_scripts" "$bench_work/compiler-jobs-native" <<'PY'
import hashlib,json,shutil,sys
from pathlib import Path
source=Path(sys.argv[1]);work=Path(sys.argv[2]);archive=work/'measurement-source';archive.mkdir()
manifest={}
for name in ['compiler_jobs.py','run.py','resources.py','clang_resources.py']:
    shutil.copy2(source/name,archive/name)
    manifest[name]=hashlib.sha256((archive/name).read_bytes()).hexdigest()
(work/'measurement-source.json').write_text(json.dumps(manifest,indent=2)+'\n')
PY
  # Homebrew Clang's non-system libraries use this recorded loader-relative RPATH.
  # Linux preparation already resolves and hashes libraries with ldd.
  python3 - "$bench_work/compiler-jobs-native/config.json" <<'PY'
import hashlib,json,platform,subprocess,sys
from pathlib import Path
if platform.system()=='Darwin':
    path=Path(sys.argv[1]);config=json.loads(path.read_text());cc=Path(config['native']['path'])
    linked=subprocess.check_output(['otool','-L',str(cc)],text=True)
    loads=subprocess.check_output(['otool','-l',str(cc)],text=True)
    if '@rpath/' in linked and '@loader_path/../lib' not in loads:
        raise SystemExit('Resolve this native compiler library layout before timing.')
    libraries=[]
    for line in linked.splitlines()[1:]:
        name=line.strip().split(' (')[0]
        if name.startswith('@rpath/'):
            lib=(cc.parent.parent/'lib'/name[len('@rpath/'):]).resolve()
            with lib.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
            libraries.append(dict(path=str(lib),sha256=digest))
    config['native']['libraries']=libraries
    config['native']['library_resolution']='otool: @loader_path/../lib; Apple system libraries remain in the OS dyld shared cache.'
    path.write_text(json.dumps(config,indent=2)+'\n')
PY
  python3 "$bench_scripts/compiler_jobs.py" measure --work "$bench_work"
else
  bench_run "$bench_scripts/clang_resources.py" measure --work "$bench_work"
fi
bench_run "$bench_scripts/boundary.py" --work "$bench_work" --runner "$bench_build/compute/runner/repro-boundary" --samples 5
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
bench_call_run "$bench_build/replay-source/calls/psizam/benchmarks/call-boundary/run.py" \
  --binary "$bench_build/calls/calls/call-boundary" --fixture "$bench_build/calls/calls/calls.wasm" \
  --aot-dir "$bench_build/calls/calls" --out "$bench_work/public-release/service-calls" --samples 5
bench_call_run "$bench_build/replay-source/calls/psizam/benchmarks/call-boundary/run_runtime.py" \
  --binary "$bench_build/calls/calls/call-runtime" \
  --fixtures "$bench_build/calls/engine/examples/composition" --out "$bench_work/public-release/runtime-calls" --samples 5
bench_run "$bench_scripts/run.py" prepare --work "$bench_work/coremark" \
  --wasi-sdk "$bench_sdk" --native-cc "$bench_llvm/bin/clang" \
  --engine-bin "$bench_work/tools" --runner "$bench_work/tools/repro-psizam" \
  --wasmtime "$bench_work/tools/wasmtime" --psio-source "$bench_build/replay-source/compute/psio" \
  --source-cache "$bench_kit/evidence/sources"
bench_run "$bench_scripts/resources.py" --work "$bench_work/coremark" --only coremark --threads 1 --samples 3 --compile-samples 3
bench_run "$bench_scripts/native_coremark.py" --work "$bench_work/coremark" --samples 3
python3 "$bench_scripts/artifact_sizes.py" --work "$bench_work" \
  --probe "$bench_build/writer/runner/repro-code-size" --readobj "$bench_llvm/bin/llvm-readobj" \
  --jit1-compiler "$bench_build/writer/engine/bin/pzam-compile" \
  --jit1-source "$bench_build/replay-source/writer/psizam" --psio-source "$bench_build/replay-source/writer/psio"
# These are original-host acceptance records, never evidence of new-host testing.
cp -R "$bench_kit/evidence/validation-evidence" "$bench_work/validation-evidence"
cp -R "$bench_kit/evidence/release-validation" "$bench_work/release-validation"
python3 "$bench_scripts/report.py" --work "$bench_work" --out "$bench_report" --comprehensive
