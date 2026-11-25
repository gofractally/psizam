// What it costs to enter a service: an export looked up by name on every
// call, against the same export bound once. Not a registered test.

#include <psizam/backend.hpp>
#include <psizam/wasmtime_admission.hpp>
#include <psizam/wasmtime_backend.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace wt = psizam::wasmtime;

struct bench_options : psizam::default_options
{
   static constexpr uint32_t max_func_local_bytes = 128 * 1024 * 1024;
   static constexpr auto     max_func_local_bytes_flags =
       psizam::max_func_local_bytes_flags_t::params | psizam::max_func_local_bytes_flags_t::locals |
       psizam::max_func_local_bytes_flags_t::stack;
   uint32_t max_stack_bytes = 1 << 20;
   bench_options()
   {
      gas_metering = false;
      defer_start  = true;
   }
};

struct host
{
};

// A service-shaped module: the entry point takes two i64 and does nothing,
// among a number of other exports as a real service has.
static const char* module_text = R"((module
  (memory (export "memory") 1)
  (func (export "start") (param i64))
  (func (export "serve"))
  (func (export "processTransaction"))
  (func (export "verify"))
  (func (export "aaa") (param i32))
  (func (export "bbb") (param i32))
  (func (export "called") (param i64 i64))
))";

template <typename F>
static double ns_per_call(F f, int calls, int rounds)
{
   double best = 1e99;
   for (int r = 0; r < rounds; ++r)
   {
      auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < calls; ++i)
         f();
      double t =
          std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
      best = std::min(best, t / calls);
   }
   return best;
}

int main(int argc, char** argv)
{
   const int calls = argc > 1 ? std::atoi(argv[1]) : 2000000;
   wt::engine_options options;
   options.native_stack = 64ull << 20;
   wt::runtime<host> rt(options);
   psizam::wasm_code code = wt::assemble(module_text);
   bench_options     psizam_options;
   auto              costs = wt::validate_and_cost(code, psizam_options);
   host              h;
   wt::instance<host> inst(rt, rt.compile_metered(code, costs), h);
   inst.set_stack_remaining(1 << 20);

   const double by_name = ns_per_call(
       [&] { inst.call_discarding_results("called", uint64_t(1), uint64_t(2)); }, calls, 5);
   auto         bound   = inst.bind<void(uint64_t, uint64_t)>("called");
   const double handle  = ns_per_call([&] { bound(1, 2); }, calls, 5);
   const double entered = ns_per_call([&] { inst.enter(wt::entry_point::called, 1, 2); }, calls, 5);
   std::printf("by name, every call:            %7.1f ns\n", by_name);
   std::printf("handle bound once, value array: %7.1f ns\n", handle);
   std::printf("entry point:                    %7.1f ns\n", entered);
   return 0;
}
