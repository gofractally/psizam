# Compiling WebAssembly to PZAM

The C++ API and `pzam-compile` produce relocatable native code for x86-64 and
AArch64. Each requested architecture and tier is compiled independently.
JIT1 uses the single-pass machine-code writer, JIT2 uses the optimizing IR
writer, and LLVM O1/O2/O3 uses LLVM AOT at the requested optimization level.
The default C++ request is host-architecture JIT2.

```cpp
#include <psizam/pzam_compile.hpp>

psizam::pzam_compile_file("module.wasm", "module.pzam", {
    .arch = psizam::pzam_arch_flags::all,
    .tier = psizam::pzam_tier_flags::all_jit,
    .flags = psizam::pzam_compile_flags::gas_meter,
});
```

This creates four code sections: JIT1 and JIT2 for each architecture. Consumers
select a section for their architecture. LLVM tiers require linking the
`psizam-llvm` target when compiling; executing saved LLVM output only requires
the execution library.

```sh
pzam-compile --target=aarch64 --backend=jit1 --gas-metering=off \
  -o module.pzam module.wasm
pzam-run --cpu-limit=none module.pzam
```

The CLI accepts `--backend=jit1|jit2|llvm`, `--backtrace`, and
`--gas-metering=on|off`. Gas is on by default in the CLI and off by default in
the C++ API. LLVM accepts `--opt-level=0|1|2|3` and compilation worker counts;
JIT1 and JIT2 CLI compilation is serial. Page-based time limits use unmetered
code. A runtime gas budget requires a file compiled with gas instrumentation.

The pSSZ container holds module metadata, native code, function descriptors,
and relocations. JIT1 records helper addresses from the beginning of code
emission, including trap handlers and import stubs. Its helper catalog is
shared by the compiler and loader. Helpers with different argument layouts
have distinct symbols. Unregistered addresses cause compilation to fail.
AArch64 saved pointers use fixed-width MOVZ/MOVK sequences, and x86-64 uses
64-bit address relocations. Pointer immediates are canonicalized before saving
and resolved in the loading process. Ordinary in-memory JIT compilation does
not collect PZAM relocations.

Saved JIT1 code is supported by `pzam-run`, `pzam_instance`, and
`runtime::load_cached()` followed by `runtime::instantiate()`. Runtime instances
own their executable mappings and mutable state. JIT1 and JIT2 files require
the runtime's memory-page layout to match the recorded layout.

The API rejects empty or unknown architecture/tier flags, unsupported
instrumentation flags, LLVM tiers without LLVM enabled, and explicit softfloat
requests containing JIT tiers. Explicit `--softfloat` is available with LLVM.
Debug-info emission is not implemented. File-writing APIs report stream errors.

## Validation

`pzam_compile_api` checks independently compiled versus combined sections,
compiler identity, cross-architecture emission, host execution, empty modules,
backtrace, gas enforcement, and LLVM floating-point selection. Public-runtime
tests cover saved JIT1/JIT2/LLVM host calls, initialization, independent memory,
budgets, and lifetime after the runtime and module handles are destroyed.

The `execution.jit1-cli.*` and `execution.jit1-api.*` CTests compile and execute
in separate processes, including scalar/SIMD floating point and distant trap
handlers. `execution.resource-limits` exercises saved JIT1 gas and page timeout
traps, including nonterminating module initialization. The installed-package
acceptance test loads JIT1 output through the public runtime.

With `PSIZAM_ENABLE_AOT_SPEC_TESTS=ON`, the scalar, SIMD and smoke suites also
have saved-JIT1 executables. Each construction invokes the real CLI and reloads
its output; there is no in-process JIT fallback. For example:

```sh
PSIZAM_AOT_SPEC_COMPILER="$PWD/build/bin/pzam-compile" \
PSIZAM_AOT_SPEC_DIR="$PWD/build/jit1-spec-artifacts" \
  build/bin/psizam_scalar_jit1_pzam_tests
```

Use a separate fresh artifact directory for each validation run. Native x86-64
and AArch64 execution remain separate platform gates; emitting foreign code
alone is not proof that it executes correctly.
