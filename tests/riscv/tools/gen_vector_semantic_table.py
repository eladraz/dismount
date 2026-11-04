#!/usr/bin/env python3
"""
Generates Source/dismount/proc/riscv/RiscvSemanticTable_vector.inc, the rows of
gRiscvVectorSemanticTable (RiscvSemanticTableVector.cpp): one row per vector
mnemonic of the encoding tables (config, int, mask, mem) that the lifters of
RiscvSemanticLiftVector*.cpp handle. The floating point (RiscvEncodingTable_vector_fp.inc), the
BFloat16, Zvbb / Zvkb / Zvbc and the cryptography (..._vector_ext.inc) rows are
not here: the lifter of those adds its own rows (a second .inc included by the
table) -- see the extension points of Include/dismount/proc/riscv/RiscvSemanticVector.h.

  python3 tests/riscv/tools/gen_vector_semantic_table.py [--check]

--check exits 1 if the file is out of date or a decoded mnemonic of the four tables has no row.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
SRC = os.path.join(REPO, 'Source', 'dismount', 'proc', 'riscv')
OUT = os.path.join(SRC, 'RiscvSemanticTable_vector.inc')
TABLES = ['config', 'int', 'mask', 'mem']


def read_names():
    names = []
    for t in TABLES:
        with open(os.path.join(SRC, 'RiscvEncodingTable_vector_%s.inc' % t)) as f:
            for line in f:
                m = re.match(r'E\("([^"]+)", (0x[0-9a-f]+), (0x[0-9a-f]+), (\w+), ([\w| ]+),', line)
                if not m:
                    continue
                if m.group(5) in ('RISCV_ENTRY_ALIAS', 'RISCV_ENTRY_ASSEMBLE_ONLY'):
                    continue
                names.append((t, m.group(1)))
    return names


# ---- the arithmetic rows: (mnemonic prefix, forms) -> (op, shape, extendA, extendB, mask mode, flags)
SAME, WIDEN, WIDEN_W, NARROW, EXTEND, MASK_DEST = 'SAME', 'WIDEN', 'WIDEN_W', 'NARROW', 'EXTEND', 'MASK_DEST'


def arith(op, shape=SAME, ea='ZERO', eb='ZERO', mask='BY_V0', flags=(), factor=0):
    return dict(op=op, shape=shape, ea=ea, eb=eb, mask=mask, flags=list(flags), factor=factor)


def build_arithmetic():
    rows = {}

    def add(prefix, forms, spec):
        for f in forms:
            rows[prefix + '.' + f] = dict(spec)

    vvx = ['vv', 'vx']
    vvxi = ['vv', 'vx', 'vi']
    for name, op in (('vadd', 'ADD'),):
        add(name, vvxi, arith('RVV_OP_' + op))
    add('vsub', vvx, arith('RVV_OP_SUB'))
    add('vrsub', ['vx', 'vi'], arith('RVV_OP_RSUB'))
    for n, op in (('vminu', 'MINU'), ('vmin', 'MIN'), ('vmaxu', 'MAXU'), ('vmax', 'MAX')):
        add(n, vvx, arith('RVV_OP_' + op))
    for n, op in (('vand', 'AND'), ('vor', 'OR'), ('vxor', 'XOR')):
        add(n, vvxi, arith('RVV_OP_' + op))
    for n, op in (('vsll', 'SLL'), ('vsrl', 'SRL'), ('vsra', 'SRA')):
        add(n, vvxi, arith('RVV_OP_' + op, flags=['IMMEDIATE_UNSIGNED']))
    for n, op in (('vmul', 'MUL'), ('vmulh', 'MULH'), ('vmulhu', 'MULHU'), ('vmulhsu', 'MULHSU'),
                  ('vdivu', 'DIVU'), ('vdiv', 'DIV'), ('vremu', 'REMU'), ('vrem', 'REM')):
        add(n, vvx, arith('RVV_OP_' + op))
    # carries: the sum / difference, the carry / borrow out (a mask), merge and move
    add('vadc', ['vvm', 'vxm', 'vim'], arith('RVV_OP_ADC', mask='DATA'))
    add('vsbc', ['vvm', 'vxm'], arith('RVV_OP_SBC', mask='DATA'))
    add('vmadc', ['vvm', 'vxm', 'vim'], arith('RVV_OP_MADC', MASK_DEST, mask='DATA'))
    add('vmadc', ['vv', 'vx', 'vi'], arith('RVV_OP_MADC', MASK_DEST, mask='NONE'))
    add('vmsbc', ['vvm', 'vxm'], arith('RVV_OP_MSBC', MASK_DEST, mask='DATA'))
    add('vmsbc', ['vv', 'vx'], arith('RVV_OP_MSBC', MASK_DEST, mask='NONE'))
    add('vmerge', ['vvm', 'vxm', 'vim'], arith('RVV_OP_MERGE', mask='DATA'))
    add('vmv.v', ['v', 'x', 'i'], arith('RVV_OP_MOVE', mask='NONE'))
    # compares
    for n, op, forms in (('vmseq', 'SEQ', vvxi), ('vmsne', 'SNE', vvxi), ('vmsltu', 'SLTU', vvx),
                         ('vmslt', 'SLT', vvx), ('vmsleu', 'SLEU', vvxi), ('vmsle', 'SLE', vvxi),
                         ('vmsgtu', 'SGTU', ['vx', 'vi']), ('vmsgt', 'SGT', ['vx', 'vi'])):
        add(n, forms, arith('RVV_OP_' + op, MASK_DEST))
    # multiply-add
    for n, op in (('vmacc', 'MACC'), ('vnmsac', 'NMSAC'), ('vmadd', 'MADD'), ('vnmsub', 'NMSUB')):
        add(n, vvx, arith('RVV_OP_' + op, flags=['READS_DESTINATION']))
    # widening
    for n, op, e in (('vwaddu', 'ADD', 'ZERO'), ('vwadd', 'ADD', 'SIGN'),
                     ('vwsubu', 'SUB', 'ZERO'), ('vwsub', 'SUB', 'SIGN')):
        add(n, vvx, arith('RVV_OP_' + op, WIDEN, e, e))
        add(n, ['wv', 'wx'], arith('RVV_OP_' + op, WIDEN_W, e, e))
    add('vwmul', vvx, arith('RVV_OP_MUL', WIDEN, 'SIGN', 'SIGN'))
    add('vwmulu', vvx, arith('RVV_OP_MUL', WIDEN, 'ZERO', 'ZERO'))
    add('vwmulsu', vvx, arith('RVV_OP_MUL', WIDEN, 'SIGN', 'ZERO'))
    add('vwmaccu', vvx, arith('RVV_OP_MACC', WIDEN, 'ZERO', 'ZERO', flags=['READS_DESTINATION']))
    add('vwmacc', vvx, arith('RVV_OP_MACC', WIDEN, 'SIGN', 'SIGN', flags=['READS_DESTINATION']))
    add('vwmaccsu', vvx, arith('RVV_OP_MACC', WIDEN, 'ZERO', 'SIGN', flags=['READS_DESTINATION']))
    add('vwmaccus', ['vx'], arith('RVV_OP_MACC', WIDEN, 'SIGN', 'ZERO', flags=['READS_DESTINATION']))
    # narrowing shifts
    add('vnsrl', ['wv', 'wx', 'wi'], arith('RVV_OP_NSRL', NARROW, 'ZERO', 'ZERO', flags=['IMMEDIATE_UNSIGNED']))
    add('vnsra', ['wv', 'wx', 'wi'], arith('RVV_OP_NSRA', NARROW, 'SIGN', 'ZERO', flags=['IMMEDIATE_UNSIGNED']))
    # extension
    for k, f in ((1, 'vf2'), (2, 'vf4'), (3, 'vf8')):
        rows['vzext.' + f] = arith('RVV_OP_EXTEND', EXTEND, 'ZERO', flags=['NO_B'], factor=k)
        rows['vsext.' + f] = arith('RVV_OP_EXTEND', EXTEND, 'SIGN', flags=['NO_B'], factor=k)
    rows['vid.v'] = arith('RVV_OP_ID', flags=['NO_B'])
    # fixed point
    sat = ['SATURATES']
    add('vsaddu', vvxi, arith('RVV_OP_SADDU', flags=sat))
    add('vsadd', vvxi, arith('RVV_OP_SADD', flags=sat))
    add('vssubu', vvx, arith('RVV_OP_SSUBU', flags=sat))
    add('vssub', vvx, arith('RVV_OP_SSUB', flags=sat))
    for n, op in (('vaaddu', 'AADDU'), ('vaadd', 'AADD'), ('vasubu', 'ASUBU'), ('vasub', 'ASUB')):
        add(n, vvx, arith('RVV_OP_' + op))
    add('vsmul', vvx, arith('RVV_OP_SMUL', flags=sat))
    add('vssrl', vvxi, arith('RVV_OP_SSRL', flags=['IMMEDIATE_UNSIGNED']))
    add('vssra', vvxi, arith('RVV_OP_SSRA', flags=['IMMEDIATE_UNSIGNED']))
    add('vnclipu', ['wv', 'wx', 'wi'], arith('RVV_OP_NCLIPU', NARROW, 'ZERO', 'ZERO', flags=sat + ['IMMEDIATE_UNSIGNED']))
    add('vnclip', ['wv', 'wx', 'wi'], arith('RVV_OP_NCLIP', NARROW, 'SIGN', 'ZERO', flags=sat + ['IMMEDIATE_UNSIGNED']))
    return rows


def arith_row(name, spec, fixed):
    flags = ' | '.join('RVV_FLAG_' + f for f in spec['flags']) or '0'
    cat = 'VECTOR_INTEGER'
    param = 'RVV_PARAM(%s, SH(%s), EX(%s), EX(%s), MK(%s), %s, %d)' % (
        spec['op'], spec['shape'], spec['ea'], spec['eb'], spec['mask'], flags, spec['factor'])
    return 'RVA("%s", %s),' % (name, param)


def build_other():
    rows = {}
    permute = {'vmv.x.s': 'RVV_PERMUTE_MV_X_S', 'vmv.s.x': 'RVV_PERMUTE_MV_S_X',
               'vslideup.vx': 'RVV_PERMUTE_SLIDEUP', 'vslideup.vi': 'RVV_PERMUTE_SLIDEUP',
               'vslidedown.vx': 'RVV_PERMUTE_SLIDEDOWN', 'vslidedown.vi': 'RVV_PERMUTE_SLIDEDOWN',
               'vslide1up.vx': 'RVV_PERMUTE_SLIDE1UP', 'vslide1down.vx': 'RVV_PERMUTE_SLIDE1DOWN',
               'vrgather.vv': 'RVV_PERMUTE_GATHER', 'vrgather.vx': 'RVV_PERMUTE_GATHER',
               'vrgather.vi': 'RVV_PERMUTE_GATHER', 'vrgatherei16.vv': 'RVV_PERMUTE_GATHER_EI16',
               'vcompress.vm': 'RVV_PERMUTE_COMPRESS'}
    for n, p in permute.items():
        rows[n] = 'RVP("%s", %s),' % (n, p)
    for r in (1, 2, 4, 8):
        rows['vmv%dr.v' % r] = 'RVP("vmv%dr.v", RVV_PERMUTE_MOVE | (%d << 8)),' % (r, r)
    for n, k, s in (('vredsum', 'SUM', 0), ('vredand', 'AND', 0), ('vredor', 'OR', 0), ('vredxor', 'XOR', 0),
                    ('vredminu', 'MIN', 0), ('vredmin', 'MIN', 1), ('vredmaxu', 'MAX', 0), ('vredmax', 'MAX', 1)):
        rows[n + '.vs'] = 'RVP("%s.vs", RVV_PERMUTE_REDUCE | (RVV_REDUCE_%s << 8) | (%d << 17)),' % (n, k, s)
    rows['vwredsumu.vs'] = 'RVP("vwredsumu.vs", RVV_PERMUTE_REDUCE | (RVV_REDUCE_SUM << 8) | (1 << 16)),'
    rows['vwredsum.vs'] = 'RVP("vwredsum.vs", RVV_PERMUTE_REDUCE | (RVV_REDUCE_SUM << 8) | (1 << 16) | (1 << 17)),'
    for n, op in (('vmand.mm', 'AND'), ('vmnand.mm', 'NAND'), ('vmandn.mm', 'ANDN'), ('vmxor.mm', 'XOR'),
                  ('vmor.mm', 'OR'), ('vmnor.mm', 'NOR'), ('vmorn.mm', 'ORN'), ('vmxnor.mm', 'XNOR'),
                  ('vcpop.m', 'CPOP'), ('vfirst.m', 'FIRST'), ('vmsbf.m', 'SBF'), ('vmsof.m', 'SOF'),
                  ('vmsif.m', 'SIF'), ('viota.m', 'IOTA')):
        rows[n] = 'RVM("%s", RVV_MASK_%s),' % (n, op)
    rows['vsetvli'] = 'RVC("vsetvli", 0),'
    rows['vsetivli'] = 'RVC("vsetivli", 1),'
    rows['vsetvl'] = 'RVC("vsetvl", 2),'
    # memory
    for eew in (8, 16, 32, 64):
        lg = {8: 0, 16: 1, 32: 2, 64: 3}[eew]
        for kind, k in (('u', 'RVV_MEM_UNIT'),):
            pass
        rows['vle%d.v' % eew] = 'RVL("vle%d.v", %s),' % (eew, 'RVV_MEMORY(RVV_MEM_UNIT, 0, 0, 1, %d)' % lg)
        rows['vle%dff.v' % eew] = 'RVL("vle%dff.v", %s),' % (eew, 'RVV_MEMORY(RVV_MEM_UNIT, 0, 1, 1, %d)' % lg)
        rows['vlse%d.v' % eew] = 'RVL("vlse%d.v", %s),' % (eew, 'RVV_MEMORY(RVV_MEM_STRIDED, 0, 0, 1, %d)' % lg)
        rows['vluxei%d.v' % eew] = 'RVL("vluxei%d.v", %s),' % (eew, 'RVV_MEMORY(RVV_MEM_INDEXED_UNORDERED, 0, 0, 1, %d)' % lg)
        rows['vloxei%d.v' % eew] = 'RVL("vloxei%d.v", %s),' % (eew, 'RVV_MEMORY(RVV_MEM_INDEXED_ORDERED, 0, 0, 1, %d)' % lg)
        rows['vse%d.v' % eew] = 'RVS("vse%d.v", %s),' % (eew, 'RVV_MEMORY(RVV_MEM_UNIT, 1, 0, 1, %d)' % lg)
        rows['vsse%d.v' % eew] = 'RVS("vsse%d.v", %s),' % (eew, 'RVV_MEMORY(RVV_MEM_STRIDED, 1, 0, 1, %d)' % lg)
        rows['vsuxei%d.v' % eew] = 'RVS("vsuxei%d.v", %s),' % (eew, 'RVV_MEMORY(RVV_MEM_INDEXED_UNORDERED, 1, 0, 1, %d)' % lg)
        rows['vsoxei%d.v' % eew] = 'RVS("vsoxei%d.v", %s),' % (eew, 'RVV_MEMORY(RVV_MEM_INDEXED_ORDERED, 1, 0, 1, %d)' % lg)
        for nf in range(2, 9):
            rows['vlseg%de%d.v' % (nf, eew)] = 'RVL("vlseg%de%d.v", %s),' % (nf, eew, 'RVV_MEMORY(RVV_MEM_UNIT, 0, 0, %d, %d)' % (nf, lg))
            rows['vlseg%de%dff.v' % (nf, eew)] = 'RVL("vlseg%de%dff.v", %s),' % (nf, eew, 'RVV_MEMORY(RVV_MEM_UNIT, 0, 1, %d, %d)' % (nf, lg))
            rows['vlsseg%de%d.v' % (nf, eew)] = 'RVL("vlsseg%de%d.v", %s),' % (nf, eew, 'RVV_MEMORY(RVV_MEM_STRIDED, 0, 0, %d, %d)' % (nf, lg))
            rows['vluxseg%dei%d.v' % (nf, eew)] = 'RVL("vluxseg%dei%d.v", %s),' % (nf, eew, 'RVV_MEMORY(RVV_MEM_INDEXED_UNORDERED, 0, 0, %d, %d)' % (nf, lg))
            rows['vloxseg%dei%d.v' % (nf, eew)] = 'RVL("vloxseg%dei%d.v", %s),' % (nf, eew, 'RVV_MEMORY(RVV_MEM_INDEXED_ORDERED, 0, 0, %d, %d)' % (nf, lg))
            rows['vsseg%de%d.v' % (nf, eew)] = 'RVS("vsseg%de%d.v", %s),' % (nf, eew, 'RVV_MEMORY(RVV_MEM_UNIT, 1, 0, %d, %d)' % (nf, lg))
            rows['vssseg%de%d.v' % (nf, eew)] = 'RVS("vssseg%de%d.v", %s),' % (nf, eew, 'RVV_MEMORY(RVV_MEM_STRIDED, 1, 0, %d, %d)' % (nf, lg))
            rows['vsuxseg%dei%d.v' % (nf, eew)] = 'RVS("vsuxseg%dei%d.v", %s),' % (nf, eew, 'RVV_MEMORY(RVV_MEM_INDEXED_UNORDERED, 1, 0, %d, %d)' % (nf, lg))
            rows['vsoxseg%dei%d.v' % (nf, eew)] = 'RVS("vsoxseg%dei%d.v", %s),' % (nf, eew, 'RVV_MEMORY(RVV_MEM_INDEXED_ORDERED, 1, 0, %d, %d)' % (nf, lg))
        for n in (1, 2, 4, 8):
            rows['vl%dre%d.v' % (n, eew)] = 'RVL("vl%dre%d.v", %s),' % (n, eew, 'RVV_MEMORY(RVV_MEM_WHOLE, 0, 0, %d, %d)' % (n, lg))
    for n in (1, 2, 4, 8):
        rows['vs%dr.v' % n] = 'RVS("vs%dr.v", %s),' % (n, 'RVV_MEMORY(RVV_MEM_WHOLE, 1, 0, %d, 0)' % n)
    rows['vlm.v'] = 'RVL("vlm.v", RVV_MEMORY(RVV_MEM_MASK, 0, 0, 1, 0)),'
    rows['vsm.v'] = 'RVS("vsm.v", RVV_MEMORY(RVV_MEM_MASK, 1, 0, 1, 0)),'
    return rows


def main():
    names = read_names()
    arithmetic = build_arithmetic()
    other = build_other()
    lines = ['// GENERATED by tests/riscv/tools/gen_vector_semantic_table.py: do not edit']
    missing = []
    for table, name in names:
        if name in arithmetic:
            lines.append(arith_row(name, arithmetic[name], True))
        elif name in other:
            lines.append(other[name])
        else:
            missing.append(name)
    text = '\n'.join(lines) + '\n'
    if '--check' in sys.argv:
        ok = os.path.exists(OUT) and open(OUT).read() == text and not missing
        if missing:
            print('no row:', ' '.join(missing))
        sys.exit(0 if ok else 1)
    with open(OUT, 'w') as f:
        f.write(text)
    print('%d rows, %d without a row: %s' % (len(lines) - 1, len(missing), ' '.join(missing)))


if __name__ == '__main__':
    main()
