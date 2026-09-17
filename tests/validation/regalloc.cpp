#include <psizam/detail/jit_regalloc.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace psizam;
using namespace psizam::detail;

static void require(bool condition, const char* message) {
   if (!condition) throw std::runtime_error(message);
}

template<bool AArch64>
static void check(bool with_calls) {
   // Repeated disjoint groups require far more than twelve total XMM spills.
   // Mixed scalar/vector pressure also checks that the two types cannot own
   // the same physical register while their lifetimes overlap.
   std::vector<ir_inst> instructions(640);
   for (auto& inst : instructions) inst.opcode = ir_op::nop;
   std::vector<ir_live_interval> intervals;
   for (uint32_t group = 0; group < 32; ++group) {
      if (with_calls) instructions[group * 20 + 5].opcode = ir_op::call;
      for (uint32_t n = 0; n < 40; ++n) {
         ir_live_interval iv{};
         iv.vreg = intervals.size();
         iv.start = group * 20;
         iv.end = iv.start + 10;
         iv.phys_reg = iv.phys_xmm = iv.spill_slot = -1;
         iv.type = n < 28 ? types::i64 : n < 34 ? types::f64 : types::v128;
         intervals.push_back(iv);
      }
   }
   ir_function func{};
   func.insts = instructions.data();
   func.inst_count = instructions.size();
   func.intervals = intervals.data();
   func.next_vreg = func.interval_count = intervals.size();
   jit_regalloc::allocate_registers<AArch64>(func);
   for (size_t i = 0; i < intervals.size(); ++i) {
      const auto& a = intervals[i];
      require(a.phys_reg >= 0 || a.phys_xmm >= 0 || a.spill_slot >= 0,
              "live value has no storage");
      require(a.phys_reg < (AArch64 ? 21 : 9), "wrong target register bank");
      if (with_calls) {
         require(a.phys_xmm < 0, "XMM value does not survive a call");
         require(a.phys_reg < 0 || a.phys_reg >= (AArch64 ? 14 : 4),
                 "caller-saved value does not survive a call");
      }
      for (size_t j = 0; j < i; ++j) {
         const auto& b = intervals[j];
         if (a.start > b.end || b.start > a.end) continue;
         require(a.phys_reg < 0 || a.phys_reg != b.phys_reg,
                 "overlapping values share a GPR");
         require(a.phys_xmm < 0 || a.phys_xmm != b.phys_xmm,
                 "overlapping values share an XMM register");
         if (a.spill_slot >= 0 && b.spill_slot >= 0) {
            const int a_end = a.spill_slot + (a.type == types::v128 ? 2 : 1);
            const int b_end = b.spill_slot + (b.type == types::v128 ? 2 : 1);
            require(a_end <= b.spill_slot || b_end <= a.spill_slot,
                    "overlapping values share a spill slot");
         }
      }
   }
}

int main() {
   try {
      check<false>(false);
      check<false>(true);
      check<true>(false);
      check<true>(true);
      std::cout << "Register reuse, interference, call survival and both target banks passed\n";
      return 0;
   } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return 1;
   }
}
