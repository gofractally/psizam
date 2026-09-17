#include <psizam/runtime.hpp>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
   try {
      if (argc < 2) throw std::runtime_error("expected fixture paths");
      std::ifstream in(argv[1], std::ios::binary);
      if (!in) throw std::runtime_error("fixture not found");
      std::vector<uint8_t> wasm{std::istreambuf_iterator<char>(in), {}};
      psizam::runtime rt({.guarded_pool_size = 0});
      psizam::instance_policy policy;
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      policy.initial = psizam::instance_policy::compile_tier::jit_llvm;
      const auto expected = psizam::backend_kind::jit_llvm;
#else
      policy.initial = psizam::instance_policy::compile_tier::interpret;
      const auto expected = psizam::backend_kind::interpreter;
#endif
      auto module = rt.prepare(psizam::wasm_bytes{wasm}, policy);
      auto instance = rt.instantiate(module);
      if (instance.kind() != expected) throw std::runtime_error("wrong installed factory");
      if (instance.run_start() != 0) throw std::runtime_error("guest failed");
      uint32_t result = 0;
      std::memcpy(&result, instance.linear_memory(), sizeof(result));
      if (result != 42) throw std::runtime_error("guest result mismatch");
      for (int i = 2; i < argc; ++i) {
         std::ifstream input(argv[i], std::ios::binary);
         if (!input) throw std::runtime_error("PZAM fixture not found");
         std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
         auto cached = rt.load_cached(psizam::pzam_bytes{bytes});
         auto saved = rt.instantiate(cached);
         if (saved.run_start() != 0) throw std::runtime_error("saved guest failed");
         std::memcpy(&result, saved.linear_memory(), sizeof(result));
         if (result != 42) throw std::runtime_error("saved guest result mismatch");
      }
      std::cout << "Installed runtime factory and cached execution passed\n";
   } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return 1;
   }
}
