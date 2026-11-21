#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/pzam_compile.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include <psizam/detail/jit_optimize.hpp>
#include <psizam/detail/jit_regalloc.hpp>
#include <cstring>
#include <limits>
#include <array>

using namespace psizam;
namespace {
struct policy : default_options {
   uint32_t max_pages = 2;
   uint32_t max_call_depth = 512;
   static constexpr fp_mode fp = fp_mode::softfloat;
   policy() { gas_metering = false; }
};

template<class Options, class Debug, pzam_tier_flags Tier>
struct saved_parser {
   saved_parser(growable_allocator&, const Options&, bool, bool) {}
   module& parse_module(wasm_code& wasm, module& mod, Debug&) {
      auto bytes = pzam_compile(wasm, {.tier=Tier,
         .flags=Tier == pzam_tier_flags::llvm_O2 ? pzam_compile_flags::softfloat : pzam_compile_flags::none});
      auto loaded = detail::load_pzam(pzam_load(bytes));
      std::shared_ptr<void> owner(loaded.exec_code, [](void* p) { jit_allocator::instance().free(p); });
      mod.allocator.release_base_memory();
      mod = std::move(loaded.mod);
      loaded.mod.allocator = growable_allocator{};
      mod.jit_engine = std::move(owner);
      return mod;
   }
};
template<pzam_tier_flags Tier>
struct saved_impl {
   using context = detail::jit_execution_context<false>;
   template<class Options, class Debug> using parser = saved_parser<Options, Debug, Tier>;
   static constexpr bool is_jit = true, enable_backtrace = false, reverse_host_args = false;
};
using saved_jit1 = saved_impl<pzam_tier_flags::jit1>;
using saved_jit2 = saved_impl<pzam_tier_flags::jit2>;
using saved_aot = saved_impl<pzam_tier_flags::llvm_O2>;

template<class Impl, class T>
void exercise(const char* path) {
   auto code = read_wasm(path);
   wasm_allocator memory;
   backend<std::nullptr_t, Impl, policy> vm(code, &memory, policy{});
   auto* bytes = vm.get_context().linear_memory();
   auto load = [&](size_t width, uint32_t address) {
      auto result = vm.call_with_return("env", "load-" + std::to_string(width), address);
      REQUIRE(result);
      if constexpr (sizeof(T)==4) return T(result->to_ui32());
      else return T(result->to_ui64());
   };
   auto rmw = [&](const std::string& name, uint32_t address, T value, T replacement = 0) {
      auto result = name.starts_with("cmpxchg")
         ? vm.call_with_return("env", name, address, value, replacement)
         : vm.call_with_return("env", name, address, value);
      REQUIRE(result);
      if constexpr (sizeof(T)==4) return T(result->to_ui32());
      else return T(result->to_ui64());
   };
   const auto widths = sizeof(T)==4 ? std::vector<size_t>{1,2,4} : std::vector<size_t>{1,2,4,8};
   for (auto width : widths) {
      CAPTURE(width, sizeof(T));
      const T mask = std::numeric_limits<T>::max() >> ((sizeof(T)-width)*8);
      auto reset = [&](T value) { std::memset(bytes+16, 0x5a, 32); std::memcpy(bytes+24, &value, width); };
      reset(mask);
      CHECK(load(width,24) == mask);
      vm("env", "store-"+std::to_string(width), uint32_t{24}, T{0x1234});
      CHECK(load(width,24) == (T{0x1234}&mask));
      CHECK(uint8_t(bytes[23]) == 0x5a);
      CHECK(uint8_t(bytes[24+width]) == 0x5a);
      for (auto op : {"add","sub","and","or","xor","xchg"}) {
         CAPTURE(op);
         const T before = mask;
         reset(before);
         const T value = 3;
         CHECK(rmw(std::string(op)+"-"+std::to_string(width),24,value) == before);
         T expected = 0;
         if (std::string_view(op)=="add") expected = T(before+value);
         if (std::string_view(op)=="sub") expected = T(before-value);
         if (std::string_view(op)=="and") expected = before&value;
         if (std::string_view(op)=="or") expected = before|value;
         if (std::string_view(op)=="xor") expected = before^value;
         if (std::string_view(op)=="xchg") expected = value;
         CHECK(load(width,24) == (expected&mask));
         CHECK(uint8_t(bytes[23]) == 0x5a);
         CHECK(uint8_t(bytes[24+width]) == 0x5a);
      }
      reset(mask);
      auto name = "cmpxchg-"+std::to_string(width);
      CHECK(rmw(name,24,T(mask-1),7) == mask);
      CHECK(load(width,24) == mask);
      CHECK(rmw(name,24,mask,7) == mask);
      CHECK(load(width,24) == 7);
      if (width < sizeof(T)) {
         reset(mask);
         // Both expected and replacement are wrapped to the access width.
         CHECK(rmw(name,24,T(mask | (T{1} << (width*8))),T{0x1234}) == mask);
         CHECK(load(width,24) == (T{0x1234}&mask));
      }
      std::array<char,64> snapshot;
      std::memcpy(snapshot.data(), bytes, snapshot.size());
      if (width>1) {
         CHECK_THROWS_AS(load(width,25), wasm_memory_exception);
         CHECK_THROWS_AS(vm("env", "store-"+std::to_string(width), uint32_t{25}, T{1}), wasm_memory_exception);
         CHECK_THROWS_AS(rmw("add-"+std::to_string(width),25,1), wasm_memory_exception);
      }
      CHECK_THROWS_AS(load(width,65536), wasm_memory_exception);
      CHECK_THROWS_AS(vm("env", "store-"+std::to_string(width),uint32_t{65536},T{1}), wasm_memory_exception);
      CHECK_THROWS_AS(rmw("add-"+std::to_string(width),65536,1), wasm_memory_exception);
      CHECK(std::memcmp(snapshot.data(), bytes, snapshot.size()) == 0);
   }
   CHECK(vm.call_with_return("env","fence")->to_ui32() == 7);
   CHECK(vm.call_with_return("env","notify",uint32_t{0},uint32_t{9})->to_ui32() == 0);
   CHECK_THROWS_AS(vm.call_with_return("env","notify",uint32_t{1},uint32_t{0}), wasm_memory_exception);
   CHECK_THROWS_AS(vm.call_with_return("env","notify",uint32_t{65536},uint32_t{0}), wasm_memory_exception);
   for (T expected : {T{0}, T{1}})
      CHECK_THROWS_AS(vm.call_with_return("env","wait",uint32_t{0},expected,uint64_t{0}), wasm_interpreter_exception);
   CHECK_THROWS_AS(vm.call_with_return("env","overflow"), wasm_memory_exception);
   CHECK_THROWS_AS(vm("env","unused-load",uint32_t{65536}), wasm_memory_exception);
   CHECK_THROWS_AS(vm("env","unused-load",uint32_t{25}), wasm_memory_exception);
   vm("env","store-"+std::to_string(sizeof(T)),uint32_t{24},T{9});
   CHECK(rmw("computed",24,T{7}) == 9);
   CHECK(load(sizeof(T),24) == 16);
   for (auto name : {"odd-stack-load", "odd-stack-rmw", "odd-stack-store"}) {
      auto value = vm.call_with_return("env",name,uint32_t{24});
      REQUIRE(value);
      const auto expected = std::string_view(name)=="odd-stack-store" ? 5u : 21u;
      if constexpr (sizeof(T)==4) CHECK(value->to_ui32()==expected);
      else CHECK(value->to_ui64()==expected);
   }
   auto offset_result = vm.call_with_return("env","offset-load",uint32_t{23});
   REQUIRE(offset_result);
   if constexpr (sizeof(T)==4) CHECK(offset_result->to_ui32()==16);
   else CHECK(offset_result->to_ui64()==16);
   CHECK_THROWS_AS(vm.call_with_return("env","offset-load",uint32_t{24}), wasm_memory_exception);
   for (auto name : {"spill-pressure", "store-pressure"}) {
      auto result = vm.call_with_return("env",name,uint32_t{24},
         T{1},T{2},T{3},T{4},T{5},T{6},T{7},T{8},T{9},T{10},T{11},T{12},T{13},T{14},T{15},T{16},
         T{17},T{18},T{19},T{20},T{21},T{22},T{23},T{24},T{25},T{26},T{27},T{28},T{29},T{30},T{31},T{32});
      REQUIRE(result);
      if constexpr (sizeof(T)==4) CHECK(result->to_ui32()==528);
      else CHECK(result->to_ui64()==528);
   }
   CHECK(vm.call_with_return("env","grow",uint32_t{1})->to_ui32() == 1);
   vm("env", "store-"+std::to_string(sizeof(T)),uint32_t{65536},T{42});
   CHECK(load(sizeof(T),65536) == 42);
   auto result = vm.call_with_return("env","pressure",uint32_t{24},T{1},T{2},T{3},T{4},T{5},T{6},T{7},T{8});
   REQUIRE(result);
   if constexpr (sizeof(T)==4) CHECK(result->to_ui32()==35);
   else CHECK(result->to_ui64()==35);
}
}

TEMPLATE_TEST_CASE("Private atomics preserve values and traps", "[private-atomics]", jit, jit2, saved_jit1, saved_jit2, saved_aot) {
   SECTION("i32") { exercise<TestType,uint32_t>(PRIVATE_ATOMICS_32_WASM); }
   SECTION("i64") { exercise<TestType,uint64_t>(PRIVATE_ATOMICS_64_WASM); }
}


TEST_CASE("Atomic IR preserves full-width sources and side effects", "[private-atomics][ir]") {
   using namespace psizam::detail;
   static_assert(sizeof(ir_inst) == 24);
   growable_allocator arena(16*1024*1024);
   jit_scratch_allocator scratch(arena);
   ir_function func;
   func.init(scratch, 32);
   func.next_vreg = 70004;
   for (uint32_t i=0; i<3; ++i) {
      auto& inst = func.insts[func.inst_count++];
      inst = {};
      inst.opcode = i ? ir_op::const_i64 : ir_op::const_i32;
      inst.type = i ? types::i64 : types::i32;
      inst.dest = 70000+i;
      inst.imm64 = i+1;
   }
   auto& atomic = func.insts[func.inst_count++];
   atomic = {};
   atomic.opcode = ir_op::atomic_op;
   atomic.type = static_cast<uint8_t>(atomic_sub::i64_atomic_rmw_cmpxchg);
   atomic.flags = IR_SIDE_EFFECT;
   atomic.dest = 70003;
   atomic.atomic = {70000,70001,70002,0x10203040};
   CHECK(atomic.value_type() == types::i64);
   CHECK(atomic.atomic.offset == 0x10203040);
   jit_optimizer::optimize(func, scratch);
   CHECK_FALSE((atomic.flags & IR_DEAD)); // unused atomic results still access/trap
   for (uint32_t v=70000; v<70003; ++v) {
      CHECK(func.use_count[v] == 1);
      CHECK_FALSE((func.insts[v-70000].flags & IR_DEAD));
   }
   jit_regalloc::compute_live_intervals(func, scratch);
   for (uint32_t v=70000; v<70003; ++v) CHECK(func.intervals[v].end == 3);
   CHECK(func.intervals[70003].type == types::i64);
}

TEST_CASE("Private atomics compile for both native architectures", "[private-atomics][aot]") {
   for (auto path : {PRIVATE_ATOMICS_32_WASM, PRIVATE_ATOMICS_64_WASM}) {
      auto wasm = read_wasm(path);
      // Every JIT section must carry a relocatable common-helper reference.
      auto jit_file = pzam_compile(wasm, {.arch=pzam_arch_flags::all,
                                         .tier=pzam_tier_flags::all_jit});
      CHECK(pzam_load(jit_file).code_sections.size() == 4);
      auto llvm_file = pzam_compile(wasm, {.arch=pzam_arch_flags::all,
         .tier=pzam_tier_flags::llvm_O2,.flags=pzam_compile_flags::softfloat});
      CHECK(pzam_load(llvm_file).code_sections.size() == 2);
   }
}


namespace {
wasm_code atomic_validation_module(bool memory, std::initializer_list<uint8_t> instruction) {
   wasm_code code{0,97,115,109,1,0,0,0, 1,4,1,0x60,0,0, 3,2,1,0};
   if (memory) code.insert(code.end(), {5,3,1,0,1});
   code.push_back(10);
   code.push_back(static_cast<uint8_t>(instruction.size()+4));
   code.push_back(1);
   code.push_back(static_cast<uint8_t>(instruction.size()+2));
   code.push_back(0); // local declarations
   code.insert(code.end(), instruction);
   code.push_back(0x0b);
   return code;
}
}
TEMPLATE_TEST_CASE("Malformed atomics fail validation", "[private-atomics][validation]", jit, jit2, saved_aot) {
   auto parse = [](wasm_code code) {
      wasm_allocator memory;
      backend<std::nullptr_t,TestType,policy> vm(code,&memory,policy{});
   };
   // Full-width i32 atomic.load requires align=2, exactly its access width.
   CHECK_THROWS_AS(parse(atomic_validation_module(true, {0x41,0,0xfe,0x10,0,0,0x1a})), wasm_parse_exception);
   CHECK_THROWS_AS(parse(atomic_validation_module(true, {0x41,0,0xfe,0x10,3,0,0x1a})), wasm_parse_exception);
   CHECK_THROWS_AS(parse(atomic_validation_module(true, {0x41,0,0xfe,0x10,32,0,0x1a})), wasm_parse_exception);
   CHECK_THROWS_AS(parse(atomic_validation_module(false,{0x41,0,0xfe,0x10,2,0,0x1a})), wasm_parse_exception);
   // Invalid subopcode0x110 must not truncate to valid atomic.load0x10.
   CHECK_THROWS_AS(parse(atomic_validation_module(true, {0x41,0,0xfe,0x90,2,2,0,0x1a})), wasm_parse_exception);
   CHECK_THROWS_AS(parse(atomic_validation_module(true, {0xfe,3,1})), wasm_parse_exception);
   CHECK_NOTHROW(parse(atomic_validation_module(false,{0xfe,3,0})));
}


namespace {
void append_leb(wasm_code& bytes, uint32_t value) {
   do {
      auto byte = uint8_t(value & 0x7f);
      value >>= 7;
      bytes.push_back(byte | (value ? 0x80 : 0));
   } while(value);
}
wasm_code high_vreg_module() {
   wasm_code code{0,97,115,109,1,0,0,0,
      1,8,1,0x60,3,0x7f,0x7f,0x7f,1,0x7f,
      3,2,1,0, 5,3,1,0,1, 7,7,1,3,'c','m','p',0,0};
   wasm_code body{0};
   for (unsigned i=0; i<66000; ++i) body.insert(body.end(),{0x41,0,0x1a});
   body.insert(body.end(),{0x20,0,0x20,1,0x20,2,0xfe,0x48,2,0,0x0b});
   wasm_code section{1};
   append_leb(section,body.size());
   section.insert(section.end(),body.begin(),body.end());
   code.push_back(10);
   append_leb(code,section.size());
   code.insert(code.end(),section.begin(),section.end());
   return code;
}
}
TEMPLATE_TEST_CASE("Atomic operands beyond vreg65535 execute correctly", "[private-atomics][high-vreg]",
                   jit, jit2, saved_jit1, saved_jit2, saved_aot) {
   auto wasm = high_vreg_module();
   wasm_allocator memory;
   backend<std::nullptr_t,TestType,policy> vm(wasm,&memory,policy{});
   auto result = vm.call_with_return("env","cmp",uint32_t{24},uint32_t{0},uint32_t{0x12345678});
   REQUIRE(result);
   CHECK(result->to_ui32() == 0);
   uint32_t stored = 0;
   std::memcpy(&stored,vm.get_context().linear_memory()+24,sizeof(stored));
   CHECK(stored == 0x12345678);
}

TEMPLATE_TEST_CASE("Private atomic memory64 addresses do not truncate", "[private-atomics][memory64]",
                   jit, jit2, saved_jit1, saved_jit2, saved_aot) {
   // Library-level check only: psibase's native consensus profile rejects memory64.
   wasm_code wasm{0,97,115,109,1,0,0,0,
      1,6,1,0x60,1,0x7e,1,0x7e, 3,2,1,0,
      5,3,1,4,1, 7,8,1,4,'l','o','a','d',0,0,
      10,10,1,8,0,0x20,0,0xfe,0x11,3,0,0x0b};
   wasm_allocator memory;
   backend<std::nullptr_t,TestType,policy> vm(wasm,&memory,policy{});
   const uint64_t value = 0xfedcba9876543210;
   std::memcpy(vm.get_context().linear_memory(),&value,sizeof(value));
   REQUIRE(vm.call_with_return("env","load",uint64_t{0})->to_ui64() == value);
   CHECK_THROWS_AS(vm.call_with_return("env","load",uint64_t{1}<<32), wasm_memory_exception);
   CHECK_THROWS_AS(vm.call_with_return("env","load",UINT64_MAX), wasm_memory_exception);
}

TEST_CASE("Saved memory metadata retains optional address width", "[private-atomics][metadata]") {
   for (bool memory64 : {false,true}) {
      pzam_file file;
      pzam_memory_type memory;
      memory.limits.initial = 1;
      if (memory64) memory.is_memory64 = true;
      file.metadata.memories.push_back(memory);
      file.metadata.num_imported_memories = 1;
      pzam_import_entry imported;
      imported.kind = static_cast<uint8_t>(Memory);
      imported.memory_type = memory;
      file.metadata.imports.push_back(imported);
      auto decoded = pzam_load(pzam_save(file));
      CHECK(decoded.metadata.memories[0].is_memory64.has_value() == memory64);
      auto restored = restore_module(decoded.metadata);
      CHECK(restored.memories[0].is_memory64 == memory64);
      CHECK(restored.imports[0].type.mem_t.is_memory64 == memory64);
      auto extracted = extract_metadata(restored);
      CHECK(extracted.memories[0].is_memory64.value_or(false) == memory64);
      CHECK(extracted.imports[0].memory_type.is_memory64.value_or(false) == memory64);
   }
}
