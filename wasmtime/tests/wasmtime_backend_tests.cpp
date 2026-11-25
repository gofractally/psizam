// Tests for the wasmtime execution backend: the templated host binding, error
// transport across wasm frames, limits, interruption, and float agreement
// with psizam's own engines.
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psizam/backend.hpp>
#include <psizam/wasmtime_backend.hpp>

#include <chrono>
#include <cstring>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include "float_parity_ops.hpp"
#include "wide_arithmetic_fixture.hpp"

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

TEST_CASE("wasmtime: the two float profiles differ exactly in how relaxed madd rounds")
{
   // (1+ulp)^2 - (1+2ulp): the exact product is 1+2ulp+ulp^2. Rounding the
   // product first loses ulp^2 and the sum is zero; a fused multiply-add
   // keeps it.
   constexpr std::string_view text = R"((module
     (func (export "madd") (param i32 i32 i32) (result i32)
       (i32x4.extract_lane 0 (f32x4.relaxed_madd (i32x4.splat (local.get 0))
         (i32x4.splat (local.get 1)) (i32x4.splat (local.get 2)))))
     (func (export "nmadd") (param i32 i32 i32) (result i32)
       (i32x4.extract_lane 0 (f32x4.relaxed_nmadd (i32x4.splat (local.get 0))
         (i32x4.splat (local.get 1)) (i32x4.splat (local.get 2)))))
     (func (export "madd64") (param i64 i64 i64) (result i64)
       (i64x2.extract_lane 0 (f64x2.relaxed_madd (i64x2.splat (local.get 0))
         (i64x2.splat (local.get 1)) (i64x2.splat (local.get 2)))))
   ))";
   struct none {};
   none n;
   auto run = [&](wt::float_profile profile) {
      wt::engine_options options;
      options.fp = profile;
      wt::runtime<none>  rt(options);
      wt::instance<none> inst(rt, rt.compile(text), n);
      return std::tuple{
          inst.call<uint32_t, uint32_t, uint32_t, uint32_t>("madd", 0x3f800001u, 0x3f800001u,
                                                           0xbf800002u),
          inst.call<uint32_t, uint32_t, uint32_t, uint32_t>("nmadd", 0x3f800001u, 0x3f800001u,
                                                            0x3f800002u),
          inst.call<uint64_t, uint64_t, uint64_t, uint64_t>("madd64", 0x3ff0000000000001ull,
                                                           0x3ff0000000000001ull,
                                                           0xbff0000000000002ull)};
   };
   auto [strict_madd, strict_nmadd, strict_madd64] = run(wt::float_profile::strict);
   CHECK(strict_madd == 0u);
   CHECK(strict_nmadd == 0u);
   CHECK(strict_madd64 == 0ull);
   auto [fused_madd, fused_nmadd, fused_madd64] = run(wt::float_profile::performance);
   CHECK(fused_madd == 0x28800000u);              // 2^-46
   CHECK(fused_nmadd == 0xa8800000u);             // -2^-46
   CHECK(fused_madd64 == 0x3970000000000000ull);  // 2^-104
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
   psizam::float_profile float_profile       = psizam::float_profile::strict;
};
}  // namespace

TEST_CASE("wasmtime: float and SIMD results agree bit for bit with psizam in both profiles")
{
   auto code = psizam::read_wasm(FLOAT_PARITY_WASM);
   struct none {};
   none n;
   // The profiles differ only in how relaxed multiply-add rounds. In each one
   // every operation, over every pair of edge-case inputs, must produce the
   // same bits on wasmtime as on psizam.
   uint64_t madd_strict = 0, madd_performance = 0;
   for (auto profile : {wt::float_profile::strict, wt::float_profile::performance})
   {
      const bool strict = profile == wt::float_profile::strict;
      INFO((strict ? "strict profile" : "performance profile"));
      wt::engine_options options;
      options.fp = profile;
      wt::runtime<none>  rt(options);
      wt::instance<none> wasmtime(rt, rt.compile(std::span<const uint8_t>(code)), n);

      float_options psizam_options;
      psizam_options.float_profile =
          strict ? psizam::float_profile::strict : psizam::float_profile::performance;
      psizam::wasm_allocator                                      wa;
      psizam::backend<std::nullptr_t, psizam::jit, float_options> reference(code, &wa,
                                                                            psizam_options);
      for (const char* op : float_parity_ops)
      {
         auto ours   = wasmtime.call<uint64_t>(op);
         auto theirs = reference.call_with_return("env", op)->to_ui64();
         INFO("operation " << op);
         CHECK(ours == theirs);
         if (std::string_view(op) == "f32x4_relaxed_madd")
            (strict ? madd_strict : madd_performance) = ours;
      }
   }
   // And the profiles really are different where they are meant to be.
   CHECK(madd_strict != madd_performance);
}

// ── deterministic stack accounting ──────────────────────────────────────────

#include <psizam/wasmtime_admission.hpp>

namespace {

struct stack_options : psizam::default_options {
   static constexpr uint32_t max_func_local_bytes = 128 * 1024 * 1024;
   static constexpr auto     max_func_local_bytes_flags =
       psizam::max_func_local_bytes_flags_t::params | psizam::max_func_local_bytes_flags_t::locals |
       psizam::max_func_local_bytes_flags_t::stack;
   uint32_t max_stack_bytes = 65536;
   stack_options()
   {
      gas_metering = false;
      defer_start  = true;
   }
};

// Functions of different shapes: parameters, locals of every width, operand
// stack depth, early return, branches to the function label, indirect calls,
// and a host call at each level that records the bytes remaining there.
constexpr std::string_view stack_module = R"((module
  (import "env" "observe" (func $observe (param i32) (result i32)))
  (import "env" "reenter" (func $reenter (param i32) (result i32)))
  (type $one (func (param i32) (result i32)))
  (table 2 2 funcref)
  (elem (i32.const 0) $leaf $wide)
  (func $leaf (export "leaf") (param i32) (result i32) (local i64 i64)
    (call $observe (i32.const 1)))
  (func $wide (param i32) (result i32) (local v128 f64 f32 i32)
    (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) drop drop drop drop
    (call $observe (i32.const 2)))
  (func (export "outer") (param i32) (result i32) (local $v i32)
    (drop (call $observe (i32.const 3)))
    (local.set $v (call $leaf (i32.const 0)))
    (drop (call $observe (i32.const 4)))
    (local.get $v))
  (func (export "indirect") (param i32) (result i32)
    (call_indirect (type $one) (i32.const 0) (local.get 0)))
  (func (export "early") (param i32) (result i32)
    (if (local.get 0) (then (return (call $observe (i32.const 5)))))
    (block (result i32) (br 1 (call $observe (i32.const 6)))))
  (func $recursive (export "recursive") (param $n i32) (result i32) (local i64)
    (if (result i32) (i32.eqz (local.get $n))
      (then (call $leaf (i32.const 0)))
      (else (call $recursive (i32.sub (local.get $n) (i32.const 1))))))
  (func (export "nested") (param i32) (result i32)
    (drop (call $observe (i32.const 7)))
    (drop (call $reenter (local.get 0)))
    (call $observe (i32.const 8)))
  (func $bare (export "bare") (call $bare))
  (func (export "trap") (param i32) (result i32) unreachable)
))";

struct stack_host {
   std::function<uint32_t()>         remaining;
   std::function<uint32_t(uint32_t)> nested;
   std::vector<uint64_t>             seen;
   uint32_t observe(uint32_t tag)
   {
      auto value = remaining();
      seen.push_back((uint64_t(tag) << 32) | value);
      return value;
   }
   uint32_t reenter(uint32_t n) { return nested(n); }
};

struct stack_outcome {
   bool                  ok    = false;
   uint32_t              value = 0;
   uint32_t              after = 0;
   std::vector<uint64_t> seen;
   bool operator==(const stack_outcome&) const = default;
};

// One call with a fresh budget; `run` and `remaining` hide the engine.
stack_outcome run_once(stack_host& h, uint32_t budget, const std::function<void(uint32_t)>& set,
                       const std::function<uint32_t()>& call)
{
   h.seen.clear();
   set(budget);
   stack_outcome out;
   try
   {
      out.value = call();
      out.ok    = true;
   }
   catch (const std::exception&)
   {
   }
   // After a completed call every byte has been given back. After a trap the
   // engines leave different residue, and the embedder resets the budget
   // before the next call, so only the completed case is compared.
   out.after = out.ok ? h.remaining() : 0;
   out.seen  = h.seen;
   return out;
}

}  // namespace

TEST_CASE("wasmtime: stack accounting matches psizam's engines byte for byte")
{
   // Assemble the text once with wasmtime so both engines get the same binary.
   wt::engine_options options;
   options.features |= wt::feature::reference_types;
   wt::runtime<stack_host> rt(options);
   rt.add<&stack_host::observe>("env", "observe").add<&stack_host::reenter>("env", "reenter");

   psizam::wasm_code code = wt::assemble(stack_module);
   stack_options     psizam_options;
   auto              costs = wt::validate_and_cost(code, psizam_options);
   REQUIRE(costs.size() == 9);
   // 16 bytes of frame, plus parameters, locals and the deepest operand stack.
   for (auto cost : costs)
      CHECK(cost >= 16);

   // Reference: psizam's JIT with the same options.
   stack_host                                        ref_host;
   psizam::wasm_allocator                            wa;
   psizam::host_function_table                       table;
   table.add<&stack_host::observe>("env", "observe");
   table.add<&stack_host::reenter>("env", "reenter");
   using reference_backend =
       psizam::backend<psizam::registered_host_functions<stack_host>, psizam::jit, stack_options>;
   reference_backend reference(code, std::move(table), &ref_host, &wa, psizam_options);
   ref_host.remaining = [&] { return reference.get_context().get_remaining_call_depth(); };
   ref_host.nested    = [&](uint32_t n) {
      return reference.call_with_return(ref_host, "env", "recursive", n)->to_ui32();
   };

   stack_host               our_host;
   wt::instance<stack_host> ours(rt, rt.compile_metered(code, costs), our_host);
   our_host.remaining = [&] { return ours.stack_remaining(); };
   our_host.nested    = [&](uint32_t n) { return ours.call<uint32_t, uint32_t>("recursive", n); };

   auto both = [&](const char* name, uint32_t arg, uint32_t budget) {
      auto expected = run_once(
          ref_host, budget, [&](uint32_t b) { reference.get_context().set_max_call_depth(b); },
          [&] { return reference.call_with_return(ref_host, "env", name, arg)->to_ui32(); });
      auto actual = run_once(
          our_host, budget, [&](uint32_t b) { ours.set_stack_remaining(b); },
          [&] { return ours.call<uint32_t, uint32_t>(name, arg); });
      INFO("export " << name << " arg " << arg << " budget " << budget);
      CHECK(actual.ok == expected.ok);
      CHECK(actual.value == expected.value);
      CHECK(actual.after == expected.after);
      CHECK(actual.seen == expected.seen);
      return actual.ok;
   };

   for (uint32_t budget : {65536u, 4096u, 1000u, 200u})
   {
      both("leaf", 0, budget);
      both("outer", 0, budget);
      both("indirect", 0, budget);
      both("indirect", 1, budget);
      both("early", 0, budget);
      both("early", 1, budget);
      both("nested", 3, budget);
      both("trap", 0, budget);
      for (uint32_t depth : {0u, 1u, 2u, 7u, 50u, 500u, 5000u})
         both("recursive", depth, budget);
   }

   // The exact budget at which each shape first fits is the same on both
   // engines: sweep every byte across the boundary.
   size_t succeeded = 0, failed = 0;
   for (uint32_t budget = 0; budget <= 400; ++budget)
      for (auto [name, arg] : {std::pair{"leaf", 0u}, {"outer", 0u}, {"indirect", 1u},
                               {"recursive", 3u}, {"nested", 2u}})
         (both(name, arg, budget) ? succeeded : failed)++;
   CHECK(succeeded > 0);
   CHECK(failed > 0);
}

TEST_CASE("wasmtime: an exhausted stack budget is reported as a stack overflow")
{
   wt::engine_options options;
   options.features |= wt::feature::reference_types;
   wt::runtime<stack_host> wide(options);
   wide.add<&stack_host::observe>("env", "observe").add<&stack_host::reenter>("env", "reenter");

   psizam::wasm_code code  = wt::assemble(stack_module);
   stack_options     psizam_options;
   auto              costs = wt::validate_and_cost(code, psizam_options);
   stack_host        h;
   wt::instance<stack_host> inst(wide, wide.compile_metered(code, costs), h);
   h.remaining = [&] { return inst.stack_remaining(); };
   h.nested    = [&](uint32_t) { return 0u; };

   // A function with no locals still costs its 16-byte frame, so unbounded
   // recursion ends at the budget, long before the native stack does.
   inst.set_stack_remaining(1 << 20);
   CHECK(trap_of([&] { inst.call("bare"); }) == wt::trap_kind::stack_overflow);
   // A real unreachable is still reported as one.
   inst.set_stack_remaining(1 << 20);
   CHECK(trap_of([&] { inst.call<uint32_t, uint32_t>("trap", 0); }) == wt::trap_kind::unreachable);
   // The instance keeps working once the budget is set again.
   inst.set_stack_remaining(1 << 20);
   CHECK(inst.call<uint32_t, uint32_t>("leaf", 0) > 0);
   CHECK(inst.stack_remaining() == 1 << 20);
}

namespace {
// How deep the native stack is at a host call, and what the logical budget
// shows there, for the cheapest possible frames.
struct depth_host {
   std::function<uint32_t()> remaining;
   uintptr_t                 shallowest = 0, deepest = UINTPTR_MAX;
   uint32_t                  left_at_deepest = 0;
   void mark()
   {
      auto here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
      shallowest = std::max(shallowest, here);
      if (here < deepest)
      {
         deepest         = here;
         left_at_deepest = remaining();
      }
   }
};
}  // namespace

TEST_CASE("wasmtime: the byte budget ends recursion before the native stack can")
{
   // Each shape recurses n levels and calls the host at the bottom. The host
   // records how much native stack that took.
   constexpr std::string_view shapes = R"((module
     (import "env" "mark" (func $mark))
     (func $bare (export "bare") (param i32)
       (if (local.get 0) (then (call $bare (i32.sub (local.get 0) (i32.const 1)))) (else (call $mark))))
     (func $locals (export "locals") (param i32) (local i32 i32 i32 i32 i64 i64 f32 f64)
       (if (local.get 0) (then (call $locals (i32.sub (local.get 0) (i32.const 1)))) (else (call $mark))))
     (func $vectors (export "vectors") (param i32) (local v128 v128 v128)
       (if (local.get 0) (then (call $vectors (i32.sub (local.get 0) (i32.const 1)))) (else (call $mark))))
     (func (export "params") (param i32)
       (call $params (local.get 0) (i64.const 1) (i64.const 2) (f64.const 3) (f64.const 4)
         (i32.const 5) (i32.const 6) (i64.const 7)))
     (func $params (param i32 i64 i64 f64 f64 i32 i32 i64)
       (if (local.get 0)
         (then (call $params (i32.sub (local.get 0) (i32.const 1)) (local.get 1) (local.get 2)
                 (local.get 3) (local.get 4) (local.get 5) (local.get 6) (local.get 7)))
         (else (call $mark))))
     (func $live (export "live") (param i32) (result i64)
       (if (result i64) (local.get 0)
         (then (i64.add (i64.add (i64.const 1) (i64.extend_i32_u (local.get 0)))
                        (call $live (i32.sub (local.get 0) (i32.const 1)))))
         (else (call $mark) (i64.const 0))))
     (func $forever (export "forever") (call $forever))
   ))";
   constexpr uint32_t budget = 1 << 20;
   wt::engine_options options;
   options.native_stack = 64ull << 20;
   wt::runtime<depth_host> rt(options);
   rt.add<&depth_host::mark>("env", "mark");
   psizam::wasm_code code = wt::assemble(shapes);
   stack_options     psizam_options;
   auto              costs = wt::validate_and_cost(code, psizam_options);

   double worst = 0;
   for (const char* shape : {"bare", "locals", "vectors", "params", "live"})
   {
      depth_host               h;
      wt::instance<depth_host> inst(rt, rt.compile_metered(code, costs), h);
      h.remaining = [&] { return inst.stack_remaining(); };
      uint32_t levels = 4000;
      inst.set_stack_remaining(budget);
      inst.call_discarding_results(shape, levels);
      depth_host top;
      wt::instance<depth_host> again(rt, rt.compile_metered(code, costs), top);
      top.remaining = [&] { return again.stack_remaining(); };
      again.set_stack_remaining(budget);
      again.call_discarding_results(shape, 0u);
      double native  = double(top.deepest) - double(h.deepest);
      double logical = double(top.left_at_deepest) - double(h.left_at_deepest);
      INFO(shape << ": native " << native / levels << " bytes per frame, logical "
                 << logical / levels);
      REQUIRE(logical > 0);
      worst = std::max(worst, native / logical);
      // The dedicated stack is 64 times the budget; frames must stay far
      // inside that for the budget to be the limit that binds.
      CHECK(native / logical < 16);
   }
   WARN("worst native bytes per budget byte: " << worst);

   // With the whole budget, unbounded recursion of the cheapest frame ends in
   // the injected check, not in wasmtime's native-stack check.
   depth_host               h;
   wt::instance<depth_host> inst(rt, rt.compile_metered(code, costs), h);
   inst.set_stack_remaining(budget);
   try
   {
      inst.call("forever");
      FAIL("expected a stack overflow");
   }
   catch (const wt::trap& t)
   {
      CHECK(t.kind == wt::trap_kind::stack_overflow);
      CHECK(std::string(t.what()) == "stack overflow");
   }
}

TEST_CASE("wasmtime: stack accounting covers multi-result functions and the start function")
{
   constexpr std::string_view text = R"((module
     (import "env" "observe" (func $observe (param i32) (result i32)))
     (import "env" "reenter" (func $reenter (param i32) (result i32)))
     (global $started (mut i32) (i32.const 0))
     (func $pair (param i32) (result i32 i64) (local i64)
       (if (local.get 0) (then (return (call $observe (i32.const 1)) (i64.const 7))))
       (call $observe (i32.const 2)) (i64.const 9))
     (func (export "sum") (param i32) (result i32)
       (call $pair (local.get 0)) (i32.wrap_i64) (i32.add))
     (func $init (global.set $started (call $observe (i32.const 3))))
     (func (export "started") (param i32) (result i32) (global.get $started))
     (start $init)
   ))";
   wt::engine_options options;
   options.features |= wt::feature::multi_value;
   wt::runtime<stack_host> rt(options);
   rt.add<&stack_host::observe>("env", "observe").add<&stack_host::reenter>("env", "reenter");
   psizam::wasm_code code = wt::assemble(text);
   stack_options     psizam_options;
   auto              costs = wt::validate_and_cost(code, psizam_options);

   stack_host ref_host;
   psizam::wasm_allocator      wa;
   psizam::host_function_table table;
   table.add<&stack_host::observe>("env", "observe");
   table.add<&stack_host::reenter>("env", "reenter");
   psizam::backend<psizam::registered_host_functions<stack_host>, psizam::jit, stack_options>
       reference(code, std::move(table), &ref_host, &wa, psizam_options);
   ref_host.remaining = [&] { return reference.get_context().get_remaining_call_depth(); };
   ref_host.nested    = [](uint32_t) { return 0u; };
   reference.get_context().set_max_call_depth(5000);
   reference.initialize(&ref_host);
   auto reference_start = ref_host.seen;

   stack_host               h;
   wt::instance<stack_host> inst(rt, rt.compile_metered(code, costs), h);
   h.remaining = [&] { return inst.stack_remaining(); };
   h.nested    = [](uint32_t) { return 0u; };
   // Nothing has run yet: the start function waits for a budget.
   CHECK(h.seen.empty());
   inst.set_stack_remaining(5000);
   inst.run_start();
   CHECK(h.seen == reference_start);
   REQUIRE(h.seen.size() == 1);

   for (uint32_t budget : {5000u, 100u, 60u, 40u, 20u})
      for (uint32_t arg : {0u, 1u})
      {
         auto expected = run_once(
             ref_host, budget, [&](uint32_t b) { reference.get_context().set_max_call_depth(b); },
             [&] { return reference.call_with_return(ref_host, "env", "sum", arg)->to_ui32(); });
         auto actual = run_once(h, budget, [&](uint32_t b) { inst.set_stack_remaining(b); },
                                [&] { return inst.call<uint32_t, uint32_t>("sum", arg); });
         INFO("budget " << budget << " arg " << arg);
         CHECK(actual == expected);
      }
}

TEST_CASE("wasmtime: host spans reach a memory the module does not export")
{
   constexpr std::string_view text = R"((module
     (import "env" "note" (func $note (param i32 i32)))
     (memory 1)
     (data (i32.const 8) "private")
     (func (export "say") (call $note (i32.const 8) (i32.const 7)))
   ))";
   struct note_host {
      std::string seen;
      void note(std::span<const char> s) { seen.assign(s.begin(), s.end()); }
   };
   wt::runtime<note_host> rt;
   rt.add<&note_host::note>("env", "note");
   psizam::wasm_code code = wt::assemble(text);
   stack_options     psizam_options;
   note_host         h;
   wt::instance<note_host> inst(
       rt, rt.compile_metered(code, wt::validate_and_cost(code, psizam_options)), h);
   inst.set_stack_remaining(4096);
   inst.call("say");
   CHECK(h.seen == "private");
   CHECK(inst.memory().size() == 65536);
}

TEST_CASE("wasmtime: a module with no functions still gets its memory and stack counter")
{
   // No functions means an empty cost list. The module must still be treated
   // as metered: the host reaches its memory through the reserved export.
   psizam::wasm_code code = wt::assemble(R"((module (memory 2) (data (i32.const 4) "abc")))");
   stack_options     psizam_options;
   auto              costs = wt::validate_and_cost(code, psizam_options);
   REQUIRE(costs.empty());
   struct none {};
   none               n;
   wt::runtime<none>  rt;
   wt::instance<none> inst(rt, rt.compile_metered(code, costs), n);
   CHECK(inst.memory().size() == 2 * 65536);
   CHECK(std::string_view(inst.memory().data() + 4, 3) == "abc");
   inst.set_stack_remaining(1234);
   CHECK(inst.stack_remaining() == 1234);
}

TEST_CASE("wasmtime: wide arithmetic matches 128-bit host arithmetic, plain and stack-metered")
{
   struct no_host {};
   psizam::wasm_code     code = wide_arithmetic::module_bytes();
   wt::runtime<no_host>  rt;
   stack_options         psizam_options;
   const auto            costs = wt::validate_and_cost(code, psizam_options);
   for (bool metered : {false, true})
   {
      CAPTURE(metered);
      no_host               h;
      wt::instance<no_host> inst(rt, metered ? rt.compile_metered(code, costs) : rt.compile(code), h);
      if (metered)
         inst.set_stack_remaining(1 << 20);
      uint64_t mismatches = 0;
      wide_arithmetic::run_all(
          [&](const char* name, uint64_t a, uint64_t b) {
             return inst.call<uint64_t, uint64_t, uint64_t>(name, a, b);
          },
          [&](const char* name, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
             return inst.call<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>(name, a, b, c, d);
          },
          [&](const char* name) { return inst.call<uint64_t>(name); },
          [&](uint64_t actual, uint64_t expected, const std::string& name) {
             if (actual != expected && mismatches++ < 8)
             {
                CAPTURE(name);
                CHECK(actual == expected);
             }
          });
      CHECK(mismatches == 0);
   }
}

TEST_CASE("wasmtime: pooled instances start from the module's initial state and fall back when the pool cannot serve")
{
   struct no_host {};
   constexpr std::string_view counter = R"((module
     (memory (export "memory") 1 4)
     (data (i32.const 8) "\07\00\00\00")
     (global $g (mut i32) (i32.const 3))
     (func (export "bump") (result i32)
       (i32.store (i32.const 8) (i32.add (i32.load (i32.const 8)) (i32.const 1)))
       (i32.store (i32.const 60000) (i32.const 99))
       (global.set $g (i32.add (global.get $g) (i32.const 1)))
       (i32.add (i32.load (i32.const 8)) (global.get $g)))
     (func (export "far") (result i32) (i32.load (i32.const 60000)))
     (func (export "grow") (result i32) (memory.grow (i32.const 2)))
     (func (export "pages") (result i32) (memory.size))))";
   wt::engine_options pooled_options;
   pooled_options.pool_instances = 2;
   wt::runtime<no_host> pooled(pooled_options);
   wt::runtime<no_host> on_demand;
   const auto           module = pooled.compile(counter);
   no_host              h;

   // Every instance sees the data segment, a zeroed heap, the initial global
   // and the initial size, whatever the slot's previous user did.
   for (int round = 0; round < 50; ++round)
   {
      wt::instance<no_host> inst(pooled, module, h);
      REQUIRE(inst.call<uint32_t>("far") == 0);
      REQUIRE(inst.call<uint32_t>("pages") == 1);
      REQUIRE(inst.call<uint32_t>("bump") == 12);
      REQUIRE(inst.call<uint32_t>("bump") == 14);
      if (round % 2)
         REQUIRE(inst.call<uint32_t>("grow") == 1);
   }

   // Two slots: a third live instance cannot be created, and nothing ran.
   {
      wt::instance<no_host> first(pooled, module, h);
      wt::instance<no_host> second(pooled, module, h);
      CHECK_THROWS_AS((wt::instance<no_host>(pooled, module, h)), wt::instantiation_error);
      // The same compiled code runs in a runtime without a pool.
      const auto            artifact = module.serialize();
      wt::instance<no_host> third(on_demand, on_demand.load(artifact), h);
      CHECK(third.call<uint32_t>("bump") == 12);
      CHECK(first.call<uint32_t>("bump") == 12);
      CHECK(second.call<uint32_t>("bump") == 12);
   }
   // The slots are free again.
   wt::instance<no_host> again(pooled, module, h);
   CHECK(again.call<uint32_t>("bump") == 12);
}

// ── debugging aids: wasm backtraces and native debug information ────────────

// The GDB JIT interface, which the wasmtime shim defines. A debugger reads
// this list to find code compiled at run time.
extern "C" {
struct jit_code_entry {
   jit_code_entry* next_entry;
   jit_code_entry* prev_entry;
   const char*     symfile_addr;
   uint64_t        symfile_size;
};
struct jit_descriptor {
   uint32_t        version;
   uint32_t        action_flag;
   jit_code_entry* relevant_entry;
   jit_code_entry* first_entry;
};
extern jit_descriptor __jit_debug_descriptor;
}

namespace {

struct trace_host {
   wt::instance<trace_host>* self  = nullptr;
   int                       calls = 0;
   std::vector<wt::frame>    frames;
   void capture()
   {
      ++calls;
      frames = self->backtrace();
   }
   void fail() { throw std::logic_error("host failed"); }
};

wt::runtime<trace_host> trace_runtime(const wt::engine_options& options)
{
   wt::runtime<trace_host> rt(options);
   rt.add<&trace_host::capture>("env", "capture").add<&trace_host::fail>("env", "fail");
   return rt;
}

wt::engine_options with_backtrace(uint32_t max_frames = 512)
{
   wt::engine_options options;
   options.backtrace            = true;
   options.backtrace_max_frames = max_frames;
   return options;
}

// capture is function 0 and fail is function 1, so inner, middle and outer
// are 2, 3 and 4. The functions are named but only outer is exported: the
// names can only come from the name section.
constexpr std::string_view host_chain = R"((module
  (import "env" "capture" (func $capture))
  (import "env" "fail" (func $fail))
  (func $inner (param i32)
    (if (local.get 0) (then (call $fail)) (else (call $capture))))
  (func $middle (param i32) (call $inner (local.get 0)))
  (func $outer (export "run") (param i32) (call $middle (local.get 0)))
))";

// No imports: inner, middle and outer are functions 0, 1 and 2.
constexpr std::string_view trap_chain = R"((module
  (func $inner unreachable)
  (func $middle (call $inner))
  (func $outer (export "run") (call $middle))
))";

// Recursion to a chosen depth with a host call at the bottom, and recursion
// without end.
constexpr std::string_view deep = R"((module
  (import "env" "capture" (func $capture))
  (func $down (export "down") (param i32)
    (if (local.get 0)
      (then (call $down (i32.sub (local.get 0) (i32.const 1))))
      (else (call $capture))))
  (func $forever (export "forever") (call $forever))
))";

void require_chain(const std::vector<wt::frame>& frames, uint32_t first_index)
{
   REQUIRE(frames.size() == 3);
   const char* names[] = {"inner", "middle", "outer"};
   for (uint32_t i = 0; i < 3; ++i)
   {
      CHECK(frames[i].function_name == names[i]);
      CHECK(frames[i].function_index == first_index + i);
      CHECK(frames[i].module_offset.has_value());
      // No details were asked for.
      CHECK(frames[i].file.empty());
      CHECK(frames[i].line == 0);
   }
   // Each frame is at a different instruction, later functions further on.
   CHECK(*frames[0].module_offset < *frames[1].module_offset);
   CHECK(*frames[1].module_offset < *frames[2].module_offset);
}

std::set<const jit_code_entry*> registered_images()
{
   std::set<const jit_code_entry*> out;
   for (auto* e = __jit_debug_descriptor.first_entry; e; e = e->next_entry)
      out.insert(e);
   return out;
}

// The entries registered since `before` whose image mentions `text`.
size_t new_images_naming(const std::set<const jit_code_entry*>& before, std::string_view text)
{
   size_t count = 0;
   for (auto* e = __jit_debug_descriptor.first_entry; e; e = e->next_entry)
      if (!before.contains(e) &&
          std::string_view(e->symfile_addr, e->symfile_size).find(text) != std::string_view::npos)
         ++count;
   return count;
}

// The line of the call to capture() in debug_fixture.c.
constexpr uint32_t debug_fixture_call_line = 18;

}  // namespace

TEST_CASE("wasmtime: a host function can capture the wasm frames that called it")
{
   auto                     rt = trace_runtime(with_backtrace());
   trace_host               h;
   wt::instance<trace_host> inst(rt, rt.compile(host_chain), h);
   h.self = &inst;

   // Nothing is running, so there is nothing to capture.
   CHECK(inst.backtrace().empty());

   inst.call<void, uint32_t>("run", 0);
   REQUIRE(h.calls == 1);
   require_chain(h.frames, 2);
   // The call succeeded: no failure was recorded.
   CHECK(inst.failure_backtrace().empty());
}

TEST_CASE("wasmtime: a failed call leaves the backtrace of where it failed")
{
   auto       rt = trace_runtime(with_backtrace());
   trace_host h;
   {
      wt::instance<trace_host> inst(rt, rt.compile(trap_chain), h);
      CHECK(inst.failure_backtrace().empty());
      CHECK(trap_of([&] { inst.call("run"); }) == wt::trap_kind::unreachable);
      require_chain(inst.failure_backtrace(), 0);
      // Reading it does not consume it.
      require_chain(inst.failure_backtrace(), 0);
   }
   {
      // A host function that throws: the frames it was called from.
      wt::instance<trace_host> inst(rt, rt.compile(host_chain), h);
      h.self = &inst;
      CHECK_THROWS_AS((inst.call<void, uint32_t>("run", 1)), std::logic_error);
      require_chain(inst.failure_backtrace(), 2);
      // Every call starts with none, so after one that succeeds a failure
      // from somewhere else cannot be given this one's frames.
      inst.call<void, uint32_t>("run", 0);
      CHECK(inst.failure_backtrace().empty());
   }
}

TEST_CASE("wasmtime: backtraces are off unless the engine options ask for them")
{
   // Not even wasmtime's own environment switch turns anything on.
   setenv("WASMTIME_BACKTRACE_DETAILS", "1", 1);
   CHECK_FALSE(wt::engine_options{}.backtrace);
   CHECK_FALSE(wt::engine_options{}.backtrace_details);
   CHECK_FALSE(wt::engine_options{}.debug_info);
   CHECK(wt::engine_options{}.backtrace_max_frames == 512);

   auto       rt = trace_runtime({});
   trace_host h;
   {
      wt::instance<trace_host> inst(rt, rt.compile(host_chain), h);
      h.self = &inst;
      inst.call<void, uint32_t>("run", 0);
      REQUIRE(h.calls == 1);
      CHECK(h.frames.empty());
      CHECK_THROWS_AS((inst.call<void, uint32_t>("run", 1)), std::logic_error);
      CHECK(inst.failure_backtrace().empty());
   }
   {
      wt::instance<trace_host> inst(rt, rt.compile(trap_chain), h);
      try
      {
         inst.call("run");
         FAIL("expected a trap");
      }
      catch (const wt::trap& t)
      {
         CHECK(t.kind == wt::trap_kind::unreachable);
         // The message names no frames either.
         CHECK(std::string_view(t.what()).find("inner") == std::string_view::npos);
      }
      CHECK(inst.failure_backtrace().empty());
   }

   // Capture without details: frames, but no source positions, even for a
   // module that has DWARF.
   {
      auto                     plain = trace_runtime(with_backtrace());
      wt::instance<trace_host> inst(plain, plain.compile(psizam::read_wasm(DEBUG_FIXTURE_WASM)), h);
      h.self = &inst;
      h.frames.clear();
      CHECK(inst.call<uint32_t, uint32_t>("debug_fixture_entry", 1) == 2);
      REQUIRE(h.frames.size() == 1);
      CHECK(h.frames[0].function_name == "debug_fixture_entry");
      CHECK(h.frames[0].file.empty());
      CHECK(h.frames[0].line == 0);
   }

   // Options that cannot be honoured are refused, not adjusted.
   wt::engine_options details_only;
   details_only.backtrace_details = true;
   CHECK_THROWS_AS(wt::runtime<trace_host>(details_only), wt::error);
   CHECK_THROWS_AS(wt::runtime<trace_host>(with_backtrace(0)), wt::error);
}

TEST_CASE("wasmtime: a backtrace holds no more frames than the engine's limit")
{
   trace_host h;
   {
      auto                     rt = trace_runtime(with_backtrace(16));
      wt::instance<trace_host> inst(rt, rt.compile(deep), h);
      h.self = &inst;

      // Below the limit every frame is there: `down` once per level.
      inst.call<void, uint32_t>("down", 9);
      CHECK(h.frames.size() == 10);
      // Above it, the innermost 16.
      inst.call<void, uint32_t>("down", 99);
      REQUIRE(h.frames.size() == 16);
      for (auto& f : h.frames)
         CHECK(f.function_name == "down");

      // Recursion that runs out of native stack has thousands of frames.
      CHECK(trap_of([&] { inst.call("forever"); }) == wt::trap_kind::stack_overflow);
      auto frames = inst.failure_backtrace();
      REQUIRE(frames.size() == 16);
      for (auto& f : frames)
         CHECK(f.function_name == "forever");
   }
   {
      // The same with deterministic stack accounting, where the overflow is
      // the byte budget running out.
      auto options           = with_backtrace(16);
      auto              rt   = trace_runtime(options);
      psizam::wasm_code code = wt::assemble(deep);
      stack_options     psizam_options;
      auto              costs = wt::validate_and_cost(code, psizam_options);
      wt::instance<trace_host> inst(rt, rt.compile_metered(code, costs), h);
      h.self = &inst;
      inst.set_stack_remaining(1 << 20);
      CHECK(trap_of([&] { inst.call("forever"); }) == wt::trap_kind::stack_overflow);
      auto frames = inst.failure_backtrace();
      REQUIRE(frames.size() == 16);
      for (auto& f : frames)
         CHECK(f.function_name == "forever");
   }
   {
      // A wider limit is honoured too.
      auto                     rt = trace_runtime(with_backtrace(1000));
      wt::instance<trace_host> inst(rt, rt.compile(deep), h);
      h.self = &inst;
      inst.call<void, uint32_t>("down", 599);
      CHECK(h.frames.size() == 600);
      CHECK(trap_of([&] { inst.call("forever"); }) == wt::trap_kind::stack_overflow);
      CHECK(inst.failure_backtrace().size() == 1000);
   }
}

TEST_CASE("wasmtime: backtrace details give the source file and line from the module's DWARF")
{
   auto options              = with_backtrace();
   options.backtrace_details = true;
   auto       rt             = trace_runtime(options);
   auto       module         = rt.compile(psizam::read_wasm(DEBUG_FIXTURE_WASM));
   trace_host h;
   wt::instance<trace_host> inst(rt, module, h);
   h.self = &inst;

   CHECK(inst.call<uint32_t, uint32_t>("debug_fixture_entry", 41) == 42);
   REQUIRE(h.frames.size() == 1);
   CHECK(h.frames[0].function_name == "debug_fixture_entry");
   CHECK(h.frames[0].function_index == 1);
   CHECK(h.frames[0].file == "./debug_fixture.c");
   CHECK(h.frames[0].line == debug_fixture_call_line);

   // Details are part of how a module is compiled: its artifact does not
   // load into a runtime without them.
   auto plain = trace_runtime(with_backtrace());
   CHECK_THROWS_AS(plain.load(module.serialize()), wt::error);
   // Capturing and its frame limit are not: artifacts move freely between
   // runtimes that differ only in those.
   auto off      = trace_runtime({});
   auto artifact = off.compile(host_chain).serialize();
   {
      wt::instance<trace_host> inst(plain, plain.load(artifact), h);
      h.self = &inst;
      inst.call<void, uint32_t>("run", 0);
      require_chain(h.frames, 2);
   }

   // What the frame of a module compiled some other way says of its source.
   struct position {
      std::string file;
      uint32_t    line;
      bool operator==(const position&) const = default;
   };
   const position    placed{"./debug_fixture.c", debug_fixture_call_line};
   psizam::wasm_code code = psizam::read_wasm(DEBUG_FIXTURE_WASM);
   stack_options     psizam_options;
   const auto        costs = wt::validate_and_cost(code, psizam_options);
   REQUIRE(costs.size() == 1);
   REQUIRE(costs[0] > 0);
   auto position_with = [&](wt::float_profile fp, bool metered) {
      auto o  = options;
      o.fp    = fp;
      auto rt = trace_runtime(o);
      wt::instance<trace_host> inst(rt, metered ? rt.compile_metered(code, costs) : rt.compile(code),
                                    h);
      h.self = &inst;
      h.frames.clear();
      inst.set_stack_remaining(1 << 20);
      CHECK(inst.call<uint32_t, uint32_t>("debug_fixture_entry", 41) == 42);
      // A metered module really is metered: the call spent some of the budget
      // and gave it back.
      if (metered)
         CHECK(inst.stack_remaining() == 1 << 20);
      REQUIRE(h.frames.size() == 1);
      CHECK(h.frames[0].function_name == "debug_fixture_entry");
      CHECK(h.frames[0].function_index == 1);
      return position{h.frames[0].file, h.frames[0].line};
   };
   using wt::float_profile;
   // No module is rewritten before it is compiled, under either float
   // profile and metered or not, so its DWARF always describes it.
   CHECK(position_with(float_profile::performance, true) == placed);
   CHECK(position_with(float_profile::performance, false) == placed);
   CHECK(position_with(float_profile::strict, true) == placed);
   CHECK(position_with(float_profile::strict, false) == placed);
}

namespace {
// One export per way a call can fail, each a call deep so there are frames.
constexpr std::string_view failures = R"((module
  (import "env" "fail" (func $fail))
  (type $binary (func (param i32 i32) (result i32)))
  (memory 1)
  (table 1 funcref)
  (elem (i32.const 0) $nothing)
  (global $zero (mut i32) (i32.const 0))
  (func $nothing)
  (func $unreachable unreachable)
  (func $load (drop (i32.load (i32.add (global.get $zero) (i32.const 70000)))))
  (func $divide (drop (i32.div_u (i32.const 1) (global.get $zero))))
  (func $indirect
    (drop (call_indirect (type $binary) (i32.const 1) (i32.const 2) (global.get $zero))))
  (func $forever (call $forever))
  (func $host (call $fail))
  (func (export "unreachable") (call $unreachable))
  (func (export "load") (call $load))
  (func (export "divide") (call $divide))
  (func (export "indirect") (call $indirect))
  (func (export "forever") (call $forever))
  (func (export "host") (call $host))
))";

struct failure {
   std::string text;
   int         kind   = -1;  // trap_kind, or -1 for the host's own exception
   size_t      frames = 0;
};

failure fail(const wt::engine_options& options, bool metered, std::string_view name)
{
   auto              rt   = trace_runtime(options);
   psizam::wasm_code code = wt::assemble(failures);
   stack_options     psizam_options;
   trace_host        h;
   wt::instance<trace_host> inst(
       rt, metered ? rt.compile_metered(code, wt::validate_and_cost(code, psizam_options))
                   : rt.compile(code),
       h);
   inst.set_stack_remaining(1 << 16);
   failure out;
   try
   {
      inst.call(name);
      FAIL("expected " << name << " to fail");
   }
   catch (const wt::trap& t)
   {
      out.text = t.what();
      out.kind = static_cast<int>(t.kind);
   }
   catch (const std::logic_error& e)
   {
      out.text = e.what();
   }
   out.frames = inst.failure_backtrace().size();
   return out;
}
}  // namespace

TEST_CASE("wasmtime: the text of a failure is the same with backtraces on and off")
{
   using kind = wt::trap_kind;
   const std::pair<const char*, int> cases[] = {
       {"unreachable", int(kind::unreachable)},
       {"load", int(kind::memory_out_of_bounds)},
       {"divide", int(kind::integer_division_by_zero)},
       {"indirect", int(kind::bad_signature)},
       {"forever", int(kind::stack_overflow)},
       {"host", -1},
   };
   {
      for (bool metered : {false, true})
         for (auto [name, expected] : cases)
         {
            INFO(name << (metered ? ", metered" : ", not metered"));
            wt::engine_options off;
            auto on       = off;
            on.backtrace  = true;
            auto detailed = on;
            detailed.backtrace_details = true;

            const auto without = fail(off, metered, name);
            const auto with    = fail(on, metered, name);
            const auto most    = fail(detailed, metered, name);
            CHECK(without.kind == expected);
            CHECK(with.kind == expected);
            CHECK_FALSE(without.text.empty());
            // The frames were captured, and are not in the text.
            CHECK(without.frames == 0);
            CHECK(with.frames > 0);
            CHECK(with.text == without.text);
            CHECK(most.text == without.text);
            CHECK(with.text.find("backtrace") == std::string::npos);
            // What wasmtime adds besides frames is still there.
            if (std::string_view(name) == "load")
               CHECK(without.text.find("memory fault at wasm address 0x11170 in linear memory of "
                                       "size 0x10000") != std::string::npos);
            // Not metered, the recursion runs out of native stack; metered,
            // out of budget. The text is the same.
            if (std::string_view(name) == "forever")
               CHECK(without.text == "stack overflow");
         }
   }
}

TEST_CASE("wasmtime: debug_info registers compiled code with the debugger interface")
{
   const auto wasm = psizam::read_wasm(DEBUG_FIXTURE_WASM);
   trace_host h;
   REQUIRE(__jit_debug_descriptor.version == 1);

   // Off: compiling and running a module with DWARF registers nothing.
   {
      const auto               before = registered_images();
      auto                     rt     = trace_runtime({});
      wt::instance<trace_host> inst(rt, rt.compile(wasm), h);
      h.self = &inst;
      CHECK(inst.call<uint32_t, uint32_t>("debug_fixture_entry", 1) == 2);
      CHECK(registered_images() == before);
   }

   const auto before = registered_images();
   {
      wt::engine_options options;
      options.debug_info = true;
      auto                     rt = trace_runtime(options);
      wt::instance<trace_host> inst(rt, rt.compile(wasm), h);
      h.self = &inst;
      // The module still runs, and its image is on the debugger's list. The
      // image is an ELF file whose symbols and DWARF name the function.
      CHECK(inst.call<uint32_t, uint32_t>("debug_fixture_entry", 1) == 2);
      CHECK(new_images_naming(before, "debug_fixture_entry") == 1);
      for (auto* e = __jit_debug_descriptor.first_entry; e; e = e->next_entry)
         if (!before.contains(e))
            CHECK(std::string_view(e->symfile_addr, e->symfile_size).starts_with("\x7f" "ELF"));
   }
   // Dropping the module takes its image off the list again.
   CHECK(registered_images() == before);

   // Debug information is part of how a module is compiled: an artifact made
   // with it does not load into a runtime without it, nor the reverse.
   wt::engine_options options;
   options.debug_info = true;
   auto with = trace_runtime(options), without = trace_runtime({});
   CHECK_THROWS_AS(without.load(with.compile(wasm).serialize()), wt::error);
   CHECK_THROWS_AS(with.load(without.compile(wasm).serialize()), wt::error);
}

TEST_CASE("wasmtime: entry points are found once per module and entered without a lookup")
{
   struct none {};
   constexpr std::string_view text = R"((module
     (global $sum (mut i64) (i64.const 0))
     (global $started (mut i64) (i64.const 0))
     (func (export "start") (param i64) (global.set $started (local.get 0)))
     (func (export "called") (param i64 i64) (global.set $sum (i64.add (global.get $sum)
        (i64.add (local.get 0) (local.get 1)))))
     (func (export "processTransaction") (unreachable))
     (func (export "sum") (result i64) (global.get $sum))
     (func (export "started") (result i64) (global.get $started))
   ))";
   wt::runtime<none>  rt;
   none               n;
   wt::module         compiled = rt.compile(text);
   wt::instance<none> inst(rt, compiled, n);

   CHECK(inst.has_entry(wt::entry_point::start));
   CHECK(inst.has_entry(wt::entry_point::called));
   CHECK(inst.has_entry(wt::entry_point::process_transaction));
   CHECK_FALSE(inst.has_entry(wt::entry_point::serve));

   inst.enter(wt::entry_point::start, 0x1122334455667788ull);
   CHECK(inst.call<uint64_t>("started") == 0x1122334455667788ull);
   for (uint64_t i = 1; i <= 1000; ++i)
      inst.enter(wt::entry_point::called, i, 0xffffffff00000000ull);
   CHECK(inst.call<uint64_t>("sum") == 500500 + 1000 * 0xffffffff00000000ull);

   // An absent entry point is an error that names it; nothing runs.
   CHECK_THROWS_WITH(inst.enter(wt::entry_point::serve),
                     Catch::Matchers::Contains("no callable export named serve"));
   // A trap inside one is reported as a trap and the instance stays usable.
   CHECK(trap_of([&] { inst.enter(wt::entry_point::process_transaction); }) ==
         wt::trap_kind::unreachable);
   inst.enter(wt::entry_point::called, 1, 1);

   // A module loaded from its compiled form has the same entry points.
   auto               artifact = compiled.serialize();
   wt::instance<none> loaded(rt, rt.load(artifact), n);
   CHECK(loaded.has_entry(wt::entry_point::called));
   CHECK_FALSE(loaded.has_entry(wt::entry_point::serve));
   loaded.enter(wt::entry_point::called, 2, 3);
   CHECK(loaded.call<uint64_t>("sum") == 5);

   // An entry point's name with any other signature is not compiled: a
   // result, a different parameter type or count, or an export of that name
   // that is not a function.
   for (std::string_view wrong : {
            R"((module (func (export "called") (param i64 i64) (result i32) (i32.const 0))))",
            R"((module (func (export "called") (param i64 i32))))",
            R"((module (func (export "called") (param i64))))",
            R"((module (func (export "start"))))",
            R"((module (func (export "serve") (param i64))))",
            R"((module (func (export "processTransaction") (result i64) (i64.const 0))))",
            R"((module (memory (export "called") 1)))",
        })
   {
      INFO(wrong);
      CHECK_THROWS_AS(rt.compile(wrong), wt::error);
   }
   // A module with no entry points at all is still a module.
   wt::instance<none> bare(rt, rt.compile(R"((module (func (export "f"))))"), n);
   CHECK_FALSE(bare.has_entry(wt::entry_point::called));
}
