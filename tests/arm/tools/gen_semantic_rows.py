#!/usr/bin/env python3
"""Generate tests/arm/Arm64SemanticRows.inc, the rows of tests/TestArm64Semantics.cpp: an A64
instruction (or a short sequence), a start state, and the state it must leave, as the Arm ARM
defines them. The expected values are computed here by a small independent model of the
pseudocode (AddWithCarry, DecodeBitMasks, the reflected CRC-32...) and not by running dismount,
so the unit test is a check of the lifter against the architecture for the instructions no
processor available to the differential tests (tests/arm/semantics) implements: FEAT_CSSC,
FEAT_LSUI, FEAT_HBC, FEAT_LS64, the writeback forms of FEAT_LRCPC3.

The words are assembled by clang (tests/arm/semantics/armasm.py).

    gen_semantic_rows.py

Row syntax of the generated file, see tests/TestArm64Semantics.cpp:
    { {word, ...}, count, "text", "setup", "expected" }
The setup and expected strings are blank separated tokens: x0..x30, sp, pc (the address of the
next instruction for a branch), nzcv (4 bits), fpcr, fpsr, v0..v31 (128 bits), exit=<kind>,
mem=<addr>:<hex bytes>,... (the initial memory, and the bytes which differ at the end).
Registers which the expectation does not name must be unchanged.
"""
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'semantics'))
import armlib
import armasm

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'Arm64SemanticRows.inc')

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1
PC = 0x1000
BASE = 0x4000          # the data the memory tests use


def mask(bits):
    return (1 << bits) - 1


def sext(value, bits):
    value &= mask(bits)
    return value - (1 << bits) if value >> (bits - 1) else value


def add_with_carry(a, b, carry, bits):
    """(result, nzcv) of AddWithCarry(a, b, carry) in the Arm ARM."""
    a &= mask(bits)
    b &= mask(bits)
    unsigned = a + b + carry
    signed = sext(a, bits) + sext(b, bits) + carry
    result = unsigned & mask(bits)
    n = result >> (bits - 1)
    z = 1 if result == 0 else 0
    c = 0 if result == unsigned else 1
    v = 0 if sext(result, bits) == signed else 1
    return result, (n << 3) | (z << 2) | (c << 1) | v


def sub_flags(a, b, bits):
    return add_with_carry(a, ~b & mask(bits), 1, bits)


def hexs(value):
    return '%x' % value


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


ROWS = []


def row(text, setup, expect):
    ROWS.append((text, setup, expect))


def le(value, size):
    return list((value & mask(size * 8)).to_bytes(size, 'little'))


# --------------------------------------------------------------------------------------------
# Arithmetic and flags
# --------------------------------------------------------------------------------------------
for bits, r in ((64, 'x'), (32, 'w')):
    for a, b in ((mask(bits - 1), 1), (mask(bits), 1), (1 << (bits - 1), 1), (5, 5), (4, 5), (0, 1), (1 << (bits - 1), mask(bits)),
                 (mask(bits), mask(bits)), (0, 0)):
        res, nzcv = add_with_carry(a, b, 0, bits)
        row('adds %s0, %s1, %s2' % (r, r, r), {'x1': a, 'x2': b}, {'x0': res, 'nzcv': nzcv})
        res, nzcv = sub_flags(a, b, bits)
        row('subs %s0, %s1, %s2' % (r, r, r), {'x1': a, 'x2': b}, {'x0': res, 'nzcv': nzcv})
        _, nzcv = sub_flags(a, b, bits)
        row('cmp %s1, %s2' % (r, r), {'x1': a, 'x2': b}, {'nzcv': nzcv})
        _, nzcv = add_with_carry(a, b, 0, bits)
        row('cmn %s1, %s2' % (r, r), {'x1': a, 'x2': b}, {'nzcv': nzcv})
for carry in (0, 1):
    for a, b in ((1, 2), (M64, 0), (0, 0), (mask(63), 0), (M64, M64)):
        nzcv_in = 2 if carry else 0
        res, nzcv = add_with_carry(a, b, carry, 64)
        row('adcs x0, x1, x2', {'x1': a, 'x2': b, 'nzcv': nzcv_in}, {'x0': res, 'nzcv': nzcv})
        res, _ = add_with_carry(a, b, carry, 64)
        row('adc x0, x1, x2', {'x1': a, 'x2': b, 'nzcv': nzcv_in}, {'x0': res})
        res, nzcv = add_with_carry(a, ~b & M64, carry, 64)
        row('sbcs x0, x1, x2', {'x1': a, 'x2': b, 'nzcv': nzcv_in}, {'x0': res, 'nzcv': nzcv})
        res, _ = add_with_carry(a, ~b & M64, carry, 32)
        row('sbc w0, w1, w2', {'x1': a, 'x2': b, 'nzcv': nzcv_in}, {'x0': res})
row('add x0, sp, #16', {'sp': 0x8000}, {'x0': 0x8010})
row('add sp, x1, #1, lsl #12', {'x1': 0x100}, {'sp': 0x1100})
row('sub x0, x1, w2, sxtw #2', {'x1': 100, 'x2': 0xfffffffe}, {'x0': (100 - (-2 << 2)) & M64})
row('add w0, w1, w2, uxtb #1', {'x1': 1, 'x2': 0x1ff}, {'x0': (1 + (0xff << 1)) & M32})
row('neg x0, x1', {'x1': 5}, {'x0': (-5) & M64})
row('negs w0, w1', {'x1': 0}, {'x0': 0, 'nzcv': 6})

# --------------------------------------------------------------------------------------------
# Logic and bitfields
# --------------------------------------------------------------------------------------------
row('ands x0, x1, x2', {'x1': 1 << 63, 'x2': 1 << 63}, {'x0': 1 << 63, 'nzcv': 8})
row('ands x0, x1, x2', {'x1': 1, 'x2': 2, 'nzcv': 0xf}, {'x0': 0, 'nzcv': 4})
row('bics w0, w1, w2', {'x1': 0xf0f0, 'x2': 0xff00}, {'x0': 0x00f0})
row('tst x1, #1', {'x1': 2, 'nzcv': 0xf}, {'nzcv': 4})
row('orr x0, xzr, #0x5555555555555555', {}, {'x0': 0x5555555555555555})
row('eon w0, w1, w2, ror #7', {'x1': 0x12345678, 'x2': 0x0fedcba9},
    {'x0': (0x12345678 ^ ~(((0x0fedcba9 >> 7) | (0x0fedcba9 << 25)) & M32)) & M32})
row('mov x0, sp', {'sp': 0x7ff0}, {'x0': 0x7ff0})
row('mov sp, x1', {'x1': 0x7f00}, {'sp': 0x7f00})
row('lsl x0, x1, #4', {'x1': 0x0123456789abcdef}, {'x0': 0x123456789abcdef0})
row('lsr w0, w1, #8', {'x1': 0xaabbccdd}, {'x0': 0x00aabbcc})
row('asr x0, x1, #60', {'x1': 1 << 63}, {'x0': M64 - 7})
row('ror x0, x1, #8', {'x1': 0x1122334455667788}, {'x0': 0x8811223344556677})
row('ubfx x0, x1, #8, #16', {'x1': 0xaabbccddeeff1122}, {'x0': 0xff11})
row('sbfx x0, x1, #8, #16', {'x1': 0xaabbccddeeff1122}, {'x0': sext(0xff11, 16) & M64})
row('sbfx w0, w1, #4, #4', {'x1': 0x1234a678}, {'x0': sext(0x7, 4) & M32})
row('bfi x0, x1, #8, #8', {'x0': M64, 'x1': 0x12}, {'x0': 0xffffffffffff12ff})
row('bfxil w0, w1, #4, #8', {'x0': M32, 'x1': 0xabcdef12}, {'x0': 0xfffffff1})
row('bfc w0, #4, #8', {'x0': M32}, {'x0': 0xfffff00f})
row('sxtb x0, w1', {'x1': 0x80}, {'x0': M64 - 0x7f})
row('sxth w0, w1', {'x1': 0x8001}, {'x0': 0xffff8001})
row('sxtw x0, w1', {'x1': 0x80000000}, {'x0': 0xffffffff80000000})
row('uxtb w0, w1', {'x1': 0x1ff}, {'x0': 0xff})
row('ubfiz x0, x1, #60, #4', {'x1': 0xff}, {'x0': 0xf000000000000000})
row('sbfiz w0, w1, #4, #4', {'x1': 0x9}, {'x0': (sext(9, 4) << 4) & M32})
row('ubfm x0, x1, #62, #3', {'x1': 0xf}, {'x0': (0xf << 2) & M64})
row('extr x0, x1, x2, #8', {'x1': 0x1122334455667788, 'x2': 0x99aabbccddeeff00}, {'x0': 0x8899aabbccddeeff})
row('extr w0, w1, w2, #31', {'x1': 0x80000000, 'x2': 0x80000001}, {'x0': ((0x80000000 << 1) | (0x80000001 >> 31)) & M32})
row('movz x0, #0x1234, lsl #32', {}, {'x0': 0x123400000000})
row('movn w0, #0', {}, {'x0': M32})
row('movn x0, #0xffff, lsl #48', {}, {'x0': 0x0000ffffffffffff})
row('movk x0, #0xbeef, lsl #16', {'x0': M64}, {'x0': 0xffffffffbeefffff})
row('movk w0, #0, lsl #16', {'x0': M64}, {'x0': 0x0000ffff})
row('adr x0, #0x20', {}, {'x0': PC + 0x20})
row('adrp x0, #0x2000', {'pc': PC + 0x234}, {'x0': ((PC + 0x234) & ~0xfff) + 0x2000})

# --------------------------------------------------------------------------------------------
# Multiply, divide, bit operations
# --------------------------------------------------------------------------------------------
row('udiv x0, x1, x2', {'x1': 7, 'x2': 0}, {'x0': 0})
row('sdiv x0, x1, x2', {'x1': 1 << 63, 'x2': M64}, {'x0': 1 << 63})
row('sdiv w0, w1, w2', {'x1': 0xfffffff9, 'x2': 2}, {'x0': 0xfffffffd})
row('udiv w0, w1, w2', {'x1': 7, 'x2': 2}, {'x0': 3})
row('sdiv w0, w1, w2', {'x1': 0x80000000, 'x2': M32}, {'x0': 0x80000000})
row('madd x0, x1, x2, x3', {'x1': 3, 'x2': 4, 'x3': 5}, {'x0': 17})
row('msub w0, w1, w2, w3', {'x1': 3, 'x2': 4, 'x3': 20}, {'x0': 8})
row('mul x0, x1, x2', {'x1': 1 << 63, 'x2': 2}, {'x0': 0})
row('mneg x0, x1, x2', {'x1': 3, 'x2': 4}, {'x0': (-12) & M64})
row('smulh x0, x1, x2', {'x1': M64, 'x2': 1}, {'x0': M64})
row('smulh x0, x1, x2', {'x1': 1 << 62, 'x2': 8}, {'x0': 2})
row('umulh x0, x1, x2', {'x1': M64, 'x2': M64}, {'x0': M64 - 1})
row('smaddl x0, w1, w2, x3', {'x1': M32, 'x2': 2, 'x3': 10}, {'x0': 8})
row('umaddl x0, w1, w2, x3', {'x1': M32, 'x2': 2, 'x3': 10}, {'x0': (M32 * 2 + 10) & M64})
row('smsubl x0, w1, w2, x3', {'x1': 3, 'x2': 4, 'x3': 20}, {'x0': 8})
row('umull x0, w1, w2', {'x1': M32, 'x2': M32}, {'x0': (M32 * M32) & M64})
row('smnegl x0, w1, w2', {'x1': M32, 'x2': 5}, {'x0': 5})
row('clz x0, x1', {'x1': 1}, {'x0': 63})
row('clz w0, w1', {'x1': 0}, {'x0': 32})
row('cls w0, w1', {'x1': M32}, {'x0': 31})
row('cls x0, x1', {'x1': 0}, {'x0': 63})
row('cls x0, x1', {'x1': 0x00ff000000000000}, {'x0': 7})
row('rbit w0, w1', {'x1': 1}, {'x0': 0x80000000})
row('rbit x0, x1', {'x1': 0x0123456789abcdef}, {'x0': int('{:064b}'.format(0x0123456789abcdef)[::-1], 2)})
row('rev x0, x1', {'x1': 0x0102030405060708}, {'x0': 0x0807060504030201})
row('rev w0, w1', {'x1': 0x01020304}, {'x0': 0x04030201})
row('rev16 w0, w1', {'x1': 0x11223344}, {'x0': 0x22114433})
row('rev16 x0, x1', {'x1': 0x1122334455667788}, {'x0': 0x2211443366558877})
row('rev32 x0, x1', {'x1': 0x0102030405060708}, {'x0': 0x0403020108070605})
row('lsl x0, x1, x2', {'x1': 1, 'x2': 65}, {'x0': 2})
row('asr w0, w1, w2', {'x1': 0x80000000, 'x2': 33}, {'x0': 0xc0000000})
row('ror w0, w1, w2', {'x1': 0x80000001, 'x2': 1}, {'x0': 0xc0000000})
row('lsr x0, x1, x2', {'x1': M64, 'x2': 63}, {'x0': 1})


def crc32(acc, data, size, poly):
    crc = acc
    for i in range(size):
        crc ^= (data >> (8 * i)) & 0xff
        for _ in range(8):
            crc = (crc >> 1) ^ (poly if crc & 1 else 0)
    return crc


for name, size, reg in (('b', 1, 'w'), ('h', 2, 'w'), ('w', 4, 'w'), ('x', 8, 'x')):
    for prefix, poly in (('crc32', 0xEDB88320), ('crc32c', 0x82F63B78)):
        for acc, data in ((0xffffffff, 0x61), (0, 0x12345678abcdef01), (0x12345678, M64)):
            data &= mask(size * 8)
            expected = crc32(acc, data, size, poly)
            if prefix == 'crc32':
                # Cross check with zlib: crc32 continues from the inverted accumulator
                assert expected == (zlib.crc32(data.to_bytes(size, 'little'), acc ^ 0xffffffff) ^ 0xffffffff)
            row('%s%s w0, w1, %s2' % (prefix, name, reg), {'x1': acc, 'x2': data}, {'x0': expected})

# --------------------------------------------------------------------------------------------
# CSSC
# --------------------------------------------------------------------------------------------
row('abs w0, w1', {'x1': 0xffffff80}, {'x0': 128})
row('abs x0, x1', {'x1': 1 << 63}, {'x0': 1 << 63})
row('abs x0, x1', {'x1': (-5) & M64}, {'x0': 5})
row('cnt x0, x1', {'x1': 0xff00ff}, {'x0': 16})
row('cnt w0, w1', {'x1': M64}, {'x0': 32})
row('ctz w0, w1', {'x1': 0x100}, {'x0': 8})
row('ctz x0, x1', {'x1': 0}, {'x0': 64})
row('ctz w0, w1', {'x1': 0}, {'x0': 32})
row('smax w0, w1, w2', {'x1': 0xfffffff0, 'x2': 3}, {'x0': 3})
row('smin w0, w1, w2', {'x1': 0xfffffff0, 'x2': 3}, {'x0': 0xfffffff0})
row('umax x0, x1, x2', {'x1': M64, 'x2': 3}, {'x0': M64})
row('umin x0, x1, x2', {'x1': M64, 'x2': 3}, {'x0': 3})
row('smax w0, w1, #-1', {'x1': 0xfffffff0}, {'x0': M32})
row('smin x0, x1, #-1', {'x1': 5}, {'x0': M64})
row('umax w0, w1, #200', {'x1': 100}, {'x0': 200})
row('umin x0, x1, #5', {'x1': 100}, {'x0': 5})

# --------------------------------------------------------------------------------------------
# Conditions
# --------------------------------------------------------------------------------------------
CONDITIONS = {
    'eq': lambda n, z, c, v: z, 'ne': lambda n, z, c, v: not z,
    'hs': lambda n, z, c, v: c, 'lo': lambda n, z, c, v: not c,
    'mi': lambda n, z, c, v: n, 'pl': lambda n, z, c, v: not n,
    'vs': lambda n, z, c, v: v, 'vc': lambda n, z, c, v: not v,
    'hi': lambda n, z, c, v: c and not z, 'ls': lambda n, z, c, v: (not c) or z,
    'ge': lambda n, z, c, v: n == v, 'lt': lambda n, z, c, v: n != v,
    'gt': lambda n, z, c, v: (not z) and n == v, 'le': lambda n, z, c, v: z or n != v,
}
for cond, test in CONDITIONS.items():
    for nzcv in range(16):
        n, z, c, v = (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1
        holds = bool(test(n, z, c, v))
        row('csel x0, x1, x2, %s' % cond, {'x1': 11, 'x2': 22, 'nzcv': nzcv}, {'x0': 11 if holds else 22})
        row('csinc w0, w1, w2, %s' % cond, {'x1': 11, 'x2': M32, 'nzcv': nzcv}, {'x0': 11 if holds else 0})
        row('csinv x0, x1, x2, %s' % cond, {'x1': 11, 'x2': 22, 'nzcv': nzcv}, {'x0': 11 if holds else (~22 & M64)})
        row('csneg w0, w1, w2, %s' % cond, {'x1': 11, 'x2': 22, 'nzcv': nzcv}, {'x0': 11 if holds else ((-22) & M32)})
        # ccmp: the flags of x1 - x2 when the condition holds, #9 otherwise
        _, compared = sub_flags(5, 5, 64)
        row('ccmp x1, x2, #9, %s' % cond, {'x1': 5, 'x2': 5, 'nzcv': nzcv}, {'nzcv': compared if holds else 9})
        _, compared = add_with_carry(M32, 1, 0, 32)
        row('ccmn w1, #1, #6, %s' % cond, {'x1': M32, 'nzcv': nzcv}, {'nzcv': compared if holds else 6})
        # a branch on the condition
        row('b.%s #8' % cond, {'nzcv': nzcv}, {'exit': 'branch' if holds else 'fall', 'pc': PC + (8 if holds else 4)})
        if cond in ('eq', 'ne', 'hi', 'ls'):
            row('bc.%s #8' % cond, {'nzcv': nzcv}, {'exit': 'branch' if holds else 'fall', 'pc': PC + (8 if holds else 4)})
row('cset w0, ne', {'nzcv': 0}, {'x0': 1})
row('csetm x0, eq', {'nzcv': 4}, {'x0': M64})
row('cinc x0, x1, hi', {'x1': 5, 'nzcv': 2}, {'x0': 6})
row('cneg x0, x1, mi', {'x1': 5, 'nzcv': 8}, {'x0': (-5) & M64})

# --------------------------------------------------------------------------------------------
# Branches
# --------------------------------------------------------------------------------------------
row('b #8', {}, {'exit': 'branch', 'pc': PC + 8})
row('b #-4', {}, {'exit': 'branch', 'pc': PC - 4})
row('bl #16', {}, {'exit': 'branch', 'pc': PC + 16, 'x30': PC + 4})
row('br x1', {'x1': 0x2468}, {'exit': 'branch', 'pc': 0x2468})
row('blr x30', {'x30': 0x2468}, {'exit': 'branch', 'pc': 0x2468, 'x30': PC + 4})
row('ret', {'x30': 0x3000}, {'exit': 'branch', 'pc': 0x3000})
row('ret x4', {'x4': 0x3004}, {'exit': 'branch', 'pc': 0x3004})
row('cbz x0, #12', {'x0': 0}, {'exit': 'branch', 'pc': PC + 12})
row('cbz x0, #12', {'x0': 1}, {'exit': 'fall', 'pc': PC + 4})
row('cbnz w0, #12', {'x0': 1 << 32}, {'exit': 'fall', 'pc': PC + 4})
row('cbnz w0, #12', {'x0': 1}, {'exit': 'branch', 'pc': PC + 12})
row('tbz x0, #63, #12', {'x0': 1 << 63}, {'exit': 'fall', 'pc': PC + 4})
row('tbnz x0, #63, #12', {'x0': 1 << 63}, {'exit': 'branch', 'pc': PC + 12})
row('tbz w0, #3, #-8', {'x0': 0xf7}, {'exit': 'branch', 'pc': PC - 8})

# --------------------------------------------------------------------------------------------
# Loads and stores
# --------------------------------------------------------------------------------------------
data8 = list(range(0x10, 0x30))
mem = [(BASE, data8)]
word = lambda off, size: int.from_bytes(bytes(data8[off:off + size]), 'little')
row('ldr x0, [x1]', {'x1': BASE, 'mem': mem}, {'x0': word(0, 8)})
row('ldr w0, [x1, #8]', {'x1': BASE, 'mem': mem}, {'x0': word(8, 4)})
row('ldrb w0, [x1, #1]', {'x1': BASE, 'mem': mem}, {'x0': word(1, 1)})
row('ldrh w0, [x1, #2]', {'x1': BASE, 'mem': mem}, {'x0': word(2, 2)})
row('ldrsb x0, [x1]', {'x1': BASE, 'mem': [(BASE, [0x80])]}, {'x0': M64 - 0x7f})
row('ldrsb w0, [x1]', {'x1': BASE, 'mem': [(BASE, [0x80])]}, {'x0': 0xffffff80})
row('ldrsh x0, [x1]', {'x1': BASE, 'mem': [(BASE, [0x00, 0x80])]}, {'x0': M64 - 0x7fff})
row('ldrsw x0, [x1]', {'x1': BASE, 'mem': [(BASE, le(0xfffffffe, 4))]}, {'x0': (-2) & M64})
row('ldr x0, [x1, x2, lsl #3]', {'x1': BASE, 'x2': 2, 'mem': mem}, {'x0': word(16, 8)})
row('ldr w0, [x1, w2, sxtw #2]', {'x1': BASE + 8, 'x2': 0xfffffffe, 'mem': mem}, {'x0': word(0, 4)})
row('ldr w0, [x1, w2, uxtw]', {'x1': BASE, 'x2': 4, 'mem': mem}, {'x0': word(4, 4)})
row('ldur x0, [x1, #-3]', {'x1': BASE + 11, 'mem': mem}, {'x0': word(8, 8)})
row('ldr x0, [x1, #8]!', {'x1': BASE, 'mem': mem}, {'x0': word(8, 8), 'x1': BASE + 8})
row('ldr x0, [x1], #8', {'x1': BASE, 'mem': mem}, {'x0': word(0, 8), 'x1': BASE + 8})
row('ldr q0, [x1]', {'x1': BASE, 'mem': mem}, {'v0': word(0, 16)})
row('ldr d1, [x1, #8]', {'x1': BASE, 'mem': mem}, {'v1': word(8, 8)})
row('ldr b2, [x1]', {'x1': BASE, 'mem': mem}, {'v2': word(0, 1)})
row('ldr s3, [x1], #4', {'x1': BASE, 'mem': mem}, {'v3': word(0, 4), 'x1': BASE + 4})
row('str x0, [x1, #8]', {'x0': 0x1122334455667788, 'x1': BASE}, {'mem': [(BASE + 8, le(0x1122334455667788, 8))]})
row('strb w0, [x1]', {'x0': 0x1234, 'x1': BASE}, {'mem': [(BASE, [0x34])]})
row('strh w0, [x1, #2]', {'x0': 0x1234, 'x1': BASE}, {'mem': [(BASE + 2, le(0x1234, 2))]})
row('str w0, [x1, #-4]!', {'x0': 0x11223344, 'x1': BASE + 8}, {'x1': BASE + 4, 'mem': [(BASE + 4, le(0x11223344, 4))]})
row('str q0, [x1]', {'x1': BASE, 'v0': 0x00112233445566778899aabbccddeeff}, {'mem': [(BASE, le(0x00112233445566778899aabbccddeeff, 16))]})
row('ldr x0, #16', {'mem': [(PC + 16, le(0xfeedfacecafebeef, 8))]}, {'x0': 0xfeedfacecafebeef})
row('ldrsw x0, #16', {'mem': [(PC + 16, le(0x80000000, 4))]}, {'x0': 0xffffffff80000000})
row('ldp x0, x1, [x2]', {'x2': BASE, 'mem': mem}, {'x0': word(0, 8), 'x1': word(8, 8)})
row('ldp w0, w1, [x2, #8]', {'x2': BASE, 'mem': mem}, {'x0': word(8, 4), 'x1': word(12, 4)})
row('ldp x0, x1, [x2, #16]!', {'x2': BASE, 'mem': mem}, {'x0': word(16, 8), 'x1': word(24, 8), 'x2': BASE + 16})
row('ldp x0, x1, [x2], #16', {'x2': BASE, 'mem': mem}, {'x0': word(0, 8), 'x1': word(8, 8), 'x2': BASE + 16})
row('ldp x0, x2, [x2]', {'x2': BASE, 'mem': mem}, {'x0': word(0, 8), 'x2': word(8, 8)})
row('ldpsw x0, x1, [x2]', {'x2': BASE, 'mem': [(BASE, le(0xfffffff0, 4) + le(7, 4))]}, {'x0': (-16) & M64, 'x1': 7})
row('ldp q0, q1, [x2]', {'x2': BASE, 'mem': mem}, {'v0': word(0, 16), 'v1': word(16, 16)})
row('ldp d0, d1, [x2, #-8]', {'x2': BASE + 8, 'mem': mem}, {'v0': word(0, 8), 'v1': word(8, 8)})
row('stp x0, x1, [sp, #-16]!', {'x0': 0x11, 'x1': 0x22, 'sp': BASE + 16}, {'sp': BASE, 'mem': [(BASE, le(0x11, 8) + le(0x22, 8))]})
row('stp w0, w1, [x2], #8', {'x0': 1, 'x1': 2, 'x2': BASE}, {'x2': BASE + 8, 'mem': [(BASE, le(1, 4) + le(2, 4))]})
row('stp x1, x2, [x2]', {'x1': 0xaa, 'x2': BASE}, {'mem': [(BASE, le(0xaa, 8) + le(BASE, 8))]})
row('stnp x0, x1, [x2]', {'x0': 5, 'x1': 6, 'x2': BASE}, {'mem': [(BASE, le(5, 8) + le(6, 8))]})
row('ldr x0, [x1, #8]', {'x1': 0x100000, 'mem': mem}, {'exit': 'fault'})
row('str x0, [x1]', {'x1': 0x1fffc, 'x0': 1}, {'exit': 'fault'})
row('prfm pldl1keep, [x1]', {'x1': 0x100000}, {})

# --------------------------------------------------------------------------------------------
# Exclusives and acquire / release
# --------------------------------------------------------------------------------------------
row('ldxr x0, [x1] ; stxr w2, x3, [x1]', {'x1': BASE, 'x3': 0x77, 'mem': mem},
    {'x0': word(0, 8), 'x2': 0, 'mem': [(BASE, le(0x77, 8))]})
row('ldxr x0, [x1] ; stxr w2, x3, [x4]', {'x1': BASE, 'x3': 0x77, 'x4': BASE + 16, 'mem': mem},
    {'x0': word(0, 8), 'x2': 1})
row('stxr w2, x3, [x1]', {'x1': BASE, 'x3': 0x77, 'mem': mem}, {'x2': 1})
row('ldaxr w0, [x1] ; stlxr w2, w3, [x1]', {'x1': BASE, 'x3': 0x1234567890, 'mem': mem},
    {'x0': word(0, 4), 'x2': 0, 'mem': [(BASE, le(0x34567890, 4))]})
row('ldxrb w0, [x1] ; stxrb w2, w3, [x1]', {'x1': BASE, 'x3': 0x1ff, 'mem': mem},
    {'x0': word(0, 1), 'x2': 0, 'mem': [(BASE, [0xff])]})
row('ldxr x0, [x1] ; clrex ; stxr w2, x3, [x1]', {'x1': BASE, 'x3': 1, 'mem': mem}, {'x0': word(0, 8), 'x2': 1})
row('ldxr x0, [x1] ; stxr w2, x3, [x1] ; stxr w4, x3, [x1]', {'x1': BASE, 'x3': 1, 'mem': mem},
    {'x0': word(0, 8), 'x2': 0, 'x4': 1, 'mem': [(BASE, le(1, 8))]})
row('ldxp x0, x1, [x2] ; stxp w3, x4, x5, [x2]', {'x2': BASE, 'x4': 0xa, 'x5': 0xb, 'mem': mem},
    {'x0': word(0, 8), 'x1': word(8, 8), 'x3': 0, 'mem': [(BASE, le(0xa, 8) + le(0xb, 8))]})
row('ldar x0, [x1]', {'x1': BASE, 'mem': mem}, {'x0': word(0, 8)})
row('ldarb w0, [x1]', {'x1': BASE, 'mem': mem}, {'x0': word(0, 1)})
row('stlr w0, [x1]', {'x0': 0x11223344, 'x1': BASE}, {'mem': [(BASE, le(0x11223344, 4))]})
row('ldapr x0, [x1]', {'x1': BASE, 'mem': mem}, {'x0': word(0, 8)})
row('ldapur w0, [x1, #-4]', {'x1': BASE + 8, 'mem': mem}, {'x0': word(4, 4)})
row('ldapursb x0, [x1, #1]', {'x1': BASE, 'mem': [(BASE + 1, [0xf0])]}, {'x0': (-16) & M64})
row('stlur w0, [x1, #4]', {'x0': 9, 'x1': BASE}, {'mem': [(BASE + 4, le(9, 4))]})
row('ldapr w0, [x1], #4', {'x1': BASE, 'mem': mem}, {'x0': word(0, 4), 'x1': BASE + 4})
row('stlr x0, [x1, #-8]!', {'x0': 9, 'x1': BASE + 8}, {'x1': BASE, 'mem': [(BASE, le(9, 8))]})

# --------------------------------------------------------------------------------------------
# LSE atomics (and the unprivileged FEAT_LSUI forms, which are the same operations)
# --------------------------------------------------------------------------------------------
ops = {
    'add': lambda old, v, bits: (old + v) & mask(bits),
    'clr': lambda old, v, bits: old & ~v & mask(bits),
    'eor': lambda old, v, bits: old ^ v,
    'set': lambda old, v, bits: old | v,
    'smax': lambda old, v, bits: max(sext(old, bits), sext(v, bits)) & mask(bits),
    'smin': lambda old, v, bits: min(sext(old, bits), sext(v, bits)) & mask(bits),
    'umax': lambda old, v, bits: max(old, v),
    'umin': lambda old, v, bits: min(old, v),
}
for op, fn in ops.items():
    for suffix, bits, reg in (('', 32, 'w'), ('', 64, 'x'), ('b', 8, 'w'), ('h', 16, 'w')):
        for old, value in ((5, M64), (0x80, 1), (0x12345678abcdef01, 0xff)):
            old &= mask(bits)
            value &= mask(bits)
            new = fn(old, value, bits)
            size = bits // 8
            text = 'ld%s%s %s1, %s0, [x2]' % (op, suffix, reg, reg)
            row(text, {'x1': value, 'x2': BASE, 'mem': [(BASE, le(old, size))]},
                {'x0': old, 'mem': [(BASE, le(new, size))]})
            row('ld%sal%s %s1, %s0, [x2]' % (op, suffix, reg, reg), {'x1': value, 'x2': BASE, 'mem': [(BASE, le(old, size))]},
                {'x0': old, 'mem': [(BASE, le(new, size))]})
            row('st%s%s %s1, [x2]' % (op, suffix, reg), {'x1': value, 'x2': BASE, 'mem': [(BASE, le(old, size))]},
                {'mem': [(BASE, le(new, size))]})
            if suffix == '' and op in ('add', 'clr', 'set'):
                row('ldt%s %s1, %s0, [x2]' % (op, reg, reg), {'x1': value, 'x2': BASE, 'mem': [(BASE, le(old, size))]},
                    {'x0': old, 'mem': [(BASE, le(new, size))]})
row('ldadd w1, wzr, [x2]', {'x1': 1, 'x2': BASE, 'mem': [(BASE, le(7, 4))]}, {'mem': [(BASE, le(8, 4))]})
row('swp w1, w0, [x2]', {'x1': 0x11223344, 'x2': BASE, 'mem': mem}, {'x0': word(0, 4), 'mem': [(BASE, le(0x11223344, 4))]})
row('swpalb w1, w0, [x2]', {'x1': 0x1ff, 'x2': BASE, 'mem': mem}, {'x0': word(0, 1), 'mem': [(BASE, [0xff])]})
row('swpt w1, w0, [x2]', {'x1': 0x11223344, 'x2': BASE, 'mem': mem}, {'x0': word(0, 4), 'mem': [(BASE, le(0x11223344, 4))]})
row('cas x0, x1, [x2]', {'x0': word(0, 8), 'x1': 0x99, 'x2': BASE, 'mem': mem}, {'mem': [(BASE, le(0x99, 8))]})
row('cas x0, x1, [x2]', {'x0': 5, 'x1': 0x99, 'x2': BASE, 'mem': mem}, {'x0': word(0, 8)})
row('casb w0, w1, [x2]', {'x0': 0x1000 + word(0, 1), 'x1': 0x99, 'x2': BASE, 'mem': mem}, {'x0': word(0, 1), 'mem': [(BASE, [0x99])]})
row('casalh w0, w1, [x2]', {'x0': word(0, 2), 'x1': 0x1234, 'x2': BASE, 'mem': mem}, {'mem': [(BASE, le(0x1234, 2))]})
row('cast x0, x1, [x2]', {'x0': word(0, 8), 'x1': 0x99, 'x2': BASE, 'mem': mem}, {'mem': [(BASE, le(0x99, 8))]})
row('casp x0, x1, x2, x3, [x4]', {'x0': word(0, 8), 'x1': word(8, 8), 'x2': 0xa, 'x3': 0xb, 'x4': BASE, 'mem': mem},
    {'mem': [(BASE, le(0xa, 8) + le(0xb, 8))]})
row('casp x0, x1, x2, x3, [x4]', {'x0': 1, 'x1': 2, 'x2': 0xa, 'x3': 0xb, 'x4': BASE, 'mem': mem},
    {'x0': word(0, 8), 'x1': word(8, 8)})
row('casp w0, w1, w2, w3, [x4]', {'x0': word(0, 4), 'x1': word(4, 4), 'x2': 0xa, 'x3': 0xb, 'x4': BASE, 'mem': mem},
    {'mem': [(BASE, le(0xa, 4) + le(0xb, 4))]})
row('caspal w0, w1, w2, w3, [x4]', {'x0': 1, 'x1': 2, 'x2': 0xa, 'x3': 0xb, 'x4': BASE, 'mem': mem},
    {'x0': word(0, 4), 'x1': word(4, 4)})

# --------------------------------------------------------------------------------------------
# FEAT_LSE128 (the 128 bit atomics) and FEAT_LRCPC3 (the pairs)
# --------------------------------------------------------------------------------------------
pair_mem = list(range(0x90, 0xa0))
old128 = int.from_bytes(bytes(pair_mem), 'little')
val128 = 0x00ff00ff00ff00ff0f0f0f0f0f0f0f0f
for suffix in ('', 'a', 'l', 'al'):
    for name, fn in (('ldclrp' + suffix, lambda o, v: o & ~v), ('ldsetp' + suffix, lambda o, v: o | v),
                     ('swpp' + suffix, lambda o, v: v)):
        new128 = fn(old128, val128) & mask(128)
        row('%s x0, x1, [x2]' % name, {'x0': val128 & M64, 'x1': val128 >> 64, 'x2': BASE, 'mem': [(BASE, pair_mem)]},
            {'x0': old128 & M64, 'x1': old128 >> 64, 'mem': [(BASE, le(new128, 16))]})
row('ldiapp x0, x1, [x2]', {'x2': BASE, 'mem': mem}, {'x0': word(0, 8), 'x1': word(8, 8)})
row('ldiapp w0, w1, [x2], #8', {'x2': BASE, 'mem': mem}, {'x0': word(0, 4), 'x1': word(4, 4), 'x2': BASE + 8})
row('stilp x0, x1, [x2, #-16]!', {'x0': 0x11, 'x1': 0x22, 'x2': BASE + 16}, {'x2': BASE, 'mem': [(BASE, le(0x11, 8) + le(0x22, 8))]})
row('stilp w0, w1, [x2]', {'x0': 0x11, 'x1': 0x22, 'x2': BASE}, {'mem': [(BASE, le(0x11, 4) + le(0x22, 4))]})

# --------------------------------------------------------------------------------------------
# FEAT_CMPBR (the compare and branch)
# --------------------------------------------------------------------------------------------
cmp_values = [(0, 0), (1, 2), (2, 1), (M64, 1), (1, M64), (1 << 63, (1 << 63) - 1), (0x80, 0x7f), (0x8000, 0x7fff), (5, 5),
              (0x1ff, 0xff), (0x10080, 0x7f)]
cmp_tests = {
    'gt': lambda a, b, bits: sext(a, bits) > sext(b, bits), 'ge': lambda a, b, bits: sext(a, bits) >= sext(b, bits),
    'lt': lambda a, b, bits: sext(a, bits) < sext(b, bits), 'hi': lambda a, b, bits: (a & mask(bits)) > (b & mask(bits)),
    'hs': lambda a, b, bits: (a & mask(bits)) >= (b & mask(bits)), 'lo': lambda a, b, bits: (a & mask(bits)) < (b & mask(bits)),
    'eq': lambda a, b, bits: (a & mask(bits)) == (b & mask(bits)), 'ne': lambda a, b, bits: (a & mask(bits)) != (b & mask(bits)),
}
for cc in ('gt', 'ge', 'hi', 'hs', 'eq', 'ne'):
    for a, b in cmp_values:
        for prefix, bits, reg in (('cb', 64, 'x'), ('cb', 32, 'w'), ('cbb', 8, 'w'), ('cbh', 16, 'w')):
            taken = cmp_tests[cc](a, b, bits)
            row('%s%s %s0, %s1, #8' % (prefix, cc, reg, reg), {'x0': a, 'x1': b},
                {'exit': 'branch' if taken else 'fall', 'pc': PC + (8 if taken else 4)})
for cc in ('gt', 'lt', 'hi', 'lo', 'eq', 'ne'):
    for a in (0, 1, 5, 6, 63, 64, M64, 1 << 63):
        for imm in (0, 5, 63):
            taken = cmp_tests[cc](a, imm, 64)
            row('cb%s x0, #%d, #-8' % (cc, imm), {'x0': a},
                {'exit': 'branch' if taken else 'fall', 'pc': PC + (-8 if taken else 4)})
            taken = cmp_tests[cc](a, imm, 32)
            row('cb%s w0, #%d, #12' % (cc, imm), {'x0': a},
                {'exit': 'branch' if taken else 'fall', 'pc': PC + (12 if taken else 4)})

# --------------------------------------------------------------------------------------------
# LS64
# --------------------------------------------------------------------------------------------
big = list(range(0x40, 0x80))
row('ld64b x0, [x8]', {'x8': BASE, 'mem': [(BASE, big)]},
    dict(('x%d' % i, int.from_bytes(bytes(big[8 * i:8 * i + 8]), 'little')) for i in range(8)))
row('st64b x0, [x8]', dict([('x8', BASE)] + [('x%d' % i, 0x0101010101010101 * (i + 1)) for i in range(8)]),
    {'mem': [(BASE, sum([le(0x0101010101010101 * (i + 1), 8) for i in range(8)], []))]})

# --------------------------------------------------------------------------------------------
# System
# --------------------------------------------------------------------------------------------
row('mrs x0, nzcv', {'nzcv': 0xa}, {'x0': 0xa0000000})
row('msr nzcv, x0', {'x0': 0x50000000, 'nzcv': 0xa}, {'nzcv': 5})
row('mrs x0, fpcr', {'fpcr': 0x00c00000}, {'x0': 0x00c00000})
row('msr fpcr, x0', {'x0': 0x00400000}, {'fpcr': 0x00400000})
row('msr fpsr, x0', {'x0': 0x10}, {'fpsr': 0x10})
row('cfinv', {'nzcv': 0}, {'nzcv': 2})
row('cfinv', {'nzcv': 2}, {'nzcv': 0})
for nzcv in range(16):
    n, z, c, v = (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1
    row('axflag', {'nzcv': nzcv}, {'nzcv': (0 << 3) | ((z | v) << 2) | ((c & (1 - v)) << 1)})
    row('xaflag', {'nzcv': nzcv}, {'nzcv': ((1 - c) & (1 - z)) << 3 | ((z & c) << 2) | ((c | z) << 1) | ((1 - c) & z)})
row('rmif x1, #4, #15', {'x1': 0xa0}, {'nzcv': 0xa})
row('rmif x1, #0, #5', {'x1': 0xf, 'nzcv': 0xa}, {'nzcv': 0xf & 0xa | (0x5 & 0xf)})
row('setf8 w1', {'x1': 0x1ff, 'nzcv': 2}, {'nzcv': 0xa})
row('setf8 w1', {'x1': 0x100, 'nzcv': 0}, {'nzcv': 5})
row('setf16 w1', {'x1': 0x10000, 'nzcv': 2}, {'nzcv': 7})
row('nop', {}, {})
row('cfp rctx, x1', {'x1': 5}, {})
row('dvp rctx, x1', {'x1': 5}, {})
row('cpp rctx, x1', {'x1': 5}, {})
row('cosp rctx, x1', {'x1': 5}, {})
row('wfet x1', {}, {})
row('wfit x1', {}, {})
row('stshh keep', {}, {})
row('psb csync', {}, {})
row('tsb csync', {}, {})
row('esb', {}, {})
row('yield', {}, {})
row('dmb ish', {}, {})
row('dsb sy', {}, {})
row('isb', {}, {})
row('clrex', {}, {})
row('bti c', {}, {})
row('svc #5', {}, {'exit': 'trap:svc', 'trapnumber': 5})
row('brk #0x1234', {}, {'exit': 'trap:brk', 'trapnumber': 0x1234})
row('udf #7', {}, {'exit': 'trap:undef', 'trapnumber': 7})
row('hlt #1', {}, {'exit': 'trap:undef'})
row('dc zva, x1', {'x1': BASE + 0x25, 'mem': [(BASE, [0xaa] * 128)]}, {'mem': [(BASE, [0] * 64)]})
row('dc civac, x1', {'x1': BASE, 'mem': mem}, {})
row('dc civac, x1', {'x1': 0x100000}, {'exit': 'fault'})
row('ic ivau, x1', {'x1': BASE, 'mem': mem}, {})
for value in (0x00ff80ff12345678, 0x0080ffff12345678, 0xff80ffff12345678, 0x0000ffff12345678, 0x56ffc14c7b35b3a2):
    ext = M64 & ~mask(47)
    stripped = (value | ext) if (value >> 55) & 1 else (value & mask(47))
    stripped_data = (value | ext) if (value >> 55) & 1 else ((value & mask(47)) | (value & 0xff00000000000000))
    row('xpaci x0', {'x0': value}, {'x0': stripped})
    row('xpacd x0', {'x0': value}, {'x0': stripped_data})
    row('xpaclri', {'x30': value}, {'x30': stripped})
row('paciasp', {}, {'exit': 'helper'})
row('cpyfp [x0]!, [x1]!, x2!', {}, {'exit': 'undefined'})
row('mrs x0, id_aa64isar0_el1', {}, {'exit': 'helper'})


def main():
    texts = [r[0] for r in ROWS]
    pieces = []
    for text in texts:
        pieces += text.split(' ; ')
    unique = sorted(set(pieces))
    assembled = dict(zip(unique, armasm.assemble('a64', unique)))
    # The instructions clang does not know (FEAT_CMPBR) are assembled by the assembler of dismount
    # (checked against LLVM by tests/arm/run_compat.py)
    for text in unique:
        if assembled[text][1] is not None:
            result = armlib.run([armlib.CLI, 'asm', '--arch', 'a64'], text + '\n').stdout.strip()
            if not result.startswith('OK '):
                raise SystemExit('%s: %s / %s' % (text, assembled[text][1], result))
            assembled[text] = (result[3:], None)
    out = ['// Generated by tests/arm/tools/gen_semantic_rows.py: the rows of tests/TestArm64Semantics.cpp.',
           '// { {words}, count, "text", "setup", "expected" }',
           '#define ARM64_SEMANTIC_ROWS \\']
    for text, setup, expect in ROWS:
        words = []
        for piece in text.split(' ; '):
            hexb, err = assembled[piece]
            if err:
                raise SystemExit('%s: %s' % (piece, err))
            words.append(int.from_bytes(bytes.fromhex(hexb), 'little'))
        cwords = ', '.join('0x%08x' % w for w in words)
        out.append('    { {%s}, %d, "%s", "%s", "%s" }, \\' % (cwords, len(words), text, tokens(setup), tokens(expect)))
    out.append('')
    open(OUTPUT, 'w').write('\n'.join(out) + '\n')
    print('wrote %s: %d rows' % (OUTPUT, len(ROWS)))


if __name__ == '__main__':
    main()
