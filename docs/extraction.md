# Standalone extraction

Engine history split from psiserve `0c2004ec770d9b391e3dcebe8b78b5f8cf7bcdcd` at `libraries/psizam`.
Split commit: `8340279de346572c85dfb201d43043a015841c99`.

The standalone history is published at
[gofractally/psizam](https://github.com/gofractally/psizam). The split preserves
the original engine history; later commits contain standalone packaging, current
psio/PSSZ integration, runtime fixes and validation. Publication does not certify
a stable release; the remaining gates are in `docs/runtime-release-review.md`.

The source subtree at
[`psiserve/main@28330232`](https://github.com/gofractally/psiserve/tree/28330232d7d156d201161dba3485a737f2b28a07/libraries/psizam)
has Git tree `53646a3fffd29f97765ca1c11476d8dc25352ce9`, exactly matching the
split commit's tree. The standalone repository retains newer changes; replacing
it with that published psiserve subtree would discard them.

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
