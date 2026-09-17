#include <stdint.h>
__attribute__((import_module("env"),import_name("increment")))
extern int32_t host_increment(int32_t);
int32_t guest_increment(int32_t x) { return x+1; }
int32_t bounce(int32_t n) {
   int32_t value=0;
   for(int32_t i=0;i<n;++i) value=host_increment(value);
   return value;
}
