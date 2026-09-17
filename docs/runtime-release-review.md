# Public runtime release review

This checklist covers the independently installed C++ runtime (`psizam-exec`,
with optional `psizam-llvm`). The lower-level backend API and the CLI have
separate capabilities; a working CLI path does not establish public API parity.

## Completed acceptance gates

| Item | Acceptance evidence |
| --- | --- |
| Installed LLVM factory | `execution.installed-runtime`: downstream `find_package` projects execute using base and LLVM packages without compiling runtime sources. |
| Public gas state and policies | `runtime_release`: live counters, finite budgets, deadline changes, interrupts, unmetered code, initialization limits, timeout polling, yield handlers and pools across available backends. |
| Cached PZAM instantiation | `runtime_release`: saved JIT1, JIT2 and LLVM O2 code, ordered import arguments, indirect calls, isolated memories, gas compatibility and instance lifetime. Installed base consumers execute LLVM PZAM without LLVM. |
| Import diagnostics | `runtime_release`: missing imports, signature mismatch, absent dispatch, nonfunction imports, prepared and cached bindings, WIT signatures and malformed Wasm. |

Recorded focused public acceptance passes 232 assertions on native ARM64/macOS
and native x86-64/Linux with LLVM; each run retains its source identity.
The x86-64 Rosetta run without LLVM passes 175 assertions. Gas-handler tests also
verify that exceptions leave generated code safely, release their exception object
and permit subsequent calls. The installed-consumer gate covers both base and LLVM
packages. These gates establish the items above, not all pending contracts below.

## Decisions and additional work to review before a stable release

- **Resource policy coverage.** Audit every `instance_policy` field. Memory
  safety, floating-point mode, maximum pages/stack, compiler sandboxing,
  compilation timeout and optimized-tier promotion are not yet all wired into
  the facade. Reject unsupported requests explicitly or implement them; do not
  publish these fields as enforced guarantees until there are negative tests.
- **Public timeout semantics.** `meter_mode::timeout` currently means wall-clock
  polling at gas checkpoints, with a deadline starting immediately before
  module initialization. It includes elapsed time between calls. The benchmark
  uses the lower-level code-page interruption path. A public scoped execution
  deadline API should support code-page interruption without allocating a timer
  on each service call and must handle nested calls and shared code ownership.
- **Prepared code caching.** `prepare()` currently retains Wasm bytes and policy;
  compilation occurs in `instantiate()`. Its timing/native-size accessors do not
  describe compilation. A future cache must separate immutable code from mutable
  bindings and policy, key by full compilation identity, and avoid hash-only
  equality. Guarded-region and arena objects are provisioned but not yet leased
  by the normal instantiation path.
- **Multiple bound consumers.** Nonscalar bridges capture the prepared module's
  single `live_be` pointer. Multiple live instances of that handle need
  instance-specific bridge bindings before this is a supported use case.
- **Gas pool accounting.** Review lease sizes, failed partial leases, metering
  overshoot, interrupts/deadline edits and unused credit at teardown. A deadline
  is not a sufficient ledger for outstanding pool credit after a caller edits it.
- **Library linking.** `register_library()` retains bytes; it does not link archive
  members or automatically bind registered Wasm libraries. Diagnostics must not
  report registration alone as a resolved import.
- **WIT and composition.** `composition::unresolved()` and
  `module_handle::wit_sections()` are still stubs. Dynamic bindings need full
  logical WIT schema compatibility and nonscalar/indirect argument support;
  matching the 16-slot core Wasm signature alone does not establish WIT type
  compatibility. Cached modules need an explicit WIT metadata strategy.
- **Saved-code setup and policy.** Cached instances currently relocate a separate
  executable mapping per instance. Measure that cold cost before adding shared
  code mappings; sharing must preserve interruption ownership. Saved code fixes
  the compilation tier and instrumentation. External memory is explicitly rejected
  on the cached path.
- **Cache identity and loader hardening.** Review compiler identity, compilation
  options and runtime ABI invalidation, malformed relocation/metadata bounds,
  architecture selection and 4 KiB versus 16 KiB JIT page layouts.
- **Saved-artifact storage.** Use each run's `artifact-sizes.json` to inspect
  Clang's relocation counts and bytes outside the native region. Classify
  remaining relocation kinds and distinguish module data from serialization
  overhead before changing the representation. Same-batch PC-relative function
  calls already have an optimization; investigate other base-independent
  relocations and cold loading with focused measurements. These are proposed
  storage/load-time improvements, not measured runtime gains.
- **Supported platforms and spec scope.** Keep native x86-64 and ARM acceptance
  in the release gate;
  document multiple memories as unsupported. The current scalar suite's
  `memory_grow_2` failures are in this unsupported feature, not a passing gate.
- **Generated specification coverage.** WABT 1.0.40 cannot parse the GC syntax
  in the pinned `table_init.wast`; the generated `table_init_tests.cpp` is empty
  on both recorded hosts. Split supported table-initialization cases from
  unsupported proposal cases and retain a generation coverage manifest. An
  omitted source must be visible in CI and must not count as a passing suite.
- **Validation build cost.** The largest generated SIMD `pmin`/`pmax` translation
  units dominate the observed native Release validation build. Split these
  generated files into smaller compilation units while preserving all assertions
  and optimized builds, so routine correctness checks remain practical.
- **Hardware-deterministic floating point in x86 JIT1.** The scalar arithmetic
  emitters in `detail/x86_64.hpp` route every non-`fast` mode through software
  helpers, including `hw_deterministic`. Implement the promised native path with
  explicit NaN canonicalization, rounding and trap semantics; validate against
  the softfloat oracle before using focused benchmarks to assess the gain.
- **Compiler bootstrap.** The measured Clang jobs compile two C++ translation
  units. They do not establish a complete Clang self-bootstrap. Reproduce and
  retain the full `clang.wasm -> clang.pzam -> clang.wasm` chain before claiming
  that release milestone.
- **Debug support.** Review v128 locals, DWARF range endpoints, function pointers,
  accurate prologue frames and unsupported DWARF references. Separate debugger
  completeness from execution support. JIT1 SIMD return preservation during
  backtrace cleanup has a focused regression gate (`execution.jit1-backtrace`).
- **Signal integration.** Review chaining of previously installed signal-handler
  flags/masks, nested interruption ownership and exception cleanup on every
  generated-code helper's slow path.

The raw TODO inventory includes code-generation placeholders and optimization
ideas which are not missing functionality. Performance follow-ups include JIT1/2
host-import overhead, LLVM versus Cranelift compilation latency, cached
cross-service entry overhead, and register-allocation quality. Track these with
focused benchmarks; they are distinct from safety and API contract gaps above.

## Compiler hosted in Wasm

A fresh compile-only WASI configuration using `cmake/wasi-toolchain.cmake` needs
review before claiming the compiler itself is a supported Wasm-hosted product.
The current toolchain disables C++ exceptions while pSSZ and the public PZAM API
use throws; native execution-context layout assertions also fire with 32-bit
pointers. These are separate from native `pzam-compile` producing PZAM files
and from running the Clang compiler guest. The full compiler bootstrap remains
a distinct release gate.
