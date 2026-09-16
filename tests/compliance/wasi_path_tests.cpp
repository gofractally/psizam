#include <psizam/detail/wasi_host.hpp>
#include <catch2/catch.hpp>
#include <filesystem>
#include <fstream>

namespace {
struct path_fixture {
   std::filesystem::path temp;
   psizam::detail::wasi_host host;
   path_fixture() {
      auto pattern = (std::filesystem::temp_directory_path() / "psizam-wasi-XXXXXX").string();
      auto* created = ::mkdtemp(pattern.data());
      if (!created) throw std::runtime_error("mkdtemp failed");
      temp = created;
      std::filesystem::create_directories(temp / "root/llvm");
      std::filesystem::create_directories(temp / "root/third-party");
      std::filesystem::create_directories(temp / "root-sibling");
      std::ofstream(temp / "root/third-party/header.h") << "header";
      std::ofstream(temp / "root/name..with-dots") << "dots";
      std::ofstream(temp / "root-sibling/outside") << "outside";
      std::filesystem::create_directory_symlink(temp / "root-sibling", temp / "root/escape");
      host.add_preopen(".", (temp / "root").string());
   }
   ~path_fixture() {
      host.fd_close(3);
      std::filesystem::remove_all(temp);
   }
   std::string resolve(const std::string& path) {
      return host.resolve_path(3, path.data(), static_cast<uint32_t>(path.size()));
   }
};
}

TEST_CASE("WASI paths allow parent components inside a preopen", "[wasi][path]") {
   path_fixture f;
   CHECK_FALSE(f.resolve("llvm/../third-party/header.h").empty());
   CHECK_FALSE(f.resolve("llvm/../third-party/new-file").empty());
   CHECK_FALSE(f.resolve("name..with-dots").empty());
   CHECK_FALSE(f.resolve("llvm/..").empty());
}

TEST_CASE("WASI paths reject targets outside a preopen", "[wasi][path]") {
   path_fixture f;
   CHECK(f.resolve("../root-sibling/outside").empty());
   CHECK(f.resolve("../root-sibling/new-file").empty());
   CHECK(f.resolve("escape/outside").empty());
   CHECK(f.resolve("escape/new-file").empty());
   CHECK(f.resolve("/absolute").empty());
   CHECK(f.resolve(std::string("name..with-dots\0ignored", 23)).empty());
   CHECK(f.host.fd_close(3) == psizam::detail::WASI_ESUCCESS);
   CHECK(f.resolve("third-party/header.h").empty());
}
