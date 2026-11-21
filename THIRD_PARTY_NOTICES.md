# Third-party notices and extracted support headers

This inventory records the bundled sources and their retained notices. It does
not replace the license text accompanying each component. Installed packages
place this document and psizam's `LICENSE` under `share/psizam`, with dependency
license copies under `share/psizam/licenses` (subject to CMake's configured data
directory).

## Psizam and EOS VM

Psizam is based on [EOS VM](https://github.com/gofractally/eos-vm). The root
`LICENSE` contains the MIT license and retains the copyright notices for
GofractallyIO and its contributors, the EOS Network Foundation and its
contributors, and block.one and its contributors. Source-file notices remain
in place. The engine was extracted from
[`gofractally/psiserve` at `0c2004ec770d9b391e3dcebe8b78b5f8cf7bcdcd`](https://github.com/gofractally/psiserve/tree/0c2004ec770d9b391e3dcebe8b78b5f8cf7bcdcd/libraries/psizam).

## Bundled dependencies

| Component | Bundled source and use | Retained license | Installed license copy |
| --- | --- | --- | --- |
| [Berkeley SoftFloat](https://github.com/AntelopeIO/berkeley-softfloat-3), Release 3e | `external/softfloat`; floating-point implementation linked into the default runtime | `external/softfloat/COPYING.txt`; its three redistribution conditions, copyright, and disclaimer are preserved verbatim | `licenses/SoftFloat-COPYING.txt` |
| [Catch2](https://github.com/catchorg/Catch2) | `external/Catch2`; test framework | `external/Catch2/LICENSE.txt`; Boost Software License 1.0 | `licenses/Catch2-LICENSE.txt` |
| [wasm3](https://github.com/wasm3/wasm3) | `external/wasm3`; optional reference interpreter for differential fuzzing | `external/wasm3/LICENSE`; MIT, copyright 2019 Steven Massey and Volodymyr Shymanskyy | `licenses/wasm3-LICENSE.txt` |
| `ucc::typed_int` | `external/ucc/include/ucc/typed_int.hpp`; support header | `external/ucc/LICENSE`; MIT, copyright 2026 Daniel Larimer | `licenses/ucc-LICENSE.txt` |

The `ucc/typed_int.hpp` copy matches
[`libraries/ucc/include/ucc/typed_int.hpp`](https://github.com/gofractally/arbtrie/blob/5521ac22228a6694138c9f6d3b2f4a91bd4f8f76/libraries/ucc/include/ucc/typed_int.hpp)
at `gofractally/arbtrie` commit `5521ac22228a6694138c9f6d3b2f4a91bd4f8f76`, the
`external/psitri` revision pinned by the extraction's psiserve snapshot.
Its license was copied without alteration from that commit's root
[`LICENSE`](https://github.com/gofractally/arbtrie/blob/5521ac22228a6694138c9f6d3b2f4a91bd4f8f76/LICENSE).

## WASI C++ support headers

`external/wasi/include/wasi` was copied from
[`libraries/wasi/cpp/include/wasi`](https://github.com/gofractally/psiserve/tree/0c2004ec770d9b391e3dcebe8b78b5f8cf7bcdcd/libraries/wasi/cpp/include/wasi)
in the same psiserve snapshot, then adapted to the current psio API. These are
the psiserve C++ bindings and helper types, rather than a copy of a WASI SDK
sysroot. The source snapshot contains no separate license file for this header
tree. This inventory records its provenance without asserting an additional
license or replacing any source-file notices.

## Optional test corpus and external build inputs

The `tests/spec/testsuite` submodule comes from
[`WebAssembly/testsuite`](https://github.com/WebAssembly/testsuite/tree/51279a9d02cbba193cb25142d115388d7b83299c)
at commit `51279a9d02cbba193cb25142d115388d7b83299c`. Its own `LICENSE` is Apache
License 2.0 and remains with the submodule. This test corpus is not installed
with the runtime.

Psio, Boost, optional LLVM libraries, WASI SDKs, and downloaded compiler or
benchmark inputs are separate dependencies. They retain their own licenses;
the notices above do not relicense those components. The benchmark source
manifest records the fetched source and license inputs.
