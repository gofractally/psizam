#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>
#include <psizam/detail/llvm_aot_compiler.hpp>
#include <psizam/detail/ir_writer_llvm_aot.hpp>
#include <psizam/detail/parser.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using psizam::detail::llvm_aot_compile;

struct input {
   std::unique_ptr<llvm::LLVMContext> context = std::make_unique<llvm::LLVMContext>();
   std::unique_ptr<llvm::Module> ir = std::make_unique<llvm::Module>("physical-frame-fixture", *context);
};

// Compiler correctness fixtures only. They establish metadata/probe behavior,
// not WASM logical charges or performance.
input frame_fixture(bool large = false, bool dynamic = false, bool non_entry = false,
                    bool missing = false) {
   input result;
   auto& context = *result.context;
   auto* i64 = llvm::Type::getInt64Ty(context);
   auto* pointer = llvm::PointerType::getUnqual(context);
   auto* type = llvm::FunctionType::get(i64, {pointer, pointer, i64}, false);
   auto* body = llvm::Function::Create(type,
      missing ? llvm::Function::AvailableExternallyLinkage : llvm::Function::ExternalLinkage,
      "wasm_func_0", result.ir.get());
   body->addFnAttr(llvm::Attribute::NoInline);
   auto* entry = llvm::BasicBlock::Create(context, "entry", body);
   llvm::IRBuilder<> builder(entry);
   if (non_entry) {
      auto* other = llvm::BasicBlock::Create(context, "scratch", body);
      builder.CreateBr(other);
      builder.SetInsertPoint(other);
   }
   auto* argument = body->getArg(2);
   if (large || dynamic || non_entry) {
      llvm::Value* count = builder.getInt64(16384);
      if (dynamic) count = argument;
      auto* scratch = builder.CreateAlloca(builder.getInt8Ty(), count);
      auto* index = builder.CreateAnd(argument, builder.getInt64(16383));
      auto* address = builder.CreateGEP(builder.getInt8Ty(), scratch, index);
      builder.CreateStore(builder.getInt8(42), address, true);
      auto* value = builder.CreateLoad(builder.getInt8Ty(), address, true);
      builder.CreateRet(builder.CreateZExt(value, i64));
   } else {
      builder.CreateRet(argument);
   }
   auto* wrapper = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                          "wasm_entry_0", result.ir.get());
   llvm::IRBuilder<> wrap(llvm::BasicBlock::Create(context, "entry", wrapper));
   auto* call = wrap.CreateCall(body, {wrapper->getArg(0), wrapper->getArg(1), wrapper->getArg(2)});
   wrap.CreateRet(call);
   return result;
}

auto compile(input fixture, std::string target) {
   psizam::module wasm;
   wasm.code.emplace_back();
   return llvm_aot_compile(std::move(fixture.context), std::move(fixture.ir), wasm, target, 2);
}

void maybe_dump(const psizam::detail::llvm_aot_result& result, const char* name) {
   if (auto* directory = std::getenv("PSIZAM_PHYSICAL_TEST_DUMP")) {
      auto path = std::filesystem::path(directory) / (std::string{name} + ".bin");
      std::ofstream output(path, std::ios::binary);
      REQUIRE(output.good());
      output.write(reinterpret_cast<const char*>(result.code.data()), result.code.size());
      REQUIRE(output.good());
   }
}
}

TEST_CASE("O2 ELF frame metadata covers static bodies and wrappers including zero frames",
          "[aot-physical][compiler]") {
   for (auto target : {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"}) {
      CAPTURE(target);
      auto empty = compile(frame_fixture(), target);
      INFO(empty.error);
      REQUIRE(empty.error.empty());
      CHECK(empty.native_stack_frames_complete);
      CHECK(empty.native_stack_probe_size == 4096);
      REQUIRE(empty.entry_stack_sizes.size() == 1);
      REQUIRE(empty.body_stack_sizes.size() == 1);
      CHECK(empty.body_stack_sizes[0] == 0);
      auto large = compile(frame_fixture(true), target);
      INFO(large.error);
      REQUIRE(large.error.empty());
      CHECK(large.native_stack_frames_complete);
      CHECK(large.body_stack_sizes[0] >= 16384);
      CHECK(large.function_offsets[0].second > 0);
      CHECK(large.body_offsets[0].second > 0);
      maybe_dump(large, target);
   }
}

TEST_CASE("O2 AOT compilation fails closed on dynamic allocations and missing frame coverage",
          "[aot-physical][compiler]") {
   for (auto target : {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"}) {
      CAPTURE(target);
      for (auto fixture : {0, 1}) {
         auto invalid = compile(frame_fixture(false, fixture == 0, fixture == 1), target);
         CHECK(invalid.error.find("unbounded native stack allocation") != std::string::npos);
         CHECK_FALSE(invalid.native_stack_frames_complete);
      }
      auto missing = compile(frame_fixture(false, false, false, true), target);
      CHECK_FALSE(missing.error.empty());
      CHECK_FALSE(missing.native_stack_frames_complete);
      auto disabled = frame_fixture();
      disabled.ir->getFunction("wasm_func_0")->addFnAttr("no-stack-arg-probe");
      auto no_probes = compile(std::move(disabled), target);
      CHECK(no_probes.error.find("conflicting native stack probe") != std::string::npos);
      CHECK_FALSE(no_probes.native_stack_frames_complete);
      auto split = frame_fixture();
      split.ir->getFunction("wasm_func_0")->setSection(".text.extra");
      auto unretained = compile(std::move(split), target);
      CHECK(unretained.error.find("retained code section") != std::string::npos);
      CHECK_FALSE(unretained.native_stack_frames_complete);
      for (auto invalid_name : {"wasm_func_4294967296", "wasm_func_100", "wasm_func_bad"}) {
         auto invalid_index = frame_fixture();
         invalid_index.ir->getFunction("wasm_func_0")->setName(invalid_name);
         auto result = compile(std::move(invalid_index), target);
         CHECK(result.error.find("invalid AOT function index") != std::string::npos);
         CHECK_FALSE(result.native_stack_frames_complete);
      }
      auto unpaired = frame_fixture();
      unpaired.ir->getFunction("wasm_entry_0")->eraseFromParent();
      auto no_wrapper = compile(std::move(unpaired), target);
      CHECK(no_wrapper.error.find("missing AOT body or entry wrapper") != std::string::npos);
      CHECK_FALSE(no_wrapper.native_stack_frames_complete);
      auto aligned = frame_fixture(true);
      auto* allocation = llvm::cast<llvm::AllocaInst>(
         &aligned.ir->getFunction("wasm_func_0")->getEntryBlock().front());
      allocation->setAlignment(llvm::Align(64));
      auto bad_alignment = compile(std::move(aligned), target);
      CHECK(bad_alignment.error.find("unsupported native stack alignment") != std::string::npos);
      CHECK_FALSE(bad_alignment.native_stack_frames_complete);
      auto aligned_function = frame_fixture();
      aligned_function.ir->getFunction("wasm_func_0")->addFnAttr(
         llvm::Attribute::getWithStackAlignment(*aligned_function.context, llvm::Align(64)));
      auto bad_function_alignment = compile(std::move(aligned_function), target);
      CHECK(bad_function_alignment.error.find("unsupported native stack alignment") != std::string::npos);
      CHECK_FALSE(bad_function_alignment.native_stack_frames_complete);
   }
}

TEST_CASE("WASM loop scratch has complete frame metadata in serial and batch compilers",
          "[aot-physical][compiler]") {
   struct options : psizam::default_options { uint32_t compile_threads = 0; };
   for (auto threads : {0u, 2u}) {
      for (auto target : {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"}) {
       for (bool backtrace : {false, true}) {
        for (bool gas : {false, true}) {
         CAPTURE(threads, target, backtrace, gas);
         psizam::module module;
         psizam::detail::pzam_compile_result result;
         result.target_triple = target;
         result.opt_level = 2;
         auto wasm = psizam::read_wasm(PSIZAM_AOT_PHYSICAL_WASM);
         psizam::detail::null_debug_info debug;
         auto settings = options{.compile_threads = threads};
         settings.gas_metering = gas;
         {
            psizam::detail::binary_parser<psizam::detail::ir_writer_llvm_aot, options,
                                         psizam::detail::null_debug_info>
               parser(module.allocator, settings, backtrace, false);
            parser.set_compile_result(&result);
            parser.parse_module(wasm, module, debug);
         }
         INFO(result.error);
         REQUIRE(result.error.empty());
         CHECK(result.native_stack_frames_complete);
         CHECK(result.native_stack_probe_size == 4096);
         CHECK(result.entry_stack_sizes.size() == 2);
         CHECK(result.body_stack_sizes.size() == 2);
         CHECK(result.body_stack_sizes[0] > 0);
        }
       }
      }
   }
}

TEST_CASE("AOT native ABI identity requires matching typed call conventions", "[aot-physical][abi]") {
   for (auto target : {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"}) {
      CAPTURE(target);
      auto mismatch = frame_fixture();
      mismatch.ir->getFunction("wasm_func_0")->setCallingConv(llvm::CallingConv::Tail);
      auto bad = compile(std::move(mismatch), target);
      CHECK(bad.error.find("mismatched AOT guest call convention") != std::string::npos);
      CHECK_FALSE(bad.native_stack_frames_complete);

      auto matched = frame_fixture();
      matched.ir->getFunction("wasm_func_0")->setCallingConv(llvm::CallingConv::Tail);
      auto* call = llvm::cast<llvm::CallInst>(
         &matched.ir->getFunction("wasm_entry_0")->getEntryBlock().front());
      call->setCallingConv(llvm::CallingConv::Tail);
      auto good = compile(std::move(matched), target);
      INFO(good.error);
      REQUIRE(good.error.empty());
      CHECK(good.native_guest_abi == psizam::native_guest_abi::llvm_tailcc);
      CHECK(good.native_stack_frames_complete);
   }
}

TEST_CASE("Serial and batch AOT preserve the requested checked memory policy", "[aot-physical][policy]") {
   struct options : psizam::default_options {
      uint32_t compile_threads = 0;
      psizam::mem_safety memory_mode = psizam::mem_safety::guarded;
      psizam::checked_mode checked_kind = psizam::checked_mode::strict;
   };
   for (auto target : {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"})
      for (uint32_t threads : {0u, 4u}) {
         CAPTURE(target, threads);
         std::vector<uint8_t> guarded_code;
         for (auto mode : {psizam::mem_safety::guarded, psizam::mem_safety::checked}) {
            auto wasm = psizam::read_wasm(PSIZAM_AOT_PHYSICAL_WASM);
            psizam::module module;
            psizam::detail::pzam_compile_result result;
            result.target_triple = target;
            result.opt_level = 2;
            auto settings = options{.compile_threads = threads};
            settings.gas_metering = false;
            settings.memory_mode = mode;
            settings.checked_kind = psizam::checked_mode::strict;
            psizam::detail::null_debug_info debug;
            {
               psizam::detail::binary_parser<psizam::detail::ir_writer_llvm_aot, options>
                  parser(module.allocator, settings, false, false);
               parser.set_compile_result(&result);
               parser.parse_module(wasm, module, debug);
            }
            INFO(result.error);
            REQUIRE(result.error.empty());
            REQUIRE(result.native_stack_frames_complete);
            if (mode == psizam::mem_safety::guarded) guarded_code = result.code_blob;
            else CHECK(result.code_blob != guarded_code);
         }
      }
}

TEST_CASE("A missing WASM code section does not fabricate compiler measurements",
          "[aot-physical][compiler]") {
   psizam::module module;
   psizam::detail::pzam_compile_result result;
   result.target_triple = "aarch64-unknown-linux-gnu";
   psizam::wasm_code wasm{0,97,115,109,1,0,0,0};
   psizam::detail::null_debug_info debug;
   psizam::detail::binary_parser<psizam::detail::ir_writer_llvm_aot, psizam::default_options,
                                psizam::detail::null_debug_info>
      parser(module.allocator);
   parser.set_compile_result(&result);
   parser.parse_module(wasm, module, debug);
   CHECK(module.code.empty());
   CHECK(result.error.empty());
   CHECK(result.entry_stack_sizes.empty());
   CHECK(result.body_stack_sizes.empty());
   CHECK_FALSE(result.native_stack_frames_complete);
   CHECK(result.native_stack_probe_size == 0);
}

TEST_CASE("Native frame metadata survives a second parallel compilation batch",
          "[aot-physical][compiler]") {
   struct options : psizam::default_options { uint32_t compile_threads = 2; };
   psizam::module module;
   psizam::detail::pzam_compile_result result;
   result.target_triple = "aarch64-unknown-linux-gnu";
   result.opt_level = 2;
   auto wasm = psizam::read_wasm(PSIZAM_AOT_PHYSICAL_BATCH_WASM);
   psizam::detail::null_debug_info debug;
   {
      psizam::detail::binary_parser<psizam::detail::ir_writer_llvm_aot, options,
                                   psizam::detail::null_debug_info>
         parser(module.allocator);
      parser.set_compile_result(&result);
      parser.parse_module(wasm, module, debug);
   }
   INFO(result.error);
   REQUIRE(result.error.empty());
   REQUIRE(module.code.size() == 1025);
   CHECK(result.native_stack_frames_complete);
   CHECK(result.native_stack_probe_size == 4096);
   REQUIRE(result.entry_stack_sizes.size() == 1025);
   REQUIRE(result.body_stack_sizes.size() == 1025);
   CHECK(result.body_stack_sizes.back() > 0);
   CHECK(result.function_offsets.back().second > 0);
   CHECK(result.body_offsets.back().second > 0);
}

TEST_CASE("An ELF relocation outside the artifact's set fails the compile, not the process",
          "[aot-physical][compiler]") {
   // x86_64 shares the mapping code, but this fixture only draws an unlisted
   // relocation from the AArch64 backend.
   for (auto target : {"aarch64-unknown-linux-gnu"}) {
      CAPTURE(target);
      // A GOT-indirect call is a relocation kind the artifact loader does not apply.
      auto fixture = frame_fixture();
      auto& context = *fixture.context;
      auto* pointer = llvm::PointerType::getUnqual(context);
      auto* i64 = llvm::Type::getInt64Ty(context);
      auto* i32 = llvm::Type::getInt32Ty(context);
      auto* type = llvm::FunctionType::get(pointer, {pointer, i32, i64}, false);
      auto* memset = llvm::Function::Create(type, llvm::Function::ExternalLinkage, "memset", fixture.ir.get());
      memset->addFnAttr(llvm::Attribute::NonLazyBind);
      auto* body = fixture.ir->getFunction("wasm_func_0");
      auto* ret = body->getEntryBlock().getTerminator();
      llvm::IRBuilder<> builder(ret);
      builder.CreateCall(memset, {body->getArg(1), builder.getInt32(0), body->getArg(2)});
      auto result = compile(std::move(fixture), target);
      INFO(result.error);
      CHECK(result.error.find("Unsupported aarch64 ELF relocation type") != std::string::npos);
   }
}
