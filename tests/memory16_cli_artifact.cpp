// Use the maintained marker and PZAM reader/loader to inspect CLI output. No
// duplicate binary parser and no compiler fallback participates in this gate.
#include <psizam/backend.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include <psizam/execution_profile.hpp>
#include <psizam/pzam_format.hpp>

#include <fstream>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
   using namespace psizam;
   try {
      if (argc == 5 && std::string_view(argv[1]) == "tag") {
         auto wasm = read_wasm(argv[2]);
         append_execution_profile(wasm, std::stoul(argv[3]));
         std::ofstream out(argv[4], std::ios::binary);
         out.write(reinterpret_cast<const char*>(wasm.data()), wasm.size());
         return out ? 0 : 1;
      }
      if (argc != 5 || std::string_view(argv[1]) != "check") return 2;
      const auto expected_profile = uint32_t(std::stoul(argv[3]));
      const auto expected_tier = uint8_t(std::stoul(argv[4]));
      std::ifstream in(argv[2], std::ios::binary);
      if (!in) return 3;
      std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
      auto file = pzam_load(bytes);
      if (file.metadata.execution_profile.value_or(0) != expected_profile || file.code_sections.size() != 1)
         return 4;
      const auto& section = file.code_sections.front();
      if (section.execution_profile.value_or(0) != expected_profile || section.opt_tier != expected_tier)
         return 5;
      auto loaded = detail::load_pzam(file);
      std::shared_ptr<void> owner(loaded.exec_code, [](void* p) { jit_allocator::instance().free(p); });
      wasm_allocator memory;
      detail::jit_execution_context<> context(loaded.mod, 8192);
      context.set_wasm_allocator(&memory);
      context.reset();
      bool trapped = false;
      try {
         context.execute(nullptr, detail::jit_visitor{nullptr}, loaded.mod.get_exported_function("_start"));
      } catch (const psizam::exception&) {
         trapped = true;
      }
      if (expected_profile) {
         if (trapped || !memory.is_memory16() || memory.get_base_ptr<uint8_t>()[0] != 123) return 6;
      } else if (!trapped || memory.is_memory16()) return 7;
      std::cout << "format=" << file.format_version << " profile=" << expected_profile
                << " tier=" << unsigned(section.opt_tier) << " trapped=" << trapped << '\n';
      return 0;
   } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return 8;
   }
}
