#pragma once

#include <psizam/exceptions.hpp>
#include <psizam/utils.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace psizam {

// psizam.execution v1: four bytes {version, address bits, pointer ABI bits, 0}.
// An absent section selects ordinary Wasm semantics. Pointer storage and the
// execution profile are independent: both p16 and p32 can use memory16-v1.
inline constexpr std::string_view execution_profile_section = "psizam.execution";
inline constexpr uint32_t memory16_p16_v1 = 0x00101001;
inline constexpr uint32_t memory16_p32_v1 = 0x00201001;
inline constexpr uint32_t memory16_address_mask = 0xffff;
// A masked dynamic address + a u16 memarg offset + a v128 access. Padding is
// private, writable state, zeroed on instance initialization/reset.
inline constexpr uint32_t memory16_accessible_bytes = 2 * 65535 + 16;

inline constexpr bool is_memory16_profile(uint32_t profile) {
   return profile == memory16_p16_v1 || profile == memory16_p32_v1;
}

inline void validate_execution_profile(uint32_t profile) {
   PSIZAM_ASSERT(profile == 0 || is_memory16_profile(profile), wasm_parse_exception,
                 "unsupported psizam.execution profile");
}

// Scan section envelopes before compiling any function: custom sections may
// legally follow the code section. Never infer this profile from memory size.
inline uint32_t read_execution_profile(std::span<const uint8_t> wasm) {
   PSIZAM_ASSERT(wasm.size() >= 8, wasm_parse_exception, "truncated wasm header");
   size_t pos = 8;
   auto u32 = [&](size_t end) {
      uint32_t value = 0;
      for (unsigned i = 0; i < 5; ++i) {
         PSIZAM_ASSERT(pos < end, wasm_parse_exception, "truncated section length");
         uint8_t b = wasm[pos++];
         PSIZAM_ASSERT(i < 4 || (b & 0xf0) == 0, wasm_parse_exception,
                       "section length overflow");
         value |= uint32_t(b & 0x7f) << (7 * i);
         if (!(b & 0x80)) return value;
      }
      PSIZAM_THROW(wasm_parse_exception, "section length overflow");
   };
   uint32_t profile = 0;
   while (pos < wasm.size()) {
      const auto id = wasm[pos++];
      const auto len = u32(wasm.size());
      PSIZAM_ASSERT(len <= wasm.size() - pos, wasm_parse_exception, "truncated section");
      const size_t end = pos + len;
      if (id == 0) {
         const auto name_len = u32(end);
         PSIZAM_ASSERT(name_len <= end - pos, wasm_parse_exception, "truncated custom section name");
         std::string_view name(reinterpret_cast<const char*>(wasm.data() + pos), name_len);
         pos += name_len;
         if (name == execution_profile_section) {
            // LLD concatenates identically named custom sections from objects.
            // Accept repeated identical declarations, reject conflicting ABIs.
            PSIZAM_ASSERT(pos < end && (end - pos) % 4 == 0, wasm_parse_exception,
                          "invalid psizam.execution payload");
            while (pos < end) {
               uint32_t next = uint32_t(wasm[pos]) | uint32_t(wasm[pos + 1]) << 8 |
                               uint32_t(wasm[pos + 2]) << 16 | uint32_t(wasm[pos + 3]) << 24;
               pos += 4;
               PSIZAM_ASSERT(is_memory16_profile(next), wasm_parse_exception,
                             "unsupported psizam.execution profile");
               PSIZAM_ASSERT(profile == 0 || profile == next, wasm_parse_exception,
                             "conflicting psizam.execution profiles");
               profile = next;
            }
         }
      }
      pos = end;
   }
   return profile;
}

inline void append_execution_profile(std::vector<uint8_t>& wasm, uint32_t profile) {
   PSIZAM_ASSERT(is_memory16_profile(profile), wasm_parse_exception,
                 "unsupported psizam.execution profile");
   auto existing = read_execution_profile(wasm);
   PSIZAM_ASSERT(existing == 0 || existing == profile, wasm_parse_exception,
                 "conflicting psizam.execution profiles");
   if (existing) return;
   wasm.push_back(0);
   wasm.push_back(static_cast<uint8_t>(1 + execution_profile_section.size() + 4));
   wasm.push_back(static_cast<uint8_t>(execution_profile_section.size()));
   wasm.insert(wasm.end(), execution_profile_section.begin(), execution_profile_section.end());
   for (unsigned i = 0; i < 4; ++i) wasm.push_back(static_cast<uint8_t>(profile >> (8 * i)));
}

} // namespace psizam
