#pragma once
#include <psizam/backend.hpp>
#include <psizam/runtime.hpp>
#include <psizam/detail/wit_section_finder.hpp>
#include <psio/wit_parser.hpp>
#include <psio/wit_gen.hpp>
#include <sstream>

namespace psizam::detail {
inline std::string signature_text(const func_type& type) {
   auto name = [](uint8_t t) -> const char* {
      switch (t) {
         case types::i32: return "i32";
         case types::i64: return "i64";
         case types::f32: return "f32";
         case types::f64: return "f64";
         case types::v128: return "v128";
         case types::funcref: return "funcref";
         case types::externref: return "externref";
         default: return "unknown";
      }
   };
   std::string result = "(";
   auto append = [&](const auto& list) {
      bool first = true;
      for (auto t : list) { if (!first) result += ", "; first = false; result += name(t); }
   };
   append(type.param_types); result += ") -> (";
   append(type.return_types); result += ")";
   return result;
}

inline std::string import_wit_signature(const wasm_code& wasm, std::string_view module_name,
                                       std::string_view function_name) {
   std::span<const uint8_t> payload;
   if (!find_component_wit_section(wasm, module_name, payload)) return {};
   const auto world = psio::wit_parser::parse_wit({
      reinterpret_cast<const char*>(payload.data()), payload.size()});
   // A bare `import env;` refers to a top-level interface, which the parser
   // retains in exports. Prefer an inline import, then its named definition.
   for (const auto* interfaces : {&world.imports, &world.exports})
   for (const auto& iface : *interfaces) {
      if (iface.name != module_name) continue;
      for (auto index : iface.func_idxs) {
         if (index >= world.funcs.size()) continue;
         const auto& func = world.funcs[index];
         auto symbol = func.name;
         std::replace(symbol.begin(), symbol.end(), '-', '_');
         if (func.name != function_name && symbol != function_name) continue;
         std::ostringstream out;
         psio::detail::wit_emit_func(out, world, func, "");
         return out.str();
      }
   }
   return {};
}

inline std::vector<unresolved_import> check_module_imports(const module& mod,
   const host_function_table& table, const wasm_code& wasm = {}) {
   std::vector<unresolved_import> result;
   for (const auto& import : mod.imports) {
      unresolved_import issue;
      issue.module_name.assign(reinterpret_cast<const char*>(import.module_str.data()), import.module_str.size());
      issue.func_name.assign(reinterpret_cast<const char*>(import.field_str.data()), import.field_str.size());
      if (import.kind != external_kind::Function) {
         issue.reason = unresolved_import::reason_code::unsupported_kind;
      } else {
         const auto& expected = mod.types[import.type.func_t];
         issue.expected_signature = signature_text(expected);
         issue.wit_signature = import_wit_signature(wasm, issue.module_name, issue.func_name);
         const auto* entry = table.find_entry(issue.module_name, issue.func_name);
         if (!entry) issue.reason = unresolved_import::reason_code::missing;
         else if (!(entry->signature == expected)) {
            issue.reason = unresolved_import::reason_code::signature_mismatch;
            func_type actual;
            actual.param_types.assign(entry->signature.params.begin(), entry->signature.params.end());
            actual.return_types.assign(entry->signature.ret.begin(), entry->signature.ret.end());
            issue.provided_signature = signature_text(actual);
         } else if (!entry->trampoline && !entry->slow_dispatch) {
            issue.reason = unresolved_import::reason_code::missing_dispatch;
         } else continue;
      }
      result.push_back(std::move(issue));
   }
   return result;
}

inline std::vector<unresolved_import> check_wasm_imports(wasm_code wasm,
                                                       const host_function_table& table) {
   psizam::module mod;
   mod.allocator.use_default_memory();
   null_debug_info debug;
   binary_parser<null_writer, default_options, null_debug_info> parser(
      mod.allocator, default_options{}, false, false);
   parser.parse_module(wasm, mod, debug);
   return check_module_imports(mod, table, wasm);
}
} // namespace psizam::detail
