# Performance gaps and an order for focused optimization

Review date: 2026-09-17. This review uses the completed comprehensive benchmark
on Apple M5 Max and source inspection at `34c2ff4`. It distinguishes measured
gaps, confirmed implementation costs, and untested explanations. No proposed
optimization below has a measured speedup yet.

The execution comparison uses saved LLVM PZAM versus Cranelift AOT. Service-call
comparisons use loaded LLVM PZAM versus warm, cached Wasmtime C API calls;
compilation is outside the call timer. These are scalar boundary tests, not full
database-backed IPC. JIT1/JIT2 are assessed separately as the fast compilation
tiers. All ratios below are **our elapsed time / Cranelift elapsed time**.
Gas and fuel runs use non-exhausting budgets; their accounting units are not
claimed to be equivalent. Results describe this ARM64 host and these fixtures,
not x86 performance or a complete psibase transaction.

## Gaps worth investigating

| Case | Measured gap | Why it matters |
| --- | --- | --- |
| Compile Clang to a saved artifact, 18 workers, no limits | 64.69 s versus 2.13 s; 30.4x time. Peak RSS 7,204 versus 2,075 MiB. | The largest deployment/cache-miss disadvantage. It does not recur on every call to retained code. |
| ARM JIT1/JIT2 vector matrix/grid workloads, no limits | About 20-23x time. Grid: JIT2 PZAM 6.05 s versus Cranelift 0.260 s. | A conspicuous code-generation limitation if services execute SIMD on these tiers. It does not describe LLVM performance. |
| LLVM vector grid update | 1.35x time without limits; 1.37x with gas/fuel. | The largest stable execution deficit in the compute suite; relevant to repeated vector loads and arithmetic. |
| LLVM vector matrix multiplication | 1.15x time without limits; 1.17x with gas/fuel. | A second vector kernel confirms that the grid result is not the only vector-code weakness. |
| LLVM running Clang on APInt | Report medians: 2.341 versus 2.016 s without limits; 3.387 versus 2.469 s with gas/fuel. | A substantial compiler/application workload. Focused checking found two LLVM timing bands at nearly identical instruction counts within each setting. Diagnose that variability before attributing the metered gap. |
| Cached protected service calls | A-to-B: 44.68 versus 31.28 ns; A-to-B-to-A: 67.24 versus 46.81 ns. | Adds about 13.4 ns per two-entry round trip, or 20.4 ns per three-entry round trip. Call frequency determines transaction impact. |
| LLVM Clang process memory and artifact storage | APInt RSS 468 versus 150 MiB without limits. Saved files 135.03 versus 103.08 MiB. | Capacity and distribution costs can matter even when latency is acceptable. RSS is whole-process memory, not isolated engine overhead. |

CoreMark is a smaller gap: approximately 1.058x elapsed time without limits and
1.041x with gas/fuel. LLVM executes about 5.4% fewer retired instructions per
iteration but uses about 5.4% more cycles in the unmetered measurements. That
supports investigating scheduling, dependencies and memory behavior; it does
not support a simple explanation that LLVM emits far more work. LLVM leads
Cranelift in several other compute workloads and in repeated host-query costs.

## First resolve the APInt timing split

An additional focused check interleaved four configurations using a seeded
random order: saved LLVM and Cranelift AOT, each without limits and with gas/fuel.
Each configuration had one warmup followed by five measured process launches.
All 24 outputs matched the expected object hash. The same frozen executables,
artifacts and input were used, with loader profiling disabled.

| Configuration | Five measured elapsed times, sorted (seconds) | Median retired instructions |
| --- | --- | --- |
| LLVM, no limits | 2.258, 2.270, 2.272, **3.073, 3.077** | 33.39 billion |
| LLVM, gas | 2.428, 2.464, **3.243, 3.260, 3.274** | 37.64 billion |
| Cranelift, no limits | 1.870, 1.875, 1.877, 1.878, 1.911 | 41.86 billion |
| Cranelift, fuel | 2.331, 2.360, 2.361, 2.436, 2.456 | 49.35 billion |

LLVM's instruction counts vary by less than 0.1% within each configuration,
while its elapsed time changes sharply. Measured cycles also increase in the
slower runs. Instructions per cycle fall from approximately 3.38 to 2.47 without
limits and from 3.53 to 2.63 with gas. These are whole-process counters, not a
profile of individual guest functions. The gas instruction-count increment is
about 12.7% for LLVM versus 17.9% for Cranelift; instruction counts alone do not
explain elapsed time.

This makes a median-only interpretation of the gas penalty unreliable: three
of the five unmetered LLVM runs occupy the faster band, while three of the five
metered runs occupy the slower band. Selecting only the fast runs would also
be misleading. The observed variability is itself an issue to investigate;
it is not a reason to dismiss the slower results.

Candidate explanations include CPU placement/scheduling and address-sensitive
cache or branch behavior. The present data does **not** identify which applies.
Major page faults were four in every measured LLVM run, page reclaims were
nearly constant, and involuntary context-switch counts did not cleanly separate
the bands. No core-placement or code-address trace was collected.

Before claiming an APInt optimization, record CPU scheduling/frequency and
code/linear-memory addresses, correlate those with per-run counters, and compare
repeated execution in a retained process against fresh-process launches. Retain
the full distribution. A change must improve comparable execution conditions,
not merely change how many samples fall in each band. This diagnostic gate
comes before attributing APInt's gap to gas, helper calls or the LLVM pipeline.

## Order to try changes

This order weighs implementation size, correctness risk, probable reach and
evidence. It is not a promise of speedup; the first item is the cheapest useful
experiment, not the largest expected gain. If fast-tier SIMD is an actual product
requirement, move item 4 to the front. If deployment latency is the bottleneck,
start the profiling work in item 7 first.

| Order | Candidate | Effort and confidence | Focused acceptance experiment |
| --- | --- | --- | --- |
| 1 | Mark LLVM gas exhaustion as an unlikely slow path; inspect resulting block layout and spills. | Small experiment, low semantic risk. Missing branch weights are confirmed; speedup is unproven. This is not a confirmed explanation of APInt's timing split. | A stable call-dense integer kernel, exact gas/yield/exhaustion checks, then APInt after the diagnostic gate. |
| 2 | Lower simple globals and direct-call depth bookkeeping into LLVM IR, retaining out-of-line trap handling. | Small-to-medium implementation. External helper calls are confirmed; their hot-path share needs measurement. | APInt, CoreMark and a small repeated-call/global kernel; compare instructions, execution time and relocations. |
| 3 | Add a protected callable specialized at binding, with cached signature, entry and return adaptation. | Medium. Strong evidence of per-entry overhead; no guarantee that metadata checks explain all of it. | One entry, A-to-B, A-to-B-to-A; retain a trap boundary at every entry and zero hot-path C++ allocations. |
| 4 | Emit native ARM vector add/subtract/multiply for hardware FP modes in JIT1/JIT2. | Medium; strongest code-supported explanation for an extreme execution gap. Begin with the few hot operations, not the entire SIMD instruction set. | GEMM/Jacobi, scalar and SIMD specs, deterministic FP edge cases, saved-code reload. |
| 5 | Compare full LLVM O2 and explicit CPU tuning against the custom pipeline. | Moderate experiment; execution benefit unknown and compilation cost may rise. These are separate variables. | Vector grid/matrix, CoreMark, then APInt; track execution, compile time, RSS and artifact size. |
| 6 | Remove redundant PZAM validation/copies and shorten temporary-buffer lifetimes; then reduce avoidable relocations. | Small cleanup followed by medium ownership/format work. Memory opportunity is clearer than a large runtime gain. | Load/instantiate-only timing and RSS, multiple cached instances, malformed inputs and lifetime tests. |
| 7 | Profile and reduce LLVM artifact-preparation work. | Medium-to-large investigation; the 30x gap is measured but its phase breakdown is not. | One Clang artifact, fixed grouping, 4/18 workers, phase and peak-memory measurements. |
| 8 | Develop less restrictive LLVM memory lowering while preserving every required trap. | Large and correctness-sensitive. Volatile accesses are confirmed optimization barriers. | Minimal OOB/dead-load/reordering cases, vector kernels, full scalar/SIMD validation. |

### 1. Gas slow-path layout: a cheap experiment

`emit_gas_prologue_check` emits an atomic deadline load, counter update,
comparison, conditional branch and returning exhaustion helper. Its branch has
no probability metadata and the exhaustion helper is not declared cold. Nearby
bounds-check code already uses branch weights. The likely opportunity is better
hot-block layout and fewer spills around an uncommon callback. LLVM can infer
some branch behavior itself, so the missing metadata is not proof of a defect.

Do not remove checkpoints, make the deadline non-atomic, or describe a handler
as nonreturning: refilling/yield handlers can return. Confirm finite budgets,
interrupts, replenishment and exception cleanup alongside timing.

Sources: [gas lowering](../src/llvm_ir_translator.cpp), function
`emit_gas_prologue_check`; [runtime gas helpers](../src/llvm_runtime_helpers.cpp).

### 2. Avoid opaque calls for simple runtime operations

LLVM scalar `global.get` and `global.set` call C++ helpers. Direct Wasm calls
also have separate depth-decrement/increment helpers. These helpers perform
small state accesses but remain external declarations in the LLVM module.
That introduces calls and can prevent the optimizer from retaining state
across them. A full module pipeline cannot inline bodies it does not have.

Start with constant, validated global indices and direct depth accounting in
IR. Preserve current overflow boundaries, imported/shared-global behavior,
exception restoration and observable host changes. Keep the slow trap helper.
Measure each change independently; do not attach unverified memory-effect
attributes to helpers that can trap or update runtime state.

Sources: [global/call lowering](../src/llvm_ir_translator.cpp), cases
`global_get`, `global_set`, and direct-call handling;
[helper bodies](../src/llvm_runtime_helpers.cpp), `__psizam_global_get`,
`__psizam_global_set`, `__psizam_call_depth_dec`, `__psizam_call_depth_inc`.

### 3. Protected entry: remove stable work, preserve dynamic protection

The single-entry difference is 20.85 - 14.01 = 6.85 ns. Twice that is 13.69 ns,
close to the two-entry round-trip gap of 13.40 ns; three times is 20.54 ns,
close to the three-entry gap of 20.42 ns. This is a useful localization clue,
not a causal decomposition: the tests also differ in guest/host work.

The absolute cost matters: 100 entries at the measured 6.85 ns difference add
approximately 0.69 microseconds. That is arithmetic illustrating scale, not a
measured transaction. Large percentages on a tiny boundary test do not imply
large database-backed request slowdowns. Conversely, very dense IPC loops can
make the same cost worth removing. Repeated host queries already favor LLVM
in this suite (1.77 versus 3.37 ns without limits).

psizam's indexed call still retrieves function metadata, checks argument types,
marshals slots, dispatches on backend/return shape, manages an entry-stack lease
and saves/restores trap state. Wasmtime's cached comparison checks its signature
at binding. A bound protected entry can specialize the stable parts. The public
runtime already caches names and adapters: repeating that optimization is not
the proposal. TLS range switching and trap setup need separate profiling.

Do not compare an unprotected typed function or one trap scope around a million
calls against independently protected IPC entries. Keep A-to-B-to-A reentry,
memory growth, host exceptions, expiration, gas exhaustion and trap recovery.

Sources: [indexed backend call](../include/psizam/backend.hpp), `call_by_index`;
[execution entry](../include/psizam/detail/execution_context.hpp), `execute`;
[signal scope](../include/psizam/detail/signals.hpp), `invoke_with_signal_handler`.

### 4. ARM fast-tier SIMD: a concrete implementation limitation

ARM JIT1/JIT2 route common vector FP arithmetic through softfloat helpers.
JIT2's binary adapter loads vector halves into general registers, calls an
external function, adjusts its operand stack and writes the result back.
Native NEON arithmetic can remove both the software arithmetic and this
per-operation call/stack traffic. Integer SIMD already has native-emission
helpers that provide a structural starting point.

Implement the common add/subtract/multiply operations first, selected by FP
mode. Keep software mode as the reference and retain NaN, signed-zero,
subnormal and rounding requirements in deterministic hardware mode. Defer
min/max, conversions and other edge-heavy operations until the initial change
is independently measured and validated. This can substantially improve the
fast tiers; it does not directly improve the already-native LLVM vector path.

Sources: [JIT2 SIMD dispatch and adapters](../include/psizam/detail/jit_codegen_a64.hpp),
`f32x4_add`, `f64x2_add`, `simd_v128_binop_softfloat`;
[JIT1 SIMD dispatch](../include/psizam/detail/aarch64.hpp), `emit_f32x4_add`.

### 5. LLVM pipeline and CPU tuning: test, do not assume

The current custom IR pipeline omits general inlining, multiple loop transforms
and vector optimizations. The AArch64 AOT TargetMachine also uses an empty CPU
string, rather than an explicit tuning target. Its target/layout setup occurs
after translator optimization. A fair full-O2 experiment should establish the
target before running the passes and preserve the same FP/memory/gas policies.

Change pipeline and CPU tuning separately. Hold function grouping constant:
the single-worker path currently emits one function per LLVM module, while
parallel AOT groups 1,024 functions. Cross-function optimization would make
that distinction especially significant. Preserve callable entry/body symbols
and relocation contracts. Full O2 cannot overcome opaque external helpers or
the semantic restrictions of volatile memory operations by itself.
Any CPU-specific artifact must record and enforce its required target features;
host-specific tuning must not silently narrow a portable artifact's contract.

Sources: [IR pipeline](../src/llvm_ir_translator.cpp), `finalize`;
[AOT TargetMachine](../src/llvm_aot_compiler.cpp), `llvm_aot_compile`;
[batching](../include/psizam/detail/ir_writer_llvm_aot.hpp).

### 6. PZAM loading and storage: improve capacity without overstating latency

The CLI explicitly validates and then calls `pzam_load`, which validates again.
It retains a mapped file, decodes owned code/metadata, creates another relocation
vector and copies code into executable memory. The library cache already parses
the container once per module; its next opportunities are shared immutable
representations and temporary-buffer lifetimes, not reparsing on every call.

Remove only redundant validation: one validated boundary remains mandatory.
Consider borrowing validated pSSZ code bytes, applying relocations without an
intermediate representation where practical, and releasing dead input/code
buffers. Sharing executable mappings across instances is a later design change
because code-page interruption has ownership implications.

The LLVM Clang artifact has 2,803,016 relocations and 59.03 MiB outside its
76.00 MiB native region. Not all outside bytes are relocation overhead. Classify
the entries before changing the representation. ARM AOT already uses the Small
code model and resolves same-batch PC-relative calls early; those optimizations
are not missing fixes.

Sources: [CLI loading phases](../tools/pzam_run.cpp);
[validated decoding](../include/psizam/pzam_format.hpp), `pzam_load`;
[cached runtime](../src/runtime_cached.hpp);
[relocation handling](../src/llvm_aot_compiler.cpp).

### 7. Artifact compilation: the largest cold-path gap needs attribution

The 18-worker comparison already includes parallel compilation. Adding more
workers is not a diagnosis. Some difference is the expected tradeoff of a more
expensive optimizing compiler; there is no evidence that all 30x is avoidable.
Current architecture creates fresh LLVM contexts,
modules, TargetMachines and object files per batch, then parses ELF, extracts
relocations, merges code and serializes the result. The translator initially
creates many allocas which later passes promote to SSA. Candidate costs are
IR construction/optimization, native code generation, per-batch setup,
allocation and the final serial work.

Instrument these phases before implementing a major change. Test reusable
per-worker target state, batch sizing and lifetime reductions only where the
profile shows cost. One concrete cleanup is returning batch-local function
offsets: every batch currently allocates two arrays sized to all 56,288 Wasm
functions. Across 55 batches that is about 47 MiB of offset records instead of
about 0.86 MiB total. This is avoidable work, but cannot explain 7 GiB RSS or
30x compilation time on its own.

Sources: [batch/result arrays](../include/psizam/detail/ir_writer_llvm_aot.hpp);
[offset-array allocation and object emission](../src/llvm_aot_compiler.cpp).

### 8. Memory lowering: potentially broad, not low-hanging fruit

Guest scalar loads and vector loads/stores are marked volatile to preserve
faulting accesses. This limits removal, combination and movement of memory
operations and is a plausible contributor to the vector-kernel gaps. LLVM's
[volatile semantics](https://llvm.org/docs/LangRef.html#volatile-memory-accesses)
require preserving volatile accesses; that is more restrictive than ordinary
optimizable memory operations.

Simply removing volatile can eliminate an unused out-of-bounds load and change
required trapping behavior. A valid replacement needs explicit trap-preserving
checks or another proven memory-lowering strategy, followed by tests of dead
loads, reordering, stores before traps, memory growth and host reentry. Treat
this as a design project after cheaper experiments, not a flag to switch off.

## Additional focused evidence

Six diagnostic runs of the existing saved LLVM Clang artifact (three without
limits, three with gas) produced the exact expected APInt object hash. The
loader's pre-`_start` phase timer reported medians of 41.02 ms and 40.45 ms,
respectively. Unmetered phase medians included 12.54 ms decoding, 4.70 ms code
copying, 3.75 ms relocation-vector construction, 3.08 ms relocation application
and 2.13 ms for the explicit extra validation pass. Phase medians need not sum
to the median total. These are diagnostic observations on the current desktop,
not a new engine ranking or a controlled measurement of recoverable time.

This narrows the loading hypothesis: the measured pre-start region is much
smaller than the report's 325 ms unmetered and 918 ms metered APInt gaps.
Guest initialization, execution and teardown are outside that phase timer;
the data does not isolate their individual costs.

Raw diagnostic scripts, commands, stderr, input/tool hashes and hash checks are
retained in `psiserve/outputs/psizam-gap-review-2026-09-17`. The interleaved APInt
check above is a separate diagnostic cohort; it does not replace the comprehensive
report's frozen measurement set. All six loader runs and all 24 interleaved runs
produced object SHA-256
`0daa34e941fdc3bf11e9328302c1b70189c16e254dc8e1b4b098273ad6b34467`.

## Decision rule

Start with independent, reversible changes and matching correctness gates. A
successful focused result earns a broader run. Do not optimize to win an
isolated chart by dropping FP semantics, gas enforcement or per-entry protection.
For retained LLVM services, prioritize call-heavy integer execution and actual
IPC capacity. For fast-tier SIMD users, native vector arithmetic is the clearest
large opportunity. For deployment latency, LLVM preparation is the largest
problem, with the least defensible attribution until phase profiling is added.
Keep CoreMark's roughly 4-6% gap behind these larger opportunities unless a
profile shows the same inexpensive change benefits real service workloads.
