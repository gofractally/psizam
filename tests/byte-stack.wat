(module
  (import "env" "observe" (func $observe (param i32) (result i32)))
  (import "env" "reenter" (func $reenter (result i32)))
  (import "env" "fail" (func $fail))
  (type $one (func (param i32) (result i32)))
  (table 1 1 funcref)
  (elem (i32.const 0) $leaf)
  (func $leaf (export "leaf") (param i32) (result i32) (local i64 i64)
    i32.const 1 call $observe)
  (func (export "outer") (param i32) (result i32) (local $value i32)
    i32.const 2 call $observe drop
    i32.const 0 call $leaf local.set $value
    i32.const 3 call $observe drop
    local.get $value)
  (func (export "indirect") (param i32) (result i32)
    i32.const 0 i32.const 0 call_indirect (type $one))
  (func (export "tail-direct") (param i32) (result i32)
    i32.const 0 return_call $leaf)
  (func (export "tail-indirect") (param i32) (result i32)
    i32.const 0 i32.const 0 return_call_indirect (type $one))
  (func $recursive (export "recursive") (param $n i32) (result i32) (local i64)
    local.get $n i32.eqz
    if (result i32) i32.const 0 call $leaf
    else local.get $n i32.const 1 i32.sub call $recursive end)
  (func $tail (export "tail-self") (param $n i32) (result i32)
    local.get $n i32.eqz
    if (result i32) i32.const 0 return_call $leaf
    else local.get $n i32.const 1 i32.sub return_call $tail end)
  (func (export "host-nested") (param i32) (result i32)
    i32.const 4 call $observe drop call $reenter drop i32.const 5 call $observe)
  (func (export "host-failure") (param i32) (result i32) call $fail i32.const 0)
  (func (export "trap") (param i32) (result i32) unreachable))
