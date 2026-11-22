#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/host_function.hpp>
#include <psizam/detail/pzam_compile_result.hpp>
#include <fstream>
#include <iterator>
#include <functional>
#include <limits>

namespace {
struct observations {
   uintptr_t low = std::numeric_limits<uintptr_t>::max();
   uintptr_t high = 0;
   uint32_t min_budget = UINT32_MAX;
   uint32_t max_budget = 0;
   uint32_t samples = 0;
   std::function<uint32_t()> remaining;
   std::function<uint64_t()> nested;
};
thread_local observations* active = nullptr;
uint64_t host(uint64_t a, uint32_t b) {
   if (active && active->nested) return a + b + active->nested();
   return a + b;
}
void sample() {
   if (!active) return;
   const auto frame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
   active->low = std::min(active->low, frame);
   active->high = std::max(active->high, frame);
   const auto budget = active->remaining();
   active->min_budget = std::min(active->min_budget, budget);
   active->max_budget = std::max(active->max_budget, budget);
   ++active->samples;
}
using functions = psizam::registered_host_functions<psizam::standalone_function_t>;

struct tail_count_options : psizam::default_options {
   uint32_t max_call_depth = 4;
   tail_count_options() { gas_metering = false; }
};
struct tail_byte_options : psizam::default_options {
   uint32_t max_stack_bytes = 65536;
   uint32_t compile_threads = 0;
   static constexpr uint32_t max_func_local_bytes = 1024 * 1024;
   static constexpr uint32_t stack_usage_for_call = 4096;
   static constexpr auto max_func_local_bytes_flags =
      psizam::max_func_local_bytes_flags_t::params |
      psizam::max_func_local_bytes_flags_t::locals |
      psizam::max_func_local_bytes_flags_t::stack;
   tail_byte_options() { gas_metering = false; }
};
struct traced_tail_byte_options : tail_byte_options {};

std::vector<uint8_t> fixture(const char* path = PROPER_TAIL_WASM) {
   std::ifstream in(path, std::ios::binary);
   REQUIRE(in.good());
   return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

template<class Impl>
std::vector<uint32_t> logical_costs(uint32_t threads) {
   tail_byte_options options;
   options.compile_threads = threads;
   psizam::wasm_allocator allocator;
   psizam::backend<functions, Impl, tail_byte_options> vm(fixture(), &allocator, options);
   std::vector<uint32_t> result;
   for (const auto& body : vm.get_module().code) result.push_back(body.byte_stack_usage);
   return result;
}

template<class Writer>
void verify_import_metadata(uint32_t threads, bool backtrace) {
   auto wasm = fixture();
   psizam::module mod;
   mod.allocator.use_default_memory();
   tail_byte_options options;
   options.compile_threads = threads;
   psizam::detail::pzam_compile_result output;
   psizam::detail::null_debug_info debug;
   psizam::detail::binary_parser<Writer, tail_byte_options> parser(mod.allocator, options, backtrace, true);
   parser.set_compile_result(&output);
   parser.parse_module(wasm, mod, debug);
   mod.finalize();
   REQUIRE(output.error.empty());
   CHECK(output.native_guest_abi == mod.native_guest_abi);
   const auto count = mod.get_imported_functions_size();
   REQUIRE(output.import_thunk_offsets.size() == count);
   REQUIRE(output.import_thunk_stack_sizes.size() == count);
   REQUIRE(mod.native_function_ptrs.size() == mod.get_functions_total());
   for (uint32_t i = 0; i < count; ++i) {
      const auto [offset, size] = output.import_thunk_offsets[i];
      CHECK(size > 0);
      CHECK(uint64_t(offset) + size <= mod.allocator.get_actual_code_size());
      CHECK(mod.native_function_ptrs[i] == mod.allocator._code_base + offset);
#if defined(__aarch64__)
      CHECK(output.import_thunk_stack_sizes[i] >=
         32 + ((uint64_t(mod.get_function_type(i).param_types.size()) * 8 + 15) & ~uint64_t{15}));
#else
      CHECK(output.import_thunk_stack_sizes[i] >= (backtrace ? 48 : 40));
#endif
   }
}

TEMPLATE_TEST_CASE("AArch64 JIT1 proper tails replace signatures without native or logical growth",
                   "[proper-tail][resource]", tail_count_options, tail_byte_options, traced_tail_byte_options) {
   functions::add<&host>("env", "host");
   functions::add<&sample>("env", "sample");
   psizam::wasm_allocator allocator;
   using impl = std::conditional_t<std::is_same_v<TestType, traced_tail_byte_options>, psizam::jit_profile, psizam::jit>;
   psizam::backend<functions, impl, TestType> vm(fixture(), &allocator, TestType{});
   functions::resolve(vm.get_module());
   CHECK(vm.get_module().native_guest_abi == psizam::native_guest_abi::jit_callee_cleanup);
   observations observed;
   observed.remaining = [&] { return vm.get_context().get_remaining_call_depth(); };
   struct scope {
      scope(observations& v) { active = &v; }
      ~scope() { active = nullptr; }
   } owner(observed);
   for (const char* name : {"cycle", "cycle_indirect"}) {
      for (uint32_t count : {0u, 100u, 100000u}) {
         observed.low = UINTPTR_MAX; observed.high = 0;
         observed.samples = 0; observed.min_budget = UINT32_MAX; observed.max_budget = 0;
         CHECK(vm.call_with_return("env", name, count)->to_ui64() == UINT64_C(0x123456789abce54));
         CHECK(observed.samples == count + 1);
         CHECK(observed.high - observed.low <= 64);
         CHECK(observed.min_budget == observed.max_budget);
      }
   }
   CHECK(vm.call_with_return("env", "host_tail", 23u)->to_ui64() == 1023);
   CHECK(vm.call_with_return("env", "host_indirect_tail", 23u)->to_ui64() == 1023);
   observed.nested = [&] { return vm.call_with_return("env", "cycle", 10000u)->to_ui64(); };
   CHECK(vm.call_with_return("env", "host_tail", 23u)->to_ui64() == UINT64_C(0x123456789abd253));
   CHECK(vm.call_with_return("env", "host_indirect_tail", 23u)->to_ui64() == UINT64_C(0x123456789abd253));
   observed.nested = {};
   CHECK(vm.call_with_return("env", "vector_tail", 10000u)->to_ui64() == UINT64_MAX);
   CHECK(vm.call_with_return("env", "multi_tail", 10000u)->to_ui64() == UINT64_C(0xfedcba9876543207));
   observed.nested = []() -> uint64_t { throw std::runtime_error("intentional tail host failure"); };
   CHECK_THROWS_AS(vm.call_with_return("env", "host_tail", 23u), std::runtime_error);
   observed.nested = {};
   for (uint32_t invalid : {1u, 5u, 6u})
      CHECK_THROWS(vm.call_with_return("env", "indirect_bad", invalid));
   vm.call("env", "set_fail", 1u);
   CHECK_THROWS(vm.call_with_return("env", "cycle", 10000u));
   vm.call("env", "set_fail", 0u);
   CHECK(vm.call_with_return("env", "cycle", 0u)->to_ui64() == UINT64_C(0x123456789abce54));
}
}

TEST_CASE("Parser publishes identical logical byte costs for native admission", "[proper-tail][byte-metadata]") {
   functions::add<&host>("env", "host");
   functions::add<&sample>("env", "sample");
   const auto reference = logical_costs<psizam::interpreter>(0);
   REQUIRE(!reference.empty());
   for (uint32_t cost : reference) CHECK(cost >= 16);
   CHECK(logical_costs<psizam::jit>(0) == reference);
   CHECK(logical_costs<psizam::jit2>(0) == reference);
   CHECK(logical_costs<psizam::jit2>(2) == reference);
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   CHECK(logical_costs<psizam::jit_llvm>(0) == reference);
   CHECK(logical_costs<psizam::jit_llvm>(2) == reference);
#endif
}

TEST_CASE("Native import target metadata follows actual serial and merged code", "[proper-tail][import-metadata]") {
   verify_import_metadata<psizam::detail::machine_code_writer>(0, false);
   verify_import_metadata<psizam::detail::machine_code_writer>(0, true);
   verify_import_metadata<psizam::detail::ir_writer>(0, false);
   verify_import_metadata<psizam::detail::ir_writer>(2, true);
}

TEMPLATE_TEST_CASE("Large imports retain every argument across ordinary and tail calls", "[proper-tail][large-import]",
                   psizam::interpreter, psizam::jit, psizam::jit2) {
   uint32_t calls = 0;
   psizam::host_function_table table;
   psizam::host_function_table::entry entry;
   entry.module_name = "env"; entry.func_name = "large";
   entry.signature.params.assign(2000, psizam::types::i64);
   entry.signature.ret = {psizam::types::i64};
   entry.slow_dispatch = [&](void*, psizam::native_value* args, char*) {
      for (uint32_t i = 0; i < 2000; ++i)
         if (args[i].i64 != UINT64_C(0x10203040) + i)
            throw std::runtime_error("large import argument mismatch");
      ++calls;
      return psizam::native_value{UINT64_C(0x123456789abcdef)};
   };
   table.add_entry(std::move(entry));
   auto wasm = fixture(PROPER_TAIL_IMPORT_WASM);
   psizam::wasm_allocator allocator;
   psizam::backend<std::nullptr_t, TestType, tail_byte_options> vm(wasm, std::move(table), nullptr, &allocator);
   CHECK(vm.call_with_return("env", "ordinary_host")->to_ui64() == UINT64_C(0x123456789abce54));
   CHECK(vm.call_with_return("env", "tail_host")->to_ui64() == UINT64_C(0x123456789abcdef));
   CHECK(calls == 2);
}

TEST_CASE("AArch64 JIT1 tail and ordinary cleanup handle large argument areas", "[proper-tail][large-args]") {
   functions::add<&host>("env", "host");
   functions::add<&sample>("env", "sample");
   psizam::wasm_allocator allocator;
   psizam::backend<functions, psizam::jit, tail_byte_options> vm(fixture(PROPER_TAIL_LARGE_WASM), &allocator);
   functions::resolve(vm.get_module());
   CHECK(vm.call_with_return("env", "ordinary_large")->to_ui64() == UINT64_C(0x123456789abce54));
   CHECK(vm.call_with_return("env", "cycle_large", 10000u)->to_ui64() == UINT64_C(0x123456789abce54));
   CHECK(vm.call_with_return("env", "ordinary_large")->to_ui64() == UINT64_C(0x123456789abce54));
}

TEST_CASE("AArch64 JIT1 proper tails honor the params-counted byte boundary", "[proper-tail][byte-boundary]") {
   functions::add<&host>("env", "host");
   functions::add<&sample>("env", "sample");
   psizam::wasm_allocator allocator;
   psizam::backend<functions, psizam::jit, tail_byte_options> vm(fixture(), &allocator);
   functions::resolve(vm.get_module());
   // Interpreter metadata is the existing independent logical-cost oracle.
   // Native JIT1 does not currently retain these costs in function_body.
   psizam::wasm_allocator reference_allocator;
   psizam::backend<functions, psizam::interpreter, tail_byte_options> reference(fixture(), &reference_allocator);
   auto& mod = reference.get_module();
   auto cost = [&](const char* name) { return mod.get_function_stack_size(mod.get_exported_function(name)); };
   const uint32_t minimum = cost("cycle") + std::max(cost("small"), cost("large"));
   REQUIRE(minimum > 0);
   tail_byte_options options;
   options.max_stack_bytes = minimum;
   vm.initialize(nullptr, options);
   CHECK(vm.call_with_return("env", "cycle", 10000u)->to_ui64() == UINT64_C(0x123456789abce54));
   CHECK(vm.get_context().get_remaining_call_depth() == minimum);
   options.max_stack_bytes = minimum - 1;
   vm.initialize(nullptr, options);
   CHECK_THROWS_AS(vm.call_with_return("env", "cycle", 1u), psizam::wasm_interpreter_exception);
   options.max_stack_bytes = minimum;
   vm.initialize(nullptr, options);
   CHECK(vm.call_with_return("env", "cycle", 1u)->to_ui64() == UINT64_C(0x123456789abce54));
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

TEMPLATE_TEST_CASE("Shared funcref resolution preserves dynamic tail targets", "[proper-tail][table]",
                   psizam::interpreter, psizam::jit, psizam::jit2) {
   functions::add<&host>("env", "host");
   functions::add<&sample>("env", "sample");
   psizam::wasm_allocator allocator;
   psizam::backend<functions, TestType> vm(fixture(), &allocator);
   functions::resolve(vm.get_module());
   for (const char* name : {"set_tail", "fill_tail", "grow_tail", "set_secondary_tail"})
      CHECK(vm.call_with_return("env", name, 0u)->to_ui64() == UINT64_C(0x123456789abcdef));
   CHECK(vm.call_with_return("env", "set_import_tail", 23u)->to_ui64() == 1023);
   CHECK(vm.call_with_return("env", "grow_import_tail", 23u)->to_ui64() == 1023);
   CHECK(vm.call_with_return("env", "ordinary")->to_ui64() == 3155);
}
