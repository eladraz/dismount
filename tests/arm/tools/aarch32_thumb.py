#!/usr/bin/env python3
"""The Thumb tables (T16 and T32 integer, branch, load / store and system
instructions), generated from the LLVM records like the A32 one (see
gen_aarch32_tables.py): ArmEncodingTableT16.cpp and ArmEncodingTableT32.cpp.

The oracle (LLVM 21) differs from the records in what its C++ decoder does
(the loads whose base is pc, the hints, the it block...): the differences are
the fixups of this file.
"""
import os

import aarch32_classes as classes
import aarch32_classes_thumb
import gen_aarch32_tables as gen
from aarch32_classes import S

KEY = 'thumb'

THUMB_NAMESPACES = ('Thumb', 'ThumbSBit', 'Thumb2', 'Thumb2CoProc')


def thumb_handler(ctx, op, piece, records):
    # (only for the instructions of the Thumb decoder namespaces: the other
    # areas have classes with the same names)
    if ctx.ins.namespace not in THUMB_NAMESPACES:
        return None
    return aarch32_classes_thumb.handle(ctx, op, piece, records)


classes.EXTRA_HANDLERS.append(thumb_handler)

HELPERS = {}


def helper(name, body):
    HELPERS[name] = body
    return name


def by_name(entries, name):
    for entry in entries:
        if entry.comment == name:
            return entry
    raise KeyError(name)


def remove(entries, *names):
    names = set(names)
    return [e for e in entries if e.comment not in names]


def new(entries, comment, mnemonic, mask, value, specs, feature, flags=None, condition=None, flow='0'):
    entry = gen.Entry(mnemonic, mask, value, specs, feature, flags or [], flow, condition, comment)
    entries.append(entry)
    return entry


COND_IT = S('ARM_KIND_CONDITION_MNEMONIC', 0, 'ARM_A32_COND_IT')


def gpr(position, width=4, flags='ARM_A32_GPR_ANY'):
    return S('ARM_KIND_GPR', position, flags, width)


T16_SKIP = set([
    # printed as the entries of the fixups
])

T32_SKIP = set([
    # ADD / SUB sp are the same as the general ones for the text
])


def fixups16(entries):
    # ---- it: a zero mask is not it (and it can't be in an it block)
    it = by_name(entries, 't2IT')
    it.condition = helper('itMaskNotZero', 'return BITS(3, 0) != 0;')
    it.flags = list(it.flags) + ['ARM_ENTRY_T32_NOT_IN_IT']
    # ---- b<c>: the conditions 1110 and 1111 are udf and svc
    bcc = by_name(entries, 'tBcc')
    bcc.condition = helper('thumbConditionValid', 'return BITS(11, 8) < 14;')
    # ---- the hints
    hint = by_name(entries, 'tHINT')
    for number, name, feature in ((0, 'nop', 'FEAT_V6M'), (1, 'yield', 'FEAT_V6M'), (2, 'wfe', 'FEAT_V6M'),
                                  (3, 'wfi', 'FEAT_V6M'), (4, 'sev', 'FEAT_V6M'), (5, 'sevl', 'FEAT_V8_AA32')):
        new(entries, 'tHINT', name, 0xffff, 0xbf00 | (number << 4), [COND_IT], feature,
            flags=['ARM_ENTRY_ALIAS'])
    # ---- nop before ARMv6-M is mov r8, r8 (the assembler writes it, the
    # disassembler prints the mov)
    new(entries, 'NOP', 'nop', 0xffff, 0x46c0, [COND_IT], 'FEAT_T32_BASE',
        flags=['ARM_ENTRY_ASSEMBLE_ONLY'])
    return entries


def restrict(entry, name, body):
    """Add a condition (a C++ expression of the encoding 'e')."""
    if entry.condition:
        body = '%s(e) && (%s)' % (entry.condition, body)
        name = entry.condition + '_' + name
    entry.condition = helper(name, 'return %s;' % body)


LOADS = ('LDR', 'LDRB', 'LDRH', 'LDRSB', 'LDRSH')
STORES = ('STR', 'STRB', 'STRH')


def fixups32(entries):
    # ---- b<c>.w: the conditions 1110 and 1111 are not conditions
    bcc = by_name(entries, 't2Bcc')
    bcc.condition = helper('thumbWideConditionValid', 'return BITS(25, 22) < 14;')

    # ---- The loads and stores with a base register: pc as the base makes them
    # the literal loads (the t2*pci records), a pc store is invalid, and the
    # loads of the halfword with Rt = pc are pld / pli (records of their own)
    forms = ('i12', 'i8', 's', 'T', '_PRE', '_POST')
    for entry in entries:
        name = entry.comment
        for family in LOADS:
            for form in forms:
                if name == 't2%s%s' % (family, form):
                    restrict(entry, 'baseNotPc', 'BITS(19, 16) != 15')
                    if family == 'LDRSH' and form in ('i12', 'i8', 's'):
                        restrict(entry, 'targetNotPc', 'BITS(15, 12) != 15')
                    if family in ('LDRH', 'LDRSB') and form in ('i12', 's', 'i8'):
                        if family == 'LDRH' and form == 'i8':
                            restrict(entry, 'targetNotPcOrAdd', 'BITS(15, 12) != 15 || BITS(9, 9) != 0')
                        else:
                            restrict(entry, 'targetNotPc', 'BITS(15, 12) != 15')
        for family in STORES:
            for form in ('i12', 'i8', 's', 'T', '_PRE', '_POST'):
                if name == 't2%s%s' % (family, form):
                    restrict(entry, 'baseNotPc', 'BITS(19, 16) != 15')
        for prefix in ('t2PLD', 't2PLI', 't2PLDW'):
            for form in ('i12', 'i8', 's'):
                if name == prefix + form:
                    restrict(entry, 'baseNotPc', 'BITS(19, 16) != 15')
        if name in ('t2LDRBpci', 't2LDRHpci', 't2LDRSBpci', 't2LDRSHpci'):
            restrict(entry, 'targetNotPc', 'BITS(15, 12) != 15')

    # ---- ldrh pc, [pc, #imm] is pld
    pld = by_name(entries, 't2PLDpci')
    new(entries, 't2PLDpci', pld.mnemonic, pld.mask, pld.value | 0x00200000, pld.specs, pld.feature,
        condition=helper('literalPldFromLdrh', 'return BITS(23, 23) == 0 && BITS(11, 6) != 0 && BITS(11, 8) != 12;'))

    # ---- cps: the C++ decoder decides the form by imod and M
    entries[:] = remove(entries, 't2CPS1p', 't2CPS2p', 't2CPS3p')
    imod = S('ARM_KIND_A32_MNEMONIC_PART', 0, '0', 'ARM_A32_PART_CPS_IMOD_T32')
    iflags = S('ARM_KIND_A32_OPTION', 5, '0', 'ARM_A32_OPT_IFLAGS')
    mode = S('ARM_KIND_IMMEDIATE', 0, 'ARM_SPEC_HEX', 5)
    new(entries, 't2CPS3p', 'cps', 0xfffff900 | 0x400, 0xf3af8100 | 0x400, [imod, iflags, mode],
        'FEAT_V6T2')
    new(entries, 't2CPS2p', 'cps.w', 0xfffff91f | 0x400, 0xf3af8000 | 0x400, [imod, iflags],
        'FEAT_V6T2')
    new(entries, 't2CPS1p', 'cps', 0xfffff900 | 0x600, 0xf3af8100, [mode], 'FEAT_V6T2')

    # ---- The hints
    for number, name, feature in ((0, 'nop.w', 'FEAT_V6T2'), (1, 'yield.w', 'FEAT_V6T2'),
                                  (2, 'wfe.w', 'FEAT_V6T2'), (3, 'wfi.w', 'FEAT_V6T2'),
                                  (4, 'sev.w', 'FEAT_V6T2'), (5, 'sevl.w', 'FEAT_V8_AA32'),
                                  (16, 'esb.w', 'FEAT_RAS_AA32'), (20, 'csdb', 'FEAT_V6T2'),
                                  (15, 'bti', 'FEAT_V6T2')):
        new(entries, 't2HINT', name, 0xffffffff, 0xf3af8000 | number,
            [] if name == 'bti' else [COND_IT], feature, flags=['ARM_ENTRY_ALIAS'])
    for number, name in ((13, 'pacbti'), (29, 'pac'), (45, 'aut')):
        new(entries, 't2HINT', name, 0xffffffff, 0xf3af8000 | number,
            [S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_FIXED_R12'), S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_FIXED_LR'),
             S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_FIXED_SP')], 'FEAT_V6T2',
            flags=['ARM_ENTRY_ALIAS'])
    new(entries, 't2DSB', 'ssbb', 0xffffffff, 0xf3bf8f40, [], 'FEAT_V7', flags=['ARM_ENTRY_ALIAS'])
    new(entries, 't2DSB', 'pssbb', 0xffffffff, 0xf3bf8f44, [], 'FEAT_V7', flags=['ARM_ENTRY_ALIAS'])
    new(entries, 't2SUBS_PC_LR', 'eret', 0xffffffff, 0xf3de8f00, [COND_IT], 'FEAT_VIRT',
        flags=['ARM_ENTRY_ALIAS'])

    # ---- push and pop of several registers
    helper('manyRegisters16', 'uint32 list = BITS(15, 0);\n    return (list & (list - 1)) != 0;')
    for record, alias in (('t2STMDB_UPD', 'push.w'), ('t2LDMIA_UPD', 'pop.w')):
        base = by_name(entries, record)
        spec_list = [s_ for s_ in base.specs if s_[0] == 'ARM_KIND_GPR_LIST'][0]
        new(entries, record, alias, base.mask | 0x000f0000, base.value | 0x000d0000,
            [COND_IT, spec_list], 'FEAT_V6T2', flags=['ARM_ENTRY_ALIAS'], condition='manyRegisters16')

    # ---- adr.w: the sign bits are equal, a subtraction of zero is subw
    adr = by_name(entries, 't2ADR') if any(e.comment == 't2ADR' for e in entries) else None
    entries[:] = remove(entries, 't2ADR')
    rd = gpr(8)
    new(entries, 't2ADR', 'adr.w', 0xfb5f8000, 0xf20f0000,
        [COND_IT, rd, S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_T32_ADR')], 'FEAT_V6T2',
        condition=helper('adrSignsValid',
                         'return BITS(21, 21) == BITS(23, 23) && !(BITS(21, 21) != 0 && '
                         '(BITS(26, 26) | BITS(14, 12) | BITS(7, 0)) == 0);'))
    new(entries, 't2SUBri12', 'subw', 0xfb5f8000 | (1 << 21) | (1 << 23) | (1 << 26) | 0x70ff,
        0xf20f0000 | (1 << 21) | (1 << 23),
        [COND_IT, rd, S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_FIXED_PC'),
         S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_FIXED_HEX_ZERO')], 'FEAT_V6T2')

    # ---- tsb is printed without a condition
    for entry in entries:
        if entry.comment == 't2TSB':
            entry.specs = [sp for sp in entry.specs if sp[0] != 'ARM_KIND_CONDITION_MNEMONIC']

    # ---- ldc / stc: ARMv8 allows cp14 only
    for entry in entries:
        if entry.comment.startswith(('t2LDC_', 't2LDCL_', 't2STC_', 't2STCL_')):
            for i, spec in enumerate(entry.specs):
                if spec[0] == 'ARM_KIND_A32_OPTION' and spec[3] == 'ARM_A32_OPT_COPROC':
                    entry.specs[i] = (spec[0], spec[1], 'ARM_A32_COPROC_CP14_V8', spec[3])
        if entry.comment in ('t2STRBi8', 't2STRHi8', 't2STRi8', 't2STRBT', 't2STRHT', 't2STRT',
                             't2STRBs', 't2STRHs', 't2STRs', 't2STRBi12', 't2STRHi12', 't2STRi12'):
            pass
    # ---- clrex is ARMv7 in Thumb (clang: armv6t2 has no v7clrex)
    for entry in entries:
        if entry.mnemonic == 'clrex':
            entry.feature = 'FEAT_V7'
    return entries


def generate(records, args):
    for key, filename, table, what, namespaces, skip, size, fix in (
            ('t16', 'ArmEncodingTableT16.cpp', 'gArmT16EncodingTable',
             'The encoding table of the 16 bit Thumb instructions',
             set(['Thumb', 'ThumbSBit', 'Thumb2']), T16_SKIP, 2, fixups16),
            ('t32', 'ArmEncodingTableT32.cpp', 'gArmT32EncodingTable',
             'The encoding table of the 32 bit Thumb instructions (but the floating point and Advanced SIMD)',
             set(['Thumb', 'Thumb2', 'Thumb2CoProc']), T32_SKIP, 4, fixups32)):
        entries, skipped = gen.generate_namespace(records, namespaces, skip, size)
        entries = fix(entries)
        entries = gen.sort_entries(entries)
        used = set(e.condition for e in entries if e.condition)
        helpers = ''.join('bool %s(uint32 e)\n{\n    %s\n}\n\n' % (n, b)
                          for n, b in HELPERS.items() if n in used)
        gen.emit(os.path.join(gen.SOURCE, filename), table, what, entries, helpers=helpers)
        print('%s: %d entries -> %s' % (key, len(entries), filename))
        for name, why in skipped.items():
            print('   skipped %s: %s' % (name, why))
