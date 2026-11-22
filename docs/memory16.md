# memory16-v1 execution profile

This is a psizam execution profile using the Wasm32 instruction set. It is
opt-in and deliberately differs from standard WebAssembly at the memory
boundary. A one-page memory declaration alone does not select it.

## Module declaration

The Wasm custom section `psizam.execution` contains four bytes:

| Byte | Meaning | Value |
| --- | --- | --- |
| 0 | Profile version | 1 |
| 1 | Dynamic address bits | 16 |
| 2 | C/C++ pointer ABI bits | 16 or 32 |
| 3 | Reserved | 0 |

The compiler's pointer ABI describes object layout, including pointers stored
in structures and arrays. It does not change the Wasm value types: addresses
are still carried in i32 values. Declaring p16 does not rewrite p32 object
layouts, and the marker is a declaration of intent, not proof of provenance.

The parser scans the declaration before generating code, even when the
section follows the code section. Identical records (including records
concatenated by LLD from multiple objects) are accepted. Unknown versions,
malformed records and conflicting declarations are rejected. Untagged modules
retain ordinary Wasm semantics unless the embedding application explicitly
requests `mem_safety::memory16`; that override records the p32 variant.

## Memory and arithmetic rules

* Exactly one memory32 memory, with initial and maximum sizes both one 64 KiB
  page. `memory.grow(0)` returns 1; positive growth returns -1.
* For scalar, SIMD and supported atomic memory accesses:
  `effective_address = (uint32(dynamic_address) & 65535) + memarg_offset`.
  Each memarg offset must be at most 65535. The offset is added **after**
  truncation, without further truncation. Access bytes are contiguous; an
  access crossing 65535 extends into padding rather than wrapping each byte.
* i32/i64 arithmetic retains its original width. In particular,
  `(p + 1); load offset=0` cannot become `p; load offset=1`: the mask occurs
  between the addition and the load offset. Optimizers must preserve that
  boundary. LLVM receives an explicit mask, and JIT2 disables this address
  folding only for memory16.
* The accessible physical buffer is 131086 bytes: maximum address 65535,
  maximum offset 65535, and maximum access width 16. It is rounded up to a
  host page for mapping. Self-managed instances use this compact reservation
  instead of the ordinary 8 GiB guard reservation (plus context/guard pages).
* The entire buffer starts at zero before active data initialization. Padding
  is private **writable** state; stores into it are observable by later scalar,
  SIMD or atomic accesses. Reset clears it. Snapshot implementations must
  preserve all 131086 bytes, not just the logical 65536 bytes.
* Bulk operations (`memory.copy/fill/init`), active data segments and validated
  host pointers retain explicit logical-memory bounds of 65536 bytes. They
  do not wrap or expose padding. Empty ranges at the end are valid.
* Atomic operations retain the engine's single-thread execution model; this
  profile does not introduce shared-memory concurrency. Unaligned accesses
  use the same byte semantics across backends. Private-memory wait32/wait64
  trap rather than introducing shared-memory blocking. The historical ordinary-mode
  interpreter wait stub remains outside this change; memory16 always rejects
  private waits consistently with the native tiers.

Host tables select memory16-specific pointer-conversion trampolines at load
time. Custom entries supplied with `add_entry` must provide a memory16
trampoline or linking fails. Standard spans/proxies and converters using
`execution_interface::validate_pointer` enforce the logical limit. Hosts
accepting raw integer offsets or unchecked `void*` values must validate their
own complete ranges; the compact reservation cannot sandbox arbitrary host
C++ pointer arithmetic. Built-in WASI checks complete ranges in memory16;
its Preview1 records retain their 32-bit ABI and require an appropriate SDK
bridge if the C library uses 16-bit pointers.

All other execution rules, including gas, host imports and floating-point
policy, remain the embedding application's responsibility. Memory16 does not
remove the runtime's exception/signal machinery needed for other traps.

## SDK emission

Use the existing gofractally LLVM fork's `wasm32-unknown-unknown-p16` target
and force-include the installed `psizam/memory16.h` header. Link with fixed
one-page memory limits (`--initial-memory=65536 --max-memory=65536`), preserve
`psizam.execution` when stripping, and supply p16-compatible libraries and
sysroot. The header checks `__SIZEOF_POINTER__ == 2`; an ordinary wasm32
compiler cannot silently produce a falsely labeled p16 module.

For an intentional p32 ABI with memory16 execution, define
`PSI_MEMORY16_POINTER_BITS=32` and use a wasm32 compiler with the same one-page
linker settings. All linked code and libraries must agree on the pointer ABI;
unmarked libraries cannot be verified from this metadata alone.

`default_options::memory_mode` and `pzam_compile_options::memory_mode` accept
`mem_safety::memory16` to select the p32 profile for an untagged module. An
explicit valid p16/p32 module marker takes precedence over this fallback. The
default remains `mem_safety::guarded`, and an untagged one-page module retains
ordinary WebAssembly semantics.
This selection is immutable after compilation or artifact loading.
`backend::initialize(host, new_options)` rejects an attempt to switch an
ordinary compiled module to memory16 before changing runtime state. A stored
memory16 profile remains active when initialized with default/guarded options.
Low-level users rebinding an execution context to an ordinary module must
reapply its ordinary memory mode and page limit before resetting it. Reset
rejects a leftover memory16 mode paired with an ordinary compiled profile
before changing allocator or execution state.

## AOT files and testing

PZAM format version 8 records the execution profile in both module metadata
and every native code section. Loaders reject disagreements and restore
compact memory automatically. Existing exact-version validation invalidates
older cached artifacts; rebuild them when upgrading. The canonical PZAM API
compiles each requested architecture and tier independently and preserves the
current FP, private guest ABI, native source ranges and embedder identity.
PZAM contains compiled code and module initialization metadata, not a live
instance snapshot. A runtime snapshot must separately preserve all 131086
physical bytes, including writable padding; a fresh instance starts with those
bytes zeroed before active data initialization.

The memory16 qualification suite exercises interpreter, JIT1, JIT2, parallel
JIT2, LLVM when available, and saved native artifacts: wrap boundaries, live
addresses, large offsets, SIMD, private atomics, bulk/host bounds, and padding
reset. Qualification must run on both aarch64 and x86_64. Untagged one-page
modules must retain ordinary semantics. Memory mode selection happens during
native code generation: ordinary wasm32 gets no new per-access mode branch
or mask.
