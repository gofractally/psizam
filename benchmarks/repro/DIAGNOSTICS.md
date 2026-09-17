# Findings at psizam 4316638

These findings accompany the benchmark baseline. The benchmark task does not
change engine code. Observed failures and source-based diagnoses are separated.

## CoreMark: basis for the 2.26x Cranelift / LLVM throughput gap

Performance-seed medians from three validated trials, using the identical Wasm:

| Mode | Iterations/sec | Minimum - maximum |
|---|---:|---:|
| Cranelift AOT | 52,329.337449 | 51,856.594110 - 52,344.367057 |
| LLVM O2 PZAM | 23,106.087703 | 22,673.031026 - 23,119.318589 |
| LLVM JIT O2, fast FP | 23,053.315252 | 22,946.859903 - 23,114.355231 |
| JIT2, fast FP | 17,600.904785 | 17,395.556798 - 17,682.147422 |
| JIT1, fast FP | 15,370.449072 | 15,348.705616 - 15,373.715905 |

The timer is CoreMark's internal kernel timer, excluding module compilation,
artifact loading and process startup. Both required seed sets pass. Each engine
uses a calibrated iteration count to exceed ten seconds; the score normalizes
the work by dividing iterations by elapsed time. The repeated-run ranges are
far smaller than the gap. Raw iteration counts, durations, CRCs, flags and
output are retained in `coremark.json` and the corresponding stdout logs.

The near-equality of fast LLVM JIT and canonical LLVM PZAM is evidence against
artifact loading or NaN canonicalization being the dominant explanation here.
This comparison measures psizam's lowering and runtime integration as well as
LLVM code generation. It is not a general claim about LLVM versus Cranelift.

A separate matched-policy control enabled Cranelift NaN canonicalization:
55,194.573367 iterations/sec median (55,178.813620-55,368.526610), from three
performance and three validation trials, all valid and longer than 13.9 seconds.
That is 2.39x LLVM PZAM throughput, so matching this policy does not remove the
gap. Its 369,096 instructions and 82,684 cycles per iteration are close to the
original Cranelift profile. The separate batch and uncontrolled CPU frequency
prevent attributing its approximately 5% higher score to canonicalization.

macOS `/usr/bin/time -l` also retained process-wide hardware counters. Median
instructions and cycles per performance-seed iteration were:

| Mode | Retired instructions | Cycles | Instructions/cycle |
|---|---:|---:|---:|
| Cranelift AOT | 369,142 | 83,043 | 4.45 |
| LLVM O2 PZAM | 676,167 | 187,978 | 3.60 |

Thus LLVM PZAM executes about 1.83x the instructions and uses about 1.24x the
cycles per instruction, yielding about 2.26x the cycles per iteration. These
are whole-process counters, not per-function attribution. Time outside the
CoreMark internal timer was below 0.1% of process wall time for every one of
these precompiled trials. This supports investigating instruction generation
and runtime overhead; it does not identify which helper or pass causes the gap.

Source-based investigation targets, pending profiling and controlled variants:

- `src/llvm_ir_translator.cpp:5515` manually constructs a reduced optimization
  pipeline. All nonzero tiers take that pipeline; it is not the standard LLVM
  `default<O2>` pipeline. It includes mem2reg, SROA, EarlyCSE, InstCombine,
  SimplifyCFG, Reassociate, GVN, LICM and ADCE, with cleanup, but no general
  interprocedural inlining pipeline. The opt level separately selects the
  backend code-generation level in `src/llvm_aot_compiler.cpp:200`.
- The lowering of direct Wasm calls includes external call-depth helpers
  (`src/llvm_ir_translator.cpp:2056`). Wasm globals also use helper calls
  (`src/llvm_runtime_helpers.cpp:46`). These calls can impose both dispatch
  overhead and restrictions on optimization across calls.
- An initial source-level suspicion about indirect list comparators was
  checked against the actual Wasm and is not supported for its hot kernel.
  The linked benchmark has no comparator-signature `call_indirect`; Clang O3
  inlines/specializes those paths in `core_bench_list`. The standalone generic
  mergesort has an indirect call in its object file but is discarded when
  linking. Retained disassembly distinguishes the hot kernel from indirect
  calls used by libc I/O. Do not attribute the gap to comparator dispatch.
- AArch64 AOT creates a TargetMachine with an empty CPU string and
  `+reserve-x18`, not an explicit Apple M5 tuning target
  (`src/llvm_aot_compiler.cpp:213`). The benchmark does not yet isolate CPU
  tuning from lowering or runtime costs.

These are code-supported explanations to test, not a numerical attribution of
the measured gap. Profiling and same-input ablations are needed before claiming
that a particular pass or helper accounts for a specified fraction of it.

## Saved ARM JIT2 PZAM: FP policy override and process-local helpers

Observed: saved JIT2 artifacts trap for nbody, spectral-norm, scalar/SIMD GEMM,
scalar/SIMD Jacobi, and CoreMark calibration. In-memory JIT2 with explicit
`fp_mode::fast` passes those workloads. The retained stderr reports JIT faults
and control-flow corruption or stack overflow; the diagnostic text by itself
does not establish actual stack exhaustion.

Source evidence:

1. `tools/pzam_compile.cpp:32` defines `compile_options` with no explicit `fp`.
2. `include/psizam/detail/parser.hpp:220` defaults FP to softfloat in builds with
   softfloat enabled. This baseline enables softfloat support.
3. `parser.hpp:3385` forwards that value through `code_writer.set_fp_mode`.
4. `include/psizam/detail/ir_writer.hpp:465` first chooses fast FP for an AOT
   compile result with `softfloat=false`, then `ir_writer.hpp:473` overrides it
   with the parser-requested value.
5. `include/psizam/detail/jit_codegen_a64.hpp:4211` emits the softfloat binary
   helper's absolute pointer via `emit_mov_imm64`, then calls it using `BLR X8`.
   That immediate emitter does not record a helper relocation. The comment at
   `ir_writer.hpp:457` itself describes why these raw pointers are unsafe for AOT.

Diagnosis: this path can serialize process-local softfloat helper addresses
while the CLI's requested/default artifact policy says otherwise. It is a
strong explanation for the observed saved-artifact failures, but a minimized
regression test and a verified repair are still required. CoreMark's integer
kernel eventually uses FP for timing/reporting, so its calibration failure does
not rule out this cause. Merely changing the scalar FP selection will not fix
unconditionally emitted SIMD helper calls.

Reproduce from the kit after building the engine (one shell command per line):

```sh
pzam-compile --target=aarch64 --backend=jit2 --opt-level=2 -j1 evidence/modules/nbody.wasm -o nbody.pzam
pzam-run nbody.pzam
```

## ARM FP SIMD still calls softfloat with fast FP requested

The measured runner explicitly sets fast FP. Nevertheless,
`detail/aarch64.hpp:4079` emits the f64x2 add helper, and
`detail/jit_codegen_a64.hpp:3852` emits `simd_v128_binop_softfloat` for f64x2 add.
The helpers in `detail/softfloat.hpp:904` and nearby use Berkeley softfloat.
LLVM emits hardware vector arithmetic (with canonicalization for AOT).

This is an implementation difference that affects interpretation of the very
large JIT1/JIT2 SIMD gaps. It is not a sampled CPU profile, so the report does not
attribute an exact fraction of runtime to those helper calls.

## LLVM AOT has a different FP policy from the in-process LLVM JIT

`detail/ir_writer_llvm_aot.hpp:167` selects `hw_deterministic` when softfloat is
disabled. The benchmark in-process LLVM JIT explicitly selects fast FP.
The AOT baseline is therefore compared with Cranelift using
`-W nan-canonicalization=y`; fast JIT results use default-FP Cranelift.

Policy matching is not identical implementation: LLVM's deterministic mode
also routes payload-sensitive float promotion/demotion through helpers
(`src/llvm_ir_translator.cpp:3414`). Other runtime and sandbox differences remain.

## Timeout policy is not a compiler failure diagnosis

The first single-worker Clang LLVM AOT attempt used a 900-second watchdog. The
user clarified that Clang compilation can require up to 45 minutes. That
attempt is retained as censored data, and the retry allows 3,600 seconds with
one worker. That retry completed in 910.2078 seconds. A watchdog expiration is
not evidence of deadlock or an engine bug.

## Fresh JIT1/JIT2 Clang compilation traps

Both fresh-Wasm benchmark backends trap on SipHash.cpp and APInt.cpp with
`jit control-flow corruption or stack overflow`. A separate diagnostic repeats
the SipHash command using the regular `psizam-wasi --backend=jit|jit2` tool;
both fail with the same message. Both tools/backends successfully execute
`clang.wasm --version`. This narrows the failure to the compiler execution
workload, rather than an inability to load or start the module. The benchmark
runner requests fast FP; the regular runner uses its default FP configuration.

This is distinct from saved-JIT2 artifact relocation: these fresh backends do
not load a PZAM artifact. Root cause has not been isolated. The diagnostic text
alone does not establish stack exhaustion. The report excludes these failed
pairs rather than presenting their short wall times as useful compilation.
Commands and outputs are retained in `clang/diagnostics.json` and `logs/`.

## Parallel compilation control added to the bar-chart report

The original baseline forced `-j1` and disabled Cranelift parallel compilation.
It did not measure the LLVM CLI default, which selects hardware concurrency
(`tools/pzam_compile.cpp:177`). The new large-compiler trials give both engines
4 and 18 requested workers. Cranelift's Rayon worker limit is set explicitly,
not inherited from the original one-worker `Run` environment.

Thread count is also an artifact-strategy choice. The serial LLVM writer emits
one LLVM module per function. With more than one worker it switches to batches
of 1,024 functions (`include/psizam/detail/ir_writer_llvm_aot.hpp:39,135`), capping
the worker count at the number of batches. This can change code, artifact size
and runtime performance, even when a small module fits into one batch. Thus a
speedup over `-j1` must not be attributed entirely to extra CPU cores.

`parallel_compile.py` measures complete CLI creation, retains hashes and RSS,
and verifies both frozen compiler jobs for every generated artifact.
`parallel_runtime.py` compares fresh single-worker and parallel/batched artifacts
for the eight kernels and CoreMark, with matched NaN policy. The report keeps
these new measurements separate from the original fast-FP and serial batches.
See `clang/parallel-compilation.json` and `parallel-runtime.json` for samples;
no engine implementation was changed for these controls.
