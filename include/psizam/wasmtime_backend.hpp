#pragma once

// psizam/wasmtime_backend.hpp — wasmtime as a psizam execution engine.
//
// Built only with PSIZAM_ENABLE_WASMTIME. The engine is upstream wasmtime
// (Cranelift) behind a small Rust shim (wasmtime/shim); this header is the
// C++ side. Host functions are bound with templates: each one gets its own
// entry point that unpacks its arguments statically, so there is no per-call
// type dispatch on the C++ side.
//
// What is deterministic here: float results (positive canonical NaN, fixed
// relaxed-SIMD behaviour), memory and table limits, and the accepted proposal
// set, which is stated in full. What is not yet: the stack-overflow point,
// which is wasmtime's native-stack limit until the psizam fork adds the
// logical byte counter. Admission (psizam's validator and limits) is the
// embedder's job and happens before a module reaches this engine.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace psizam::wasmtime {

namespace abi {
   extern "C" {
   struct PzwtEngine;
   struct PzwtModule;
   struct PzwtStore;
   struct PzwtContext;
   struct PzwtLinker;
   struct PzwtInstance;
   struct PzwtFunc;
   struct PzwtInterrupter;

   struct PzwtConfig {
      uint32_t features;
      uint32_t float_profile;
      uint64_t max_wasm_stack;
      uint8_t  epoch_interruption;
   };
   struct PzwtError {
      int32_t status;
      int32_t trap_code;
      char*   message;
   };
   struct PzwtLimits {
      uint64_t max_memory_bytes;
      uint64_t max_table_elements;
   };
   // Matches wasmtime's ValRaw: 16 bytes, 16-aligned, little-endian payload.
   union alignas(16) PzwtValue {
      int32_t  i32;
      int64_t  i64;
      uint32_t f32;
      uint64_t f64;
      uint8_t  v128[16];
   };
   using PzwtHostFn = int32_t (*)(void* env, PzwtContext*, void* host, uint8_t* memory,
                                  size_t memory_len, PzwtValue* values);
   struct PzwtImport {
      const char*    module;
      size_t         module_len;
      const char*    name;
      size_t         name_len;
      const uint8_t* params;
      size_t         params_len;
      const uint8_t* results;
      size_t         results_len;
      PzwtHostFn     callback;
      void*          env;
      uint8_t        needs_memory;
   };

   void        pzwt_string_free(char*);
   PzwtEngine* pzwt_engine_new(const PzwtConfig*, PzwtError*);
   void        pzwt_engine_delete(PzwtEngine*);
   int32_t     pzwt_wat_to_wasm(const char*, size_t, void (*)(void*, const uint8_t*, size_t), void*,
                                PzwtError*);
   PzwtModule* pzwt_module_new(const PzwtEngine*, const uint8_t*, size_t, PzwtError*);
   PzwtModule* pzwt_module_new_metered(const PzwtEngine*, const uint8_t*, size_t, const uint32_t*,
                                       size_t, PzwtError*);
   int32_t     pzwt_module_serialize(const PzwtModule*, void (*)(void*, const uint8_t*, size_t),
                                     void*, PzwtError*);
   PzwtModule* pzwt_module_deserialize(const PzwtEngine*, const uint8_t*, size_t, PzwtError*);
   void        pzwt_module_delete(PzwtModule*);
   PzwtStore*  pzwt_store_new(const PzwtEngine*, void* host, const PzwtLimits*);
   PzwtContext* pzwt_store_context(PzwtStore*);
   void         pzwt_store_delete(PzwtStore*);
   PzwtInterrupter* pzwt_store_interrupter(PzwtStore*);
   void             pzwt_interrupter_fire(const PzwtInterrupter*);
   void             pzwt_interrupter_delete(PzwtInterrupter*);
   void        pzwt_context_memory(PzwtContext*, uint8_t** base, size_t* len);
   void        pzwt_context_set_stack(PzwtContext*, uint32_t bytes);
   uint32_t    pzwt_context_stack(PzwtContext*);
   PzwtLinker* pzwt_linker_new(const PzwtEngine*, const PzwtImport*, size_t, PzwtError*);
   void        pzwt_linker_delete(PzwtLinker*);
   PzwtInstance* pzwt_instantiate(PzwtContext*, const PzwtLinker*, const PzwtModule*, PzwtError*);
   void          pzwt_instance_delete(PzwtInstance*);
   PzwtFunc* pzwt_instance_func(PzwtContext*, const PzwtInstance*, const char* name, size_t name_len,
                                uint8_t* params, size_t* params_len, uint8_t* results,
                                size_t* results_len, size_t capacity);
   void      pzwt_func_delete(PzwtFunc*);
   int32_t   pzwt_call(PzwtContext*, const PzwtFunc*, PzwtValue*, size_t, PzwtError*);
   }
}  // namespace abi

// ── errors ──────────────────────────────────────────────────────────────────

struct error : std::runtime_error {
   using std::runtime_error::runtime_error;
};

enum class trap_kind : int32_t {
   other                     = 0,
   stack_overflow            = 1,
   memory_out_of_bounds      = 2,
   table_out_of_bounds       = 3,
   indirect_call_to_null     = 4,
   bad_signature             = 5,
   integer_overflow          = 6,
   integer_division_by_zero  = 7,
   bad_conversion_to_integer = 8,
   unreachable               = 9,
   interrupt                 = 10,
   heap_misaligned           = 11,
   atomic_wait_non_shared    = 12,
};

struct trap : error {
   trap_kind kind;
   trap(trap_kind kind, const std::string& what) : error(what), kind(kind) {}
};

// A host function was handed a pointer/length that is not inside linear memory.
struct host_memory_error : error {
   using error::error;
};

// ── options ─────────────────────────────────────────────────────────────────

namespace feature {
   inline constexpr uint32_t simd                    = 1u << 0;
   inline constexpr uint32_t relaxed_simd            = 1u << 1;
   inline constexpr uint32_t bulk_memory             = 1u << 2;
   inline constexpr uint32_t reference_types         = 1u << 3;
   inline constexpr uint32_t multi_value             = 1u << 4;
   inline constexpr uint32_t threads                 = 1u << 5;
   inline constexpr uint32_t tail_call               = 1u << 6;
   inline constexpr uint32_t sign_extension          = 1u << 7;
   inline constexpr uint32_t saturating_float_to_int = 1u << 8;
}

// Both profiles are deterministic. They differ only in relaxed madd/nmadd:
// performance fuses (one rounding), strict follows the WebAssembly
// deterministic profile (two roundings). Strict needs the wasmtime fork.
enum class float_profile : uint32_t { performance = 0, strict = 1 };

struct engine_options {
   uint32_t      features = feature::simd | feature::relaxed_simd | feature::bulk_memory |
                            feature::sign_extension | feature::saturating_float_to_int;
   float_profile fp       = float_profile::performance;
   // Native stack for wasm frames. Zero keeps wasmtime's default.
   uint64_t max_wasm_stack     = 0;
   bool     epoch_interruption = true;
};

struct instance_limits {
   uint64_t max_memory_bytes   = uint64_t(512) * 65536;
   uint64_t max_table_elements = 8192;
};

// ── value mapping ───────────────────────────────────────────────────────────

namespace detail {
   inline constexpr uint8_t type_i32 = 0, type_i64 = 1, type_f32 = 2, type_f64 = 3;

   [[noreturn]] inline void raise(abi::PzwtError& e)
   {
      std::string message = e.message ? e.message : "wasmtime error";
      abi::pzwt_string_free(e.message);
      if (e.status == 1)
         throw trap(static_cast<trap_kind>(e.trap_code), message);
      throw error(message);
   }

   struct memory_view {
      uint8_t* base;
      size_t   size;
   };

   // How one C++ parameter or result maps onto wasm values.
   template <typename T, typename = void>
   struct value_traits;

   template <typename T>
   struct value_traits<T, std::enable_if_t<std::is_integral_v<T> && sizeof(T) <= 4>> {
      static constexpr std::array<uint8_t, 1> types{type_i32};
      static T    load(const abi::PzwtValue* v, memory_view) { return static_cast<T>(v->i32); }
      static void store(abi::PzwtValue* v, T x) { v->i64 = 0; v->i32 = static_cast<int32_t>(x); }
   };
   template <typename T>
   struct value_traits<T, std::enable_if_t<std::is_integral_v<T> && sizeof(T) == 8>> {
      static constexpr std::array<uint8_t, 1> types{type_i64};
      static T    load(const abi::PzwtValue* v, memory_view) { return static_cast<T>(v->i64); }
      static void store(abi::PzwtValue* v, T x) { v->i64 = static_cast<int64_t>(x); }
   };
   template <>
   struct value_traits<float> {
      static constexpr std::array<uint8_t, 1> types{type_f32};
      static float load(const abi::PzwtValue* v, memory_view)
      {
         float x;
         std::memcpy(&x, &v->f32, 4);
         return x;
      }
      static void store(abi::PzwtValue* v, float x) { v->i64 = 0; std::memcpy(&v->f32, &x, 4); }
   };
   template <>
   struct value_traits<double> {
      static constexpr std::array<uint8_t, 1> types{type_f64};
      static double load(const abi::PzwtValue* v, memory_view)
      {
         double x;
         std::memcpy(&x, &v->f64, 8);
         return x;
      }
      static void store(abi::PzwtValue* v, double x) { std::memcpy(&v->f64, &x, 8); }
   };
   // A span is (i32 pointer, i32 length) into the caller's linear memory.
   // An empty span is accepted wherever it points, matching psizam's engines,
   // which validate by touching the last byte.
   template <typename T, std::size_t Extent>
   struct value_traits<std::span<T, Extent>,
                       std::enable_if_t<Extent == std::dynamic_extent && alignof(T) == 1>> {
      static constexpr std::array<uint8_t, 2> types{type_i32, type_i32};
      static std::span<T> load(const abi::PzwtValue* v, memory_view m)
      {
         uint64_t ptr   = static_cast<uint32_t>(v[0].i32);
         uint64_t count = static_cast<uint32_t>(v[1].i32);
         if (count == 0)
            return {};
         uint64_t bytes = count * sizeof(T);
         if (ptr > m.size || bytes > m.size - ptr)
            throw host_memory_error("host span is outside linear memory");
         return {reinterpret_cast<T*>(m.base + ptr), static_cast<size_t>(count)};
      }
   };

   template <typename... Args>
   constexpr auto param_types()
   {
      std::array<uint8_t, (value_traits<std::decay_t<Args>>::types.size() + ... + 0)> out{};
      size_t                                                                       n = 0;
      auto append = [&](const auto& types) {
         for (auto t : types)
            out[n++] = t;
      };
      (append(value_traits<std::decay_t<Args>>::types), ...);
      return out;
   }

   template <typename R>
   constexpr auto result_types()
   {
      if constexpr (std::is_void_v<R>)
         return std::array<uint8_t, 0>{};
      else
         return value_traits<R>::types;
   }

   // What a store carries as its host pointer: the user's host object plus the
   // state a host call needs to re-enter the store and to carry an exception
   // across the wasm frames, which C++ cannot unwind through.
   struct store_state {
      void*              host    = nullptr;
      abi::PzwtContext*  active  = nullptr;
      std::exception_ptr pending = nullptr;
   };

   template <typename Host, auto Fn, typename R, typename... Args>
   struct thunk {
      static constexpr auto params  = param_types<Args...>();
      static constexpr auto results = result_types<R>();
      // Only functions with a pointer argument need linear memory looked up.
      static constexpr bool needs_memory =
          ((value_traits<std::decay_t<Args>>::types.size() > 1) || ...);

      static int32_t invoke(void*, abi::PzwtContext* context, void* host, uint8_t* memory,
                            size_t memory_len, abi::PzwtValue* values) noexcept
      {
         auto* state = static_cast<store_state*>(host);
         auto* saved = std::exchange(state->active, context);
         int32_t status = 0;
         try
         {
            memory_view m{memory, memory_len};
            size_t      slot = 0;
            // Braced initialization fixes left-to-right evaluation, so each
            // argument reads the slots that follow the previous one.
            std::tuple<std::decay_t<Args>...> args{load<Args>(values, slot, m)...};
            auto& self = *static_cast<Host*>(state->host);
            if constexpr (std::is_void_v<R>)
               std::apply([&](auto&... a) { (self.*Fn)(a...); }, args);
            else
               value_traits<R>::store(values,
                                      std::apply([&](auto&... a) { return (self.*Fn)(a...); }, args));
         }
         catch (...)
         {
            state->pending = std::current_exception();
            status         = 1;
         }
         state->active = saved;
         return status;
      }

     private:
      template <typename A>
      static std::decay_t<A> load(const abi::PzwtValue* values, size_t& slot, memory_view m)
      {
         using traits = value_traits<std::decay_t<A>>;
         auto at      = slot;
         slot += traits::types.size();
         return traits::load(values + at, m);
      }
   };

   template <typename Host, auto Fn, typename R, typename C, typename... Args>
   abi::PzwtImport make_import(R (C::*)(Args...), std::string_view module, std::string_view name)
   {
      using T = thunk<Host, Fn, R, Args...>;
      return {module.data(),    module.size(),    name.data(),         name.size(),
              T::params.data(), T::params.size(), T::results.data(),   T::results.size(),
              &T::invoke,       nullptr,          T::needs_memory};
   }
}  // namespace detail

// Assembles WebAssembly text into a binary module.
inline std::vector<uint8_t> assemble(std::string_view wat)
{
   std::vector<uint8_t> out;
   abi::PzwtError       e{};
   auto sink = [](void* self, const uint8_t* data, size_t size) {
      static_cast<std::vector<uint8_t>*>(self)->assign(data, data + size);
   };
   if (abi::pzwt_wat_to_wasm(wat.data(), wat.size(), sink, &out, &e))
      detail::raise(e);
   return out;
}

// ── module ──────────────────────────────────────────────────────────────────

class module {
   std::shared_ptr<abi::PzwtModule> ptr_;
   template <typename>
   friend class runtime;
   template <typename>
   friend class instance;
   explicit module(abi::PzwtModule* p) : ptr_(p, abi::pzwt_module_delete) {}

  public:
   // The compiled artifact. Valid only for the same wasmtime version, target
   // and engine options; key any cache on all three.
   std::vector<uint8_t> serialize() const
   {
      std::vector<uint8_t> out;
      abi::PzwtError       e{};
      auto sink = [](void* self, const uint8_t* data, size_t size) {
         static_cast<std::vector<uint8_t>*>(self)->assign(data, data + size);
      };
      if (abi::pzwt_module_serialize(ptr_.get(), sink, &out, &e))
         detail::raise(e);
      return out;
   }
};

// ── runtime: engine plus the host functions modules may import ──────────────

template <typename Host>
class runtime {
   std::shared_ptr<abi::PzwtEngine>       engine_;
   std::vector<std::pair<std::string, std::string>> names_;
   std::vector<abi::PzwtImport>           imports_;
   mutable std::shared_ptr<abi::PzwtLinker> linker_;
   template <typename>
   friend class instance;

   const abi::PzwtLinker* linker() const
   {
      if (!linker_)
      {
         abi::PzwtError e{};
         auto imports = imports_;
         for (size_t i = 0; i < imports.size(); ++i)
         {
            imports[i].module     = names_[i].first.data();
            imports[i].module_len = names_[i].first.size();
            imports[i].name       = names_[i].second.data();
            imports[i].name_len   = names_[i].second.size();
         }
         auto* p = abi::pzwt_linker_new(engine_.get(), imports.data(), imports.size(), &e);
         if (!p)
            detail::raise(e);
         linker_.reset(p, abi::pzwt_linker_delete);
      }
      return linker_.get();
   }

  public:
   explicit runtime(const engine_options& options = {})
   {
      abi::PzwtConfig config{options.features, static_cast<uint32_t>(options.fp),
                             options.max_wasm_stack, options.epoch_interruption};
      abi::PzwtError  e{};
      auto*           p = abi::pzwt_engine_new(&config, &e);
      if (!p)
         detail::raise(e);
      engine_.reset(p, abi::pzwt_engine_delete);
   }

   // Makes a member function of Host importable as module.name. The wasm
   // signature is derived from the C++ one.
   template <auto Fn>
   runtime& add(std::string_view module, std::string_view name)
   {
      names_.emplace_back(std::string(module), std::string(name));
      imports_.push_back(detail::make_import<Host, Fn>(Fn, module, name));
      linker_.reset();
      return *this;
   }

   // Compiles wasm (binary or text). Validation here is wasmtime's; the
   // embedder's own admission rules must already have passed.
   module compile(std::span<const uint8_t> wasm) const
   {
      abi::PzwtError e{};
      auto*          p = abi::pzwt_module_new(engine_.get(), wasm.data(), wasm.size(), &e);
      if (!p)
         detail::raise(e);
      return module(p);
   }
   module compile(std::string_view wat) const
   {
      return compile(std::span(reinterpret_cast<const uint8_t*>(wat.data()), wat.size()));
   }

   // Compiles a binary module with deterministic stack accounting. `costs`
   // holds the byte cost of each defined function, in order, as psizam's
   // validator computes them (see wasmtime_admission.hpp). Instances of the
   // result overflow at exactly the point psizam's engines do, whatever the
   // native frame sizes are; set the budget with instance::set_stack_remaining.
   module compile_metered(std::span<const uint8_t> wasm, std::span<const uint32_t> costs) const
   {
      abi::PzwtError e{};
      auto* p = abi::pzwt_module_new_metered(engine_.get(), wasm.data(), wasm.size(), costs.data(),
                                             costs.size(), &e);
      if (!p)
         detail::raise(e);
      return module(p);
   }

   // Trusts the bytes: wasmtime does not validate machine code it loads.
   module load(std::span<const uint8_t> artifact) const
   {
      abi::PzwtError e{};
      auto* p = abi::pzwt_module_deserialize(engine_.get(), artifact.data(), artifact.size(), &e);
      if (!p)
         detail::raise(e);
      return module(p);
   }
};

// Stops one instance from any thread. Takes effect at the instance's next loop
// iteration or function entry; a host function in progress finishes first.
// The instance must not be used again afterwards.
class interrupter {
   std::shared_ptr<abi::PzwtInterrupter> ptr_;
   template <typename>
   friend class instance;
   explicit interrupter(abi::PzwtInterrupter* p) : ptr_(p, abi::pzwt_interrupter_delete) {}

  public:
   void fire() const { abi::pzwt_interrupter_fire(ptr_.get()); }
};

// ── instance ────────────────────────────────────────────────────────────────

template <typename Host>
class instance {
   std::unique_ptr<detail::store_state>                              state_;
   std::unique_ptr<abi::PzwtStore, void (*)(abi::PzwtStore*)>        store_;
   std::unique_ptr<abi::PzwtInstance, void (*)(abi::PzwtInstance*)>  instance_;

   [[noreturn]] void fail(abi::PzwtError& e)
   {
      if (e.status == 2)
         std::rethrow_exception(std::exchange(state_->pending, nullptr));
      detail::raise(e);
   }

  public:
   instance(const runtime<Host>& rt, const module& m, Host& host, const instance_limits& limits = {})
       : state_(std::make_unique<detail::store_state>()),
         store_(nullptr, abi::pzwt_store_delete),
         instance_(nullptr, abi::pzwt_instance_delete)
   {
      state_->host = &host;
      abi::PzwtLimits l{limits.max_memory_bytes, limits.max_table_elements};
      store_.reset(abi::pzwt_store_new(rt.engine_.get(), state_.get(), &l));
      state_->active = abi::pzwt_store_context(store_.get());
      abi::PzwtError e{};
      auto* p = abi::pzwt_instantiate(state_->active, rt.linker(), m.ptr_.get(), &e);
      if (!p)
         fail(e);
      instance_.reset(p);
   }

   // An export bound once: the signature is checked here, not per call.
   template <typename Signature>
   class function;
   template <typename R, typename... Args>
   class function<R(Args...)> {
      instance*                                               owner_;
      std::unique_ptr<abi::PzwtFunc, void (*)(abi::PzwtFunc*)> func_;
      friend class instance;
      function(instance* owner, abi::PzwtFunc* f) : owner_(owner), func_(f, abi::pzwt_func_delete) {}

     public:
      R operator()(Args... args) const
      {
         constexpr size_t n =
             std::max<size_t>({size_t(1), detail::param_types<Args...>().size(),
                               detail::result_types<R>().size()});
         abi::PzwtValue values[n];
         size_t         slot = 0;
         auto put = [&](auto x) {
            detail::value_traits<decltype(x)>::store(values + slot, x);
            slot += detail::value_traits<decltype(x)>::types.size();
         };
         (put(args), ...);
         abi::PzwtError e{};
         if (abi::pzwt_call(owner_->state_->active, func_.get(), values, n, &e))
            owner_->fail(e);
         if constexpr (!std::is_void_v<R>)
            return detail::value_traits<R>::load(values, {});
      }
   };

   template <typename Signature>
   function<Signature> bind(std::string_view name)
   {
      return bind_impl(name, static_cast<Signature*>(nullptr));
   }

   template <typename R = void, typename... Args>
   R call(std::string_view name, Args... args)
   {
      return bind<R(Args...)>(name)(args...);
   }

   std::span<char> memory()
   {
      uint8_t* base = nullptr;
      size_t   size = 0;
      abi::pzwt_context_memory(state_->active, &base, &size);
      return {reinterpret_cast<char*>(base), size};
   }

   // Stack bytes wasm may still use, for a module compiled with
   // compile_metered. The counter is not reset between calls: set it before
   // each top-level call. Without stack accounting, set does nothing and
   // stack_remaining returns UINT32_MAX.
   void     set_stack_remaining(uint32_t bytes) { abi::pzwt_context_set_stack(state_->active, bytes); }
   uint32_t stack_remaining() { return abi::pzwt_context_stack(state_->active); }

   interrupter make_interrupter() { return interrupter(abi::pzwt_store_interrupter(store_.get())); }

  private:
   template <typename R, typename... Args>
   function<R(Args...)> bind_impl(std::string_view name, R (*)(Args...))
   {
      constexpr auto want_params  = detail::param_types<Args...>();
      constexpr auto want_results = detail::result_types<R>();
      uint8_t params[32], results[32];
      size_t  nparams = 0, nresults = 0;
      auto*   f = abi::pzwt_instance_func(state_->active, instance_.get(), name.data(), name.size(),
                                          params, &nparams, results, &nresults, 32);
      if (!f)
         throw error("no callable export named " + std::string(name));
      function<R(Args...)> bound(this, f);
      if (nparams != want_params.size() || nresults != want_results.size() ||
          !std::equal(want_params.begin(), want_params.end(), params) ||
          !std::equal(want_results.begin(), want_results.end(), results))
         throw error("export " + std::string(name) + " does not have the requested signature");
      return bound;
   }
};

}  // namespace psizam::wasmtime
