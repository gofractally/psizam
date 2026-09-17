(module
  (import "env" "subtract" (func $subtract (param i32 i32) (result i32)))
  (memory (export "memory") 1)
  (table 1 funcref)
  (type $binary (func (param i32 i32) (result i32)))
  (func $difference (type $binary) (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.sub)
  (elem (i32.const 0) $difference)
  (func $init i32.const 0 i32.const 1 i32.store)
  (start $init)
  (func (export "_start")
    i32.const 4 i32.const 100 i32.const 58 call $subtract i32.store
    i32.const 8 i32.const 90 i32.const 48 i32.const 0 call_indirect (type $binary) i32.store))
