# psizam

C++23 WebAssembly engine with interpreter, native JIT, two-pass JIT, and optional
LLVM compilation backends. Native execution supports Linux and macOS on x86_64
and ARM64. The core templates are header based; runtime support, debug support,
SoftFloat and LLVM integrations also contain compiled libraries.

**Engine status.** The wasmtime backend is the engine psibase runs services on; LLVM AOT is
the second engine and is paused. The interpreter, JIT1 and JIT2 are experimental: they are
kept as independent cross-checks, are largely compatible with only minor known differences,
and are not for production use. See [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

The standalone repository is [gofractally/psizam](https://github.com/gofractally/psizam).
Its history was extracted from psiserve and includes subsequent standalone
runtime, packaging and validation improvements. It is under release
preparation, not a newly certified stable release. See [extraction](docs/extraction.md)
for source provenance and [compiler bootstrap](docs/compiler-bootstrap.md) for the
Wasm compiler chain and its verification gate.

The opt-in [memory16 execution profile](docs/memory16.md) uses compact private
memory with deterministic address masking and padding. Select it through the
native runtime options, `--memory16` in the command-line tools, or the
[Wasmtime backend's runtime option](docs/wasmtime-memory16.md). Untagged modules
use ordinary WebAssembly semantics by default.

## Build independently

Install the independent psio C++ package first. Dependencies are CMake 3.20+,
a C++23 compiler, Boost headers, and psio's installed CMake configuration.
macOS builds use Homebrew LLVM; set CC/CXX accordingly. SoftFloat and Catch2 are retained in this repository. WIT bindings and PZAM
serialization use the independent current psio library.

```sh
CC=/path/to/clang CXX=/path/to/clang++ cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/psio-install \
  -DCMAKE_INSTALL_PREFIX=/path/to/psizam-install -DPSIZAM_ENABLE_TOOLS=ON
cmake --build build
cmake --install build
```

For source co-development, replace `CMAKE_PREFIX_PATH` with
`-DFETCHCONTENT_SOURCE_DIR_PSIO=/path/to/independent/psio`.
No sibling psiserve or psitri directory is required.

```cmake
find_package(psizam CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE psizam::psizam-exec)
```

Source consumers can use `add_subdirectory` and link `psizam-exec`.
See [consumer example](examples/consumer). To build LLVM-backed tools, add
`-DPSIZAM_ENABLE_LLVM=ON -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm`.
`PSIZAM_LLVM_TARGETS` selects `X86`, `AArch64`, or both. Installed LLVM consumers
request `find_package(psizam COMPONENTS LLVM CONFIG REQUIRED)` and link
`psizam::psizam-llvm`.

## Verification

The independent check builds engine tests, tools, and source/install consumers.
Its guest fixtures require a WASI SDK, supplied via `WASI_SDK`.

```sh
WASI_SDK=/path/to/wasi-sdk CC=/path/to/clang CXX=/path/to/clang++ \
  bash scripts/verify-independent.sh /tmp/psizam-check /path/to/psio-install
```

This check excludes the external WebAssembly spec corpus and long-running
fuzz/benchmark suites. Passing it does not establish complete proposal support,
security certification, or success of the full LLVM bootstrap.

The [standalone publication record](docs/github-publication.md) records the
source provenance and initial native package checks.

The **Standalone library** GitHub workflow runs these native package and quick
execution checks on Linux x86-64 and macOS ARM64 for pushes and pull requests,
using an explicitly pinned psio commit and WASI SDK. It also tests both installed
and source consumers.

The separate **Compiler qualification** workflow is manually dispatched. It
requires a pinned HTTPS compiler-fixture archive through its input or the
`PSIZAM_COMPILER_FIXTURES_URL` repository variable; the checked-in manifest
verifies the fixture contents. Missing fixtures fail that workflow. This is a
required release qualification gate, separate from ordinary package CI; a green
standalone workflow does not imply compiler qualification or a stable release.

[LLVM O2 saved-PZAM spec tests](docs/aot-spec-tests.md) exercise the actual AOT
compiler, file serialization, loading and scalar/SIMD assertions. They are
separate from the ordinary interpreter/JIT suites. Current results and remaining
release gates are recorded in [release status](docs/release-status.md).

## License

See [LICENSE](LICENSE). Originally forked from EOS VM. Third-party components
retain their own license files; packaging notices still require release review.
