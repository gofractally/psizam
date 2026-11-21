#include "utils.hpp"
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <string>

namespace {
struct table_options : psizam::default_options {
   uint32_t max_table_elements = 1;
   bool     limit_table_growth = true;
   table_options() { gas_metering = false; }
};
} // namespace

TEMPLATE_TEST_CASE("Optional table growth caps preserve compiled storage layout", "[table-limit]", psizam::interpreter,
                   psizam::jit, psizam::jit_profile, psizam::jit2) {
   for (auto file : { TABLE_LIMIT_WASM, TABLE_UNBOUNDED_WASM }) {
      INFO("fixture=" << file);
      {
         INFO("fixed capped table");
         auto                                                     code = psizam::read_wasm(file);
         table_options                                            options;
         psizam::backend<std::nullptr_t, TestType, table_options> engine(code, &wa, options);
         CHECK(engine.get_module().tables[0].limits.maximum == 1);
         CHECK(engine.call_with_return("env", "run")->to_ui32() == 42);
         CHECK(engine.call_with_return("env", "grow")->to_i32() == -1);
         CHECK(engine.call_with_return("env", "size")->to_ui32() == 1);
         CHECK(engine.call_with_return("env", "run")->to_ui32() == 42);
      }
      {
         INFO("growable capped table");
         auto          code = psizam::read_wasm(file);
         table_options options;
         options.max_table_elements = 2;
         psizam::backend<std::nullptr_t, TestType, table_options> engine(code, &wa, options);
         CHECK(engine.call_with_return("env", "grow")->to_i32() == 1);
         CHECK(engine.call_with_return("env", "run")->to_ui32() == 42);
         CHECK(engine.call_with_return("env", "size")->to_ui32() == 2);
         CHECK(engine.call_with_return("env", "grow")->to_i32() == -1);
         CHECK(engine.call_with_return("env", "run")->to_ui32() == 42);
      }
      {
         INFO("default initial-size policy");
         auto          code = psizam::read_wasm(file);
         table_options options;
         options.limit_table_growth = false;
         psizam::backend<std::nullptr_t, TestType, table_options> engine(code, &wa, options);
         CHECK(engine.call_with_return("env", "run")->to_ui32() == 42);
         CHECK(engine.call_with_return("env", "grow")->to_i32() == 1);
         CHECK(engine.call_with_return("env", "size")->to_ui32() == 2);
      }
   }
}
