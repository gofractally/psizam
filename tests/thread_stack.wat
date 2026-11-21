(module
  (import "env" "observe" (func $observe (param i32) (result i32)))
  (memory 1)
  (func (export "value") (param i32) (result i32)
    local.get 0 i32.const 1 i32.add)
  (func (export "nested") (param i32) (result i32)
    local.get 0 call $observe i32.const 1 i32.add)
  (func (export "trap") unreachable))
