// LLVM AOT Compiler implementation
//
// Compiles LLVM IR to a relocatable object file using LLVM's TargetMachine,
// then parses the object to extract the .text section and relocations.
// The result is packaged for .pzam serialization.

#include <psizam/detail/llvm_aot_compiler.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/DataExtractor.h>
#include <llvm/Support/Error.h>

// Initialize only the architectures linked into the shared AOT library.
extern "C" {
   void LLVMInitializeX86TargetInfo();
   void LLVMInitializeX86Target();
   void LLVMInitializeX86TargetMC();
   void LLVMInitializeX86AsmPrinter();
   void LLVMInitializeAArch64TargetInfo();
   void LLVMInitializeAArch64Target();
   void LLVMInitializeAArch64TargetMC();
   void LLVMInitializeAArch64AsmPrinter();
}
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace psizam::detail {

   enum class wasm_native_kind { other, body, entry, import_thunk };
   struct wasm_native_ref {
      wasm_native_kind kind = wasm_native_kind::other;
      uint32_t index = 0; // global function index; imports occupy the initial prefix
   };

   static llvm::Expected<wasm_native_ref>
   parse_wasm_native_ref(llvm::StringRef name, const module& mod) {
      wasm_native_ref result;
      llvm::StringRef suffix;
      if (name.consume_front("wasm_func_")) {
         result.kind = wasm_native_kind::body;
         suffix = name;
      } else if (name.consume_front("wasm_entry_")) {
         result.kind = wasm_native_kind::entry;
         suffix = name;
      } else if (name.consume_front("wasm_import_")) {
         result.kind = wasm_native_kind::import_thunk;
         suffix = name;
      } else {
         return result;
      }
      if (suffix.getAsInteger(10, result.index) ||
          suffix != std::to_string(result.index))
         return llvm::createStringError("invalid AOT function index");
      const uint32_t imports = mod.get_imported_functions_size();
      if (result.kind == wasm_native_kind::import_thunk) {
         if (result.index >= imports)
            return llvm::createStringError("invalid AOT import index");
      } else if (result.index < imports || uint64_t(result.index) - imports >= mod.code.size()) {
         return llvm::createStringError("invalid AOT function index");
      }
      return result;
   }

   // ELF stack-size records use relocations to identify their function. A
   // section symbol plus addend is common, so names alone are insufficient.
   using native_function_key = std::pair<uint64_t, uint64_t>; // section, value
   static llvm::Expected<std::map<native_function_key, uint64_t>>
   read_native_stack_sizes(const llvm::object::ObjectFile& obj) {
      std::map<native_function_key, uint64_t> frames;
      if (!llvm::isa<llvm::object::ELFObjectFileBase>(obj) ||
          obj.getBytesInAddress() != 8 || !obj.isLittleEndian())
         return llvm::createStringError("native frame metadata requires little-endian ELF64");
      for (const auto& section : obj.sections()) {
         auto name = section.getName();
         if (!name) return name.takeError();
         if (*name != ".stack_sizes") continue;
         auto contents = section.getContents();
         if (!contents) return contents.takeError();
         std::map<uint64_t, native_function_key> references;
         for (const auto& relocation_section : obj.sections()) {
            auto target = relocation_section.getRelocatedSection();
            if (!target) return target.takeError();
            if (*target == obj.section_end() || **target != section) continue;
            for (const auto& relocation : relocation_section.relocations()) {
               auto symbol = relocation.getSymbol();
               if (symbol == obj.symbol_end())
                  return llvm::createStringError("stack-size relocation has no symbol");
               auto function_section = symbol->getSection();
               if (!function_section) return function_section.takeError();
               auto value = symbol->getValue();
               if (!value) return value.takeError();
               auto addend = llvm::object::ELFRelocationRef(relocation).getAddend();
               if (!addend) return addend.takeError();
               if (*function_section == obj.section_end() || *addend < 0 ||
                   uint64_t(*addend) > UINT64_MAX - *value ||
                   (relocation.getType() != 1 && relocation.getType() != 257))
                  return llvm::createStringError("unsupported stack-size relocation");
               if (!references.emplace(relocation.getOffset(), native_function_key{
                       (*function_section)->getIndex(), *value + uint64_t(*addend)}).second)
                  return llvm::createStringError("duplicate stack-size relocation");
            }
         }
         llvm::DataExtractor data(*contents, true, 8);
         uint64_t offset = 0;
         size_t count = 0;
         while (offset < contents->size()) {
            auto reference = references.find(offset);
            if (reference == references.end() || contents->size() - offset < 8)
               return llvm::createStringError("missing stack-size function reference");
            offset += 8;
            llvm::Error error = llvm::Error::success();
            const auto size = data.getULEB128(&offset, &error);
            if (error) return std::move(error);
            auto [it, inserted] = frames.emplace(reference->second, size);
            if (!inserted && it->second != size)
               return llvm::createStringError("conflicting native frame sizes");
            ++count;
         }
         if (count != references.size())
            return llvm::createStringError("unconsumed stack-size relocation");
      }
      return frames;
   }

   static bool ensure_llvm_targets() {
      static bool done = [] {
#ifdef PSIZAM_LLVM_TARGET_X86
         LLVMInitializeX86TargetInfo();
         LLVMInitializeX86Target();
         LLVMInitializeX86TargetMC();
         LLVMInitializeX86AsmPrinter();
#endif
#ifdef PSIZAM_LLVM_TARGET_AARCH64
         LLVMInitializeAArch64TargetInfo();
         LLVMInitializeAArch64Target();
         LLVMInitializeAArch64TargetMC();
         LLVMInitializeAArch64AsmPrinter();
#endif
         return true;
      }();
      return done;
   }

   // Map LLVM symbol name to reloc_symbol
   static reloc_symbol map_llvm_symbol(llvm::StringRef name) {
      static const std::unordered_map<std::string, reloc_symbol> sym_map = {
         {"__psizam_global_get",       reloc_symbol::llvm_global_get},
         {"__psizam_global_set",       reloc_symbol::llvm_global_set},
         {"__psizam_global_get_v128",  reloc_symbol::llvm_global_get_v128},
         {"__psizam_global_set_v128",  reloc_symbol::llvm_global_set_v128},
         {"__psizam_memory_size",      reloc_symbol::llvm_memory_size},
         {"__psizam_memory_grow",      reloc_symbol::llvm_memory_grow},
         {"__psizam_call_host",        reloc_symbol::llvm_call_host},
         {"__psizam_call_host_full",   reloc_symbol::llvm_call_host_full},
         {"__psizam_memory_init",      reloc_symbol::llvm_memory_init},
         {"__psizam_data_drop",        reloc_symbol::llvm_data_drop},
         {"__psizam_memory_copy",      reloc_symbol::llvm_memory_copy},
         {"__psizam_memory_fill",      reloc_symbol::llvm_memory_fill},
         {"__psizam_table_init",       reloc_symbol::llvm_table_init},
         {"__psizam_elem_drop",        reloc_symbol::llvm_elem_drop},
         {"__psizam_table_copy",       reloc_symbol::llvm_table_copy},
         {"__psizam_call_indirect",    reloc_symbol::llvm_call_indirect},
         {"__psizam_table_get",        reloc_symbol::llvm_table_get},
         {"__psizam_table_set",        reloc_symbol::llvm_table_set},
         {"__psizam_table_grow",       reloc_symbol::llvm_table_grow},
         {"__psizam_table_size",       reloc_symbol::llvm_table_size},
         {"__psizam_table_fill",       reloc_symbol::llvm_table_fill},
         {"__psizam_resolve_indirect", reloc_symbol::llvm_resolve_indirect},
         {"__psizam_resolve_indirect_entry", reloc_symbol::llvm_resolve_indirect_entry},
         {"__psizam_atomic_rmw",       reloc_symbol::llvm_atomic_rmw},
         {"__psizam_call_depth_dec",   reloc_symbol::llvm_call_depth_dec},
         {"__psizam_call_depth_inc",   reloc_symbol::llvm_call_depth_inc},
         {"__psizam_gas_charge",       reloc_symbol::llvm_gas_charge},
         {"__psizam_gas_exhausted_check", reloc_symbol::llvm_gas_exhausted_check},
         {"__psizam_trap",             reloc_symbol::llvm_trap},
         {"__psizam_get_memory",       reloc_symbol::llvm_get_memory},
         {"fmaf",                     reloc_symbol::libm_f32_fma},
         {"fma",                      reloc_symbol::libm_f64_fma},
         // Softfloat function symbols
         {"__psizam_sf_f32_add",       reloc_symbol::sf_f32_add},
         {"__psizam_sf_f32_sub",       reloc_symbol::sf_f32_sub},
         {"__psizam_sf_f32_mul",       reloc_symbol::sf_f32_mul},
         {"__psizam_sf_f32_div",       reloc_symbol::sf_f32_div},
         {"__psizam_sf_f32_min",       reloc_symbol::sf_f32_min},
         {"__psizam_sf_f32_max",       reloc_symbol::sf_f32_max},
         {"__psizam_sf_f32_copysign",  reloc_symbol::sf_f32_copysign},
         {"__psizam_sf_f32_abs",       reloc_symbol::sf_f32_abs},
         {"__psizam_sf_f32_neg",       reloc_symbol::sf_f32_neg},
         {"__psizam_sf_f32_sqrt",      reloc_symbol::sf_f32_sqrt},
         {"__psizam_sf_f32_ceil",      reloc_symbol::sf_f32_ceil},
         {"__psizam_sf_f32_floor",     reloc_symbol::sf_f32_floor},
         {"__psizam_sf_f32_trunc",     reloc_symbol::sf_f32_trunc},
         {"__psizam_sf_f32_nearest",   reloc_symbol::sf_f32_nearest},
         {"__psizam_sf_f64_add",       reloc_symbol::sf_f64_add},
         {"__psizam_sf_f64_sub",       reloc_symbol::sf_f64_sub},
         {"__psizam_sf_f64_mul",       reloc_symbol::sf_f64_mul},
         {"__psizam_sf_f64_div",       reloc_symbol::sf_f64_div},
         {"__psizam_sf_f64_min",       reloc_symbol::sf_f64_min},
         {"__psizam_sf_f64_max",       reloc_symbol::sf_f64_max},
         {"__psizam_sf_f64_copysign",  reloc_symbol::sf_f64_copysign},
         {"__psizam_sf_f64_abs",       reloc_symbol::sf_f64_abs},
         {"__psizam_sf_f64_neg",       reloc_symbol::sf_f64_neg},
         {"__psizam_sf_f64_sqrt",      reloc_symbol::sf_f64_sqrt},
         {"__psizam_sf_f64_ceil",      reloc_symbol::sf_f64_ceil},
         {"__psizam_sf_f64_floor",     reloc_symbol::sf_f64_floor},
         {"__psizam_sf_f64_trunc",     reloc_symbol::sf_f64_trunc},
         {"__psizam_sf_f64_nearest",   reloc_symbol::sf_f64_nearest},
         {"__psizam_sf_f32_convert_i32s", reloc_symbol::sf_f32_convert_i32s},
         {"__psizam_sf_f32_convert_i32u", reloc_symbol::sf_f32_convert_i32u},
         {"__psizam_sf_f32_convert_i64s", reloc_symbol::sf_f32_convert_i64s},
         {"__psizam_sf_f32_convert_i64u", reloc_symbol::sf_f32_convert_i64u},
         {"__psizam_sf_f64_convert_i32s", reloc_symbol::sf_f64_convert_i32s},
         {"__psizam_sf_f64_convert_i32u", reloc_symbol::sf_f64_convert_i32u},
         {"__psizam_sf_f64_convert_i64s", reloc_symbol::sf_f64_convert_i64s},
         {"__psizam_sf_f64_convert_i64u", reloc_symbol::sf_f64_convert_i64u},
         {"__psizam_sf_f32_demote_f64",  reloc_symbol::sf_f32_demote_f64},
         {"__psizam_sf_f64_promote_f32", reloc_symbol::sf_f64_promote_f32},
      };
      auto it = sym_map.find(name.str());
      if (it != sym_map.end()) return it->second;
      return reloc_symbol::unknown;
   }

   // Map ELF relocation type to pzam reloc_type. Which relocations LLVM picks
   // depends on the module being compiled, so an unlisted one is a failed
   // compilation of that module, never a reason to end the process.
   static std::optional<reloc_type> map_elf_reloc_type(uint64_t elf_type, bool is_x86_64) {
      if (is_x86_64) {
         // ELF x86_64 relocation types (from elf.h)
         switch (elf_type) {
            case 1:  return reloc_type::abs64;           // R_X86_64_64
            case 2:  return reloc_type::x86_64_pc32;     // R_X86_64_PC32
            case 4:  return reloc_type::x86_64_pc32;     // R_X86_64_PLT32 (same encoding)
            default: return std::nullopt;
         }
      } else {
         // ELF aarch64 relocation types (from AArch64.def)
         switch (elf_type) {
            case 257: return reloc_type::abs64;                     // R_AARCH64_ABS64
            case 282: return reloc_type::aarch64_call26;            // R_AARCH64_JUMP26
            case 283: return reloc_type::aarch64_call26;            // R_AARCH64_CALL26
            case 263:                                                 // R_AARCH64_MOVW_UABS_G0     (0x107)
            case 264: return reloc_type::aarch64_movw_uabs_g0_nc;  // R_AARCH64_MOVW_UABS_G0_NC  (0x108)
            case 265:                                                 // R_AARCH64_MOVW_UABS_G1     (0x109)
            case 266: return reloc_type::aarch64_movw_uabs_g1_nc;  // R_AARCH64_MOVW_UABS_G1_NC  (0x10a)
            case 267:                                                 // R_AARCH64_MOVW_UABS_G2     (0x10b)
            case 268: return reloc_type::aarch64_movw_uabs_g2_nc;  // R_AARCH64_MOVW_UABS_G2_NC  (0x10c)
            case 269: return reloc_type::aarch64_movw_uabs_g3;     // R_AARCH64_MOVW_UABS_G3     (0x10d)
            case 275:                                                 // R_AARCH64_ADR_PREL_PG_HI21 (0x113)
            case 276: return reloc_type::aarch64_adr_prel_pg_hi21; // R_AARCH64_ADR_PREL_PG_HI21_NC (0x114)
            case 277: return reloc_type::aarch64_add_abs_lo12_nc;  // R_AARCH64_ADD_ABS_LO12_NC  (0x115)
            case 278: return reloc_type::aarch64_ldst8_abs_lo12_nc;   // R_AARCH64_LDST8_ABS_LO12_NC  (0x116)
            case 284: return reloc_type::aarch64_ldst16_abs_lo12_nc;  // R_AARCH64_LDST16_ABS_LO12_NC (0x11c)
            case 285: return reloc_type::aarch64_ldst32_abs_lo12_nc;  // R_AARCH64_LDST32_ABS_LO12_NC (0x11d)
            case 286: return reloc_type::aarch64_ldst64_abs_lo12_nc;  // R_AARCH64_LDST64_ABS_LO12_NC (0x11e)
            case 299: return reloc_type::aarch64_ldst128_abs_lo12_nc; // R_AARCH64_LDST128_ABS_LO12_NC (0x12b)
            default: return std::nullopt;
         }
      }
   }

   static llvm_aot_result unsupported_relocation(uint64_t elf_type, bool is_x86_64) {
      return llvm_aot_result{.error = std::string("Unsupported ") + (is_x86_64 ? "x86_64" : "aarch64") +
                                      " ELF relocation type " + std::to_string(elf_type)};
   }

   llvm_aot_result llvm_aot_compile(
         std::unique_ptr<llvm::LLVMContext> llvm_ctx,
         std::unique_ptr<llvm::Module> llvm_mod,
         const module& mod,
         const std::string& target_triple,
         unsigned opt_level) {

      ensure_llvm_targets();

      llvm::Triple triple(target_triple);
      bool is_x86_64 = triple.getArch() == llvm::Triple::x86_64;
      if (!is_x86_64 && triple.getArch() != llvm::Triple::aarch64)
         return llvm_aot_result{.error = "native frame metadata requires X86-64 or AArch64"};
      const uint32_t num_imports = mod.get_imported_functions_size();
      // Pending code_blob_self references use strictly negative int32 addends.
      // Keep INT32_MIN out of the encoding so resolving them can negate safely.
      if (mod.code.size() > uint64_t(INT32_MAX) / 2 ||
          uint64_t(mod.code.size()) * 2 + num_imports > uint64_t(INT32_MAX))
         return llvm_aot_result{.error = "AOT function reference index exceeds relocation range"};
      std::set<std::string> expected_functions;
      std::vector<uint8_t> expected_presence(mod.code.size());
      bool guest_c_abi = false;
      bool guest_tail_abi = false;
      for (auto& function : *llvm_mod) {
         auto reference = parse_wasm_native_ref(function.getName(), mod);
         if (!reference)
            return llvm_aot_result{.error = llvm::toString(reference.takeError()) + ": " + function.getName().str()};
         if (reference->kind == wasm_native_kind::body || reference->kind == wasm_native_kind::import_thunk) {
            if (function.getCallingConv() == llvm::CallingConv::Tail)
               guest_tail_abi = true;
            else if (function.getCallingConv() == llvm::CallingConv::C)
               guest_c_abi = true;
            else
               return llvm_aot_result{.error = "unsupported AOT guest calling convention: " + function.getName().str()};
            if (reference->kind == wasm_native_kind::import_thunk &&
                function.getCallingConv() != llvm::CallingConv::Tail)
               return llvm_aot_result{.error = "AOT typed import requires tailcc: " + function.getName().str()};
         } else if (reference->kind == wasm_native_kind::entry &&
                    function.getCallingConv() != llvm::CallingConv::C) {
            return llvm_aot_result{.error = "AOT entry wrapper requires C calling convention: " + function.getName().str()};
         }
         if (function.isDeclaration()) continue;
         if (reference->kind == wasm_native_kind::other)
            return llvm_aot_result{.error = "unsupported AOT helper definition: " + function.getName().str()};
         if (reference->kind != wasm_native_kind::import_thunk)
            expected_presence[reference->index - num_imports] |=
               reference->kind == wasm_native_kind::entry ? 1 : 2;
         if (function.hasFnAttribute("no-stack-arg-probe"))
            return llvm_aot_result{.error = "conflicting native stack probe attribute"};
         if (function.hasFnAttribute(llvm::Attribute::Naked) ||
             (function.getFnStackAlign() && function.getFnStackAlign()->value() > 16))
            return llvm_aot_result{.error = "unsupported native stack alignment or naked function"};
         expected_functions.insert(function.getName().str());
         function.addFnAttr("probe-stack", "inline-asm");
         function.addFnAttr("stack-probe-size", "4096");
         function.addFnAttr(llvm::Attribute::NoRedZone);
         for (const auto& block : function)
            for (const auto& instruction : block) {
               if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                  if (const auto* callee = call->getCalledFunction()) {
                     auto target_ref = parse_wasm_native_ref(callee->getName(), mod);
                     if (!target_ref)
                        return llvm_aot_result{.error = llvm::toString(target_ref.takeError())};
                     if ((target_ref->kind == wasm_native_kind::body ||
                          target_ref->kind == wasm_native_kind::import_thunk) &&
                         call->getCallingConv() != callee->getCallingConv())
                        return llvm_aot_result{.error = "mismatched AOT guest call convention: " + callee->getName().str()};
                  }
               }
               if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
                  if (!alloca->isStaticAlloca())
                     return llvm_aot_result{.error = "unbounded native stack allocation in " + function.getName().str()};
                  if (alloca->getAlign().value() > 16)
                     return llvm_aot_result{.error = "unsupported native stack alignment in " + function.getName().str()};
               }
            }
      }
      for (auto presence : expected_presence)
         if (presence && presence != 3)
            return llvm_aot_result{.error = "missing AOT body or entry wrapper"};
      if (guest_c_abi && guest_tail_abi)
         return llvm_aot_result{.error = "mixed AOT guest calling conventions"};

      // Look up the target
      std::string error;
#if LLVM_VERSION_MAJOR >= 22
      auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
#else
      auto* target = llvm::TargetRegistry::lookupTarget(triple.str(), error);
#endif
      if (!target) {
         return llvm_aot_result{.error = "LLVM target lookup failed: " + error};
      }

      if (opt_level > 3)
         return llvm_aot_result{.error = "LLVM optimization level must be 0 through 3"};
      const auto codegen_level = static_cast<llvm::CodeGenOptLevel>(opt_level);
      llvm::TargetOptions opts;
      opts.EmitStackSizeSection = true;
      opts.EnableMachineOutliner = false;
      auto rm = llvm::Reloc::Static;
      // aarch64: Small code model uses ADRP+ADD (8 bytes per reference, ±4GB
      // range) — our relocation system already handles adr_prel_pg_hi21 and
      // add_abs_lo12_nc.  Large uses MOVZ+MOVK×3 (16 bytes, any address) but
      // doubles code size for reference-heavy modules like clang.wasm.
      // x86_64: Large uses movabs (10 bytes) for abs64 relocations.  Small
      // would use RIP-relative call/lea (5-7 bytes, ±2GB) which is fine for
      // in-process execution but requires the code blob and runtime helpers
      // to be allocated within 2GB — safe in practice but keep Large for now.
      auto cm = is_x86_64 ? llvm::CodeModel::Large : llvm::CodeModel::Small;
      // x86-64-v2 includes SSE4.1/4.2 — avoids libc calls for ceil/floor/etc.
      std::string cpu = is_x86_64 ? "x86-64-v2" : "";
      // On aarch64, reserve x18 — macOS uses it as a platform register.
      // We keep the linux-gnu triple for ELF output but must not clobber x18.
      std::string features = is_x86_64 ? "" : "+reserve-x18";
      auto tm = std::unique_ptr<llvm::TargetMachine>(
#if LLVM_VERSION_MAJOR >= 22
         target->createTargetMachine(triple, cpu, features, opts, rm, cm,
                                     codegen_level));
#else
         target->createTargetMachine(triple.str(), cpu, features, opts, rm, cm,
                                     codegen_level));
#endif
      if (!tm) {
         return llvm_aot_result{.error = "Failed to create LLVM TargetMachine"};
      }

      // AArch64 enables GlobalISel implicitly at O0. Its local CSE dominance
      // scans become quadratic on the large blocks produced by translated
      // compiler workloads. Use SelectionDAG for this diagnostic level too.
      if (!is_x86_64 && opt_level == 0)
         tm->setGlobalISel(false);

      llvm_mod->setDataLayout(tm->createDataLayout());
#if LLVM_VERSION_MAJOR >= 22
      llvm_mod->setTargetTriple(triple);
#else
      llvm_mod->setTargetTriple(target_triple);
#endif

      // Emit object code to memory buffer
      llvm::SmallVector<char, 0> obj_buf;
      llvm::raw_svector_ostream obj_stream(obj_buf);
      llvm::legacy::PassManager pm;

      if (tm->addPassesToEmitFile(pm, obj_stream, nullptr,
                                   llvm::CodeGenFileType::ObjectFile)) {
         return llvm_aot_result{.error = "TargetMachine cannot emit object file"};
      }
      pm.run(*llvm_mod);

      // Parse the object file
      auto buf = std::make_unique<llvm::SmallVectorMemoryBuffer>(std::move(obj_buf), false);
      auto obj_or_err = llvm::object::ObjectFile::createObjectFile(*buf);
      if (!obj_or_err) {
         std::string msg;
         llvm::raw_string_ostream os(msg);
         os << obj_or_err.takeError();
         return llvm_aot_result{.error = "Failed to parse emitted object: " + msg};
      }
      auto& obj = **obj_or_err;
      auto native_frames = read_native_stack_sizes(obj);
      if (!native_frames)
         return llvm_aot_result{.error = "native frame metadata: " + llvm::toString(native_frames.takeError())};

      // Find the .text section and merge any .rodata sections into the code blob.
      // LLVM may place jump tables and constant pools in .rodata, which must be
      // accessible from the code at runtime. We append .rodata after .text with
      // alignment, and emit code_blob_self relocations for cross-section references.
      llvm_aot_result result;
      if (guest_tail_abi)
         result.native_guest_abi = psizam::native_guest_abi::llvm_tailcc;
      std::optional<llvm::object::SectionRef> text_section;

      // Track section index → blob offset for resolving internal relocations
      std::unordered_map<uint64_t, uint64_t> section_blob_offsets;

      for (const auto& section : obj.sections()) {
         auto name_or_err = section.getName();
         if (!name_or_err) continue;
         // Large code model on x86_64 puts code in .ltext instead of .text
         if (*name_or_err == ".text" || *name_or_err == ".ltext") {
            auto contents_or_err = section.getContents();
            if (!contents_or_err) {
               return llvm_aot_result{.error = "Failed to read code section"};
            }
            if (!contents_or_err->empty()) {
               result.code.assign(contents_or_err->begin(), contents_or_err->end());
               text_section = section;
               section_blob_offsets[section.getIndex()] = 0;
               break;
            }
         }
      }

      // Append .rodata sections to the code blob
      for (const auto& section : obj.sections()) {
         auto name_or_err = section.getName();
         if (!name_or_err) continue;
         auto name = *name_or_err;
         if (name == ".rodata" || name.starts_with(".rodata.") ||
             name == ".lrodata" || name.starts_with(".lrodata.")) {
            auto contents_or_err = section.getContents();
            if (!contents_or_err || contents_or_err->empty()) continue;

            const uint64_t alignment = std::max<uint64_t>(16, section.getAlignment().value());
            size_t align_pad = (alignment - (result.code.size() % alignment)) % alignment;
            result.code.resize(result.code.size() + align_pad, 0);
            uint64_t offset = result.code.size();
            result.code.insert(result.code.end(), contents_or_err->begin(), contents_or_err->end());
            section_blob_offsets[section.getIndex()] = offset;
         }
      }

      if (!text_section || result.code.empty()) {
         return llvm_aot_result{.error = "No .text section found in emitted object"};
      }

      // Build symbol name → index map from the object's symbol table
      std::unordered_map<uint64_t, std::string> sym_index_to_name;
      for (const auto& sym : obj.symbols()) {
         auto name_or_err = sym.getName();
         if (!name_or_err) continue;
         // Use the symbol's raw index
         sym_index_to_name[sym.getRawDataRefImpl().p] = name_or_err->str();
      }

      // Jump tables can contain absolute code pointers. Relocate every section
      // merged into the blob, including the pointers stored in read-only data.
      for (const auto& section : obj.sections()) {
         auto relocated_or_err = section.getRelocatedSection();
         if (!relocated_or_err) {
            llvm::consumeError(relocated_or_err.takeError());
            continue;
         }
         if (*relocated_or_err == obj.section_end()) continue;
         const auto source_section = section_blob_offsets.find((*relocated_or_err)->getIndex());
         if (source_section == section_blob_offsets.end()) continue;

         for (const auto& reloc : section.relocations()) {
            const auto reloc_offset = static_cast<uint32_t>(source_section->second + reloc.getOffset());
            auto sym_it = reloc.getSymbol();
            if (sym_it == obj.symbol_end()) continue;

            auto sym_name_or_err = sym_it->getName();
            if (!sym_name_or_err) continue;
            std::string sym_name = sym_name_or_err->str();

            auto psizam_sym = map_llvm_symbol(sym_name);

            // Handle unknown symbols — either internal references or data section refs
            if (psizam_sym == reloc_symbol::unknown) {
               // Check which section the symbol belongs to
               auto sec_or_err = sym_it->getSection();
               bool is_internal_code = false;
               bool is_data_section = false;
               uint64_t data_section_blob_offset = 0;

               if (sec_or_err && *sec_or_err != obj.section_end()) {
                  auto sn = (*sec_or_err)->getName();
                  if (sn) {
                     // Check if this symbol is in a data section we merged into the blob
                     auto blob_it = section_blob_offsets.find((*sec_or_err)->getIndex());
                     if (blob_it != section_blob_offsets.end()) {
                        auto text_name = text_section ? text_section->getName() : llvm::Expected<llvm::StringRef>(llvm::StringRef(""));
                        if (*sec_or_err == *text_section) {
                           is_internal_code = true; // reference within .text
                        } else {
                           is_data_section = true;
                           data_section_blob_offset = blob_it->second;
                        }
                     }
                  }
               }

               if (is_data_section) {
                  // Cross-section reference to merged data (e.g., .rodata jump tables).
                  // Emit a code_blob_self relocation so the loader resolves it to
                  // code_base + data_offset + symbol_value_within_section + addend.
                  auto val_or_err = llvm::object::ELFRelocationRef(reloc).getAddend();
                  int32_t elf_addend = val_or_err ? static_cast<int32_t>(*val_or_err) : 0;
                  auto sym_val_or_err = sym_it->getValue();
                  uint64_t sym_val = sym_val_or_err ? *sym_val_or_err : 0;

                  code_relocation cr;
                  cr.code_offset = reloc_offset;
                  cr.symbol = reloc_symbol::code_blob_self;
                  auto cr_type = map_elf_reloc_type(reloc.getType(), is_x86_64);
                  if (!cr_type) return unsupported_relocation(reloc.getType(), is_x86_64);
                  cr.type = *cr_type;
                  cr.addend = static_cast<int32_t>(data_section_blob_offset + sym_val) + elf_addend;
                  result.relocations.push_back(cr);
                  continue;
               }

               // WASM function references (per-function compilation).
               // Must check before is_internal_code: with ExternalLinkage, LLVM
               // emits relocations for calls between symbols in the same .text
               // section (e.g., wasm_entry_N → wasm_func_N within one compilation
               // unit). These are NOT pre-resolved by the assembler.
               //
               // Batch mode optimization: if the target symbol is DEFINED in
               // this .text (same batch) AND the relocation is PC-relative,
               // the final operand value is independent of where the batch
               // sits in the merged blob. Patch the code bytes directly and
               // skip emitting an output relocation.
               auto reference = parse_wasm_native_ref(sym_name, mod);
               if (!reference)
                  return llvm_aot_result{.error = llvm::toString(reference.takeError()) + ": " + sym_name};
               if (reference->kind != wasm_native_kind::other) {
                  auto known_type = map_elf_reloc_type(reloc.getType(), is_x86_64);
                  if (!known_type) return unsupported_relocation(reloc.getType(), is_x86_64);
                  const reloc_type mapped_type = *known_type;
                  uint32_t reloc_off = reloc_offset;

                  // Same-batch + PC-relative → resolve at compile time.
                  // `is_internal_code` means the symbol is defined in this
                  // object's .text section (batch-local).
                  bool pc_relative_intra_text =
                     is_internal_code &&
                     (mapped_type == reloc_type::aarch64_call26 ||
                      mapped_type == reloc_type::x86_64_pc32);

                  if (pc_relative_intra_text) {
                     auto sym_addr_or_err = sym_it->getAddress();
                     if (!sym_addr_or_err)
                        return llvm_aot_result{.error = llvm::toString(sym_addr_or_err.takeError())};
                     auto elf_addend_or_err = llvm::object::ELFRelocationRef(reloc).getAddend();
                     if (!elf_addend_or_err)
                        return llvm_aot_result{.error = llvm::toString(elf_addend_or_err.takeError())};
                     if (uint64_t(reloc_off) + 4 <= result.code.size()) {
                        int64_t target = static_cast<int64_t>(*sym_addr_or_err);
                        int64_t addend = *elf_addend_or_err;
                        int64_t pc = static_cast<int64_t>(reloc_off);

                        if (mapped_type == reloc_type::aarch64_call26) {
                           // BL/B imm26: (target - pc + addend) >> 2
                           const int64_t distance = target - pc + addend;
                           if ((distance & 3) || distance < -(1LL << 27) || distance >= (1LL << 27))
                              return llvm_aot_result{.error = "AArch64 intra-batch branch exceeds range"};
                           int64_t offset = distance >> 2;
                           uint32_t insn;
                           std::memcpy(&insn, result.code.data() + reloc_off, 4);
                           insn = (insn & 0xFC000000u) |
                                  (static_cast<uint32_t>(offset) & 0x03FFFFFFu);
                           std::memcpy(result.code.data() + reloc_off, &insn, 4);
                        } else {
                           // x86_64 CALL/JMP rel32: target - pc + addend
                           const int64_t distance = target - pc + addend;
                           if (distance < INT32_MIN || distance > INT32_MAX)
                              return llvm_aot_result{.error = "x86 intra-batch branch exceeds range"};
                           int32_t rel32 = static_cast<int32_t>(distance);
                           std::memcpy(result.code.data() + reloc_off, &rel32, 4);
                        }
                        continue; // no output reloc needed
                     }
                  }

                  code_relocation cr;
                  cr.code_offset = reloc_off;
                  cr.symbol = reloc_symbol::code_blob_self;
                  cr.type = mapped_type;
                  // Negative addend = pending function ref
                  // Body refs:  -(code_idx + 1)             [range -1..-N]
                  // Entry refs: -(code_idx + 1 + code_count) [range -(N+1)..-(2N)]
                  // Imports:    -(2 * code_count + import_idx + 1)
                  //              [range -(2N+1)..-(2N+I)]
                  uint64_t pending_index;
                  if (reference->kind == wasm_native_kind::import_thunk)
                     pending_index = uint64_t(mod.code.size()) * 2 + reference->index + 1;
                  else {
                     pending_index = uint64_t(reference->index) - num_imports + 1;
                     if (reference->kind == wasm_native_kind::entry)
                        pending_index += mod.code.size();
                  }
                  if (!pending_index || pending_index > uint64_t(INT32_MAX))
                     return llvm_aot_result{.error = "AOT function reference exceeds relocation range"};
                  cr.addend = -static_cast<int32_t>(pending_index);
                  result.relocations.push_back(cr);
                  continue;
               }

               // A local label still needs relocation when referenced through
               // an absolute pointer, including x86 large-model jump tables.
               if (is_internal_code) {
                  auto addend = llvm::object::ELFRelocationRef(reloc).getAddend();
                  auto value = sym_it->getValue();
                  if (!addend || !value) {
                     if (!addend) llvm::consumeError(addend.takeError());
                     if (!value) llvm::consumeError(value.takeError());
                     return llvm_aot_result{.error = "Cannot resolve local code relocation"};
                  }
                  code_relocation cr;
                  cr.code_offset = reloc_offset;
                  cr.symbol = reloc_symbol::code_blob_self;
                  auto cr_type = map_elf_reloc_type(reloc.getType(), is_x86_64);
                  if (!cr_type) return unsupported_relocation(reloc.getType(), is_x86_64);
                  cr.type = *cr_type;
                  cr.addend = static_cast<int32_t>(*value + *addend);
                  result.relocations.push_back(cr);
                  continue;
               }

               // Map libc/compiler-rt symbols to known reloc_symbols
               reloc_symbol mapped = reloc_symbol::unknown;
               if (sym_name == "memset")
                  mapped = reloc_symbol::libc_memset;
               else if (sym_name == "memmove" || sym_name == "memcpy")
                  mapped = reloc_symbol::libc_memmove;

               if (mapped != reloc_symbol::unknown) {
                  code_relocation cr;
                  cr.code_offset = reloc_offset;
                  cr.symbol = mapped;
                  auto cr_type = map_elf_reloc_type(reloc.getType(), is_x86_64);
                  if (!cr_type) return unsupported_relocation(reloc.getType(), is_x86_64);
                  cr.type = *cr_type;
                  auto val_or_err = llvm::object::ELFRelocationRef(reloc).getAddend();
                  cr.addend = val_or_err ? static_cast<int32_t>(*val_or_err) : 0;
                  result.relocations.push_back(cr);
                  continue;
               }

               return llvm_aot_result{.error = "Unknown external symbol in LLVM object: " + sym_name};
            }

            code_relocation cr;
            cr.code_offset = reloc_offset;
            cr.symbol = psizam_sym;
            auto cr_type = map_elf_reloc_type(reloc.getType(), is_x86_64);
            if (!cr_type) return unsupported_relocation(reloc.getType(), is_x86_64);
            cr.type = *cr_type;

            // Get addend (RELA-style; for REL, addend is in the instruction)
            auto val_or_err = llvm::object::ELFRelocationRef(reloc).getAddend();
            cr.addend = val_or_err ? static_cast<int32_t>(*val_or_err) : 0;

            result.relocations.push_back(cr);
         }
      }

      // Sparse vectors are indexed by the complete WASM module. A declaration
      // in another compilation unit has no range here and no proven frame.
      result.function_offsets.resize(mod.code.size(), {0, 0});
      result.body_offsets.resize(mod.code.size(), {0, 0});
      result.import_thunk_offsets.resize(num_imports, {0, 0});
      result.import_thunk_stack_sizes.resize(num_imports, UINT64_MAX);
      result.entry_stack_sizes.resize(mod.code.size());
      result.body_stack_sizes.resize(mod.code.size());
      std::set<std::string> covered_functions;

      for (const auto& sym : obj.symbols()) {
         auto name_or_err = sym.getName();
         if (!name_or_err) continue;
         std::string name = name_or_err->str();

         uint64_t native_frame = 0;
         uint64_t native_offset = 0, native_size = 0;
         if (expected_functions.contains(name)) {
            auto section = sym.getSection();
            if (!section) return llvm_aot_result{.error = llvm::toString(section.takeError())};
            auto value = sym.getValue();
            if (!value) return llvm_aot_result{.error = llvm::toString(value.takeError())};
            if (*section == obj.section_end()) continue;
            if (**section != *text_section)
               return llvm_aot_result{.error = "AOT function is outside retained code section: " + name};
            const auto size = llvm::object::ELFSymbolRef(sym).getSize();
            if (!size || *value > text_section->getSize() ||
                size > text_section->getSize() - *value ||
                *value > UINT32_MAX || size > UINT32_MAX)
               return llvm_aot_result{.error = "invalid AOT native function range: " + name};
            native_offset = *value;
            native_size = size;
            auto frame = native_frames->find({(*section)->getIndex(), *value});
            if (frame == native_frames->end())
               return llvm_aot_result{.error = "missing native frame size for " + name};
            native_frame = frame->second;
            covered_functions.insert(name);
         } else {
            auto type = sym.getType();
            if (!type) return llvm_aot_result{.error = llvm::toString(type.takeError())};
            if (*type == llvm::object::SymbolRef::ST_Function) {
               auto section = sym.getSection();
               if (!section) return llvm_aot_result{.error = llvm::toString(section.takeError())};
               if (*section != obj.section_end())
                  return llvm_aot_result{.error = "unsupported outlined native function: " + name};
            }
            continue;
         }

         auto reference = parse_wasm_native_ref(name, mod);
         if (!reference)
            return llvm_aot_result{.error = llvm::toString(reference.takeError()) + ": " + name};
         const std::pair<uint32_t, uint32_t> range{
            static_cast<uint32_t>(native_offset), static_cast<uint32_t>(native_size)};
         if (reference->kind == wasm_native_kind::import_thunk) {
            result.import_thunk_offsets[reference->index] = range;
            result.import_thunk_stack_sizes[reference->index] = native_frame;
         } else if (reference->kind == wasm_native_kind::entry) {
            const auto code_index = reference->index - num_imports;
            result.entry_stack_sizes[code_index] = native_frame;
            result.function_offsets[code_index] = range;
         } else if (reference->kind == wasm_native_kind::body) {
            const auto code_index = reference->index - num_imports;
            result.body_stack_sizes[code_index] = native_frame;
            result.body_offsets[code_index] = range;
         }
      }

      if (covered_functions != expected_functions)
         return llvm_aot_result{.error = "incomplete AOT native frame coverage"};
      result.native_stack_probe_size = 4096;
      result.native_stack_frames_complete = true;

      if (!is_x86_64) {
         // Keep every unresolved BL/B close to a batch-local trampoline.
         // Appending a single island at the end of the final multi-hundred-MB
         // module cannot reach early callers. A literal-address trampoline
         // also handles cross-batch function calls, not just host symbols.
         std::unordered_map<uint64_t, uint32_t> veneers;
         std::vector<code_relocation> literals;
         for (auto& reloc : result.relocations) {
            if (reloc.type != reloc_type::aarch64_call26) continue;
            const uint64_t key = (uint64_t(static_cast<uint16_t>(reloc.symbol)) << 32) |
                                 static_cast<uint32_t>(reloc.addend);
            auto [it, inserted] = veneers.try_emplace(key, 0);
            if (inserted) {
               const auto offset = (result.code.size() + 7) & ~size_t(7);
               if (offset + 16 > INT32_MAX)
                  return llvm_aot_result{.error = "AArch64 AOT batch exceeds PZAM offset range"};
               it->second = static_cast<uint32_t>(offset);
               result.code.resize(offset + 16, 0);
               const uint32_t instructions[] = {0x58000050u, 0xD61F0200u}; // ldr x16, +8; br x16
               std::memcpy(result.code.data() + offset, instructions, sizeof(instructions));
               literals.push_back({static_cast<uint32_t>(offset + 8), reloc.symbol,
                                   reloc_type::abs64, 0, reloc.addend});
            }
            const int64_t distance = int64_t(it->second) - reloc.code_offset;
            if (distance < -(1LL << 27) || distance >= (1LL << 27))
               return llvm_aot_result{.error = "AArch64 AOT batch too large for branch island; reduce batch size"};
            reloc.symbol = reloc_symbol::code_blob_self;
            reloc.addend = static_cast<int32_t>(it->second);
         }
         result.relocations.insert(result.relocations.end(), literals.begin(), literals.end());
      }

      // Module must be destroyed before Context (Module references its Context)
      llvm_mod.reset();
      llvm_ctx.reset();

      return result;
   }

} // namespace psizam::detail
