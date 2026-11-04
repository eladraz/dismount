"""A64 system instructions: hints, barriers, pstate, sys, mrs / msr.

Layout: 1101010100 L op0(2) op1(3) CRn(4) CRm(4) op2(3) Rt(5)
"""
import a64dsl
from a64dsl import *

HINT = '1101010100 0 00 011 0010 %s %s 11111'


def fixed_x(number):
    return lambda: a64dsl._spec('ARM_KIND_A64_FIXED_REGISTER', 0, 'ARM_GPR_X', number)


def hints():
    # (hint number, mnemonic, operands, feature)
    named = [
        (0, 'nop', [], 'FEAT_A64_BASE'), (1, 'yield', [], 'FEAT_A64_BASE'),
        (2, 'wfe', [], 'FEAT_A64_BASE'), (3, 'wfi', [], 'FEAT_A64_BASE'),
        (4, 'sev', [], 'FEAT_A64_BASE'), (5, 'sevl', [], 'FEAT_A64_BASE'),
        (6, 'dgh', [], 'FEAT_A64_BASE'), (7, 'xpaclri', [], 'FEAT_A64_BASE'),
        (8, 'pacia1716', [], 'FEAT_A64_BASE'), (10, 'pacib1716', [], 'FEAT_A64_BASE'),
        (12, 'autia1716', [], 'FEAT_A64_BASE'), (14, 'autib1716', [], 'FEAT_A64_BASE'),
        (16, 'esb', [], 'FEAT_RAS'),
        (17, 'psb', [opt('ARM_OPT_CSYNC')], 'FEAT_SPE'),
        (18, 'tsb', [opt('ARM_OPT_CSYNC')], 'FEAT_TRF'),
        (20, 'csdb', [], 'FEAT_A64_BASE'), (22, 'clrbhb', [], 'FEAT_A64_BASE'),
        (24, 'paciaz', [], 'FEAT_A64_BASE'), (25, 'paciasp', [], 'FEAT_A64_BASE'),
        (26, 'pacibz', [], 'FEAT_A64_BASE'), (27, 'pacibsp', [], 'FEAT_A64_BASE'),
        (28, 'autiaz', [], 'FEAT_A64_BASE'), (29, 'autiasp', [], 'FEAT_A64_BASE'),
        (30, 'autibz', [], 'FEAT_A64_BASE'), (31, 'autibsp', [], 'FEAT_A64_BASE'),
        (32, 'bti', [opt('ARM_OPT_BTI')], 'FEAT_A64_BASE'),
        (34, 'bti', [opt('ARM_OPT_BTI')], 'FEAT_A64_BASE'),
        (36, 'bti', [opt('ARM_OPT_BTI')], 'FEAT_A64_BASE'),
        (38, 'bti', [opt('ARM_OPT_BTI')], 'FEAT_A64_BASE'),
        (39, 'pacm', [], 'FEAT_A64_BASE'),
        (40, 'chkfeat', [fixed_x(16)], 'FEAT_A64_BASE'),
        (19, 'gcsb', [opt('ARM_OPT_DSYNC')], 'FEAT_A64_BASE'),
    ]
    for number, mnemonic, ops, feat in named:
        crm, op2 = number >> 3, number & 7
        E(mnemonic, HINT % (format(crm, '04b'), format(op2, '03b')), ops, feat=feat,
          flags='ARM_ENTRY_ALIAS')
    E('hint', HINT % ('CRm', 'op2'), [],
      flags=0)
    ENTRIES[-1].pattern = HINT % ('CRm', 'op2')
    ENTRIES[-1].ops = [a64dsl._spec('ARM_KIND_IMMEDIATE', 5, 'ARM_SPEC_HEX', 7)]


def barriers():
    base = '1101010100 0 00 011 0011 %s %s 11111'
    E('ssbb', base % ('0000', '100'), [], flags='ARM_ENTRY_ALIAS')
    E('pssbb', base % ('0100', '100'), [], flags='ARM_ENTRY_ALIAS')
    E('dfb', base % ('1100', '100'), [], flags='ARM_ENTRY_ALIAS')
    E('dsb', base % ('CRm', '100'), [opt('ARM_OPT_BARRIER')],
      cond='getOptionName(ARM_OPT_BARRIER, $CRm) != NULL')
    E('dsb', base % ('CRm', '100'), [imm('CRm')])
    E('dsb', base % ('nx:2 10', '001'), [opt('ARM_OPT_BARRIER_NXS')], feat='FEAT_XS')
    E('dmb', base % ('CRm', '101'), [opt('ARM_OPT_BARRIER')],
      cond='getOptionName(ARM_OPT_BARRIER, $CRm) != NULL')
    E('dmb', base % ('CRm', '101'), [imm('CRm')])
    E('isb', base % ('1111', '110'), [])
    E('isb', base % ('CRm', '110'), [imm('CRm')])
    E('sb', base % ('CRm', '111'), [], feat='FEAT_SB')
    E('clrex', base % ('1111', '010'), [])
    E('clrex', base % ('CRm', '010'), [imm('CRm', hex=True)])
    E('tcommit', base % ('0000', '011'), [], feat='FEAT_TME')


def pstate():
    base = '1101010100 0 00 %s 0100 %s %s 11111'
    E('cfinv', base % ('000', 'CRm', '000'), [], feat='FEAT_FLAGM')
    E('xaflag', base % ('000', 'CRm', '001'), [], feat='FEAT_FLAGM2')
    E('axflag', base % ('000', 'CRm', '010'), [], feat='FEAT_FLAGM2')
    E('smstart', base % ('011', '0111', '011'), [])
    E('smstop', base % ('011', '0110', '011'), [])
    for crm, name in [('0011', 'start'), ('0101', 'start')]:
        E('smstart', base % ('011', crm, '011'), [opt('ARM_OPT_SVCR')])
    for crm in ['0010', '0100']:
        E('smstop', base % ('011', crm, '011'), [opt('ARM_OPT_SVCR')])
    fields = [('SPSel', 0, 5, 'FEAT_A64_BASE'), ('DAIFSet', 3, 6, 'FEAT_A64_BASE'),
              ('DAIFClr', 3, 7, 'FEAT_A64_BASE'), ('PAN', 0, 4, 'FEAT_A64_BASE'),
              ('UAO', 0, 3, 'FEAT_A64_BASE'), ('DIT', 3, 2, 'FEAT_A64_BASE'),
              ('SSBS', 3, 1, 'FEAT_A64_BASE'), ('TCO', 3, 4, 'FEAT_MTE'),
              ('PM', 1, 0, 'FEAT_A64_BASE')]
    # ALLINT is op1 1, op2 0 and CRm 000x, the value 0x48 of the names
    E('msr', base % ('001', '000 imm1:1', '000'),
      [opt('ARM_OPT_PSTATE'), imm('imm1', hex=True)], feat='FEAT_A64_BASE')
    for name, op1, op2, feat in fields:
        if name == 'PM':
            # CRm is 001 and the one bit value
            E('msr', base % ('001', '001 imm1:1', '000'),
              [opt('ARM_OPT_PSTATE'), imm('imm1', hex=True)], feat=feat)
            continue
        E('msr', base % (format(op1, '03b'), 'CRm', format(op2, '03b')),
          [opt('ARM_OPT_PSTATE'), imm('CRm', hex=True)], feat=feat)


def wfx():
    E('wfet', '1101010100 0 00 011 0001 0000 000 Rd', [X('Rd')], feat='FEAT_WFXT')
    E('wfit', '1101010100 0 00 011 0001 0000 001 Rd', [X('Rd')], feat='FEAT_WFXT')


def sys_aliases():
    sysl = '1101010100 1 01 op1 CRn CRm op2 Rt'
    sysp = '1101010100 0 01 op1 CRn CRm op2 Rt'
    # Options with names (the name tables are generated)
    for mnemonic, scheme in [('dc', 'ARM_OPT_SYS_DC'), ('ic', 'ARM_OPT_SYS_IC'),
                             ('at', 'ARM_OPT_SYS_AT'), ('tlbi', 'ARM_OPT_SYS_TLBI'),
                             ('plbi', 'ARM_OPT_SYS_PLBI')]:
        E(mnemonic, sysp, [sys_opt(scheme), X('Rt')],
          cond='isNamedOption(e, %s, true)' % scheme)
        E(mnemonic, sysp, [sys_opt(scheme)], cond='isNamedOption(e, %s, false)' % scheme)
    E('brb', '1101010100 0 01 001 0111 0010 op2 11111', [sys_opt('ARM_OPT_SYS_BRB')],
      cond='isNamedOption(e, ARM_OPT_SYS_BRB, true) || isNamedOption(e, ARM_OPT_SYS_BRB, false)')
    for mnemonic, op2, feat in [('cfp', '100', 'FEAT_SPECRES'), ('dvp', '101', 'FEAT_SPECRES'),
                                ('cosp', '110', 'FEAT_SPECRES'), ('cpp', '111', 'FEAT_SPECRES')]:
        E(mnemonic, '1101010100 0 01 011 0111 0011 %s Rt' % op2,
          [sys_opt('ARM_OPT_SYS_PREDRES'), X('Rt')], feat=feat)
    E('apas', '1101010100 0 01 110 0111 0000 000 Rt', [X('Rt')])
    E('trcit', '1101010100 0 01 011 0111 0010 111 Rt', [X('Rt')])
    E('gcspushm', '1101010100 0 01 011 0111 0111 000 Rt', [X('Rt')], feat='FEAT_GCS')
    E('gcsss1', '1101010100 0 01 011 0111 0111 010 Rt', [X('Rt')], feat='FEAT_GCS')
    E('gcspushx', '1101010100 0 01 000 0111 0111 100 11111', [], feat='FEAT_GCS')
    E('gcspopcx', '1101010100 0 01 000 0111 0111 101 11111', [], feat='FEAT_GCS')
    E('gcspopx', '1101010100 0 01 000 0111 0111 110 11111', [], feat='FEAT_GCS')
    E('gcspopm', '1101010100 1 01 011 0111 0111 001 11111', [], feat='FEAT_GCS')
    E('gcspopm', '1101010100 1 01 011 0111 0111 001 Rt', [X('Rt')], feat='FEAT_GCS')
    E('gcsss2', '1101010100 1 01 011 0111 0111 011 Rt', [X('Rt')], feat='FEAT_GCS')
    E('tstart', '1101010100 1 00 011 0011 0000 011 Rt', [X('Rt')], feat='FEAT_TME')
    E('ttest', '1101010100 1 00 011 0011 0001 011 Rt', [X('Rt')], feat='FEAT_TME')
    # tlbip and the generic sysp (a pair of registers, even)
    E('tlbip', '1101010101 0 01 op1 CRn CRm op2 Rt', [sys_opt('ARM_OPT_SYS_TLBIP'), XPZ('Rt')],
      feat='FEAT_SYSREG128', cond='isNamedOption(e, ARM_OPT_SYS_TLBIP, true)')
    E('sysp', '1101010101 0 01 op1 CRn CRm op2 11111',
      [simm_imm('op1'), cr('CRn'), cr('CRm'), simm_imm('op2')], feat='FEAT_SYSREG128')
    E('sysp', '1101010101 0 01 op1 CRn CRm op2 Rt',
      [simm_imm('op1'), cr('CRn'), cr('CRm'), simm_imm('op2'), XP('Rt')], feat='FEAT_SYSREG128')
    # The generic forms
    E('sys', sysp.replace('Rt', '11111'),
      [simm_imm('op1'), cr('CRn'), cr('CRm'), simm_imm('op2')])
    E('sys', sysp, [simm_imm('op1'), cr('CRn'), cr('CRm'), simm_imm('op2'), X('Rt')])
    E('sysl', sysl, [X('Rt'), simm_imm('op1'), cr('CRn'), cr('CRm'), simm_imm('op2')])


def sys_opt(scheme):
    return lambda: a64dsl._spec('ARM_KIND_OPTION', 0, '0', scheme)


def simm_imm(f):
    return imm(f, hex=True)


def cr(f):
    return lambda: a64dsl._spec('ARM_KIND_OPTION', a64dsl._field(f)[0], '0', 'ARM_OPT_CREG')


def tchange():
    for name, bf in [('tchangef', '0'), ('tchangeb', '1')]:
        E(name, '110101011 0000 %s 1 0 000000 Rn Rd' % bf, [X('Rd'), X('Rn'), opt('ARM_OPT_NB')],
          feat='FEAT_POE2')
        E(name, '110101011 0000 %s 0 0 000000 Rn Rd' % bf, [X('Rd'), X('Rn')], feat='FEAT_POE2')
    for name, bf in [('tchangef', '0'), ('tchangeb', '1')]:
        E(name, '110101011 00 1 0 %s 1 0 imm7 Rd' % bf if False else
          '110101011 00 1 0 %s 1 0 0000 imm7 Rd' % bf,
          [X('Rd'), imm('imm7', hex=True), opt('ARM_OPT_NB')], feat='FEAT_POE2')
        E(name, '110101011 00 1 0 %s 0 0 0000 imm7 Rd' % bf,
          [X('Rd'), imm('imm7', hex=True)], feat='FEAT_POE2')
    E('texit', '1101011 0111 11111 00000 1 11111 00000', [opt('ARM_OPT_NB')], feat='FEAT_TEV')
    E('texit', '1101011 0111 11111 00000 0 11111 00000', [], feat='FEAT_TEV')


def msr_mrs():
    # mrs / msr with the system register, any op0 but the sys encodings (01)
    E('mrs', '1101010100 1 op0 op1 CRn CRm op2 Rt', [X('Rt'), sysreg()])
    E('msr', '1101010100 0 op0 op1 CRn CRm op2 Rt', [sysreg(), X('Rt')])
    # The 128 bit system registers (FEAT_SYSREG128): the register pair is even
    E('mrrs', '1101010101 1 op0 op1 CRn CRm op2 Rt', [XP('Rt'), sysreg()], feat='FEAT_SYSREG128')
    E('msrr', '1101010101 0 op0 op1 CRn CRm op2 Rt', [sysreg(), XP('Rt')], feat='FEAT_SYSREG128')


def system():
    stshh()
    hints()
    barriers()
    pstate()
    wfx()
    sys_aliases()
    tchange()
    msr_mrs()


def stshh():
    E('stshh', '1101010100 0 00 001 1001 0110 op2 11111', [opt('ARM_OPT_STSHH')],
      feat='FEAT_PCDPHINT')
