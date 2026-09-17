// Process-isolated validation of public compilation and explicit FP modes.
#include <psizam/backend.hpp>
#include <psizam/detail/wasi_host.hpp>
#include <psizam/pzam_compile.hpp>
#include <fstream>
#include <iostream>

using namespace psizam;
using namespace psizam::detail;
struct validation_options : default_options {
   fp_mode fp = fp_mode::fast;
   uint32_t compile_threads = 1;
   // Match pzam-run's limit for the real compiler workloads.
   uint32_t max_call_depth = 8192;
};

template<class Impl>
int execute(wasm_code& code, host_function_table table, wasi_host& host,
            const validation_options& options) {
   wasm_allocator allocator;
   backend<std::nullptr_t, Impl, validation_options> engine(
      code, std::move(table), &host, &allocator, options);
   engine.call(&host, "_start");
   return host.exit_code;
}

int main(int argc, char** argv) {
   try {
      if (argc == 5 && std::string_view(argv[1]) == "compile") {
         const std::string_view mode = argv[2];
         if (mode != "jit2" && mode != "llvm-o2") return 2;
         const auto tier = mode == "jit2" ? pzam_tier_flags::jit2 : pzam_tier_flags::llvm_O2;
         auto bytes = pzam_compile(read_wasm(argv[3]), {.tier=tier});
         std::ofstream output(argv[4], std::ios::binary);
         output.write(bytes.data(), bytes.size());
         output.close();
         return output ? 0 : 2;
      }
      if (argc < 5 || std::string_view(argv[1]) != "run") return 2;
      const std::string_view mode = argv[2], fp = argv[3];
      if (fp != "fast" && fp != "softfloat") return 2;
      validation_options options;
      options.fp = fp == "fast" ? fp_mode::fast : fp_mode::softfloat;
      wasi_host host;
      int i = 4;
      for (; i < argc && std::string_view(argv[i]).starts_with("--dir="); ++i) {
         std::string value = argv[i] + 6;
         auto colon = value.find(':');
         if (colon == std::string::npos) return 2;
         host.add_preopen(value.substr(0, colon), value.substr(colon + 1));
      }
      if (i == argc) return 2;
      auto code = read_wasm(argv[i]);
      for (; i < argc; ++i) host.args.emplace_back(argv[i]);
      host_function_table table;
      register_wasi(table);
      if (mode == "interpreter") return execute<interpreter>(code, std::move(table), host, options);
      if (mode == "jit1") return execute<jit>(code, std::move(table), host, options);
      if (mode == "jit2") return execute<jit2>(code, std::move(table), host, options);
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      set_llvm_opt_level(2);
      if (mode == "llvm") return execute<jit_llvm>(code, std::move(table), host, options);
#endif
      return 2;
   } catch (const wasi_host::wasi_exit_exception& e) {
      return e.code;
   } catch (const std::exception& e) {
      std::cerr << "validation driver: " << e.what() << '\n';
      return 1;
   }
}
