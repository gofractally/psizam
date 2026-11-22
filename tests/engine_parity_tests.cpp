// Differential tests: every native engine must agree with JIT1 on results and
// on whether a call traps. Each fixture was a divergence at some point.
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/pzam_compile.hpp>
#include <psizam/detail/pzam_loader.hpp>
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
#include <psizam/aot.hpp>
#include <pthread.h>
#endif
#include <functional>
#include <string>
#include <vector>

namespace {
using namespace psizam;
struct parity_options : default_options {
   uint32_t max_pages = 16;
   uint32_t compile_threads = 0;
   parity_options() { gas_metering = false; defer_start = true; }
};
struct host {};
struct outcome {
   bool success = false;
   uint32_t value = 0;
   std::string failure;   // exception class only; messages differ per engine
   bool operator==(const outcome&) const = default;
};
std::ostream& operator<<(std::ostream& out, const outcome& o) {
   return o.success ? out << "value " << o.value : out << "trap " << o.failure;
}
using invoke = std::function<uint32_t(const char*, uint32_t)>;
outcome call(const invoke& run, const char* name, uint32_t argument) {
   outcome result;
   try { result.value = run(name, argument); result.success = true; }
   catch (const wasm_interpreter_exception&) { result.failure = "wasm"; }
   catch (const wasm_memory_exception&) { result.failure = "memory"; }
   catch (const std::exception& e) { result.failure = std::string("other:") + e.what(); }
   return result;
}
struct query { const char* name; uint32_t argument; };
using queries = std::vector<query>;
std::vector<outcome> run_all(const invoke& run, const queries& calls) {
   std::vector<outcome> results;
   for (auto& q : calls) results.push_back(call(run, q.name, q.argument));
   return results;
}
template<class Impl, class Options = parity_options>
std::vector<outcome> native(const char* path, const queries& calls) {
   host h;
   auto wasm = read_wasm(path);
   Options options;
   wasm_allocator memory;
   using registry = registered_host_functions<host>;
   backend<registry, Impl, Options> engine(wasm, host_function_table{}, &h, &memory, options);
   invoke run = [&](const char* name, uint32_t n) {
      auto value = engine.call_with_return(h, "env", name, n);
      if (!value) throw std::runtime_error("missing return value");
      return value->to_ui32();
   };
   return run_all(run, calls);
}
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
std::vector<char> saved(const char* path) {
   auto wasm = read_wasm(path);
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
   parity_options options;
   detail::null_debug_info debug;
   {
      detail::binary_parser<detail::ir_writer_llvm_aot, parity_options, detail::null_debug_info>
         parser(mod.allocator, options, false, false);
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
   return pzam_save(file);
}
// Saved and reloaded, exactly as a warm node executes it.
std::vector<outcome> aot(const char* path, const queries& calls) {
   host h;
   const auto bytes = saved(path);
   auto loaded = detail::load_pzam(pzam_load(bytes));
   std::shared_ptr<void> owner(loaded.exec_code, [](void* p) { jit_allocator::instance().free(p); });
   host_function_table bindings;
   bindings.resolve(loaded.mod);
   wasm_allocator memory;
   detail::jit_execution_context<> context(loaded.mod, 250);
   context.set_wasm_allocator(&memory);
   context.set_host_table(&bindings);
   context.set_max_pages(16);
   context.reset();
   invoke run = [&](const char* name, uint32_t n) {
      auto value = context.execute(&h, detail::jit_visitor{nullptr},
                                  loaded.mod.get_exported_function(name), n);
      if (!value) throw std::runtime_error("missing return value");
      return value->to_ui32();
   };
   return run_all(run, calls);
}
#endif
void expect_parity(const char* path, const queries& calls, const std::vector<outcome>& expected) {
   auto check = [&](const std::vector<outcome>& actual, const char* engine) {
      REQUIRE(actual.size() == expected.size());
      for (size_t i = 0; i < actual.size(); ++i) {
         CAPTURE(engine, calls[i].name, calls[i].argument);
         CHECK(actual[i] == expected[i]);
      }
   };
   check(native<jit>(path, calls), "jit1");
   check(native<jit2>(path, calls), "jit2");
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   check(aot(path, calls), "LLVM-O2-saved");
   check(native<aot_profile>(path, calls), "LLVM-O2-cold");
#endif
}
outcome value(uint32_t v) { return {true, v, {}}; }
outcome trap() { return {false, 0, "wasm"}; }
}

TEST_CASE("A null table element stays null across engines and artifact reload", "[parity][table]") {
   expect_parity(ENGINE_PARITY_WASM, {{"indirect-good", 0}, {"indirect-null", 0}, {"indirect-good", 0}},
                 {value(42), trap(), value(42)});
}

TEST_CASE("br_if to a loop leaves its parameters intact when not taken", "[parity][loop]") {
   expect_parity(ENGINE_PARITY_WASM,
      {{"loop-param-fallthrough", 0}, {"loop-param-countdown", 1}, {"loop-param-countdown", 4},
       {"loop-param-deep", 1}, {"loop-param-deep", 3},
       {"loop-param-f64", 0}, {"loop-param-f32", 0}, {"loop-param-i64", 0}},
      // countdown(n): the parameter becomes n+1000, ..., 2+1000 on taken
      // branches; the final pass adds 1+1000 to the last parameter.
      {value(107), value(7 + 1001), value(1002 + 1001),
       value(((10 + 20 + 30 + 3) * 7 + 2) * 5 + 1), value(((10 + 20 + 30 + 30) * 7 + 20) * 5 + 10),
       value(15), value(15), value(0x80)});
}

TEST_CASE("Loop parameters are assigned in parallel on every branch kind", "[parity][loop]") {
   expect_parity(ENGINE_PARITY_WASM,
      {{"loop-param-swap", 1}, {"loop-param-swap", 2}, {"loop-param-swap", 5},
       {"loop-param-fib", 1}, {"loop-param-fib", 10}},
      {value(5 | (3 << 16)), value(3 | (5 << 16)), value(5 | (3 << 16)),
       value(1), value(55)});
}

TEST_CASE("Conditional branches into a wide loop compile on every engine", "[parity][loop][capacity]") {
   uint32_t expected = 1200;
   for (int i = 10; i >= 0; --i) expected = expected * 3 + (i >= 12 ? 0 : 100 * (i + 1));
   for (int i = 11; i >= 0; --i) expected = expected * 3 + (i + 1);
   expect_parity(ENGINE_PARITY_WIDE_LOOP_WASM, {{"wide-loop", 0}}, {value(expected)});
}

// SIMD operand ids in the shared IR are 16 bits wide. A function too large
// for them must run correctly or be refused at compile time; it must never be
// compiled with one vector aliasing another.
namespace {
const queries wide_simd_calls{{"wide", 5}, {"wide", 77}};
const std::vector<outcome> wide_simd_expected{value(5 + 1000), value(77 + 1000)};
template<class Run>
void matches_or_refuses(Run&& run) {
   std::vector<outcome> actual;
   try {
      actual = run();
   } catch (const wasm_parse_exception& e) {
      CHECK(std::string(e.what()).find("SIMD") != std::string::npos);
      return;
   }
   CHECK(actual == wide_simd_expected);
}
}
TEST_CASE("Shuffle lane indices are never taken for operand registers", "[parity][simd]") {
   expect_parity(ENGINE_PARITY_WASM, {{"shuffle-lanes-are-immediate", 0}, {"shuffle-lanes-are-immediate", 4}},
                 {value(10203 + 8 * 7), value(50607 + 8 * 7)});
}
// Each deterministic float mode and profile, on every engine.
template<fp_mode Mode, float_profile Profile>
struct float_options : parity_options {
   static constexpr auto fp = Mode;
   static constexpr auto float_profile = Profile;
};
template<fp_mode Mode, float_profile Profile>
void expect_float_parity(const char* mode) {
   using options = float_options<Mode, Profile>;
   const bool fused = Profile == float_profile::performance;
   const queries calls{
      {"madd-f32", 0}, {"nmadd-f32", 0}, {"madd-f64", 0}, {"nmadd-f64", 0},
      {"madd-nan", 0xffa12345}, {"madd-nan", 0x7f800001},
      {"min-f32", 0xffc12345}, {"max-f32", 0x7fa00001}, {"min-f32", 0x80000000}, {"max-f32", 0},
      {"min-f64", 0xfff81234}, {"max-f64", 0x7ff40000}, {"min-f64", 0x80000000}, {"max-f64", 0},
      {"promote", 0x7fa00001}, {"promote", 0xffc12345}, {"promote", 0x3f800000},
      {"demote", 0x7ff40000}, {"demote", 0xfff81234}, {"demote", 0x3ff00000},
      {"promote-low", 0xffa12345}, {"demote-zero", 0xfff40000}};
   const std::vector<outcome> expected{
      value(fused ? 0x33800000 : 0), value(fused ? 0xb3800000 : 0),
      value(fused ? 0x3c900000 : 0), value(fused ? 0xbc900000 : 0),
      value(0x7fc00000), value(0x7fc00000),
      value(0x7fc00000), value(0x7fc00000), value(0x80000000), value(0),
      value(0x7ff80000), value(0x7ff80000), value(0x80000000), value(0),
      value(0x7ff80000), value(0x7ff80000), value(0x3ff00000),
      value(0x7fc00000), value(0x7fc00000), value(0x3f800000),
      value(0x7ff80000), value(0x7fc00000)};
   auto check = [&](const std::vector<outcome>& actual, const char* engine) {
      REQUIRE(actual.size() == expected.size());
      for (size_t i = 0; i < actual.size(); ++i) {
         CAPTURE(mode, engine, calls[i].name, calls[i].argument);
         CHECK(actual[i] == expected[i]);
      }
   };
   check(native<interpreter, options>(ENGINE_PARITY_FLOAT_WASM, calls), "interpreter");
   check(native<jit, options>(ENGINE_PARITY_FLOAT_WASM, calls), "jit1");
   check(native<jit2, options>(ENGINE_PARITY_FLOAT_WASM, calls), "jit2");
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   check(native<aot_profile, options>(ENGINE_PARITY_FLOAT_WASM, calls), "LLVM-O2-cold");
#endif
}
TEST_CASE("The strict float profile rounds relaxed_madd twice on every engine", "[parity][float]") {
   expect_float_parity<fp_mode::softfloat, float_profile::strict>("softfloat");
   expect_float_parity<fp_mode::hw_deterministic, float_profile::strict>("hw_deterministic");
}
TEST_CASE("The performance float profile fuses relaxed_madd on every engine", "[parity][float]") {
   expect_float_parity<fp_mode::softfloat, float_profile::performance>("softfloat");
   expect_float_parity<fp_mode::hw_deterministic, float_profile::performance>("hw_deterministic");
}
TEST_CASE("JIT1 runs a function with more than 65535 values and SIMD", "[parity][simd]") {
   CHECK(native<jit>(ENGINE_PARITY_WIDE_SIMD_WASM, wide_simd_calls) == wide_simd_expected);
}
TEST_CASE("JIT2 never aliases SIMD operands in an oversized function", "[parity][simd][simd-jit2]") {
   matches_or_refuses([] { return native<jit2>(ENGINE_PARITY_WIDE_SIMD_WASM, wide_simd_calls); });
}
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
TEST_CASE("LLVM AOT never aliases SIMD operands in an oversized function", "[parity][simd][simd-aot]") {
   matches_or_refuses([] { return aot(ENGINE_PARITY_WIDE_SIMD_WASM, wide_simd_calls); });
}
#endif

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
// A cold node compiles wherever the module is first needed, which may be a
// small or nearly exhausted stack (a nested call runs on the caller's WASM
// stack). The compile must not depend on the stack it was requested from.
TEST_CASE("A cold AOT compile does not run on the requesting stack", "[parity][cold-stack]") {
   struct job {
      std::vector<outcome> actual;
      std::string error;
   } state;
   pthread_attr_t attributes;
   REQUIRE(pthread_attr_init(&attributes) == 0);
   REQUIRE(pthread_attr_setstacksize(&attributes, 128 * 1024) == 0);
   pthread_t thread;
   REQUIRE(pthread_create(&thread, &attributes, [](void* arg) -> void* {
      auto& state = *static_cast<job*>(arg);
      try {
         state.actual = native<aot_profile>(ENGINE_PARITY_WIDE_LOOP_WASM, {{"wide-loop", 0}});
      } catch (const std::exception& e) {
         state.error = e.what();
      }
      return nullptr;
   }, &state) == 0);
   pthread_attr_destroy(&attributes);
   REQUIRE(pthread_join(thread, nullptr) == 0);
   CHECK(state.error == "");
   CHECK(state.actual == native<jit>(ENGINE_PARITY_WIDE_LOOP_WASM, {{"wide-loop", 0}}));
}
#endif
