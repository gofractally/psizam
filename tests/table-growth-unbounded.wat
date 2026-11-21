(module
  (type $value (func (result i32)))
  (table 1 funcref)
  (elem (i32.const 0) $target)
  (func $target (type $value) i32.const 42)
  (func (export "run") (result i32) i32.const 0 call_indirect (type $value))
  (func (export "grow") (result i32) ref.null func i32.const 1 table.grow)
  (func (export "size") (result i32) table.size))
