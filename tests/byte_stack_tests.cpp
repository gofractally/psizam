#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/pzam_compile.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include <functional>
#include <string>
#include <vector>

namespace {
using namespace psizam;
struct byte_options : default_options {
   static constexpr uint32_t max_func_local_bytes = 128 * 1024 * 1024;
   static constexpr uint32_t stack_usage_for_call = 4096;
   uint32_t max_stack_bytes = 65536;
   uint32_t max_pages = 16;
   uint32_t compile_threads = 0;
   byte_options() { gas_metering = false; defer_start = true; }
};
struct host {
   std::function<uint32_t()> remaining;
   std::function<uint32_t()> nested;
   std::vector<uint64_t> observations;
   uint32_t observe(uint32_t tag) {
      auto value = remaining();
      observations.push_back((uint64_t(tag) << 32) | value);
      return value;
   }
   uint32_t reenter() { return nested(); }
   void fail() { throw std::runtime_error("intentional host failure"); }
};
host_function_table table() {
   host_function_table result;
   result.add<&host::observe>("env", "observe");
   result.add<&host::reenter>("env", "reenter");
   result.add<&host::fail>("env", "fail");
   return result;
}
struct outcome {
   bool success = false;
   uint32_t value = 0;
   uint32_t remaining = 0;
   std::string error;
   std::vector<uint64_t> observations;
   bool operator==(const outcome&) const = default;
};
using invoke = std::function<uint32_t(const char*, uint32_t)>;
outcome call(host& h, const invoke& run, const char* name, uint32_t n) {
   h.observations.clear();
   outcome result;
   try { result.value = run(name, n); result.success = true; }
   catch (const wasm_interpreter_exception& e) { result.error = std::string("wasm:") + e.what(); }
   catch (const wasm_memory_exception& e) { result.error = std::string("memory:") + e.what(); }
   catch (const std::runtime_error& e) { result.error = std::string("host:") + e.what(); }
   catch (const std::exception& e) { result.error = std::string("other:") + e.what(); }
   result.remaining = h.remaining();
   result.observations = h.observations;
   return result;
}
const auto entries = {"leaf", "outer", "indirect", "tail-direct", "tail-indirect",
                      "recursive", "tail-self", "host-nested", "host-failure", "trap", "leaf"};
void compare(const std::vector<outcome>& actual, const std::vector<outcome>& reference, const char* engine) {
   REQUIRE(actual.size() == reference.size());
   auto name = entries.begin();
   for (size_t i = 0; i < actual.size(); ++i, ++name) {
      CAPTURE(engine, *name, actual[i].success, reference[i].success, actual[i].value, reference[i].value,
              actual[i].remaining, reference[i].remaining, actual[i].error, reference[i].error,
              actual[i].observations, reference[i].observations);
      CHECK(actual[i] == reference[i]);
   }
}
std::vector<outcome> exercise(host& h, const invoke& run) {
   h.nested = [&] { return run("leaf", 0); };
   std::vector<outcome> results;
   for (auto name : entries)
      results.push_back(call(h, run, name, std::string_view(name) == "tail-self" ? 25 : 3));
   return results;
}
template<class Impl>
std::vector<outcome> native(uint32_t budget, const char* path = BYTE_STACK_WASM, bool eh = false) {
   host h;
   auto wasm = read_wasm(path);
   byte_options options;
   options.max_stack_bytes = budget;
   wasm_allocator memory;
   using registry = registered_host_functions<host>;
   backend<registry, Impl, byte_options> engine(wasm, table(), &h, &memory, options);
   h.remaining = [&] { return engine.get_context().get_remaining_call_depth(); };
   invoke run = [&](const char* name, uint32_t n) {
      auto value = engine.call_with_return(h, "env", name, n);
      if (!value) throw std::runtime_error("missing return value");
      return value->to_ui32();
   };
   if (eh) {
      return {call(h, run, "caught", 0), call(h, run, "leaf", 0)};
   }
   return exercise(h, run);
}

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
std::vector<char> saved(uint32_t threads) {
   auto wasm = read_wasm(BYTE_STACK_WASM);
   module mod;
   mod.allocator.use_default_memory();
   detail::pzam_compile_result result;
#if defined(__aarch64__)
   constexpr auto arch = pzam_arch::aarch64;
   result.target_triple = "aarch64-unknown-linux-gnu";
#else
   constexpr auto arch = pzam_arch::x86_64;
   result.target_triple = "x86_64-unknown-linux-gnu";
#endif
   result.opt_level = 2;
   result.softfloat = true;
   byte_options options;
   options.compile_threads = threads;
   detail::null_debug_info debug;
   {
      detail::binary_parser<detail::ir_writer_llvm_aot, byte_options, detail::null_debug_info>
         parser(mod.allocator, options, false, true);
      parser.set_compile_result(&result);
      parser.parse_module(wasm, mod, debug);
   }
   CAPTURE(result.error);
   REQUIRE(result.error.empty());
   mod.finalize();
   pzam_file file;
   file.input_hash = pzam_cache::hash_wasm(wasm);
   file.metadata = extract_metadata(mod);
   file.code_sections.push_back(detail::make_pzam_code_section(mod, result, arch,
      pzam_opt_tier::llvm_O2, true, false));
   REQUIRE(file.code_sections[0].stack_limit_mode == 1);
   return pzam_save(file);
}
std::vector<outcome> aot(const std::vector<char>& bytes, uint32_t budget) {
   host h;
   auto loaded = detail::load_pzam(pzam_load(bytes));
   std::shared_ptr<void> owner(loaded.exec_code, [](void* p) { jit_allocator::instance().free(p); });
   auto bindings = table();
   bindings.resolve(loaded.mod);
   wasm_allocator memory;
   detail::jit_execution_context<> context(loaded.mod, budget);
   context.set_wasm_allocator(&memory);
   context.set_host_table(&bindings);
   context.set_max_pages(16);
   context.reset();
   h.remaining = [&] { return context.get_remaining_call_depth(); };
   invoke run = [&](const char* name, uint32_t n) {
      auto value = context.execute(&h, detail::jit_visitor{nullptr},
                                  loaded.mod.get_exported_function(name), n);
      if (!value) throw std::runtime_error("missing return value");
      return value->to_ui32();
   };
   return exercise(h, run);
}
#endif
}

TEST_CASE("IR backends retain exact JIT1 logical byte budgets", "[byte-stack]") {
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   const auto serial = saved(0);
   const auto parallel = saved(2);
#endif
   for (auto budget : {65536u, 256u, 128u, 16u}) {
      CAPTURE(budget);
      auto reference = native<jit>(budget);
      REQUIRE(reference.size() == entries.size());
      compare(native<jit2>(budget), reference, "jit2");
      compare(native<jit_profile>(budget), reference, "jit-profile");
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      compare(aot(serial, budget), reference, "LLVM-O2-serial");
      compare(aot(parallel, budget), reference, "LLVM-O2-parallel");
#endif
      CHECK(reference.back().remaining == budget);
      CHECK(reference.front().success == (budget >= 128));
   }
}

TEST_CASE("A byte budget accepts its exact frame cost and rejects one byte less", "[byte-stack]") {
   auto high = native<jit>(65536);
   REQUIRE(high.front().success);
   REQUIRE(high.front().observations.size() == 1);
   const auto cost = 65536u - uint32_t(high.front().observations[0]);
   REQUIRE(cost >= 16);
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   const auto bytes = saved(0);
#endif
   for (auto budget : {cost - 1, cost}) {
      CAPTURE(budget, cost);
      auto reference = native<jit>(budget);
      CHECK(reference.front().success == (budget == cost));
      CHECK(reference.front().remaining == budget);
      compare(native<jit2>(budget), reference, "jit2");
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      compare(aot(bytes, budget), reference, "LLVM-O2");
#endif
   }
}

TEST_CASE("Caught exceptions restore JIT byte budgets", "[byte-stack][eh]") {
   // LLVM AOT currently rejects the existing EH helper relocations. psibase's
   // common profile disables EH; keep this broader JIT-only regression separate.
   for (auto budget : {65536u, 256u, 128u, 16u}) {
      CAPTURE(budget);
      auto reference = native<jit>(budget, BYTE_STACK_EH_WASM, true);
      auto actual = native<jit2>(budget, BYTE_STACK_EH_WASM, true);
      REQUIRE(reference.size() == 2);
      REQUIRE(actual.size() == reference.size());
      for (size_t i = 0; i < actual.size(); ++i) {
         CAPTURE(i, actual[i].success, reference[i].success, actual[i].value,
                 reference[i].value, actual[i].remaining, reference[i].remaining,
                 actual[i].error, reference[i].error, actual[i].observations,
                 reference[i].observations);
         CHECK(actual[i] == reference[i]);
      }
   }
}
