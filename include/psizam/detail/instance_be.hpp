#pragma once

// psizam/detail/instance_be.hpp — backend-kind dispatch for instance_impl.
//
// `runtime::instantiate` constructs one of four concrete `backend<Host, Impl>`
// types depending on `instance_policy.initial`. The rest of the runtime needs
// to talk to a uniform interface — `instance_be` — that erases the Impl type.
// Per-backend `instance_be_impl<Impl>` (defined in runtime.cpp) holds the
// typed backend and exposes the polymorphic accessors.
//
// Step 1 of psizam-runtime-api-maturation. Step 10 will extend this interface
// with `call_with_return_erased(...)` so `instance::as<Tag>()` can drive any
// backend kind via the proxy adapter.

#include <psizam/gas.hpp>   // gas_state, gas_handler_t

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace psizam {

class wasm_allocator;
class host_function_table;
struct host_function;
union native_value;

namespace detail {

class operand_stack_elem;   // detail/stack_elem.hpp

enum class backend_kind : uint8_t {
   interpreter,
   jit,
   jit2,
   jit_llvm,
};

constexpr const char* to_string(backend_kind k) noexcept {
   switch (k) {
      case backend_kind::interpreter: return "interpreter";
      case backend_kind::jit:         return "jit";
      case backend_kind::jit2:        return "jit2";
      case backend_kind::jit_llvm:    return "jit_llvm";
   }
   return "unknown";
}

// Type-erased instance backend. Concrete implementation
// `instance_be_impl<Impl>` lives in runtime.cpp where the per-backend
// template instantiations are realized exactly once.
class instance_be {
   // A bound import may outlive its provider. Weak tokens detect that without
   // allocating or taking a shared ownership reference on the call path.
   std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
public:
   std::weak_ptr<void> lifetime() const noexcept { return lifetime_; }
   using canonical_call = std::function<native_value(void*, native_value*, char*)>;
   virtual canonical_call bind_canonical_export(std::string_view name, void* host) = 0;
   virtual ~instance_be() = default;

   // Which backend variant this is. Lets `instance::as<Tag>()` and
   // future call-erasure code dispatch correctly.
   virtual backend_kind kind() const noexcept = 0;

   // Internal opaque backend pointer. Saved-code implementations have distinct
   // types; backend kind alone is not sufficient to cast this pointer.
   virtual void* raw_backend() noexcept = 0;

   // Linear memory + page count, type-erased. Always valid post-construction.
   virtual char*    linear_memory()       noexcept = 0;
   virtual uint32_t current_pages() const noexcept = 0;

   virtual void initialize_start(void* host_ptr) = 0;

   // Resolve and call `_start`. Module initialization has already run. The host
   // pointer flows through to host-function trampolines. Returns the WASI
   // exit code captured from `wasi_host::wasi_exit_exception`; returns 0 if
   // `_start` returns normally without proc_exit. Other psizam / std
   // exceptions propagate to the caller.
   //
   // Step 5 of psizam-runtime-api-maturation. Lets pzam-run's main shrink
   // to `return inst.run_start(&host);`.
   virtual int run_start(void* host_ptr) = 0;

   // Call an exported function by name with up to 16 u64 arguments.
   // Matches the `backend::call_with_return(host, name, u64×16)` shape
   // that `invoke_canonical_export_void` already uses — the canonical-ABI
   // 16-slot flat-call convention.
   //
   // Step 10 of psizam-runtime-api-maturation: this is the call-erasure
   // path that lets `instance::as<Tag>()` work for any backend variant.
   // Args beyond `count` are zero-padded to 16 by the implementation.
   virtual std::optional<operand_stack_elem>
      call_export_canonical(void*              host,
                            std::string_view   name,
                            const uint64_t*    args,
                            std::size_t        count) = 0;

   // Resolve an exported function name to its WASM function index. Used
   // by bridge_executor_erased to cache indices for cabi_realloc and
   // the target export, avoiding per-call re-resolution on the hot
   // cross-module path.
   virtual uint32_t resolve_export(std::string_view name) = 0;
   virtual uint32_t resolve_export_checked(std::string_view name,
                                          const host_function& signature) = 0;

   // Canonical 16-slot flat call by resolved index. Counterpart to
   // `call_export_canonical` that skips the export-name lookup; used
   // by bridge_executor_erased to invoke the bound export without a
   // string search per call.
   virtual std::optional<operand_stack_elem>
      call_export_by_index(void*              host,
                           uint32_t           index,
                           const uint64_t*    args,
                           std::size_t        count) = 0;

   // Call `cabi_realloc(old_ptr, old_size, align, size) -> ptr` with
   // its declared (i32, i32, i32, i32) → i32 signature. Carved out
   // from the generic index call because the interpreter type-checks
   // template arg types against the WASM function's declared params
   // — passing uint64_t here would mismatch the i32 declaration.
   // Returns 0 on failure (caller treats as allocation failure).
   virtual uint32_t call_cabi_realloc(void*    host,
                                      uint32_t index,
                                      uint32_t old_ptr,
                                      uint32_t old_size,
                                      uint32_t align,
                                      uint32_t size) = 0;

   // ── Gas plumbing (backend-erased accessors on execution_context) ──
   // Multi-module gas tracking sets these at instantiate time (and reads
   // them at teardown to credit unused lease back to a shared pool).

   virtual void set_gas_budget(uint64_t budget) = 0;
   virtual gas_state& gas() noexcept = 0;
   virtual void set_gas_handler(void (*handler)(gas_state*, void*),
                                void* user_data) = 0;
   virtual uint64_t gas_consumed_raw() const = 0;
   virtual uint64_t gas_deadline_raw() const = 0;
};

// Factory: pick the impl for `kind`, construct the concrete backend with
// (wasm, table, host_ptr) over a freshly-owned wasm_allocator. Throws
// `unsupported_backend` if `kind` is gated out of this build (e.g. jit_llvm
// without PSIZAM_ENABLE_LLVM_BACKEND, jit / jit2 on platforms without
// __x86_64__ / __aarch64__).
//
// If external_base is non-null, the instance uses caller-provided memory
// at (external_base, 1 << external_budget) instead of mmap'ing its own
// guarded region. Memory mode is forced to checked.
std::unique_ptr<instance_be> make_instance_be(
   backend_kind kind,
   const std::vector<uint8_t>& wasm,
   host_function_table table,
   void* host_ptr,
   char* external_base = nullptr,
   uint32_t external_budget_bytes = 0,
   bool gas_metering = true);

// The optional LLVM library supplies this factory. Selecting it at the call
// site gives static linkers a strong reference and keeps psizam-exec usable
// without installing or linking LLVM.
using instance_backend_factory = decltype(&make_instance_be);
std::unique_ptr<instance_be> make_llvm_instance_be(
   backend_kind kind, const std::vector<uint8_t>& wasm,
   host_function_table table, void* host_ptr,
   char* external_base, uint32_t external_budget_bytes, bool gas_metering);

} // namespace detail
} // namespace psizam
