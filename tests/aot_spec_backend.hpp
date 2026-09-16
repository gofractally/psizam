#pragma once

#include <psizam/backend.hpp>

// Test-only backend: compile with the real CLI, deserialize its PZAM output,
// and execute the saved LLVM code. No in-process JIT fallback is permitted.
namespace psizam::test {
void load_aot_o2_roundtrip(wasm_code& code, module& mod);

template <typename Options, typename DebugInfo>
struct aot_spec_parser {
   aot_spec_parser(growable_allocator&, const Options&, bool, bool) {}
   module& parse_module(wasm_code& code, module& mod, DebugInfo&) {
      static_assert(std::is_same_v<Options, default_options>);
      load_aot_o2_roundtrip(code, mod);
      return mod;
   }
};

struct llvm_aot_o2_pzam {
   using context = detail::jit_execution_context<false>;
   template <typename Options, typename DebugInfo>
   using parser = aot_spec_parser<Options, DebugInfo>;
   static constexpr bool is_jit = true;
   static constexpr bool enable_backtrace = false;
   static constexpr bool reverse_host_args = false;
};
}
