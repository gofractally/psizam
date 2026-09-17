#pragma once

// Shared implementation for the baseline and optional LLVM runtime factories.
#include <psizam/backend.hpp>
#include <psizam/detail/instance_be.hpp>
#include <psizam/detail/wasi_host.hpp>

namespace psizam::detail {

template <typename Impl, typename Options = default_options>
class instance_be_impl final : public instance_be {
   using backend_t = backend<std::nullptr_t, Impl, Options>;

   wasm_allocator             alloc_;
   std::unique_ptr<backend_t> be_;
   backend_kind               kind_;

public:
   instance_be_impl(backend_kind kind, const std::vector<uint8_t>& wasm,
                    host_function_table table, void* host_ptr, const Options& options)
      : kind_(kind)
   {
      auto& code = const_cast<std::vector<uint8_t>&>(wasm);
      be_ = std::make_unique<backend_t>(code, std::move(table), host_ptr, &alloc_, options);
   }

   // Self-managed mode: allocator does its own mmap with guard pages.
   instance_be_impl(backend_kind kind,
                    const std::vector<uint8_t>& wasm,
                    host_function_table table,
                    void* host_ptr, bool gas_metering = true)
      : kind_(kind)
   {
      auto& code = const_cast<std::vector<uint8_t>&>(wasm);
      be_ = std::make_unique<backend_t>(
         code, std::move(table), host_ptr, &alloc_,
         default_options{.gas_metering = gas_metering, .defer_start = true});
   }

   // Caller-provided mode: allocator uses pre-allocated buffer.
   instance_be_impl(backend_kind kind,
                    const std::vector<uint8_t>& wasm,
                    host_function_table table,
                    void* host_ptr,
                    char* external_base,
                    uint32_t external_bytes, bool gas_metering = true)
      : alloc_(external_base, external_bytes)
      , kind_(kind)
   {
      auto& code = const_cast<std::vector<uint8_t>&>(wasm);
      be_ = std::make_unique<backend_t>(
         code, std::move(table), host_ptr, &alloc_,
         default_options{.gas_metering = gas_metering, .defer_start = true});
   }

   // Pooled mode: allocator uses a pre-mapped guarded region from
   // the runtime's pool. Guard pages already in place, mprotect ok
   // for memory.grow (infrequent). No mmap/munmap.
   instance_be_impl(backend_kind kind,
                    const std::vector<uint8_t>& wasm,
                    host_function_table table,
                    void* host_ptr,
                    wasm_allocator::pooled_tag tag,
                    char* pooled_raw, bool gas_metering = true)
      : alloc_(tag, pooled_raw)
      , kind_(kind)
   {
      auto& code = const_cast<std::vector<uint8_t>&>(wasm);
      be_ = std::make_unique<backend_t>(
         code, std::move(table), host_ptr, &alloc_,
         default_options{.gas_metering = gas_metering, .defer_start = true});
   }

   backend_kind kind()         const noexcept override { return kind_; }
   void*        raw_backend()        noexcept override { return be_.get(); }

   char* linear_memory() noexcept override {
      return be_->get_context().linear_memory();
   }
   uint32_t current_pages() const noexcept override {
      return be_->get_context().current_linear_memory();
   }

   void initialize_start(void* host_ptr) override {
      auto& mod = be_->get_module();
      if (mod.start != std::numeric_limits<uint32_t>::max())
         be_->call_by_index(host_ptr, mod.start);
   }

   int run_start(void* host_ptr) override {
      try {
         be_->call(host_ptr, std::string_view{"_start"});
      } catch (const wasi_host::wasi_exit_exception& e) {
         return e.code;
      }
      return 0;
   }

   std::optional<operand_stack_elem>
      call_export_canonical(void*            host,
                            std::string_view name,
                            const uint64_t*  args,
                            std::size_t      count) override
   {
      // `backend::call_with_return` is templated on arg count and the
      // interpreter type-checks `sizeof...(Args) == WASM param count`.
      // Pass exactly `count` args so cabi_realloc (4 params),
      // canonical-ABI PSIO_MODULE exports (16 flat slots), and
      // WIT-declared narrow signatures each match their declared arity.
      switch (count) {
         case 0:  return be_->call_with_return(host, name);
         case 1:  return be_->call_with_return(host, name, args[0]);
         case 2:  return be_->call_with_return(host, name, args[0], args[1]);
         case 3:  return be_->call_with_return(host, name, args[0], args[1], args[2]);
         case 4:  return be_->call_with_return(host, name, args[0], args[1], args[2], args[3]);
         case 5:  return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4]);
         case 6:  return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5]);
         case 7:  return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
         case 8:  return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
         case 9:  return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
         case 10: return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
         case 11: return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]);
         case 12: return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]);
         case 13: return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12]);
         case 14: return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13]);
         case 15: return be_->call_with_return(host, name, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14]);
         default: /* 16 */
            return be_->call_with_return(
               host, name,
               args[0],  args[1],  args[2],  args[3],
               args[4],  args[5],  args[6],  args[7],
               args[8],  args[9],  args[10], args[11],
               args[12], args[13], args[14], args[15]);
      }
   }

   uint32_t resolve_export(std::string_view name) override {
      return be_->resolve_export(name);
   }

   uint32_t resolve_export_checked(std::string_view name,
                                   const host_function& signature) override {
      const uint32_t index = be_->resolve_export(name);
      PSIZAM_ASSERT(index != std::numeric_limits<uint32_t>::max(), wasm_link_exception,
                    "bound provider export not found");
      PSIZAM_ASSERT(signature == be_->get_module().get_function_type(index), wasm_link_exception,
                    "bound provider export signature mismatch");
      return index;
   }

   canonical_call bind_canonical_export(std::string_view name, void* host) override {
      host_function signature;
      signature.params.assign(16, types::i64);
      signature.ret = {types::i64};
      const auto index = resolve_export_checked(name, signature);
      return [this, index, host, live = lifetime()](void*, native_value* args, char*) {
         PSIZAM_ASSERT(!live.expired(), wasm_link_exception, "bound provider instance expired");
         std::optional<operand_stack_elem> result;
         if constexpr (Impl::is_jit) {
            // Signature and arity are fixed by the sealed binding above. Keep
            // the ordinary protected entry, host/depth restoration and stack
            // leases, but consume the caller's slots without copying them.
            result = be_->get_context().execute_dynamic(host, index, args, 16);
         } else {
            uint64_t slots[16];
            for (int i = 0; i < 16; ++i) slots[i] = args[i].i64;
            result = call_export_by_index(host, index, slots, 16);
         }
         native_value value;
         value.i64 = result ? result->to_ui64() : 0;
         return value;
      };
   }

   uint32_t call_cabi_realloc(void*    host,
                              uint32_t index,
                              uint32_t old_ptr,
                              uint32_t old_size,
                              uint32_t align,
                              uint32_t size) override
   {
      auto r = be_->call_by_index(host, index, old_ptr, old_size, align, size);
      return r ? r->to_ui32() : 0u;
   }

   std::optional<operand_stack_elem>
      call_export_by_index(void*           host,
                           uint32_t        index,
                           const uint64_t* args,
                           std::size_t     count) override
   {
      // `backend::call_by_index` is templated on arg count and the
      // interpreter type-checks `sizeof...(Args) == WASM param count`.
      // We must pass exactly `count` args so cabi_realloc (4 params)
      // and canonical-ABI exports (16 flat slots) each match their
      // declared signature.
      switch (count) {
         case 0:  return be_->call_by_index(host, index);
         case 1:  return be_->call_by_index(host, index, args[0]);
         case 2:  return be_->call_by_index(host, index, args[0], args[1]);
         case 3:  return be_->call_by_index(host, index, args[0], args[1], args[2]);
         case 4:  return be_->call_by_index(host, index, args[0], args[1], args[2], args[3]);
         case 5:  return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4]);
         case 6:  return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5]);
         case 7:  return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
         case 8:  return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
         case 9:  return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
         case 10: return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
         case 11: return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]);
         case 12: return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]);
         case 13: return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12]);
         case 14: return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13]);
         case 15: return be_->call_by_index(host, index, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14]);
         default: /* 16 */
            return be_->call_by_index(
               host, index,
               args[0],  args[1],  args[2],  args[3],
               args[4],  args[5],  args[6],  args[7],
               args[8],  args[9],  args[10], args[11],
               args[12], args[13], args[14], args[15]);
      }
   }

   // ── Gas plumbing ──────────────────────────────────────────────────
   gas_state& gas() noexcept override { return be_->get_context().gas(); }
   void set_gas_budget(uint64_t budget) override {
      be_->get_context().set_gas_budget(gas_units{budget});
   }
   void set_gas_handler(void (*handler)(gas_state*, void*),
                        void* user_data) override {
      be_->get_context().set_gas_handler(handler, user_data);
   }
   uint64_t gas_consumed_raw() const override {
      return *be_->get_context().gas_consumed();
   }
   uint64_t gas_deadline_raw() const override {
      return *be_->get_context().gas_deadline();
   }
};


} // namespace psizam::detail
