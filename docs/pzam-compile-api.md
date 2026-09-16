# PZAM compilation API correction — September 16, 2026

`pzam_compile(wasm, options)` now compiles each requested architecture/tier
combination separately. Both x86-64 and ARM64 use their respective JIT2 writer;
LLVM O1/O2/O3 requests select `ir_writer_llvm_aot`, the correct target triple,
and the requested optimization level. LLVM must be enabled by linking the
`psizam-llvm` target. The default remains host JIT2.

Each section takes its bytes, function offsets and relocations from its own
compilation. Relocation sites are canonicalized before serialization. The CLI
and C++ API share section construction, which uses explicit compiler identity
and tier rather than guessing LLVM from a nonempty code blob. Consequently an
empty LLVM module retains the correct LLVM identity and tier.

Unsupported requests throw `std::invalid_argument` before compilation:

- Empty or unknown architecture/tier flags.
- JIT1 PZAM output (including a bitmap that requests JIT1 alongside other tiers).
- Gas-meter or debug-info flags, whose emitters are not implemented here.
- Softfloat on any requested JIT2 section; softfloat is supported with LLVM.
- LLVM tiers in a build without the LLVM backend.

Backtrace remains supported. `pzam_compile_file` also checks stream errors after
writing and closing its output. Target availability errors from LLVM propagate
as compilation failures; a missing target cannot produce a mislabeled section.

The CLI now labels O0/O1/O2/O3 accurately and rejects levels outside 0..3.
`llvm_O0 = 5` is appended to the format's tier enum, preserving all existing tier
numbers. Readers of these newly labeled O0 artifacts must include the O0-aware
loader/runner changes. The container layout and PZAM version remain 3. Existing
bootstrap artifacts retain their original labels and tool provenance.

## Regression coverage

Build and run `psizam_pzam_compile_tests` (CTest name `pzam_compile_api`) with
LLVM enabled and disabled. The tests exercise the public compile API, reload
its output through the shared PZAM loader, and execute host code through the
execution context. They cover both targets, requested tiers, independent versus
combined compilations, explicit rejection, empty modules, backtrace, and
software versus hardware floating-point code. Foreign-architecture code is
generated and compared; execution of x86-64 code still requires an x86-64 host.

On macOS ARM64 the LLVM build passes 74 assertions in four cases; the build
without LLVM passes 30 assertions in three cases. A separate CLI check compiles
16 artifacts (both targets, O0..O3, empty/nonempty), verifies their labels with
`pzam inspect`, and executes all four nonempty host variants. Evidence is under
`/Users/dlarimer/psiserve/outputs/tag-and-pzam-fixes-2026-09-16/`.

The scalar/SIMD AOT O2 test backend now requires the exact O2 metadata even for
empty modules. The separate ORC JIT remains outside that suite.

## Separate release gates

This corrects compile dispatch and section labels. Compiler/cache identity still
needs a stronger source/options/ABI fingerprint. Also, `runtime::instantiate()`
does not yet consume the module loaded by `runtime::load_cached()`; it tries to
parse empty Wasm bytes. That separate runtime API gap was exposed during this
work and remains listed in `release-status.md`. It does not affect the shared
loader execution checks or the CLI bootstrap path.
