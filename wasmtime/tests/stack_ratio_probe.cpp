// Measures how much native stack one activation of a wasm function takes on
// the wasmtime backend against what the deterministic byte budget charges for
// it (gofractally/psibase#2279). Not a registered test.
//
//   psizam_wasmtime_stack_ratio_probe module.wat|module.wasm [levels [functions]]
//
// The module imports env.mark (no parameters) and exports functions of type
// f1..fN of type (param i32): each recurses that many levels and calls mark at
// the bottom. For every such export the probe prints the native bytes and the budget
// bytes one level took, and their ratio.
#include <psizam/backend.hpp>
#include <psizam/wasmtime_admission.hpp>
#include <psizam/wasmtime_backend.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace wt = psizam::wasmtime;

namespace
{
   struct stack_options : psizam::default_options
   {
      static constexpr uint32_t max_func_local_bytes = 128 * 1024 * 1024;
      static constexpr auto     max_func_local_bytes_flags =
          psizam::max_func_local_bytes_flags_t::params |
          psizam::max_func_local_bytes_flags_t::locals |
          psizam::max_func_local_bytes_flags_t::stack;
      uint32_t max_stack_bytes = 65536;
      stack_options()
      {
         gas_metering = false;
         defer_start  = true;
      }
   };

   struct depth_host
   {
      std::function<uint32_t()> remaining;
      uintptr_t                 deepest         = UINTPTR_MAX;
      uint32_t                  left_at_deepest = 0;
      void                      mark()
      {
         auto here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
         if (here < deepest)
         {
            deepest         = here;
            left_at_deepest = remaining();
         }
      }
   };
}  // namespace

int main(int argc, char** argv)
{
   if (argc < 2)
   {
      std::fprintf(stderr, "usage: %s module.wat|module.wasm [levels [functions]]\n", argv[0]);
      return 2;
   }
   const uint32_t levels    = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 64;
   const int      functions = argc > 3 ? std::atoi(argv[3]) : 1;
   std::ifstream  in(argv[1], std::ios::binary);
   std::string    text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
   if (text.empty())
   {
      std::fprintf(stderr, "cannot read %s\n", argv[1]);
      return 2;
   }
   try
   {
      constexpr uint32_t budget = 1u << 30;
      wt::engine_options options;
      options.native_stack = 2048ull << 20;
      wt::runtime<depth_host> rt(options);
      rt.add<&depth_host::mark>("env", "mark");
      psizam::wasm_code code = wt::assemble(text);
      stack_options     psizam_options;
      psizam_options.max_stack_bytes = budget;
      auto costs  = wt::validate_and_cost(code, psizam_options);
      auto module = rt.compile_metered(code, costs);

      double worst = 0;
      for (int i = 1; i <= functions; ++i)
      {
         std::string name = "f" + std::to_string(i);
         depth_host  deep, top;
         {
            wt::instance<depth_host> inst(rt, module, top);
            top.remaining = [&] { return inst.stack_remaining(); };
            inst.set_stack_remaining(budget);
            inst.call_discarding_results(name, 0u);
         }
         {
            wt::instance<depth_host> inst(rt, module, deep);
            deep.remaining = [&] { return inst.stack_remaining(); };
            inst.set_stack_remaining(budget);
            inst.call_discarding_results(name, levels);
         }
         double native  = (double(top.deepest) - double(deep.deepest)) / levels;
         double logical = (double(top.left_at_deepest) - double(deep.left_at_deepest)) / levels;
         double ratio   = logical > 0 ? native / logical : 0;
         std::printf("%s %s native_per_level=%.0f budget_per_level=%.0f ratio=%.2f\n", argv[1],
                     name.c_str(), native, logical, ratio);
         if (ratio > worst)
            worst = ratio;
      }
      std::printf("%s worst_ratio=%.2f\n", argv[1], worst);

      // With a native stack, a budget and a depth given: recurse that deep.
      // The same depth under two native stack sizes shows whether the budget
      // or the native stack is what a module runs into.
      if (argc > 5)
      {
         const uint64_t     native_mib = std::strtoull(argv[4], nullptr, 10);
         const uint32_t     run_budget = static_cast<uint32_t>(std::strtoull(argv[5], nullptr, 10));
         wt::engine_options bounded;
         bounded.native_stack = native_mib << 20;
         wt::runtime<depth_host> rt2(bounded);
         rt2.add<&depth_host::mark>("env", "mark");
         stack_options run_options;
         run_options.max_stack_bytes = run_budget;
         psizam::wasm_code code2    = wt::assemble(text);
         auto              costs2   = wt::validate_and_cost(code2, run_options);
         depth_host        h;
         wt::instance<depth_host> inst(rt2, rt2.compile_metered(code2, costs2), h);
         h.remaining = [&] { return inst.stack_remaining(); };
         inst.set_stack_remaining(run_budget);
         try
         {
            const uint32_t depth = argc > 6 ? static_cast<uint32_t>(std::strtoull(argv[6], nullptr, 10)) : 0x7fffffffu;
            inst.call_discarding_results("f1", depth);
            std::printf("%s bounded: native_stack=%lluMiB budget=%u depth=%u returned normally\n",
                        argv[1], (unsigned long long)native_mib, run_budget, depth);
         }
         catch (const wt::trap& t)
         {
            std::printf("%s bounded: native_stack=%lluMiB budget=%u trap=\"%s\" kind=%d\n",
                        argv[1], (unsigned long long)native_mib, run_budget, t.what(),
                        int(t.kind));
         }
      }
   }
   catch (const std::exception& e)
   {
      std::printf("%s ERROR %s\n", argv[1], e.what());
      return 1;
   }
   return 0;
}
