#!/usr/bin/env python3
"""Keep trap sites beyond ARM64's 1 MiB conditional-branch range."""
from pathlib import Path
import subprocess
import sys

out = Path(sys.argv[1])
wat2wasm = sys.argv[2]
out.mkdir(parents=True, exist_ok=True)
# Stores cannot be removed by IR DCE. Many modest functions also avoid testing
# per-function resource limits instead of handler addressing.
padding = '(func ' + 'i32.const 0 i32.const 1 i32.store\n' * 1024 + ')\n'
prefix = '(module (memory 1) (table 1 funcref)\n' + padding * 160
bodies = {
    'far-stack': '(func $recurse call $recurse) (func (export "_start") call $recurse)',
    'far-div32': '(func $div (param i32) (result i32) i32.const 42 local.get 0 i32.div_s) (func (export "_start") i32.const 0 call $div drop)',
    'far-div64': '(func $div (param i64) (result i64) i64.const 42 local.get 0 i64.rem_u) (func (export "_start") i64.const 0 call $div drop)',
}
for name, body in bodies.items():
    wat = out / (name + '.wat')
    wat.write_text(prefix + body + ')\n')
    subprocess.run([wat2wasm, str(wat), '-o', str(out / (name + '.wasm'))], check=True)
