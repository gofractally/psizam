// Optional live-LLVM factory; saved LLVM PZAM execution needs only psizam-exec.
#include "runtime_backend.hpp"

namespace psizam::detail {

std::unique_ptr<instance_be> make_llvm_instance_be(
   backend_kind kind, const std::vector<uint8_t>& wasm,
   host_function_table table, void* host_ptr,
   char* external_base, uint32_t external_budget_bytes, bool gas_metering)
{
   if (kind != backend_kind::jit_llvm)
      return make_instance_be(kind, wasm, std::move(table), host_ptr,
                              external_base, external_budget_bytes, gas_metering);
   if (external_base)
      return std::make_unique<instance_be_impl<jit_llvm>>(
         kind, wasm, std::move(table), host_ptr, external_base, external_budget_bytes, gas_metering);
   return std::make_unique<instance_be_impl<jit_llvm>>(
      kind, wasm, std::move(table), host_ptr, gas_metering);
}

} // namespace psizam::detail
