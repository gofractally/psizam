// Runtime helper functions for JIT backends.
// These are called from JIT-generated native code (both custom JIT and LLVM backends).
// They do NOT depend on LLVM — they only use psizam core types.
//
// See llvm_runtime_helpers.cpp for the exception escape rationale.

#include <psizam/detail/llvm_runtime_helpers.hpp>
#include <psizam/detail/execution_context.hpp>
#include <psizam/types.hpp>

#include <cstring>

namespace {
   using namespace psizam;
   using namespace psizam::detail;
   using ctx_t = jit_execution_context<false>;

   ctx_t& as_ctx(void* ctx) {
      return *static_cast<ctx_t*>(ctx);
   }

   template<typename E>
   [[noreturn]] void escape_or_throw(const char* msg) {
      if (trap_jmp_ptr) {
         saved_exception = std::make_exception_ptr(E{msg});
         siglongjmp(*trap_jmp_ptr, -1);
      }
      throw E{msg};
   }

   template <typename F>
   void invoke_or_escape(F&& f) {
      if (trap_jmp_ptr) longjmp_on_exception([&] { f(); });
      else f();
   }

   struct indirect_target_result {
      const table_entry* entry;
      const char* error;
   };

   // Shared authority for both the legacy null-return API and the typed LLVM
   // entry API. The latter preserves its existing detailed trap diagnostics.
   indirect_target_result resolve_indirect_entry(ctx_t& c, uint32_t type_idx,
                                                  uint32_t table_idx, uint32_t elem_idx) {
      auto& mod = c.get_module();
      if (table_idx >= mod.tables.size() || elem_idx >= c.get_table_size(table_idx))
         return {nullptr, "undefined element"};
      const auto* entry = c.get_table_base(table_idx) + elem_idx;
      if (entry->type == UINT32_MAX || entry->index >= mod.get_functions_total())
         return {nullptr, "undefined element"};
      if (type_idx >= mod.types.size() || !(mod.get_function_type(entry->index) == mod.types[type_idx]))
         return {nullptr, "indirect call type mismatch"};
      if (!entry->code_ptr) return {nullptr, "undefined element"};
      return {entry, nullptr};
   }

   void invoke_gas_handler(gas_state& gas) {
      // Custom handlers may throw (timeouts, exhausted pools, scheduler
      // failures). Never unwind through generated code. The helper leaves
      // the catch scope before escaping so the exception is not leaked.
      if (trap_jmp_ptr)
         longjmp_on_exception([&] { gas.handler(&gas, gas.user_data); });
      else
         gas.handler(&gas, gas.user_data);
   }
}

extern "C" {

const void* __psizam_resolve_indirect(void* ctx, uint32_t type_idx,
                                       uint32_t table_idx, uint32_t elem_idx) {
   auto result = resolve_indirect_entry(as_ctx(ctx), type_idx, table_idx, elem_idx);
   return result.entry ? result.entry->code_ptr : nullptr;
}

const psizam::table_entry* __psizam_resolve_indirect_entry(void* ctx, uint32_t type_idx,
                                                           uint32_t table_idx, uint32_t elem_idx) {
   auto result = resolve_indirect_entry(as_ctx(ctx), type_idx, table_idx, elem_idx);
   if (result.error) escape_or_throw<wasm_interpreter_exception>(result.error);
   return result.entry;
}

void __psizam_table_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n,
                          uint32_t dst_table, uint32_t src_table) {
   invoke_or_escape([&] {
      auto& c = as_ctx(ctx);
      auto* s = c.get_table_ptr(src, n, src_table);
      auto* d = c.get_table_ptr(dest, n, dst_table);
      if (n > 0)
         std::memmove(d, s, n * sizeof(table_entry));
   });
}

void __psizam_table_init(void* ctx, uint32_t elem_idx,
                          uint32_t dest, uint32_t src, uint32_t n,
                          uint32_t table_idx) {
   invoke_or_escape([&] {
      as_ctx(ctx).init_table(elem_idx, dest, src, n, table_idx);
   });
}

uint32_t __psizam_table_get(void* ctx, uint32_t table_idx, uint32_t elem_idx) {
   auto& c = as_ctx(ctx);
   if (elem_idx >= c.get_table_size(table_idx))
      escape_or_throw<wasm_interpreter_exception>("table index out of range");
   return c.get_table_base(table_idx)[elem_idx].index;
}

void __psizam_table_set(void* ctx, uint32_t table_idx, uint32_t elem_idx, uint32_t val) {
   auto& c = as_ctx(ctx);
   if (elem_idx >= c.get_table_size(table_idx))
      escape_or_throw<wasm_interpreter_exception>("table index out of range");
   c.get_table_base(table_idx)[elem_idx] = c.make_table_entry(table_idx, val);
}

uint32_t __psizam_table_grow(void* ctx, uint32_t table_idx, uint32_t delta, uint32_t init_val) {
   auto& c = as_ctx(ctx);
   auto te = c.make_table_entry(table_idx, init_val);
   return c.table_grow(table_idx, delta, te);
}

uint32_t __psizam_table_size(void* ctx, uint32_t table_idx) {
   return as_ctx(ctx).get_table_size(table_idx);
}

void __psizam_table_fill(void* ctx, uint32_t table_idx, uint32_t i, uint32_t val, uint32_t n) {
   invoke_or_escape([&] {
      auto& c = as_ctx(ctx);
      auto te = c.make_table_entry(table_idx, val);
      c.table_fill(table_idx, i, te, n);
   });
}

// Private-memory atomic semantics. All engine tiers delegate here so alignment,
// i33 addressing, narrow truncation and modular results are identical. This is
// deliberately not a shared-memory synchronization implementation.
uint64_t __psizam_atomic_rmw(void* ctx, uint8_t sub, uint64_t addr, uint32_t offset,
                              uint64_t val1, uint64_t val2) {
   const auto op = static_cast<atomic_sub>(sub);
   if (op == atomic_sub::atomic_fence) return 0;
   auto& c = as_ctx(ctx);
   if (op == atomic_sub::memory_atomic_wait32 || op == atomic_sub::memory_atomic_wait64)
      escape_or_throw<wasm_interpreter_exception>("atomic wait requires shared memory");
   const auto width = atomic_access_size(op);
   if (!width) escape_or_throw<wasm_interpreter_exception>("invalid private atomic operation");
   const auto& memories = c.get_module().memories;
   if (memories.empty()) escape_or_throw<wasm_memory_exception>("atomic instruction requires memory");
   if (!memories[0].is_memory64) addr = static_cast<uint32_t>(addr);
   const bool memory16 = c.mem_mode() == mem_safety::memory16;
   if (memory16) addr &= memory16_address_mask;
   const uint64_t size = memory16 ? memory16_accessible_bytes :
      uint64_t(c.current_linear_memory()) * 65536;
   if (addr > size || offset > size - addr)
      escape_or_throw<wasm_memory_exception>("out of bounds memory access");
   const auto effective = addr + offset;
   if (width > size - effective)
      escape_or_throw<wasm_memory_exception>("out of bounds memory access");
   if (!memory16 && (effective & (width - 1)))
      escape_or_throw<wasm_memory_exception>("unaligned atomic access");
   if (op == atomic_sub::memory_atomic_notify) return 0; // no private-memory waiters
   char* ptr = c.linear_memory() + effective;
   uint64_t old = 0;
   std::memcpy(&old, ptr, width);
   if (sub >= 0x10 && sub <= 0x16) return old;
   const uint64_t mask = width == 8 ? UINT64_MAX : (uint64_t{1} << (width * 8)) - 1;
   val1 &= mask;
   val2 &= mask;
   uint64_t value = val1;
   if (sub >= 0x1e && sub <= 0x4e) {
      switch ((sub - 0x1e) / 7) {
      case 0: value = old + val1; break;
      case 1: value = old - val1; break;
      case 2: value = old & val1; break;
      case 3: value = old | val1; break;
      case 4: value = old ^ val1; break;
      case 5: value = val1; break;
      case 6: value = old == val1 ? val2 : old; break;
      }
   } else if (!(sub >= 0x17 && sub <= 0x1d)) {
      escape_or_throw<wasm_interpreter_exception>("invalid private atomic operation");
   }
   value &= mask;
   std::memcpy(ptr, &value, width);
   return sub >= 0x17 && sub <= 0x1d ? 0 : old;
}

void __psizam_gas_charge(void* ctx, int64_t cost) {
   // Callable from JIT-generated code across every backend. Do the
   // consumed bump + exhaustion dispatch directly (rather than
   // forwarding to the inline method) so the throw path uses
   // escape_or_throw, which hands off to the JIT's setjmp trampoline.
   // Forwarding through the inline gas_charge method uses a plain C++
   // throw, which fails to unwind through jit2 / jit_llvm frames.
   auto& c = as_ctx(ctx);
   auto& gs = c.gas();
   gs.consumed += static_cast<uint64_t>(cost);
   if (gs.consumed >= gs.deadline.load(std::memory_order_relaxed)) {
      if (gs.handler) invoke_gas_handler(gs);
      else escape_or_throw<wasm_gas_exhausted_exception>("gas exhausted");
   }
}

void __psizam_gas_exhausted_check(void* ctx) {
   auto& gs = as_ctx(ctx).gas();
   // JIT already bumped consumed inline; re-read deadline (the handler
   // may have advanced it, or an external thread may have raced us) and
   // dispatch only if we're still over the line.
   if (gs.consumed < gs.deadline.load(std::memory_order_relaxed)) return;
   if (gs.handler) invoke_gas_handler(gs);
   else escape_or_throw<wasm_gas_exhausted_exception>("gas exhausted");
}

} // extern "C"
