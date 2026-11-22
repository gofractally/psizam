(module
  (memory 1)
  (func (export "f32_bits") (param i32) (result i32)
    local.get 0 f32.reinterpret_i32 f32.const 1 f32.add i32.reinterpret_f32)
  (func (export "f64_bits") (param i64) (result i64)
    local.get 0 f64.reinterpret_i64 f64.const 1 f64.add i64.reinterpret_f64)
  (func (export "load") (param i32) (result i32)
    i32.const 0 i32.const 42 i32.store8
    local.get 0 i32.load8_u)
  (func (export "neighbor") (result i32) i32.const 7)
)
