#!/usr/bin/env python3
"""Generate tests/arm/AArch32SemanticRowsInt.inc, the integer rows of tests/TestArmAArch32Semantics.cpp:
an A32 or T32 instruction (or a short sequence, an IT block), a start state and the state it must
leave, as the Arm ARM defines them. The expected values are computed here by a small independent
model of the pseudocode (AddWithCarry, Shift_C, ARMExpandImm_C, SignedSatQ, the parallel
arithmetic, the exclusive monitor, the load / store multiple address ranges...) and not by running
dismount, so the unit test is a check of the lifter against the architecture also for what Unicorn
(the only AArch32 processor of the differential tests) gets wrong or cannot run (oracle_bugs.txt).

The words are assembled by clang (tests/arm/semantics/armasm.py).

    gen_semantic_rows_aarch32.py

Row syntax of the generated file, see tests/TestArmAArch32Semantics.cpp:
    { isThumb, "hex bytes", "text", "setup", "expected" }
The setup and expected strings are blank separated tokens: r0..r12, sp, lr, pc, apsr (N Z C V Q
bits 31-27, GE bits 19-16), exit=<kind>, mem=<addr>:<hex bytes>,... Registers which the expectation
does not name must be unchanged.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'semantics'))
import armlib
import armasm

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'AArch32SemanticRowsInt.inc')

M32 = 0xFFFFFFFF
PC = 0x1000
BASE = 0x4000


def mask(bits):
    return (1 << bits) - 1


def sext(value, bits):
    value &= mask(bits)
    return value - (1 << bits) if value >> (bits - 1) else value


def u32(value):
    return value & M32


# The APSR: N Z C V Q at bits 31-27, GE at 19-16
def apsr(n=0, z=0, c=0, v=0, q=0, ge=0):
    return (n << 31) | (z << 30) | (c << 29) | (v << 28) | (q << 27) | (ge << 16)


def flags_of(value):
    return ((value >> 31) & 1, (value >> 30) & 1, (value >> 29) & 1, (value >> 28) & 1, (value >> 27) & 1,
            (value >> 16) & 15)


# ---------------------------------------------------------------------------------------------
# The pseudocode of the Arm ARM
# ---------------------------------------------------------------------------------------------
def add_with_carry(x, y, carry_in):
    unsigned_sum = (x & M32) + (y & M32) + carry_in
    signed_sum = sext(x, 32) + sext(y, 32) + carry_in
    result = unsigned_sum & M32
    carry = 0 if result == unsigned_sum else 1
    overflow = 0 if sext(result, 32) == signed_sum else 1
    return result, carry, overflow


def shift_c(kind, value, amount, carry_in):
    """Shift_C: (result, carry out). amount is an integer >= 0."""
    value &= M32
    if kind == 'rrx':
        return ((carry_in << 31) | (value >> 1)) & M32, value & 1
    if amount == 0:
        return value, carry_in
    if kind == 'lsl':
        extended = value << amount
        return extended & M32, (extended >> 32) & 1
    if kind == 'lsr':
        extended = value
        return (extended >> amount) & M32, (extended >> (amount - 1)) & 1
    if kind == 'asr':
        extended = sext(value, 32)
        return (extended >> amount) & M32, (extended >> (amount - 1)) & 1
    if kind == 'ror':
        m = amount % 32
        result = ((value >> m) | (value << (32 - m))) & M32
        return result, (result >> 31) & 1
    raise ValueError(kind)


def signed_sat(value, bits):
    """SignedSatQ: (result, saturated)."""
    top, bottom = (1 << (bits - 1)) - 1, -(1 << (bits - 1))
    if value > top:
        return top, True
    if value < bottom:
        return bottom, True
    return value, False


def unsigned_sat(value, bits):
    top = (1 << bits) - 1
    if value > top:
        return top, True
    if value < 0:
        return 0, True
    return value, False


# ---------------------------------------------------------------------------------------------
# The rows
# ---------------------------------------------------------------------------------------------
ROWS = []


def tokens(d):
    parts = []
    for key, value in d.items():
        if key == 'exit':
            parts.append('exit=%s' % value)
        elif key == 'mem':
            parts.append('mem=' + ','.join('%x:%s' % (a, bytes(b).hex()) for a, b in value))
        else:
            parts.append('%s=%x' % (key, value))
    return ' '.join(parts)


class Pending(object):
    def __init__(self):
        self.items = []


PENDING = []


def row(arch, text, setup, expect):
    # The loads read the pattern around BASE unless the row sets the memory
    if 'mem' not in setup and (text.startswith(('ld', 'lda', 'pld', 'pli')) or ' ; ld' in text):
        setup = dict(setup)
        setup['mem'] = [(BASE - 64, window(BASE - 64, 256))]
    PENDING.append((arch, text, setup, expect))


def le(value, size):
    return list((value & mask(size * 8)).to_bytes(size, 'little'))


def flag_expect(n, z, c, v):
    return apsr(n, z, c, v)


# --- data processing ---------------------------------------------------------------------
EDGES = [0, 1, 0x7fffffff, 0x80000000, M32, 0x55555555, 0x00000100, 0xfffffffe]


def dp_result(op, a, b, carry):
    """(result, carry out, overflow, writes) of the data processing op with a (Rn), b (the
    shifter operand) and the carry in; the carry/overflow are None for the logical ops (the
    carry of those comes from the shifter)."""
    if op in ('and', 'tst'):
        return a & b, None, None
    if op in ('eor', 'teq'):
        return a ^ b, None, None
    if op == 'orr':
        return a | b, None, None
    if op == 'orn':
        return (a | (~b & M32)) & M32, None, None
    if op == 'bic':
        return a & ~b & M32, None, None
    if op == 'mov':
        return b, None, None
    if op == 'mvn':
        return ~b & M32, None, None
    if op in ('add', 'cmn'):
        return add_with_carry(a, b, 0)
    if op == 'adc':
        return add_with_carry(a, b, carry)
    if op in ('sub', 'cmp'):
        return add_with_carry(a, ~b & M32, 1)
    if op == 'sbc':
        return add_with_carry(a, ~b & M32, carry)
    if op == 'rsb':
        return add_with_carry(~a & M32, b, 1)
    if op == 'rsc':
        return add_with_carry(~a & M32, b, carry)
    raise ValueError(op)


LOGICAL = ('and', 'eor', 'orr', 'orn', 'bic', 'mov', 'mvn', 'tst', 'teq')


def dp_row(arch, op, setsflags, text, a, b_value, shifter_carry, carry_in, nzcv_in, dest='r0', rn='r1'):
    """One data processing row: the shifter operand is already evaluated (b_value, shifter_carry)."""
    res = dp_result(op, a, b_value, carry_in)
    result = res[0]
    n = (result >> 31) & 1
    z = 1 if result == 0 else 0
    if op in LOGICAL:
        c, v = shifter_carry, (nzcv_in >> 0) & 1
        v = (nzcv_in & 1)
    else:
        c, v = res[1], res[2]
    setup = {rn: a, 'apsr': apsr((nzcv_in >> 3) & 1, (nzcv_in >> 2) & 1, carry_in, nzcv_in & 1)}
    expect = {}
    if op not in ('tst', 'teq', 'cmp', 'cmn'):
        expect[dest] = result
    if setsflags:
        expect['apsr'] = apsr(n, z, c, v)
    return setup, expect


def gen_data_processing():
    # A32: register, immediate and shifted operands; a representative of every op
    ops = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc', 'orr', 'bic']
    for op in ops:
        for setsflags in (False, True):
            suffix = 's' if setsflags else ''
            for a, b in ((0x7fffffff, 1), (M32, 1), (0x80000000, 0x80000000), (5, 5), (4, 5), (0, 0),
                         (0x80000000, M32), (0x12345678, 0xedcba987)):
                for carry in (0, 1):
                    for nzcv in ((0, 0, carry, 0), (1, 1, carry, 1)):
                        nzcv_in = (nzcv[0] << 3) | (nzcv[1] << 2) | (nzcv[2] << 1) | nzcv[3]
                        setup, expect = dp_row('a32', op, setsflags, None, a, b, carry, carry, nzcv_in)
                        setup['r2'] = b
                        row('a32', '%s%s r0, r1, r2' % (op, suffix), setup, expect)
    # the compares and tests (carry from the operand)
    for op in ('cmp', 'cmn', 'tst', 'teq'):
        for a, b in ((0x7fffffff, 1), (M32, 1), (5, 5), (4, 5), (0x80000000, 0x80000000), (0xf0f0f0f0, 0x0f0f0f0f)):
            for carry in (0, 1):
                nzcv_in = (1 << 3) | (carry << 1)
                setup, expect = dp_row('a32', op, True, None, a, b, carry, carry, nzcv_in)
                setup['r2'] = b
                row('a32', '%s r1, r2' % op, setup, expect)
    # the modified immediates: ARMExpandImm_C
    for imm_text, rotation_value, rotated in (('#0x000000ff', 0xff, False), ('#0xff000000', 0xff000000, True),
                                              ('#0x80000000', 0x80000000, True), ('#0x00000104', 0x104, True),
                                              ('#0x3fc', 0x3fc, True), ('#0xc000003f', 0xc000003f, True)):
        for carry in (0, 1):
            for op in ('mov', 'mvn', 'and', 'orr', 'eor', 'bic', 'tst', 'teq'):
                a = 0xa5a5a5a5
                carry_out = ((rotation_value >> 31) & 1) if rotated else carry
                nzcv_in = (carry << 1)
                setup, expect = dp_row('a32', op, True, None, a, rotation_value, carry_out, carry, nzcv_in)
                if op in ('mov', 'mvn'):
                    row('a32', '%ss r0, %s' % (op, imm_text), setup, expect)
                elif op in ('tst', 'teq'):
                    row('a32', '%s r1, %s' % (op, imm_text), setup, expect)
                else:
                    row('a32', '%ss r0, r1, %s' % (op, imm_text), setup, expect)
    # the shifted register operand, by an immediate and by a register
    values = [0x80000001, 0x7fffffff, 0x00000001, 0xf0000000, M32, 0]
    for kind, amounts in (('lsl', (1, 4, 31)), ('lsr', (1, 4, 31, 32)), ('asr', (1, 4, 31, 32)), ('ror', (1, 4, 31))):
        for amount in amounts:
            for value in values:
                for carry in (0, 1):
                    shifted, shifter_carry = shift_c(kind, value, amount, carry)
                    setup, expect = dp_row('a32', 'mov', True, None, 0, shifted, shifter_carry, carry, carry << 1)
                    setup['r2'] = value
                    row('a32', 'movs r0, r2, %s #%d' % (kind, amount), setup, expect)
                    setup, expect = dp_row('a32', 'eor', True, None, 0x0f0f0f0f, shifted, shifter_carry, carry, carry << 1)
                    setup['r2'] = value
                    row('a32', 'eors r0, r1, r2, %s #%d' % (kind, amount), setup, expect)
                    setup, expect = dp_row('a32', 'add', True, None, 0x0f0f0f0f, shifted, shifter_carry, carry, carry << 1)
                    setup['r2'] = value
                    row('a32', 'adds r0, r1, r2, %s #%d' % (kind, amount), setup, expect)
    for carry in (0, 1):
        for value in values:
            shifted, shifter_carry = shift_c('rrx', value, 1, carry)
            setup, expect = dp_row('a32', 'mov', True, None, 0, shifted, shifter_carry, carry, carry << 1)
            setup['r2'] = value
            row('a32', 'movs r0, r2, rrx', setup, expect)
            row('a32', 'rrxs r0, r2', dict(setup), dict(expect))
    for kind in ('lsl', 'lsr', 'asr', 'ror'):
        for amount in (0, 1, 7, 31, 32, 33, 64, 255, 256, 257):
            for value in (0x80000001, 0x7fffffff, 0x1, M32):
                for carry in (0, 1):
                    shifted, shifter_carry = shift_c(kind, value, amount & 0xff, carry)
                    setup, expect = dp_row('a32', 'mov', True, None, 0, shifted, shifter_carry, carry, carry << 1)
                    setup['r2'] = value
                    setup['r3'] = amount
                    row('a32', 'movs r0, r2, %s r3' % kind, setup, expect)
                    row('a32', '%ss r0, r2, r3' % kind, dict(setup), dict(expect))
    # pc as an operand: the address + 8
    row('a32', 'mov r0, pc', {}, {'r0': PC + 8})
    row('a32', 'add r0, pc, #4', {}, {'r0': PC + 12})
    row('a32', 'sub r0, pc, #4', {}, {'r0': PC + 4})
    # T32: the 16 bit flag setting outside of an IT block, the wide forms and the modified immediates
    t16 = [('adds r0, r1, r2', 'add', 'r1', 'r2'), ('subs r0, r1, r2', 'sub', 'r1', 'r2'),
           ('ands r0, r1', 'and', 'r0', 'r1'), ('eors r0, r1', 'eor', 'r0', 'r1'), ('orrs r0, r1', 'orr', 'r0', 'r1'),
           ('bics r0, r1', 'bic', 'r0', 'r1'), ('adcs r0, r1', 'adc', 'r0', 'r1'), ('sbcs r0, r1', 'sbc', 'r0', 'r1'),
           ('mvns r0, r1', 'mvn', None, 'r1'), ('movs r0, r1', 'mov', None, 'r1')]
    for text, op, rn, rm in t16:
        for a, b in ((0x7fffffff, 1), (M32, 1), (5, 5), (0x80000000, 0x80000000), (3, 7)):
            for carry in (0, 1):
                nzcv_in = (1 << 3) | (carry << 1)
                if rn is None:
                    setup = {rm: b, 'apsr': apsr(1, 0, carry, 0)}
                    result, c_out, v_out = dp_result(op, 0, b, carry)[0], None, None
                    expect = {'r0': result, 'apsr': apsr((result >> 31) & 1, 1 if result == 0 else 0, carry, 0)}
                else:
                    first = a
                    setup = {rn: first, rm: b, 'apsr': apsr(1, 0, carry, 0)}
                    if rn == rm:
                        continue
                    res = dp_result(op, first, b, carry)
                    result = res[0]
                    if op in LOGICAL:
                        c_out, v_out = carry, 0
                    else:
                        c_out, v_out = res[1], res[2]
                    expect = {'r0': result, 'apsr': apsr((result >> 31) & 1, 1 if result == 0 else 0, c_out, v_out)}
                row('t32', text, setup, expect)
    row('t32', 'rsbs r0, r1, #0', {'r1': 5}, {'r0': u32(-5), 'apsr': apsr(1, 0, 0, 0)})
    row('t32', 'rsbs r0, r1, #0', {'r1': 0}, {'r0': 0, 'apsr': apsr(0, 1, 1, 0)})
    row('t32', 'rsbs r0, r1, #0', {'r1': 0x80000000}, {'r0': 0x80000000, 'apsr': apsr(1, 0, 0, 1)})
    row('t32', 'cmp r0, #255', {'r0': 255}, {'apsr': apsr(0, 1, 1, 0)})
    row('t32', 'movs r0, #200', {'apsr': apsr(0, 0, 1, 1)}, {'r0': 200, 'apsr': apsr(0, 0, 1, 1)})
    row('t32', 'mov r8, r1', {'r1': 9}, {'r8': 9})
    row('t32', 'add r0, r9', {'r0': 1, 'r9': 2}, {'r0': 3})
    row('t32', 'mov r0, pc', {}, {'r0': PC + 4})
    row('t32', 'add r0, sp, #8', {'sp': 0x8000}, {'r0': 0x8008})
    row('t32', 'sub sp, #16', {'sp': 0x8000}, {'sp': 0x7ff0})
    row('t32', 'adr r0, #8', {}, {'r0': ((PC + 4) & ~3) + 8})
    for op in ops + ['orn']:
        for setsflags in (False, True):
            for a, b in ((0x7fffffff, 1), (M32, 1), (5, 5), (0x80000000, 0x80000000)):
                for carry in (0, 1):
                    if op == 'rsc':
                        continue
                    suffix = 's' if setsflags else ''
                    nzcv_in = (1 << 3) | (carry << 1)
                    setup, expect = dp_row('t32', op, setsflags, None, a, b, carry, carry, nzcv_in)
                    setup['r2'] = b
                    wide = '' if op == 'orn' else '.w'
                    row('t32', '%s%s%s r0, r1, r2' % (op, suffix, wide), setup, expect)
    # the T32 modified immediates: ThumbExpandImm_C
    for text, value, rotated in (('#0x00ab00ab', 0x00ab00ab, False), ('#0xab00ab00', 0xab00ab00, False),
                                 ('#0xabababab', 0xabababab, False), ('#0x000000ff', 0xff, False),
                                 ('#0x3fc00000', 0x3fc00000, True), ('#0x80000000', 0x80000000, True),
                                 ('#0x00000f00', 0xf00, True)):
        for carry in (0, 1):
            for op in ('mov', 'mvn', 'and', 'orr', 'eor', 'bic', 'orn', 'tst', 'teq'):
                a = 0xa5a5a5a5
                carry_out = ((value >> 31) & 1) if rotated else carry
                setup, expect = dp_row('t32', op, True, None, a, value, carry_out, carry, carry << 1)
                if op in ('mov', 'mvn'):
                    row('t32', '%ss.w r0, %s' % (op, text), setup, expect)
                elif op in ('tst', 'teq'):
                    row('t32', '%s.w r1, %s' % (op, text), setup, expect)
                else:
                    row('t32', '%ss r0, r1, %s' % (op, text), setup, expect)
    for kind in ('lsl', 'lsr', 'asr', 'ror'):
        for amount in (1, 5, 31):
            for value in values:
                for carry in (0, 1):
                    shifted, shifter_carry = shift_c(kind, value, amount, carry)
                    setup, expect = dp_row('t32', 'mov', True, None, 0, shifted, shifter_carry, carry, carry << 1)
                    setup['r2'] = value
                    row('t32', '%ss.w r0, r2, #%d' % (kind, amount), setup, expect)
                    row('t32', 'eors.w r0, r1, r2, %s #%d' % (kind, amount),
                        *dp_row_with_operand(value, kind, amount, carry))
        for amount in (0, 1, 31, 32, 33, 255):
            for carry in (0, 1):
                value = 0x80000001
                shifted, shifter_carry = shift_c(kind, value, amount, carry)
                setup, expect = dp_row('t32', 'mov', True, None, 0, shifted, shifter_carry, carry, carry << 1)
                setup['r2'] = value
                setup['r3'] = amount
                row('t32', '%ss.w r0, r2, r3' % kind, setup, expect)


def dp_row_with_operand(value, kind, amount, carry):
    shifted, shifter_carry = shift_c(kind, value, amount, carry)
    setup, expect = dp_row('t32', 'eor', True, None, 0x0f0f0f0f, shifted, shifter_carry, carry, carry << 1)
    setup['r2'] = value
    return setup, expect


# --- conditions ----------------------------------------------------------------------------
def cond_holds(cond, n, z, c, v):
    base = {0: z == 1, 1: z == 0, 2: c == 1, 3: c == 0, 4: n == 1, 5: n == 0, 6: v == 1, 7: v == 0,
            8: c == 1 and z == 0, 9: c == 0 or z == 1, 10: n == v, 11: n != v,
            12: z == 0 and n == v, 13: z == 1 or n != v}[cond]
    return base


CONDS = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le']


def gen_conditions():
    for index, name in enumerate(CONDS):
        for nzcv in range(16):
            n, z, c, v = (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1
            taken = cond_holds(index, n, z, c, v)
            setup = {'r1': 3, 'r2': 4, 'apsr': apsr(n, z, c, v)}
            expect = {'r0': 7} if taken else {}
            row('a32', 'add%s r0, r1, r2' % name, setup, expect)
            row('a32', 'adds%s r0, r1, r2' % name, setup,
                {'r0': 7, 'apsr': apsr(0, 0, 0, 0)} if taken else {})
            # a conditional branch
            expect_branch = {'exit': 'branch', 'pc': PC + 8 + 0x20} if taken else {}
            row('a32', 'b%s #0x20' % name, setup, expect_branch)
            # T32: a block of one instruction, and the 16 bit branch
            row('t32', 'it %s ; add%s r0, r1, r2' % (name, name), setup, expect)
            row('t32', 'b%s #0x20' % name, setup,
                {'exit': 'branch', 'pc': PC + 4 + 0x20} if taken else {})
    # IT blocks: the then / else conditions
    inverse = {0: 1, 1: 0, 2: 3, 3: 2, 4: 5, 5: 4, 6: 7, 7: 6, 8: 9, 9: 8, 10: 11, 11: 10, 12: 13, 13: 12}
    for pattern in ('', 't', 'e', 'tt', 'te', 'et', 'ee', 'ttt', 'tte', 'tet', 'tee', 'ett', 'ete', 'eet', 'eee'):
        slots = ['t'] + list(pattern)
        for cond in (0, 1, 8, 9, 10, 13):
            for nzcv in (0, 4, 2, 6, 8, 9, 12):
                n, z, c, v = (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1
                holds = cond_holds(cond, n, z, c, v)
                lines = []
                expect = {}
                for k, slot in enumerate(slots):
                    use = cond if slot == 't' else inverse[cond]
                    lines.append('add%s r%d, r%d, #%d' % (CONDS[use], k, k, k + 1))
                    if holds == (slot == 't'):
                        expect['r%d' % k] = k + 1 + 10 * k
                setup = {'r%d' % k: 10 * k for k in range(len(slots))}
                setup['apsr'] = apsr(n, z, c, v)
                row('t32', 'it%s %s ; %s' % (pattern, CONDS[cond], ' ; '.join(lines)), setup, expect)
    # the flags are not set by the 16 bit instruction inside of an IT block, and set outside
    row('t32', 'it al ; add r0, r1, r2', {'r1': M32, 'r2': 1, 'apsr': apsr(0, 0, 0, 0)}, {'r0': 0})
    row('t32', 'add r0, r1, r2', {'r1': 1, 'r2': 2}, {'r0': 3}) if False else None
    row('t32', 'adds r0, r1, r2', {'r1': M32, 'r2': 1}, {'r0': 0, 'apsr': apsr(0, 1, 1, 0)})


# --- multiplies ---------------------------------------------------------------------------------
def gen_multiplies():
    for a, b, acc in ((3, 5, 7), (M32, M32, 1), (0x80000000, 2, 0), (0x7fffffff, 0x7fffffff, M32), (0, 9, 9)):
        prod = (a * b) & M32
        row('a32', 'mul r0, r1, r2', {'r1': a, 'r2': b}, {'r0': prod})
        row('a32', 'muls r0, r1, r2', {'r1': a, 'r2': b, 'apsr': apsr(0, 0, 1, 1)},
            {'r0': prod, 'apsr': apsr((prod >> 31) & 1, 1 if prod == 0 else 0, 1, 1)})
        row('a32', 'mla r0, r1, r2, r3', {'r1': a, 'r2': b, 'r3': acc}, {'r0': (prod + acc) & M32})
        row('a32', 'mls r0, r1, r2, r3', {'r1': a, 'r2': b, 'r3': acc}, {'r0': (acc - prod) & M32})
        row('t32', 'mul r0, r1, r2', {'r1': a, 'r2': b}, {'r0': prod})
        row('t32', 'mla r0, r1, r2, r3', {'r1': a, 'r2': b, 'r3': acc}, {'r0': (prod + acc) & M32})
        row('t32', 'mls r0, r1, r2, r3', {'r1': a, 'r2': b, 'r3': acc}, {'r0': (acc - prod) & M32})
        row('t32', 'muls r0, r1, r0', {'r1': a, 'r0': b, 'apsr': apsr(0, 0, 1, 1)},
            {'r0': prod, 'apsr': apsr((prod >> 31) & 1, 1 if prod == 0 else 0, 1, 1)})
    for a, b in ((3, 5), (M32, M32), (0x80000000, 2), (0x7fffffff, 0x80000000), (0x12345678, 0x9abcdef0)):
        ulong = a * b
        slong = (sext(a, 32) * sext(b, 32)) & mask(64)
        lo, hi = 0x11111111, 0x22222222
        acc = (hi << 32) | lo
        for arch in ('a32', 't32'):
            row(arch, 'umull r0, r1, r2, r3', {'r2': a, 'r3': b},
                {'r0': ulong & M32, 'r1': (ulong >> 32) & M32})
            row(arch, 'smull r0, r1, r2, r3', {'r2': a, 'r3': b},
                {'r0': slong & M32, 'r1': (slong >> 32) & M32})
            row(arch, 'umlal r0, r1, r2, r3', {'r0': lo, 'r1': hi, 'r2': a, 'r3': b},
                {'r0': (ulong + acc) & M32, 'r1': ((ulong + acc) >> 32) & M32})
            row(arch, 'smlal r0, r1, r2, r3', {'r0': lo, 'r1': hi, 'r2': a, 'r3': b},
                {'r0': (slong + acc) & M32, 'r1': ((slong + acc) >> 32) & M32})
            row(arch, 'umaal r0, r1, r2, r3', {'r0': lo, 'r1': hi, 'r2': a, 'r3': b},
                {'r0': (ulong + lo + hi) & M32, 'r1': ((ulong + lo + hi) >> 32) & M32})
        row('a32', 'umulls r0, r1, r2, r3' if False else 'umulls r0, r1, r2, r3', {'r2': a, 'r3': b}, {
            'r0': ulong & M32, 'r1': (ulong >> 32) & M32,
            'apsr': apsr((ulong >> 63) & 1, 1 if ulong & mask(64) == 0 else 0, 0, 0)}) if False else None
        row('a32', 'umulls r0, r1, r2, r3', {'r2': a, 'r3': b}, {
            'r0': ulong & M32, 'r1': (ulong >> 32) & M32,
            'apsr': apsr((ulong >> 63) & 1, 1 if ulong == 0 else 0, 0, 0)})
    # the halfword multiplies and Q
    for a, b, acc in ((0x7fff7fff, 0x7fff7fff, 0x7fffffff), (0x80008000, 0x80008000, 0x80000000),
                      (0x12348765, 0xfedc0123, 5), (0xffff8000, 0x00018000, M32)):
        halves = lambda v: (sext(v & 0xffff, 16), sext(v >> 16, 16))
        for name, ai, bi in (('bb', 0, 0), ('bt', 0, 1), ('tb', 1, 0), ('tt', 1, 1)):
            x, y = halves(a)[ai], halves(b)[bi]
            product = x * y
            for arch in ('a32', 't32'):
                row(arch, 'smul%s r0, r1, r2' % name, {'r1': a, 'r2': b}, {'r0': product & M32})
                total = product + sext(acc, 32)
                q = 0 if sext(total & M32, 32) == total else 1
                row(arch, 'smla%s r0, r1, r2, r3' % name, {'r1': a, 'r2': b, 'r3': acc},
                    {'r0': total & M32, 'apsr': apsr(q=q)})
                long_total = product + sext(0x22222222, 32) * (1 << 32) + 0x11111111
                row(arch, 'smlal%s r0, r1, r2, r3' % name, {'r0': 0x11111111, 'r1': 0x22222222, 'r2': a, 'r3': b},
                    {'r0': long_total & M32, 'r1': (long_total >> 32) & M32})
        for name, bi in (('b', 0), ('t', 1)):
            product = (sext(a, 32) * halves(b)[bi]) >> 16
            for arch in ('a32', 't32'):
                row(arch, 'smulw%s r0, r1, r2' % name, {'r1': a, 'r2': b}, {'r0': product & M32})
                total = product + sext(acc, 32)
                q = 0 if sext(total & M32, 32) == total else 1
                row(arch, 'smlaw%s r0, r1, r2, r3' % name, {'r1': a, 'r2': b, 'r3': acc},
                    {'r0': total & M32, 'apsr': apsr(q=q)})
        # dual multiplies: the Arm ARM sums in unbounded integers (SMLAD's Q is the final sum only)
        for swap in (False, True):
            sx = '' if not swap else 'x'
            b2 = ((b >> 16) | ((b & 0xffff) << 16)) & M32 if swap else b
            p1 = halves(a)[0] * halves(b2)[0]
            p2 = halves(a)[1] * halves(b2)[1]
            for arch in ('a32', 't32'):
                for name, total, uses_acc in (('smuad', p1 + p2, False), ('smusd', p1 - p2, False),
                                              ('smlad', p1 + p2 + sext(acc, 32), True),
                                              ('smlsd', p1 - p2 + sext(acc, 32), True)):
                    q = 0 if (name == 'smusd' or sext(total & M32, 32) == total) else 1
                    text = '%s%s r0, r1, r2' % (name, sx) + (', r3' if uses_acc else '')
                    row(arch, text, {'r1': a, 'r2': b, 'r3': acc}, {'r0': total & M32, 'apsr': apsr(q=q)})
                for name, total in (('smlald', p1 + p2), ('smlsld', p1 - p2)):
                    acc64 = (0x22222222 << 32) | 0x11111111
                    full = total + sext(acc64, 64)
                    row(arch, '%s%s r0, r1, r2, r3' % (name, sx),
                        {'r0': 0x11111111, 'r1': 0x22222222, 'r2': a, 'r3': b},
                        {'r0': full & M32, 'r1': (full >> 32) & M32})
        # most significant word
        prod = sext(a, 32) * sext(b, 32)
        for arch in ('a32', 't32'):
            row(arch, 'smmul r0, r1, r2', {'r1': a, 'r2': b}, {'r0': (prod >> 32) & M32})
            row(arch, 'smmulr r0, r1, r2', {'r1': a, 'r2': b}, {'r0': ((prod + 0x80000000) >> 32) & M32})
            full = (acc << 32) + prod
            row(arch, 'smmla r0, r1, r2, r3', {'r1': a, 'r2': b, 'r3': acc}, {'r0': (full >> 32) & M32})
            row(arch, 'smmlar r0, r1, r2, r3', {'r1': a, 'r2': b, 'r3': acc},
                {'r0': ((full + 0x80000000) >> 32) & M32})
            full = (acc << 32) - prod
            row(arch, 'smmls r0, r1, r2, r3', {'r1': a, 'r2': b, 'r3': acc}, {'r0': (full >> 32) & M32})
            row(arch, 'smmlsr r0, r1, r2, r3', {'r1': a, 'r2': b, 'r3': acc},
                {'r0': ((full + 0x80000000) >> 32) & M32})
    for a, b in ((0x01020304, 0x04030201), (0xff00ff00, 0x00ff00ff), (0x80808080, 0x7f7f7f7f)):
        total = sum(abs(((a >> (8 * i)) & 0xff) - ((b >> (8 * i)) & 0xff)) for i in range(4))
        for arch in ('a32', 't32'):
            row(arch, 'usad8 r0, r1, r2', {'r1': a, 'r2': b}, {'r0': total})
            row(arch, 'usada8 r0, r1, r2, r3', {'r1': a, 'r2': b, 'r3': 100}, {'r0': (total + 100) & M32})
    # divide: by zero is zero, the minimum / -1 is the minimum
    for a, b in ((100, 7), (M32, 3), (0x80000000, M32), (5, 0), (0x80000000, 2), (7, M32)):
        sdiv = 0 if b == 0 else int(sext(a, 32) / sext(b, 32)) if abs(sext(a, 32)) < (1 << 52) else 0
        if b != 0:
            q = abs(sext(a, 32)) // abs(sext(b, 32))
            sdiv = q if (sext(a, 32) < 0) == (sext(b, 32) < 0) else -q
        udiv = 0 if b == 0 else a // b
        for arch in ('a32', 't32'):
            row(arch, 'sdiv r0, r1, r2', {'r1': a, 'r2': b}, {'r0': sdiv & M32})
            row(arch, 'udiv r0, r1, r2', {'r1': a, 'r2': b}, {'r0': udiv & M32})


# --- saturating, parallel, extends, bit fields -----------------------------------------------------
def gen_dsp():
    for a, b in ((0x7fffffff, 1), (0x80000000, M32), (5, 6), (0x40000000, 0x40000000), (0x80000000, 0x80000000),
                 (0x7fffffff, M32)):
        sa, sb = sext(a, 32), sext(b, 32)
        for arch in ('a32', 't32'):
            r, s = signed_sat(sa + sb, 32)
            row(arch, 'qadd r0, r1, r2', {'r1': a, 'r2': b}, {'r0': r & M32, 'apsr': apsr(q=1 if s else 0)})
            r, s = signed_sat(sa - sb, 32)
            row(arch, 'qsub r0, r1, r2', {'r1': a, 'r2': b}, {'r0': r & M32, 'apsr': apsr(q=1 if s else 0)})
            doubled, s1 = signed_sat(2 * sb, 32)
            r, s2 = signed_sat(sa + doubled, 32)
            row(arch, 'qdadd r0, r1, r2', {'r1': a, 'r2': b}, {'r0': r & M32, 'apsr': apsr(q=1 if (s1 or s2) else 0)})
            r, s2 = signed_sat(sa - doubled, 32)
            row(arch, 'qdsub r0, r1, r2', {'r1': a, 'r2': b}, {'r0': r & M32, 'apsr': apsr(q=1 if (s1 or s2) else 0)})
    # Q is sticky
    row('a32', 'qadd r0, r1, r2', {'r1': 1, 'r2': 2, 'apsr': apsr(q=1)}, {'r0': 3, 'apsr': apsr(q=1)})
    for n in (1, 8, 16, 32):
        for value in (0x7fffffff, 0x80000000, 0x100, 0xffffff00, 0, 1, M32):
            for arch in ('a32', 't32'):
                r, s = signed_sat(sext(value, 32), n)
                row(arch, 'ssat r0, #%d, r1' % n, {'r1': value}, {'r0': r & M32, 'apsr': apsr(q=1 if s else 0)})
    for n in (0, 1, 8, 16, 31):
        for value in (0x7fffffff, 0x80000000, 0x100, 0xffffff00, 0, 1, 0xff, M32):
            for arch in ('a32', 't32'):
                r, s = unsigned_sat(sext(value, 32), n)
                row(arch, 'usat r0, #%d, r1' % n, {'r1': value}, {'r0': r & M32, 'apsr': apsr(q=1 if s else 0)})
    for shift_text, fn in (('lsl #4', lambda v: sext(v << 4, 32)), ('asr #8', lambda v: sext(v, 32) >> 8)):
        for value in (0x7ffff, 0xfff80000, 0x7fffffff, 0x80000000):
            r, s = signed_sat(fn(value), 8)
            row('a32', 'ssat r0, #8, r1, %s' % shift_text, {'r1': value}, {'r0': r & M32, 'apsr': apsr(q=1 if s else 0)})
            r, s = unsigned_sat(fn(value), 8)
            row('a32', 'usat r0, #8, r1, %s' % shift_text, {'r1': value}, {'r0': r & M32, 'apsr': apsr(q=1 if s else 0)})
    for n in (1, 8, 16):
        for value in (0x7fff8000, 0x00ff0100, 0xff00ffff):
            lo, so = signed_sat(sext(value & 0xffff, 16), n)
            hi, sh = signed_sat(sext(value >> 16, 16), n)
            row('a32', 'ssat16 r0, #%d, r1' % n, {'r1': value},
                {'r0': ((hi & 0xffff) << 16) | (lo & 0xffff), 'apsr': apsr(q=1 if (so or sh) else 0)})
    for n in (0, 1, 8, 15):
        for value in (0x7fff8000, 0x00ff0100, 0xff00ffff):
            lo, so = unsigned_sat(sext(value & 0xffff, 16), n)
            hi, sh = unsigned_sat(sext(value >> 16, 16), n)
            row('a32', 'usat16 r0, #%d, r1' % n, {'r1': value},
                {'r0': ((hi & 0xffff) << 16) | (lo & 0xffff), 'apsr': apsr(q=1 if (so or sh) else 0)})
    # parallel add / subtract
    def lanes(v, width):
        return [(v >> (width * i)) & mask(width) for i in range(32 // width)]

    def pack(values, width):
        return sum((v & mask(width)) << (width * i) for i, v in enumerate(values))

    for a, b in ((0x7fff8000, 0x00018000), (0x80008000, 0x80008000), (0xffff0001, 0x00010001),
                 (0x12345678, 0x9abcdef0), (0xff80007f, 0x017f0080), (0x7f7f7f7f, 0x01010101),
                 (0x80808080, 0x80808080), (0x00ff00ff, 0xff00ff00)):
        for prefix in ('s', 'q', 'sh', 'u', 'uq', 'uh'):
            unsigned = prefix.startswith('u')
            for op in ('add16', 'add8', 'sub16', 'sub8', 'asx', 'sax'):
                width = 8 if op.endswith('8') else 16
                n = 32 // width
                av, bv = lanes(a, width), lanes(b, width)
                conv = (lambda x: x) if unsigned else (lambda x: sext(x, width))
                outputs, ges = [], []
                for i in range(n):
                    if op in ('add16', 'add8'):
                        total = conv(av[i]) + conv(bv[i])
                        is_add = True
                    elif op in ('sub16', 'sub8'):
                        total = conv(av[i]) - conv(bv[i])
                        is_add = False
                    elif op == 'asx':
                        # Rd[15:0] = Rn[15:0] - Rm[31:16]; Rd[31:16] = Rn[31:16] + Rm[15:0]
                        total = (conv(av[0]) - conv(bv[1])) if i == 0 else (conv(av[1]) + conv(bv[0]))
                        is_add = i == 1
                    else:
                        total = (conv(av[0]) + conv(bv[1])) if i == 0 else (conv(av[1]) - conv(bv[0]))
                        is_add = i == 0
                    if prefix in ('s', 'u'):
                        outputs.append(total & mask(width))
                        ge = (total >= (1 << width)) if (unsigned and is_add) else (total >= 0)
                        ges.append(1 if ge else 0)
                    elif prefix == 'q':
                        outputs.append(signed_sat(total, width)[0] & mask(width))
                    elif prefix == 'uq':
                        outputs.append(unsigned_sat(total, width)[0] & mask(width))
                    else:
                        outputs.append((total >> 1) & mask(width))
                expect = {'r0': pack(outputs, width)}
                if prefix in ('s', 'u'):
                    ge_bits = 0
                    for i in range(4):
                        ge_bits |= ges[i if width == 8 else i // 2] << i
                    expect['apsr'] = apsr(ge=ge_bits)
                for arch in ('a32', 't32'):
                    row(arch, '%s%s r0, r1, r2' % (prefix, op), {'r1': a, 'r2': b}, dict(expect))
    for ge in range(16):
        a, b = 0x11223344, 0xaabbccdd
        result = 0
        for i in range(4):
            byte = ((a if (ge >> i) & 1 else b) >> (8 * i)) & 0xff
            result |= byte << (8 * i)
        row('a32', 'sel r0, r1, r2', {'r1': a, 'r2': b, 'apsr': apsr(ge=ge)}, {'r0': result})
        row('t32', 'sel r0, r1, r2', {'r1': a, 'r2': b, 'apsr': apsr(ge=ge)}, {'r0': result})
    # extends
    for value in (0x80ff7f01, 0x01ff0180, 0xfedcba98):
        for rot in (0, 8, 16, 24):
            rotated = ((value >> rot) | (value << (32 - rot))) & M32 if rot else value
            rot_text = ', ror #%d' % rot if rot else ''
            for arch in ('a32', 't32'):
                row(arch, 'sxtb r0, r1%s' % rot_text, {'r1': value}, {'r0': sext(rotated & 0xff, 8) & M32})
                row(arch, 'sxth r0, r1%s' % rot_text, {'r1': value}, {'r0': sext(rotated & 0xffff, 16) & M32})
                row(arch, 'uxtb r0, r1%s' % rot_text, {'r1': value}, {'r0': rotated & 0xff})
                row(arch, 'uxth r0, r1%s' % rot_text, {'r1': value}, {'r0': rotated & 0xffff})
                lo, hi = sext(rotated & 0xff, 8) & 0xffff, sext((rotated >> 16) & 0xff, 8) & 0xffff
                row(arch, 'sxtb16 r0, r1%s' % rot_text, {'r1': value}, {'r0': (hi << 16) | lo})
                row(arch, 'uxtb16 r0, r1%s' % rot_text, {'r1': value},
                    {'r0': (rotated & 0xff) | (rotated & 0xff0000)})
                for acc in (0x00010002, 0xffff8000):
                    row(arch, 'sxtab r0, r2, r1%s' % rot_text, {'r1': value, 'r2': acc},
                        {'r0': (acc + sext(rotated & 0xff, 8)) & M32})
                    row(arch, 'uxtah r0, r2, r1%s' % rot_text, {'r1': value, 'r2': acc},
                        {'r0': (acc + (rotated & 0xffff)) & M32})
                    lo = (((acc & 0xffff) + sext(rotated & 0xff, 8)) & 0xffff)
                    hi = ((((acc >> 16) & 0xffff) + sext((rotated >> 16) & 0xff, 8)) & 0xffff)
                    row(arch, 'sxtab16 r0, r2, r1%s' % rot_text, {'r1': value, 'r2': acc}, {'r0': (hi << 16) | lo})
    # pack
    for a, b in ((0x12345678, 0x9abcdef0), (0xffff0000, 0x0000ffff)):
        for n in (0, 8, 16, 31):
            shifted = (b << n) & M32
            for arch in ('a32', 't32'):
                row(arch, 'pkhbt r0, r1, r2, lsl #%d' % n if n else 'pkhbt r0, r1, r2', {'r1': a, 'r2': b},
                    {'r0': (shifted & 0xffff0000) | (a & 0xffff)})
        for n in (8, 16, 31, 32):
            shifted = (sext(b, 32) >> n) & M32
            for arch in ('a32', 't32'):
                row(arch, 'pkhtb r0, r1, r2, asr #%d' % n, {'r1': a, 'r2': b},
                    {'r0': (a & 0xffff0000) | (shifted & 0xffff)})
    # bit fields
    for lsb, width in ((0, 1), (0, 8), (4, 8), (16, 16), (31, 1), (0, 32), (5, 27)):
        field_mask = (mask(width) << lsb) & M32
        for arch in ('a32', 't32'):
            row(arch, 'bfc r0, #%d, #%d' % (lsb, width), {'r0': M32}, {'r0': M32 & ~field_mask})
            row(arch, 'bfi r0, r1, #%d, #%d' % (lsb, width), {'r0': 0x5a5a5a5a, 'r1': 0x12345678},
                {'r0': (0x5a5a5a5a & ~field_mask & M32) | ((0x12345678 << lsb) & field_mask)})
            row(arch, 'ubfx r0, r1, #%d, #%d' % (lsb, width), {'r1': 0x9abcdef0},
                {'r0': (0x9abcdef0 >> lsb) & mask(width)})
            row(arch, 'sbfx r0, r1, #%d, #%d' % (lsb, width), {'r1': 0x9abcdef0},
                {'r0': sext((0x9abcdef0 >> lsb) & mask(width), width) & M32})
    # reverse
    for value in (0, 1, 0x80000000, M32, 0x00ff00ff, 0x12345678, 0x8000):
        bits = format(value, '032b')
        clz = 32 if value == 0 else len(bits) - len(bits.lstrip('0'))
        rev = int.from_bytes(value.to_bytes(4, 'little'), 'big')
        rev16 = int.from_bytes(b''.join(value.to_bytes(4, 'little')[i:i + 2][::-1] for i in (0, 2)), 'little')
        low = value & 0xffff
        revsh = sext(((low & 0xff) << 8) | (low >> 8), 16) & M32
        for arch in ('a32', 't32'):
            row(arch, 'clz r0, r1', {'r1': value}, {'r0': clz})
            row(arch, 'rbit r0, r1', {'r1': value}, {'r0': int(bits[::-1], 2)})
            row(arch, 'rev r0, r1', {'r1': value}, {'r0': rev})
            row(arch, 'rev16 r0, r1', {'r1': value}, {'r0': rev16})
            row(arch, 'revsh r0, r1', {'r1': value}, {'r0': revsh})
    # crc32 (the reflected CRC-32 of the IEEE and Castagnoli polynomials, bit by bit)
    def crc(initial, data, size, poly):
        value = initial
        for byte in data.to_bytes(size, 'little'):
            value ^= byte
            for _ in range(8):
                value = (value >> 1) ^ (poly if value & 1 else 0)
        return value & M32

    for initial, data in ((0, 0), (M32, 0x12345678), (0x12345678, 0xabcdef01)):
        for name, size in (('b', 1), ('h', 2), ('w', 4)):
            for arch in ('a32', 't32'):
                row(arch, 'crc32%s r0, r1, r2' % name, {'r1': initial, 'r2': data},
                    {'r0': crc(initial, data & mask(8 * size), size, 0xEDB88320)})
                row(arch, 'crc32c%s r0, r1, r2' % name, {'r1': initial, 'r2': data},
                    {'r0': crc(initial, data & mask(8 * size), size, 0x82F63B78)})


# --- branches ---------------------------------------------------------------------------------------
def gen_branches():
    for target in (0x2001, 0x2000, 0x2002, 0x2003):
        thumb = target & 1
        address = (target & ~1) if thumb else (target & ~3)
        for arch in ('a32', 't32'):
            row(arch, 'bx r1', {'r1': target}, {'exit': 'branch', 'pc': address, 't': thumb})
            link = (PC + 2) | 1 if arch == 't32' else PC + 4
            row(arch, 'blx r1', {'r1': target}, {'exit': 'branch', 'pc': address, 't': thumb, 'lr': link})
    row('a32', 'b #8', {}, {'exit': 'branch', 'pc': PC + 8 + 8})
    row('a32', 'bl #16', {}, {'exit': 'branch', 'pc': PC + 8 + 16, 'lr': PC + 4})
    row('t32', 'b #8', {}, {'exit': 'branch', 'pc': PC + 4 + 8})
    row('t32', 'bl #16', {}, {'exit': 'branch', 'pc': PC + 4 + 16, 'lr': (PC + 4) | 1})
    row('a32', 'blx #16', {}, {'exit': 'branch', 'pc': PC + 8 + 16, 'lr': PC + 4, 't': 1})
    row('t32', 'blx #16', {}, {'exit': 'branch', 'pc': ((PC + 4) & ~3) + 16, 'lr': (PC + 4) | 1, 't': 0})
    for value in (0, 1):
        row('t32', 'cbz r0, #8', {'r0': value}, {'exit': 'branch', 'pc': PC + 4 + 8} if value == 0 else {})
        row('t32', 'cbnz r0, #8', {'r0': value}, {'exit': 'branch', 'pc': PC + 4 + 8} if value != 0 else {})
    # tbb / tbh: pc + 2 * the table entry
    row('t32', 'tbb [r4, r5]', {'r4': BASE, 'r5': 2, 'mem': [(BASE, [0x0c, 0x0b, 0x0a, 0x09])]},
        {'exit': 'branch', 'pc': PC + 4 + 2 * 0x0a})
    row('t32', 'tbh [r4, r5, lsl #1]', {'r4': BASE, 'r5': 2, 'mem': [(BASE, le(0x0c, 2) + le(0x0b, 2) + le(0x0123, 2))]},
        {'exit': 'branch', 'pc': PC + 4 + 2 * 0x0123})
    # mov pc / the A32 data processing interworks, the T32 one doesn't
    for value, address, thumb in ((0x4001, 0x4000, 1), (0x4002, 0x4000, 0), (0x4003, 0x4002, 1), (0x4000, 0x4000, 0)):
        row('a32', 'mov pc, r1', {'r1': value}, {'exit': 'branch', 'pc': address, 't': thumb})
    for value in (0x4001, 0x4002, 0x4003):
        row('t32', 'mov pc, r1', {'r1': value}, {'exit': 'branch', 'pc': value & ~1, 't': 1})
    # the exceptions
    row('a32', 'svc #5', {}, {'exit': 'trap:svc'})
    row('t32', 'svc #5', {}, {'exit': 'trap:svc'})
    row('a32', 'bkpt #3', {}, {'exit': 'trap:brk'})
    row('a32', 'udf #1', {}, {'exit': 'trap:undef'})
    row('t32', 'udf #1', {}, {'exit': 'trap:undef'})


# --- the APSR ---------------------------------------------------------------------------------------------
def gen_apsr():
    for flags in (0, 0xf8000000, 0xf80f0000, 0x80050000, 0x08000000):
        for arch in ('a32', 't32'):
            row(arch, 'mrs r0, apsr', {'apsr': flags}, {'r0': flags | 0x10})
    for value in (0xf80f0000, 0x08000000, 0x000f0000, 0x00050000, M32, 0):
        for field, field_mask in (('apsr_nzcvq', 0xf8000000), ('apsr_g', 0x000f0000), ('apsr_nzcvqg', 0xf80f0000)):
            for arch in ('a32', 't32'):
                row(arch, 'msr %s, r1' % field, {'r1': value, 'apsr': apsr(1, 0, 1, 0, 0, 5)},
                    {'apsr': ((value & field_mask) | (apsr(1, 0, 1, 0, 0, 5) & ~field_mask & 0xf80f0000))})
    row('a32', 'msr apsr_nzcvq, #0x80000000', {}, {'apsr': 0x80000000})
    row('a32', 'msr apsr_nzcvq, #0xf0000000', {}, {'apsr': 0xf0000000})
    row('a32', 'msr apsr_g, #0x00050000', {}, {'apsr': 0x00050000})


# --- loads and stores -------------------------------------------------------------------------------------
PATTERN = bytes((i * 7 + 0x13) & 0xff for i in range(256))


def window(address, size):
    return [PATTERN[(address + i) & 0xff] for i in range(size)]


def mem_setup(*ranges):
    entries = []
    for address, size in ranges:
        entries.append((address, window(address, size)))
    return entries


def gen_memory():
    base = BASE
    for arch in ('a32', 't32'):
        for text, offset, size, signed, writeback, post in (
                ('ldr r0, [r4]', 0, 4, False, None, False), ('ldr r0, [r4, #8]', 8, 4, False, None, False),
                ('ldr r0, [r4, #-8]', -8, 4, False, None, False), ('ldr r0, [r4, #8]!', 8, 4, False, 8, False),
                ('ldr r0, [r4], #8', 0, 4, False, 8, True), ('ldr r0, [r4], #-8', 0, 4, False, -8, True),
                ('ldrb r0, [r4, #3]', 3, 1, False, None, False), ('ldrh r0, [r4, #2]', 2, 2, False, None, False),
                ('ldrsb r0, [r4, #3]', 3, 1, True, None, False), ('ldrsh r0, [r4, #2]', 2, 2, True, None, False),
                ('ldrsb r0, [r4, #3]!', 3, 1, True, 3, False), ('ldrsh r0, [r4], #6', 0, 2, True, 6, True)):
            if arch == 't32' and text.startswith(('ldrsb', 'ldrsh')) and ('!' in text or '], #' in text):
                pass
            address = base + offset
            raw = int.from_bytes(bytes(window(address, size)), 'little')
            value = sext(raw, size * 8) & M32 if signed else raw
            expect = {'r0': value}
            if writeback is not None:
                expect['r4'] = base + writeback
            row(arch, text, {'r4': base}, expect)
        for text, offset, size, writeback in (
                ('str r0, [r4]', 0, 4, None), ('str r0, [r4, #8]', 8, 4, None), ('str r0, [r4, #-8]!', -8, 4, -8),
                ('str r0, [r4], #8', 0, 4, 8), ('strb r0, [r4, #3]', 3, 1, None), ('strh r0, [r4, #2]', 2, 2, None),
                ('strb r0, [r4], #-1', 0, 1, -1)):
            value = 0x89abcdef
            expect = {'mem': [(base + offset, le(value, size))]}
            if writeback is not None:
                expect['r4'] = base + writeback
            row(arch, text, {'r0': value, 'r4': base}, expect)
    # register offsets and the shifts
    row('a32', 'ldr r0, [r4, r5]', {'r4': base, 'r5': 8}, {'r0': int.from_bytes(bytes(window(base + 8, 4)), 'little')})
    row('a32', 'ldr r0, [r4, -r5]', {'r4': base, 'r5': 8}, {'r0': int.from_bytes(bytes(window(base - 8, 4)), 'little')})
    row('a32', 'ldr r0, [r4, r5, lsl #2]', {'r4': base, 'r5': 3}, {'r0': int.from_bytes(bytes(window(base + 12, 4)), 'little')})
    row('a32', 'ldr r0, [r4, r5, lsr #1]', {'r4': base, 'r5': 16}, {'r0': int.from_bytes(bytes(window(base + 8, 4)), 'little')})
    row('a32', 'ldr r0, [r4, r5, asr #4]', {'r4': base, 'r5': 0x80}, {'r0': int.from_bytes(bytes(window(base + 8, 4)), 'little')})
    row('a32', 'ldr r0, [r4, r5, ror #4]', {'r4': base, 'r5': 0x80}, {'r0': int.from_bytes(bytes(window(base + 8, 4)), 'little')})
    row('a32', 'ldr r0, [r4, r5, rrx]', {'r4': base, 'r5': 0x10, 'apsr': apsr(c=0)},
        {'r0': int.from_bytes(bytes(window(base + 8, 4)), 'little')})
    row('a32', 'ldr r0, [r4], r5, lsl #2', {'r4': base, 'r5': 3},
        {'r0': int.from_bytes(bytes(window(base, 4)), 'little'), 'r4': base + 12})
    row('a32', 'ldr r0, [r4, r5]!', {'r4': base, 'r5': 8},
        {'r0': int.from_bytes(bytes(window(base + 8, 4)), 'little'), 'r4': base + 8})
    row('a32', 'str r0, [r4, r5, lsl #2]!', {'r4': base, 'r5': 2, 'r0': 0x01020304},
        {'mem': [(base + 8, le(0x01020304, 4))], 'r4': base + 8})
    row('t32', 'ldr.w r0, [r4, r5, lsl #2]', {'r4': base, 'r5': 3}, {'r0': int.from_bytes(bytes(window(base + 12, 4)), 'little')})
    row('t32', 'ldr r0, [r4, r5]', {'r4': base, 'r5': 8}, {'r0': int.from_bytes(bytes(window(base + 8, 4)), 'little')})
    row('t32', 'ldr r0, [r4, #124]', {'r4': base}, {'r0': int.from_bytes(bytes(window(base + 124, 4)), 'little')})
    # literal loads: the aligned pc
    row('a32', 'ldr r0, [pc, #8]', {'mem': [(PC + 16, le(0xcafef00d, 4))]}, {'r0': 0xcafef00d})
    row('a32', 'ldr r0, [pc, #-8]', {'mem': [(PC, le(0xcafef00d, 4))]}, {'r0': 0xcafef00d})
    row('t32', 'ldr r0, [pc, #8]', {'mem': [(((PC + 4) & ~3) + 8, le(0xcafef00d, 4))]}, {'r0': 0xcafef00d})
    row('t32', 'ldr.w r0, [pc, #-8]', {'mem': [(((PC + 4) & ~3) - 8, le(0xcafef00d, 4))]}, {'r0': 0xcafef00d})
    # a load of the pc interworks
    for value, address, thumb in ((0x4001, 0x4000, 1), (0x4000, 0x4000, 0), (0x4002, 0x4000, 0), (0x4003, 0x4002, 1)):
        row('a32', 'ldr pc, [r4]', {'r4': base, 'mem': [(base, le(value, 4))]}, {'exit': 'branch', 'pc': address, 't': thumb})
        row('t32', 'ldr.w pc, [r4]', {'r4': base, 'mem': [(base, le(value, 4))]},
            {'exit': 'branch', 'pc': address if thumb else address, 't': thumb})
    # doubleword
    data = int.from_bytes(bytes(window(base + 8, 8)), 'little')
    row('a32', 'ldrd r2, r3, [r4, #8]', {'r4': base}, {'r2': data & M32, 'r3': data >> 32})
    row('t32', 'ldrd r2, r3, [r4, #8]', {'r4': base}, {'r2': data & M32, 'r3': data >> 32})
    row('a32', 'ldrd r2, r3, [r4, #8]!', {'r4': base}, {'r2': data & M32, 'r3': data >> 32, 'r4': base + 8})
    row('t32', 'ldrd r2, r3, [r4], #8', {'r4': base}, {'r2': int.from_bytes(bytes(window(base, 4)), 'little'),
                                                      'r3': int.from_bytes(bytes(window(base + 4, 4)), 'little'),
                                                      'r4': base + 8})
    row('a32', 'strd r2, r3, [r4, #8]', {'r4': base, 'r2': 0x11223344, 'r3': 0x55667788},
        {'mem': [(base + 8, le(0x11223344, 4) + le(0x55667788, 4))]})
    row('t32', 'strd r2, r3, [r4, #-8]!', {'r4': base, 'r2': 0x11223344, 'r3': 0x55667788},
        {'mem': [(base - 8, le(0x11223344, 4) + le(0x55667788, 4))], 'r4': base - 8})
    # load and store multiple: the four modes, the writeback, the pc
    regs = [0, 2, 5]
    for arch, names in (('a32', ('ldmia', 'ldmib', 'ldmda', 'ldmdb')), ('t32', ('ldmia.w', 'ldmdb'))):
        for name in names:
            mode = name[3:5] if name[3:5] in ('ia', 'ib', 'da', 'db') else 'ia'
            for wb in (False, True):
                count = len(regs)
                start = {'ia': base, 'ib': base + 4, 'da': base - 4 * count + 4, 'db': base - 4 * count}[mode]
                final = base + 4 * count if mode in ('ia', 'ib') else base - 4 * count
                expect = {}
                for k, r in enumerate(regs):
                    expect['r%d' % r] = int.from_bytes(bytes(window(start + 4 * k, 4)), 'little')
                if wb:
                    expect['r4'] = final
                row(arch, '%s r4%s, {r0, r2, r5}' % (name, '!' if wb else ''), {'r4': base}, expect)
        for name in (('stmia', 'stmib', 'stmda', 'stmdb') if arch == 'a32' else ('stmia.w', 'stmdb')):
            mode = name[3:5] if name[3:5] in ('ia', 'ib', 'da', 'db') else 'ia'
            for wb in (False, True):
                count = len(regs)
                start = {'ia': base, 'ib': base + 4, 'da': base - 4 * count + 4, 'db': base - 4 * count}[mode]
                final = base + 4 * count if mode in ('ia', 'ib') else base - 4 * count
                values = {0: 0x10000001, 2: 0x20000002, 5: 0x50000005}
                mem = []
                for k, r in enumerate(regs):
                    mem.append((start + 4 * k, le(values[r], 4)))
                setup = {'r4': base, 'r0': values[0], 'r2': values[2], 'r5': values[5]}
                expect = {'mem': mem}
                if wb:
                    expect['r4'] = final
                row(arch, '%s r4%s, {r0, r2, r5}' % (name, '!' if wb else ''), setup, expect)
    row('a32', 'push {r0, r1, lr}', {'sp': 0x8000, 'r0': 1, 'r1': 2, 'lr': 3},
        {'sp': 0x8000 - 12, 'mem': [(0x8000 - 12, le(1, 4) + le(2, 4) + le(3, 4))]})
    row('t32', 'push {r0, r1, lr}', {'sp': 0x8000, 'r0': 1, 'r1': 2, 'lr': 3},
        {'sp': 0x8000 - 12, 'mem': [(0x8000 - 12, le(1, 4) + le(2, 4) + le(3, 4))]})
    for arch in ('a32', 't32'):
        row(arch, 'pop {r0, r1}', {'sp': 0x8000, 'mem': [(0x8000, le(5, 4) + le(6, 4))]}, {'sp': 0x8008, 'r0': 5, 'r1': 6})
    for value, address, thumb in ((0x4001, 0x4000, 1), (0x4000, 0x4000, 0), (0x4002, 0x4000, 0)):
        for arch in ('a32', 't32'):
            row(arch, 'pop {r0, pc}', {'sp': 0x8000, 'mem': [(0x8000, le(5, 4) + le(value, 4))]},
                {'sp': 0x8008, 'r0': 5, 'exit': 'branch', 'pc': address, 't': thumb})
    row('a32', 'stmdb sp!, {r0, pc}', {'sp': 0x8000, 'r0': 9}, {'sp': 0x7ff8, 'mem': [(0x7ff8, le(9, 4) + le(PC + 8, 4))]})
    # exclusives
    for arch in ('a32', 't32'):
        for load, store, size in (('ldrex', 'strex', 4), ('ldrexb', 'strexb', 1), ('ldrexh', 'strexh', 2)):
            value = int.from_bytes(bytes(window(base, size)), 'little')
            row(arch, '%s r1, [r4]' % load, {'r4': base}, {'r1': value})
            # a store exclusive without the monitor fails and writes nothing
            row(arch, '%s r0, r2, [r4]' % store, {'r4': base, 'r2': 0x12345678, 'r0': 77}, {'r0': 1})
            # after the load it succeeds, and a store to another address fails
            row(arch, '%s r1, [r4] ; %s r0, r2, [r4]' % (load, store), {'r4': base, 'r2': 0x12345678},
                {'r1': value, 'r0': 0, 'mem': [(base, le(0x12345678, size))]})
            row(arch, '%s r1, [r4] ; %s r0, r2, [r5]' % (load, store), {'r4': base, 'r5': base + 16, 'r2': 0x12345678},
                {'r1': value, 'r0': 1})
            row(arch, '%s r1, [r4] ; %s r0, r2, [r4] ; %s r3, r2, [r4]' % (load, store, store),
                {'r4': base, 'r2': 0x12345678}, {'r1': value, 'r0': 0, 'r3': 1, 'mem': [(base, le(0x12345678, size))]})
        data = bytes(window(base, 8))
        row(arch, 'ldrexd r2, r3, [r4]', {'r4': base}, {'r2': int.from_bytes(data[:4], 'little'),
                                                        'r3': int.from_bytes(data[4:], 'little')})
        row(arch, 'ldrexd r6, r7, [r4] ; strexd r0, r2, r3, [r4]', {'r4': base, 'r2': 0x11111111, 'r3': 0x22222222},
            {'r6': int.from_bytes(data[:4], 'little'), 'r7': int.from_bytes(data[4:], 'little'), 'r0': 0,
             'mem': [(base, le(0x11111111, 4) + le(0x22222222, 4))]})
        row(arch, 'ldrex r1, [r4] ; clrex ; strex r0, r2, [r4]', {'r4': base, 'r2': 7},
            {'r1': int.from_bytes(bytes(window(base, 4)), 'little'), 'r0': 1})
        row(arch, 'lda r0, [r4]', {'r4': base}, {'r0': int.from_bytes(bytes(window(base, 4)), 'little')})
        row(arch, 'stl r0, [r4]', {'r4': base, 'r0': 0xdeadbeef}, {'mem': [(base, le(0xdeadbeef, 4))]})
    # a fault
    row('a32', 'ldr r0, [r4]', {'r4': 0x40000}, {'exit': 'fault'})
    row('t32', 'str r0, [r4]', {'r4': 0x40000}, {'exit': 'fault'})


def main():
    gen_data_processing()
    gen_conditions()
    gen_multiplies()
    gen_dsp()
    gen_branches()
    gen_apsr()
    gen_memory()
    # assemble all the texts of each architecture at once
    out = []
    for arch in ('a32', 't32'):
        items = [p for p in PENDING if p[0] == arch]
        results = armasm.assemble(arch, [p[1] for p in items])
        for (a, text, setup, expect), (hexbytes, error) in zip(items, results):
            if error:
                print('assembler error (%s): %s: %s' % (arch, text, error.splitlines()[0]))
                continue
            out.append((a, hexbytes, text, setup, expect))
    # keep the order of the generation
    order = {id(p): i for i, p in enumerate(PENDING)}
    lines = ['// Generated by tests/arm/tools/gen_semantic_rows_aarch32.py: the integer rows of tests/TestArmAArch32Semantics.cpp.',
             '// { isThumb, "code", "text", "setup", "expected" }',
             '#define AARCH32_SEMANTIC_ROWS_INT \\']
    for a, hexbytes, text, setup, expect in out:
        lines.append('    { %d, "%s", "%s", "%s", "%s" }, \\' % (1 if a == 't32' else 0, hexbytes, text.replace('"', '\\"'),
                                                              tokens(setup), tokens(expect)))
    with open(OUTPUT, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote %s: %d rows' % (OUTPUT, len(out)))


if __name__ == '__main__':
    main()
