// Fixture for the wasmtime backend's backtrace-detail and debug-info tests:
// a module with DWARF whose only function calls a host function at a known
// line. debug_fixture.wasm is this file compiled with wasi-sdk 32.0 (clang 22.1.0):
//
//   cd wasmtime/tests && $WASI_SDK/bin/clang --target=wasm32-wasip1 -g -O0 \
//     -fdebug-compilation-dir=. -nostdlib -Wl,--no-entry \
//     -Wl,--export=debug_fixture_entry -o debug_fixture.wasm debug_fixture.c
//
// Run it from this directory with the relative file name, so the DWARF names
// the source as "debug_fixture.c" and holds no absolute path. The tests
// require the line of the call below (debug_fixture_call_line in wasmtime_backend_tests.cpp); keep the two
// in step and rebuild the .wasm whenever this file changes.

__attribute__((import_module("env"), import_name("capture"))) void capture(void);

int debug_fixture_entry(int x)
{
   capture();  // line 18
   return x + 1;
}
