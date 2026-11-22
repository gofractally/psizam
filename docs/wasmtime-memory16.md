# Wasmtime memory16 integration

The Wasmtime backend uses the psizam Wasmtime fork through the Rust shim in
`wasmtime/shim`. Memory16 changes execution of the Wasm32 instruction set;
it does not compile Rust guest code with 16-bit pointers.

`psizam::wasmtime::engine_options::memory16` is false by default. A valid
`psizam.execution` declaration still selects its declared memory16-v1 p16 or
p32 profile. Setting the option to true selects memory16-v1/p32 for untagged
source. The fork records the resolved profile per memory, so these modules
share one Engine, Linker and Store implementation without engine switching.
One-page declarations by themselves retain ordinary Wasm semantics.

The logical memory is exactly one 65536-byte page. Scalar, SIMD and supported
private atomic accesses mask the dynamic address to 16 bits, then add the
instruction offset without another mask. An access crossing the logical end
continues into private writable padding. The complete observable memory state
is 131086 bytes, excluding host-page rounding. Bulk operations, active data
segments and host pointer conversions retain logical bounds. Private atomic
wait traps on unshared memory; notify returns 0 with memory16's masked,
unaligned address semantics. Shared-memory execution remains disabled.

`instance::memory()` and host callback spans expose only logical memory.
`instance::memory_storage()` exposes complete mutable storage for trusted
snapshot/restore code, including memory16 padding. For ordinary memories it
returns the normal logical memory span. Storage spans must not outlive the
instance or be used while Wasm is running. The padding starts at zero in a
fresh instance and must be included when copying state.

Serialized memory16 artifacts restore their stored profile under a default
engine, whether source originally selected it through a declaration or the
runtime override. Loading an ordinary-memory artifact under an engine with
the memory16 override is rejected rather than silently ignoring the option.
These checks belong to the fork's module metadata/admission, not a bytecode
rewrite in the shim.

Persistent artifact keys must include the module bytes, all `engine_options`
(including `memory16`), and `build_identity()`. The identity contains the shim
ABI, memory16 contract version, target and actual Wasmtime source identity.
For a locked git dependency, `shim/build.rs` records the resolved git source.
For local path builds, set `PSIZAM_WASMTIME_BUILD_ID` to a receipt covering the
fork HEAD and dirty source state. A package version alone is insufficient;
the build fails when neither source identity is available.
