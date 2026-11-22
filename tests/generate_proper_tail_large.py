#!/usr/bin/env python3
"""Generate a correctness fixture whose native argument area exceeds 4095 bytes."""
import pathlib
import sys

count = 300
params = " ".join("i64" for _ in range(count)) + " i32"
args = " ".join(f"(i64.const {0x10203040 + i})" for i in range(count))
checks = "\n".join(
    f"(if (i64.ne (local.get {i}) (i64.const {0x10203040 + i})) (then unreachable))"
    for i in range(count)
)
text = f"""(module
  (import "env" "host" (func (param i64 i32) (result i64)))
  (import "env" "sample" (func $sample))
  (memory 1)
  (func $large_leaf (param {params}) (result i64)
    {checks}
    (i64.const 0x123456789abcdef))
  (func $large_tail (param {params}) (result i64)
    {checks}
    (return_call $small (local.get {count})))
  (func $small (param $n i32) (result i64)
    (call $sample)
    (if (result i64) (i32.eqz (local.get $n))
      (then (i64.const 0x123456789abcdef))
      (else (return_call $large_tail {args} (i32.sub (local.get $n) (i32.const 1))))))
  (func (export "ordinary_large") (result i64)
    (i64.add (i64.const 101) (call $large_leaf {args} (i32.const 0))))
  (func (export "cycle_large") (param i32) (result i64)
    (i64.add (i64.const 101) (call $small (local.get 0))))
)"""
pathlib.Path(sys.argv[1]).write_text(text + "\n")

if len(sys.argv) > 2:
    # Exercises >4095-byte import packing and >32760-byte stack load offsets.
    imports = 2000
    import_params = " ".join("i64" for _ in range(imports))
    import_args = " ".join(f"(i64.const {0x10203040 + i})" for i in range(imports))
    pathlib.Path(sys.argv[2]).write_text(f"""(module
      (import "env" "large" (func $large (param {import_params}) (result i64)))
      (memory 1)
      (func (export "ordinary_host") (result i64)
        (i64.add (i64.const 101) (call $large {import_args})))
      (func (export "tail_host") (result i64)
        (return_call $large {import_args}))
    )\n""")
