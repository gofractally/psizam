#!/usr/bin/env python3
"""Generates the float-parity fixture for the wasmtime backend tests.

Each export runs one float or SIMD operation over every pair of edge-case
seeds (signed zeros, subnormals, infinities, quiet and signalling NaNs with
payloads, rounding boundaries) and returns an FNV-style hash of every result
bit. Two engines agree on an operation exactly when its hash matches.

usage: gen_float_parity.py <out.wat> <out_names.hpp>
"""
import sys
import struct
f32 = [0x00000000,0x80000000,0x3f800000,0xbf800000,0x00000001,0x807fffff,0x7f800000,0xff800000,
       0x7fc00000,0xffc00000,0x7fa00001,0xffa12345,0x7fe00002,0x3f000000,0xbfc00000,0x40200000,
       0x4f000000,0xcf000001,0x5f000000,0x7f7fffff,0x3f800001,0x33800000,0x4b000001,0xcb7fffff]
def widen(b):
    return struct.unpack('<Q', struct.pack('<d', struct.unpack('<f', struct.pack('<I', b))[0]))[0]
f64 = [0,0x8000000000000000,0x3ff0000000000000,0xbff0000000000000,1,0x800fffffffffffff,
       0x7ff0000000000000,0xfff0000000000000,0x7ff8000000000000,0xfff8000000000000,
       0x7ff4000000000001,0xfff4123456789abc,0x7ffc000000000002,0x3fe0000000000000,
       0xbff8000000000000,0x4004000000000000,0x41e0000000000000,0xc1e0000000200000,
       0x43e0000000000000,0x7fefffffffffffff,0x3ff0000000000001,0x3ca0000000000000,
       0x4330000000000001,0xc32fffffffffffff]
N=len(f32); assert len(f64)==N
data = b''.join(struct.pack('<I',x) for x in f32) + b''.join(struct.pack('<Q',x) for x in f64)
F32,F64=0,4*N
out=['(module (memory (export "memory") 1)',
     '(data (i32.const 0) "%s")' % ''.join('\\%02x'%b for b in data)]
def s32(i): return f'(f32.load (i32.add (i32.const {F32}) (i32.shl (i32.rem_u {i} (i32.const {N})) (i32.const 2))))'
def s64(i): return f'(f64.load (i32.add (i32.const {F64}) (i32.shl (i32.rem_u {i} (i32.const {N})) (i32.const 3))))'
def v32(i): return f'(f32x4.replace_lane 3 (f32x4.replace_lane 2 (f32x4.replace_lane 1 (f32x4.splat {s32(i)}) {s32(f"(i32.add {i} (i32.const 1))")}) {s32(f"(i32.add {i} (i32.const 5))")}) {s32(f"(i32.add {i} (i32.const 11))")})'
def v64(i): return f'(f64x2.replace_lane 1 (f64x2.splat {s64(i)}) {s64(f"(i32.add {i} (i32.const 7))")})'
I,J,K='(local.get $i)','(local.get $j)','(i32.add (local.get $i) (i32.mul (local.get $j) (i32.const 3)))'
MIX='(local.set $h (i64.mul (i64.xor (local.get $h) %s) (i64.const 0x100000001b3)))'
def mix(kind, e):
    if kind=='f32': return MIX % f'(i64.extend_i32_u (i32.reinterpret_f32 {e}))'
    if kind=='f64': return MIX % f'(i64.reinterpret_f64 {e})'
    if kind=='i32': return MIX % f'(i64.extend_i32_u {e})'
    if kind=='i64': return MIX % e
    if kind=='v': return f'(local.set $v {e})' + MIX % '(i64x2.extract_lane 0 (local.get $v))' + MIX % '(i64x2.extract_lane 1 (local.get $v))'
names=[]
def fn(name, kind, e):
    names.append(name)
    out.append(f'''(func (export "{name}") (result i64) (local $i i32) (local $j i32) (local $h i64) (local $v v128)
 (local.set $h (i64.const 0xcbf29ce484222325))
 (loop $li (local.set $j (i32.const 0))
  (loop $lj {mix(kind,e)}
   (local.set $j (i32.add (local.get $j) (i32.const 1))) (br_if $lj (i32.lt_u (local.get $j) (i32.const {N}))))
  (local.set $i (i32.add (local.get $i) (i32.const 1))) (br_if $li (i32.lt_u (local.get $i) (i32.const {N}))))
 (local.get $h))''')
for t,s in (('f32',s32),('f64',s64)):
    for op in 'add sub mul div min max copysign'.split(): fn(f'{t}_{op}',t,f'({t}.{op} {s(I)} {s(J)})')
    for op in 'abs neg sqrt ceil floor trunc nearest'.split(): fn(f'{t}_{op}',t,f'({t}.{op} {s(I)})')
    for it in ('i32','i64'):
        for sg in 'su': fn(f'{it}_trunc_sat_{t}_{sg}',it,f'({it}.trunc_sat_{t}_{sg} {s(I)})')
fn('f32_demote','f32',f'(f32.demote_f64 {s64(I)})'); fn('f64_promote','f64',f'(f64.promote_f32 {s32(I)})')
for t,v in (('f32x4',v32),('f64x2',v64)):
    for op in 'add sub mul div min max pmin pmax'.split(): fn(f'{t}_{op}','v',f'({t}.{op} {v(I)} {v(J)})')
    for op in 'abs neg sqrt ceil floor trunc nearest'.split(): fn(f'{t}_{op}','v',f'({t}.{op} {v(I)})')
    for op in ('relaxed_madd','relaxed_nmadd'): fn(f'{t}_{op}','v',f'({t}.{op} {v(I)} {v(J)} {v(K)})')
    for op in ('relaxed_min','relaxed_max'): fn(f'{t}_{op}','v',f'({t}.{op} {v(I)} {v(J)})')
fn('f32x4_demote','v',f'(f32x4.demote_f64x2_zero {v64(I)})'); fn('f64x2_promote','v',f'(f64x2.promote_low_f32x4 {v32(I)})')
for op in ('trunc_sat_f32x4_s','trunc_sat_f32x4_u','relaxed_trunc_f32x4_s','relaxed_trunc_f32x4_u'): fn(f'i32x4_{op}','v',f'(i32x4.{op} {v32(I)})')
for op in ('trunc_sat_f64x2_s_zero','trunc_sat_f64x2_u_zero','relaxed_trunc_f64x2_s_zero','relaxed_trunc_f64x2_u_zero'): fn(f'i32x4_{op}','v',f'(i32x4.{op} {v64(I)})')
fn('relaxed_swizzle','v',f'(i8x16.relaxed_swizzle {v64(I)} {v64(J)})')
fn('relaxed_laneselect8','v',f'(i8x16.relaxed_laneselect {v64(I)} {v64(J)} {v64(K)})')
fn('relaxed_laneselect16','v',f'(i16x8.relaxed_laneselect {v64(I)} {v64(J)} {v64(K)})')
fn('relaxed_laneselect32','v',f'(i32x4.relaxed_laneselect {v64(I)} {v64(J)} {v64(K)})')
fn('relaxed_laneselect64','v',f'(i64x2.relaxed_laneselect {v64(I)} {v64(J)} {v64(K)})')
fn('relaxed_q15mulr','v',f'(i16x8.relaxed_q15mulr_s {v64(I)} {v64(J)})')
fn('relaxed_dot16','v',f'(i16x8.relaxed_dot_i8x16_i7x16_s {v64(I)} {v64(J)})')
fn('relaxed_dot32_add','v',f'(i32x4.relaxed_dot_i8x16_i7x16_add_s {v64(I)} {v64(J)} {v64(K)})')
out.append(')')
open(sys.argv[1], 'w').write('\n'.join(out))
with open(sys.argv[2], 'w') as f:
    f.write('// Generated by gen_float_parity.py. Do not edit.\n')
    f.write('inline constexpr const char* float_parity_ops[] = {\n')
    for n in names:
        f.write(f'   "{n}",\n')
    f.write('};\n')
