(module
  (memory 1)
  ;; Keep both scalar and SIMD locals live across a loop and a branch. These
  ;; must become SSA values even when the function-entry gas check branches.
  (func (export "run") (param $n i32) (result i32)
    (local $sum i32) (local $lanes v128)
    (loop $again
      local.get $sum
      i32.const 0
      i32.load
      i32.add
      local.set $sum
      local.get $lanes
      v128.const i32x4 1 2 3 4
      i32x4.add
      local.set $lanes
      local.get $n
      i32.const 1
      i32.sub
      local.tee $n
      br_if $again)
    local.get $sum
    local.get $lanes
    i32x4.extract_lane 0
    i32.add))
