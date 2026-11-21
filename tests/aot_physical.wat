(module
  (import "env" "unused" (func))
  (memory 1)
  (global $vector (mut v128) (v128.const i32x4 1 2 3 4))
  (func (export "loop") (param $n i32) (result i32)
    (loop $again
      i32.const 0 memory.grow drop
      global.get $vector global.set $vector
      global.get $vector i32.const 0 v128.load i8x16.swizzle drop
      local.get $n i32.const 1 i32.sub local.tee $n br_if $again)
    i32.const 42)
  (func (export "value") (result i32) i32.const 42))
