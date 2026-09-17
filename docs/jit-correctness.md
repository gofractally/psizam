# JIT1 and JIT2 correctness repairs

The September 16 benchmark failures exposed several independent defects. The
execution gate now exercises fresh JIT1/JIT2 in both FP modes, saved JIT2 from
both compilation APIs in a separate process, and real Clang compilation with
verified object hashes. See [validation.md](validation.md) for reproduction.

## Repairs

| Area | Defect | Correction |
| --- | --- | --- |
| ARM64 JIT1/JIT2 traps | Distant branches embedded scratch-buffer addresses; some JIT2 divide checks truncated branch displacements. | Page-relative inline trampolines and range-aware branches for stack, indirect-call and division traps. |
| Saved JIT2 FP | Parser defaults overrode the artifact's FP policy; SIMD and conversion helpers retained process-local addresses. | Compilation policy matches section metadata; a shared helper-symbol registry supplies relocations and loader bindings. Missing bindings fail during loading. |
| ARM64 JIT2 vectors | Vector arguments already on the native stack acquired an extra scalar spill value; mixed arguments were ordered incorrectly. | Pack complete argument groups in signature order, preserving both vector halves. |
| x86-64 host exceptions | C++ unwinding through generated frames restored incorrect callee-saved registers. Even WASI `proc_exit` could crash. | Catch exceptions in a native bridge and escape through the existing trap boundary, retaining the existing argument buffer. |
| x86-64 JIT2 normal return | The entry wrapper saved R15 on the guest stack and restored it from the host stack. | Reserve a slot in the host frame and use it for both save and restore, including when switching stacks. |
| x86-64 JIT2 register allocation | Expired integer XMM spill entries overflowed a fixed-size active array on a large Clang function; separate integer/float allocation could conflict or lose values across calls. | One lifetime-ordered XMM allocation pass, call-aware spills, compact spill slots and regression checks for interference and register reuse. |
| JIT2 cross compilation | Register allocation selected the host register bank; loaders also accepted incompatible memory page layouts. | Select the target register bank and reject JIT artifacts whose embedded page layout differs from the runtime. |
| x86-64 JIT2 addresses | Folding `i32.add` into a memory displacement lost 32-bit wrapping semantics. | Use the computed Wasm address, then add the memory instruction's offset separately. |
| x86-64 JIT2 helper ABI | Saving one pointer after aligning the stack misaligned subsequent native calls. | Preserve 16-byte call alignment when saving the restore pointer. |
| LLVM vector control | The new mixed-vector regression exposed overlapping indirect-call arguments and a lost vector return half, including tail calls. | Size and pack both vector slots and load the full returned vector. |

The compiler validation and benchmark source runners now use an 8,192-frame
limit, matching `pzam-run`. Their former 251-frame limit was too low for these
Clang workloads. The engine's default resource policy is unchanged. Dedicated
large-module tests verify that exhausting the limit produces the expected
stack-overflow trap rather than a signal or control-flow failure.

## Regression coverage

- Integer arithmetic with a wrapping address calculation followed by a load offset.
- Normal guest return with verified output, in addition to exception-based WASI exit.
- Scalar f32/f64 arithmetic and all eight trapping float-to-integer conversions.
- SIMD arithmetic and mixed scalar/vector direct, indirect and tail-indirect calls.
- Large modules with distant stack-overflow and 32/64-bit division trap handlers.
- Register allocation under repeated mixed register pressure, with and without calls,
  checked for both target register banks on every host.
- Both pinned LLVM source files compiled by Clang under fresh JIT1/JIT2 and saved
  JIT2; every resulting object must match its independently established hash.

The harness also rejects skipped execution, stale artifacts, generic corruption
errors masquerading as expected traps, and incomplete compiler inputs.

## Results and limits

Validation ran on an Apple M5 host with Clang 22.1.2. ARM64 runs were native;
x86-64 binaries used Rosetta. No native Linux run is claimed.

| Validation | Result |
| --- | --- |
| ARM64 quick execution matrix, including LLVM controls | 108 / 108 passed |
| ARM64 compiler matrix | 18 / 18 CTest entries passed, including 14 object-hash-verified Clang jobs |
| x86-64 execution matrix, LLVM disabled | 91 / 91 passed, including 12 object-hash-verified Clang jobs |
| Fresh scalar specs, each architecture | 9,150 / 9,153 cases; 71,529 / 71,532 assertions |
| Fresh SIMD specs, each architecture | 3,447 / 3,447 cases; 75,219 assertions |
| ARM64 LLVM AOT O2 scalar specs | 3,050 / 3,051 cases; 23,843 / 23,844 assertions |
| ARM64 LLVM AOT O2 SIMD specs | 1,149 / 1,149 cases; 25,073 assertions |
| Public compilation API and page-layout rejection | 5 / 5 cases; 75 assertions |

Both final quick matrices were rerun after adding the page-layout guard; the
compiler matrices passed before that load-time validation change. ARM64's
final quick matrix and compiler matrix ran separately. Fresh spec
counts combine interpreter, JIT1 and JIT2. Scalar failures are all the same
pre-existing `memory_grow_2` multi-memory parser rejection, repeated per backend.
They remain visible failures; no suppression or expected-failure marking was added.
The specification harnesses were compiled at C++ `-O0` with CMake unity builds
to shorten build time; the LLVM AOT guest code was compiled at `-O2`.

JIT2 PZAM still embeds OS-page-relative global and table offsets. An ARM64/macOS
compiler using 16 KiB pages cannot produce a JIT2 artifact that runs unchanged
on a 4 KiB-page runtime. Such files now fail at load time with
`JIT PZAM page size does not match runtime`, rather than executing with incorrect
offsets. Target page-layout selection or relocatable prefix offsets remain
portability work. Matching architecture alone is insufficient.

These Clang jobs compile two pinned LLVM translation units. They do not replace
the complete compiler bootstrap or establish a byte-identical bootstrap fixed
point. The other release gates in [release-status.md](release-status.md) remain open.

These repairs require new performance measurements. The retained benchmark
report and reproduction kit describe the earlier source and remain historical
evidence; timings from that run must not be presented as measurements of these
repairs. In particular, the exception bridge changes x86-64 host-call overhead
and the register-allocation changes can affect generated code.
