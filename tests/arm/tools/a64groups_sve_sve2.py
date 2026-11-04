"""SVE2 and the later extensions: the halving, saturating and rounding integer
instructions, the widening, narrowing and complex ones, the bit permutes, the
crypto, histogram, match, the matrix multiplies, bfloat16, and the SVE2.1 /
SME2 instructions which live in the SVE encoding space."""
from a64dsl_sve import *

SZ = 'SIZE_22'
WIDE = 'SIZE_22_WIDE'
NARROW = 'SIZE_22_NARROW'


def pg3(q='m'):
    return P('Pg', q=q, w=3)


def groups():
    predicated()
    widening()
    narrowing()
    shifts()
    indexed_integer()
    complex_integer()
    bit_permute_and_crypto()
    dot_products_and_matrix()
    fp_widening()
    sve2_misc()


def predicated():
    """The predicated SVE2 integer instructions (merging; the zeroing ones of
    SVE2.2 for the unary ones)"""
    ops = [Z('Zdn', SZ), pg3(), Z('Zdn', SZ), Z('Zm', SZ)]
    for mn, opc in [('shadd', '0000'), ('uhadd', '0001'), ('shsub', '0010'), ('uhsub', '0011'),
                    ('srhadd', '0100'), ('urhadd', '0101'), ('shsubr', '0110'), ('uhsubr', '0111'),
                    ('sqadd', '1000'), ('uqadd', '1001'), ('sqsub', '1010'), ('uqsub', '1011'),
                    ('suqadd', '1100'), ('usqadd', '1101'), ('sqsubr', '1110'), ('uqsubr', '1111')]:
        E(mn, '01000100 size 01 %s 100 Pg:3 Zm Zdn' % opc, ops, feat='SVE2')
    for mn, opc in [('subp', '0000'), ('addp', '0001'), ('smaxp', '0100'), ('umaxp', '0101'),
                    ('sminp', '0110'), ('uminp', '0111')]:
        E(mn, '01000100 size 01 %s 101 Pg:3 Zm Zdn' % opc, ops, feat='SVE2')
    for mn, opc in [('srshl', '0010'), ('urshl', '0011'), ('srshlr', '0110'), ('urshlr', '0111'),
                    ('sqshl', '1000'), ('uqshl', '1001'), ('sqrshl', '1010'), ('uqrshl', '1011'),
                    ('sqshlr', '1100'), ('uqshlr', '1101'), ('sqrshlr', '1110'), ('uqrshlr', '1111')]:
        E(mn, '01000100 size 00 %s 100 Pg:3 Zm Zdn' % opc, ops, feat='SVE2')
    for mn, opc in [('sadalp', '0100'), ('uadalp', '0101')]:
        E(mn, '01000100 size 00 %s 101 Pg:3 Zn Zda' % opc,
          [Z('Zda', WIDE), pg3(), Z('Zn', NARROW)], feat='SVE2')
    for mn, opc in [('sqabs', '1000'), ('sqneg', '1001')]:
        E(mn, '01000100 size 00 %s 101 Pg:3 Zn Zd' % opc,
          [Z('Zd', SZ), pg3(), Z('Zn', SZ)], feat='SVE2')
        E(mn, '01000100 size 00 %s 101 Pg:3 Zn Zd' % format(int(opc, 2) + 2, '04b'),
          [Z('Zd', SZ), pg3('z'), Z('Zn', SZ)], feat='SVE2P2')
    for mn, opc in [('urecpe', '0000'), ('ursqrte', '0001')]:
        E(mn, '01000100 10 00 %s 101 Pg:3 Zn Zd' % opc, [Z('Zd', 'S'), pg3(), Z('Zn', 'S')],
          feat='SVE2')
        E(mn, '01000100 10 00 %s 101 Pg:3 Zn Zd' % format(int(opc, 2) + 2, '04b'),
          [Z('Zd', 'S'), pg3('z'), Z('Zn', 'S')], feat='SVE2P2')
    # Saturating shifts by immediate (these live in the SVE integer space)
    for mn, opc, imm in [('sqshl', '00110', 'SHIFT_PRED_LEFT'), ('uqshl', '00111', 'SHIFT_PRED_LEFT'),
                         ('srshr', '01100', 'SHIFT_PRED_RIGHT'), ('urshr', '01101', 'SHIFT_PRED_RIGHT'),
                         ('sqshlu', '01111', 'SHIFT_PRED_LEFT')]:
        E(mn, '00000100 tszh:2 0 %s 100 Pg:3 tszl:2 imm3 Zdn' % opc,
          [Z('Zdn', 'TSZ'), pg3(), Z('Zdn', 'TSZ'), IMM(imm)], feat='SVE2')
    # Saturating multiply high, unpredicated
    for mn, opc in [('sqdmulh', '011100'), ('sqrdmulh', '011101')]:
        E(mn, '00000100 size 1 Zm %s Zn Zd' % opc, [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)],
          feat='SVE2')
    # sqrdmlah, sqrdmlsh
    for mn, opc in [('sqrdmlah', '011100'), ('sqrdmlsh', '011101')]:
        E(mn, '01000100 size 0 Zm %s Zn Zda' % opc, [Z('Zda', SZ), Z('Zn', SZ), Z('Zm', SZ)],
          feat='SVE2')
    # saba, uaba
    for mn, opc in [('saba', '111110'), ('uaba', '111111')]:
        E(mn, '01000101 size 0 Zm %s Zn Zda' % opc, [Z('Zda', SZ), Z('Zn', SZ), Z('Zm', SZ)],
          feat='SVE2')
    # sclamp, uclamp
    for mn, opc in [('sclamp', '110000'), ('uclamp', '110001')]:
        E(mn, '01000100 size 0 Zm %s Zn Zd' % opc, [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)],
          feat='SVE2P1')
    E('fclamp', '01100100 size 1 Zm 001001 Zn Zd', [Z('Zd', WIDE), Z('Zn', WIDE), Z('Zm', WIDE)],
      feat='SVE2P1')
    E('bfclamp', '01100100 00 1 Zm 001001 Zn Zd', [Z('Zd', 'H'), Z('Zn', 'H'), Z('Zm', 'H')],
      feat='B16B16')
    # histcnt, histseg
    E('histcnt', '01000101 1 sz 1 Zm 110 Pg:3 Zn Zd',
      [Z('Zd', 'BIT_22_SD'), pg3('z'), Z('Zn', 'BIT_22_SD'), Z('Zm', 'BIT_22_SD')], feat='SVE2')
    E('histseg', '01000101 00 1 Zm 101000 Zn Zd', [Z('Zd', 'B'), Z('Zn', 'B'), Z('Zm', 'B')],
      feat='SVE2')
    # match, nmatch
    for mn, ne in [('match', '0'), ('nmatch', '1')]:
        E(mn, '01000101 0 sz 1 Zm 100 Pg:3 Zn %s Pd:4' % ne,
          [P('Pd', 'BIT_22_BH'), pg3('z'), Z('Zn', 'BIT_22_BH'), Z('Zm', 'BIT_22_BH')],
          feat='SVE2')
    # Pairwise floating point is in the fp group; pmul is in the int group
    # eorbt, eortb
    for mn, opc in [('eorbt', '100100'), ('eortb', '100101')]:
        E(mn, '01000101 size 0 Zm %s Zn Zd' % opc, [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)],
          feat='SVE2')
    # cadd, sqcadd
    for mn, sat in [('cadd', '0'), ('sqcadd', '1')]:
        E(mn, '01000101 size 00000 %s 11011 r Zm Zdn' % sat,
          [Z('Zdn', SZ), Z('Zdn', SZ), Z('Zm', SZ), IMM('ROT_90_270_10')], feat='SVE2')
    # adclb, adclt, sbclb, sbclt
    for mn, o, t in [('adclb', '0', '0'), ('adclt', '0', '1'), ('sbclb', '1', '0'), ('sbclt', '1', '1')]:
        E(mn, '01000101 %s sz 0 Zm 1101 0 %s Zn Zda' % (o, t),
          [Z('Zda', 'BIT_22_SD'), Z('Zn', 'BIT_22_SD'), Z('Zm', 'BIT_22_SD')], feat='SVE2')


def widening():
    """The long (b / t) instructions: the destination is twice as wide as the
    sources"""
    long3 = lambda: [Z('Zd', WIDE), Z('Zn', NARROW), Z('Zm', NARROW)]
    for mn, opc in [('saddlb', '000000'), ('saddlt', '000001'), ('uaddlb', '000010'),
                    ('uaddlt', '000011'), ('ssublb', '000100'), ('ssublt', '000101'),
                    ('usublb', '000110'), ('usublt', '000111'), ('sabdlb', '001100'),
                    ('sabdlt', '001101'), ('uabdlb', '001110'), ('uabdlt', '001111'),
                    ('sqdmullb', '011000'), ('sqdmullt', '011001'), ('smullb', '011100'),
                    ('smullt', '011101'), ('umullb', '011110'), ('umullt', '011111'),
                    ('saddlbt', '100000'), ('ssublbt', '100010'), ('ssubltb', '100011')]:
        E(mn, '01000101 size 0 Zm %s Zn Zd' % opc, long3(), feat='SVE2')
    # pmullb, pmullt: the sizes are special
    for mn, t in [('pmullb', '0'), ('pmullt', '1')]:
        for size, dst, src, feat in [('01', 'H', 'B', 'SVE2'), ('11', 'D', 'S', 'SVE2'),
                                     ('00', 'Q', 'D', 'AES')]:
            E(mn, '01000101 %s 0 Zm 011 01%s Zn Zd' % (size, t),
              [Z('Zd', dst), Z('Zn', src), Z('Zm', src)], feat=feat)
    # the wide forms: the first source has the destination size
    for mn, opc in [('saddwb', '010000'), ('saddwt', '010001'), ('uaddwb', '010010'),
                    ('uaddwt', '010011'), ('ssubwb', '010100'), ('ssubwt', '010101'),
                    ('usubwb', '010110'), ('usubwt', '010111')]:
        E(mn, '01000101 size 0 Zm %s Zn Zd' % opc, [Z('Zd', WIDE), Z('Zn', WIDE), Z('Zm', NARROW)],
          feat='SVE2')
    # accumulating long
    acc = lambda: [Z('Zda', WIDE), Z('Zn', NARROW), Z('Zm', NARROW)]
    for mn, opc in [('sabalb', '110000'), ('sabalt', '110001'), ('uabalb', '110010'),
                    ('uabalt', '110011')]:
        E(mn, '01000101 size 0 Zm %s Zn Zda' % opc, acc(), feat='SVE2')
    for mn, opc in [('smlalb', '010000'), ('smlalt', '010001'), ('umlalb', '010010'),
                    ('umlalt', '010011'), ('smlslb', '010100'), ('smlslt', '010101'),
                    ('umlslb', '010110'), ('umlslt', '010111'), ('sqdmlalb', '011000'),
                    ('sqdmlalt', '011001'), ('sqdmlslb', '011010'), ('sqdmlslt', '011011'),
                    ('sqdmlalbt', '000010'), ('sqdmlslbt', '000011')]:
        E(mn, '01000100 size 0 Zm %s Zn Zda' % opc, acc(), feat='SVE2')
    for mn, opc in [('sabal', '110101'), ('uabal', '110111')]:
        E(mn, '01000100 size 0 Zm %s Zn Zda' % opc, acc(), feat='SVE2P2')
    # addhnb... subhnt: the destination is half as wide
    for mn, opc in [('addhnb', '011000'), ('addhnt', '011001'), ('raddhnb', '011010'),
                    ('raddhnt', '011011'), ('subhnb', '011100'), ('subhnt', '011101'),
                    ('rsubhnb', '011110'), ('rsubhnt', '011111')]:
        E(mn, '01000101 size 1 Zm %s Zn Zd' % opc, [Z('Zd', NARROW), Z('Zn', WIDE), Z('Zm', WIDE)],
          feat='SVE2')


def narrowing():
    """sqxtnb... (one source), the shifts right narrow (tsz:imm3)"""
    for mn, opc in [('sqxtnb', '010000'), ('sqxtnt', '010001'), ('uqxtnb', '010010'),
                    ('uqxtnt', '010011'), ('sqxtunb', '010100'), ('sqxtunt', '010101')]:
        E(mn, '01000101 0 tszh:1 1 tszl:2 000 %s Zn Zd' % opc,
          [Z('Zd', 'XTN_DEST'), Z('Zn', 'XTN_SOURCE')], feat='SVE2')
    table = [('sqshrunb', '000000'), ('sqshrunt', '000001'), ('sqrshrunb', '000010'),
             ('sqrshrunt', '000011'), ('shrnb', '000100'), ('shrnt', '000101'),
             ('rshrnb', '000110'), ('rshrnt', '000111'), ('sqshrnb', '001000'),
             ('sqshrnt', '001001'), ('sqrshrnb', '001010'), ('sqrshrnt', '001011'),
             ('uqshrnb', '001100'), ('uqshrnt', '001101'), ('uqrshrnb', '001110'),
             ('uqrshrnt', '001111')]
    for mn, opc in table:
        E(mn, '01000101 0 tszh:1 1 tszl:2 imm3 %s Zn Zd' % opc,
          [Z('Zd', 'TSZ_NARROW'), Z('Zn', 'TSZ_NARROW_SOURCE'), IMM('SHIFT_NARROW')], feat='SVE2')


def shifts():
    for mn, opc in [('sshllb', '101000'), ('sshllt', '101001'), ('ushllb', '101010'),
                    ('ushllt', '101011')]:
        E(mn, '01000101 0 tszh:1 0 tszl:2 imm3 %s Zn Zd' % opc,
          [Z('Zd', 'TSZ_WIDEN_DEST'), Z('Zn', 'TSZ_WIDEN_SOURCE'), IMM('SHIFT_WIDEN')],
          feat='SVE2')
    for mn, opc, imm in [('sri', '111100', 'SHIFT_RIGHT'), ('sli', '111101', 'SHIFT_LEFT'),
                         ('ssra', '111000', 'SHIFT_RIGHT'), ('usra', '111001', 'SHIFT_RIGHT'),
                         ('srsra', '111010', 'SHIFT_RIGHT'), ('ursra', '111011', 'SHIFT_RIGHT')]:
        E(mn, '01000101 tszh:2 0 tszl:2 imm3 %s Zn Zd' % opc,
          [Z('Zd', 'TSZ_UNPRED'), Z('Zn', 'TSZ_UNPRED'), IMM(imm)], feat='SVE2')


def indexed_integer():
    """The by element forms: mla mls mul sqdmulh sqrdmulh sqrdmlah sqrdmlsh and
    the widening ones, the dot products"""
    for mn, opc in [('mla', '000010'), ('mls', '000011'), ('mul', '111110'),
                    ('sqdmulh', '111100'), ('sqrdmulh', '111101'),
                    ('sqrdmlah', '000100'), ('sqrdmlsh', '000101')]:
        dst = 'Zda' if mn in ('mla', 'mls', 'sqrdmlah', 'sqrdmlsh') else 'Zd'
        E(mn, '01000100 0 i3h:1 1 i3l:2 Zm:3 %s Zn %s' % (opc, dst),
          [Z(dst, 'H'), Z('Zn', 'H'), ZI('Zm', 'h', 'I3H')], feat='SVE2')
        E(mn, '01000100 10 1 i2:2 Zm:3 %s Zn %s' % (opc, dst),
          [Z(dst, 'S'), Z('Zn', 'S'), ZI('Zm', 's', 'I2')], feat='SVE2')
        E(mn, '01000100 11 1 i1:1 Zm:4 %s Zn %s' % (opc, dst),
          [Z(dst, 'D'), Z('Zn', 'D'), ZI('Zm', 'd', 'I1')], feat='SVE2')
    # the long ones: size 10 is .s from .h, 11 is .d from .s
    for mn, opc in [('smlalb', '1000'), ('smlalt', '1000'), ('umlalb', '1001'), ('umlalt', '1001'),
                    ('smlslb', '1010'), ('smlslt', '1010'), ('umlslb', '1011'), ('umlslt', '1011'),
                    ('sqdmlalb', '0010'), ('sqdmlalt', '0010'), ('sqdmlslb', '0011'),
                    ('sqdmlslt', '0011'), ('smullb', '1100'), ('smullt', '1100'),
                    ('umullb', '1101'), ('umullt', '1101'), ('sqdmullb', '1110'),
                    ('sqdmullt', '1110')]:
        t = '1' if mn.endswith('t') else '0'
        dst = 'Zd' if ('mull' in mn) else 'Zda'
        E(mn, '01000100 10 1 i3h:2 Zm:3 %s i3l:1 %s Zn %s' % (opc, t, dst),
          [Z(dst, 'S'), Z('Zn', 'H'), ZI('Zm', 'h', 'WIDE_I3')], feat='SVE2')
        E(mn, '01000100 11 1 i2h:1 Zm:4 %s i2l:1 %s Zn %s' % (opc, t, dst),
          [Z(dst, 'D'), Z('Zn', 'S'), ZI('Zm', 's', 'WIDE_I2')], feat='SVE2')


def complex_integer():
    for mn, opc in [('cmla', '0010'), ('sqrdcmlah', '0011')]:
        E(mn, '01000100 size 0 Zm %s rot:2 Zn Zda' % opc,
          [Z('Zda', SZ), Z('Zn', SZ), Z('Zm', SZ), IMM('ROT_10')], feat='SVE2')
        E(mn, '01000100 10 1 i2:2 Zm:3 01%s rot:2 Zn Zda' % ('10' if mn == 'cmla' else '11'),
          [Z('Zda', 'H'), Z('Zn', 'H'), ZI('Zm', 'h', 'I2'), IMM('ROT_10')], feat='SVE2')
        E(mn, '01000100 11 1 i1:1 Zm:4 01%s rot:2 Zn Zda' % ('10' if mn == 'cmla' else '11'),
          [Z('Zda', 'S'), Z('Zn', 'S'), ZI('Zm', 's', 'I1'), IMM('ROT_10')], feat='SVE2')
    E('cdot', '01000100 1 sz 0 Zm 0001 rot:2 Zn Zda',
      [Z('Zda', 'BIT_22_SD'), Z('Zn', 'BIT_22_BH'), Z('Zm', 'BIT_22_BH'), IMM('ROT_10')],
      feat='SVE2')
    E('cdot', '01000100 10 1 i2:2 Zm:3 0100 rot:2 Zn Zda',
      [Z('Zda', 'S'), Z('Zn', 'B'), ZI('Zm', 'b', 'I2'), IMM('ROT_10')], feat='SVE2')
    E('cdot', '01000100 11 1 i1:1 Zm:4 0100 rot:2 Zn Zda',
      [Z('Zda', 'D'), Z('Zn', 'H'), ZI('Zm', 'h', 'I1'), IMM('ROT_10')], feat='SVE2')


def bit_permute_and_crypto():
    for mn, opc in [('bext', '1011 00'), ('bdep', '1011 01'), ('bgrp', '1011 10')]:
        E(mn, '01000101 size 0 Zm %s Zn Zd' % opc, [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)],
          feat='BITPERM')
    E('aese', '01000101 00 10001 0 111000 Zm Zdn', [Z('Zdn', 'B'), Z('Zdn', 'B'), Z('Zm', 'B')],
      feat='AES')
    E('aesd', '01000101 00 10001 0 111001 Zm Zdn', [Z('Zdn', 'B'), Z('Zdn', 'B'), Z('Zm', 'B')],
      feat='AES')
    E('aesmc', '01000101 00 10000 0 111000 00000 Zdn', [Z('Zdn', 'B'), Z('Zdn', 'B')], feat='AES')
    E('aesimc', '01000101 00 10000 0 111001 00000 Zdn', [Z('Zdn', 'B'), Z('Zdn', 'B')], feat='AES')
    E('sm4e', '01000101 00 10001 1 111000 Zm Zdn', [Z('Zdn', 'S'), Z('Zdn', 'S'), Z('Zm', 'S')],
      feat='SM4')
    E('sm4ekey', '01000101 00 1 Zm 111100 Zn Zd', [Z('Zd', 'S'), Z('Zn', 'S'), Z('Zm', 'S')],
      feat='SM4')
    E('rax1', '01000101 00 1 Zm 111101 Zn Zd', [Z('Zd', 'D'), Z('Zn', 'D'), Z('Zm', 'D')],
      feat='SHA3')


def dot_products_and_matrix():
    # sdot, udot: 8 bit to 32 bit (sz = 0) and 16 bit to 64 bit (sz = 1)
    for mn, u in [('sdot', '0'), ('udot', '1')]:
        E(mn, '01000100 1 sz 0 Zm 00000 %s Zn Zda' % u,
          [Z('Zda', 'BIT_22_SD'), Z('Zn', 'BIT_22_BH'), Z('Zm', 'BIT_22_BH')])
        E(mn, '01000100 10 1 i2:2 Zm:3 00000 %s Zn Zda' % u,
          [Z('Zda', 'S'), Z('Zn', 'B'), ZI('Zm', 'b', 'I2')])
        E(mn, '01000100 11 1 i1:1 Zm:4 00000 %s Zn Zda' % u,
          [Z('Zda', 'D'), Z('Zn', 'H'), ZI('Zm', 'h', 'I1')])
    # I8MM
    E('usdot', '01000100 10 0 Zm 011110 Zn Zda', [Z('Zda', 'S'), Z('Zn', 'B'), Z('Zm', 'B')],
      feat='I8MM')
    E('usdot', '01000100 10 1 i2:2 Zm:3 000110 Zn Zda',
      [Z('Zda', 'S'), Z('Zn', 'B'), ZI('Zm', 'b', 'I2')], feat='I8MM')
    E('sudot', '01000100 10 1 i2:2 Zm:3 000111 Zn Zda',
      [Z('Zda', 'S'), Z('Zn', 'B'), ZI('Zm', 'b', 'I2')], feat='I8MM')
    for mn, size in [('smmla', '00'), ('usmmla', '10'), ('ummla', '11')]:
        E(mn, '01000101 %s 0 Zm 100110 Zn Zda' % size,
          [Z('Zda', 'S'), Z('Zn', 'B'), Z('Zm', 'B')], feat='I8MM')
    # BF16
    E('bfdot', '01100100 011 Zm 100000 Zn Zda', [Z('Zda', 'S'), Z('Zn', 'H'), Z('Zm', 'H')],
      feat='BF16')
    E('bfdot', '01100100 01 1 i2:2 Zm:3 010000 Zn Zda',
      [Z('Zda', 'S'), Z('Zn', 'H'), ZI('Zm', 'h', 'I2')], feat='BF16')
    E('bfmmla', '01100100 011 Zm 111001 Zn Zda', [Z('Zda', 'S'), Z('Zn', 'H'), Z('Zm', 'H')],
      feat='BF16')
    # F32MM / F64MM
    E('fmmla', '01100100 10 1 Zm 111001 Zn Zda', [Z('Zda', 'S'), Z('Zn', 'S'), Z('Zm', 'S')],
      feat='F32MM')
    E('fmmla', '01100100 11 1 Zm 111001 Zn Zda', [Z('Zda', 'D'), Z('Zn', 'D'), Z('Zm', 'D')],
      feat='F64MM')


def fp_widening():
    # bfmlalb... and fmlalb... (fp16 to fp32)
    for mn, opc in [('bfmlalb', '100000'), ('bfmlalt', '100001'), ('bfmlslb', '101000'),
                    ('bfmlslt', '101001')]:
        E(mn, '01100100 111 Zm %s Zn Zda' % opc, [Z('Zda', 'S'), Z('Zn', 'H'), Z('Zm', 'H')],
          feat='BF16')
        t = '1' if mn.endswith('t') else '0'
        o = '0100' if mn[2] == 'm' and mn[4] == 'l' and mn[5] == 'a' else '0110'
        E(mn, '01100100 111 i3h:2 Zm:3 %s i3l:1 %s Zn Zda' % ('0100' if 'la' in mn else '0110', t),
          [Z('Zda', 'S'), Z('Zn', 'H'), ZI('Zm', 'h', 'WIDE_I3')], feat='BF16')
    for mn, opc in [('fmlalb', '100000'), ('fmlalt', '100001'), ('fmlslb', '101000'),
                    ('fmlslt', '101001')]:
        E(mn, '01100100 101 Zm %s Zn Zda' % opc, [Z('Zda', 'S'), Z('Zn', 'H'), Z('Zm', 'H')],
          feat='SVE2')
        t = '1' if mn.endswith('t') else '0'
        E(mn, '01100100 101 i3h:2 Zm:3 %s i3l:1 %s Zn Zda' % ('0100' if 'la' in mn else '0110', t),
          [Z('Zda', 'S'), Z('Zn', 'H'), ZI('Zm', 'h', 'WIDE_I3')], feat='SVE2')


def sve2_misc():
    pass
