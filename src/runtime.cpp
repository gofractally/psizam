// runtime.cpp — Phase 2 + Steps 1, 5 of psizam-runtime-api-maturation.
// prepare() parses + compiles WASM; instantiate() creates live instances
// dispatched on the requested backend kind via the abstract `instance_be`.
// instance provides gas_state, linear_memory, run_start, and typed proxy
// access.

#include <psizam/runtime.hpp>
#include "runtime_backend.hpp"
#include "runtime_cached.hpp"
#include "runtime_imports.hpp"
#include <psizam/arena_budget.hpp>
#include <psizam/backend.hpp>
#include <psizam/detail/instance_be.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include <psizam/detail/wasi_host.hpp>
#include <psizam/detail/wit_section_finder.hpp>
#include <psizam/host_function.hpp>
#include <psizam/host_function_table.hpp>

#include <psio/wit_parser.hpp>
#include <psio/wit_types.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace psizam {

// ═════════════════════════════════════════════════════════════════════
// detail::instance_be_impl<Impl>
// ═════════════════════════════════════════════════════════════════════
//
// One concrete realization per backend variant. Each owns a fresh
// wasm_allocator and the typed `backend<std::nullptr_t, Impl>` that
// drives execution.

namespace detail {


std::unique_ptr<instance_be> make_instance_be(
   backend_kind kind,
   const std::vector<uint8_t>& wasm,
   host_function_table table,
   void* host_ptr,
   char* external_base,
   uint32_t external_budget_bytes, bool gas_metering)
{
   auto make = [&]<typename Impl>(Impl*) -> std::unique_ptr<instance_be> {
      if (external_base)
         return std::make_unique<instance_be_impl<Impl>>(
            kind, wasm, std::move(table), host_ptr,
            external_base, external_budget_bytes, gas_metering);
      else
         return std::make_unique<instance_be_impl<Impl>>(
            kind, wasm, std::move(table), host_ptr, gas_metering);
   };

   switch (kind) {
      case backend_kind::interpreter:
         return make(static_cast<interpreter*>(nullptr));

#if defined(__x86_64__) || defined(__aarch64__)
      case backend_kind::jit:
         return make(static_cast<jit*>(nullptr));
      case backend_kind::jit2:
         return make(static_cast<jit2*>(nullptr));
#else
      case backend_kind::jit:
      case backend_kind::jit2:
         throw std::runtime_error{
            "psizam: jit / jit2 backend requires x86_64 or aarch64"};
#endif

      case backend_kind::jit_llvm:
         throw std::runtime_error{
            "psizam: LLVM runtime requires the psizam-llvm library"};
   }
   throw std::runtime_error{"psizam: unknown backend_kind"};
}

// Map instance_policy::compile_tier → detail::backend_kind. Lives here
// (rather than the public header) so the policy enum can keep its
// runtime-facing names while the dispatch switch reads the canonical
// backend identity.
backend_kind tier_to_backend_kind(instance_policy::compile_tier t) noexcept {
   switch (t) {
      case instance_policy::compile_tier::interpret: return backend_kind::interpreter;
      case instance_policy::compile_tier::jit1:      return backend_kind::jit;
      case instance_policy::compile_tier::jit2:      return backend_kind::jit2;
      case instance_policy::compile_tier::jit_llvm:  return backend_kind::jit_llvm;
   }
   return backend_kind::interpreter;
}

} // namespace detail

// ═════════════════════════════════════════════════════════════════════
// module_handle_impl — owns the WASM bytes and host function table
// template. The backend is created per-instance (each gets its own
// linear memory) but the module/table are shared.
// ═════════════════════════════════════════════════════════════════════

struct module_handle_impl {
   // Common
   host_function_table   table;
   instance_policy       policy;
   void*                 host_ptr = nullptr;
   uint32_t              compile_ms = 0;

   // prepare()-path state — raw WASM bytes; backend constructed at
   // instantiate() time.
   std::vector<uint8_t>  wasm_copy;

   // Parsed once; each instance relocates its own executable mapping and
   // restores independent globals, tables and linear memory.
   std::unique_ptr<pzam_file> pzam;

   // Late-bound back-pointer to the currently-live instance's
   // `instance_be*`. Set by `runtime::instantiate` and nulled by the
   // instance's destructor. Cross-module bridges registered before
   // the consumer is instantiated capture a pointer to this field
   // and deref it at call time — the composition pattern, adapted
   // to the module_handle / instance split.
   //
   // Single-instance-per-module today; multi-instance sharing of a
   // module_handle through the bridge path is a follow-up (would
   // need per-instance bridge tables or a lookup indirection).
   detail::instance_be*  live_be = nullptr;

   module_handle_impl() = default;

};

host_function_table& module_handle::table() { return impl_->table; }
void module_handle::set_host_ptr(void* p) { impl_->host_ptr = p; }

namespace detail_runtime {
   detail::instance_be* live_be_of(const module_handle_impl* mod) {
      return mod ? mod->live_be : nullptr;
   }
}

uint32_t module_handle::compile_time_ms() const {
   return impl_ ? impl_->compile_ms : 0;
}
uint32_t module_handle::native_code_size() const {
   if (!impl_) return 0;
   if (impl_->pzam) return static_cast<uint32_t>(detail::pick_code_section(*impl_->pzam).code_blob.size());
   return 0;   // prepare()-path: real value lands with Track B's compile.
}
uint32_t module_handle::wasm_size() const {
   return impl_ ? static_cast<uint32_t>(impl_->wasm_copy.size()) : 0;
}
std::vector<std::string> module_handle::wit_sections() const { return {}; }

// ═════════════════════════════════════════════════════════════════════
// instance_impl — owns a polymorphic backend holder + gas state
// ═════════════════════════════════════════════════════════════════════

struct instance_impl {
   // Strong reference to the module; keeps module_handle_impl alive as
   // long as the instance does so bridge lambdas registered on the
   // module's host_function_table can deref `module->live_be` safely.
   std::shared_ptr<module_handle_impl>  module;
   std::unique_ptr<detail::instance_be> be;
   // Shared pool for multi-module gas tracking. Kept alive for the
   // instance's lifetime so the pool outlives any leases.
   std::shared_ptr<gas_pool>            pool;
   void*                                host_ptr = nullptr;
   std::chrono::steady_clock::time_point wall_deadline;
   uint64_t gas_slice = 0;
   gas_handler_t yield_handler = nullptr;
   void* yield_user_data = nullptr;
   instance_policy::meter_mode metering = instance_policy::meter_mode::none;

   gas_state* active_gas() {
      return be ? &be->gas() : nullptr;
   }

   static void on_deadline(gas_state* gas, void* data) {
      auto& self = *static_cast<instance_impl*>(data);
      // A public interrupt must also stop instances with refill handlers.
      if (gas->deadline.load(std::memory_order_relaxed) == 0)
         throw wasm_gas_exhausted_exception{"runtime: interrupted"};
      if (self.pool) {
         pool_yield_handler(gas, self.pool.get());
      } else if (self.metering == instance_policy::meter_mode::timeout) {
         if (std::chrono::steady_clock::now() >= self.wall_deadline)
            throw timeout_exception{"runtime: execution timed out"};
         const auto room = std::numeric_limits<uint64_t>::max() - gas->consumed;
         if (!room) throw timeout_exception{"runtime: timeout counter exhausted"};
         gas->deadline.store(gas->consumed + std::min(room, self.gas_slice),
                             std::memory_order_relaxed);
      } else {
         self.yield_handler(gas, self.yield_user_data);
      }
   }

   ~instance_impl() {
      // Null the module's live_be back-pointer if it was pointing at us.
      // Guards against a later instantiate() overwriting it first and
      // this destructor then mistakenly nulling the newer instance.
      if (module && be && module->live_be == be.get()) {
         module->live_be = nullptr;
      }

      // Multi-module pool: credit any unused lease back to the pool so
      // subsequent instances (or teardown reporting) see the correct
      // remaining balance. When the pool handler failed (threw), the
      // deadline equals consumed so the delta is zero — no credit.
      if (pool && be) {
         const uint64_t consumed = be->gas_consumed_raw();
         const uint64_t deadline = be->gas_deadline_raw();
         if (deadline > consumed) {
            pool->remaining.fetch_add(deadline - consumed,
                                      std::memory_order_relaxed);
         }
      }
   }
};

instance::instance() = default;
instance::instance(std::unique_ptr<instance_impl> p) : impl_(std::move(p)) {}
instance::~instance() = default;
instance::instance(instance&&) noexcept = default;
instance& instance::operator=(instance&&) noexcept = default;

uint64_t instance::gas_consumed() const {
   return impl_ ? impl_->active_gas()->consumed : 0;
}
uint64_t instance::gas_deadline() const {
   return impl_ ? impl_->active_gas()->deadline.load(std::memory_order_relaxed) : 0;
}
void instance::set_deadline(uint64_t d) {
   if (impl_) impl_->active_gas()->deadline.store(d, std::memory_order_relaxed);
}
void instance::interrupt() {
   if (impl_) impl_->active_gas()->deadline.store(0, std::memory_order_relaxed);
}
gas_state* instance::gas() {
   return impl_ ? impl_->active_gas() : nullptr;
}
const gas_state* instance::gas() const {
   return impl_ ? impl_->active_gas() : nullptr;
}
void* instance::backend_ptr() {
   return impl_ && impl_->be ? impl_->be->raw_backend() : nullptr;
}
void* instance::host_ptr() {
   return impl_ ? impl_->host_ptr : nullptr;
}
detail::instance_be* instance::get_instance_be() {
   return impl_ ? impl_->be.get() : nullptr;
}

char* instance::linear_memory() {
   return impl_ && impl_->be ? impl_->be->linear_memory() : nullptr;
}
std::size_t instance::memory_size() const {
   return impl_ && impl_->be
      ? static_cast<std::size_t>(impl_->be->current_pages()) * 65536
      : 0;
}

// Enum value parity (one source of truth, two declarations) — public
// `psizam::backend_kind` and internal `detail::backend_kind` must stay
// in lockstep because instance::kind() casts between them.
static_assert(static_cast<uint8_t>(backend_kind::interpreter) ==
              static_cast<uint8_t>(detail::backend_kind::interpreter));
static_assert(static_cast<uint8_t>(backend_kind::jit) ==
              static_cast<uint8_t>(detail::backend_kind::jit));
static_assert(static_cast<uint8_t>(backend_kind::jit2) ==
              static_cast<uint8_t>(detail::backend_kind::jit2));
static_assert(static_cast<uint8_t>(backend_kind::jit_llvm) ==
              static_cast<uint8_t>(detail::backend_kind::jit_llvm));

backend_kind instance::kind() const {
   if (!impl_ || !impl_->be) return backend_kind::interpreter;
   return static_cast<backend_kind>(impl_->be->kind());
}

int instance::run_start() {
   if (!impl_ || !impl_->be) return -1;
   return impl_->be->run_start(impl_->host_ptr);
}

// ═════════════════════════════════════════════════════════════════════
// runtime_impl
// ═════════════════════════════════════════════════════════════════════

// Stored bytes for a registered library — the runtime keeps the bytes
// for future link / lookup; archive parsing into member .o files is left
// to whatever consumes them downstream (this layer just owns the data).
struct registered_library {
   enum class kind { wasm, archive };
   kind                   kind;
   std::vector<uint8_t>   bytes;
};

// Stable 64-bit hash over a wasm byte range. Used as the module-cache
// key. Track B will replace this with module_cache_key (composing
// wasm_hash + compile_hash + env_hash) once compile_policy lands.
inline uint64_t hash_wasm_bytes(std::span<const uint8_t> bytes) noexcept {
   return std::hash<std::string_view>{}(std::string_view{
      reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

// ── Guarded region pool ──────────────────────────────────────────────
//
// Pre-maps N guarded regions at runtime construction. Instances lease
// a region instead of mmap'ing their own. On release the memory is
// zeroed and returned to the free list. No kernel calls in the
// steady state.

struct guarded_region {
   char*  base      = nullptr;  // raw mmap base (before prefix adjustment)
   size_t total_len = 0;        // total mmap size including prefix + suffix
};

struct guarded_pool {
   std::vector<guarded_region> all;       // all regions (indexed)
   std::vector<size_t>         free_list; // indices of available regions

   explicit guarded_pool(size_t count) {
      if (count == 0) return;
      const size_t prefix  = wasm_allocator::prefix_size_static();
      const size_t suffix  = wasm_allocator::suffix_size_static();
      const size_t total   = max_memory + prefix + suffix;

      all.reserve(count);
      free_list.reserve(count);
      for (size_t i = 0; i < count; ++i) {
         char* base = static_cast<char*>(
            ::mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
         if (base == MAP_FAILED) break;
         // Pre-commit the table page (prefix region) so the allocator
         // doesn't need mprotect on first use.
         ::mprotect(base, wasm_allocator::table_size(), PROT_READ | PROT_WRITE);
         all.push_back({base, total});
         free_list.push_back(i);
      }
   }

   ~guarded_pool() {
      for (auto& r : all)
         if (r.base) ::munmap(r.base, r.total_len);
   }

   // Lease a region. Returns (adjusted raw ptr, index) or (nullptr, -1).
   std::pair<char*, size_t> lease() {
      if (free_list.empty()) return {nullptr, SIZE_MAX};
      size_t idx = free_list.back();
      free_list.pop_back();
      char* raw = all[idx].base + wasm_allocator::prefix_size_static();
      return {raw, idx};
   }

   void release(size_t idx) {
      if (idx >= all.size()) return;
      // Zero the usable region to ensure determinism on reuse.
      // The prefix (table page) is re-zeroed by wasm_allocator::reset().
      free_list.push_back(idx);
   }

   size_t available() const { return free_list.size(); }
   size_t capacity()  const { return all.size(); }

   guarded_pool(const guarded_pool&) = delete;
   guarded_pool& operator=(const guarded_pool&) = delete;
};

struct runtime_impl {
   runtime_config config;
   detail::instance_backend_factory factory;
   guarded_pool   pool;
   arena_budget   arena;

   // Wasm hash → most recently prepared handle, for weak bookkeeping only.
   // Mutable policies/bindings are never reused on a hash hit.
   std::unordered_map<uint64_t, std::weak_ptr<module_handle_impl>>
      module_cache;

   // Library name → bytes + kind. Lookup-by-name is what consumers
   // need; archive parsing happens on demand at link time.
   std::unordered_map<std::string, registered_library>
      libraries;

   runtime_impl(runtime_config cfg, detail::instance_backend_factory make)
      : config(cfg), factory(make)
      , pool(cfg.guarded_pool_size)
      , arena(cfg.arena_size_gb * std::size_t{1024 * 1024 * 1024})
   {}
};

runtime::runtime(runtime_config config, detail::instance_backend_factory factory)
   : impl_(std::make_unique<runtime_impl>(config, factory)) {
   if (!factory) throw std::invalid_argument("runtime: null backend factory");
}

runtime::~runtime() = default;
runtime::runtime(runtime&&) noexcept = default;
runtime& runtime::operator=(runtime&&) noexcept = default;

void runtime::register_library(std::string_view name, archive_bytes archive) {
   if (!impl_) return;
   registered_library lib;
   lib.kind = registered_library::kind::archive;
   lib.bytes.assign(archive.data.begin(), archive.data.end());
   impl_->libraries[std::string{name}] = std::move(lib);
}

void runtime::register_library(std::string_view name, wasm_bytes wasm) {
   if (!impl_) return;
   registered_library lib;
   lib.kind = registered_library::kind::wasm;
   lib.bytes.assign(wasm.data.begin(), wasm.data.end());
   impl_->libraries[std::string{name}] = std::move(lib);
}

module_handle runtime::prepare(wasm_bytes wasm, const instance_policy& policy) {
   const uint64_t key = hash_wasm_bytes(wasm.data);
   // Handles contain mutable bindings and an instance policy. Returning a
   // previous handle by Wasm hash aliases both, even with identical policies.
   // Native-code caching belongs in a separate immutable compiled template.

   const auto t0 = std::chrono::high_resolution_clock::now();

   auto mod = std::make_shared<module_handle_impl>();
   mod->wasm_copy.assign(wasm.data.begin(), wasm.data.end());
   mod->policy = policy;

   const auto t1 = std::chrono::high_resolution_clock::now();
   mod->compile_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

   if (impl_)
      impl_->module_cache[key] = std::weak_ptr<module_handle_impl>{mod};

   return module_handle{std::move(mod)};
}

module_handle runtime::load_cached(pzam_bytes pzam) {
   const auto t0 = std::chrono::high_resolution_clock::now();

   // Parse the .pzam container envelope.
   const std::span<const char> raw{
      reinterpret_cast<const char*>(pzam.data.data()),
      pzam.data.size()};
   if (!pzam_validate(raw))
      throw std::runtime_error{"runtime::load_cached: invalid .pzam"};

   pzam_file file = pzam_load(raw);
   if (file.magic != PZAM_MAGIC)
      throw std::runtime_error{"runtime::load_cached: bad .pzam magic"};

   auto mod = std::make_shared<module_handle_impl>();
   mod->pzam = std::make_unique<pzam_file>(std::move(file));
   const auto& section = detail::pick_code_section(*mod->pzam);
   PSIZAM_ASSERT(pzam_page_layout_compatible(section), wasm_parse_exception, pzam_page_layout_error);
   switch (static_cast<pzam_opt_tier>(section.opt_tier)) {
      case pzam_opt_tier::jit1: mod->policy.initial = instance_policy::compile_tier::jit1; break;
      case pzam_opt_tier::jit2: mod->policy.initial = instance_policy::compile_tier::jit2; break;
      case pzam_opt_tier::llvm_O0: case pzam_opt_tier::llvm_O1:
      case pzam_opt_tier::llvm_O2: case pzam_opt_tier::llvm_O3:
         mod->policy.initial = instance_policy::compile_tier::jit_llvm; break;
      default: throw std::invalid_argument("runtime::load_cached: unsupported PZAM tier");
   }
   mod->policy.metering = section.instrumentation.gas_metering ?
      instance_policy::meter_mode::gas_trap : instance_policy::meter_mode::none;
   mod->policy.floats = section.instrumentation.softfloat ?
      instance_policy::float_mode::soft : instance_policy::float_mode::native;

   const auto t1 = std::chrono::high_resolution_clock::now();
   mod->compile_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

   return module_handle{std::move(mod)};
}

std::vector<unresolved_import> runtime::check(wasm_bytes wasm) const {
   return detail::check_wasm_imports(wasm_code(wasm.data.begin(), wasm.data.end()), {});
}

std::vector<unresolved_import> runtime::check(const module_handle& tmpl) const {
   if (!tmpl) throw std::invalid_argument("runtime::check: empty module handle");
   auto& source = *tmpl.get();
   if (source.pzam) {
      auto mod = restore_module(source.pzam->metadata);
      return detail::check_module_imports(mod, source.table);
   }
   return detail::check_wasm_imports(source.wasm_copy, source.table);
}

instance runtime::instantiate(const module_handle& tmpl) {
   return tmpl ? instantiate(tmpl, tmpl.get()->policy) : instance{};
}

instance runtime::instantiate(const module_handle& tmpl,
                               const instance_policy& policy) {
   auto* mod = tmpl.get();
   if (!mod) return instance{};

   auto inst = std::make_unique<instance_impl>();
   inst->module = tmpl.share_impl();   // keep module_handle_impl alive
   inst->host_ptr = mod->host_ptr;
   using meter = instance_policy::meter_mode;
   if (policy.pool && policy.metering != meter::gas_trap)
      throw std::invalid_argument("runtime: gas_pool requires gas_trap metering");
   if (policy.metering == meter::gas_yield && !policy.yield_handler)
      throw std::invalid_argument("runtime: gas_yield requires yield_handler");
   if ((policy.metering == meter::timeout || policy.metering == meter::gas_yield)
         && policy.gas_slice == 0)
      throw std::invalid_argument("runtime: gas_slice must be positive");
   if (policy.external_memory && (policy.mem_budget < 16 || policy.mem_budget > 31))
      throw std::invalid_argument("runtime: external memory budget must be 16..31");
   if (policy.metering != meter::none && policy.metering != meter::gas_trap &&
       policy.metering != meter::gas_yield && policy.metering != meter::timeout)
      throw std::invalid_argument("runtime: invalid metering mode");
   inst->metering = policy.metering;
   inst->gas_slice = policy.gas_slice;
   inst->yield_handler = policy.yield_handler;
   inst->yield_user_data = policy.yield_user_data;

   // Pick the backend impl per `policy.initial`. The factory throws on
   // backends gated out of this build (jit/jit2 on non-x86/arm,
   // jit_llvm without PSIZAM_ENABLE_LLVM_BACKEND).
   //
   // If external_memory is set, the instance uses caller-provided memory
   // instead of mmap'ing its own guarded region. The mem_budget field
   // (power-of-2 exponent) determines the max size.
   auto be_kind = detail::tier_to_backend_kind(policy.initial);
   auto table_copy = mod->table;

   char*    ext_base  = policy.external_memory;
   uint32_t ext_bytes = ext_base
      ? (1u << policy.mem_budget)
      : 0;

   if (mod->pzam)
      inst->be = detail::make_cached_instance_be(*mod->pzam, policy,
         std::move(table_copy), mod->host_ptr, ext_base, ext_bytes);
   else
      inst->be = impl_->factory(
         be_kind, mod->wasm_copy, std::move(table_copy), mod->host_ptr,
         ext_base, ext_bytes, policy.metering != meter::none);

   inst->be->set_gas_budget(policy.metering == meter::none ? gas_budget_unlimited :
      policy.metering == meter::gas_trap ? policy.gas_budget : policy.gas_slice);
   inst->wall_deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(policy.timeout_ms);
   if (policy.metering == meter::timeout || policy.metering == meter::gas_yield)
      inst->be->set_gas_handler(&instance_impl::on_deadline, inst.get());

   // Publish the instance's be pointer on the module so any bridge
   // lambdas registered on the module's table (by a prior bind()) can
   // reach the consumer at call time.
   mod->live_be = inst->be.get();

   // Multi-module gas tracking: if a pool is provided, lease an initial
   // deadline from it and wire pool_yield_handler so future handler
   // entries debit more from the pool. Each instance's teardown credits
   // any unused lease back (see instance_impl::~instance_impl).
   if (policy.pool) {
      auto lease = policy.pool->try_lease(policy.pool->lease_size);
      if (*lease == 0) {
         throw std::runtime_error{
            "runtime::instantiate: gas_pool exhausted at instance creation"};
      }
      inst->pool = policy.pool;
      inst->be->set_gas_budget(*lease);
      inst->be->set_gas_handler(&instance_impl::on_deadline, inst.get());
   }

   inst->be->initialize_start(mod->host_ptr);

   return instance{std::move(inst)};
}

void runtime::bind(module_handle& consumer_mod, instance& provider,
                   std::string_view interface_name)
{
   if (!consumer_mod || !provider) return;

   auto* mod_impl = consumer_mod.get();
   if (mod_impl->wasm_copy.empty())
      throw std::runtime_error{
         "runtime::bind(dynamic): consumer module has no WASM bytes; "
         "load_cached-path modules don't carry WIT custom sections — "
         "use the typed bind<InterfaceTag>() overload instead"};

   // Find the `component-wit:<interface_name>` custom section in the
   // consumer's raw WASM module.
   std::span<const uint8_t> wit_payload;
   const std::span<const uint8_t> wasm{mod_impl->wasm_copy};
   if (!detail::find_component_wit_section(wasm, interface_name, wit_payload))
      throw std::runtime_error{
         std::string{"runtime::bind(dynamic): consumer has no "
                     "component-wit:"} + std::string{interface_name} +
         " custom section"};

   // Parse the WIT text. The component-wit section payload is utf-8 text.
   const std::string_view wit_text{
      reinterpret_cast<const char*>(wit_payload.data()), wit_payload.size()};
   psio::wit_world world = psio::wit_parser::parse_wit(wit_text);

   // Locate the interface by name. Consumer-side: the named interface
   // is what the consumer IMPORTS from the provider, so search imports
   // first; fall back to exports for symmetric / round-tripped WIT.
   const psio::wit_interface* iface = nullptr;
   for (const auto& candidate : world.imports)
      if (candidate.name == interface_name) { iface = &candidate; break; }
   if (!iface)
      for (const auto& candidate : world.exports)
         if (candidate.name == interface_name) { iface = &candidate; break; }
   if (!iface)
      throw std::runtime_error{
         std::string{"runtime::bind(dynamic): interface `"} +
         std::string{interface_name} +
         "` not found in consumer's WIT world"};

   // For each WIT-declared method, register a bridge entry on the
   // consumer's host function table. The entry's slow_dispatch routes
   // calls through a validated callable resolved at bind time, independently
   // of the provider backend. Each call retains its trap boundary.
   //
   // The bridge uses the canonical-ABI 16-slot flat call convention
   // (matches the typed bind<InterfaceTag>() in runtime.hpp). The
   // consumer's import import-side stub is generated by PSIO_WIT to
   // pack args into the 16-slot flat shape, and the provider's
   // exported function unpacks them — both sides agree on the shape
   // regardless of the WIT-declared logical signature.
   detail::instance_be* provider_be = provider.get_instance_be();
   void*                provider_host = provider.host_ptr();
   if (!provider_be)
      throw std::runtime_error{
         "runtime::bind(dynamic): provider instance has no live backend"};

   for (uint32_t func_idx : iface->func_idxs) {
      if (func_idx >= world.funcs.size()) continue;
      const psio::wit_func& wf = world.funcs[func_idx];

      host_function_table::entry e;
      e.module_name = std::string{interface_name};
      e.func_name   = wf.name;
      e.signature.params.assign(16, types::i64);
      e.signature.ret = {types::i64};

      // PSIO renders WIT names in kebab case while its core Wasm symbols
      // retain C++ underscores. Prefer an exact export, then the PSIO spelling.
      std::string export_name = wf.name;
      if (provider_be->resolve_export(export_name) == std::numeric_limits<uint32_t>::max())
         std::replace(export_name.begin(), export_name.end(), '-', '_');
      e.func_name = export_name;
      e.slow_dispatch = provider_be->bind_canonical_export(export_name, provider_host);
      if (e.func_name != wf.name) {
         auto alias = e;
         alias.func_name = wf.name;
         mod_impl->table.add_entry(std::move(alias));
      }

      mod_impl->table.add_entry(std::move(e));
   }
}

runtime::cache_stats runtime::stats() const {
   if (!impl_) return {0, 0, 0};

   std::size_t modules            = 0;
   std::size_t pzam_bytes_total   = 0;
   std::size_t compile_time_total = 0;
   for (const auto& [_, weak] : impl_->module_cache) {
      if (auto live = weak.lock()) {
         ++modules;
         if (live->pzam) pzam_bytes_total += detail::pick_code_section(*live->pzam).code_blob.size();
         compile_time_total += live->compile_ms;
      }
   }
   return {modules, pzam_bytes_total, compile_time_total};
}

void runtime::evict(wasm_bytes wasm) {
   if (!impl_) return;
   impl_->module_cache.erase(hash_wasm_bytes(wasm.data));
}

void runtime::clear_cache() {
   if (!impl_) return;
   impl_->module_cache.clear();
   impl_->libraries.clear();
}

} // namespace psizam
