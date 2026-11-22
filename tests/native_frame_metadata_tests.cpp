#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/host_function.hpp>
#include <fstream>
#include <iterator>

namespace {
struct options : psizam::default_options {
   uint32_t max_stack_bytes = 65536;
   uint32_t compile_threads = 0;
   static constexpr uint32_t max_func_local_bytes = 1024*1024;
   static constexpr auto max_func_local_bytes_flags =
      psizam::max_func_local_bytes_flags_t::params |
      psizam::max_func_local_bytes_flags_t::locals |
      psizam::max_func_local_bytes_flags_t::stack;
   psizam::mem_safety memory_mode = psizam::mem_safety::guarded;
   options() { gas_metering = false; }
};
struct traced_jit2 : psizam::jit2 {
   using context = psizam::detail::jit_execution_context<true>;
   static constexpr bool enable_backtrace = true;
};
void tick() {}
using functions = psizam::registered_host_functions<psizam::standalone_function_t>;
std::vector<uint8_t> fixture() {
   std::ifstream stream(NATIVE_FRAME_METADATA_WASM,std::ios::binary);
   REQUIRE(stream.good());
   return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
#if defined(__aarch64__)
constexpr uint64_t native_slot = 16, fixed_linkage = 16;
#else
constexpr uint64_t native_slot = 8, fixed_linkage = 8;
#endif
}

TEMPLATE_TEST_CASE("Native fixed frames exclude incoming and dynamic operands", "[native-frames]",
                   psizam::jit,psizam::jit_profile,psizam::jit2,traced_jit2) {
   functions::add<&tick>("env","tick");
   for (uint32_t threads : {0u,4u}) {
      for (auto mode : {psizam::mem_safety::guarded,psizam::mem_safety::checked}) {
         CAPTURE(threads, static_cast<int>(mode));
         options policy;
         policy.compile_threads = threads;
         policy.memory_mode = mode;
         psizam::wasm_allocator allocator;
         psizam::backend<functions,TestType,options> vm(fixture(),&allocator,policy);
         functions::resolve(vm.get_module());
         auto frame = [&](const char* name) {
            auto& mod = vm.get_module();
            const auto index = mod.get_exported_function(name)-mod.get_imported_functions_size();
            const auto& body = mod.code[index];
            REQUIRE(body.native_stack_frame_complete);
            REQUIRE(body.native_stack_frame_bytes != UINT64_MAX);
            return body.native_stack_frame_bytes;
         };
         const auto bare = frame("bare");
         CHECK(bare >= fixed_linkage);
         CHECK(frame("scalar") == bare + 4*native_slot);
         CHECK(frame("vector") == frame("scalar"));
         CHECK(frame("params") == bare); // incoming argument storage is separate
         if constexpr (std::is_same_v<TestType,psizam::jit> || std::is_same_v<TestType,psizam::jit_profile>) {
            CHECK(bare == fixed_linkage);
            CHECK(frame("spill") == bare); // operand stack is dynamic in JIT1
            CHECK(frame("exception") == bare); // EH helper temporaries are separate
         } else {
            CHECK(frame("spill") > bare); // saved registers and spill slots survive the call
            CHECK(frame("exception") > bare); // dedicated native EH save slots
#if defined(__x86_64__)
            CHECK(bare == (mode == psizam::mem_safety::checked ? 24u : 8u));
#endif
         }
         for (const char* name : {"bare","scalar","vector","exception"})
            CHECK(vm.call_with_return("env",name)->to_ui64() == 1);
         CHECK(vm.call_with_return("env","params",uint64_t{1},uint64_t{2},uint64_t{3},uint64_t{4})->to_ui64() == 1);
         CHECK(vm.call_with_return("env","spill",uint64_t{1},uint64_t{2},uint64_t{3},uint64_t{4},uint64_t{5},uint64_t{6},uint64_t{7},uint64_t{8},uint64_t{9},uint64_t{10},uint64_t{11},uint64_t{12},uint64_t{13},uint64_t{14},uint64_t{15},uint64_t{16},uint64_t{17},uint64_t{18},uint64_t{19},uint64_t{20},uint64_t{21})->to_ui64() == 231);
      }
   }
}
