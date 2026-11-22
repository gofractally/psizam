#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <fstream>
#include <iterator>

namespace {
struct options : psizam::default_options {
   uint32_t max_stack_bytes = 65536;
   uint32_t compile_threads = 0;
   static constexpr uint32_t max_func_local_bytes = 1024 * 1024;
   static constexpr auto max_func_local_bytes_flags =
      psizam::max_func_local_bytes_flags_t::params |
      psizam::max_func_local_bytes_flags_t::locals |
      psizam::max_func_local_bytes_flags_t::stack;
   psizam::mem_safety memory_mode = psizam::mem_safety::guarded;
   psizam::checked_mode checked_kind = psizam::checked_mode::strict;
   psizam::fp_mode fp = psizam::fp_mode::softfloat;
   options() { gas_metering = false; }
};
struct traced_jit2 : psizam::jit2 {
   using context = psizam::detail::jit_execution_context<true>;
   static constexpr bool enable_backtrace = true;
};
std::vector<uint8_t> fixture() {
   std::ifstream stream(JIT_WORKER_POLICY_WASM, std::ios::binary);
   REQUIRE(stream.good());
   return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
constexpr uint32_t f32_payload = 0x7fc01234;
constexpr uint64_t f64_payload = 0x7ff8000012345678;
constexpr uint32_t f32_canonical = 0x7fc00000;
constexpr uint64_t f64_canonical = 0x7ff8000000000000;
}

TEMPLATE_TEST_CASE("Parallel JIT workers honor floating-point policy", "[jit2][worker-policy]",
                   psizam::jit2, traced_jit2) {
   for (auto fp : {psizam::fp_mode::fast, psizam::fp_mode::hw_deterministic,
                   psizam::fp_mode::softfloat}) {
      uint32_t serial32 = 0;
      uint64_t serial64 = 0;
      for (uint32_t threads : {0u, 4u}) {
         CAPTURE(threads, static_cast<int>(fp));
         options policy;
         policy.compile_threads = threads;
         policy.fp = fp;
         psizam::wasm_allocator allocator;
         psizam::backend<std::nullptr_t, TestType, options> vm(fixture(), &allocator, policy);
         const auto bits32 = vm.call_with_return("env", "f32_bits", f32_payload)->to_ui32();
         const auto bits64 = vm.call_with_return("env", "f64_bits", f64_payload)->to_ui64();
         if (!threads) { serial32 = bits32; serial64 = bits64; }
         CHECK(bits32 == serial32);
         CHECK(bits64 == serial64);
         if (fp != psizam::fp_mode::fast) {
            CHECK(bits32 == f32_canonical);
            CHECK(bits64 == f64_canonical);
         }
         CHECK(vm.call_with_return("env", "neighbor")->to_ui32() == 7);
      }
   }
}

TEMPLATE_TEST_CASE("Parallel JIT workers honor memory policy", "[jit2][worker-policy]",
                   psizam::jit2, traced_jit2) {
   for (auto mode : {psizam::mem_safety::guarded, psizam::mem_safety::checked,
                     psizam::mem_safety::memory16}) {
      for (auto kind : {psizam::checked_mode::strict, psizam::checked_mode::relaxed,
                       psizam::checked_mode::immediate}) {
         for (uint32_t threads : {0u, 4u}) {
            CAPTURE(threads, static_cast<int>(mode), static_cast<int>(kind));
            options policy;
            policy.compile_threads = threads;
            policy.memory_mode = mode;
            policy.checked_kind = kind;
            psizam::wasm_allocator allocator;
            psizam::backend<std::nullptr_t, TestType, options> vm(fixture(), &allocator, policy);
            CHECK(vm.call_with_return("env", "load", uint32_t{0})->to_ui32() == 42);
            if (mode == psizam::mem_safety::memory16) {
               CHECK(vm.call_with_return("env", "load", uint32_t{65536})->to_ui32() == 42);
            } else {
               CHECK_THROWS_AS(vm.call_with_return("env", "load", uint32_t{65536}), psizam::exception);
            }
            CHECK(vm.call_with_return("env", "load", uint32_t{0})->to_ui32() == 42);
         }
      }
   }
}

TEST_CASE("Parallel JIT compilation preserves explicit artifact FP policy", "[jit2][worker-policy]") {
   for (bool software : {false, true}) {
      uint32_t serial32 = 0;
      uint64_t serial64 = 0;
      for (uint32_t threads : {0u, 4u}) {
         CAPTURE(threads, software);
         auto wasm = fixture();
         psizam::module mod;
         mod.allocator.use_default_memory();
         options policy;
         policy.compile_threads = threads;
         // Explicit artifact policy takes precedence over the parser option.
         policy.fp = software ? psizam::fp_mode::fast : psizam::fp_mode::softfloat;
         psizam::detail::pzam_compile_result output;
         output.softfloat = software;
         psizam::detail::null_debug_info debug;
         psizam::detail::binary_parser<psizam::detail::ir_writer, options>
            parser(mod.allocator, policy, false, true);
         parser.set_compile_result(&output);
         parser.parse_module(wasm, mod, debug);
         mod.finalize();
         REQUIRE(output.error.empty());
         psizam::wasm_allocator allocator;
         psizam::detail::jit_execution_context<> context(mod, 65536);
         context.set_wasm_allocator(&allocator);
         context.reset();
         const auto bits32 = context.execute(nullptr, psizam::detail::jit_visitor{nullptr},
            mod.get_exported_function("f32_bits"), f32_payload)->to_ui32();
         const auto bits64 = context.execute(nullptr, psizam::detail::jit_visitor{nullptr},
            mod.get_exported_function("f64_bits"), f64_payload)->to_ui64();
         if (!threads) { serial32 = bits32; serial64 = bits64; }
         CHECK(bits32 == serial32);
         CHECK(bits64 == serial64);
         if (software) {
            CHECK(bits32 == f32_canonical);
            CHECK(bits64 == f64_canonical);
         }
      }
   }
}
