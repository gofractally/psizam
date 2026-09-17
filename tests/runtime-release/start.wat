(module
  (memory (export "memory") 1)
  (func $init
    i32.const 0
    i32.const 0
    i32.load
    i32.const 1
    i32.add
    i32.store)
  (start $init)
  (func (export "_start")
    i32.const 4
    i32.const 42
    i32.store))
