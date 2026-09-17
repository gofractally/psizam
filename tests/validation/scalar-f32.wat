(module
  (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
  (memory (export "memory") 1)
  (func $add (param f32 f32) (result f32)
    local.get 0 local.get 1 f32.add)
  (func (export "_start")
    f32.const 1.5 f32.const 2.25 call $add
    f32.const 3.75 f32.eq
    if (result i32) i32.const 42 else i32.const 1 end
    call $exit))
