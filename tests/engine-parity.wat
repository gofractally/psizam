(module
  (type $nullary (func (result i32)))
  (table 2 funcref)
  (elem (i32.const 0) funcref (ref.null func) (ref.func $forty-two))
  (func $forty-two (result i32) i32.const 42)

  ;; A reloaded artifact must still see a ref.null element as null.
  (func (export "indirect-null") (param i32) (result i32)
    i32.const 0 call_indirect (type $nullary))
  (func (export "indirect-good") (param i32) (result i32)
    i32.const 1 call_indirect (type $nullary))

  ;; br_if to a loop with a parameter: the untaken path still owns the
  ;; original parameter value.
  (func (export "loop-param-fallthrough") (param i32) (result i32)
    i32.const 7
    (loop (param i32) (result i32)
      i32.const 100
      local.get 0
      br_if 0
      i32.add))

  ;; As above, but the branch is taken a few times first.
  (func (export "loop-param-countdown") (param $n i32) (result i32)
    i32.const 7
    (loop (param i32) (result i32)
      local.get $n
      i32.const 1000
      i32.add
      (local.tee $n (i32.sub (local.get $n) (i32.const 1)))
      i32.const 0
      i32.gt_s
      br_if 0
      i32.add))

  ;; Loop parameters exchanged on the back edge are a parallel assignment.
  (func (export "loop-param-swap") (param $n i32) (result i32) (local $a i32) (local $b i32)
    i32.const 3
    i32.const 5
    (loop (param i32 i32) (result i32 i32)
      local.set $b
      local.set $a
      local.get $b
      local.get $a
      (local.tee $n (i32.sub (local.get $n) (i32.const 1)))
      i32.const 0
      i32.gt_s
      br_if 0)
    i32.const 16
    i32.shl
    i32.or)

  ;; Fibonacci through a two-parameter loop: (a, b) -> (b, a + b).
  (func (export "loop-param-fib") (param $n i32) (result i32) (local $a i32) (local $b i32)
    i32.const 0
    i32.const 1
    (loop (param i32 i32) (result i32 i32)
      local.set $b
      local.set $a
      local.get $b
      local.get $a
      local.get $b
      i32.add
      (local.tee $n (i32.sub (local.get $n) (i32.const 1)))
      i32.const 0
      i32.gt_s
      br_if 0)
    drop)

  ;; The untaken path of br_if keeps deeper loop parameters as well.
  (func (export "loop-param-deep") (param $n i32) (result i32)
    i32.const 1
    i32.const 2
    i32.const 3
    (loop (param i32 i32 i32) (result i32)
      i32.const 10
      i32.const 20
      i32.const 30
      (local.tee $n (i32.sub (local.get $n) (i32.const 1)))
      i32.const 0
      i32.gt_s
      br_if 0
      ;; not taken: [p0 p1 p2 10 20 30]
      i32.add
      i32.add
      i32.add
      i32.const 7
      i32.mul
      i32.add
      i32.const 5
      i32.mul
      i32.add))

  ;; The same untaken-path rule for every scalar type.
  (func (export "loop-param-f64") (param i32) (result i32)
    f64.const 1.5
    (loop (param f64) (result f64)
      f64.const 2.25
      local.get 0
      br_if 0
      f64.add)
    f64.const 4
    f64.mul
    i32.trunc_f64_s)
  (func (export "loop-param-f32") (param i32) (result i32)
    f32.const 1.5
    (loop (param f32) (result f32)
      f32.const 2.25
      local.get 0
      br_if 0
      f32.add)
    f32.const 4
    f32.mul
    i32.trunc_f32_s)
  (func (export "loop-param-i64") (param i32) (result i32)
    i64.const 0x700000005
    (loop (param i64) (result i64)
      i64.const 0x100000001
      local.get 0
      br_if 0
      i64.add)
    i64.const 28
    i64.shr_u
    i32.wrap_i64)

  ;; A shuffle's sixteen lane indices are an immediate. Read as operand
  ;; registers they name the scalar values below, which then reach the call
  ;; from vector storage that was never written.
  (func $weigh3 (param i32 i32 i32) (result i32)
    (i32.add (i32.mul (local.get 0) (i32.const 10000))
      (i32.add (i32.mul (local.get 1) (i32.const 100)) (local.get 2))))
  (func (export "shuffle-lanes-are-immediate") (param i32) (result i32)
    (call $weigh3
      (i32.add (local.get 0) (i32.const 1))
      (i32.add (local.get 0) (i32.const 2))
      (i32.add (local.get 0) (i32.const 3)))
    (i8x16.extract_lane_u 0
      (i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 0 0 1 0 2 0
        (v128.const i8x16 7 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)
        (v128.const i8x16 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9)))
    i32.add
    (i8x16.extract_lane_u 0
      (i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 3 0 4 0 5 0
        (v128.const i8x16 7 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)
        (v128.const i8x16 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9)))
    i32.add
    (i8x16.extract_lane_u 0
      (i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 6 0 7 0 8 0
        (v128.const i8x16 7 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)
        (v128.const i8x16 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9)))
    i32.add
    (i8x16.extract_lane_u 0
      (i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 9 0 10 0 11 0
        (v128.const i8x16 7 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)
        (v128.const i8x16 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9)))
    i32.add
    (i8x16.extract_lane_u 0
      (i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 12 0 13 0 14 0
        (v128.const i8x16 7 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)
        (v128.const i8x16 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9)))
    i32.add
    (i8x16.extract_lane_u 0
      (i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 15 0 16 0 17 0
        (v128.const i8x16 7 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)
        (v128.const i8x16 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9)))
    i32.add
    (i8x16.extract_lane_u 0
      (i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 18 0 19 0 20 0
        (v128.const i8x16 7 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)
        (v128.const i8x16 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9)))
    i32.add
    (i8x16.extract_lane_u 0
      (i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 21 0 22 0 23 0
        (v128.const i8x16 7 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)
        (v128.const i8x16 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9 9)))
    i32.add
)
)
