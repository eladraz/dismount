#!/usr/bin/env python3
"""The A32 entries which don't come from the LLVM records as they are: the
aliases of the LLVM printer, and the instructions which the C++ decoder of
LLVM decodes differently from the encoding of the record.

fixups(entries) edits the list of the A32 entries (generated from the records
by gen_aarch32_tables.py) and returns the C++ helpers which the new entries
use (the conditions).
"""
from aarch32_classes import S

# Conditions (C++ functions of the encoding): name -> body
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


COND = S('ARM_KIND_CONDITION_MNEMONIC', 28)
SETS = S('ARM_KIND_SETS_FLAGS', 20)


def gpr(position, flags='ARM_A32_GPR_ANY'):
    return S('ARM_KIND_GPR', position, flags, 4)


def new(entries, comment, mnemonic, mask, value, specs, feature='FEAT_A32_BASE', flags=None,
        condition=None, flow='0'):
    from gen_aarch32_tables import Entry
    entry = Entry(mnemonic, mask, value, specs, feature, flags or [], flow, condition, comment)
    entries.append(entry)
    return entry


def fixups(entries, records):
    from gen_aarch32_tables import Entry

    # ---- The shifts: mov Rd, Rm, lsl #n is printed lsl Rd, Rm, #n
    for record, kind in (('MOVsi', 'imm'), ('MOVsr', 'reg')):
        base = by_name(entries, record)
        entries[:] = [e for e in entries if e is not base]
        for type_, name in enumerate(('lsl', 'lsr', 'asr', 'ror')):
            mask = base.mask | 0x60
            value = base.value | (type_ << 5)
            if kind == 'imm':
                condition = None
                if name in ('lsl', 'ror'):
                    condition = helper('shiftAmountNotZero', 'return BITS(11, 7) != 0;')
                new(entries, record, name, mask, value,
                    [SETS, COND, gpr(12), gpr(0),
                     S('ARM_KIND_A32_FIELD', 7, '0', 'ARM_A32_FIELD_SHIFT_AMOUNT')],
                    condition=condition)
            else:
                new(entries, record, name, mask, value, [SETS, COND, gpr(12), gpr(0), gpr(8)])
        if kind == 'imm':
            # rrx: ror #0
            new(entries, record, 'rrx', base.mask | 0x60 | 0xF80, base.value | 0x60,
                [SETS, COND, gpr(12), gpr(0)])

    # ---- push and pop with several registers
    helper('manyRegisters', 'uint32 list = BITS(15, 0);\n    return (list & (list - 1)) != 0;')
    for record, alias in (('STMDB_UPD', 'push'), ('LDMIA_UPD', 'pop')):
        base = by_name(entries, record)
        new(entries, record, alias, base.mask | 0x000f0000, base.value | 0x000d0000,
            [COND, S('ARM_KIND_GPR_LIST', 0, '0', 16)], condition='manyRegisters')
        entries[-1].flags = ['ARM_ENTRY_ALIAS']

    # ---- The hints
    hint = by_name(entries, 'HINT')
    for number, name, feature in ((0, 'nop', 'FEAT_V6K'), (1, 'yield', 'FEAT_V6K'),
                                  (2, 'wfe', 'FEAT_V6K'), (3, 'wfi', 'FEAT_V6K'),
                                  (4, 'sev', 'FEAT_V6K'), (5, 'sevl', 'FEAT_V8_AA32'),
                                  (16, 'esb', 'FEAT_RAS_AA32'), (20, 'csdb', 'FEAT_V6K')):
        new(entries, 'HINT', name, 0x0fffffff, 0x0320f000 | number, [COND], feature,
            flags=['ARM_ENTRY_ALIAS'])
    # dbg (the hint space 0xf0 - 0xff) is before hint (all of them)
    # ssbb and pssbb
    new(entries, 'DSB', 'ssbb', 0xffffffff, 0xf57ff040, [], 'FEAT_V7', flags=['ARM_ENTRY_ALIAS'])
    new(entries, 'DSB', 'pssbb', 0xffffffff, 0xf57ff044, [], 'FEAT_V7', flags=['ARM_ENTRY_ALIAS'])

    # ---- cps. The C++ decoder of LLVM decides the form by imod and M, and it
    # is reached from the records of cps and from the ones that share the
    # encoding with it when the condition is 1111 (qadd, smlabb, smlabt, swp)
    imod = S('ARM_KIND_A32_MNEMONIC_PART', 0, '0', 'ARM_A32_PART_CPS_IMOD')
    iflags = S('ARM_KIND_A32_OPTION', 6, '0', 'ARM_A32_OPT_IFLAGS')
    mode = S('ARM_KIND_IMMEDIATE', 0, 'ARM_SPEC_HEX', 5)
    patterns = [(0xffffffe0, 0xf1020000, 'FEAT_A32_BASE', []),
                (0xfff3fe3f, 0xf1000000, 'FEAT_A32_BASE', []),
                (0xfff3fe20, 0xf1020000, 'FEAT_A32_BASE', [])]
    for record in ('QADD', 'SMLABB', 'SMLABT', 'SWP'):
        base = by_name(entries, record)
        patterns.append((base.mask | 0xf0000000, base.value | 0xf0000000, base.feature,
                         list(base.flags)))
    entries[:] = remove(entries, 'CPS1p', 'CPS2p', 'CPS3p')
    forms = [(0x000a0000, 0x000a0000, [imod, iflags, mode]),      # imod 2 / 3, M = 1
             (0x000a0000, 0x00080000, [imod, iflags]),            # imod 2 / 3, M = 0
             (0x000c0000, 0x00000000, [mode])]                    # imod 0
    for mask, value, feature, flags in patterns:
        for form_mask, form_value, specs in forms:
            if (value ^ form_value) & mask & form_mask:
                continue
            # bit 5 and bit 16 are zero
            if value & 0x10020:
                continue
            new(entries, 'CPS', 'cps' if specs[0] is mode else 'cps', mask | form_mask | 0x10020,
                value | form_value, specs, feature, flags=flags)

    # ---- setpan: the bits 0-3 and 8-19 are not decoded
    entries[:] = remove(entries, 'SETPAN')
    new(entries, 'SETPAN', 'setpan', 0xfff000f0, 0xf1100000,
        [S('ARM_KIND_IMMEDIATE', 9, 'ARM_SPEC_HEX', 1)], 'FEAT_PAN_AA32')

    # ---- rfe with writeback: the registers are not decoded when the low bits
    # are not the ones of the record (the printed operand is the addressing
    # mode: "rfeia #0x1!")
    for pu, name, mode_text in ((0, 'rfeda', 'ARM_A32_FIXED_RFE_DA'), (1, 'rfeia', 'ARM_A32_FIXED_RFE_IA'),
                                (2, 'rfedb', 'ARM_A32_FIXED_RFE_DB'), (3, 'rfeib', 'ARM_A32_FIXED_RFE_IB')):
        new(entries, 'RFE_UPD', name, 0xfff00000, 0xf8300000 | (pu << 23),
            [S('ARM_KIND_A32_FIXED', 0, '0', mode_text)])

    # ---- mov to pc prints the immediate unsigned: an entry before mov
    base = by_name(entries, 'MOVi')
    new(entries, 'MOVi', 'mov', base.mask | 0xf000, base.value | 0xf000,
        [SETS, COND, gpr(12), S('ARM_KIND_A32_MODIFIED_IMMEDIATE', 0, 'ARM_A32_MODIMM_UNSIGNED', 0)])
    # ---- the long multiplies and mul / mla are older than the LLVM predicates say
    for entry in entries:
        if entry.comment in ('MUL', 'MLA', 'SMULL', 'UMULL', 'SMLAL', 'UMLAL'):
            entry.feature = 'FEAT_A32_BASE'
    # ---- nop before ARMv6K is mov r0, r0 (the assembler writes it, the
    # disassembler prints the mov)
    new(entries, 'NOP', 'nop', 0x0fffffff, 0x01a00000, [COND], flags=['ARM_ENTRY_ASSEMBLE_ONLY'])

    # ---- hvc is decoded whatever the condition is (but 1111)
    hvc = by_name(entries, 'HVC')
    hvc.mask = 0x0ff000f0
    hvc.value = 0xe1400070
    hvc.condition = helper('conditionNotNever', 'return BITS(31, 28) != 15;')

    # ---- ldc / stc: ARMv8 has only the coprocessor 14
    for entry in entries:
        if entry.comment.startswith(('LDC_', 'LDCL_', 'STC_', 'STCL_')):
            for i, spec in enumerate(entry.specs):
                if spec[0] == 'ARM_KIND_A32_OPTION' and spec[3] == 'ARM_A32_OPT_COPROC':
                    entry.specs[i] = (spec[0], spec[1], 'ARM_A32_COPROC_CP14_V8', spec[3])
    return entries
