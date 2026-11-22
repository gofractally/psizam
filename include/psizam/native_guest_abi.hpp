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
}
