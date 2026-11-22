;; CLI correctness smoke, not a performance workload. Ordinary Wasm traps at
;; the store; memory16 wraps the dynamic address and must observe the write.
(module
  (memory (export "memory") 1 1)
  (func (export "_start")
    i32.const 65536 i32.const 123 i32.store8
    i32.const 0 i32.load8_u i32.const 123 i32.ne
    if unreachable end))
