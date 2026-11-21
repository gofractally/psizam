#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/host_function.hpp>
#include <fstream>
#include <iterator>

namespace {
uint64_t host(uint64_t a, uint32_t b) { return a + b; }
void sample() {}
using functions = psizam::registered_host_functions<psizam::standalone_function_t>;

std::vector<uint8_t> fixture() {
   std::ifstream in(PROPER_TAIL_WASM, std::ios::binary);
   REQUIRE(in.good());
   return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

TEST_CASE("Private guest cleanup preserves ordinary call continuations and return bits", "[proper-tail][abi]") {
   functions::add<&host>("env", "host");
   functions::add<&sample>("env", "sample");
   psizam::wasm_allocator allocator;
   psizam::backend<functions, psizam::jit> vm(fixture(), &allocator);
   functions::resolve(vm.get_module());
   CHECK(vm.call_with_return("env", "ordinary")->to_ui64() == 3155);
   CHECK(vm.call_with_return("env", "ordinary_indirect")->to_ui64() == UINT64_C(0x123456789abce54));
   CHECK(vm.call_with_return("env", "ordinary_vector")->to_ui64() == UINT64_C(0xfedcba9876543275));
   CHECK(vm.call_with_return("env", "ordinary_multi")->to_ui64() == UINT64_C(0xfedcba9876543275));
   for (uint64_t bits : {UINT64_C(0), UINT64_C(0x8000000000000000),
                        UINT64_C(0x7ff800123456789a), UINT64_C(0xfff000123456789a)})
      CHECK(vm.call_with_return("env", "ordinary_bits", bits)->to_ui64() == bits);
   CHECK(vm.call_with_return("env", "ordinary")->to_ui64() == 3155);
}
