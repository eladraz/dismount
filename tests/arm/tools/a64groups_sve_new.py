"""The newest instructions of the SVE encoding space: adr, the SVE2.1 quadword
permutes, ld1q / the quadword structure accesses, psel / pext / pmov, the
lookup tables (luti2...), the FP8 instructions, the SVE2.2 zeroing
floating point forms, the SVE2.3 converts, the two and four register AES
instructions, the multi vector narrowing instructions, and the predicate as
counter instructions."""
from a64dsl_sve import *
import a64groups_sve_data as data

SZ = 'SIZE_22'
WIDE = 'SIZE_22_WIDE'
NARROW = 'SIZE_22_NARROW'


def pg3(q=None):
    return P('Pg', q=q, w=3)


def groups():
    adr()
    quadword_permutes()
    quadword_memory()
    predicate_indexed()
    lookup_tables()
    dot_products()
    fp8()
    zeroing_fp()
    converts()
    aes_multi()
    narrowing_multi()
    counters()
    multi_vector_memory()


def adr():
    for size, scheme, dst in (('10', 'ADR_S', 'S'), ('11', 'ADR_D', 'D'),
                              ('00', 'ADR_SXTW', 'D'), ('01', 'ADR_UXTW', 'D')):
        E('adr', '00000100 %s 1 Zm 1010 msz:2 Zn Zd' % size,
          [Z('Zd', dst), MEM(scheme, 0, base='Zn')])


def quadword_permutes():
    E('dupq', '00000101 00 1 tsz:5 001001 Zn Zd', [Z('Zd', 'DUP_TSZ'), ZI('Zn', 'none', 'DUPQ')],
      feat='SVE2P1')
    E('extq', '00000101 011 0 imm4 001001 Zm Zdn',
      [Z('Zdn', 'B'), Z('Zdn', 'B'), Z('Zm', 'B'), IMM('EXTQ')], feat='SVE2P1')
    E('tblq', '01000100 size 0 Zm 111110 Zn Zd',
      [Z('Zd', SZ), ZL('Zn', 1, SZ), Z('Zm', SZ)], feat='SVE2P1')
    E('tbxq', '00000101 size 1 Zm 001101 Zn Zd', [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)],
      feat='SVE2P1')
    for mn, opc in [('zipq1', '111000'), ('zipq2', '111001'), ('uzpq1', '111010'),
                    ('uzpq2', '111011')]:
        E(mn, '01000100 size 0 Zm %s Zn Zd' % opc, [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)],
          feat='SVE2P1')
    E('addqp', '00000100 size 1 Zm 011110 Zn Zd', [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)],
      feat='SVE2P3')
    E('addsubp', '00000100 size 1 Zm 011111 Zn Zd', [Z('Zd', SZ), Z('Zn', SZ), Z('Zm', SZ)],
      feat='SVE2P3')


def quadword_memory():
    q = lambda n: ZL('Zt', n, 'Q')
    # ld1w / ld1d / st1w / st1d of a quadword
    for mn, top, msz in (('ld1w', '000', 2), ('ld1d', '100', 3)):
        E(mn, '10100101 %s 1 imm4 001 Pg:3 Rn Zt' % top, [q(1), P('Pg', q='z', w=3), MEM('MUL_VL4', 1)],
          feat='SVE2P1')
        E(mn, '10100101 %s Rm 100 Pg:3 Rn Zt' % top,
          [q(1), P('Pg', q='z', w=3), MEM('SCALAR_SCALAR', msz)], feat='SVE2P1')
    for mn, top, msz in (('st1w', '000', 2), ('st1d', '110', 3)):
        E(mn, '11100101 %s 0 imm4 111 Pg:3 Rn Zt' % top, [q(1), pg3(), MEM('MUL_VL4', 1)],
          feat='SVE2P1')
        E(mn, '11100101 %s Rm 010 Pg:3 Rn Zt' % top,
          [q(1), pg3(), MEM('SCALAR_SCALAR', msz)], feat='SVE2P1')
    # ld2q... st4q
    loads = ((2, '10100100 100', '10100100 101'), (3, '10100101 000', '10100101 001'),
             (4, '10100101 100', '10100101 101'))
    for count, imm, reg in loads:
        E('ld%dq' % count, '%s 1 imm4 111 Pg:3 Rn Zt' % imm,
          [q(count), P('Pg', q='z', w=3), MEM('MUL_VL4', count)], feat='SVE2P1')
        E('ld%dq' % count, '%s Rm 100 Pg:3 Rn Zt' % reg,
          [q(count), P('Pg', q='z', w=3), MEM('SCALAR_SCALAR', 4)], feat='SVE2P1')
    stores = ((2, '11100100 010', '11100100 011'), (3, '11100100 100', '11100100 101'),
              (4, '11100100 110', '11100100 111'))
    for count, imm, reg in stores:
        E('st%dq' % count, '%s 0 imm4 000 Pg:3 Rn Zt' % imm,
          [q(count), pg3(), MEM('MUL_VL4', count)], feat='SVE2P1')
        E('st%dq' % count, '%s Rm 000 Pg:3 Rn Zt' % reg,
          [q(count), pg3(), MEM('SCALAR_SCALAR', 4)], feat='SVE2P1')


def predicate_indexed():
    E('psel', '00100101 tszh:2 1 tszl:3 Rv:2 01 Pg:4 0 Pn:4 0 Pd:4',
      [P('Pd'), P('Pg'), PIDX('Pn', 'PSEL')], feat='SVE2P1')
    E('pext', '00100101 size 100000 011100 idx:2 PNn:3 1 Pd:4',
      [P('Pd', SZ), PIDX('PNn', 'PEXT_1')], feat='SVE2P1')
    E('pext', '00100101 size 100000 011101 0 idx:1 PNn:3 1 Pd:4',
      [P('Pd', SZ, pair=True), PIDX('PNn', 'PEXT_2')], feat='SVE2P1')
    E('pmov', '00000101 tszh:2 1 01 tszl:2 0 001110 Zn 0 Pd:4',
      [P('Pd', 'TSZ_PMOV'), ZI('Zn', 'none', 'PMOV')], feat='SVE2P1')
    E('pmov', '00000101 tszh:2 1 01 tszl:2 1 001110 0 Pn:4 Zd',
      [ZI('Zd', 'none', 'PMOV'), P('Pn', 'TSZ_PMOV')], feat='SVE2P1')


def lookup_tables():
    luti = lambda n, size: ZL('Zn', n, size)
    E('luti2', '01000101 idx:2 1 Zm 101100 Zn Zd',
      [Z('Zd', 'B'), luti(1, 'B'), ZI('Zm', 'none', 'LUT_2')], feat='LUT')
    E('luti2', '01000101 idx:2 1 Zm 101 idx0:1 10 Zn Zd',
      [Z('Zd', 'H'), luti(1, 'H'), ZI('Zm', 'none', 'LUT_3')], feat='LUT')
    E('luti4', '01000101 idx:1 1 1 Zm 101001 Zn Zd',
      [Z('Zd', 'B'), luti(1, 'B'), ZI('Zm', 'none', 'LUT_1')], feat='LUT')
    E('luti4', '01000101 idx:2 1 Zm 101111 Zn Zd',
      [Z('Zd', 'H'), luti(1, 'H'), ZI('Zm', 'none', 'LUT_2')], feat='LUT')
    E('luti4', '01000101 idx:2 1 Zm 101101 Zn Zd',
      [Z('Zd', 'H'), luti(2, 'H'), ZI('Zm', 'none', 'LUT_2')], feat='LUT')
    E('luti6', '01000101 00 1 Zm 101011 Zn Zd',
      [Z('Zd', 'B'), luti(2, 'B'), Z('Zm', 'NONE')], feat='SVE2P3')
    E('luti6', '01000101 idx:1 1 1 Zm 101011 Zn Zd',
      [Z('Zd', 'H'), luti(2, 'H'), ZI('Zm', 'none', 'LUT_1')], feat='SVE2P3')


def dot_products():
    # sdot, udot: 16 bit to 32 bit (SVE2.1)
    for mn, u in (('sdot', '0'), ('udot', '1')):
        E(mn, '01000100 00 0 Zm 11001 %s Zn Zda' % u,
          [Z('Zda', 'S'), Z('Zn', 'H'), Z('Zm', 'H')], feat='SVE2P1')
        E(mn, '01000100 10 0 i2:2 Zm:3 11001 %s Zn Zda' % u,
          [Z('Zda', 'S'), Z('Zn', 'H'), ZI('Zm', 'h', 'I2')], feat='SVE2P1')
    E('fdot', '01100100 00 1 Zm 100000 Zn Zda', [Z('Zda', 'S'), Z('Zn', 'H'), Z('Zm', 'H')],
      feat='SVE2P1')
    E('fdot', '01100100 00 1 i2:2 Zm:3 010000 Zn Zda',
      [Z('Zda', 'S'), Z('Zn', 'H'), ZI('Zm', 'h', 'I2')], feat='SVE2P1')


def fp8():
    # fdot to half (FP8DOT2) and to single (FP8DOT4)
    E('fdot', '01100100 00 1 Zm 100001 Zn Zda', [Z('Zda', 'H'), Z('Zn', 'B'), Z('Zm', 'B')],
      feat='FP8DOT2')
    E('fdot', '01100100 00 1 i3h:2 Zm:3 0100 i3l:1 1 Zn Zda',
      [Z('Zda', 'H'), Z('Zn', 'B'), ZI('Zm', 'b', 'WIDE_I3')], feat='FP8DOT2')
    E('fdot', '01100100 01 1 Zm 100001 Zn Zda', [Z('Zda', 'S'), Z('Zn', 'B'), Z('Zm', 'B')],
      feat='FP8DOT4')
    E('fdot', '01100100 01 1 i2:2 Zm:3 010001 Zn Zda',
      [Z('Zda', 'S'), Z('Zn', 'B'), ZI('Zm', 'b', 'I2')], feat='FP8DOT4')
    # fmlalb / fmlalt to half (FP8FMA)
    for mn, top, opc in (('fmlalb', '10', '100010'), ('fmlalt', '10', '100110')):
        E(mn, '01100100 %s 1 Zm %s Zn Zda' % (top, opc),
          [Z('Zda', 'H'), Z('Zn', 'B'), Z('Zm', 'B')], feat='FP8FMA')
    E('fmlalb', '01100100 00 1 i4h:2 Zm:3 0101 i4l:2 Zn Zda',
      [Z('Zda', 'H'), Z('Zn', 'B'), ZI('Zm', 'b', 'I4')], feat='FP8FMA')
    E('fmlalt', '01100100 10 1 i4h:2 Zm:3 0101 i4l:2 Zn Zda',
      [Z('Zda', 'H'), Z('Zn', 'B'), ZI('Zm', 'b', 'I4')], feat='FP8FMA')
    # fmlallbb... to single
    for mn, top, vec, idx in (('fmlallbb', '00', '100010', '00'), ('fmlallbt', '00', '100110', '01'),
                              ('fmlalltb', '00', '101010', '10'), ('fmlalltt', '00', '101110', '11')):
        E(mn, '01100100 %s 1 Zm %s Zn Zda' % (top, vec),
          [Z('Zda', 'S'), Z('Zn', 'B'), Z('Zm', 'B')], feat='FP8FMA')
        E(mn, '01100100 %s 1 i4h:2 Zm:3 1100 i4l:2 Zn Zda' % idx,
          [Z('Zda', 'S'), Z('Zn', 'B'), ZI('Zm', 'b', 'I4')], feat='FP8FMA')
    # fmmla
    E('fmmla', '01100100 00 1 Zm 111000 Zn Zda', [Z('Zda', 'S'), Z('Zn', 'B'), Z('Zm', 'B')],
      feat='F8F32MM')
    E('fmmla', '01100100 01 1 Zm 111000 Zn Zda', [Z('Zda', 'H'), Z('Zn', 'B'), Z('Zm', 'B')],
      feat='F8F16MM')
    E('fmmla', '01100100 00 1 Zm 111001 Zn Zda', [Z('Zda', 'S'), Z('Zn', 'H'), Z('Zm', 'H')],
      feat='SVE2P1')
    # The conversions from FP8 and to FP8
    for mn, op in (('f1cvt', '00'), ('f2cvt', '01'), ('bf1cvt', '10'), ('bf2cvt', '11')):
        for lt, suffix in ((0, ''), (1, 'lt')):
            E(mn + suffix, '01100101 00 00 100 %d 0011 %s Zn Zd' % (lt, op),
              [Z('Zd', 'H'), Z('Zn', 'B')], feat='FP8')
    for mn, op, src in (('fcvtn', '00', 'H'), ('fcvtnb', '01', 'S'), ('bfcvtn', '10', 'H'),
                        ('fcvtnt', '11', 'S')):
        E(mn, '01100101 00 00 1010 0011 %s Zn:4 0 Zd' % op,
          [Z('Zd', 'B'), ZL('Zn', 2, src, multiple=2)], feat='FP8')


def zeroing_fp():
    """The SVE2.2 zeroing forms of the floating point instructions"""
    for key, mn, dst, src in data.ZEROING_FP:
        E(mn, '01100100 %s Pg:3 Zn Zd' % key,
          [Z('Zd', dst), P('Pg', q='z', w=3), Z('Zn', src)], feat='SVE2P2')


def converts():
    # integer to floating point and back, unpredicated (SVE2.3)
    for mn, op in (('scvtf', '00'), ('ucvtf', '01'), ('scvtflt', '10'), ('ucvtflt', '11')):
        E(mn, '01100101 size 00 1100 0011 %s Zn Zd' % op,
          [Z('Zd', WIDE), Z('Zn', NARROW)], feat='SVE2P3')
    for mn, op in (('fcvtzsn', '00'), ('fcvtzun', '01')):
        E(mn, '01100101 size 00 1101 0011 %s Zn:4 0 Zd' % op,
          [Z('Zd', NARROW), ZL('Zn', 2, WIDE, multiple=2)], feat='SVE2P3')


def aes_multi():
    for mn, opc, low in (('aesd', '111011', '010'), ('aesdimc', '111011', '011'),
                         ('aese', '111010', '010'), ('aesemc', '111010', '011')):
        E(mn, '01000101 00 1 idx:2 %s %s Zm Zdn:4 0' % (low, opc),
          [ZL('Zdn', 2, 'B', multiple=2), ZL('Zdn', 2, 'B', multiple=2), ZI('Zm', 'q', 'AES')],
          feat='AES2')
        low4 = '1' + low[1:]
        E(mn, '01000101 00 1 idx:2 %s %s Zm Zdn:3 00' % (low4, opc),
          [ZL('Zdn', 4, 'B', multiple=4), ZL('Zdn', 4, 'B', multiple=4), ZI('Zm', 'q', 'AES')],
          feat='AES2')
    E('pmlal', '01000101 00 1 Zm 111111 Zn Zda:4 0',
      [ZL('Zda', 2, 'Q', multiple=2), Z('Zn', 'D'), Z('Zm', 'D')], feat='AES2')
    E('pmull', '01000101 00 1 Zm 111110 Zn Zd:4 0',
      [ZL('Zd', 2, 'Q', multiple=2), Z('Zn', 'D'), Z('Zm', 'D')], feat='AES2')


def narrowing_multi():
    for mn, opc in (('sqcvtn', '010000'), ('uqcvtn', '010010'), ('sqcvtun', '010100')):
        E(mn, '01000101 00 1 10001 %s Zn:4 0 Zd' % opc,
          [Z('Zd', 'H'), ZL('Zn', 2, 'S', multiple=2)], feat='SVE2P1')
    for mn, opc in (('sqrshrun', '000010'), ('sqrshrn', '001010'), ('uqrshrn', '001110')):
        E(mn, '01000101 10 1 1 imm4 %s Zn:4 0 Zd' % opc,
          [Z('Zd', 'H'), ZL('Zn', 2, 'S', multiple=2), IMM('SHIFT_MULTI')], feat='SVE2P1')


def counters():
    """Predicates as counters: ptrue, cntp"""
    E('ptrue', '00100101 size 100000 011110 00000 1 0 PNd:3', [PN('PNd', SZ)], feat='SVE2P1')
    for vl, text in ((0, 'vlx2'), (1, 'vlx4')):
        pass
    E('cntp', '00100101 size 100000 10000 vl 1 PNn:4 Rd',
      [X('Rd'), PN('PNn', SZ, high=False), OPT('VLX', 'vl')], feat='SVE2P1')


def multi_vector_memory():
    """ld1 / ldnt1 / st1 / stnt1 of two or four registers with a predicate as
    counter (consecutive, or strided: { z0.s, z8.s }): they live in the SME
    encoding class, the instructions of SVE2.1 and SME2"""
    for msz in range(4):
        n = 'bhwd'[msz]
        es = 'BHSD'[msz]
        for count in (2, 4):
            for strided in (0, 1):
                for base in ('ld1', 'ldnt1', 'st1', 'stnt1'):
                    store = 1 if base.startswith('st') else 0
                    nt = 1 if 'nt' in base else 0
                    op = (1 if count == 4 else 0) << 2 | msz
                    f = format(op, '03b')
                    top = '1010000%d' % strided
                    if count == 2 and not strided:
                        low = 'Zt:4 %d' % nt
                    elif count == 2:
                        low = 'hi:1 %d Zlo:3' % nt
                    elif not strided:
                        low = 'Zt:3 0 %d' % nt
                    else:
                        low = 'hi:1 %d 0 Zlo:2' % nt
                    field = 'Zlo' if strided else 'Zt'
                    zl = ZL(field, count, es, multiple=1 if strided else count, strided=bool(strided))
                    pn = PN('PNg', q='z' if not store else None)
                    E(base + n, '%s 0 1 %d 0 imm4 %s PNg Rn %s' % (top, store, f, low),
                      [zl, pn, MEM('MUL_VL4', count)], feat='SVE2P1')
                    E(base + n, '%s 0 0 %d Rm %s PNg Rn %s' % (top, store, f, low),
                      [zl, pn, MEM('SCALAR_SCALAR_ZR', msz)], feat='SVE2P1')
