#pragma once

#include <psizam/backend.hpp>

// Test-only backend: compile with the real CLI, deserialize its PZAM output,
// and execute the saved LLVM code. No in-process JIT fallback is permitted.
namespace psizam::test {
void load_aot_roundtrip(wasm_code& code, module& mod, bool jit1);

template <typename Options, typename DebugInfo, bool Jit1>
struct aot_spec_parser {
   aot_spec_parser(growable_allocator&, const Options&, bool, bool) {}
   module& parse_module(wasm_code& code, module& mod, DebugInfo&) {
      static_assert(std::is_same_v<Options, default_options>);
      load_aot_roundtrip(code, mod, Jit1);
      return mod;
   }
};

template<bool Jit1> struct saved_spec_pzam {
   using context = detail::jit_execution_context<false>;
   template <typename Options, typename DebugInfo>
   using parser = aot_spec_parser<Options, DebugInfo, Jit1>;
   static constexpr bool is_jit = true;
   static constexpr bool enable_backtrace = false;
   static constexpr bool reverse_host_args = Jit1;
};
using llvm_aot_o2_pzam = saved_spec_pzam<false>;
using jit1_pzam = saved_spec_pzam<true>;
}
