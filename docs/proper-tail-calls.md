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
packing uses full-size SP adjustments and sufficient code capacity for large
offset instructions. An additional correctness fixture verifies every one of
2000 scalar imported arguments across ordinary and tail calls. These prologue
bounds exclude incoming arguments, host C++ fallback arrays and nested native
host continuations; the integrating physical policy must cover those separately.
LLVM can publish exact prologue allocations in the same bound fields. Artifact
format persistence and ABI rejection remain the integrating owner's work.

For frame replacement, let `B` be the current frame pointer, `M` its incoming
native argument bytes and `K` the target's argument bytes. The retained ordinary
caller continuation expects completion at `S = B + 16 + M`. Stage the target
arguments at `[S-K,S)`, restore the original caller frame pointer/return address,
and branch to the target without adding a link. An overlap-safe high-to-low
copy is required. Every target returns with `SP=S`, including larger and
smaller parameter signatures in a cycle. Incoming argument storage, temporary
staging and native slot inflation need independent physical bounds; the logical
WASM frame cost is not such a bound.

LLVM bodies will use `tailcc` and `musttail` followed immediately by `ret`.
Typed import thunks must own their argument buffers after replacing the caller.
Indirect tails need validated typed-body/import-thunk pointers, not the public
C argument-array wrappers. Target budget admission must happen before machine
entry because a check inside LLVM IR runs after its native prologue has already
allocated a frame. Actual logical debit/refund stays in the separately reviewed
common byte-stack implementation.

New tests in this series are correctness fixtures, not performance workloads.
Qualification must cover native stack high-water independent of chain length,
signature changes, indirect/import tails, exact return bits, traps, exception
scope removal, nested host entry and boundary logical budgets. Cross-target
code generation alone does not qualify execution on the other architecture.
