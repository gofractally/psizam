#pragma once

#include <cstdint>

namespace psizam {
   // Private generated-code call conventions. Public C entry points and
   // registered host function signatures are independent of this identity.
   // Saved native code must be loaded only under its declared convention.
   enum class native_guest_abi : uint32_t {
      legacy_caller_cleanup = 0,
      jit_callee_cleanup = 1,
      llvm_tailcc = 2,
   };

   // Generated native64 code uses this ABI even when its compiler is wasm32.
   // Never derive these target offsets from the compiler host's pointer size.
   namespace native_guest_context_abi {
      inline constexpr uint32_t remaining_stack_offset = 16;
      inline constexpr uint32_t function_costs_offset = 176;
      inline constexpr uint32_t table_entry_index_offset = 4;
      inline constexpr uint32_t table_entry_target_offset = 8;
   }
}
