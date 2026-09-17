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
