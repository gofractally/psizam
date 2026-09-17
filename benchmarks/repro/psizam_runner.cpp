// Benchmark-only WASI runner. Explicit hardware FP and one compilation worker.
// Backend setup includes parsing, code generation, linking and instantiation.
#include <psizam/backend.hpp>
#include "../../tools/resource_limit_options.hpp"
#include <psizam/detail/wasi_host.hpp>
#include <chrono>
#include <cstdio>
#include <string>

using namespace psizam;
using namespace psizam::detail;
using timer = std::chrono::steady_clock;
struct bench_options : default_options {
   fp_mode fp = fp_mode::hw_deterministic;
   uint32_t compile_threads = 1;
   bool defer_start = true;
   uint32_t max_call_depth = 8192; // Match pzam-run for compiler workloads.
};

template<class Impl>
int run(wasm_code& code, host_function_table table, wasi_host& host, const execution_limits& limits) {
   bench_options options;
   options.gas_metering = limits.policy == cpu_limit::gas || limits.policy == cpu_limit::timeout_poll;
   wasm_allocator allocator;
   const auto begin = timer::now();
   backend<std::nullptr_t, Impl, bench_options> engine(code, std::move(table), &host, &allocator, options);
   const auto ready = timer::now();
   int result = 0;
   try { with_execution_limits(engine.get_module(), engine.get_context(), limits,
      [&] {
         auto& ctx = engine.get_context();
         ctx.execute_start(&host, interpret_visitor(ctx));
         engine.call(&host, "_start");
      }, Impl::is_jit); }
   catch (const wasi_host::wasi_exit_exception& e) { result = e.code; }
   const auto end = timer::now();
   std::fprintf(stderr, "BENCH_GAS_CONSUMED=%llu\n", (unsigned long long)engine.get_context().gas().consumed);
   std::fprintf(stderr, "\nBENCH_SETUP_NS=%lld\nBENCH_START_NS=%lld\n",
      (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(ready-begin).count(),
      (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(end-ready).count());
   return result ? result : host.exit_code;
}

int main(int argc, char** argv) {
   if (argc < 3) { std::fprintf(stderr, "Usage: repro-psizam jit|jit2|llvm|interpreter [--cpu-limit=none|gas|timeout|timeout-poll] [--gas-budget=N] [--timeout-ms=N] [--dir=guest:host] module.wasm [args]\n"); return 2; }
   try {
      const std::string backend_name = argv[1];
      wasi_host host;
      execution_limits limits;
      int i = 2;
      for (; i < argc && std::string_view(argv[i]).starts_with("--"); ++i) {
         if (parse_limit_option(argv[i], limits)) continue;
         if (!std::string_view(argv[i]).starts_with("--dir=")) return 2;
         std::string value = argv[i] + 6;
         auto colon = value.find(':');
         if (colon == std::string::npos) host.add_preopen(value, value);
         else host.add_preopen(value.substr(0,colon),value.substr(colon+1));
      }
      if (i == argc) return 2;
      auto code = read_wasm(argv[i]);
      for (; i < argc; ++i) host.args.emplace_back(argv[i]);
      host_function_table table;
      register_wasi(table);
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      set_llvm_opt_level(2);
#endif
      if (backend_name == "jit") return run<jit>(code,std::move(table),host,limits);
      if (backend_name == "jit2") return run<jit2>(code,std::move(table),host,limits);
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      if (backend_name == "llvm") return run<jit_llvm>(code,std::move(table),host,limits);
#endif
      if (backend_name == "interpreter") return run<interpreter>(code,std::move(table),host,limits);
      return 2;
   } catch (const wasm_gas_exhausted_exception&) {
      std::fprintf(stderr,"RESOURCE_LIMIT=gas\n"); return 75;
   } catch (const timeout_exception&) {
      std::fprintf(stderr,"RESOURCE_LIMIT=timeout\n"); return 76;
   } catch (const psizam::exception& e) {
      std::fprintf(stderr,"psizam: %s: %s\n",e.what(),e.detail());
   } catch (const std::exception& e) { std::fprintf(stderr,"error: %s\n",e.what()); }
   return 1;
}
