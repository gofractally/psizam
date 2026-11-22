#pragma once

// psizam/wasmtime_admission.hpp — psizam's validator in front of wasmtime.
//
// The wasmtime backend executes; psizam decides what is allowed to execute.
// Running psizam's parser over a module with the embedder's options applies
// every psizam limit and proposal switch, and yields the per-function stack
// byte costs that wasmtime_backend.hpp's compile_metered needs. A module that
// fails here is rejected exactly as it would be by psizam's own engines.

#include <psizam/backend.hpp>

#include <cstdint>
#include <vector>

namespace psizam::wasmtime {

// Validates `code` under `options` and returns the stack byte cost of each
// defined function, in order. Options must declare max_stack_bytes, which is
// what selects psizam's byte accounting. Throws psizam's parse exception if
// the module is not admissible.
template <typename Options>
std::vector<uint32_t> validate_and_cost(psizam::wasm_code& code, const Options& options)
{
   static_assert(requires { options.max_stack_bytes; },
                 "stack costs need an Options type with max_stack_bytes");
   psizam::backend<std::nullptr_t, psizam::null_backend, Options> validator(code, nullptr, options);
   const auto&           mod = validator.get_module();
   std::vector<uint32_t> costs;
   costs.reserve(mod.code.size());
   for (const auto& body : mod.code)
      costs.push_back(body.byte_stack_usage);
   return costs;
}

}  // namespace psizam::wasmtime
