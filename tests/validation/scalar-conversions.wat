(module
  (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
  (memory (export "memory") 1)
  (func $a (param f32) (result i32) local.get 0 i32.trunc_f32_s)
  (func $b (param f32) (result i32) local.get 0 i32.trunc_f32_u)
  (func $c (param f64) (result i32) local.get 0 i32.trunc_f64_s)
  (func $d (param f64) (result i32) local.get 0 i32.trunc_f64_u)
  (func $e (param f32) (result i64) local.get 0 i64.trunc_f32_s)
  (func $f (param f32) (result i64) local.get 0 i64.trunc_f32_u)
  (func $g (param f64) (result i64) local.get 0 i64.trunc_f64_s)
  (func $h (param f64) (result i64) local.get 0 i64.trunc_f64_u)
  (func (export "_start")
    f32.const -42.75 call $a i32.const -42 i32.eq
    f32.const 42.75 call $b i32.const 42 i32.eq i32.and
    f64.const -2147483648.75 call $c i32.const -2147483648 i32.eq i32.and
    f64.const 42.75 call $d i32.const 42 i32.eq i32.and
    f32.const -42.75 call $e i64.const -42 i64.eq i32.and
    f32.const 42.75 call $f i64.const 42 i64.eq i32.and
    f64.const -1234567890123.75 call $g i64.const -1234567890123 i64.eq i32.and
    f64.const 1234567890123.75 call $h i64.const 1234567890123 i64.eq i32.and
    if (result i32) i32.const 42 else i32.const 1 end call $exit))
