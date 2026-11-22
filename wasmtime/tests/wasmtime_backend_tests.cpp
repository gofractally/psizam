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
};

// Operations where psizam at this commit is known to differ from wasmtime's
// deterministic behaviour. Both are deterministic; they made different choices.
//  - promote/demote: psizam keeps a NaN's sign and payload; wasmtime (and the
//    WebAssembly deterministic profile) return the positive canonical NaN.
//  - relaxed_min/max: psizam behaves like pmin/pmax; wasmtime (and the
//    profile) like regular min/max.
//  - relaxed_madd/nmadd: psizam is unfused (the profile's choice); wasmtime is
//    fused. This is the one point where the two float profiles differ.
const std::set<std::string> rounding_differences = {
    "f32x4_relaxed_madd", "f32x4_relaxed_nmadd", "f64x2_relaxed_madd", "f64x2_relaxed_nmadd",
};
const std::set<std::string> other_differences = {
    "f32_demote",        "f64_promote",       "f32x4_demote",      "f64x2_promote",
    "f32x4_relaxed_min", "f32x4_relaxed_max", "f64x2_relaxed_min", "f64x2_relaxed_max",
};
}  // namespace

TEST_CASE("wasmtime: float and SIMD results agree bit for bit with psizam")
{
   auto code = psizam::read_wasm(FLOAT_PARITY_WASM);
   psizam::wasm_allocator                                      wa;
   psizam::backend<std::nullptr_t, psizam::jit, float_options> reference(code, &wa,
                                                                         float_options{});
   struct none {};
   none n;
   // psizam's relaxed madd is unfused today, which is the strict profile. So
   // under strict the madd operations must match it too, and under
   // performance they must not.
   for (auto profile : {wt::float_profile::strict, wt::float_profile::performance})
   {
      const bool strict = profile == wt::float_profile::strict;
      INFO((strict ? "strict profile" : "performance profile"));
      wt::engine_options options;
      options.fp = profile;
      wt::runtime<none>  rt(options);
      wt::instance<none> wasmtime(rt, rt.compile(std::span<const uint8_t>(code)), n);

      size_t checked = 0, still_different = 0;
      for (const char* op : float_parity_ops)
      {
         auto ours   = wasmtime.call<uint64_t>(op);
         auto theirs = reference.call_with_return("env", op)->to_ui64();
         if (other_differences.count(op))
         {
            still_different += ours != theirs;
            continue;
         }
         INFO("operation " << op);
         if (rounding_differences.count(op) && !strict)
         {
            CHECK(ours != theirs);
            continue;
         }
         CHECK(ours == theirs);
         ++checked;
      }
      CHECK(checked == std::size(float_parity_ops) - other_differences.size() -
                           (strict ? 0 : rounding_differences.size()));
      WARN((strict ? "strict: " : "performance: ")
           << still_different << " of " << other_differences.size()
           << " operations psizam is known to define differently still differ");
   }
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
