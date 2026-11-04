#!/usr/bin/env python3
"""
The input states of the ARM semantics tests: random and directed states for A32, T32 and A64.

A state is drawn from the text of the instruction (the registers of a memory operand point into
the memory window, the lane size of the vector registers decides which floating point values are
drawn). The same text always gives the same states (seeded by crc32 of arch + text).

Value classes:
  * integers: boundary values (0, 1, -1, 0x7f.., 0x80.., 0xffff.., sign bits) mixed with random
    bits of random width;
  * floating point lanes: zeros, ones, infinities, quiet and signaling NaNs, denormals, the
    smallest and largest normal numbers, the powers of two around the integer conversions, and
    random normal numbers; random raw bits for the rest;
  * the flags: random NZCV (and QC / GE / Q for A32); the FPCR / FPSCR: the rounding mode, FZ
    and DN at random, the cumulative exception flags sometimes set (they are sticky);
  * predicates: all true, all false, random, and the patterns of ptrue;
  * memory: the registers named in a bracket `[xN, xM, ...]` point into the window.
"""
import random
import re
import zlib

import armstate as S

M64 = S.M64
M32 = S.M32

EDGE64 = [0, 0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff,
          0x100000000, 0x7fffffffffffffff, 0x8000000000000000, M64, M64 - 1, 0x5555555555555555,
          0xaaaaaaaaaaaaaaaa]
EDGE32 = [v for v in EDGE64 if v <= M32] + [0xfffffffe, 0x7ffffffe, 0x80000001, 0x55555555, 0xaaaaaaaa]

FP32 = [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x40000000, 0x3f000000, 0x7f800000,
        0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0xff800001, 0x7fa00000, 0x00000001,
        0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x7f7fffff, 0xff7fffff, 0x4b000000,
        0xcb000000, 0x4effffff, 0x4f000000, 0xcf000000, 0x5f000000, 0xdf000000, 0x3f800001,
        0x41200000, 0x3effffff, 0x7fffffff]
FP64 = [0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000,
        0x4000000000000000, 0x3fe0000000000000, 0x7ff0000000000000, 0xfff0000000000000,
        0x7ff8000000000000, 0xfff8000000000000, 0x7ff0000000000001, 0xfff0000000000001,
        0x7ff4000000000000, 0x0000000000000001, 0x8000000000000001, 0x000fffffffffffff,
        0x800fffffffffffff, 0x0010000000000000, 0x7fefffffffffffff, 0xffefffffffffffff,
        0x4330000000000000, 0xc330000000000000, 0x41dfffffffc00000, 0x41e0000000000000,
        0xc1e0000000000000, 0x43e0000000000000, 0xc3e0000000000000, 0x3ff0000000000001,
        0x4024000000000000, 0x7fffffffffffffff]
FP16 = [0x0000, 0x8000, 0x3c00, 0xbc00, 0x4000, 0x3800, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7c01,
        0xfc01, 0x7d00, 0x0001, 0x8001, 0x03ff, 0x83ff, 0x0400, 0x7bff, 0xfbff, 0x6400, 0x7800,
        0x5640, 0x3c01, 0x7fff]
BF16 = [0x0000, 0x8000, 0x3f80, 0xbf80, 0x7f80, 0xff80, 0x7fc0, 0x7f81, 0x0001, 0x007f, 0x0080,
        0x7f7f, 0x4f00, 0x7fff]
FP_TABLES = {16: FP16, 32: FP32, 64: FP64}


def stable_seed(arch, text):
    return zlib.crc32(('%s:%s' % (arch, text)).encode('utf-8'))


def draw_int(rng, bits):
    """An integer value of `bits` bits (32 or 64)."""
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


def draw_fp_lane(rng, width):
    """A floating point lane of 16, 32 or 64 bits."""
    r = rng.random()
    table = FP_TABLES.get(width)
    if table is None:
        return rng.getrandbits(width)
    if r < 0.50:
        return rng.choice(table)
    if r < 0.85:
        # a random normal number: a random sign, an exponent near the bias, a random mantissa
        ebits, mbits = {16: (5, 10), 32: (8, 23), 64: (11, 52)}[width]
        bias = (1 << (ebits - 1)) - 1
        exp = max(1, min((1 << ebits) - 2, bias + rng.randint(-24, 24)))
        return rng.getrandbits(1) << (width - 1) | exp << mbits | rng.getrandbits(mbits)
    return rng.getrandbits(width)


def draw_vector(rng, bits, lane):
    """A `bits` bit vector of `lane` bit floating point lanes, or raw bits (lane 0)."""
    if lane == 0 or rng.random() < 0.15:
        # raw: half of them are structured (all ones, all zero, alternating bytes)
        r = rng.random()
        if r < 0.15:
            return 0
        if r < 0.25:
            return (1 << bits) - 1
        if r < 0.35:
            return int.from_bytes(bytes([rng.getrandbits(8)]) * (bits // 8), 'little')
        return rng.getrandbits(bits)
    v = 0
    # one lane value is repeated in some of the states, so the compare / min / max see equal lanes
    same = draw_fp_lane(rng, lane) if rng.random() < 0.25 else None
    for i in range(bits // lane):
        v |= (same if same is not None and rng.random() < 0.5 else draw_fp_lane(rng, lane)) << (i * lane)
    return v


EDGE_BYTES = [0x00, 0x01, 0x02, 0x7f, 0x80, 0x81, 0xfe, 0xff, 0x40, 0xc0, 0x3f, 0x20]


def draw_vector_a32(rng, bits, lane):
    """AArch32: like draw_vector, and for the integer data (lane 0) 40% of the vectors are made of
    boundary bytes (0, 1, 0x7f, 0x80, 0xff...), so the saturation, the shifts by a register and the
    comparisons reach their edges."""
    if lane == 0 and rng.random() < 0.4:
        v = 0
        for i in range(bits // 8):
            v |= rng.choice(EDGE_BYTES) << (8 * i)
        return v
    return draw_vector(rng, bits, lane)


def lane_width(arch, text):
    """The lane size in bits that the vector registers of the instruction are drawn with, 0 for
    none (integer data)."""
    t = text.lower()
    if arch == 'a64':
        if re.search(r'\bf(?:add|sub|mul|div|max|min|sqrt|abs|neg|cmp|cmpe|ccmp|madd|msub|nmadd|nmsub|'
                     r'cvt|mov|rint|recp|rsqrt|mla|mls|abd|scale|nm|addp|maxp|minp|trn|exp|mad|nmad|'
                     r'nmla|nmls|nmsb|ad)', t) or re.search(r'\b[sufb]cvtf|\bfcvt|\bf[a-z]+\b', t):
            if re.search(r'\bh\d|\.\d*h\b', t):
                return 16
            if re.search(r'\bd\d|\.\d*d\b', t):
                return 64
            return 32
        return 0
    if re.search(r'\.f64|\bd\d+', t) and re.search(r'\bv[a-z]+\.f', t):
        return 64
    if re.search(r'\.f16', t):
        return 16
    if re.search(r'\.f32|\.f\b|\bs\d+', t) and re.search(r'\bv[a-z0-9]+\.f|\bvcvt|\bvmov\.f|\bvrint', t):
        return 32
    if re.search(r'\bv(?:add|sub|mul|div|abs|neg|cmp|sqrt|fma|fms|nmul|nmla|nmls|max|min|rint|cvt|sel|mov)\.f', t):
        return 32
    return 0


def memory_registers(arch, text):
    """(base registers, index registers) named in the memory operands of the text:
    ldr x0, [x1, x2, lsl #3] -> (['x1'], ['x2'])."""
    bases, indexes = [], []
    prefix = r'(?:x|sp|w)' if arch == 'a64' else r'(?:r|sp|lr)'
    for m in re.finditer(r'\[([^\]]*)\]', text):
        inner = [x.strip() for x in m.group(1).split(',')]
        if inner and re.match(r'^(?:x\d+|sp|r\d+|lr)(?::\d+)?$', inner[0]):
            bases.append(inner[0].split(':')[0])
            for x in inner[1:]:
                mm = re.match(r'^-?([xwr]\d+)', x)
                if mm:
                    indexes.append(mm.group(1))
    # A32 load / store multiple: `ldm r1!, {..}`, `push` / `pop` use sp
    m = re.match(r'^\s*(?:ldm|stm|vldm|vstm|ldmia|stmia|ldmdb|stmdb|ldmib|stmib|ldmda|stmda)\w*(?:\.\w+)?\s+(r\d+|sp)!?\s*,', text.lower())
    if m:
        bases.append(m.group(1))
    # SVE / SME: `[x0, #1, mul vl]`, `[x0, x1, lsl #2]`, `[z0.s, #4]` (a vector base: not handled)
    return bases, indexes


def reg_index(arch, name):
    if name in ('sp', 'lr'):
        return name
    return int(name[1:])


def make_state(arch, rng, text, overrides, mode):
    st = S.default_state(arch)
    for key in ('vl', 'svl'):
        if key in overrides:
            st[key] = overrides[key]
    a64 = arch == 'a64'
    bits = 64 if a64 else 32
    base = S.A64_MEM_BASE if a64 else S.A32_MEM_BASE
    lane = lane_width(arch, text)
    regs = st['x'] if a64 else st['r']
    for i in range(len(regs)):
        regs[i] = draw_int(rng, bits)
    st['sp'] = base + 0x8000 + 16 * rng.randint(0, 0x100)
    if not a64:
        st['lr'] = draw_int(rng, 32)
    # The memory operands: the base registers point into the window (aligned most of the time),
    # the index registers are small
    bases, indexes = memory_registers(arch, text)
    # The atomics and the exclusive / acquire / release accesses are aligned: an unaligned access
    # faults on Unicorn (no FEAT_LSE2) and not on the host (oracle_bugs.txt)
    aligned = bool(re.match(r'^\s*(?:ld|st)(?:a?x|l?a?x|ar|lr|lar|apr|apur|ur|lur)|^\s*(?:cas|swp|ldadd|ldclr|ldeor|ldset|ldsmax|'
                            r'ldsmin|ldumax|ldumin|stadd|stclr|steor|stset|stsmax|stsmin|stumax|stumin|ld64b|st64b|ldap)', text.lower())) or arch != 'a64'
    for b in bases:
        value = base + 0x4000 + 16 * rng.randint(0, 0x200) + (rng.choice([0, 0, 0, 1, 2, 3, 4, 7, 8]) if (rng.random() < 0.3 and not aligned) else 0)
        if b == 'sp':
            st['sp'] = value & ~15
        elif b == 'lr':
            st['lr'] = value
        else:
            regs[reg_index(arch, b)] = value
    for ix in indexes:
        if ix[0] in 'xr' and reg_index(arch, ix) not in [reg_index(arch, b) for b in bases if b not in ('sp', 'lr')]:
            regs[reg_index(arch, ix)] = rng.choice([0, 1, 2, 3, 4, 8, 16, 0x20, 0x40, M64 if a64 else M32])
    # the flags and the floating point control
    if a64:
        st['nzcv'] = rng.getrandbits(4)
        st['fpcr'] = fp_control(rng, 'a64', random.Random(stable_seed(arch, text + '|fpcr')))
        st['fpsr'] = rng.choice([0, 0, 0, rng.getrandbits(5), (1 << 27) | rng.getrandbits(5), 0x80])
        st['v'] = [draw_vector(rng, 128, lane) for _ in range(32)]
        st['mem'] = {}
        if mode in ('sve', 'sm'):
            st['sve'] = True
            st['sm'] = mode == 'sm'
            vbits = S.vector_bits(st)
            st['z'] = [draw_vector(rng, vbits, lane) for _ in range(32)]
            st['p'] = [draw_predicate(rng, vbits // 8, lane) for _ in range(16)]
            st['ffr'] = draw_predicate(rng, vbits // 8, lane) if mode == 'sve' else 0
        if mode == 'sm' and 'za' in text.lower() or mode == 'za':
            st['sm'] = True
            st['sve'] = True
            st['za'] = True
            vbits = S.vector_bits(st)
            st['z'] = [draw_vector(rng, vbits, lane) for _ in range(32)]
            st['p'] = [draw_predicate(rng, vbits // 8, lane) for _ in range(16)]
            st['ffr'] = 0
            st['zarows'] = [draw_vector(rng, st['svl'], lane) for _ in range(st['svl'] // 8)]
            st['zt0'] = rng.getrandbits(512)
        # a w12 / w13 slice index and the vector select registers stay small
        for r in (8, 9, 10, 11, 12, 13, 14, 15):
            if st['za'] or st['sm']:
                if rng.random() < 0.8:
                    st['x'][r] = rng.randint(0, 15)
    else:
        st['apsr'] = (rng.getrandbits(4) << 28) | (rng.getrandbits(1) << 27) | (rng.getrandbits(4) << 16)
        st['fpscr'] = fp_control(rng, 'a32') | (rng.getrandbits(4) << 28)
        st['q'] = [draw_vector_a32(rng, 128, lane) for _ in range(16)]
    for key in ('x', 'r'):
        for i, v in overrides.get(key, {}).items():
            if key in st:
                st[key][i] = v
    st.update({k: v for k, v in overrides.items() if k not in ('x', 'r')})
    if a64:
        S.complete_vectors(st)
    return st


def fp_control(rng, arch, ext=None):
    """FPCR (a64) or FPSCR (a32) control bits: the rounding mode, FZ, DN, sometimes the
    cumulative flags. 70% of the states have the default control (round to nearest)."""
    v = 0
    if rng.random() < 0.3:
        v |= rng.getrandbits(2) << 22
        v |= rng.getrandbits(1) << 24             # FZ
        v |= rng.getrandbits(1) << 25             # DN
        if arch == 'a64' and ext is not None:
            # FZ16 and AHP (the alternative half precision of the conversions) come from a stream of their own, so
            # the other random draws of a line don't depend on them
            v |= ext.getrandbits(1) << 19         # FZ16
            if ext.random() < 0.3:
                v |= 1 << 26                      # AHP
    if arch == 'a32' and rng.random() < 0.1:
        v |= 1 << 26                              # AHP
    if arch == 'a32' and rng.random() < 0.15:
        v |= rng.choice([0, 1, 2, 4, 8, 16, 0x80, 0x9f])      # the cumulative flags
    if arch == 'a32' and rng.random() < 0.1:
        v |= (1 << 27)                            # QC
    return v


def draw_predicate(rng, bits, lane):
    """A predicate of `bits` bits: all true, all false, random, or an alternating pattern."""
    r = rng.random()
    if r < 0.2:
        return (1 << bits) - 1
    if r < 0.3:
        return 0
    if r < 0.5:
        # the bits of a lane size: one bit per `lane` bits (ptrue .s sets every 4th bit)
        step = max(1, (lane or 32) // 8)
        v = 0
        for i in range(0, bits, step):
            if rng.random() < 0.7:
                v |= 1 << i
        return v
    if r < 0.6:
        # a prefix
        n = rng.randint(0, bits)
        return (1 << n) - 1
    return rng.getrandbits(bits)


def make_states(arch, text, count, overrides=None, mode=None, directed=()):
    """`count` random states for the instruction `text`, then the directed states: each string of
    `directed` (the tokens of a corpus `@state:`) applied on top of a random one. `mode` is
    None, 'sve' or 'sm' (a64 only)."""
    rng = random.Random(stable_seed(arch, text + '|' + str(mode)))
    states = [make_state(arch, rng, text, overrides or {}, mode) for _ in range(count)]
    for n, spec in enumerate(directed):
        st = copy_state(states[n % len(states)])
        S.parse_tokens(arch, spec.split(), st)
        if arch == 'a64':
            S.complete_vectors(st)
        states.append(st)
    return states


def copy_state(st):
    out = dict(st)
    for key, value in st.items():
        if isinstance(value, list):
            out[key] = list(value)
        elif isinstance(value, dict):
            out[key] = dict(value)
    return out
