// Keep upstream algorithms untouched: compile their main as benchmark_main.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
int benchmark_main(int argc, char** argv);
static uint64_t nanoseconds(void) {
   struct timespec t;
   if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
   return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
int main(int argc, char** argv) {
   // PolyBench's existing runtime condition dumps arrays with these arguments.
   char* validation_args[44];
   if (argc == 2 && !strcmp(argv[1],"--validate")) {
      for (int i=0;i<43;++i) validation_args[i] = "";
      validation_args[43] = NULL; argc=43; argv=validation_args;
   }
   uint64_t begin=nanoseconds();
   int result=benchmark_main(argc,argv);
   uint64_t end=nanoseconds();
   if (!begin || end < begin) return 125;
   fprintf(stderr,"\nBENCH_TIME_NS=%llu\n",(unsigned long long)(end-begin));
   return result;
}
