#!/usr/bin/env python3
"""An independent model of the A64 SME / SME2 pseudocode of the Arm ARM, executed on the textual form
of an instruction (the LLVM syntax) and a state of the protocol of tests/arm/semantics/armstate.py:
tiles and slices of za (the ZAtile / ZAslice / ZAvector accessors), the outer products, the moves,
the loads and stores, the multi vector arithmetic with the vector groups of za, the lookup tables of
zt0. It does not use dismount: it is the expected value generator of the unit rows of
tests/TestArm64SemanticsSme.cpp (gen_sme_semantic_rows.py) for the instructions no processor
available to the differential tests implements (the features the M4 lacks), and it is checked against
the M4 on the ones it runs (tests/arm/tools/check_sme_model.py), so the other forms are the same code
with the other widths.

The floating point is fpmodel.py (exact rational arithmetic). FPCR: the rounding mode, FZ, DN; the
cumulative flags accumulate into fpsr.

    run(text, state) -> the result dict (exit 'fall' or 'trap:undef' / 'trap:fault'), the state is
                        not modified

Author: Elad Raz <e@eladraz.com>
"""
import copy
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'semantics'))
import fpmodel as F
import armstate as S

M64 = (1 << 64) - 1
SIZES = {'b': 8, 'h': 16, 's': 32, 'd': 64, 'q': 128}


class Trap(Exception):
    def __init__(self, kind):
        Exception.__init__(self, kind)
        self.kind = kind


# ---------------------------------------------------------------------------------------------
# Bit helpers
# ---------------------------------------------------------------------------------------------

def mask(bits):
    return (1 << bits) - 1


def sext(value, bits):
    value &= mask(bits)
    return value - (1 << bits) if value >> (bits - 1) else value


def elem(value, i, esize):
    return (value >> (i * esize)) & mask(esize)


def set_elem(value, i, esize, x):
    value &= ~(mask(esize) << (i * esize))
    return value | ((x & mask(esize)) << (i * esize))


def pred_active(p, e, esize):
    """ActivePredicateElement: bit e * esize / 8 of the predicate."""
    return (p >> (e * esize // 8)) & 1


# ---------------------------------------------------------------------------------------------
# Operand parsing
# ---------------------------------------------------------------------------------------------

def split_operands(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class Za(object):
    """za operand: tile (or None for the array), direction h / v / None, size letter, slice register,
    offset, offset_last, vector group (0, 2, 4)."""

    def __init__(self, text):
        m = re.match(r'^za(\d+)?([hv])?(?:\.(\w))?(?:\[(.*)\])?$', text)
        self.tile = int(m.group(1)) if m.group(1) else None
        self.dir = m.group(2)
        self.size = m.group(3)
        self.esize = SIZES[m.group(3)] if m.group(3) else None
        self.reg = None
        self.off = 0
        self.last = 0
        self.vg = 0
        self.has_index = m.group(4) is not None
        if m.group(4):
            for k, part in enumerate(x.strip() for x in m.group(4).split(',')):
                if k == 0:
                    self.reg = int(part[1:])
                elif part.startswith('vgx'):
                    self.vg = int(part[3:])
                elif ':' in part:
                    a, b = part.split(':')
                    self.off, self.last = int(a, 0), int(b, 0)
                else:
                    self.off = self.last = int(part, 0)


class Zr(object):
    """z register operand with an optional element size and index."""

    def __init__(self, text):
        m = re.match(r'^z(\d+)(?:\.(\w))?(?:\[(\d+)\])?$', text)
        self.n = int(m.group(1))
        self.size = m.group(2)
        self.esize = SIZES[m.group(2)] if m.group(2) else None
        self.index = int(m.group(3)) if m.group(3) is not None else None


class ZList(object):
    def __init__(self, text):
        inner = text.strip()[1:-1].strip()
        m = re.match(r'^z(\d+)\.(\w)\s*-\s*z(\d+)\.(\w)$', inner)
        if m:
            a, b = int(m.group(1)), int(m.group(3))
            self.regs = list(range(a, b + 1))
            self.size = m.group(2)
        else:
            items = [x.strip() for x in inner.split(',')]
            self.regs = [int(x.split('.')[0][1:]) for x in items]
            self.size = items[0].split('.')[1] if '.' in items[0] else None
        self.esize = SIZES[self.size] if self.size else None


class Pred(object):
    def __init__(self, text):
        m = re.match(r'^p(n?)(\d+)(?:/([mz]))?$', text)
        self.counter = bool(m.group(1))
        self.n = int(m.group(2))
        self.qual = m.group(3)


def parse_operand(text):
    t = text.strip()
    if t in ('sm', 'za', 'svcr'):
        return t
    if t.startswith('za'):
        return Za(t)
    if t.startswith('{'):
        if re.match(r'^\{\s*zt0\s*\}$', t):
            return 'zt0list'
        mm = re.match(r'^\{\s*(za\d+[hv]\.\w\[.*\])\s*\}$', t)
        if mm:
            return Za(mm.group(1))
        if 'za' in t:
            return t
        return ZList(t)
    if re.match(r'^z\d+', t):
        return Zr(t)
    if re.match(r'^pn?\d+(/[mz])?$', t):
        return Pred(t)
    if t.startswith('['):
        return Mem(t)
    if t.startswith('#'):
        return int(t[1:], 0)
    if t.startswith('zt0'):
        m = re.match(r'^zt0(?:\[(\d+)(?:, mul vl)?\])?$', t)
        return ('zt0', int(m.group(1)) if m and m.group(1) else 0, 'mul vl' in t)
    if re.match(r'^[xw]\d+$', t) or t in ('sp', 'xzr', 'wzr'):
        return Gpr(t)
    return t


class Gpr(object):
    def __init__(self, text):
        self.text = text
        self.n = 31 if text in ('sp', 'xzr', 'wzr') else int(text[1:])
        self.is64 = text[0] != 'w'


class Mem(object):
    def __init__(self, text):
        inner = text.strip()[1:-1]
        parts = [x.strip() for x in inner.split(',')]
        self.base = parts[0]
        self.index = None
        self.shift = 0
        self.imm = 0
        self.mul_vl = False
        for part in parts[1:]:
            if re.match(r'^x\d+$', part):
                self.index = int(part[1:])
            elif part.startswith('lsl'):
                self.shift = int(part.split('#')[1])
            elif part.startswith('#'):
                self.imm = int(part[1:], 0)
            elif part == 'mul vl':
                self.mul_vl = True


# ---------------------------------------------------------------------------------------------
# The machine
# ---------------------------------------------------------------------------------------------

class Machine(object):
    def __init__(self, state):
        self.st = copy.deepcopy(state)
        st = self.st
        self.svl = st.get('svl', 512)
        self.vl = self.svl if st.get('sm') else st.get('vl', 128)
        self.rows = self.svl // 8
        if 'zarows' not in st:
            st['zarows'] = [0] * self.rows
        if 'zt0' not in st:
            st['zt0'] = 0
        if 'z' not in st:
            st['z'] = [0] * 32
        if 'p' not in st:
            st['p'] = [0] * 16
        self.written = {}
        self.fst = F.State(0)
        self.ctl = F.Ctl.from_fpscr(st['fpcr'])
        self.keep_fpsr = False

    def za_float(self, dn=True):
        """The floating point of the instructions which accumulate into za: default NaN (not for
        every widening form), and the cumulative flags of fpsr are not updated."""
        self.ctl.dn = dn
        self.keep_fpsr = True

    # --- mode checks
    def need_streaming(self):
        if not self.st.get('sm'):
            raise Trap('undef')

    def need_za(self):
        if not self.st.get('za'):
            raise Trap('undef')

    # --- registers
    def z(self, n):
        return self.st['z'][n] & mask(self.svl)

    def set_z(self, n, v):
        self.st['z'][n] = v & mask(self.svl)
        # the low 128 bits of z are v
        self.st['v'][n] = self.st['z'][n] & mask(128)

    def p(self, n):
        return self.st['p'][n] & mask(self.svl // 8)

    def set_p(self, n, v):
        self.st['p'][n] = v & mask(self.svl // 8)

    def x(self, n, is64=True):
        if n == 31:
            return 0
        return self.st['x'][n] if is64 else self.st['x'][n] & 0xFFFFFFFF

    def xsp(self, n):
        return self.st['sp'] if n == 31 else self.st['x'][n]

    def w(self, n):
        return self.st['x'][n] & 0xFFFFFFFF

    # --- memory
    def read_byte(self, a):
        if a in self.written:
            return self.written[a]
        if 'mem_base' in self.st:
            # (the memory of the unit tests: zero but for the bytes of the state)
            return self.st.get('mem', {}).get(a, 0)
        return S.mem_value('a64', {}, a, b'', self.st)

    def check(self, a, n):
        base = self.st.get('mem_base', S.A64_MEM_BASE)
        size = self.st.get('mem_size', S.MEM_SIZE)
        if a < base or a + n > base + size:
            raise Trap('fault')

    def read(self, a, n):
        self.check(a, n)
        return int.from_bytes(bytes(self.read_byte(a + i) for i in range(n)), 'little')

    def write(self, a, n, v):
        self.check(a, n)
        for i, b in enumerate(v.to_bytes(n, 'little')):
            self.written[a + i] = b

    # --- ZA
    def za_row(self, i):
        return self.st['zarows'][i]

    def set_za_row(self, i, v):
        self.st['zarows'][i] = v & mask(self.svl)

    def tile_ntiles(self, esize):
        return esize // 8

    def tile_dim(self, esize):
        return self.svl // esize

    def tile_rowindex(self, tile, esize, r):
        return r * (esize // 8) + tile

    def tile_hslice(self, tile, esize, idx):
        return self.za_row(self.tile_rowindex(tile, esize, idx))

    def tile_set_hslice(self, tile, esize, idx, v):
        self.set_za_row(self.tile_rowindex(tile, esize, idx), v)

    def tile_vslice(self, tile, esize, idx):
        dim = self.tile_dim(esize)
        out = 0
        for j in range(dim):
            out = set_elem(out, j, esize, elem(self.tile_hslice(tile, esize, j), idx, esize))
        return out

    def tile_set_vslice(self, tile, esize, idx, v):
        dim = self.tile_dim(esize)
        for j in range(dim):
            row = self.tile_hslice(tile, esize, j)
            self.tile_set_hslice(tile, esize, j, set_elem(row, idx, esize, elem(v, j, esize)))

    def tile_elem(self, tile, esize, r, c):
        return elem(self.tile_hslice(tile, esize, r), c, esize)

    def tile_set_elem(self, tile, esize, r, c, x):
        row = self.tile_hslice(tile, esize, r)
        self.tile_set_hslice(tile, esize, r, set_elem(row, c, esize, x))

    def slice_get(self, za, idx):
        if za.dir == 'h':
            return self.tile_hslice(za.tile, za.esize, idx)
        return self.tile_vslice(za.tile, za.esize, idx)

    def slice_set(self, za, idx, v):
        if za.dir == 'h':
            self.tile_set_hslice(za.tile, za.esize, idx, v)
        else:
            self.tile_set_vslice(za.tile, za.esize, idx, v)

    def slice_index(self, za, k=0):
        dim = self.tile_dim(za.esize)
        return (self.w(za.reg) + za.off + k) % dim

    def zero_all(self):
        self.st['zarows'] = [0] * self.rows

    # --- floating point
    def fmt(self, esize, bf=False):
        if bf and esize == 16:
            return F.BFLOAT
        return {16: F.HALF, 32: F.SINGLE, 64: F.DOUBLE}[esize]

    def finish(self, exit_kind='fall'):
        st = self.st
        if not self.keep_fpsr:
            st['fpsr'] |= self.fst.flags
        res = st
        res['exit'] = exit_kind
        res['pc'] = st['pc'] + 4
        res['mem'] = dict(self.written)
        return res


# ---------------------------------------------------------------------------------------------
# The handlers and the driver
# ---------------------------------------------------------------------------------------------

HANDLERS = {}


def handler(*names):
    def deco(fn):
        for n in names:
            HANDLERS[n] = fn
        return fn
    return deco


def zlist_of(op):
    """The register numbers of an operand that is a single z register or a list."""
    if isinstance(op, ZList):
        return op.regs
    return [op.n]


def za_group(m, za, nreg):
    """The ZA vector numbers of an array operand za.T[w, off, vgxN]: (w + off) mod (rows / nreg)
    + r * (rows / nreg)."""
    stride = m.rows // nreg
    base = (m.w(za.reg) + za.off) % stride
    return [base + r * stride for r in range(nreg)]


def run_one(text, state):
    text = text.strip()
    mn, _, rest = text.partition(' ')
    mn = mn.lower()
    m = Machine(state)
    ops = [parse_operand(o) for o in split_operands(rest)]
    try:
        if mn not in HANDLERS:
            raise NotImplementedError(mn)
        HANDLERS[mn](m, mn, ops)
    except Trap as t:
        res = copy.deepcopy(state)
        res['exit'] = 'trap:' + t.kind
        res['mem'] = {}
        return res
    return m.finish()


def run(text, state):
    """Execute the instruction `text` (several, separated by ';', run one after the other) on
    `state`. Returns the result dict."""
    res = state
    written = {}
    for part in text.split(';'):
        res = run_one(part, res)
        if res['exit'] != 'fall':
            return res
        written.update(res['mem'])
        res['mem'] = dict(written)
    return res


import sme_ops_move       # noqa: E402,F401
import sme_ops_outer      # noqa: E402,F401
import sme_ops_array      # noqa: E402,F401
import sme_ops_vec        # noqa: E402,F401
import sme_ops_vec2       # noqa: E402,F401
