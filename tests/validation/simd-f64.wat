(module
  (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
  (memory (export "memory") 1)
  (func $add (param v128 v128) (result v128)
    local.get 0 local.get 1 f64x2.add)
  (func (export "_start")
    v128.const f64x2 1.5 -2.5
    v128.const f64x2 2.25 3.25
    call $add
    v128.const f64x2 3.75 0.75
    f64x2.eq i64x2.all_true
    if (result i32) i32.const 42 else i32.const 1 end
    call $exit))
