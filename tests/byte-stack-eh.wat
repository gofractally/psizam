(module
  (import "env" "observe" (func $observe (param i32) (result i32)))
  (tag $failure (param i32))
  (func (export "leaf") (param i32) (result i32) (local i64 i64)
    i32.const 1 call $observe)
  (func $thrower (param i32) (result i32)
    i32.const 6 call $observe drop local.get 0 throw $failure)
  (func (export "caught") (param i32) (result i32)
    i32.const 7 call $observe drop
    (block $caught (try_table (catch_all $caught) i32.const 0 call $thrower drop))
    i32.const 8 call $observe))
