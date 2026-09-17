# psizam / Cranelift reproduction kit

September 16, 2026 baseline: Apple M5 Max, ARM64 macOS, one compiler worker.
The PDF and HTML summarize results. `summary.json` has medians and ranges;
`samples.csv` and `evidence/` retain the underlying measurements and failures.

## Contents and integrity

- `source/psizam/`: commit 431663847ab46c762e6a9c0a8cacbd7f009362eb,
  plus the new benchmark harness in `benchmarks/repro/`.
- `source/psio/`: commit ff114b6a8c9265e6b5da5ffb85df1617b2b43514.
- `evidence/sources/`: pinned upstream CoreMark and LLVM test-suite files,
  including their licenses. `sources.lock.json` is in the harness.
- `evidence/modules/`: exact Wasm benchmark inputs.
- `evidence/clang/`: source-built Clang Wasm and frozen LLVM C++ inputs.
- `evidence/provenance/`: build settings, tool download hashes, earlier Clang
  bootstrap provenance and benchmark validation records.
- `evidence/commands.jsonl`, `logs/` and JSON: original command arguments,
  stdout/stderr, failures, timing and RSS. Original absolute paths are retained
  as provenance; replay creates commands using the new workspace paths.
- `MANIFEST.json`: SHA-256 and size of every included file except itself.

Native binaries and generated PZAM/CWASM artifacts are intentionally rebuilt.
The kit contains independent source trees because psizam had no published
remote when this baseline was taken. Git history is not required for replay.
The supplied source-revision metadata preserves the original commit identity.

## Dependencies

Use Python 3.12+, CMake, Ninja, Boost, simdjson, Homebrew LLVM 22.1.2 on macOS,
and WASI SDK 32.0 (Clang 22.1.0). Report generation needs ReportLab, Matplotlib
and NumPy. The engine and benchmark runner use C++23 and a Release build.

Download Wasmtime 48.0.2 CLI and C API for the machine being measured. For the
original ARM64 Mac:

```sh
curl -fL -o wasmtime-cli.tar.xz https://github.com/bytecodealliance/wasmtime/releases/download/v48.0.2/wasmtime-v48.0.2-aarch64-macos.tar.xz
curl -fL -o wasmtime-api.tar.xz https://github.com/bytecodealliance/wasmtime/releases/download/v48.0.2/wasmtime-v48.0.2-aarch64-macos-c-api.tar.xz
echo '8abc55958f04678bf01d4d0c46868f45aa8083bacc29e5a1685f971f110fbc14  wasmtime-cli.tar.xz' | shasum -a 256 -c -
echo 'c4b95346d92168963607f8ac56ef70298aa83b9d2b23af3e801759df3443ed09  wasmtime-api.tar.xz' | shasum -a 256 -c -
tar -xf wasmtime-cli.tar.xz
tar -xf wasmtime-api.tar.xz
```

Use absolute paths below. From the extracted kit directory:

```sh
python3 source/psizam/benchmarks/repro/verify_package.py .

sh source/psizam/benchmarks/repro/build.sh \
  "$PWD/source/psizam" "$PWD/source/psio" /path/to/new-build \
  /opt/homebrew/opt/llvm /path/to/wasmtime-v48.0.2-aarch64-macos-c-api

sh source/psizam/benchmarks/repro/replay.sh \
  "$PWD" /path/to/new-build /path/to/new-work /path/to/wasi-sdk \
  /opt/homebrew/opt/llvm/bin/clang \
  /path/to/wasmtime-v48.0.2-aarch64-macos/wasmtime /path/to/new-report
```

Stop competing benchmarks and builds before timing. Replay is serial and
requires a new work directory. Expect a sustained run: each CoreMark trial
lasts at least ten seconds. Allow up to 45 minutes for Clang Wasm compilation;
the artifact watchdog allows 60 minutes. The original one-worker LLVM trial
took 910 seconds. Replay also measures 4- and 18-worker artifact creation for
both engines and remeasures small workloads/CoreMark with 1 and 18 workers.
Expected failures are recorded and excluded; they are not treated as timings.
Read `source/psizam/benchmarks/repro/README.md` for individual stages, profiles,
metric definitions and call-boundary API differences.

## Comparing results

LLVM O2 PZAM uses hardware NaN canonicalization and is compared with Cranelift
using `nan-canonicalization=y`. In-process fast-FP JITs use default-FP Cranelift.
ARM JIT1/JIT2 still use softfloat helpers for FP SIMD despite requesting fast
FP. CoreMark is integer work; both Cranelift FP profiles are retained as a control.

Saved PZAM loads with its default validation; Wasmtime explicitly trusts its
locally generated precompiled artifact. Startup measures those configured
public paths. OS page caches are warm. RSS is process-wide, not per instance.
The Clang jobs compile real LLVM translation units but do not rerun a full
self-bootstrap. The report is an ARM64 Mac engineering baseline; repeat on
Linux x86-64 before making claims about that platform.

## Bar-chart and parallel-compilation edition

`parallel_compile.py` gives both engines identical requested worker budgets
and verifies every resulting compiler artifact against two frozen object
hashes. `parallel_runtime.py` checks execution of artifacts built using LLVM's
different serial and batched strategies. Worker counts do not make the guest
workload multithreaded. The original one-worker large compilation is a single
historical trial, while the parallel settings have three repeats each.
`parallel_clang_jobs.py` then compares the compiler jobs in a separate
interleaved batch, after artifact compilation ends, with five measured fresh
processes after one excluded warmup per engine/input pair.

`validation-evidence/` retains the standard CTest regression results, including
known failures. `validation-overlay/` can be copied onto `source/psizam/` to
reproduce this gate; it changes test/build wiring, not the measured engine.
The report's validation status is a dated snapshot, not a fresh remote CI run.
