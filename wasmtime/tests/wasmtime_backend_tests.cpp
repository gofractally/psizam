// Tests for the wasmtime execution backend: the templated host binding, error
// transport across wasm frames, limits, interruption, and float agreement
// with psizam's own engines.
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psizam/backend.hpp>
#include <psizam/wasmtime_backend.hpp>

#include <chrono>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include "float_parity_ops.hpp"

namespace wt = psizam::wasmtime;

namespace {

struct host {
   std::string        seen;
   int                notes = 0;
   wt::instance<host>* self = nullptr;

   uint32_t add(uint32_t a, uint32_t b) { return a + b; }
   uint64_t mix(uint64_t a, int32_t b, double c, float d)
   {
      return a + static_cast<uint64_t>(static_cast<int64_t>(b)) + static_cast<uint64_t>(c) +
             static_cast<uint64_t>(d);
   }
   double half(double x) { return x / 2; }
   void   note(std::span<const char> text)
   {
      ++notes;
      seen.assign(text.begin(), text.end());
   }
   void fill(std::span<char> out, uint32_t byte) { std::memset(out.data(), int(byte), out.size()); }
   void fail() { throw std::logic_error("host failed"); }
   // Calls back into the instance that is calling this function.
   uint32_t reenter(uint32_t n) { return self->call<uint32_t, uint32_t>("countdown", n); }
};

constexpr std::string_view service = R"((module
  (import "env" "add" (func $add (param i32 i32) (result i32)))
  (import "env" "mix" (func $mix (param i64 i32 f64 f32) (result i64)))
  (import "env" "half" (func $half (param f64) (result f64)))
  (import "env" "note" (func $note (param i32 i32)))
  (import "env" "fill" (func $fill (param i32 i32 i32)))
  (import "env" "fail" (func $fail))
  (import "env" "reenter" (func $reenter (param i32) (result i32)))
  (memory (export "memory") 1 4)
  (data (i32.const 16) "hello")
  (global $g (mut i32) (i32.const 0))
  (func (export "add3") (param i32 i32 i32) (result i32)
    (call $add (call $add (local.get 0) (local.get 1)) (local.get 2)))
  (func (export "mix") (param i64 i32 f64 f32) (result i64)
    (call $mix (local.get 0) (local.get 1) (local.get 2) (local.get 3)))
  (func (export "half") (param f64) (result f64) (call $half (local.get 0)))
  (func (export "note") (call $note (i32.const 16) (i32.const 5)))
  (func (export "note_end") (call $note (i32.const 65531) (i32.const 5)))
  (func (export "note_empty_far") (call $note (i32.const 0x7fffffff) (i32.const 0)))
  (func (export "note_past_end") (call $note (i32.const 65535) (i32.const 2)))
  (func (export "note_wrapping") (call $note (i32.const 0xffffffff) (i32.const 0xffffffff)))
  (func (export "fill") (result i32)
    (call $fill (i32.const 100) (i32.const 4) (i32.const 0x41))
    (i32.load (i32.const 100)))
  (func (export "fail") (global.set $g (i32.const 7)) (call $fail))
  (func (export "g") (result i32) (global.get $g))
  (func (export "countdown") (param i32) (result i32)
    (if (result i32) (i32.eqz (local.get 0))
      (then (i32.const 0))
      (else (i32.add (i32.const 1) (call $reenter (i32.sub (local.get 0) (i32.const 1)))))))
  (func (export "grow") (param i32) (result i32) (memory.grow (local.get 0)))
  (func (export "spin") (loop $l (br $l)))
  (func (export "unreachable") unreachable)
  (func (export "div") (param i32 i32) (result i32) (i32.div_s (local.get 0) (local.get 1)))
  (func (export "load") (param i32) (result i32) (i32.load (local.get 0)))
  (func $rec (export "rec") (call $rec))
))";

wt::runtime<host> make_runtime(const wt::engine_options& options = {})
{
   wt::runtime<host> rt(options);
   rt.add<&host::add>("env", "add")
       .add<&host::mix>("env", "mix")
       .add<&host::half>("env", "half")
       .add<&host::note>("env", "note")
       .add<&host::fill>("env", "fill")
       .add<&host::fail>("env", "fail")
       .add<&host::reenter>("env", "reenter");
   return rt;
}

struct fixture {
   wt::runtime<host>  rt = make_runtime();
   host               h;
   wt::instance<host> inst;
   explicit fixture(const wt::instance_limits& limits = {}) : inst(rt, rt.compile(service), h, limits)
   {
      h.self = &inst;
   }
};

wt::trap_kind trap_of(auto&& f)
{
   try
   {
      f();
   }
   catch (const wt::trap& t)
   {
      return t.kind;
   }
   FAIL("expected a trap");
   return wt::trap_kind::other;
}

}  // namespace

TEST_CASE("wasmtime: host functions receive and return every scalar type")
{
   fixture f;
   CHECK(f.inst.call<uint32_t, uint32_t, uint32_t, uint32_t>("add3", 1, 2, 3) == 6);
   CHECK(f.inst.call<uint32_t, uint32_t, uint32_t, uint32_t>("add3", 0xffffffffu, 1, 5) == 5);
   CHECK(f.inst.call<uint64_t, uint64_t, int32_t, double, float>("mix", 0x100000000ull, -1, 7.0,
                                                                 2.0f) == 0x100000008ull);
   CHECK(f.inst.call<double, double>("half", 3.0) == 1.5);
}

TEST_CASE("wasmtime: host spans are checked against linear memory")
{
   fixture f;
   f.inst.call("note");
   CHECK(f.h.seen == "hello");
   // The last five bytes of the only page are in bounds.
   CHECK_NOTHROW(f.inst.call("note_end"));
   // An empty span is accepted wherever it points.
   CHECK_NOTHROW(f.inst.call("note_empty_far"));
   CHECK(f.h.seen.empty());
   CHECK(f.h.notes == 3);
   CHECK_THROWS_AS(f.inst.call("note_past_end"), wt::host_memory_error);
   CHECK_THROWS_AS(f.inst.call("note_wrapping"), wt::host_memory_error);
   CHECK(f.h.notes == 3);
   CHECK(f.inst.call<uint32_t>("fill") == 0x41414141u);
   CHECK(f.inst.memory().size() == 65536);
   CHECK(std::string_view(f.inst.memory().data() + 16, 5) == "hello");
}

TEST_CASE("wasmtime: a host exception crosses wasm frames and the instance stays usable")
{
   fixture f;
   CHECK_THROWS_WITH(f.inst.call("fail"), "host failed");
   CHECK_THROWS_AS(f.inst.call("fail"), std::logic_error);
   // State written before the failing host call is still there.
   CHECK(f.inst.call<uint32_t>("g") == 7);
   CHECK(f.inst.call<uint32_t, uint32_t, uint32_t, uint32_t>("add3", 1, 1, 1) == 3);
}

TEST_CASE("wasmtime: a host function can re-enter the calling instance")
{
   fixture f;
   CHECK(f.inst.call<uint32_t, uint32_t>("countdown", 0) == 0);
   CHECK(f.inst.call<uint32_t, uint32_t>("countdown", 64) == 64);
}

TEST_CASE("wasmtime: memory growth stops at the instance limit")
{
   fixture f(wt::instance_limits{.max_memory_bytes = 2 * 65536});
   CHECK(f.inst.call<uint32_t, uint32_t>("grow", 1) == 1);
   CHECK(f.inst.call<uint32_t, uint32_t>("grow", 1) == 0xffffffffu);
   CHECK(f.inst.memory().size() == 2 * 65536);
}

TEST_CASE("wasmtime: an initial memory above the limit fails to instantiate")
{
   auto rt = make_runtime();
   host h;
   CHECK_THROWS_AS(wt::instance<host>(rt, rt.compile("(module (memory 3))"), h,
                                      wt::instance_limits{.max_memory_bytes = 2 * 65536}),
                   wt::error);
}

TEST_CASE("wasmtime: traps are classified and leave the instance usable")
{
   fixture f;
   auto&   i = f.inst;
   CHECK(trap_of([&] { i.call("unreachable"); }) == wt::trap_kind::unreachable);
   CHECK(trap_of([&] { i.call<uint32_t, uint32_t, uint32_t>("div", 1, 0); }) ==
         wt::trap_kind::integer_division_by_zero);
   CHECK(trap_of([&] { i.call<uint32_t, uint32_t, uint32_t>("div", 0x80000000u, 0xffffffffu); }) ==
         wt::trap_kind::integer_overflow);
   CHECK(trap_of([&] { i.call<uint32_t, uint32_t>("load", 65533); }) ==
         wt::trap_kind::memory_out_of_bounds);
   CHECK(trap_of([&] { i.call("rec"); }) == wt::trap_kind::stack_overflow);
   CHECK(i.call<uint32_t, uint32_t, uint32_t>("div", 7, 2) == 3);
}

TEST_CASE("wasmtime: another thread can interrupt a running instance")
{
   fixture f;
   auto    stop = f.inst.make_interrupter();
   std::thread watchdog([stop] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      stop.fire();
   });
   auto kind = trap_of([&] { f.inst.call("spin"); });
   watchdog.join();
   CHECK(kind == wt::trap_kind::interrupt);
}

TEST_CASE("wasmtime: interrupting one instance leaves the others running")
{
   fixture a, b;
   // Both instances share nothing but must each see an engine tick: a's
   // interrupter only stops a.
   a.inst.make_interrupter().fire();
   CHECK(trap_of([&] { a.inst.call("spin"); }) == wt::trap_kind::interrupt);
   CHECK(b.inst.call<uint32_t, uint32_t>("countdown", 8) == 8);
}

TEST_CASE("wasmtime: binding checks exports and imports")
{
   fixture f;
   CHECK_THROWS_AS(f.inst.call("missing"), wt::error);
   // add3 takes three i32 and returns i32.
   CHECK_THROWS_AS((f.inst.call<uint64_t, uint32_t, uint32_t, uint32_t>("add3", 1, 2, 3)), wt::error);
   CHECK_THROWS_AS((f.inst.call<uint32_t, uint32_t>("add3", 1)), wt::error);

   auto rt = make_runtime();
   host h;
   CHECK_THROWS_AS(
       wt::instance<host>(rt, rt.compile(R"((module (import "env" "nope" (func))))"), h), wt::error);
   // Right name, wrong signature.
   CHECK_THROWS_AS(
       wt::instance<host>(
           rt, rt.compile(R"((module (import "env" "add" (func (param i64) (result i32)))))"), h),
       wt::error);
}

TEST_CASE("wasmtime: only the proposals the options name are accepted")
{
   auto rt = make_runtime();
   CHECK_THROWS_AS(rt.compile("(module (func $f (return_call $f)))"), wt::error);
   CHECK_THROWS_AS(rt.compile("(module (memory 1) (memory 1))"), wt::error);
   CHECK_THROWS_AS(rt.compile("(module (memory i64 1))"), wt::error);
   CHECK_THROWS_AS(rt.compile("(module (func (result i32 i32) (i32.const 1) (i32.const 2)))"),
                   wt::error);
   CHECK_THROWS_AS(rt.compile("(module (func (param externref)))"), wt::error);
   CHECK_NOTHROW(rt.compile("(module (func (result i32) (i32.extend8_s (i32.const 1))))"));

   wt::engine_options options;
   options.features |= wt::feature::tail_call | wt::feature::multi_value;
   auto wide = make_runtime(options);
   CHECK_NOTHROW(wide.compile("(module (func $f (return_call $f)))"));
   CHECK_NOTHROW(wide.compile("(module (func (result i32 i32) (i32.const 1) (i32.const 2)))"));
}

TEST_CASE("wasmtime: the strict float profile is refused until the fork provides it")
{
   wt::engine_options options;
   options.fp = wt::float_profile::strict;
   CHECK_THROWS_AS(wt::runtime<host>(options), wt::error);
}

TEST_CASE("wasmtime: a compiled module survives serialize and load")
{
   fixture f;
   auto    artifact = f.rt.compile(service).serialize();
   REQUIRE(artifact.size() > 0);
   host               h;
   wt::instance<host> loaded(f.rt, f.rt.load(artifact), h);
   CHECK(loaded.call<uint32_t, uint32_t, uint32_t, uint32_t>("add3", 4, 5, 6) == 15);
}

namespace {
struct float_options {
   static constexpr auto fp                  = psizam::fp_mode::softfloat;
   static constexpr bool enable_relaxed_simd = true;
};

// Operations where psizam at this commit is known to differ from wasmtime's
// deterministic behaviour. Both are deterministic; they made different choices.
//  - promote/demote: psizam keeps a NaN's sign and payload; wasmtime (and the
//    WebAssembly deterministic profile) return the positive canonical NaN.
//  - relaxed_min/max: psizam behaves like pmin/pmax; wasmtime (and the
//    profile) like regular min/max.
//  - relaxed_madd/nmadd: psizam is unfused (the profile's choice); wasmtime is
//    fused. This is the one point where the two float profiles differ.
const std::set<std::string> known_differences = {
    "f32_demote",          "f64_promote",         "f32x4_demote",       "f64x2_promote",
    "f32x4_relaxed_min",   "f32x4_relaxed_max",   "f64x2_relaxed_min",  "f64x2_relaxed_max",
    "f32x4_relaxed_madd",  "f32x4_relaxed_nmadd", "f64x2_relaxed_madd", "f64x2_relaxed_nmadd",
};
}  // namespace

TEST_CASE("wasmtime: float and SIMD results agree bit for bit with psizam")
{
   auto code = psizam::read_wasm(FLOAT_PARITY_WASM);

   struct none {};
   wt::runtime<none>  rt;
   none               n;
   wt::instance<none> wasmtime(rt, rt.compile(std::span<const uint8_t>(code)), n);

   psizam::wasm_allocator                                          wa;
   psizam::backend<std::nullptr_t, psizam::jit, float_options>     reference(code, &wa,
                                                                             float_options{});

   size_t checked = 0, still_different = 0;
   for (const char* op : float_parity_ops)
   {
      auto ours   = wasmtime.call<uint64_t>(op);
      auto theirs = reference.call_with_return("env", op)->to_ui64();
      if (known_differences.count(op))
      {
         still_different += ours != theirs;
         continue;
      }
      INFO("operation " << op);
      CHECK(ours == theirs);
      ++checked;
   }
   CHECK(checked == std::size(float_parity_ops) - known_differences.size());
   WARN(still_different << " of " << known_differences.size()
                        << " known-different operations still differ from psizam");
}
