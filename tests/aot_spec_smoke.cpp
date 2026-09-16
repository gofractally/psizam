#include <catch2/catch.hpp>
#include "utils.hpp"
#include "wasm_config.hpp"
extern psizam::wasm_allocator wa;

TEST_CASE("AOT O2 PZAM reload executes scalar and SIMD exports", "[aot-smoke]") {
   using backend = psizam::backend<psizam::standalone_function_t, psizam::test::llvm_aot_o2_pzam>;
   {
      auto code = psizam::read_wasm(std::string(psizam::wasm_directory) + "i32.0.wasm");
      backend b(code, &wa);
      CHECK(b.call_with_return("env", "add", uint32_t{19}, uint32_t{23})->to_ui32() == 42);
      CHECK_THROWS(b.call_with_return("env", "div_s", uint32_t{1}, uint32_t{0}));
   }
   {
      auto code = psizam::read_wasm(std::string(psizam::wasm_directory) + "simd_i32x4_arith.0.wasm");
      backend b(code, &wa);
      CHECK(b.call_with_return("env", "i32x4.add", make_v128_i32(1,2,3,4), make_v128_i32(5,6,7,8))->to_v128()
            == make_v128_i32(6,8,10,12));
   }
   {
      psizam::wasm_code empty{0, 97, 115, 109, 1, 0, 0, 0};
      CHECK_NOTHROW(backend(empty, &wa));
   }
}
