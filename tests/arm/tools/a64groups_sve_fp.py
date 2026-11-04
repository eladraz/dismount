"""SVE floating point instructions: arithmetic, multiply-add, reductions,
compares, conversions, rounding, the complex ones, and the indexed forms."""
from a64dsl_sve import *

FP = 'SIZE_22_WIDE'


def pg3(q='m'):
    return P('Pg', q=q, w=3)


def groups():
    arithmetic()
    multiply_add()
    reductions()
    compares()
    unary()
    conversions()
    narrowing_converts()
    complex_arithmetic()
    indexed()
    misc()


def arithmetic():
    ops = [Z('Zd', FP), Z('Zn', FP), Z('Zm', FP)]
    for mn, opc in [('fadd', '000'), ('fsub', '001'), ('fmul', '010'), ('ftsmul', '011'),
                    ('frecps', '110'), ('frsqrts', '111')]:
        E(mn, '01100101 size 0 Zm 000 %s Zn Zd' % opc, ops)
    for mn, opc in [('bfadd', '000'), ('bfsub', '001'), ('bfmul', '010')]:
        E(mn, '01100101 00 0 Zm 000 %s Zn Zd' % opc, [Z('Zd', 'H'), Z('Zn', 'H'), Z('Zm', 'H')],
          feat='B16B16')
    table = [('fadd', '0000'), ('fsub', '0001'), ('fmul', '0010'), ('fsubr', '0011'),
             ('fmaxnm', '0100'), ('fminnm', '0101'), ('fmax', '0110'), ('fmin', '0111'),
             ('fabd', '1000'), ('fscale', '1001'), ('fmulx', '1010'), ('fdivr', '1100'),
             ('fdiv', '1101'), ('famax', '1110'), ('famin', '1111')]
    for mn, opc in table:
        feat = 'FAMINMAX' if mn in ('famax', 'famin') else 'SVE'
        E(mn, '01100101 size 00 %s 100 Pg:3 Zm Zdn' % opc,
          [Z('Zdn', FP), pg3(), Z('Zdn', FP), Z('Zm', FP)], feat=feat)
    for mn, opc, feat in [('bfadd', '0000', 'B16B16'), ('bfsub', '0001', 'B16B16'),
                          ('bfmul', '0010', 'B16B16'), ('bfmaxnm', '0100', 'B16B16'),
                          ('bfminnm', '0101', 'B16B16'), ('bfmax', '0110', 'B16B16'),
                          ('bfmin', '0111', 'B16B16'), ('bfscale', '1001', 'BFSCALE')]:
        E(mn, '01100101 00 00 %s 100 Pg:3 Zm Zdn' % opc,
          [Z('Zdn', 'H'), pg3(), Z('Zdn', 'H'), Z('Zm', 'H')], feat=feat)
    # With the constants 0.5 / 1.0 / 2.0 / 0.0
    for mn, opc, imm in [('fadd', '000', 'FP_HALF_ONE'), ('fsub', '001', 'FP_HALF_ONE'),
                         ('fmul', '010', 'FP_HALF_TWO'), ('fsubr', '011', 'FP_HALF_ONE'),
                         ('fmaxnm', '100', 'FP_ZERO_ONE'), ('fminnm', '101', 'FP_ZERO_ONE'),
                         ('fmax', '110', 'FP_ZERO_ONE'), ('fmin', '111', 'FP_ZERO_ONE')]:
        E(mn, '01100101 size 011 %s 100 Pg:3 0000 i1 Zdn' % opc,
          [Z('Zdn', FP), pg3(), Z('Zdn', FP), IMM(imm)])
    # Pairwise (SVE2)
    for mn, opc in [('faddp', '000'), ('fmaxnmp', '100'), ('fminnmp', '101'), ('fmaxp', '110'),
                    ('fminp', '111')]:
        E(mn, '01100100 size 010 %s 100 Pg:3 Zm Zdn' % opc,
          [Z('Zdn', FP), pg3(), Z('Zdn', FP), Z('Zm', FP)], feat='SVE2')
    # ftmad
    E('ftmad', '01100101 size 010 imm3 100000 Zm Zdn',
      [Z('Zdn', FP), Z('Zdn', FP), Z('Zm', FP), IMM('UIMM3_16')])
    E('ftssel', '00000100 size 1 Zm 101100 Zn Zd', [Z('Zd', FP), Z('Zn', FP), Z('Zm', FP)])
    E('fexpa', '00000100 size 1 00000 101110 Zn Zd', [Z('Zd', FP), Z('Zn', FP)])


def multiply_add():
    ops = [Z('Zda', FP), pg3(), Z('Zn', FP), Z('Zm', FP)]
    for mn, opc in [('fmla', '000'), ('fmls', '001'), ('fnmla', '010'), ('fnmls', '011')]:
        E(mn, '01100101 size 1 Zm %s Pg:3 Zn Zda' % opc, ops)
    ops = [Z('Zdn', FP), pg3(), Z('Zm', FP), Z('Za', FP)]
    for mn, opc in [('fmad', '100'), ('fmsb', '101'), ('fnmad', '110'), ('fnmsb', '111')]:
        # the operand written first is the one at bit 5
        E(mn, '01100101 size 1 Za %s Pg:3 Zm Zdn' % opc, ops)
    for mn, opc in [('bfmla', '000'), ('bfmls', '001')]:
        E(mn, '01100101 00 1 Zm %s Pg:3 Zn Zda' % opc,
          [Z('Zda', 'H'), pg3(), Z('Zn', 'H'), Z('Zm', 'H')], feat='B16B16')


def reductions():
    for mn, opc in [('faddv', '00000'), ('fmaxnmv', '00100'), ('fminnmv', '00101'),
                    ('fmaxv', '00110'), ('fminv', '00111')]:
        E(mn, '01100101 size 0 %s 001 Pg:3 Zn Vd' % opc,
          [fpr('Vd', 'size'), P('Pg', w=3), Z('Zn', FP)])
    E('fadda', '01100101 size 0 11000 001 Pg:3 Zm Vdn',
      [fpr('Vdn', 'size'), P('Pg', w=3), fpr('Vdn', 'size'), Z('Zm', FP)])
    # SVE2.1: the quadword reductions
    for mn, opc in [('faddqv', '10000'), ('fmaxnmqv', '10100'), ('fminnmqv', '10101'),
                    ('fmaxqv', '10110'), ('fminqv', '10111')]:
        E(mn, '01100100 size 0 %s 101 Pg:3 Zn Vd' % opc,
          [QV('Vd', FP), P('Pg', w=3), Z('Zn', FP)], feat='SVE2P1')


def compares():
    for mn, opc, ne in [('fcmge', '010', '0'), ('fcmgt', '010', '1'), ('fcmeq', '011', '0'),
                        ('fcmne', '011', '1'), ('fcmuo', '110', '0'), ('facge', '110', '1'),
                        ('facgt', '111', '1')]:
        E(mn, '01100101 size 0 Zm %s Pg:3 Zn %s Pd:4' % (opc, ne),
          [P('Pd', FP), P('Pg', q='z', w=3), Z('Zn', FP), Z('Zm', FP)])
    for mn, opc, ne in [('fcmge', '0000', '0'), ('fcmgt', '0000', '1'), ('fcmlt', '0001', '0'),
                        ('fcmle', '0001', '1'), ('fcmeq', '0010', '0'), ('fcmne', '0011', '0')]:
        E(mn, '01100101 size 01 %s 001 Pg:3 Zn %s Pd:4' % (opc, ne),
          [P('Pd', FP), P('Pg', q='z', w=3), Z('Zn', FP), IMM('FP_ZERO')])


def unary():
    # frintn frintp frintm frintz frinta frintx frinti frecpx fsqrt, merging and (SVE2.2) zeroing
    table = [('frintn', '0000'), ('frintp', '0001'), ('frintm', '0010'), ('frintz', '0011'),
             ('frinta', '0100'), ('frintx', '0110'), ('frinti', '0111'), ('frecpx', '1100'),
             ('fsqrt', '1101')]
    for mn, opc in table:
        E(mn, '01100101 size 00 %s 101 Pg:3 Zn Zd' % opc,
          [Z('Zd', FP), pg3(), Z('Zn', FP)])
    E('frecpe', '01100101 size 00 1110 001100 Zn Zd', [Z('Zd', FP), Z('Zn', FP)])
    E('frsqrte', '01100101 size 00 1111 001100 Zn Zd', [Z('Zd', FP), Z('Zn', FP)])
    # frint32z... on 32 and 64 bit elements (FRINTTS)
    for mn, opc in [('frint32z', '0000'), ('frint32x', '0001'), ('frint64z', '0100'),
                    ('frint64x', '0101')]:
        for d, z in [('S', '0'), ('D', '1')]:
            op = opc[:2] + z + opc[3]
            E(mn, '01100101 00 01 %s 101 Pg:3 Zn Zd' % op, [Z('Zd', d), pg3(), Z('Zn', d)],
              feat='FRINTTS')
    # flogb (SVE2)
    for size, bits in [('H', '01'), ('S', '10'), ('D', '11')]:
        E('flogb', '01100101 00 01 1 %s 0 101 Pg:3 Zn Zd' % bits,
          [Z('Zd', size), pg3(), Z('Zn', size)], feat='SVE2')


def conversions():
    """fcvt, fcvtzs, fcvtzu, scvtf, ucvtf and the narrowing ones, predicated;
    the table rows are (size 23:22, bits 19:16, destination, source)"""
    sizes = {'H': 'H', 'S': 'S', 'D': 'D'}
    for mn, s, opc, dst, src in [
            ('fcvt', '10', '1000', 'H', 'S'), ('fcvt', '10', '1001', 'S', 'H'),
            ('fcvt', '11', '1000', 'H', 'D'), ('fcvt', '11', '1001', 'D', 'H'),
            ('fcvt', '11', '1010', 'S', 'D'), ('fcvt', '11', '1011', 'D', 'S'),
            ('bfcvt', '10', '1010', 'H', 'S')]:
        feat = 'BF16' if mn == 'bfcvt' else 'SVE'
        E(mn, '01100101 %s 00 %s 101 Pg:3 Zn Zd' % (s, opc),
          [Z('Zd', dst), pg3(), Z('Zn', src)], feat=feat)
    E('fcvtx', '01100101 00 00 1010 101 Pg:3 Zn Zd', [Z('Zd', 'S'), pg3(), Z('Zn', 'D')],
      feat='SVE2')
    for u, (smn, zmn) in [('0', ('scvtf', 'fcvtzs')), ('1', ('ucvtf', 'fcvtzu'))]:
        for s, opc, dst, src in [('01', '01', 'H', 'H'), ('01', '10', 'H', 'S'),
                                 ('01', '11', 'H', 'D'), ('10', '10', 'S', 'S'),
                                 ('11', '00', 'D', 'S'), ('11', '10', 'S', 'D'),
                                 ('11', '11', 'D', 'D')]:
            E(smn, '01100101 %s 01 0 %s %s 101 Pg:3 Zn Zd' % (s, opc, u),
              [Z('Zd', dst), pg3(), Z('Zn', src)])
        for s, opc, dst, src in [('01', '01', 'H', 'H'), ('01', '10', 'S', 'H'),
                                 ('01', '11', 'D', 'H'), ('10', '10', 'S', 'S'),
                                 ('11', '00', 'S', 'D'), ('11', '10', 'D', 'S'),
                                 ('11', '11', 'D', 'D')]:
            E(zmn, '01100101 %s 01 1 %s %s 101 Pg:3 Zn Zd' % (s, opc, u),
              [Z('Zd', dst), pg3(), Z('Zn', src)])


def narrowing_converts():
    """fcvtnt fcvtlt bfcvtnt fcvtxnt (SVE2), merging"""
    for key, mn, dst, src, feat in (('00001010101', 'fcvtxnt', 'S', 'D', 'SVE2'),
                                    ('10001000101', 'fcvtnt', 'H', 'S', 'SVE2'),
                                    ('10001001101', 'fcvtlt', 'S', 'H', 'SVE2'),
                                    ('10001010101', 'bfcvtnt', 'H', 'S', 'SVE2'),
                                    ('11001010101', 'fcvtnt', 'S', 'D', 'SVE2'),
                                    ('11001011101', 'fcvtlt', 'D', 'S', 'SVE2')):
        E(mn, '01100100 %s Pg:3 Zn Zd' % key,
          [Z('Zd', dst), pg3(), Z('Zn', src)], feat=feat)


def complex_arithmetic():
    E('fcadd', '01100100 size 00000 r 100 Pg:3 Zm Zdn',
      [Z('Zdn', FP), pg3(), Z('Zdn', FP), Z('Zm', FP), IMM('ROT_90_270_16')])
    E('fcmla', '01100100 size 0 Zm 0 rot:2 Pg:3 Zn Zda',
      [Z('Zda', FP), pg3(), Z('Zn', FP), Z('Zm', FP), IMM('ROT_13')])


def indexed():
    """fmla fmls fmul and fcmla by element"""
    for mn, opc in [('fmla', '000000'), ('fmls', '000001'), ('fmul', '001000')]:
        E(mn, '01100100 0 i3h:1 1 i3l:2 Zm:3 %s Zn Zda' % opc,
          [Z('Zda', 'H'), Z('Zn', 'H'), ZI('Zm', 'h', 'I3H')])
        E(mn, '01100100 10 1 i2:2 Zm:3 %s Zn Zda' % opc,
          [Z('Zda', 'S'), Z('Zn', 'S'), ZI('Zm', 's', 'I2')])
        E(mn, '01100100 11 1 i1:1 Zm:4 %s Zn Zda' % opc,
          [Z('Zda', 'D'), Z('Zn', 'D'), ZI('Zm', 'd', 'I1')])
    for mn, opc in [('bfmla', '000010'), ('bfmls', '000011'), ('bfmul', '001010')]:
        E(mn, '01100100 0 i3h:1 1 i3l:2 Zm:3 %s Zn Zda' % opc,
          [Z('Zda', 'H'), Z('Zn', 'H'), ZI('Zm', 'h', 'I3H')], feat='B16B16')
    E('fcmla', '01100100 10 1 i2:2 Zm:3 0001 rot:2 Zn Zda',
      [Z('Zda', 'H'), Z('Zn', 'H'), ZI('Zm', 'h', 'I2'), IMM('ROT_10')])
    E('fcmla', '01100100 11 1 i1:1 Zm:4 0001 rot:2 Zn Zda',
      [Z('Zda', 'S'), Z('Zn', 'S'), ZI('Zm', 's', 'I1'), IMM('ROT_10')])


def misc():
    pass
