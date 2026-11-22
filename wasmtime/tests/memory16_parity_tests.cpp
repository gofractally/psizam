// Cross-engine correctness fixtures, not performance workloads. Both engines
// consume the same compiled Wasm and the same complete physical-byte oracle.
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/execution_profile.hpp>
#include <psizam/wasmtime_backend.hpp>

#include "memory16_test_support.hpp"

#include <cstring>
#include <optional>
#include <string_view>

namespace wt = psizam::wasmtime;
namespace {
using namespace psizam;

struct host {
   uint32_t calls = 0;
   uint32_t bytes(std::span<const char> value) {
      ++calls;
      return value.empty() ? 0 : uint8_t(value.front());
   }
};

wt::engine_options options(bool force = false) {
   wt::engine_options result;
   result.memory16 = force;
   result.features |= wt::feature::threads;
   return result;
}

wasm_code fixture(uint32_t profile = memory16_p32_v1, const char* path = MEMORY16_PARITY_WASM) {
   auto code = read_wasm(path);
   if (profile) append_execution_profile(code, profile);
   return code;
}

std::span<const uint8_t> storage(wt::instance<host>& instance) {
   const auto memory = instance.memory_storage();
   return {reinterpret_cast<const uint8_t*>(memory.data()), memory.size()};
}

// Scalar bit container matching the native caller's observation API. The
// fixture returns i64 only for atomic64; mutation exports return no value.
struct word {
   uint64_t bits;
   uint32_t to_ui32() const { return uint32_t(bits); }
   uint64_t to_ui64() const { return bits; }
};
auto caller(wt::instance<host>& instance) {
   return [&instance](const char* name, auto... args) -> std::optional<word> {
      const std::string_view n = name;
      if (n == "store" || n == "store_offset" || n == "wide_store" ||
          n == "atomic_store" || n == "fill" || n == "copy" ||
          n == "copy_from" || n == "init" || n == "drop_data" || n == "seed_all") {
         instance.call(name, args...);
         return {};
      }
      if (n == "atomic64") return word{instance.call<uint64_t>(name, args...)};
      return word{instance.call<uint32_t>(name, args...)};
   };
}

void check(wt::instance<host>& instance) {
   REQUIRE(instance.memory().size() == memory16_test::logical_bytes);
   auto run = caller(instance);
   memory16_test::exercise(run, storage(instance));
   memory16_test::exercise_bulk_ranges(run, storage(instance));
   memory16_test::seed_and_compare(run, storage(instance));
}

void check_fresh(wt::instance<host>& instance) {
   REQUIRE(instance.memory().size() == memory16_test::logical_bytes);
   const auto memory = storage(instance);
   REQUIRE(memory.size() == memory16_test::physical_bytes);
   CHECK(std::all_of(memory.begin() + 65536, memory.end(), [](auto byte) { return byte == 0; }));
   CHECK(instance.call<uint32_t, uint32_t>("word", 0u) == 0x3322112a);
}

struct selection { uint32_t profile; bool force; };
constexpr selection selections[] = {
   {memory16_p16_v1, false}, {memory16_p32_v1, false},
   {0, true}, {memory16_p16_v1, true}, {memory16_p32_v1, true},
};
}

TEST_CASE("memory16: native and Wasmtime agree on all physical bytes", "[memory16][parity]") {
   for (auto selected : selections) {
      CAPTURE(selected.profile, selected.force);
      auto code = fixture(selected.profile);
      wt::runtime<host> runtime(options(selected.force));
      auto module = runtime.compile(code);
      host h;
      wt::instance<host> instance(runtime, module, h);
      check(instance);

      default_options native_options;
      if (selected.force) native_options.memory_mode = mem_safety::memory16;
      wasm_allocator allocator;
      backend<std::nullptr_t, jit2> native(code, &allocator, native_options);
      auto run = [&](const char* name, auto... args) { return native.call_with_return("env", name, args...); };
      std::span<const uint8_t> image(allocator.get_base_ptr<uint8_t>(), memory16_accessible_bytes);
      memory16_test::exercise(run, image);
      memory16_test::exercise_bulk_ranges(run, image);
      memory16_test::seed_and_compare(run, image);
      REQUIRE(image.size() == storage(instance).size());
      CHECK(std::equal(image.begin(), image.end(), storage(instance).begin()));
   }
}

TEST_CASE("memory16: complete padding state can be copied and restored privately", "[memory16][snapshot]") {
   wt::runtime<host> runtime{options()};
   auto module = runtime.compile(fixture());
   host h;
   wt::instance<host> instance(runtime, module, h);
   check(instance);
   const std::vector<uint8_t> snapshot(storage(instance).begin(), storage(instance).end());
   instance.call("store_offset", UINT32_MAX, 0xaabbccddu);
   CHECK_FALSE(std::equal(snapshot.begin(), snapshot.end(), storage(instance).begin()));
   auto destination = instance.memory_storage();
   REQUIRE(destination.size() == snapshot.size());
   std::memcpy(destination.data(), snapshot.data(), snapshot.size());
   CHECK(std::equal(snapshot.begin(), snapshot.end(), storage(instance).begin()));
   CHECK(instance.call<uint32_t, uint32_t>("lane", UINT32_MAX) == 0x03020100);
   CHECK(instance.memory().size() == 65536);

   // A second instance of the same compiled module must not inherit any of
   // the first instance's writable padding, and cannot mutate its state.
   wt::instance<host> fresh(runtime, module, h);
   check_fresh(fresh);
   fresh.call("store_offset", UINT32_MAX, 0xdeadbeefu);
   CHECK(std::equal(snapshot.begin(), snapshot.end(), storage(instance).begin()));
}

TEST_CASE("memory16: marker and runtime override preserve saved module identity", "[memory16][artifact]") {
   for (auto selected : selections) {
      CAPTURE(selected.profile, selected.force);
      wt::runtime<host> producer(options(selected.force));
      auto module = producer.compile(fixture(selected.profile));
      auto artifact = module.serialize();
      REQUIRE(!artifact.empty());
      wt::runtime<host> consumer{options()};
      auto loaded = consumer.load(artifact);
      host h;
      wt::instance<host> instance(consumer, loaded, h);
      check(instance);
   }
   wt::runtime<host> ordinary{options()};
   auto standard = ordinary.compile(fixture(0)).serialize();
   wt::runtime<host> forced(options(true));
   CHECK_THROWS(forced.load(standard));
}

TEST_CASE("memory16: untagged defaults still use ordinary Wasm bounds", "[memory16][off]") {
   auto code = fixture(0);
   wt::runtime<host> runtime{options()};
   auto module = runtime.compile(code);
   host h;
   wt::instance<host> instance(runtime, module, h);
   REQUIRE(instance.memory().size() == 65536);
   REQUIRE(instance.memory_storage().size() == 65536);
   CHECK(instance.call<uint32_t, uint32_t>("read", 0u) == 42);
   CHECK_THROWS(instance.call<uint32_t, uint32_t>("read", 65536u));
   CHECK_THROWS(instance.call<uint32_t, uint32_t>("word", 65535u));
   CHECK_THROWS(instance.call<uint32_t, uint32_t>("offset", 1u));
   CHECK(instance.call<uint32_t, uint32_t>("read", 0u) == 42);
}

TEST_CASE("memory16: host spans never expose private padding or mask lengths", "[memory16][host]") {
   auto code = fixture(memory16_p32_v1, MEMORY16_PARITY_HOST_WASM);
   wt::runtime<host> runtime{options()};
   runtime.add<&host::bytes>("env", "span");
   auto module = runtime.compile(code);
   host h;
   wt::instance<host> instance(runtime, module, h);
   auto read = [&](uint32_t pointer, uint32_t length) {
      return instance.call<uint32_t, uint32_t, uint32_t>("span", pointer, length);
   };
   CHECK(read(65532, 1) == 42);
   CHECK(read(65535, 1) == 23);
   CHECK(read(65536, 0) == 0);
   const auto calls = h.calls;
   for (const auto [pointer, length] : {std::pair{65535u, 2u}, {65536u, 1u},
         {65537u, 0u}, {UINT32_MAX, 0u}, {0u, UINT32_MAX}})
      CHECK_THROWS_AS(read(pointer, length), wt::host_memory_error);
   CHECK(h.calls == calls);
   CHECK(instance.memory().size() == 65536);
   CHECK(instance.memory_storage().size() == 131086);
   CHECK(read(65532, 1) == 42);
}

TEST_CASE("memory16: malformed profiles and unsupported memory shapes reject", "[memory16][admission]") {
   wt::runtime<host> runtime{options()};
   auto original = fixture();
   auto rejects = [&](wasm_code code) {
      CHECK_THROWS([&] { auto module = runtime.compile(code); host h; wt::instance<host> instance(runtime, module, h); }());
   };
   auto code = original;
   code.back() = 1;
   rejects(code);
   code = original;
   code.pop_back();
   rejects(code);
   code = original;
   const uint8_t memory[] = {5, 4, 1, 1, 1, 1};
   auto position = std::search(code.begin(), code.end(), std::begin(memory), std::end(memory));
   REQUIRE(position != code.end());
   position[5] = 2;
   rejects(code);
   code = original;
   position = std::search(code.begin(), code.end(), std::begin(memory), std::end(memory));
   REQUIRE(position != code.end());
   position[4] = position[5] = 0;
   rejects(code);
   code = original;
   const uint8_t offset[] = {0x2d, 0, 0xff, 0xff, 3};
   position = std::search(code.begin(), code.end(), std::begin(offset), std::end(offset));
   REQUIRE(position != code.end());
   position[2] = 0x80; position[3] = 0x80; position[4] = 4;
   rejects(code);
}
