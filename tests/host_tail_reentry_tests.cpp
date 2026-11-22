#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/host_function.hpp>
#include <fstream>
#include <functional>
#include <iterator>

namespace {
struct byte_options : psizam::default_options {
   uint32_t max_stack_bytes = 128 * 1024;
   static constexpr uint32_t max_func_local_bytes = 1024 * 1024;
   static constexpr uint32_t stack_usage_for_call = 4096;
   static constexpr auto max_func_local_bytes_flags =
      psizam::max_func_local_bytes_flags_t::params |
      psizam::max_func_local_bytes_flags_t::locals |
      psizam::max_func_local_bytes_flags_t::stack;
   byte_options() { gas_metering = false; }
};
struct count_options : psizam::default_options {
   uint32_t max_call_depth = 64;
   static constexpr uint32_t stack_usage_for_call = 4096;
   count_options() { gas_metering = false; }
};
struct host_state {
   std::function<uint64_t(uint32_t)> enter;
   std::function<uint32_t()> remaining;
   std::vector<uint32_t> budgets;
   bool fail = false;
   uint64_t host(uint64_t a, uint32_t b) {
      budgets.push_back(remaining());
      if (b) return a + b + enter(b - 1);
      if (fail) throw std::runtime_error("intentional reentry bottom failure");
      return a;
   }
   void sample() {}
};
std::vector<uint8_t> fixture() {
   std::ifstream stream(PROPER_TAIL_WASM, std::ios::binary);
   REQUIRE(stream.good());
   return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

template<class Impl, class Options>
void exercise(Options options, uint32_t initial, uint32_t reserve) {
   using functions = psizam::registered_host_functions<host_state>;
   functions::template add<&host_state::host>("env", "host");
   functions::template add<&host_state::sample>("env", "sample");
   host_state state;
   psizam::wasm_allocator allocator;
   psizam::backend<functions, Impl, Options> vm(fixture(), state, &allocator, options);
   functions::resolve(vm.get_module());
   state.remaining = [&] { return vm.get_context().get_remaining_call_depth(); };
   state.enter = [&](uint32_t n) {
      auto value = vm.call_with_return(state, "env", "host_tail", n);
      if (!value) throw std::runtime_error("missing reentry return");
      return value->to_ui64();
   };
   // Sixteen finite ordinary C++ host continuations, with each guest tail
   // caller physically replaced. This is resource correctness, not timing.
   CHECK(state.enter(15) == 16120);
   REQUIRE(state.budgets.size() == 16);
   for (uint32_t i = 0; i < state.budgets.size(); ++i) {
      INFO("host continuation=" << i << " remaining=" << state.budgets[i]);
      CHECK(state.budgets[i] == initial - (i + 1) * reserve);
   }
   CHECK(state.remaining() == initial);
   state.fail = true;
   CHECK_THROWS_AS(state.enter(15), std::runtime_error);
   CHECK(state.remaining() == initial);
   state.fail = false;
   state.budgets.clear();
   CHECK(state.enter(0) == 1000);
   REQUIRE(state.budgets.size() == 1);
   CHECK(state.budgets[0] == initial - reserve);
   CHECK(state.remaining() == initial);
}
}

TEMPLATE_TEST_CASE("Host continuations retain a budget across guest tail reentry",
                   "[proper-tail][host-reentry-budget]", psizam::interpreter, psizam::jit, psizam::jit2) {
   SECTION("params-counted byte budget") { exercise<TestType>(byte_options{}, 128 * 1024, 4096); }
   SECTION("call-count budget") { exercise<TestType>(count_options{}, 64, 1); }
}
