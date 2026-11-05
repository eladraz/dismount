#!/usr/bin/env python3
"""
The input states of the RISC-V semantics tests: random and directed states for RV64 and RV32.

A state is drawn from the text of the instruction (the registers of a memory operand point into the
data window, the format of the floating point registers comes from the mnemonic, a vector
instruction gets a vtype / vl / vstart and element data of its width). The same text always gives
the same states (seeded by crc32 of arch + text).

Value classes:
  * integers: boundary values (0, 1, -1, 0x7f.., 0x80.., 0xffff.., sign bits) mixed with random bits
    of random width (XLEN is 32 or 64);
  * floating point: zeros, ones, infinities, quiet and signaling NaNs, denormals, the smallest and
    largest normal numbers, the powers of two around the integer conversions, random normal numbers;
    the single / half precision values are NaN boxed in 80% of the draws and badly boxed (upper bits
    random / zero) in the others;
  * fcsr: the rounding mode (0-4 and, rarely, the reserved 5-7) and the sticky flags;
  * vectors: vtype over every SEW / LMUL / policy (a few reserved ones), vl from 0 to VLMAX with
    the edges, vstart, vxrm / vxsat, masks, elements of the width of SEW (floating point values for
    the vf* instructions);
  * memory: the registers named in a memory operand point into the window; the bytes around are
    the pattern or random, sometimes interesting floating point constants.

`@state:` tags of a corpus line (x10=80100100 f1=7ff8000000000000 v3=... fcsr=60 vtype=18 vl=4
vstart=1 ssp=.. elp=1) replace the drawn values; `win+N` in a value is the window base + N.
"""
import random
import re
import zlib

import rvstate as S

M64 = S.M64
M32 = S.M32

EDGE64 = [0, 0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff,
          0x100000000, 0x7fffffffffffffff, 0x8000000000000000, M64, M64 - 1, 0x5555555555555555,
          0xaaaaaaaaaaaaaaaa, 0xfffffffffffff000, 0x0000000000000fff, 0x1f, 0x20, 0x3f, 0x40]
EDGE32 = [v for v in EDGE64 if v <= M32] + [0xfffffffe, 0x7ffffffe, 0x80000001, 0x55555555, 0xaaaaaaaa,
                                            0xfffff000]

FP32 = [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x40000000, 0x3f000000, 0x7f800000,
        0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0xff800001, 0x7fa00000, 0x00000001,
        0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x7f7fffff, 0xff7fffff, 0x4b000000,
        0xcb000000, 0x4effffff, 0x4f000000, 0xcf000000, 0x5f000000, 0xdf000000, 0x3f800001,
        0x41200000, 0x3effffff, 0x7fffffff, 0x3f000001, 0x40490fdb, 0xbfc00000, 0x4f7fffff,
        0x5effffff, 0x5f7fffff, 0xdf7fffff, 0x3fc00000, 0x00400000]
FP64 = [0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000,
        0x4000000000000000, 0x3fe0000000000000, 0x7ff0000000000000, 0xfff0000000000000,
        0x7ff8000000000000, 0xfff8000000000000, 0x7ff0000000000001, 0xfff0000000000001,
        0x7ff4000000000000, 0x0000000000000001, 0x8000000000000001, 0x000fffffffffffff,
        0x800fffffffffffff, 0x0010000000000000, 0x7fefffffffffffff, 0xffefffffffffffff,
        0x4330000000000000, 0xc330000000000000, 0x41dfffffffc00000, 0x41e0000000000000,
        0xc1e0000000000000, 0x43e0000000000000, 0xc3e0000000000000, 0x3ff0000000000001,
        0x4024000000000000, 0x7fffffffffffffff, 0x3fe0000000000001, 0x400921fb54442d18,
        0x43dfffffffffffff, 0x43efffffffffffff, 0xc3e0000000000000, 0x41efffffffe00000,
        0x7ff8000000000001, 0x3ff8000000000000, 0x0008000000000000]
FP16 = [0x0000, 0x8000, 0x3c00, 0xbc00, 0x4000, 0x3800, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7c01,
        0xfc01, 0x7d00, 0x0001, 0x8001, 0x03ff, 0x83ff, 0x0400, 0x7bff, 0xfbff, 0x6400, 0x7800,
        0x5640, 0x3c01, 0x7fff, 0x7bff, 0x4248, 0x0200]
BF16 = [0x0000, 0x8000, 0x3f80, 0xbf80, 0x7f80, 0xff80, 0x7fc0, 0x7f81, 0x0001, 0x007f, 0x0080,
        0x7f7f, 0x4f00, 0x7fff, 0x4049, 0x3f81, 0xff7f, 0x7fa0]
FP_TABLES = {16: FP16, 32: FP32, 64: FP64}
FP_FIELDS = {16: (5, 10), 32: (8, 23), 64: (11, 52)}

ABI_X = {'zero': 0, 'ra': 1, 'sp': 2, 'gp': 3, 'tp': 4, 't0': 5, 't1': 6, 't2': 7, 's0': 8, 'fp': 8, 's1': 9}
for _i in range(8):
    ABI_X['a%d' % _i] = 10 + _i
for _i in range(2, 12):
    ABI_X['s%d' % _i] = 16 + _i
for _i in range(3, 7):
    ABI_X['t%d' % _i] = 25 + _i
ABI_F = {}
for _i in range(8):
    ABI_F['ft%d' % _i] = _i
ABI_F.update({'fs0': 8, 'fs1': 9})
for _i in range(8):
    ABI_F['fa%d' % _i] = 10 + _i
for _i in range(2, 12):
    ABI_F['fs%d' % _i] = 16 + _i
for _i in range(8, 12):
    ABI_F['ft%d' % _i] = 20 + _i


def stable_seed(arch, text):
    return zlib.crc32(('%s:%s' % (arch, text)).encode('utf-8'))


def draw_int(rng, bits):
    mask = (1 << bits) - 1
    r = rng.random()
    if r < 0.45:
        return rng.choice(EDGE64 if bits == 64 else EDGE32) & mask
    if r < 0.75:
        return rng.getrandbits(bits)
    if r < 0.88:
        return rng.getrandbits(32) & mask
    if r < 0.95:
        return rng.getrandbits(16)
    return rng.getrandbits(8)


def draw_fp(rng, width):
    """A floating point value of 16, 32 or 64 bits (an IEEE binary16 / binary32 / binary64)."""
    r = rng.random()
    table = FP_TABLES[width]
    if r < 0.50:
        return rng.choice(table)
    if r < 0.85:
        ebits, mbits = FP_FIELDS[width]
        bias = (1 << (ebits - 1)) - 1
        exp = max(1, min((1 << ebits) - 2, bias + rng.randint(-24, 24)))
        return rng.getrandbits(1) << (width - 1) | exp << mbits | rng.getrandbits(mbits)
    return rng.getrandbits(width)


def draw_fp_kind(rng, width, bf16=False):
    """draw_fp, or a BFloat16 value (the 16 bit format with 8 exponent bits)"""
    if bf16 and width == 16:
        return rng.choice(BF16) if rng.random() < 0.6 else rng.getrandbits(16)
    return draw_fp(rng, width)


def float_roles(mn, ops, sew):
    """The element width and kind ('f', 'bf16', 'i') of the vector registers an OPFVV / OPFVF instruction reads, by
    its operand order: the widening and narrowing forms have wide operands. {register: (bits, kind)}"""
    regs = [reg_v(o) for o in ops if o.strip() != 'v0.t']
    roles = {}
    wide = sew * 2

    def put(i, bits, kind):
        if i < len(regs) and regs[i] is not None:
            roles[regs[i]] = (bits, kind)

    is_bf = 'bf16' in mn
    base = 'bf16' if is_bf else 'f'
    int_source = bool(re.search(r'\.f\.xu?\.[vw]$', mn))
    if mn.startswith('vfwcvt'):
        put(0, wide, 'f')
        put(1, sew, 'i' if int_source else base)
    elif mn.startswith('vfncvt'):
        put(0, sew, 'f')
        put(1, wide, 'i' if int_source else 'f')
    elif mn.startswith('vfcvt'):
        put(0, sew, 'f')
        put(1, sew, 'i' if int_source else 'f')
    elif mn.startswith(('vfwmacc', 'vfwnmacc', 'vfwmsac', 'vfwnmsac')):
        put(0, wide, 'f')
        put(1, sew, base)
        put(2, sew, base)
    elif mn.startswith(('vfwredosum', 'vfwredusum')):
        put(0, wide, 'f')
        put(1, sew, 'f')
        put(2, wide, 'f')
    elif mn.startswith('vfw') and re.search(r'\.w[vf]$', mn):
        put(0, wide, 'f')
        put(1, wide, 'f')
        put(2, sew, 'f')
    elif mn.startswith('vfw'):
        put(0, wide, 'f')
        put(1, sew, 'f')
        put(2, sew, 'f')
    return roles


def box(rng, value, width, always=False):
    """A 64 bit f register holding `value` of `width` bits: NaN boxed (80% of the draws, always if
    asked), else with the upper bits random or zero."""
    if width == 64:
        return value
    ones = ((1 << 64) - 1) ^ ((1 << width) - 1)
    if always or rng.random() < 0.8:
        return ones | value
    r = rng.random()
    if r < 0.4:
        return value
    if r < 0.7:
        return (ones ^ (1 << (width + rng.randrange(64 - width)))) | value
    return (rng.getrandbits(64 - width) << width) | value


# ---------------------------------------------------------------------------
# Reading the operands of an instruction text
# ---------------------------------------------------------------------------

def reg_x(tok):
    tok = tok.strip()
    if tok in ABI_X:
        return ABI_X[tok]
    m = re.fullmatch(r'x(\d+)', tok)
    return int(m.group(1)) if m and int(m.group(1)) < 32 else None


def reg_f(tok):
    tok = tok.strip()
    if tok in ABI_F:
        return ABI_F[tok]
    m = re.fullmatch(r'f(\d+)', tok)
    return int(m.group(1)) if m and int(m.group(1)) < 32 else None


def reg_v(tok):
    m = re.fullmatch(r'v(\d+)', tok.strip())
    return int(m.group(1)) if m and int(m.group(1)) < 32 else None


def analyze(text):
    """The registers named by the instruction(s) of a corpus line:
    {'mnemonic', 'x': set, 'f': set, 'v': set, 'bases': [x numbers used as an address], 'strides': set,
     'suffixes': [formats]}"""
    info = {'x': set(), 'f': set(), 'v': set(), 'bases': [], 'strides': set(), 'mnemonics': [], 'insns': []}
    for part in text.split(';'):
        part = part.strip()
        if not part:
            continue
        mn, _, rest = part.partition(' ')
        mn = mn.lower()
        info['mnemonics'].append(mn)
        ops = [o.strip() for o in re.split(r',(?![^(]*\))', rest)] if rest.strip() else []
        info['insns'].append((mn, ops))
        for o in ops:
            m = re.fullmatch(r'(?:[^()]*)\(\s*([a-z0-9]+)\s*\)', o)
            if m:
                x = reg_x(m.group(1))
                if x is not None:
                    info['bases'].append(x)
                    info['x'].add(x)
                continue
            x, f, v = reg_x(o), reg_f(o), reg_v(o)
            if x is not None:
                info['x'].add(x)
            elif f is not None:
                info['f'].add(f)
            elif v is not None:
                info['v'].add(v)
        if re.match(r'v[ls]s(seg\d+)?e\d+\.v$', mn) and len(ops) >= 3:
            x = reg_x(ops[2])
            if x is not None:
                info['strides'].add(x)
    return info


def fp_formats(mnemonics):
    """The floating point widths the instruction(s) work on, from the suffixes of the mnemonic."""
    widths = set()
    for mn in mnemonics:
        for p in mn.split('.')[1:]:
            if p == 's':
                widths.add(32)
            elif p == 'd':
                widths.add(64)
            elif p == 'h':
                widths.add(16)
            elif p == 'bf16':
                widths.add(0)
        if mn in ('flw', 'fsw', 'c.flw', 'c.fsw', 'c.flwsp', 'c.fswsp'):
            widths.add(32)
        if mn in ('fld', 'fsd', 'c.fld', 'c.fsd', 'c.fldsp', 'c.fsdsp'):
            widths.add(64)
        if mn in ('flh', 'fsh'):
            widths.add(16)
        if mn in ('flq', 'fsq'):
            widths.add(128)
    return widths


# ---------------------------------------------------------------------------
# The generator
# ---------------------------------------------------------------------------

def parse_value(token, win_base):
    token = token.strip().lower()
    m = re.fullmatch(r'win([+-][0-9a-f]+)?', token)
    if m:
        return win_base + (int(m.group(1), 16) if m.group(1) else 0)
    return int(token, 16)


def apply_fixed(st, fixed, xlen, overrides):
    """Apply the directed tokens of the `@state:` tags and of --set."""
    toks = []
    for line in fixed:
        toks += line.split()
    base = st['win'][0]
    for tok in toks:
        k, _, v = tok.partition('=')
        if k == 'mem':
            # mem=ADDR:HEXBYTES (the bytes in the order of the addresses; ADDR may be win+N)
            addr, _, data = v.partition(':')
            start = parse_value(addr, base)
            for i in range(0, len(data) - 1, 2):
                st['mem'][start + i // 2] = int(data[i:i + 2], 16)
            continue
        val = parse_value(v, base)
        _set(st, k, val, xlen)
    for k, val in overrides.items():
        if k in ('x', 'f', 'v'):
            for i, x in val.items():
                st[k][i] = x
        else:
            _set(st, k, val, xlen)


def _set(st, k, val, xlen):
    if re.fullmatch(r'x\d+', k):
        st['x'][int(k[1:])] = val & S.mask(xlen)
    elif re.fullmatch(r'f\d+', k):
        st['f'][int(k[1:])] = val & M64
    elif re.fullmatch(r'v\d+', k):
        st['v'][int(k[1:])] = val & ((1 << st['vlen']) - 1)
        st['vector'] = True
    elif k in ('vl', 'vtype', 'vstart', 'vxrm', 'vxsat'):
        st[k] = val
        st['vector'] = True
    elif k in ('fcsr', 'pc', 'ssp', 'elp'):
        st[k] = val
    elif k == 'frm':
        st['fcsr'] = (st['fcsr'] & 0x1f) | (val << 5)
    elif k == 'fflags':
        st['fcsr'] = (st['fcsr'] & 0xe0) | val
    elif k == 'vector':
        st['vector'] = bool(val)
    else:
        raise ValueError('unknown state token %r' % k)


LMULS = [(5, 'mf8'), (6, 'mf4'), (7, 'mf2'), (0, 'm1'), (1, 'm2'), (2, 'm4'), (3, 'm8')]


def vtype_fields(sew, vlmul, ta, ma):
    return (ma << 7) | (ta << 6) | ({8: 0, 16: 1, 32: 2, 64: 3}[sew] << 3) | vlmul


def lmul_value(vlmul):
    return {0: 1.0, 1: 2.0, 2: 4.0, 3: 8.0, 5: 0.125, 6: 0.25, 7: 0.5}.get(vlmul)


def vtype_is_valid(vtype, xlen):
    """A vtype value vsetvl accepts: no bit above bit 7 (bit 8 is the altfmt of Zvfbfa, which is not modeled), lmul
    not reserved (4), vsew <= 3 and SEW <= 64 * min(LMUL, 1)"""
    if vtype >> 8:
        return False
    vlmul, vsew = vtype & 7, (vtype >> 3) & 7
    if vlmul == 4 or vsew > 3:
        return False
    lmul_log = vlmul if vlmul < 4 else vlmul - 8
    return vsew <= 3 + min(lmul_log, 0)


def crypto_group_size(mnemonic):
    """EGS of a vector cryptography instruction (0 for the others)"""
    if mnemonic.startswith(('vsm3',)):
        return 8
    if mnemonic.startswith(('vaes', 'vsha2', 'vsm4', 'vghsh', 'vgmul')):
        return 4
    return 0


def draw_vector_state(rng, st, info, xlen, is_float, mn_first, n=0, offset=0, force=None):
    vlen = st['vlen']
    r = rng.random()
    # the SEWs a floating point instruction can have: 16, 32, 64
    sews = [16, 32, 64] if is_float else [8, 16, 32, 64]
    egs = crypto_group_size(mn_first)
    if egs:
        # the cryptography works on SEW 32 (64 for SHA-512); a few states have another one
        sews = [32, 32, 32, 64, 64, 64, 16, 8] if mn_first.startswith('vsha2') else [32, 32, 32, 32, 32, 32, 64, 16, 8]
    if is_float and re.match(r'vf[wn]?cvt', mn_first) and 'bf16' not in mn_first:
        # (the integer side of the conversions has 8 bit elements)
        sews = [8, 16, 32, 64]
    force = force or {}
    # (the states of a line cycle through the SEWs and the LMULs; the offset is drawn once for the line)
    sew = force['sew'] if 'sew' in force else sews[(n + offset) % len(sews)]
    # the LMULs the named registers allow
    named = info['v']
    ok = []
    for vlmul, _ in LMULS:
        lm = lmul_value(vlmul)
        if lm >= 1 and any(n % int(lm) for n in named):
            continue
        if lm < 1 and sew > 64 * lm:
            continue
        ok.append(vlmul)
    vlmul = (ok if ok else [0])[((n + offset) // len(sews)) % len(ok if ok else [0])]
    if 'lmul' in force:
        vlmul = force['lmul']
    elif rng.random() < 0.06:
        # any raw field: includes the reserved lmul 4 and the reserved sew / lmul pairs
        vlmul = rng.randrange(8)
    ta, ma = rng.getrandbits(1), rng.getrandbits(1)
    vtype = vtype_fields(sew, vlmul, ta, ma)
    lm = lmul_value(vlmul)
    vlmax = int(vlen // sew * lm) if lm else 0
    if rng.random() < 0.02:
        vtype |= 1 << (xlen - 1)                              # vill
        vlmax = 0
    elif rng.random() < 0.02:
        vtype |= 1 << rng.randrange(9, min(xlen - 1, 20))     # a reserved bit (not bit 8: altfmt of Zvfbfa, which QEMU 11 implements)
    st['vtype'] = vtype
    if vtype >> (xlen - 1) or vtype >> 8:
        st['vl'] = 0
    else:
        st['vl'] = rng.choice([0, 1, vlmax, vlmax, max(0, vlmax - 1), rng.randint(0, max(0, vlmax)),
                               rng.randint(0, max(0, vlmax)), max(0, vlmax // 2)]) if vlmax else 0
    if 'vl' in force and vlmax:
        sel = force['vl']
        if sel == 'max':
            st['vl'] = vlmax
        elif sel == 'zero':
            st['vl'] = 0
        elif sel == 'one':
            st['vl'] = 1
        elif sel == 'half':
            st['vl'] = vlmax // 2
        elif sel == 'odd':
            st['vl'] = (rng.randint(0, vlmax) | 1) if vlmax > 1 else 1
            st['vl'] = min(st['vl'], vlmax)
        elif sel == 'rand':
            st['vl'] = rng.randint(0, vlmax)
        else:
            st['vl'] = min(int(sel), vlmax)
    if not vtype_is_valid(vtype, xlen):
        # The processor can't be in this configuration: vsetvl makes it illegal (vtype is the vill bit alone, vl is 0)
        st['vtype'] = 1 << (xlen - 1)
        st['vl'] = 0
    st['vstart'] = 0
    if st['vl'] and rng.random() < 0.2:
        st['vstart'] = rng.randrange(0, min(st['vl'], vlen - 1) + 1) if rng.random() < 0.9 else rng.randrange(0, min(vlmax * 2 + 2, vlen))
        # (the vstart CSR holds log2(VLEN) bits: a write of a larger value is truncated, so the states stay below VLEN)
    if egs and 'vl' not in force and st['vtype'] >> (xlen - 1) == 0 and rng.random() < 0.85:
        # (the element groups need a vl and a vstart that are multiples of EGS)
        st['vl'] = st['vl'] // egs * egs
        if rng.random() < 0.85:
            st['vstart'] = st['vstart'] // egs * egs
    st['vxrm'] = rng.randrange(4)
    st['vxsat'] = rng.randrange(2)
    st['vector'] = True
    return sew, vlmax


def draw_v_register(rng, vlen, sew, is_float, indexed, mask_like, bf16=False):
    if is_float and sew not in (16, 32, 64):
        is_float = False
    if mask_like:
        r = rng.random()
        if r < 0.1:
            return 0
        if r < 0.2:
            return (1 << vlen) - 1
        if r < 0.3:
            return int.from_bytes(bytes([0x55]) * (vlen // 8), 'little')
        return rng.getrandbits(vlen)
    r = rng.random()
    if indexed:
        # offsets into the window
        n = vlen // sew
        v = 0
        for i in range(n):
            v |= (rng.randrange(0, 0x100) & ((1 << sew) - 1)) << (i * sew)
        return v
    if not is_float and r < 0.1:
        return 0
    if not is_float and r < 0.2:
        return (1 << vlen) - 1
    n = vlen // sew
    v = 0
    same = None
    if is_float:
        same = draw_fp_kind(rng, sew, bf16) if rng.random() < 0.25 else None
    elif rng.random() < 0.25:
        same = draw_int(rng, min(sew, 64)) & ((1 << sew) - 1)
    for i in range(n):
        if is_float:
            e = same if same is not None and rng.random() < 0.5 else draw_fp_kind(rng, sew, bf16)
        else:
            e = same if same is not None and rng.random() < 0.5 else draw_int(rng, 64) & ((1 << sew) - 1)
            if rng.random() < 0.3:
                e = rng.choice([0, 1, (1 << sew) - 1, 1 << (sew - 1), (1 << (sew - 1)) - 1]) & ((1 << sew) - 1)
        v |= e << (i * sew)
    return v


def make_states(xlen, vlen, text, count, overrides=None, mode=None, fixed=(), seed_extra='', boxed=False):
    """Draw `count` states for the instruction text. Returns a list of state dicts (rvstate)."""
    overrides = overrides or {}
    rng = random.Random(stable_seed('rv%d%s' % (xlen, seed_extra), text))
    info = analyze(text)
    mnemonics = info['mnemonics']
    first = mnemonics[0] if mnemonics else ''
    is_vec = first.startswith('v') and not first.startswith('vsetvl') or mode == 'vector'
    is_setvl = first.startswith('vsetvl') or first.startswith('vsetivli')
    is_float_vec = bool(re.match(r'v(f|fw|fn|mf)', first)) or first.startswith('vfmv') or first.startswith('vfcvt')
    is_indexed = bool(re.search(r'v[ls][uo]xei\d+\.v|v[ls][uo]xseg\d+ei\d+\.v', first))
    widths = fp_formats(mnemonics) - {0, 128}
    bf16 = 0 in fp_formats(mnemonics)
    is_amo = first.startswith(('amo', 'lr.', 'sc.'))
    mask = S.mask(xlen)
    states = []
    base, size = S.WIN_BASE, S.WIN_SIZE
    # sew=N lmul=m1|m2|m4|m8|mf2|mf4|mf8 vl=max|zero|one|half|odd|rand|N in a @state: force the vector configuration
    # of every state (vl is then chosen for the configuration); the other tokens set the registers
    force = {}
    rest = []
    lmul_codes = {'m1': 0, 'm2': 1, 'm4': 2, 'm8': 3, 'mf8': 5, 'mf4': 6, 'mf2': 7}
    for line in fixed:
        kept = []
        for tok in line.split():
            k, _, v = tok.partition('=')
            if k == 'sew':
                force['sew'] = int(v)
            elif k == 'lmul':
                force['lmul'] = lmul_codes[v]
            elif k == 'vlsel':
                force['vl'] = v
            else:
                kept.append(tok)
        rest.append(' '.join(kept))
    fixed = rest
    line_offset = random.Random(stable_seed('rv%d%s' % (xlen, seed_extra), text + '#offset')).randrange(1 << 16)
    for n in range(count):
        st = S.default_state(xlen, vlen)
        st['win'] = (base, size)
        # integers
        # (the registers the text names, and a few others: the rest stay 0, which keeps the draws cheap)
        for i in sorted(info['x'] - {0}) + [rng.randrange(1, 32) for _ in range(4)]:
            st['x'][i] = draw_int(rng, xlen) & mask
        st['x'][7] = st['x'][7] or draw_int(rng, xlen) & mask
        # branches and compares: sometimes equal sources
        if rng.random() < 0.25 and len(info['x']) >= 2:
            regs = sorted(info['x'])
            st['x'][regs[1]] = st['x'][regs[0]]
        # addresses
        for b in info['bases']:
            align = 8
            if is_amo:
                align = 4 if re.search(r'\.[wb]($|\.)', first) or first.endswith('.w') else 8
            if rng.random() < 0.08 and not is_amo:
                st['x'][b] = base + 0x1000 + rng.randrange(0, 0x1800)        # unaligned
            else:
                st['x'][b] = base + 0x1000 + (rng.randrange(0, 0x1800) & ~(align - 1))
            if rng.random() < 0.04:
                st['x'][b] = (base + size + rng.choice([0, 8, 0x100, 0x10000])) & mask   # outside: fault
            if is_vec and not is_amo and first[1:2] in ('l', 's') and not is_setvl:
                # the vector loads and stores: now and then the window edge (an access that crosses it faults in the
                # middle: vstart, the fault only first trimming of vl), or its start (the negative strides and indexes)
                r = rng.random()
                if r < 0.12:
                    st['x'][b] = base + size - rng.randrange(1, 300)
                elif r < 0.17:
                    st['x'][b] = base + rng.randrange(0, 40)
        for s in info['strides']:
            st['x'][s] = rng.choice([0, 1, 2, 4, 8, 16, 32, M64 - 7, M64 - 31, rng.randrange(0, 64)]) & mask
        # floating point registers
        fmts = sorted(widths) or [32, 64]
        if boxed and fmts:
            # only the source format of the instruction: fcvt.d.s reads a single
            src = [m.split('.')[-1] for m in mnemonics if m.startswith('f') and '.' in m]
            w = {'s': 32, 'd': 64, 'h': 16}.get(src[0] if src else '')
            fmts = [w] if w else fmts
        for i in sorted(info['f']) + [rng.randrange(32) for _ in range(3)]:
            w = rng.choice(fmts)
            v = draw_fp(rng, w)
            st['f'][i] = box(rng, v, w, boxed)
            if bf16 and rng.random() < 0.5:
                st['f'][i] = box(rng, rng.choice(BF16) if rng.random() < 0.6 else rng.getrandbits(16), 16, boxed)
        if rng.random() < 0.15 and len(info['f']) >= 2:
            regs = sorted(info['f'])
            st['f'][regs[1]] = st['f'][regs[0]]
        frm = rng.choice([0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0, 5, 6, 7]) if rng.random() < 0.98 else rng.randrange(8)
        flags = rng.choice([0, 0, 0, rng.randrange(32)])
        st['fcsr'] = (frm << 5) | flags
        # the vector state
        if is_vec or is_setvl or mode == 'vector':
            sew, vlmax = draw_vector_state(rng, st, info, xlen, is_float_vec, first, n, line_offset, force)
            roles = float_roles(first, info['insns'][0][1], sew) if is_float_vec else {}
            for i in range(32):
                if i in roles:
                    bits, kind = roles[i]
                    st['v'][i] = draw_v_register(rng, vlen, bits, kind != 'i', False, False, kind == 'bf16')
                else:
                    st['v'][i] = draw_v_register(rng, vlen, sew, is_float_vec, False, False)
            if is_float_vec:
                # the scalar of the .vf forms is a float of SEW bits (BFloat16 for vfwmaccbf16), boxed 80% of the time
                for i in info['f']:
                    w = 16 if 'bf16' in first else (sew if sew in (16, 32, 64) else 32)
                    st['f'][i] = box(rng, draw_fp_kind(rng, w, 'bf16' in first), w, boxed)
            if mode != 'noindex' and is_indexed:
                for i in info['v']:
                    st['v'][i] = draw_v_register(rng, vlen, sew, False, True, False)
            if re.search(r'\.v0|,\s*v0\.t|\.vvm|\.vxm|\.vim|\.vfm|\.vm$', text) or ', v0.t' in text:
                st['v'][0] = draw_v_register(rng, vlen, sew, False, False, True)
            if first.startswith(('vm', 'vcpop', 'vfirst', 'vid', 'viota')) or rng.random() < 0.3:
                st['v'][0] = draw_v_register(rng, vlen, sew, False, False, True)
            if is_setvl:
                # the avl and the vtype in the registers
                for x in info['x']:
                    r = rng.random()
                    if r < 0.5:
                        s2 = rng.choice(sews_for_setvl())
                        st['x'][x] = (vtype_fields(s2, rng.choice([0, 1, 2, 3, 5, 6, 7]), rng.getrandbits(1), rng.getrandbits(1))
                                      if rng.random() < 0.9 else draw_int(rng, xlen)) & mask
                    else:
                        st['x'][x] = rng.choice([0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 64, 1000, mask]) & mask
        elif mode == 'fvec':
            pass
        # memory: random bytes around the pointers
        for b in info['bases']:
            if rng.random() < 0.6:
                addr = st['x'][b]
                if base <= addr < base + size - 256:
                    for k in range(rng.choice([16, 32, 64, 128])):
                        a = addr + k - 8
                        if base <= a < base + size:
                            st['mem'][a] = rng.getrandbits(8) if rng.random() < 0.7 else rng.choice([0, 0xff, 0x7f, 0x80, 0xc0, 0x7e])
            if rng.random() < 0.3 and widths:
                w = rng.choice(sorted(widths))
                addr = st['x'][b]
                for k in range(0, 16, w // 8):
                    val = draw_fp(rng, w)
                    for j in range(w // 8):
                        a = addr + k + j
                        if base <= a < base + size:
                            st['mem'][a] = (val >> (8 * j)) & 0xff
        if mode == 'cfi':
            draw_cfi_state(rng, st, info, first, xlen, mask)
        apply_fixed(st, fixed, xlen, overrides)
        states.append(st)
    return states


SS_VA = 0x80200000


def draw_cfi_state(rng, st, info, first, xlen, mask):
    """Zicfilp / Zicfiss enabled: the shadow stack pointer into the shadow stack page, the expected
    landing pad state, the label register (x7) of lpad, the top of the shadow stack equal to ra / t0 half
    of the time (sspopchk)."""
    st['cfi'] = True
    st['ssp'] = SS_VA + 8 * rng.randint(2, 500)
    st['elp'] = rng.getrandbits(1)
    m = re.match(r'lpad\s+(\S+)', info['insns'][0][0] + ' ' + (info['insns'][0][1][0] if info['insns'][0][1] else '')) if info['insns'] else None
    label = int(info['insns'][0][1][0], 0) if first == 'lpad' and info['insns'][0][1] else None
    if label is not None:
        st['x'][7] = (((label if rng.random() < 0.6 else rng.getrandbits(20)) << 12) | rng.getrandbits(12)) & mask
        if rng.random() < 0.3:
            st['x'][7] = rng.getrandbits(xlen) & mask
    for b in info['bases']:
        if first.startswith('ssamo'):
            st['x'][b] = SS_VA + 8 * rng.randint(1, 500) if rng.random() < 0.9 else SS_VA + rng.randrange(0, 0x1000)
    wide = xlen // 8
    for r in (1, 5):
        if rng.random() < 0.6:
            val = st['x'][r] if rng.random() < 0.6 else rng.getrandbits(xlen)
            if rng.random() < 0.5:
                st['x'][r] = val & mask
            for k in range(wide):
                st['mem'][st['ssp'] + k] = (st['x'][r] >> (8 * k)) & 0xff
    # the data the shadow stack instructions pop / swap
    for k in range(wide):
        st['mem'].setdefault(st['ssp'] - 8 + k, rng.getrandbits(8))


def sews_for_setvl():
    return [8, 16, 32, 64]
