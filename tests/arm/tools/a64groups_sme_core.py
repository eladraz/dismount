"""SME (ARMv9.2) and the SME2.1 moves: the loads and stores of the za tiles and
array, mova / movaz between the tiles, the array and the z registers, zero,
addha / addva and the outer products."""
from a64dsl_sme import *
import a64dsl_sve as sve

SZ = 'SIZE_22'
SIZE_LETTERS = 'bhsdq'


def groups():
    loads_and_stores()
    mova_single()
    mova_multi()
    movt()
    zero()
    add_horizontal()
    outer_products()


def pn3(q='m'):
    return P('Pn', q=q, w=3)


def pm3(q='m'):
    return P('Pm', q=q, w=3)


def loads_and_stores():
    """ld1b..ld1q / st1b..st1q of a slice of a tile, ldr / str of an array
    vector, ldr / str zt0"""
    for size, name in enumerate('bhwdq'):
        letter = SIZE_LETTERS[size]
        for store in (0, 1):
            mn = ('st1' if store else 'ld1') + name
            E(mn, '1110000%d %s %d Rm:5 V:1 Rs:2 Pg:3 Rn:5 0 ZAt:4' % (
                  1 if size == 4 else 0, format(3 if size == 4 else size, '02b'), store),
              [SLICE('ZAt', letter, braces=True), P('Pg', q=None if store else 'z', w=3),
               ZAMEM('SCALAR_OPTIONAL', 'Rn', size)], feat='SME')
    for store in (0, 1):
        mn = 'str' if store else 'ldr'
        E(mn, '11100001 00 %d 00000 0 Rv:2 000 Rn:5 0 imm4:4' % store,
          [ARRAY('imm4', 'none', 0, 1, 4, w12=True), ZAMEM('ZA')], feat='SME')
        E(mn, '11100001 00 %d 11111 1 00000 Rn:5 00000' % store,
          [ZT0('PLAIN'), ZAMEM('BASE')], feat='SME2')


def mova_single():
    """mova between a tile slice and a z register; movaz (SME2.1) reads the
    slice and zeroes it"""
    ZSZ = 'SIZE_MOVA'
    # mova za0h.b[w12, 0], p0/m, z0.b: LLVM prints "mov", the assembler takes both
    for alias in (False, True):
        mn = 'mova' if alias else 'mov'
        kw = {'assemble_only': True} if alias else {}
        E(mn, '11000000 sz:2 00000 Q:1 V:1 Rs:2 Pg:3 Zn:5 0 ZAd:4',
          [SLICE('ZAd', 'field'), P('Pg', q='m', w=3), Z('Zn', ZSZ)], feat='SME', **kw)
        E(mn, '11000000 sz:2 0000 1 Q:1 V:1 Rs:2 Pg:3 0 ZAn:4 Zd:5',
          [Z('Zd', ZSZ), P('Pg', q='m', w=3), SLICE('ZAn', 'field')], feat='SME', **kw)
    E('movaz', '11000000 sz:2 0000 1 Q:1 V:1 Rs:2 000 1 ZAn:4 Zd:5',
      [Z('Zd', ZSZ), SLICE('ZAn', 'field')], feat='SME2P1')


def mova_multi():
    """The two and four register forms of mova / movaz (SME2, SME2.1)"""
    for alias in (False, True):
        mn = 'mova' if alias else 'mov'
        kw = {'assemble_only': True} if alias else {}
        # tile slices <- 2 / 4 registers
        E(mn, '11000000 sz:2 00 0100 V:1 Rs:2 0 00 Zn:4 000 f:3',
          [SLICE('f', 'field', 2), ZL('Zn', 2, SZ, 2)], feat='SME2', **kw)
        E(mn, '11000000 sz:2 00 0100 V:1 Rs:2 0 01 Zn:3 0000 f:3',
          [SLICE('f', 'field', 4), ZL('Zn', 4, SZ, 4)], feat='SME2', **kw)
        # 2 / 4 registers <- tile slices
        E(mn, '11000000 sz:2 00 0110 V:1 Rs:2 0 00 0 0 f:3 Zd:4 0',
          [ZL('Zd', 2, SZ, 2), SLICE('f', 'field', 2)], feat='SME2', **kw)
        E(mn, '11000000 sz:2 00 0110 V:1 Rs:2 0 01 0 0 f:3 Zd:3 00',
          [ZL('Zd', 4, SZ, 4), SLICE('f', 'field', 4)], feat='SME2', **kw)
        # the array vectors <- registers
        E(mn, '11000000 00 00 0100 0 Rv:2 0 10 Zn:4 000 off:3',
          [ARRAY('off', 'd', 2, optional_group=True), ZL('Zn', 2, 'D', 2)], feat='SME2', **kw)
        E(mn, '11000000 00 00 0100 0 Rv:2 0 11 Zn:3 0000 off:3',
          [ARRAY('off', 'd', 4, optional_group=True), ZL('Zn', 4, 'D', 4)], feat='SME2', **kw)
        E(mn, '11000000 00 00 0110 0 Rv:2 0 10 0 0 off:3 Zd:4 0',
          [ZL('Zd', 2, 'D', 2), ARRAY('off', 'd', 2, optional_group=True)], feat='SME2', **kw)
        E(mn, '11000000 00 00 0110 0 Rv:2 0 11 0 0 off:3 Zd:3 00',
          [ZL('Zd', 4, 'D', 4), ARRAY('off', 'd', 4, optional_group=True)], feat='SME2', **kw)
    # movaz
    E('movaz', '11000000 sz:2 00 0110 V:1 Rs:2 0 00 1 0 f:3 Zd:4 0',
      [ZL('Zd', 2, SZ, 2), SLICE('f', 'field', 2)], feat='SME2P1')
    E('movaz', '11000000 sz:2 00 0110 V:1 Rs:2 0 01 1 0 f:3 Zd:3 00',
      [ZL('Zd', 4, SZ, 4), SLICE('f', 'field', 4)], feat='SME2P1')
    E('movaz', '11000000 00 00 0110 0 Rv:2 0 10 1 0 off:3 Zd:4 0',
      [ZL('Zd', 2, 'D', 2), ARRAY('off', 'd', 2, optional_group=True)], feat='SME2P1')
    E('movaz', '11000000 00 00 0110 0 Rv:2 0 11 1 0 off:3 Zd:3 00',
      [ZL('Zd', 4, 'D', 4), ARRAY('off', 'd', 4, optional_group=True)], feat='SME2P1')


def movt():
    """movt between zt0 and a general purpose or a z register (SME2 and FEAT_SME_LUTv2)"""
    E('movt', '1100000001001100 0 idx:3 00 11111 Rt:5', [X('Rt'), ZT0('INDEX', 'idx')], feat='SME2',
      streaming=False)
    E('movt', '1100000001001110 0 idx:3 00 11111 Rt:5', [ZT0('INDEX', 'idx'), X('Rt')], feat='SME2',
      streaming=False)
    E('movt', '1100000001001111 00 n:2 00 11111 Zt:5', [ZT0('MUL_VL', 'n'), Z('Zt', 'NONE')], feat='LUTV2')


def zero():
    E('zero', '11000000 00001000 00000000 mask:8', [TILELIST('mask')], feat='SME')
    E('zero', '11000000 01001000 00000000 00000001', [ZT0('BRACED')], feat='SME2')
    # zero za.d[w8, 0, vgx2]...: (vgx, range) and the offset field
    forms = [
        ('11000', 2, 1, 3), ('11001', 0, 2, 3), ('11010', 2, 2, 2), ('11011', 4, 2, 2),
        ('11100', 4, 1, 3), ('11101', 0, 4, 2), ('11110', 2, 4, 1), ('11111', 4, 4, 1),
    ]
    for bits, group, rng, width in forms:
        # bits 31-24 | 23-20 | 19-16 | 15 | 14-13 | 12-3 | 2-0
        pattern = '11000000 0000 %s %s Rv:2 0000000000 %s off:%d' % (
            bits[:4], bits[4], '0' * (3 - width), width)
        E('zero', pattern, [ARRAY('off', 'd', group, rng, width)], feat='SME2P1')


def add_horizontal():
    for mn, vertical in (('addha', 0), ('addva', 1)):
        E(mn, '11000000 10 01 000%d Pm:3 Pn:3 Zn:5 000 ZAda:2' % vertical,
          [TILE('ZAda', 's', 2), pn3(), pm3(), Z('Zn', 'S')], feat='SME')
        E(mn, '11000000 11 01 000%d Pm:3 Pn:3 Zn:5 00 ZAda:3' % vertical,
          [TILE('ZAda', 'd', 3), pn3(), pm3(), Z('Zn', 'D')], feat='I16I64')


def outer_products():
    def opa(mn, top, b3, feat, za, zsize, width, feat2=None):
        """top is bits 31-21, b3 the bit 3. S (bit 4) selects the a / s forms"""
        for s_bit, name in ((0, mn + 'a'), (1, mn + 's')):
            if s_bit and feat in ('F8F16', 'F8F32'):
                continue
            if width == 2:
                low = '%d %d 0 ZAda:2' % (s_bit, b3)
            elif width == 3:
                low = '%d 0 ZAda:3' % s_bit
            else:
                low = '%d 1 00 ZAda:1' % s_bit
            E(name, '%s Zm:5 Pm:3 Pn:3 Zn:5 %s' % (top, low),
              [TILE('ZAda', za, width), pn3(), pm3(), Z('Zn', zsize.upper()), Z('Zm', zsize.upper())],
              feat=feat, feat2=feat2)

    opa_defs = [
        # name, bits 31-21, bit 3, feature, tile size, source size, tile bits, second feature
        ('fmop', '10000000 100', 0, 'SME', 's', 's', 2, None),
        ('fmop', '10000000 110', 0, 'F64F64', 'd', 'd', 3, None),
        ('fmop', '10000001 101', 0, 'SME', 's', 'h', 2, None),
        ('bfmop', '10000001 100', 0, 'SME', 's', 'h', 2, None),
        ('fmop', '10000001 100', 1, 'F16F16', 'h', 'h', 1, None),
        ('bfmop', '10000001 101', 1, 'SME_B16B16', 'h', 'h', 1, None),
        ('fmop', '10000000 101', 1, 'F8F16', 'h', 'b', 1, None),
        ('fmop', '10000000 101', 0, 'F8F32', 's', 'b', 2, None),
        ('bmop', '10000000 100', 1, 'SME2', 's', 's', 2, None),
        ('smop', '10100000 100', 0, 'SME', 's', 'b', 2, None),
        ('smop', '10100000 100', 1, 'SME2', 's', 'h', 2, None),
        ('smop', '10100000 110', 0, 'I16I64', 'd', 'h', 3, None),
        ('sumop', '10100000 101', 0, 'SME', 's', 'b', 2, None),
        ('sumop', '10100000 111', 0, 'I16I64', 'd', 'h', 3, None),
        ('usmop', '10100001 100', 0, 'SME', 's', 'b', 2, None),
        ('usmop', '10100001 110', 0, 'I16I64', 'd', 'h', 3, None),
        ('umop', '10100001 101', 0, 'SME', 's', 'b', 2, None),
        ('umop', '10100001 100', 1, 'SME2', 's', 'h', 2, None),
        ('umop', '10100001 111', 0, 'I16I64', 'd', 'h', 3, None),
    ]
    for name, top, b3, feat, za, zsize, width, feat2 in opa_defs:
        opa(name, top, b3, feat, za, zsize, width, feat2)
