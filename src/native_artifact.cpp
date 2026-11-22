#include <psizam/native_artifact.hpp>
#include <psizam/pzam_compile.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include "psizam-native-build.hpp"

namespace psizam {
   std::string_view native_aot_build_identity() noexcept {
      return PSIZAM_NATIVE_BUILD_ID;
   }

   std::vector<char> save_native_aot(std::span<const uint8_t> wasm,
                                     const module& mod,
                                     const detail::pzam_compile_result& result,
                                     const std::array<uint8_t, 32>& identity) {
      if (!result.error.empty())
         throw std::runtime_error("LLVM AOT: " + result.error);
      if (wasm.size() > std::numeric_limits<uint32_t>::max())
         throw std::length_error("native AOT: WASM source exceeds artifact offset range");
      pzam_file file;
      file.input_hash = pzam_cache::hash_wasm(wasm);
      file.metadata = extract_metadata(mod);
#if defined(__x86_64__)
      constexpr auto arch = pzam_arch::x86_64;
#elif defined(__aarch64__)
      constexpr auto arch = pzam_arch::aarch64;
#else
#error Unsupported native AOT architecture
#endif
      file.code_sections.push_back(detail::make_pzam_code_section(
         mod, result, arch, detail::llvm_pzam_tier(result.opt_level),
         result.softfloat, result.backtrace));
      auto& contract = file.code_sections.back().native_contract.emplace();
      contract.identity = identity;
      for (size_t i = 0; i < result.body_offsets.size(); ++i) {
         const auto& source = mod.code.at(i);
         auto [offset, size] = result.body_offsets[i];
         if (size && source.body_start) {
            const auto source_address = reinterpret_cast<uintptr_t>(source.body_start);
            const auto wasm_address = reinterpret_cast<uintptr_t>(wasm.data());
            const auto wasm_offset = source_address - wasm_address;
            if (source_address < wasm_address || wasm_offset > wasm.size() ||
                source.wasm_body_bytes > wasm.size() - wasm_offset)
               throw std::runtime_error("native AOT: source range is outside WASM input");
            contract.sources.push_back({offset, size, static_cast<uint32_t>(wasm_offset),
                                         source.wasm_body_bytes});
         }
      }
      return pzam_save(file);
   }

   module load_native_aot(std::span<const char> artifact, native_aot_metadata* metadata,
                          const std::array<uint8_t, 32>& expected_identity,
                          std::span<const uint8_t> expected_wasm) {
      auto file = pzam_load(artifact);
      if (file.code_sections.size() != 1 ||
          detail::derive_reverse_host_args(file.code_sections.front().opt_tier))
         throw std::runtime_error("native AOT: expected a single LLVM code section");
      const auto& section = file.code_sections.front();
      if (!section.native_contract)
         throw std::runtime_error("native AOT: missing embedder contract");
      if (section.native_contract->identity != expected_identity)
         throw std::runtime_error("native AOT: policy or compiler identity mismatch");
      if (!expected_wasm.empty() && file.input_hash != pzam_cache::hash_wasm(expected_wasm))
         throw std::runtime_error("native AOT: WASM input mismatch");
      for (const auto& source : section.native_contract->sources) {
         if (uint64_t(source.native_offset) + source.native_size > section.code_blob.size() ||
             (!expected_wasm.empty() &&
              uint64_t(source.wasm_offset) + source.wasm_size > expected_wasm.size()))
            throw std::runtime_error("native AOT: invalid source map");
      }
      if (metadata)
         *metadata = *section.native_contract;
      auto loaded = detail::load_pzam(file);
      // shared_ptr invokes the deleter even if allocating its control block
      // fails. Take ownership before moving or finalizing any module state.
      std::shared_ptr<void> owner(loaded.exec_code, [](void* p) {
         jit_allocator::instance().free(p);
      });
      loaded.mod.jit_engine = std::move(owner);
      module result = std::move(loaded.mod);
      loaded.mod.allocator = growable_allocator{};
      return result;
   }
}
