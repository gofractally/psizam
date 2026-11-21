#include <psizam/detail/signals.hpp>
#include <chrono>
#include <csignal>
#include <thread>
#include <iostream>

#include <catch2/catch.hpp>

struct test_exception {};

TEST_CASE("Testing signals", "[invoke_with_signal_handler]") {
   bool okay = false;
   psizam::growable_allocator code_alloc;
   psizam::wasm_allocator wasm_alloc;
   try {
      psizam::detail::invoke_with_signal_handler([&]() {
         volatile auto i = *wasm_alloc.get_base_ptr<unsigned char>();
      }, [](int sig) {
         throw test_exception{};
      }, code_alloc, &wasm_alloc);
   } catch(test_exception&) {
      okay = true;
   }
   CHECK(okay);
}

TEST_CASE("Testing throw", "[signal_handler_throw]") {
   psizam::growable_allocator code_alloc;
   psizam::wasm_allocator wasm_alloc;
   CHECK_THROWS_AS(psizam::detail::invoke_with_signal_handler([](){
      psizam::detail::signal_throw<psizam::wasm_exit_exception>( "Exiting" );
   }, [](int){}, code_alloc, &wasm_alloc), psizam::wasm_exit_exception);
}

static volatile sig_atomic_t sig_handled;

static void handle_signal(int sig) {
   sig_handled = 42 + sig;
}

static void handle_signal_sigaction(int sig, siginfo_t* info, void* uap) {
   sig_handled = 142 + sig;
}

TEST_CASE("Test signal handler forwarding", "[signal_handler_forward]") {
   // reset backup signal handlers
   auto guard = psizam::scope_guard{[]{
      std::signal(SIGSEGV, SIG_DFL);
      std::signal(SIGBUS, SIG_DFL);
      std::signal(SIGFPE, SIG_DFL);
      psizam::detail::setup_signal_handler_impl(); // This is normally only called once
   }};
   {
      std::signal(SIGSEGV, &handle_signal);
      std::signal(SIGBUS, &handle_signal);
      std::signal(SIGFPE, &handle_signal);
      psizam::detail::setup_signal_handler_impl();
      sig_handled = 0;
      std::raise(SIGSEGV);
      CHECK(sig_handled == 42 + SIGSEGV);
#ifndef __linux__
      sig_handled = 0;
      std::raise(SIGBUS);
      CHECK(sig_handled == 42 + SIGBUS);
#endif
      sig_handled = 0;
      std::raise(SIGFPE);
      CHECK(sig_handled == 42 + SIGFPE);
   }
   {
      struct sigaction sa;
      sa.sa_sigaction = &handle_signal_sigaction;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_NODEFER | SA_SIGINFO;
      sigaction(SIGSEGV, &sa, nullptr);
      sigaction(SIGBUS, &sa, nullptr);
      sigaction(SIGFPE, &sa, nullptr);
      psizam::detail::setup_signal_handler_impl();
      sig_handled = 0;
      std::raise(SIGSEGV);
      CHECK(sig_handled == 142 + SIGSEGV);
#ifndef __linux__
      sig_handled = 0;
      std::raise(SIGBUS);
      CHECK(sig_handled == 142 + SIGBUS);
#endif
      sig_handled = 0;
      std::raise(SIGFPE);
      CHECK(sig_handled == 142 + SIGFPE);
   }
}

TEST_CASE("Trap entry restores nested state and interrupted signal masks", "[call-entry][signals]") {
   using namespace psizam::detail;
   setup_signal_handler();
   sigset_t original;
   REQUIRE(pthread_sigmask(SIG_SETMASK, nullptr, &original) == 0);
   auto restore_mask = psizam::scope_guard{[&] { pthread_sigmask(SIG_SETMASK, &original, nullptr); }};
   psizam::growable_allocator outer_code, inner_code;
   psizam::wasm_allocator outer_memory, inner_memory;
   auto* original_trap = trap_jmp_ptr;
   for (bool block_prof : {false, true}) {
      sigset_t desired = original;
      sigaddset(&desired, SIGUSR2);
      if (block_prof) sigaddset(&desired, SIGPROF);
      else sigdelset(&desired, SIGPROF);
      REQUIRE(pthread_sigmask(SIG_SETMASK, &desired, nullptr) == 0);
      auto check_mask = [&] {
         sigset_t actual;
         REQUIRE(pthread_sigmask(SIG_SETMASK, nullptr, &actual) == 0);
         for (int signal = 1; signal < NSIG; ++signal)
            CHECK(sigismember(&actual, signal) == sigismember(&desired, signal));
      };
      for (int repeat = 0; repeat < 3; ++repeat) {
         invoke_with_signal_handler([&] {
            auto* outer_trap = trap_jmp_ptr;
            const auto outer_range = memory_range;
            CHECK_THROWS_AS(invoke_with_signal_handler([&] {
               volatile auto value = *inner_memory.get_base_ptr<unsigned char>();
               (void)value;
            }, [](int) { throw test_exception{}; }, inner_code, &inner_memory), test_exception);
            CHECK(trap_jmp_ptr == outer_trap);
            CHECK(memory_range.data() == outer_range.data());
            CHECK(memory_range.size() == outer_range.size());
            check_mask();
            CHECK_THROWS_AS(invoke_with_signal_handler([] {
               longjmp_on_exception([] { throw test_exception{}; });
            }, [](int) {}, inner_code, &inner_memory), test_exception);
            CHECK(trap_jmp_ptr == outer_trap);
            CHECK(saved_exception == nullptr);
            check_mask();
         }, [](int) { FAIL("unexpected outer signal"); }, outer_code, &outer_memory);
         CHECK(trap_jmp_ptr == original_trap);
         check_mask();
      }
   }
}

TEST_CASE("Explicit trap observers restore nested scopes and executing-thread TLS",
          "[signal_handler_throw][trap_observer]") {
   using namespace psizam::detail;
   REQUIRE(trap_observer == nullptr);
   REQUIRE(trap_observer_data == nullptr);
   unsigned outer   = 0;
   unsigned inner   = 0;
   auto     observe = +[](const char*, void* data) noexcept { ++*static_cast<unsigned*>(data); };
   {
      scoped_trap_observer first(observe, &outer);
      REQUIRE(trap_observer_data == &outer);
      {
         scoped_trap_observer second(observe, &inner);
         trap_observer("inner", trap_observer_data);
      }
      CHECK(trap_observer == observe);
      CHECK(trap_observer_data == &outer);
      bool        isolated = false;
      std::thread worker([&] {
         isolated = trap_observer == nullptr && trap_observer_data == nullptr;
         scoped_trap_observer thread_scope(observe, &inner);
         trap_observer("worker", trap_observer_data);
      });
      worker.join();
      CHECK(isolated);
      trap_observer("outer", trap_observer_data);
   }
   CHECK(outer == 1);
   CHECK(inner == 2);
   CHECK(trap_observer == nullptr);
   CHECK(trap_observer_data == nullptr);
}

TEST_CASE("Explicit trap diagnostics preserve original errors and allocator ownership",
          "[signal_handler_throw][trap_observer]") {
   using namespace psizam::detail;
   psizam::growable_allocator code;
   psizam::wasm_allocator     memory;
   struct observations {
      unsigned                          calls             = 0;
      bool                              allocator_matches = false;
      const psizam::growable_allocator* expected;
   } result{ 0, false, &code };
   {
      scoped_trap_observer observer(
            +[](const char*, void* data) noexcept {
               auto& value = *static_cast<observations*>(data);
               ++value.calls;
               value.allocator_matches = active_code_allocator == value.expected;
               try {
                  throw std::runtime_error("diagnostic failed");
               } catch (...) {}
            },
            &result);
      CHECK_THROWS_AS(
            invoke_with_signal_handler([] { signal_throw<psizam::wasm_interpreter_exception>("original trap"); },
                                       [](int) {}, code, &memory),
            psizam::wasm_interpreter_exception);
      CHECK(result.calls == 1);
      CHECK(result.allocator_matches);
      // The signal path must not call arbitrary diagnostic code.
      CHECK_THROWS_AS(invoke_with_signal_handler(
                            [&] {
                               volatile auto ignored = *memory.get_base_ptr<unsigned char>();
                               (void)ignored;
                            },
                            [](int) { throw test_exception{}; }, code, &memory),
                      test_exception);
      CHECK(result.calls == 1);
   }
   CHECK(trap_observer == nullptr);
   CHECK(trap_observer_data == nullptr);
   CHECK(active_code_allocator == nullptr);
   CHECK_NOTHROW(invoke_with_signal_handler([] {}, [](int) {}, code, &memory));
}
