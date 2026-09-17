(module
  (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
  (memory (export "memory") 1)
  ;; Data initialization is outside generated function IR. Keep the arithmetic
  ;; operands unknown to optimization so portable targets must lower FMA.
  (data (i32.const 0) "\00\00\00\40\00\00\40\40\00\00\80\40\00\00\a0\40\00\00\40\40\00\00\80\40\00\00\a0\40\00\00\c0\40\00\00\80\40\00\00\a0\40\00\00\c0\40\00\00\e0\40\00\00\00\00\00\00\00\40\00\00\00\00\00\00\08\40\00\00\00\00\00\00\08\40\00\00\00\00\00\00\10\40\00\00\00\00\00\00\10\40\00\00\00\00\00\00\14\40")
  (func (export "_start")
    i32.const 0 v128.load i32.const 16 v128.load i32.const 32 v128.load
    f32x4.relaxed_madd
    v128.const f32x4 10 17 26 37 f32x4.eq i32x4.all_true
    i32.const 0 v128.load i32.const 16 v128.load i32.const 32 v128.load
    f32x4.relaxed_nmadd
    v128.const f32x4 -2 -7 -14 -23 f32x4.eq i32x4.all_true
    i32.and
    i32.const 48 v128.load i32.const 64 v128.load i32.const 80 v128.load
    f64x2.relaxed_madd
    v128.const f64x2 10 17 f64x2.eq i64x2.all_true
    i32.and
    i32.const 48 v128.load i32.const 64 v128.load i32.const 80 v128.load
    f64x2.relaxed_nmadd
    v128.const f64x2 -2 -7 f64x2.eq i64x2.all_true
    i32.and
    if (result i32) i32.const 42 else i32.const 1 end
    call $exit))
