#include <psizam/backend.hpp>
#include <psizam/pzam_compile.hpp>
#include <cstdint>
#include <vector>
int main() {
   std::vector<std::uint8_t> wasm{0,97,115,109,1,0,0,0,1,5,1,0x60,0,1,0x7f,
      3,2,1,0,7,7,1,3,'r','u','n',0,0,10,6,1,4,0,0x41,42,0x0b};
   psizam::wasm_allocator memory;
#ifdef PSIZAM_CONSUMER_LLVM
   using implementation = psizam::jit_llvm;
   constexpr auto tier = psizam::pzam_tier_flags::llvm_O2;
   constexpr auto expected_tier = psizam::pzam_opt_tier::llvm_O2;
#else
   using implementation = psizam::jit;
   constexpr auto tier = psizam::pzam_tier_flags::jit2;
   constexpr auto expected_tier = psizam::pzam_opt_tier::jit2;
#endif
   auto compiled = psizam::pzam_load(psizam::pzam_compile(wasm, {.tier=tier}));
   if (compiled.code_sections.size() != 1 || compiled.code_sections[0].code_blob.empty() ||
       compiled.code_sections[0].opt_tier != static_cast<uint8_t>(expected_tier)) return 2;
   psizam::backend<std::nullptr_t, implementation> engine(wasm, &memory);
   return engine.call_with_return("env", "run")->to_ui32() == 42 ? 0 : 1;
}
