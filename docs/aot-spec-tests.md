# LLVM O2 saved-PZAM specification tests

The ordinary spec executables instantiate interpreter/JIT backends directly.
The dedicated AOT executables instead run each generated assertion through:

```
pinned spec .wasm
  -> pzam-compile --backend=llvm --opt-level=2 --target=<host> -j6
  -> saved .pzam file
  -> pzam_validate / pzam_load / shared detail::load_pzam
  -> LLVM native-code execution and the original assertion
```

This uses the CLI's LLVM AOT compiler, not the in-process LLVM JIT or the
multi-tier `pzam_compile()` convenience API, which now has its own regression tests. The test implementation
is in `tests/aot_spec_backend.{hpp,cpp}`. Generated assertion sources are shared
with the ordinary suites; the test backend is the only substitution. There is
no JIT fallback. Invalid-module checks still test compilation rejection.

```sh
cmake -S /path/to/psizam -B /path/to/test-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/psio-install \
  -DPSIZAM_ENABLE_TESTS=ON -DPSIZAM_ENABLE_SPEC_TESTS=ON \
  -DPSIZAM_ENABLE_AOT_SPEC_TESTS=ON
cmake --build /path/to/test-build --parallel 8 --target \
  psizam_scalar_aot_o2_tests psizam_simd_aot_o2_tests
python3 scripts/run-aot-spec.py \
  --test-bin /path/to/test-build/bin \
  --compiler /path/to/llvm-enabled-build/bin/pzam-compile \
  --work /path/to/fresh-aot-results
```

The test build itself need not enable LLVM; the external compiler must. The
runner freezes a copy of that compiler, hashes it and both test executables,
and retains commands, diagnostics, Wasm inputs and successful PZAM outputs in
separate directories for scalar and SIMD. It rejects a nonempty work directory.
Compilation runs on every construction to avoid stale-cache success.

The CLI and C++ API now share code-section construction with an explicit backend
and tier. Tests require `psizam-llvm` and `llvm_O2` even for empty modules, in
addition to recording the actual compiler command. Historical artifacts still
contain their original labels.

Coverage is limited by the existing generated harness: omitted scenarios and
compatibility-filtered inputs remain omitted here too. An AOT pass must be
reported separately from interpreter/JIT totals and cannot certify missing
tests, untested architectures, deterministic softfloat or all Wasm proposals.

Current run: macOS ARM64, PZAM version 3, native floating point, O2, six compiler
workers. The scalar/SIMD/trap/empty-module smoke test passes four assertions.

| Suite | Cases | Assertions | Result |
|---|---:|---:|---|
| Scalar | 3,050 / 3,051 pass | 23,843 / 23,844 pass | One unsupported multiple-memory `memory.grow` case |
| SIMD | 1,149 / 1,149 pass | 25,073 / 25,073 pass | Pass |

The repaired-compiler run is recorded at
`/tmp/psizam-stable-work/aot-spec-correct-tiers/manifest.json`.
This fresh run uses the corrected compiler labels and the stricter metadata
checks; its scalar/SIMD counts are unchanged.
Its overall gate remains failed because the scalar failure is retained, not
silently excluded. Counts aggregate no other backend.
The logs and manifest are also retained under psiserve's
`outputs/release-work-2026-09-16/psizam/`; complete test artifacts and executables
are retained in this checkout's `build/bootstrap-2026-09-16/aot-spec-o2/`.

The first SIMD run exposed a missed-wakeup deadlock in `compile_reactor` on a
50-byte constant-vector module. The coordinator and worker were both waiting
on condition variables. Phase changes now hold the wait mutex before notifying.
The original reactor reproduces a deadlock under the regression workload; the
repaired reactor passes 18,000 two-phase batches across 90 pool lifetimes.
The subsequent full O2 SIMD run finishes in about 17 seconds and the scalar
run in about 41 seconds on this Mac. These are diagnostic timings, not a
cross-machine benchmark.

The repaired compiler also regenerates both large stage-0 Clang and LLD PZAM
artifacts byte for byte identically to the earlier compiler. That checks this
synchronization repair on the compiler-sized workload as well as tiny spec
modules. Evidence: `reactor-fixed-large-aot/manifest.json` in the retained logs.
