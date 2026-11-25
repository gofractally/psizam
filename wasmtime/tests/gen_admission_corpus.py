#!/usr/bin/env python3
"""Generates small WebAssembly modules that look for "acceptance splits":
modules psizam's validator admits and wasmtime refuses to compile.

    gen_admission_corpus.py --seed 1 --count 0 --out DIR
    psizam_wasmtime_differential_fuzz --admission-only DIR/*.wasm

Every module comes from a correct binary encoder and is valid except for ONE
"twist" taken from a catalogue (some twists are themselves valid: the catalogue
also sweeps things an engine may or may not support). The twist is named in the
file name (NNNNN__family__name.wasm) and in a custom section called "twist", so
a split says what caused it. Every module exports two empty functions,
`start` (i64)->() and `called` (i64,i64)->(), unless the twist is about the
header or the sections themselves.

--count 0 writes the whole catalogue once, in catalogue order. --count N picks
N entries at random. The seed also decides the incidental choices (constants,
sizes, whether an unused type is present); it never changes which twist a
catalogue entry is. --list prints the catalogue. Standard library only.
"""

import argparse
import os
import random
import re
import struct

# ----------------------------------------------------------------- encoding


def u(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def s(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if (n == 0 and not b & 0x40) or (n == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def pad(n, width):
    """Unsigned LEB128 of n in exactly `width` bytes (non-minimal)."""
    out = bytearray()
    for i in range(width):
        b = (n >> (7 * i)) & 0x7F
        out.append(b | (0x80 if i < width - 1 else 0))
    return bytes(out)


def spad(n, width):
    """Signed LEB128 of n sign-extended to exactly `width` bytes."""
    out = bytearray()
    for i in range(width):
        b = (n >> (7 * i)) & 0x7F
        out.append(b | (0x80 if i < width - 1 else 0))
    return bytes(out)


def vec(items):
    return u(len(items)) + b"".join(items)


def name(text):
    raw = text if isinstance(text, bytes) else text.encode()
    return u(len(raw)) + raw


def B(*xs):
    return bytes(xs)


I32, I64, F32, F64, V128, FUNCREF, EXTERNREF = (B(x) for x in (0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x70, 0x6F))
END, UNREACHABLE, DROP, NOP = B(0x0B), B(0x00), B(0x1A), B(0x01)
C0 = B(0x41, 0x00)  # i32.const 0


def ft(params, results):
    return B(0x60) + vec(params) + vec(results)


def body(code=b"", locals_=()):
    return vec([u(n) + t for n, t in locals_]) + code + END


def limits(flags, lo=1, hi=2, page=None):
    out = B(flags) + u(lo)
    if flags & 1:
        out += u(hi)
    if page is not None:
        out += u(page)
    return out


def heap(tb):
    """The heap type inside a reference type's bytes."""
    return tb[1:] if len(tb) > 1 else tb


def default_init(tb):
    """A constant of type `tb`, or ref.null of it when it is not a number."""
    if tb == I32:
        return B(0x41, 0)
    if tb == I64:
        return B(0x42, 0)
    if tb == F32:
        return B(0x43) + bytes(4)
    if tb == F64:
        return B(0x44) + bytes(8)
    if tb == V128:
        return B(0xFD, 0x0C) + bytes(16)
    return B(0xD0) + heap(tb)


class Mod:
    """One module. Function indices 0 and 1 (after imports) are start/called."""

    def __init__(self, rng):
        self.r = rng
        self.types = [ft([I64], []), ft([I64, I64], []), ft([], [])]
        if rng.random() < 0.5:  # an unused type; twists never hard-code indices above 2
            pick = lambda: [rng.choice([I32, I64, F32, F64]) for _ in range(rng.randrange(3))]
            self.types.append(ft(pick(), pick()[:1]))
        self.imports = []  # (kind, raw)
        self.funcs = []  # [typeidx bytes, body, size override]
        self.tables, self.mems, self.tags, self.globals = [], [], [], []
        self.exports = []
        self.start = None
        self.elems, self.datas = [], []
        self.datacount = None
        self.counts = {}  # section id -> function(n) -> bytes for the count
        self.post = None  # function(list of [id, payload]) -> list
        self.raw = None  # function(bytes) -> bytes
        self.base_exports = True

    # -- indices
    def nimp(self, kind=0):
        return sum(1 for k, _ in self.imports if k == kind)

    def ty(self, params, results):
        self.types.append(ft(params, results))
        return len(self.types) - 1

    def tyraw(self, raw):
        self.types.append(raw)
        return len(self.types) - 1

    def fn(self, tidx, code, size=None):
        self.funcs.append([u(tidx) if isinstance(tidx, int) else tidx, code, size])
        return self.nimp(0) + 1 + len(self.funcs)

    def imp(self, kind, desc, mod="env", field=None):
        assert not self.funcs or kind != 0, "import functions before defining any"
        self.imports.append((kind, name(mod) + name(field or "i%d" % len(self.imports)) + B(kind) + desc))
        return self.nimp(kind) - 1

    def small(self):
        return self.r.choice([0, 1, 2, 7, 42, 127, 128, 255, 65535, -1])

    def c32(self):
        return B(0x41) + s(self.small())

    # -- common furniture
    def mem(self, n=1):
        while len(self.mems) + self.nimp(2) < n:
            self.mems.append(limits(1, self.r.randrange(1, 4), 16))
        return self

    def table(self, n=1, tb=FUNCREF):
        while len(self.tables) + self.nimp(1) < n:
            self.tables.append(tb + limits(1, self.r.randrange(1, 8), 16))
        return self

    def glob(self, tb=I32, mut=1, init=None):
        self.globals.append(tb + B(mut) + (init if init is not None else default_init(tb)) + END)
        return self.nimp(3) + len(self.globals) - 1

    def export(self, text, kind, idx):
        self.exports.append(name(text) + B(kind) + u(idx))

    def helper(self):
        """An extra ()->() function, declared for ref.func by a passive segment."""
        h = self.fn(2, body())
        self.elems.append(B(1, 0) + vec([u(h)]))
        return h

    def rich(self):
        """Everything a single instruction might refer to with index 0."""
        self.mem().table()
        self.glob(I32, 1)
        h = self.helper()
        self.datas.append(B(1) + vec([B(self.r.randrange(256))]))
        self.datacount = len(self.datas)
        return h

    # -- output
    def sections(self):
        n0 = self.nimp(0)
        funcs = [[u(0), body(), None], [u(1), body(), None]] + self.funcs
        exports = []
        if self.base_exports:
            exports = [name("start") + B(0) + u(n0), name("called") + B(0) + u(n0 + 1)]
        exports += self.exports

        def sec(i, items):
            count = self.counts[i](len(items)) if i in self.counts else u(len(items))
            return [i, count + b"".join(items)]

        out = [sec(1, self.types)]
        if self.imports:
            out.append(sec(2, [raw for _, raw in self.imports]))
        out.append(sec(3, [f[0] for f in funcs]))
        if self.tables:
            out.append(sec(4, self.tables))
        if self.mems:
            out.append(sec(5, self.mems))
        if self.tags:
            out.append(sec(13, self.tags))
        if self.globals:
            out.append(sec(6, self.globals))
        if exports:
            out.append(sec(7, exports))
        if self.start is not None:
            out.append([8, u(self.start)])
        if self.elems:
            out.append(sec(9, self.elems))
        if self.datacount is not None:
            out.append([12, u(self.datacount)])
        out.append(sec(10, [(u(len(f[1])) if f[2] is None else f[2]) + f[1] for f in funcs]))
        if self.datas:
            out.append(sec(11, self.datas))
        return out


def serialize(secs):
    out = b"\0asm\1\0\0\0"
    for sec in secs:
        size = sec[2] if len(sec) > 2 else u(len(sec[1]))
        out += B(sec[0]) + size + sec[1]
    return out


def custom(text, payload=b""):
    return [0, name(text) + payload]


# ---------------------------------------------------------------- catalogue

CAT = []  # (family, name, function(Mod))


def add(family, text, f):
    CAT.append((family, text, f))


def code(m, instrs, locals_=(), tidx=2):
    return m.fn(tidx, body(instrs, locals_))


# ---- 0. controls: plain valid modules, which must be admitted and compile

add("control", "base", lambda m: None)
add("control", "rich", lambda m: m.rich() and None)
add("control", "rich-used", lambda m: code(m, B(0x10) + u(m.rich()) + B(0x41, 0, 0x28, 2, 0, 0x1A)))

# ---- 1. value types in every position a type can appear


def p_param(m, tb):
    m.fn(m.tyraw(B(0x60) + vec([tb]) + vec([])), body())


def p_result(m, tb):
    m.fn(m.tyraw(B(0x60) + vec([]) + vec([tb])), body(UNREACHABLE))


def p_local(m, tb):
    code(m, b"", [(1, tb)])


def p_global(m, tb):
    m.glob(tb, 0)


def p_global_mut(m, tb):
    m.glob(tb, 1)


def p_global_import(m, tb):
    m.imp(3, tb + B(0))


def p_block(op):
    def f(m, tb):
        if op == 4:
            code(m, C0 + B(4) + tb + UNREACHABLE + B(5) + UNREACHABLE + END + DROP)
        else:
            code(m, B(op) + tb + UNREACHABLE + END + DROP)

    return f


def p_block_empty(m, tb):
    code(m, B(2) + tb + END + UNREACHABLE)


def p_table(m, tb):
    m.tables.append(tb + limits(0, 1))


def p_table_import(m, tb):
    m.imp(1, tb + limits(0, 1))


def p_elem(m, tb):
    m.elems.append(B(5) + tb + vec([]))


def p_elem_item(m, tb):
    m.elems.append(B(5) + tb + vec([B(0xD0) + heap(tb) + END]))


def p_select(m, tb):
    code(m, UNREACHABLE + B(0x1C, 1) + tb + DROP)


def p_select_typed(m, tb):
    code(m, default_init(tb) * 2 + C0 + B(0x1C, 1) + tb + DROP)


def p_ref_null(m, tb):
    code(m, B(0xD0) + heap(tb) + DROP)


def p_ref_null_global(m, tb):
    m.globals.append(FUNCREF + B(0) + B(0xD0) + heap(tb) + END)


POSITIONS = [
    ("param", p_param), ("result", p_result), ("local", p_local), ("global", p_global),
    ("globalmut", p_global_mut), ("globalimport", p_global_import), ("block", p_block(2)),
    ("loop", p_block(3)), ("if", p_block(4)), ("blockempty", p_block_empty), ("table", p_table),
    ("tableimport", p_table_import), ("elem", p_elem), ("elemitem", p_elem_item),
    ("select", p_select), ("selecttyped", p_select_typed), ("refnull", p_ref_null),
    ("refnullglobal", p_ref_null_global),
]  # fmt: skip

for pos, f in POSITIONS:
    for b in range(0x40, 0x80):
        add("valtype", "%s-%02x" % (pos, b), lambda m, f=f, b=b: f(m, B(b)))

# The two-byte reference types (ref null? heaptype) of typed function references.
HEAPS = [(B(x), "%02x" % x) for x in (0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x71, 0x72, 0x73, 0x74, 0x40, 0x7F)]
HEAPS += [(B(0), "type0"), (B(2), "type2"), (B(0x3F), "type63"), (pad(0, 2), "type0padded"), (B(0x7B), "7b")]
for pos, f in POSITIONS:
    for lead in (0x63, 0x64):
        for hb, hn in HEAPS:
            add("reftype2", "%s-%02x-%s" % (pos, lead, hn), lambda m, f=f, lead=lead, hb=hb: f(m, B(lead) + hb))

# ---- 2. every single-byte opcode

SIG_RANGES = [
    (0x45, 0x45, "i", 1), (0x46, 0x4F, "ii", 1), (0x50, 0x50, "I", 1), (0x51, 0x5A, "II", 1),
    (0x5B, 0x60, "ff", 1), (0x61, 0x66, "FF", 1), (0x67, 0x69, "i", 1), (0x6A, 0x78, "ii", 1),
    (0x79, 0x7B, "I", 1), (0x7C, 0x8A, "II", 1), (0x8B, 0x91, "f", 1), (0x92, 0x98, "ff", 1),
    (0x99, 0x9F, "F", 1), (0xA0, 0xA6, "FF", 1), (0xA7, 0xA7, "I", 1), (0xA8, 0xA9, "f", 1),
    (0xAA, 0xAB, "F", 1), (0xAC, 0xAD, "i", 1), (0xAE, 0xAF, "f", 1), (0xB0, 0xB1, "F", 1),
    (0xB2, 0xB3, "i", 1), (0xB4, 0xB5, "I", 1), (0xB6, 0xB6, "F", 1), (0xB7, 0xB8, "i", 1),
    (0xB9, 0xBA, "I", 1), (0xBB, 0xBB, "f", 1), (0xBC, 0xBC, "f", 1), (0xBD, 0xBD, "F", 1),
    (0xBE, 0xBE, "i", 1), (0xBF, 0xBF, "I", 1), (0xC0, 0xC1, "i", 1), (0xC2, 0xC4, "I", 1),
    (0x28, 0x35, "i", 1), (0x36, 0x36, "ii", 0), (0x37, 0x37, "iI", 0), (0x38, 0x38, "if", 0),
    (0x39, 0x39, "iF", 0), (0x3A, 0x3B, "ii", 0), (0x3C, 0x3E, "iI", 0), (0x3F, 0x3F, "", 1),
    (0x40, 0x40, "i", 1), (0x41, 0x44, "", 1), (0x1A, 0x1A, "i", 0), (0x1B, 0x1B, "iii", 1),
    (0x1C, 0x1C, "iii", 1), (0x20, 0x20, "", 1), (0x21, 0x21, "i", 0), (0x22, 0x22, "i", 1),
    (0x23, 0x23, "", 1), (0x24, 0x24, "i", 0), (0x25, 0x25, "i", 1), (0x26, 0x26, "ir", 0),
    (0xD0, 0xD0, "", 1), (0xD1, 0xD1, "r", 1), (0xD2, 0xD2, "", 1), (0x0D, 0x0D, "i", 0),
    (0x0E, 0x0E, "i", 0), (0x10, 0x10, "", 0), (0x11, 0x11, "i", 0), (0x04, 0x04, "i", 0),
    (0x01, 0x03, "", 0), (0x0C, 0x0C, "", 0), (0x0F, 0x0F, "", 0),
]  # fmt: skip
PUSH = {"i": B(0x41, 0), "I": B(0x42, 0), "f": B(0x43) + bytes(4), "F": B(0x44) + bytes(8), "r": B(0xD0, 0x70)}


def sig(op):
    for lo, hi, params, results in SIG_RANGES:
        if lo <= op <= hi:
            return params, results
    return None


def op_imm(op, h):
    """Plausible minimal immediates of a single-byte opcode, and what closes it."""
    if op in (2, 3, 4, 6):
        return B(0x40), END
    if op == 0x1F:
        return B(0x40, 0), END
    if op in (7, 8, 9, 0x0C, 0x0D, 0x18, 0xD5, 0xD6):
        return B(0), b""
    if op == 0x0E:
        return B(0, 0), b""
    if op in (0x10, 0x12, 0xD2):
        return u(h), b""
    if op in (0x11, 0x13):
        return B(2, 0), b""
    if op in (0x14, 0x15):
        return B(2), b""
    if op == 0x1C:
        return B(1, 0x7F), b""
    if 0x20 <= op <= 0x26 or op in (0x3F, 0x40, 0x41, 0x42):
        return B(0), b""
    if 0x28 <= op <= 0x3E:
        return B(0, 0), b""
    if op == 0x43:
        return bytes(4), b""
    if op == 0x44:
        return bytes(8), b""
    if op == 0xD0:
        return B(0x70), b""
    if op == 0xFB:
        return B(0, 0), b""
    if op == 0xFC:
        return B(0), b""
    if op == 0xFD:
        return B(0, 0, 0), b""
    if op == 0xFE:
        return B(0, 2, 0), b""
    return b"", b""


def t_op(variant, op):
    def f(m):
        h = m.rich()
        imm, close = op_imm(op, h)
        ins = B(op) + imm + close
        if variant == "first":
            code(m, ins + UNREACHABLE, [(1, I32)])
        elif variant == "poly":
            code(m, UNREACHABLE + ins + UNREACHABLE, [(1, I32)])
        else:
            params, results = sig(op)
            tail = DROP * results
            if op in (0x0C, 0x0E, 0x0F):
                tail = b""
            code(m, b"".join(PUSH[p] for p in params) + ins + tail, [(1, I32)])

    return f


for op in range(256):
    add("op", "first-%02x" % op, t_op("first", op))
    add("op", "poly-%02x" % op, t_op("poly", op))
    if sig(op):
        add("op", "typed-%02x" % op, t_op("typed", op))

# ---- 3. every sub-opcode of the prefixes

MEMARG = B(0, 0)


def fb_imm(n):
    if n in (0, 1, 6, 7, 11, 12, 13, 14, 16):
        return B(0)
    if n in (2, 3, 4, 5, 8, 9, 10, 17, 18, 19):
        return B(0, 0)
    if n in (20, 21, 22, 23):
        return B(0x70)
    if n in (24, 25):
        return B(0, 0, 0x70, 0x70)
    return b""


def fc_imm(n):
    if n in (8, 10, 12, 14):
        return B(0, 0)
    if n in (9, 11, 13, 15, 16, 17, 18):
        return B(0)
    return b""


FC_SIG = {0: ("f", 1), 1: ("f", 1), 2: ("F", 1), 3: ("F", 1), 4: ("f", 1), 5: ("f", 1), 6: ("F", 1),
          7: ("F", 1), 8: ("iii", 0), 9: ("", 0), 10: ("iii", 0), 11: ("iii", 0), 12: ("iii", 0),
          13: ("", 0), 14: ("iii", 0), 15: ("ri", 1), 16: ("", 1), 17: ("iri", 0), 18: ("ii", 0),
          19: ("IIII", 2), 20: ("IIII", 2), 21: ("II", 2), 22: ("II", 2)}  # fmt: skip


def fd_imm(n):
    if n <= 11 or n in (92, 93):
        return MEMARG
    if n in (12, 13):
        return bytes(16)
    if 21 <= n <= 34:
        return B(0)
    if 84 <= n <= 91:
        return MEMARG + B(0)
    return b""


FE_ALIGN = {0: 2, 1: 2, 2: 3}
for i, a in enumerate([2, 3, 0, 1, 0, 1, 2, 2, 3, 0, 1, 0, 1, 2]):
    FE_ALIGN[0x10 + i] = a
for i in range(0x1E, 0x4F):
    FE_ALIGN[i] = [2, 3, 0, 1, 0, 1, 2][(i - 0x1E) % 7]


def fe_imm(n, natural=True):
    if n == 3:
        return B(0)
    if n in FE_ALIGN:
        return B(FE_ALIGN[n] if natural else 0, 0)
    return b""


def t_sub(prefix, n, imm, variant):
    def f(m):
        m.rich()
        m.elems.append(B(1, 0) + vec([]))
        ins = B(prefix) + u(n) + imm
        if variant == "first":
            code(m, ins + UNREACHABLE)
        elif variant == "poly":
            code(m, UNREACHABLE + ins + UNREACHABLE)
        else:
            params, results = FC_SIG[n]
            code(m, b"".join(PUSH[p] for p in params) + ins + DROP * results)

    return f


for n in range(41):
    for v in ("first", "poly"):
        add("gc", "%s-fb-%d" % (v, n), t_sub(0xFB, n, fb_imm(n), v))
for n in range(31):
    for v in ("first", "poly"):
        add("fc", "%s-fc-%d" % (v, n), t_sub(0xFC, n, fc_imm(n), v))
    if n in FC_SIG:
        add("fc", "typed-fc-%d" % n, t_sub(0xFC, n, fc_imm(n), "typed"))
for n in range(301):
    for v in ("first", "poly"):
        add("simd", "%s-fd-%d" % (v, n), t_sub(0xFD, n, fd_imm(n), v))
for n in range(0x50):
    for v in ("first", "poly"):
        add("atomics", "%s-fe-%d" % (v, n), t_sub(0xFE, n, fe_imm(n), v))
    if n in FE_ALIGN:
        add("atomics", "poly-align0-fe-%d" % n, t_sub(0xFE, n, fe_imm(n, False), "poly"))

# ---- 4. constant expressions in each position that takes one


def cx_global(tb=I32):
    def f(m, expr):
        m.globals.append(tb + B(0) + expr)

    return f


def cx_elem_offset(m, expr):
    m.table()
    m.elems.append(B(0) + expr + vec([]))


def cx_elem_item(m, expr):
    m.elems.append(B(5) + FUNCREF + vec([expr]))


def cx_data_offset(m, expr):
    m.mem()
    m.datas.append(B(0) + expr + vec([]))


def cx_table_init(m, expr):
    m.tables.append(B(0x40, 0) + FUNCREF + limits(0, 1) + expr)


CX_REF = [("global-funcref", cx_global(FUNCREF)), ("elemitem", cx_elem_item), ("tableinit", cx_table_init)]
CX_INT = [("global-i32", cx_global(I32)), ("elemoffset", cx_elem_offset), ("dataoffset", cx_data_offset)]
CX_ALL = CX_INT + CX_REF + [("global-externref", cx_global(EXTERNREF)), ("global-i64", cx_global(I64))]
NATURAL = {0x42: I64, 0x43: F32, 0x44: F64, 0xD0: FUNCREF, 0xD2: FUNCREF}


def cx_context(m):
    """Global 0 is an imported immutable i32; a helper function exists and is exported."""
    m.imp(3, I32 + B(0))
    h = m.fn(2, body())
    m.export("h", 0, h)
    return h


def t_cx_op(pos, put, op):
    def f(m):
        h = cx_context(m)
        imm, close = op_imm(op, h)
        if pos == "global":
            cx_global(NATURAL.get(op, I32))(m, B(op) + imm + close + END)
        else:
            put(m, B(op) + imm + close + END)

    return f


for pos, put in [("global", None), ("elemoffset", cx_elem_offset), ("elemitem", cx_elem_item),
                 ("dataoffset", cx_data_offset), ("tableinit", cx_table_init)]:  # fmt: skip
    for op in range(256):
        add("constop", "%s-%02x" % (pos, op), t_cx_op(pos, put, op))


def t_cx(put, make):
    def f(m):
        put(m, make(m))

    return f


def gg_imported(tb, mut):
    return lambda m: B(0x23) + u(m.imp(3, tb + B(mut))) + END


def gg_defined(tb, mut):
    return lambda m: B(0x23) + u(m.glob(tb, mut)) + END


def gg_forward(m):
    # the global being defined is the next one; the one after that is "forward"
    return B(0x23) + u(len(m.globals) + 1) + END


def rf_helper(m):
    return B(0xD2) + u(m.fn(2, body())) + END


def rf_imported(m):
    return B(0xD2) + u(m.imp(0, u(2))) + END


for pos, put in CX_ALL:
    for tn, tb in (("i32", I32), ("i64", I64), ("funcref", FUNCREF), ("externref", EXTERNREF)):
        for mut in (0, 1):
            add("constexpr", "%s-globalget-imported-%s-mut%d" % (pos, tn, mut), t_cx(put, gg_imported(tb, mut)))
            add("constexpr", "%s-globalget-defined-%s-mut%d" % (pos, tn, mut), t_cx(put, gg_defined(tb, mut)))
    add("constexpr", "%s-globalget-outofrange" % pos, t_cx(put, lambda m: B(0x23, 9) + END))
    add("constexpr", "%s-reffunc-defined" % pos, t_cx(put, rf_helper))
    add("constexpr", "%s-reffunc-start" % pos, t_cx(put, lambda m: B(0xD2, 0) + END))
    add("constexpr", "%s-reffunc-imported" % pos, t_cx(put, rf_imported))
    add("constexpr", "%s-reffunc-outofrange" % pos, t_cx(put, lambda m: B(0xD2, 9) + END))
    add("constexpr", "%s-reffunc-padded" % pos, t_cx(put, lambda m: B(0xD2) + pad(0, 3) + END))
    add("constexpr", "%s-empty" % pos, t_cx(put, lambda m: END))
    add("constexpr", "%s-two-values" % pos, t_cx(put, lambda m: C0 + C0 + END))
    add("constexpr", "%s-nop" % pos, t_cx(put, lambda m: NOP + C0 + END))
    add("constexpr", "%s-const-drop-const" % pos, t_cx(put, lambda m: C0 + DROP + C0 + END))
    add("constexpr", "%s-no-end" % pos, t_cx(put, lambda m: C0))
    add("constexpr", "%s-two-ends" % pos, t_cx(put, lambda m: C0 + END + END))
    add("constexpr", "%s-block" % pos, t_cx(put, lambda m: B(2, 0x7F) + C0 + END + END))
    add("constexpr", "%s-wrong-const-i64" % pos, t_cx(put, lambda m: B(0x42, 0) + END))
    add("constexpr", "%s-wrong-const-f32" % pos, t_cx(put, lambda m: B(0x43) + bytes(4) + END))
    add("constexpr", "%s-v128-const" % pos, t_cx(put, lambda m: B(0xFD, 0x0C) + bytes(16) + END))
    for b in range(0x40, 0x80):
        add("constexpr", "%s-refnull-%02x" % (pos, b), t_cx(put, lambda m, b=b: B(0xD0, b) + END))
    for hn, hb in (("type0", B(0)), ("type2", B(2)), ("type63", B(63)), ("type0padded", pad(0, 2)),
                   ("func-padded", B(0xF0, 0x7F)), ("minus1", B(0x7F)), ("two-byte-neg", B(0xBF, 0x7F))):  # fmt: skip
        add("constexpr", "%s-refnull-%s" % (pos, hn), t_cx(put, lambda m, hb=hb: B(0xD0) + hb + END))
add("constexpr", "global-globalget-self", lambda m: m.globals.append(I32 + B(0) + B(0x23, 0) + END))
add("constexpr", "global-globalget-forward",
    lambda m: (m.globals.append(I32 + B(0) + B(0x23, 1) + END), m.globals.append(I32 + B(0) + C0 + END)) and None)
add("constexpr", "global-v128", lambda m: m.globals.append(V128 + B(0) + B(0xFD, 0x0C) + bytes(16) + END))
add("constexpr", "global-mut-v128", lambda m: m.globals.append(V128 + B(1) + B(0xFD, 0x0C) + bytes(16) + END))

# Any numeric instruction applied to constants in a global initialiser.
RESULT = {"i": I32, "I": I64, "f": F32, "F": F64}


def result_type(op):
    for lo, hi, tb in ((0x45, 0x66, I32), (0x67, 0x78, I32), (0x79, 0x8A, I64), (0x8B, 0x98, F32),
                       (0x99, 0xA6, F64), (0xA7, 0xAB, I32), (0xAC, 0xB1, I64), (0xB2, 0xB6, F32),
                       (0xB7, 0xBB, F64), (0xBC, 0xBC, I32), (0xBD, 0xBD, I64), (0xBE, 0xBE, F32),
                       (0xBF, 0xBF, F64), (0xC0, 0xC1, I32), (0xC2, 0xC4, I64)):  # fmt: skip
        if lo <= op <= hi:
            return tb


for op in range(0x45, 0xC5):
    def f(m, op=op):
        m.globals.append(result_type(op) + B(0) + b"".join(PUSH[p] for p in sig(op)[0]) + B(op) + END)

    add("constnumeric", "global-%02x" % op, f)
for op in (0x6A, 0x6B, 0x6C):
    for pos, put in CX_INT[1:]:
        add("constnumeric", "%s-%02x" % (pos, op), t_cx(put, lambda m, op=op: C0 + C0 + B(op) + END))

# Prefixed instructions in a global initialiser.
for n in range(31):
    for tn, tb in (("i32", I32), ("funcref", FUNCREF), ("anyref", B(0x6E))):
        add("constprefix", "global-%s-fb-%d" % (tn, n),
            lambda m, n=n, tb=tb: m.globals.append(tb + B(0) + B(0xFB) + u(n) + fb_imm(n) + END))
        add("constprefix", "global-%s-i32-fb-%d" % (tn, n),
            lambda m, n=n, tb=tb: m.globals.append(tb + B(0) + C0 + B(0xFB) + u(n) + fb_imm(n) + END))
for n in range(23):
    def f(m, n=n):
        params, results = FC_SIG[n]
        m.rich()
        tb = I64 if n >= 19 or n in (4, 5, 6, 7) else I32
        m.globals.append(tb + B(0) + b"".join(PUSH[p] for p in params) + B(0xFC) + u(n) + fc_imm(n) + END)

    add("constprefix", "global-fc-%d" % n, f)
for n in list(range(0, 40)) + [0x4D, 0x53, 0x61, 0x6E, 0x8E, 0xAE, 0xE4, 0x100, 0x105]:
    add("constprefix", "global-v128-fd-%d" % n,
        lambda m, n=n: (m.mem(), m.globals.append(V128 + B(0) + B(0xFD, 0x0C) + bytes(16) + B(0xFD) + u(n) + fd_imm(n) + END)) and None)
for n in (0, 1, 2, 3, 0x10, 0x17, 0x1E):
    add("constprefix", "global-i32-fe-%d" % n,
        lambda m, n=n: (m.mem(), m.globals.append(I32 + B(0) + C0 + B(0xFE) + u(n) + fe_imm(n) + END)) and None)

# ---- 5. limits, memories and tables


def t_limits(kind, flags, page=None, imported=False, lo=1, hi=2, wide=False):
    def f(m):
        enc = limits(flags, lo, hi, page)
        if wide:  # a memory64/table64 minimum that only fits 64 bits
            enc = B(flags) + u(1 << 33) + (u(1 << 34) if flags & 1 else b"")
        if kind == "memory":
            m.imp(2, enc) if imported else m.mems.append(enc)
        else:
            m.imp(1, FUNCREF + enc) if imported else m.tables.append(FUNCREF + enc)

    return f


for kind in ("memory", "table"):
    for flags in list(range(0x20)) + [0x40, 0x80, 0xFF]:
        for imported in (False, True):
            tag = "%s%s-flags-%02x" % (kind, "-import" if imported else "", flags)
            add("limits", tag, t_limits(kind, flags, imported=imported))
            if flags & 8:
                for page in (0, 1, 12, 16):
                    add("limits", "%s-page%d" % (tag, page), t_limits(kind, flags, page, imported))
            if flags & 4 and flags < 0x10:
                add("limits", tag + "-wide", t_limits(kind, flags, imported=imported, wide=True))
    for tag, lo, hi, flags in (("min-gt-max", 3, 2, 1), ("min0", 0, 0, 0), ("min0-max0", 0, 0, 1),
                               ("min65536", 65536, 65536, 1), ("min65537", 65537, 65537, 0),
                               ("max65537", 1, 65537, 1), ("max-u32", 1, 0xFFFFFFFF, 1),
                               ("min-u32", 0xFFFFFFFF, 0, 0), ("min-1000000", 1000000, 0, 0),
                               ("min-over-u32", 1 << 32, 0, 0), ("max-over-u32", 1, 1 << 32, 1)):  # fmt: skip
        add("limits", "%s-%s" % (kind, tag), t_limits(kind, flags, lo=lo, hi=hi))
    for n in (0, 1, 2, 3):
        add("counts", "%s-count-%d" % (kind, n),
            lambda m, kind=kind, n=n: (m.mem(n) if kind == "memory" else m.table(n)) and None)
        add("counts", "%s-import-plus-%d" % (kind, n),
            lambda m, kind=kind, n=n: (m.imp(2, limits(1, 1, 4)), m.mem(n + 1)) and None if kind == "memory"
            else (m.imp(1, FUNCREF + limits(1, 1, 4)), m.table(n + 1)) and None)
    add("counts", "%s-two-imports" % kind,
        lambda m, kind=kind: [m.imp(2, limits(1, 1, 4)) if kind == "memory" else m.imp(1, FUNCREF + limits(1, 1, 4))
                              for _ in range(2)] and None)
for tb, tn in ((FUNCREF, "funcref"), (EXTERNREF, "externref")):
    add("limits", "table-init-%s" % tn,
        lambda m, tb=tb: m.tables.append(B(0x40, 0) + tb + limits(0, 1) + B(0xD0) + tb + END))
    add("limits", "table-init-bad-lead-%s" % tn,
        lambda m, tb=tb: m.tables.append(B(0x40, 1) + tb + limits(0, 1) + B(0xD0) + tb + END))

# Instructions that name a memory or a table, with index 0 and 1.


def t_mem_use(n_mem, imported, make):
    def f(m):
        if imported:
            m.imp(2, limits(1, 1, 4))
        m.mem(n_mem)
        m.datas.append(B(1) + vec([B(7)]))
        m.datacount = 1
        r = make(m)
        if r is not None:
            code(m, r)

    return f


I3 = C0 * 3
MEM_USES = []
for x in (0, 1):
    MEM_USES += [
        ("load-%d" % x, lambda m, x=x: C0 + B(0x28, 0x42, x, 0) + DROP),
        ("store-%d" % x, lambda m, x=x: C0 + C0 + B(0x36, 0x42, x, 0)),
        ("size-%d" % x, lambda m, x=x: B(0x3F, x) + DROP),
        ("grow-%d" % x, lambda m, x=x: C0 + B(0x40, x) + DROP),
        ("fill-%d" % x, lambda m, x=x: I3 + B(0xFC, 11, x)),
        ("init-%d" % x, lambda m, x=x: I3 + B(0xFC, 8, 0, x)),
        ("simd-load-%d" % x, lambda m, x=x: C0 + B(0xFD, 0, 0x44, x, 0) + DROP),
        ("atomic-load-%d" % x, lambda m, x=x: C0 + B(0xFE, 0x10, 0x42, x, 0) + DROP),
        ("data-active-%d" % x, lambda m, x=x: m.datas.append(B(2, x) + C0 + END + vec([B(1)])) or setattr(m, "datacount", 2)),
    ]
    for y in (0, 1):
        MEM_USES.append(("copy-%d-%d" % (x, y), lambda m, x=x, y=y: I3 + B(0xFC, 10, x, y)))
for n_mem, imported, tag in ((0, False, "nomem"), (1, False, "mem1"), (2, False, "mem2"), (2, True, "imp1def1"), (1, True, "imp1")):
    for un, make in MEM_USES:
        add("memindex", "%s-%s" % (tag, un), t_mem_use(n_mem, imported, make))


def t_table_use(n_tab, second, imported, make):
    def f(m):
        if imported:
            m.imp(1, FUNCREF + limits(1, 1, 4))
        m.table(min(n_tab, 1))
        m.table(n_tab, second)
        h = m.helper()
        r = make(m, h)
        if r is not None:
            code(m, r)

    return f


TABLE_USES = []
for x in (0, 1):
    TABLE_USES += [
        ("get-%d" % x, lambda m, h, x=x: C0 + B(0x25, x) + DROP),
        ("set-%d" % x, lambda m, h, x=x: C0 + B(0xD0, 0x70, 0x26, x)),
        ("set-extern-%d" % x, lambda m, h, x=x: C0 + B(0xD0, 0x6F, 0x26, x)),
        ("size-%d" % x, lambda m, h, x=x: B(0xFC, 16, x) + DROP),
        ("grow-%d" % x, lambda m, h, x=x: B(0xD0, 0x70) + C0 + B(0xFC, 15, x) + DROP),
        ("fill-%d" % x, lambda m, h, x=x: C0 + B(0xD0, 0x70) + C0 + B(0xFC, 17, x)),
        ("init-%d" % x, lambda m, h, x=x: I3 + B(0xFC, 12, 0, x)),
        ("call-indirect-%d" % x, lambda m, h, x=x: C0 + B(0x11, 2, x)),
        ("elem-active-flag2-%d" % x, lambda m, h, x=x: m.elems.append(B(2, x) + C0 + END + B(0) + vec([u(h)]))),
        ("elem-active-flag6-%d" % x, lambda m, h, x=x: m.elems.append(B(6, x) + C0 + END + FUNCREF + vec([B(0xD2) + u(h) + END]))),
    ]
    for y in (0, 1):
        TABLE_USES.append(("copy-%d-%d" % (x, y), lambda m, h, x=x, y=y: I3 + B(0xFC, 14, x, y)))
TABLE_USES.append(("elem-active-flag0", lambda m, h: m.elems.append(B(0) + C0 + END + vec([u(h)]))))
for n_tab, second, imported, tag in ((0, FUNCREF, False, "notable"), (1, FUNCREF, False, "tab1"),
                                     (2, FUNCREF, False, "tab2"), (2, EXTERNREF, False, "tab2extern"),
                                     (2, FUNCREF, True, "imp1def1")):  # fmt: skip
    for un, make in TABLE_USES:
        add("tableindex", "%s-%s" % (tag, un), t_table_use(n_tab, second, imported, make))
add("tableindex", "externref-only-call-indirect",
    lambda m: (m.table(1, EXTERNREF), code(m, C0 + B(0x11, 2, 0))) and None)
add("tableindex", "externref-only-elem-funcs",
    lambda m: (m.table(1, EXTERNREF), m.elems.append(B(0) + C0 + END + vec([u(0)]))) and None)

# ---- 6. element and data segments


def elem_segment(flag, items_as, h, tb=FUNCREF, kind=0):
    layout = flag & 7
    exprs = bool(layout & 4)
    if items_as == "empty":
        items = []
    elif exprs:
        items = [B(0xD2) + u(h) + END, B(0xD0) + heap(tb) + END] if items_as == "two" else [B(0xD2) + u(h) + END]
    else:
        items = [u(h), u(h)] if items_as == "two" else [u(h)]
    out = u(flag) if flag < 0x80 else B(flag)
    if layout in (2, 6):
        out += B(0)
    if not layout & 1:
        out += C0 + END
    if layout & 3:
        out += tb if exprs else B(kind)
    return out + vec(items)


def t_elem(flag, items_as, datacount, table=True):
    def f(m):
        if table:
            m.table()
        h = m.fn(2, body())
        m.elems.append(elem_segment(flag, items_as, h))
        if datacount:
            m.datacount = 0

    return f


for flag in list(range(16)) + [0x20, 0x40, 0x80]:
    for items_as in ("empty", "one", "two"):
        for dc in (False, True):
            add("elemseg", "flag-%02x-%s%s" % (flag, items_as, "-datacount" if dc else ""), t_elem(flag, items_as, dc))
    add("elemseg", "flag-%02x-notable" % flag, t_elem(flag, "one", False, table=False))
for kind in range(1, 5):
    for flag in (1, 2, 3):
        add("elemseg", "elemkind-%d-flag-%d" % (kind, flag),
            lambda m, kind=kind, flag=flag: (m.table(), m.elems.append(elem_segment(flag, "one", 0, kind=kind))) and None)
add("elemseg", "func-out-of-range", lambda m: (m.table(), m.elems.append(B(0) + C0 + END + vec([u(77)]))) and None)
add("elemseg", "passive-func-out-of-range", lambda m: m.elems.append(B(1, 0) + vec([u(77)])))
add("elemseg", "declared-func-out-of-range", lambda m: m.elems.append(B(3, 0) + vec([u(77)])))
add("elemseg", "externref-passive-null", lambda m: m.elems.append(B(5) + EXTERNREF + vec([B(0xD0, 0x6F, 0x0B)])))
add("elemseg", "externref-passive-reffunc", lambda m: m.elems.append(B(5) + EXTERNREF + vec([B(0xD2, 0, 0x0B)])))
add("elemseg", "externref-active-into-funcref-table",
    lambda m: (m.table(), m.elems.append(B(6, 0) + C0 + END + EXTERNREF + vec([B(0xD0, 0x6F, 0x0B)]))) and None)
add("elemseg", "offset-i64", lambda m: (m.table(), m.elems.append(B(0, 0x42, 0, 0x0B) + vec([]))) and None)
add("elemseg", "offset-beyond-table", lambda m: (m.table(), m.elems.append(B(0, 0x41) + s(1000) + END + vec([u(0)]))) and None)
add("elemseg", "many-segments", lambda m: [m.elems.append(B(1, 0) + vec([u(0)])) for _ in range(300)] and None)


def data_segment(flag, payload=b"\x2a"):
    layout = flag & 3
    out = u(flag) if flag < 0x80 else B(flag)
    if layout in (2, 3):
        out += B(0)
    if layout != 1:
        out += C0 + END
    return out + vec([B(x) for x in payload])


def t_data(flag, dc, use=None, memory=True):
    def f(m):
        if memory:
            m.mem()
        m.datas.append(data_segment(flag))
        if dc is not None:
            m.datacount = len(m.datas) + dc
        if use:
            code(m, use)

    return f


for flag in list(range(8)) + [0x40, 0x80]:
    for dc, dn in ((None, "nocount"), (0, "count"), (1, "count-too-many"), (-1, "count-too-few")):
        add("dataseg", "flag-%02x-%s" % (flag, dn), t_data(flag, dc))
    add("dataseg", "flag-%02x-nomemory" % flag, t_data(flag, 0, memory=False))
for dc, dn in ((None, "nocount"), (0, "count")):
    add("dataseg", "memory-init-%s" % dn, t_data(1, dc, I3 + B(0xFC, 8, 0, 0)))
    add("dataseg", "data-drop-%s" % dn, t_data(1, dc, B(0xFC, 9, 0)))
    add("dataseg", "memory-init-out-of-range-%s" % dn, t_data(1, dc, I3 + B(0xFC, 8, 5, 0)))
    add("dataseg", "data-drop-out-of-range-%s" % dn, t_data(1, dc, B(0xFC, 9, 5)))
add("dataseg", "count-without-data", lambda m: setattr(m, "datacount", 0))
add("dataseg", "count-1-without-data", lambda m: setattr(m, "datacount", 1))
add("dataseg", "offset-i64", lambda m: (m.mem(), m.datas.append(B(0, 0x42, 0, 0x0B) + vec([]))) and None)
add("dataseg", "offset-beyond-memory",
    lambda m: (m.mem(), m.datas.append(B(0, 0x41) + s(0x7FFFFFF0) + END + vec([B(1)]))) and None)
add("dataseg", "length-overruns", lambda m: (m.mem(), m.datas.append(B(0) + C0 + END + u(1000) + b"xy")) and None)
add("elemseg", "elem-drop", lambda m: (m.elems.append(B(1, 0) + vec([u(0)])), code(m, B(0xFC, 13, 0))) and None)
add("elemseg", "elem-drop-out-of-range", lambda m: code(m, B(0xFC, 13, 3)) and None)
add("elemseg", "table-init-out-of-range", lambda m: (m.table(), code(m, I3 + B(0xFC, 12, 3, 0))) and None)

# ---- 7. sections

SECTION_NAMES = {1: "type", 2: "import", 3: "function", 4: "table", 5: "memory", 6: "global",
                 7: "export", 8: "start", 9: "element", 10: "code", 11: "data", 12: "datacount", 13: "tag"}  # fmt: skip


def full(m):
    """A module with every standard section present (no tag section)."""
    m.imp(0, u(2))
    m.rich()
    m.start = m.nimp(0) + 2


def with_post(setup, post):
    def f(m):
        setup(m)
        m.post = post

    return f


def find(secs, i):
    return next(k for k, sec in enumerate(secs) if sec[0] == i)


def duplicate(i):
    def post(secs):
        k = find(secs, i)
        return secs[: k + 1] + [list(secs[k])] + secs[k + 1 :]

    return post


def duplicate_empty(i):
    def post(secs):
        k = find(secs, i)
        return secs[: k + 1] + [[i, B(0)]] + secs[k + 1 :]

    return post


def move_first(i):
    def post(secs):
        k = find(secs, i)
        return [secs[k]] + secs[:k] + secs[k + 1 :]

    return post


def swap_with_previous(i):
    def post(secs):
        k = find(secs, i)
        secs[k - 1], secs[k] = secs[k], secs[k - 1]
        return secs

    return post


def move_last(i):
    def post(secs):
        k = find(secs, i)
        return secs[:k] + secs[k + 1 :] + [secs[k]]

    return post


def insert_before(i, sec):
    def post(secs):
        k = find(secs, i)
        return secs[:k] + [list(sec)] + secs[k:]

    return post


def drop_section(i):
    return lambda secs: [sec for sec in secs if sec[0] != i]


def resize(i, delta):
    def post(secs):
        k = find(secs, i)
        secs[k] = [i, secs[k][1], u(len(secs[k][1]) + delta)]
        return secs

    return post


def empty_payload(i):
    def post(secs):
        k = find(secs, i)
        secs[k] = [i, b""]
        return secs

    return post


for i in range(1, 13):
    sn = SECTION_NAMES[i]
    add("sections", "duplicate-%s" % sn, with_post(full, duplicate(i)))
    add("sections", "duplicate-empty-%s" % sn, with_post(full, duplicate_empty(i)))
    add("sections", "swap-with-previous-%s" % sn, with_post(full, swap_with_previous(i))) if i != 1 else None
    add("sections", "move-first-%s" % sn, with_post(full, move_first(i))) if i != 1 else None
    add("sections", "move-last-%s" % sn, with_post(full, move_last(i))) if i != 11 else None
    add("sections", "missing-%s" % sn, with_post(full, drop_section(i)))
    add("sections", "size-plus-1-%s" % sn, with_post(full, resize(i, 1)))
    add("sections", "size-minus-1-%s" % sn, with_post(full, resize(i, -1)))
    add("sections", "zero-length-%s" % sn, with_post(full, empty_payload(i)))
    add("sections", "custom-before-%s" % sn, with_post(full, insert_before(i, custom("pad", b"\1\2\3"))))
    for extra in (4, 5, 13):
        if extra != i:
            add("sections", "empty-%s-before-%s" % (SECTION_NAMES[extra], sn),
                with_post(lambda m: None, insert_before(i, [extra, B(0)]))) if i in (1, 3, 7, 10) else None
for i in list(range(13, 32)) + [0x3F, 0x40, 0x7F, 0x80, 0xFF]:
    add("sections", "unknown-id-%02x-last" % i, with_post(full, lambda secs, i=i: secs + [[i, B(0)]]))
    add("sections", "unknown-id-%02x-first" % i, with_post(full, lambda secs, i=i: [[i, B(0)]] + secs))
    add("sections", "unknown-id-%02x-empty" % i, with_post(full, lambda secs, i=i: secs + [[i, b""]]))
for tag, payload in (("empty-name", name("")), ("no-name", b""), ("name-overruns", u(50) + b"ab"),
                     ("bad-utf8-name", name(b"\xff\xfe")), ("name-section-garbage", name("name") + b"\xff" * 9),
                     ("name-section-ok", name("name") + B(0, 2) + name("m")),
                     ("producers-garbage", name("producers") + b"\xff" * 5),
                     ("target-features-garbage", name("target_features") + b"\xff" * 5),
                     ("dylink-garbage", name("dylink.0") + b"\xff" * 5),
                     ("linking-garbage", name("linking") + b"\xff" * 5),
                     ("large", name("blob") + bytes(70000))):  # fmt: skip
    add("sections", "custom-%s-first" % tag, with_post(full, lambda secs, payload=payload: [[0, payload]] + secs))
    add("sections", "custom-%s-last" % tag, with_post(full, lambda secs, payload=payload: secs + [[0, payload]]))
add("sections", "function-count-more-than-code", lambda m: m.counts.__setitem__(3, lambda n: u(n + 1)))
add("sections", "code-count-more-than-functions", lambda m: m.counts.__setitem__(10, lambda n: u(n + 1)))
add("sections", "type-count-too-many", lambda m: m.counts.__setitem__(1, lambda n: u(n + 1)))
add("sections", "type-count-too-few", lambda m: m.counts.__setitem__(1, lambda n: u(n - 1)))
add("sections", "export-count-too-few", lambda m: m.counts.__setitem__(7, lambda n: u(n - 1)))
add("sections", "datacount-after-code", with_post(full, lambda secs: move_last(12)(secs)))
add("sections", "datacount-before-element", with_post(full, swap_with_previous(12)))
add("sections", "tag-after-global",
    with_post(lambda m: (m.glob(), m.tags.append(B(0, 2))) and None, lambda secs: swap_with_previous(6)(secs)))


def raw(f):
    def g(m):
        m.raw = f

    return g


add("header", "bad-magic", raw(lambda b: b"\0asn" + b[4:]))
add("header", "version-0", raw(lambda b: b[:4] + B(0, 0, 0, 0) + b[8:]))
add("header", "version-2", raw(lambda b: b[:4] + B(2, 0, 0, 0) + b[8:]))
add("header", "version-0d-pre-mvp", raw(lambda b: b[:4] + B(0x0D, 0, 0, 0) + b[8:]))
add("header", "version-component", raw(lambda b: b[:4] + B(0x0D, 0, 1, 0) + b[8:]))
add("header", "version-high-bytes", raw(lambda b: b[:4] + B(1, 0, 1, 0) + b[8:]))
add("header", "empty-file", raw(lambda b: b""))
add("header", "magic-only", raw(lambda b: b[:4]))
add("header", "truncated-version", raw(lambda b: b[:7]))
add("header", "header-only", raw(lambda b: b[:8]))
add("header", "trailing-zero-byte", raw(lambda b: b + B(0)))
add("header", "trailing-section-id-only", raw(lambda b: b + B(1)))
add("header", "trailing-garbage", raw(lambda b: b + b"\xde\xad\xbe\xef"))
add("header", "truncated-last-byte", raw(lambda b: b[:-1]))
add("header", "truncated-half", raw(lambda b: b[: len(b) // 2]))
add("header", "second-module-appended", raw(lambda b: b + b))

# ---- 8. start, exports, imports, types, tags


def t_start(params, results, imported):
    def f(m):
        if imported:
            t = m.ty(params, results)
            m.start = m.imp(0, u(t))
        else:
            t = m.ty(params, results)
            m.start = m.fn(t, body(UNREACHABLE))

    return f


for pn, params in (("none", []), ("i32", [I32]), ("i64", [I64]), ("i64-i64", [I64, I64]), ("funcref", [FUNCREF])):
    for rn, results in (("none", []), ("i32", [I32]), ("i64", [I64]), ("i32-i32", [I32, I32])):
        for imported in (False, True):
            add("start", "%s-%s-to-%s" % ("import" if imported else "defined", pn, rn), t_start(params, results, imported))
add("start", "the-exported-start", lambda m: setattr(m, "start", 0))
add("start", "called", lambda m: setattr(m, "start", 1))
add("start", "out-of-range", lambda m: setattr(m, "start", 50))
add("start", "padded-index", with_post(lambda m: setattr(m, "start", m.fn(2, body())), lambda secs: [
    [8, pad(2, 5)] if sec[0] == 8 else sec for sec in secs]))
add("start", "trailing-byte", with_post(lambda m: setattr(m, "start", m.fn(2, body())), lambda secs: [
    [8, sec[1] + B(0)] if sec[0] == 8 else sec for sec in secs]))


def t_export(kind, idx, present):
    def f(m):
        if present:
            m.rich()
            m.tags.append(B(0, 2)) if kind == 4 and present == "tag" else None
        m.export("x", kind, idx)

    return f


for kind in range(8):
    for idx in (0, 1, 9):
        add("export", "kind-%d-index-%d" % (kind, idx), t_export(kind, idx, True))
        add("export", "kind-%d-index-%d-bare" % (kind, idx), t_export(kind, idx, False))
add("export", "kind-4-with-tag-section", t_export(4, 0, "tag"))
add("export", "duplicate-name", lambda m: m.export("start", 0, 0))
add("export", "duplicate-new-name", lambda m: (m.export("x", 0, 0), m.export("x", 0, 1)) and None)
add("export", "same-function-twice", lambda m: (m.export("x", 0, 0), m.export("y", 0, 0)) and None)
add("export", "empty-name", lambda m: m.export("", 0, 0))
add("export", "bad-utf8-name", lambda m: m.export(b"\xff\xfe", 0, 0))
add("export", "overlong-utf8-name", lambda m: m.export(b"\xc0\x80", 0, 0))
add("export", "surrogate-utf8-name", lambda m: m.export(b"\xed\xa0\x80", 0, 0))
add("export", "nul-in-name", lambda m: m.export(b"a\0b", 0, 0))
add("export", "long-name", lambda m: m.export("n" * 5000, 0, 0))
add("export", "mutable-global", lambda m: m.export("g", 3, m.glob(I32, 1)))
add("export", "immutable-global", lambda m: m.export("g", 3, m.glob(I32, 0)))
add("export", "v128-global", lambda m: m.export("g", 3, m.glob(V128, 0)))
add("export", "funcref-global", lambda m: m.export("g", 3, m.glob(FUNCREF, 1)))
add("export", "imported-function", lambda m: m.export("f", 0, m.imp(0, u(2))))
add("export", "imported-global", lambda m: m.export("g", 3, m.imp(3, I32 + B(0))))
add("export", "imported-memory", lambda m: m.export("mm", 2, m.imp(2, limits(0, 1))))
add("export", "imported-table", lambda m: m.export("t", 1, m.imp(1, FUNCREF + limits(0, 1))))
add("export", "v128-function", lambda m: m.export("f", 0, m.fn(m.ty([V128], [V128]), body(B(0x20, 0)))))
add("export", "funcref-function", lambda m: m.export("f", 0, m.fn(m.ty([FUNCREF], [EXTERNREF]), body(B(0xD0, 0x6F)))))
add("export", "multi-value-function", lambda m: m.export("f", 0, m.fn(m.ty([], [I32, I64]), body(B(0x41, 0, 0x42, 0)))))

IMPORT_DESCS = [("func-type0", 0, u(0)), ("func-type2", 0, u(2)), ("func-type-out-of-range", 0, u(60)),
                ("table-funcref", 1, FUNCREF + limits(0, 1)), ("table-externref", 1, EXTERNREF + limits(1, 1, 2)),
                ("memory", 2, limits(0, 1)), ("memory-max", 2, limits(1, 1, 2)),
                ("tag-attr0-type2", 4, B(0, 2)), ("tag-attr0-type0", 4, B(0, 0)), ("tag-attr1-type2", 4, B(1, 2)),
                ("tag-type-out-of-range", 4, B(0, 60)), ("kind-5", 5, B(0)), ("kind-6", 6, B(0)),
                ("kind-7f", 0x7F, B(0)), ("kind-ff", 0xFF, B(0))]  # fmt: skip
for tn, tb in (("i32", I32), ("i64", I64), ("f32", F32), ("f64", F64), ("v128", V128), ("funcref", FUNCREF), ("externref", EXTERNREF)):
    for mut in range(4):
        IMPORT_DESCS.append(("global-%s-mut%d" % (tn, mut), 3, tb + B(mut)))
for tag, kind, desc in IMPORT_DESCS:
    add("import", tag, lambda m, kind=kind, desc=desc: m.imp(kind, desc) and None)
add("import", "empty-names", lambda m: m.imp(0, u(2), "", "") and None)
add("import", "bad-utf8-module", lambda m: m.imp(0, u(2), b"\xff", "f") and None)
add("import", "bad-utf8-field", lambda m: m.imp(0, u(2), "env", b"\xff") and None)
add("import", "duplicate-same-type", lambda m: (m.imp(0, u(2), "env", "f"), m.imp(0, u(2), "env", "f")) and None)
add("import", "duplicate-other-type", lambda m: (m.imp(0, u(2), "env", "f"), m.imp(0, u(0), "env", "f")) and None)
add("import", "duplicate-other-kind", lambda m: (m.imp(0, u(2), "env", "f"), m.imp(3, I32 + B(0), "env", "f")) and None)
add("import", "many-functions", lambda m: [m.imp(0, u(2)) for _ in range(300)] and None)
add("import", "call-imported", lambda m: code(m, B(0x10) + u(m.imp(0, u(2)))) and None)
add("import", "global-get-imported", lambda m: code(m, B(0x23) + u(m.imp(3, I64 + B(0))) + DROP) and None)
add("import", "global-set-imported-mutable", lambda m: code(m, B(0x42, 0, 0x24) + u(m.imp(3, I64 + B(1)))) and None)
add("import", "global-set-imported-immutable", lambda m: code(m, B(0x42, 0, 0x24) + u(m.imp(3, I64 + B(0)))) and None)

for b in range(0x40, 0x80):
    add("typeform", "form-%02x-func-shape" % b, lambda m, b=b: m.tyraw(B(b, 0, 0)) and None)
    add("typeform", "form-%02x-one-byte" % b, lambda m, b=b: m.tyraw(B(b, 0)) and None)
for tag, enc in (("struct-empty", B(0x5F, 0)), ("struct-i32", B(0x5F, 1, 0x7F, 0)), ("array-i32", B(0x5E, 0x7F, 0)),
                 ("array-i8", B(0x5E, 0x78, 1)), ("rec-empty", B(0x4E, 0)), ("rec-one-func", B(0x4E, 1, 0x60, 0, 0)),
                 ("sub-func", B(0x50, 0, 0x60, 0, 0)), ("sub-final-func", B(0x4F, 0, 0x60, 0, 0)),
                 ("sub-with-super", B(0x50, 1, 2, 0x60, 0, 0)), ("shared-func", B(0x65, 0x60, 0, 0)),
                 ("cont", B(0x5D, 2)), ("func-three-vectors", B(0x60, 0, 0, 0)),
                 ("func-many-results", ft([], [I32] * 20)), ("func-many-params", ft([I64] * 300, [])),
                 ("func-1001-params", ft([I32] * 1001, [])), ("func-1001-results", ft([], [I32] * 1001)),
                 ("func-v128", ft([V128], [V128])), ("func-refs", ft([FUNCREF, EXTERNREF], [FUNCREF]))):  # fmt: skip
    add("typeform", tag, lambda m, enc=enc: m.tyraw(enc) and None)
add("typeform", "function-type-index-out-of-range", lambda m: m.fn(50, body()) and None)
add("typeform", "function-of-struct-type", lambda m: m.fn(m.tyraw(B(0x5F, 0)), body()) and None)
add("typeform", "many-types", lambda m: [m.ty([I32], []) for _ in range(2000)] and None)

for attr in range(4):
    for t, tn in ((2, "noargs"), (0, "i64"), (60, "out-of-range")):
        add("tag", "section-attr%d-%s" % (attr, tn), lambda m, attr=attr, t=t: m.tags.append(B(attr) + u(t)))
add("tag", "section-result-type", lambda m: m.tags.append(B(0) + u(m.ty([], [I32]))))
add("tag", "section-empty", with_post(lambda m: None, insert_before(7, [13, B(0)])))
# Exception handling instructions in a module that also has a tag (two things at once:
# only informative if a tag section alone is admitted).
EH = [("throw", B(8, 0)), ("legacy-try-catch", B(6, 0x40, 7, 0, 0x0B)), ("legacy-try-catch-all", B(6, 0x40, 0x19, 0x0B)),
      ("legacy-try-delegate", B(6, 0x40, 0x18, 0)), ("legacy-rethrow", B(6, 0x40, 0x19, 9, 0, 0x0B)),
      ("try-table-empty", B(0x1F, 0x40, 0, 0x0B)), ("try-table-catch", B(2, 0x40, 0x1F, 0x40, 1, 0, 0, 0, 0x0B, 0x0B)),
      ("try-table-catch-all", B(2, 0x40, 0x1F, 0x40, 1, 2, 0, 0x0B, 0x0B)),
      ("try-table-catch-ref", B(2, 0x69, 0x1F, 0x40, 1, 1, 0, 0, 0x0B, 0x00, 0x0B, 0x1A)),
      ("try-table-catch-all-ref", B(2, 0x69, 0x1F, 0x40, 1, 3, 0, 0x0B, 0x00, 0x0B, 0x1A)),
      ("throw-ref", B(0xD0, 0x69, 0x0A)), ("delegate-alone", B(0x18, 0)), ("catch-alone", B(7, 0)),
      ("catch-all-alone", B(0x19)), ("rethrow-alone", B(9, 0)),
      ("select-exnref", B(0xD0, 0x69, 0xD0, 0x69, 0x41, 0, 0x1C, 1, 0x69, 0x1A))]  # fmt: skip
for tag, ins in EH:
    add("eh", "tagged-" + tag, lambda m, ins=ins: (m.tags.append(B(0, 2)), code(m, ins)) and None)
    add("eh", "untagged-" + tag, lambda m, ins=ins: code(m, ins) and None)
    add("eh", "untagged-poly-" + tag, lambda m, ins=ins: code(m, UNREACHABLE + ins + UNREACHABLE) and None)

# ---- 9. block types, control flow, calls


def t_blocktype(op, params, results, bt=None, with_else=True):
    def f(m):
        t = m.ty(params, results)
        enc = bt(t) if bt else s(t)
        push = b"".join(default_init(p) for p in params)
        inner = DROP * len(params) + b"".join(default_init(r) for r in results)
        if op == 4:
            mid = (B(5) + inner) if with_else else b""
            code(m, push + C0 + B(4) + enc + inner + mid + END + DROP * len(results))
        else:
            code(m, push + B(op) + enc + inner + END + DROP * len(results))

    return f


SHAPES = [("none-none", [], []), ("none-i32", [], [I32]), ("i32-none", [I32], []), ("i32-i32", [I32], [I32]),
          ("i32i32-i32", [I32, I32], [I32]), ("none-i32i32", [], [I32, I32]), ("i64i64-i64i64", [I64, I64], [I64, I64]),
          ("f32f64-i32i64", [F32, F64], [I32, I64]), ("funcref-externref", [FUNCREF], [EXTERNREF]),
          ("v128-v128", [V128], [V128]), ("none-17", [], [I32] * 17)]  # fmt: skip
for op, on in ((2, "block"), (3, "loop"), (4, "if")):
    for sn, params, results in SHAPES:
        add("blocktype", "%s-%s" % (on, sn), t_blocktype(op, params, results))
        add("blocktype", "%s-%s-padded-index" % (on, sn), t_blocktype(op, params, results, lambda t: spad(t, 3)))
    add("blocktype", "%s-type-out-of-range" % on, t_blocktype(op, [], [], lambda t: s(500)))
    add("blocktype", "%s-s33-five-bytes" % on, t_blocktype(op, [], [], lambda t: spad(t, 5)))
    add("blocktype", "%s-s33-six-bytes" % on, t_blocktype(op, [], [], lambda t: spad(t, 6)))
    add("blocktype", "%s-empty-padded" % on, t_blocktype(op, [], [], lambda t: B(0xC0, 0x7F)))
    add("blocktype", "%s-i32-padded" % on, t_blocktype(op, [], [I32], lambda t: B(0xFF, 0x7F)))
    add("blocktype", "%s-negative-two-bytes" % on, t_blocktype(op, [], [], lambda t: B(0xBF, 0x7F)))
for sn, params, results in SHAPES:
    add("blocktype", "if-no-else-%s" % sn, t_blocktype(4, params, results, with_else=False))
    add("blocktype", "function-%s" % sn,
        lambda m, params=params, results=results: m.fn(m.ty(params, results), body(b"".join(default_init(r) for r in results))) and None)
    add("blocktype", "call-%s" % sn,
        lambda m, params=params, results=results: code(m, b"".join(default_init(p) for p in params) + B(0x10) + u(
            m.fn(m.ty(params, results), body(b"".join(default_init(r) for r in results)))) + DROP * len(results)) and None)

CONTROL = [
    ("br-0", B(0x0C, 0)), ("br-function-depth-in-block", B(2, 0x40, 0x0C, 1, 0x0B)), ("br-out-of-range", B(0x0C, 1)),
    ("br-out-of-range-in-block", B(2, 0x40, 0x0C, 2, 0x0B)), ("br-padded", B(0x0C) + pad(0, 5)),
    ("br-if", C0 + B(0x0D, 0)), ("br-if-out-of-range", C0 + B(0x0D, 3)),
    ("br-table-empty", C0 + B(0x0E, 0, 0)), ("br-table-three", B(2, 0x40, 2, 0x40) + C0 + B(0x0E, 3, 0, 1, 2, 0, 0x0B, 0x0B)),
    ("br-table-default-out-of-range", C0 + B(0x0E, 0, 4)), ("br-table-label-out-of-range", C0 + B(0x0E, 1, 4, 0)),
    ("br-table-label-out-of-range-unreachable", UNREACHABLE + B(0x0E, 1, 4, 0)),
    ("br-table-arity-mismatch", B(2, 0x7F) + C0 + C0 + B(0x0E, 1, 0, 1, 0x0B, 0x1A)),
    ("br-table-arity-mismatch-unreachable", B(2, 0x7F) + UNREACHABLE + B(0x0E, 1, 0, 1, 0x0B, 0x1A)),
    ("br-table-type-mismatch-unreachable", B(2, 0x7F, 2, 0x7E) + UNREACHABLE + B(0x0E, 1, 0, 1, 0x0B, 0x1A, 0x41, 0, 0x0B, 0x1A)),
    ("br-table-loop-and-block", B(2, 0x7F, 3, 0x40) + C0 + B(0x0E, 1, 0, 1, 0x0B, 0x41, 0, 0x0B, 0x1A)),
    ("br-table-count-overruns", C0 + B(0x0E) + u(100000) + B(0)),
    ("br-table-5000", C0 + B(0x0E) + u(5000) + B(0) * 5001),
    ("br-table-with-value", B(2, 0x7F) + C0 + C0 + B(0x0E, 1, 0, 0, 0x0B, 0x1A)),
    ("br-table-padded-count", C0 + B(0x0E) + pad(0, 5) + B(0)),
    ("return", B(0x0F)), ("return-then-garbage-types", B(0x0F, 0x6A, 0x1A)),
    ("unreachable-i32-add", UNREACHABLE + B(0x6A, 0x1A)), ("unreachable-select", UNREACHABLE + B(0x1B, 0x1A)),
    ("unreachable-select-mixed", UNREACHABLE + B(0x41, 0, 0x42, 0, 0x41, 0, 0x1B, 0x1A)),
    ("unreachable-select-ref", UNREACHABLE + B(0xD0, 0x70, 0x41, 0, 0x1B, 0x1A)),
    ("unreachable-i64-into-i32-add", UNREACHABLE + B(0x42, 0, 0x6A, 0x1A)),
    ("unreachable-drop-drop", UNREACHABLE + DROP + DROP), ("unreachable-leaves-values", UNREACHABLE + C0 + C0),
    ("leaves-value", C0), ("drop-empty", DROP), ("i32-add-one-operand", C0 + B(0x6A, 0x1A)),
    ("i32-add-wrong-type", C0 + B(0x42, 0, 0x6A, 0x1A)), ("else-alone", B(5)),
    ("if-no-else-with-result", C0 + B(4, 0x7F) + C0 + B(0x0B, 0x1A)),
    ("if-else-else", C0 + B(4, 0x40, 5, 5, 0x0B)), ("else-in-block", B(2, 0x40, 5, 0x0B)),
    ("else-in-loop", B(3, 0x40, 5, 0x0B)), ("extra-end", END), ("extra-end-then-code", END + NOP),
    ("block-unclosed", B(2, 0x40)), ("block-value-below", C0 + B(2, 0x40, 0x1A, 0x0B)),
    ("loop-br-with-result", B(3, 0x7F) + C0 + B(0x0C, 0, 0x0B, 0x1A)),
    ("deep-nesting-500", B(2, 0x40) * 500 + END * 500), ("deep-nesting-70000", B(2, 0x40) * 70000 + END * 70000),
    ("select-refs-untyped", B(0xD0, 0x70, 0xD0, 0x70, 0x41, 0, 0x1B, 0x1A)),
    ("select-v128-untyped", (B(0xFD, 0x0C) + bytes(16)) * 2 + B(0x41, 0, 0x1B, 0x1A)),
    ("select-t-empty", C0 * 3 + B(0x1C, 0, 0x1A)), ("select-t-two", C0 * 3 + B(0x1C, 2, 0x7F, 0x7F, 0x1A)),
    ("select-t-padded-count", C0 * 3 + B(0x1C) + pad(1, 3) + B(0x7F, 0x1A)),
    ("select-t-funcref", B(0xD0, 0x70, 0xD0, 0x70, 0x41, 0, 0x1C, 1, 0x70, 0x1A)),
    ("select-t-externref", B(0xD0, 0x6F, 0xD0, 0x6F, 0x41, 0, 0x1C, 1, 0x6F, 0x1A)),
    ("select-t-mismatch", B(0xD0, 0x6F, 0xD0, 0x70, 0x41, 0, 0x1C, 1, 0x70, 0x1A)),
    ("local-get-out-of-range", B(0x20, 5, 0x1A)), ("global-get-out-of-range", B(0x23, 5, 0x1A)),
    ("call-out-of-range", B(0x10, 90)), ("call-padded", B(0x10) + pad(0, 5) if False else B(0x42, 0, 0x10) + pad(0, 5)),
    ("call-padded-6", B(0x42, 0, 0x10) + pad(0, 6)), ("call-index-bit-32", B(0x42, 0, 0x10, 0x80, 0x80, 0x80, 0x80, 0x10)),
    ("call-missing-argument", B(0x10, 0)), ("call-wrong-argument", C0 + B(0x10, 0)),
    ("return-call", B(0x42, 0, 0x12, 0)), ("return-call-self-type", B(0x12, 2)),
    ("ref-is-null-func", B(0xD0, 0x70, 0xD1, 0x1A)), ("ref-is-null-extern", B(0xD0, 0x6F, 0xD1, 0x1A)),
    ("ref-is-null-i32", C0 + B(0xD1, 0x1A)), ("ref-eq", B(0xD0, 0x70, 0xD0, 0x70, 0xD3, 0x1A)),
    ("ref-as-non-null", B(0xD0, 0x70, 0xD4, 0x1A)), ("br-on-null", B(2, 0x40, 0xD0, 0x70, 0xD5, 0, 0x1A, 0x0B)),
    ("br-on-non-null", B(2, 0x70, 0xD0, 0x70, 0xD6, 0, 0xD0, 0x70, 0x0B, 0x1A)),
    ("call-ref", B(0xD0, 2, 0x14, 2)), ("return-call-ref", B(0xD0, 2, 0x15, 2)),
    ("ref-null-type-index", B(0xD0, 2, 0x1A)), ("ref-null-padded-func", B(0xD0, 0xF0, 0x7F, 0x1A)),
    ("ref-null-minus-one", B(0xD0, 0x7F, 0x1A)), ("ref-func-out-of-range", B(0xD2, 90, 0x1A)),
    ("ref-func-undeclared-start", B(0xD2, 0, 0x1A)),
    ("memory-size-no-memory", B(0x3F, 0, 0x1A)), ("i32-load-no-memory", C0 + B(0x28, 2, 0, 0x1A)),
    ("table-get-no-table", C0 + B(0x25, 0, 0x1A)),
    ("code-after-function-end", b""),
]  # fmt: skip
for tag, ins in CONTROL:
    if tag == "code-after-function-end":
        add("control-flow", tag, lambda m: m.fn(2, vec([]) + END + NOP + END) and None)
    else:
        add("control-flow", tag, lambda m, ins=ins: code(m, ins) and None)


def t_call_indirect(op, type_idx, table_idx, tables, tb=FUNCREF, padded=False):
    def f(m):
        m.table(tables, tb)
        enc = (pad(type_idx, 2) + pad(table_idx, 2)) if padded else (u(type_idx) + u(table_idx))
        code(m, B(0x42, 0) * {0: 1, 1: 2}.get(type_idx, 0) + C0 + B(op) + enc)

    return f


for op, on in ((0x11, "call-indirect"), (0x13, "return-call-indirect")):
    for type_idx in (0, 1, 2, 90):
        for table_idx in (0, 1):
            for tables in (0, 1, 2):
                add("calls", "%s-type%d-table%d-of-%d" % (on, type_idx, table_idx, tables), t_call_indirect(op, type_idx, table_idx, tables))
    add("calls", "%s-externref-table" % on, t_call_indirect(op, 2, 0, 1, EXTERNREF))
    add("calls", "%s-padded" % on, t_call_indirect(op, 2, 0, 1, padded=True))
    add("calls", "%s-table-byte-80" % on, lambda m, op=op: (m.table(), code(m, C0 + B(op, 2, 0x80))) and None)
    add("calls", "%s-table-byte-7f" % on, lambda m, op=op: (m.table(), code(m, C0 + B(op, 2, 0x7F))) and None)
for target, tn in ((0, "start"), (1, "called")):
    add("calls", "return-call-%s-from-same-type" % tn,
        lambda m, target=target: m.fn(target, body(B(0x42, 0) * (target + 1) + B(0x12, target))) and None)
add("calls", "return-call-result-mismatch",
    lambda m: m.fn(m.ty([], [I32]), body(B(0x42, 0, 0x12, 0))) and None)

# ref.func and how the function it names was declared.
for how in ("export", "elem-active", "elem-passive", "elem-declared", "elem-declared-expr", "global", "start",
            "undeclared", "self", "imported-undeclared", "imported-exported", "other-body-only"):
    def f(m, how=how):
        imp = m.imp(0, u(2)) if how.startswith("imported") else None
        h = m.fn(2, body())
        target = imp if imp is not None else h
        if how in ("export", "imported-exported"):
            m.export("h", 0, target)
        elif how == "elem-active":
            m.table()
            m.elems.append(B(0) + C0 + END + vec([u(h)]))
        elif how == "elem-passive":
            m.elems.append(B(1, 0) + vec([u(h)]))
        elif how == "elem-declared":
            m.elems.append(B(3, 0) + vec([u(h)]))
        elif how == "elem-declared-expr":
            m.elems.append(B(7) + FUNCREF + vec([B(0xD2) + u(h) + END]))
        elif how == "global":
            m.globals.append(FUNCREF + B(0) + B(0xD2) + u(h) + END)
        elif how == "start":
            m.start = h
        elif how == "other-body-only":
            code(m, B(0xD2) + u(h) + DROP)
        if how == "self":
            target = h + 1
        code(m, B(0xD2) + u(target) + DROP)

    add("reffunc", how, f)

# ---- 10. function bodies, locals, memory arguments

add("body", "size-plus-1", lambda m: m.fn(2, body(NOP), u(4)) and None)
add("body", "size-minus-1", lambda m: m.fn(2, body(NOP), u(2)) and None)
add("body", "size-zero", lambda m: m.fn(2, b"", u(0)) and None)
add("body", "size-padded", lambda m: m.fn(2, body(NOP), pad(3, 5)) and None)
add("body", "size-padded-6", lambda m: m.fn(2, body(NOP), pad(3, 6)) and None)
add("body", "no-end", lambda m: m.fn(2, vec([]) + NOP) and None)
add("body", "locals-only", lambda m: m.fn(2, vec([])) and None)
add("body", "locals-zero-count-group", lambda m: code(m, b"", [(0, I32)]) and None)
add("body", "locals-zero-count-bad-type", lambda m: code(m, b"", [(0, B(0x41))]) and None)
add("body", "locals-many-groups", lambda m: code(m, b"", [(1, I32), (1, I64)] * 200) and None)
add("body", "locals-50000", lambda m: code(m, b"", [(50000, I64)]) and None)
add("body", "locals-50001", lambda m: code(m, b"", [(50001, I64)]) and None)
add("body", "locals-1000000", lambda m: code(m, b"", [(1000000, I64)]) and None)
add("body", "locals-u32-max", lambda m: code(m, b"", [(0xFFFFFFFF, I32)]) and None)
add("body", "locals-overflow-two-groups", lambda m: code(m, b"", [(0xFFFFFFFF, I32), (1, I32)]) and None)
add("body", "locals-overflow-with-params", lambda m: code(m, b"", [(0xFFFFFFFF, I32)], tidx=1) and None)
add("body", "locals-count-padded", lambda m: m.fn(2, vec([pad(1, 5) + I32]) + END) and None)
add("body", "locals-count-over-u32", lambda m: m.fn(2, vec([u(1 << 32) + I32]) + END) and None)
add("body", "locals-group-count-overruns", lambda m: m.fn(2, u(5) + B(1, 0x7F) + END) and None)
add("body", "locals-v128", lambda m: code(m, B(0x20, 0, 0x1A), [(1, V128)]) and None)
add("body", "locals-funcref-externref", lambda m: code(m, B(0x20, 0, 0x1A, 0x20, 1, 0x1A), [(1, FUNCREF), (1, EXTERNREF)]) and None)
add("body", "big-function", lambda m: code(m, (C0 + DROP) * 20000) and None)
add("body", "deep-stack", lambda m: code(m, C0 * 20000 + DROP * 20000) and None)
add("body", "many-functions", lambda m: [m.fn(2, body()) for _ in range(3000)] and None)

NATURAL_ALIGN = {0x28: 2, 0x29: 3, 0x2A: 2, 0x2B: 3, 0x2C: 0, 0x2D: 0, 0x2E: 1, 0x2F: 1, 0x30: 0, 0x31: 0,
                 0x32: 1, 0x33: 1, 0x34: 2, 0x35: 2, 0x36: 2, 0x37: 3, 0x38: 2, 0x39: 3, 0x3A: 0, 0x3B: 1,
                 0x3C: 0, 0x3D: 1, 0x3E: 2}  # fmt: skip


def t_memarg(op, align, offset=B(0)):
    def f(m):
        m.mem()
        code(m, UNREACHABLE + B(op) + align + offset + UNREACHABLE)

    return f


for op, natural in NATURAL_ALIGN.items():
    add("memarg", "%02x-natural" % op, t_memarg(op, B(natural)))
    add("memarg", "%02x-natural-plus-1" % op, t_memarg(op, B(natural + 1)))
for align in list(range(4, 9)) + [31, 32, 33, 63, 64, 65, 127]:
    add("memarg", "i64-load-align-%d" % align, t_memarg(0x29, B(align)))
add("memarg", "align-padded", t_memarg(0x28, pad(2, 5)))
add("memarg", "align-padded-6", t_memarg(0x28, pad(2, 6)))
add("memarg", "align-over-u32", t_memarg(0x28, u(1 << 32)))
add("memarg", "offset-u32-max", t_memarg(0x28, B(2), u(0xFFFFFFFF)))
add("memarg", "offset-padded", t_memarg(0x28, B(2), pad(8, 5)))
add("memarg", "offset-padded-6", t_memarg(0x28, B(2), pad(8, 6)))
add("memarg", "offset-2-to-32", t_memarg(0x28, B(2), u(1 << 32)))
add("memarg", "offset-2-to-63", t_memarg(0x28, B(2), u(1 << 63)))
add("memarg", "offset-bit-32-in-five-bytes", t_memarg(0x28, B(2), B(0x80, 0x80, 0x80, 0x80, 0x10)))
for n, natural in ((0, 4), (1, 3), (5, 3), (7, 2), (8, 3), (9, 2), (10, 3), (11, 4), (84, 0), (85, 1), (86, 2), (87, 3), (92, 2), (93, 3)):
    for delta in (0, 1):
        add("memarg", "simd-%d-align-%d" % (n, natural + delta),
            lambda m, n=n, a=natural + delta: (m.mem(), code(m, UNREACHABLE + B(0xFD) + u(n) + B(a, 0) + (B(0) if 84 <= n <= 91 else b"") + UNREACHABLE)) and None)
for n in (0x10, 0x11, 0x17, 0x1E, 0x48):
    for delta in (-1, 1):
        if FE_ALIGN[n] + delta >= 0:
            add("memarg", "atomic-%d-align-%d" % (n, FE_ALIGN[n] + delta),
                lambda m, n=n, a=FE_ALIGN[n] + delta: (m.mem(), code(m, UNREACHABLE + B(0xFE) + u(n) + B(a, 0) + UNREACHABLE)) and None)
for lane_op, lanes in ((21, 16), (24, 8), (27, 4), (29, 2), (31, 4), (33, 2), (22, 16), (25, 8), (28, 4), (30, 2), (32, 4), (34, 2)):
    add("simdimm", "lane-op-%d-lane-%d" % (lane_op, lanes),
        lambda m, lane_op=lane_op, lanes=lanes: code(m, UNREACHABLE + B(0xFD, lane_op, lanes) + UNREACHABLE) and None)
    add("simdimm", "lane-op-%d-lane-255" % lane_op,
        lambda m, lane_op=lane_op: code(m, UNREACHABLE + B(0xFD, lane_op, 255) + UNREACHABLE) and None)
add("simdimm", "shuffle-lane-31", lambda m: code(m, UNREACHABLE + B(0xFD, 13) + B(31) * 16 + UNREACHABLE) and None)
add("simdimm", "shuffle-lane-32", lambda m: code(m, UNREACHABLE + B(0xFD, 13) + B(32) * 16 + UNREACHABLE) and None)
for n, lanes in ((84, 16), (85, 8), (86, 4), (87, 2), (88, 16), (89, 8), (90, 4), (91, 2)):
    add("simdimm", "memory-lane-op-%d-lane-%d" % (n, lanes),
        lambda m, n=n, lanes=lanes: (m.mem(), code(m, UNREACHABLE + B(0xFD, n, 0, 0, lanes) + UNREACHABLE)) and None)

# ---- 11. LEB128: non-minimal and over-long encodings


def t_secsize(i, width):
    def post(secs):
        k = find(secs, i)
        secs[k] = [i, secs[k][1], pad(len(secs[k][1]), width)]
        return secs

    return with_post(full, post)


for i in range(1, 13):
    for width in (5, 6):
        add("leb", "section-size-%s-%d-bytes" % (SECTION_NAMES[i], width), t_secsize(i, width))
    for width in (5, 6):
        if i not in (8, 12):
            add("leb", "section-count-%s-%d-bytes" % (SECTION_NAMES[i], width),
                lambda m, i=i, width=width: (full(m), m.counts.__setitem__(i, lambda n: pad(n, width))) and None)
add("leb", "section-size-bit-32", with_post(full, lambda secs: [[sec[0], sec[1], B(0x80, 0x80, 0x80, 0x80, 0x10)] if sec[0] == 7 else sec for sec in secs]))
add("leb", "section-size-huge", with_post(full, lambda secs: [[sec[0], sec[1], u(0xFFFFFFFF)] if sec[0] == 7 else sec for sec in secs]))
add("leb", "section-id-high-bit", with_post(full, lambda secs: [[0x81, sec[1]] if sec[0] == 1 else sec for sec in secs]))
for tag, enc in (("i32-const-5-bytes-zero", B(0x41, 0x80, 0x80, 0x80, 0x80, 0x00)), ("i32-const-6-bytes", B(0x41) + pad(0, 6)),
                 ("i32-const-minus-1-5-bytes", B(0x41, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F)),
                 ("i32-const-unused-bits-0f", B(0x41, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F)),
                 ("i32-const-unused-bits-70", B(0x41, 0x80, 0x80, 0x80, 0x80, 0x70)),
                 ("i32-const-min", B(0x41, 0x80, 0x80, 0x80, 0x80, 0x78)),
                 ("i32-const-unused-bits-4f", B(0x41, 0xFF, 0xFF, 0xFF, 0xFF, 0x4F)),
                 ("i32-const-two-bytes-zero", B(0x41, 0x80, 0x00)), ("i32-const-two-bytes-minus-1", B(0x41, 0xFF, 0x7F)),
                 ("i64-const-10-bytes-zero", B(0x42) + B(0x80) * 9 + B(0)), ("i64-const-10-bytes-min", B(0x42) + B(0x80) * 9 + B(0x7F)),
                 ("i64-const-10-bytes-bad-01", B(0x42) + B(0x80) * 9 + B(1)), ("i64-const-10-bytes-bad-7e", B(0x42) + B(0xFF) * 9 + B(0x7E)),
                 ("i64-const-10-bytes-minus-1", B(0x42) + B(0xFF) * 9 + B(0x7F)), ("i64-const-11-bytes", B(0x42) + B(0x80) * 10 + B(0))):  # fmt: skip
    add("leb", tag, lambda m, enc=enc: code(m, enc + DROP) and None)
for tag, ins in (("local-get-padded", B(0x20) + pad(0, 5) + DROP), ("local-get-padded-6", B(0x20) + pad(0, 6) + DROP),
                 ("local-get-bit-32", B(0x20, 0x80, 0x80, 0x80, 0x80, 0x10) + DROP),
                 ("local-get-unused-bits", B(0x20, 0x80, 0x80, 0x80, 0x80, 0x70) + DROP),
                 ("global-get-padded", B(0x23) + pad(0, 5) + DROP), ("memory-size-80-00", B(0x3F, 0x80, 0x00) + DROP),
                 ("memory-size-01", B(0x3F, 1) + DROP), ("memory-grow-80-00", C0 + B(0x40, 0x80, 0x00) + DROP),
                 ("fc-subop-padded", B(0x43) + bytes(4) + B(0xFC) + pad(0, 2) + DROP),
                 ("fc-subop-padded-5", B(0x43) + bytes(4) + B(0xFC) + pad(0, 5) + DROP),
                 ("fc-subop-padded-6", B(0x43) + bytes(4) + B(0xFC) + pad(0, 6) + DROP),
                 ("fc-subop-bit-32", B(0x43) + bytes(4) + B(0xFC, 0x80, 0x80, 0x80, 0x80, 0x10) + DROP),
                 ("fd-subop-padded", B(0xFD) + pad(12, 2) + bytes(16) + DROP),
                 ("fd-subop-padded-5", B(0xFD) + pad(12, 5) + bytes(16) + DROP),
                 ("fd-subop-padded-6", B(0xFD) + pad(12, 6) + bytes(16) + DROP),
                 ("fe-subop-padded", B(0xFE) + pad(3, 2) + B(0)),
                 ("memory-fill-padded", I3 + B(0xFC, 11) + pad(0, 2)), ("memory-copy-padded", I3 + B(0xFC, 10) + pad(0, 2) * 2),
                 ("table-get-padded", C0 + B(0x25) + pad(0, 2) + DROP), ("ref-func-padded", B(0xD2) + pad(0, 5) + DROP)):  # fmt: skip
    add("leb", tag, lambda m, ins=ins: (m.rich(), m.export("z", 0, 0), code(m, ins, [(1, I32)])) and None)
add("leb", "type-param-count-padded", lambda m: m.tyraw(B(0x60) + pad(1, 5) + I32 + vec([])) and None)
add("leb", "type-form-padded", lambda m: m.tyraw(B(0xE0, 0x7F, 0, 0)) and None)
add("leb", "valtype-padded", lambda m: m.tyraw(B(0x60, 1, 0xFF, 0x7F, 0)) and None)
add("leb", "export-name-length-padded", lambda m: m.exports.append(pad(1, 5) + b"x" + B(0, 0)))
add("leb", "export-index-padded", lambda m: m.exports.append(name("x") + B(0) + pad(0, 5)))
add("leb", "export-index-padded-6", lambda m: m.exports.append(name("x") + B(0) + pad(0, 6)))
add("leb", "export-kind-padded", lambda m: m.exports.append(name("x") + B(0x80, 0, 0)))
add("leb", "import-name-length-padded", lambda m: m.imports.append((0, pad(3, 5) + b"env" + name("f") + B(0, 2))))
add("leb", "function-type-index-padded", lambda m: m.fn(pad(2, 5), body()) and None)
add("leb", "function-type-index-padded-6", lambda m: m.fn(pad(2, 6), body()) and None)
add("leb", "limits-flags-padded", lambda m: m.mems.append(B(0x80, 0x00, 1)))
add("leb", "limits-min-padded", lambda m: m.mems.append(B(0) + pad(1, 5)))
add("leb", "limits-min-padded-6", lambda m: m.mems.append(B(0) + pad(1, 6)))
add("leb", "table-limits-min-padded", lambda m: m.tables.append(FUNCREF + B(0) + pad(1, 5)))
add("leb", "global-mutability-padded", lambda m: m.globals.append(I32 + B(0x80, 0) + C0 + END))
add("leb", "elem-flag-padded", lambda m: (m.table(), m.elems.append(pad(0, 2) + C0 + END + vec([]))) and None)
add("leb", "elem-flag-padded-5", lambda m: (m.table(), m.elems.append(pad(0, 5) + C0 + END + vec([]))) and None)
add("leb", "elem-flag-padded-6", lambda m: (m.table(), m.elems.append(pad(0, 6) + C0 + END + vec([]))) and None)
add("leb", "elem-count-padded", lambda m: (m.table(), m.elems.append(B(0) + C0 + END + pad(1, 5) + u(0))) and None)
add("leb", "data-flag-padded", lambda m: (m.mem(), m.datas.append(pad(0, 2) + C0 + END + vec([]))) and None)
add("leb", "data-flag-padded-6", lambda m: (m.mem(), m.datas.append(pad(0, 6) + C0 + END + vec([]))) and None)
add("leb", "data-length-padded", lambda m: (m.mem(), m.datas.append(B(0) + C0 + END + pad(1, 5) + b"x")) and None)
add("leb", "data-memory-index-padded", lambda m: (m.mem(), m.datas.append(B(2) + pad(0, 3) + C0 + END + vec([]))) and None)
add("leb", "datacount-padded", with_post(full, lambda secs: [[12, pad(1, 5)] if sec[0] == 12 else sec for sec in secs]))
add("leb", "start-index-padded-6", with_post(full, lambda secs: [[8, pad(3, 6)] if sec[0] == 8 else sec for sec in secs]))
add("leb", "custom-name-length-padded", with_post(full, lambda secs: secs + [[0, pad(1, 5) + b"x"]]))
add("leb", "tag-type-padded", lambda m: m.tags.append(B(0) + pad(2, 5)))
add("leb", "global-init-const-padded", lambda m: m.globals.append(I32 + B(0) + B(0x41) + spad(0, 5) + END))
add("leb", "global-init-const-padded-6", lambda m: m.globals.append(I32 + B(0) + B(0x41) + spad(0, 6) + END))
add("leb", "global-init-i64-const-11", lambda m: m.globals.append(I64 + B(0) + B(0x42) + spad(0, 11) + END))

# -------------------------------------------------------------------- output


def build(index, seed):
    family, text, f = CAT[index]
    rng = random.Random("%d/%s/%s" % (seed, family, text))
    m = Mod(rng)
    f(m)
    secs = m.sections()
    if m.post:
        secs = m.post(secs)
    secs = secs + [custom("twist", ("%s/%s" % (family, text)).encode())]
    out = serialize(secs)
    if m.raw:
        out = m.raw(out)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=0, help="0: the whole catalogue once")
    ap.add_argument("--out", help="directory to write the modules into")
    ap.add_argument("--list", action="store_true", help="print the catalogue and exit")
    args = ap.parse_args()
    names = [(family, text) for family, text, _ in CAT]
    assert len(set(names)) == len(names), "duplicate twist name"
    for family, text in names:
        assert re.fullmatch(r"[a-z0-9-]+", family + text), (family, text)
    if args.list:
        for family, text in names:
            print("%s/%s" % (family, text))
        return
    if not args.out:
        ap.error("--out is required")
    os.makedirs(args.out, exist_ok=True)
    if args.count == 0:
        order = list(range(len(CAT)))
    else:
        pick = random.Random(args.seed)
        order = [pick.randrange(len(CAT)) for _ in range(args.count)]
    for n, index in enumerate(order):
        family, text, _ = CAT[index]
        # A repeated entry gets other incidental choices.
        blob = build(index, args.seed if args.count == 0 else args.seed * 1000003 + n)
        with open(os.path.join(args.out, "%05d__%s__%s.wasm" % (n, family, text)), "wb") as out:
            out.write(blob)
    print("%d modules written to %s (catalogue has %d twists)" % (len(order), args.out, len(CAT)))


if __name__ == "__main__":
    main()
