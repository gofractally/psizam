#include <psizam/backend.hpp>
#include <catch2/catch.hpp>
#include <cstring>

struct backtrace_options : psizam::default_options {
   psizam::fp_mode fp = psizam::fp_mode::softfloat;
};
TEST_CASE("JIT1 backtrace preserves every SIMD result lane", "[jit1][backtrace][simd]") {
   using namespace psizam;
   auto wasm = read_wasm(JIT1_BACKTRACE_WASM);
   wasm_allocator allocator;
   backtrace_options options; options.gas_metering = false;
   backend<std::nullptr_t, jit_profile, backtrace_options> engine(wasm, &allocator, options);
   auto vector = [](float a, float b, float c, float d) {
      float lanes[4]{a,b,c,d}; v128_t result;
      std::memcpy(&result, lanes, sizeof(result)); return result;
   };
   auto a = vector(-1.5f, -2.5f, -3.5f, -4.5f);
   auto b = vector(10, 20, 30, 40);
   auto c = vector(1, 2, 3, 4);
   CHECK(engine.call_with_return("env", "unary", a)->to_v128() == vector(1.5f,2.5f,3.5f,4.5f));
   CHECK(engine.call_with_return("env", "binary", a, b)->to_v128() == vector(8.5f,17.5f,26.5f,35.5f));
   CHECK(engine.call_with_return("env", "ternary", a, b, c)->to_v128() == vector(-14,-48,-102,-176));
}
