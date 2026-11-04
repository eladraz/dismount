#!/usr/bin/env python3
"""A small independent model of the A64 SVE registers and of the pseudocode helpers of the Arm ARM,
for the unit test rows of tests/TestArm64SemanticsSve.cpp (see gen_sve_semantic_rows.py). The
models of the instructions are in sve_model_<group>.py: each defines `rows()`, a list of Row
objects. A row gives the instruction text, the vector length, a start state (Machine) and a function
which applies the instruction to a copy of it, written from the Arm ARM pseudocode and not from the
lifter; the expected tokens are the differences between the two states.
"""
import os
import random
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'semantics'))

M64 = (1 << 64) - 1


def mask(bits):
    return (1 << bits) - 1


def sext(value, bits):
    value &= mask(bits)
    return value - (1 << bits) if value >> (bits - 1) else value


class Machine:
    """The state of a row: the SVE registers at the vector length `vl`, x0..x30, sp, nzcv, fpsr,
    and the memory (a dict of bytes, the window [0, 0x20000) of the test)."""

    MEMORY_SIZE = 0x20000

    def __init__(self, vl):
        self.vl = vl
        self.z = [0] * 32
        self.p = [0] * 16
        self.ffr = 0
        self.x = [0] * 31
        self.sp = 0x8000
        self.nzcv = 0
        self.fpsr = 0
        self.fpcr = 0
        self.mem = {}
        self.exit = 'fall'

    def copy(self):
        other = Machine(self.vl)
        other.z = list(self.z)
        other.p = list(self.p)
        other.ffr = self.ffr
        other.x = list(self.x)
        other.sp = self.sp
        other.nzcv = self.nzcv
        other.fpsr = self.fpsr
        other.fpcr = self.fpcr
        other.mem = dict(self.mem)
        other.exit = self.exit
        return other

    # vector helpers
    def lanes(self, esize):
        return self.vl // esize

    def zget(self, n, esize):
        v = self.z[n]
        return [(v >> (i * esize)) & mask(esize) for i in range(self.vl // esize)]

    def zset(self, n, esize, values):
        v = 0
        for i, e in enumerate(values):
            v |= (e & mask(esize)) << (i * esize)
        self.z[n] = v

    def active(self, pred, i, esize):
        """ElemP[pred, i, esize]: the bit of the first byte of the element."""
        return (pred >> (i * esize // 8)) & 1

    def pset_elems(self, n, esize, bits):
        """The predicate whose element i is bits[i] (the other bits of an element are 0)."""
        v = 0
        for i, b in enumerate(bits):
            v |= (b & 1) << (i * esize // 8)
        self.p[n] = v

    # memory helpers (little endian)
    def read(self, address, size):
        if address < 0 or address + size > self.MEMORY_SIZE:
            return None
        return int.from_bytes(bytes(self.mem.get(address + i, 0) for i in range(size)), 'little')

    def write(self, address, size, value):
        if address < 0 or address + size > self.MEMORY_SIZE:
            return False
        for i in range(size):
            self.mem[address + i] = (value >> (8 * i)) & 0xff
        return True


def predicate_test(vl, mask_bits, result, esize):
    """PredTest of the Arm ARM: the NZCV value."""
    elements = vl // esize
    first = none = 1
    last = 0
    seen = False
    for e in range(elements):
        m = (mask_bits >> (e * esize // 8)) & 1
        r = (result >> (e * esize // 8)) & 1
        if m and not seen:
            first = r
            seen = True
        if m and r:
            none = 0
        if m:
            last = r
    n = first if seen else 0
    return (n << 3) | (none << 2) | ((1 - last) << 1)


def decode_pred_count(pattern, elements):
    """DecodePredCount of the Arm ARM."""
    if pattern == 0:
        count = 1
        while count * 2 <= elements:
            count *= 2
        return count if elements else 0
    if 1 <= pattern <= 8:
        count = pattern
    elif pattern == 9:
        count = 16
    elif pattern == 10:
        count = 32
    elif pattern == 11:
        count = 64
    elif pattern == 12:
        count = 128
    elif pattern == 13:
        count = 256
    elif pattern == 29:
        return elements - elements % 4
    elif pattern == 30:
        return elements - elements % 3
    elif pattern == 31:
        return elements
    else:
        return 0
    return count if count <= elements else 0


def f32(value):
    """Round a Python float to binary32."""
    return struct.unpack('<f', struct.pack('<f', value))[0]


def f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def f32_from_bits(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xffffffff))[0]


def f64_bits(value):
    return struct.unpack('<Q', struct.pack('<d', value))[0]


def f64_from_bits(bits):
    return struct.unpack('<d', struct.pack('<Q', bits & M64))[0]


class Row:
    def __init__(self, text, vl, start, model):
        self.text = text
        self.vl = vl
        self.start = start
        self.model = model


def random_machine(vl, seed, memory_pattern=True):
    """A reproducible machine: random z, p and ffr, x registers pointing into the memory window,
    and a memory with a pattern."""
    rng = random.Random(zlib.crc32(('%s:%d' % (seed, vl)).encode()))
    m = Machine(vl)
    m.z = [rng.getrandbits(vl) for _ in range(32)]
    m.p = [rng.getrandbits(vl // 8) for _ in range(16)]
    m.ffr = (1 << (vl // 8)) - 1
    m.x = [rng.getrandbits(64) for _ in range(31)]
    m.nzcv = rng.getrandbits(4)
    if memory_pattern:
        for i in range(0, 0x20000, 1):
            pass
    return m


def memory_pattern(m, base, size, seed=0):
    """Fill [base, base+size) with a position dependent pattern."""
    for i in range(size):
        m.mem[base + i] = (((base + i) * 167 + seed * 31 + 13) >> 1 ^ (base + i)) & 0xff


def tokens_of(machine, vl, reference=None):
    """The tokens of the registers of `machine` which differ from `reference` (all the non zero
    ones if None), as the test parses them."""
    out = []
    digits = vl // 4
    for i in range(32):
        if reference is None and machine.z[i] or reference is not None and machine.z[i] != reference.z[i]:
            out.append('z%d=%0*x' % (i, digits, machine.z[i]))
    pdigits = max(1, vl // 32)
    for i in range(16):
        if reference is None and machine.p[i] or reference is not None and machine.p[i] != reference.p[i]:
            out.append('p%d=%0*x' % (i, pdigits, machine.p[i]))
    if reference is None and machine.ffr or reference is not None and machine.ffr != reference.ffr:
        out.append('ffr=%0*x' % (pdigits, machine.ffr))
    for i in range(31):
        if reference is None and machine.x[i] or reference is not None and machine.x[i] != reference.x[i]:
            out.append('x%d=%x' % (i, machine.x[i]))
    if reference is None:
        if machine.fpcr:
            out.append('fpcr=%x' % machine.fpcr)
        if machine.sp != 0x8000:
            out.append('sp=%x' % machine.sp)
        if machine.nzcv:
            out.append('nzcv=%x' % machine.nzcv)
        if machine.fpsr:
            out.append('fpsr=%x' % machine.fpsr)
    else:
        if machine.sp != reference.sp:
            out.append('sp=%x' % machine.sp)
        if machine.nzcv != reference.nzcv:
            out.append('nzcv=%x' % machine.nzcv)
        if machine.fpsr != reference.fpsr:
            out.append('fpsr=%x' % machine.fpsr)
    return out
