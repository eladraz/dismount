#!/usr/bin/env python3
"""
Generates the rows of gRiscvVectorSemanticTable (RiscvSemanticTableVector.cpp), one row per vector mnemonic
of the encoding tables that the lifters of RiscvSemanticLiftVector*.cpp handle, in four files of
Source/dismount/proc/riscv:
  RiscvSemanticTable_vector.inc         config, int, mask, mem: the integer vector instructions
  RiscvSemanticTable_vector_float.inc   the floating point (RiscvEncodingTable_vector_fp.inc) and BFloat16
  RiscvSemanticTable_vector_bits.inc    Zvbb, Zvkb, Zvbc
  RiscvSemanticTable_vector_crypto.inc  Zvkg, Zvkned, Zvknha / Zvknhb, Zvksed, Zvksh
The files are included inside the initializer of the table (no macro wraps it).

  python3 tests/riscv/tools/gen_vector_semantic_table.py [--check]

--check exits 1 if a file is out of date or a decoded mnemonic of the tables has no row.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
SRC = os.path.join(REPO, 'Source', 'dismount', 'proc', 'riscv')
OUTS = {'int': os.path.join(SRC, 'RiscvSemanticTable_vector.inc'),
        'float': os.path.join(SRC, 'RiscvSemanticTable_vector_float.inc'),
        'bits': os.path.join(SRC, 'RiscvSemanticTable_vector_bits.inc'),
        'crypto': os.path.join(SRC, 'RiscvSemanticTable_vector_crypto.inc')}
TABLES = ['config', 'int', 'mask', 'mem', 'fp', 'ext']


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


# ---- the floating point rows: (operation, shape, mask mode, flags)
def build_float():
    rows = {}

    def fl(name, op, shape='SAME', mask='BY_V0', flags=()):
        f = ' | '.join('RVF_FLAG_' + x for x in flags) or '0'
        rows[name] = 'RVX("%s", RVF_PARAM(RVF_OP_%s, SH(%s), MK(%s), %s)),' % (name, op, shape, mask, f)

    def both(prefix, op, shape='SAME', mask='BY_V0', flags=(), forms=('vv', 'vf')):
        for f in forms:
            fl(prefix + '.' + f, op, shape, mask, flags)

    both('vfadd', 'ADD')
    both('vfsub', 'SUB')
    fl('vfrsub.vf', 'RSUB')
    both('vfmul', 'MUL')
    both('vfdiv', 'DIV')
    fl('vfrdiv.vf', 'RDIV')
    both('vfmin', 'MIN')
    both('vfmax', 'MAX')
    for n, op in (('vfsgnj', 'SGNJ'), ('vfsgnjn', 'SGNJN'), ('vfsgnjx', 'SGNJX')):
        both(n, op)
    # the multiply-add family: vd = +-(vs1 * vs2) +- vd, vd = +-(vs1 * vd) +- vs2
    fma = (('vfmacc', ()), ('vfnmacc', ('NEGATE_PRODUCT', 'NEGATE_ADDEND')), ('vfmsac', ('NEGATE_ADDEND',)),
           ('vfnmsac', ('NEGATE_PRODUCT',)), ('vfmadd', ('MULTIPLY_DESTINATION',)),
           ('vfnmadd', ('MULTIPLY_DESTINATION', 'NEGATE_PRODUCT', 'NEGATE_ADDEND')),
           ('vfmsub', ('MULTIPLY_DESTINATION', 'NEGATE_ADDEND')),
           ('vfnmsub', ('MULTIPLY_DESTINATION', 'NEGATE_PRODUCT')))
    for n, fg in fma:
        both(n, 'MACC', flags=fg)
    for n, fg in (('vfwmacc', ()), ('vfwnmacc', ('NEGATE_PRODUCT', 'NEGATE_ADDEND')),
                  ('vfwmsac', ('NEGATE_ADDEND',)), ('vfwnmsac', ('NEGATE_PRODUCT',))):
        both(n, 'MACC', 'WIDEN', flags=fg)
    both('vfwmaccbf16', 'MACC', 'WIDEN', flags=['BFLOAT16'])
    for n, op in (('vfwadd', 'ADD'), ('vfwsub', 'SUB')):
        both(n, op, 'WIDEN')
        both(n, op, 'WIDEN_W', forms=('wv', 'wf'))
    both('vfwmul', 'MUL', 'WIDEN')
    fl('vfsqrt.v', 'SQRT')
    fl('vfrsqrt7.v', 'RSQRT7')
    fl('vfrec7.v', 'REC7')
    fl('vfclass.v', 'CLASS')
    fl('vfmerge.vfm', 'MERGE', mask='DATA')
    fl('vfmv.v.f', 'MOVE', mask='NONE')
    for n, op, forms in (('vmfeq', 'EQ', ('vv', 'vf')), ('vmfne', 'NE', ('vv', 'vf')), ('vmflt', 'LT', ('vv', 'vf')),
                         ('vmfle', 'LE', ('vv', 'vf')), ('vmfgt', 'GT', ('vf',)), ('vmfge', 'GE', ('vf',))):
        both(n, op, 'MASK_DEST', forms=forms)
    # the conversions
    for suffix, shape in (('', 'SAME'), ('w', 'WIDEN'), ('n', 'NARROW')):
        src = '.w' if shape == 'NARROW' else '.v'
        base = 'vf%scvt' % suffix
        for n, op, fg in (('xu.f', 'TO_INTEGER', ['UNSIGNED']), ('x.f', 'TO_INTEGER', []),
                          ('f.xu', 'FROM_INTEGER', ['UNSIGNED']), ('f.x', 'FROM_INTEGER', []),
                          ('rtz.xu.f', 'TO_INTEGER', ['UNSIGNED', 'RTZ']),
                          ('rtz.x.f', 'TO_INTEGER', ['RTZ'])):
            if shape != 'SAME' or True:
                fl('%s.%s%s' % (base, n, src), op, shape, flags=fg)
        if shape != 'SAME':
            fl('%s.f.f%s' % (base, src), 'CONVERT', shape)
    fl('vfncvt.rod.f.f.w', 'CONVERT', 'NARROW', flags=['ROD'])
    fl('vfwcvtbf16.f.f.v', 'CONVERT', 'WIDEN', flags=['BFLOAT16'])
    fl('vfncvtbf16.f.f.w', 'CONVERT', 'NARROW', flags=['BFLOAT16'])
    # the reductions
    for n, k, w in (('vfredosum', 'SUM_ORDERED', 0), ('vfredusum', 'SUM_UNORDERED', 0), ('vfredmax', 'MAX', 0),
                    ('vfredmin', 'MIN', 0), ('vfwredosum', 'SUM_ORDERED', 1), ('vfwredusum', 'SUM_UNORDERED', 1)):
        rows[n + '.vs'] = 'RVR("%s.vs", RVF_REDUCE_%s | (%d << 8)),' % (n, k, w)
    # the moves between the f registers and the vector, and the slides with a float scalar
    rows['vfmv.f.s'] = 'RVP("vfmv.f.s", RVV_PERMUTE_MV_X_S | (1 << 8)),'
    rows['vfmv.s.f'] = 'RVP("vfmv.s.f", RVV_PERMUTE_MV_S_X | (1 << 8)),'
    rows['vfslide1up.vf'] = 'RVP("vfslide1up.vf", RVV_PERMUTE_SLIDE1UP | (1 << 8)),'
    rows['vfslide1down.vf'] = 'RVP("vfslide1down.vf", RVV_PERMUTE_SLIDE1DOWN | (1 << 8)),'
    return rows


def build_extension():
    """Zvbb, Zvkb, Zvbc and the vector cryptography rows."""
    rows = {}

    def bits(name, op, shape='SAME', ea='ZERO', eb='ZERO', flags=()):
        f = ' | '.join('RVV_FLAG_' + x for x in flags) or '0'
        rows[name] = 'RVB("%s", RVV_PARAM(RVB_OP_%s, SH(%s), EX(%s), EX(%s), MK(BY_V0), %s, 0)),' % (
            name, op, shape, ea, eb, f)

    bits('vandn.vv', 'ANDN')
    bits('vandn.vx', 'ANDN')
    for n, op in (('vbrev8.v', 'BREV8'), ('vrev8.v', 'REV8'), ('vbrev.v', 'BREV'), ('vclz.v', 'CLZ'),
                  ('vctz.v', 'CTZ'), ('vcpop.v', 'CPOP')):
        bits(n, op, flags=['NO_B'])
    for n, op in (('vrol', 'ROL'), ('vror', 'ROR')):
        bits(n + '.vv', op)
        bits(n + '.vx', op)
    bits('vror.vi', 'ROR', flags=['IMMEDIATE_UNSIGNED'])
    for f in ('vv', 'vx', 'vi'):
        bits('vwsll.' + f, 'WSLL', 'WIDEN', flags=['IMMEDIATE_UNSIGNED'])
    for n, op in (('vclmul', 'CLMUL'), ('vclmulh', 'CLMULH')):
        bits(n + '.vv', op)
        bits(n + '.vx', op)
    crypto = (('vaesef.vv', 'AESEF_VV'), ('vaesef.vs', 'AESEF_VS'), ('vaesem.vv', 'AESEM_VV'),
              ('vaesem.vs', 'AESEM_VS'), ('vaesdf.vv', 'AESDF_VV'), ('vaesdf.vs', 'AESDF_VS'),
              ('vaesdm.vv', 'AESDM_VV'), ('vaesdm.vs', 'AESDM_VS'), ('vaesz.vs', 'AESZ_VS'),
              ('vaeskf1.vi', 'AESKF1'), ('vaeskf2.vi', 'AESKF2'), ('vghsh.vv', 'GHSH'), ('vgmul.vv', 'GMUL'),
              ('vsha2ms.vv', 'SHA2MS'), ('vsha2ch.vv', 'SHA2CH'), ('vsha2cl.vv', 'SHA2CL'),
              ('vsm3me.vv', 'SM3ME'), ('vsm3c.vi', 'SM3C'), ('vsm4k.vi', 'SM4K'), ('vsm4r.vv', 'SM4R_VV'),
              ('vsm4r.vs', 'SM4R_VS'))
    for name, op in crypto:
        rows[name] = 'RVK("%s", RVK_%s),' % (name, op)
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


def area_of(table, name):
    if table in ('config', 'int', 'mask', 'mem'):
        return 'int'
    if table == 'fp' or name.startswith(('vfwcvtbf16', 'vfncvtbf16', 'vfwmaccbf16')):
        return 'float'
    if name.startswith(('vg', 'vaes', 'vsm', 'vsha')):
        return 'crypto'
    return 'bits'


def main():
    names = read_names()
    arithmetic = build_arithmetic()
    other = build_other()
    floats = build_float()
    extra = build_extension()
    texts = {a: ['// GENERATED by tests/riscv/tools/gen_vector_semantic_table.py: do not edit'] for a in OUTS}
    missing = []
    for table, name in names:
        area = area_of(table, name)
        if name in arithmetic:
            texts[area].append(arith_row(name, arithmetic[name], True))
        elif name in other:
            texts[area].append(other[name])
        elif name in floats:
            texts[area].append(floats[name])
        elif name in extra:
            texts[area].append(extra[name])
        else:
            missing.append(name)
    ok = True
    for area, path in OUTS.items():
        text = '\n'.join(texts[area]) + '\n'
        if '--check' in sys.argv:
            ok = ok and os.path.exists(path) and open(path).read() == text
        else:
            with open(path, 'w') as f:
                f.write(text)
    if '--check' in sys.argv:
        if missing:
            print('no row:', ' '.join(missing))
        sys.exit(0 if ok and not missing else 1)
    print('%d rows, %d without a row: %s' % (sum(len(t) - 1 for t in texts.values()), len(missing), ' '.join(missing)))


if __name__ == '__main__':
    main()
