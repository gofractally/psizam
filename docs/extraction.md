# Standalone extraction

Engine history split from psiserve `0c2004ec770d9b391e3dcebe8b78b5f8cf7bcdcd` at `libraries/psizam`.
Split commit: `8340279de346572c85dfb201d43043a015841c99`. No remote or release created.

Vendored from that same psiserve tree: namespaced legacy psio1 C++ headers and
source, rapidjson, the generator tool, WASI C++ headers, CMake WASM/LLVM helpers,
build scripts, and psitri's `ucc/typed_int.hpp`.

Current psio is an explicit dependency. For co-development set
`FETCHCONTENT_SOURCE_DIR_PSIO` to the independent psio checkout, or install psio
and set CMAKE_PREFIX_PATH. The installed dependency must include the new CMake package configuration.
No implicit dependency on a sibling psiserve checkout or moving remote branch is used.
The psio packaging changes must be tagged/published before distributing a release.

Legacy psio1 is required for PZAM metadata and component APIs. Removing this
compatibility snapshot requires an API migration.

RapidJSON snapshot: `24b5e7a8b27f42fa16b96fc70aade9106cf7102f`.
