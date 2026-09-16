# Compiler bootstrap gate

The required chain is LLVM C++ source → clang.wasm and wasm-ld.wasm → clang.pzam
and wasm-ld.pzam → compilation of the same LLVM C++ source back to Wasm. A
`--version` check or hello world alone does not satisfy this gate.

## Inputs

- gofractally/llvm-project revision
  `5e48a2978cb565ff30b72b7671588fbd0139d55c` (LLVM 22.1.2 fork), with only
  `cmake/llvm-wasi/patches/0001-wasi-close-result.patch` applied. The driver
  checks the exact revision and source diff and records the patch hash.
- WASI SDK with Clang, libc/libc++, compiler-rt, llvm-ar and llvm-ranlib.
- Native matching llvm-tblgen and clang-tblgen, CMake, Ninja and Python 3.11+.
- Independently built psizam tools with the LLVM backend enabled.

```sh
git -C /path/to/llvm-project worktree add --detach /path/to/llvm-bootstrap \
  5e48a2978cb565ff30b72b7671588fbd0139d55c
git -C /path/to/llvm-bootstrap apply \
  /path/to/psizam/cmake/llvm-wasi/patches/0001-wasi-close-result.patch
python3 scripts/bootstrap-clang.py \
  --llvm-source /path/to/llvm-bootstrap --wasi-sdk /path/to/wasi-sdk \
  --engine-bin /path/to/psizam-build/bin \
  --llvm-tblgen /path/to/llvm-tblgen --clang-tblgen /path/to/clang-tblgen \
  --work /path/to/bootstrap-work --jobs 4 --aot-jobs 4
```

`--through wasm` stops after native compilation to Wasm. `--through pzam` also
converts the tools and compiles/links/runs a C++ standard-library probe. The
default continues to build LLVM itself with those PZAM tools, converts the
resulting compiler and linker, and uses them for the same probe.

The work directory retains every command, log, input hash and output hash in
`manifest.json`. It marks `complete` only after the final probe passes.
`--reuse-stage0-wasm` and `--reuse-stage0-pzam` accept existing tool directories
for diagnostic continuation; retain their original build records as well.
Engine executables are copied into the work directory before hashing so a
concurrent development build cannot silently replace them. Workstation include
paths and compiler flags are cleared; locale, time zone and source-date epoch
are fixed and recorded. `--aot-opt-level` defaults to 2; level 0 is available
for diagnostics.

Stage 1 uses `pzam-clang-driver.py`: the guest compiler generates its own job
plan, then the host wrapper executes cc1 inside clang.pzam and link jobs inside
wasm-ld.pzam. It never invokes a native C/C++ compiler for stage 1 compilation.
Native table generators, archive tools and build orchestration remain allowed.
Compiler caching is disabled. Guest preopens are explicitly recorded in the
generated driver configuration.

## Checkpoint, September 16, 2026

**The full functional bootstrap passed on macOS ARM64.** The patched LLVM source
builds into Wasm; LLVM AOT O2 converts both tools into PZAM; those PZAM tools
compile and link the same LLVM C++ sources into new clang.wasm and wasm-ld.wasm.
The new tools convert to PZAM, report the expected revision, and compile/link
a C++ `<vector>` / `<cstdio>` program whose result executes and prints `42`.
Every recorded command exits successfully and the manifest has `complete: true`.

Original work directory: `/tmp/psizam-stable-work/bootstrap-contained`.
Its `manifest.json` records commands, results and hashes. The originating
native source build is `/tmp/psizam-stable-work/bootstrap-closefix/llvm-stage0`;
its log is `/tmp/psizam-stable-work/bootstrap-closefix/build-stage0.log`.
Durable tools, Wasm/PZAM artifacts and evidence are retained at
`/Users/dlarimer/gofractally/psizam/build/bootstrap-2026-09-16/`.
The psiserve `outputs/release-work-2026-09-16/psizam/` directory also retains
the manifests and logs. The retained-artifact manifest includes the final PZAM
hashes, which the original bootstrap command manifest did not enumerate.

| Source-built artifact | Bytes | SHA-256 |
|---|---:|---|
| clang.wasm | 50,902,063 | `644767c9f9004dc2ae3cdfc5e552978e82409100beb8a9407a9f180d582eaf3c` |
| wasm-ld.wasm | 29,715,283 | `2dddbc193113b4490f76e539d708eb52f3a5b6eb94effb286a0ef3b17ba82bbd` |
| Self-built clang.wasm | 85,486,403 | `5e29ae625290c8642222f036e957a7803e4a075e6f4550ea28a458498ca00f2a` |
| Self-built wasm-ld.wasm | 51,694,563 | `ae5e9a95a0d91a380e79aec2ec2e1271c58ba152d52b70c1dc72121ad9755360` |

On this local run, LLVM O2 AOT took about 185 seconds for Clang and 113 seconds
for LLD with six workers. Compiling and linking the C++ probe took 1.31 seconds.
These are diagnostic timings on this Mac, not portable release benchmarks.
The manifest holds the exact PZAM hashes and tool/sysroot inputs.

The guest LLVM source build took 3,379.51 seconds (about 56 minutes), including
single-threaded ThinLTO links of about 14 minutes for LLD and 19 minutes for
Clang. Converting the self-built tools to PZAM took 171.73 and 95.35 seconds;
their final C++ compile/link probe took 1.30 seconds and executed successfully.
Native CMake/Ninja, table generators, archive tools, SDK libraries and the
native PZAM compiler remain bootstrap inputs; the C/C++ compilation and links
of LLVM itself ran through the guest tools.

This establishes a **functional bootstrap, not a bit-identical fixed point**.
Stage 0 and stage 1 Wasm hashes differ. The new Clang contains a 24,440,577-byte
name section absent from stage 0, and its code section is also larger. A CMake
`HAVE_LIBDL` probe differs between the native SDK and guest tools. These are
recorded in `wasm-size-comparison.json` and `configuration-differences.json`;
a further bootstrap stage and normalized build settings are needed to establish
a reproducible fixed point. Portable acquisition of a pinned SDK bundle also
remains release work.

The O2 spec run subsequently found a compiler thread-pool missed-wakeup race.
The synchronization fix passes its regression and the full saved-PZAM SIMD
suite; scalar has one existing multiple-memory failure. Recompiling both large
stage-0 PZAM tools with the repaired compiler produces identical artifact hashes.
The original bootstrap preserves its frozen compiler for provenance; the
retained local toolchain supplies the repaired compiler for subsequent work.
See [AOT spec tests](aot-spec-tests.md).

## Defects repaired during bootstrap

- LLVM AOT now applies the requested optimization level to native code
  generation as well as IR optimization. O0 still promotes synthetic virtual
  registers to SSA, and ARM64 O0 uses SelectionDAG to avoid the measured
  GlobalISel CSE bottleneck.
- Large ARM64 code images use branch trampolines per compilation batch.
  Relocation application rejects out-of-range or misaligned BL/B targets
  instead of silently truncating their displacements. Both compiler tools
  now execute with zero branch-relocation overflows.
- The Wasm toolchain explicitly reserves a 16 MiB guest stack. The linker
  default of 64 KiB caused the earlier C++ compiler memory trap.
- The pinned LLVM patch fixes WASI `Process::SafelyCloseFileDescriptor`, which
  returned stale `errno` after successful closes. A regression linked against
  the actual LLVM Support archive reproduces failure with the original code
  (`close success=28`) and passes with the patch (`close success=0`). Repeated
  closes still report error 8 in both versions. The original LLVM checkout is
  unchanged; the patch was applied in a separate worktree.
- WASI path resolution accepts parent components whose resolved target stays
  inside the preopen. This permits LLVM's `llvm/../third-party` include path.
  Tests still reject parent/symlink escapes, sibling-prefix matches, absolute
  inputs, embedded NULs and closed descriptors. The previously failing
  `SipHash.cpp` compilation passes through PZAM after this fix.

The bootstrap disables the inherited mimalloc archive: its `MI_OVERRIDE=OFF`
setting did not replace libc allocation. Any future claim that mimalloc solves
WASM32 memory pressure requires a working replacement and a measured test.
