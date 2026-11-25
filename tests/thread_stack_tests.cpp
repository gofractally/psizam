#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>

#include <exception>
#include <functional>
#include <pthread.h>
#include <stdexcept>
#include <thread>

namespace {
void ensure(bool condition, const char* message) {
   if (!condition) throw std::runtime_error(message);
}

// Catch's assertion machinery stays on the test thread; workers report errors
// through exception_ptr after joining.
void on_small_thread(std::size_t size, std::function<void()> function) {
   struct work {
      std::function<void()> function;
      std::exception_ptr error;
   } task{std::move(function), {}};
   pthread_attr_t attributes;
   REQUIRE(pthread_attr_init(&attributes) == 0);
   REQUIRE(pthread_attr_setstacksize(&attributes, size) == 0);
   pthread_t thread;
   const auto status = pthread_create(&thread, &attributes, +[](void* pointer) -> void* {
      auto& task = *static_cast<work*>(pointer);
      try { task.function(); } catch (...) { task.error = std::current_exception(); }
      return nullptr;
   }, &task);
   REQUIRE(pthread_attr_destroy(&attributes) == 0);
   REQUIRE(status == 0);
   REQUIRE(pthread_join(thread, nullptr) == 0);
   if (task.error) std::rethrow_exception(task.error);
}

void check_discovery(std::size_t expected_maximum = 0) {
   const auto bounds = psizam::detail::current_thread_stack_bounds();
   auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
   ensure(bounds.low < frame && frame < bounds.high, "thread frame outside discovered bounds");
   if (expected_maximum)
      // Darwin may include its page-aligned pthread bookkeeping in the size.
      ensure(bounds.high - bounds.low < expected_maximum + psizam::detail::native_stack_headroom,
             "discovered thread stack is too large");
   const auto available = psizam::detail::available_native_stack(bounds, frame);
   ensure(available < frame - bounds.low, "host headroom was not reserved");
   ensure(available <= psizam::detail::native_stack_borrow_limit, "native borrow cap exceeded");
}

struct host {
   std::function<std::int32_t(std::int32_t)> callback;
   std::int32_t observe(std::int32_t value) { return callback(value); }
};
struct options : psizam::default_options {
   std::uint32_t max_stack_bytes = 512*1024;
   std::uint32_t max_func_local_bytes = 1024*1024;
};

void register_host() {
   static const bool registered = [] {
      psizam::registered_host_functions<host>::add<&host::observe>("env", "observe");
      return true;
   }();
   (void)registered;
}

template<typename Impl>
struct runtime {
   using functions = psizam::registered_host_functions<host>;
   using backend_type = psizam::backend<functions, Impl, options>;
   psizam::wasm_allocator memory;
   psizam::wasm_code code = psizam::read_wasm(PSIZAM_THREAD_STACK_WASM);
   backend_type backend{code, nullptr, options{}};
   host h;
   psizam::detail::stack_manager stacks;

   runtime() {
      h.callback = [](std::int32_t value) { return value + 1; };
      backend.set_wasm_allocator(&memory);
      backend.initialize(stacks, &h);
   }
   auto explicit_call(std::string_view name, auto... args) {
      return backend.get_context().template execute<psizam::type_converter<host>>(
         stacks, &h, psizam::detail::jit_visitor{backend.get_context()},
         backend.get_module().get_exported_function(name), args...);
   }
   auto implicit_call(std::string_view name, auto... args) {
      return backend.call_with_return(h, "env", name, args...);
   }
};

template<typename Impl>
void check_guest_entries() {
   using runtime_type = runtime<Impl>;
   register_host();
   runtime_type rt;
   const auto native = psizam::detail::current_thread_stack_bounds();
   const auto on_alternate_stack = [&] {
      auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
      ensure(frame < native.low || frame >= native.high, "guest host callback used the small native stack");
      ensure(!psizam::detail::stack_guard_range.empty(), "alternate guard not registered");
   };
   unsigned callbacks = 0;
   rt.h.callback = [&](std::int32_t value) {
      on_alternate_stack();
      const auto guard = psizam::detail::stack_guard_range;
      volatile std::uint64_t marker = 0x1122334455667788ull;
      auto inner = rt.explicit_call("value", value);
      ensure(inner && inner->to_i32() == value + 1, "nested explicit return value");
      ensure(marker == 0x1122334455667788ull, "nested call damaged the suspended host stack");
      ensure(guard.data() == psizam::detail::stack_guard_range.data(), "nested guard was not restored");
      ++callbacks;
      return inner->to_i32();
   };
   for (int i = 0; i < 3; ++i) {
      auto explicit_result = rt.explicit_call("nested", std::int32_t{40});
      ensure(explicit_result && explicit_result->to_i32() == 42, "explicit guest result");
      auto implicit_result = rt.implicit_call("nested", std::int32_t{40});
      ensure(implicit_result && implicit_result->to_i32() == 42, "implicit guest result");
      bool trapped = false;
      try { rt.explicit_call("trap"); } catch (const psizam::exception&) { trapped = true; }
      ensure(trapped, "guest did not trap");
      ensure(rt.explicit_call("value", std::int32_t{41})->to_i32() == 42, "stack unusable after trap");
      ensure(rt.stacks.base == 0 && rt.stacks.size == 0, "manager state escaped entry scope");
   }
   ensure(callbacks == 6, "not all nested calls ran");

   // An entry point bound once: the export's index is resolved up front and
   // the call goes by index, on the alternate stack like a call by name.
   const auto nested = rt.backend.resolve_export("nested");
   for (int i = 0; i < 3; ++i)
      ensure(rt.backend.call_by_index(rt.stacks, rt.h, nested, std::int32_t{40}),
             "call by index on the alternate stack");
   ensure(callbacks == 9, "calls by index did not run the guest");
   ensure(rt.stacks.base == 0 && rt.stacks.size == 0, "manager state escaped a call by index");

   // The returned handle must own its mapping after get_typed_function returns.
   // A copied handle keeps it alive after the original handle is released.
   auto function = rt.backend.template get_typed_function<std::int32_t(std::int32_t)>("value");
   auto copy = function;
   function = {};
   rt.backend.scoped_execution([&] {
      ensure(copy(41) == 42, "returned typed handle lost its stack mapping");
   });
   psizam::stack_allocator borrowed(rt.backend.get_context().get_maximum_stack_size(), 0);
   auto borrowed_function = rt.backend.template get_typed_function<std::int32_t(std::int32_t)>("value", borrowed);
   rt.backend.scoped_execution([&] {
      ensure(borrowed_function(41) == 42, "caller-owned typed stack failed");
   });
}

struct llvm_entry_observation {
   psizam::detail::thread_stack_bounds native;
   bool called = false;
   bool fail = false;
};
thread_local llvm_entry_observation* llvm_observation = nullptr;

// This is an entry-ABI correctness fixture, not LLVM-generated code or a
// performance workload. Actual LLVM/AOT policy and byte accounting are separate.
std::int64_t llvm_entry_fixture(void*, void*, psizam::native_value* arguments) {
   auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
   auto& observation = *llvm_observation;
   ensure(frame < observation.native.low || frame >= observation.native.high,
          "LLVM entry ABI did not switch to the supplied stack");
   ensure(!psizam::detail::stack_guard_range.empty(), "LLVM entry guard was not registered");
   observation.called = true;
   if (observation.fail) throw std::runtime_error("entry fixture exception");
   return arguments[0].i32 + 1;
}
}

TEST_CASE("Native stack discovery is bounded and unknown stacks are not borrowed", "[thread-stack]") {
   using namespace psizam::detail;
   CHECK(available_native_stack({}, 0x100000) == 0);
   CHECK(available_native_stack({100, 99}, 100) == 0);
   CHECK(available_native_stack({100, 200}, 99) == 0);
   CHECK(available_native_stack({100, 200}, 200) == 0);
   CHECK(available_native_stack({100, 200}, 150) == 0);
   CHECK(available_native_stack({100, 100 + native_stack_headroom + 100},
                                100 + native_stack_headroom + 42) == 42);
   CHECK(available_native_stack({100, 100 + 2*native_stack_borrow_limit},
                                100 + native_stack_borrow_limit + native_stack_headroom + 1)
         == native_stack_borrow_limit);
   check_discovery();
   for (auto size : {256*1024u, 512*1024u}) {
      CAPTURE(size);
      on_small_thread(size, [=] { check_discovery(size); });
   }
   std::exception_ptr error;
   std::thread worker([&] { try { check_discovery(); } catch (...) { error = std::current_exception(); } });
   worker.join();
   if (error) std::rethrow_exception(error);
}

TEST_CASE("Small threads use guarded explicit and cached entry stacks", "[thread-stack]") {
   for (auto size : {256*1024u, 512*1024u}) {
      on_small_thread(size, [] {
         constexpr std::size_t requested = 1024*1024;
         psizam::stack_allocator direct(requested);
         ensure(direct.top() != nullptr, "direct allocator borrowed nonexistent native capacity");
         psizam::detail::stack_manager manager;
         manager.execute(requested, [&](auto& outer) {
            ensure(outer.top() != nullptr, "explicit manager borrowed nonexistent native capacity");
            const auto base = manager.base;
            bool threw = false;
            try {
               manager.execute(requested, [&](auto& inner) {
                  ensure(inner.top() && inner.top() != outer.top(), "nested entry reused a live stack");
                  throw std::runtime_error("host trap");
               });
            } catch (const std::runtime_error&) { threw = true; }
            ensure(threw && manager.base == base, "nested manager state was not restored");
         });
         ensure(manager.base == 0 && manager.size == 0, "outer manager state was not restored");
         psizam::entry_stack_cache cache;
         void* first = nullptr;
         for (int i = 0; i < 3; ++i) cache.execute(requested, [&](auto& stack) {
            ensure(stack.top() != nullptr, "implicit entry borrowed nonexistent native capacity");
            if (first) ensure(first == stack.top(), "entry mapping was not reused");
            first = stack.top();
         });
         ensure(cache.cached_stack_count() == 1, "implicit stack was not cached");
      });
   }
}

TEMPLATE_TEST_CASE("Small-thread native entries preserve nested calls traps and typed handles",
                   "[thread-stack][execution]", psizam::jit, psizam::jit2) {
   for (auto size : {256*1024u, 512*1024u}) {
      CAPTURE(size);
      on_small_thread(size, [] { check_guest_entries<TestType>(); });
   }
}

TEST_CASE("Typed LLVM entry ABI respects stack ownership and exception restoration",
          "[thread-stack][entry-abi]") {
   for (auto size : {256*1024u, 512*1024u}) {
      on_small_thread(size, [] {
         register_host();
         runtime<psizam::jit> rt;
         psizam::module image;
         image.code.emplace_back();
         image.code[0].jit_code_offset = reinterpret_cast<std::uintptr_t>(&llvm_entry_fixture);
         auto owner = std::make_shared<psizam::stack_allocator>(1024*1024, 0);
         using function_type = runtime<psizam::jit>::backend_type::typed_function<std::int32_t(std::int32_t)>;
         function_type function{&rt.backend.get_context(), image, 0, owner->top(), owner,
            {static_cast<std::byte*>(owner->guard_base()), owner->guard_size()}};
         owner.reset();
         llvm_entry_observation observation{psizam::detail::current_thread_stack_bounds()};
         llvm_observation = &observation;
         const auto original_guard = psizam::detail::stack_guard_range;
         rt.backend.scoped_execution([&] { ensure(function(41) == 42, "LLVM entry ABI return value"); });
         ensure(observation.called, "LLVM entry ABI fixture did not run");
         observation.fail = true;
         bool threw = false;
         try { rt.backend.scoped_execution([&] { function(41); }); }
         catch (const std::runtime_error& e) { threw = std::string_view{e.what()} == "entry fixture exception"; }
         ensure(threw, "LLVM entry exception was not propagated");
         ensure(psizam::detail::stack_guard_range.data() == original_guard.data(), "LLVM entry guard escaped exception");
         observation.fail = false;
         rt.backend.scoped_execution([&] { ensure(function(41) == 42, "LLVM entry could not be reused"); });
         llvm_observation = nullptr;
      });
   }
}
