#pragma once

#include <psizam/detail/jit_reloc.hpp>
#include <psizam/native_guest_abi.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace psizam::detail {
   /// Result from compilation — captures relocations before codegen is destroyed.
   /// For LLVM AOT, also captures the code blob and function offsets.
   struct pzam_compile_result {
      std::vector<code_relocation> relocs;
      std::vector<uint8_t>         code_blob;          // filled by LLVM AOT path
      std::vector<std::pair<uint32_t, uint32_t>> function_offsets; // entry wrapper (offset, size)
      std::vector<std::pair<uint32_t, uint32_t>> body_offsets;     // function body (offset, size) — LLVM only
      // Private typed import targets, indexed by imported function index.
      // Offsets are relative to the same code blob as function/body offsets.
      std::vector<std::pair<uint32_t, uint32_t>> import_thunk_offsets;
      // Proven native prologue allocation upper bounds. LLVM may report exact
      // allocations; host C++ helpers/reentry and incoming arguments are separate.
      std::vector<uint64_t>        import_thunk_stack_sizes;
      psizam::native_guest_abi     native_guest_abi = psizam::native_guest_abi::legacy_caller_cleanup;
      std::vector<uint64_t>        entry_stack_sizes;  // native prologue bytes, LLVM only
      std::vector<uint64_t>        body_stack_sizes;
      uint32_t                    native_stack_probe_size = 0;
      bool                        native_stack_frames_complete = false;
      std::string                  target_triple;       // set by caller for LLVM AOT
      std::string                  error;               // non-empty on failure (e.g. LLVM AOT)
      bool                         softfloat = false;   // use softfloat wrappers (runtime option)
      bool                         backtrace = false;   // enable async backtrace frame tracking
      int                          opt_level = 2;       // LLVM optimization level (0-3)
   };

}
