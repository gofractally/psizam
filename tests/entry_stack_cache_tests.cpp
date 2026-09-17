#include <psizam/allocator.hpp>
#include <catch2/catch.hpp>
#include <stdexcept>

TEST_CASE("Entry stacks reuse mappings and lease distinct reentrant stacks", "[call-entry][allocator]") {
   psizam::entry_stack_cache cache;
   constexpr std::size_t large = 5*1024*1024;
   void* outer = nullptr;
   void* inner = nullptr;
   auto nested = [&] {
      cache.execute(large, [&](psizam::stack_allocator& a) {
         REQUIRE(a.top() != nullptr);
         if (outer) CHECK(a.top() == outer);
         outer = a.top();
         auto* marker = static_cast<unsigned char*>(a.top()) - 64;
         *marker = 123;
         cache.execute(large, [&](psizam::stack_allocator& b) {
            REQUIRE(b.top() != nullptr);
            REQUIRE(a.top() != b.top());
            if (inner) CHECK(b.top() == inner);
            inner = b.top();
         });
         CHECK(*marker == 123);
      });
   };
   nested();
   for (int i = 0; i < 10; ++i) nested();
   CHECK(cache.cached_stack_count() == 2);
   CHECK_THROWS_AS(cache.execute(large, [&](auto&) {
      cache.execute(large, [](auto&) { throw std::runtime_error("trap"); });
   }), std::runtime_error);
   nested(); // Exception unwinding released both leases.
   cache.execute(1024, [](auto& stack) { CHECK(stack.top() == nullptr); });
   CHECK(cache.cached_stack_count() == 2);
   cache.execute(large * 2, [](auto& stack) { CHECK(stack.top() != nullptr); });
   CHECK(cache.cached_stack_count() == 2);
}
