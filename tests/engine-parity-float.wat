;; Operators whose deterministic result the float profile defines.
(module
  ;; a*a = 1 + 2^-11 + 2^-24. Rounded to f32 the last term is lost, so
  ;; multiply-then-add gives 0 and one fused rounding gives 2^-24.
  (func (export "madd-f32") (param i32) (result i32)
    (i32x4.extract_lane 0 (f32x4.relaxed_madd
      (v128.const f32x4 0x1.001p+0 0x1.001p+0 0x1.001p+0 0x1.001p+0)
      (v128.const f32x4 0x1.001p+0 0x1.001p+0 0x1.001p+0 0x1.001p+0)
      (v128.const f32x4 -0x1.002p+0 -0x1.002p+0 -0x1.002p+0 -0x1.002p+0))))
  (func (export "nmadd-f32") (param i32) (result i32)
    (i32x4.extract_lane 0 (f32x4.relaxed_nmadd
      (v128.const f32x4 0x1.001p+0 0x1.001p+0 0x1.001p+0 0x1.001p+0)
      (v128.const f32x4 0x1.001p+0 0x1.001p+0 0x1.001p+0 0x1.001p+0)
      (v128.const f32x4 0x1.002p+0 0x1.002p+0 0x1.002p+0 0x1.002p+0))))
  ;; a*a = 1 + 2^-26 + 2^-54; the high word of the f64 result is returned.
  (func (export "madd-f64") (param i32) (result i32)
    (i32.wrap_i64 (i64.shr_u (i64x2.extract_lane 0 (f64x2.relaxed_madd
      (v128.const f64x2 0x1.0000002p+0 0x1.0000002p+0)
      (v128.const f64x2 0x1.0000002p+0 0x1.0000002p+0)
      (v128.const f64x2 -0x1.0000004p+0 -0x1.0000004p+0))) (i64.const 32))))
  (func (export "nmadd-f64") (param i32) (result i32)
    (i32.wrap_i64 (i64.shr_u (i64x2.extract_lane 0 (f64x2.relaxed_nmadd
      (v128.const f64x2 0x1.0000002p+0 0x1.0000002p+0)
      (v128.const f64x2 0x1.0000002p+0 0x1.0000002p+0)
      (v128.const f64x2 0x1.0000004p+0 0x1.0000004p+0))) (i64.const 32))))
  ;; A NaN operand of a fused operator gives the canonical NaN.
  (func (export "madd-nan") (param i32) (result i32)
    (i32x4.extract_lane 0 (f32x4.relaxed_madd
      (i32x4.splat (local.get 0))
      (v128.const f32x4 2 2 2 2) (v128.const f32x4 1 1 1 1))))
  ;; Relaxed min and max are the ordinary ones: NaN wins, -0 is below +0.
  (func (export "min-f32") (param i32) (result i32)
    (i32x4.extract_lane 0 (f32x4.relaxed_min
      (v128.const f32x4 0 0 0 0) (i32x4.splat (local.get 0)))))
  (func (export "max-f32") (param i32) (result i32)
    (i32x4.extract_lane 0 (f32x4.relaxed_max
      (v128.const f32x4 -0 -0 -0 -0) (i32x4.splat (local.get 0)))))
  (func (export "min-f64") (param i32) (result i32)
    (i32.wrap_i64 (i64.shr_u (i64x2.extract_lane 0 (f64x2.relaxed_min
      (v128.const f64x2 0 0)
      (i64x2.splat (i64.shl (i64.extend_i32_u (local.get 0)) (i64.const 32)))))
      (i64.const 32))))
  (func (export "max-f64") (param i32) (result i32)
    (i32.wrap_i64 (i64.shr_u (i64x2.extract_lane 0 (f64x2.relaxed_max
      (v128.const f64x2 -0 -0)
      (i64x2.splat (i64.shl (i64.extend_i32_u (local.get 0)) (i64.const 32)))))
      (i64.const 32))))
  ;; A NaN converts to the positive canonical NaN of the result type.
  (func (export "promote") (param i32) (result i32)
    (i32.wrap_i64 (i64.shr_u
      (i64.reinterpret_f64 (f64.promote_f32 (f32.reinterpret_i32 (local.get 0))))
      (i64.const 32))))
  (func (export "demote") (param i32) (result i32)
    (i32.reinterpret_f32 (f32.demote_f64 (f64.reinterpret_i64
      (i64.or (i64.shl (i64.extend_i32_u (local.get 0)) (i64.const 32)) (i64.const 1))))))
  (func (export "promote-low") (param i32) (result i32)
    (i32.wrap_i64 (i64.shr_u (i64x2.extract_lane 0
      (f64x2.promote_low_f32x4 (i32x4.splat (local.get 0)))) (i64.const 32))))
  (func (export "demote-zero") (param i32) (result i32)
    (i32x4.extract_lane 0 (f32x4.demote_f64x2_zero (i64x2.splat
      (i64.or (i64.shl (i64.extend_i32_u (local.get 0)) (i64.const 32)) (i64.const 1))))))
)
