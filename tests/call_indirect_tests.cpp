#include <algorithm>
#include <vector>
#include <iterator>
#include <cstdlib>
#include <fstream>
#include <string>

#include <catch2/catch.hpp>

#include <psizam/backend.hpp>
#include "wasm_config.hpp"
#include "utils.hpp"

using namespace psizam;
using namespace psizam::detail;

namespace {

struct dynamic_options {
   std::uint32_t max_stack_bytes = 1024 * 1024;
   static constexpr std::uint32_t max_func_local_bytes = 1024 * 1024;
};

wasm_code call_indirect_wasm = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x03, 0x60,
  0x01, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f,
  0x01, 0x7f, 0x03, 0x10, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x01,
  0x01, 0x01, 0x01, 0x02, 0x01, 0x00, 0x01, 0x01, 0x04, 0x04, 0x01, 0x70,
  0x00, 0x0a, 0x05, 0x03, 0x01, 0x00, 0x01, 0x06, 0x0e, 0x02, 0x7f, 0x01,
  0x41, 0x80, 0x80, 0x04, 0x0b, 0x7f, 0x01, 0x41, 0x80, 0x08, 0x0b, 0x07,
  0xa8, 0x01, 0x08, 0x0f, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x69, 0x6e, 0x64,
  0x69, 0x72, 0x65, 0x63, 0x74, 0x5f, 0x30, 0x00, 0x05, 0x0f, 0x74, 0x65,
  0x73, 0x74, 0x5f, 0x69, 0x6e, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x5f,
  0x31, 0x00, 0x06, 0x0f, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x69, 0x6e, 0x64,
  0x69, 0x72, 0x65, 0x63, 0x74, 0x5f, 0x32, 0x00, 0x07, 0x12, 0x74, 0x65,
  0x73, 0x74, 0x5f, 0x69, 0x6e, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x5f,
  0x6c, 0x6f, 0x6f, 0x70, 0x00, 0x08, 0x14, 0x74, 0x65, 0x73, 0x74, 0x5f,
  0x69, 0x6e, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x5f, 0x32, 0x70, 0x61,
  0x72, 0x61, 0x6d, 0x00, 0x09, 0x14, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x6e,
  0x65, 0x73, 0x74, 0x65, 0x64, 0x5f, 0x69, 0x6e, 0x64, 0x69, 0x72, 0x65,
  0x63, 0x74, 0x00, 0x0b, 0x17, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x61, 0x6c,
  0x6c, 0x6f, 0x63, 0x5f, 0x61, 0x6e, 0x64, 0x5f, 0x69, 0x6e, 0x64, 0x69,
  0x72, 0x65, 0x63, 0x74, 0x00, 0x0d, 0x11, 0x74, 0x65, 0x73, 0x74, 0x5f,
  0x67, 0x72, 0x6f, 0x77, 0x5f, 0x61, 0x6e, 0x64, 0x5f, 0x75, 0x73, 0x65,
  0x00, 0x0e, 0x09, 0x0b, 0x01, 0x00, 0x41, 0x00, 0x0b, 0x05, 0x00, 0x01,
  0x02, 0x03, 0x04, 0x0a, 0xf5, 0x01, 0x0f, 0x07, 0x00, 0x20, 0x00, 0x41,
  0x0a, 0x6a, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x41, 0x03, 0x6c, 0x0b, 0x07,
  0x00, 0x20, 0x00, 0x41, 0x05, 0x6b, 0x0b, 0x04, 0x00, 0x20, 0x00, 0x0b,
  0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b, 0x09, 0x00, 0x41, 0x05,
  0x41, 0x00, 0x11, 0x00, 0x00, 0x0b, 0x09, 0x00, 0x41, 0x07, 0x41, 0x01,
  0x11, 0x00, 0x00, 0x0b, 0x09, 0x00, 0x41, 0x14, 0x41, 0x02, 0x11, 0x00,
  0x00, 0x0b, 0x30, 0x01, 0x02, 0x7f, 0x41, 0x00, 0x21, 0x00, 0x41, 0x00,
  0x21, 0x01, 0x02, 0x40, 0x03, 0x40, 0x20, 0x01, 0x41, 0x04, 0x4f, 0x0d,
  0x01, 0x20, 0x00, 0x41, 0x0a, 0x20, 0x01, 0x11, 0x00, 0x00, 0x6a, 0x21,
  0x00, 0x20, 0x01, 0x41, 0x01, 0x6a, 0x21, 0x01, 0x0c, 0x00, 0x0b, 0x0b,
  0x20, 0x00, 0x0b, 0x0c, 0x00, 0x41, 0xe4, 0x00, 0x41, 0x2a, 0x41, 0x04,
  0x11, 0x02, 0x00, 0x0b, 0x09, 0x00, 0x20, 0x01, 0x20, 0x00, 0x11, 0x00,
  0x00, 0x0b, 0x0c, 0x00, 0x41, 0x01, 0x41, 0x00, 0x41, 0x05, 0x10, 0x0a,
  0x10, 0x0a, 0x0b, 0x11, 0x01, 0x01, 0x7f, 0x23, 0x01, 0x21, 0x01, 0x23,
  0x01, 0x20, 0x00, 0x6a, 0x24, 0x01, 0x20, 0x01, 0x0b, 0x25, 0x01, 0x01,
  0x7f, 0x41, 0x10, 0x10, 0x0c, 0x21, 0x00, 0x20, 0x00, 0x41, 0x00, 0x36,
  0x02, 0x00, 0x20, 0x00, 0x41, 0x07, 0x36, 0x02, 0x04, 0x20, 0x00, 0x28,
  0x02, 0x04, 0x20, 0x00, 0x28, 0x02, 0x00, 0x11, 0x00, 0x00, 0x0b, 0x23,
  0x01, 0x02, 0x7f, 0x41, 0x01, 0x40, 0x00, 0x21, 0x00, 0x20, 0x00, 0x41,
  0x80, 0x80, 0x04, 0x6c, 0x21, 0x01, 0x20, 0x01, 0x41, 0xf8, 0xac, 0xd1,
  0x91, 0x01, 0x36, 0x02, 0x00, 0x20, 0x01, 0x28, 0x02, 0x00, 0x0b
};

}

BACKEND_TEST_CASE( "Test call_indirect patterns", "[call_indirect]") {
   wasm_allocator wa;
   using backend_t = psizam::backend<nullptr_t, TestType, dynamic_options>;

   backend_t bkend(call_indirect_wasm, get_wasm_allocator(), dynamic_options{});

   // add10(5) = 15
   CHECK(bkend.call_with_return("env", "test_indirect_0")->to_ui32() == 15u);

   // mul3(7) = 21
   CHECK(bkend.call_with_return("env", "test_indirect_1")->to_ui32() == 21u);

   // sub5(20) = 15
   CHECK(bkend.call_with_return("env", "test_indirect_2")->to_ui32() == 15u);

   // Loop: add10(10)+mul3(10)+sub5(10)+identity(10) = 20+30+5+10 = 65
   CHECK(bkend.call_with_return("env", "test_indirect_loop")->to_ui32() == 65u);

   // add2(100, 42) = 142
   CHECK(bkend.call_with_return("env", "test_indirect_2param")->to_ui32() == 142u);

   // nested: mul3(add10(5)) = 45
   CHECK(bkend.call_with_return("env", "test_nested_indirect")->to_ui32() == 45u);

   // alloc + indirect call: add10(7) = 17
   CHECK(bkend.call_with_return("env", "test_alloc_and_indirect")->to_ui32() == 17u);

   // memory.grow + use: 0x12345678
   CHECK(bkend.call_with_return("env", "test_grow_and_use")->to_ui32() == 0x12345678u);
}

// gofractally/psizam#6: the two-byte reference types of the typed function
// references proposal are read as funcref/externref by default; a host whose
// other engines refuse them turns them off, and then no place a type can
// appear accepts them.
namespace {

struct no_typed_references_options {
   std::uint32_t max_stack_bytes = 1024 * 1024;
   static constexpr std::uint32_t max_func_local_bytes = 1024 * 1024;
   static constexpr bool enable_typed_references = false;
};

// (func block (result (ref func)) unreachable end drop)
wasm_code typed_ref_block_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
   0x03, 0x02, 0x01, 0x00,
   0x0a, 0x0a, 0x01, 0x08, 0x00, 0x02, 0x64, 0x70, 0x00, 0x0b, 0x1a, 0x0b};
// (table 1 (ref null func))
wasm_code typed_ref_table_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   0x04, 0x05, 0x01, 0x63, 0x70, 0x00, 0x01};
// (type (func)) (func ref.null 0 drop)
wasm_code typed_ref_null_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
   0x03, 0x02, 0x01, 0x00,
   0x0a, 0x07, 0x01, 0x05, 0x00, 0xd0, 0x00, 0x1a, 0x0b};

}

TEST_CASE("Typed reference encodings are refused when the option is off", "[typed_references]") {
   using lenient_t = psizam::backend<nullptr_t, psizam::interpreter, dynamic_options>;
   using strict_t  = psizam::backend<nullptr_t, psizam::interpreter, no_typed_references_options>;
   // Accepted by default, as before.
   CHECK_NOTHROW(lenient_t(typed_ref_block_wasm, get_wasm_allocator(), dynamic_options{}));
   CHECK_NOTHROW(lenient_t(typed_ref_table_wasm, get_wasm_allocator(), dynamic_options{}));
   // Refused in a block type, in a declaration and as ref.null's heap type.
   CHECK_THROWS_AS(strict_t(typed_ref_block_wasm, get_wasm_allocator(), no_typed_references_options{}),
                   wasm_parse_exception);
   CHECK_THROWS_AS(strict_t(typed_ref_table_wasm, get_wasm_allocator(), no_typed_references_options{}),
                   wasm_parse_exception);
   CHECK_THROWS_AS(strict_t(typed_ref_null_wasm, get_wasm_allocator(), no_typed_references_options{}),
                   wasm_parse_exception);
}

// gofractally/psizam#7: modules another engine refuses. Each is a whole
// module with two empty exported functions `start` and `called`.
namespace {

std::vector<uint8_t> from_hex(std::string_view hex) {
   std::vector<uint8_t> out;
   auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
   for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
      out.push_back(static_cast<uint8_t>(nibble(hex[i]) * 16 + nibble(hex[i + 1])));
   return out;
}

struct strict_host_options {
   std::uint32_t max_stack_bytes = 1024 * 1024;
   static constexpr std::uint32_t max_func_local_bytes = 1024 * 1024;
   static constexpr bool enable_exception_handling    = false;
   static constexpr bool enable_typed_references      = false;
   static constexpr bool enable_const_defined_globals = false;
};

// Invalid under every version of the specification.
const char* ref_func_out_of_range =
   "0061736d01000000010d0360017e0060027e7e0060000003030200010606017000d2090b07120205737461727400000663616c6c656400010a070202000b02000b";
const char* funcref_global_null_extern =
   "0061736d01000000010d0360017e0060027e7e0060000003030200010606017000d06f0b07120205737461727400000663616c6c656400010a070202000b02000b";
// Forms of the exception handling proposal.
const char* legacy_try =
   "0061736d01000000010d0360017e0060027e7e0060000003040300010207120205737461727400000663616c6c656400010a0d0302000b02000b050006400b0b";
const char* exnref_local =
   "0061736d01000000010d0360017e0060027e7e0060000003040300010207120205737461727400000663616c6c656400010a0c0302000b02000b040101690b";
const char* exnref_block =
   "0061736d01000000010d0360017e0060027e7e0060000003040300010207120205737461727400000663616c6c656400010a0f0302000b02000b07000269000b1a0b";
// global.get of a defined global in a constant expression.
const char* defined_global_in_global =
   "0061736d01000000010d0360017e0060027e7e006000000303020001060b027f0041010b7f0023000b07120205737461727400000663616c6c656400010a070202000b02000b";
const char* defined_global_in_elem =
   "0061736d01000000010d0360017e0060027e7e0060000003030200010606017000d0700b07120205737461727400000663616c6c6564000109070105700123000b0a070202000b02000b";

}

TEST_CASE("The validator refuses what other engines refuse", "[validator_gaps]") {
   using lenient_t = psizam::backend<nullptr_t, psizam::interpreter, dynamic_options>;
   using strict_t  = psizam::backend<nullptr_t, psizam::interpreter, strict_host_options>;
   auto lenient = [](const char* hex) {
      auto code = from_hex(hex);
      lenient_t b(code, get_wasm_allocator(), dynamic_options{});
   };
   auto strict = [](const char* hex) {
      auto code = from_hex(hex);
      strict_t b(code, get_wasm_allocator(), strict_host_options{});
   };
   // Never valid: refused whatever the options.
   CHECK_THROWS_AS(lenient(ref_func_out_of_range), wasm_parse_exception);
   CHECK_THROWS_AS(lenient(funcref_global_null_extern), wasm_parse_exception);
   CHECK_THROWS_AS(strict(ref_func_out_of_range), wasm_parse_exception);
   CHECK_THROWS_AS(strict(funcref_global_null_extern), wasm_parse_exception);
   // Proposal forms: refused when the proposal is off.
   CHECK_THROWS_AS(strict(legacy_try), wasm_parse_exception);
   CHECK_THROWS_AS(strict(exnref_local), wasm_parse_exception);
   CHECK_THROWS_AS(strict(exnref_block), wasm_parse_exception);
   CHECK_THROWS_AS(strict(defined_global_in_global), wasm_parse_exception);
   CHECK_THROWS_AS(strict(defined_global_in_elem), wasm_parse_exception);
   // Still accepted by default.
   CHECK_NOTHROW(lenient(defined_global_in_global));
   CHECK_NOTHROW(lenient(defined_global_in_elem));
}

// gofractally/psizam#3: a function with more values live at once than a
// 16-bit spill slot index can number. JIT2 returned a wrong result past
// 32,767 (the index wrapped and two values shared a slot).
namespace {

void leb_u(std::vector<uint8_t>& out, uint32_t v) {
   do {
      uint8_t b = v & 0x7f;
      v >>= 7;
      out.push_back(v ? (b | 0x80) : b);
   } while (v);
}
void leb_s(std::vector<uint8_t>& out, int32_t v) {
   for (;;) {
      uint8_t b = v & 0x7f;
      v >>= 7;
      if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) {
         out.push_back(b);
         return;
      }
      out.push_back(b | 0x80);
   }
}

// (func (export "sum") (result i32) (local i32)
//    (i32.add (local.get 0) (i32.const k)) for k = 0 .. n-1, all left on the
//    stack: n computed values live at once, then n-1 additions.
std::vector<uint8_t> many_live_values(uint32_t n) {
   std::vector<uint8_t> body = {0x01, 0x01, 0x7f};  // one i32 local
   for (uint32_t i = 0; i < n; ++i) {
      body.push_back(0x20);
      body.push_back(0x00);
      body.push_back(0x41);
      leb_s(body, static_cast<int32_t>(i));
      body.push_back(0x6a);
   }
   for (uint32_t i = 1; i < n; ++i)
      body.push_back(0x6a);
   body.push_back(0x0b);
   std::vector<uint8_t> code;
   leb_u(code, 1);
   leb_u(code, static_cast<uint32_t>(body.size()));
   code.insert(code.end(), body.begin(), body.end());
   std::vector<uint8_t> m = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
                             0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,        // type () -> i32
                             0x03, 0x02, 0x01, 0x00,                          // one function
                             0x07, 0x07, 0x01, 0x03, 's',  'u',  'm',  0x00, 0x00};  // export
   m.push_back(0x0a);
   leb_u(m, static_cast<uint32_t>(code.size()));
   m.insert(m.end(), code.begin(), code.end());
   return m;
}

struct deep_stack_options {
   std::uint32_t max_stack_bytes = 8 * 1024 * 1024;
   static constexpr std::uint32_t max_func_local_bytes = 8 * 1024 * 1024;
};

}

BACKEND_TEST_CASE("More values live at once than 16 bits can number", "[spill_slots]") {
   using backend_t = psizam::backend<nullptr_t, TestType, deep_stack_options>;
   for (uint32_t n : {1000u, 32000u, 33000u, 40000u, 70000u}) {
      auto     code = many_live_values(n);
      backend_t bkend(code, get_wasm_allocator(), deep_stack_options{});
      // 0 + 1 + ... + (n-1), modulo 2^32
      const uint32_t expected = static_cast<uint32_t>(uint64_t(n) * (n - 1) / 2);
      INFO("values " << n);
      CHECK(bkend.call_with_return("env", "sum")->to_ui32() == expected);
   }
}
