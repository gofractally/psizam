# Reproducing the comprehensive psizam / Cranelift report

The report contains the latest completed measurements for each experiment: eight compute workloads,
CoreMark, Clang artifact creation and two real C++ jobs, memory, startup costs,
scalar host-call paths, independently protected service round trips, and actual
C++ runtime proxies. Direct-engine experiments compare no CPU limits, gas/fuel,
and an armed elapsed deadline. The canonical runtime proxy experiment has
active gas instrumentation and reports actual backend consumption. Compilation and timing run serially.

The opening abstract interprets the results for reusable psibase services and
distinguishes measured engine costs from hypothetical transaction-level impact.
Its call-cost examples assume unchanged surrounding work; they do not measure
database execution, serialization, full IPC or production tail latency.
Deployment can use JIT2 for initial runs while LLVM AOT is prepared or loaded
into cache, then use cached AOT for subsequent runs. The full LLVM preparation
time need not block initial service availability. The benchmarks measure each
tier separately; concurrent background compilation, resource contention and
the transition to cached AOT are not timed.

## Inspect or regenerate the report

Install Python 3.11 or newer, matplotlib and reportlab, then run from the extracted kit:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 report-source/report.py --work evidence --out ../regenerated-report --comprehensive
```

`MANIFEST.json` hashes every included file. Verify the archive with
`experiment-source/verify_package.py` before modifying any content. The original
measured engine and psio sources are in `source/`; their manifest is in
`evidence/source-manifest.json`. The experiment harness and report renderer are
separately retained in `experiment-source/` and `report-source/`. Engine/tool identities are checked within each experiment. The compute, CoreMark,
Clang and supplementary scalar-boundary experiments use one frozen build.
`evidence/public-release/source/` preserves the independently measured service-call
build. `evidence/artifact-sizes.json` identifies the JIT1 writer snapshot used for
saved-file sizes and release validation. Timing cells never pool different builds.
No native executable or native-code cache is needed to regenerate charts.

When `platform-comparison/` is included, it preserves the prior ARM summary
and a comparison against this run, also included in the PDF. The archived
`evidence/platform-reference/arm-summary.json` makes full report regeneration
include those comparison pages automatically. Regenerate just its bars with:

```sh
python3 experiment-source/compare_platforms.py \
  --arm platform-comparison/arm-summary.json --x86 summary.json \
  --out ../regenerated-platform-comparison
```

The comparison requires identical compute Wasm hashes, compiler fixtures and
Wasmtime versions. Each host supplies its own Wasmtime reference; the resulting
ratios describe the measured platforms, without isolating instruction-set effects.

## Build and rerun the measurements

Use WASI SDK 32, CMake, Ninja, WABT, Python 3, Wasmtime CLI 48.0.2 and its matching
C API distribution. The report and `evidence/config.json` identify the measured
host. The recorded compiler/platform pairings are LLVM 22.1.2 on Apple M5 Max
macOS ARM64 and LLVM 20.1.8 on the Linux x86-64 VPS. Other machines are new
experimental conditions. A Clang artifact compilation has a 3,660-second
watchdog. Reserve several hours for the full run.

The Linux x86-64 VPS experiment is a separate set of conditions: AMD EPYC-Turin
virtual CPU, LLVM 20.1.8, WASI SDK 32 and Wasmtime 48.0.2. Runtime subprocesses
are pinned to vCPU 2; artifact compilation can use all vCPUs. LLVM and Cranelift
compiler-worker budgets are 1/16 for compute and 4/16 for Clang. The VPS kit's
`orchestration/` directory retains the exact build, validation, CPU-affinity and
serial execution scripts. Per-process steal-time observations accompany the
runtime evidence. Hardware instruction/cycle counters are unavailable there;
missing counters are never represented as zero. The Mac and VPS results do not
isolate instruction-set effects from CPU, OS, compiler or source differences.

Keep the supplied frozen sources intact. This command verifies the kit, creates
working copies of all three source snapshots, overlays only the experiment harness,
and builds every required tool before starting timed measurements:

```sh
sh experiment-source/replay-comprehensive.sh \
  "$PWD" /absolute/path/bench-build /absolute/path/new-measurements \
  /path/to/llvm /path/to/wasmtime-c-api /path/to/wasi-sdk \
  /path/to/wasmtime /absolute/path/new-report
```

For the recorded Linux configuration, prefix that command with `BENCH_CPU=2` and
`BENCH_LLVM_DIR="$PWD/evidence/platform-config/llvm20-cmake"`, using
`/usr/lib/llvm-20` as the LLVM prefix. The retained CMake wrapper selects the
distro LLVM headers without introducing libc++ headers into the libstdc++
build. Both X86 and AArch64 LLVM targets are enabled, including the API's
cross-compilation checks. Adjust paths for another distribution. Worker budgets are read
from the kit's recorded compute and compiler matrices. CPU affinity applies
to execution; compilation retains the available CPU set.

Use a new measurement directory. The script refuses to overwrite trial files.
It verifies pinned input hashes, records every process command and output,
checks exact compiler-object hashes and requires all timing cells. A failed
engine check blocks rankings; diagnostic evidence is retained.

The bundled standard-validation logs describe the identical engine snapshot on
the recorded host. The replay retains that provenance; validation for a new host
must also be run using `source/psizam/docs/validation.md` and labeled separately.
The scalar spec suite has one unsupported multiple-memory case; the report
states that limitation explicitly.

## Running the compiler with Wasmtime

The pinned `clang.wasm` imports only WASI Preview 1 functions. It runs with
Wasmtime's standard WASI implementation; a psizam-specific host is unnecessary.
Both compiler workloads in this suite run the same guest compiler under
Wasmtime and psizam and require the same output-object SHA-256.

For example, with the pinned files under `fixtures/` and an existing `output/`:

```sh
wasmtime run -C compiler=cranelift,cache=n \
  -W max-wasm-stack=16777216 \
  --dir "$PWD/fixtures::/fixtures" --dir "$PWD/output::/output" \
  fixtures/clang.wasm -cc1 -triple wasm32-unknown-wasi -emit-obj \
  -O2 -std=c++17 -x c++-cpp-output /fixtures/SipHash.cpp.ii \
  -o /output/result.o
```

Ordinary unpreprocessed inputs additionally need accessible headers and a
sysroot. A complete build must orchestrate compiler and linker jobs outside
the guest: WASI Preview 1 does not provide the process spawning used by a
native Clang driver. `scripts/pzam-clang-driver.py` currently performs that
orchestration for PZAM; an equivalent wrapper can invoke Wasmtime for Clang
and the Wasm linker. This is separate from implementing custom Wasm imports.
Wasmtime uses its own saved native-code artifact, not `.pzam`.

## Definitions and interpretation

- Compute: five trials plus one excluded warmup per cell. The eight workloads
  use a guest or kernel timer, excluding engine setup. All outputs are checked.
- CoreMark: three trials plus one warmup for each of two CRC seed sets, with
  every run at least ten seconds. Counts are calibrated per engine and held
  fixed across its limit settings. Compare iterations/second across engines.
- Clang preparation: three trials per artifact, with 4 and 18 workers on the
  Mac, or 4 and 16 on the VPS, for LLVM and Cranelift; JIT2 uses one.
  Time includes compilation and saving. Page
  timeout uses the exact unrestricted psizam artifact. Cranelift epoch code is
  separately compiled. Budgets count compiler workers, not guest threads.
- C++ jobs: five trials plus one warmup, including startup, fresh engine
  compilation where applicable, guest Clang work and object writing. Fresh JITs
  use one compiler worker; this is not the fastest parallel Cranelift startup.
  Saved LLVM
  and Cranelift use the higher-worker artifact consistently. Whole-module LLVM ORC
  cold Clang startup and a full Clang self-bootstrap are outside this test.
- Boundaries: five trials plus one warmup, ten million scalar calls per path.
  Checked and cached/unchecked API paths have different semantics, stated in
  the report. This does not benchmark WIT records or serialization.
- Service calls: five trials plus one warmup per cell. Natural-signature calls
  exercise two different host queries, A-to-B and A-to-B-to-A round trips, with
  separate protection at every guest entry. Cached Cranelift uses raw C API
  slots with a bind-time signature check and per-call trap containment.
- Runtime proxies: five trials plus one warmup, one million scalar canonical
  calls through instance::as and runtime::bind. Both backends have a finite gas
  budget, configured and inspected through the public runtime API. Separate
  acceptance gates exercise exhaustion and resource-state updates. This ABI experiment does not include a
  matching Wasmtime component binding. It does not measure records or strings.
- Saved-file sizes: logical .pzam and Cranelift AOT byte lengths, with native
  regions reported separately. PZAM uses the complete code blob; ELF uses all
  executable sections. Both include constants, stubs and alignment padding.
  JIT1 is compiled by the separately archived writer; other engines are inspected
  from the exact timed artifacts. Compiler worker variants remain in raw data.
  Native artifacts are omitted from the kit: `packaged-evidence.json` explicitly
  binds the inspected records and lists omitted files. Verify `MANIFEST.json`
  before regenerating; present artifacts with incorrect hashes are rejected.
- Memory: peak process RSS, not reserved virtual address space. Startup charts
  subtract the guest computation from process wall time; the remainder includes
  loading, JIT compilation, setup and teardown, not just instantiation.
- Limits: psizam gas and Wasmtime fuel units differ. Timed runs use a 10^12 work
  budget and 120-second elapsed deadline, both sufficient to finish. Separate
  gates verify exhaustion. No-limit profiles omit both kinds of checking.
- Runtime figures show one fixed build per engine; worker counts appear only
  in compilation comparisons. Overhead bars use a 0% unrestricted baseline and
  show only work-budget and time-limit overhead, including negative variation.

Raw trials and observed ranges are retained. Medians are descriptive estimates
on the recorded host, not proof of statistical equivalence. Source-confirmed
extra work and controlled ablations are distinguished from optimization
hypotheses that need further experiments.
