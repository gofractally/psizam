(module
  (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
  (memory (export "memory") 1)
  ;; i32.add wraps first; the load's offset is added afterward without wrapping.
  ;; Folding both additions into one native 64-bit address incorrectly traps.
  (func $load (param i32) (result i32)
    local.get 0 i32.const 32 i32.add i32.load offset=4)
  (func (export "_start")
    i32.const 12
    i32.const 19
    i32.const 23
    i32.add
    i32.store
    i32.const -24
    call $load
    call $exit))
