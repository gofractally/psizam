(module
  (func (export "unary") (param v128) (result v128)
    local.get 0 f32x4.abs)
  (func (export "binary") (param v128 v128) (result v128)
    local.get 0 local.get 1 f32x4.add)
  (func (export "ternary") (param v128 v128 v128) (result v128)
    local.get 0 local.get 1 local.get 2 f32x4.relaxed_madd))
