# Standalone extraction

Engine history split from psiserve `0c2004ec770d9b391e3dcebe8b78b5f8cf7bcdcd` at `libraries/psizam`.
Split commit: `8340279de346572c85dfb201d43043a015841c99`. No remote or release created.

Vendored from that same psiserve tree: WASI C++ headers, CMake WASM/LLVM helpers,
build scripts, and psitri's `ucc/typed_int.hpp`.

Current psio is an explicit dependency. For co-development set
`FETCHCONTENT_SOURCE_DIR_PSIO` to the independent psio checkout, or install psio
and set CMAKE_PREFIX_PATH. The installed dependency must include the new CMake package configuration.
No implicit dependency on a sibling psiserve checkout or moving remote branch is used.
The psio packaging changes must be tagged/published before distributing a release.

PZAM serialization uses current psio's pSSZ format, the successor to the legacy
fracpack prototype retained in psio for benchmark comparisons. WIT interfaces,
component bindings, and owning buffers use current psio. Wasm/DWARF byte-stream
operations live in the engine's debug utilities. The engine has no psio1 or
RapidJSON dependency.
