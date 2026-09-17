#include <psizam/hosted.hpp>
#include <catch2/catch.hpp>

TEST_CASE("Canonical argument slots preserve values and reject overflow", "[call-entry][canonical]") {
   psizam::detail::canonical_call_slots slots;
   CHECK(slots.size() == 0);
   for (uint64_t i = 0; i < psio::MAX_FLAT_PARAMS; ++i) {
      psio::native_value v;
      v.i64 = 0xFEDCBA9876543200ull + i;
      slots.push_back(v);
   }
   CHECK(slots.size() == psio::MAX_FLAT_PARAMS);
   for (uint64_t i = 0; i < slots.size(); ++i)
      CHECK(slots[i].i64 == 0xFEDCBA9876543200ull + i);
   psio::native_value extra;
   extra.i64 = 0;
   CHECK_THROWS_AS(slots.push_back(extra), psizam::wasm_interpreter_exception);
   CHECK(slots.size() == psio::MAX_FLAT_PARAMS);
}
