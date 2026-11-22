;; The memory is intentionally unexported. Host calls in start must obtain its
;; actual profile and bounds from the caller, before an instance cache exists.
(module
  (import "env" "span" (func $span (param i32 i32) (result i32)))
  (import "env" "go_far" (func $go_far (result i32)))
  (import "env" "reenter" (func $reenter (result i32)))
  (memory 1 1)
  (data (i32.const 65532) "\2a")
  (func $start
    i32.const 65532 i32.const 1 call $span drop
    i32.const 65536 i32.const 0 call $span drop
    call $go_far
    if i32.const -1 i32.const 0 call $span drop end)
  (start $start)
  (func (export "empty_far") (result i32)
    i32.const -1 i32.const 0 call $span)
  (func (export "empty_end") (result i32)
    i32.const 65536 i32.const 0 call $span)
  (func (export "reenter") (result i32) call $reenter))
