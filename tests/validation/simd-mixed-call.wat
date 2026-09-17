(module
  (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
  (memory (export "memory") 1)
  (type $mixed (func (param i32 v128 i64 v128 i32) (result v128)))
  (table 1 funcref)
  (elem (i32.const 0) $mix)
  (func $mix (type $mixed)
    local.get 0 i32.const 11 i32.eq
    local.get 2 i64.const 1234567890123 i64.eq i32.and
    local.get 4 i32.const 31 i32.eq i32.and
    if (result v128)
      local.get 1 local.get 3 f32x4.add
    else
      v128.const i32x4 0 0 0 0
    end)
  (func $tail (type $mixed)
    local.get 0 local.get 1 local.get 2 local.get 3 local.get 4
    i32.const 0 return_call_indirect (type $mixed))
  (func (export "_start")
    i32.const 11 v128.const f32x4 1.5 2.5 -1.5 -2.5
    i64.const 1234567890123 v128.const f32x4 2.25 3.25 2.25 3.25 i32.const 31
    call $mix
    v128.const f32x4 3.75 5.75 0.75 0.75 f32x4.eq i32x4.all_true
    i32.const 11 v128.const f32x4 1.5 2.5 -1.5 -2.5
    i64.const 1234567890123 v128.const f32x4 2.25 3.25 2.25 3.25 i32.const 31
    i32.const 0 call_indirect (type $mixed)
    v128.const f32x4 3.75 5.75 0.75 0.75 f32x4.eq i32x4.all_true
    i32.and
    i32.const 11 v128.const f32x4 1.5 2.5 -1.5 -2.5
    i64.const 1234567890123 v128.const f32x4 2.25 3.25 2.25 3.25 i32.const 31
    call $tail
    v128.const f32x4 3.75 5.75 0.75 0.75 f32x4.eq i32x4.all_true
    i32.and
    if (result i32) i32.const 42 else i32.const 1 end
    call $exit))
