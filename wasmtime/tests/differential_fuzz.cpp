// Runs modules on psizam's JIT2 and on the wasmtime backend and compares what
// each call did: whether it trapped, what it returned, and all of linear
// memory afterwards. Both engines get the same stack byte budget.
//
//   differential_fuzz [--performance] <file.wasm>...
//   differential_fuzz --admission-only [--verbose] <file.wasm>...
//
// --admission-only executes nothing. It asks one question of each module: if
// psizam's validator admits it under the node's options, does wasmtime compile
// it? A module that is admitted and that wasmtime refuses is printed as
//   SPLIT <file>: <wasmtime's message>
// and one the embedder's own preflight would have refused (a shared memory) as
//   PREFLIGHT <file>: <message>
// With --verbose every other module gets an ADMIT or REFUSE line as well. The
// last line counts modules, admitted, refused and splits.
//
// Modules psizam's validator refuses are skipped: nothing would run them.
// Exit status is the number of modules that behaved differently (max 255).

#include <psizam/backend.hpp>
#include <psizam/wasmtime_admission.hpp>
#include <psizam/wasmtime_backend.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace wt = psizam::wasmtime;

namespace {

struct options : psizam::default_options {
   static constexpr uint32_t max_func_local_bytes       = 128 * 1024 * 1024;
   static constexpr auto     max_func_local_bytes_flags =
       psizam::max_func_local_bytes_flags_t::params | psizam::max_func_local_bytes_flags_t::locals |
       psizam::max_func_local_bytes_flags_t::stack;
   static constexpr auto     fp                         = psizam::fp_mode::softfloat;
   static constexpr bool     enable_tail_calls          = false;
   static constexpr bool     enable_exception_handling  = false;
   // What the node's admission refuses because wasmtime, as the node
   // configures it, does not compile it (gofractally/psizam#6, #7).
   static constexpr bool     enable_typed_references      = false;
   static constexpr bool     enable_const_defined_globals = false;
   static constexpr uint32_t max_memory_section_elements = 1;
   uint32_t              max_stack_bytes = 65536;
   uint32_t              max_pages       = 512;
   psizam::float_profile float_profile   = psizam::float_profile::strict;
   options()
   {
      gas_metering = false;
      defer_start  = true;
   }
};

constexpr uint32_t budget = 65536;

struct none {};

uint64_t fnv(const char* data, size_t size)
{
   uint64_t h = 0xcbf29ce484222325ull;
   for (size_t i = 0; i < size; ++i)
   {
      h ^= static_cast<unsigned char>(data[i]);
      h *= 0x100000001b3ull;
   }
   return h;
}

// What one call did, as far as the chain could tell.
struct outcome {
   bool     ok     = false;
   uint64_t value  = 0;
   uint64_t memory = 0;
   size_t   pages  = 0;
   bool operator==(const outcome&) const = default;
};

// Arguments are a fixed pattern by position, so both engines get the same.
constexpr uint64_t pattern[4] = {0, 1, 0x7fffffffffffffffull, 0xfffffffffffffff7ull};

template <typename Call, typename... A>
auto with_arguments(const std::vector<uint8_t>& params, size_t i, Call&& call, A... a)
    -> decltype(call())
{
   if (i == params.size())
      return call(a...);
   if constexpr (sizeof...(A) < 4)
   {
      switch (params[i])
      {
         case psizam::types::i32:
            return with_arguments(params, i + 1, call, a..., static_cast<uint32_t>(pattern[i]));
         case psizam::types::i64:
            return with_arguments(params, i + 1, call, a..., static_cast<uint64_t>(pattern[i]));
         case psizam::types::f32:
            return with_arguments(params, i + 1, call, a..., static_cast<float>(pattern[i] % 97));
         default:
            return with_arguments(params, i + 1, call, a..., static_cast<double>(pattern[i] % 97));
      }
   }
   return call();
}

}  // namespace

int main(int argc, char** argv)
{
   wt::engine_options engine;
   engine.features = wt::feature::simd | wt::feature::relaxed_simd | wt::feature::bulk_memory |
                     wt::feature::sign_extension | wt::feature::saturating_float_to_int |
                     wt::feature::reference_types | wt::feature::multi_value | wt::feature::threads;
   // Both engines run the same float profile: strict unless asked otherwise.
   engine.fp           = wt::float_profile::strict;
   engine.native_stack = 64ull << 20;
   int  first          = 1;
   bool admission_only = false, verbose = false;
   for (; first < argc; ++first)
   {
      std::string arg = argv[first];
      if (arg == "--performance")
         engine.fp = wt::float_profile::performance;
      else if (arg == "--admission-only")
         admission_only = true;
      else if (arg == "--verbose")
         verbose = true;
      else
         break;
   }
   wt::runtime<none> rt(engine);

   if (admission_only)
   {
      size_t seen = 0, admitted = 0, refused = 0, preflight = 0, splits = 0;
      for (int m = first; m < argc; ++m)
      {
         ++seen;
         std::ifstream     in(argv[m], std::ios::binary);
         psizam::wasm_code code((std::istreambuf_iterator<char>(in)), {});
         options           policy;
         std::vector<uint32_t> costs;
         std::string           why;
         try
         {
            costs = wt::validate_and_cost(code, policy);
         }
         catch (const psizam::exception& e)
         {
            why = std::string(e.what()) + ": " + e.detail();
         }
         catch (const std::exception& e)
         {
            why = e.what();
         }
         catch (...)
         {
            why = "unknown exception";
         }
         if (!why.empty())
         {
            ++refused;
            if (verbose)
               std::printf("REFUSE %s: %s\n", argv[m], why.c_str());
            continue;
         }
         ++admitted;
         try
         {
            (void)rt.compile_metered(code, costs);
            if (verbose)
               std::printf("ADMIT %s\n", argv[m]);
         }
         catch (const std::exception& e)
         {
            std::string message = e.what();
            for (auto& c : message)
               if (c == '\n' || c == '\r')
                  c = ' ';
            // The embedder's preflight refuses a shared memory before either
            // engine sees the module, so that one is not a split.
            if (message.find("shared memory") != std::string::npos)
            {
               ++preflight;
               std::printf("PREFLIGHT %s: %s\n", argv[m], message.c_str());
            }
            else
            {
               ++splits;
               std::printf("SPLIT %s: %s\n", argv[m], message.c_str());
            }
         }
         std::fflush(stdout);
      }
      std::printf("modules %zu, admitted %zu, refused %zu, splits %zu (and %zu admitted that the "
                  "embedder's shared-memory preflight refuses)\n",
                  seen, admitted, refused, splits, preflight);
      return splits > 255 ? 255 : int(splits);
   }

   size_t modules = 0, skipped = 0, calls = 0, different = 0;
   for (int m = first; m < argc; ++m)
   {
      std::ifstream     in(argv[m], std::ios::binary);
      psizam::wasm_code code((std::istreambuf_iterator<char>(in)), {});
      options           policy;
      policy.float_profile = engine.fp == wt::float_profile::strict
                                 ? psizam::float_profile::strict
                                 : psizam::float_profile::performance;
      std::vector<uint32_t> costs;
      try
      {
         costs = wt::validate_and_cost(code, policy);
      }
      catch (...)
      {
         ++skipped;
         continue;
      }
      ++modules;

      // Each engine instantiates, runs the start function, then every
      // exported function it can pass arguments to, in export order.
      std::vector<outcome>     reference_results, wasmtime_results;
      std::vector<std::string> names;
      std::string              note;
      // The embedder's own preflight refuses shared memories before either
      // engine sees the module; psizam's parser alone does not.
      std::optional<wt::module> compiled;
      try
      {
         compiled = rt.compile_metered(code, costs);
      }
      catch (const std::exception& e)
      {
         if (std::string(e.what()).find("shared memory") != std::string::npos)
         {
            --modules;
            ++skipped;
            continue;
         }
         note = std::string("wasmtime refused an admitted module: ") + e.what();
      }
      if (note.empty())
      try
      {
         psizam::wasm_allocator wa;
         using backend = psizam::backend<std::nullptr_t, psizam::jit2, options>;
         // Building the instance applies data and element segments; a segment
         // out of bounds fails here on psizam and at instantiation on wasmtime.
         std::optional<backend> built;
         std::string            why;
         try
         {
            built.emplace(code, &wa, policy);
         }
         catch (const psizam::exception& e)
         {
            why = std::string("psizam: ") + e.what() + ": " + e.detail();
            reference_results.push_back(outcome{});
         }
         catch (const std::exception& e)
         {
            why = std::string("psizam: ") + e.what();
            reference_results.push_back(outcome{});
         }
         std::optional<wt::instance<none>> ours_built;
         none                              host;
         try
         {
            ours_built.emplace(rt, *compiled, host,
                               wt::instance_limits{.max_memory_bytes = 512ull * 65536});
         }
         catch (const std::exception& e)
         {
            why += std::string(" wasmtime: ") + e.what();
            wasmtime_results.push_back(outcome{});
         }
         if (!ours_built && why.find("shared memory") != std::string::npos)
         {
            // Refused by the embedder's preflight in a real node.
            --modules;
            ++skipped;
            continue;
         }
         // Where a failing start function or segment surfaces differs: psizam
         // may run it while the instance is built, wasmtime when asked. Either
         // way the service never starts, so the two count as one stage.
         bool reference_started = false, wasmtime_started = false;
         if (built)
         {
            built->get_context().set_max_call_depth(budget);
            try
            {
               built->initialize(nullptr);
               reference_started = true;
            }
            catch (...)
            {
            }
         }
         if (ours_built)
         {
            ours_built->set_stack_remaining(budget);
            try
            {
               ours_built->run_start();
               wasmtime_started = true;
            }
            catch (...)
            {
            }
         }
         names.push_back("(instantiate and start)");
         reference_results.clear();
         wasmtime_results.clear();
         if (!reference_started || !wasmtime_started)
         {
            reference_results.push_back(outcome{reference_started});
            wasmtime_results.push_back(outcome{wasmtime_started});
            if (reference_started != wasmtime_started)
               note = std::string("only ") + (reference_started ? "wasmtime" : "psizam") +
                      " failed to start: " + why;
            throw std::runtime_error(note);
         }
         backend& reference = *built;
         auto&    mod       = reference.get_module();
         auto    measure = [&](bool ok, uint64_t value) {
            // The allocator reports -1 until a memory has been set up.
            auto   current = reference.get_context().current_linear_memory();
            size_t pages   = mod.memories.empty() || current < 0 ? 0 : static_cast<size_t>(current);
            return outcome{ok, value,
                           pages ? fnv(reference.get_context().linear_memory(), pages * 65536) : 0,
                           pages};
         };
         reference_results.push_back(measure(true, 0));
         if (reference_results.back().ok)
            for (const auto& e : mod.exports)
            {
               if (e.kind != psizam::external_kind::Function)
                  continue;
               const auto& type = mod.get_function_type(e.index);
               std::vector<uint8_t> params(type.param_types.begin(), type.param_types.end());
               bool supported = params.size() <= 4 && type.return_types.size() <= 1;
               for (auto p : params)
                  supported &= p == psizam::types::i32 || p == psizam::types::i64 ||
                               p == psizam::types::f32 || p == psizam::types::f64;
               for (auto r : type.return_types)
                  supported &= r == psizam::types::i32 || r == psizam::types::i64 ||
                               r == psizam::types::f32 || r == psizam::types::f64;
               if (!supported)
                  continue;
               std::string name(reinterpret_cast<const char*>(e.field_str.data()),
                                e.field_str.size());
               names.push_back(name);
               reference.get_context().set_max_call_depth(budget);
               try
               {
                  auto result = with_arguments(params, 0, [&](auto... a) {
                     return reference.call_with_return("env", name, a...);
                  });
                  // A 32-bit result is only its low half; the rest is not part of it.
                  uint64_t value = 0;
                  if (result)
                     value = type.return_types[0] == psizam::types::i32 ||
                                     type.return_types[0] == psizam::types::f32
                                 ? result->to_ui32()
                                 : result->to_ui64();
                  reference_results.push_back(measure(true, value));
               }
               catch (...)
               {
                  reference_results.push_back(measure(false, 0));
               }
            }

         wt::instance<none>& ours = *ours_built;
         auto measure_ours = [&](bool ok, uint64_t value) {
            auto memory = ours.memory();
            return outcome{ok, value, memory.size() ? fnv(memory.data(), memory.size()) : 0,
                           memory.size() / 65536};
         };
         wasmtime_results.push_back(measure_ours(true, 0));
         if (wasmtime_results.back().ok)
            for (size_t i = 1; i < names.size(); ++i)
            {
               const auto& type = mod.get_function_type(mod.get_exported_function(names[i]));
               std::vector<uint8_t> params(type.param_types.begin(), type.param_types.end());
               ours.set_stack_remaining(budget);
               try
               {
                  uint64_t value = 0;
                  with_arguments(params, 0, [&](auto... a) {
                     if (type.return_types.empty())
                        ours.call_discarding_results(names[i], a...);
                     else if (type.return_types[0] == psizam::types::i32)
                        value = ours.template call<uint32_t>(names[i], a...);
                     else if (type.return_types[0] == psizam::types::i64)
                        value = ours.template call<uint64_t>(names[i], a...);
                     else if (type.return_types[0] == psizam::types::f32)
                     {
                        float f = ours.template call<float>(names[i], a...);
                        uint32_t bits;
                        std::memcpy(&bits, &f, 4);
                        value = bits;
                     }
                     else
                     {
                        double d = ours.template call<double>(names[i], a...);
                        std::memcpy(&value, &d, 8);
                     }
                     return std::optional<psizam::detail::operand_stack_elem>{};
                  });
                  wasmtime_results.push_back(measure_ours(true, value));
               }
               catch (...)
               {
                  wasmtime_results.push_back(measure_ours(false, 0));
               }
            }
      }
      catch (const std::exception& e)
      {
         note = e.what();
      }

      calls += wasmtime_results.size();
      bool same = note.empty() && reference_results.size() == wasmtime_results.size();
      size_t at = 0;
      for (; same && at < reference_results.size(); ++at)
         if (!(reference_results[at] == wasmtime_results[at]))
            same = false;
      if (!same)
      {
         ++different;
         if (!note.empty())
            std::printf("DIFF %s: %s\n", argv[m], note.c_str());
         else if (reference_results.size() != wasmtime_results.size())
            std::printf("DIFF %s: psizam ran %zu calls, wasmtime %zu\n", argv[m],
                        reference_results.size(), wasmtime_results.size());
         else
         {
            const auto& r = reference_results[at - 1];
            const auto& w = wasmtime_results[at - 1];
            std::printf("DIFF %s: call %zu %s: psizam ok=%d value=%llx pages=%zu memory=%llx; "
                        "wasmtime ok=%d value=%llx pages=%zu memory=%llx\n",
                        argv[m], at - 1, names[at - 1].c_str(), r.ok,
                        (unsigned long long)r.value, r.pages, (unsigned long long)r.memory, w.ok,
                        (unsigned long long)w.value, w.pages, (unsigned long long)w.memory);
         }
      }
   }
   std::printf("modules run %zu (skipped %zu not admitted), calls compared %zu, modules that "
               "behaved differently %zu\n",
               modules, skipped, calls, different);
   return different > 255 ? 255 : int(different);
}
