#pragma once

// Force-included by the memory16 SDK toolchain. Each object carries its ABI;
// LLD concatenates these records and psizam rejects conflicting declarations.
#ifndef __wasm32__
#error "memory16 requires the wasm32 instruction set"
#endif
#ifndef PSI_MEMORY16_POINTER_BITS
#define PSI_MEMORY16_POINTER_BITS 16
#endif
#if __SIZEOF_POINTER__ * 8 != PSI_MEMORY16_POINTER_BITS
#error "memory16 SDK pointer ABI mismatch; use the psiserve p16 LLVM compiler"
#endif
#if PSI_MEMORY16_POINTER_BITS != 16 && PSI_MEMORY16_POINTER_BITS != 32
#error "unsupported memory16 pointer ABI"
#endif

// A C section attribute creates a linear-memory data segment. Use a Wasm
// assembler custom section so the record never consumes guest memory.
#if PSI_MEMORY16_POINTER_BITS == 16
__asm__(".section .custom_section.psizam.execution,\"\",@\n"
        ".int32 0x00101001\n");
#else
__asm__(".section .custom_section.psizam.execution,\"\",@\n"
        ".int32 0x00201001\n");
#endif
