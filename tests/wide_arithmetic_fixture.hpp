#pragma once
// One hand-assembled module and one arithmetic oracle for every engine's
// wide-arithmetic test. The module is built here because the opcodes are newer
// than the text-format tools the other fixtures rely on.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace wide_arithmetic {

enum class op : uint8_t { add128 = 0x13, sub128 = 0x14, mul_wide_s = 0x15, mul_wide_u = 0x16 };
enum class part { low, high, mix };

struct result { uint64_t low, high; };

inline result evaluate(op o, uint64_t a, uint64_t b, uint64_t c = 0, uint64_t d = 0) {
   using u128 = unsigned __int128;
   u128 value = 0;
   switch (o) {
      case op::add128: value = ((u128(b) << 64) | a) + ((u128(d) << 64) | c); break;
      case op::sub128: value = ((u128(b) << 64) | a) - ((u128(d) << 64) | c); break;
      case op::mul_wide_s: value = u128(__int128(int64_t(a)) * __int128(int64_t(b))); break;
      case op::mul_wide_u: value = u128(a) * u128(b); break;
   }
   return {uint64_t(value), uint64_t(value >> 64)};
}
inline uint64_t rotl31(uint64_t v) { return (v << 31) | (v >> 33); }
inline uint64_t select(result r, part p) {
   return p == part::low ? r.low : p == part::high ? r.high : (r.low ^ rotl31(r.high));
}

inline const char* op_name(op o) {
   switch (o) {
      case op::add128: return "add128";
      case op::sub128: return "sub128";
      case op::mul_wide_s: return "mul_wide_s";
      default: return "mul_wide_u";
   }
}
inline std::string export_name(op o, part p) {
   return std::string(op_name(o)) + (p == part::low ? "_lo" : p == part::high ? "_hi" : "_mix");
}
inline bool takes_four(op o) { return o == op::add128 || o == op::sub128; }

// Fixed operands of the constant-folding exports.
inline constexpr uint64_t const_a = 0xfedcba9876543210ull, const_b = 0x8000000000000001ull,
                          const_c = 0xffffffffffffffffull, const_d = 0x0123456789abcdefull;

inline std::vector<uint8_t> module_bytes() {
   using bytes = std::vector<uint8_t>;
   auto append = [](bytes& out, const bytes& in) { out.insert(out.end(), in.begin(), in.end()); };
   auto uleb = [](bytes& out, uint64_t v) {
      do { uint8_t b = v & 0x7f; v >>= 7; out.push_back(b | (v ? 0x80 : 0)); } while (v);
   };
   auto sleb = [](bytes& out, int64_t v) {
      for (;;) {
         uint8_t b = v & 0x7f; v >>= 7;
         if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) { out.push_back(b); break; }
         out.push_back(b | 0x80);
      }
   };
   auto section = [&](bytes& out, uint8_t id, const bytes& body) {
      out.push_back(id); uleb(out, body.size()); append(out, body);
   };
   struct function { std::string name; uint32_t type; bytes code; };
   std::vector<function> functions;
   // Leaves one i64: the low half, the high half, or both folded together.
   auto finish = [&](bytes& code, part p, uint32_t scratch) {
      if (p == part::low) code.push_back(0x1a);                                   // drop
      else if (p == part::high) { append(code, {0x21, uint8_t(scratch), 0x1a, 0x20, uint8_t(scratch)}); }
      else append(code, {0x42, 31, 0x89, 0x85});                                  // i64.const 31; rotl; xor
      code.push_back(0x0b);
   };
   for (op o : {op::add128, op::sub128, op::mul_wide_s, op::mul_wide_u}) {
      const uint32_t params = takes_four(o) ? 4 : 2;
      for (part p : {part::low, part::high, part::mix}) {
         bytes code;
         for (uint32_t i = 0; i < params; ++i) append(code, {0x20, uint8_t(i)});
         append(code, {0xfc, uint8_t(o)});
         finish(code, p, params);
         functions.push_back({export_name(o, p), takes_four(o) ? 0u : 1u, code});
      }
   }
   // a * b + (c, d): a product feeding a 128-bit add, every half live at once.
   {
      bytes code{0x20, 0, 0x20, 1, 0xfc, uint8_t(op::mul_wide_u), 0x20, 2, 0x20, 3, 0xfc, uint8_t(op::add128)};
      finish(code, part::mix, 4);
      functions.push_back({"multiply_add", 0, code});
   }
   // Constant operands, for engines that fold.
   for (op o : {op::add128, op::sub128, op::mul_wide_s, op::mul_wide_u}) {
      bytes code;
      for (uint64_t v : {const_a, const_b, const_c, const_d}) {
         if (!takes_four(o) && (v == const_c || v == const_d)) continue;
         code.push_back(0x42); sleb(code, int64_t(v));
      }
      append(code, {0xfc, uint8_t(o)});
      finish(code, part::mix, 0);
      functions.push_back({std::string("const_") + op_name(o), 2, code});
   }

   bytes out{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
   section(out, 1, {3, 0x60, 4, 0x7e, 0x7e, 0x7e, 0x7e, 1, 0x7e,
                       0x60, 2, 0x7e, 0x7e, 1, 0x7e,
                       0x60, 0, 1, 0x7e});
   bytes types, exports, bodies;
   uleb(types, functions.size()); uleb(exports, functions.size()); uleb(bodies, functions.size());
   for (uint32_t i = 0; i < functions.size(); ++i) {
      auto& f = functions[i];
      uleb(types, f.type);
      uleb(exports, f.name.size()); exports.insert(exports.end(), f.name.begin(), f.name.end());
      exports.push_back(0x00); uleb(exports, i);
      bytes body{1, 1, 0x7e}; // one i64 scratch local
      append(body, f.code);
      uleb(bodies, body.size()); append(bodies, body);
   }
   section(out, 3, types);
   section(out, 7, exports);
   section(out, 10, bodies);
   return out;
}

// Boundary operands first, then a fixed pseudo-random stream.
inline std::vector<uint64_t> operands(uint32_t random_count) {
   std::vector<uint64_t> values{0, 1, 2, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull,
                                0xffffffffull, 0x100000000ull, 0xfffffffffffffffeull};
   uint64_t state = 0x9e3779b97f4a7c15ull;
   for (uint32_t i = 0; i < random_count; ++i) {
      state += 0x9e3779b97f4a7c15ull;
      uint64_t z = state;
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
      values.push_back(z ^ (z >> 31));
   }
   return values;
}

// Calls `invoke(name, args...)` for every export and compares with the oracle.
// `check(actual, expected, name)` reports a mismatch.
template<class Invoke2, class Invoke4, class Invoke0, class Check>
void run_all(Invoke2&& call2, Invoke4&& call4, Invoke0&& call0, Check&& check) {
   const auto values = operands(24);
   const auto boundary = operands(0);
   for (op o : {op::mul_wide_s, op::mul_wide_u})
      for (part p : {part::low, part::high, part::mix}) {
         const auto name = export_name(o, p);
         for (uint64_t a : values)
            for (uint64_t b : values)
               check(call2(name.c_str(), a, b), select(evaluate(o, a, b), p), name);
      }
   for (op o : {op::add128, op::sub128})
      for (part p : {part::low, part::high, part::mix}) {
         const auto name = export_name(o, p);
         for (uint64_t a : boundary) for (uint64_t b : boundary)
            for (uint64_t c : boundary) for (uint64_t d : boundary)
               check(call4(name.c_str(), a, b, c, d), select(evaluate(o, a, b, c, d), p), name);
         for (size_t i = 0; i + 3 < values.size(); ++i)
            check(call4(name.c_str(), values[i], values[i + 1], values[i + 2], values[i + 3]),
                  select(evaluate(o, values[i], values[i + 1], values[i + 2], values[i + 3]), p), name);
      }
   for (size_t i = 0; i + 3 < values.size(); ++i) {
      const uint64_t a = values[i], b = values[i + 1], c = values[i + 2], d = values[i + 3];
      const auto product = evaluate(op::mul_wide_u, a, b);
      check(call4("multiply_add", a, b, c, d),
            select(evaluate(op::add128, product.low, product.high, c, d), part::mix), "multiply_add");
   }
   for (op o : {op::add128, op::sub128, op::mul_wide_s, op::mul_wide_u}) {
      const auto name = std::string("const_") + op_name(o);
      check(call0(name.c_str()), select(evaluate(o, const_a, const_b, const_c, const_d), part::mix), name);
   }
}

}  // namespace wide_arithmetic
