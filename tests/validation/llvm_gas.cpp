#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/detail/llvm_ir_translator.hpp>
#include <psizam/pzam_compile.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <fstream>
#include <iterator>
#include <thread>

using namespace psizam;
using namespace psizam::detail;

namespace {
wasm_code read_fixture() {
   std::ifstream input(LLVM_GAS_WASM, std::ios::binary);
   REQUIRE(input.good());
   return wasm_code{std::istreambuf_iterator<char>(input), {}};
}

// Exercise the real parser and translator without generating native code, so
// failed promotion is a test failure even when execution remains correct.
class inspect_writer : public ir_writer {
 public:
   static inline llvm_translate_options options;
   static inline std::unique_ptr<llvm_ir_translator> translator;

   inspect_writer(growable_allocator& alloc, size_t bytes, module& mod,
                  bool backtrace = false, bool stack_bytes = false,
                  uint32_t threads = 0)
       : ir_writer(alloc, bytes, mod, backtrace, stack_bytes, threads) {
      _skip_codegen = true;
   }
   void finalize(function_body& body) {
      if (!translator)
         translator = std::make_unique<llvm_ir_translator>(get_ir_module(), options);
      translator->translate_function(*_func);
      ir_writer::finalize(body);
   }
};

struct translated_fixture {
   psizam::module mod;
   std::unique_ptr<llvm_ir_translator> translator;
   translated_fixture(bool meter, llvm_translate_options options) {
      auto wasm = read_fixture();
      mod.allocator.use_default_memory();
      inspect_writer::options = options;
      inspect_writer::translator.reset();
      default_options parse_options;
      parse_options.gas_metering = meter;
      null_debug_info debug;
      binary_parser<inspect_writer, default_options, null_debug_info> parser(
          mod.allocator, parse_options);
      parser.parse_module(wasm, mod, debug);
      translator = std::move(inspect_writer::translator);
      REQUIRE(translator);
   }
};

size_t check_deadlines(llvm::Module& mod, bool meter) {
   size_t loads = 0, checks = 0;
   for (auto& fn : mod) {
      if (fn.isDeclaration()) continue;
      for (auto& block : fn) for (auto& inst : block) {
         if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            if (auto* callee = call->getCalledFunction(); callee &&
                callee->getName() == "__psizam_gas_exhausted_check") ++checks;
         }
         auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst);
         if (!load || !load->getType()->isIntegerTy(64)) continue;
         llvm::APInt offset(64, 0);
         auto* base = load->getPointerOperand()->stripAndAccumulateConstantOffsets(
             mod.getDataLayout(), offset, true);
         if (base != fn.getArg(0) ||
             offset.getZExtValue() != jit_execution_context<false>::gas_deadline_offset()) continue;
         ++loads;
         CHECK(load->getOrdering() == llvm::AtomicOrdering::Monotonic);
         CHECK(load->getAlign() == llvm::Align(8));
      }
   }
   // One entry check and one loop check, retained after optimization.
   CHECK(loads == (meter ? 2 : 0));
   CHECK(checks == loads);
   return loads;
}

template<class Call>
void check_runtime(gas_state& gas, Call call, uint64_t entry_cost) {
   constexpr uint32_t iterations = 1000;
   gas.consumed = 0;
   gas.deadline.store(gas_budget_unlimited, std::memory_order_relaxed);
   CHECK(call(iterations) == iterations);
   CHECK(gas.consumed == entry_cost + iterations);

   // Refill and return normally at repeated checkpoints; optimizations must
   // preserve both the callback and its ability to extend the deadline.
   unsigned refills = 0;
   gas.consumed = 0;
   gas.deadline.store(1, std::memory_order_relaxed);
   gas.user_data = &refills;
   gas.handler = +[](gas_state* gs, void* data) {
      ++*static_cast<unsigned*>(data);
      gs->deadline.store(gs->consumed + 17, std::memory_order_relaxed);
   };
   CHECK(call(iterations) == iterations);
   CHECK(gas.consumed == entry_cost + iterations);
   CHECK(refills == 1 + iterations / 17);

   // The first callback proves execution has started. It returns, and another
   // thread shortens the deadline while the guest loop is running.
   std::atomic<bool> started{false};
   gas.consumed = 0;
   gas.deadline.store(1, std::memory_order_relaxed);
   gas.user_data = &started;
   gas.handler = +[](gas_state* gs, void* data) {
      gs->deadline.store(gas_budget_unlimited, std::memory_order_relaxed);
      gs->handler = nullptr;
      static_cast<std::atomic<bool>*>(data)->store(true, std::memory_order_release);
   };
   {
      std::jthread watcher([&](std::stop_token stop) {
         while (!started.load(std::memory_order_acquire)) {
            if (stop.stop_requested()) return;
            std::this_thread::yield();
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(2));
         gas.deadline.store(0, std::memory_order_relaxed);
      });
      CHECK_THROWS_AS(call(1'000'000'000), wasm_gas_exhausted_exception);
   }
   CHECK(started.load());
   CHECK(gas.consumed > entry_cost);
   gas.handler = nullptr;
   gas.user_data = nullptr;
   gas.consumed = 0;
   gas.deadline.store(gas_budget_unlimited, std::memory_order_relaxed);
   CHECK(call(iterations) == iterations);
   CHECK(gas.consumed == entry_cost + iterations);
}
}

TEST_CASE("LLVM gas checks preserve local promotion", "[llvm][gas][promotion]") {
   const bool meter = GENERATE(false, true);
   const auto mem = GENERATE(mem_safety::guarded, mem_safety::checked);
   const int opt = GENERATE(0, 2);
   const bool per_function = GENERATE(false, true);
   const auto fp = GENERATE(fp_mode::fast, fp_mode::hw_deterministic, fp_mode::softfloat);
   CAPTURE(meter, int(mem), opt, per_function, int(fp));
   translated_fixture fixture(meter, {.fp=fp, .opt_level=opt,
       .per_function=per_function, .mem_mode=mem});
   size_t allocas = 0;
   for (auto& fn : *fixture.translator->get_module()) {
      if (fn.isDeclaration()) continue;
      for (auto& block : fn) for (auto& inst : block) {
         if (auto* slot = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
            ++allocas;
            INFO(slot->getName().str());
            CHECK(slot->isStaticAlloca());
            CHECK(&block == &fn.getEntryBlock());
         }
      }
   }
   REQUIRE(allocas > 0);
   fixture.translator->finalize();
   CHECK_FALSE(llvm::verifyModule(*fixture.translator->get_module()));
   for (auto& fn : *fixture.translator->get_module())
      for (auto& block : fn) for (auto& inst : block)
         CHECK_FALSE(llvm::isa<llvm::AllocaInst>(inst));
}

TEST_CASE("LLVM observes the atomic gas deadline at every checkpoint", "[llvm][gas][atomic]") {
   const bool meter = GENERATE(false, true);
   const int opt = GENERATE(0, 2);
   const bool per_function = GENERATE(false, true);
   translated_fixture fixture(meter, {.opt_level=opt, .per_function=per_function});
   check_deadlines(*fixture.translator->get_module(), meter);
   fixture.translator->finalize();
   check_deadlines(*fixture.translator->get_module(), meter);
}

TEST_CASE("LLVM gas accounting, refills and asynchronous interruption", "[llvm][gas][runtime]") {
   auto wasm = read_fixture();
   wasm_allocator wa;
   SECTION("fresh LLVM JIT") {
      default_options options;
      options.gas_metering = true;
      backend<std::nullptr_t, jit_llvm> engine(wasm, &wa, options);
      check_runtime(engine.get_context().gas(), [&](uint32_t count) {
         return engine.call_with_return("env", "run", count)->to_ui32();
      }, engine.get_module().code[0].wasm_body_bytes);
   }
   SECTION("saved LLVM O2 PZAM") {
      auto file = pzam_load(pzam_compile(wasm, {.tier=pzam_tier_flags::llvm_O2,
                                              .flags=pzam_compile_flags::gas_meter}));
      auto loaded = load_pzam(file);
      std::shared_ptr<void> owner(loaded.exec_code, [](void* p) { jit_allocator::instance().free(p); });
      jit_execution_context<> context(loaded.mod, 8192);
      context.set_wasm_allocator(&wa);
      context.reset();
      // The source parser supplies the expected entry charge; PZAM keeps the
      // compiled charge in code rather than retaining source body sizes.
      translated_fixture source(true, {});
      check_runtime(context.gas(), [&](uint32_t count) {
         return context.execute(nullptr, jit_visitor{nullptr},
             loaded.mod.get_exported_function("run"), count)->to_ui32();
      }, source.mod.code[0].wasm_body_bytes);
   }
}
