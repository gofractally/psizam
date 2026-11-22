#pragma once

// Codec-free boundary for embedders which have not migrated their own wire
// format to current PSIO. The implementation uses the canonical PZAM codec;
// neither PZAM's reflected types nor PSIO types cross this boundary.
#include <psizam/types.hpp>
#include <psizam/detail/pzam_compile_result.hpp>
#include <span>
#include <vector>
#include <array>
#include <string_view>

namespace psizam {
   // Fingerprint of the exact native runtime/codec sources, canonical codec
   // dependencies and configured compiler/toolchain. Bind persistent cache keys
   // to this value as well as the actual Wasm bytes and execution options.
   std::string_view native_aot_build_identity() noexcept;

   struct native_aot_source_range {
      uint32_t native_offset = 0;
      uint32_t native_size = 0;
      uint32_t wasm_offset = 0;
      uint32_t wasm_size = 0;
   };
   struct native_aot_metadata {
      // Opaque identity supplied by the embedder. Persistent caches must bind
      // it to code, every policy option, target and compiler/runtime revisions.
      std::array<uint8_t, 32> identity = {};
      std::vector<native_aot_source_range> sources;
   };
   /// Save a single host-architecture LLVM AOT section. The module must have
   /// been parsed with the embedder's actual Options and finalized first.
   std::vector<char> save_native_aot(std::span<const uint8_t> wasm,
                                     const module& mod,
                                     const detail::pzam_compile_result& result,
                                     const std::array<uint8_t, 32>& identity = {});

   /// Load an artifact produced locally by save_native_aot. This is executable
   /// native code, not a sandboxed input format: never load untrusted artifacts.
   /// The returned module owns its RX mapping through module::jit_engine.
   module load_native_aot(std::span<const char> artifact,
                          native_aot_metadata* metadata = nullptr,
                          const std::array<uint8_t, 32>& expected_identity = {},
                          std::span<const uint8_t> expected_wasm = {});
}
