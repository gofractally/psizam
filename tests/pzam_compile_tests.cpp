#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/pzam_compile.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include <psizam/backend.hpp>
#include <psizam/detail/execution_limits.hpp>

using namespace psizam;
namespace {
// (module (func (export "add") (param i32 i32) (result i32)
//   local.get 0 local.get 1 i32.add))
const std::vector<uint8_t> add_wasm{
   0,97,115,109,1,0,0,0, 1,7,1,0x60,2,0x7f,0x7f,1,0x7f,
   3,2,1,0, 7,7,1,3,'a','d','d',0,0,
   10,9,1,7,0,0x20,0,0x20,1,0x6a,0x0b};
const std::vector<uint8_t> empty_wasm{0,97,115,109,1,0,0,0};

void execute(const pzam_file& file, bool floating = false) {
   auto bytes = pzam_save(file);
   auto loaded = psizam::detail::load_pzam(pzam_load(bytes));
   std::shared_ptr<void> code(loaded.exec_code, [](void* p) { jit_allocator::instance().free(p); });
   wasm_allocator wa;
   psizam::detail::jit_execution_context<> context(loaded.mod, 8192);
   context.set_wasm_allocator(&wa);
   context.reset();
   if (floating) {
      auto value = context.execute(nullptr, psizam::detail::jit_visitor{nullptr},
                                  loaded.mod.get_exported_function("add"), 1.5, 2.25);
      REQUIRE(value);
      CHECK(value->to_f64() == 3.75);
   } else {
      auto value = context.execute(nullptr, psizam::detail::jit_visitor{nullptr},
                                  loaded.mod.get_exported_function("add"), uint32_t{19}, uint32_t{23});
      REQUIRE(value);
      CHECK(value->to_ui32() == 42);
   }
}

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
TEST_CASE("pzam_compile softfloat selects software floating point code", "[pzam-compile]") {
   auto wasm = add_wasm;
   for (auto& b : wasm) if (b == 0x7f) b = 0x7c; // i32 type -> f64
   wasm[wasm.size()-2] = 0xa0; // f64.add
   auto hardware = pzam_load(pzam_compile(wasm, {.tier=pzam_tier_flags::llvm_O2}));
   auto software = pzam_load(pzam_compile(wasm, {.tier=pzam_tier_flags::llvm_O2,
                                               .flags=pzam_compile_flags::softfloat}));
   CHECK(hardware.code_sections[0].code_blob != software.code_sections[0].code_blob);
   const auto sf_add = uint16_t(psizam::detail::reloc_symbol::sf_f64_add);
   CHECK(std::any_of(software.code_sections[0].relocations.begin(),
                     software.code_sections[0].relocations.end(),
                     [=](const auto& r) { return r.symbol == sf_add; }));
   execute(hardware, true);
   execute(software, true);
}
#endif
}

TEST_CASE("pzam_compile rejects requests it cannot fulfill", "[pzam-compile]") {
   for (auto arch : {pzam_arch_flags::none, static_cast<pzam_arch_flags>(128)})
      CHECK_THROWS_AS(pzam_compile(add_wasm, {.arch=arch}), std::invalid_argument);
   for (auto tier : {pzam_tier_flags::none, static_cast<pzam_tier_flags>(128)})
      CHECK_THROWS_AS(pzam_compile(add_wasm, {.tier=tier}), std::invalid_argument);
   for (auto flags : {pzam_compile_flags::debug_info,
                      pzam_compile_flags::softfloat, static_cast<pzam_compile_flags>(128)})
      CHECK_THROWS_AS(pzam_compile(add_wasm, {.flags=flags}), std::invalid_argument);
#ifndef PSIZAM_ENABLE_LLVM_BACKEND
   CHECK_THROWS_AS(pzam_compile(add_wasm, {.tier=pzam_tier_flags::llvm_O2}), std::invalid_argument);
#endif
}

TEST_CASE("JIT PZAM rejects an incompatible memory page layout", "[pzam-compile]") {
   auto file = pzam_load(pzam_compile(add_wasm, {.tier=pzam_tier_flags::jit2}));
   file.code_sections.front().page_size = wasm_allocator::table_size() == 4096 ? 16384 : 4096;
   CHECK_THROWS_WITH(psizam::detail::load_pzam(file), pzam_page_layout_error);
}

TEST_CASE("pzam_compile emits actual code for each requested target and tier", "[pzam-compile]") {
   auto tiers = pzam_tier_flags::all_jit;
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   tiers = tiers | pzam_tier_flags::all_llvm;
#endif
   auto bytes = pzam_compile(add_wasm, {.arch=pzam_arch_flags::all, .tier=tiers});
   auto file = pzam_load(bytes);
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   REQUIRE(file.code_sections.size() == 10);
#else
   REQUIRE(file.code_sections.size() == 4);
#endif
   for (const auto& section : file.code_sections) {
      auto arch = static_cast<pzam_arch>(section.arch);
      auto tier = static_cast<pzam_opt_tier>(section.opt_tier);
      CAPTURE(section.arch, section.opt_tier);
      REQUIRE(section.functions.size() == 1);
      REQUIRE_FALSE(section.code_blob.empty());
      CHECK(section.compiler.compiler_name == (tier == pzam_opt_tier::jit1 ? "psizam-jit1" : tier == pzam_opt_tier::jit2 ? "psizam-jit2" : "psizam-llvm"));
      pzam_compile_options single;
      for (auto [flag, val] : psizam::detail::arch_map) if (val == arch) single.arch = flag;
      for (auto [flag, val] : psizam::detail::tier_map) if (val == tier) single.tier = flag;
      auto one = pzam_load(pzam_compile(add_wasm, single));
      CHECK(one.code_sections.front().code_blob == section.code_blob);
      // Each host section must reload and execute through the shared PZAM loader and execution context.
      if (single.arch == pzam_arch_flags::host) execute(one);
   }
   // Foreign code must not be a relabeled copy of the host's JIT2 code.
   const auto half = file.code_sections.size()/2;
   CHECK(file.code_sections[0].code_blob != file.code_sections[half].code_blob);
}

TEST_CASE("pzam_compile preserves identity for empty modules and instrumentation", "[pzam-compile]") {
   auto tiers = pzam_tier_flags::all_jit;
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   tiers = tiers | pzam_tier_flags::all_llvm;
#endif
   auto file = pzam_load(pzam_compile(empty_wasm, {.tier=tiers}));
   for (const auto& section : file.code_sections) {
      CHECK(section.compiler.compiler_name == (section.opt_tier == uint8_t(pzam_opt_tier::jit1) ? "psizam-jit1" : section.opt_tier == uint8_t(pzam_opt_tier::jit2) ? "psizam-jit2" : "psizam-llvm"));
      CHECK(section.functions.empty());
      CHECK(section.code_blob.empty());
   }
   auto flags = pzam_compile_flags::backtrace;
   auto tier = pzam_tier_flags::jit2;
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   tier = pzam_tier_flags::llvm_O2;
   flags = flags | pzam_compile_flags::softfloat;
#endif
   auto instrumented = pzam_load(pzam_compile(add_wasm, {.tier=tier, .flags=flags}));
   CHECK(instrumented.code_sections[0].instrumentation.async_backtrace == 1);
   CHECK(instrumented.code_sections[0].instrumentation.softfloat == !!(flags & pzam_compile_flags::softfloat));
   execute(instrumented);
}

namespace {
const std::vector<uint8_t> resource_recovery_wasm{0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,3,2,1,0,7,7,1,3,114,117,110,0,0,10,13,1,11,0,3,64,32,0,13,0,11,65,42,11};
template<class Impl> void check_page_timeout_recovery() {
   auto wasm = resource_recovery_wasm;
   wasm_allocator wa;
   default_options options;
   options.gas_metering = false;
   backend<std::nullptr_t, Impl> engine(wasm, &wa, options);
   for (int trial = 0; trial < 2; ++trial) {
      CHECK_THROWS_AS(engine.timed_run(detail::watchdog{std::chrono::milliseconds(10)}, [&] {
         engine.call("env", "run", uint32_t{1});
      }), timeout_exception);
      CHECK(engine.get_context().gas().consumed == 0);
      auto value = engine.call_with_return("env", "run", uint32_t{0});
      REQUIRE(value);
      CHECK(value->to_ui32() == 42);
   }
}
}

TEST_CASE("page interruption restores reusable code without gas checks", "[resource-limits]") {
   check_page_timeout_recovery<interpreter>();
   check_page_timeout_recovery<jit>();
   check_page_timeout_recovery<jit2>();
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   check_page_timeout_recovery<jit_llvm>();
#endif
}

TEST_CASE("PZAM gas instrumentation is effective and correctly labeled", "[resource-limits][pzam-compile]") {
   std::vector<pzam_tier_flags> tiers{pzam_tier_flags::jit1, pzam_tier_flags::jit2};
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   tiers.push_back(pzam_tier_flags::llvm_O2);
#endif
   for (auto tier : tiers) {
      std::vector<uint8_t> plain;
      for (bool meter : {false, true}) {
         auto file = pzam_load(pzam_compile(add_wasm, {.tier=tier,
            .flags=meter ? pzam_compile_flags::gas_meter : pzam_compile_flags::none}));
         CHECK(file.code_sections[0].instrumentation.gas_metering == meter);
         if (!meter) plain = file.code_sections[0].code_blob;
         else CHECK(file.code_sections[0].code_blob != plain);
         auto loaded = detail::load_pzam(file);
         std::shared_ptr<void> owner(loaded.exec_code, [](void* p) { jit_allocator::instance().free(p); });
         wasm_allocator wa;
         detail::jit_execution_context<> context(loaded.mod, 8192);
         context.set_wasm_allocator(&wa); context.reset();
         context.set_gas_budget(gas_units{1});
         auto call = [&] { return context.execute(nullptr, detail::jit_visitor{nullptr},
            loaded.mod.get_exported_function("add"), uint32_t{19}, uint32_t{23}); };
         if (meter) CHECK_THROWS_AS(call(), wasm_gas_exhausted_exception);
         else { CHECK(call()->to_ui32() == 42); CHECK(context.gas().consumed == 0); }
      }
   }
}
