# Service-call implementation and focused measurements

The scalar call path validates stable information during binding and retains
protection at every guest entry. The benchmark models frequent host queries,
service A calling B through the host, and nested A-to-B-to-A calls. It does not
measure database work, PSSZ payload serialization, or full psibase IPC.

## Current implementation

* Protected entry uses a no-mask-saving signal checkpoint. On a signal escape,
  the handler restores the interrupted signal mask before jumping. Nested
  checkpoints restore the enclosing trap and memory/stack guard state.
* Large guarded entry stacks are cached per execution context. Each active
  reentry holds a distinct stack lease. Warm calls reuse mappings; destruction
  releases them. Contexts retain the same thread-confinement requirement.
* LLVM uses one exception-safe helper for import dispatch, depth accounting and
  memory refresh. C++ exception cleanup completes before escaping generated
  code. The helper has an explicit PZAM relocation and is tested after loading
  an artifact produced by the LLVM O2 CLI.
* Canonical arguments use sixteen inline slots, without C++ heap allocation.
  Unsupported excess flattened arguments fail explicitly.
* Reflected proxies and scalar service bindings resolve exports and validate
  the lowered Wasm signature once. Calls retain a lifetime check and their own
  protected entry. Proxy allocator state is instance-specific and enclosing
  canonical state is restored after normal returns and exceptions.

## Natural-signature measurements

Apple M5 Max, macOS 26.3.1 ARM64, LLVM 22.1.2, Wasmtime 48.0.2. Five measured
fresh processes plus one excluded warmup per cell, executed serially with no
concurrent build. Values are median nanoseconds. Gas/fuel units differ between
engines. Timeout cases arm a deadline and separately prove interruption and
recovery. Saved LLVM results use freshly generated O2 PZAM files.

| Engine/API | Host query, no limits | A-to-B, no limits | A-to-B, gas/fuel | A-to-B-to-A, no limits |
|---|---:|---:|---:|---:|
| JIT1 | 5.49 | 57.80 | 54.97 | 86.34 |
| JIT2 | 5.57 | 55.84 | 56.09 | 82.68 |
| LLVM O2 PZAM | 1.77 | 44.68 | 45.11 | 67.24 |
| Cranelift checked | 3.34 | 138.11 | 136.80 | 226.08 |
| Cranelift cached | 3.36 | 31.28 | 32.77 | 46.81 |

The query timer includes guest loop instructions and two distinct scalar host
functions. A-to-B includes two protected guest entries and a query from B;
A-to-B-to-A includes three entries. Every result is checked against a native
integer reference. All measured psizam paths use zero C++ new/new[] allocations
after warmup. Those counters exclude direct malloc/mmap and Rust allocations;
they are not cross-engine allocation comparisons.

Wasmtime checked and raw calls both cache the export and retain a trap boundary
per invocation. The raw path validates its signature at binding. psizam uses
checked indexed calls in this natural-signature experiment. Legacy batched
scoped_execution timings have a different scope and are not IPC comparisons.

## Actual runtime proxy

`benchmarks/call-boundary/runtime_bind.cpp` separately measures the full scalar
`instance::as` -> consumer -> `runtime::bind` -> provider chain, with typed and
WIT-driven dynamic binding. Both instances receive a finite budget through their public policy;
actual consumption is verified. This uses the canonical ABI and is not the same
workload as the natural-signature table. There is no corresponding Wasmtime
component-binding measurement in this experiment.

The final five-trial medians are:

| Backend | Typed binding, ns/call | Dynamic binding, ns/call | C++ allocations/call |
|---|---:|---:|---:|
| JIT1 | 82.64 | 83.73 | 0 |
| JIT2 | 80.63 | 80.80 | 0 |
| LLVM O2 JIT | 55.19 | 56.44 | 0 |

These include the host proxy and both guest entries. All six cells consume
56,000,056 consumer gas units and 34,000,034 provider units over one warmup call
plus one million measured calls; gas units are not execution-time units.

The public runtime uses the executing backend's gas state. Its budget,
interruption, startup and handler behavior are covered by `runtime_release`.
The focused proxy runner configures budgets and reads accounting through that
public API. Additional public API contracts requiring review are listed in
[runtime-release-review.md](runtime-release-review.md).

## Remaining work

1. Isolate the remaining protected entry and argument-adapter costs against
   cached Cranelift. JIT1/JIT2 still repack arguments and use a generic host
   dispatcher; frequent queries expose that cost.
2. Add cold binding, variable-size strings/records, and realistic database
   operations. Validate logical WIT schemas and dynamic cross-memory marshalling;
   a matching lowered sixteen-i64 signature does not establish schema identity.
3. Validate supported architectures independently. These performance results
   establish ARM64 behavior on one host, not x86 performance or full Wasm
   proposal conformance.

Focused tests cover missing/wrong signatures, expired bound instances, moved
owners, gas exhaustion, timeout interruption, stack and memory traps, repeated
host exceptions and destruction, mixed values, reentry, and memory growth from
a callback. Run these focused tests and benchmarks before the broader suites.
See `benchmarks/call-boundary/README.md` for reproduction commands and
`docs/validation.md` for the standard correctness gates.


Measured source: `19d5f9c`. Exact source hashes, binaries, fixture hashes, commands
and raw samples are recorded under
`build/call-optimization-2026-09-17/public-release-boundary` and
`build/call-optimization-2026-09-17/public-release-runtime`.
