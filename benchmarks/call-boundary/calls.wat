(module
 (import "db" "get" (func $get (param i32) (result i32)))
 (import "db" "mix" (func $mix (param i32 i32) (result i32)))
 (import "service" "cross" (func $cross (param i32 i32) (result i32)))
 (memory (export "memory") 1 8)
 (func (export "leaf") (param i32) (result i32) local.get 0 call $get)
 (func (export "roundtrip") (param i32) (result i32)
   local.get 0 call $get local.get 0 call $mix)
 (func (export "reenter") (param i32 i32) (result i32)
   local.get 1
   if (result i32)
     local.get 0 local.get 1 i32.const 1 i32.sub call $cross
   else
     local.get 0 call $get
   end)
 (func (export "queries") (param $n i32) (param $x i32) (result i32)
   block $done
     loop $again
       local.get $n i32.eqz br_if $done
       local.get $x call $get local.get $x call $mix local.set $x
       local.get $n i32.const 1 i32.sub local.set $n
       br $again
     end
   end
   local.get $x)
 (func (export "bad_memory") (result i32) i32.const 65536 i32.load)
 (func (export "unreachable") unreachable)
 (func (export "grow") (result i32) i32.const 1 memory.grow)
 (func (export "read") (param i32) (result i32) local.get 0 i32.load)
 (func (export "mixed") (param i32 f64 i64 f32) (result f64)
   local.get 0 f64.convert_i32_s local.get 1 f64.add
   local.get 2 f64.convert_i64_s f64.add local.get 3 f64.promote_f32 f64.add)
 (func $recurse (export "overflow") (param i32) (result i32)
   local.get 0 i32.const 1 i32.add call $recurse i32.const 1 i32.add)
 (func (export "canonical")
   (param i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64)
   (result i64) local.get 0 local.get 15 i64.add)
)
