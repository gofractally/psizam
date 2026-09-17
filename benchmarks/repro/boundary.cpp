// Purpose-built scalar boundary microbenchmark, not a Component Model benchmark.
#include <wasmtime.h>
#include <psizam/backend.hpp>
#include <psizam/detail/execution_limits.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>

using timer=std::chrono::steady_clock;
static constexpr int32_t count=10000000;
static double ns(timer::time_point a,timer::time_point b) {
   return std::chrono::duration<double,std::nano>(b-a).count()/count;
}
__attribute__((noinline)) int32_t increment(int32_t x) { return x+1; }
struct options : psizam::default_options {
   psizam::fp_mode fp=psizam::fp_mode::fast;
   uint32_t compile_threads=1;
};
static void output(double host,double checked,double batched) {
   std::printf("{\"guest_to_host_ns\":%.6f,\"host_to_guest_checked_ns\":%.6f,\"host_to_guest_optimized_ns\":%.6f,\"iterations\":%d,\"valid\":true}\n",host,checked,batched,count);
}
template<class Impl> void psizam_run(psizam::wasm_code code, const std::string& policy) {
   using hosts=psizam::registered_host_functions<psizam::standalone_function_t>;
   hosts::add<&increment>("env","increment");
   psizam::wasm_allocator memory;
   options opts;
   opts.gas_metering = policy == "gas";
   psizam::backend<hosts,Impl,options> engine(code,&memory,opts);
   hosts::resolve(engine.get_module());engine.initialize(nullptr);
   psizam::detail::execution_limits limits;
   if (policy == "gas") limits.policy = psizam::detail::cpu_limit::gas;
   if (policy == "timeout") limits.policy = psizam::detail::cpu_limit::timeout;
   psizam::detail::with_execution_limits(engine.get_module(),engine.get_context(),limits,[&] {
   auto bounce=[&](int32_t n){return engine.call_with_return("env","bounce",n)->to_i32();};
   if(bounce(100)!=100) throw std::runtime_error("bounce validation");
   auto a=timer::now();auto answer=bounce(count);auto b=timer::now();
   if(answer!=count) throw std::runtime_error("bounce result");
   double host=ns(a,b);
   int32_t x=0;
   a=timer::now();
   for(int i=0;i<count;++i) x=engine.call_with_return("env","guest_increment",x)->to_i32();
   b=timer::now(); if(x!=count) throw std::runtime_error("checked result");
   double checked=ns(a,b);
   psizam::stack_allocator stack(engine.get_context().get_maximum_stack_size());
   auto fn=engine.template get_typed_function<int32_t(int32_t)>("guest_increment",stack);
   double optimized=0;
   engine.scoped_execution([&]{
      if(fn(10)!=11) throw std::runtime_error("typed validation");
      x=0;a=timer::now();for(int i=0;i<count;++i)x=fn(x);b=timer::now();
      optimized=ns(a,b);
   });
   if(x!=count) throw std::runtime_error("typed result");
   if((policy=="gas") != (engine.get_context().gas().consumed>0))
      throw std::runtime_error("gas instrumentation mismatch");
   output(host,checked,optimized);
   });
}
static void check(wasmtime_error_t* error,wasm_trap_t* trap=nullptr) {
   if(error) {wasm_name_t msg;wasmtime_error_message(error,&msg);std::string text(msg.data,msg.size);wasm_name_delete(&msg);wasmtime_error_delete(error);throw std::runtime_error(text);}
   if(trap){wasm_name_t msg;wasm_trap_message(trap,&msg);std::string text(msg.data,msg.size);wasm_name_delete(&msg);wasm_trap_delete(trap);throw std::runtime_error(text);}
}
static wasm_trap_t* host_increment(void*,wasmtime_caller_t*,wasmtime_val_raw_t* values,size_t) {
   values[0].i32=increment(values[0].i32);return nullptr;
}
static void wasmtime_run(const psizam::wasm_code& code, const std::string& policy) {
   auto* config=wasm_config_new();
   wasmtime_config_strategy_set(config,WASMTIME_STRATEGY_CRANELIFT);
   wasmtime_config_cranelift_opt_level_set(config,WASMTIME_OPT_LEVEL_SPEED);
   wasmtime_config_parallel_compilation_set(config,false);
   wasmtime_config_consume_fuel_set(config,policy=="gas");
   wasmtime_config_epoch_interruption_set(config,policy=="timeout");
   wasmtime_config_memory_reservation_set(config,1ull<<32);
   wasmtime_config_memory_guard_size_set(config,1ull<<32);
   auto* engine=wasm_engine_new_with_config(config);
   wasmtime_module_t* module=nullptr;check(wasmtime_module_new(engine,code.data(),code.size(),&module));
   auto* store=wasmtime_store_new(engine,nullptr,nullptr);auto* ctx=wasmtime_store_context(store);
   if(policy=="gas")check(wasmtime_context_set_fuel(ctx,1'000'000'000'000ull));
   if(policy=="timeout")wasmtime_context_set_epoch_deadline(ctx,1);
   std::jthread deadline;
   if(policy=="timeout")deadline=std::jthread([&](std::stop_token stop) {
      std::mutex mutex;
      std::condition_variable_any cv;
      std::unique_lock lock(mutex);
      cv.wait_for(lock,stop,std::chrono::seconds(120),[]{return false;});
      if(!stop.stop_requested())wasmtime_engine_increment_epoch(engine);
   });
   auto* linker=wasmtime_linker_new(engine);
   auto* type=wasm_functype_new_1_1(wasm_valtype_new_i32(),wasm_valtype_new_i32());
   check(wasmtime_linker_define_func_unchecked(linker,"env",3,"increment",9,type,host_increment,nullptr,nullptr));
   wasm_functype_delete(type);
   wasmtime_instance_t instance;wasm_trap_t* trap=nullptr;
   auto* error=wasmtime_linker_instantiate(linker,ctx,module,&instance,&trap);check(error,trap);
   wasmtime_extern_t guest,bounce;
   if(!wasmtime_instance_export_get(ctx,&instance,"guest_increment",15,&guest) || !wasmtime_instance_export_get(ctx,&instance,"bounce",6,&bounce))throw std::runtime_error("missing export");
   auto rawcall=[&](wasmtime_extern_t& fn,int32_t value){
      wasmtime_val_raw_t values[1]{};values[0].i32=value;wasm_trap_t* t=nullptr;
      auto* e=wasmtime_func_call_unchecked(ctx,&fn.of.func,values,1,&t);check(e,t);return values[0].i32;
   };
   if(rawcall(bounce,100)!=100)throw std::runtime_error("bounce validation");
   auto a=timer::now();auto answer=rawcall(bounce,count);auto b=timer::now();
   if(answer!=count)throw std::runtime_error("bounce result");double host=ns(a,b);
   int32_t x=0;a=timer::now();
   for(int i=0;i<count;++i){
      wasmtime_val_t arg{.kind=WASMTIME_I32,.of={.i32=x}},result;
      wasm_trap_t* t=nullptr;auto* e=wasmtime_func_call(ctx,&guest.of.func,&arg,1,&result,1,&t);check(e,t);x=result.of.i32;
   }
   b=timer::now();if(x!=count)throw std::runtime_error("checked result");double checked=ns(a,b);
   x=0;a=timer::now();for(int i=0;i<count;++i)x=rawcall(guest,x);b=timer::now();
   if(x!=count)throw std::runtime_error("unchecked result");
   if(policy=="gas") {
      uint64_t remaining=0;check(wasmtime_context_get_fuel(ctx,&remaining));
      if(remaining>=1'000'000'000'000ull)throw std::runtime_error("fuel not consumed");
   }
   output(host,checked,ns(a,b));
   wasmtime_extern_delete(&guest);wasmtime_extern_delete(&bounce);
   deadline.request_stop();if(deadline.joinable())deadline.join();
   wasmtime_linker_delete(linker);wasmtime_store_delete(store);wasmtime_module_delete(module);wasm_engine_delete(engine);
}
int main(int argc,char** argv) {
   if(argc!=4)return 2;
   try {
      std::ifstream file(argv[2],std::ios::binary);
      if(!file)throw std::runtime_error("cannot read module");
      psizam::wasm_code code{std::istreambuf_iterator<char>(file),{}};
      std::string mode=argv[1],policy=argv[3];psizam::detail::set_llvm_opt_level(2);
      if(policy!="none" && policy!="gas" && policy!="timeout")return 2;
      if(mode=="jit1")psizam_run<psizam::jit>(code,policy);
      else if(mode=="jit2")psizam_run<psizam::jit2>(code,policy);
      else if(mode=="llvm-jit")psizam_run<psizam::jit_llvm>(code,policy);
      else if(mode=="cranelift")wasmtime_run(code,policy);
      else return 2;
   } catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
