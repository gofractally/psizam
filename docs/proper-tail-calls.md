# Proper tail calls: implementation checkpoints

This is development work. It does not enable a qualified production tail-call
profile or establish a physical-stack bound for every backend.

The existing private guest ABI makes callers remove argument slots after a
return. A tail call with a larger parameter area cannot safely reuse that
caller's continuation under this convention. This series changes the private
guest ABI to callee cleanup, while keeping public C entry points and registered
host functions unchanged.

The first checkpoint changes JIT1 AArch64 ordinary guest returns, import thunks
and result handling together. It is an ABI prerequisite; the old restricted
tail paths remain until the next checkpoint. Existing serialized native
artifacts use the old private ABI and must not be reused with changed code.
The integrating artifact contract must identify and reject that mismatch.

The second checkpoint replaces every JIT1 AArch64 direct/indirect tail,
including imports, without a call/return fallback. It consumes the separately
reviewed common logical byte-stack dependency. Its local focused checks include
100,000 signature-changing cycles, count and params-counted byte modes, backtrace
mode, imported targets, nested host entry/failure, vector and multiple results,
traps and reuse, and a 301-parameter fixture exceeding 4095 native argument bytes.
The parameter-copy loop reads payload words only and copies high-to-low. The
existing large-SP add/sub emitter is corrected to the AArch64 extended-register
encoding; the previous words address XZR rather than SP. The large ordinary
return fixture specifically exercises the corrected large positive cleanup.
The independent interpreter metadata supplies the exact boundary-budget oracle.
Dynamic nonnull table mutation and saved import-thunk reconstruction remain
separate integration prerequisites. Other native backends and LLVM have not yet
been ported by this checkpoint, so the new long-tail matrix is not qualified on
those targets.

The following metadata checkpoint consumes the reviewed shared table resolver
and publishes private import code ranges plus proven native prologue allocation
upper bounds. Serial and merged JIT2 publication runs on the owning thread after
workers join. Result arrays are prepared before finalization. AArch64 import
packing uses full-size SP adjustments and the maintained contiguous code-buffer
growth mechanism for large offset instructions. An additional correctness fixture verifies every one of
2000 scalar imported arguments across ordinary and tail calls. These prologue
bounds exclude incoming arguments, host C++ fallback arrays and nested native
host continuations; the integrating physical policy must cover those separately.
LLVM can publish exact prologue allocations in the same bound fields. Artifact
format persistence and ABI rejection remain the integrating owner's work.

The AArch64 JIT2 checkpoint uses the same completion-SP contract for every
direct/indirect/import/self tail. Ordinary and explicit returns share the
maintained epilogue, preserve result channels, release the logical frame and
remove native parameters. Both AArch64 JITs synchronize the live logical budget
before an exception snapshot. The shared JIT2 matrix includes serial and four
worker compilation, long signature-changing cycles, imported and mutated table
targets, exception scope removal and argument cleanup beyond 65535 bytes.
This does not by itself bound ordinary C++ host continuations retained by
recursive host-to-guest reentry. A positive host-boundary reserve is a separate
required contract and release gate owned by the integrating runtime work.

The fixed-frame metadata checkpoint publishes code-indexed native body bounds
both in each live `function_body` and in `body_stack_sizes`. An explicit
completeness flag distinguishes a proven zero frame from an unavailable bound;
an unavailable per-body bound is `UINT64_MAX`. AArch64 JIT1 reports its saved
FP/LR plus all native local slots. AArch64 JIT2 additionally accounts for its
actual allocated spill and callee-saved slots, including alignment, with checked
size arithmetic before narrowing. LLVM uses its extracted native frame sizes.
The bounds exclude dynamic operand storage, incoming arguments and transient
helpers or host continuations. They are inputs to the integrating physical
policy, and do not change the logical WASM charge or artifact format. Native
result publication fails closed when a nonlegacy ABI lacks body-frame coverage;
the interpreter explicitly retains unavailable metadata.

For frame replacement, let `B` be the current frame pointer, `M` its incoming
native argument bytes and `K` the target's argument bytes. The retained ordinary
caller continuation expects completion at `S = B + 16 + M`. Stage the target
arguments at `[S-K,S)`, restore the original caller frame pointer/return address,
and branch to the target without adding a link. An overlap-safe high-to-low
copy is required. Every target returns with `SP=S`, including larger and
smaller parameter signatures in a cycle. Incoming argument storage, temporary
staging and native slot inflation need independent physical bounds; the logical
WASM frame cost is not such a bound.

The LLVM development port uses `tailcc` for private typed bodies, declarations,
call sites and import thunks. Every guest tail emits `musttail` immediately
followed by `ret`, with the existing logical refund before the call and no
frame-dependent data or cleanup afterward. Public argument-array wrappers
retain the C ABI. Import argument packing now belongs to the target thunk's
fixed entry allocation. Scalar host results are supported; host vector
arguments and vector/multivalue results fail explicitly until the maintained
host API supplies their complete result channels. Guest vector and multivalue
channels remain distinct and use the existing typed/context representations.

Typed indirect calls reuse the maintained signature/bounds/null resolver and
obtain the existing table entry's global function index and private target.
The module owns a readonly derived logical-cost table exposed at native64
context offset 176. Target offsets are explicit constants, with native64
layout assertions, and do not depend on a wasm32 compiler host's `offsetof`.
Byte-mode admission reads this table before entering the target's machine
prologue; public entry also checks the selected body before its C wrapper.
Actual body debit/refund remains in the reviewed logical byte-stack code.
Imported targets have no guest-body cost in this checkpoint: the integrating
host-reserve contract must add pre-thunk admission and reserve the C++ helper
continuation before final qualification. The table is built once from validated
per-body costs at module finalization or explicit loader preparation, before
sharing the immutable module. Contexts only bind the readonly pointer; they
reject a missing byte-mode table and never mutate the shared module to lazily
initialize it. This adds no payload framing.

AOT units declare typed imports everywhere but emit them exactly once, in the
first serial function or first compilation batch. Import-only modules get a
dedicated compiler unit even without a code section. Aggregate publication
requires full, unique import coverage and resolves checked pending body,
wrapper and import references before publishing native metadata. Import ranges
and exact extracted fixed frames use the existing additive compile-result
fields. Artifact persistence, ABI rejection, compiler-free saved-artifact
execution and integrating physical storage remain separate release gates.
ORC retains one complete parsed IR module; its inherited native parallel-parse
hook is intentionally hidden, so a compile-thread option cannot skip parsing
and present empty IR as compiled code. This does not claim parallel ORC codegen.

New tests in this series are correctness fixtures, not performance workloads.
Qualification must cover native stack high-water independent of chain length,
signature changes, indirect/import tails, exact return bits, traps, exception
scope removal, nested host entry and boundary logical budgets. Cross-target
code generation alone does not qualify execution on the other architecture.
