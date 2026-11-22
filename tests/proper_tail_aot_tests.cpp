#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/detail/ir_writer_llvm_aot.hpp>
#include <psizam/detail/parser.hpp>

namespace {
struct options : psizam::default_options {
   uint32_t max_stack_bytes = 65536;
   uint32_t compile_threads = 0;
   static constexpr uint32_t max_func_local_bytes = 1024 * 1024;
   static constexpr uint32_t stack_usage_for_call = 4096;
   static constexpr auto max_func_local_bytes_flags =
      psizam::max_func_local_bytes_flags_t::params |
      psizam::max_func_local_bytes_flags_t::locals |
      psizam::max_func_local_bytes_flags_t::stack;
   options() { gas_metering = false; }
};

void check(const char* path, const char* target, uint32_t threads) {
   auto wasm = psizam::read_wasm(path);
   psizam::module module;
   module.allocator.use_default_memory();
   psizam::detail::pzam_compile_result result;
   result.target_triple = target;
   result.opt_level = 2;
   options settings;
   settings.compile_threads = threads;
   psizam::detail::null_debug_info debug;
   {
      psizam::detail::binary_parser<psizam::detail::ir_writer_llvm_aot, options>
         parser(module.allocator, settings, true, true);
      parser.set_compile_result(&result);
      parser.parse_module(wasm, module, debug);
   }
   INFO(result.error);
   REQUIRE(result.error.empty());
   REQUIRE(result.native_guest_abi == psizam::native_guest_abi::llvm_tailcc);
   REQUIRE(result.native_stack_frames_complete);
   REQUIRE(result.native_stack_probe_size == 4096);
   REQUIRE(!result.code_blob.empty());
   REQUIRE(result.body_offsets.size() == module.code.size());
   REQUIRE(result.function_offsets.size() == module.code.size());
   REQUIRE(result.body_stack_sizes.size() == module.code.size());
   REQUIRE(result.entry_stack_sizes.size() == module.code.size());
   REQUIRE(result.import_thunk_offsets.size() == module.get_imported_functions_size());
   REQUIRE(result.import_thunk_stack_sizes.size() == module.get_imported_functions_size());
   auto range = [&](auto code) {
      CHECK(code.second > 0);
      CHECK(uint64_t(code.first) + code.second <= result.code_blob.size());
   };
   for (size_t i = 0; i < module.code.size(); ++i) {
      range(result.body_offsets[i]);
      range(result.function_offsets[i]);
      CHECK(module.code[i].native_stack_frame_complete);
      CHECK(module.code[i].native_stack_frame_bytes == result.body_stack_sizes[i]);
      CHECK(module.code[i].byte_stack_usage >= 16);
   }
   for (size_t i = 0; i < result.import_thunk_offsets.size(); ++i) {
      range(result.import_thunk_offsets[i]);
      CHECK(result.import_thunk_stack_sizes[i] != UINT64_MAX);
   }
   for (const auto& relocation : result.relocs) {
      if (relocation.symbol == psizam::detail::reloc_symbol::code_blob_self) {
         CHECK(relocation.addend >= 0);
         CHECK(uint32_t(relocation.addend) < result.code_blob.size());
      }
   }
}
}

TEST_CASE("Typed LLVM import targets have complete O2 native metadata", "[proper-tail][aot][metadata]") {
   for (auto target : {"aarch64-unknown-linux-gnu", "x86_64-unknown-linux-gnu"})
      for (uint32_t threads : {0u, 4u}) {
         CAPTURE(target, threads);
         check(PROPER_TAIL_WASM, target, threads);
         check(PROPER_TAIL_IMPORT_ONLY_WASM, target, threads);
      }
}

TEST_CASE("Typed imports resolve across separate LLVM compilation batches", "[proper-tail][aot][batch]") {
   for (auto target : {"aarch64-unknown-linux-gnu", "x86_64-unknown-linux-gnu"}) {
      CAPTURE(target);
      check(PROPER_TAIL_AOT_BATCH_WASM, target, 4);
   }
}
