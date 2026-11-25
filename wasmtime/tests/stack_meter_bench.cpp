// What deterministic stack accounting costs on the wasmtime backend.
//
// Runs call-heavy and call-free functions from one module compiled with its
// real stack costs and with every cost zero, and prints the time per wasm
// call. The budget travels as a hidden argument either way, so the
// difference is the compare against the cost and the subtraction. Not a
// registered test: it measures, it does not assert.

#include <psizam/backend.hpp>
#include <psizam/wasmtime_admission.hpp>
#include <psizam/wasmtime_backend.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

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

// fib:   two recursive calls per call, almost no other work.
// leaf:  a loop that calls a one-instruction function each iteration.
// spin:  the same loop with the addition inline: no calls at all.
// mixed: a loop calling a function that does a little arithmetic and a
//        memory load and store, closer to ordinary service code.
static const char* module_text = R"((module
  (memory 1)
  (func $fib (export "fib") (param i32) (result i32)
    (if (result i32) (i32.lt_u (local.get 0) (i32.const 2))
      (then (local.get 0))
      (else (i32.add (call $fib (i32.sub (local.get 0) (i32.const 1)))
                     (call $fib (i32.sub (local.get 0) (i32.const 2)))))))
  (func $inc (param i32) (result i32) (i32.add (local.get 0) (i32.const 1)))
  (func (export "leaf") (param i32) (result i32) (local i32)
    (loop $l
      (local.set 1 (call $inc (local.get 1)))
      (br_if $l (i32.lt_u (local.get 1) (local.get 0))))
    (local.get 1))
  (func (export "spin") (param i32) (result i32) (local i32)
    (loop $l
      (local.set 1 (i32.add (local.get 1) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get 1) (local.get 0))))
    (local.get 1))
  (func $work (param i32) (result i32) (local i32)
    (local.set 1 (i32.and (i32.mul (local.get 0) (i32.const 2654435761)) (i32.const 0xfffc)))
    (i32.store (local.get 1) (i32.add (i32.load (local.get 1)) (local.get 0)))
    (i32.add (local.get 0) (i32.const 1)))
  (func (export "mixed") (param i32) (result i32) (local i32)
    (loop $l
      (local.set 1 (call $work (local.get 1)))
      (br_if $l (i32.lt_u (local.get 1) (local.get 0))))
    (local.get 1))
))";

template <typename F>
static double best_seconds(F f, int rounds)
{
   double best = 1e99;
   for (int i = 0; i < rounds; ++i)
   {
      auto start = std::chrono::steady_clock::now();
      f();
      best = std::min(
          best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
   }
   return best;
}

int main(int argc, char** argv)
{
   const int rounds = argc > 1 ? std::atoi(argv[1]) : 7;
   wt::engine_options options;
   options.native_stack = 64ull << 20;
   wt::runtime<host> rt(options);

   psizam::wasm_code code = wt::assemble(module_text);
   bench_options     psizam_options;
   auto              costs = wt::validate_and_cost(code, psizam_options);

   host               h;
   wt::instance<host> free_of_cost(rt, rt.compile(code), h);
   wt::instance<host> metered(rt, rt.compile_metered(code, costs), h);

   struct shape
   {
      const char* name;
      uint32_t    arg;
      double      calls;  // wasm calls made by one run
   };
   // fib(n) makes 2*fib(n+1)-1 calls; fib(35) = 9227465.
   const shape shapes[] = {
       {"fib", 34, 2.0 * 9227465 - 1},
       {"leaf", 200000000, 200000000.0},
       {"mixed", 100000000, 100000000.0},
       {"spin", 400000000, 0},
   };
   std::printf("%-6s %12s %12s %9s %14s\n", "shape", "zero-cost s", "metered s", "ratio",
               "added ns/call");
   for (const auto& s : shapes)
   {
      const uint32_t expect = free_of_cost.call<uint32_t, uint32_t>(s.name, s.arg);
      auto           time   = [&](wt::instance<host>& inst)
      {
         return best_seconds(
             [&]
             {
                inst.set_stack_remaining(1 << 20);
                if (inst.call<uint32_t, uint32_t>(s.name, s.arg) != expect)
                   std::abort();
             },
             rounds);
      };
      const double z = time(free_of_cost), m = time(metered);
      if (s.calls > 0)
         std::printf("%-6s %12.4f %12.4f %9.3f %14.3f\n", s.name, z, m, m / z,
                     (m - z) * 1e9 / s.calls);
      else
         std::printf("%-6s %12.4f %12.4f %9.3f %14s\n", s.name, z, m, m / z, "-");
   }
   return 0;
}
