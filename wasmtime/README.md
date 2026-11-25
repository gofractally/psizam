# psizam wasmtime backend

`shim/` is a Rust static library with a C ABI over the pinned wasmtime fork.
`include/psizam/wasmtime_backend.hpp` is the C++ side, and the only interface
meant for embedders. `tests/` holds the backend's tests. See also
[docs/wasmtime-memory16.md](../docs/wasmtime-memory16.md).

## Debugging aids

Three `engine_options` fields help with debugging guest code. All are off by
default, are chosen per runtime (engine), and do not change what a module
computes. An option that cannot be honoured makes the runtime's constructor
throw; nothing is silently adjusted.

### Wasm backtraces: `backtrace`, `backtrace_max_frames`, `backtrace_details`

With `backtrace = true`:

- `instance::backtrace()`, called from a host function, returns the wasm
  frames of that instance now on the stack.
- `instance::failure_backtrace()` returns the frames that were on the stack
  when the most recent call into the instance failed: where it trapped, or
  from where the host function that threw was called. Every call into the
  instance clears it first, so after a successful call it is empty.

The text of a failure (`trap::what()`) never lists frames: it is byte for byte
the same with backtraces on and off, so it is safe to put where nodes with
different settings compare it. The frames are only in `failure_backtrace()`.

Frames come innermost first. Each `frame` has the function's index, its name
from the module's name section exactly as written there (demangle it
yourself), and the offset of its instruction in the module binary that was
compiled. With `backtrace_details = true` as well, a frame also has the source
file, line and column from the module's DWARF.

What they cost, and why they are off by default:

- Off, a failing call walks no stack and both functions return no frames.
- On, every failing call walks the whole wasm stack and looks up the innermost
  `backtrace_max_frames` frames (default 512), whether or not anyone reads the
  result. A guest can make calls fail as often as it likes, and a stack
  overflow is thousands of frames deep, so the limit is what bounds the work.
- `backtrace_details` makes each module keep its DWARF for as long as it is
  loaded, and makes each captured frame a DWARF lookup.

`backtrace_details` without `backtrace`, or `backtrace` with a limit of zero,
is refused.

Only frames of the instance asked are returned: when instances call each other
through host functions, ask each one.

### Native debug information: `debug_info`

With `debug_info = true`, wasmtime translates each module's DWARF into native
debug information and registers the compiled code with the GDB JIT interface
(`__jit_debug_descriptor` and `__jit_debug_register_code`, which the shim
defines). gdb or lldb attached to the process then sees wasm functions and
their source lines: a breakpoint can be set on a wasm function by name, and
stops show the C source position. A module without DWARF is registered with
its function symbols only. The registration lasts as long as the compiled
module.

It costs compile time, and each registered image is a second copy of the
module's compiled code held for the module's lifetime. On macOS lldb must be
told to read the interface, before the process starts:
`settings set plugin.jit-loader.gdb.enable on`. Do not count on reading wasm
locals: in the one case tried (a `-O0` C function under lldb) the parameter
showed as unavailable.

### Limits

- A serialized artifact (`module::serialize`) only loads into a runtime with
  the same `backtrace_details` and `debug_info`. Make both part of any cache
  key. `backtrace` and `backtrace_max_frames` do not affect artifacts.
- Source positions need the module's code to be where its DWARF says it is.
  The engine compiles every module as handed in, under both float profiles
  and with or without stack accounting (`compile_metered` only appends custom
  sections), so a frame's `module_offset` is an offset into the module as
  deployed and its file and line come from that module's own DWARF.
- Running out of native stack and running out of the stack budget are
  reported identically, as a stack overflow with the text `stack overflow`.
- wasmtime's `WASMTIME_BACKTRACE_DETAILS` environment variable has no effect;
  only the options do.
