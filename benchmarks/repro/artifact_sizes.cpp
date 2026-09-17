// Untimed structural measurements. Never execute the inspected guest.
#include <psizam/pzam_format.hpp>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
   try {
      if (argc < 3) throw std::invalid_argument("usage: repro-code-size pzam FILE");
      std::ifstream input(argv[2], std::ios::binary);
      if (!input) throw std::runtime_error("cannot open input");
      std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
      if (std::string{argv[1]} == "pzam") {
         auto file = psizam::pzam_load({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
         std::printf("{\"artifact_bytes\":%zu,\"sections\":[", bytes.size());
         bool first = true;
         for (const auto& section : file.code_sections) {
            uint64_t functions = 0;
            for (const auto& function : section.functions) functions += function.code_size;
            std::printf("%s{\"arch\":%u,\"tier\":%u,\"gas_metering\":%u,\"native_payload_bytes\":%zu,\"entry_range_bytes\":%llu,\"functions\":%zu,\"relocations\":%zu}",
               first ? "" : ",", section.arch, section.opt_tier, section.instrumentation.gas_metering,
               section.code_blob.size(), static_cast<unsigned long long>(functions),
               section.functions.size(), section.relocations.size());
            first = false;
         }
         std::puts("]}");
      } else throw std::invalid_argument("unknown format");
   } catch (const std::exception& e) {
      std::fprintf(stderr, "%s\n", e.what()); return 1;
   }
}
