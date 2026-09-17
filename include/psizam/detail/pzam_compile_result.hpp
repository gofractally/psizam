#pragma once

#include <psizam/detail/jit_reloc.hpp>
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
      std::string                  target_triple;       // set by caller for LLVM AOT
      std::string                  error;               // non-empty on failure (e.g. LLVM AOT)
      bool                         softfloat = false;   // use softfloat wrappers (runtime option)
      bool                         backtrace = false;   // enable async backtrace frame tracking
      int                          opt_level = 2;       // LLVM optimization level (0-3)
   };

}
