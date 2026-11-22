#include <psizam/backend.hpp>
#include <psizam/pzam_compile.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include <psizam/detail/wasi_host.hpp>
#include <catch2/catch.hpp>

#include <fstream>
#include <iterator>
#include "memory16_test_support.hpp"

using namespace psizam;

namespace {
wasm_code fixture(bool tagged = true) {
   std::ifstream in(PSIZAM_MEMORY16_FIXTURE, std::ios::binary);
   REQUIRE(in.good());
   wasm_code code{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
   if (tagged) append_execution_profile(code, memory16_p16_v1);
   return code;
}

struct parallel_options : default_options { uint32_t compile_threads = 4; };
struct parallel_jit2 : jit2 {};
struct reuse_options : default_options { uint32_t max_pages = 2; };

uint32_t host_span(std::span<const uint32_t> values) { return values.empty() ? 0 : values.front(); }
uint32_t host_pointer(const uint32_t* value) { return *value; }
struct pointer_converter : type_converter<standalone_function_t> {
   using type_converter::type_converter;
   using type_converter::from_wasm;
   PSIZAM_FROM_WASM(const uint32_t*, (wasm_ptr_t ptr)) {
      return static_cast<const uint32_t*>(this->template validate_pointer<uint32_t>(ptr, 1));
   }
};

wasm_code host_fixture() {
   std::ifstream in(PSIZAM_MEMORY16_HOST_FIXTURE, std::ios::binary);
   REQUIRE(in.good());
   wasm_code code{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
   append_execution_profile(code, memory16_p32_v1);
   return code;
}

template<class Call>
void exercise_host(Call&& call) {
   CHECK(call("span", 65532u, 1u)->to_ui32() == 42);
   CHECK(call("span", 65536u, 0u)->to_ui32() == 0);
   CHECK(call("pointer", 65532u)->to_ui32() == 42);
   for (uint32_t ptr : {65535u, 65536u, UINT32_MAX}) {
      CHECK_THROWS(call("span", ptr, 1u));
      CHECK_THROWS(call("pointer", ptr));
   }
   CHECK_THROWS(call("span", 0u, UINT32_MAX));
}

// Same exact byte oracle is consumed by the Wasmtime cross-engine target.
template<class Call>
void exercise(Call&& call, wasm_allocator& wa) {
   REQUIRE(wa.is_memory16());
   CHECK(wa.reservation_size() < 256 * 1024);
   std::span<const uint8_t> storage(wa.get_base_ptr<uint8_t>(), memory16_accessible_bytes);
   memory16_test::exercise(call, storage);
   memory16_test::exercise_bulk_ranges(call, storage);
   memory16_test::seed_and_compare(call, storage);
}
}

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
#define MEMORY16_BACKENDS interpreter, jit, jit_profile, jit2, parallel_jit2, jit_llvm
#else
#define MEMORY16_BACKENDS interpreter, jit, jit_profile, jit2, parallel_jit2
#endif

TEMPLATE_TEST_CASE("memory16: host pointer conversions respect the logical limit", "[memory16]", MEMORY16_BACKENDS) {
   auto code = host_fixture();
   SECTION("runtime table") {
      host_function_table table;
      table.add<host_span>("env", "span");
      table.add<host_pointer, pointer_converter>("env", "pointer");
      wasm_allocator wa;
      backend<std::nullptr_t, TestType> b(code, std::move(table), nullptr, &wa);
      b.initialize(nullptr);
      exercise_host([&](const char* name, auto... args) { return b.call_with_return(nullptr, name, args...); });
   }
   SECTION("legacy registration") {
      using host_functions = registered_host_functions<standalone_function_t, execution_interface, pointer_converter>;
      static const bool registered = [] {
         host_functions::add<host_span>("env", "span");
         host_functions::add<host_pointer>("env", "pointer");
         return true;
      }();
      (void)registered;
      wasm_allocator wa;
      backend<host_functions, TestType> b(code, &wa);
      b.initialize(nullptr);
      exercise_host([&](const char* name, auto... args) { return b.call_with_return("env", name, args...); });
   }
}

TEST_CASE("memory16: WASI validates complete raw-offset ranges", "[memory16]") {
   wasm_allocator wa;
   wa.configure_memory16(true);
   wa.reset(1);
   psizam::detail::wasi_host host;
   auto* memory = wa.get_base_ptr<char>();
   native_value args[2] = {native_value{uint32_t{65534}}, native_value{uint32_t{4}}};
   CHECK_THROWS(psizam::detail::wasi_trampoline<&psizam::detail::wasi_host::random_get, true>(&host, args, memory));
   CHECK_FALSE(host.psizam_memory16);
   host.memory = memory;
   host.psizam_memory16 = true;
   CHECK_THROWS(host.mem(UINT32_MAX, 1));
   CHECK_THROWS(host.check_iov_bounds(0, UINT32_MAX));
   CHECK_THROWS(host.poll_oneoff(0, 0, UINT32_MAX, 0));
   host.args = {"abcd"};
   CHECK_THROWS(host.args_get(0, 65534));
   CHECK_THROWS(host.args_get(UINT32_MAX, 0));
   CHECK_NOTHROW(host.mem(65536, 0));
}

TEMPLATE_TEST_CASE("memory16: all backends preserve wrap boundaries", "[memory16]", MEMORY16_BACKENDS) {
   INFO(Catch::getResultCapture().getCurrentTestName());
   wasm_allocator wa;
   using opts = std::conditional_t<std::is_same_v<TestType, parallel_jit2>, parallel_options, default_options>;
   backend<std::nullptr_t, TestType, opts> b(fixture(), &wa);
   b.initialize(nullptr);
   exercise([&](auto name, auto... args) { return b.call_with_return("env", name, args...); }, wa);
   auto interface = b.get_context().get_interface();
   CHECK_THROWS(interface.template validate_pointer<char>(wasm_ptr_t{65536}, 1));
   CHECK_THROWS(interface.validate_null_terminated_pointer(wasm_ptr_t{65536}));
   b.initialize(nullptr);
   CHECK(std::all_of(wa.get_base_ptr<char>() + 65536,
                    wa.get_base_ptr<char>() + memory16_accessible_bytes, [](auto b) { return b == 0; }));
}

TEMPLATE_TEST_CASE("memory16: untagged one-page wasm retains ordinary semantics", "[memory16]", MEMORY16_BACKENDS) {
   wasm_allocator wa;
   backend<std::nullptr_t, TestType> b(fixture(false), &wa);
   b.initialize(nullptr);
   CHECK_FALSE(wa.is_memory16());
   CHECK(b.call_with_return("env", "read", 0u)->to_ui32() == 42);
   CHECK_THROWS(b.call_with_return("env", "read", 65536u));
}

TEMPLATE_TEST_CASE("memory16: reinitialization preserves the compiled profile", "[memory16][reuse]", MEMORY16_BACKENDS) {
   SECTION("tagged and forced profiles survive default guarded options") {
      for (bool tagged : {false, true}) {
         CAPTURE(tagged);
         reuse_options initial;
         if (!tagged) initial.memory_mode = mem_safety::memory16;
         wasm_allocator memory;
         backend<std::nullptr_t, TestType, reuse_options> b(fixture(tagged), &memory, initial);
         b.call_with_return("env", "store_offset", UINT32_MAX, 0xaabbccddu);
         reuse_options next;
         CHECK_NOTHROW(b.initialize(nullptr, next));
         REQUIRE(memory.is_memory16());
         CHECK(b.call_with_return("env", "read", 65536u)->to_ui32() == 42);
         CHECK(b.call_with_return("env", "grow", 1u)->to_ui32() == UINT32_MAX);
         CHECK(std::all_of(memory.get_base_ptr<uint8_t>() + 65536,
                          memory.get_base_ptr<uint8_t>() + memory16_accessible_bytes,
                          [](auto byte) { return byte == 0; }));
      }
   }
   SECTION("ordinary profile switch rejects before changing current state") {
      auto code = fixture(false);
      const uint8_t layout[] = {5, 4, 1, 1, 1, 1};
      auto position = std::search(code.begin(), code.end(), std::begin(layout), std::end(layout));
      REQUIRE(position != code.end());
      position[5] = 2;
      reuse_options initial;
      wasm_allocator memory;
      backend<std::nullptr_t, TestType, reuse_options> b(code, &memory, initial);
      b.call_with_return("env", "store", 0u, 99u);
      reuse_options invalid;
      invalid.memory_mode = mem_safety::memory16;
      invalid.max_pages = 1;
      CHECK_THROWS_AS(b.initialize(nullptr, invalid), wasm_interpreter_exception);
      CHECK_FALSE(memory.is_memory16());
      CHECK(b.call_with_return("env", "size")->to_ui32() == 1);
      CHECK(b.call_with_return("env", "word", 0u)->to_ui32() == 99);
      CHECK_THROWS(b.call_with_return("env", "read", 65536u));
      CHECK(b.call_with_return("env", "grow", 1u)->to_ui32() == 1);
      CHECK(b.call_with_return("env", "word", 65536u)->to_ui32() == 0);
      CHECK_NOTHROW(b.initialize(nullptr, initial));
      CHECK(b.call_with_return("env", "size")->to_ui32() == 1);
      CHECK_THROWS(b.call_with_return("env", "read", 65536u));
      CHECK(b.call_with_return("env", "grow", 1u)->to_ui32() == 1);
      CHECK(b.call_with_return("env", "word", 65536u)->to_ui32() == 0);
   }
}

TEST_CASE("memory16: nested host modes restore on return and exception", "[memory16][host][reentry]") {
   struct state { bool psizam_memory16; };
   for (bool initial : {false, true}) {
      state host{initial};
      {
         detail::host_memory16_scope<state, true> outer(&host);
         CHECK(host.psizam_memory16);
         {
            detail::host_memory16_scope<state, false> ordinary(&host);
            CHECK_FALSE(host.psizam_memory16);
            {
               detail::host_memory16_scope<state, true> nested(&host);
               CHECK(host.psizam_memory16);
            }
            CHECK_FALSE(host.psizam_memory16);
         }
         CHECK(host.psizam_memory16);
         try {
            detail::host_memory16_scope<state, false> ordinary(&host);
            CHECK_FALSE(host.psizam_memory16);
            throw 1;
         } catch (int) {}
         CHECK(host.psizam_memory16);
      }
      CHECK(host.psizam_memory16 == initial);
   }
}

TEMPLATE_TEST_CASE("memory16: low-level rebind rejects stale profile options", "[memory16][reuse]", MEMORY16_BACKENDS) {
   auto code = fixture(false);
   const uint8_t layout[] = {5, 4, 1, 1, 1, 1};
   auto position = std::search(code.begin(), code.end(), std::begin(layout), std::end(layout));
   REQUIRE(position != code.end());
   position[4] = position[5] = 2;
   // Keep the ordinary module alive beyond the rebound context's lifetime.
   wasm_allocator ordinary_memory;
   backend<std::nullptr_t, TestType, reuse_options> ordinary(code, &ordinary_memory);
   wasm_allocator memory;
   backend<std::nullptr_t, TestType, reuse_options> small(fixture(), &memory);
   auto& context = small.get_context();
   memory.get_base_ptr<uint8_t>()[65536] = 0x55;
   context.set_module(&ordinary.get_module());
   CHECK_THROWS_AS(context.reset(), wasm_interpreter_exception);
   REQUIRE(memory.is_memory16());
   CHECK(memory.get_current_page() == 1);
   CHECK(memory.get_base_ptr<uint8_t>()[65536] == 0x55);
   context.set_mem_mode(mem_safety::guarded);
   context.set_max_pages(2);
   CHECK_NOTHROW(context.reset());
   CHECK_FALSE(memory.is_memory16());
   CHECK(memory.get_current_page() == 2);
   CHECK(memory.get_base_ptr<uint8_t>()[0] == 42);
   CHECK(memory.get_base_ptr<uint8_t>()[65536] == 0);
}

TEST_CASE("memory16: profiles are explicit and validated before compilation", "[memory16]") {
   auto code = fixture();
   CHECK(read_execution_profile(code) == memory16_p16_v1);
   auto original = code;
   append_execution_profile(code, memory16_p16_v1);
   CHECK(code == original);
   // Linker-concatenated records and repeated custom sections are supported.
   auto duplicate = fixture(false);
   auto section = std::span(original).subspan(duplicate.size());
   duplicate.insert(duplicate.end(), section.begin(), section.end());
   duplicate.insert(duplicate.end(), section.begin(), section.end());
   CHECK(read_execution_profile(duplicate) == memory16_p16_v1);
   duplicate[duplicate.size() - 2] = 32;
   CHECK_THROWS(read_execution_profile(duplicate));
   CHECK_THROWS(append_execution_profile(code, memory16_p32_v1));
   code.back() = 1;
   CHECK_THROWS(backend<std::nullptr_t, jit2>(code, nullptr));
   code = original;
   code.pop_back();
   CHECK_THROWS(backend<std::nullptr_t, jit2>(code, nullptr));
   code = original;
   // Offset 65535 = ff ff 03; change to 65536 = 80 80 04.
   const uint8_t needle[] = {0x2d, 0, 0xff, 0xff, 3};
   auto it = std::search(code.begin(), code.end(), std::begin(needle), std::end(needle));
   REQUIRE(it != code.end());
   it[2] = 0x80; it[3] = 0x80; it[4] = 4;
   CHECK_THROWS(backend<std::nullptr_t, jit2>(code, nullptr));
   CHECK_THROWS(backend<std::nullptr_t, jit2, parallel_options>(code, nullptr));
   code = original;
   const uint8_t memory[] = {5, 4, 1, 1, 1, 1};
   it = std::search(code.begin(), code.end(), std::begin(memory), std::end(memory));
   REQUIRE(it != code.end());
   it[5] = 2;
   CHECK_THROWS(backend<std::nullptr_t, jit2>(code, nullptr));
   auto shared = fixture(false);
   it = std::search(shared.begin(), shared.end(), std::begin(memory), std::end(memory));
   REQUIRE(it != shared.end());
   it[3] = 3; // limits include maximum + shared; valid ordinary Wasm
   CHECK_NOTHROW(backend<std::nullptr_t, jit2>(shared, nullptr));
   auto tagged_shared = shared;
   append_execution_profile(tagged_shared, memory16_p16_v1);
   CHECK_THROWS(backend<std::nullptr_t, jit2>(tagged_shared, nullptr));
   default_options forced;
   forced.memory_mode = mem_safety::memory16;
   CHECK_THROWS(backend<std::nullptr_t, jit2>(shared, nullptr, forced));
}

TEST_CASE("memory16: pzam carries and validates the execution profile", "[memory16]") {
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   auto tier = GENERATE(pzam_tier_flags::jit1, pzam_tier_flags::jit2, pzam_tier_flags::llvm_O1,
                        pzam_tier_flags::llvm_O2, pzam_tier_flags::llvm_O3);
#else
   auto tier = GENERATE(pzam_tier_flags::jit1, pzam_tier_flags::jit2);
#endif
   CAPTURE(uint32_t(tier));
   auto bytes = pzam_compile(fixture(), {.arch = pzam_arch_flags::all, .tier = tier});
   auto file = pzam_load(bytes);
   REQUIRE(file.metadata.execution_profile == memory16_p16_v1);
   REQUIRE(file.code_sections.size() == 2);
   const auto expected_tier = tier == pzam_tier_flags::jit1 ? pzam_opt_tier::jit1
      : tier == pzam_tier_flags::jit2 ? pzam_opt_tier::jit2
      : tier == pzam_tier_flags::llvm_O1 ? pzam_opt_tier::llvm_O1
      : tier == pzam_tier_flags::llvm_O2 ? pzam_opt_tier::llvm_O2 : pzam_opt_tier::llvm_O3;
   CHECK(file.code_sections[0].opt_tier == uint8_t(expected_tier));
   CHECK(file.code_sections[1].opt_tier == uint8_t(expected_tier));
   REQUIRE(file.code_sections[0].execution_profile == memory16_p16_v1);
   auto loaded = psizam::detail::load_pzam(file);
   struct free_code { void* ptr; ~free_code() { jit_allocator::instance().free(ptr); } } cleanup{loaded.exec_code};
   wasm_allocator wa;
   psizam::detail::jit_execution_context<> ctx(loaded.mod, 8192);
   ctx.set_wasm_allocator(&wa);
   ctx.reset();
   exercise([&](const char* name, auto... args) {
      return ctx.execute(nullptr, psizam::detail::jit_visitor{nullptr}, loaded.mod.get_exported_function(name), args...);
   }, wa);
   file.code_sections[0].execution_profile = 0;
   CHECK_THROWS(pzam_load(pzam_save(file)));
}

TEST_CASE("memory16: optional cross-compiled pzam fixture", "[.memory16-cross]") {
   const char* path = std::getenv("PSIZAM_MEMORY16_PZAM");
   if (!path) { SUCCEED("Set PSIZAM_MEMORY16_PZAM to test a cross-compiled artifact"); return; }
   std::ifstream in(path, std::ios::binary);
   REQUIRE(in.good());
   std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
   auto loaded = psizam::detail::load_pzam(pzam_load(bytes));
   struct free_code { void* ptr; ~free_code() { jit_allocator::instance().free(ptr); } } cleanup{loaded.exec_code};
   wasm_allocator wa;
   psizam::detail::jit_execution_context<> ctx(loaded.mod, 8192);
   ctx.set_wasm_allocator(&wa);
   ctx.reset();
   exercise([&](const char* name, auto... args) {
      return ctx.execute(nullptr, psizam::detail::jit_visitor{nullptr}, loaded.mod.get_exported_function(name), args...);
   }, wa);
}
