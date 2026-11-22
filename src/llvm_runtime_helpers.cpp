// Runtime helper functions for the LLVM JIT backend.
// These are called from LLVM-generated native code via absolute symbol resolution.
//
// Exception safety: All helpers that can throw use longjmp via trap_jmp_ptr
// (set up by invoke_with_signal_handler / setjmp) to bypass JIT frames.
// This matches the mechanism used by jit/jit2 backends and signal handlers.

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

   template <typename F>
   auto invoke_or_escape(F&& f) -> std::invoke_result_t<F> {
      using result_type = std::invoke_result_t<F>;
      if (!trap_jmp_ptr) return f();
      if constexpr (std::is_void_v<result_type>) {
         longjmp_on_exception([&] { f(); });
      } else {
         static_assert(std::is_trivially_destructible_v<result_type>);
         result_type result{};
         longjmp_on_exception([&] { result = f(); });
         return result;
      }
   }

   template <typename F>
   auto preserving_stack_budget(ctx_t& c, F&& f) {
      const auto remaining = c.get_remaining_call_depth();
      auto restore = scope_guard([&] { c.set_max_call_depth(remaining); });
      return f();
   }

   int64_t dispatch_host(ctx_t& c, uint32_t func_idx, native_value* args) {
      if (c._host_trampoline_ptrs)
         if (auto trampoline = c._host_trampoline_ptrs[func_idx])
            return trampoline(c.get_host_ptr(), args, c.linear_memory()).i64;
      const auto mapped = c.get_module().import_functions[func_idx];
      return c.get_host_table()->call(c.get_host_ptr(), mapped, args, c.linear_memory()).i64;
   }

   // Escape via longjmp with a new exception, or throw if no escape target.
   template<typename E>
   [[noreturn]] void escape_or_throw(const char* msg) {
      if (trap_jmp_ptr) {
         if (trap_observer)
            trap_observer(msg, trap_observer_data);
         saved_exception = std::make_exception_ptr(E{msg});
         siglongjmp(*trap_jmp_ptr, -1);
      }
      throw E{msg};
   }
}

extern "C" {

int64_t __psizam_global_get(void* ctx, uint32_t idx) {
   return as_ctx(ctx).get_global(idx).value.i64;
}

void __psizam_global_set(void* ctx, uint32_t idx, int64_t value) {
   as_ctx(ctx).get_global(idx).value.i64 = value;
}

void __psizam_global_get_v128(void* ctx, uint32_t idx, void* out) {
   auto& v = as_ctx(ctx).get_global(idx).value.v128;
   std::memcpy(out, &v, 16);
}

void __psizam_global_set_v128(void* ctx, uint32_t idx, const void* in) {
   auto& v = as_ctx(ctx).get_global(idx).value.v128;
   std::memcpy(&v, in, 16);
}

void* __psizam_get_memory(void* ctx) {
   return as_ctx(ctx).linear_memory();
}

int32_t __psizam_memory_size(void* ctx) {
   return as_ctx(ctx).current_linear_memory();
}

int32_t __psizam_memory_grow(void* ctx, int32_t pages, void** new_mem_out) {
   auto& c = as_ctx(ctx);
   int32_t result = c.grow_linear_memory(pages);
   // Return the (possibly new) memory base pointer so the caller can update %mem
   if (new_mem_out)
      *new_mem_out = c.linear_memory();
   return result;
}

int64_t __psizam_call_host(void* ctx, uint32_t func_idx,
                            void* args_buf, uint32_t nargs) {
   auto& c = as_ctx(ctx);
   auto* args = static_cast<native_value*>(args_buf);
   return invoke_or_escape([&] {
      return preserving_stack_budget(c, [&] { return dispatch_host(c, func_idx, args); });
   });
}

// The ORC path has registered unwind information. Its C++ cleanup still owns
// the suspended caller's live budget when a host callback re-enters this context.
int64_t __psizam_call_host_nothrow(void* ctx, uint32_t func_idx,
                                    void* args_buf, uint32_t nargs) {
   auto& c = as_ctx(ctx);
   return preserving_stack_budget(c, [&] {
      return dispatch_host(c, func_idx, static_cast<native_value*>(args_buf));
   });
}

// Combined host call: one exception-safe native frame owns count tracking,
// dispatch and memory refresh. Byte mode already charges the Wasm body.
int64_t __psizam_call_host_full(void* ctx, uint32_t func_idx,
                                 void* args_buf, uint32_t nargs,
                                 void** mem_out) {
   auto& c = as_ctx(ctx);
   return invoke_or_escape([&] {
      return preserving_stack_budget(c, [&] {
         if (!c.get_module().stack_limit_is_bytes) {
            const auto depth = c.get_remaining_call_depth();
            if (depth <= 1)
               throw wasm_interpreter_exception("stack overflow");
            c.set_max_call_depth(depth - 1);
         }
         const auto result = dispatch_host(c, func_idx, static_cast<native_value*>(args_buf));
         *mem_out = c.linear_memory();
         return result;
      });
   });
}

void __psizam_memory_init(void* ctx, uint32_t seg_idx,
                           uint32_t dest, uint32_t src, uint32_t n) {
   invoke_or_escape([&] {
      as_ctx(ctx).init_linear_memory(seg_idx, dest, src, n);
   });
}

void __psizam_data_drop(void* ctx, uint32_t seg_idx) {
   invoke_or_escape([&] {
      as_ctx(ctx).drop_data(seg_idx);
   });
}

void __psizam_memory_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n) {
   auto& c = as_ctx(ctx);
   char* mem = c.linear_memory();
   uint32_t mem_size = static_cast<uint32_t>(c.current_linear_memory()) * 65536u;
   // Spec: trap if dest + n > |mem| or src + n > |mem| (even when n=0)
   if (uint64_t(dest) + n > mem_size || uint64_t(src) + n > mem_size)
      escape_or_throw<wasm_memory_exception>("out of bounds memory access");
   if (n > 0) {
      std::memmove(mem + dest, mem + src, n);
   }
}

void __psizam_memory_fill(void* ctx, uint32_t dest, uint32_t val, uint32_t n) {
   auto& c = as_ctx(ctx);
   char* mem = c.linear_memory();
   uint32_t mem_size = static_cast<uint32_t>(c.current_linear_memory()) * 65536u;
   // Spec: trap if dest + n > |mem| (even when n=0)
   if (uint64_t(dest) + n > mem_size)
      escape_or_throw<wasm_memory_exception>("out of bounds memory access");
   if (n > 0) {
      std::memset(mem + dest, static_cast<uint8_t>(val), n);
   }
}

void __psizam_elem_drop(void* ctx, uint32_t seg_idx) {
   invoke_or_escape([&] {
      as_ctx(ctx).drop_elem(seg_idx);
   });
}

int64_t __psizam_call_indirect(void* ctx, void* mem, uint32_t type_idx,
                                uint32_t table_elem, void* args_buf, uint32_t nargs) {
   // type_idx packs: type index in lower 16 bits, table index in upper 16 bits
   uint32_t table_idx = type_idx >> 16;
   type_idx &= 0xFFFF;
   auto& c = as_ctx(ctx);
   auto& mod = c.get_module();

   if (table_idx >= mod.tables.size())
      escape_or_throw<wasm_interpreter_exception>("no table");
   uint32_t table_size = c.get_table_size(table_idx);

   if (table_elem >= table_size)
      escape_or_throw<wasm_interpreter_exception>("undefined element");

   // Get table entry
   auto* table_base = c.get_table_base(table_idx);
   auto& entry = table_base[table_elem];

   // Null/uninitialized check (0xFFFFFFFF is the sentinel)
   if (entry.type == UINT32_MAX || entry.index == UINT32_MAX)
      escape_or_throw<wasm_interpreter_exception>("undefined element");

   // Type check. The lookup can throw; generated frames cannot be unwound.
   const bool type_matches = invoke_or_escape([&] {
      return mod.get_function_type(entry.index) == mod.types.at(type_idx);
   });
   if (!type_matches)
      escape_or_throw<wasm_interpreter_exception>("indirect call type mismatch");

   // Call the function
   uint32_t num_imports = mod.get_imported_functions_size();
   if (entry.index < num_imports) {
      // Host function call — __psizam_call_host already handles escape
      return __psizam_call_host(ctx, entry.index, args_buf, nargs);
   } else {
      // WASM function — call through the entry wrapper
      // Wrap in try/catch to escape if the callee throws via a runtime helper
      uint32_t code_idx = entry.index - num_imports;
      auto offset = mod.code[code_idx].jit_code_offset;
      using llvm_entry_fn_t = int64_t(*)(void*, void*, native_value*);
      auto fn = reinterpret_cast<llvm_entry_fn_t>(offset);
      return invoke_or_escape([&] {
         return fn(ctx, mem, static_cast<native_value*>(args_buf));
      });
   }
}

// Nothrow variant of call_indirect for LLVM JIT path.
int64_t __psizam_call_indirect_nothrow(void* ctx, void* mem, uint32_t type_idx,
                                        uint32_t table_elem, void* args_buf, uint32_t nargs) {
   uint32_t table_idx = type_idx >> 16;
   type_idx &= 0xFFFF;
   auto& c = as_ctx(ctx);
   auto& mod = c.get_module();

   if (table_idx >= mod.tables.size())
      throw wasm_interpreter_exception{"no table"};
   uint32_t table_size = c.get_table_size(table_idx);

   if (table_elem >= table_size)
      throw wasm_interpreter_exception{"undefined element"};

   auto* table_base = c.get_table_base(table_idx);
   auto& entry = table_base[table_elem];

   if (entry.type == UINT32_MAX || entry.index == UINT32_MAX)
      throw wasm_interpreter_exception{"undefined element"};

   const auto& expected_type = mod.types[type_idx];
   const auto& actual_type = mod.get_function_type(entry.index);
   if (!(actual_type == expected_type))
      throw wasm_interpreter_exception{"indirect call type mismatch"};

   uint32_t num_imports = mod.get_imported_functions_size();
   if (entry.index < num_imports) {
      return __psizam_call_host_nothrow(ctx, entry.index, args_buf, nargs);
   } else {
      uint32_t code_idx = entry.index - num_imports;
      auto offset = mod.code[code_idx].jit_code_offset;
      using llvm_entry_fn_t = int64_t(*)(void*, void*, native_value*);
      auto fn = reinterpret_cast<llvm_entry_fn_t>(offset);
      return fn(ctx, mem, static_cast<native_value*>(args_buf));
   }
}

void __psizam_call_depth_dec(void* ctx) {
   auto& c = as_ctx(ctx);
   if (c.get_module().stack_limit_is_bytes) return;
   uint32_t depth = c.get_remaining_call_depth();
   if (depth == 0) {
      escape_or_throw<wasm_interpreter_exception>("stack overflow");
   }
   uint32_t new_depth = depth - 1;
   c.set_max_call_depth(new_depth);
   if (new_depth == 0) {
      escape_or_throw<wasm_interpreter_exception>("stack overflow");
   }
}

void __psizam_call_depth_inc(void* ctx) {
   auto& c = as_ctx(ctx);
   if (c.get_module().stack_limit_is_bytes) return;
   c.set_max_call_depth(c.get_remaining_call_depth() + 1);
}

void __psizam_trap(void* ctx, uint32_t trap_code) {
   const char* msg;
   switch (trap_code) {
      case 0:  msg = "unreachable"; break;
      case 1:  msg = "integer divide by zero"; break;
      case 2:  msg = "integer overflow"; break;
      case 3:  msg = "integer overflow"; break;
      case 4:  msg = "undefined element"; break;
      case 5:  // memory OOB — use wasm_memory_exception
         escape_or_throw<wasm_memory_exception>("out of bounds memory access");
      case 6:  msg = "stack overflow"; break;
      default: msg = "unknown trap"; break;
   }
   escape_or_throw<wasm_interpreter_exception>(msg);
}

} // extern "C"
