// Equivalent protected entry per service invocation; linking is outside timing.
#ifdef HAVE_WASMTIME
// wasm.h's native float aliases collide with SoftFloat's global struct names.
#define float32_t wasmtime_float32_t
#define float64_t wasmtime_float64_t
#include <wasmtime.h>
#undef float64_t
#undef float32_t
#endif
#include <psizam/backend.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include <psizam/detail/watchdog.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
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

static void require(bool ok, const char* what) { if(!ok) throw std::runtime_error(what); }
static uint32_t get_value(uint32_t x) { return (x*1664525u+1013904223u)&0x7fffffffu; }
static uint32_t mix_value(uint32_t a,uint32_t b) { return (a^(b*33u))&0x7fffffffu; }
static int destroyed_host_exceptions=0;
struct host_failure : std::exception {
   ~host_failure() override { ++destroyed_host_exceptions; }
   const char* what() const noexcept override { return "host failure"; }
};
struct host {
   std::function<int32_t(int32_t,int32_t)> cross_call;
   std::function<void()> on_get;
   bool fail=false;
   __attribute__((noinline)) int32_t get(int32_t x) {
      if(fail) throw host_failure{};
      if(on_get) on_get();
      return get_value(x);
   }
   __attribute__((noinline)) int32_t mix(int32_t a,int32_t b) { return mix_value(a,b); }
   int32_t cross(int32_t x,int32_t depth) { return cross_call(x,depth); }
};
struct options : psizam::default_options {
   psizam::fp_mode fp=psizam::fp_mode::fast;
   uint32_t compile_threads=1;
   uint32_t max_call_depth=64;
};
// Execute only the saved native code; the compiler is a separate CLI process.
template<typename Options, typename DebugInfo> struct saved_llvm_parser {
   saved_llvm_parser(psizam::growable_allocator&, const Options&, bool, bool) {}
   psizam::module& parse_module(psizam::wasm_code& bytes,psizam::module& mod,DebugInfo&) {
      auto file=psizam::pzam_load({reinterpret_cast<const char*>(bytes.data()),bytes.size()});
      require(file.code_sections.size()==1 && file.code_sections[0].opt_tier==static_cast<uint8_t>(psizam::pzam_opt_tier::llvm_O2),"expected LLVM O2 PZAM");
      require(file.code_sections[0].compiler.compiler_name=="psizam-llvm","expected LLVM compiler");
      auto loaded=psizam::detail::load_pzam(file);
      mod.allocator.release_base_memory();mod=std::move(loaded.mod);
      loaded.mod.allocator=psizam::growable_allocator{};
      mod.jit_engine=std::shared_ptr<void>(loaded.exec_code,[](void* p){psizam::jit_allocator::instance().free(p);});
      return mod;
   }
};
struct saved_llvm {
   using context=psizam::detail::jit_execution_context<false>;
   template<typename Options,typename DebugInfo> using parser=saved_llvm_parser<Options,DebugInfo>;
   static constexpr bool is_jit=true,enable_backtrace=false,reverse_host_args=false;
};
using clock_type=std::chrono::steady_clock;
static constexpr int loops=1000000;
struct metric { double ns; uint64_t allocs; };
template<class F> metric measure(int n,F&& f) {
   allocations=0;count_allocations=true;
   const auto a=clock_type::now();
   f();
   const auto b=clock_type::now();
   count_allocations=false;
   return {std::chrono::duration<double,std::nano>(b-a).count()/n,allocations};
}
static void print(const char* key,metric m) {
   std::printf("\"%s_ns\":%.6f,\"%s_cpp_allocations\":%llu,",key,m.ns,key,(unsigned long long)m.allocs);
}
template<class Impl> void run(psizam::wasm_code code,bool gas,bool check_only,bool timeout=false) {
   using hosts=psizam::registered_host_functions<host>;
   hosts::template add<&host::get>("db","get");
   hosts::template add<&host::mix>("db","mix");
   hosts::template add<&host::cross>("service","cross");
   psizam::wasm_allocator ma,mb;
   options opts;opts.gas_metering=gas;
   host ha,hb;
   psizam::backend<hosts,Impl,options> a(code,&ma,opts),b(code,&mb,opts);
   hosts::resolve(a.get_module());hosts::resolve(b.get_module());
   a.initialize(&ha);b.initialize(&hb);
   if(gas) {a.get_context().set_gas_budget(psizam::gas_units{1000000000000ull});b.get_context().set_gas_budget(psizam::gas_units{1000000000000ull});}
   auto test_and_measure = [&] {
   const auto leaf=a.resolve_export("leaf"),round=a.resolve_export("roundtrip"),queries=a.resolve_export("queries"),reenter=a.resolve_export("reenter");
   const auto other_reenter=b.resolve_export("reenter");
   ha.cross_call=[&](int32_t x,int32_t d){return b.call_by_index(&hb,other_reenter,x,d)->to_i32();};
   hb.cross_call=[&](int32_t x,int32_t d){return a.call_by_index(&ha,reenter,x,d)->to_i32();};
   auto expect_throw=[&](auto&& f){ bool caught=false;try{f();}catch(const std::exception&){caught=true;}require(caught,"missing expected exception");};
   for(int32_t x : {0,1,37,23456}) {
      require(a.call_by_index(&ha,round,x)->to_ui32()==mix_value(get_value(x),x),"roundtrip mismatch");
      require(a.call_by_index(&ha,reenter,x,int32_t(4))->to_ui32()==get_value(x),"A-B-A reentry mismatch");
   }
   require(a.resolve_export("missing")==UINT32_MAX,"missing export lookup");
   expect_throw([&]{a.call_by_index(&ha,leaf,double(1));});
   for(int n=0;n<3;++n) {
      expect_throw([&]{a.call_with_return(&ha,"bad_memory");});
      expect_throw([&]{a.call_with_return(&ha,"unreachable");});
      expect_throw([&]{a.call_with_return(&ha,"overflow",int32_t(0));});
      hb.fail=true;
      const int destroyed_before=destroyed_host_exceptions;
      expect_throw([&]{a.call_by_index(&ha,reenter,int32_t(1),int32_t(1));});
      hb.fail=false;
      require(destroyed_host_exceptions==destroyed_before+1,"host exception cleanup skipped");
      require(a.call_by_index(&ha,reenter,int32_t(7),int32_t(4))->to_ui32()==get_value(7),"recovery failed");
   }
   // Exercise actual A -> B -> A execution on distinct leased large stacks.
   auto max_a=a.get_module().maximum_stack,max_b=b.get_module().maximum_stack;
   a.get_module().maximum_stack=b.get_module().maximum_stack=8192;
   const auto prior_guard=psizam::detail::stack_guard_range;
   for(int i=0;i<3;++i) {
      require(a.call_by_index(&ha,reenter,int32_t(17),int32_t(4))->to_ui32()==get_value(17),"large-stack reentry mismatch");
      require(psizam::detail::stack_guard_range.data()==prior_guard.data(),"stale stack guard after return");
      hb.fail=true;
      const int destroyed_before=destroyed_host_exceptions;
      expect_throw([&]{a.call_by_index(&ha,reenter,int32_t(1),int32_t(1));});
      hb.fail=false;
      require(destroyed_host_exceptions==destroyed_before+1,"host exception cleanup skipped");
      require(psizam::detail::stack_guard_range.data()==prior_guard.data(),"stale stack guard after trap");
   }
   a.get_module().maximum_stack=max_a;b.get_module().maximum_stack=max_b;
   // Host grows the active guest memory; the following entry reads the new page.
   ha.on_get=[&]{require(a.call_with_return(&ha,"grow")->to_i32()==1,"growth failed");};
   a.call_by_index(&ha,leaf,int32_t(9));ha.on_get={};
   require(a.call_with_return(&ha,"read",int32_t(65536))->to_i32()==0,"new memory not visible");
   require(a.call_with_return(&ha,"mixed",int32_t(3),double(4.5),int64_t(6),float(1.25))->to_f64()==14.75,"mixed arguments");
   if(gas) {
      a.get_context().set_gas_budget(psizam::gas_units{0});
      expect_throw([&]{a.call_by_index(&ha,queries,int32_t(1000),int32_t(1));});
      a.get_context().set_gas_budget(psizam::gas_units{1000000000000ull});
      require(a.call_by_index(&ha,leaf,int32_t(1))->to_ui32()==get_value(1),"gas recovery");
   }
   if(timeout) {
      for(auto* engine : {&a,&b}) {
         auto* h=engine==&a?&ha:&hb;
         bool interrupted=false;
         try {
            engine->timed_run(psizam::detail::watchdog{std::chrono::milliseconds(2)},[&] {
               engine->call_by_index(h,queries,int32_t(500000000),int32_t(1));
            });
         }catch(const psizam::timeout_exception&) { interrupted=true; }
         require(interrupted,"page timeout did not interrupt guest queries");
         require(engine->call_by_index(h,leaf,int32_t(1))->to_ui32()==get_value(1),"timeout recovery failed");
      }
   }
   if(check_only) {std::puts("{\"valid\":true}");return;}
   std::printf("{");
   auto time_calls=[&](const char* key,auto&& call,auto&& expected){
      int32_t value=1;
      // Warm the exact path before allocation counts and timing.
      require(uint32_t(call(value))==expected(value),"warmup mismatch");
      auto result=measure(loops,[&]{for(int i=0;i<loops;++i) value=call(value);});
      uint32_t wanted=1;for(int i=0;i<loops;++i) wanted=expected(wanted);
      require(uint32_t(value)==wanted,"timed result mismatch");print(key,result);
   };
   time_calls("entry_by_name",[&](int32_t x){return a.call_with_return(&ha,"leaf",x)->to_i32();},get_value);
   time_calls("entry_by_index",[&](int32_t x){return a.call_by_index(&ha,leaf,x)->to_i32();},get_value);
   time_calls("host_service_host",[&](int32_t x){return a.call_by_index(&ha,round,x)->to_i32();},[](uint32_t x){return mix_value(get_value(x),x);});
   time_calls("service_a_host_service_b",[&](int32_t x){return a.call_by_index(&ha,reenter,x,int32_t(1))->to_i32();},get_value);
   time_calls("service_a_b_a",[&](int32_t x){return a.call_by_index(&ha,reenter,x,int32_t(2))->to_i32();},get_value);
   uint32_t value=0;
   auto query_result=measure(loops*2,[&]{value=a.call_by_index(&ha,queries,int32_t(loops),int32_t(1))->to_ui32();});
   uint32_t expected=1;for(int i=0;i<loops;++i) expected=mix_value(get_value(expected),expected);
   require(value==expected,"query result mismatch");print("repeated_host_query",query_result);
   // A module with a conservative stack bound forces the alternate-stack path.
   // Compilation and linking are unchanged; only entry stack reservation grows.
   a.get_module().maximum_stack=8192;
   constexpr int large_loops=1000;
   a.call_by_index(&ha,leaf,int32_t(1));
   auto large=measure(large_loops,[&]{for(int i=0;i<large_loops;++i) require(a.call_by_index(&ha,leaf,int32_t(i))->to_ui32()==get_value(i),"large stack result");});
   print("alternate_stack_entry",large);
   require((a.get_context().gas().consumed>0)==gas,"gas instrumentation mismatch");
   std::printf("\"iterations\":%d,\"valid\":true}\n",loops);
   };
   if(timeout) {
      a.timed_run(psizam::detail::watchdog{std::chrono::seconds(120)},[&] {
         b.timed_run(psizam::detail::watchdog{std::chrono::seconds(120)},test_and_measure);
      });
   } else test_and_measure();
}
#ifdef HAVE_WASMTIME
static void check(wasmtime_error_t* error,wasm_trap_t* trap=nullptr) {
   if(error) {wasm_name_t msg;wasmtime_error_message(error,&msg);std::string text(msg.data,msg.size);wasm_name_delete(&msg);wasmtime_error_delete(error);throw std::runtime_error(text);}
   if(trap) {wasm_name_t msg;wasm_trap_message(trap,&msg);std::string text(msg.data,msg.size);wasm_name_delete(&msg);wasm_trap_delete(trap);throw std::runtime_error(text);}
}
static wasm_trap_t* callback(void* env,wasmtime_caller_t*,wasmtime_val_raw_t* args,size_t) {
   auto& h=*static_cast<std::pair<host*,int>*>(env);
   try {
      if(h.second==0)args[0].i32=h.first->get(args[0].i32);
      else if(h.second==1)args[0].i32=h.first->mix(args[0].i32,args[1].i32);
      else args[0].i32=h.first->cross(args[0].i32,args[1].i32);
      return nullptr;
   }catch(const std::exception& e){return wasmtime_trap_new(e.what(),std::strlen(e.what()));}
}
static void run_wasmtime(const psizam::wasm_code& code,bool gas,bool check_only,bool checked,bool timeout=false) {
   auto* config=wasm_config_new();
   wasmtime_config_strategy_set(config,WASMTIME_STRATEGY_CRANELIFT);
   wasmtime_config_cranelift_opt_level_set(config,WASMTIME_OPT_LEVEL_SPEED);
   wasmtime_config_parallel_compilation_set(config,false);
   wasmtime_config_consume_fuel_set(config,gas);
   wasmtime_config_epoch_interruption_set(config,timeout);
   wasmtime_config_memory_reservation_set(config,1ull<<32);
   wasmtime_config_memory_guard_size_set(config,1ull<<32);
   auto* engine=wasm_engine_new_with_config(config);
   wasmtime_module_t* module=nullptr;check(wasmtime_module_new(engine,code.data(),code.size(),&module));
   auto* store=wasmtime_store_new(engine,nullptr,nullptr);auto* ctx=wasmtime_store_context(store);
   if(gas)check(wasmtime_context_set_fuel(ctx,1000000000000ull));
   if(timeout)wasmtime_context_set_epoch_deadline(ctx,1);
   host hosts[2];std::pair<host*,int> callbacks[2][3];
   wasmtime_func_t functions[2][4];
   for(int instance=0;instance<2;++instance) {
      auto* linker=wasmtime_linker_new(engine);
      for(int f=0;f<3;++f) {
         callbacks[instance][f]={&hosts[instance],f};
         auto* type=f==0?wasm_functype_new_1_1(wasm_valtype_new_i32(),wasm_valtype_new_i32()):wasm_functype_new_2_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32());
         const char* ns=f==2?"service":"db";const char* name=f==0?"get":f==1?"mix":"cross";
         check(wasmtime_linker_define_func_unchecked(linker,ns,std::strlen(ns),name,std::strlen(name),type,callback,&callbacks[instance][f],nullptr));
         wasm_functype_delete(type);
      }
      wasmtime_instance_t inst;wasm_trap_t* trap=nullptr;
      auto* err=wasmtime_linker_instantiate(linker,ctx,module,&inst,&trap);check(err,trap);
      const char* names[]={"leaf","roundtrip","reenter","queries"};
      for(int f=0;f<4;++f) {
         wasmtime_extern_t item;
         require(wasmtime_instance_export_get(ctx,&inst,names[f],std::strlen(names[f]),&item),"missing Wasmtime export");
         require(item.kind==WASMTIME_EXTERN_FUNC,"wrong Wasmtime export kind");functions[instance][f]=item.of.func;
         // Validate cached raw-call signatures at bind time.
         auto* type=wasmtime_func_type(ctx,&item.of.func);
         require(wasm_functype_params(type)->size==(f<2?1:2) && wasm_functype_results(type)->size==1,"Wasmtime signature mismatch");
         for(size_t i=0;i<wasm_functype_params(type)->size;++i) require(wasm_valtype_kind(wasm_functype_params(type)->data[i])==WASM_I32,"Wasmtime argument type");
         require(wasm_valtype_kind(wasm_functype_results(type)->data[0])==WASM_I32,"Wasmtime result type");
         wasm_functype_delete(type);wasmtime_extern_delete(&item);
      }
      wasmtime_linker_delete(linker);
   }
   auto call=[&](int instance,int f,int32_t x,int32_t y=0) {
      wasm_trap_t* trap=nullptr;wasmtime_error_t* error;
      int32_t result;
      if(checked) {
         wasmtime_val_t args[2]={{.kind=WASMTIME_I32,.of={.i32=x}},{.kind=WASMTIME_I32,.of={.i32=y}}},out;
         error=wasmtime_func_call(ctx,&functions[instance][f],args,f<2?1:2,&out,1,&trap);
         check(error,trap);result=out.of.i32;
      } else {
         wasmtime_val_raw_t args[2]{};args[0].i32=x;args[1].i32=y;
         error=wasmtime_func_call_unchecked(ctx,&functions[instance][f],args,f<2?1:2,&trap);
         check(error,trap);result=args[0].i32;
      }
      return result;
   };
   hosts[0].cross_call=[&](int32_t x,int32_t d){return call(1,2,x,d);};
   hosts[1].cross_call=[&](int32_t x,int32_t d){return call(0,2,x,d);};
   auto test_and_measure = [&] {
   if(timeout) {
      bool interrupted=false;
      {
         auto deadline=psizam::detail::watchdog{std::chrono::milliseconds(2)}.scoped_run([&] {wasmtime_engine_increment_epoch(engine);});
         try { call(0,3,500000000,1); }
         catch(const std::exception&) { interrupted=true; }
      }
      require(interrupted,"Wasmtime epoch timeout did not interrupt queries");
      wasmtime_context_set_epoch_deadline(ctx,1);
   }
   for(int32_t x : {0,1,37,23456}) {
      require(uint32_t(call(0,1,x))==mix_value(get_value(x),x),"Wasmtime roundtrip mismatch");
      require(uint32_t(call(0,2,x,4))==get_value(x),"Wasmtime reentry mismatch");
   }
   if(!check_only) {
      std::printf("{");
      auto timed=[&](const char* name,int f,int depth,auto expected) {
         int32_t value=1;call(0,f,1,depth);
         auto result=measure(loops,[&]{for(int i=0;i<loops;++i)value=call(0,f,value,depth);});
         uint32_t wanted=1;for(int i=0;i<loops;++i)wanted=expected(wanted);
         require(uint32_t(value)==wanted,"Wasmtime timed mismatch");print(name,result);
      };
      timed("entry_by_index",0,0,get_value);
      timed("host_service_host",1,0,[](uint32_t x){return mix_value(get_value(x),x);});
      timed("service_a_host_service_b",2,1,get_value);
      timed("service_a_b_a",2,2,get_value);
      uint32_t value;
      auto result=measure(loops*2,[&]{value=call(0,3,loops,1);});
      uint32_t expected=1;for(int i=0;i<loops;++i)expected=mix_value(get_value(expected),expected);
      require(value==expected,"Wasmtime query mismatch");print("repeated_host_query",result);
      std::printf("\"iterations\":%d,\"valid\":true}\n",loops);
   }else std::puts("{\"valid\":true}");
   };
   if(timeout) {
      auto deadline=psizam::detail::watchdog{std::chrono::seconds(120)}.scoped_run([&] {wasmtime_engine_increment_epoch(engine);});
      test_and_measure();
   } else test_and_measure();
   wasmtime_store_delete(store);wasmtime_module_delete(module);wasm_engine_delete(engine);
}
#endif
int main(int argc,char** argv) {
   if(argc!=5) {std::fprintf(stderr,"Usage: call-boundary jit1|jit2|llvm|cranelift-checked|cranelift-cached calls.wasm none|gas|timeout check|bench\n");return 2;}
   try {
      std::ifstream file(argv[2],std::ios::binary);require(bool(file),"cannot read Wasm");
      psizam::wasm_code code{std::istreambuf_iterator<char>(file),{}};
      std::string mode=argv[1],policy=argv[3];require(policy=="none"||policy=="gas"||policy=="timeout","invalid policy");
      const bool timeout=policy=="timeout",gas=policy=="gas",checks=std::string(argv[4])=="check";
      if(mode=="jit1")run<psizam::jit>(code,gas,checks,timeout);
      else if(mode=="jit2")run<psizam::jit2>(code,gas,checks,timeout);
      else if(mode=="llvm-aot")run<saved_llvm>(code,gas,checks,timeout);
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      else if(mode=="llvm") {psizam::detail::set_llvm_opt_level(2);run<psizam::jit_llvm>(code,gas,checks,timeout);}
#endif
#ifdef HAVE_WASMTIME
      else if(mode=="cranelift-checked"||mode=="cranelift-cached")run_wasmtime(code,gas,checks,mode=="cranelift-checked",timeout);
#endif
      else throw std::runtime_error("unknown or disabled engine");
   }catch(const std::exception& e){count_allocations=false;std::fprintf(stderr,"%s\n",e.what());return 1;}
}
