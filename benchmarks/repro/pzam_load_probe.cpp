// Container-only diagnostic: no relocation, module restoration, or execution.
// Both paths validate and copy exactly the selected native code into new pages.
#include <psizam/pzam_format.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) try {
   if (argc != 3) throw std::runtime_error("usage: pzam-load-probe FILE owned-single|owned-double|view-single|view-double");
   const std::string_view mode = argv[2];
   if (mode != "owned-single" && mode != "owned-double" &&
       mode != "view-single" && mode != "view-double")
      throw std::runtime_error("invalid mode");
   using clock = std::chrono::steady_clock;
   auto anchor = clock::now();
   auto mark = [&] { auto now = clock::now();
      double ms = std::chrono::duration<double, std::milli>(now-anchor).count();
      anchor = now; return ms; };
   int fd = open(argv[1], O_RDONLY);
   struct stat st{};
   if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) throw std::runtime_error("open/stat failed");
   void* mapping = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
   close(fd);
   if (mapping == MAP_FAILED) throw std::runtime_error("mmap failed");
   madvise(mapping, st.st_size, MADV_WILLNEED);
   std::span<const char> data{static_cast<const char*>(mapping), size_t(st.st_size)};
   double mmap_ms = mark();
   if (mode.ends_with("double") && !psizam::pzam_validate(data)) throw std::runtime_error("invalid PZAM");
   double extra_validation_ms = mark();
   if (!psizam::pzam_validate(data)) throw std::runtime_error("invalid PZAM");
   double validation_ms = mark();
#if defined(__aarch64__) || defined(__arm64__)
   constexpr uint8_t arch = uint8_t(psizam::pzam_arch::aarch64);
#elif defined(__x86_64__)
   constexpr uint8_t arch = uint8_t(psizam::pzam_arch::x86_64);
#else
#error Unsupported host
#endif
   using view_type = psio::view<psizam::pzam_file, psio::pssz, psio::storage::const_borrow>;
   std::optional<psizam::pzam_file> owned;
   std::span<const char> code;
   size_t functions = 0, relocations = 0, section_index = 0, sections_count = 0;
   if (mode.starts_with("owned")) {
      owned.emplace(psio::decode<psizam::pzam_file>(psio::pssz{}, data));
      sections_count = owned->code_sections.size();
      for (auto& section : owned->code_sections) {
         if (section.arch == arch) {
            code = {reinterpret_cast<const char*>(section.code_blob.data()), section.code_blob.size()};
            functions = section.functions.size(); relocations = section.relocations.size(); break;
         }
         ++section_index;
      }
   } else {
      auto sections = view_type{data}.get<4>(); sections_count = sections.size();
      for (size_t i = 0; i < sections.size(); ++i) {
         auto section = sections[i];
         if (section.get<0>() == arch) {
            code = psio::bytes(section.get<10>());
            functions = section.get<8>().size(); relocations = section.get<9>().size();
            section_index = i; break;
         }
      }
   }
   if (code.empty()) throw std::runtime_error("no native code");
   double decode_select_ms = mark();
   void* destination = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
   if (destination == MAP_FAILED) throw std::runtime_error("destination mmap failed");
   double allocation_ms = mark();
   std::memcpy(destination, code.data(), code.size());
   double copy_ms = mark();
   // Exact byte comparison and checksum deliberately excluded from phase timings.
   auto reference = psio::bytes(view_type{data}.get<4>()[section_index].get<10>());
   if (reference.size() != code.size() || std::memcmp(destination, reference.data(), code.size()))
      throw std::runtime_error("code copy mismatch");
   uint64_t checksum = 14695981039346656037ULL;
   for (size_t i = 0; i < code.size(); ++i) {
      checksum ^= static_cast<const unsigned char*>(destination)[i]; checksum *= 1099511628211ULL;
   }
   std::printf("{\"variant\":\"%s\",\"file_bytes\":%zu,\"code_bytes\":%zu,"
               "\"code_file_offset\":%zu,\"sections\":%zu,\"functions\":%zu,\"relocations\":%zu,"
               "\"code_fnv1a64\":\"%016llx\",\"valid\":true,"
               "\"mmap_ms\":%.6f,\"extra_validation_ms\":%.6f,\"validation_ms\":%.6f,"
               "\"decode_select_ms\":%.6f,\"allocation_ms\":%.6f,\"copy_ms\":%.6f,\"total_ms\":%.6f}\n",
               argv[2], data.size(), code.size(), size_t(reference.data()-data.data()), sections_count,
               functions, relocations, static_cast<unsigned long long>(checksum), mmap_ms,
               extra_validation_ms, validation_ms, decode_select_ms, allocation_ms, copy_ms,
               mmap_ms+extra_validation_ms+validation_ms+decode_select_ms+allocation_ms+copy_ms);
   munmap(destination, code.size()); owned.reset(); munmap(mapping, data.size());
   return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
