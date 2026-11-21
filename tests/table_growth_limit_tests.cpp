#include "utils.hpp"
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <array>
#include <string>

namespace {
struct table_options : psizam::default_options {
   uint32_t max_table_elements = 1;
   bool     limit_table_growth = true;
   table_options() { gas_metering = false; }
};
} // namespace

TEST_CASE("Inline table storage excludes the entire metadata tail", "[table-limit][table-tail]") {
   const auto bytes = psizam::wasm_allocator::inline_table_bytes();
   const auto entries = bytes / sizeof(psizam::table_entry);
   CHECK(bytes == psizam::wasm_allocator::table_size() - 20);
   CHECK(entries * sizeof(psizam::table_entry) <= bytes);
   CHECK((entries + 1) * sizeof(psizam::table_entry) > bytes);
   struct { std::array<psizam::table_type, 1> tables; } layout;
   layout.tables[0].limits = {true, static_cast<uint32_t>(entries), static_cast<uint32_t>(entries)};
   CHECK_FALSE(psizam::module::indirect_table_impl(layout, 0));
   ++layout.tables[0].limits.initial;
   ++layout.tables[0].limits.maximum;
   CHECK(psizam::module::indirect_table_impl(layout, 0));
}

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

TEMPLATE_TEST_CASE("Memory growth preserves the final fixed table entry", "[table-limit][table-tail]",
                   psizam::interpreter, psizam::jit, psizam::jit_profile, psizam::jit2) {
   for (auto size : {255u, 1023u, 4095u}) {
      CAPTURE(size);
      auto code = psizam::read_wasm(std::string(TABLE_TAIL_DIR) + "/table-tail-" + std::to_string(size) + ".wasm");
      table_options options;
      options.max_table_elements = 8192;
      psizam::backend<std::nullptr_t, TestType, table_options> engine(code, &wa, options);
      CHECK(engine.call_with_return("env", "last")->to_ui32() == 42);
      CHECK(engine.call_with_return("env", "grow-and-last")->to_ui32() == 42);
      CHECK(engine.call_with_return("env", "last")->to_ui32() == 42);
   }
}
