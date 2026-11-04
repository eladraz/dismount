"""SVE loads, stores and prefetches: contiguous (scalar plus immediate / scalar),
first fault, non fault, non temporal, structure (ld2-ld4), replicating
(ld1r, ld1rq, ld1ro), the gather loads and scatter stores with all their
vector and scalar addressing forms, ldr / str of z and p, and prfb... prfd."""
from a64dsl_sve import *

# The data types of the contiguous loads: (dtype, name, element, memory size log2)
DTYPES = [(0, 'b', 'B', 0), (1, 'b', 'H', 0), (2, 'b', 'S', 0), (3, 'b', 'D', 0),
          (4, 'sw', 'D', 2), (5, 'h', 'H', 1), (6, 'h', 'S', 1), (7, 'h', 'D', 1),
          (8, 'sh', 'D', 1), (9, 'sh', 'S', 1), (10, 'w', 'S', 2), (11, 'w', 'D', 2),
          (12, 'sb', 'D', 0), (13, 'sb', 'S', 0), (14, 'sb', 'H', 0), (15, 'd', 'D', 3)]

MSZ_NAMES = ['b', 'h', 'w', 'd']


def groups():
    contiguous_loads()
    replicating_loads()
    structure_loads()
    non_temporal()
    stores()
    gathers()
    scatters()
    prefetches()
    spills()


def bits(value, width):
    return format(value, '0%db' % width)


def pg3z():
    return P('Pg', q='z', w=3)


def pg3():
    return P('Pg', w=3)


def contiguous_loads():
    for dtype, name, es, msz in DTYPES:
        d = bits(dtype, 4)
        mem_imm = MEM('MUL_VL4', 1)
        ld1 = 'ld1' + name
        # scalar plus immediate: [xn, #imm, mul vl]
        E(ld1, '1010010 %s 0 imm4 101 Pg:3 Rn Zt' % d,
          [ZL('Zt', 1, es), pg3z(), mem_imm])
        # scalar plus scalar
        E(ld1, '1010010 %s Rm 010 Pg:3 Rn Zt' % d, [ZL('Zt', 1, es), pg3z(), MEM('SCALAR_SCALAR', msz)])
        # first fault and non fault
        E('ldff1' + name, '1010010 %s Rm 011 Pg:3 Rn Zt' % d,
          [ZL('Zt', 1, es), pg3z(), MEM('SCALAR_SCALAR_OPTIONAL', msz)])
        E('ldnf1' + name, '1010010 %s 1 imm4 101 Pg:3 Rn Zt' % d,
          [ZL('Zt', 1, es), pg3z(), mem_imm])


def replicating_loads():
    for dtype, name, es, msz in DTYPES:
        d = bits(dtype, 4)
        # ld1r: scalar plus immediate (imm6 scaled by the memory size)
        E('ld1r' + name, '1000010 %s 1 imm6 1 %s Pg:3 Rn Zt' % (d[:2], d[2:]),
          [ZL('Zt', 1, es), pg3z(), MEM('IMM6', msz)])
    # ld1rq, ld1ro (F64MM): the quadword / octaword replicating loads
    for msz in range(4):
        n = MSZ_NAMES[msz]
        es = 'BHSD'[msz]
        m = bits(msz, 2)
        E('ld1rq' + n, '1010010 %s 00 0 imm4 001 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, es), pg3z(), MEM('SIMM4_SCALED', 4)])
        E('ld1rq' + n, '1010010 %s 00 Rm 000 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, es), pg3z(), MEM('SCALAR_SCALAR', msz)])
        E('ld1ro' + n, '1010010 %s 01 0 imm4 001 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, es), pg3z(), MEM('SIMM4_SCALED', 5)], feat='F64MM')
        E('ld1ro' + n, '1010010 %s 01 Rm 000 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, es), pg3z(), MEM('SCALAR_SCALAR', msz)], feat='F64MM')


def structure_loads():
    """ld2... ld4, st2... st4 (the contiguous structure accesses)"""
    for msz in range(4):
        n = MSZ_NAMES[msz]
        es = 'BHSD'[msz]
        m = bits(msz, 2)
        for count in (2, 3, 4):
            opc = bits(count - 1, 2)
            E('ld%d%s' % (count, n), '1010010 %s %s 0 imm4 111 Pg:3 Rn Zt' % (m, opc),
              [ZL('Zt', count, es), pg3z(), MEM('MUL_VL4', count)])
            E('ld%d%s' % (count, n), '1010010 %s %s Rm 110 Pg:3 Rn Zt' % (m, opc),
              [ZL('Zt', count, es), pg3z(), MEM('SCALAR_SCALAR', msz)])
            E('st%d%s' % (count, n), '1110010 %s %s 1 imm4 111 Pg:3 Rn Zt' % (m, opc),
              [ZL('Zt', count, es), pg3(), MEM('MUL_VL4', count)])
            E('st%d%s' % (count, n), '1110010 %s %s Rm 011 Pg:3 Rn Zt' % (m, opc),
              [ZL('Zt', count, es), pg3(), MEM('SCALAR_SCALAR', msz)])
    # The quadword structure accesses (SVE2.1)
    for count, top in ((2, '1010010 01 0'), (3, '1010010 10 0'), (4, '1010010 11 0')):
        pass
    for count, imm, reg in ((2, '1010010 0 10 0', '1010010 0 11'),
                            (3, '1010010 1 00 0', '1010010 1 01'),
                            (4, '1010010 1 10 0', '1010010 1 11')):
        pass


def non_temporal():
    for msz in range(4):
        n = MSZ_NAMES[msz]
        es = 'BHSD'[msz]
        m = bits(msz, 2)
        E('ldnt1' + n, '1010010 %s 00 0 imm4 111 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, es), pg3z(), MEM('MUL_VL4', 1)])
        E('ldnt1' + n, '1010010 %s 00 Rm 110 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, es), pg3z(), MEM('SCALAR_SCALAR', msz)])
        E('stnt1' + n, '1110010 %s 00 1 imm4 111 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, es), pg3(), MEM('MUL_VL4', 1)])
        E('stnt1' + n, '1110010 %s 00 Rm 011 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, es), pg3(), MEM('SCALAR_SCALAR', msz)])


def stores():
    """st1b... st1d: contiguous, with the register element size >= memory size"""
    for msz in range(4):
        n = MSZ_NAMES[msz]
        for size in range(msz, 4):
            es = 'BHSD'[size]
            sz = bits(size, 2)
            E('st1' + n, '1110010 %s %s 0 imm4 111 Pg:3 Rn Zt' % (bits(msz, 2), sz),
              [ZL('Zt', 1, es), pg3(), MEM('MUL_VL4', 1)])
            E('st1' + n, '1110010 %s %s Rm 010 Pg:3 Rn Zt' % (bits(msz, 2), sz),
              [ZL('Zt', 1, es), pg3(), MEM('SCALAR_SCALAR', msz)])


# The valid (msz, U) of a gather load into elements of 32 and 64 bits: U = 1 is
# the zero extending load
GATHER_32 = [(0, 1), (0, 0), (1, 1), (1, 0), (2, 1)]
GATHER_64 = [(0, 1), (0, 0), (1, 1), (1, 0), (2, 1), (2, 0), (3, 1)]


def gather_name(msz, unsigned, first_fault):
    name = ('ldff1' if first_fault else 'ld1') + ('' if unsigned else 's') + MSZ_NAMES[msz]
    return name


def gathers():
    for b30, table, es, mem32 in ((0, GATHER_32, 'S', 'ZM_S_EXT'), (1, GATHER_64, 'D', 'ZM_D_EXT')):
        for msz, u in table:
            m = bits(msz, 2)
            for ff in (0, 1):
                name = gather_name(msz, u, ff)
                # 32 bit offsets, uxtw / sxtw, unscaled and scaled
                for scaled in (0, 1):
                    if scaled and msz == 0:
                        continue
                    E(name, '1 %d 00010 %s xs %d Zm 0 %d %d Pg:3 Rn Zt' % (b30, m, scaled, u, ff),
                      [ZL('Zt', 1, es), pg3z(), MEM(mem32, msz if scaled else 0)])
                # vector base plus immediate
                E(name, '1 %d 00010 %s 01 imm5 1 %d %d Pg:3 Rn Zt' % (b30, m, u, ff),
                  [ZL('Zt', 1, es), pg3z(), MEM('ZN_S_IMM5' if b30 == 0 else 'ZN_D_IMM5', msz)])
            # non temporal: vector base plus scalar (SVE2)
            if not (b30 == 0 and (msz, u) == (2, 0)):
                E('ldnt1' + ('' if u else 's') + MSZ_NAMES[msz],
                  ('1 0 00010 %s 00 Rm 1 0 %d Pg:3 Rn Zt' % (m, u)) if b30 == 0 else
                  ('1 1 00010 %s 00 Rm 1 %d 0 Pg:3 Rn Zt' % (m, u)),
                  [ZL('Zt', 1, es), pg3z(), MEM('ZN_S_SCALAR' if b30 == 0 else 'ZN_D_SCALAR', 0)],
                  feat='SVE2')
    # 64 bit offsets, with lsl
    for msz, u in GATHER_64:
        m = bits(msz, 2)
        for ff in (0, 1):
            for scaled in (0, 1):
                if scaled and msz == 0:
                    continue
                E(gather_name(msz, u, ff), '11 00010 %s 1 %d Zm 1 %d %d Pg:3 Rn Zt' % (m, scaled, u, ff),
                  [ZL('Zt', 1, 'D'), pg3z(), MEM('ZM_D_LSL', msz if scaled else 0)])
    # ld1q (SVE2.1)
    E('ld1q', '11000100 000 Rm 101 Pg:3 Rn Zt',
      [ZL('Zt', 1, 'Q'), pg3z(), MEM('ZN_D_SCALAR', 0)], feat='SVE2P1')


def scatters():
    # 32 bit offsets (xs in bit 14), unscaled and scaled; 64 bit offsets
    for msz in range(4):
        n = MSZ_NAMES[msz]
        m = bits(msz, 2)
        for sw, es, mem in ((1, 'S', 'ZM_S_EXT'), (0, 'D', 'ZM_D_EXT')):
            if es == 'S' and msz == 3:
                continue
            for scaled in (0, 1):
                if scaled and msz == 0:
                    continue
                E('st1' + n, '1110010 %s %d %d Zm 1 xs 0 Pg:3 Rn Zt' % (m, sw, scaled),
                  [ZL('Zt', 1, es), pg3(), MEM(mem + '14', msz if scaled else 0)])
        for scaled in (0, 1):
            if scaled and msz == 0:
                continue
            E('st1' + n, '1110010 %s 0 %d Zm 101 Pg:3 Rn Zt' % (m, scaled),
              [ZL('Zt', 1, 'D'), pg3(), MEM('ZM_D_LSL', msz if scaled else 0)])
        # vector base plus immediate
        if msz < 3:
            E('st1' + n, '1110010 %s 1 1 imm5 101 Pg:3 Rn Zt' % m,
              [ZL('Zt', 1, 'S'), pg3(), MEM('ZN_S_IMM5', msz)])
        E('st1' + n, '1110010 %s 1 0 imm5 101 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, 'D'), pg3(), MEM('ZN_D_IMM5', msz)])
        # non temporal scatter (SVE2)
        E('stnt1' + n, '1110010 %s 0 0 Rm 001 Pg:3 Rn Zt' % m,
          [ZL('Zt', 1, 'D'), pg3(), MEM('ZN_D_SCALAR', 0)], feat='SVE2')
        if msz < 3:
            E('stnt1' + n, '1110010 %s 1 0 Rm 001 Pg:3 Rn Zt' % m,
              [ZL('Zt', 1, 'S'), pg3(), MEM('ZN_S_SCALAR', 0)], feat='SVE2')
    E('st1q', '11100100 001 Rm 001 Pg:3 Rn Zt',
      [ZL('Zt', 1, 'Q'), pg3(), MEM('ZN_D_SCALAR', 0)], feat='SVE2P1')


def prefetches():
    for msz in range(4):
        n = MSZ_NAMES[msz]
        m = bits(msz, 2)
        prf = OPT('PREFETCH', 'prfop')
        E('prf' + n, '10000101 11 imm6 0 %s Pg:3 Rn 0 prfop' % m,
          [prf, pg3(), MEM('MUL_VL6', 1)])
        E('prf' + n, '1000010 %s 00 Rm 110 Pg:3 Rn 0 prfop' % m,
          [prf, pg3(), MEM('SCALAR_SCALAR', msz)])
        for b30, mem in ((0, 'ZM_S_EXT'), (1, 'ZM_D_EXT')):
            E('prf' + n, '1 %d 000100 0 xs 1 Zm 0 %s Pg:3 Rn 0 prfop' % (b30, m),
              [prf, pg3(), MEM(mem, msz)])
            E('prf' + n, '1 %d 00010 %s 00 imm5 111 Pg:3 Rn 0 prfop' % (b30, m),
              [prf, pg3(), MEM('ZN_S_IMM5' if b30 == 0 else 'ZN_D_IMM5', msz)])
        E('prf' + n, '11000100 011 Zm 1 %s Pg:3 Rn 0 prfop' % m,
          [prf, pg3(), MEM('ZM_D_LSL', msz)])


def spills():
    E('ldr', '10000101 10 imm9h:6 010 imm9l:3 Rn Zt', [Z('Zt', 'NONE'), MEM('MUL_VL9', 1)])
    E('ldr', '10000101 10 imm9h:6 000 imm9l:3 Rn 0 Pt:4', [P('Pt'), MEM('MUL_VL9', 1)])
    E('str', '11100101 10 imm9h:6 010 imm9l:3 Rn Zt', [Z('Zt', 'NONE'), MEM('MUL_VL9', 1)])
    E('str', '11100101 10 imm9h:6 000 imm9l:3 Rn 0 Pt:4', [P('Pt'), MEM('MUL_VL9', 1)])
