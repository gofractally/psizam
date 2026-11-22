(module
  (import "env" "span" (func $span (param i32 i32) (result i32)))
  (import "env" "pointer" (func $pointer (param i32) (result i32)))
  (memory (export "memory") 1 1)
  (data (i32.const 65532) "\2a\00\00\00")
  (func (export "span") (param i32 i32) (result i32)
    local.get 0 local.get 1 call $span)
  (func (export "pointer") (param i32) (result i32)
    local.get 0 call $pointer))
