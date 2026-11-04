"""SVE predicate instructions: the logical operations on predicates, the break
instructions, ptrue / pfalse / ptest, pfirst / pnext, the first fault
register, the while instructions, the predicate counts, the permutes of
predicates and the integer compares which write a predicate."""
from a64dsl_sve import *

SZ = 'SIZE_22'


def groups():
    logical()
    breaks()
    ptrue_and_tests()
    first_fault_register()
    predicate_permutes()
    while_instructions()
    predicate_counts()
    compares()


def logical():
    """and bic eor sel orr orn nor nand (and the flag setting ones)"""
    b = lambda f: P(f, 'B')
    base = '00100101 %s %s 00 Pm:4 01 Pg:4 %s Pn:4 %s Pd:4'
    zeroing = lambda: P('Pg', q='z')
    # The aliases first: mov, movs, not, nots
    for s, mn_mov, mn_not, flag in [(0, 'mov', 'not', 0), (1, 'movs', 'nots', 1)]:
        # mov pd.b, pg/z, pn.b = and pd, pg/z, pn, pn
        E(mn_mov, base % ('0', str(s), '0', '0'),
          [b('Pd'), zeroing(), b('Pn'), TIED('Pm', 'Pn', 4)], alias=True, cond='$Pn == $Pm')
        # mov pd.b, pn.b = orr pd, pn/z, pn, pn
        E(mn_mov, base % ('1', str(s), '0', '0'),
          [b('Pd'), b('Pn'), TIED('Pm', 'Pn', 4), TIED('Pg', 'Pn', 4)],
          alias=True, cond='$Pn == $Pm && $Pg == $Pn')
        # not pd.b, pg/z, pn.b = eor pd, pg/z, pn, pg
        E(mn_not, base % ('0', str(s), '1', '0'),
          [b('Pd'), zeroing(), b('Pn'), TIED('Pm', 'Pg', 4)], alias=True, cond='$Pm == $Pg')
    # mov pd.b, pg/m, pn.b = sel pd, pg, pn, pd
    E('mov', base % ('0', '0', '1', '1'),
      [b('Pd'), P('Pg', q='m'), b('Pn'), TIED('Pm', 'Pd', 4)], alias=True, cond='$Pm == $Pd')
    table = [('and', '0', '0', '0', '0'), ('bic', '0', '0', '0', '1'), ('eor', '0', '0', '1', '0'),
             ('orr', '1', '0', '0', '0'), ('orn', '1', '0', '0', '1'), ('nor', '1', '0', '1', '0'),
             ('nand', '1', '0', '1', '1')]
    for mn, op, s, o2, o3 in table:
        E(mn, base % (op, s, o2, o3), [b('Pd'), zeroing(), b('Pn'), b('Pm')])
        E(mn + 's', base % (op, '1', o2, o3), [b('Pd'), zeroing(), b('Pn'), b('Pm')])
    E('sel', base % ('0', '0', '1', '1'), [b('Pd'), P('Pg'), b('Pn'), b('Pm')])


def breaks():
    b = lambda f: P(f, 'B')
    # brka, brkb: merging or zeroing for the plain ones (bit 4), zeroing for the flag setting
    for mn, bb in [('brka', '0'), ('brkb', '1')]:
        E(mn, '00100101 %s 0 01 0000 01 Pg:4 0 Pn:4 M Pd:4' % bb,
          [b('Pd'), P('Pg', q='M4'), b('Pn')])
        E(mn + 's', '00100101 %s 1 01 0000 01 Pg:4 0 Pn:4 0 Pd:4' % bb,
          [b('Pd'), P('Pg', q='z'), b('Pn')])
    E('brkn', '00100101 0 0 01 1000 01 Pg:4 0 Pn:4 0 Pdm:4',
      [b('Pdm'), P('Pg', q='z'), b('Pn'), b('Pdm')])
    E('brkns', '00100101 0 1 01 1000 01 Pg:4 0 Pn:4 0 Pdm:4',
      [b('Pdm'), P('Pg', q='z'), b('Pn'), b('Pdm')])
    for mn, bb in [('brkpa', '0'), ('brkpb', '1')]:
        E(mn, '00100101 0 0 00 Pm:4 11 Pg:4 0 Pn:4 %s Pd:4' % bb,
          [b('Pd'), P('Pg', q='z'), b('Pn'), b('Pm')])
        E(mn + 's', '00100101 0 1 00 Pm:4 11 Pg:4 0 Pn:4 %s Pd:4' % bb,
          [b('Pd'), P('Pg', q='z'), b('Pn'), b('Pm')])


def ptrue_and_tests():
    # ptrue, ptrues: the pattern all is dropped
    for mn, s in [('ptrue', '0'), ('ptrues', '1')]:
        E(mn, '00100101 size 01100 %s 111000 11111 0 Pd:4' % s, [P('Pd', SZ)], alias=True)
        E(mn, '00100101 size 01100 %s 111000 pattern 0 Pd:4' % s,
          [P('Pd', SZ), OPT('PATTERN', 'pattern')])
    E('pfalse', '00100101 00 011000 111001 0000 0 0000 Pd:4' if False else
      '00100101 00 011000 111001 0000 0 0000 0000'.replace('0000 0000', 'Pd:4')
      if False else '00100101 00 011000 111001 00000 0 Pd:4', [P('Pd', 'B')])
    E('ptest', '00100101 01 010000 11 Pg:4 0 Pn:4 0 0000', [P('Pg'), P('Pn', 'B')])
    E('pfirst', '00100101 01 011000 11000 00 Pg:4 0 Pdn:4',
      [P('Pdn', 'B'), P('Pg'), P('Pdn', 'B')])
    E('pnext', '00100101 size 011001 11000 10 Pg:4 0 Pdn:4',
      [P('Pdn', SZ), P('Pg'), P('Pdn', SZ)])


def first_fault_register():
    E('setffr', '00100101 0010 1100 1001 0000 0000 0000', [])
    E('wrffr', '00100101 0010 1000 1001 000 Pn:4 00000', [P('Pn', 'B')])
    E('rdffr', '00100101 0001 1001 1111 0000 0000 Pd:4', [P('Pd', 'B')])
    E('rdffr', '00100101 0001 1000 1111 000 Pg:4 0 Pd:4', [P('Pd', 'B'), P('Pg', q='z')])
    E('rdffrs', '00100101 0101 1000 1111 000 Pg:4 0 Pd:4', [P('Pd', 'B'), P('Pg', q='z')])


def predicate_permutes():
    for mn, opc in [('zip1', '000'), ('zip2', '001'), ('uzp1', '010'), ('uzp2', '011'),
                    ('trn1', '100'), ('trn2', '101')]:
        E(mn, '00000101 size 10 Pm:4 010 %s 0 Pn:4 0 Pd:4' % opc,
          [P('Pd', SZ), P('Pn', SZ), P('Pm', SZ)])
    E('rev', '00000101 size 110100 0100000 Pn:4 0 Pd:4', [P('Pd', SZ), P('Pn', SZ)])
    E('punpklo', '00000101 00 110000 0100000 Pn:4 0 Pd:4', [P('Pd', 'H'), P('Pn', 'B')])
    E('punpkhi', '00000101 00 110001 0100000 Pn:4 0 Pd:4', [P('Pd', 'H'), P('Pn', 'B')])


def while_instructions():
    """whilege... on w or x registers; the SME2 forms write a predicate as
    counter (vlx2 / vlx4) or a pair of predicates"""
    table = [('whilege', 0, 0, 0), ('whilegt', 0, 0, 1), ('whilehs', 1, 0, 0),
             ('whilehi', 1, 0, 1), ('whilele', 0, 1, 1), ('whilelt', 0, 1, 0),
             ('whilelo', 1, 1, 0), ('whilels', 1, 1, 1)]
    for mn, u, lt, eq in table:
        for sf, reg in [(0, W), (1, X)]:
            E(mn, '00100101 size 1 Rm 000 %d %d %d Rn %d Pd:4' % (sf, u, lt, eq),
              [P('Pd', SZ), reg('Rn'), reg('Rm')])
        # predicate as counter: pn8-pn15, vlx2 / vlx4 (bit 13)
        E(mn, '00100101 size 1 Rm 01 vl 0 %d %d Rn 1 %d PNd:3' % (u, lt, eq),
          [PN('PNd', SZ), X('Rn'), X('Rm'), OPT('VLX', 'vl')], feat='SVE2P1')
        # a pair of predicates
        E(mn, '00100101 size 1 Rm 01 0 1 %d %d Rn 1 Pd:3 %d' % (u, lt, eq),
          [P('Pd', SZ, w=3, pair=True), X('Rn'), X('Rm')], feat='SVE2P1')
    E('whilewr', '00100101 size 1 Rm 001100 Rn 0 Pd:4', [P('Pd', SZ), X('Rn'), X('Rm')],
      feat='SVE2')
    E('whilerw', '00100101 size 1 Rm 001100 Rn 1 Pd:4', [P('Pd', SZ), X('Rn'), X('Rm')],
      feat='SVE2')
    # ctermeq, ctermne
    for mn, ne in [('ctermeq', '0'), ('ctermne', '1')]:
        E(mn, '00100101 1 0 1 Rm 001000 Rn %s 0000' % ne, [W('Rn'), W('Rm')])
        E(mn, '00100101 1 1 1 Rm 001000 Rn %s 0000' % ne, [X('Rn'), X('Rm')])


def predicate_counts():
    E('cntp', '00100101 size 100000 10 Pg:4 0 Pn:4 Rd', [X('Rd'), P('Pg'), P('Pn', SZ)])
    E('firstp', '00100101 size 100001 10 Pg:4 0 Pn:4 Rd', [X('Rd'), P('Pg'), P('Pn', SZ)],
      feat='SVE2P2')
    E('lastp', '00100101 size 100010 10 Pg:4 0 Pn:4 Rd', [X('Rd'), P('Pg'), P('Pn', SZ)],
      feat='SVE2P2')
    # incp, decp and the saturating ones: z, x and (sq, uq) w
    for mn, opc in [('incp', '1100'), ('decp', '1101'), ('sqincp', '1000'), ('sqdecp', '1010'),
                    ('uqincp', '1001'), ('uqdecp', '1011')]:
        E(mn, '00100101 size 10 %s 1000 00 0 Pm:4 Zdn' % opc,
          [Z('Zdn', 'SIZE_22_WIDE'), P('Pm', 'SIZE_22_WIDE')])
        if mn in ('incp', 'decp'):
            E(mn, '00100101 size 10 %s 1000 10 0 Pm:4 Rdn' % opc, [X('Rdn'), P('Pm', SZ)])
        elif mn.startswith('sq'):
            E(mn, '00100101 size 10 %s 1000 11 0 Pm:4 Rdn' % opc, [X('Rdn'), P('Pm', SZ)])
            E(mn, '00100101 size 10 %s 1000 10 0 Pm:4 Rdn' % opc,
              [X('Rdn'), P('Pm', SZ), W('Rdn')])
        else:
            E(mn, '00100101 size 10 %s 1000 11 0 Pm:4 Rdn' % opc, [X('Rdn'), P('Pm', SZ)])
            E(mn, '00100101 size 10 %s 1000 10 0 Pm:4 Rdn' % opc, [W('Rdn'), P('Pm', SZ)])


def compares():
    """cmp<cc> pd.T, pg/z, zn.T, zm.T | zm.d | #imm"""
    # vectors, the same size: [15:13] ne
    for mn, opc, ne in [('cmphs', '000', '0'), ('cmphi', '000', '1'), ('cmpge', '100', '0'),
                        ('cmpgt', '100', '1'), ('cmpeq', '101', '0'), ('cmpne', '101', '1')]:
        E(mn, '00100100 size 0 Zm %s Pg:3 Zn %s Pd:4' % (opc, ne),
          [P('Pd', SZ), P('Pg', q='z', w=3), Z('Zn', SZ), Z('Zm', SZ)])
    # vectors, wide
    for mn, opc, ne in [('cmpeq', '001', '0'), ('cmpne', '001', '1'), ('cmpge', '010', '0'),
                        ('cmpgt', '010', '1'), ('cmplt', '011', '0'), ('cmple', '011', '1'),
                        ('cmphs', '110', '0'), ('cmphi', '110', '1'), ('cmplo', '111', '0'),
                        ('cmpls', '111', '1')]:
        E(mn, '00100100 size 0 Zm %s Pg:3 Zn %s Pd:4' % (opc, ne),
          [P('Pd', 'SIZE_22_BHS'), P('Pg', q='z', w=3), Z('Zn', 'SIZE_22_BHS'), Z('Zm', 'D')])
    # signed immediates
    for mn, opc, ne in [('cmpge', '000', '0'), ('cmpgt', '000', '1'), ('cmplt', '001', '0'),
                        ('cmple', '001', '1'), ('cmpeq', '100', '0'), ('cmpne', '100', '1')]:
        E(mn, '00100101 size 0 imm5 %s Pg:3 Zn %s Pd:4' % (opc, ne),
          [P('Pd', SZ), P('Pg', q='z', w=3), Z('Zn', SZ), IMM('SIGNED5_16')])
    # unsigned immediates
    for mn, lt, ne in [('cmphs', '0', '0'), ('cmphi', '0', '1'), ('cmplo', '1', '0'),
                       ('cmpls', '1', '1')]:
        E(mn, '00100100 size 1 imm7:7 %s Pg:3 Zn %s Pd:4' % (lt, ne),
          [P('Pd', SZ), P('Pg', q='z', w=3), Z('Zn', SZ), IMM('UNSIGNED7_14')])
