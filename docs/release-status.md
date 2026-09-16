# Independent engine release checkpoint

September 16, 2026; macOS ARM64, Homebrew LLVM 22.1.2. This is a local,
unpublished checkout. See [extraction.md](extraction.md) for source provenance
and [compiler-bootstrap.md](compiler-bootstrap.md) for the compiler chain.

## Verified

- Independent source configuration, installed-package consumption, and a
  combined source consumer with the independent psio checkout pass.
- 320 unit cases / 1,964 assertions and 40 runtime cases / 160 assertions pass
  in the build without the optional LLVM backend.
- 68 focused compliance cases / 360 assertions pass after repairing the
  differential harness's tagged-value comparison, ARM relocation bounds, and
  contained parent-directory resolution in the WASI host.
- An installed LLVM-enabled consumer executes a Wasm function returning 42.
- An AArch64-only LLVM build passes; the optional architecture targets share
  one implementation library rather than duplicate LLVM initialization code.
- The pinned LLVM C++ sources build into clang.wasm and wasm-ld.wasm. Their
  LLVM O2 PZAM versions rebuild those LLVM sources into new compiler tools.
  The new tools convert to PZAM and compile/link a C++ program that runs and
  prints 42. The full functional bootstrap passes; see its separate record.

The executable commands are in `scripts/verify-independent.sh` and
`scripts/bootstrap-clang.py`. The independent smoke script explicitly disables
the spec suite; it is not a complete WebAssembly conformance gate.

## Which backend these results exercise

| Check | Backend / artifact path |
|---|---|
| Compiler bootstrap | LLVM AOT O2, AArch64, PZAM version 3, executed by `pzam-run` |
| Fresh scalar and SIMD spec suites | Interpreter, JIT1 and JIT2; totals aggregate all three |
| Dedicated saved-PZAM scalar/SIMD suites | LLVM AOT O2, real compiler CLI, file serialization and shared PZAM loader |
| Focused differential tests | Interpreter, JIT1 and JIT2 |
| Installed LLVM consumer | LLVM backend, small Wasm function returning 42 |
| Psio language/package tests | Native C++, Rust and Node.js |

The ordinary fresh spec build has `PSIZAM_ENABLE_LLVM=OFF`. The dedicated AOT
executables add O2 PZAM round trips without enabling the in-process LLVM JIT.
The separate ORC/in-process LLVM JIT path remains outside these spec results.
The historical bootstrap's O2 setting is established by its recorded commands;
its saved artifacts retain the old O3 label. Newly generated PZAM sections now
carry the actual LLVM optimization tier, including empty modules.

## Fresh spec-suite result

With testsuite commit `51279a9d02cbba193cb25142d115388d7b83299c`, the freshly
built non-SIMD executable now runs 9,153 cases: 9,150 passed and three failed
(71,529 / 71,532 assertions passed). The three reject a multiple-memory
`memory.grow` immediate. The earlier 99 missing-fixture failures are repaired:
comment inputs are parsed without the lossy text filter when possible, and
tail-call assertions/fixtures are regenerated from the current testsuite root.
The generated harness still omits some test scenarios, and `table_init` still
falls back to checked-in assertions after a fixture-generation warning. Its
totals do not establish full specification coverage.

The freshly built SIMD executable passes all 3,447 cases and 75,219 assertions.
This supersedes the SIGILL observed with the old checkout's stale binary;
it is a result for this pinned generated suite, not a claim that the harness
covers every specification scenario.

Evidence: `/tmp/psizam-stable-work/spec-fixture-repair-tests.log`,
`spec-fixture-repair-build.log`, and `fresh-simd-{build,tests}.log`.
Copies are retained under the psiserve `outputs/release-work-2026-09-16/psizam`
directory. These fresh results supersede assumptions drawn from stale binaries
in the original psiserve checkout.

The dedicated **LLVM AOT O2 PZAM** run passes 3,050 of 3,051 scalar cases
(23,843 / 23,844 assertions), with the same unsupported multiple-memory case.
It passes all 1,149 SIMD cases and 25,073 assertions. These counts are for AOT
alone. The first run found a compilation-pool missed-wakeup deadlock; phase
changes now hold the wait mutex. A regression reproduces the old deadlock and
the repaired implementation passes 18,000 batches across 90 pool lifetimes.
See [aot-spec-tests.md](aot-spec-tests.md) for commands, limits and evidence.

## Release gates still open

1. Extend the successful functional bootstrap with broader compiler acceptance
   tests and a normalized, repeated build to establish a byte-identical fixed
   point. Pin and distribute
   the sysroot/resource inputs; replace placeholder prebuilt hashes only with
   real, verified artifacts.
2. Finish the spec harness cleanup, generate into the build tree, and define
   supported proposals/backends. Unsupported modules must be rejected
   consistently. Add SIMD coverage to the platform matrix; recheck JIT2
   compiler-sized PZAM execution and the
   LLVM high-instance-count stress test under explicit budgets.
3. Strengthen compiler identity beyond the format version and architecture so
   cache acceptance reflects source/options/ABI compatibility. The public
   `pzam_compile()` dispatch and CLI tier labels are now corrected; see
   `pzam-compile-api.md`. Historical artifacts were not rewritten.
   Separately, `runtime::load_cached()` creates a loaded PZAM module, but
   `runtime::instantiate()` still calls the ordinary Wasm factory with an empty
   `wasm_copy`. A direct regression attempt raises `guarded ptr out of bounds`.
   Wire that instantiate path to the loaded code before advertising cached
   execution through this particular runtime API. The shared PZAM loader and
   CLI execution paths do execute the new compilation results successfully.
4. Review and selectively recover agent2's zero-copy PZAM loader optimization;
   it has not been merged into this checkout. PZAM metadata and component APIs
   now use current psio; the WIT port preserves the existing calling convention.
5. Run the independent package and execution matrix on Linux/x86_64 as well
   as ARM64. Establish immutable dependency versions, license/notice files,
   public tags and release artifacts. No publishing has occurred here.
