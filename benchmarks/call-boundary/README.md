# Service calls and host queries

This focused benchmark isolates the boundaries used by psibase-style services.
It does not measure a database, PSSZ payload serialization, or complete IPC.

* **Host → service → host:** one independently protected guest entry and two
  different scalar host queries, including the return to the host.
* **Service A → host → service B:** two independently protected guest entries,
  with B performing a host query. The A → B → A case adds another entry.
* **Repeated host queries:** a guest loop issues two different host queries per
  iteration; reported time is per query and includes guest loop instructions.
* **Entry by name/index:** identical host query and result with name lookup or
  a resolved index. Linking and compilation happen before timing.
* **Alternate stack entry:** a conservative module stack bound requires a
  separate guarded stack. This isolates allocation/reuse cost, not guest work.

Every timed result is checked against a native integer reference. Correctness
checks cover wrong argument types, missing exports, memory and stack traps,
recovery after repeated host exceptions, A → B → A reentry, mixed numeric
arguments, memory growth from a reentrant callback, and gas exhaustion/recovery.

Cranelift uses Wasmtime's C API with cached exports in one store and separate
module instances/memories. Both its checked and bind-time-validated raw-call
APIs retain a trap boundary per invocation. No engine batches protected entries.
C++ allocation counts cover `operator new/new[]`, not direct malloc, mmap, or
allocations inside the Rust Wasmtime library. They are not cross-engine memory
allocation comparisons. Raw times and all samples are retained.

## Reproduce

Use a Release build, WABT's `wat2wasm`, LLVM when enabled, and an installed psio
package or the explicit source path. On macOS use Homebrew LLVM:

```sh
cmake -S benchmarks/call-boundary -B build/calls -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DLLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm \
  -DFETCHCONTENT_SOURCE_DIR_PSIO=/path/to/psio \
  -DWASMTIME_C_API=/path/to/wasmtime-c-api
cmake --build build/calls --target call-boundary -j 8
ctest --test-dir build/calls --output-on-failure
python3 benchmarks/call-boundary/run.py \
  --binary build/calls/call-boundary --fixture build/calls/calls.wasm \
  --out build/call-results --samples 5
```

The runner refuses to overwrite results, records binary/fixture/source hashes,
warms every cell, shuffles trials deterministically and executes serially. Run
on an otherwise idle machine; do not compile or run another benchmark at the
same time. To omit Wasmtime, leave its CMake path unset and pass
`--modes jit1 jit2 llvm` to the runner.

## Reflected runtime proxy

`call-runtime` exercises the actual `runtime::bind` and `instance::as` scalar
path using the composition provider and consumer fixtures. Build the existing
`composition_provider_guest` and `composition_consumer_guest` targets in a
normal engine test build, then run:

```sh
cmake --build build/calls --target call-runtime -j 8
build/calls/call-runtime /path/to/engine-build/examples/composition jit1 typed
```

For a complete serial run with retained evidence:

```sh
python3 benchmarks/call-boundary/run_runtime.py \
  --binary build/calls/call-runtime \
  --fixtures /path/to/engine-build/examples/composition \
  --out build/runtime-call-results --samples 5
```

`jit2` and `llvm` select the other backends; `dynamic` selects WIT-driven binding. The output
includes exact-result validation, C++ allocation counts and the observed gas
counters. Both backend instances receive a finite 10^12 gas budget, and the
runner rejects zero consumption. The public runtime facade currently has
separate resource state; this benchmark sets and reads the executing backend
explicitly. It is a gas-instrumented experiment, not an unrestricted baseline. This executable uses the same optional LLVM runtime factory as an installed
library consumer; it does not compile a private copy of `runtime.cpp`. Its resource settings and calling
convention differ from the natural-signature microbenchmark; compare results
within each experiment, not across those workloads. Keep warmup and measured
runs separate and run serially.

Reflected proxies and scalar service bindings validate the canonical Wasm
signature at binding time and retain one protected entry per invocation. A
proxy is tied to its instance; destroying the instance invalidates its cached
calls, and moving the owning instance preserves them. Instances and proxies
remain confined to their executing thread. The runtime benchmark reads the
backend's actual gas counter. Logical WIT schema compatibility and dynamic
non-scalar marshalling are separate from validation of the lowered Wasm
signature; this scalar benchmark does not certify either.

The natural-signature runner covers no CPU limits, gas/fuel and timeout
interruption. Timeout cases first force an interruption and verify recovery,
then arm a long deadline around the timed calls. psizam protects executable
pages; Wasmtime enables epoch interruption. Every nested instance is covered.
Enable `PSIZAM_ENABLE_TOOLS=ON` in the standalone build to produce fresh LLVM
O2 PZAM fixtures through `pzam-compile`. Pass `--aot-dir build/calls` to `run.py`
to include those saved artifacts in the measured matrix. CTest also executes
all three resource modes through the saved LLVM O2 code.
