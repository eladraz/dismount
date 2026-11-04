"""A64 Advanced SIMD structure loads and stores: ld1-ld4 / st1-st4 of whole
vectors and of one element, the replicating ld1r-ld4r, and (FEAT_LRCPC3)
ldap1 / stl1."""
import a64dsl
from a64dsl import *
from a64simd_dsl import *

# The bytes of the element sizes of the single structure forms
ELEMENT_BYTES = {'b': 1, 'h': 2, 's': 4, 'd': 8}


def multiple():
    """The multiple structures: ld1 of one to four registers, ld2, ld3, ld4."""
    for opcode, base, count in [('0111', 'ld1', 1), ('1010', 'ld1', 2), ('0110', 'ld1', 3),
                                ('0010', 'ld1', 4), ('1000', 'ld2', 2), ('0100', 'ld3', 3),
                                ('0000', 'ld4', 4)]:
        # The 1d arrangement is valid for ld1 only
        scheme = 'q_size_1d' if base == 'ld1' else 'q_size'
        for l, name in [(1, base), (0, base.replace('ld', 'st'))]:
            registers = vlist('Rt', count, scheme, size10=True)
            # 0 Q 0011000 L 000000 opcode size Rn Rt
            E(name, '0 Q 0011000 %d 000000 %s size Rn Rt' % (l, opcode),
              [registers, base_mem()])
            # The post index: 0 Q 0011001 L 0 Rm opcode size Rn Rt, the
            # immediate form has Rm = 31
            E(name, '0 Q 0011001 %d 0 11111 %s size Rn Rt' % (l, opcode),
              [registers, post_multi(count)])
            E(name, '0 Q 0011001 %d 0 Rm %s size Rn Rt' % (l, opcode),
              [registers, post_reg()])


def single():
    """One element of 1-4 registers, and the replicating loads."""
    # ld1 / ld2 / ld3 / ld4: (mnemonic, R, the low bit of the opcode)
    cases = [('ld1', 0, 0, 1), ('ld2', 1, 0, 2), ('ld3', 0, 1, 3), ('ld4', 1, 1, 4)]
    for base, r, odd, count in cases:
        for element, opcode_hi, size_pattern, s_pattern in [
                ('b', '00', 'size', 'S'), ('h', '01', 'i1:1 0', 'S'),
                ('s', '10', '00', 'S'), ('d', '10', '01', '0')]:
            # 0 Q 0011010 L R 00000 opcode S size Rn Rt
            opcode = opcode_hi + str(odd)
            registers = vlist('Rt', count, 'elem_' + element, element=True)
            tail = '%s %s %s Rn Rt' % (opcode, s_pattern, size_pattern)
            for l, name in [(1, base), (0, base.replace('ld', 'st'))]:
                E(name, '0 Q 0011010 %d %d 00000 %s' % (l, r, tail),
                  [registers, base_mem()])
                E(name, '0 Q 0011011 %d %d 11111 %s' % (l, r, tail),
                  [registers, post_fixed(ELEMENT_BYTES[element] * count)])
                E(name, '0 Q 0011011 %d %d Rm %s' % (l, r, tail),
                  [registers, post_reg()])
    # The replicating loads: 0 Q 0011010 1 R 00000 11 odd 0 size Rn Rt
    for base, r, odd, count in cases:
        registers = vlist('Rt', count, 'q_size_1d', size10=True)
        E(base + 'r', '0 Q 0011010 1 %d 00000 11%d 0 size Rn Rt' % (r, odd),
          [registers, base_mem()])
        # The post index immediate is the bytes of the registers: it depends on
        # the size field
        for size, element_bytes in [('00', 1), ('01', 2), ('10', 4), ('11', 8)]:
            E(base + 'r', '0 Q 0011011 1 %d 11111 11%d 0 %s Rn Rt' % (r, odd, size),
              [registers, post_fixed(element_bytes * count)])
        E(base + 'r', '0 Q 0011011 1 %d Rm 11%d 0 size Rn Rt' % (r, odd),
          [registers, post_reg()])
    # FEAT_LRCPC3: ldap1 / stl1 of a 64 bit element, with Rm = 1
    for l, name in [(1, 'ldap1'), (0, 'stl1')]:
        E(name, '0 Q 0011010 %d 0 00001 100 0 01 Rn Rt' % l,
          [vlist('Rt', 1, 'elem_d', element=True), base_mem()], feat='FEAT_LRCPC3')


def simd_memory():
    multiple()
    single()
