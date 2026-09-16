#pragma once
// Byte streams for the engine's Wasm/DWARF reader and native debug-image writer.
// These are fixed-width and LEB128 operations, independent of psio's data formats.
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psizam::debug::binary {
inline void check(bool condition, const char* message) {
   if (!condition) throw std::runtime_error(message);
}
   struct vector_stream
   {
      std::vector<char>& data;
      vector_stream(std::vector<char>& data) : data(data) {}

      void about_to_write(size_t amount) { data.reserve(data.size() + amount); }

      void write(char ch) { data.push_back(ch); }

      void write(const void* src, size_t size)
      {
         auto s = reinterpret_cast<const char*>(src);
         data.insert(data.end(), s, s + size);
      }

      void rewrite(size_t offset, const void* src, size_t size)
      {
         std::memcpy(data.data() + offset, src, size);
      }

      template <typename T>
      void write_raw(const T& v)
      {
         write(&v, sizeof(v));
      }

      template <typename T>
      void rewrite_raw(size_t offset, const T& v)
      {
         rewrite(offset, &v, sizeof(v));
      }

      size_t written() const { return data.size(); }
   };
   struct fixed_buf_stream
   {
      char* begin;
      char* pos;
      char* end;

      fixed_buf_stream(char* pos, size_t size) : begin(pos), pos{pos}, end{pos + size} {}

      void about_to_write(size_t amount) {}

      void write(char ch)
      {
         if (pos >= end)
            throw std::runtime_error("binary stream overrun");
         *pos++ = ch;
      }

      void write(const void* src, size_t size)
      {
         if (pos + size > end)
            throw std::runtime_error("binary stream overrun");
         std::memcpy(pos, src, size);
         pos += size;
      }

      void rewrite(size_t offset, const void* src, size_t size)
      {
         std::memcpy(begin + offset, src, size);
      }

      template <typename T>
      void write_raw(const T& v)
      {
         write(&v, sizeof(v));
      }

      template <typename T>
      void rewrite_raw(size_t offset, const T& v)
      {
         rewrite(offset, &v, sizeof(v));
      }

      void skip(int32_t s)
      {
         if ((pos + s > end) or (pos + s < begin))
            throw std::runtime_error("binary stream overrun");
         pos += s;
      }

      size_t remaining() const { return end - pos; }
      size_t consumed() const { return pos - begin; }
      size_t written() const { return pos - begin; }
   };
   struct size_stream
   {
      size_t size = 0;

      void about_to_write(size_t amount) {}

      void write(char ch) { ++size; }
      void write(const void* src, size_t size) { this->size += size; }

      void rewrite(size_t offset, const void* src, size_t size) {}

      template <typename T>
      void write_raw(const T& v)
      {
         size += sizeof(v);
      }

      template <typename T>
      void rewrite_raw(size_t offset, const T& v)
      {
      }

      void   skip(int32_t s) { size += s; }
      size_t written() const { return size; }
   };
   struct input_stream
   {
      const char* pos;
      const char* end;

      input_stream() : pos{nullptr}, end{nullptr} {}
      input_stream(const char* pos, size_t size) : pos{pos}, end{pos + size} {}
      input_stream(const char* pos, const char* end) : pos{pos}, end{end}
      {
         if (end < pos)
            throw std::runtime_error("binary stream overrun");
      }
      input_stream(const std::vector<char>& v) : pos{v.data()}, end{v.data() + v.size()} {}
      input_stream(std::string_view v) : pos{v.data()}, end{v.data() + v.size()} {}
      input_stream(std::span<const char> v) : pos{v.data()}, end{v.data() + v.size()} {}
      input_stream(const input_stream&) = default;

      input_stream& operator=(const input_stream&) = default;

      std::string_view  string_view() const { return {pos, remaining()}; }
      std::vector<char> vector() const { return {pos, pos + remaining()}; }

      size_t remaining() const { return end - pos; }

      void check_available(size_t size) const
      {
         if (size > size_t(end - pos))
            throw std::runtime_error("binary stream overrun");
      }

      void read(void* dest, size_t size)
      {
         if (size > size_t(end - pos))
            throw std::runtime_error("binary stream overrun");
         std::memcpy(dest, pos, size);
         pos += size;
      }

      template <typename T>
      void read_raw(T& dest)
      {
         read(&dest, sizeof(dest));
      }

      void skip(size_t size)
      {
         if (size > size_t(end - pos))
            throw std::runtime_error("binary stream overrun");
         pos += size;
      }

      void read_reuse_storage(const char*& result, size_t size)
      {
         if (size > size_t(end - pos))
            throw std::runtime_error("binary stream overrun");
         result = pos;
         pos += size;
      }

      operator std::span<const char>() const { return {pos, end}; }
   };
inline uint64_t read_uleb(input_stream& stream, unsigned bits) {
   uint64_t result = 0;
   for (unsigned shift = 0; shift < bits; shift += 7) {
      uint8_t byte; stream.read(&byte, 1);
      unsigned remaining = bits - shift;
      check(remaining >= 7 || (byte & 0x7f) < (uint64_t{1} << remaining), "LEB128 overflow");
      result |= uint64_t(byte & 0x7f) << shift;
      if (!(byte & 0x80)) return result;
   }
   throw std::runtime_error("unterminated LEB128");
}
inline uint32_t varuint32_from_bin(input_stream& s) { return static_cast<uint32_t>(read_uleb(s, 32)); }
inline uint64_t varuint64_from_bin(input_stream& s) { return read_uleb(s, 64); }
inline void varuint32_from_bin(uint32_t& v, input_stream& s) { v = varuint32_from_bin(s); }
inline int64_t sleb64_from_bin(input_stream& s) {
   uint64_t result = 0;
   for (unsigned shift = 0; shift < 64; shift += 7) {
      uint8_t byte; s.read(&byte, 1);
      check(shift != 63 || (byte & 0x7f) == 0 || (byte & 0x7f) == 0x7f, "signed LEB128 overflow");
      result |= uint64_t(byte & 0x7f) << shift;
      if (!(byte & 0x80)) {
         if (shift < 57 && (byte & 0x40)) result |= ~uint64_t{0} << (shift + 7);
         return std::bit_cast<int64_t>(result);
      }
   }
   throw std::runtime_error("unterminated signed LEB128");
}
inline int32_t sleb32_from_bin(input_stream& s) {
   auto v = sleb64_from_bin(s);
   check(v >= INT32_MIN && v <= INT32_MAX, "signed LEB32 overflow");
   return static_cast<int32_t>(v);
}
template <typename S> void varuint32_to_bin(uint32_t v, S& s) {
   do { uint8_t b = v & 0x7f; v >>= 7; s.write(static_cast<char>(b | (v ? 0x80 : 0))); } while (v);
}
template <typename S> void sleb64_to_bin(int64_t v, S& s) {
   bool more;
   do {
      uint8_t b = v & 0x7f; v >>= 7;
      more = !((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)));
      s.write(static_cast<char>(b | (more ? 0x80 : 0)));
   } while (more);
}
template <typename T> requires (std::is_integral_v<T> || std::is_enum_v<T>)
void from_bin(T& v, input_stream& s) {
   static_assert(std::endian::native == std::endian::little);
   s.read(&v, sizeof(v));
}
inline void from_bin(std::string& v, input_stream& s) {
   auto size = varuint32_from_bin(s); s.check_available(size);
   v.assign(s.pos, size); s.skip(size);
}
template <typename T> T from_bin(input_stream& s) { T v; from_bin(v, s); return v; }
template <typename T, typename S> requires (std::is_integral_v<T> || std::is_enum_v<T>)
void to_bin(T v, S& s) {
   static_assert(std::endian::native == std::endian::little);
   s.write(&v, sizeof(v));
}
template <typename S> void to_bin(std::string_view v, S& s) {
   check(v.size() <= UINT32_MAX, "string too large");
   varuint32_to_bin(static_cast<uint32_t>(v.size()), s);
   s.write(v.data(), v.size());
}
template <typename S> void to_bin(std::span<const char> v, S& s) {
   to_bin(std::string_view{v.data(), v.size()}, s);
}
template <typename S> void to_bin(const std::vector<char>& v, S& s) {
   to_bin(std::span<const char>{v}, s);
}
} // namespace psizam::debug::binary
