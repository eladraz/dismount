"""A64 loads and stores: literal, pairs, register forms, exclusives, atomics."""
import a64dsl
from a64dsl import *

GPR_SIZES = {0: ('b', 0), 1: ('h', 1), 2: ('w', 2), 3: ('x', 3)}


def literal():
    for opc, mnemonic, reg in [('00', 'ldr', W), ('01', 'ldr', X), ('10', 'ldrsw', X)]:
        E(mnemonic, '%s 011 0 00 imm19 Rt' % opc, [reg('Rt'), pcrel('ARM_PCREL_LITERAL')])
    E('prfm', '11 011 0 00 imm19 Rt', [opt('ARM_OPT_PREFETCH'), pcrel('ARM_PCREL_LITERAL')])
    for opc, size in [('00', 's'), ('01', 'd'), ('10', 'q')]:
        E('ldr', '%s 011 1 00 imm19 Rt' % opc, [fpr('Rt', size), pcrel('ARM_PCREL_LITERAL')])


def pairs():
    # idx: bits 25-23
    forms = [('001', 'ARM_ADDR_PAIR_POST_INDEX'), ('010', 'ARM_ADDR_PAIR_OFFSET'),
             ('011', 'ARM_ADDR_PAIR_PRE_INDEX')]
    for idx, scheme in forms:
        for l, mnemonic in [('0', 'stp'), ('1', 'ldp')]:
            for opc, reg, scale in [('00', W, 2), ('10', X, 3)]:
                E(mnemonic, '%s 101 0 %s %s imm7 Rt2 Rn Rt' % (opc, idx, l),
                  [reg('Rt'), reg('Rt2'), mem(scheme, scale)])
            for opc, size, scale in [('00', 's', 2), ('01', 'd', 3), ('10', 'q', 4)]:
                E(mnemonic, '%s 101 1 %s %s imm7 Rt2 Rn Rt' % (opc, idx, l),
                  [fpr('Rt', size), fpr('Rt2', size), mem(scheme, scale)])
        E('ldpsw', '01 101 0 %s 1 imm7 Rt2 Rn Rt' % idx, [X('Rt'), X('Rt2'), mem(scheme, 2)])
        E('stgp', '01 101 0 %s 0 imm7 Rt2 Rn Rt' % idx, [X('Rt'), X('Rt2'), mem(scheme, 4)],
          feat='FEAT_MTE')
        # Unprivileged pairs (FEAT_LSUI)
        for l, mnemonic in [('0', 'sttp'), ('1', 'ldtp')]:
            E(mnemonic, '11 101 0 %s %s imm7 Rt2 Rn Rt' % (idx, l),
              [X('Rt'), X('Rt2'), mem(scheme, 3)], feat='FEAT_LSUI')
            E(mnemonic, '11 101 1 %s %s imm7 Rt2 Rn Rt' % (idx, l),
              [fpr('Rt', 'q'), fpr('Rt2', 'q'), mem(scheme, 4)], feat='FEAT_LSUI')
    # No-allocate pairs
    for l, mnemonic in [('0', 'stnp'), ('1', 'ldnp')]:
        for opc, reg, scale in [('00', W, 2), ('10', X, 3)]:
            E(mnemonic, '%s 101 0 000 %s imm7 Rt2 Rn Rt' % (opc, l),
              [reg('Rt'), reg('Rt2'), mem('ARM_ADDR_PAIR_OFFSET', scale)])
        for opc, size, scale in [('00', 's', 2), ('01', 'd', 3), ('10', 'q', 4)]:
            E(mnemonic, '%s 101 1 000 %s imm7 Rt2 Rn Rt' % (opc, l),
              [fpr('Rt', size), fpr('Rt2', size), mem('ARM_ADDR_PAIR_OFFSET', scale)])
    for l, mnemonic in [('0', 'sttnp'), ('1', 'ldtnp')]:
        E(mnemonic, '11 101 0 000 %s imm7 Rt2 Rn Rt' % l,
          [X('Rt'), X('Rt2'), mem('ARM_ADDR_PAIR_OFFSET', 3)], feat='FEAT_LSUI')
        E(mnemonic, '11 101 1 000 %s imm7 Rt2 Rn Rt' % l,
          [fpr('Rt', 'q'), fpr('Rt2', 'q'), mem('ARM_ADDR_PAIR_OFFSET', 4)], feat='FEAT_LSUI')


def single():
    """The loads and stores of a register: unscaled, post / pre index,
    unprivileged, unsigned offset and register offset."""
    # (size, opc, mnemonic suffix handling, register operand builder, scale)
    gpr = []
    for size, suffix in [(0, 'b'), (1, 'h')]:
        gpr.append((size, '00', 'str' + suffix, W, size, 'str'))
        gpr.append((size, '01', 'ldr' + suffix, W, size, 'ldr'))
        gpr.append((size, '10', 'ldrs' + suffix, X, size, 'ldrs'))
        gpr.append((size, '11', 'ldrs' + suffix, W, size, 'ldrs'))
    gpr.append((2, '00', 'str', W, 2, 'str'))
    gpr.append((2, '01', 'ldr', W, 2, 'ldr'))
    gpr.append((2, '10', 'ldrsw', X, 2, 'ldrs'))
    gpr.append((3, '00', 'str', X, 3, 'str'))
    gpr.append((3, '01', 'ldr', X, 3, 'ldr'))
    simd = []
    for size, reg in [(0, 'b'), (1, 'h'), (2, 's'), (3, 'd')]:
        simd.append((size, '00', 'str', reg, size))
        simd.append((size, '01', 'ldr', reg, size))
    simd.append((0, '10', 'str', 'q', 4))
    simd.append((0, '11', 'ldr', 'q', 4))

    def unscaled_name(name):
        # str -> stur, ldr -> ldur, ldrsb -> ldursb, ldrsw -> ldursw
        return name.replace('str', 'stur').replace('ldr', 'ldur')

    def unpriv_name(name):
        return name.replace('str', 'sttr').replace('ldr', 'ldtr')

    for size, opc, name, reg, scale, kind in gpr:
        sz = format(size, '02b')
        # unsigned offset
        E(name, '%s 111 0 01 %s imm12 Rn Rt' % (sz, opc), [reg('Rt'), mem('ARM_ADDR_UNSIGNED_OFFSET', scale)])
        E(name, '%s 111 0 00 %s 1 Rm option S 10 Rn Rt' % (sz, opc),
          [reg('Rt'), mem('ARM_ADDR_REGISTER_OFFSET', scale)])
        E(unscaled_name(name), '%s 111 0 00 %s 0 imm9 00 Rn Rt' % (sz, opc),
          [reg('Rt'), mem('ARM_ADDR_UNSCALED')])
        E(name, '%s 111 0 00 %s 0 imm9 01 Rn Rt' % (sz, opc),
          [reg('Rt'), mem('ARM_ADDR_POST_INDEX')])
        E(unpriv_name(name), '%s 111 0 00 %s 0 imm9 10 Rn Rt' % (sz, opc),
          [reg('Rt'), mem('ARM_ADDR_UNSCALED')])
        E(name, '%s 111 0 00 %s 0 imm9 11 Rn Rt' % (sz, opc),
          [reg('Rt'), mem('ARM_ADDR_PRE_INDEX')])
    for size, opc, name, reg, scale in simd:
        sz = format(size, '02b')
        E(name, '%s 111 1 01 %s imm12 Rn Rt' % (sz, opc), [fpr('Rt', reg), mem('ARM_ADDR_UNSIGNED_OFFSET', scale)])
        E(name, '%s 111 1 00 %s 1 Rm option S 10 Rn Rt' % (sz, opc),
          [fpr('Rt', reg), mem('ARM_ADDR_REGISTER_OFFSET', scale)])
        E(unscaled_name(name), '%s 111 1 00 %s 0 imm9 00 Rn Rt' % (sz, opc),
          [fpr('Rt', reg), mem('ARM_ADDR_UNSCALED')])
        E(name, '%s 111 1 00 %s 0 imm9 01 Rn Rt' % (sz, opc),
          [fpr('Rt', reg), mem('ARM_ADDR_POST_INDEX')])
        E(name, '%s 111 1 00 %s 0 imm9 11 Rn Rt' % (sz, opc),
          [fpr('Rt', reg), mem('ARM_ADDR_PRE_INDEX')])
    # prefetch
    E('prfm', '11 111 0 01 10 imm12 Rn Rt', [opt('ARM_OPT_PREFETCH'), mem('ARM_ADDR_UNSIGNED_OFFSET', 3)])
    E('prfm', '11 111 0 00 10 1 Rm option S 10 Rn Rt', [opt('ARM_OPT_PREFETCH'), mem('ARM_ADDR_REGISTER_OFFSET', 3)])
    E('prfum', '11 111 0 00 10 0 imm9 00 Rn Rt', [opt('ARM_OPT_PREFETCH'), mem('ARM_ADDR_UNSCALED')])


SUFFIX = {0: 'b', 1: 'h', 2: '', 3: ''}


def exclusives():
    for size in range(4):
        sz = format(size, '02b')
        suffix = SUFFIX[size]
        data = W if size < 3 else X
        base = mem('ARM_ADDR_BASE')
        # stxr, stlxr, ldxr, ldaxr
        for mnemonic, l, o0 in [('stxr', 0, 0), ('stlxr', 0, 1), ('ldxr', 1, 0), ('ldaxr', 1, 1)]:
            if l == 0:
                E(mnemonic + suffix, '%s 001000 0 0 0 Rs %d Rt2! Rn Rt' % (sz, o0),
                  [W('Rs'), data('Rt'), base])
            else:
                E(mnemonic + suffix, '%s 001000 0 1 0 Rs! %d Rt2! Rn Rt' % (sz, o0),
                  [data('Rt'), base])
        # stllr, stlr, ldlar, ldar
        for mnemonic, l, o0, feat in [('stllr', 0, 0, 'FEAT_LOR'), ('stlr', 0, 1, 'FEAT_A64_BASE'),
                                      ('ldlar', 1, 0, 'FEAT_LOR'), ('ldar', 1, 1, 'FEAT_A64_BASE')]:
            E(mnemonic + suffix, '%s 001000 1 %d 0 Rs! %d Rt2! Rn Rt' % (sz, l, o0),
              [data('Rt'), base], feat=feat)
        # cas
        for a, l_bit in [(0, 0), (1, 1)]:
            for r, o0 in [(0, 0), (1, 1)]:
                name = 'cas' + ('a' if a else '') + ('l' if r else '') + suffix
                E(name, '%s 001000 1 %d 1 Rs %d 11111 Rn Rt' % (sz, l_bit, o0),
                  [data('Rs'), data('Rt'), base], feat='FEAT_LSE')
        if size >= 2:
            pair = WP if size == 2 else XP
            for mnemonic, l, o0 in [('stxp', 0, 0), ('stlxp', 0, 1)]:
                E(mnemonic, '1%d 001000 0 0 1 Rs %d Rt2 Rn Rt' % (size & 1, o0),
                  [W('Rs'), data('Rt'), data('Rt2'), base])
            for mnemonic, l, o0 in [('ldxp', 1, 0), ('ldaxp', 1, 1)]:
                E(mnemonic, '1%d 001000 0 1 1 Rs! %d Rt2 Rn Rt' % (size & 1, o0),
                  [data('Rt'), data('Rt2'), base])
            for a, l_bit in [(0, 0), (1, 1)]:
                for r, o0 in [(0, 0), (1, 1)]:
                    name = 'casp' + ('a' if a else '') + ('l' if r else '')
                    E(name, '0%d 001000 0 %d 1 Rs %d 11111 Rn Rt' % (size & 1, l_bit, o0),
                      [pair('Rs'), pair('Rt'), base], feat='FEAT_LSE')
            # unprivileged compare and swap (FEAT_LSUI)
            if size == 3:
                for a, l_bit in [(0, 0), (1, 1)]:
                    for r, o0 in [(0, 0), (1, 1)]:
                        name = 'cas' + ('a' if a else '') + ('l' if r else '') + 't'
                        E(name, '11 001001 1 %d 0 Rs %d Rt2! Rn Rt' % (l_bit, o0),
                          [X('Rs'), X('Rt'), base], feat='FEAT_LSUI')
            # unprivileged compare and swap pairs (FEAT_LSUI)
            if size == 3:
                for a, l_bit in [(0, 0), (1, 1)]:
                    for r, o0 in [(0, 0), (1, 1)]:
                        name = 'casp' + ('a' if a else '') + ('l' if r else '') + 't'
                        E(name, '01 001001 1 %d 0 Rs %d Rt2! Rn Rt' % (l_bit, o0),
                          [XP('Rs'), XP('Rt'), base], feat='FEAT_LSUI')
            # unprivileged exclusives (FEAT_LSUI)
            for mnemonic, o0 in [('sttxr', 0), ('stltxr', 1)]:
                E(mnemonic, '1%d 001001 0 0 0 Rs %d Rt2! Rn Rt' % (size & 1, o0),
                  [W('Rs'), data('Rt'), base], feat='FEAT_LSUI')
            for mnemonic, o0 in [('ldtxr', 0), ('ldatxr', 1)]:
                E(mnemonic, '1%d 001001 0 1 0 Rs! %d Rt2! Rn Rt' % (size & 1, o0),
                  [data('Rt'), base], feat='FEAT_LSUI')


def atomics():
    ops = [('add', '000'), ('clr', '001'), ('eor', '010'), ('set', '011'),
           ('smax', '100'), ('smin', '101'), ('umax', '110'), ('umin', '111')]
    for size in range(4):
        sz = format(size, '02b')
        suffix = SUFFIX[size]
        data = W if size < 3 else X
        base = mem('ARM_ADDR_BASE')
        for name, opc in ops:
            # The st aliases: the load has no destination and no acquire
            for r in (0, 1):
                E('st' + name + ('l' if r else '') + suffix,
                  '%s 111 0 00 0 %d 1 Rs 0 %s 00 Rn 11111' % (sz, r, opc),
                  [data('Rs'), base], feat='FEAT_LSE', flags='ARM_ENTRY_ALIAS')
            for a in (0, 1):
                for r in (0, 1):
                    E('ld' + name + ('a' if a else '') + ('l' if r else '') + suffix,
                      '%s 111 0 00 %d %d 1 Rs 0 %s 00 Rn Rt' % (sz, a, r, opc),
                      [data('Rs'), data('Rt'), base], feat='FEAT_LSE')
        for a in (0, 1):
            for r in (0, 1):
                E('swp' + ('a' if a else '') + ('l' if r else '') + suffix,
                  '%s 111 0 00 %d %d 1 Rs 1 000 00 Rn Rt' % (sz, a, r),
                  [data('Rs'), data('Rt'), base], feat='FEAT_LSE')
        E('ldapr' + suffix, '%s 111 0 00 1 0 1 11111 1 100 00 Rn Rt' % sz,
          [data('Rt'), base], feat='FEAT_LRCPC')


def unscaled_acquire_release():
    """ldapur / stlur (FEAT_LRCPC2), and the SIMD forms (FEAT_LRCPC3)."""
    cases = [(0, '00', 'stlurb', W), (0, '01', 'ldapurb', W), (0, '10', 'ldapursb', X),
             (0, '11', 'ldapursb', W), (1, '00', 'stlurh', W), (1, '01', 'ldapurh', W),
             (1, '10', 'ldapursh', X), (1, '11', 'ldapursh', W), (2, '00', 'stlur', W),
             (2, '01', 'ldapur', W), (2, '10', 'ldapursw', X), (3, '00', 'stlur', X),
             (3, '01', 'ldapur', X)]
    for size, opc, name, reg in cases:
        E(name, '%s 011 0 01 %s 0 imm9 00 Rn Rt' % (format(size, '02b'), opc),
          [reg('Rt'), mem('ARM_ADDR_UNSCALED')], feat='FEAT_LRCPC2')
    for size, reg in [(0, 'b'), (1, 'h'), (2, 's'), (3, 'd')]:
        E('stlur', '%s 011 1 01 00 0 imm9 10 Rn Rt' % format(size, '02b'),
          [fpr('Rt', reg), mem('ARM_ADDR_UNSCALED')], feat='FEAT_LRCPC3')
        E('ldapur', '%s 011 1 01 01 0 imm9 10 Rn Rt' % format(size, '02b'),
          [fpr('Rt', reg), mem('ARM_ADDR_UNSCALED')], feat='FEAT_LRCPC3')
    E('stlur', '00 011 1 01 10 0 imm9 10 Rn Rt', [fpr('Rt', 'q'), mem('ARM_ADDR_UNSCALED')],
      feat='FEAT_LRCPC3')
    E('ldapur', '00 011 1 01 11 0 imm9 10 Rn Rt', [fpr('Rt', 'q'), mem('ARM_ADDR_UNSCALED')],
      feat='FEAT_LRCPC3')


def pauth_loads():
    for m, name in [(0, 'ldraa'), (1, 'ldrab')]:
        E(name, '11 111 0 00 %d S 1 imm9 0 1 Rn Rt' % m, [X('Rt'), mem('ARM_ADDR_PAUTH_OFFSET')],
          feat='FEAT_PAUTH')
        E(name, '11 111 0 00 %d S 1 imm9 1 1 Rn Rt' % m, [X('Rt'), mem('ARM_ADDR_PAUTH_PRE_INDEX')],
          feat='FEAT_PAUTH')


def memory_tagging():
    E('addg', '1 0 0 100011 0 imm6 op3:2 imm4 Rn Rd',
      [XS('Rd'), XS('Rn'), imm('imm6', scale=4, hex=True), imm('imm4', hex=True)], feat='FEAT_MTE')
    E('subg', '1 1 0 100011 0 imm6 op3:2 imm4 Rn Rd',
      [XS('Rd'), XS('Rn'), imm('imm6', scale=4, hex=True), imm('imm4', hex=True)], feat='FEAT_MTE')
    for opc, name in [('00', 'stg'), ('01', 'stzg'), ('10', 'st2g'), ('11', 'stz2g')]:
        for op2, scheme in [('01', 'ARM_ADDR_POST_INDEX'), ('10', 'ARM_ADDR_UNSCALED'),
                            ('11', 'ARM_ADDR_PRE_INDEX')]:
            E(name, '11011001 %s 1 imm9 %s Rn Rt' % (opc, op2), [XS('Rt'), mem(scheme, 4)],
              feat='FEAT_MTE')
    E('ldg', '11011001 01 1 imm9 00 Rn Rt', [X('Rt'), mem('ARM_ADDR_UNSCALED', 4)], feat='FEAT_MTE')
    E('stzgm', '11011001 00 1 000000000 00 Rn Rt', [X('Rt'), mem('ARM_ADDR_BASE')], feat='FEAT_MTE')
    E('stgm', '11011001 10 1 000000000 00 Rn Rt', [X('Rt'), mem('ARM_ADDR_BASE')], feat='FEAT_MTE')
    E('ldgm', '11011001 11 1 000000000 00 Rn Rt', [X('Rt'), mem('ARM_ADDR_BASE')], feat='FEAT_MTE')


def ls64():
    base = mem('ARM_ADDR_BASE')
    E('ld64b', '11 111 0 00 0 0 1 11111 1 101 00 Rn Rt', [X('Rt'), base], feat='FEAT_LS64',
      cond='($Rt & 1) == 0 && $Rt <= 22')
    E('st64b', '11 111 0 00 0 0 1 11111 1 001 00 Rn Rt', [X('Rt'), base], feat='FEAT_LS64',
      cond='($Rt & 1) == 0 && $Rt <= 22')
    E('st64bv', '11 111 0 00 0 0 1 Rs 1 011 00 Rn Rt', [X('Rs'), X('Rt'), base], feat='FEAT_LS64',
      cond='($Rt & 1) == 0 && $Rt <= 22')
    E('st64bv0', '11 111 0 00 0 0 1 Rs 1 010 00 Rn Rt', [X('Rs'), X('Rt'), base], feat='FEAT_LS64',
      cond='($Rt & 1) == 0 && $Rt <= 22')


def newer_atomics():
    """The atomic instructions of LSFE, RCW (FEAT_THE), LSUI and LSE128."""
    base = mem('ARM_ADDR_BASE')
    suffixes = [('', 0, 0), ('a', 1, 0), ('l', 0, 1), ('al', 1, 1)]
    # LSFE: size 00 bf16 (ldbf), 01 half (ldf), 10 single, 11 double
    fp_ops = [('add', '000'), ('max', '100'), ('min', '101'), ('maxnm', '110'), ('minnm', '111')]
    for size, prefix, reg in [('00', 'ldbf', 'h'), ('01', 'ldf', 'h'), ('10', 'ldf', 's'),
                              ('11', 'ldf', 'd')]:
        for op, bits in fp_ops:
            for suffix, a, r in suffixes:
                E(prefix + op + suffix, '%s 111 1 00 %d %d 1 Rs 0 %s 00 Rn Rt' % (size, a, r, bits),
                  [fpr('Rs', reg), fpr('Rt', reg), base], feat='FEAT_LSFE')
            for suffix, a, r in [('', 0, 0), ('l', 0, 1)]:
                E('st' + prefix[2:] + op + suffix,
                  '%s 111 1 00 0 %d 1 Rs 1 %s 00 Rn 11111' % (size, r, bits),
                  [fpr('Rs', reg), base], feat='FEAT_LSFE')
    # RCW: size 00 rcw*, 01 rcws*; the scalar forms are in the 0x38 / 0x78 space
    for size, infix in [('00', ''), ('01', 's')]:
        for op, bits in [('clr', '001'), ('swp', '010'), ('set', '011')]:
            for suffix, a, r in suffixes:
                E('rcw' + infix + op + suffix, '%s 111 0 00 %d %d 1 Rs 1 %s 00 Rn Rt' % (size, a, r, bits),
                  [X('Rs'), X('Rt'), base], feat='FEAT_THE')
        # The 128 bit and the compare and swap forms are in the 011001 space
        for op, bits in [('clr', '1001'), ('swp', '1010'), ('set', '1011')]:
            for suffix, a, r in suffixes:
                E('rcw' + infix + op + 'p' + suffix,
                  '%s 011 0 01 %d %d 1 Rs %s 00 Rn Rt' % (size, a, r, bits),
                  [X('Rt'), X('Rs'), base], feat='FEAT_THE', cond='$Rt != 31 && $Rs != 31')
        for suffix, a, r in suffixes:
            E('rcw' + infix + 'cas' + suffix, '%s 011 0 01 %d %d 1 Rs 0000 10 Rn Rt' % (size, a, r),
              [X('Rs'), X('Rt'), base], feat='FEAT_THE')
            E('rcw' + infix + 'casp' + suffix, '%s 011 0 01 %d %d 1 Rs 0000 11 Rn Rt' % (size, a, r),
              [XP('Rs'), XP('Rt'), base], feat='FEAT_THE')
    # LSUI: ldtadd, ldtclr, ldtset, swpt (size 00 w, 01 x)
    for size, reg in [('00', W), ('01', X)]:
        for name, low in [('ldtadd', '0 000 01'), ('ldtclr', '0 001 01'), ('ldtset', '0 011 01'),
                          ('swpt', '1 000 01')]:
            for suffix, a, r in suffixes:
                E(name + suffix, '%s 011 0 01 %d %d 1 Rs %s Rn Rt' % (size, a, r, low),
                  [reg('Rs'), reg('Rt'), base], feat='FEAT_LSUI')
    # LSE128: ldclrp, ldsetp, swpp
    for name, low in [('ldclrp', '0 001 00'), ('ldsetp', '0 011 00'), ('swpp', '1 000 00')]:
        for suffix, a, r in suffixes:
            E(name + suffix, '00 011 0 01 %d %d 1 Rs %s Rn Rt' % (a, r, low),
              [X('Rt'), X('Rs'), base], feat='FEAT_LSE128', cond='$Rt != 31 && $Rs != 31')


def rcpc3():
    """ldiapp / stilp and the indexed ldapr / stlr (FEAT_LRCPC3)."""
    for size, reg, scale in [('10', W, 2), ('11', X, 3)]:
        pair_scale = scale + 1
        E('ldiapp', '%s 011001 01 0 Rs 000 0 10 Rn Rt' % size,
          [reg('Rt'), reg('Rs'), mem('ARM_ADDR_FIXED_POST_INDEX', pair_scale)], feat='FEAT_LRCPC3')
        E('ldiapp', '%s 011001 01 0 Rs 000 1 10 Rn Rt' % size,
          [reg('Rt'), reg('Rs'), mem('ARM_ADDR_BASE')], feat='FEAT_LRCPC3')
        E('stilp', '%s 011001 00 0 Rs 000 0 10 Rn Rt' % size,
          [reg('Rt'), reg('Rs'), mem('ARM_ADDR_FIXED_PRE_INDEX', pair_scale)], feat='FEAT_LRCPC3')
        E('stilp', '%s 011001 00 0 Rs 000 1 10 Rn Rt' % size,
          [reg('Rt'), reg('Rs'), mem('ARM_ADDR_BASE')], feat='FEAT_LRCPC3')
        E('ldapr', '%s 011001 11 0 00000 000010 Rn Rt' % size,
          [reg('Rt'), mem('ARM_ADDR_FIXED_POST_INDEX', scale)], feat='FEAT_LRCPC3')
        E('stlr', '%s 011001 10 0 00000 000010 Rn Rt' % size,
          [reg('Rt'), mem('ARM_ADDR_FIXED_PRE_INDEX', scale)], feat='FEAT_LRCPC3')


def mops():
    """The memory copy and set instructions (FEAT_MOPS)."""
    a_names = ['', 'wt', 'rt', 't']          # bits 13-12 of the copy
    b_names = ['', 'wn', 'rn', 'n']          # bits 15-14 of the copy
    stages = [('p', '00'), ('m', '01'), ('e', '10')]
    distinct = '$Rd != $Rn && $Rd != $Rs && $Rn != $Rs && $Rd != 31'
    for fixed in (0, 1):
        # bit 26 clear: cpyf, set: setg
        for stage, bits in stages:
            for b in range(4):
                for a in range(4):
                    name = ('cpyf' if fixed == 0 else 'cpy') + stage + a_names[a] + b_names[b]
                    E(name, '00011 %d 01 %s 0 Rs %s %s 01 Rn Rd' % (
                        fixed, bits, format(b, '02b'), format(a, '02b')),
                      [mem('ARM_ADDR_BASE_WRITEBACK', base='Rd'),
                       mem('ARM_ADDR_BASE_WRITEBACK', base='Rs'), XWB('Rn')],
                      feat='FEAT_MOPS', cond=distinct + ' && $Rs != 31')
    set_variants = ['', 't', 'n', 'tn']
    for guarded in (0, 1):
        for stage, bits in [('p', '00'), ('m', '01'), ('e', '10')]:
            for v in range(4):
                name = ('setg' if guarded else 'set') + stage + set_variants[v]
                E(name, '00011 %d 01 11 0 Rs %s %s 01 Rn Rd' % (guarded, bits, format(v, '02b')),
                  [mem('ARM_ADDR_BASE_WRITEBACK', base='Rd'), XWB('Rn'), X('Rs')],
                  feat='FEAT_MOPS' if not guarded else 'FEAT_MTE', cond=distinct)


def gcs():
    E('gcsstr', '1101100100011111 000011 Rn Rt', [X('Rt'), mem('ARM_ADDR_BASE')], feat='FEAT_GCS')
    E('gcssttr', '1101100100011111 000111 Rn Rt', [X('Rt'), mem('ARM_ADDR_BASE')], feat='FEAT_GCS')


def rprfm():
    E('rprfm', '11 111 0 00 10 1 Rm o15:1 1 o13:1 o12:1 10 Rn 11 rt:3',
      [opt('ARM_OPT_RPRFM'), X('Rm'), mem('ARM_ADDR_BASE')])


def loadstore():
    gcs()
    rprfm()
    rcpc3()
    newer_atomics()
    mops()
    literal()
    pairs()
    ls64()
    exclusives()
    atomics()
    unscaled_acquire_release()
    pauth_loads()
    memory_tagging()
    single()
