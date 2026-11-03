#!/usr/bin/env python3
"""
The random "State v2" of the semantics tests (segments, x87, SSE and the
selector operands of the lines that load segment registers), see
"State v2" in tests/semantics/TEST_PLAN.md.

All the draws use their own random generator (seeded from the line text), so
adding them doesn't change the integer registers of the states.
"""
import re
import zlib

import random

import oracle

M32 = 0xFFFFFFFF
SREGS = 'es|ss|ds|fs|gs'

# --- float edge pools ------------------------------------------------------

F32_EDGES = [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000,
             0x7fc00000, 0xffc00000, 0x7fa00000, 0xffa00000, 0x00000001, 0x007fffff,
             0x80000001, 0x00800000, 0x7f7fffff, 0xff7fffff, 0x3f000000, 0x40490fdb]
F64_EDGES = [0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000,
             0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000,
             0x7ff4000000000000, 0xfff4000000000000, 0x0000000000000001, 0x000fffffffffffff,
             0x8000000000000001, 0x0010000000000000, 0x7fefffffffffffff, 0xffefffffffffffff,
             0x3fe0000000000000, 0x400921fb54442d18]


def x87_value(rng):
    """An 80 bit x87 register value: edge values, invalid encodings or a random normal."""
    sign = rng.getrandbits(1) << 79
    j = 1 << 63
    frac = rng.getrandbits(63)
    kind = rng.randrange(14)
    if kind == 0:                                        # zero
        return sign
    if kind == 1:                                        # one
        return sign | (0x3fff << 64) | j
    if kind == 2:                                        # infinity
        return sign | (0x7fff << 64) | j
    if kind == 3:                                        # quiet NaN
        return sign | (0x7fff << 64) | j | (1 << 62) | (frac & ((1 << 62) - 1))
    if kind == 4:                                        # signaling NaN
        return sign | (0x7fff << 64) | j | ((frac & ((1 << 62) - 1)) or 1)
    if kind == 5:                                        # denormal
        return sign | (frac or 1)
    if kind == 6:                                        # pseudo-denormal (exp 0, J = 1)
        return sign | j | frac
    if kind == 7:                                        # largest normal
        return sign | (0x7ffe << 64) | j | ((1 << 63) - 1)
    if kind == 8:                                        # smallest normal
        return sign | (1 << 64) | j
    if kind == 9:                                        # unnormal (J = 0, exp != 0)
        return sign | (rng.randrange(1, 0x7fff) << 64) | frac
    if kind == 10:                                       # pseudo-infinity / pseudo-NaN
        return sign | (0x7fff << 64) | frac
    if kind == 11:                                       # a small integer
        v = rng.choice([2, 3, 4, 5, 7, 10, 100, 255, 256, 1000, 65535, 65536])
        e = v.bit_length() - 1
        return sign | ((0x3fff + e) << 64) | ((v << (63 - e)) & ((1 << 64) - 1))
    # a random normal, mostly near 1
    exp = 0x3fff + rng.randrange(-70, 70) if rng.random() < 0.8 else rng.randrange(1, 0x7fff)
    return sign | (exp << 64) | j | frac


def sse_register(rng):
    """A 128 bit xmm value: four f32 lanes, two f64 lanes or random integers."""
    mode = rng.randrange(3)
    if mode == 0:
        lanes = [rng.choice(F32_EDGES) if rng.random() < 0.7 else rng.getrandbits(32)
                 for _ in range(4)]
        return sum(v << (32 * i) for i, v in enumerate(lanes))
    if mode == 1:
        lanes = [rng.choice(F64_EDGES) if rng.random() < 0.7 else rng.getrandbits(64)
                 for _ in range(2)]
        return sum(v << (64 * i) for i, v in enumerate(lanes))
    return rng.getrandbits(128)


def fp_state(rng):
    """The x87 and SSE keys of a state."""
    if rng.random() < 0.5:
        fcw = 0x37f
    else:
        # precision (not the reserved 1), rounding, all the exceptions masked
        fcw = 0x7f | (rng.choice([0, 2, 3]) << 8) | (rng.randrange(4) << 10)
    top = rng.randrange(8)
    fsw = (top << 11) | (rng.getrandbits(1) << 8) | (rng.getrandbits(1) << 9) | \
        (rng.getrandbits(1) << 10) | (rng.getrandbits(1) << 14)
    if rng.random() < 0.5:
        mxcsr = 0x1f80
    else:
        mxcsr = 0x1f80 | (rng.randrange(4) << 13) | (rng.getrandbits(1) << 15) | \
            (rng.getrandbits(1) << 6)
    return {'fcw': fcw, 'fsw': fsw, 'ftw': rng.getrandbits(8),
            'fpr': [x87_value(rng) for _ in range(8)],
            'mxcsr': mxcsr, 'xmm': [sse_register(rng) for _ in range(8)]}


# --- x87 stacks and memory operands -----------------------------------------
#
# Unicorn (QEMU) has no x87 stack faults: it reads an empty register as is and
# pushes onto a full stack. The random states of the x87 lines therefore have a
# well formed stack for the instruction (enough valid registers at the top for
# the operands, a free register for a push). The stack faults are tested by the
# hand corpus tests/semantics/corpus/x87_edge.asm.

X87_PUSHES = ('fld', 'fild', 'fbld', 'fld1', 'fldz', 'fldl2t', 'fldl2e', 'fldpi', 'fldlg2',
              'fldln2', 'fxtract', 'fptan', 'fsincos')
X87_STACKLESS = ('fnstenv', 'fnsave', 'fnstsw', 'fnstcw', 'fldcw', 'fldenv', 'frstor', 'fninit',
                 'fnclex', 'fwait', 'fnop', 'fincstp', 'fdecstp', 'ffree', 'ffreep', 'fxsave',
                 'fxrstor', 'fxam', 'fneni', 'fndisi', 'fsetpm', 'emms')
X87_TWO = ('fscale', 'fcompp', 'fucompp', 'fprem', 'fprem1', 'fyl2x', 'fyl2xp1', 'fpatan')


def x87_needs(text):
    """(minimum, maximum) valid registers a line needs at the top of the x87 stack."""
    mnemonic = text.split()[0]
    lo, hi = 1, 8
    if mnemonic in X87_PUSHES:
        lo, hi = (1 if mnemonic in ('fxtract', 'fptan', 'fsincos') else 0), 7
    elif mnemonic in X87_STACKLESS:
        lo = 0
    elif mnemonic in X87_TWO:
        lo = 2
    for m in re.finditer(r'\bst(\d)\b', text):
        lo = max(lo, int(m.group(1)) + 1)
    if mnemonic == 'fld' and '[' not in text:
        hi = 7
    # "fadd st1" is st0 += st1: nasm writes the register as st1
    if mnemonic in ('fxch', 'fcom', 'fcomp', 'fucom', 'fucomp') and re.search(r'\bst\b', text) is None \
            and not re.search(r'\bst\d\b', text):
        lo = max(lo, 2)
    return lo, max(lo, hi)


def effective_address(text, state):
    """The linear address of the (flat) memory operand [..] of a line, or None."""
    m = re.search(r'\[([^\]]*)\]', text)
    if not m:
        return None
    total = 0
    for term in m.group(1).replace(' ', '').split('+'):
        if not term:
            continue
        t = re.match(r'^(e[a-ds][xip]|e[sd]i)(?:\*(\d))?$', term)
        if t:
            total += state[t.group(1)] * int(t.group(2) or 1)
        elif re.match(r'^(0x[0-9a-f]+|\d+)$', term):
            total += int(term, 0)
        else:
            return None
    return total & M32


def clear_pseudo_denormal(v):
    """A pseudo-denormal (exponent 0, integer bit 1) becomes a denormal."""
    if (v >> 64) & 0x7fff == 0 and v & (1 << 63):
        v &= ~(1 << 63)
        if v & ((1 << 63) - 1) == 0:
            v |= 1
    return v


def sane_control_word(rng):
    """A control word without reserved bits (bit 6 set, bits 7 and 13-15 clear): the processor
    keeps those as they are, QEMU stores the word as it is."""
    return 0x7f | (rng.choice([0, 2, 3]) << 8) | (rng.randrange(4) << 10) | (rng.getrandbits(1) << 12)


def normal_value(rng):
    """A random normal number of moderate magnitude."""
    return (rng.getrandbits(1) << 79) | ((0x3fff + rng.randrange(-70, 70)) << 64) | (1 << 63) | \
        rng.getrandbits(63)


def in_bcd_range(rng, v):
    """v if it is a normal number below 10^18, otherwise a random one."""
    exp = (v >> 64) & 0x7fff
    # (not below 1.0: QEMU stores the sign of the integer, SDM: of the number, -0.3 is -0)
    if v & (1 << 63) and 0x3fff <= exp <= 0x3fff + 58:
        return v
    return (rng.getrandbits(1) << 79) | ((0x3fff + rng.randrange(0, 58)) << 64) | (1 << 63) | rng.getrandbits(63)


def in_integer_range(rng, v, bits):
    """v if its truncation fits a signed integer of `bits` bits, otherwise a random normal one."""
    exp = (v >> 64) & 0x7fff
    if exp == 0 or (exp < 0x7fff and v & (1 << 63) and exp - 0x3fff < bits - 1):
        return v
    return (rng.getrandbits(1) << 79) | ((0x3fff + rng.randrange(-4, bits - 2)) << 64) | (1 << 63) | rng.getrandbits(63)


def moderate_value(rng, v):
    """v unless it is a denormal or has an extreme exponent (infinities and NaNs stay)."""
    exp = (v >> 64) & 0x7fff
    if exp != 0x7fff and abs(exp - 0x3fff) > 8000 and v & ((1 << 79) - 1):
        return normal_value(rng)
    return v


def quiet_nan(v):
    """A signaling NaN becomes quiet."""
    if (v >> 64) & 0x7fff == 0x7fff and v & (1 << 63) and v & ((1 << 63) - 1) and not v & (1 << 62):
        v |= 1 << 62
    return v


def clear_unsupported(v):
    """An unnormal or a pseudo-infinity / pseudo-NaN gets its integer bit."""
    if (v >> 64) & 0x7fff != 0:
        v |= 1 << 63
    return v


def qemu_workarounds(rng, text, st, mnemonic):
    """Keep the registers of the states away from the cases where Unicorn's x87 is wrong."""
    # QEMU's soft float computes with a pseudo-denormal (exponent 0, integer bit 1) as if the
    # integer bit were part of a denormal significand: it drops the carry of an addition and
    # returns wrong differences. The pseudo-denormals are tested by the hand corpus
    # (x87_edge.asm), the random registers get the denormal with the integer bit cleared
    st['fpr'] = [clear_pseudo_denormal(v) for v in st['fpr']]
    # QEMU's results for the classes below are wrong (oracle_bugs.txt has the evidence), the states
    # of these lines avoid them: the hand corpus has them
    if mnemonic == 'fxtract':
        # QEMU computes with a NaN, an infinity, a zero and a denormal as with a normal number
        st['fpr'] = [normal_value(rng) if (v >> 64) & 0x7fff in (0, 0x7fff) or not v & (1 << 63) else v
                     for v in st['fpr']]
    if mnemonic == 'fscale':
        # QEMU rounds the result of fscale to the precision control (the host does not, SDM: PC applies
        # to fadd, fsub, fmul, fdiv, fsqrt), and returns a signaling NaN operand as it is
        st['fcw'] |= 0x300
        st['fpr'] = [quiet_nan(v) for v in st['fpr']]
    if mnemonic in ('fldl2t', 'fldl2e', 'fldpi', 'fldlg2', 'fldln2'):
        # QEMU does not round the constants by the rounding control (always to nearest)
        st['fcw'] &= ~0xc00
    if mnemonic == 'fbstp':
        # QEMU stores wrong decimals for a value outside the BCD range (SDM: the packed BCD indefinite)
        st['fpr'] = [in_bcd_range(rng, v) for v in st['fpr']]
    if mnemonic == 'fisttp':
        # QEMU saturates a positive overflow (SDM: the integer indefinite)
        bits = {'word': 16, 'dword': 32, 'qword': 64}[re.search(r'\b(word|dword|qword)\b', text).group(1)]
        st['fpr'] = [in_integer_range(rng, v, bits) for v in st['fpr']]
    if re.match(r'^f(i?(add|sub|mul|div)r?p?|sqrt)$', mnemonic) and (st['fcw'] >> 8) & 3 != 3:
        # QEMU rounds a tiny result of a reduced precision (24 or 53 bits) to zero, whatever the
        # rounding mode; the denormals and the extreme exponents stay out of these states
        st['fpr'] = [moderate_value(rng, v) for v in st['fpr']]
    if mnemonic in ('fst', 'fstp') and re.search(r'\b[dq]word\b', text):
        # QEMU does not quiet a signaling NaN it converts to a single or a double
        st['fpr'] = [quiet_nan(v) for v in st['fpr']]
        # ... and rounds an extended denormal up to zero in the rounding mode up (SDM: the
        # smallest single / double denormal)
        st['fpr'] = [normal_value(rng) if (v >> 64) & 0x7fff == 0 and v & ((1 << 64) - 1) else v
                     for v in st['fpr']]
    if mnemonic in ('fxam', 'fscale', 'fxtract', 'fprem', 'fprem1'):
        # QEMU does not look for the unsupported encodings (unnormals, pseudo-infinities and
        # pseudo-NaNs) in these: fxam classifies them as normal numbers or NaNs, fscale and fxtract
        # compute with them. The hand corpus (x87_edge.asm) has them
        st['fpr'] = [clear_unsupported(v) for v in st['fpr']]


def x87_adjust(rng, text, st, host=False):
    """Make the x87 part of a state fit the line: a well formed stack, edge values in the
    memory operand, a 16 byte aligned fxsave area."""
    mnemonic = text.split()[0]
    lo, hi = x87_needs(text)
    n = rng.randint(lo, hi)
    if not host:
        qemu_workarounds(rng, text, st, mnemonic)
    top = (st['fsw'] >> 11) & 7
    st['ftw'] = sum(1 << ((top + i) & 7) for i in range(n))
    if host and rng.random() < 0.3:
        # the host x87 has the stack faults: any tag word
        st['ftw'] = rng.getrandbits(8)
    mem = st.setdefault('mem', {})
    if mnemonic in ('fldenv', 'frstor') and not host:
        # QEMU takes the control word and the status word of the image as they are, the processor
        # clears the reserved bits of the control word and derives ES and B from the flags: the
        # image has a plain control word and a status word without exception flags
        addr = effective_address(text, st)
        if addr is not None:
            fsw_offset = 2 if mnemonic == 'fxrstor' else 4
            fsw = (rng.randrange(8) << 11) | (rng.getrandbits(1) << 8) | (rng.getrandbits(1) << 9) | \
                (rng.getrandbits(1) << 10) | (rng.getrandbits(1) << 14)
            for offset, value in ((0, sane_control_word(rng)), (fsw_offset, fsw)):
                for i in range(2):
                    mem[(addr + offset + i) & M32] = (value >> (8 * i)) & 0xff
    if mnemonic in ('fxsave', 'fxrstor'):
        # The area must be 16 byte aligned (#GP otherwise, in QEMU too). The register form is
        # aligned through ebx, the absolute form of the corpus stays unaligned (a #GP test)
        addr = effective_address(text, st)
        if addr is not None and 'ebx' in text:
            st['ebx'] = (st['ebx'] + ((-addr) & 0xf)) & M32
            addr = effective_address(text, st)
        if addr is not None and mnemonic == 'fxrstor' and not host:
            # see fldenv: a plain control word and a status word without exception flags
            fsw = (rng.randrange(8) << 11) | (rng.getrandbits(1) << 8) | (rng.getrandbits(1) << 9) | \
                (rng.getrandbits(1) << 10) | (rng.getrandbits(1) << 14)
            for offset, value in ((0, sane_control_word(rng)), (2, fsw)):
                for i in range(2):
                    mem[(addr + offset + i) & M32] = (value >> (8 * i)) & 0xff
        if addr is not None and mnemonic == 'fxrstor':
            # a valid MXCSR (QEMU does not #GP a reserved bit, the SDM does)
            mxcsr = 0x1f80 | (rng.randrange(4) << 13) | (rng.getrandbits(1) << 15) | (rng.getrandbits(1) << 6)
            for i in range(4):
                mem[(addr + 24 + i) & M32] = (mxcsr >> (8 * i)) & 0xff
        return
    if '[' not in text or (rng.random() < 0.25 and not (mnemonic == 'fldcw' and not host)):
        return
    addr = effective_address(text, st)
    if addr is None:
        return
    size = None
    value = None
    if mnemonic == 'fldcw':
        size, value = 2, sane_control_word(rng)
    elif mnemonic == 'fbld':
        if rng.random() < 0.8:
            raw = int(''.join(str(rng.randrange(10)) for _ in range(18)), 16)
        else:
            raw = rng.getrandbits(72)
        size, value = 10, raw | (rng.getrandbits(1) << 79)
    elif re.search(r'\btword\b', text):
        size, value = 10, x87_value(rng)
    elif mnemonic[:2] == 'fi' and mnemonic not in ('fincstp', 'finit'):
        size = {'word': 2, 'dword': 4, 'qword': 8}[re.search(r'\b(word|dword|qword)\b', text).group(1)]
        edge = [0, 1, -1, 0x7fff, 0x8000, 0x7fffffff, 0x80000000, 0xffffffff, 0x7fffffffffffffff,
                0x8000000000000000, 100, 255, 1000, 65535]
        value = (rng.choice(edge) if rng.random() < 0.6 else rng.getrandbits(8 * size)) & ((1 << (8 * size)) - 1)
    elif re.search(r'\bqword\b', text):
        size, value = 8, rng.choice(F64_EDGES) if rng.random() < 0.7 else rng.getrandbits(64)
    elif re.search(r'\bdword\b', text):
        size, value = 4, rng.choice(F32_EDGES) if rng.random() < 0.7 else rng.getrandbits(32)
    if size and mnemonic == 'fld' and size in (4, 8):
        # QEMU does not quiet a signaling NaN it converts to the extended format
        fraction_bits = 23 if size == 4 else 52
        exp_mask = ((1 << (8 if size == 4 else 11)) - 1) << fraction_bits
        if value & exp_mask == exp_mask and value & ((1 << fraction_bits) - 1):
            value |= 1 << (fraction_bits - 1)
    if size:
        for i in range(size):
            mem[(addr + i) & M32] = (value >> (8 * i)) & 0xff


# --- segments ---------------------------------------------------------------

def varied_segments(rng, bits):
    """Non default selectors: the flat start or the GDT ones (32 bit), any (16 bit)."""
    if bits == 32:
        seg = {'cs': rng.choice([0, 0x08]), 'ss': 0x10}
        for n in ('ds', 'es', 'fs', 'gs'):
            seg[n] = rng.choice([0, 0x10, 0x18, 0x40])
        return seg
    seg = {'cs': rng.choice([0, 0, 0x100]), 'ss': rng.choice([0, 0x100, 0x1234])}
    for n in ('ds', 'es', 'fs', 'gs'):
        seg[n] = rng.choice([0, 0x100, 0x800, 0x1234, 0xffff])
    return seg


def data_selector(rng, bits):
    """A selector that a data segment register can load (usually)."""
    if bits == 16:
        return rng.getrandbits(16)
    return rng.choice([0x10, 0x18, 0x30, 0x40, 0x08, 0x20, 0x38]) | (rng.randrange(4) if rng.random() < 0.4 else 0)


def stack_selector(rng, bits):
    if bits == 16:
        return rng.getrandbits(16)
    return rng.choice([0x10, 0x18, 0x38]) | (rng.randrange(4) if rng.random() < 0.2 else 0)


def code_selector(rng, bits):
    """A selector for a far branch to a 32 bit code segment of CPL 0 (0x28 and
    0x48 aren't lifted: they are written in the corpus lines)."""
    if bits == 16:
        return rng.getrandbits(16)
    return rng.choice([0x08, 0x20]) | (rng.randrange(4) if rng.random() < 0.15 else 0)


def _le(value, size):
    return {i: (value >> (8 * i)) & 0xff for i in range(size)}


def _put(mem, address, value, size):
    for i, b in _le(value, size).items():
        mem[(address + i) & M32] = b


def _operand_bytes(bits, text):
    """The operand size in bytes of a line (the o16 / o32 prefixes of nasm)."""
    size = bits // 8
    if text.startswith('o16 '):
        size = 2
    elif text.startswith('o32 '):
        size = 4
    return size


def _absolute_address(bits, text, state):
    """The linear address of an absolute memory operand [seg:0x...] of the
    line, or None."""
    m = re.search(r'\[\s*(?:(es|cs|ss|ds|fs|gs)\s*:\s*)?(0x[0-9a-f]+)\s*\]', text)
    if not m:
        return None
    seg = m.group(1) or 'ds'
    disp = int(m.group(2), 16)
    if bits == 16:
        disp &= 0xffff
    return (oracle.segment_base(bits, state.get('seg', {}).get(seg, 0)) + disp) & M32


def selector_operands(rng, bits, text, state):
    """In about half of the states, give the lines that load a selector valid
    selectors / far pointers: in the register operand, at the absolute address
    the line reads, or in the stack frame. Changes the state in place."""
    text = text.lower()
    if bits == 16 and re.match(r'^(?:o16 |o32 )?(retf|iret|call .*far|call [0-9a-fx]+:)', text):
        # Unicorn doesn't wrap sp between the two accesses of these
        # instructions (a pop at sp = 0xfffe reads the next word at 0x10000):
        # keep the stack away from the 64K border
        if (state['esp'] & 0xffff) >= 0xfff0 or (state['esp'] & 0xffff) < 0x10:
            state['esp'] = (state['esp'] & ~0xffff & M32) | rng.randrange(0x100, 0x8000)
    if bits == 32 and re.match(r'^(?:o16 |o32 )?(retf|iret)', text):
        # Unicorn raises #GP for these two with the null ss of its flat start
        # (a stack access of push / pop / ret is fine). The SDM doesn't use
        # ss for a return to the same privilege level: give them a stack
        state.setdefault('seg', {}).setdefault('ss', 0x10)
    size = _operand_bytes(bits, text)
    mem = state.setdefault('mem', {})
    seg = state.get('seg', {})

    def stack_address(offset=0):
        base = oracle.segment_base(bits, seg.get('ss', 0))
        sp = state['esp'] & (0xffff if bits == 16 else M32)
        return (base + ((sp + offset) & (0xffff if bits == 16 else M32))) & M32

    if bits == 32 and size == 4 and re.match(r'^(?:o16 |o32 )?iret', text):
        # The image of EFLAGS never has the VM bit (a return to virtual 8086
        # mode isn't lifted); the other bits are not interesting garbage
        flags = 0x202
        for bit in (0x1, 0x4, 0x10, 0x40, 0x80, 0x800, 0x400, 0x40000, 0x200000):
            if rng.random() < 0.5:
                flags |= bit
        _put(mem, stack_address(8), flags, 4)
    if rng.random() < 0.5:
        return

    m = re.match(r'^(?:o16 |o32 )?mov (%s), (\w+)$' % SREGS, text)
    if m and m.group(2) in ('ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp', 'sp',
                            'eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp', 'esp'):
        sel = (stack_selector if m.group(1) == 'ss' else data_selector)(rng, bits)
        reg = m.group(2)
        full = 'e' + reg if len(reg) == 2 else reg
        state[full] = (state[full] & ~0xffff & M32) | sel
        return
    m = re.match(r'^(?:o16 |o32 )?mov (%s), .*\[' % SREGS, text)
    if m:
        address = _absolute_address(bits, text, state)
        if address is not None:
            _put(mem, address, (stack_selector if m.group(1) == 'ss' else data_selector)(rng, bits), 2)
        return
    m = re.match(r'^(?:o16 |o32 )?pop (%s)$' % SREGS, text)
    if m:
        sel = (stack_selector if m.group(1) == 'ss' else data_selector)(rng, bits)
        _put(mem, stack_address(), sel, size)
        return
    m = re.match(r'^(?:o16 |o32 )?l(ds|es|ss|fs|gs) ', text)
    if m:
        address = _absolute_address(bits, text, state)
        if address is not None:
            sel = (stack_selector if m.group(1) == 'ss' else data_selector)(rng, bits)
            _put(mem, address, rng.getrandbits(8 * size), size)
            _put(mem, address + size, sel, 2)
        return
    if re.match(r'^(?:o16 |o32 )?(jmp|call) .*far .*\[', text) or \
            re.match(r'^(?:o16 |o32 )?(jmp|call) far \[', text):
        address = _absolute_address(bits, text, state)
        if address is not None:
            # m16:16 or m16:32: the size of the offset follows the word / dword
            if re.search(r'\bdword far\b', text):
                size = 4
            elif re.search(r'\bword far\b', text):
                size = 2
            _put(mem, address, rng.randrange(0x1000, 0x5000), size)
            _put(mem, address + size, code_selector(rng, bits), 2)
        return
    m = re.match(r'^(?:o16 |o32 )?(retf|iret)', text)
    if m:
        _put(mem, stack_address(), rng.randrange(0x1000, 0x5000), size)
        _put(mem, stack_address(size), code_selector(rng, bits), 2)
        if m.group(1) == 'iret':
            flags = 0x202
            for bit in (0x1, 0x4, 0x10, 0x40, 0x80, 0x800, 0x400):
                if rng.random() < 0.5:
                    flags |= bit
            _put(mem, stack_address(2 * size), flags, size)


NAN_SELECTING = re.compile(r'^(add|sub|mul|div)(ps|pd|ss|sd) xmm([0-7]), xmm([0-7])$')


def is_nan(value, size):
    """True if `value` (the bits of an f32 / f64) is a NaN."""
    exponent_bits, fraction_bits = (8, 23) if size == 32 else (11, 52)
    exponent = (value >> fraction_bits) & ((1 << exponent_bits) - 1)
    return exponent == (1 << exponent_bits) - 1 and value & ((1 << fraction_bits) - 1) != 0


def avoid_two_nans(text, state):
    """When both operands of an add / sub / mul / div are NaNs, the SSE result is
    the first operand (SDM Vol. 1 table 4-7), but QEMU 5 picks the NaN with the
    larger significand like the x87 (oracle_bugs.txt). The random states of
    those lines have no lane with two NaNs; sse_edge.asm has them."""
    m = NAN_SELECTING.match(text.strip().lower())
    if not m or m.group(3) == m.group(4):
        return
    size = 32 if m.group(2) in ('ps', 'ss') else 64
    lanes = 1 if m.group(2) in ('ss', 'sd') else 128 // size
    first, second = int(m.group(3)), int(m.group(4))
    mask = (1 << size) - 1
    one = 0x3f800000 if size == 32 else 0x3ff0000000000000
    for lane in range(lanes):
        x = (state['xmm'][first] >> (size * lane)) & mask
        y = (state['xmm'][second] >> (size * lane)) & mask
        if is_nan(x, size) and is_nan(y, size):
            state['xmm'][second] = (state['xmm'][second] & ~(mask << (size * lane))) | (one << (size * lane))


def add_v2(bits, text, states, varied, all_fp, host=False):
    """Add the v2 keys to the random states of a line.

    varied - non default selectors in the segment registers
    all_fp - random x87/SSE state even for the lines that don't use it
    host   - the x87 lines run on the host x87 unit (--host-x87), not on Unicorn
    """
    rng = random.Random(zlib.crc32(('v2:%d:%s' % (bits, text)).encode('utf-8')))
    mnemonic = text.split()[0] if text.split() else ''
    uses_fp = all_fp or re.match(r'^(lock |rep[a-z]* )?f[a-z0-9]+$', mnemonic) or \
        re.search(r'\b(xmm[0-7]|mm[0-7]|mxcsr)\b', text) or mnemonic in ('emms', 'fxsave', 'fxrstor')
    for st in states:
        if varied:
            st['seg'] = varied_segments(rng, bits)
        if uses_fp:
            st.update(fp_state(rng))
            if re.match(r'^f[a-z0-9]+$', mnemonic):
                x87_adjust(rng, text, st, host)
            avoid_two_nans(text, st)
        selector_operands(rng, bits, text, st)
