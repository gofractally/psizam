(module
  (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
  (memory (export "memory") 1)
  (func $add (param f64 f64) (result f64)
    local.get 0 local.get 1 f64.add)
  (func (export "_start")
    f64.const 1.5 f64.const 2.25 call $add
    f64.const 3.75 f64.eq
    if (result i32) i32.const 42 else i32.const 1 end
    call $exit))
