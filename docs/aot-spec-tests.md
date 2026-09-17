# Saved-PZAM scalar and SIMD specification tests

The ordinary spec executables instantiate interpreter/JIT backends directly.
Dedicated saved-code executables run the same generated assertions through:

```
pinned spec .wasm
  -> pzam-compile --backend=llvm --opt-level=2 or --backend=jit1
  -> saved .pzam file
  -> pzam_validate / pzam_load / shared detail::load_pzam
  -> native-code execution and the original assertion
```

Each construction launches the real CLI and reloads its output. There is no
in-process JIT fallback. Invalid-module checks require compiler rejection.
Metadata checks require the requested backend, architecture and tier, including
empty modules. Public `pzam_compile()` has separate CLI/API execution gates.
The implementation is in `tests/aot_spec_backend.{hpp,cpp}`.

```sh
cmake -S /path/to/psizam -B /path/to/test-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/psio-install \
  -DPSIZAM_ENABLE_TESTS=ON -DPSIZAM_ENABLE_SPEC_TESTS=ON \
  -DPSIZAM_ENABLE_AOT_SPEC_TESTS=ON
cmake --build /path/to/test-build --parallel 8 --target \
  psizam_scalar_aot_o2_tests psizam_simd_aot_o2_tests psizam_smoke_aot_o2_tests \
  psizam_scalar_jit1_pzam_tests psizam_simd_jit1_pzam_tests psizam_smoke_jit1_pzam_tests
python3 scripts/run-aot-spec.py \
  --test-bin /path/to/test-build/bin \
  --compiler /path/to/llvm-enabled-build/bin/pzam-compile \
  --work /path/to/fresh-saved-spec-results
```

The test build itself need not enable LLVM; the external compiler must support
the selected backend. The default runs scalar, SIMD and smoke for both LLVM O2
and JIT1. Use `--backend jit1` or `--backend llvm` for focused acceptance, and
`--suites smoke` for a wiring check. A smoke pass does not establish spec coverage.

The runner freezes and hashes the compiler, hashes each test executable, and
retains commands, diagnostics, input Wasm and successful PZAM outputs. It rejects
a nonempty work directory. Two suites may run concurrently during correctness
validation; never overlap this run with timed benchmarks. Any suite failure
makes the overall command fail, including an unsupported-proposal failure.

The format is PZAM version 4 with psio pSSZ serialization. Current ARM64 saved
LLVM O2 and JIT1 acceptance results, each reported independently, are:

| Suite | Cases | Assertions | Result |
|---|---:|---:|---|
| Scalar | 3,050 / 3,051 pass | 23,843 / 23,844 pass | One unsupported multiple-memory `memory.grow` case |
| SIMD | 1,149 / 1,149 pass | 25,073 / 25,073 pass | Pass |
| Smoke | 1 / 1 pass | 4 / 4 pass | Pass |

These suites inherit the generated harness's omitted and compatibility-filtered
scenarios. A pass cannot certify missing tests, deterministic softfloat, foreign
architectures or all Wasm proposals. Native ARM64 and x86-64 execution require
separate acceptance runs. The Actions execution gate covers focused saved-JIT1
CLI/API tests and real compiler jobs; the complete saved scalar/SIMD run above
is a separate release acceptance step.
