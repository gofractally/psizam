#pragma once

#include <psizam/backend.hpp>
#include <psizam/native_artifact.hpp>
#include <psizam/detail/ir_writer_llvm_aot.hpp>

#include <exception>
#include <pthread.h>
#include <system_error>
#include <type_traits>

namespace psizam::detail {
   // LLVM recurses deeply. The module may first be needed on a small or
   // nearly exhausted stack (a nested call runs on its caller's WASM stack),
   // and whether a node compiles at all depends on its cache. Give every cold
   // compile the same dedicated stack so that it cannot overflow on one node
   // and succeed on another.
   inline constexpr std::size_t aot_compile_stack_size = 8 * 1024 * 1024;

   template<typename F>
   void run_on_aot_compile_stack(F&& compile) {
      struct job {
         std::remove_reference_t<F>* compile;
         std::exception_ptr failure;
      } state{&compile, nullptr};
      pthread_attr_t attributes;
      if (int error = pthread_attr_init(&attributes))
         throw std::system_error(error, std::generic_category(), "AOT compile stack");
      pthread_t thread;
      int error = pthread_attr_setstacksize(&attributes, aot_compile_stack_size);
      if (!error)
         error = pthread_create(&thread, &attributes, [](void* arg) -> void* {
            auto& state = *static_cast<job*>(arg);
            try {
               (*state.compile)();
            } catch (...) {
               state.failure = std::current_exception();
            }
            return nullptr;
         }, &state);
      pthread_attr_destroy(&attributes);
      if (error)
         throw std::system_error(error, std::generic_category(), "AOT compile stack");
      pthread_join(thread, nullptr);
      if (state.failure) std::rethrow_exception(state.failure);
   }

   template<typename Options, typename DebugInfo>
   struct aot_parser {
      const Options& options;
      bool backtrace;
      bool byte_stack;
      aot_parser(growable_allocator&, const Options& options,
                 bool backtrace, bool byte_stack)
         : options(options), backtrace(backtrace), byte_stack(byte_stack) {}

      module& parse_module(wasm_code& wasm, module& mod, DebugInfo& debug) {
         std::array<uint8_t, 32> identity{};
         if constexpr (requires { options.native_aot_identity(wasm, options.max_func_local_bytes,
                            get_max_func_local_bytes_flags(options), backtrace, byte_stack); })
            identity = options.native_aot_identity(wasm, options.max_func_local_bytes,
                           get_max_func_local_bytes_flags(options), backtrace, byte_stack);
         else if constexpr (requires { options.native_aot_identity(wasm); })
            identity = options.native_aot_identity(wasm);
         if constexpr (requires { options.load_native_aot(identity); }) {
            static_assert(requires { options.native_aot_identity(wasm); } ||
                          requires { options.native_aot_identity(wasm, options.max_func_local_bytes,
                              get_max_func_local_bytes_flags(options), backtrace, byte_stack); },
                          "an AOT cache requires a complete embedder identity");
            if (identity == std::array<uint8_t, 32>{})
               throw std::invalid_argument("AOT cache identity must not be empty");
            auto artifact = options.load_native_aot(identity);
            if (!artifact.empty()) {
               try {
                  return install(wasm, mod, debug, artifact, identity);
               } catch (const psizam::exception&) {
                  // An incompatible cache is a miss. Compilation uses the
                  // actual Options again; the artifact is never a wire input.
               } catch (const std::runtime_error&) {
               }
            }
         }
         if constexpr (requires { options.native_aot_cache_only; }) {
            if (options.native_aot_cache_only)
               throw std::runtime_error("required native AOT artifact is missing or incompatible");
         }
         pzam_compile_result result;
#if defined(__x86_64__)
         result.target_triple = "x86_64-unknown-linux-gnu";
#elif defined(__aarch64__)
         result.target_triple = "aarch64-unknown-linux-gnu";
#endif
         if (get_memory_mode(options) != mem_safety::guarded)
            throw std::invalid_argument("saved AOT profile requires guarded memory");
         result.softfloat = get_fp(options) == fp_mode::softfloat;
         result.backtrace = backtrace;
         result.opt_level = 2;
         // The writer emits object code and relocations, not ORC JIT code.
         // Its destructor finishes the result, so end its lifetime before save.
         null_debug_info unused_debug;
         run_on_aot_compile_stack([&] {
            binary_parser<ir_writer_llvm_aot, Options, null_debug_info> parser(
               mod.allocator, options, backtrace, byte_stack);
            parser.set_compile_result(&result);
            parser.parse_module(wasm, mod, unused_debug);
         });
         mod.finalize();
         auto artifact = save_native_aot(wasm, mod, result, identity);
         if constexpr (requires { options.store_native_aot(identity, artifact); })
            options.store_native_aot(identity, artifact);
         return install(wasm, mod, debug, artifact, identity);
      }

      module& install(wasm_code& wasm, module& mod, DebugInfo& debug,
                      std::span<const char> artifact,
                      const std::array<uint8_t, 32>& identity) {
         native_aot_metadata metadata;
         auto loaded = load_native_aot(artifact, &metadata, identity, wasm);
         // LLVM optimization does not preserve the JIT's instruction map.
         // Retain exact function-body ranges with function-level WASM locations.
         typename DebugInfo::builder map;
         const auto base = loaded.allocator._exec_code_base;
         map.on_code_start(base, wasm.data());
         for (const auto& source : metadata.sources) {
            map.on_function_start(base + source.native_offset, wasm.data() + source.wasm_offset);
            map.on_function_end(base + source.native_offset + source.native_size,
                                wasm.data() + source.wasm_offset + source.wasm_size);
         }
         map.on_code_end(base + loaded.allocator._exec_code_size, wasm.data() + wasm.size());
         debug.set(std::move(map));
         debug.relocate(base);
         mod.allocator.release_base_memory();
         mod = std::move(loaded);
         loaded.allocator = growable_allocator{};
         return mod;
      }
   };
}

namespace psizam {
   /// True saved LLVM AOT (O2), using the existing backend host ABI, external
   /// allocator, signal boundary and alternate-stack machinery.
   struct aot_profile {
      using context = detail::jit_execution_context<true>;
      template<typename Options, typename DebugInfo>
      using parser = detail::aot_parser<Options, DebugInfo>;
      static constexpr bool is_jit = true;
      static constexpr bool enable_backtrace = true;
      static constexpr bool reverse_host_args = false;
   };
}
