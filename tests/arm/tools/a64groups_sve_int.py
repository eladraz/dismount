"""SVE integer instructions: the predicated and unpredicated arithmetic, the
logical, shifts, multiply-add, reductions, the element count instructions,
index, the immediate forms (add, sub, dup, logical) and the permutes (dup, rev,
tbl, ext, splice, compact, sel, clast, last).

The bit patterns are written as in the Arm ARM (from bit 31); the names of the
fields in a pattern give the operands their positions."""
from a64dsl_sve import *

SZ = 'SIZE_22'


def pg3(q='m'):
    return P('Pg', q=q, w=3)


def groups():
    binary_predicated()
    reductions()
    unary_predicated()
    shifts_predicated()
    multiply_add()
    arithmetic_unpredicated()
    shifts_unpredicated()
    element_counts()
    index_and_vector_length()
    immediates()
    permutes()
    dup_and_select()


def binary_predicated():
    """add / sub / min / max / mul / div / logical, with a predicate (merging)"""
    ops = [Z('Zdn', SZ), pg3(), Z('Zdn', SZ), Z('Zm', SZ)]
    ops_sd = [Z('Zdn', 'SIZE_22_SD'), pg3(), Z('Zdn', 'SIZE_22_SD'), Z('Zm', 'SIZE_22_SD')]
    for mn, opc in [('add', '00000'), ('sub', '00001'), ('subr', '00011'),
                    ('smax', '01000'), ('umax', '01001'), ('smin', '01010'),
                    ('umin', '01011'), ('sabd', '01100'), ('uabd', '01101'),
                    ('mul', '10000'), ('smulh', '10010'), ('umulh', '10011'),
                    ('orr', '11000'), ('eor', '11001'), ('and', '11010'),
                    ('bic', '11011')]:
        E(mn, '00000100 size 0 %s 000 Pg:3 Zm Zdn' % opc, ops)
    for mn, opc in [('sdiv', '10100'), ('udiv', '10101'), ('sdivr', '10110'),
                    ('udivr', '10111')]:
        E(mn, '00000100 size 0 %s 000 Pg:3 Zm Zdn' % opc, ops_sd)
    # CPA: addpt / subpt (64 bit elements)
    for mn, opc in [('addpt', '00100'), ('subpt', '00101')]:
        E(mn, '00000100 11 0 %s 000 Pg:3 Zm Zdn' % opc,
          [Z('Zdn', 'D'), pg3(), Z('Zdn', 'D'), Z('Zm', 'D')], feat='CPA')


def reductions():
    """saddv... and the other horizontal reductions to a scalar register"""
    for mn, opc in [('uaddv', '00001'), ('smaxv', '01000'), ('umaxv', '01001'),
                    ('sminv', '01010'), ('uminv', '01011'), ('orv', '11000'),
                    ('eorv', '11001'), ('andv', '11010')]:
        scalar = fpr('Vd', 'd') if mn == 'uaddv' else fpr('Vd', 'size')
        E(mn, '00000100 size 0 %s 001 Pg:3 Zn Vd' % opc,
          [scalar, P('Pg', w=3), Z('Zn', SZ)])
    E('saddv', '00000100 size 0 00000 001 Pg:3 Zn Vd',
      [fpr('Vd', 'd'), P('Pg', w=3), Z('Zn', 'SIZE_22_BHS')])
    # movprfx, predicated
    for q, opc in [('z', '10000'), ('m', '10001')]:
        E('movprfx', '00000100 size 0 %s 001 Pg:3 Zn Zd' % opc,
          [Z('Zd', SZ), pg3(q), Z('Zn', SZ)])
    # SVE2.1: the quadword reductions
    for mn, opc in [('addqv', '00101'), ('smaxqv', '01100'), ('umaxqv', '01101'),
                    ('sminqv', '01110'), ('uminqv', '01111'), ('orqv', '11100'),
                    ('eorqv', '11101'), ('andqv', '11110')]:
        E(mn, '00000100 size 0 %s 001 Pg:3 Zn Vd' % opc,
          [QV('Vd', SZ), P('Pg', w=3), Z('Zn', SZ)], feat='SVE2P1')


def unary_predicated():
    """sxtb... abs neg cls clz cnt cnot fabs fneg not (merging; the zeroing
    forms are SVE2.2)"""
    table = [('sxtb', '0000', 'SIZE_22_WIDE'), ('uxtb', '0001', 'SIZE_22_WIDE'),
             ('sxth', '0010', 'SIZE_22_SD'), ('uxth', '0011', 'SIZE_22_SD'),
             ('sxtw', '0100', 'D'), ('uxtw', '0101', 'D'),
             ('abs', '0110', SZ), ('neg', '0111', SZ),
             ('cls', '1000', SZ), ('clz', '1001', SZ),
             ('cnt', '1010', SZ), ('cnot', '1011', SZ),
             ('fabs', '1100', 'SIZE_22_WIDE'), ('fneg', '1101', 'SIZE_22_WIDE'),
             ('not', '1110', SZ)]
    for mn, opc, size in table:
        for merging, feat in [(1, 'SVE'), (0, 'SVE2P2')]:
            q = 'm' if merging else 'z'
            pattern = '00000100 size 0 %d %s 101 Pg:3 Zn Zd' % (merging, opc)
            if size == 'D':
                pattern = pattern.replace('size', '11')
            E(mn, pattern, [Z('Zd', size), pg3(q), Z('Zn', size)], feat=feat)


def shifts_predicated():
    """Shift by immediate, by vector and by wide element (predicated)"""
    for mn, opc, imm in [('asr', '00000', 'SHIFT_PRED_RIGHT'), ('lsr', '00001', 'SHIFT_PRED_RIGHT'),
                         ('lsl', '00011', 'SHIFT_PRED_LEFT'), ('asrd', '00100', 'SHIFT_PRED_RIGHT')]:
        E(mn, '00000100 tszh:2 0 %s 100 Pg:3 tszl:2 imm3 Zdn' % opc,
          [Z('Zdn', 'TSZ'), pg3(), Z('Zdn', 'TSZ'), IMM(imm)])
    ops = [Z('Zdn', SZ), pg3(), Z('Zdn', SZ), Z('Zm', SZ)]
    for mn, opc in [('asr', '10000'), ('lsr', '10001'), ('lsl', '10011'), ('asrr', '10100'),
                    ('lsrr', '10101'), ('lslr', '10111')]:
        E(mn, '00000100 size 0 %s 100 Pg:3 Zm Zdn' % opc, ops)
    for mn, opc in [('asr', '11000'), ('lsr', '11001'), ('lsl', '11011')]:
        E(mn, '00000100 size 0 %s 100 Pg:3 Zm Zdn' % opc,
          [Z('Zdn', 'SIZE_22_BHS'), pg3(), Z('Zdn', 'SIZE_22_BHS'), Z('Zm', 'D')])


def multiply_add():
    """mla mls mad msb, predicated"""
    ops = [Z('Zda', SZ), pg3(), Z('Zn', SZ), Z('Zm', SZ)]
    E('mla', '00000100 size 0 Zm 010 Pg:3 Zn Zda', ops)
    E('mls', '00000100 size 0 Zm 011 Pg:3 Zn Zda', ops)
    ops = [Z('Zdn', SZ), pg3(), Z('Zm', SZ), Z('Za', SZ)]
    E('mad', '00000100 size 0 Zm 110 Pg:3 Za Zdn', ops)
    E('msb', '00000100 size 0 Zm 111 Pg:3 Za Zdn', ops)


def arithmetic_unpredicated():
    ops = [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)]
    for mn, opc in [('add', '000'), ('sub', '001'), ('sqadd', '100'), ('uqadd', '101'),
                    ('sqsub', '110'), ('uqsub', '111')]:
        E(mn, '00000100 size 1 Zm 000 %s Zn Zd' % opc, ops)
    # CPA: addpt / subpt
    for mn, opc in [('addpt', '000010'), ('subpt', '000011')]:
        E(mn, '00000100 11 1 Zm %s Zn Zd' % opc, [Z('Zd', 'D'), Z('Zn', 'D'), Z('Zm', 'D')],
          feat='CPA')
    E('madpt', '01000100 11 0 Zm 110110 Za Zdn', [Z('Zdn', 'D'), Z('Zm', 'D'), Z('Za', 'D')],
      feat='CPA')
    E('mlapt', '01000100 11 0 Zm 110100 Zn Zda', [Z('Zda', 'D'), Z('Zn', 'D'), Z('Zm', 'D')],
      feat='CPA')
    # The bitwise operations: the size field is the operation. mov is the
    # alias of orr with the same operands
    E('mov', '00000100 01 1 Zm 001100 Zn Zd', [Z('Zd', 'D'), Z('Zn', 'D'), TIED('Zm', 'Zn', 5)],
      alias=True, cond='$Zn == $Zm')
    for mn, opc in [('and', '00'), ('orr', '01'), ('eor', '10'), ('bic', '11')]:
        E(mn, '00000100 %s 1 Zm 001100 Zn Zd' % opc, [Z('Zd', 'D'), Z('Zn', 'D'), Z('Zm', 'D')])
    # Ternary logical operations (SVE2)
    four = [Z('Zdn', 'D'), Z('Zdn', 'D'), Z('Zm', 'D'), Z('Zk', 'D')]
    E('eor3', '00000100 00 1 Zm 001110 Zk Zdn', four, feat='SVE2')
    E('bcax', '00000100 01 1 Zm 001110 Zk Zdn', four, feat='SVE2')
    for mn, opc in [('bsl', '00'), ('bsl1n', '01'), ('bsl2n', '10'), ('nbsl', '11')]:
        E(mn, '00000100 %s 1 Zm 001111 Zk Zdn' % opc, four, feat='SVE2')
    # mul, smulh, umulh, pmul (SVE2)
    for mn, opc, size in [('mul', '011000', SZ), ('pmul', '011001', 'B'),
                          ('smulh', '011010', SZ), ('umulh', '011011', SZ)]:
        E(mn, ('00000100 size 1 Zm %s Zn Zd' % opc) if size != 'B' else
          ('00000100 00 1 Zm %s Zn Zd' % opc),
          [Z('Zd', size), Z('Zn', size), Z('Zm', size)], feat='SVE2')
    # xar (SVE2)
    E('xar', '00000100 tszh:2 1 tszl:2 imm3 001101 Zm Zdn',
      [Z('Zdn', 'TSZ_UNPRED'), Z('Zdn', 'TSZ_UNPRED'), Z('Zm', 'TSZ_UNPRED'),
       IMM('SHIFT_RIGHT')], feat='SVE2')


def shifts_unpredicated():
    for mn, opc, imm in [('asr', '100100', 'SHIFT_RIGHT'), ('lsr', '100101', 'SHIFT_RIGHT'),
                         ('lsl', '100111', 'SHIFT_LEFT')]:
        E(mn, '00000100 tszh:2 1 tszl:2 imm3 %s Zn Zd' % opc,
          [Z('Zd', 'TSZ_UNPRED'), Z('Zn', 'TSZ_UNPRED'), IMM(imm)])
    for mn, opc in [('asr', '100000'), ('lsr', '100001'), ('lsl', '100011')]:
        E(mn, '00000100 size 1 Zm %s Zn Zd' % opc,
          [Z('Zd', 'SIZE_22_BHS'), Z('Zn', 'SIZE_22_BHS'), Z('Zm', 'D')])


SIZES = [(0, 'b', 'B'), (1, 'h', 'H'), (2, 'w', 'S'), (3, 'd', 'D')]


def element_counts():
    """cntb... incb... decb..., sqincb..., on scalars and vectors; with the
    aliases which drop the pattern (all) and the multiplier (1)"""
    for size, suffix, zsize in SIZES:
        sz = format(size, '02b')
        count_forms('cnt' + suffix, sz, '10', '111000', [X('Rd')], 'Rd')
        count_forms('inc' + suffix, sz, '11', '111000', [X('Rd'), X('Rd')][:1], 'Rd')
        count_forms('dec' + suffix, sz, '11', '111001', [X('Rd')], 'Rd')
        if size > 0:
            count_forms('inc' + suffix, sz, '11', '110000', [Z('Zdn', zsize)], 'Zdn')
            count_forms('dec' + suffix, sz, '11', '110001', [Z('Zdn', zsize)], 'Zdn')
        for mn, vec, scalar in [('sqinc', '110000', '111100'), ('uqinc', '110001', '111101'),
                                ('sqdec', '110010', '111110'), ('uqdec', '110011', '111111')]:
            if size > 0:
                count_forms(mn + suffix, sz, '10', vec, [Z('Zdn', zsize)], 'Zdn')
            signed = mn.startswith('sq')
            count_forms(mn + suffix, sz, '11', scalar, [X('Rdn')], 'Rdn')
            count_forms(mn + suffix, sz, '10', scalar,
                        [X('Rdn'), W('Rdn')] if signed else [W('Rdn')], 'Rdn')


def count_forms(mnemonic, sz, mode, opc, regs, field):
    """The three forms of an element count instruction: the register alone
    (pattern all, multiplier 1), with the pattern, with the pattern and the
    multiplier."""
    full = '00000100 %s %s imm4 %s pattern %s' % (sz, mode, opc, field)
    pat = '00000100 %s %s 0000 %s pattern %s' % (sz, mode, opc, field)
    none = '00000100 %s %s 0000 %s 11111 %s' % (sz, mode, opc, field)
    E(mnemonic, none, list(regs), alias=True)
    E(mnemonic, pat, list(regs) + [OPT('PATTERN', 'pattern')], alias=True)
    E(mnemonic, full, list(regs) + [OPT('PATTERN', 'pattern'), IMM('MUL')])


def index_and_vector_length():
    # index zd.T, first, step: [11:10] says which operands are registers
    for kind, first, second in [('00', 'IMM', 'IMM'), ('01', 'REG', 'IMM'),
                                ('10', 'IMM', 'REG'), ('11', 'REG', 'REG')]:
        ops = [Z('Zd', SZ)]
        ops.append(IMM('INDEX5') if first == 'IMM' else GS('Rn'))
        ops.append(IMM('INDEX5_16') if second == 'IMM' else GS('Rm'))
        E('index', '00000100 size 1 Rm 0100 %s Rn Zd' % kind, ops)
    # addvl / addpl / rdvl, and the streaming ones
    E('addvl', '00000100 001 Rn 01010 imm6 Rd', [XS('Rd'), XS('Rn'), IMM('SIMM6')])
    E('addpl', '00000100 011 Rn 01010 imm6 Rd', [XS('Rd'), XS('Rn'), IMM('SIMM6')])
    E('rdvl', '00000100 10111111 01010 imm6 Rd', [X('Rd'), IMM('SIMM6')])
    E('addsvl', '00000100 001 Rn 01011 imm6 Rd', [XS('Rd'), XS('Rn'), IMM('SIMM6')], feat='SME')
    E('addspl', '00000100 011 Rn 01011 imm6 Rd', [XS('Rd'), XS('Rn'), IMM('SIMM6')], feat='SME')
    E('rdsvl', '00000100 10111111 01011 imm6 Rd', [X('Rd'), IMM('SIMM6')], feat='SME')


def immediates():
    """add / sub / ... #imm, dup / cpy #imm, and / orr / eor / dupm #bitmask"""
    for mn, opc in [('add', '000'), ('sub', '001'), ('subr', '011'), ('sqadd', '100'),
                    ('uqadd', '101'), ('sqsub', '110'), ('uqsub', '111')]:
        E(mn, '00100101 size 100 %s 11 sh imm8 Zdn' % opc,
          [Z('Zdn', SZ), Z('Zdn', SZ), IMM('ADDSUB')])
    for mn, opc, imm in [('smax', '000', 'SIGNED8'), ('umax', '001', 'UNSIGNED8'),
                         ('smin', '010', 'SIGNED8'), ('umin', '011', 'UNSIGNED8')]:
        E(mn, '00100101 size 101 %s 110 imm8 Zdn' % opc,
          [Z('Zdn', SZ), Z('Zdn', SZ), IMM(imm)])
    E('mul', '00100101 size 110 000 110 imm8 Zdn', [Z('Zdn', SZ), Z('Zdn', SZ), IMM('SIGNED8')])
    # mov zd.T, #imm (dup, cpy), and the zeroing / merging forms
    E('mov', '00100101 size 111 000 11 sh imm8 Zd', [Z('Zd', SZ), IMM('DUP')])
    for m, q in [(1, 'm'), (0, 'z')]:
        E('mov', '00000101 size 01 Pg:4 0 %d sh imm8 Zd' % m,
          [Z('Zd', SZ), P('Pg', q=q), IMM('DUP')])
    # fmov zd.T, #imm8 (fdup) and fmov zd.T, pg/m, #imm8 (fcpy)
    E('fmov', '00100101 size 111 001 110 imm8 Zd', [Z('Zd', 'SIZE_22_WIDE'), IMM('FP8')])
    E('fmov', '00000101 size 01 Pg:4 110 imm8 Zd',
      [Z('Zd', 'SIZE_22_WIDE'), P('Pg', q='m'), IMM('FP8')])
    # The bitmask immediates
    for mn, opc in [('orr', '00'), ('eor', '01'), ('and', '10')]:
        E(mn, '00000101 %s 0000 imm13:13 Zdn' % opc,
          [Z('Zdn', 'LOGICAL_IMM'), Z('Zdn', 'LOGICAL_IMM'), IMM('LOGICAL')])
    E('mov', '00000101 11 0000 imm13:13 Zd', [Z('Zd', 'LOGICAL_IMM'), IMM('LOGICAL')],
      alias=True, cond='isDupmMoveAlias(e)')
    E('dupm', '00000101 11 0000 imm13:13 Zd', [Z('Zd', 'LOGICAL_IMM'), IMM('LOGICAL')])


def permutes():
    """The vector permutes: rev, unpack, tbl, ext, zip / uzp / trn, splice,
    compact, revb... clast / last"""
    E('insr', '00000101 size 1 00100 001110 Rm Zdn', [Z('Zdn', SZ), GS('Rm')])
    E('insr', '00000101 size 1 10100 001110 Vm Zdn', [Z('Zdn', SZ), fpr('Vm', 'size')])
    E('rev', '00000101 size 1 11000 001110 Zn Zd', [Z('Zd', SZ), Z('Zn', SZ)])
    for mn, opc in [('sunpklo', '10000'), ('sunpkhi', '10001'), ('uunpklo', '10010'),
                    ('uunpkhi', '10011')]:
        E(mn, '00000101 size 1 %s 001110 Zn Zd' % opc,
          [Z('Zd', 'SIZE_22_WIDE'), Z('Zn', 'SIZE_22_NARROW')])
    # tbl, tbx
    E('tbl', '00000101 size 1 Zm 001100 Zn Zd',
      [Z('Zd', SZ), ZL('Zn', 1, SZ), Z('Zm', SZ)])
    E('tbl', '00000101 size 1 Zm 001010 Zn Zd',
      [Z('Zd', SZ), ZL('Zn', 2, SZ), Z('Zm', SZ)], feat='SVE2')
    E('tbx', '00000101 size 1 Zm 001011 Zn Zd', [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)],
      feat='SVE2')
    # zip, uzp, trn (vectors)
    for mn, opc in [('zip1', '000'), ('zip2', '001'), ('uzp1', '010'), ('uzp2', '011'),
                    ('trn1', '100'), ('trn2', '101')]:
        E(mn, '00000101 size 1 Zm 011 %s Zn Zd' % opc, [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)])
    # F64MM: the quadword versions
    for mn, opc in [('zip1', '000'), ('zip2', '001'), ('uzp1', '010'), ('uzp2', '011'),
                    ('trn1', '110'), ('trn2', '111')]:
        E(mn, '00000101 10 1 Zm 000 %s Zn Zd' % opc, [Z('Zd', 'Q'), Z('Zn', 'Q'), Z('Zm', 'Q')],
          feat='F64MM')
    # ext
    E('ext', '00000101 001 imm8h:5 000 imm8l:3 Zm Zdn',
      [Z('Zdn', 'B'), Z('Zdn', 'B'), Z('Zm', 'B'), IMM('EXT')])
    E('ext', '00000101 011 imm8h:5 000 imm8l:3 Zn Zd',
      [Z('Zd', 'B'), ZL('Zn', 2, 'B'), IMM('EXT')], feat='SVE2')
    # splice, compact, expand
    E('splice', '00000101 size 101100 100 Pg:3 Zm Zdn',
      [Z('Zdn', SZ), P('Pg', w=3), Z('Zdn', SZ), Z('Zm', SZ)])
    E('splice', '00000101 size 101101 100 Pg:3 Zn Zd',
      [Z('Zd', SZ), P('Pg', w=3), ZL('Zn', 2, SZ)], feat='SVE2')
    E('compact', '00000101 1 sz 100001 100 Pg:3 Zn Zd',
      [Z('Zd', 'SZ_22'), P('Pg', w=3), Z('Zn', 'SZ_22')])
    E('compact', '00000101 0 sz 100001 100 Pg:3 Zn Zd',
      [Z('Zd', 'BIT_22_BH'), P('Pg', w=3), Z('Zn', 'BIT_22_BH')], feat='SVE2P2')
    E('expand', '00000101 size 110001 100 Pg:3 Zn Zd',
      [Z('Zd', SZ), P('Pg', w=3), Z('Zn', SZ)], feat='SVE2P2')
    # revb, revh, revw, rbit: merging (SVE) and zeroing (SVE2.2)
    for mn, opc, size in [('revb', '00100', 'SIZE_22_WIDE'), ('revh', '00101', 'SIZE_22_SD'),
                          ('revw', '00110', 'D'), ('rbit', '00111', SZ)]:
        for m, feat, q in [(0, 'SVE', 'm'), (1, 'SVE2P2', 'z')]:
            pattern = '00000101 size 1 %s 10%d Pg:3 Zn Zd' % (opc, m)
            if size == 'D':
                pattern = pattern.replace('size', '11')
            E(mn, pattern, [Z('Zd', size), pg3(q), Z('Zn', size)], feat=feat)
    # revd (SVE2.1), predicated
    E('revd', '00000101 00 1011 10 100 Pg:3 Zn Zd',
      [Z('Zd', 'Q'), pg3('m'), Z('Zn', 'Q')], feat='SVE2P1')
    E('revd', '00000101 00 1011 10 101 Pg:3 Zn Zd',
      [Z('Zd', 'Q'), pg3('z'), Z('Zn', 'Q')], feat='SVE2P2')
    # clasta, clastb: vector, SIMD&FP scalar, general scalar
    for mn, b in [('clasta', '0'), ('clastb', '1')]:
        E(mn, '00000101 size 1010 0%s 100 Pg:3 Zm Zdn' % b,
          [Z('Zdn', SZ), P('Pg', w=3), Z('Zdn', SZ), Z('Zm', SZ)])
        E(mn, '00000101 size 1010 1%s 100 Pg:3 Zm Vdn' % b,
          [fpr('Vdn', 'size'), P('Pg', w=3), fpr('Vdn', 'size'), Z('Zm', SZ)])
        E(mn, '00000101 size 1100 0%s 101 Pg:3 Zm Rdn' % b,
          [GS('Rdn'), P('Pg', w=3), GS('Rdn'), Z('Zm', SZ)])
    for mn, b in [('lasta', '0'), ('lastb', '1')]:
        E(mn, '00000101 size 1000 1%s 100 Pg:3 Zn Vd' % b,
          [fpr('Vd', 'size'), P('Pg', w=3), Z('Zn', SZ)])
        E(mn, '00000101 size 1000 0%s 101 Pg:3 Zn Rd' % b,
          [GS('Rd'), P('Pg', w=3), Z('Zn', SZ)])


def dup_and_select():
    """dup / mov (printed mov), cpy, sel"""
    # dup z.T, Rn (mov z.T, Rn)
    E('mov', '00000101 size 1 00000 001110 Rn Zd', [Z('Zd', SZ), GS('Rn', sp=True)])
    # cpy z.T, p/m, Rn and Vn
    E('mov', '00000101 size 101000 101 Pg:3 Rn Zd',
      [Z('Zd', SZ), pg3(), GS('Rn', sp=True)])
    E('mov', '00000101 size 100000 100 Pg:3 Vn Zd',
      [Z('Zd', SZ), pg3(), fpr('Vn', 'size')])
    # dup z.T, z.T[0] is mov z.T, V (an alias), the others mov z.T, z.T[i]
    for size, tsz, vreg in [('B', '00001', 'b'), ('H', '00010', 'h'), ('S', '00100', 's'),
                            ('D', '01000', 'd'), ('Q', '10000', 'q')]:
        E('mov', '00000101 00 1 %s 001000 Zn Zd' % tsz, [Z('Zd', size), fpr('Zn', vreg)],
          alias=True)
    E('mov', '00000101 imm2:2 1 tsz:5 001000 Zn Zd',
      [Z('Zd', 'DUP_TSZ'), ZI('Zn', 'none', 'DUP')])
    # sel z, and its mov alias
    E('mov', '00000101 size 1 Zm 11 Pv:4 Zn Zd',
      [Z('Zd', SZ), P('Pv', q='m'), Z('Zn', SZ), TIED('Zm', 'Zd', 5)], alias=True,
      cond='$Zm == $Zd')
    E('sel', '00000101 size 1 Zm 11 Pv:4 Zn Zd',
      [Z('Zd', SZ), P('Pv'), Z('Zn', SZ), Z('Zm', SZ)])
    # movprfx z, z
    E('movprfx', '00000100 00 1 00000 101111 Zn Zd', [Z('Zd', 'NONE'), Z('Zn', 'NONE')])
