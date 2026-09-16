#include "aot_spec_backend.hpp"
#include <psizam/detail/pzam_loader.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_metadata.hpp>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <fcntl.h>

extern char** environ;

namespace psizam::test {
namespace {
std::filesystem::path compile_artifact(const wasm_code& code) {
   const char* compiler = std::getenv("PSIZAM_AOT_SPEC_COMPILER");
   const char* directory = std::getenv("PSIZAM_AOT_SPEC_DIR");
   if (!compiler || !directory || ::access(compiler, X_OK) != 0) {
      std::cerr << "Set PSIZAM_AOT_SPEC_COMPILER to pzam-compile and "
                   "PSIZAM_AOT_SPEC_DIR to a fresh artifact directory\n";
      std::abort(); // Infrastructure failures must not satisfy CHECK_THROWS.
   }
   std::ostringstream hex;
   for (auto byte : pzam_cache::hash_wasm(code))
      hex << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
   std::filesystem::path base = std::filesystem::path(directory) / hex.str();
   std::filesystem::create_directories(directory);
   const auto wasm = base.string() + ".wasm";
   const auto pzam = base.string() + ".pzam";
   const auto log = base.string() + ".log";
   // Deliberately compile on every construction: a stale cache cannot turn
   // this into a test of code produced by a different compiler or tier.
   { std::ofstream out(wasm, std::ios::binary); out.write(reinterpret_cast<const char*>(code.data()), code.size()); }
#if defined(__aarch64__)
   const char* target = "--target=aarch64";
#elif defined(__x86_64__)
   const char* target = "--target=x86_64";
#else
#error AOT spec execution requires ARM64 or x86_64
#endif
   std::vector<std::string> args{compiler, target, "--backend=llvm", "--opt-level=2", "-j6", wasm, "-o", pzam};
   std::vector<char*> argv;
   for (auto& arg : args) argv.push_back(arg.data());
   argv.push_back(nullptr);
   { std::ofstream commands(std::filesystem::path(directory) / "commands.log", std::ios::app);
     for (const auto& arg : args) commands << std::quoted(arg) << ' ';
     commands << '\n'; }
   posix_spawn_file_actions_t actions;
   posix_spawn_file_actions_init(&actions);
   posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
   posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
   pid_t pid;
   const int error = posix_spawn(&pid, compiler, &actions, nullptr, argv.data(), environ);
   posix_spawn_file_actions_destroy(&actions);
   if (error) { std::cerr << "Cannot spawn AOT compiler: " << error << '\n'; std::abort(); }
   int status;
   while (waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) std::abort();
   }
   if (!WIFEXITED(status)) { std::cerr << "AOT compiler crashed; see " << log << '\n'; std::abort(); }
   if (WEXITSTATUS(status) != 0)
      throw std::runtime_error("AOT compiler rejected module; see " + log);
   return pzam;
}
}

void load_aot_o2_roundtrip(wasm_code& code, module& mod) {
   const auto path = compile_artifact(code);
   std::ifstream in(path, std::ios::binary);
   std::vector<char> bytes{std::istreambuf_iterator<char>{in}, {}};
   if (!pzam_validate(bytes)) throw std::runtime_error("AOT produced invalid PZAM: " + path.string());
   auto file = pzam_load(bytes);
   if (file.magic != PZAM_MAGIC || file.format_version != PZAM_VERSION || file.code_sections.size() != 1)
      throw std::runtime_error("Unexpected PZAM header");
   const auto& cs = file.code_sections.front();
   if (cs.compiler.compiler_name != "psizam-llvm" || file.input_hash != pzam_cache::hash_wasm(code))
      throw std::runtime_error("Wrong AOT compiler or input hash");
   if (cs.opt_tier != static_cast<uint8_t>(pzam_opt_tier::llvm_O2))
      throw std::runtime_error("Expected LLVM O2 PZAM code");

   auto loaded = detail::load_pzam(file);
   mod.allocator.release_base_memory();
   mod = std::move(loaded.mod);
   // growable_allocator has no ownership-transferring move constructor.
   // Relinquish the source's raw pointers after transferring the module.
   loaded.mod.allocator = growable_allocator{};
   mod.jit_engine = std::shared_ptr<void>(loaded.exec_code,
      [](void* p) { jit_allocator::instance().free(p); });
}
}
