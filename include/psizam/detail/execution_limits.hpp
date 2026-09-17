#pragma once

#include <psizam/detail/watchdog.hpp>
#include <psizam/detail/signals.hpp>
#include <psizam/gas.hpp>
#include <psizam/exceptions.hpp>
#include <psizam/utils.hpp>
#include <chrono>

namespace psizam::detail {

// CPU-work policies. Timeouts measure elapsed wall time, not process CPU time.
// The caller owns the module exclusively while a page timeout is armed.
enum class cpu_limit { none, gas, timeout, timeout_poll };
struct execution_limits {
   cpu_limit policy = cpu_limit::none;
   uint64_t gas_budget = 1'000'000'000'000ull;
   std::chrono::milliseconds duration{120'000};
   uint64_t poll_slice = 10'000;
};

// Page deadlines protect all registered executable mappings, including LLVM ORC.
template<class Module, class Context, class F>
void with_execution_limits(Module& mod, Context& ctx, const execution_limits& limits,
                           F&& invoke, bool native_code = true) {
   if (limits.policy == cpu_limit::gas || limits.policy == cpu_limit::timeout_poll) {
      PSIZAM_ASSERT(mod.gas_metering, wasm_interpreter_exception,
                    "resource limit requires gas-instrumented code");
      struct poll_state {
         std::chrono::steady_clock::time_point deadline;
         uint64_t slice;
      } poll{std::chrono::steady_clock::now() + limits.duration, limits.poll_slice};
      auto reset = scope_guard{[&] { ctx.set_gas_handler(nullptr); }};
      ctx.set_gas_budget(gas_units{limits.policy == cpu_limit::gas
                                    ? limits.gas_budget : limits.poll_slice});
      if (limits.policy == cpu_limit::timeout_poll) {
         ctx.set_gas_handler(+[](gas_state* gas, void* data) {
            auto& p = *static_cast<poll_state*>(data);
            if (std::chrono::steady_clock::now() >= p.deadline)
               throw timeout_exception{"execution timed out"};
            gas->deadline.store(gas->consumed + p.slice, std::memory_order_relaxed);
         }, &poll);
      } else ctx.set_gas_handler(nullptr);
      std::forward<F>(invoke)();
   } else if (limits.policy == cpu_limit::timeout) {
      PSIZAM_ASSERT(mod.allocator.has_code(), wasm_interpreter_exception,
                    "page timeout requires registered executable mappings");
      auto& timed_out = timed_run_has_timed_out;
      auto restore = scope_guard{[&] {
         if (timed_out.load(std::memory_order_acquire)) {
            mod.allocator.enable_code(native_code);
            timed_out.store(false, std::memory_order_release);
         }
      }};
      try {
         auto guard = watchdog{limits.duration}.scoped_run([&] {
            timed_out.store(true, std::memory_order_release);
            mod.allocator.disable_code();
         });
         std::forward<F>(invoke)();
      } catch (wasm_memory_exception&) {
         if (timed_out.load(std::memory_order_acquire))
            throw timeout_exception{"execution timed out"};
         throw;
      }
   } else std::forward<F>(invoke)();
}
} // namespace psizam::detail
