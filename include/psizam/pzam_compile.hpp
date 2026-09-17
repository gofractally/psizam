#pragma once

// High-level API for compiling .wasm to .pzam.
//
//   // Defaults: host arch, jit2 tier, no instrumentation
//   psizam::pzam_compile_file("module.wasm", "module.pzam");
//
//   // Cross-compile with LLVM softfloat (requires psizam-llvm):
//   using enum psizam::pzam_arch_flags;
//   using enum psizam::pzam_tier_flags;
//   using enum psizam::pzam_compile_flags;
//   psizam::pzam_compile_file("module.wasm", "module.pzam",
//                             {.arch = x86_64 | aarch64, .tier = llvm_O3,
//                              .flags = softfloat});

#include <psizam/detail/parser.hpp>
#include <psizam/detail/ir_writer.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/pzam_metadata.hpp>
#include <psizam/utils.hpp>

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
#include <psizam/detail/ir_writer_llvm_aot.hpp>
#endif

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace psizam {

   // ---- Compile option bitmaps ----

   enum class pzam_arch_flags : uint32_t {
      none    = 0,
      x86_64  = 1 << 0,
      aarch64 = 1 << 1,
      all     = x86_64 | aarch64,
#if defined(__x86_64__)
      host    = x86_64,
#elif defined(__aarch64__)
      host    = aarch64,
#else
      host    = none,
#endif
   };

   enum class pzam_tier_flags : uint32_t {
      none    = 0,
      jit1    = 1 << 0,
      jit2    = 1 << 1,
      llvm_O1 = 1 << 2,
      llvm_O2 = 1 << 3,
      llvm_O3 = 1 << 4,
      all_jit  = jit1 | jit2,
      all_llvm = llvm_O1 | llvm_O2 | llvm_O3,
      all      = all_jit | all_llvm,
   };

   enum class pzam_compile_flags : uint32_t {
      none       = 0,
      softfloat  = 1 << 0,
      backtrace  = 1 << 1,
      gas_meter  = 1 << 2,
      debug_info = 1 << 3,
   };

   // Bitwise operators for flag enums
   #define PSIZAM_FLAG_OPS(E) \
      constexpr E operator|(E a, E b) { return E(uint32_t(a) | uint32_t(b)); } \
      constexpr E operator&(E a, E b) { return E(uint32_t(a) & uint32_t(b)); } \
      constexpr E operator~(E a)      { return E(~uint32_t(a)); } \
      constexpr bool operator!(E a)   { return uint32_t(a) == 0; }
   PSIZAM_FLAG_OPS(pzam_arch_flags)
   PSIZAM_FLAG_OPS(pzam_tier_flags)
   PSIZAM_FLAG_OPS(pzam_compile_flags)
   #undef PSIZAM_FLAG_OPS

   /// Options for pzam_compile.  All fields are bitmaps — compile for
   /// multiple architectures and optimization tiers in a single call,
   /// producing one code section per arch×tier combination.
   struct pzam_compile_options {
      pzam_arch_flags    arch  = pzam_arch_flags::host;
      pzam_tier_flags    tier  = pzam_tier_flags::jit2;
      pzam_compile_flags flags = pzam_compile_flags::none;
   };

   // Map flag bits to format enum values
   namespace detail {
      inline constexpr std::pair<pzam_arch_flags, pzam_arch> arch_map[] = {
         {pzam_arch_flags::x86_64,  pzam_arch::x86_64},
         {pzam_arch_flags::aarch64, pzam_arch::aarch64},
      };
      inline constexpr std::pair<pzam_tier_flags, pzam_opt_tier> tier_map[] = {
         {pzam_tier_flags::jit1,    pzam_opt_tier::jit1},
         {pzam_tier_flags::jit2,    pzam_opt_tier::jit2},
         {pzam_tier_flags::llvm_O1, pzam_opt_tier::llvm_O1},
         {pzam_tier_flags::llvm_O2, pzam_opt_tier::llvm_O2},
         {pzam_tier_flags::llvm_O3, pzam_opt_tier::llvm_O3},
      };
   }

   namespace detail {
      inline pzam_opt_tier llvm_pzam_tier(int level) {
         switch (level) {
            case 0: return pzam_opt_tier::llvm_O0;
            case 1: return pzam_opt_tier::llvm_O1;
            case 2: return pzam_opt_tier::llvm_O2;
            case 3: return pzam_opt_tier::llvm_O3;
            default: throw std::invalid_argument("pzam_compile: LLVM optimization level must be 0..3");
         }
      }

      // Backend identity is explicit: even an empty LLVM module is LLVM output.
      inline pzam_code_section make_pzam_code_section(
            const module& mod, const pzam_compile_result& result,
            pzam_arch arch, pzam_opt_tier tier, bool softfloat, bool backtrace) {
         const bool llvm = tier != pzam_opt_tier::jit2;
         if (llvm && result.function_offsets.size() != mod.code.size())
            throw std::runtime_error("pzam_compile: incomplete LLVM function table");
         pzam_code_section cs;
         cs.arch = static_cast<uint8_t>(arch);
         cs.opt_tier = static_cast<uint8_t>(tier);
         cs.instrumentation.softfloat = softfloat;
         cs.instrumentation.gas_metering = mod.gas_metering;
         cs.instrumentation.async_backtrace = backtrace;
         cs.stack_limit_mode = mod.stack_limit_is_bytes;
         cs.page_size = static_cast<uint32_t>(wasm_allocator::table_size());
         cs.max_stack = static_cast<uint32_t>(mod.maximum_stack);
         cs.compiler.compiler_name = llvm ? "psizam-llvm" : "psizam-jit2";
         cs.compiler.compiler_version = "0.1.0";
         cs.compiler.compiler_hash = pzam_cache::compiler_identity(arch);
         cs.functions.resize(mod.code.size());
         for (size_t i = 0; i < mod.code.size(); ++i) {
            cs.functions[i].code_offset = llvm ? result.function_offsets[i].first
               : static_cast<uint32_t>(mod.code[i].jit_code_offset);
            cs.functions[i].code_size = llvm ? result.function_offsets[i].second
               : mod.code[i].jit_code_size;
            cs.functions[i].stack_size = mod.code[i].stack_size;
         }
         for (const auto& r : result.relocs)
            cs.relocations.push_back({r.code_offset, static_cast<uint16_t>(r.symbol),
                                      static_cast<uint8_t>(r.type), r.addend});
         if (llvm) {
            cs.code_blob = result.code_blob;
         } else if (auto size = mod.allocator.get_actual_code_size()) {
            auto start = reinterpret_cast<const uint8_t*>(mod.allocator.get_code_start());
            cs.code_blob.assign(start, start + size);
         }
         if (!cs.code_blob.empty() && !result.relocs.empty())
            canonicalize_reloc_sites(reinterpret_cast<char*>(cs.code_blob.data()),
                                    result.relocs.data(), result.relocs.size());
         return cs;
      }

      template<class Writer>
      inline void compile_pzam_section(std::vector<uint8_t>& wasm, pzam_file& file,
            pzam_arch arch, pzam_opt_tier tier, bool softfloat, bool backtrace,
            int opt_level = 2, bool gas_metering = false) {
         module mod;
         mod.allocator.use_default_memory();
         pzam_compile_result result;
         result.target_triple = arch == pzam_arch::x86_64
            ? "x86_64-unknown-linux-gnu" : "aarch64-unknown-linux-gnu";
         result.softfloat = softfloat;
         result.backtrace = backtrace;
         result.opt_level = opt_level;
         null_debug_info debug;
         default_options parser_options;
         parser_options.gas_metering = gas_metering;
         binary_parser<Writer, default_options, null_debug_info> parser(
            mod.allocator, parser_options, backtrace, false);
         parser.set_compile_result(&result);
         parser.parse_module(wasm, mod, debug);
         mod.finalize();
         if (!result.error.empty()) throw std::runtime_error("pzam_compile: " + result.error);
         if (file.code_sections.empty()) file.metadata = extract_metadata(mod);
         file.code_sections.push_back(make_pzam_code_section(mod, result, arch, tier,
                                                             softfloat, backtrace));
      }
   }

   /// Compile each requested architecture/tier independently. JIT1 PZAM output
   /// and debug-info emission are not implemented and are rejected.
   /// Link psizam-llvm for LLVM tiers; both JIT2 targets are always available.
   inline std::vector<char> pzam_compile(std::span<const uint8_t> wasm_bytes,
                                         const pzam_compile_options& opts = {}) {
      using namespace detail;
      if (!opts.arch || !!(opts.arch & ~pzam_arch_flags::all))
         throw std::invalid_argument("pzam_compile: invalid or empty architecture flags");
      if (!opts.tier || !!(opts.tier & ~pzam_tier_flags::all))
         throw std::invalid_argument("pzam_compile: invalid or empty tier flags");
      if (!!(opts.tier & pzam_tier_flags::jit1))
         throw std::invalid_argument("pzam_compile: JIT1 PZAM output is not supported");
      constexpr auto supported = pzam_compile_flags::softfloat | pzam_compile_flags::backtrace | pzam_compile_flags::gas_meter;
      if (!!(opts.flags & ~supported))
         throw std::invalid_argument("pzam_compile: unsupported instrumentation flags");
      const bool softfloat = !!(opts.flags & pzam_compile_flags::softfloat);
      const bool gas_metering = !!(opts.flags & pzam_compile_flags::gas_meter);
      const bool backtrace = !!(opts.flags & pzam_compile_flags::backtrace);
      if (softfloat && !!(opts.tier & pzam_tier_flags::jit2))
         throw std::invalid_argument("pzam_compile: softfloat requires LLVM for every requested tier");
#ifndef PSIZAM_ENABLE_LLVM_BACKEND
      if (!!(opts.tier & pzam_tier_flags::all_llvm))
         throw std::invalid_argument("pzam_compile: LLVM backend is not enabled; link psizam-llvm");
#endif
      pzam_file file;
      file.input_hash = pzam_cache::hash_wasm(wasm_bytes);
      std::vector<uint8_t> wasm(wasm_bytes.begin(), wasm_bytes.end());
      for (auto [arch_flag, arch] : arch_map) {
         if (!(opts.arch & arch_flag)) continue;
         for (auto [tier_flag, tier] : tier_map) {
            if (!(opts.tier & tier_flag)) continue;
            if (tier == pzam_opt_tier::jit2) {
               if (arch == pzam_arch::x86_64)
                  compile_pzam_section<ir_writer_x64>(wasm, file, arch, tier, false, backtrace, 2, gas_metering);
               else
                  compile_pzam_section<ir_writer_a64>(wasm, file, arch, tier, false, backtrace, 2, gas_metering);
            } else {
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
               const int level = tier == pzam_opt_tier::llvm_O1 ? 1
                               : tier == pzam_opt_tier::llvm_O2 ? 2 : 3;
               compile_pzam_section<ir_writer_llvm_aot>(wasm, file, arch, tier,
                                                       softfloat, backtrace, level, gas_metering);
#endif
            }
         }
      }
      return pzam_save(file);
   }

   /// Compile a .wasm file and write the result to a .pzam file.
   inline void pzam_compile_file(const std::string& wasm_path, const std::string& pzam_path,
                                 const pzam_compile_options& opts = {}) {
      auto wasm = read_wasm(wasm_path);
      auto pzam = pzam_compile(wasm, opts);

      std::ofstream out(pzam_path, std::ios::binary);
      if (!out.is_open())
         throw std::runtime_error("pzam_compile_file: cannot open " + pzam_path);
      out.write(pzam.data(), static_cast<std::streamsize>(pzam.size()));
      out.close();
      if (!out) throw std::runtime_error("pzam_compile_file: write failed: " + pzam_path);
   }

} // namespace psizam
