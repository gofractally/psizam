#pragma once
#include <catch2/catch.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Shared byte oracle for native psizam and Wasmtime. The constants and expected
// bytes are protocol values, independent of either engine's implementation.
namespace memory16_test {
inline constexpr size_t logical_bytes = 65536;
inline constexpr size_t physical_bytes = 131086;

template<class Call>
void exercise(Call&& call, std::span<const uint8_t> storage) {
   REQUIRE(storage.size() == physical_bytes);
   auto* mem = storage.data();
   CHECK(std::all_of(mem + 65536, mem + physical_bytes, [](auto b) { return b == 0; }));
   CHECK(call("size")->to_ui32() == 1);
   CHECK(call("grow", uint32_t{0})->to_ui32() == 1);
   CHECK(call("grow", uint32_t{1})->to_ui32() == UINT32_MAX);
   for (auto a : {0u, 65536u, 0xffff0000u}) {
      CHECK(call("read", a)->to_ui32() == 42);
      CHECK(call("large_add", a)->to_ui32() == 42);
      CHECK(call("live_address", a)->to_ui32() == a + 42);
   }
   for (auto a : {65535u, 131071u, UINT32_MAX}) {
      CHECK(call("add", a)->to_ui32() == 42);
      CHECK(call("add_offset", a)->to_ui32() == 17);
      CHECK(call("offset", a)->to_ui32() == 0);
      CHECK(call("wide", a)->to_ui32() == 0);
   }
   CHECK(call("adjacent_loads", 65532u)->to_ui32() == 0x3322112a);
   call("store", 65536u, 0x12345678u);
   CHECK(call("word", 0u)->to_ui32() == 0x12345678);
   call("store", 65535u, 0x12345678u);
   CHECK(call("word", UINT32_MAX)->to_ui32() == 0x12345678);
   call("store_offset", UINT32_MAX, 0xaabbccddu);
   CHECK(call("lane", UINT32_MAX)->to_ui32() == 0xaabbccdd);
   call("wide_store", UINT32_MAX, 0x11223344u);
   CHECK(call("wide", UINT32_MAX)->to_ui32() == 0x11223344);
   // These are aligned after offset addition, but have nonzero high bits.
   call("atomic_store", 65537u, 9u);
   CHECK(call("atomic_load", 65537u)->to_ui32() == 9);
   CHECK(call("atomic_add", 65537u)->to_ui32() == 9);
   CHECK(call("atomic_cmpxchg", 65537u)->to_ui32() == 16);
   CHECK(call("atomic_load", 65537u)->to_ui32() == 23);
   // Unaligned atomics use byte accesses in this single-threaded profile.
   call("atomic_store", UINT32_MAX, 9u);
   CHECK(call("atomic_add", UINT32_MAX)->to_ui32() == 9);
   CHECK(call("atomic_load", UINT32_MAX)->to_ui32() == 16);
   CHECK(call("atomic64", UINT32_MAX, uint64_t{0xfedcba9876543210})->to_ui64() == 0xfedcba9876543210);
   CHECK(call("fence_notify", UINT32_MAX)->to_ui32() == 0);
   // Preserve canonical single-thread policy: wait on private memory traps.
   CHECK_THROWS(call("wait", UINT32_MAX));
   call("wide_store", UINT32_MAX, 0x11223344u);
   call("fill", 65536u, 0u);
   CHECK_THROWS(call("fill", 65536u, 1u));
   CHECK_THROWS(call("fill", 65537u, 0u));
   CHECK_THROWS(call("copy", 65535u, 2u));
   call("fill", 100u, 3u);
   call("copy", 110u, 4u);
   std::vector<uint8_t> expected(physical_bytes);
   auto put32 = [&](size_t p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) expected[p + i] = uint8_t(v >> (i * 8)); };
   put32(0, 0x12345678);
   put32(65535, 0x12345678);
   put32(65536, 23);
   for (unsigned i = 0; i < 4; ++i) put32(131070 + i * 4, 0x11223344);
   expected[100] = expected[101] = expected[102] = 77;
   put32(110, 0x12345678);
   CHECK(std::equal(expected.begin(), expected.end(), mem));
}
template<class Call>
void exercise_bulk_ranges(Call&& call, std::span<const uint8_t> storage) {
   const std::vector<uint8_t> before(storage.begin(), storage.end());
   CHECK_THROWS(call("copy_from", 0u, 65535u, 2u));
   CHECK_THROWS(call("copy_from", 65535u, 0u, 2u));
   CHECK_THROWS(call("init", 65535u, 0u, 2u));
   CHECK_THROWS(call("init", 0u, 3u, 2u));
   CHECK_THROWS(call("init", 0u, 5u, 0u));
   CHECK(std::equal(before.begin(), before.end(), storage.begin()));
   call("copy_from", 65536u, 65536u, 0u);
   call("init", 65536u, 0u, 0u);
   call("init", 400u, 0u, 4u);
   const std::array<uint8_t, 4> initialized{0x77, 0x88, 0x99, 0xaa};
   CHECK(std::equal(initialized.begin(), initialized.end(), storage.begin() + 400));
   call("copy_from", 401u, 400u, 3u);
   const std::array<uint8_t, 4> overlapping{0x77, 0x77, 0x88, 0x99};
   CHECK(std::equal(overlapping.begin(), overlapping.end(), storage.begin() + 400));
   call("drop_data");
   call("init", 0u, 0u, 0u);
   CHECK_THROWS(call("init", 0u, 0u, 1u));
}

inline std::vector<uint8_t> seeded_image() {
   std::vector<uint8_t> result(physical_bytes);
   for (size_t i = 0; i < logical_bytes; ++i) result[i] = uint8_t(i * 17 + 23);
   for (size_t i = 0; i < logical_bytes; ++i) result[i + 65535] = uint8_t(i * 29 + 7);
   for (size_t i = 0; i < 16; ++i) result[131070 + i] = uint8_t(i);
   return result;
}

template<class Call>
void seed_and_compare(Call&& call, std::span<const uint8_t> storage) {
   REQUIRE(storage.size() == physical_bytes);
   call("seed_all");
   const auto expected = seeded_image();
   CHECK(std::equal(expected.begin(), expected.end(), storage.begin()));
}
} // namespace memory16_test
