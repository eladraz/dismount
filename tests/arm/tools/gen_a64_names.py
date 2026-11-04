#!/usr/bin/env python3
"""Generate Source/dismount/proc/arm/Arm64Names.cpp: the names of the options
(barriers, prefetch operations, pstate fields, the operations of dc / ic / at
/ tlbi...) and of the system registers, as the LLVM disassembler prints them.

The names are read from objdump: the words of the sparse spaces are
disassembled and the names that come out are the table. Needs clang and
objdump (Apple LLVM 21 or newer).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

REPO = armlib.REPO
OUTPUT = os.path.join(REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64Names.cpp')

NO_REGISTER = 1


def scan(words):
    return list(zip(words, armlib.objdump_words(words)))


def option_tables():
    tables = {}

    def add(scheme, value, name, flags=0):
        tables.setdefault(scheme, {})[value] = (name, flags)

    # The hints, the barriers and the pstate fields: d5000000-d507ffff
    for word, text in scan(list(range(0xd5000000, 0xd5080000, 1))):
        mnemonic = text.split(' ')[0]
        op1 = (word >> 16) & 7
        crm = (word >> 8) & 15
        op2 = (word >> 5) & 7
        rest = text[len(mnemonic):].strip()
        if mnemonic == 'dmb' and not rest.startswith('#') and (word & 0xff) == 0xbf \
                and ((word >> 12) & 15) == 3:
            add('ARM_OPT_BARRIER', crm, rest)
        elif mnemonic == 'dsb' and rest.endswith('nxs') and (word & 0x1f) == 0x1f:
            add('ARM_OPT_BARRIER_NXS', crm >> 2, rest)
        elif mnemonic == 'dsb' and not rest.startswith('#') and not rest.endswith('nxs') \
                and op2 == 4 and ((word >> 12) & 15) == 3:
            add('ARM_OPT_BARRIER', crm, rest)
        elif mnemonic == 'msr' and re.match(r'^[A-Za-z]\w*, #', rest) \
                and ((word >> 12) & 15) == 4 and (word & 0x1f) == 0x1f:
            add('ARM_OPT_PSTATE', (op1 << 3) | op2, rest.split(',')[0])
        elif mnemonic == 'bti' and ((word >> 12) & 15) == 2:
            add('ARM_OPT_BTI', op2 >> 1, rest)
        elif mnemonic == 'stshh' and not rest.startswith('#') and (word & 0x1f) == 0x1f:
            add('ARM_OPT_STSHH', op2, rest)
        elif mnemonic in ('smstart', 'smstop') and rest and (word & 0x1f) == 0x1f:
            add('ARM_OPT_SVCR', (word >> 9) & 3, rest)

    # The sys aliases: op1:CRn:CRm:op2 with Rt = 0, 1 and 31
    families = {'dc': 'ARM_OPT_SYS_DC', 'ic': 'ARM_OPT_SYS_IC', 'at': 'ARM_OPT_SYS_AT',
                'tlbi': 'ARM_OPT_SYS_TLBI', 'plbi': 'ARM_OPT_SYS_PLBI',
                'brb': 'ARM_OPT_SYS_BRB', 'tlbip': 'ARM_OPT_SYS_TLBIP'}
    words = []
    for op in range(1 << 14):
        for rt in (0, 31):
            words.append(0xd5080000 | (op << 5) | rt)
            words.append(0xd5480000 | (op << 5) | rt)
    seen = {}
    for word, text in scan(words):
        mnemonic = text.split(' ')[0]
        if mnemonic not in families:
            continue
        op = (word >> 5) & 0x3fff
        rest = text[len(mnemonic):].strip()
        name = rest.split(',')[0].strip()
        takes_register = ',' in rest
        key = (mnemonic, op)
        entry = seen.setdefault(key, {'name': name, 'regs': set()})
        entry['regs'].add((word & 31, takes_register))
    for (mnemonic, op), entry in seen.items():
        # Rt = 0: if no register is printed the operation takes none
        flags = 0
        for rt, takes in entry['regs']:
            if rt == 0 and not takes:
                flags = NO_REGISTER
        add(families[mnemonic], op, entry['name'], flags)

    add('ARM_OPT_PSTATE', 0x48, 'ALLINT')
    add('ARM_OPT_CSYNC', 0, 'csync')
    add('ARM_OPT_NB', 0, 'nb')
    add('ARM_OPT_SYS_PREDRES', 0, 'rctx')
    # rprfm: the 6 bit operation
    words = []
    for op in range(64):
        words.append(0xf8a04818 | (((op >> 5) & 1) << 15) | (((op >> 4) & 1) << 13) |
                     (((op >> 3) & 1) << 12) | (op & 7))
    for word, text in scan(words):
        m = re.match(r'^rprfm (\w+),', text)
        if m and not m.group(1).startswith('#'):
            op = (((word >> 15) & 1) << 5) | (((word >> 13) & 1) << 4) | \
                 (((word >> 12) & 1) << 3) | (word & 7)
            add('ARM_OPT_RPRFM', op, m.group(1))
    add('ARM_OPT_DSYNC', 0, 'dsync')

    # The prefetch operations: prfm (literal), Rt is the operation
    words = [0xd8000000 | rt for rt in range(32)]
    for word, text in scan(words):
        m = re.match(r'^prfm (\w+),', text)
        if m and not m.group(1).startswith('#'):
            add('ARM_OPT_PREFETCH', word & 31, m.group(1))
    return tables


def system_registers():
    """op0:op1:CRn:CRm:op2 (16 bits) -> (name, access)."""
    rows = {}
    generic = re.compile(r'^S\d_\d_C\d+_C\d+_\d$')
    # op0 is bits 20-19, L (mrs) is bit 21
    mrs = [0xd5200000 | (packed << 5) for packed in range(1 << 16)]
    msr = [0xd5000000 | (packed << 5) for packed in range(1 << 16)]
    for access, words, pattern in [(1, mrs, r'^mrs x0, (\S+)$'),
                                   (2, msr, r'^msr (\S+), x0$')]:
        for word, text in scan(words):
            m = re.match(pattern, text)
            if m and not generic.match(m.group(1)):
                packed = (word >> 5) & 0xffff
                # The read and the write of a register can have two names
                # (DBGDTRRX_EL0 / DBGDTRTX_EL0): one row each
                for row in rows.setdefault(packed, []):
                    if row[0] == m.group(1):
                        row[1] |= access
                        break
                else:
                    rows[packed].append([m.group(1), access])
    return rows


def main():
    options = option_tables()
    sysregs = system_registers()
    schemes = ['ARM_OPT_BARRIER', 'ARM_OPT_BARRIER_NXS', 'ARM_OPT_ISB', 'ARM_OPT_PREFETCH',
               'ARM_OPT_PSTATE', 'ARM_OPT_SYS_DC', 'ARM_OPT_SYS_IC', 'ARM_OPT_SYS_AT',
               'ARM_OPT_SYS_TLBI', 'ARM_OPT_SYS_PLBI', 'ARM_OPT_SYS_BRB',
               'ARM_OPT_SYS_PREDRES', 'ARM_OPT_BTI', 'ARM_OPT_CSYNC', 'ARM_OPT_SVCR',
               'ARM_OPT_STSHH', 'ARM_OPT_DSYNC', 'ARM_OPT_CREG', 'ARM_OPT_SYS_TLBIP',
               'ARM_OPT_RPRFM', 'ARM_OPT_NB']
    out = ['''#include "dismount/dismount.h"
/*
 * Arm64Names.cpp
 *
 * The names of the A64 options and system registers, as LLVM prints them.
 *
 * GENERATED by tests/arm/tools/gen_a64_names.py (from objdump): edit the
 * generator, not this file.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"

namespace armdis {

namespace {
''']
    for scheme in schemes:
        if scheme not in options:
            continue
        rows = sorted(options[scheme].items())
        out.append('const ArmNamedValue %s_NAMES[] = {\n' % scheme)
        for value, (name, flags) in rows:
            out.append('    { 0x%x, %d, "%s" },\n' % (value, flags, name))
        out.append('    { 0, 0, NULL }\n};\n\n')
    out.append('} // anonymous namespace\n\n')
    out.append('const ArmNamedValue* const gArmOptionNames[ARM_OPT_SCHEMES_COUNT] = {\n')
    for scheme in schemes:
        out.append('    /* %s */ %s,\n' % (scheme, scheme + '_NAMES' if scheme in options else 'NULL'))
    out.append('};\n\n')
    out.append('const ArmSystemRegisterName gArmSystemRegisters[] = {\n')
    count = 0
    for packed in sorted(sysregs):
        for name, access in sysregs[packed]:
            out.append('    { 0x%04x, %d, "%s" },\n' % (packed, access, name))
            count += 1
    out.append('    { 0, 0, NULL }\n};\n')
    out.append('const uint gArmSystemRegistersCount = %d;\n\n' % count)
    out.append('} // namespace armdis\n')
    open(OUTPUT, 'w').write(''.join(out))
    print('%d system registers, %s -> %s' % (
        count, ', '.join('%s %d' % (k.replace('ARM_OPT_', ''), len(v))
                                for k, v in sorted(options.items())), OUTPUT))


if __name__ == '__main__':
    main()
