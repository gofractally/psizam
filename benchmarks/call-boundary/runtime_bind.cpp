#include <psizam/runtime.hpp>
#include "shared.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
static thread_local bool count_allocations = false;
static thread_local uint64_t allocations = 0;
void* operator new(std::size_t n) {
   if(count_allocations) ++allocations;
   if(auto* p=std::malloc(n ? n : 1)) return p;
   throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }


struct bind_host {
   void log_u64(uint64_t) {}
   void log_string(std::string_view) {}
};
PSIO_HOST_MODULE(bind_host, interface(env, log_u64, log_string))
static std::vector<uint8_t> read(const std::string& path) {
   std::ifstream file(path,std::ios::binary);
   if(!file)throw std::runtime_error("cannot read "+path);
   return {std::istreambuf_iterator<char>(file),{}};
}
int main(int argc,char** argv) {
   if(argc!=4)return 2;
   try {
      auto provider_code=read(std::string(argv[1])+"/provider.wasm"),consumer_code=read(std::string(argv[1])+"/consumer.wasm");
      psizam::instance_policy policy;
      policy.gas_budget=1000000000000ull;
      std::string mode=argv[2],binding=argv[3];
      if(mode=="jit1")policy.initial=psizam::instance_policy::compile_tier::jit1;
      else if(mode=="jit2")policy.initial=psizam::instance_policy::compile_tier::jit2;
      else if(mode=="llvm")policy.initial=psizam::instance_policy::compile_tier::jit_llvm;
      else throw std::runtime_error("unknown engine");
      psizam::runtime runtime;bind_host host;
      auto pm=runtime.prepare(psizam::wasm_bytes{provider_code},policy),cm=runtime.prepare(psizam::wasm_bytes{consumer_code},policy);
      runtime.provide(pm,host);runtime.provide(cm,host);
      auto provider=runtime.instantiate(pm,policy);
      if(binding=="typed")runtime.bind<greeter>(cm,provider);
      else if(binding=="dynamic")runtime.bind(cm,provider,"greeter");
      else throw std::runtime_error("unknown binding");
      auto consumer=runtime.instantiate(cm,policy);
      auto proxy=consumer.as<processor>();
      if(proxy.test_add(7u,11u)!=18)throw std::runtime_error("warmup mismatch");
      constexpr uint32_t n=1000000;
      uint64_t sum=0;
      allocations=0;count_allocations=true;
      auto start=std::chrono::steady_clock::now();
      for(uint32_t i=0;i<n;++i)sum+=proxy.test_add(i,17u);
      auto end=std::chrono::steady_clock::now();
      count_allocations=false;
      if(sum!=uint64_t(n)*(n-1)/2+uint64_t(n)*17)throw std::runtime_error("result mismatch");
      std::printf("{\"ns_per_call\":%.6f,\"iterations\":%u,\"cpp_allocations\":%llu,\"consumer_gas\":%llu,\"provider_gas\":%llu,\"valid\":true}\n",
         std::chrono::duration<double,std::nano>(end-start).count()/n,n,(unsigned long long)allocations,
         (unsigned long long)consumer.get_instance_be()->gas_consumed_raw(),(unsigned long long)provider.get_instance_be()->gas_consumed_raw());
   }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
