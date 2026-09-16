#include <psizam/detail/ir_writer.hpp>
#include <atomic>
#include <iostream>

// Repeated startup, two-phase dispatch and teardown exercise missed wakeups.
// CTest bounds this executable so a regression is a failure, not an infinite
// test run. Item stamps and phase totals also check work is neither lost nor
// duplicated while the pool is reused.
int main() {
   for (unsigned workers : {1u, 2u, 6u}) {
      for (unsigned pool = 0; pool < 30; ++pool) {
         psizam::detail::compile_reactor reactor(workers);
         for (unsigned batch = 1; batch <= 200; ++batch) {
            std::array<unsigned, 19> stamps{};
            const unsigned count = 1 + batch % stamps.size();
            std::atomic<unsigned> completed{0}, merged{0}, errors{0};
            reactor.run_batch(count,
               [&](unsigned worker, unsigned item) {
                  if (worker >= workers || item >= count) { ++errors; return; }
                  stamps[item] = batch;
                  ++completed;
               },
               [&] {
                  if (completed != count) ++errors;
                  for (unsigned i = 0; i < count; ++i)
                     if (stamps[i] != batch) ++errors;
               },
               [&](unsigned worker) {
                  if (worker >= workers || completed != count) ++errors;
                  ++merged;
               });
            if (errors || merged != workers) {
               std::cerr << "Compilation reactor lost work or crossed phase boundaries\n";
               return 1;
            }
         }
      }
   }
   std::cout << "18,000 two-phase batches passed across 90 thread-pool lifetimes\n";
}
