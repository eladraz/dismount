#!/usr/bin/env python3
"""Generate tests/arm/Arm64SimdSemanticRowsMemory.inc (ARM64_SIMD_ROWS_MEMORY), the unit rows of the Advanced
SIMD structure loads and stores (ld1-ld4, st1-st4, the single structure forms, ld1r-ld4r, ldap1, stl1; the post
index by an immediate and by a register). The expected values are computed here by an independent model of the
LDn / STn pseudocode of the Arm ARM, on a memory of the pattern (7 * i + 3) mod 256 at BASE.

    gen_simd_semantic_rows_memory.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib
from simd_rows_common import Rows, le, lane, set_lane, mask, M128

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'Arm64SimdSemanticRowsMemory.inc')
BASE = 0x4000
DATA = [(7 * i + 3) & 0xFF for i in range(256)]

ARRANGEMENTS = [('8b', 8, 8), ('16b', 8, 16), ('4h', 16, 4), ('8h', 16, 8), ('2s', 32, 2), ('4s', 32, 4),
                ('1d', 64, 1), ('2d', 64, 2)]
SUFFIX = {8: 'b', 16: 'h', 32: 's', 64: 'd'}
PATTERN = 0x0f1e2d3c4b5a69788796a5b4c3d2e1f0


def vpattern(n):
    """Distinct initial register contents."""
    value = 0
    for i in range(16):
        value |= ((0x11 * (n + 1) + 0x37 * i) & 0xFF) << (8 * i)
    return value


def memory_setup():
    return [(BASE, DATA)]


def read(offset, size):
    return int.from_bytes(bytes(DATA[offset:offset + size]), 'little')


def reglist(first, count, arrangement):
    return ', '.join('v%d.%s' % ((first + i) % 32, arrangement) for i in range(count))


def post(kind, total):
    """(text suffix, setup of the index register, the new base)."""
    if kind == 'none':
        return '', {}, BASE
    if kind == 'imm':
        return ', #%d' % total, {}, BASE + total
    return ', x1', {'x1': 0x40}, BASE + 0x40


rows = Rows()

# ---------------------------------------------------------------------------------------------------
# Multiple structures
# ---------------------------------------------------------------------------------------------------
for name, esize, lanes in ARRANGEMENTS:
    total = esize * lanes // 8
    for mnemonic, regs in (('ld1', 1), ('ld1', 2), ('ld1', 3), ('ld1', 4), ('ld2', 2), ('ld3', 3), ('ld4', 4)):
        if mnemonic != 'ld1' and name == '1d':
            continue
        interleaved = mnemonic != 'ld1'
        for kind in ('none', 'imm', 'reg'):
            if kind != 'none' and not (regs in (1, 4) or lanes == 8):
                continue
            first = 30 if regs > 1 and kind == 'none' else 5
            text_post, extra, newbase = post(kind, total * regs)
            setup = {'x0': BASE}
            setup.update(extra)
            expect = {}
            for r in range(regs):
                value = 0
                for e in range(lanes):
                    index = (e * regs + r) if interleaved else (r * lanes + e)
                    value = set_lane(value, e, esize, read(index * esize // 8, esize // 8))
                expect['v%d' % ((first + r) % 32)] = value
            for r in range(regs):
                setup['v%d' % ((first + r) % 32)] = vpattern(first + r)
            if kind != 'none':
                expect['x0'] = newbase
            setup['mem'] = memory_setup()
            rows.add('%s { %s }, [x0]%s' % (mnemonic, reglist(first, regs, name), text_post), setup, expect)

    for mnemonic, regs in (('st1', 1), ('st1', 2), ('st1', 3), ('st1', 4), ('st2', 2), ('st3', 3), ('st4', 4)):
        if mnemonic != 'st1' and name == '1d':
            continue
        interleaved = mnemonic != 'st1'
        for kind in ('none', 'imm', 'reg'):
            if kind != 'none' and not (regs in (1, 4) or lanes == 8):
                continue
            first = 29 if regs > 1 and kind == 'none' else 6
            text_post, extra, newbase = post(kind, total * regs)
            setup = {'x0': BASE + 0x100}
            setup.update(extra)
            written = {}
            for r in range(regs):
                value = vpattern(first + r + 3)
                setup['v%d' % ((first + r) % 32)] = value
                for e in range(lanes):
                    index = (e * regs + r) if interleaved else (r * lanes + e)
                    for b in range(esize // 8):
                        written[BASE + 0x100 + index * esize // 8 + b] = (lane(value, e, esize) >> (8 * b)) & 0xFF
            expect = {}
            if kind != 'none':
                expect['x0'] = newbase + 0x100
            addresses = sorted(written)
            expect['mem'] = [(a, [written[a]]) for a in addresses]
            setup['mem'] = memory_setup()
            rows.add('%s { %s }, [x0]%s' % (mnemonic, reglist(first, regs, name), text_post), setup, expect)

# ---------------------------------------------------------------------------------------------------
# Single structure (one lane), replicate
# ---------------------------------------------------------------------------------------------------
for esize in (8, 16, 32, 64):
    lanes = 128 // esize
    suffix = SUFFIX[esize]
    for regs in (1, 2, 3, 4):
        for index in sorted(set([0, 1 if lanes > 1 else 0, lanes - 1])):
            for kind in ('none', 'imm', 'reg'):
                if kind != 'none' and index != lanes - 1:
                    continue
                first = 31 if regs > 1 and kind == 'none' else 3
                text_post, extra, newbase = post(kind, regs * esize // 8)
                regtext = ', '.join('v%d.%s' % ((first + i) % 32, suffix) for i in range(regs))
                # load
                setup = {'x0': BASE}
                setup.update(extra)
                expect = {}
                for r in range(regs):
                    n = (first + r) % 32
                    setup['v%d' % n] = vpattern(n)
                    expect['v%d' % n] = set_lane(vpattern(n), index, esize, read(r * esize // 8, esize // 8))
                if kind != 'none':
                    expect['x0'] = newbase
                setup['mem'] = memory_setup()
                rows.add('ld%d { %s }[%d], [x0]%s' % (regs, regtext, index, text_post), setup, expect)
                # store
                setup = {'x0': BASE + 0x100}
                setup.update(extra)
                written = []
                for r in range(regs):
                    n = (first + r) % 32
                    setup['v%d' % n] = vpattern(n + 9)
                    value = lane(vpattern(n + 9), index, esize)
                    for b in range(esize // 8):
                        written.append((BASE + 0x100 + r * esize // 8 + b, (value >> (8 * b)) & 0xFF))
                expect = {'mem': [(a, [v]) for a, v in written]}
                if kind != 'none':
                    expect['x0'] = newbase + 0x100
                setup['mem'] = memory_setup()
                rows.add('st%d { %s }[%d], [x0]%s' % (regs, regtext, index, text_post), setup, expect)

for name, esize, lanes in ARRANGEMENTS:
    suffix = SUFFIX[esize]
    for regs in (1, 2, 3, 4):
        for kind in ('none', 'imm', 'reg'):
            first = 30 if regs > 1 and kind == 'none' else 4
            text_post, extra, newbase = post(kind, regs * esize // 8)
            setup = {'x0': BASE}
            setup.update(extra)
            expect = {}
            for r in range(regs):
                value = 0
                element = read(r * esize // 8, esize // 8)
                for e in range(lanes):
                    value = set_lane(value, e, esize, element)
                expect['v%d' % ((first + r) % 32)] = value
                setup['v%d' % ((first + r) % 32)] = vpattern(first + r)
            if kind != 'none':
                expect['x0'] = newbase
            setup['mem'] = memory_setup()
            rows.add('ld%dr { %s }, [x0]%s' % (regs, reglist(first, regs, name), text_post), setup, expect)

# ldap1 / stl1 (FEAT_LRCPC3): one 64 bit lane
for index in (0, 1):
    setup = {'x0': BASE, 'v2': vpattern(2), 'mem': memory_setup()}
    rows.add('ldap1 { v2.d }[%d], [x0]' % index, setup, {'v2': set_lane(vpattern(2), index, 64, read(0, 8))})
    setup = {'x0': BASE + 0x100, 'v2': vpattern(5), 'mem': memory_setup()}
    value = lane(vpattern(5), index, 64)
    rows.add('stl1 { v2.d }[%d], [x0]' % index, setup, {'mem': [(BASE + 0x100 + b, [(value >> (8 * b)) & 0xFF]) for b in range(8)]})

rows.write(OUTPUT, 'ARM64_SIMD_ROWS_MEMORY', 'gen_simd_semantic_rows_memory.py')
