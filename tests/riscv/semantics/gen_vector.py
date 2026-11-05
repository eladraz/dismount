#!/usr/bin/env python3
"""
Generate tests/riscv/semantics/corpus/{rv,rv64}_vgen_*.s: the vector semantics corpus (the instructions the
vector lifter of RiscvSemanticLiftVector*.cpp lifts: configuration, loads and stores, integer arithmetic,
fixed point, masks, permutation, integer reductions). The floating point, BFloat16, vector bit manipulation
and vector cryptography instructions are not here.

For every instruction the lines vary the register pattern (the aligned groups of LMUL 8, the overlaps the
architecture allows and the ones it reserves, v0 as an operand and as a masked destination, odd register
numbers), the operand form (vv / vx / vi / the widening and narrowing forms), the mask (`, v0.t`) and the
immediates. The states of a line (states_rv.py) cycle through the SEWs, the LMULs the registers allow (the
fractional ones too), ta/tu and ma/mu, vl = 0 / 1 / VLMAX - 1 / VLMAX / odd / random, vstart > 0 now and then, masks of
v0 (all ones, zeros, 0x55.., random) and vxrm. A `@state: sew=.. lmul=.. vlsel=..` tag forces the
configuration of every state of the line (the reserved register groups need a forced LMUL).

  python3 tests/riscv/semantics/gen_vector.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
OUT = os.path.join(HERE, 'corpus')
TABLES = [os.path.join(REPO, 'Source', 'dismount', 'proc', 'riscv', 'RiscvSemanticTable_vector%s.inc' % x)
          for x in ('', '_float', '_bits', '_crypto')]

SIMM = [-16, -1, 0, 1, 15]
UIMM = [0, 1, 7, 15, 31]
XREGS = ['a1', 'zero']

files = {}
emitted = set()

sys.path.insert(0, HERE)
import rvasm

GPR = {'zero': 0, 'ra': 1, 'sp': 2, 'gp': 3, 'tp': 4, 't0': 5, 't1': 6, 't2': 7, 's0': 8, 'fp': 8, 's1': 9}
for _i in range(8):
    GPR['a%d' % _i] = 10 + _i
for _i in range(2, 12):
    GPR['s%d' % _i] = 16 + _i
for _i in range(3, 7):
    GPR['t%d' % _i] = 25 + _i
FPR = {'ft0': 0, 'ft1': 1, 'fs0': 8, 'fs1': 9, 'fa0': 10, 'fa1': 11, 'fa2': 12, 'fa3': 13}
ENCODINGS = {}


def load_encodings():
    """name -> (mask, match, [(kind, position, extra)]) of the vector encoding tables."""
    for t in ('config', 'int', 'mask', 'mem', 'fp', 'ext'):
        path = os.path.join(REPO, 'Source', 'dismount', 'proc', 'riscv', 'RiscvEncodingTable_vector_%s.inc' % t)
        for line in open(path):
            m = re.match(r'E\("([^"]+)", (0x[0-9a-f]+), (0x[0-9a-f]+), \w+, [\w| ]+, ', line)
            if not m:
                continue
            specs = [(k, int(pos), extra) for k, pos, extra in
                     re.findall(r'S\((RISCV_KIND_\w+), (\d+), [^,]*, ([^)]*)\)', line)]
            ENCODINGS[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), specs)


def encode(text):
    """The 32 bit encoding of a vector instruction text, from the encoding tables (for the reserved encodings
    llvm-mc refuses to assemble); None if the text has an operand this encoder doesn't know."""
    if not ENCODINGS:
        load_encodings()
    mnemonic, _, rest = text.partition(' ')
    if mnemonic not in ENCODINGS:
        return None
    mask, word, specs = ENCODINGS[mnemonic]
    tokens = [t.strip() for t in rest.split(',')] if rest.strip() else []
    i = 0
    for kind, pos, extra in specs:
        if kind == 'RISCV_KIND_VR':
            if i >= len(tokens) or not re.fullmatch(r'v\d+', tokens[i]):
                return None
            word |= int(tokens[i][1:]) << pos
            i += 1
        elif kind == 'RISCV_KIND_VR_V0':
            if i < len(tokens) and tokens[i] == 'v0':
                i += 1
        elif kind == 'RISCV_KIND_VMASK':
            if (mask >> 25) & 1:
                continue
            if i < len(tokens) and tokens[i] == 'v0.t':
                i += 1
            else:
                word |= 1 << 25
        elif kind == 'RISCV_KIND_GPR':
            if i >= len(tokens) or tokens[i] not in GPR:
                return None
            word |= GPR[tokens[i]] << pos
            i += 1
        elif kind == 'RISCV_KIND_FPR':
            if i >= len(tokens) or tokens[i] not in FPR:
                return None
            word |= FPR[tokens[i]] << pos
            i += 1
        elif kind == 'RISCV_KIND_IMMEDIATE':
            if i >= len(tokens):
                return None
            word |= (int(tokens[i]) & 31) << 15
            i += 1
        elif kind == 'RISCV_KIND_MEMORY':
            m = re.fullmatch(r'\((\w+)\)', tokens[i]) if i < len(tokens) else None
            if not m or m.group(1) not in GPR:
                return None
            word |= GPR[m.group(1)] << pos
            i += 1
        else:
            return None
    return word if i == len(tokens) else None


def add(fname, text, tag='', rv64_only=False):
    name = ('rv64_' if rv64_only else 'rv_') + 'vgen_' + fname
    files.setdefault(name, []).append(text + ('  # ' + tag if tag else ''))
    emitted.add(text.split()[0])


def mask_suffix(masked):
    return ', v0.t' if masked else ''


# ---------------------------------------------------------------------------------------------------------
# Register patterns (vd, vs2, vs1) by shape

SAME = {
    'plain': [(8, 16, 24), (8, 8, 24), (8, 16, 8), (8, 8, 8), (8, 16, 16), (8, 0, 24), (8, 16, 0), (0, 16, 24),
              (1, 2, 3), (5, 7, 9), (31, 30, 29), (16, 8, 24), (24, 16, 8)],
    'masked': [(8, 16, 24), (8, 8, 24), (8, 16, 8), (8, 0, 24), (8, 16, 0), (0, 16, 24), (1, 2, 3)],
    'forced': [('m2', (9, 16, 24)), ('m2', (8, 17, 24)), ('m2', (8, 16, 25)), ('m4', (10, 16, 24)),
               ('m8', (8, 16, 20)), ('m2', (8, 16, 24)), ('m4', (8, 16, 24)), ('m8', (8, 16, 24))],
}
WIDEN = {   # vd is a group of 2 * LMUL
    'plain': [(16, 8, 12), (16, 8, 8), (16, 16, 12), (16, 8, 16), (16, 18, 20), (16, 17, 19), (16, 20, 18),
              (16, 19, 21), (8, 16, 24), (4, 8, 12), (2, 4, 5), (10, 4, 6), (0, 8, 12)],
    'masked': [(16, 8, 12), (16, 18, 20), (16, 8, 8), (0, 8, 12), (16, 0, 12), (16, 8, 0)],
    'forced': [('m8', (16, 8, 12)), ('m4', (17, 8, 12)), ('m4', (16, 9, 12)), ('m2', (16, 8, 11)),
               ('m4', (16, 8, 12)), ('m2', (16, 8, 12)), ('m4', (16, 20, 12)), ('m4', (16, 18, 24)),
               ('m1', (16, 17, 18)), ('m2', (16, 18, 20)), ('m4', (16, 22, 12))],
}
WIDEN_W = {   # vs2 is the wide group
    'plain': [(16, 16, 12), (16, 24, 12), (16, 16, 8), (16, 24, 8), (16, 24, 16), (16, 24, 18), (16, 16, 18),
              (8, 16, 4), (4, 8, 12), (2, 4, 5), (10, 4, 6)],
    'masked': [(16, 16, 12), (16, 24, 12), (0, 8, 12), (16, 0, 12)],
    'forced': [('m8', (16, 16, 12)), ('m4', (16, 16, 12)), ('m4', (17, 16, 12)), ('m4', (16, 18, 12)),
               ('m4', (16, 24, 20)), ('m2', (16, 18, 16)), ('m2', (16, 18, 18)), ('m4', (16, 24, 18))],
}
NARROW = {   # vs2 is the wide group
    'plain': [(8, 16, 24), (16, 16, 8), (8, 8, 24), (8, 16, 8), (8, 16, 16), (8, 16, 12), (9, 16, 24),
              (2, 4, 8), (1, 2, 3), (4, 4, 6), (4, 6, 4), (0, 16, 24), (8, 0, 24)],
    'masked': [(8, 16, 24), (16, 16, 8), (0, 16, 24), (8, 0, 24), (8, 16, 0)],
    'forced': [('m8', (8, 16, 24)), ('m4', (9, 16, 24)), ('m4', (8, 18, 24)), ('m4', (10, 16, 24)),
               ('m4', (8, 16, 24)), ('m2', (8, 16, 24)), ('m4', (12, 16, 24)), ('m4', (8, 8, 24)),
               ('m4', (16, 16, 8)), ('m4', (18, 16, 24)), ('m1', (2, 2, 3))],
}
EXTEND = {   # (vd, vs2); factor is the divisor
    'plain': [(8, 16), (16, 8), (8, 8), (16, 18), (16, 17), (16, 20), (4, 8), (2, 3), (5, 4), (0, 16), (8, 0), (31, 30)],
    'masked': [(8, 16), (16, 8), (0, 16), (16, 18), (8, 0)],
    'forced': [('m8', (8, 16)), ('m4', (8, 16)), ('m2', (8, 16)), ('m8', (16, 8)), ('m4', (17, 8)),
               ('m4', (16, 9)), ('m2', (16, 17)), ('m2', (18, 16)), ('m4', (16, 20)), ('m4', (16, 19)),
               ('m1', (16, 17)), ('m2', (16, 18)), ('m4', (16, 18))],
}
MASKDEST = {   # vd is a single mask register
    'plain': [(2, 8, 16), (8, 8, 16), (9, 8, 16), (16, 8, 16), (8, 16, 8), (0, 8, 16), (1, 0, 16), (1, 8, 0),
              (3, 4, 5), (7, 7, 7), (31, 30, 29), (10, 8, 24), (17, 8, 16), (9, 8, 9)],
    'masked': [(2, 8, 16), (0, 8, 16), (8, 8, 16), (9, 8, 16), (1, 0, 16), (3, 4, 5)],
    'forced': [('m2', (9, 8, 16)), ('m2', (8, 9, 16)), ('m4', (9, 8, 16)), ('m4', (10, 8, 16)),
               ('m8', (9, 8, 16)), ('m2', (8, 8, 16)), ('m2', (16, 8, 16)), ('m2', (2, 8, 16)),
               ('m4', (11, 8, 16)), ('m8', (15, 8, 16)), ('m8', (8, 8, 16))],
}


def vector_line(fname, mn, form, pat, masked, tag='', rv64=False, ops=None):
    """One instruction of the (vd, vs2, vs1 / scalar / immediate) form."""
    d, s2, s1 = pat
    if form in ('vv', 'wv', 'vvm', 'v', 'vs', 'vm'):
        src = 'v%d' % s1
    elif form in ('vx', 'wx', 'vxm', 'x'):
        src = tag_x(s1)
    elif form in ('vi', 'wi', 'vim', 'i'):
        src = str(s1)
    else:
        raise ValueError(form)
    add(fname, '%s v%d, v%d, %s%s' % (mn, d, s2, src, mask_suffix(masked)), tag, rv64)


def tag_x(n):
    return XREGS[n % len(XREGS)] if n < 2 else 'a%d' % (n % 4 + 1)


def build_binary(fname, mn, form, shape, rv64=False, immediates=None, scalar_regs=XREGS, masked=True, carry=None):
    """All the register patterns of a vd, vs2, vs1/x/imm instruction."""
    pats = shape['plain']
    if form in ('vv', 'wv'):
        for p in pats:
            add(fname, '%s v%d, v%d, v%d' % (mn, p[0], p[1], p[2]), '', rv64)
        if masked:
            for p in shape['masked']:
                add(fname, '%s v%d, v%d, v%d, v0.t' % (mn, p[0], p[1], p[2]), '', rv64)
        for lm, p in shape['forced']:
            add(fname, '%s v%d, v%d, v%d' % (mn, p[0], p[1], p[2]), '@state: lmul=%s' % lm, rv64)
    elif form in ('vx', 'wx'):
        for p in pats[:7] + pats[-2:]:
            for x in scalar_regs:
                add(fname, '%s v%d, v%d, %s' % (mn, p[0], p[1], x), '', rv64)
        if masked:
            for p in shape['masked'][:4]:
                add(fname, '%s v%d, v%d, a1, v0.t' % (mn, p[0], p[1]), '', rv64)
        for lm, p in shape['forced'][:6]:
            add(fname, '%s v%d, v%d, a1' % (mn, p[0], p[1]), '@state: lmul=%s' % lm, rv64)
    else:
        for p in pats[:6]:
            for imm in immediates:
                add(fname, '%s v%d, v%d, %d' % (mn, p[0], p[1], imm), '', rv64)
        if masked:
            for p in shape['masked'][:3]:
                for imm in immediates[:3]:
                    add(fname, '%s v%d, v%d, %d, v0.t' % (mn, p[0], p[1], imm), '', rv64)
        for lm, p in shape['forced'][:4]:
            add(fname, '%s v%d, v%d, %d' % (mn, p[0], p[1], immediates[0]), '@state: lmul=%s' % lm, rv64)


# ---------------------------------------------------------------------------------------------------------
# The families

def gen_config():
    f = 'config'
    sews = ['e8', 'e16', 'e32', 'e64']
    lmuls = ['mf8', 'mf4', 'mf2', 'm1', 'm2', 'm4', 'm8']
    for s in sews:
        for l in lmuls:
            for ta, ma in (('ta', 'ma'), ('tu', 'mu'), ('ta', 'mu'), ('tu', 'ma')):
                for rd, rs in (('a0', 'a1'), ('a0', 'zero'), ('zero', 'a1'), ('zero', 'zero')):
                    # (a few of the policy / register combinations of every configuration)
                    if (ta, ma) != ('ta', 'ma') and (rd, rs) != ('a0', 'a1') and s != 'e32':
                        continue
                    add(f, 'vsetvli %s, %s, %s, %s, %s, %s' % (rd, rs, s, l, ta, ma))
            for rd in ('a0', 'zero'):
                for uimm in (0, 1, 2, 3, 5, 8, 16, 17, 31):
                    add(f, 'vsetivli %s, %d, %s, %s, ta, ma' % (rd, uimm, s, l))
    for ta, ma in (('tu', 'mu'), ('ta', 'mu')):
        for uimm in (0, 7, 31):
            add(f, 'vsetivli a0, %d, e16, m2, %s, %s' % (uimm, ta, ma))
    # raw vtype immediates: reserved sew / lmul, the policy bits, the bits above
    for raw in (0x0, 0x4, 0x20, 0x38, 0x3b, 0x40, 0x80, 0xc0, 0xff, 0x100, 0x1c0, 0x400, 0x5, 0x2d, 0x1d, 0x23, 0x18):
        for rd, rs in (('a0', 'a1'), ('zero', 'zero'), ('a0', 'zero')):
            add(f, 'vsetvli %s, %s, %d' % (rd, rs, raw))
    for raw in (0x0, 0x4, 0x20, 0x38, 0x3b, 0xc0, 0xff, 0x100, 0x3ff):
        for rd in ('a0', 'zero'):
            add(f, 'vsetivli %s, 9, %d' % (rd, raw))
    for rd, rs1, rs2 in (('a0', 'a1', 'a2'), ('a0', 'zero', 'a2'), ('zero', 'a1', 'a2'), ('zero', 'zero', 'a2'),
                         ('a0', 'a1', 'zero'), ('a0', 'a1', 'a1'), ('a1', 'a1', 'a2'), ('a2', 'a1', 'a2'),
                         ('a0', 'zero', 'a0'), ('zero', 'zero', 'zero')):
        for k in range(6):
            add(f, 'vsetvl %s, %s, %s' % (rd, rs1, rs2), '@state: x11=%s' % ('%x' % [0, 1, 5, 17, 100, 0xffffffffffffffff][k]))
    # the vtype in the registers is drawn by the generator (valid and not), the avl too
    for rd, rs1, rs2 in (('a0', 'a1', 'a2'), ('zero', 'zero', 'a2'), ('a0', 'zero', 'a2')):
        for k in range(6):
            add(f, 'vsetvl %s, %s, %s' % (rd, rs1, rs2))


def gen_integer():
    f = 'int'
    vvx = ['vv', 'vx']
    vvxi = ['vv', 'vx', 'vi']
    simple = (('vadd', vvxi, SIMM), ('vsub', vvx, None), ('vrsub', ['vx', 'vi'], SIMM), ('vminu', vvx, None),
              ('vmin', vvx, None), ('vmaxu', vvx, None), ('vmax', vvx, None), ('vand', vvxi, SIMM),
              ('vor', vvxi, SIMM), ('vxor', vvxi, SIMM), ('vsll', vvxi, UIMM), ('vsrl', vvxi, UIMM),
              ('vsra', vvxi, UIMM), ('vmul', vvx, None), ('vmulh', vvx, None), ('vmulhu', vvx, None),
              ('vmulhsu', vvx, None), ('vdivu', vvx, None), ('vdiv', vvx, None), ('vremu', vvx, None),
              ('vrem', vvx, None))
    for mn, forms, imms in simple:
        for form in forms:
            build_binary(f, mn + '.' + form, form, SAME, immediates=imms)
    # widening
    for mn in ('vwaddu', 'vwadd', 'vwsubu', 'vwsub', 'vwmul', 'vwmulu', 'vwmulsu'):
        build_binary(f, mn + '.vv', 'vv', WIDEN)
        build_binary(f, mn + '.vx', 'vx', WIDEN)
    for mn in ('vwaddu', 'vwadd', 'vwsubu', 'vwsub'):
        build_binary(f, mn + '.wv', 'wv', WIDEN_W)
        build_binary(f, mn + '.wx', 'wx', WIDEN_W)
    # narrowing (the shift amount of the vi form is the 5 bit unsigned immediate)
    for mn in ('vnsrl', 'vnsra'):
        build_binary(f, mn + '.wv', 'wv', NARROW)
        build_binary(f, mn + '.wx', 'wx', NARROW)
        build_binary(f, mn + '.wi', 'wi', NARROW, immediates=UIMM)
    # multiply-add: vd, vs1/rs1, vs2
    for mn in ('vmacc', 'vnmsac', 'vmadd', 'vnmsub'):
        for p in SAME['plain'][:9]:
            add(f, '%s.vv v%d, v%d, v%d' % (mn, p[0], p[2], p[1]))
            add(f, '%s.vx v%d, a1, v%d' % (mn, p[0], p[1]))
        for p in SAME['masked'][:5]:
            add(f, '%s.vv v%d, v%d, v%d, v0.t' % (mn, p[0], p[2], p[1]))
            add(f, '%s.vx v%d, a1, v%d, v0.t' % (mn, p[0], p[1]))
        add(f, '%s.vx v%d, zero, v%d' % (mn, 8, 16))
        for lm, p in SAME['forced'][:5]:
            add(f, '%s.vv v%d, v%d, v%d' % (mn, p[0], p[2], p[1]), '@state: lmul=%s' % lm)
    for mn in ('vwmaccu', 'vwmacc', 'vwmaccsu'):
        for p in WIDEN['plain']:
            add(f, '%s.vv v%d, v%d, v%d' % (mn, p[0], p[2], p[1]))
            add(f, '%s.vx v%d, a1, v%d' % (mn, p[0], p[1]))
        for p in WIDEN['masked']:
            add(f, '%s.vv v%d, v%d, v%d, v0.t' % (mn, p[0], p[2], p[1]))
            add(f, '%s.vx v%d, a1, v%d, v0.t' % (mn, p[0], p[1]))
        for lm, p in WIDEN['forced']:
            add(f, '%s.vv v%d, v%d, v%d' % (mn, p[0], p[2], p[1]), '@state: lmul=%s' % lm)
    for p in WIDEN['plain']:
        add(f, 'vwmaccus.vx v%d, a1, v%d' % (p[0], p[1]))
    for p in WIDEN['masked']:
        add(f, 'vwmaccus.vx v%d, a1, v%d, v0.t' % (p[0], p[1]))
    for lm, p in WIDEN['forced'][:5]:
        add(f, 'vwmaccus.vx v%d, a1, v%d' % (p[0], p[1]), '@state: lmul=%s' % lm)
    # extension
    for fac in (2, 4, 8):
        for mn in ('vzext', 'vsext'):
            for p in EXTEND['plain']:
                add(f, '%s.vf%d v%d, v%d' % (mn, fac, p[0], p[1]))
            for p in EXTEND['masked']:
                add(f, '%s.vf%d v%d, v%d, v0.t' % (mn, fac, p[0], p[1]))
            for lm, p in EXTEND['forced']:
                add(f, '%s.vf%d v%d, v%d' % (mn, fac, p[0], p[1]), '@state: lmul=%s' % lm)
    # carries
    for mn, forms in (('vadc', ['vvm', 'vxm', 'vim']), ('vsbc', ['vvm', 'vxm'])):
        for form in forms:
            for p in SAME['plain'][:12] + [(8, 16, 0)]:
                src = {'vvm': 'v%d' % p[2], 'vxm': 'a1', 'vim': '%d' % SIMM[p[2] % 5]}[form]
                add(f, '%s.%s v%d, v%d, %s, v0' % (mn, form, p[0], p[1], src))
            for lm, p in SAME['forced'][:5]:
                src = {'vvm': 'v%d' % p[2], 'vxm': 'a1', 'vim': '3'}[form]
                add(f, '%s.%s v%d, v%d, %s, v0' % (mn, form, p[0], p[1], src), '@state: lmul=%s' % lm)
    for mn, forms in (('vmadc', ['vvm', 'vxm', 'vim', 'vv', 'vx', 'vi']), ('vmsbc', ['vvm', 'vxm', 'vv', 'vx'])):
        for form in forms:
            for p in MASKDEST['plain']:
                src = {'v': 'v%d' % p[2], 'x': 'a1', 'i': '%d' % SIMM[p[2] % 5]}[form[1]]
                tail = ', v0' if form.endswith('m') else ''
                add(f, '%s.%s v%d, v%d, %s%s' % (mn, form, p[0], p[1], src, tail))
            for lm, p in MASKDEST['forced'][:6]:
                src = {'v': 'v%d' % p[2], 'x': 'a1', 'i': '3'}[form[1]]
                tail = ', v0' if form.endswith('m') else ''
                add(f, '%s.%s v%d, v%d, %s%s' % (mn, form, p[0], p[1], src, tail), '@state: lmul=%s' % lm)
    for form, srcf in (('vvm', lambda p: 'v%d' % p[2]), ('vxm', lambda p: 'a1'), ('vim', lambda p: str(SIMM[p[2] % 5]))):
        for p in SAME['plain'][:12]:
            add(f, 'vmerge.%s v%d, v%d, %s, v0' % (form, p[0], p[1], srcf(p)))
        for lm, p in SAME['forced'][:5]:
            add(f, 'vmerge.%s v%d, v%d, %s, v0' % (form, p[0], p[1], srcf(p)), '@state: lmul=%s' % lm)
    for p in ((8, 24), (8, 8), (0, 16), (1, 2), (31, 30), (16, 8), (9, 8)):
        add(f, 'vmv.v.v v%d, v%d' % (p[0], p[1]))
    for lm, p in (('m2', (9, 8)), ('m4', (8, 10)), ('m8', (8, 20)), ('m2', (8, 9))):
        add(f, 'vmv.v.v v%d, v%d' % (p[0], p[1]), '@state: lmul=%s' % lm)
        add(f, 'vmv.v.x v%d, a1' % p[0], '@state: lmul=%s' % lm)
    for r in (8, 0, 1, 16, 31, 24):
        add(f, 'vmv.v.x v%d, a1' % r)
        add(f, 'vmv.v.x v%d, zero' % r)
        for imm in SIMM:
            add(f, 'vmv.v.i v%d, %d' % (r, imm))
    # compares
    for mn, forms, imms in (('vmseq', vvxi, SIMM), ('vmsne', vvxi, SIMM), ('vmsltu', vvx, None), ('vmslt', vvx, None),
                            ('vmsleu', vvxi, SIMM), ('vmsle', vvxi, SIMM), ('vmsgtu', ['vx', 'vi'], SIMM),
                            ('vmsgt', ['vx', 'vi'], SIMM)):
        for form in forms:
            build_binary(f, mn + '.' + form, form, MASKDEST, immediates=imms)
    # the aliases (they are lifted by the general encoding they print for)
    for p in SAME['plain'][:6]:
        add(f, 'vneg.v v%d, v%d' % (p[0], p[1]))
        add(f, 'vnot.v v%d, v%d' % (p[0], p[1]))
        add(f, 'vneg.v v%d, v%d, v0.t' % (p[0], p[1]))
        add(f, 'vnot.v v%d, v%d, v0.t' % (p[0], p[1]))
    for p in WIDEN['plain'][:8]:
        add(f, 'vwcvt.x.x.v v%d, v%d' % (p[0], p[1]))
        add(f, 'vwcvtu.x.x.v v%d, v%d' % (p[0], p[1]))
        add(f, 'vwcvt.x.x.v v%d, v%d, v0.t' % (p[0], p[1]))
    for p in NARROW['plain'][:8]:
        add(f, 'vncvt.x.x.w v%d, v%d' % (p[0], p[1]))
        add(f, 'vncvt.x.x.w v%d, v%d, v0.t' % (p[0], p[1]))
    # vid
    for r in (8, 16, 0, 1, 31, 24, 9):
        add(f, 'vid.v v%d' % r)
    for r in (8, 16, 1, 31, 24, 9):
        add(f, 'vid.v v%d, v0.t' % r)
    for lm, r in (('m2', 9), ('m4', 10), ('m8', 12), ('m2', 8), ('m8', 8), ('m4', 8)):
        add(f, 'vid.v v%d' % r, '@state: lmul=%s' % lm)
    add(f, 'vid.v v0, v0.t')


def gen_fixed():
    f = 'fixed'
    vvx = ['vv', 'vx']
    vvxi = ['vv', 'vx', 'vi']
    for mn, forms, imms in (('vsaddu', vvxi, SIMM), ('vsadd', vvxi, SIMM), ('vssubu', vvx, None),
                            ('vssub', vvx, None), ('vsmul', vvx, None), ('vaaddu', vvx, None), ('vaadd', vvx, None),
                            ('vasubu', vvx, None), ('vasub', vvx, None), ('vssrl', vvxi, UIMM), ('vssra', vvxi, UIMM)):
        for form in forms:
            build_binary(f, mn + '.' + form, form, SAME, immediates=imms)
    for mn in ('vnclipu', 'vnclip'):
        build_binary(f, mn + '.wv', 'wv', NARROW)
        build_binary(f, mn + '.wx', 'wx', NARROW)
        build_binary(f, mn + '.wi', 'wi', NARROW, immediates=UIMM)
    # the rounding modes and saturating edge values are in the states (vxrm is random, the elements are
    # the boundary values 25% of the time); a few directed lines with all four modes
    for mode in range(4):
        for mn in ('vaadd.vv', 'vasubu.vv', 'vsmul.vv', 'vssra.vv', 'vssrl.vv', 'vaaddu.vv', 'vasub.vv'):
            add(f, '%s v8, v16, v24' % mn, '@state: vxrm=%d' % mode)
        for mn in ('vnclip.wv', 'vnclipu.wv'):
            add(f, '%s v8, v16, v24' % mn, '@state: vxrm=%d' % mode)
        add(f, 'vssra.vi v8, v16, 3', '@state: vxrm=%d' % mode)
        add(f, 'vnclip.wi v8, v16, 1', '@state: vxrm=%d' % mode)


def gen_mask():
    f = 'mask'
    pats = [(1, 2, 3), (0, 2, 3), (1, 1, 1), (1, 2, 2), (1, 2, 1), (8, 16, 24), (31, 0, 1), (2, 0, 0), (0, 0, 0)]
    for mn in ('vmand.mm', 'vmnand.mm', 'vmandn.mm', 'vmxor.mm', 'vmor.mm', 'vmnor.mm', 'vmorn.mm', 'vmxnor.mm'):
        for p in pats:
            add(f, '%s v%d, v%d, v%d' % (mn, p[0], p[1], p[2]))
    add(f, 'vmmv.m v2, v3')
    add(f, 'vmclr.m v2')
    add(f, 'vmset.m v2')
    add(f, 'vmnot.m v2, v3')
    # scalar results
    for mn in ('vcpop.m', 'vfirst.m'):
        for rd in ('a0', 'zero', 'a1'):
            for s in (2, 0, 8, 31):
                add(f, '%s %s, v%d' % (mn, rd, s))
                add(f, '%s %s, v%d, v0.t' % (mn, rd, s))
    for mn in ('vmsbf.m', 'vmsif.m', 'vmsof.m'):
        for p in ((2, 3), (1, 1), (0, 3), (8, 16), (31, 30), (3, 0), (2, 2)):
            add(f, '%s v%d, v%d' % (mn, p[0], p[1]))
        for p in ((2, 3), (1, 1), (0, 3), (8, 16), (3, 0), (4, 5)):
            add(f, '%s v%d, v%d, v0.t' % (mn, p[0], p[1]))
    for p in ((8, 2), (16, 3), (8, 8), (9, 8), (0, 3), (24, 0), (31, 30), (4, 2), (2, 4), (1, 0)):
        add(f, 'viota.m v%d, v%d' % p)
    for p in ((8, 2), (16, 3), (0, 3), (9, 3), (24, 0), (4, 5)):
        add(f, 'viota.m v%d, v%d, v0.t' % p)
    for lm, p in (('m2', (9, 3)), ('m4', (10, 3)), ('m8', (12, 3)), ('m2', (8, 8)), ('m4', (8, 9)),
                  ('m4', (8, 11)), ('m8', (16, 18)), ('m2', (8, 9)), ('m1', (8, 8))):
        add(f, 'viota.m v%d, v%d' % p, '@state: lmul=%s' % lm)


def gen_permute():
    f = 'perm'
    for mn, imms in (('vslideup', UIMM), ('vslidedown', UIMM)):
        for p in SAME['plain'][:9] + [(8, 16, 0)]:
            for x in ('a1', 'a2', 'zero'):
                add(f, '%s.vx v%d, v%d, %s' % (mn, p[0], p[1], x))
            for imm in imms:
                add(f, '%s.vi v%d, v%d, %d' % (mn, p[0], p[1], imm))
        for p in SAME['masked'][:6]:
            add(f, '%s.vx v%d, v%d, a1, v0.t' % (mn, p[0], p[1]))
            add(f, '%s.vi v%d, v%d, 3, v0.t' % (mn, p[0], p[1]))
        for lm, p in SAME['forced'][:7]:
            add(f, '%s.vx v%d, v%d, a1' % (mn, p[0], p[1]), '@state: lmul=%s' % lm)
    for mn in ('vslide1up', 'vslide1down'):
        for p in SAME['plain'][:9]:
            add(f, '%s.vx v%d, v%d, a1' % (mn, p[0], p[1]))
            add(f, '%s.vx v%d, v%d, zero' % (mn, p[0], p[1]))
        for p in SAME['masked'][:6]:
            add(f, '%s.vx v%d, v%d, a1, v0.t' % (mn, p[0], p[1]))
        for lm, p in SAME['forced'][:6]:
            add(f, '%s.vx v%d, v%d, a1' % (mn, p[0], p[1]), '@state: lmul=%s' % lm)
    # the slide offsets: 0, small, the vl, beyond VLMAX
    for off in (0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 63, 64, 100, 255, 256, 511, 512, 1000, '-1'):
        for mn in ('vslideup', 'vslidedown'):
            add(f, '%s.vx v8, v16, a1' % mn, '@state: x11=%s' % ('ffffffffffffffff' if off == '-1' else '%x' % off))
    # gather
    for p in SAME['plain'][:9] + [(8, 16, 0)]:
        add(f, 'vrgather.vv v%d, v%d, v%d' % p)
        for x in ('a1', 'zero'):
            add(f, 'vrgather.vx v%d, v%d, %s' % (p[0], p[1], x))
        for imm in UIMM:
            add(f, 'vrgather.vi v%d, v%d, %d' % (p[0], p[1], imm))
    for p in SAME['masked'][:6]:
        add(f, 'vrgather.vv v%d, v%d, v%d, v0.t' % p)
        add(f, 'vrgather.vx v%d, v%d, a1, v0.t' % (p[0], p[1]))
        add(f, 'vrgather.vi v%d, v%d, 2, v0.t' % (p[0], p[1]))
    for lm, p in SAME['forced'][:7]:
        add(f, 'vrgather.vv v%d, v%d, v%d' % p, '@state: lmul=%s' % lm)
        add(f, 'vrgather.vx v%d, v%d, a1' % (p[0], p[1]), '@state: lmul=%s' % lm)
    for p in SAME['plain'][:9] + [(8, 16, 0), (8, 16, 20), (8, 16, 9)]:
        add(f, 'vrgatherei16.vv v%d, v%d, v%d' % p)
    for p in SAME['masked'][:6]:
        add(f, 'vrgatherei16.vv v%d, v%d, v%d, v0.t' % p)
    for lm, p in SAME['forced'][:7] + [('m2', (8, 16, 20)), ('m4', (8, 16, 20)), ('m1', (8, 16, 20))]:
        add(f, 'vrgatherei16.vv v%d, v%d, v%d' % p, '@state: lmul=%s' % lm)
    # the gather indexes: small, the vl, beyond (the indexed data of the states is 0..255; a few directed)
    for x in (0, 1, 5, 15, 16, 31, 63, 64, 127, 128, 255, 256, 1000, '-1'):
        add(f, 'vrgather.vx v8, v16, a1', '@state: x11=%s' % ('ffffffffffffffff' if x == '-1' else '%x' % x))
    # compress
    for p in ((8, 16, 0), (8, 16, 1), (8, 16, 2), (8, 8, 1), (9, 16, 1), (1, 2, 3), (0, 16, 1), (8, 0, 1), (24, 8, 0),
              (31, 30, 29), (8, 16, 8), (8, 16, 9), (16, 24, 17)):
        add(f, 'vcompress.vm v%d, v%d, v%d' % p)
    for lm, p in (('m2', (8, 16, 9)), ('m4', (8, 16, 9)), ('m8', (8, 16, 9)), ('m2', (9, 16, 1)), ('m2', (8, 17, 1)),
                  ('m2', (8, 8, 1)), ('m4', (8, 16, 10)), ('m2', (8, 16, 8)), ('m4', (8, 16, 11))):
        add(f, 'vcompress.vm v%d, v%d, v%d' % p, '@state: lmul=%s' % lm)
    # scalar moves
    for rd in ('a0', 'zero', 'a1'):
        for s in (8, 0, 31, 9):
            add(f, 'vmv.x.s %s, v%d' % (rd, s))
    for rs in ('a1', 'zero', 'a2'):
        for d in (8, 0, 31, 9):
            add(f, 'vmv.s.x v%d, %s' % (d, rs))
    # whole register moves
    for n in (1, 2, 4, 8):
        for p in ((8, 16), (16, 8), (8, 8), (0, 8), (8, 0), (24, 16), (1, 2), (3, 4), (9, 8), (12, 8), (10, 8), (8, 9)):
            if p[0] % n == 0 and p[1] % n == 0:
                add(f, 'vmv%dr.v v%d, v%d' % (n, p[0], p[1]))
    for lm in ('m1', 'm2', 'm4', 'm8', 'mf2'):
        for n in (1, 2, 4, 8):
            add(f, 'vmv%dr.v v16, v8' % n, '@state: lmul=%s' % lm)
    # reductions
    for mn in ('vredsum', 'vredand', 'vredor', 'vredxor', 'vredminu', 'vredmin', 'vredmaxu', 'vredmax'):
        for p in ((8, 16, 24), (8, 8, 24), (8, 16, 8), (8, 16, 16), (0, 16, 24), (8, 0, 24), (8, 16, 0), (1, 2, 3),
                  (31, 30, 29), (9, 16, 24), (8, 16, 25), (2, 8, 3), (3, 10, 3)):
            add(f, '%s.vs v%d, v%d, v%d' % (mn, p[0], p[1], p[2]))
        for p in ((8, 16, 24), (0, 16, 24), (8, 0, 24), (1, 2, 3), (8, 16, 0)):
            add(f, '%s.vs v%d, v%d, v%d, v0.t' % (mn, p[0], p[1], p[2]))
        for lm, p in (('m2', (8, 17, 24)), ('m2', (9, 16, 24)), ('m8', (8, 20, 24)), ('m4', (8, 18, 24))):
            add(f, '%s.vs v%d, v%d, v%d' % (mn, p[0], p[1], p[2]), '@state: lmul=%s' % lm)
    for mn in ('vwredsumu', 'vwredsum'):
        for p in ((8, 16, 24), (8, 8, 24), (8, 16, 8), (0, 16, 24), (8, 0, 24), (1, 2, 3), (31, 30, 29), (9, 16, 24)):
            add(f, '%s.vs v%d, v%d, v%d' % (mn, p[0], p[1], p[2]))
        for p in ((8, 16, 24), (0, 16, 24), (1, 2, 3)):
            add(f, '%s.vs v%d, v%d, v%d, v0.t' % (mn, p[0], p[1], p[2]))
        for lm, p in (('m2', (8, 17, 24)), ('m8', (8, 16, 24)), ('m4', (8, 18, 24))):
            add(f, '%s.vs v%d, v%d, v%d' % (mn, p[0], p[1], p[2]), '@state: lmul=%s' % lm)


# ---------------------------------------------------------------------------------------------------------
# The vector floating point (Zve32f / Zve64d, Zvfh, Zvfhmin, Zvfbfmin, Zvfbfwma)

FREGS = ['fa1', 'ft0']


def build_float_binary(f, mn, form, shape, masked=True):
    """vd, vs2, vs1 / fs1 forms of the floating point instructions (vv, vf, wv, wf)."""
    pats = shape['plain']
    if form in ('vv', 'wv'):
        for p in pats:
            add(f, '%s v%d, v%d, v%d' % (mn, p[0], p[1], p[2]))
        if masked:
            for p in shape['masked']:
                add(f, '%s v%d, v%d, v%d, v0.t' % (mn, p[0], p[1], p[2]))
        for lm, p in shape['forced']:
            add(f, '%s v%d, v%d, v%d' % (mn, p[0], p[1], p[2]), '@state: lmul=%s' % lm)
    else:
        for p in pats[:7] + pats[-2:]:
            for x in FREGS:
                add(f, '%s v%d, v%d, %s' % (mn, p[0], p[1], x))
        if masked:
            for p in shape['masked'][:4]:
                add(f, '%s v%d, v%d, fa1, v0.t' % (mn, p[0], p[1]))
        for lm, p in shape['forced'][:6]:
            add(f, '%s v%d, v%d, fa1' % (mn, p[0], p[1]), '@state: lmul=%s' % lm)


def build_float_unary(f, mn, shape, tag=''):
    """vd, vs2 forms: the pairs of the (vd, vs2, vs1) patterns."""
    for p in shape['plain']:
        add(f, '%s v%d, v%d' % (mn, p[0], p[1]), tag)
    for p in shape['masked']:
        add(f, '%s v%d, v%d, v0.t' % (mn, p[0], p[1]), tag)
    for lm, p in shape['forced']:
        add(f, '%s v%d, v%d' % (mn, p[0], p[1]), ('@state: lmul=%s ' % lm + tag).strip())


def build_float_fma(f, mn, form, shape):
    """vd, vs1 / fs1, vs2: the multiply-add operand order."""
    if form == 'vv':
        for p in shape['plain']:
            add(f, '%s.vv v%d, v%d, v%d' % (mn, p[0], p[2], p[1]))
        for p in shape['masked']:
            add(f, '%s.vv v%d, v%d, v%d, v0.t' % (mn, p[0], p[2], p[1]))
        for lm, p in shape['forced']:
            add(f, '%s.vv v%d, v%d, v%d' % (mn, p[0], p[2], p[1]), '@state: lmul=%s' % lm)
    else:
        for p in shape['plain'][:8] + shape['plain'][-2:]:
            for x in FREGS:
                add(f, '%s.vf v%d, %s, v%d' % (mn, p[0], x, p[1]))
        for p in shape['masked'][:4]:
            add(f, '%s.vf v%d, fa1, v%d, v0.t' % (mn, p[0], p[1]))
        for lm, p in shape['forced'][:6]:
            add(f, '%s.vf v%d, fa1, v%d' % (mn, p[0], p[1]), '@state: lmul=%s' % lm)


# the directed rounding-mode lines: frm over the five modes for the instructions that round (and the reserved ones)
FRM_LINES = ('vfadd.vv v8, v16, v24', 'vfsub.vf v8, v16, fa1', 'vfmul.vv v8, v16, v24', 'vfdiv.vv v8, v16, v24',
             'vfmacc.vv v8, v16, v24', 'vfnmsub.vf v8, fa1, v16', 'vfsqrt.v v8, v16', 'vfrec7.v v8, v16',
             'vfcvt.x.f.v v8, v16', 'vfcvt.xu.f.v v8, v16', 'vfcvt.f.x.v v8, v16', 'vfwcvt.x.f.v v8, v16',
             'vfwcvt.f.x.v v8, v16', 'vfncvt.x.f.w v8, v16', 'vfncvt.f.f.w v8, v16', 'vfncvt.f.x.w v8, v16',
             'vfwadd.vv v8, v16, v24', 'vfwmul.vf v8, v16, fa1', 'vfwmacc.vv v8, v16, v24', 'vfredusum.vs v8, v16, v24',
             'vfredosum.vs v8, v16, v24', 'vfwredusum.vs v8, v16, v24', 'vfncvt.rod.f.f.w v8, v16',
             'vfcvt.rtz.x.f.v v8, v16', 'vfmin.vv v8, v16, v24', 'vfsgnj.vv v8, v16, v24', 'vmflt.vv v8, v16, v24',
             'vfclass.v v8, v16', 'vfrsqrt7.v v8, v16', 'vfwmaccbf16.vv v8, v16, v24', 'vfncvtbf16.f.f.w v8, v16',
             'vfwcvtbf16.f.f.v v8, v16', 'vfmerge.vfm v8, v16, fa1, v0', 'vfmv.v.f v8, fa1', 'vfslide1up.vf v8, v16, fa1',
             'vfredmax.vs v8, v16, v24', 'vfmv.s.f v8, fa1')


def gen_float():
    f = 'fadd'
    # the arithmetic, min and max, the sign injection
    for mn in ('vfadd', 'vfsub', 'vfmul', 'vfdiv', 'vfmin', 'vfmax', 'vfsgnj', 'vfsgnjn', 'vfsgnjx'):
        build_float_binary(f, mn + '.vv', 'vv', SAME)
        build_float_binary(f, mn + '.vf', 'vf', SAME)
    for mn in ('vfrsub', 'vfrdiv'):
        build_float_binary(f, mn + '.vf', 'vf', SAME)
    # the aliases
    for mn in ('vfneg.v', 'vfabs.v'):
        for p in SAME['plain'][:6]:
            add(f, '%s v%d, v%d' % (mn, p[0], p[1]))
            add(f, '%s v%d, v%d, v0.t' % (mn, p[0], p[1]))
    # multiply-add
    f = 'ffma'
    for mn in ('vfmacc', 'vfnmacc', 'vfmsac', 'vfnmsac', 'vfmadd', 'vfnmadd', 'vfmsub', 'vfnmsub'):
        build_float_fma(f, mn, 'vv', SAME)
        build_float_fma(f, mn, 'vf', SAME)
    for mn in ('vfwmacc', 'vfwnmacc', 'vfwmsac', 'vfwnmsac', 'vfwmaccbf16'):
        build_float_fma(f, mn, 'vv', WIDEN)
        build_float_fma(f, mn, 'vf', WIDEN)
    # widening
    f = 'fwide'
    for mn in ('vfwadd', 'vfwsub', 'vfwmul'):
        build_float_binary(f, mn + '.vv', 'vv', WIDEN)
        build_float_binary(f, mn + '.vf', 'vf', WIDEN)
    for mn in ('vfwadd', 'vfwsub'):
        build_float_binary(f, mn + '.wv', 'wv', WIDEN_W)
        build_float_binary(f, mn + '.wf', 'wf', WIDEN_W)
    # unary
    f = 'funary'
    for mn in ('vfsqrt.v', 'vfrsqrt7.v', 'vfrec7.v', 'vfclass.v'):
        build_float_unary(f, mn, SAME)
    # compares
    f = 'fcmp'
    for mn, forms in (('vmfeq', ('vv', 'vf')), ('vmfne', ('vv', 'vf')), ('vmflt', ('vv', 'vf')), ('vmfle', ('vv', 'vf')),
                      ('vmfgt', ('vf',)), ('vmfge', ('vf',))):
        for form in forms:
            build_float_binary(f, mn + '.' + form, form, MASKDEST)
    # merge and moves
    f = 'fmove'
    for p in SAME['plain'][:12]:
        for x in FREGS:
            add(f, 'vfmerge.vfm v%d, v%d, %s, v0' % (p[0], p[1], x))
    for lm, p in SAME['forced'][:5]:
        add(f, 'vfmerge.vfm v%d, v%d, fa1, v0' % (p[0], p[1]), '@state: lmul=%s' % lm)
    for r in (8, 0, 1, 16, 31, 24, 9):
        for x in FREGS:
            add(f, 'vfmv.v.f v%d, %s' % (r, x))
    for lm, r in (('m2', 9), ('m4', 10), ('m8', 12), ('m2', 8), ('m8', 8), ('m4', 8)):
        add(f, 'vfmv.v.f v%d, fa1' % r, '@state: lmul=%s' % lm)
    for fd in ('fa0', 'ft0', 'fs1'):
        for s2 in (8, 0, 31, 9):
            add(f, 'vfmv.f.s %s, v%d' % (fd, s2))
    for fs in ('fa1', 'ft0', 'fa2'):
        for d in (8, 0, 31, 9):
            add(f, 'vfmv.s.f v%d, %s' % (d, fs))
    for mn in ('vfslide1up', 'vfslide1down'):
        for p in SAME['plain'][:9]:
            add(f, '%s.vf v%d, v%d, fa1' % (mn, p[0], p[1]))
        for p in SAME['masked'][:6]:
            add(f, '%s.vf v%d, v%d, fa1, v0.t' % (mn, p[0], p[1]))
        for lm, p in SAME['forced'][:6]:
            add(f, '%s.vf v%d, v%d, fa1' % (mn, p[0], p[1]), '@state: lmul=%s' % lm)
    # conversions
    f = 'fcvt'
    same_cvt = ('vfcvt.xu.f.v', 'vfcvt.x.f.v', 'vfcvt.f.xu.v', 'vfcvt.f.x.v', 'vfcvt.rtz.xu.f.v', 'vfcvt.rtz.x.f.v')
    for mn in same_cvt:
        build_float_unary(f, mn, SAME)
    for mn in ('vfwcvt.xu.f.v', 'vfwcvt.x.f.v', 'vfwcvt.f.xu.v', 'vfwcvt.f.x.v', 'vfwcvt.f.f.v', 'vfwcvt.rtz.xu.f.v',
               'vfwcvt.rtz.x.f.v', 'vfwcvtbf16.f.f.v'):
        build_float_unary(f, mn, WIDEN)
    for mn in ('vfncvt.xu.f.w', 'vfncvt.x.f.w', 'vfncvt.f.xu.w', 'vfncvt.f.x.w', 'vfncvt.f.f.w', 'vfncvt.rod.f.f.w',
               'vfncvt.rtz.xu.f.w', 'vfncvt.rtz.x.f.w', 'vfncvtbf16.f.f.w'):
        build_float_unary(f, mn, NARROW)
    # reductions
    f = 'fred'
    for mn in ('vfredosum', 'vfredusum', 'vfredmax', 'vfredmin'):
        for p in ((8, 16, 24), (8, 8, 24), (8, 16, 8), (8, 16, 16), (0, 16, 24), (8, 0, 24), (8, 16, 0), (1, 2, 3),
                  (31, 30, 29), (9, 16, 24), (8, 16, 25), (2, 8, 3), (3, 10, 3)):
            add(f, '%s.vs v%d, v%d, v%d' % (mn, p[0], p[1], p[2]))
        for p in ((8, 16, 24), (0, 16, 24), (8, 0, 24), (1, 2, 3), (8, 16, 0)):
            add(f, '%s.vs v%d, v%d, v%d, v0.t' % (mn, p[0], p[1], p[2]))
        for lm, p in (('m2', (8, 17, 24)), ('m2', (9, 16, 24)), ('m8', (8, 20, 24)), ('m4', (8, 18, 24))):
            add(f, '%s.vs v%d, v%d, v%d' % (mn, p[0], p[1], p[2]), '@state: lmul=%s' % lm)
    for mn in ('vfwredosum', 'vfwredusum'):
        for p in ((8, 16, 24), (8, 8, 24), (8, 16, 8), (0, 16, 24), (8, 0, 24), (1, 2, 3), (31, 30, 29), (9, 16, 24)):
            add(f, '%s.vs v%d, v%d, v%d' % (mn, p[0], p[1], p[2]))
        for p in ((8, 16, 24), (0, 16, 24), (1, 2, 3)):
            add(f, '%s.vs v%d, v%d, v%d, v0.t' % (mn, p[0], p[1], p[2]))
        for lm, p in (('m2', (8, 17, 24)), ('m8', (8, 16, 24)), ('m4', (8, 18, 24))):
            add(f, '%s.vs v%d, v%d, v%d' % (mn, p[0], p[1], p[2]), '@state: lmul=%s' % lm)
    # the rounding modes, the reserved ones, and the elements that are not floating point (SEW 8)
    f = 'fdir'
    for line in FRM_LINES:
        for frm in range(5):
            add(f, line, '@state: frm=%d' % frm)
        add(f, line, '@state: frm=5')
        add(f, line, '@state: frm=7')
        add(f, line, '@state: sew=8')
    # a scalar which is not NaN boxed
    for sew, bad in ((16, 'ffffffff00004400'), (16, '000000000000bc00'), (32, '00000000403f0000'), (32, 'ffffffff7fc00000'),
                     (16, 'ffffffffffff4400'), (32, 'ffffffffffffffff')):
        for line in ('vfadd.vf v8, v16, fa1', 'vfmul.vf v8, v16, fa1', 'vfmerge.vfm v8, v16, fa1, v0', 'vfmv.v.f v8, fa1',
                     'vfsgnj.vf v8, v16, fa1', 'vfslide1up.vf v8, v16, fa1', 'vfmv.s.f v8, fa1', 'vfwadd.vf v8, v16, fa1',
                     'vfmacc.vf v8, fa1, v16', 'vmfeq.vf v8, v16, fa1'):
            add(f, line, '@state: sew=%d f11=%s' % (sew, bad))
    for bad in ('ffffffff00004400', 'ffffffffffff4000', 'ffffffffffff7fc0', '000000000000c000'):
        for line in ('vfwmaccbf16.vf v8, fa1, v16', 'vfmv.v.f v8, fa1'):
            add(f, line, '@state: sew=16 f11=%s' % bad)


# ---------------------------------------------------------------------------------------------------------
# Zvbb, Zvkb, Zvbc

def gen_bits():
    f = 'bits'
    for mn in ('vandn', 'vrol', 'vror'):
        build_binary(f, mn + '.vv', 'vv', SAME)
        build_binary(f, mn + '.vx', 'vx', SAME)
    build_binary(f, 'vror.vi', 'vi', SAME, immediates=[0, 1, 7, 31, 32, 33, 63])
    for mn in ('vwsll',):
        build_binary(f, mn + '.vv', 'vv', WIDEN)
        build_binary(f, mn + '.vx', 'vx', WIDEN)
        build_binary(f, mn + '.vi', 'vi', WIDEN, immediates=UIMM)
    for mn in ('vbrev8.v', 'vrev8.v', 'vbrev.v', 'vclz.v', 'vctz.v', 'vcpop.v'):
        for p in SAME['plain']:
            add(f, '%s v%d, v%d' % (mn, p[0], p[1]))
        for p in SAME['masked']:
            add(f, '%s v%d, v%d, v0.t' % (mn, p[0], p[1]))
        for lm, p in SAME['forced']:
            add(f, '%s v%d, v%d' % (mn, p[0], p[1]), '@state: lmul=%s' % lm)
    for mn in ('vclmul', 'vclmulh'):
        build_binary(f, mn + '.vv', 'vv', SAME)
        build_binary(f, mn + '.vx', 'vx', SAME)
        # (SEW 64 only: the states cycle through the SEWs, the others are illegal; a directed SEW 64 pass)
        for p in SAME['plain'][:6]:
            add(f, '%s.vv v%d, v%d, v%d' % (mn, p[0], p[1], p[2]), '@state: sew=64')
            add(f, '%s.vx v%d, v%d, a1' % (mn, p[0], p[1]), '@state: sew=64')


# ---------------------------------------------------------------------------------------------------------
# The vector cryptography (Zvkg, Zvkned, Zvknha / Zvknhb, Zvksed, Zvksh)

def gen_crypto():
    f = 'crypto'
    # vd, vs2 (.vv with a vs1, .vs and .vi without)
    vvs = ('vghsh.vv', 'vsha2ms.vv', 'vsha2ch.vv', 'vsha2cl.vv', 'vsm3me.vv')
    for mn in vvs:
        for p in SAME['plain'] + [(8, 16, 18), (16, 8, 12), (16, 24, 8), (8, 24, 16)]:
            add(f, '%s v%d, v%d, v%d' % (mn, p[0], p[1], p[2]))
        for lm, p in SAME['forced'] + [('m2', (8, 16, 24)), ('m2', (16, 8, 12)), ('m4', (8, 16, 24)), ('m4', (16, 8, 24))]:
            add(f, '%s v%d, v%d, v%d' % (mn, p[0], p[1], p[2]), '@state: lmul=%s' % lm)
        for p in SAME['masked'][:3]:
            # (the masked encodings don't exist for these: llvm-mc refuses them, they are skipped)
            pass
    twos = ('vgmul.vv', 'vaesef.vv', 'vaesem.vv', 'vaesdf.vv', 'vaesdm.vv', 'vaesef.vs', 'vaesem.vs', 'vaesdf.vs',
            'vaesdm.vs', 'vaesz.vs', 'vsm4r.vv', 'vsm4r.vs')
    for mn in twos:
        for p in SAME['plain'] + [(8, 12, 0), (16, 8, 0), (16, 20, 0), (12, 8, 0)]:
            add(f, '%s v%d, v%d' % (mn, p[0], p[1]))
        for lm, p in SAME['forced'] + [('m2', (8, 16, 0)), ('m2', (16, 8, 0)), ('m4', (8, 16, 0)), ('m4', (16, 8, 0)),
                                       ('m2', (8, 12, 0)), ('m8', (8, 16, 0))]:
            add(f, '%s v%d, v%d' % (mn, p[0], p[1]), '@state: lmul=%s' % lm)
    for mn in ('vaeskf1.vi', 'vaeskf2.vi', 'vsm4k.vi', 'vsm3c.vi'):
        for imm in range(32):
            add(f, '%s v8, v16, %d' % (mn, imm))
        for p in SAME['plain'][:9]:
            add(f, '%s v%d, v%d, 3' % (mn, p[0], p[1]))
        for lm, p in SAME['forced'][:8] + [('m2', (8, 16, 0)), ('m4', (16, 8, 0)), ('m2', (8, 12, 0))]:
            add(f, '%s v%d, v%d, 5' % (mn, p[0], p[1]), '@state: lmul=%s' % lm)
    # the vl / vstart that are not multiples of EGS, and the other SEWs
    for mn in vvs + twos:
        form = '%s v8, v16, v24' % mn if mn in vvs else '%s v8, v16' % mn
        for sew in (8, 16, 64):
            add(f, form, '@state: sew=%d' % sew)
        for vl in (1, 2, 3, 5, 6, 7):
            add(f, form, '@state: sew=32 lmul=m4 vlsel=%d' % vl)
        for vstart in (1, 2, 3, 5):
            add(f, form, '@state: sew=32 lmul=m4 vlsel=max vstart=%d' % vstart)


def gen_memory():
    f = 'mem'
    eews = (8, 16, 32, 64)
    # unit stride, fault only first, strided, indexed
    for e in eews:
        for d in (8, 16, 0, 1, 9, 24, 31):
            add(f, 'vle%d.v v%d, (a1)' % (e, d))
            add(f, 'vle%dff.v v%d, (a1)' % (e, d))
            add(f, 'vse%d.v v%d, (a1)' % (e, d))
            add(f, 'vlse%d.v v%d, (a1), a2' % (e, d))
            add(f, 'vsse%d.v v%d, (a1), a2' % (e, d))
        for d in (8, 16, 1, 0, 24):
            add(f, 'vle%d.v v%d, (a1), v0.t' % (e, d))
            add(f, 'vle%dff.v v%d, (a1), v0.t' % (e, d))
            add(f, 'vse%d.v v%d, (a1), v0.t' % (e, d))
            add(f, 'vlse%d.v v%d, (a1), a2, v0.t' % (e, d))
            add(f, 'vsse%d.v v%d, (a1), a2, v0.t' % (e, d))
        for lm in ('m1', 'm2', 'm8', 'mf2', 'mf8'):
            add(f, 'vle%d.v v8, (a1)' % e, '@state: lmul=%s vlsel=max' % lm)
            add(f, 'vse%d.v v8, (a1)' % e, '@state: lmul=%s vlsel=max' % lm)
            add(f, 'vlse%d.v v8, (a1), a2' % e, '@state: lmul=%s vlsel=max' % lm)
            add(f, 'vle%dff.v v8, (a1)' % e, '@state: lmul=%s vlsel=rand' % lm)
        add(f, 'vlse%d.v v8, (a1), zero' % e)
        add(f, 'vsse%d.v v8, (a1), zero' % e)
        for d in (8, 16, 0, 1, 9, 24, 12):
            for mn in ('vluxei', 'vloxei', 'vsuxei', 'vsoxei'):
                kind = (mn.startswith('vs'))
                add(f, '%s%d.v v%d, (a1), v%d' % (mn, e, d, 16 if d != 16 else 8), '', e == 64)
        for mn in ('vluxei', 'vloxei', 'vsuxei', 'vsoxei'):
            for d, s in ((8, 8), (8, 9), (8, 10), (9, 8), (8, 12), (12, 8), (16, 8), (8, 0), (0, 8), (1, 2), (2, 1), (30, 31),
                         (10, 8), (8, 11), (24, 26)):
                add(f, '%s%d.v v%d, (a1), v%d' % (mn, e, d, s), '', e == 64)
            for d, s in ((8, 16), (16, 8), (8, 12)):
                add(f, '%s%d.v v%d, (a1), v%d, v0.t' % (mn, e, d, s), '', e == 64)
            for lm, d, s in (('m2', 8, 12), ('m4', 8, 12), ('m2', 9, 16), ('m1', 8, 8), ('m8', 8, 16), ('m4', 8, 16),
                             ('m2', 8, 9), ('m4', 16, 8), ('m2', 12, 8)):
                add(f, '%s%d.v v%d, (a1), v%d' % (mn, e, d, s), '@state: lmul=%s' % lm, e == 64)
        # segments
        for nf in range(2, 9):
            for d in (8, 16, 1, 0, 24, 4, 3):
                add(f, 'vlseg%de%d.v v%d, (a1)' % (nf, e, d))
                add(f, 'vlseg%de%dff.v v%d, (a1)' % (nf, e, d))
                add(f, 'vsseg%de%d.v v%d, (a1)' % (nf, e, d))
                add(f, 'vlsseg%de%d.v v%d, (a1), a2' % (nf, e, d))
                add(f, 'vssseg%de%d.v v%d, (a1), a2' % (nf, e, d))
            for d in (8, 1, 16):
                add(f, 'vlseg%de%d.v v%d, (a1), v0.t' % (nf, e, d))
                add(f, 'vlseg%de%dff.v v%d, (a1), v0.t' % (nf, e, d))
                add(f, 'vsseg%de%d.v v%d, (a1), v0.t' % (nf, e, d))
                add(f, 'vlsseg%de%d.v v%d, (a1), a2, v0.t' % (nf, e, d))
                add(f, 'vssseg%de%d.v v%d, (a1), a2, v0.t' % (nf, e, d))
            for lm in ('m1', 'm2', 'mf2'):
                add(f, 'vlseg%de%d.v v8, (a1)' % (nf, e), '@state: lmul=%s vlsel=max' % lm)
                add(f, 'vsseg%de%d.v v8, (a1)' % (nf, e), '@state: lmul=%s vlsel=max' % lm)
            for mn in ('vluxseg', 'vloxseg', 'vsuxseg', 'vsoxseg'):
                for d, s in ((8, 16), (8, 8), (16, 8), (1, 24), (8, 9), (4, 2), (24, 0)):
                    add(f, '%s%dei%d.v v%d, (a1), v%d' % (mn, nf, e, d, s), '', e == 64)
                add(f, '%s%dei%d.v v8, (a1), v16, v0.t' % (mn, nf, e), '', e == 64)
                for lm, d, s in (('m1', 8, 16), ('m2', 8, 16), ('mf2', 8, 16)):
                    add(f, '%s%dei%d.v v%d, (a1), v%d' % (mn, nf, e, d, s), '@state: lmul=%s' % lm, e == 64)
    # whole registers and masks (vl<n>r.v are the aliases of vl<n>re8.v)
    for n in (1, 2, 4, 8):
        for d in (8, 0, 16, 24):
            if d % n == 0:
                add(f, 'vl%dr.v v%d, (a1)' % (n, d))
        for e in eews:
            for d in (8, 0, 16, 24, 1, 2, 4):
                if d % n == 0:
                    add(f, 'vl%dre%d.v v%d, (a1)' % (n, e, d))
        for d in (8, 0, 16, 24, 1, 2, 4):
            if d % n == 0:
                add(f, 'vs%dr.v v%d, (a1)' % (n, d))
    for d in (8, 0, 31, 1):
        add(f, 'vlm.v v%d, (a1)' % d)
        add(f, 'vsm.v v%d, (a1)' % d)


def check_coverage():
    names = set()
    for table in TABLES:
        for line in open(table):
            m = re.match(r'RV\w\("([^"]+)"', line)
            if m:
                names.add(m.group(1))
    missing = sorted(n for n in names if n not in emitted)
    return missing


def add_words(lines):
    """Lines llvm-mc rejects (a reserved register overlap) get their encoding in a @word: tag."""
    texts = []
    for l in lines:
        texts.append(l.partition('  # ')[0])
    results = rvasm.assemble(64, texts)
    out = []
    for l, text, (hexb, err) in zip(lines, texts, results):
        if err is not None and '@word' not in l:
            word = encode(text)
            if word is None:
                print('cannot encode: ' + text + ' (' + err + ')')
                continue
            tag = l.partition('  # ')[2]
            l = text + '  # ' + (tag + ' ' if tag else '') + '@word: %08x' % word
        out.append(l)
    return out


def write_files():
    for name, lines in sorted(files.items()):
        lines = add_words(list(dict.fromkeys(lines)))
        path = os.path.join(OUT, name + '.s')
        with open(path, 'w') as f:
            f.write('# GENERATED by tests/riscv/semantics/gen_vector.py: do not edit\n')
            f.write('# The vector semantics corpus (%s): see gen_vector.py\n' % name)
            seen = set()
            for l in lines:
                if l in seen:
                    continue
                seen.add(l)
                f.write(l + '\n')
        print('%s: %d lines' % (name, len(seen)))


def main():
    gen_config()
    gen_integer()
    gen_fixed()
    gen_mask()
    gen_permute()
    gen_memory()
    gen_float()
    gen_bits()
    gen_crypto()
    missing = check_coverage()
    if missing:
        print('no line for: ' + ' '.join(missing))
    write_files()


if __name__ == '__main__':
    main()
