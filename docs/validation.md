# Execution correctness gates

Ordinary test builds now include process-isolated execution checks. Run the
complete compiler execution profile before release, in addition to the unit,
package, scalar/SIMD specification and full compiler-bootstrap gates.
A green quick check alone is not a release result.

## Fast development checks

With `PSIZAM_ENABLE_TESTS=ON`, the eight tiny WAT modules are compiled in the
build directory and registered with CTest automatically. Tests also build the
real compiler/loader tools even when `PSIZAM_ENABLE_TOOLS=OFF`.
Python 3.11+ and WABT's `wat2wasm` are required.

```sh
cmake --build /path/to/build --target psizam_execution_driver psizam_regalloc_tests pzam-compile pzam-run
ctest --test-dir /path/to/build -L quick --output-on-failure --no-tests=error
```

The arithmetic modules compute a result and call WASI `proc_exit(42)`. A separate
module writes an exact output marker and returns normally, exercising restoration
of host registers. Exit zero without executing the guest, incorrect arithmetic,
a signal or a timeout cannot pass. The SIMD modules pass vectors through direct,
indirect and tail-indirect calls and check every result lane. The matrix includes:

- Interpreter, JIT1 and JIT2; explicit fast and softfloat modes for both JITs.
- JIT2 artifacts produced by the CLI and the public `pzam_compile()` API.
- With LLVM enabled: LLVM JIT fast FP, CLI LLVM O2 hardware-deterministic and
  softfloat artifacts, and public-API LLVM O2 artifacts.

Every saved-artifact test exits the compiler process, then launches `pzam-run`
in a separate process. No in-memory result or old artifact may substitute for
reloading the file. The Python harness has negative controls for skipped guest
execution, stale artifacts, missing fixtures and timeout process-group cleanup.
Large generated modules verify distant stack-overflow and division traps. A
register-allocation test checks lifetime interference, repeated register reuse
and call survival for both target register banks on every host.

## Service-call checks

`ctest -L calls --output-on-failure` runs the protected natural-signature call
matrix, signal/guard restoration tests, and reflected runtime parity cases.
These are also part of the standard `quick` and `execution` gates. The natural
call matrix covers JIT1, JIT2, LLVM JIT and newly generated CLI LLVM O2 PZAM,
with no CPU limits, gas, and actual page-timeout interruption and recovery.

Regression cases cover nested A-to-B-to-A entry, stack and memory traps,
repeated host exceptions with destructor checks, memory growth during a
callback, cached stack leases, inline argument slots, missing or wrong export
signatures, moved/expired instances, and canonical allocator state restoration.
The runtime proxy checks configure backend gas directly; they do not certify
the unfinished public facade's resource-state wiring. Reproduce focused timings
with `benchmarks/call-boundary/README.md` before running the complete benchmarks.

## CPU-resource checks

The quick execution target also builds `psizam_resource_runner` and the public
compile API tests. `ctest -L resource --output-on-failure` checks:

- Finite completion, gas exhaustion, and page timeouts for JIT1, JIT2, LLVM JIT,
  JIT2 PZAM and LLVM O2 PZAM, including module start functions.
- Zero gas consumption with checks omitted, and rejection of a requested gas
  policy on an artifact compiled without gas instrumentation.
- Correct PZAM instrumentation metadata and effective public-API `gas_meter`
  selection, with separate metered and unmetered native code.
- Repeated timeout/recovery on the same interpreter/JIT module. Fresh LLVM ORC
  executable sections are registered for page protection and fault handling;
  saved LLVM PZAM uses its mapped code range. No per-loop polling is required.
- With LLVM enabled, `execution.llvm-gas` verifies entry-block allocation and
  promotion of scalar/SIMD locals at O0 and O2, with gas enabled and disabled,
  guarded and checked memory, all FP modes, and module/per-function translation.
  It checks relaxed atomic deadline loads and retained checkpoints before and
  after optimization. Fresh LLVM JIT and saved LLVM O2 PZAM also exercise exact
  gas accounting, repeated callback refills, cross-thread interruption while
  executing, and reuse after exhaustion. These checks run in the standard
  `quick`, `resource`, and `execution` gates.

Timeouts are elapsed wall-clock deadlines. The benchmark's external process
watchdog is only a harness guard and cannot satisfy an enforcement test.
Code-page tests use one execution owning its module; interrupting indefinitely
blocked host calls is outside this profile. See
[the reproducible resource benchmark](../benchmarks/repro/README.md) for the
three-policy measurement matrix and report commands.

## Required compiler execution profile

The pinned inputs are `clang.wasm`, `SipHash.cpp.ii`, and `APInt.cpp.ii` from
the September 16, 2026 reproduction kit. Their SHA-256 hashes and expected
object hashes are checked in at `tests/validation/compiler-fixtures.json`.
Expected outputs were independently matched with LLVM O2 PZAM and Cranelift
AOT, using fixed guest paths. Local host paths can differ.

Use `evidence/clang` in the extracted kit, or extract only the required files:

```sh
python3 scripts/stage-compiler-fixtures.py \
  --archive /path/to/reproduction-kit.tar.gz --dest /path/to/compiler-fixtures
```

Configure the standard execution profile from an independent checkout:

```sh
export PSIZAM_COMPILER_TEST_DATA=/path/to/compiler-fixtures
export CC=/path/to/llvm/bin/clang
export CXX=/path/to/llvm/bin/clang++
cmake --preset execution-validation \
  -DFETCHCONTENT_SOURCE_DIR_PSIO=/path/to/psio \
  -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm \
  -DPSIZAM_WASI_SDK=/path/to/wasi-sdk
cmake --build --preset execution-validation
ctest --preset execution-validation
```

The profile enables `PSIZAM_ENABLE_COMPILER_TESTS` and LLVM. Configuration fails
without the fixture directory; CTest fails on missing or altered contents.
Compiler tests use both LLVM translation units with JIT1/JIT2 fast and softfloat,
JIT1/JIT2 CLI/API artifacts, and LLVM O2 CLI artifacts. Correct object hashes are
required; `--version` alone does not establish compiler functionality.

CTest setup fixtures compile artifacts afresh before dependent jobs. Identity
checks bind artifacts to the current Wasm, compiler and test driver. Inputs,
outputs, commands and diagnostics remain under `tests/validation/results` in
the build directory. Allow up to 60 minutes per large artifact compilation;
`PSIZAM_VALIDATION_COMPILE_JOBS` defaults to four workers. These are correctness
checks, not performance measurements.

`execution` includes both layers when compiler tests are enabled; `quick` and
`compiler` select them separately. No test is marked expected-to-fail, disabled,
or allowed to pass after a crash. Repairs and measured results are documented in
[jit-correctness.md](jit-correctness.md).

The **Standalone library** Actions workflow runs package/quick execution checks
and both source and installed consumer checks on Linux x86-64 and macOS ARM64
for pull requests and pushes. It does not enable compiler-fixture tests.

The separate, manually dispatched **Compiler qualification** workflow runs the
complete execution gate on both architectures. Set the repository variable
`PSIZAM_COMPILER_FIXTURES_URL` to the retained reproduction archive's HTTPS
location, or override it during manual dispatch. The extractor accepts only
the three pinned files and verifies their content before configuration.
No URL or wrong content fails compiler qualification; it does not silently
select a smaller suite. The archive currently exists locally in the benchmark
outputs and still needs an accessible hosted fixture URL. The standalone
source is published at [gofractally/psizam](https://github.com/gofractally/psizam).
A passing package job does not qualify the compiler, spec corpus or stable
release; those gates require their own recorded passing runs.

The quick gate includes 32 deterministic saved-JIT1 compile/execute combinations
per architecture, with gas and backtrace on/off, plus malformed-input rejection.
The full saved scalar/SIMD suites are a separate acceptance run: build with
`PSIZAM_ENABLE_SPEC_TESTS=ON` and `PSIZAM_ENABLE_AOT_SPEC_TESTS=ON`, then use
`scripts/run-aot-spec.py`. It runs LLVM O2 and JIT1 by default, retaining the
unsupported multiple-memory failure. See [aot-spec-tests.md](aot-spec-tests.md).

Runner labels follow the [GitHub runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
WASI SDK 32 downloads and hashes come from the
[official release](https://github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-32).

## Why the earlier validation missed these failures

1. Fresh spec tests constructed interpreter/JIT backends in process. Saved-PZAM
   spec tests explicitly compiled LLVM O2, not JIT2 artifacts.
2. Public compile-API tests loaded artifacts in the same process and mainly
   executed integer addition. Same-process tests can hide serialized helper
   addresses that become invalid after process exit.
3. The functional Clang bootstrap used LLVM O2 PZAM. There was no required
   compiler workload for fresh JIT1/JIT2 or saved JIT2. A load/version smoke
   check passed while actual compilation trapped.
4. `verify-independent.sh` invoked selected test executables rather than every
   CTest entry. It now explicitly includes the quick execution matrix.
5. The inherited Actions workflow referenced absent `.cicd` scripts and gated
   build jobs on forked PRs. It did not provide a working validation pipeline.

The new tests close these specific gaps. They do not establish complete Wasm
conformance. Keep the existing unsupported-proposal and generated-harness
limitations visible, and report architecture, backend, FP policy and artifact
path separately.
