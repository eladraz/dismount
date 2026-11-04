"""A64 Advanced SIMD data processing: the vector and the scalar forms of the
three same / different, two register miscellaneous, across lanes, copy,
permute, extract, table lookup, by element, shift by immediate and modified
immediate classes (and the extensions that live in them: RDM, FP16, FHM, dot
product, matrix multiply, BFloat16, complex numbers, FP8, FAMINMAX, LUT)."""
import a64dsl
from a64dsl import *
from a64simd_dsl import *

BASE = 'FEAT_A64_BASE'
ALIAS = 'ARM_ENTRY_ALIAS'

B, H, S, D = 0, 1, 2, 3
BHS = [B, H, S]
HS = [H, S]


def zero():
    """The #0 of the compare with zero instructions: bit 31 is always 0."""
    return lambda: a64dsl._spec('ARM_KIND_IMMEDIATE', 31, '0', 1)


def q_suffix(q):
    return '2' if q else ''


# ---------------------------------------------------------------------------
# Three same

# (U, opcode, mnemonic, the valid sizes)
THREE_SAME_INT = [
    (0, '00000', 'shadd', BHS), (0, '00001', 'sqadd', None), (0, '00010', 'srhadd', BHS),
    (0, '00100', 'shsub', BHS), (0, '00101', 'sqsub', None), (0, '00110', 'cmgt', None),
    (0, '00111', 'cmge', None), (0, '01000', 'sshl', None), (0, '01001', 'sqshl', None),
    (0, '01010', 'srshl', None), (0, '01011', 'sqrshl', None), (0, '01100', 'smax', BHS),
    (0, '01101', 'smin', BHS), (0, '01110', 'sabd', BHS), (0, '01111', 'saba', BHS),
    (0, '10000', 'add', None), (0, '10001', 'cmtst', None), (0, '10010', 'mla', BHS),
    (0, '10011', 'mul', BHS), (0, '10100', 'smaxp', BHS), (0, '10101', 'sminp', BHS),
    (0, '10110', 'sqdmulh', HS), (0, '10111', 'addp', None),
    (1, '00000', 'uhadd', BHS), (1, '00001', 'uqadd', None), (1, '00010', 'urhadd', BHS),
    (1, '00100', 'uhsub', BHS), (1, '00101', 'uqsub', None), (1, '00110', 'cmhi', None),
    (1, '00111', 'cmhs', None), (1, '01000', 'ushl', None), (1, '01001', 'uqshl', None),
    (1, '01010', 'urshl', None), (1, '01011', 'uqrshl', None), (1, '01100', 'umax', BHS),
    (1, '01101', 'umin', BHS), (1, '01110', 'uabd', BHS), (1, '01111', 'uaba', BHS),
    (1, '10000', 'sub', None), (1, '10001', 'cmeq', None), (1, '10010', 'mls', BHS),
    (1, '10011', 'pmul', [B]), (1, '10100', 'umaxp', BHS), (1, '10101', 'uminp', BHS),
    (1, '10110', 'sqrdmulh', HS),
]

# (U, o1, opcode, mnemonic, feature): the sz bit (22) is the precision
THREE_SAME_FP = [
    (0, 0, '11000', 'fmaxnm', BASE), (0, 1, '11000', 'fminnm', BASE),
    (0, 0, '11001', 'fmla', BASE), (0, 1, '11001', 'fmls', BASE),
    (0, 0, '11010', 'fadd', BASE), (0, 1, '11010', 'fsub', BASE),
    (0, 0, '11011', 'fmulx', BASE), (0, 0, '11100', 'fcmeq', BASE),
    (0, 0, '11110', 'fmax', BASE), (0, 1, '11110', 'fmin', BASE),
    (0, 0, '11111', 'frecps', BASE), (0, 1, '11111', 'frsqrts', BASE),
    (1, 0, '11000', 'fmaxnmp', BASE), (1, 1, '11000', 'fminnmp', BASE),
    (1, 0, '11010', 'faddp', BASE), (1, 1, '11010', 'fabd', BASE),
    (1, 0, '11011', 'fmul', BASE), (1, 0, '11100', 'fcmge', BASE),
    (1, 1, '11100', 'fcmgt', BASE), (1, 0, '11101', 'facge', BASE),
    (1, 1, '11101', 'facgt', BASE), (1, 0, '11110', 'fmaxp', BASE),
    (1, 1, '11110', 'fminp', BASE), (1, 0, '11111', 'fdiv', BASE),
    (0, 1, '11011', 'famax', 'FEAT_FAMINMAX'), (1, 1, '11011', 'famin', 'FEAT_FAMINMAX'),
    (1, 1, '11111', 'fscale', 'FEAT_FP8'),
]

# The half precision forms: (U, a, opcode (3 bits), mnemonic)
THREE_SAME_FP16 = [
    (0, 0, '000', 'fmaxnm'), (0, 0, '001', 'fmla'), (0, 0, '010', 'fadd'),
    (0, 0, '011', 'fmulx'), (0, 0, '100', 'fcmeq'), (0, 0, '110', 'fmax'),
    (0, 0, '111', 'frecps'), (0, 1, '000', 'fminnm'), (0, 1, '001', 'fmls'),
    (0, 1, '010', 'fsub'), (0, 1, '110', 'fmin'), (0, 1, '111', 'frsqrts'),
    (1, 0, '000', 'fmaxnmp'), (1, 0, '010', 'faddp'), (1, 0, '011', 'fmul'),
    (1, 0, '100', 'fcmge'), (1, 0, '101', 'facge'), (1, 0, '110', 'fmaxp'),
    (1, 0, '111', 'fdiv'), (1, 1, '000', 'fminnmp'), (1, 1, '010', 'fabd'),
    (1, 1, '100', 'fcmgt'), (1, 1, '101', 'facgt'), (1, 1, '110', 'fminp'),
]
# The half precision forms of the FAMINMAX / FP8 instructions
THREE_SAME_FP16_NEW = [
    (0, 1, '011', 'famax', 'FEAT_FAMINMAX'), (1, 1, '011', 'famin', 'FEAT_FAMINMAX'),
    (1, 1, '111', 'fscale', 'FEAT_FP8'),
]


# The compare instructions which LLVM reads with the operands swapped
# (cmle v0.4s, v1.4s, v2.4s is cmge v0.4s, v2.4s, v1.4s): the mnemonic of the
# swapped form, for the mnemonic of the encoding. The disassembler prints the
# encoding
SWAPPED_COMPARES = {'cmge': 'cmle', 'cmgt': 'cmlt', 'cmhs': 'cmls', 'cmhi': 'cmlo',
                    'fcmge': 'fcmle', 'fcmgt': 'fcmlt', 'facge': 'facle', 'facgt': 'faclt'}


def swapped(name, pattern, operands, feat=BASE):
    """The assemble only entry cmle... of the compare 'name': the operands of the
    text are Rd, Rm, Rn; 'operands' has the builders of Rd, Rn and Rm."""
    if name in SWAPPED_COMPARES:
        rd, rn, rm = operands
        E(SWAPPED_COMPARES[name], pattern, [rd, rm, rn], feat=feat,
          flags=ALIAS + ' | ARM_ENTRY_ASSEMBLE_ONLY')


def three_same():
    # 0 Q U 01110 size 1 Rm opcode 1 Rn Rd
    for u, opcode, name, sizes in THREE_SAME_INT:
        pattern = '0 Q %d 01110 size 1 Rm %s 1 Rn Rd' % (u, opcode)
        ops = [vec('Rd', 'q_size', sizes), vec('Rn', 'q_size', sizes), vec('Rm', 'q_size', sizes)]
        E(name, pattern, ops, flags=ALIAS if name in SWAPPED_COMPARES else None)
        swapped(name, pattern, ops)
    # The logical instructions: the size is the operation
    E('mov', '0 Q 0 01110 10 1 Rm 00011 1 Rn Rd',
      [vec('Rd', 'q_bytes'), vec('Rn', 'q_bytes'), tied('Rm', 'Rn')],
      flags=ALIAS, cond='$Rn == $Rm')
    for u, size, name in [(0, '00', 'and'), (0, '01', 'bic'), (0, '10', 'orr'), (0, '11', 'orn'),
                          (1, '00', 'eor'), (1, '01', 'bsl'), (1, '10', 'bit'), (1, '11', 'bif')]:
        E(name, '0 Q %d 01110 %s 1 Rm 00011 1 Rn Rd' % (u, size),
          [vec('Rd', 'q_bytes'), vec('Rn', 'q_bytes'), vec('Rm', 'q_bytes')])
    # Floating point: 0 Q U 01110 o1 sz 1 Rm opcode 1 Rn Rd
    for u, o1, opcode, name, feat in THREE_SAME_FP:
        pattern = '0 Q %d 01110 %d sz 1 Rm %s 1 Rn Rd' % (u, o1, opcode)
        ops = [vec('Rd', 'q_sz'), vec('Rn', 'q_sz'), vec('Rm', 'q_sz')]
        E(name, pattern, ops, feat=feat, flags=ALIAS if name in SWAPPED_COMPARES else None)
        swapped(name, pattern, ops, feat)
    # FEAT_FHM: fmlal v0.2s, v1.2h, v2.2h
    for u, o1, opcode, name in [(0, 0, '11101', 'fmlal'), (0, 1, '11101', 'fmlsl'),
                                (1, 0, '11001', 'fmlal2'), (1, 1, '11001', 'fmlsl2')]:
        E(name, '0 Q %d 01110 %d 0 1 Rm %s 1 Rn Rd' % (u, o1, opcode),
          [vec('Rd', 'q_single'), vec('Rn', 'q_2h4h'), vec('Rm', 'q_2h4h')], feat='FEAT_FHM')
    # Half precision: 0 Q U 01110 a 1 0 Rm 00 opcode 1 Rn Rd
    for u, a, opcode, name in THREE_SAME_FP16:
        pattern = '0 Q %d 01110 %d 1 0 Rm 00 %s 1 Rn Rd' % (u, a, opcode)
        ops = [vec('Rd', 'q_half'), vec('Rn', 'q_half'), vec('Rm', 'q_half')]
        E(name, pattern, ops, feat='FEAT_FP16', flags=ALIAS if name in SWAPPED_COMPARES else None)
        swapped(name, pattern, ops, 'FEAT_FP16')
    for u, a, opcode, name, feat in THREE_SAME_FP16_NEW:
        E(name, '0 Q %d 01110 %d 1 0 Rm 00 %s 1 Rn Rd' % (u, a, opcode),
          [vec('Rd', 'q_half'), vec('Rn', 'q_half'), vec('Rm', 'q_half')], feat=feat)


# ---------------------------------------------------------------------------
# Three different

# (U, opcode, mnemonic, shape, valid sizes): the shape is 'l' (long: wide
# result of two narrow sources), 'w' (wide, wide and narrow), 'n' (narrow
# result of two wide sources)
THREE_DIFFERENT = [
    (0, '0000', 'saddl', 'l', BHS), (1, '0000', 'uaddl', 'l', BHS),
    (0, '0001', 'saddw', 'w', BHS), (1, '0001', 'uaddw', 'w', BHS),
    (0, '0010', 'ssubl', 'l', BHS), (1, '0010', 'usubl', 'l', BHS),
    (0, '0011', 'ssubw', 'w', BHS), (1, '0011', 'usubw', 'w', BHS),
    (0, '0100', 'addhn', 'n', BHS), (1, '0100', 'raddhn', 'n', BHS),
    (0, '0101', 'sabal', 'l', BHS), (1, '0101', 'uabal', 'l', BHS),
    (0, '0110', 'subhn', 'n', BHS), (1, '0110', 'rsubhn', 'n', BHS),
    (0, '0111', 'sabdl', 'l', BHS), (1, '0111', 'uabdl', 'l', BHS),
    (0, '1000', 'smlal', 'l', BHS), (1, '1000', 'umlal', 'l', BHS),
    (0, '1001', 'sqdmlal', 'l', HS), (0, '1010', 'smlsl', 'l', BHS),
    (1, '1010', 'umlsl', 'l', BHS), (0, '1011', 'sqdmlsl', 'l', HS),
    (0, '1100', 'smull', 'l', BHS), (1, '1100', 'umull', 'l', BHS),
    (0, '1101', 'sqdmull', 'l', HS),
]


def three_different():
    # 0 Q U 01110 size 1 Rm opcode 00 Rn Rd
    for u, opcode, name, shape, sizes in THREE_DIFFERENT:
        for q in (0, 1):
            pattern = '0 %d %d 01110 size 1 Rm %s 00 Rn Rd' % (q, u, opcode)
            narrow = lambda f: vec(f, 'q_size', sizes)
            wide = lambda f: vec(f, 'long_size', sizes)
            if shape == 'l':
                ops = [wide('Rd'), narrow('Rn'), narrow('Rm')]
            elif shape == 'w':
                ops = [wide('Rd'), wide('Rn'), narrow('Rm')]
            else:
                ops = [narrow('Rd'), wide('Rn'), wide('Rm')]
            E(name + q_suffix(q), pattern, ops)
    # pmull: 8b to 8h, and (FEAT_AES) 1d to 1q
    for q in (0, 1):
        E('pmull' + q_suffix(q), '0 %d 0 01110 00 1 Rm 1110 00 Rn Rd' % q,
          [vecf('Rd', '8H'), vec('Rn', 'q_bytes'), vec('Rm', 'q_bytes')])
        E('pmull' + q_suffix(q), '0 %d 0 01110 11 1 Rm 1110 00 Rn Rd' % q,
          [vecf('Rd', '1Q'), vecf('Rn', '1D' if q == 0 else '2D'),
           vecf('Rm', '1D' if q == 0 else '2D')], feat='FEAT_AES')


# ---------------------------------------------------------------------------
# Two register miscellaneous

# The integer instructions: (U, opcode, mnemonic, shape, valid sizes)
# shape: 'v' vector, 'z' vector with #0, 'p' pairwise long (long result)
TWO_MISC_INT = [
    (0, '00000', 'rev64', 'v', BHS), (0, '00001', 'rev16', 'v', [B]),
    (0, '00010', 'saddlp', 'p', BHS), (0, '00011', 'suqadd', 'v', None),
    (0, '00100', 'cls', 'v', BHS), (0, '00101', 'cnt', 'v', [B]),
    (0, '00110', 'sadalp', 'p', BHS), (0, '00111', 'sqabs', 'v', None),
    (0, '01000', 'cmgt', 'z', None), (0, '01001', 'cmeq', 'z', None),
    (0, '01010', 'cmlt', 'z', None), (0, '01011', 'abs', 'v', None),
    (1, '00000', 'rev32', 'v', [B, H]), (1, '00010', 'uaddlp', 'p', BHS),
    (1, '00011', 'usqadd', 'v', None), (1, '00100', 'clz', 'v', BHS),
    (1, '00101', 'mvn', 'v', [B]), (1, '00110', 'uadalp', 'p', BHS),
    (1, '00111', 'sqneg', 'v', None), (1, '01000', 'cmge', 'z', None),
    (1, '01001', 'cmle', 'z', None), (1, '01011', 'neg', 'v', None),
]
# (U, opcode, mnemonic): narrowing
TWO_MISC_NARROW = [
    (0, '10010', 'xtn'), (0, '10100', 'sqxtn'), (1, '10010', 'sqxtun'), (1, '10100', 'uqxtn'),
]
# The floating point instructions: (U, o1, opcode, mnemonic, has a half
# precision form, feature)
TWO_MISC_FP = [
    (0, 0, '11000', 'frintn', True), (0, 1, '11000', 'frintp', True),
    (0, 0, '11001', 'frintm', True), (0, 1, '11001', 'frintz', True),
    (0, 0, '11010', 'fcvtns', True), (0, 1, '11010', 'fcvtps', True),
    (0, 0, '11011', 'fcvtms', True), (0, 1, '11011', 'fcvtzs', True),
    (0, 0, '11100', 'fcvtas', True), (0, 0, '11101', 'scvtf', True),
    (0, 1, '11101', 'frecpe', True), (0, 1, '01111', 'fabs', True),
    (1, 0, '11000', 'frinta', True), (1, 0, '11001', 'frintx', True),
    (1, 1, '11001', 'frinti', True), (1, 0, '11010', 'fcvtnu', True),
    (1, 1, '11010', 'fcvtpu', True), (1, 0, '11011', 'fcvtmu', True),
    (1, 1, '11011', 'fcvtzu', True), (1, 0, '11100', 'fcvtau', True),
    (1, 0, '11101', 'ucvtf', True), (1, 1, '11101', 'frsqrte', True),
    (1, 1, '01111', 'fneg', True), (1, 1, '11111', 'fsqrt', True),
]
# The compares with zero: (U, o1, opcode, mnemonic)
TWO_MISC_FP_ZERO = [
    (0, 1, '01100', 'fcmgt'), (0, 1, '01101', 'fcmeq'), (0, 1, '01110', 'fcmlt'),
    (1, 1, '01100', 'fcmge'), (1, 1, '01101', 'fcmle'),
]


def two_reg_misc():
    # 0 Q U 01110 size 10000 opcode 10 Rn Rd
    for u, opcode, name, shape, sizes in TWO_MISC_INT:
        pattern = '0 Q %d 01110 size 10000 %s 10 Rn Rd' % (u, opcode)
        if shape == 'v':
            # mvn is the name LLVM prints for not
            E(name, pattern, [vec('Rd', 'q_size', sizes), vec('Rn', 'q_size', sizes)],
              flags=ALIAS if name == 'mvn' else None)
            if name == 'mvn':
                E('not', pattern, [vec('Rd', 'q_size', sizes), vec('Rn', 'q_size', sizes)],
                  flags=ALIAS + ' | ARM_ENTRY_ASSEMBLE_ONLY')
        elif shape == 'z':
            E(name, pattern, [vec('Rd', 'q_size'), vec('Rn', 'q_size'), zero()])
        else:
            E(name, pattern, [vec('Rd', 'q_long_pair', sizes), vec('Rn', 'q_size', sizes)])
    # rbit is the not instruction with size 1
    E('rbit', '0 Q 1 01110 01 10000 00101 10 Rn Rd', [vec('Rd', 'q_bytes'), vec('Rn', 'q_bytes')])
    for u, opcode, name in TWO_MISC_NARROW:
        for q in (0, 1):
            E(name + q_suffix(q), '0 %d %d 01110 size 10000 %s 10 Rn Rd' % (q, u, opcode),
              [vec('Rd', 'q_size', BHS), vec('Rn', 'long_size', BHS)])
    # shll: the shift is the element size
    for q in (0, 1):
        E('shll' + q_suffix(q), '0 %d 1 01110 size 10000 10011 10 Rn Rd' % q,
          [vec('Rd', 'long_size', BHS), vec('Rn', 'q_size', BHS), shiftimm(esize=True)])
    # Floating point: 0 Q U 01110 o1 sz 10000 opcode 10 Rn Rd
    for u, o1, opcode, name, has_half in TWO_MISC_FP:
        E(name, '0 Q %d 01110 %d sz 10000 %s 10 Rn Rd' % (u, o1, opcode),
          [vec('Rd', 'q_sz'), vec('Rn', 'q_sz')])
        if has_half:
            E(name, '0 Q %d 01110 %d 111100 %s 10 Rn Rd' % (u, o1, opcode),
              [vec('Rd', 'q_half'), vec('Rn', 'q_half')], feat='FEAT_FP16')
    for u, o1, opcode, name in TWO_MISC_FP_ZERO:
        E(name, '0 Q %d 01110 %d sz 10000 %s 10 Rn Rd' % (u, o1, opcode),
          [vec('Rd', 'q_sz'), vec('Rn', 'q_sz'), fzero(integer=True)])
        E(name, '0 Q %d 01110 %d 111100 %s 10 Rn Rd' % (u, o1, opcode),
          [vec('Rd', 'q_half'), vec('Rn', 'q_half'), fzero('h', integer=True)], feat='FEAT_FP16')
    # urecpe / ursqrte: single precision integers
    E('urecpe', '0 Q 0 01110 1 0 10000 11100 10 Rn Rd', [vec('Rd', 'q_single'), vec('Rn', 'q_single')])
    E('ursqrte', '0 Q 1 01110 1 0 10000 11100 10 Rn Rd', [vec('Rd', 'q_single'), vec('Rn', 'q_single')])
    # fcvtn / fcvtl / fcvtxn
    for q in (0, 1):
        E('fcvtn' + q_suffix(q), '0 %d 0 01110 0 sz 10000 10110 10 Rn Rd' % q,
          [vec('Rd', 'q_sz_narrow'), vec('Rn', 'sz_long')])
        E('fcvtl' + q_suffix(q), '0 %d 0 01110 0 sz 10000 10111 10 Rn Rd' % q,
          [vec('Rd', 'sz_long'), vec('Rn', 'q_sz_narrow')])
        E('fcvtxn' + q_suffix(q), '0 %d 1 01110 0 1 10000 10110 10 Rn Rd' % q,
          [vec('Rd', 'q_sz_narrow'), vec('Rn', 'sz_long')])
    # FEAT_FRINTTS
    for u, o1, name in [(0, 0, 'frint32z'), (0, 1, 'frint64z'), (1, 0, 'frint32x'), (1, 1, 'frint64x')]:
        E(name, '0 Q %d 01110 0 sz 10000 11110 10 Rn Rd' % u if o1 == 0 else
          '0 Q %d 01110 0 sz 10000 11111 10 Rn Rd' % u,
          [vec('Rd', 'q_sz'), vec('Rn', 'q_sz')], feat='FEAT_FRINTTS')
    # FEAT_BF16
    for q in (0, 1):
        E('bfcvtn' + q_suffix(q), '0 %d 0 01110 10 10000 10110 10 Rn Rd' % q,
          [vec('Rd', 'q_half'), vecf('Rn', '4S')], feat='FEAT_BF16')
    # FEAT_FP8: f1cvtl, f2cvtl, bf1cvtl, bf2cvtl
    for q in (0, 1):
        for size, name in [('00', 'f1cvtl'), ('01', 'f2cvtl'), ('10', 'bf1cvtl'), ('11', 'bf2cvtl')]:
            E(name + q_suffix(q), '0 %d 1 01110 %s 10000 10111 10 Rn Rd' % (q, size),
              [vecf('Rd', '8H'), vec('Rn', 'q_bytes')], feat='FEAT_FP8')


def simd_vector():
    three_same()
    three_different()
    two_reg_misc()
    simd_vector_rest()


# ---------------------------------------------------------------------------
# Across lanes

def across_lanes():
    # 0 Q U 01110 size 11000 opcode 10 Rn Rd
    for u, opcode, name, long_result in [(0, '00011', 'saddlv', True), (1, '00011', 'uaddlv', True),
                                         (0, '01010', 'smaxv', False), (1, '01010', 'umaxv', False),
                                         (0, '11010', 'sminv', False), (1, '11010', 'uminv', False),
                                         (0, '11011', 'addv', False)]:
        scalar = (sfp('Rd', 'long_mask', BHS) if long_result else sfp('Rd', 'size_mask', BHS))
        E(name, '0 Q %d 01110 size 11000 %s 10 Rn Rd' % (u, opcode),
          [scalar, vec('Rn', 'q_size', BHS, no2s=True)])
    # The floating point maxima: fmaxnmv / fminnmv / fmaxv / fminv of a single
    # precision vector of four, and of a half precision one
    for o1, opcode, name in [(0, '01100', 'fmaxnmv'), (1, '01100', 'fminnmv'),
                             (0, '01111', 'fmaxv'), (1, '01111', 'fminv')]:
        E(name, '0 1 1 01110 %d 0 11000 %s 10 Rn Rd' % (o1, opcode),
          [sfp('Rd', 's'), vecf('Rn', '4S')])
        E(name, '0 Q 0 01110 %d 0 11000 %s 10 Rn Rd' % (o1, opcode),
          [sfp('Rd', 'h'), vec('Rn', 'q_half')], feat='FEAT_FP16')


# ---------------------------------------------------------------------------
# Copy, permute, extract and table lookup

def copy():
    # 0 Q op 01110000 imm5 0 imm4 1 Rn Rd
    E('dup', '0 Q 0 01110000 imm5 0 0000 1 Rn Rd',
      [vec('Rd', 'q_imm5'), elem('Rn', 'imm5')])
    for k, tail in [(0, 'ig:4 1'), (1, 'ig:3 10'), (2, 'ig:2 100'), (3, 'ig:1 1000')]:
        gpr = X if k == 3 else W
        E('dup', '0 Q 0 01110000 %s 0 0001 1 Rn Rd' % tail,
          [vec('Rd', 'q_imm5'), gpr('Rn')])
    # smov: 0 Q 0 01110000 imm5 0 0101 1
    E('smov', '0 0 0 01110000 imm5 0 0101 1 Rn Rd', [W('Rd'), elem('Rn', 'imm5', [B, H])])
    E('smov', '0 1 0 01110000 imm5 0 0101 1 Rn Rd', [X('Rd'), elem('Rn', 'imm5', BHS)])
    # umov, and its aliases mov w0, v1.s[1] / mov x0, v1.d[1]
    E('umov', '0 0 0 01110000 imm5 0 0111 1 Rn Rd', [W('Rd'), elem('Rn', 'imm5', [B, H])])
    E('mov', '0 0 0 01110000 ig:2 100 0 0111 1 Rn Rd', [W('Rd'), elem('Rn', 'imm5', [S])],
      flags=ALIAS)
    E('mov', '0 1 0 01110000 ig:1 1000 0 0111 1 Rn Rd', [X('Rd'), elem('Rn', 'imm5', [D])],
      flags=ALIAS)
    # umov with the wide elements, which LLVM reads and prints as mov
    E('umov', '0 0 0 01110000 ig:2 100 0 0111 1 Rn Rd', [W('Rd'), elem('Rn', 'imm5', [S])],
      flags=ALIAS + ' | ARM_ENTRY_ASSEMBLE_ONLY')
    E('umov', '0 1 0 01110000 ig:1 1000 0 0111 1 Rn Rd', [X('Rd'), elem('Rn', 'imm5', [D])],
      flags=ALIAS + ' | ARM_ENTRY_ASSEMBLE_ONLY')
    # ins (printed mov): from a general register and from an element
    for k in range(4):
        gpr = X if k == 3 else W
        E('mov', '0 1 0 01110000 imm5 0 0011 1 Rn Rd', [elem('Rd', 'imm5', [k]), gpr('Rn')],
          flags=ALIAS)
        E('ins', '0 1 0 01110000 imm5 0 0011 1 Rn Rd', [elem('Rd', 'imm5', [k]), gpr('Rn')],
          flags=ALIAS + ' | ARM_ENTRY_ASSEMBLE_ONLY')
    E('mov', '0 1 1 01110000 imm5 0 imm4 1 Rn Rd', [elem('Rd', 'imm5'), elem('Rn', 'imm4')],
      flags=ALIAS)
    E('ins', '0 1 1 01110000 imm5 0 imm4 1 Rn Rd', [elem('Rd', 'imm5'), elem('Rn', 'imm4')],
      flags=ALIAS + ' | ARM_ENTRY_ASSEMBLE_ONLY')


def permute_extract_table():
    # 0 Q 0 01110 size 0 Rm 0 opcode 10 Rn Rd
    for opcode, name in [('001', 'uzp1'), ('010', 'trn1'), ('011', 'zip1'),
                         ('101', 'uzp2'), ('110', 'trn2'), ('111', 'zip2')]:
        E(name, '0 Q 0 01110 size 0 Rm 0 %s 10 Rn Rd' % opcode,
          [vec('Rd', 'q_size'), vec('Rn', 'q_size'), vec('Rm', 'q_size')])
    # ext: 0 Q 10 1110 00 0 Rm 0 imm4 0 Rn Rd
    E('ext', '0 Q 10 1110 00 0 Rm 0 imm4 0 Rn Rd',
      [vec('Rd', 'q_bytes'), vec('Rn', 'q_bytes'), vec('Rm', 'q_bytes'), imm('imm4', hex=True)],
      cond='$Q == 1 || ($imm4 & 8) == 0')
    # tbl / tbx: 0 Q 001110 000 Rm 0 len op 00 Rn Rd
    for op, name in [(0, 'tbl'), (1, 'tbx')]:
        for length in range(4):
            E(name, '0 Q 001110 000 Rm 0 %s %d 00 Rn Rd' % (format(length, '02b'), op),
              [vec('Rd', 'q_bytes'), vlist('Rn', length + 1, 'fixed_16b'), vec('Rm', 'q_bytes')])


# ---------------------------------------------------------------------------
# Three register extension

def three_reg_extension():
    # 0 Q U 01110 size 0 Rm 1 opcode(4) 1 Rn Rd
    def ext(name, u, size, opcode, ops, feat, q=None):
        qbit = 'Q' if q is None else str(q)
        E(name, '0 %s %d 01110 %s 0 Rm 1 %s 1 Rn Rd' % (qbit, u, size, opcode), ops, feat=feat)

    for u, opcode, name in [(1, '0000', 'sqrdmlah'), (1, '0001', 'sqrdmlsh')]:
        ext(name, u, 'size', opcode, [vec('Rd', 'q_size', HS), vec('Rn', 'q_size', HS),
                                      vec('Rm', 'q_size', HS)], 'FEAT_RDM')
    for u, name in [(0, 'sdot'), (1, 'udot')]:
        ext(name, u, '10', '0010', [vec('Rd', 'q_single'), vec('Rn', 'q_bytes'),
                                    vec('Rm', 'q_bytes')], 'FEAT_DOTPROD')
    ext('usdot', 0, '10', '0011', [vec('Rd', 'q_single'), vec('Rn', 'q_bytes'),
                                   vec('Rm', 'q_bytes')], 'FEAT_I8MM')
    for u, opcode, name in [(0, '0100', 'smmla'), (1, '0100', 'ummla'), (0, '0101', 'usmmla')]:
        ext(name, u, '10', opcode, [vecf('Rd', '4S'), vecf('Rn', '16B'), vecf('Rm', '16B')],
            'FEAT_I8MM', q=1)
    # Complex numbers: fcmla 110 rot, fcadd 111 rot 0
    ext('fcmla', 1, '01', '10 rot', [vec('Rd', 'q_half'), vec('Rn', 'q_half'), vec('Rm', 'q_half'),
                                     rotation('rot', 'mla')], 'FEAT_FP16')
    ext('fcmla', 1, '1 sz', '10 rot', [vec('Rd', 'q_sz'), vec('Rn', 'q_sz'), vec('Rm', 'q_sz'),
                                       rotation('rot', 'mla')], 'FEAT_FCMA')
    ext('fcadd', 1, '01', '11 rot1 0', [vec('Rd', 'q_half'), vec('Rn', 'q_half'),
                                        vec('Rm', 'q_half'), rotation('rot1', 'add')], 'FEAT_FP16')
    ext('fcadd', 1, '1 sz', '11 rot1 0', [vec('Rd', 'q_sz'), vec('Rn', 'q_sz'),
                                          vec('Rm', 'q_sz'), rotation('rot1', 'add')], 'FEAT_FCMA')
    # BFloat16
    ext('bfdot', 1, '01', '1111', [vec('Rd', 'q_single'), vec('Rn', 'q_half'),
                                   vec('Rm', 'q_half')], 'FEAT_BF16')
    ext('bfmmla', 1, '01', '1101', [vecf('Rd', '4S'), vecf('Rn', '8H'), vecf('Rm', '8H')],
        'FEAT_BF16', q=1)
    for q, name in [(0, 'bfmlalb'), (1, 'bfmlalt')]:
        ext(name, 1, '11', '1111', [vecf('Rd', '4S'), vecf('Rn', '8H'), vecf('Rm', '8H')],
            'FEAT_BF16', q=q)
    # FP8
    ext('fdot', 0, '00', '1111', [vec('Rd', 'q_single'), vec('Rn', 'q_bytes'), vec('Rm', 'q_bytes')],
        'FEAT_FP8DOT4')
    ext('fdot', 0, '01', '1111', [vec('Rd', 'q_half'), vec('Rn', 'q_bytes'), vec('Rm', 'q_bytes')],
        'FEAT_FP8DOT2')
    for q, name in [(0, 'fmlalb'), (1, 'fmlalt')]:
        ext(name, 0, '11', '1111', [vecf('Rd', '8H'), vecf('Rn', '16B'), vecf('Rm', '16B')],
            'FEAT_FP8FMA', q=q)
    for q, size, name in [(0, '00', 'fmlallbb'), (0, '01', 'fmlallbt'),
                          (1, '00', 'fmlalltb'), (1, '01', 'fmlalltt')]:
        ext(name, 0, size, '1000', [vecf('Rd', '4S'), vecf('Rn', '16B'), vecf('Rm', '16B')],
            'FEAT_FP8FMA', q=q)
    # fcvtn of two vectors to FP8
    E('fcvtn', '0 0 0 01110 000 Rm 1 1110 1 Rn Rd',
      [vecf('Rd', '8B'), vecf('Rn', '4S'), vecf('Rm', '4S')], feat='FEAT_FP8')
    E('fcvtn2', '0 1 0 01110 000 Rm 1 1110 1 Rn Rd',
      [vecf('Rd', '16B'), vecf('Rn', '4S'), vecf('Rm', '4S')], feat='FEAT_FP8')
    E('fcvtn', '0 0 0 01110 010 Rm 1 1110 1 Rn Rd',
      [vecf('Rd', '8B'), vecf('Rn', '4H'), vecf('Rm', '4H')], feat='FEAT_FP8')
    E('fcvtn', '0 1 0 01110 010 Rm 1 1110 1 Rn Rd',
      [vecf('Rd', '16B'), vecf('Rn', '8H'), vecf('Rm', '8H')], feat='FEAT_FP8')
    # fmmla of FP8
    E('fmmla', '0 1 1 01110 100 Rm 1 1101 1 Rn Rd',
      [vecf('Rd', '4S'), vecf('Rn', '16B'), vecf('Rm', '16B')], feat='FEAT_F8F32MM')
    E('fmmla', '0 1 1 01110 000 Rm 1 1101 1 Rn Rd',
      [vecf('Rd', '8H'), vecf('Rn', '16B'), vecf('Rm', '16B')], feat='FEAT_F8F16MM')


def simd_vector_rest():
    across_lanes()
    copy()
    permute_extract_table()
    three_reg_extension()


# ---------------------------------------------------------------------------
# By element

def by_element():
    # 0 Q U 01111 size L M Rm4 opcode H 0 Rn Rd. The codec reads the H, L and
    # M bits and the register (4 or 5 bits) itself
    def pat(q, u, size, opcode):
        return '0 %s %d 01111 %s L M Rm4 %s H 0 Rn Rd' % (q, u, size, opcode)

    # Integer: mul, mla, mls, sqdmulh, sqrdmulh (+ RDM)
    for u, opcode, name, feat in [(0, '1000', 'mul', BASE), (1, '0000', 'mla', BASE),
                                  (1, '0100', 'mls', BASE), (0, '1100', 'sqdmulh', BASE),
                                  (0, '1101', 'sqrdmulh', BASE), (1, '1101', 'sqrdmlah', 'FEAT_RDM'),
                                  (1, '1111', 'sqrdmlsh', 'FEAT_RDM')]:
        E(name, pat('Q', u, 'size', opcode),
          [vec('Rd', 'q_size', HS), vec('Rn', 'q_size', HS), elem('Rm4', 'by_size')], feat=feat)
    # Long
    for u, opcode, name in [(0, '0010', 'smlal'), (0, '0110', 'smlsl'), (0, '1010', 'smull'),
                            (0, '0011', 'sqdmlal'), (0, '0111', 'sqdmlsl'), (0, '1011', 'sqdmull'),
                            (1, '0010', 'umlal'), (1, '0110', 'umlsl'), (1, '1010', 'umull')]:
        for q in (0, 1):
            E(name + q_suffix(q), pat(q, u, 'size', opcode),
              [vec('Rd', 'long_size', HS), vec('Rn', 'q_size', HS), elem('Rm4', 'by_size')])
    # Floating point
    for u, opcode, name in [(0, '0001', 'fmla'), (0, '0101', 'fmls'), (0, '1001', 'fmul'),
                            (1, '1001', 'fmulx')]:
        E(name, pat('Q', u, '1 sz', opcode),
          [vec('Rd', 'q_sz'), vec('Rn', 'q_sz'), elem('Rm4', 'by_sz')])
        E(name, pat('Q', u, '00', opcode),
          [vec('Rd', 'q_half'), vec('Rn', 'q_half'), elem('Rm4', 'half')], feat='FEAT_FP16')
    # Complex numbers: 0 Q 1 01111 size L M Rm 0 rot 1 H 0
    # (half precision: the index of the four lanes vector has H = 0; single
    # precision: the vector of four only)
    E('fcmla', '0 0 1 01111 01 L M Rm4 0 rot 1 0 0 Rn Rd',
      [vec('Rd', 'q_half'), vec('Rn', 'q_half'), elem('Rm4', 'fcmla'), rotation('rot', 'mla')],
      feat='FEAT_FP16')
    E('fcmla', '0 1 1 01111 01 L M Rm4 0 rot 1 H 0 Rn Rd',
      [vec('Rd', 'q_half'), vec('Rn', 'q_half'), elem('Rm4', 'fcmla'), rotation('rot', 'mla')],
      feat='FEAT_FP16')
    E('fcmla', '0 1 1 01111 10 L M Rm4 0 rot 1 H 0 Rn Rd',
      [vecf('Rd', '4S'), vecf('Rn', '4S'), elem('Rm4', 'fcmla'), rotation('rot', 'mla')],
      feat='FEAT_FCMA')
    # Dot product and matrix multiply
    for u, size, opcode, name, feat in [(0, '10', '1110', 'sdot', 'FEAT_DOTPROD'),
                                        (1, '10', '1110', 'udot', 'FEAT_DOTPROD'),
                                        (0, '10', '1111', 'usdot', 'FEAT_I8MM'),
                                        (0, '00', '1111', 'sudot', 'FEAT_I8MM')]:
        E(name, pat('Q', u, size, opcode),
          [vec('Rd', 'q_single'), vec('Rn', 'q_bytes'), elem('Rm4', 'dot4b')], feat=feat)
    E('bfdot', pat('Q', 0, '01', '1111'),
      [vec('Rd', 'q_single'), vec('Rn', 'q_half'), elem('Rm4', 'pair2h')], feat='FEAT_BF16')
    for q, name in [(0, 'bfmlalb'), (1, 'bfmlalt')]:
        E(name, pat(q, 0, '11', '1111'),
          [vecf('Rd', '4S'), vecf('Rn', '8H'), elem('Rm4', 'half')], feat='FEAT_BF16')
    # FEAT_FHM
    for u, opcode, name in [(0, '0000', 'fmlal'), (0, '0100', 'fmlsl'),
                            (1, '1000', 'fmlal2'), (1, '1100', 'fmlsl2')]:
        E(name, pat('Q', u, '10', opcode),
          [vec('Rd', 'q_single'), vec('Rn', 'q_2h4h'), elem('Rm4', 'half')], feat='FEAT_FHM')
    # FP8
    E('fdot', pat('Q', 0, '00', '0000'),
      [vec('Rd', 'q_single'), vec('Rn', 'q_bytes'), elem('Rm4', 'dot4b')], feat='FEAT_FP8DOT4')
    E('fdot', pat('Q', 0, '01', '0000'),
      [vec('Rd', 'q_half'), vec('Rn', 'q_bytes'), elem('Rm4', 'pair2b')], feat='FEAT_FP8DOT2')
    for q, name in [(0, 'fmlalb'), (1, 'fmlalt')]:
        E(name, pat(q, 0, '11', '0000'),
          [vecf('Rd', '8H'), vecf('Rn', '16B'), elem('Rm4', 'byte')], feat='FEAT_FP8FMA')
    for q, size, name in [(0, '00', 'fmlallbb'), (0, '01', 'fmlallbt'),
                          (1, '00', 'fmlalltb'), (1, '01', 'fmlalltt')]:
        E(name, pat(q, 1, size, '1000'),
          [vecf('Rd', '4S'), vecf('Rn', '16B'), elem('Rm4', 'byte')], feat='FEAT_FP8FMA')


# ---------------------------------------------------------------------------
# Shift by immediate and modified immediate

# (U, opcode, mnemonic): the shifts of the same element size
SHIFT_RIGHT = [(0, '00000', 'sshr'), (0, '00010', 'ssra'), (0, '00100', 'srshr'),
               (0, '00110', 'srsra'), (1, '00000', 'ushr'), (1, '00010', 'usra'),
               (1, '00100', 'urshr'), (1, '00110', 'ursra'), (1, '01000', 'sri')]
SHIFT_LEFT = [(0, '01010', 'shl'), (0, '01110', 'sqshl'), (1, '01010', 'sli'),
              (1, '01100', 'sqshlu'), (1, '01110', 'uqshl')]
SHIFT_NARROW = [(0, '10000', 'shrn'), (0, '10001', 'rshrn'), (0, '10010', 'sqshrn'),
                (0, '10011', 'sqrshrn'), (1, '10000', 'sqshrun'), (1, '10001', 'sqrshrun'),
                (1, '10010', 'uqshrn'), (1, '10011', 'uqrshrn')]
FIXED_POINT = [(0, '11100', 'scvtf'), (1, '11100', 'ucvtf'), (0, '11111', 'fcvtzs'),
               (1, '11111', 'fcvtzu')]
# The element sizes of the fixed point conversions: immh pattern, feature
FIXED_POINT_SIZES = [('001 hx', 'FEAT_FP16'), ('01 hx:2', BASE), ('1 hx:3', BASE)]


def shift_immediate():
    # 0 Q U 011110 immh immb opcode 1 Rn Rd
    for u, opcode, name in SHIFT_RIGHT:
        E(name, '0 Q %d 011110 immh immb %s 1 Rn Rd' % (u, opcode),
          [vec('Rd', 'q_immh'), vec('Rn', 'q_immh'), shiftimm(right=True, hex_=True)])
    for u, opcode, name in SHIFT_LEFT:
        E(name, '0 Q %d 011110 immh immb %s 1 Rn Rd' % (u, opcode),
          [vec('Rd', 'q_immh'), vec('Rn', 'q_immh'), shiftimm(hex_=True)])
    for u, opcode, name in SHIFT_NARROW:
        for q in (0, 1):
            E(name + q_suffix(q), '0 %d %d 011110 immh immb %s 1 Rn Rd' % (q, u, opcode),
              [vec('Rd', 'q_immh', flags='1'), vec('Rn', 'immh_long'), shiftimm(right=True, hex_=True)])
    for u, name, extend in [(0, 'sshll', 'sxtl'), (1, 'ushll', 'uxtl')]:
        for q in (0, 1):
            E(name + q_suffix(q), '0 %d %d 011110 immh immb 10100 1 Rn Rd' % (q, u),
              [vec('Rd', 'immh_long'), vec('Rn', 'q_immh', flags='1'), shiftimm(hex_=True)],
              flags=ALIAS)
            # sxtl / uxtl: the shift is zero, LLVM prints sshll #0
            E(extend + q_suffix(q), '0 %d %d 011110 immh 000 10100 1 Rn Rd' % (q, u),
              [vec('Rd', 'immh_long'), vec('Rn', 'q_immh', flags='1')],
              flags=ALIAS + ' | ARM_ENTRY_ASSEMBLE_ONLY')
    for u, opcode, name in FIXED_POINT:
        for immh, feat in FIXED_POINT_SIZES:
            E(name, '0 Q %d 011110 %s immb %s 1 Rn Rd' % (u, immh, opcode),
              [vec('Rd', 'q_immh'), vec('Rn', 'q_immh'), fbits_vec()], feat=feat)


def modified_immediate():
    # 0 Q op 0111100000 abc cmode 01 defgh Rd
    def pat(op, cmode):
        return '0 Q %d 0111100000 abc %s 01 defgh Rd' % (op, cmode)

    # (mnemonic, op, cmode bits, scheme, arrangement scheme)
    forms = []
    for op, movi, orr in [(0, 'movi', 'orr'), (1, 'mvni', 'bic')]:
        forms.append((movi, op, '0 c:2 0', '32_lsl', 'q_single'))
        forms.append((movi, op, '10 c:1 0', '16_lsl', 'q_half'))
        forms.append((movi, op, '110 c:1', '32_msl', 'q_single'))
        forms.append((orr, op, '0 c:2 1', '32_lsl', 'q_single'))
        forms.append((orr, op, '10 c:1 1', '16_lsl', 'q_half'))
    forms.append(('movi', 0, '1110', '8', 'q_bytes'))
    for name, op, cmode, scheme, arrangement in forms:
        E(name, pat(op, cmode), [vec('Rd', arrangement), modimm(scheme)])
    # The 64 bit forms: the scalar movi d0 and movi v0.2d
    E('movi', '0 0 1 0111100000 abc 1110 01 defgh Rd', [sfp('Rd', 'd'), modimm('64')])
    E('movi', '0 1 1 0111100000 abc 1110 01 defgh Rd', [vecf('Rd', '2D'), modimm('64')])
    # fmov (vector, immediate)
    E('fmov', '0 Q 0 0111100000 abc 1111 01 defgh Rd', [vec('Rd', 'q_single'), fimm('s', split=True)])
    E('fmov', '0 1 1 0111100000 abc 1111 01 defgh Rd', [vecf('Rd', '2D'), fimm('d', split=True)])
    E('fmov', '0 Q 0 0111100000 abc 1111 11 defgh Rd', [vec('Rd', 'q_half'), fimm('h', split=True)],
      feat='FEAT_FP16')


def simd_vector_all():
    simd_vector()
    by_element()
    shift_immediate()
    modified_immediate()
    simd_scalar()
    lookup_tables()


# ---------------------------------------------------------------------------
# The scalar Advanced SIMD instructions (bits 31-30 are 01)

# (U, opcode, mnemonic, the valid sizes): d is size 3
SCALAR_THREE_SAME = [
    (0, '00001', 'sqadd', None), (0, '00101', 'sqsub', None), (0, '00110', 'cmgt', [D]),
    (0, '00111', 'cmge', [D]), (0, '01000', 'sshl', [D]), (0, '01001', 'sqshl', None),
    (0, '01010', 'srshl', [D]), (0, '01011', 'sqrshl', None), (0, '10000', 'add', [D]),
    (0, '10001', 'cmtst', [D]), (0, '10110', 'sqdmulh', HS),
    (1, '00001', 'uqadd', None), (1, '00101', 'uqsub', None), (1, '00110', 'cmhi', [D]),
    (1, '00111', 'cmhs', [D]), (1, '01000', 'ushl', [D]), (1, '01001', 'uqshl', None),
    (1, '01010', 'urshl', [D]), (1, '01011', 'uqrshl', None), (1, '10000', 'sub', [D]),
    (1, '10001', 'cmeq', [D]), (1, '10110', 'sqrdmulh', HS),
]
# (U, o1, opcode, mnemonic)
SCALAR_THREE_SAME_FP = [
    (0, 0, '11011', 'fmulx'), (0, 0, '11100', 'fcmeq'), (0, 0, '11111', 'frecps'),
    (0, 1, '11111', 'frsqrts'), (1, 0, '11100', 'fcmge'), (1, 1, '11100', 'fcmgt'),
    (1, 0, '11101', 'facge'), (1, 1, '11101', 'facgt'), (1, 1, '11010', 'fabd'),
]
SCALAR_THREE_SAME_FP16 = [
    (0, 0, '011', 'fmulx'), (0, 0, '100', 'fcmeq'), (0, 0, '111', 'frecps'),
    (0, 1, '111', 'frsqrts'), (1, 0, '100', 'fcmge'), (1, 0, '101', 'facge'),
    (1, 1, '010', 'fabd'), (1, 1, '100', 'fcmgt'), (1, 1, '101', 'facgt'),
]


def scalar_three_same():
    for u, opcode, name, sizes in SCALAR_THREE_SAME:
        regs = [sfp(f, 'size_mask', sizes) for f in ('Rd', 'Rn', 'Rm')]
        pattern = '0 1 %d 11110 size 1 Rm %s 1 Rn Rd' % (u, opcode)
        E(name, pattern, regs, flags=ALIAS if name in SWAPPED_COMPARES else None)
        swapped(name, pattern, regs)
    for u, o1, opcode, name in SCALAR_THREE_SAME_FP:
        pattern = '0 1 %d 11110 %d sz 1 Rm %s 1 Rn Rd' % (u, o1, opcode)
        regs = [sfp('Rd', 'sz'), sfp('Rn', 'sz'), sfp('Rm', 'sz')]
        E(name, pattern, regs, flags=ALIAS if name in SWAPPED_COMPARES else None)
        swapped(name, pattern, regs)
    for u, a, opcode, name in SCALAR_THREE_SAME_FP16:
        pattern = '0 1 %d 11110 %d 1 0 Rm 00 %s 1 Rn Rd' % (u, a, opcode)
        regs = [sfp('Rd', 'h'), sfp('Rn', 'h'), sfp('Rm', 'h')]
        E(name, pattern, regs, feat='FEAT_FP16', flags=ALIAS if name in SWAPPED_COMPARES else None)
        swapped(name, pattern, regs, 'FEAT_FP16')
    # FEAT_RDM: sqrdmlah / sqrdmlsh of the scalar registers, three register
    # extension class: 0 1 1 11110 size 0 Rm 1 opcode 1
    for opcode, name in [('0000', 'sqrdmlah'), ('0001', 'sqrdmlsh')]:
        E(name, '0 1 1 11110 size 0 Rm 1 %s 1 Rn Rd' % opcode,
          [sfp('Rd', 'size_mask', HS), sfp('Rn', 'size_mask', HS), sfp('Rm', 'size_mask', HS)],
          feat='FEAT_RDM')


def scalar_three_different():
    for opcode, name in [('1001', 'sqdmlal'), ('1011', 'sqdmlsl'), ('1101', 'sqdmull')]:
        E(name, '0 1 0 11110 size 1 Rm %s 00 Rn Rd' % opcode,
          [sfp('Rd', 'long_mask', HS), sfp('Rn', 'size_mask', HS), sfp('Rm', 'size_mask', HS)])


# (U, opcode, mnemonic, the valid sizes, with #0)
SCALAR_TWO_MISC = [
    (0, '00011', 'suqadd', None, False), (0, '00111', 'sqabs', None, False),
    (0, '01000', 'cmgt', [D], True), (0, '01001', 'cmeq', [D], True),
    (0, '01010', 'cmlt', [D], True), (0, '01011', 'abs', [D], False),
    (1, '00011', 'usqadd', None, False), (1, '00111', 'sqneg', None, False),
    (1, '01000', 'cmge', [D], True), (1, '01001', 'cmle', [D], True),
    (1, '01011', 'neg', [D], False),
]
# (U, o1, opcode, mnemonic, has a half precision form)
SCALAR_TWO_MISC_FP = [
    (0, 0, '11010', 'fcvtns', True), (0, 1, '11010', 'fcvtps', True),
    (0, 0, '11011', 'fcvtms', True), (0, 1, '11011', 'fcvtzs', True),
    (0, 0, '11100', 'fcvtas', True), (0, 0, '11101', 'scvtf', True),
    (0, 1, '11101', 'frecpe', True), (0, 1, '11111', 'frecpx', True),
    (1, 0, '11010', 'fcvtnu', True), (1, 1, '11010', 'fcvtpu', True),
    (1, 0, '11011', 'fcvtmu', True), (1, 1, '11011', 'fcvtzu', True),
    (1, 0, '11100', 'fcvtau', True), (1, 0, '11101', 'ucvtf', True),
    (1, 1, '11101', 'frsqrte', True),
]


def scalar_two_reg_misc():
    # 0 1 U 11110 size 10000 opcode 10 Rn Rd
    for u, opcode, name, sizes, with_zero in SCALAR_TWO_MISC:
        ops = [sfp('Rd', 'size_mask', sizes), sfp('Rn', 'size_mask', sizes)]
        if with_zero:
            ops.append(zero())
        E(name, '0 1 %d 11110 size 10000 %s 10 Rn Rd' % (u, opcode), ops)
    for u, opcode, name in [(0, '10100', 'sqxtn'), (1, '10010', 'sqxtun'), (1, '10100', 'uqxtn')]:
        E(name, '0 1 %d 11110 size 10000 %s 10 Rn Rd' % (u, opcode),
          [sfp('Rd', 'size_mask', BHS), sfp('Rn', 'long_mask', BHS)])
    for u, o1, opcode, name, has_half in SCALAR_TWO_MISC_FP:
        E(name, '0 1 %d 11110 %d sz 10000 %s 10 Rn Rd' % (u, o1, opcode),
          [sfp('Rd', 'sz'), sfp('Rn', 'sz')])
        E(name, '0 1 %d 11110 %d 111100 %s 10 Rn Rd' % (u, o1, opcode),
          [sfp('Rd', 'h'), sfp('Rn', 'h')], feat='FEAT_FP16')
    for u, o1, opcode, name in TWO_MISC_FP_ZERO:
        E(name, '0 1 %d 11110 %d sz 10000 %s 10 Rn Rd' % (u, o1, opcode),
          [sfp('Rd', 'sz'), sfp('Rn', 'sz'), fzero(integer=True)])
        E(name, '0 1 %d 11110 %d 111100 %s 10 Rn Rd' % (u, o1, opcode),
          [sfp('Rd', 'h'), sfp('Rn', 'h'), fzero('h', integer=True)], feat='FEAT_FP16')
    E('fcvtxn', '0 1 1 11110 0 1 10000 10110 10 Rn Rd', [sfp('Rd', 's'), sfp('Rn', 'd')])


def scalar_pairwise():
    # 0 1 U 11110 size 11000 opcode 10 Rn Rd
    E('addp', '0 1 0 11110 11 11000 11011 10 Rn Rd', [sfp('Rd', 'd'), vecf('Rn', '2D')])
    for o1, opcode, name in [(0, '01100', 'fmaxnmp'), (1, '01100', 'fminnmp'),
                             (0, '01101', 'faddp'), (0, '01111', 'fmaxp'), (1, '01111', 'fminp')]:
        E(name, '0 1 1 11110 %d 0 11000 %s 10 Rn Rd' % (o1, opcode),
          [sfp('Rd', 's'), vecf('Rn', '2S')])
        E(name, '0 1 1 11110 %d 1 11000 %s 10 Rn Rd' % (o1, opcode),
          [sfp('Rd', 'd'), vecf('Rn', '2D')])
        E(name, '0 1 0 11110 %d 0 11000 %s 10 Rn Rd' % (o1, opcode),
          [sfp('Rd', 'h'), vecf('Rn', '2H')], feat='FEAT_FP16')


def scalar_by_element():
    # 0 1 U 11111 size L M Rm4 opcode H 0 Rn Rd
    def pat(u, size, opcode):
        return '0 1 %d 11111 %s L M Rm4 %s H 0 Rn Rd' % (u, size, opcode)

    for opcode, name in [('0011', 'sqdmlal'), ('0111', 'sqdmlsl'), ('1011', 'sqdmull')]:
        E(name, pat(0, 'size', opcode),
          [sfp('Rd', 'long_mask', HS), sfp('Rn', 'size_mask', HS), elem('Rm4', 'by_size')])
    for u, opcode, name, feat in [(0, '1100', 'sqdmulh', BASE), (0, '1101', 'sqrdmulh', BASE),
                                  (1, '1101', 'sqrdmlah', 'FEAT_RDM'),
                                  (1, '1111', 'sqrdmlsh', 'FEAT_RDM')]:
        E(name, pat(u, 'size', opcode),
          [sfp('Rd', 'size_mask', HS), sfp('Rn', 'size_mask', HS), elem('Rm4', 'by_size')],
          feat=feat)
    for u, opcode, name in [(0, '0001', 'fmla'), (0, '0101', 'fmls'), (0, '1001', 'fmul'),
                            (1, '1001', 'fmulx')]:
        E(name, pat(u, '1 sz', opcode), [sfp('Rd', 'sz'), sfp('Rn', 'sz'), elem('Rm4', 'by_sz')])
        E(name, pat(u, '00', opcode), [sfp('Rd', 'h'), sfp('Rn', 'h'), elem('Rm4', 'half')],
          feat='FEAT_FP16')


def scalar_shift_immediate():
    # 0 1 U 111110 immh immb opcode 1 Rn Rd
    def pat(u, immh, opcode):
        return '0 1 %d 111110 %s immb %s 1 Rn Rd' % (u, immh, opcode)

    # The right shifts and sli / sri / shl take d only (immh 1xxx)
    for u, opcode, name in SHIFT_RIGHT:
        E(name, pat(u, '1 hx:3', opcode),
          [sfp('Rd', 'd'), sfp('Rn', 'd'), shiftimm(right=True, hex_=True)])
    for u, opcode, name in [(0, '01010', 'shl'), (1, '01010', 'sli')]:
        E(name, pat(u, '1 hx:3', opcode), [sfp('Rd', 'd'), sfp('Rn', 'd'), shiftimm(hex_=True)])
    # The saturating shifts take every size
    for u, opcode, name in [(0, '01110', 'sqshl'), (1, '01100', 'sqshlu'), (1, '01110', 'uqshl')]:
        E(name, pat(u, 'immh', opcode),
          [sfp('Rd', 'immh'), sfp('Rn', 'immh'), shiftimm(hex_=True)])
    for u, opcode, name in SHIFT_NARROW:
        if name in ('shrn', 'rshrn'):
            continue
        E(name, pat(u, 'immh', opcode),
          [sfp('Rd', 'immh_narrow'), sfp('Rn', 'immh_wide'), shiftimm(right=True, hex_=True)])
    for u, opcode, name in FIXED_POINT:
        for immh, feat in FIXED_POINT_SIZES:
            E(name, pat(u, immh, opcode),
              [sfp('Rd', 'immh'), sfp('Rn', 'immh'), fbits_vec()], feat=feat)


def scalar_copy():
    E('mov', '0 1 0 11110000 imm5 0 0000 1 Rn Rd', [sfp('Rd', 'imm5'), elem('Rn', 'imm5')],
      flags=ALIAS)
    E('dup', '0 1 0 11110000 imm5 0 0000 1 Rn Rd', [sfp('Rd', 'imm5'), elem('Rn', 'imm5')],
      flags=ALIAS + ' | ARM_ENTRY_ASSEMBLE_ONLY')


def simd_scalar():
    scalar_three_same()
    scalar_three_different()
    scalar_two_reg_misc()
    scalar_pairwise()
    scalar_by_element()
    scalar_shift_immediate()
    scalar_copy()


# ---------------------------------------------------------------------------
# FEAT_LUT

def lookup_tables():
    # luti2 / luti4: the index is the element of the last operand
    def lut(width, position):
        return '%d' % (width | ((position - 10) << 2))

    E('luti2', '0 1 0 01110 10 0 Rm 0 i:2 1 00 Rn Rd',
      [vecf('Rd', '16B'), vlist('Rn', 1, 'fixed_16b'), elem('Rm', 'lut', flags=lut(2, 13))],
      feat='FEAT_LUT')
    E('luti2', '0 1 0 01110 11 0 Rm 0 i:3 00 Rn Rd',
      [vecf('Rd', '8H'), vlist('Rn', 1, 'fixed_8h'), elem('Rm', 'lut', flags=lut(3, 12))],
      feat='FEAT_LUT')
    E('luti4', '0 1 0 01110 01 0 Rm 0 i:1 1 0 00 Rn Rd',
      [vecf('Rd', '16B'), vlist('Rn', 1, 'fixed_16b'), elem('Rm', 'lut', flags=lut(1, 14))],
      feat='FEAT_LUT')
    E('luti4', '0 1 0 01110 01 0 Rm 0 i:2 1 00 Rn Rd',
      [vecf('Rd', '8H'), vlist('Rn', 2, 'fixed_8h'), elem('Rm', 'lut', flags=lut(2, 13))],
      feat='FEAT_LUT')
