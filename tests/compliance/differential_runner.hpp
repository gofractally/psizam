#pragma once

// Differential testing runner: executes the same WASM module through
// all available backends and compares results for consistency.

#include <psizam/psizam.hpp>

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psizam::compliance {

   struct engine_result {
      engine      eng;
      std::string engine_name;

      // Exactly one of these is set:
      std::optional<operand_stack_elem> value;  // successful return (nullopt = void)
      std::string                       error;  // exception message (empty = no error)
      bool                              had_error = false;
   };

   inline const char* engine_name(engine e) {
      switch (e) {
         case engine::interpreter: return "interpreter";
#if defined(__x86_64__) || defined(__aarch64__)
         case engine::jit:         return "jit";
         case engine::jit2:        return "jit2";
#endif
      }
      return "unknown";
   }

   inline std::vector<engine> all_engines() {
      return {
         engine::interpreter,
#if defined(__x86_64__) || defined(__aarch64__)
         engine::jit,
         engine::jit2,
#endif
      };
   }

   /// Run a WASM function through all backends and collect results.
   template <typename... Args>
   std::vector<engine_result> run_differential(
         std::span<const uint8_t> wasm,
         std::string_view func_name,
         Args&&... args) {

      std::vector<engine_result> results;

      for (auto eng : all_engines()) {
         engine_result r;
         r.eng = eng;
         r.engine_name = engine_name(eng);

         try {
            wasm_allocator wa;
            host_function_table table;
            compiled_module mod(wasm_code(wasm.begin(), wasm.end()),
                                std::move(table), &wa, {.eng = eng});
            auto inst = mod.create_instance();
            r.value = inst.call_with_return(func_name, std::forward<Args>(args)...);
         } catch (const std::exception& e) {
            r.had_error = true;
            r.error = e.what();
         } catch (...) {
            r.had_error = true;
            r.error = "unknown exception";
         }

         results.push_back(std::move(r));
      }

      return results;
   }

   /// Run with a host_function_table and host pointer.
   template <typename... Args>
   std::vector<engine_result> run_differential(
         std::span<const uint8_t> wasm,
         host_function_table table,
         void* host,
         std::string_view func_name,
         Args&&... args) {

      std::vector<engine_result> results;

      for (auto eng : all_engines()) {
         engine_result r;
         r.eng = eng;
         r.engine_name = engine_name(eng);

         try {
            wasm_allocator wa;
            compiled_module mod(wasm_code(wasm.begin(), wasm.end()),
                                table, &wa, {.eng = eng});
            auto inst = mod.create_instance();
            inst.set_host(host);
            r.value = inst.call_with_return(func_name, std::forward<Args>(args)...);
         } catch (const std::exception& e) {
            r.had_error = true;
            r.error = e.what();
         } catch (...) {
            r.had_error = true;
            r.error = "unknown exception";
         }

         results.push_back(std::move(r));
      }

      return results;
   }

   /// Compare the active payload, never the variant's tag/padding/storage bytes.
   inline bool same_value(const operand_stack_elem& a, const operand_stack_elem& b) {
      if (a.index() != b.index()) return false;
      switch (a.index()) {
         case 0: return a.to_ui32() == b.to_ui32();
         case 1: return a.to_ui64() == b.to_ui64();
         case 2: return a.to_fui32() == b.to_fui32();
         case 3: return a.to_fui64() == b.to_fui64();
         case 4: {
            const auto av = a.to_v128(), bv = b.to_v128();
            return std::memcmp(&av, &bv, sizeof(av)) == 0;
         }
         default: return false;
      }
   }

   /// Check that all engine results agree.
   inline std::string check_agreement(const std::vector<engine_result>& results) {
      if (results.size() < 2) return "";

      const auto& ref = results[0];

      for (size_t i = 1; i < results.size(); i++) {
         const auto& other = results[i];

         if (ref.had_error != other.had_error) {
            return ref.engine_name + " " +
                   (ref.had_error ? "errored" : "succeeded") +
                   " but " + other.engine_name + " " +
                   (other.had_error ? "errored" : "succeeded");
         }

         if (ref.had_error) continue;

         bool ref_has   = ref.value.has_value();
         bool other_has = other.value.has_value();
         if (ref_has != other_has) {
            return ref.engine_name + " returned " +
                   (ref_has ? "a value" : "void") +
                   " but " + other.engine_name + " returned " +
                   (other_has ? "a value" : "void");
         }

         if (!ref_has) continue;

         if (!same_value(*ref.value, *other.value)) {
            return ref.engine_name + " and " + other.engine_name +
                   " returned different value types or payload bits";
         }
      }

      return "";
   }

   /// Assert all engines agree — throws on disagreement.
   inline void assert_all_agree(const std::vector<engine_result>& results) {
      auto msg = check_agreement(results);
      if (!msg.empty()) {
         throw std::runtime_error("Differential test disagreement: " + msg);
      }
   }

} // namespace psizam::compliance
