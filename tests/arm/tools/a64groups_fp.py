"""A64 scalar floating point: the arithmetic, the conversions, the comparisons
and the moves of the s, d and h registers (and the FP16 / BF16 / FRINTTS /
JSCVT parts of them)."""
import a64dsl
from a64dsl import *
from a64simd_dsl import *

BASE = 'FEAT_A64_BASE'
# (register size, ftype bits, feature): the half precision needs FEAT_FP16
TYPES = [('s', '00', BASE), ('d', '01', BASE), ('h', '11', 'FEAT_FP16')]


def one_source():
    """FP data-processing (1 source): M 0 S 11110 ftype 1 opcode 10000 Rn Rd"""
    simple = [('fmov', '000000'), ('fabs', '000001'), ('fneg', '000010'),
              ('fsqrt', '000011'), ('frintn', '001000'), ('frintp', '001001'),
              ('frintm', '001010'), ('frintz', '001011'), ('frinta', '001100'),
              ('frintx', '001110'), ('frinti', '001111')]
    for name, opcode in simple:
        for size, ftype, feat in TYPES:
            E(name, '0 0 0 11110 %s 1 %s 10000 Rn Rd' % (ftype, opcode),
              [fpr('Rd', size), fpr('Rn', size)], feat=feat)
    # FEAT_FRINTTS: single and double only
    for name, opcode in [('frint32z', '010000'), ('frint32x', '010001'),
                         ('frint64z', '010010'), ('frint64x', '010011')]:
        for size, ftype, feat in TYPES[:2]:
            E(name, '0 0 0 11110 %s 1 %s 10000 Rn Rd' % (ftype, opcode),
              [fpr('Rd', size), fpr('Rn', size)], feat='FEAT_FRINTTS')
    # fcvt between the half, single and double precision
    for src, src_type in [('s', '00'), ('d', '01'), ('h', '11')]:
        for dst, dst_opc in [('s', '000100'), ('d', '000101'), ('h', '000111')]:
            if src == dst:
                continue
            E('fcvt', '0 0 0 11110 %s 1 %s 10000 Rn Rd' % (src_type, dst_opc),
              [fpr('Rd', dst), fpr('Rn', src)])
    # FEAT_BF16: bfcvt h0, s1
    E('bfcvt', '0 0 0 11110 01 1 000110 10000 Rn Rd', [fpr('Rd', 'h'), fpr('Rn', 's')],
      feat='FEAT_BF16')


def compare():
    """FP compare: M 0 S 11110 ftype 1 Rm op 1000 Rn opcode2"""
    for name, opcode2, zero in [('fcmp', '00000', False), ('fcmp', '01000', True),
                                ('fcmpe', '10000', False), ('fcmpe', '11000', True)]:
        for size, ftype, feat in TYPES:
            if zero:
                E(name, '0 0 0 11110 %s 1 Rm 00 1000 Rn %s' % (ftype, opcode2),
                  [fpr('Rn', size), fzero(size)], feat=feat)
            else:
                E(name, '0 0 0 11110 %s 1 Rm 00 1000 Rn %s' % (ftype, opcode2),
                  [fpr('Rn', size), fpr('Rm', size)], feat=feat)
    for name, op in [('fccmp', '0'), ('fccmpe', '1')]:
        for size, ftype, feat in TYPES:
            E(name, '0 0 0 11110 %s 1 Rm cond 01 Rn %s nzcv' % (ftype, op),
              [fpr('Rn', size), fpr('Rm', size), nzcv(), cond_op('cond')], feat=feat)


def immediate():
    """FP immediate: M 0 S 11110 ftype 1 imm8 100 imm5 Rd"""
    for size, ftype, feat in TYPES:
        E('fmov', '0 0 0 11110 %s 1 imm8 100 00000 Rd' % ftype,
          [fpr('Rd', size), fimm(size)], feat=feat)


def two_source():
    """FP data-processing (2 source) and the conditional select."""
    ops = [('fmul', '0000'), ('fdiv', '0001'), ('fadd', '0010'), ('fsub', '0011'),
           ('fmax', '0100'), ('fmin', '0101'), ('fmaxnm', '0110'), ('fminnm', '0111'),
           ('fnmul', '1000')]
    for name, opcode in ops:
        for size, ftype, feat in TYPES:
            E(name, '0 0 0 11110 %s 1 Rm %s 10 Rn Rd' % (ftype, opcode),
              [fpr('Rd', size), fpr('Rn', size), fpr('Rm', size)], feat=feat)
    for size, ftype, feat in TYPES:
        E('fcsel', '0 0 0 11110 %s 1 Rm cond 11 Rn Rd' % ftype,
          [fpr('Rd', size), fpr('Rn', size), fpr('Rm', size), cond_op('cond')], feat=feat)


def three_source():
    """FP data-processing (3 source): M 0 S 11111 ftype o1 Rm o0 Ra Rn Rd"""
    for name, o1, o0 in [('fmadd', '0', '0'), ('fmsub', '0', '1'),
                         ('fnmadd', '1', '0'), ('fnmsub', '1', '1')]:
        for size, ftype, feat in TYPES:
            E(name, '0 0 0 11111 %s %s Rm %s Ra Rn Rd' % (ftype, o1, o0),
              [fpr('Rd', size), fpr('Rn', size), fpr('Rm', size), fpr('Ra', size)],
              feat=feat)


def conversions():
    """Conversion between floating point and fixed point / integer."""
    # sf 0 S 11110 ftype 0 rmode opcode scale Rn Rd
    for size, ftype, feat in TYPES:
        for name, opcode in [('scvtf', '010'), ('ucvtf', '011')]:
            E(name, 'sf 0 0 11110 %s 0 00 %s scale Rn Rd' % (ftype, opcode),
              [fpr('Rd', size), R('Rn'), fbits_gpr()], feat=feat)
        for name, opcode in [('fcvtzs', '000'), ('fcvtzu', '001')]:
            E(name, 'sf 0 0 11110 %s 0 11 %s scale Rn Rd' % (ftype, opcode),
              [R('Rd'), fpr('Rn', size), fbits_gpr()], feat=feat)
    # sf 0 S 11110 ftype 1 rmode opcode 000000 Rn Rd
    to_int = [('fcvtns', '00', '000'), ('fcvtnu', '00', '001'), ('fcvtps', '01', '000'),
              ('fcvtpu', '01', '001'), ('fcvtms', '10', '000'), ('fcvtmu', '10', '001'),
              ('fcvtzs', '11', '000'), ('fcvtzu', '11', '001'), ('fcvtas', '00', '100'),
              ('fcvtau', '00', '101')]
    for size, ftype, feat in TYPES:
        for name, rmode, opcode in to_int:
            E(name, 'sf 0 0 11110 %s 1 %s %s 000000 Rn Rd' % (ftype, rmode, opcode),
              [R('Rd'), fpr('Rn', size)], feat=feat)
        for name, opcode in [('scvtf', '010'), ('ucvtf', '011')]:
            E(name, 'sf 0 0 11110 %s 1 00 %s 000000 Rn Rd' % (ftype, opcode),
              [fpr('Rd', size), R('Rn')], feat=feat)
    # fmov between the general and the floating point registers
    for size, ftype, feat, sf, gpr in [('s', '00', BASE, '0', W), ('d', '01', BASE, '1', X),
                                       ('h', '11', 'FEAT_FP16', '0', W),
                                       ('h', '11', 'FEAT_FP16', '1', X)]:
        E('fmov', '%s 0 0 11110 %s 1 00 110 000000 Rn Rd' % (sf, ftype),
          [gpr('Rd'), fpr('Rn', size)], feat=feat)
        E('fmov', '%s 0 0 11110 %s 1 00 111 000000 Rn Rd' % (sf, ftype),
          [fpr('Rd', size), gpr('Rn')], feat=feat)
    # The top half of a 128 bit register: fmov x0, v1.d[1]
    E('fmov', '1 0 0 11110 10 1 01 110 000000 Rn Rd', [X('Rd'), elem('Rn', 'd1')])
    E('fmov', '1 0 0 11110 10 1 01 111 000000 Rn Rd', [elem('Rd', 'd1'), X('Rn')])
    # FEAT_JSCVT
    E('fjcvtzs', '0 0 0 11110 01 1 11 110 000000 Rn Rd', [W('Rd'), fpr('Rn', 'd')],
      feat='FEAT_JSCVT')


def scalar_fp():
    one_source()
    compare()
    immediate()
    two_source()
    three_source()
    conversions()
    fprcvt()


def fprcvt():
    """FEAT_FPRCVT: conversions between floating point and integer values in
    the floating point registers of different sizes (fcvtas s0, d1): sf 0 S
    11110 ftype 1 rmode opcode 000000 Rn Rd. For the float to integer forms
    ftype is the source and sf the size of the result (0: s, 1: d)."""
    to_int = [('fcvtns', '01', '010'), ('fcvtnu', '01', '011'), ('fcvtms', '10', '100'),
              ('fcvtmu', '10', '101'), ('fcvtas', '11', '010'), ('fcvtau', '11', '011'),
              ('fcvtps', '10', '010'), ('fcvtpu', '10', '011'), ('fcvtzs', '10', '110'),
              ('fcvtzu', '10', '111')]
    for name, rmode, opcode in to_int:
        for sf, dest, source, ftype in [(0, 's', 'h', '11'), (0, 's', 'd', '01'),
                                        (1, 'd', 'h', '11'), (1, 'd', 's', '00')]:
            E(name, '%d 0 0 11110 %s 1 %s %s 000000 Rn Rd' % (sf, ftype, rmode, opcode),
              [fpr('Rd', dest), fpr('Rn', source)], feat='FEAT_FPRCVT')
    for name, opcode in [('scvtf', '100'), ('ucvtf', '101')]:
        for sf, dest, source, ftype in [(0, 'h', 's', '11'), (1, 'h', 'd', '11'),
                                        (0, 'd', 's', '01'), (1, 's', 'd', '00')]:
            E(name, '%d 0 0 11110 %s 1 11 %s 000000 Rn Rd' % (sf, ftype, opcode),
              [fpr('Rd', dest), fpr('Rn', source)], feat='FEAT_FPRCVT')
