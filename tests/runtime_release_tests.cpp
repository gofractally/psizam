#include <psizam/runtime.hpp>
#include <catch2/catch.hpp>
#include <cstring>
#include <fstream>

using policy = psizam::instance_policy;
static std::vector<uint8_t> fixture(const char* name) {
   std::ifstream in(std::string{RUNTIME_RELEASE_DIR} + "/" + name + ".wasm", std::ios::binary);
   REQUIRE(in.good());
   return {std::istreambuf_iterator<char>(in), {}};
}
static uint32_t word(psizam::instance& inst, unsigned offset) {
   uint32_t value;
   std::memcpy(&value, inst.linear_memory() + offset, sizeof(value));
   return value;
}
static std::vector<policy::compile_tier> tiers() {
   std::vector<policy::compile_tier> result{policy::compile_tier::interpret};
#if defined(__x86_64__) || defined(__aarch64__)
   result.push_back(policy::compile_tier::jit1);
   result.push_back(policy::compile_tier::jit2);
#endif
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   result.push_back(policy::compile_tier::jit_llvm);
#endif
   return result;
}

TEST_CASE("public runtime resources and initialization", "[runtime-release][resource]") {
   const auto start = fixture("start"), spin = fixture("spin"), spin_start = fixture("spin-start");
   for (auto tier : tiers()) {
      CAPTURE(static_cast<int>(tier));
      psizam::runtime rt({.guarded_pool_size = 0});
      policy p; p.initial = tier; p.gas_budget = 100;
      auto mod = rt.prepare(psizam::wasm_bytes{start}, p);
      auto inst = rt.instantiate(mod);
      REQUIRE(word(inst, 0) == 1);
      REQUIRE(inst.gas_consumed() > 0);
      REQUIRE(inst.gas()->consumed == inst.gas_consumed());
      REQUIRE(inst.gas_deadline() == p.gas_budget);
      const auto before = inst.gas_consumed();
      CHECK(inst.run_start() == 0);
      CHECK(word(inst, 0) == 1); // module.start must not execute twice
      CHECK(word(inst, 4) == 42);
      CHECK(inst.gas_consumed() > before);
      inst.set_deadline(inst.gas_consumed());
      CHECK_THROWS_AS(inst.run_start(), psizam::wasm_gas_exhausted_exception);
      inst.set_deadline(inst.gas_consumed() + 100);
      CHECK_NOTHROW(inst.run_start());
      inst.interrupt();
      CHECK_THROWS_AS(inst.run_start(), psizam::wasm_gas_exhausted_exception);

      // The same Wasm with a new policy gets independent bindings and state.
      p.metering = policy::meter_mode::none; p.gas_budget = 0;
      auto unlimited_mod = rt.prepare(psizam::wasm_bytes{start}, p);
      CHECK(unlimited_mod.get() != mod.get());
      auto unlimited = rt.instantiate(unlimited_mod);
      CHECK_NOTHROW(unlimited.run_start());
      CHECK(unlimited.gas_consumed() == 0);
      CHECK(inst.gas_deadline() == 0);

      p.metering = policy::meter_mode::gas_trap; p.gas_budget = 0;
      CHECK_THROWS_AS(rt.instantiate(rt.prepare(psizam::wasm_bytes{start}, p)),
                      psizam::wasm_gas_exhausted_exception);
      p.gas_budget = 100;
      CHECK_THROWS_AS(rt.instantiate(rt.prepare(psizam::wasm_bytes{spin_start}, p)),
                      psizam::wasm_gas_exhausted_exception);
      auto loop = rt.instantiate(rt.prepare(psizam::wasm_bytes{spin}, p));
      CHECK_THROWS_AS(loop.run_start(), psizam::wasm_gas_exhausted_exception);

      p.metering = policy::meter_mode::timeout; p.timeout_ms = 1; p.gas_slice = 100;
      auto timed = rt.instantiate(rt.prepare(psizam::wasm_bytes{spin}, p));
      CHECK_THROWS_AS(timed.run_start(), psizam::timeout_exception);
      CHECK_THROWS_AS(rt.instantiate(rt.prepare(psizam::wasm_bytes{spin_start}, p)),
                      psizam::timeout_exception);

      unsigned yields = 0;
      p.metering = policy::meter_mode::gas_yield; p.gas_slice = 10;
      p.yield_handler = +[](psizam::gas_state* gas, void* data) {
         if (++*static_cast<unsigned*>(data) == 3)
            throw psizam::wasm_gas_exhausted_exception{"test: stop after three yields"};
         gas->deadline = gas->consumed + 10;
      };
      p.yield_user_data = &yields;
      auto yielding = rt.instantiate(rt.prepare(psizam::wasm_bytes{spin}, p));
      CHECK_THROWS_AS(yielding.run_start(), psizam::wasm_gas_exhausted_exception);
      CHECK(yields == 3);
      yielding.interrupt();
      CHECK_THROWS_AS(yielding.run_start(), psizam::wasm_gas_exhausted_exception);
      CHECK(yields == 3);

      p.metering = policy::meter_mode::gas_trap;
      p.pool = std::make_shared<psizam::gas_pool>(psizam::gas_units{1000}, psizam::gas_units{100});
      auto pooled = rt.instantiate(rt.prepare(psizam::wasm_bytes{start}, p));
      CHECK(pooled.gas_consumed() > 0);
      pooled.interrupt();
      CHECK_THROWS_AS(pooled.run_start(), psizam::wasm_gas_exhausted_exception);
   }
}
