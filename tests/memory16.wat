(module
  (memory (export "memory") 1 1)
  (data (i32.const 0) "\2a\11\22\33")
  (data $passive "\77\88\99\aa")
  (func (export "read") (param i32) (result i32)
    local.get 0 i32.load8_u)
  (func (export "word") (param i32) (result i32)
    local.get 0 i32.load)
  (func (export "offset") (param i32) (result i32)
    local.get 0 i32.load8_u offset=65535)
  (func (export "add") (param i32) (result i32)
    local.get 0 i32.const 1 i32.add i32.load8_u)
  (func (export "add_offset") (param i32) (result i32)
    local.get 0 i32.const 1 i32.add i32.load8_u offset=1)
  (func (export "large_add") (param i32) (result i32)
    local.get 0 i32.const 65536 i32.add i32.load8_u)
  (func (export "live_address") (param i32) (result i32)
    local.get 0 i32.load8_u local.get 0 i32.add)
  (func (export "adjacent_loads") (param i32) (result i32)
    local.get 0 i32.load
    local.get 0 i32.const 4 i32.add i32.load i32.xor)
  (func (export "store") (param i32 i32)
    local.get 0 local.get 1 i32.store)
  (func (export "store_offset") (param i32 i32)
    local.get 0 local.get 1 i32.store offset=65535)
  (func (export "wide") (param i32) (result i32)
    local.get 0 v128.load offset=65535 i32x4.extract_lane 3)
  (func (export "wide_store") (param i32 i32)
    local.get 0 local.get 1 i32x4.splat v128.store offset=65535)
  (func (export "lane") (param i32) (result i32)
    local.get 0 v128.const i32x4 0 0 0 0 v128.load32_lane offset=65535 3
    i32x4.extract_lane 3)
  (func (export "atomic_load") (param i32) (result i32)
    local.get 0 i32.atomic.load offset=65535)
  (func (export "atomic_store") (param i32 i32)
    local.get 0 local.get 1 i32.atomic.store offset=65535)
  (func (export "atomic_add") (param i32) (result i32)
    local.get 0 i32.const 7 i32.atomic.rmw.add offset=65535)
  (func (export "atomic_cmpxchg") (param i32) (result i32)
    local.get 0 i32.const 16 i32.const 23 i32.atomic.rmw.cmpxchg offset=65535)
  (func (export "atomic64") (param i32 i64) (result i64)
    local.get 0 local.get 1 i64.atomic.store offset=65535
    local.get 0 i64.atomic.load offset=65535)
  (func (export "fence_notify") (param i32) (result i32)
    atomic.fence local.get 0 i32.const 1 memory.atomic.notify)
  (func (export "wait") (param i32) (result i32)
    local.get 0 i32.const 0 i64.const 0 memory.atomic.wait32)
  (func (export "fill") (param i32 i32)
    local.get 0 i32.const 77 local.get 1 memory.fill)
  (func (export "copy") (param i32 i32)
    local.get 0 i32.const 0 local.get 1 memory.copy)
  (func (export "copy_from") (param i32 i32 i32)
    local.get 0 local.get 1 local.get 2 memory.copy)
  (func (export "init") (param i32 i32 i32)
    local.get 0 local.get 1 local.get 2 memory.init $passive)
  (func (export "drop_data") data.drop $passive)
  (func (export "size") (result i32) memory.size)
  (func (export "grow") (param i32) (result i32) local.get 0 memory.grow)
  ;; Write every logical and padding byte. This independently distinguishes
  ;; contiguous v1 padding from per-byte ring wrapping and stale instance data.
  (func (export "seed_all") (local $i i32)
    (loop $logical
      local.get $i
      local.get $i i32.const 17 i32.mul i32.const 23 i32.add
      i32.store8
      local.get $i i32.const 1 i32.add local.tee $i
      i32.const 65536 i32.lt_u br_if $logical)
    i32.const 0 local.set $i
    (loop $padding
      local.get $i
      local.get $i i32.const 29 i32.mul i32.const 7 i32.add
      i32.store8 offset=65535
      local.get $i i32.const 1 i32.add local.tee $i
      i32.const 65536 i32.lt_u br_if $padding)
    i32.const 65535
    v128.const i8x16 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
    v128.store offset=65535)

)
