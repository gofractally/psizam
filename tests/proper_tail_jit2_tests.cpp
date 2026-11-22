#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/host_function.hpp>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>

namespace {
struct count_options : psizam::default_options {
   uint32_t max_call_depth = 4;
   uint32_t compile_threads = 0;
   count_options() { gas_metering = false; }
};
struct byte_options : psizam::default_options {
   uint32_t max_stack_bytes = 65536;
   uint32_t compile_threads = 0;
   static constexpr uint32_t max_func_local_bytes = 1024 * 1024;
   static constexpr uint32_t stack_usage_for_call = 4096;
   static constexpr auto max_func_local_bytes_flags =
      psizam::max_func_local_bytes_flags_t::params |
      psizam::max_func_local_bytes_flags_t::locals |
      psizam::max_func_local_bytes_flags_t::stack;
   byte_options() { gas_metering = false; }
};
struct traced_options : byte_options {};
struct parallel_options : byte_options { parallel_options() { compile_threads = 4; } };
struct traced_jit2 : psizam::jit2 {
   using context = psizam::detail::jit_execution_context<true>;
   static constexpr bool enable_backtrace = true;
};
template<class Options>
using impl_for = std::conditional_t<std::is_same_v<Options,traced_options>,traced_jit2,psizam::jit2>;

struct observations {
   uintptr_t low = UINTPTR_MAX, high = 0;
   uint32_t min_budget = UINT32_MAX, max_budget = 0, samples = 0;
   std::function<uint32_t()> remaining;
   std::function<uint64_t()> nested;
   void reset() { low = UINTPTR_MAX; high = 0; min_budget = UINT32_MAX; max_budget = samples = 0; }
};
thread_local observations* active = nullptr;
struct observe_scope {
   observations* previous = active;
   explicit observe_scope(observations& value) { active = &value; }
   ~observe_scope() { active = previous; }
};
uint64_t host(uint64_t a, uint32_t b) {
   return a + b + (active && active->nested ? active->nested() : 0);
}
void sample() {
   if (!active) return;
   auto frame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
   active->low = std::min(active->low,frame);
   active->high = std::max(active->high,frame);
   auto budget = active->remaining();
   active->min_budget = std::min(active->min_budget,budget);
   active->max_budget = std::max(active->max_budget,budget);
   ++active->samples;
}
using functions = psizam::registered_host_functions<psizam::standalone_function_t>;
void register_functions() {
   functions::add<&host>("env","host");
   functions::add<&sample>("env","sample");
}
std::vector<uint8_t> fixture(const char* path) {
   std::ifstream stream(path,std::ios::binary);
   REQUIRE(stream.good());
   return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
}

TEMPLATE_TEST_CASE("JIT2 proper tails replace complete activations", "[proper-tail][jit2][resource]",
                   count_options, byte_options, traced_options, parallel_options) {
   register_functions();
   psizam::wasm_allocator allocator;
   psizam::backend<functions,impl_for<TestType>,TestType> vm(fixture(PROPER_TAIL_WASM),&allocator,TestType{});
   functions::resolve(vm.get_module());
   CHECK(vm.get_module().native_guest_abi == psizam::native_guest_abi::jit_callee_cleanup);
   observations seen;
   seen.remaining = [&] { return vm.get_context().get_remaining_call_depth(); };
   observe_scope scope(seen);
   for (auto name : {"cycle","cycle_indirect"}) {
      for (auto iterations : {0u,100u,100000u}) {
         seen.reset();
         CHECK(vm.call_with_return("env",name,iterations)->to_ui64() == UINT64_C(0x123456789abce54));
         CHECK(seen.samples == iterations + 1);
         CHECK(seen.high - seen.low <= 64);
         CHECK(seen.min_budget == seen.max_budget);
      }
   }
   for (auto name : {"host_tail","host_indirect_tail","set_import_tail","grow_import_tail"})
      CHECK(vm.call_with_return("env",name,23u)->to_ui64() == 1023);
   seen.nested = [&] { return vm.call_with_return("env","cycle",10000u)->to_ui64(); };
   CHECK(vm.call_with_return("env","host_tail",23u)->to_ui64() == UINT64_C(0x123456789abd253));
   CHECK(vm.call_with_return("env","host_indirect_tail",23u)->to_ui64() == UINT64_C(0x123456789abd253));
   seen.nested = {};
   CHECK(vm.call_with_return("env","vector_tail",10000u)->to_ui64() == UINT64_MAX);
   CHECK(vm.call_with_return("env","multi_tail",10000u)->to_ui64() == UINT64_C(0xfedcba9876543207));
   for (auto name : {"set_tail","fill_tail","grow_tail","set_secondary_tail"})
      CHECK(vm.call_with_return("env",name,10000u)->to_ui64() == UINT64_C(0x123456789abcdef));
   seen.nested = []() -> uint64_t { throw std::runtime_error("tail host failure"); };
   CHECK_THROWS_AS(vm.call_with_return("env","host_tail",23u),std::runtime_error);
   seen.nested = {};
   for (auto index : {1u,2u,65535u}) CHECK_THROWS(vm.call_with_return("env","indirect_bad",index));
   vm.call("env","set_fail",1u);
   CHECK_THROWS(vm.call_with_return("env","cycle",10000u));
   vm.call("env","set_fail",0u);
   CHECK(vm.call_with_return("env","ordinary")->to_ui64() == 3155);
}

TEMPLATE_TEST_CASE("JIT2 logical byte boundary matches the interpreter", "[proper-tail][jit2][byte-boundary]",
                   byte_options, traced_options, parallel_options) {
   register_functions();
   psizam::wasm_allocator reference_allocator;
   psizam::backend<functions,psizam::interpreter,TestType> reference(fixture(PROPER_TAIL_WASM),&reference_allocator,TestType{});
   functions::resolve(reference.get_module());
   auto& mod = reference.get_module();
   auto cost = [&](const char* name) { return mod.get_function_stack_size(mod.get_exported_function(name)); };
   const auto minimum = cost("cycle") + std::max(cost("small"),cost("large"));
   REQUIRE(minimum > 0);
   auto options = TestType{};
   options.max_stack_bytes = minimum;
   psizam::wasm_allocator exact_allocator;
   psizam::backend<functions,impl_for<TestType>,TestType> exact(fixture(PROPER_TAIL_WASM),&exact_allocator,options);
   functions::resolve(exact.get_module());
   CHECK(exact.call_with_return("env","cycle",10000u)->to_ui64() == UINT64_C(0x123456789abce54));
   CHECK(exact.get_context().get_remaining_call_depth() == minimum);
   --options.max_stack_bytes;
   psizam::wasm_allocator short_allocator;
   psizam::backend<functions,impl_for<TestType>,TestType> short_vm(fixture(PROPER_TAIL_WASM),&short_allocator,options);
   functions::resolve(short_vm.get_module());
   CHECK_THROWS_AS(short_vm.call("env","cycle",1u),psizam::wasm_interpreter_exception);
   CHECK(exact.call_with_return("env","cycle",1u)->to_ui64() == UINT64_C(0x123456789abce54));
}

TEMPLATE_TEST_CASE("JIT2 returns preserve channels and ordinary caller budgets", "[proper-tail][jit2][returns]",
                   count_options,byte_options,traced_options,parallel_options) {
   register_functions();
   psizam::wasm_allocator allocator;
   psizam::backend<functions,impl_for<TestType>,TestType> vm(fixture(PROPER_TAIL_RETURNS_WASM),&allocator,TestType{});
   functions::resolve(vm.get_module());
   observations seen;
   seen.remaining = [&] { return vm.get_context().get_remaining_call_depth(); };
   observe_scope scope(seen);
   for (unsigned iteration = 0; iteration < 3; ++iteration) {
      seen.reset();
      CHECK(vm.call_with_return("env","observe",23u)->to_ui64() == UINT64_C(0x123456789abce6b));
      CHECK(seen.samples == 3);
      CHECK(seen.min_budget == seen.max_budget);
      CHECK(vm.call_with_return("env","vector",23u)->to_ui64() == UINT64_C(0xfedcba9876543210));
      CHECK(vm.call_with_return("env","indirect_vector",23u)->to_ui64() == UINT64_C(0xfedcba9876543210));
      CHECK(vm.call_with_return("env","multi",23u)->to_ui64() == UINT64_C(0xfedcba9876543207));
      CHECK(vm.call_with_return("env","then_tail",23u)->to_ui64() == 1023);
      CHECK_THROWS(vm.call("env","early_trap"));
      seen.reset();
      CHECK(vm.call_with_return("env","tail_scope")->to_ui64() == 7);
      CHECK(seen.samples == 2);
      CHECK(seen.min_budget == seen.max_budget);
   }
}

TEMPLATE_TEST_CASE("JIT2 large tail signatures preserve ordinary continuations", "[proper-tail][jit2][large-args]",
                   byte_options,traced_options,parallel_options) {
   register_functions();
   psizam::wasm_allocator allocator;
   psizam::backend<functions,impl_for<TestType>,TestType> vm(fixture(PROPER_TAIL_LARGE_WASM),&allocator,TestType{});
   functions::resolve(vm.get_module());
   CHECK(vm.call_with_return("env","cycle_large",10000u)->to_ui64() == UINT64_C(0x123456789abce54));
   CHECK(vm.call_with_return("env","ordinary_large")->to_ui64() == UINT64_C(0x123456789abce54));
}

namespace {
void append_uleb(std::vector<uint8_t>& out, uint32_t value) {
   do {
      const auto byte = uint8_t(value & 0x7f);
      value >>= 7;
      out.push_back(byte | (value ? 0x80 : 0));
   } while (value);
}
void append_sleb(std::vector<uint8_t>& out, int64_t value) {
   bool more;
   do {
      const auto byte = uint8_t(value & 0x7f);
      value >>= 7;
      more = !((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40)));
      out.push_back(byte | (more ? 0x80 : 0));
   } while (more);
}
std::vector<uint8_t> wide_cleanup_fixture() {
   // 8201 i64 parameters require 65608 x86 bytes, beyond RET imm16. Also
   // exercises larger AArch64 native slots without an exported huge ABI.
   constexpr uint32_t count = 8201;
   std::vector<uint8_t> wasm{0,97,115,109,1,0,0,0};
   auto section = [&](uint8_t id, const std::vector<uint8_t>& bytes) {
      wasm.push_back(id); append_uleb(wasm,bytes.size());
      wasm.insert(wasm.end(),bytes.begin(),bytes.end());
   };
   std::vector<uint8_t> types{2,0x60,0,1,0x7e,0x60};
   append_uleb(types,count);
   types.insert(types.end(),count,0x7e);
   types.insert(types.end(),{1,0x7e});
   section(1,types);
   section(3,{4,0,1,0,0});
   section(7,{2,3,'r','u','n',0,0,6,'n','o','r','m','a','l',0,3});
   std::vector<uint8_t> bodies{4};
   auto body = [&](const std::vector<uint8_t>& bytes) {
      append_uleb(bodies,bytes.size());bodies.insert(bodies.end(),bytes.begin(),bytes.end());
   };
   body({0,0x10,2,0x42,0xe5,0,0x7c,0x0b});
   std::vector<uint8_t> leaf{0,0x20};
   append_uleb(leaf,count-1);
   leaf.insert(leaf.end(),{0x0f,0x0b});
   body(leaf);
   std::vector<uint8_t> args{0};
   for (uint32_t i=0;i<count;++i) {
      args.push_back(0x42);
      append_sleb(args,i+1==count ? INT64_C(0x123456789abcdef) : 0);
   }
   auto tail=args;
   tail.insert(tail.end(),{0x12,1,0x0b});
   body(tail);
   args.insert(args.end(),{0x10,1,0x42,0xe5,0,0x7c,0x0b});
   body(args);
   section(10,bodies);
   return wasm;
}
struct wide_options : byte_options { wide_options() { max_stack_bytes = 2*1024*1024; } };
}
TEMPLATE_TEST_CASE("Private cleanup exceeds RET imm16 without changing public ABI", "[proper-tail][wide-cleanup]",
                   psizam::jit,psizam::jit_profile,psizam::jit2,traced_jit2) {
   auto wasm = wide_cleanup_fixture();
   psizam::wasm_allocator allocator;
   psizam::backend<std::nullptr_t,TestType,wide_options> vm(wasm,&allocator,wide_options{});
   for (auto name : {"run","normal"})
      for (unsigned i=0;i<3;++i)
         CHECK(vm.call_with_return("env",name)->to_ui64() == UINT64_C(0x123456789abce54));
}
