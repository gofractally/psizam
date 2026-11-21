#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/* Diagnostic only: the consumer and execution tests determine correctness. */
int main(void) {
   if (sizeof(size_t) < 8) {
      puts("The sparse 16 GiB arena probe requires a 64-bit process.");
      return 0;
   }
   const size_t size = (size_t)16 * 1024 * 1024 * 1024;
   const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
   const char* names[] = {"writable-anonymous", "writable-anonymous-noreserve"};
   for (int i = 0; i < 2; ++i) {
      int selected = flags;
      if (i) {
#ifdef MAP_NORESERVE
         selected |= MAP_NORESERVE;
#else
         puts("MAP_NORESERVE is unavailable on this platform.");
         continue;
#endif
      }
      errno = 0;
      void* memory = mmap(NULL, size, PROT_READ | PROT_WRITE, selected, -1, 0);
      const int error = memory == MAP_FAILED ? errno : 0;
      printf("%s bytes=%zu success=%d errno=%d (%s)\n",
             names[i], size, memory != MAP_FAILED, error, strerror(error));
      if (memory != MAP_FAILED)
         munmap(memory, size);
   }
   return 0;
}
