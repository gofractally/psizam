#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <psizam/pzam_format.hpp>
#include <algorithm>

using namespace psizam;

namespace {
void put_u32(std::vector<char>& bytes, std::size_t offset, uint32_t value) {
   for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = char(value >> (8 * i));
}
}

TEST_CASE("PZAM uses the public psio pSSZ codec", "[pzam-format]") {
   pzam_file file;
   const auto bytes = pzam_save(file);
   CHECK(bytes == psio::encode(psio::pssz{}, file));
   REQUIRE(psio::validate<pzam_file>(psio::pssz{}, bytes).ok());
   REQUIRE(pzam_validate(bytes));
   const std::vector<char> prefix{'P','Z','A','M', char(PZAM_VERSION),0,0,0};
   REQUIRE(bytes.size() >= prefix.size());
   CHECK(std::equal(prefix.begin(), prefix.end(), bytes.begin()));
   CHECK(pzam_load(bytes).metadata.wit == std::nullopt);
   CHECK(pzam_save(pzam_load(bytes)) == bytes);
}

TEST_CASE("PZAM element entries keep full type indices and the null sentinel", "[pzam-format]") {
   pzam_file file;
   auto& segment = file.metadata.elements.emplace_back();
   segment.elems.push_back({UINT32_MAX, UINT32_MAX});   // ref.null
   segment.elems.push_back({300, 7});                   // canonical type index above 255
   const auto loaded = pzam_load(pzam_save(file));
   REQUIRE(loaded.metadata.elements.size() == 1);
   REQUIRE(loaded.metadata.elements[0].elems.size() == 2);
   CHECK(loaded.metadata.elements[0].elems[0].type == UINT32_MAX);
   CHECK(loaded.metadata.elements[0].elems[0].index == UINT32_MAX);
   CHECK(loaded.metadata.elements[0].elems[1].type == 300);
   CHECK(loaded.metadata.elements[0].elems[1].index == 7);
}

TEST_CASE("PZAM pSSZ preserves WIT and large code sections", "[pzam-format]") {
   pzam_file file;
   auto& world = file.metadata.wit.emplace();
   world.package = "test:pssz@1.0.0";
   world.name = "compiler";
   world.wit_source = "world compiler {}";
   world.attributes = {{"since", "version", "1.0.0"}};
   psio::wit_type_def type;
   type.name = "result";
   type.fields = {{"value", psio::wit_prim_idx(psio::wit_prim::u32), {}}};
   world.types.push_back(type);
   world.funcs = {{"compile", {{"source", psio::wit_prim_idx(psio::wit_prim::string_), {}}},
                  {{"result", 0, {}}}, 7, {}}};
   world.exports = {{"compiler", {0}, {0}, {}}};
   world.imports = {{"host", {}, {}, {}}};
   file.metadata.data = {{0, {0x41, 0, 0x0b}, 0, {0, 1, 0xff}}};
   file.metadata.exports = {{"", 0, 0}, {"run", 0, 1}};
   for (auto arch : {pzam_arch::aarch64, pzam_arch::x86_64}) {
      pzam_code_section cs;
      cs.arch = static_cast<uint8_t>(arch);
      cs.opt_tier = static_cast<uint8_t>(pzam_opt_tier::llvm_O2);
      cs.compiler.compiler_name = "psizam-llvm";
      cs.functions = {{0, 8, 16}, {8, 65536, 32}};
      cs.relocations = {{4, 1, 0, -12}};
      cs.attestations = {{{}, {0, 0xff, 42}}};
      cs.code_blob.resize(70000);
      for (std::size_t i = 0; i < cs.code_blob.size(); ++i) cs.code_blob[i] = uint8_t(i);
      file.code_sections.push_back(std::move(cs));
   }
   const auto bytes = pzam_save(file);
   REQUIRE(pzam_validate(bytes));
   const auto decoded = pzam_load(bytes);
   REQUIRE(decoded.metadata.wit);
   CHECK(decoded.metadata.wit->types[0].fields[0].type_idx == -6);
   CHECK(decoded.metadata.wit->funcs[0].params[0].name == "source");
   CHECK(decoded.metadata.wit->attributes[0].arg_value == "1.0.0");
   REQUIRE(decoded.code_sections.size() == 2);
   CHECK(decoded.code_sections[1].code_blob == file.code_sections[1].code_blob);
   CHECK(decoded.code_sections[0].relocations[0].addend == -12);
   CHECK(decoded.code_sections[0].functions[1].code_size == 65536);
   CHECK(pzam_save(decoded) == bytes);
}

TEST_CASE("PZAM validates identity and offsets before decoding", "[pzam-format]") {
   const auto bytes = pzam_save(pzam_file{});
   for (std::size_t n = 0; n < 48; ++n) {
      const auto truncated = std::span<const char>{bytes}.first(n);
      CHECK_FALSE(pzam_validate(truncated));
      CHECK_THROWS(pzam_load(truncated));
   }
   for (auto [offset, value] : {std::pair{0u, 0u}, {4u, 0u}, {4u, 4u}, {4u, PZAM_VERSION + 1},
                               {40u, 0u}, {40u, 49u}, {40u, UINT32_MAX},
                               {44u, 0u}, {44u, UINT32_MAX}}) {
      auto corrupt = bytes;
      put_u32(corrupt, offset, value);
      CHECK_FALSE(pzam_validate(corrupt));
      CHECK_THROWS(pzam_load(corrupt));
   }
   pzam_file invalid;
   invalid.magic = 0;
   CHECK_THROWS_AS(pzam_save(invalid), std::invalid_argument);
   invalid.magic = PZAM_MAGIC;
   invalid.format_version = 0;
   CHECK_THROWS_AS(pzam_save(invalid), std::invalid_argument);
}
