#pragma once

#include "runtime_backend.hpp"
#include <psizam/detail/pzam_loader.hpp>
#include <psizam/runtime.hpp>

namespace psizam::detail {

// The container is parsed once by load_cached. Each instance owns a relocated
// native mapping and independent module state; no compiler is linked or called.
struct cached_runtime_options : default_options {
   const pzam_file* file = nullptr;
   uint32_t max_call_depth = 0;
   uint32_t max_pages = 0;
   fp_mode fp = fp_mode::fast;
};

template<typename Options, typename DebugInfo>
struct cached_runtime_parser {
   const pzam_file& file;
   cached_runtime_parser(growable_allocator&, const Options& options, bool, bool)
      : file(*options.file) {}
   module& parse_module(wasm_code&, module& mod, DebugInfo&) {
      auto loaded = load_pzam(file);
      // Transfer ownership before any further operation can throw.
      std::shared_ptr<void> owner(loaded.exec_code, [](void* p) {
         jit_allocator::instance().free(p);
      });
      mod.allocator.release_base_memory();
      mod = std::move(loaded.mod);
      loaded.mod.allocator = growable_allocator{};
      mod.jit_engine = std::move(owner);
      return mod;
   }
};

template<bool Reverse>
struct cached_runtime_backend {
   using context = jit_execution_context<false>;
   template<typename Options, typename DebugInfo>
   using parser = cached_runtime_parser<Options, DebugInfo>;
   static constexpr bool is_jit = true, enable_backtrace = false, reverse_host_args = Reverse;
};

inline std::unique_ptr<instance_be> make_cached_instance_be(
   const pzam_file& file, const instance_policy& policy,
   host_function_table table, void* host, char* external, uint32_t external_bytes)
{
   const auto& section = pick_code_section(file);
   const auto tier = static_cast<pzam_opt_tier>(section.opt_tier);
   const auto kind = tier == pzam_opt_tier::jit1 ? backend_kind::jit :
                     tier == pzam_opt_tier::jit2 ? backend_kind::jit2 : backend_kind::jit_llvm;
   const bool metered = policy.metering != instance_policy::meter_mode::none;
   if (metered != (section.instrumentation.gas_metering != 0))
      throw std::invalid_argument("runtime: PZAM gas instrumentation does not match instance policy");
   if (external)
      throw std::invalid_argument("runtime: cached PZAM does not support external memory");
   cached_runtime_options options;
   options.gas_metering = metered;
   options.defer_start = true;
   options.file = &file;
   options.max_call_depth = section.max_stack;
   options.max_pages = policy.max_pages;
   options.fp = section.instrumentation.softfloat ? fp_mode::softfloat : fp_mode::fast;
   const wasm_code unused;
   auto make = [&]<bool Reverse>() -> std::unique_ptr<instance_be> {
      return std::make_unique<instance_be_impl<cached_runtime_backend<Reverse>, cached_runtime_options>>(
         kind, unused, std::move(table), host, options);
   };
   return derive_reverse_host_args(section.opt_tier) ? make.template operator()<true>() :
                                                      make.template operator()<false>();
}
} // namespace psizam::detail
