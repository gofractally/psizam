// Wide arithmetic (i64.add128, i64.sub128, i64.mul_wide_s, i64.mul_wide_u) is
// part of the instruction set every engine accepts. Each engine is compared
// with 128-bit host arithmetic.
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include "wide_arithmetic_fixture.hpp"

namespace {
struct wide_options : psizam::default_options {
   uint32_t compile_threads = 0;
};
}

#if defined(PSIZAM_ENABLE_LLVM_BACKEND) && (defined(__x86_64__) || defined(__aarch64__))
#define WIDE_ENGINES psizam::interpreter, psizam::jit, psizam::jit2, psizam::jit_profile, psizam::jit_llvm
#elif defined(__x86_64__) || defined(__aarch64__)
#define WIDE_ENGINES psizam::interpreter, psizam::jit, psizam::jit2, psizam::jit_profile
#else
#define WIDE_ENGINES psizam::interpreter
#endif

TEMPLATE_TEST_CASE("wide arithmetic matches 128-bit host arithmetic", "[wide-arithmetic]", WIDE_ENGINES) {
   auto wasm = wide_arithmetic::module_bytes();
   psizam::wasm_allocator memory;
   psizam::backend<std::nullptr_t, TestType, wide_options> vm(wasm, &memory, wide_options{});
   uint64_t mismatches = 0;
   wide_arithmetic::run_all(
      [&](const char* name, uint64_t a, uint64_t b) { return vm.call_with_return("env", name, a, b)->to_ui64(); },
      [&](const char* name, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
         return vm.call_with_return("env", name, a, b, c, d)->to_ui64();
      },
      [&](const char* name) { return vm.call_with_return("env", name)->to_ui64(); },
      [&](uint64_t actual, uint64_t expected, const std::string& name) {
         if (actual != expected && mismatches++ < 8) {
            CAPTURE(name);
            CHECK(actual == expected);
         }
      });
   CHECK(mismatches == 0);
}
