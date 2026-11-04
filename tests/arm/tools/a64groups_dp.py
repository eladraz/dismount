"""A64 data processing: immediate and register, and the branches and
exceptions."""
import a64dsl
from a64dsl import *

SIZES = [('w', 0, 32), ('x', 1, 64)]


def pattern_sf(pattern, sf):
    """Replace the leading 'sf' token of a pattern by the bit."""
    assert pattern.startswith('sf ')
    return str(sf) + pattern[2:]


def dp_immediate():
    # ---- PC relative addressing
    E('adr', '0 immlo 10000 immhi Rd', [X('Rd'), pcrel('ARM_PCREL_ADR')])
    E('adrp', '1 immlo 10000 immhi Rd', [X('Rd'), pcrel('ARM_PCREL_ADRP')])

    # ---- Add and subtract (immediate)
    # mov to or from the stack pointer
    E('mov', 'sf 0 0 100010 0 000000000000 Rn Rd', [RS('Rd'), RS('Rn')],
      flags='ARM_ENTRY_ALIAS', cond='$Rd == 31 || $Rn == 31')
    E('cmn', 'sf 0 1 100010 sh imm12 Rn 11111', [RS('Rn'), arith_imm()], flags='ARM_ENTRY_ALIAS')
    E('cmp', 'sf 1 1 100010 sh imm12 Rn 11111', [RS('Rn'), arith_imm()], flags='ARM_ENTRY_ALIAS')
    E('add', 'sf 0 0 100010 sh imm12 Rn Rd', [RS('Rd'), RS('Rn'), arith_imm()])
    E('adds', 'sf 0 1 100010 sh imm12 Rn Rd', [R('Rd'), RS('Rn'), arith_imm()],
      flags='ARM_ENTRY_SETS_FLAGS')
    E('sub', 'sf 1 0 100010 sh imm12 Rn Rd', [RS('Rd'), RS('Rn'), arith_imm()])
    E('subs', 'sf 1 1 100010 sh imm12 Rn Rd', [R('Rd'), RS('Rn'), arith_imm()],
      flags='ARM_ENTRY_SETS_FLAGS')

    # ---- Logical (immediate)
    for size, sf, bits in SIZES:
        n = '0' if sf == 0 else 'N'
        pattern = '%d opc 100100 %s immr imms Rn Rd' % (sf, n)

        def logical(mnemonic, opc, dest_sp):
            p = '%d %s 100100 %s immr imms Rn Rd' % (sf, opc, n)
            E(mnemonic, p, [sized_sp(size, 'Rd') if dest_sp else sized(size, 'Rd'),
                            sized(size, 'Rn'), logical_imm(size)],
              flags='ARM_ENTRY_SETS_FLAGS' if not dest_sp else 0)
        # Aliases first
        E('tst', '%d 11 100100 %s immr imms Rn 11111' % (sf, n),
          [sized(size, 'Rn'), logical_imm(size)], flags='ARM_ENTRY_ALIAS')
        E('mov', '%d 01 100100 %s immr imms 11111 Rd' % (sf, n),
          [sized_sp(size, 'Rd'), logical_imm(size, True)], flags='ARM_ENTRY_ALIAS',
          cond='isLogicalMoveAlias(e, %d)' % bits)
        logical('and', '00', True)
        logical('orr', '01', True)
        logical('eor', '10', True)
        logical('ands', '11', False)

    # ---- Move wide (immediate)
    for size, sf, bits in SIZES:
        h = 'h' if sf == 0 else 'hw'
        top = '0' if sf == 0 else ''
        prefix = '%d' % sf
        if sf == 0:
            fixed = '0 h:1 imm16 Rd'      # hw<1> is zero
        else:
            fixed = 'hw imm16 Rd'
        E('mov', '%s 10 100101 %s' % (prefix, fixed),
          [sized(size, 'Rd'), move_wide(size, 'ARM_MOVE_WIDE_MOVZ')],
          flags='ARM_ENTRY_ALIAS', cond='$imm16 != 0 || %s == 0' % ('$h' if sf == 0 else '$hw'))
        E('mov', '%s 00 100101 %s' % (prefix, fixed),
          [sized(size, 'Rd'), move_wide(size, 'ARM_MOVE_WIDE_MOVN')],
          flags='ARM_ENTRY_ALIAS', cond='isMoveNegatedAlias(e, %d)' % bits)
        E('movn', '%s 00 100101 %s' % (prefix, fixed),
          [sized(size, 'Rd'), move_wide(size)])
        E('movz', '%s 10 100101 %s' % (prefix, fixed),
          [sized(size, 'Rd'), move_wide(size)])
        E('movk', '%s 11 100101 %s' % (prefix, fixed),
          [sized(size, 'Rd'), move_wide(size)])

    # ---- Bitfield
    for size, sf, bits in SIZES:
        if sf == 0:
            head = '0 %s 100110 0 0 immr:5 0 imms:5 Rn Rd'
        else:
            head = '1 %s 100110 1 immr imms Rn Rd'
        last = bits - 1

        def bf(mnemonic, opc, ops, flags='ARM_ENTRY_ALIAS', cond=None, immr=None, imms=None,
               rn=None):
            """immr / imms: the constants of an alias, in the pattern."""
            width = 5 if sf == 0 else 6
            if immr is None:
                r_token = 'immr:%d' % width
            else:
                r_token = format(immr, '0%db' % width)
            if imms is None:
                s_token = 'imms:%d' % width
            else:
                s_token = format(imms, '0%db' % width)
            rn_token = 'Rn' if rn is None else format(rn, '05b')
            if sf == 0:
                pattern = '0 %s 100110 0 0 %s 0 %s %s Rd' % (opc, r_token, s_token, rn_token)
            else:
                pattern = '1 %s 100110 1 %s %s %s Rd' % (opc, r_token, s_token, rn_token)
            E(mnemonic, pattern, ops, flags=flags, cond=cond)

        # sbfm
        bf('asr', '00', [sized(size, 'Rd'), sized(size, 'Rn'), imm('immr')], imms=last)
        bf('sxtb', '00', [sized(size, 'Rd'), W('Rn')], immr=0, imms=7)
        bf('sxth', '00', [sized(size, 'Rd'), W('Rn')], immr=0, imms=15)
        if sf == 1:
            bf('sxtw', '00', [X('Rd'), W('Rn')], immr=0, imms=31)
        bf('sbfiz', '00', [sized(size, 'Rd'), sized(size, 'Rn'),
                           bitfield('ARM_BF_LSB_NEGATED', size), imm('imms', plus=True)],
           cond='$imms < $immr')
        bf('sbfx', '00', [sized(size, 'Rd'), sized(size, 'Rn'), imm('immr'),
                          bitfield('ARM_BF_WIDTH', size)], cond='$imms >= $immr')
        bf('sbfm', '00', [sized(size, 'Rd'), sized(size, 'Rn'), imm('immr'), imm('imms')],
           flags=0)
        # bfm. LLVM prints bfi for a bfm with the zero register (bfc is read only)
        bf('bfc', '01', [sized(size, 'Rd'), bitfield('ARM_BF_LSB_NEGATED', size),
                         imm('imms', plus=True)], cond='$imms < $immr', rn=31,
           flags='ARM_ENTRY_ALIAS | ARM_ENTRY_ASSEMBLE_ONLY')
        bf('bfi', '01', [sized(size, 'Rd'), sized(size, 'Rn'),
                         bitfield('ARM_BF_LSB_NEGATED', size), imm('imms', plus=True)],
           cond='$imms < $immr')
        bf('bfxil', '01', [sized(size, 'Rd'), sized(size, 'Rn'), imm('immr'),
                           bitfield('ARM_BF_WIDTH', size)], cond='$imms >= $immr')
        bf('bfm', '01', [sized(size, 'Rd'), sized(size, 'Rn'), imm('immr'), imm('imms')],
           flags=0)
        # ubfm
        bf('lsl', '10', [sized(size, 'Rd'), sized(size, 'Rn'),
                         bitfield('ARM_BF_LSL_AMOUNT', size)],
           cond='$imms != %d && $imms + 1 == $immr' % last)
        bf('lsr', '10', [sized(size, 'Rd'), sized(size, 'Rn'), imm('immr')], imms=last)
        if sf == 0:
            bf('uxtb', '10', [W('Rd'), W('Rn')], immr=0, imms=7)
            bf('uxth', '10', [W('Rd'), W('Rn')], immr=0, imms=15)
        bf('ubfiz', '10', [sized(size, 'Rd'), sized(size, 'Rn'),
                           bitfield('ARM_BF_LSB_NEGATED', size), imm('imms', plus=True)],
           cond='$imms < $immr')
        bf('ubfx', '10', [sized(size, 'Rd'), sized(size, 'Rn'), imm('immr'),
                          bitfield('ARM_BF_WIDTH', size)], cond='$imms >= $immr')
        bf('ubfm', '10', [sized(size, 'Rd'), sized(size, 'Rn'), imm('immr'), imm('imms')],
           flags=0)

    # ---- Extract
    for size, sf, bits in SIZES:
        if sf == 0:
            head = '0 00 100111 0 0 Rm 0 imms:5 Rn Rd'
        else:
            head = '1 00 100111 1 0 Rm imms Rn Rd'
        E('ror', head, [sized(size, 'Rd'), sized(size, 'Rn'), imm('imms', hex=True), tied('Rm', 'Rn')],
          flags='ARM_ENTRY_ALIAS', cond='$Rn == $Rm')
        E('extr', head, [sized(size, 'Rd'), sized(size, 'Rn'), sized(size, 'Rm'), imm('imms', hex=True)])


def dp_register():
    # ---- Logical (shifted register)
    for size, sf, bits in SIZES:
        n_top = '0 ' if sf == 0 else ''
        # imm6 of the 32 bit forms is below 32: bit 15 is zero
        imm6 = '0 imm5:5' if sf == 0 else 'imm6'
        base = '%d %%s 01010 shift %%s Rm %s Rn Rd' % (sf, imm6)
        # Aliases
        E('mov', '%d 01 01010 00 0 Rm 000000 11111 Rd' % sf if sf == 1 else
          '0 01 01010 00 0 Rm 0 00000 11111 Rd',
          [sized(size, 'Rd'), sized(size, 'Rm')], flags='ARM_ENTRY_ALIAS')
        E('mvn', (base % ('01', '1')).replace('Rn Rd', '11111 Rd'),
          [sized(size, 'Rd'), shreg('Rm', size)], flags='ARM_ENTRY_ALIAS')
        E('tst', (base % ('11', '0')).replace('Rn Rd', 'Rn 11111'),
          [sized(size, 'Rn'), shreg('Rm', size)], flags='ARM_ENTRY_ALIAS')
        for mnemonic, opc, n in [('and', '00', '0'), ('bic', '00', '1'),
                                 ('orr', '01', '0'), ('orn', '01', '1'),
                                 ('eor', '10', '0'), ('eon', '10', '1'),
                                 ('ands', '11', '0'), ('bics', '11', '1')]:
            E(mnemonic, base % (opc, n),
              [sized(size, 'Rd'), sized(size, 'Rn'), shreg('Rm', size)],
              flags='ARM_ENTRY_SETS_FLAGS' if opc == '11' else 0)

    # ---- Add and subtract (shifted register)
    for size, sf, bits in SIZES:
        imm6 = '0 imm5:5' if sf == 0 else 'imm6'
        base = '%d %%s %%s 01011 shift 0 Rm %s Rn Rd' % (sf, imm6)
        E('cmn', (base % ('0', '1')).replace('Rn Rd', 'Rn 11111'),
          [sized(size, 'Rn'), shreg('Rm', size, True)], flags='ARM_ENTRY_ALIAS')
        E('cmp', (base % ('1', '1')).replace('Rn Rd', 'Rn 11111'),
          [sized(size, 'Rn'), shreg('Rm', size, True)], flags='ARM_ENTRY_ALIAS')
        E('neg', (base % ('1', '0')).replace('Rn Rd', '11111 Rd'),
          [sized(size, 'Rd'), shreg('Rm', size, True)], flags='ARM_ENTRY_ALIAS')
        E('negs', (base % ('1', '1')).replace('Rn Rd', '11111 Rd'),
          [sized(size, 'Rd'), shreg('Rm', size, True)], flags='ARM_ENTRY_ALIAS')
        for mnemonic, op, s in [('add', '0', '0'), ('adds', '0', '1'),
                                ('sub', '1', '0'), ('subs', '1', '1')]:
            E(mnemonic, base % (op, s),
              [sized(size, 'Rd'), sized(size, 'Rn'), shreg('Rm', size, True)],
              flags='ARM_ENTRY_SETS_FLAGS' if s == '1' else 0)

    # ---- Add and subtract (extended register)
    E('cmn', 'sf 0 1 01011 00 1 Rm option imm3 Rn 11111', [RS('Rn'), extreg('Rm')],
      flags='ARM_ENTRY_ALIAS')
    E('cmp', 'sf 1 1 01011 00 1 Rm option imm3 Rn 11111', [RS('Rn'), extreg('Rm')],
      flags='ARM_ENTRY_ALIAS')
    E('add', 'sf 0 0 01011 00 1 Rm option imm3 Rn Rd', [RS('Rd'), RS('Rn'), extreg('Rm')])
    E('adds', 'sf 0 1 01011 00 1 Rm option imm3 Rn Rd', [R('Rd'), RS('Rn'), extreg('Rm')],
      flags='ARM_ENTRY_SETS_FLAGS')
    E('sub', 'sf 1 0 01011 00 1 Rm option imm3 Rn Rd', [RS('Rd'), RS('Rn'), extreg('Rm')])
    E('subs', 'sf 1 1 01011 00 1 Rm option imm3 Rn Rd', [R('Rd'), RS('Rn'), extreg('Rm')],
      flags='ARM_ENTRY_SETS_FLAGS')

    # ---- Add and subtract (with carry)
    E('ngc', 'sf 1 0 11010000 Rm 000000 11111 Rd', [R('Rd'), R('Rm')], flags='ARM_ENTRY_ALIAS')
    E('ngcs', 'sf 1 1 11010000 Rm 000000 11111 Rd', [R('Rd'), R('Rm')],
      flags='ARM_ENTRY_ALIAS | ARM_ENTRY_SETS_FLAGS')
    for mnemonic, op, s in [('adc', '0', '0'), ('adcs', '0', '1'),
                            ('sbc', '1', '0'), ('sbcs', '1', '1')]:
        E(mnemonic, 'sf %s %s 11010000 Rm 000000 Rn Rd' % (op, s),
          [R('Rd'), R('Rn'), R('Rm')], flags='ARM_ENTRY_SETS_FLAGS' if s == '1' else 0)

    # ---- Rotate right into flags, evaluate into flags (FEAT_FlagM)
    E('rmif', '1 0 1 11010000 imm6 00001 Rn 0 mask', [X('Rn'), imm('imm6', hex=True), imm('mask', hex=True)],
      feat='FEAT_FLAGM', flags='ARM_ENTRY_SETS_FLAGS')
    E('setf8', '0 0 1 11010000 000000 0 0010 Rn 0 1101', [W('Rn')],
      feat='FEAT_FLAGM', flags='ARM_ENTRY_SETS_FLAGS')
    E('setf16', '0 0 1 11010000 000000 1 0010 Rn 0 1101', [W('Rn')],
      feat='FEAT_FLAGM', flags='ARM_ENTRY_SETS_FLAGS')

    # ---- Conditional compare
    for mnemonic, op in [('ccmn', '0'), ('ccmp', '1')]:
        E(mnemonic, 'sf %s 1 11010010 Rm cond 0 0 Rn 0 nzcv' % op,
          [R('Rn'), R('Rm'), nzcv(), cond_op('cond')], flags='ARM_ENTRY_SETS_FLAGS')
        E(mnemonic, 'sf %s 1 11010010 imm5 cond 1 0 Rn 0 nzcv' % op,
          [R('Rn'), imm('imm5', hex=True), nzcv(), cond_op('cond')],
          flags='ARM_ENTRY_SETS_FLAGS')

    # ---- Conditional select
    not_always = '($cond >> 1) != 7'
    E('cset', 'sf 0 0 11010100 11111 cond 0 1 11111 Rd', [R('Rd'), cond_op('cond', True)],
      flags='ARM_ENTRY_ALIAS', cond=not_always)
    E('csetm', 'sf 1 0 11010100 11111 cond 0 0 11111 Rd', [R('Rd'), cond_op('cond', True)],
      flags='ARM_ENTRY_ALIAS', cond=not_always)
    E('cinc', 'sf 0 0 11010100 Rm cond 0 1 Rn Rd',
      [R('Rd'), R('Rn'), cond_op('cond', True), tied('Rm', 'Rn')],
      flags='ARM_ENTRY_ALIAS', cond=not_always + ' && $Rn == $Rm && $Rn != 31')
    E('cinv', 'sf 1 0 11010100 Rm cond 0 0 Rn Rd',
      [R('Rd'), R('Rn'), cond_op('cond', True), tied('Rm', 'Rn')],
      flags='ARM_ENTRY_ALIAS', cond=not_always + ' && $Rn == $Rm && $Rn != 31')
    E('cneg', 'sf 1 0 11010100 Rm cond 0 1 Rn Rd',
      [R('Rd'), R('Rn'), cond_op('cond', True), tied('Rm', 'Rn')],
      flags='ARM_ENTRY_ALIAS', cond=not_always + ' && $Rn == $Rm')
    for mnemonic, op, o2 in [('csel', '0', '0'), ('csinc', '0', '1'),
                             ('csinv', '1', '0'), ('csneg', '1', '1')]:
        E(mnemonic, 'sf %s 0 11010100 Rm cond 0 %s Rn Rd' % (op, o2),
          [R('Rd'), R('Rn'), R('Rm'), cond_op('cond')])

    # ---- Data processing (2 source)
    for mnemonic, opcode in [('udiv', '000010'), ('sdiv', '000011'), ('lsl', '001000'),
                             ('lsr', '001001'), ('asr', '001010'), ('ror', '001011')]:
        E(mnemonic, 'sf 0 0 11010110 Rm %s Rn Rd' % opcode, [R('Rd'), R('Rn'), R('Rm')])
    # cmpp, subp, subps, irg, gmi, pacga (MTE, PAuth)
    E('cmpp', '1 0 1 11010110 Rm 000000 Rn 11111', [XS('Rn'), XS('Rm')],
      feat='FEAT_MTE', flags='ARM_ENTRY_ASSEMBLE_ONLY | ARM_ENTRY_SETS_FLAGS')
    E('subp', '1 0 0 11010110 Rm 000000 Rn Rd', [X('Rd'), XS('Rn'), XS('Rm')], feat='FEAT_MTE')
    E('subps', '1 0 1 11010110 Rm 000000 Rn Rd', [X('Rd'), XS('Rn'), XS('Rm')],
      feat='FEAT_MTE', flags='ARM_ENTRY_SETS_FLAGS')
    E('irg', '1 0 0 11010110 11111 000100 Rn Rd', [XS('Rd'), XS('Rn')], feat='FEAT_MTE',
      flags='ARM_ENTRY_ALIAS')
    E('irg', '1 0 0 11010110 Rm 000100 Rn Rd', [XS('Rd'), XS('Rn'), X('Rm')], feat='FEAT_MTE')
    E('gmi', '1 0 0 11010110 Rm 000101 Rn Rd', [X('Rd'), XS('Rn'), X('Rm')], feat='FEAT_MTE')
    E('pacga', '1 0 0 11010110 Rm 001100 Rn Rd', [X('Rd'), X('Rn'), XS('Rm')], feat='FEAT_PAUTH')
    # crc32
    for suffix, opcode, size, rm in [('b', '010000', 'w', 'w'), ('h', '010001', 'w', 'w'),
                                     ('w', '010010', 'w', 'w'), ('x', '010011', 'x', 'x')]:
        E('crc32' + suffix, '%d 0 0 11010110 Rm %s Rn Rd' % (0 if size == 'w' else 1, opcode),
          [W('Rd'), W('Rn'), sized(rm, 'Rm')], feat='FEAT_CRC32')
    for suffix, opcode, size, rm in [('b', '010100', 'w', 'w'), ('h', '010101', 'w', 'w'),
                                     ('w', '010110', 'w', 'w'), ('x', '010111', 'x', 'x')]:
        E('crc32c' + suffix, '%d 0 0 11010110 Rm %s Rn Rd' % (0 if size == 'w' else 1, opcode),
          [W('Rd'), W('Rn'), sized(rm, 'Rm')], feat='FEAT_CRC32')

    # ---- Data processing (1 source)
    for mnemonic, sf, opcode in [('rbit', None, '000000'), ('rev16', None, '000001'),
                                 ('rev', 0, '000010'), ('rev32', 1, '000010'),
                                 ('rev', 1, '000011'), ('clz', None, '000100'),
                                 ('cls', None, '000101')]:
        pattern = 'sf 1 0 11010110 00000 %s Rn Rd' % opcode
        if sf is not None:
            pattern = '%d 1 0 11010110 00000 %s Rn Rd' % (sf, opcode)
        E(mnemonic, pattern, [R('Rd'), R('Rn')])
    # PAuth
    for mnemonic, opcode in [('pacia', '000000'), ('pacib', '000001'), ('pacda', '000010'),
                             ('pacdb', '000011'), ('autia', '000100'), ('autib', '000101'),
                             ('autda', '000110'), ('autdb', '000111')]:
        E(mnemonic, '1 1 0 11010110 00001 %s Rn Rd' % opcode, [X('Rd'), XS('Rn')],
          feat='FEAT_PAUTH')
    for mnemonic, opcode in [('paciza', '001000'), ('pacizb', '001001'), ('pacdza', '001010'),
                             ('pacdzb', '001011'), ('autiza', '001100'), ('autizb', '001101'),
                             ('autdza', '001110'), ('autdzb', '001111'), ('xpaci', '010000'),
                             ('xpacd', '010001')]:
        E(mnemonic, '1 1 0 11010110 00001 %s 11111 Rd' % opcode, [X('Rd')], feat='FEAT_PAUTH')
    # CSSC
    E('abs', 'sf 1 0 11010110 00000 001000 Rn Rd', [R('Rd'), R('Rn')], feat='FEAT_CSSC')
    E('cnt', 'sf 1 0 11010110 00000 000111 Rn Rd', [R('Rd'), R('Rn')], feat='FEAT_CSSC')
    E('ctz', 'sf 1 0 11010110 00000 000110 Rn Rd', [R('Rd'), R('Rn')], feat='FEAT_CSSC')
    # CSSC: smax, umax, smin, umin (register)
    for mnemonic, opcode in [('smax', '011000'), ('umax', '011001'),
                             ('smin', '011010'), ('umin', '011011')]:
        E(mnemonic, 'sf 0 0 11010110 Rm %s Rn Rd' % opcode, [R('Rd'), R('Rn'), R('Rm')],
          feat='FEAT_CSSC')

    # ---- Data processing (3 source)
    E('mul', 'sf 00 11011 000 Rm 0 11111 Rn Rd', [R('Rd'), R('Rn'), R('Rm')], flags='ARM_ENTRY_ALIAS')
    E('mneg', 'sf 00 11011 000 Rm 1 11111 Rn Rd', [R('Rd'), R('Rn'), R('Rm')], flags='ARM_ENTRY_ALIAS')
    E('madd', 'sf 00 11011 000 Rm 0 Ra Rn Rd', [R('Rd'), R('Rn'), R('Rm'), R('Ra')])
    E('msub', 'sf 00 11011 000 Rm 1 Ra Rn Rd', [R('Rd'), R('Rn'), R('Rm'), R('Ra')])
    for prefix, name, opcode in [('s', 'smull', '001'), ('u', 'umull', '101')]:
        E(name, '1 00 11011 %s Rm 0 11111 Rn Rd' % opcode, [X('Rd'), W('Rn'), W('Rm')],
          flags='ARM_ENTRY_ALIAS')
        E(prefix + 'mnegl', '1 00 11011 %s Rm 1 11111 Rn Rd' % opcode, [X('Rd'), W('Rn'), W('Rm')],
          flags='ARM_ENTRY_ALIAS')
        E(prefix + 'maddl', '1 00 11011 %s Rm 0 Ra Rn Rd' % opcode,
          [X('Rd'), W('Rn'), W('Rm'), X('Ra')])
        E(prefix + 'msubl', '1 00 11011 %s Rm 1 Ra Rn Rd' % opcode,
          [X('Rd'), W('Rn'), W('Rm'), X('Ra')])
    # Checked pointer arithmetic (FEAT_CPA)
    E('maddpt', '1 00 11011 011 Rm 0 Ra Rn Rd', [X('Rd'), X('Rn'), X('Rm'), X('Ra')], feat='FEAT_CPA')
    E('msubpt', '1 00 11011 011 Rm 1 Ra Rn Rd', [X('Rd'), X('Rn'), X('Rm'), X('Ra')], feat='FEAT_CPA')
    E('smulh', '1 00 11011 010 Rm 0 Ra! Rn Rd', [X('Rd'), X('Rn'), X('Rm')])
    E('umulh', '1 00 11011 110 Rm 0 Ra! Rn Rd', [X('Rd'), X('Rn'), X('Rm')])


def branches():
    E('b', '0 00101 imm26', [branch('imm26')], flow='FLOW_JUMP')
    E('bl', '1 00101 imm26', [branch('imm26')], flow='FLOW_CALL')
    E('b', '01010100 imm19 0 cond', [branch('imm19')], flow='FLOW_COND',
      # the condition is part of the mnemonic
      )
    ENTRIES[-1].ops.insert(0, 'S(ARM_KIND_CONDITION_MNEMONIC, 0, ARM_SPEC_PRINT_AL, 0)')
    E('bc', '01010100 imm19 1 cond', [branch('imm19')], feat='FEAT_HBC', flow='FLOW_COND')
    ENTRIES[-1].ops.insert(0, 'S(ARM_KIND_CONDITION_MNEMONIC, 0, ARM_SPEC_PRINT_AL, 0)')
    E('cbz', 'sf 011010 0 imm19 Rt', [R('Rt'), branch('imm19')], flow='FLOW_COND')
    E('cbnz', 'sf 011010 1 imm19 Rt', [R('Rt'), branch('imm19')], flow='FLOW_COND')
    # tbz / tbnz: the bit number is b5:b40, b5 is the sf bit of the register
    E('tbz', 'b5 011011 0 b40 imm14 Rt', [_tbz_reg, split_imm('ARM_SPLIT_TBZ_BIT', hex=True),
                                           branch('imm14')], flow='FLOW_COND')
    E('tbnz', 'b5 011011 1 b40 imm14 Rt', [_tbz_reg, split_imm('ARM_SPLIT_TBZ_BIT', hex=True),
                                            branch('imm14')], flow='FLOW_COND')

    # ---- Unconditional branch (register)
    E('br', '1101011 0000 11111 000000 Rn 00000', [X('Rn')], flow='FLOW_JUMP_REGISTER')
    E('blr', '1101011 0001 11111 000000 Rn 00000', [X('Rn')], flow='FLOW_CALL_REGISTER')
    E('ret', '1101011 0010 11111 000000 11110 00000', [], flags='ARM_ENTRY_ALIAS',
      flow='FLOW_RETURN')
    E('ret', '1101011 0010 11111 000000 Rn 00000', [X('Rn')], flow='FLOW_RETURN')
    E('eret', '1101011 0100 11111 000000 11111 00000', [], flow='FLOW_RETURN')
    E('drps', '1101011 0101 11111 000000 11111 00000', [], flow='FLOW_RETURN')
    # PAuth
    E('braaz', '1101011 0000 11111 000010 Rn 11111', [X('Rn')], feat='FEAT_PAUTH',
      flow='FLOW_JUMP_REGISTER')
    E('brabz', '1101011 0000 11111 000011 Rn 11111', [X('Rn')], feat='FEAT_PAUTH',
      flow='FLOW_JUMP_REGISTER')
    E('blraaz', '1101011 0001 11111 000010 Rn 11111', [X('Rn')], feat='FEAT_PAUTH',
      flow='FLOW_CALL_REGISTER')
    E('blrabz', '1101011 0001 11111 000011 Rn 11111', [X('Rn')], feat='FEAT_PAUTH',
      flow='FLOW_CALL_REGISTER')
    E('retaa', '1101011 0010 11111 000010 11111 11111', [], feat='FEAT_PAUTH', flow='FLOW_RETURN')
    E('retab', '1101011 0010 11111 000011 11111 11111', [], feat='FEAT_PAUTH', flow='FLOW_RETURN')
    E('eretaa', '1101011 0100 11111 000010 11111 11111', [], feat='FEAT_PAUTH', flow='FLOW_RETURN')
    E('eretab', '1101011 0100 11111 000011 11111 11111', [], feat='FEAT_PAUTH', flow='FLOW_RETURN')
    E('braa', '1101011 1000 11111 000010 Rn Rm', [X('Rn'), XS('Rm')], feat='FEAT_PAUTH',
      flow='FLOW_JUMP_REGISTER')
    E('brab', '1101011 1000 11111 000011 Rn Rm', [X('Rn'), XS('Rm')], feat='FEAT_PAUTH',
      flow='FLOW_JUMP_REGISTER')
    E('blraa', '1101011 1001 11111 000010 Rn Rm', [X('Rn'), XS('Rm')], feat='FEAT_PAUTH',
      flow='FLOW_CALL_REGISTER')
    E('blrab', '1101011 1001 11111 000011 Rn Rm', [X('Rn'), XS('Rm')], feat='FEAT_PAUTH',
      flow='FLOW_CALL_REGISTER')


def _tbz_reg():
    # Rt is x if b5, else w: the b5 bit is the sf bit of the register
    return a64dsl._spec('ARM_KIND_GPR', a64dsl._field('Rt')[0], 'ARM_GPR_SF', 5)


def newer_dp():
    # CSSC: smax, umax, smin, umin (immediate)
    for name, opc, signed in [('smax', '00', True), ('umax', '01', False),
                              ('smin', '10', True), ('umin', '11', False)]:
        E(name, 'sf 0 0 100011 1 00 %s imm8 Rn Rd' % opc,
          [R('Rd'), R('Rn'), (simm if signed else imm)('imm8', hex=True)], feat='FEAT_CSSC')
    # Checked pointer arithmetic (FEAT_CPA): addpt, subpt
    E('addpt', '1 0 0 11010 000 Rm 001 imm3 Rn Rd', [XS('Rd'), XS('Rn'), lsl_reg('Rm')],
      feat='FEAT_CPA')
    E('subpt', '1 1 0 11010 000 Rm 001 imm3 Rn Rd', [XS('Rd'), XS('Rn'), lsl_reg('Rm')],
      feat='FEAT_CPA')
    # Pointer authentication with the return address in the pc (FEAT_PAuth_LR)
    E('retaasppc', '01010101 000 imm16 11111', [pcrel('ARM_PCREL_NEGATIVE16')],
      feat='FEAT_PAUTH_LR', flow='FLOW_RETURN')
    E('retabsppc', '01010101 001 imm16 11111', [pcrel('ARM_PCREL_NEGATIVE16')],
      feat='FEAT_PAUTH_LR', flow='FLOW_RETURN')
    E('autiasppc', '11110011 100 imm16 11111', [pcrel('ARM_PCREL_NEGATIVE16')],
      feat='FEAT_PAUTH_LR')
    E('autibsppc', '11110011 101 imm16 11111', [pcrel('ARM_PCREL_NEGATIVE16')],
      feat='FEAT_PAUTH_LR')
    E('retaasppcr', '1101011 0010 11111 000010 11111 Rm', [X('Rm')], feat='FEAT_PAUTH_LR',
      flow='FLOW_RETURN')
    E('retabsppcr', '1101011 0010 11111 000011 11111 Rm', [X('Rm')], feat='FEAT_PAUTH_LR',
      flow='FLOW_RETURN')
    for name, value in [('autia171615', 0xdac1bbfe), ('autib171615', 0xdac1bffe),
                        ('pacia171615', 0xdac18bfe), ('pacib171615', 0xdac18ffe),
                        ('paciasppc', 0xdac1a3fe), ('pacibsppc', 0xdac1a7fe),
                        ('pacnbiasppc', 0xdac183fe), ('pacnbibsppc', 0xdac187fe)]:
        E(name, format(value, '032b'), [], feat='FEAT_PAUTH_LR')
    E('autiasppcr', '1101101011000001 100100 Rn 11110', [X('Rn')], feat='FEAT_PAUTH_LR')
    E('autibsppcr', '1101101011000001 100101 Rn 11110', [X('Rn')], feat='FEAT_PAUTH_LR')
    # Transactional memory
    E('tenter', '11010100 111 000 1 00000 imm7 000 00', [imm('imm7', hex=True), opt('ARM_OPT_NB')],
      feat='FEAT_TEV')
    E('tenter', '11010100 111 000000000 imm7 000 00', [imm('imm7', hex=True)],
      feat='FEAT_TEV')
    E('tcancel', '11010100 011 imm16 000 00', [imm('imm16', hex=True, decimal_zero=True)],
      feat='FEAT_TME')


def compare_branch():
    """Compare and branch (FEAT_CMPBR). cc is bits 23-21."""
    reg_cc = [('gt', '000'), ('ge', '001'), ('hi', '010'), ('hs', '011'), ('eq', '110'), ('ne', '111')]
    imm_cc = [('gt', '000'), ('lt', '001'), ('hi', '010'), ('lo', '011'), ('eq', '110'), ('ne', '111')]
    for name, cc in reg_cc:
        for sf, size in [(0, 'w'), (1, 'x')]:
            E('cb' + name, '%d 1110100 %s Rm 00 imm9 Rt' % (sf, cc),
              [sized(size, 'Rt'), sized(size, 'Rm'), branch('imm9')], feat='FEAT_CMPBR',
              flow='FLOW_COND')
        for bits, suffix in [('10', 'b'), ('11', 'h')]:
            E('cb' + suffix + name, '0 1110100 %s Rm %s imm9 Rt' % (cc, bits),
              [W('Rt'), W('Rm'), branch('imm9')], feat='FEAT_CMPBR', flow='FLOW_COND')
    for name, cc in imm_cc:
        for sf, size in [(0, 'w'), (1, 'x')]:
            E('cb' + name, '%d 1110101 %s imm6 0 imm9 Rt' % (sf, cc),
              [sized(size, 'Rt'), imm('imm6', hex=True), branch('imm9')], feat='FEAT_CMPBR',
              flow='FLOW_COND')


def exceptions():
    newer_dp()
    compare_branch()
    for mnemonic, opc, ll, feat in [('svc', '000', '01', 'FEAT_A64_BASE'),
                                    ('hvc', '000', '10', 'FEAT_A64_BASE'),
                                    ('smc', '000', '11', 'FEAT_A64_BASE'),
                                    ('brk', '001', '00', 'FEAT_A64_BASE'),
                                    ('hlt', '010', '00', 'FEAT_A64_BASE')]:
        E(mnemonic, '11010100 %s imm16 000 %s' % (opc, ll),
          [imm('imm16', hex=True, decimal_zero=True)],
          feat=feat, flow='FLOW_TRAP' if mnemonic in ('brk', 'hlt') else 0)
    for n, ll in [(1, '01'), (2, '10'), (3, '11')]:
        E('dcps%d' % n, '11010100 101 0000000000000000 000 %s' % ll, [])
        E('dcps%d' % n, '11010100 101 imm16 000 %s' % ll, [imm('imm16', hex=True)])
    E('udf', '0000000000000000 imm16', [imm('imm16', hex=True)])
