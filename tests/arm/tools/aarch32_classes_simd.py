#!/usr/bin/env python3
"""The operand classes of the floating point and Advanced SIMD instructions of
the LLVM ARM backend, mapped to the specs of ArmOperandCodecA32Simd.h.

The module registers its handlers in aarch32_classes (PIECE_HANDLERS for the
pieces of the operand text which are made of several operands: the lists, the
scalars, the memory operands with their offset; EXTRA_HANDLERS for the
single operands).
"""
import re

import aarch32_classes as classes
from aarch32_classes import S, Unmapped

# The layouts of the registers: layout -> (position, top bit position)
D_LAYOUTS = {'0-3@12 4@22': (12, 22), '0-3@16 4@7': (16, 7), '0-3@0 4@5': (0, 5)}
S_LAYOUTS = {'0@22 1-4@12': (12, 22), '0@7 1-4@16': (16, 7), '0@5 1-4@0': (0, 5)}


def register_spec(op):
    """The spec of a d, q or s register operand, None if it isn't one."""
    cls = op.cls
    lay = op.layout()
    if cls in ('DPR', 'QPR'):
        if lay not in D_LAYOUTS:
            raise Unmapped('layout %s of %s' % (lay, cls))
        pos, top = D_LAYOUTS[lay]
        return S('ARM_KIND_A32_DREG' if cls == 'DPR' else 'ARM_KIND_A32_QREG', pos, '0', top)
    if cls == 'DPR_VFP2':
        pos, width = classes.first_bit(op)
        return S('ARM_KIND_A32_DREG', pos, 'ARM_A32_REG_NO_TOP', 0)
    if cls == 'DPR_8':
        pos, width = classes.first_bit(op)
        return S('ARM_KIND_A32_DREG', pos, 'ARM_A32_REG_NO_TOP | ARM_A32_REG_BITS3', 0)
    if cls in ('SPR', 'HPR'):
        if lay not in S_LAYOUTS:
            raise Unmapped('layout %s of %s' % (lay, cls))
        pos, low = S_LAYOUTS[lay]
        return S('ARM_KIND_A32_SREG', pos, '0', low)
    if cls == 'SPR_8':
        if lay != '0@5 1-3@0':
            raise Unmapped('layout %s of SPR_8' % lay)
        return S('ARM_KIND_A32_SREG', 0, 'ARM_A32_REG_BITS3', 5)
    return None


def record_family(ctx):
    """('VLD'|'VST', n, kind) of a load / store record name; kind is STRUCT,
    LANE or DUP."""
    m = re.match(r'V(LD|ST)(\d)(LN|DUP)?', ctx.ins.name)
    if not m:
        raise Unmapped('record name %s' % ctx.ins.name)
    kind = {None: 'STRUCT', 'LN': 'LANE', 'DUP': 'DUP'}[m.group(3)]
    return m.group(1), int(m.group(2)), kind


# The index scheme of the scalar operands by the layout of the index
INDEX_SCHEMES = {
    ('VectorIndex16', '0@3 1@5'): 'ARM_A32_INDEX_M16',
    ('VectorIndex32', '0@5'): 'ARM_A32_INDEX_M32',
    ('VectorIndex32', '0@3'): 'ARM_A32_INDEX_S32',
    ('VectorIndex64', '-'): 'ARM_A32_INDEX_ZERO',
    ('VectorIndex8', '0-2@17'): 'ARM_A32_INDEX_DUP8',
    ('VectorIndex16', '0-1@18'): 'ARM_A32_INDEX_DUP16',
    ('VectorIndex32', '0@19'): 'ARM_A32_INDEX_DUP32',
    ('VectorIndex8', '0-1@5 2@21'): 'ARM_A32_INDEX_MOV8',
    ('VectorIndex16', '0@6 1@21'): 'ARM_A32_INDEX_MOV16',
    ('VectorIndex32', '0@21'): 'ARM_A32_INDEX_MOV32',
}


def scalar_spec(reg, index):
    """The spec of a register followed by its lane index."""
    spec = register_spec(reg)
    if spec is None:
        raise Unmapped('scalar register class %s' % reg.cls)
    key = (index.cls, index.layout() if index.bits else '-')
    if key not in INDEX_SCHEMES:
        raise Unmapped('scalar index %r' % (key,))
    flags = 0
    names = []
    if spec[3] == 0 and 'NO_TOP' in spec[2]:
        extra_position = 31
    else:
        extra_position = spec[3]
    flags_text = ['%d' % (extra_position << 3)]
    if 'NO_TOP' in spec[2]:
        flags_text.append('ARM_A32_REG_NO_TOP')
    if 'BITS3' in spec[2]:
        flags_text.append('ARM_A32_REG_BITS3')
    if spec[0] == 'ARM_KIND_A32_SREG':
        flags_text.append('ARM_A32_REG_SINGLE')
    if spec[0] == 'ARM_KIND_A32_QREG':
        raise Unmapped('q scalar')
    return S('ARM_KIND_A32_NEON_SCALAR', spec[1], ' | '.join(flags_text), INDEX_SCHEMES[key])


LIST_CLASSES = re.compile(r'^VecList')


def memory_scheme(ctx):
    verb, n, kind = record_family(ctx)
    return 'ARM_A32_NMEM_%s' % kind, n


def piece_handler(ctx, pieces, index):
    """Handle the pieces of the operand text which are not one operand."""
    piece = pieces[index]
    ins = ctx.ins
    names = piece.names
    if not names:
        return None
    ops = [ins.operand(n) for n in names]
    if any(o is None for o in ops):
        return None
    # A list written with braces: the registers of a lane or all lanes load
    if '{' in piece.text:
        verb, n, kind = record_family(ctx)
        vd = ops[0]
        spec = register_spec(vd)
        if spec is None or spec[0] != 'ARM_KIND_A32_DREG' or vd.layout() != '0-3@12 4@22':
            raise Unmapped('list first register')
        scheme = {'STRUCT': 'ARM_A32_LIST_STRUCT', 'LANE': 'ARM_A32_LIST_LANE',
                  'DUP': 'ARM_A32_LIST_DUP'}[kind]
        return [S('ARM_KIND_A32_FP_LIST', 12, str(n), scheme)], index + 1
    # A vector list class
    if len(ops) == 1 and LIST_CLASSES.match(ops[0].cls):
        op = ops[0]
        verb, n, kind = None, None, None
        if ins.name.startswith(('VTBL', 'VTBX')):
            count = int(re.match(r'VT(?:BL|BX)(\d)', ins.name).group(1))
            if op.layout() != '0-3@16 4@7':
                raise Unmapped('tbl list layout')
            return [S('ARM_KIND_A32_FP_LIST', 16, str(count), 'ARM_A32_LIST_TBL')], index + 1
        verb, n, kind = record_family(ctx)
        if op.layout() != '0-3@12 4@22':
            raise Unmapped('list layout %s' % op.layout())
        scheme = {'STRUCT': 'ARM_A32_LIST_STRUCT', 'LANE': 'ARM_A32_LIST_LANE',
                  'DUP': 'ARM_A32_LIST_DUP'}[kind]
        return [S('ARM_KIND_A32_FP_LIST', 12, str(n), scheme)], index + 1
    # A register and its lane
    if len(ops) == 2 and ops[1].cls.startswith('VectorIndex'):
        return [scalar_spec(ops[0], ops[1])], index + 1
    # The memory of the structure loads and stores
    if ops[0].cls.startswith('addrmode6'):
        scheme, n = memory_scheme(ctx)
        consumed = index + 1
        if consumed < len(pieces) and pieces[consumed].names == ['Rm'] and \
                ins.operand('Rm').cls == 'rGPR':
            consumed += 1
        if len(ops) > 2:
            raise Unmapped('memory piece %s' % piece.text)
        return [S('ARM_KIND_A32_NEON_MEMORY', 16, str(n), scheme)], consumed
    return None


def sibling_register(ctx, op):
    """The operand with fields of the same class which a tied operand repeats."""
    for other in ctx.ins.operands:
        if other is not op and other.cls == op.cls and other.bits:
            return other
    raise Unmapped('no register for the tied %s' % op.name)


def handler(ctx, op, piece, records):
    cls = op.cls
    if cls in ('SPR', 'DPR', 'HPR', 'QPR') and not op.bits:
        other = sibling_register(ctx, op)
        spec = register_spec(other)
        if op.name in ('src2', 'dst2'):
            # The next register (vmov s0, s1, r0, r1)
            return [(spec[0], spec[1], 'ARM_A32_REG_NEXT', spec[3])]
        return [spec]
    spec = register_spec(op)
    if spec is not None:
        return [spec]
    if cls == 'complexrotateop':
        pos, width = classes.first_bit(op)
        return [S('ARM_KIND_A32_FIELD', pos, '0', 'ARM_A32_SIMD_FIELD_ROT')]
    if cls == 'complexrotateopodd':
        return [S('ARM_KIND_A32_FIELD', 24, '0', 'ARM_A32_SIMD_FIELD_ROT_ODD')]
    if cls in ('fbits16', 'fbits32'):
        return [S('ARM_KIND_A32_FIELD', 0, cls[5:], 'ARM_A32_SIMD_FIELD_FBITS')]
    if cls == 'neon_vcvt_imm32':
        return [S('ARM_KIND_A32_FIELD', 16, '0', 'ARM_A32_SIMD_FIELD_VCVT_IMM')]
    if cls.startswith('shr_imm'):
        size = int(cls[7:])
        return [S('ARM_KIND_A32_FIELD', 16, str(size), 'ARM_A32_SIMD_FIELD_SHIFT_RIGHT')]
    if cls == 'i32imm':
        pos, width = classes.first_bit(op)
        return [S('ARM_KIND_A32_FIELD', pos, str(width), 'ARM_A32_SIMD_FIELD_SHIFT_LEFT')]
    if cls in ('imm8', 'imm16', 'imm32') and not op.bits:
        return [S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_SIMD_FIXED_%s' % cls[3:])]
    if cls in ('imm0_1', 'imm0_3', 'imm0_7', 'imm0_15', 'imm1_7', 'imm1_15', 'imm1_31') and op.bits:
        pos, width = classes.first_bit(op)
        return [S('ARM_KIND_A32_FIELD', pos, str(width), 'ARM_A32_SIMD_FIELD_PLAIN')]
    if cls.startswith('nImmSplatI') or cls == 'nImmVMOVI32':
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_SIMD_FIELD_MODIMM')]
    if cls == 'nImmVMOVF32':
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_SIMD_FIELD_MODIMM_FLOAT')]
    if cls in ('vfp_f16imm', 'vfp_f32imm', 'vfp_f64imm'):
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_SIMD_FIELD_VFP_IMM')]
    # The lists of the vldm / vstm
    if cls == 'dpr_reglist':
        if op.bits:
            lay = op.layout()
            if lay == '1-7@1 8-11@12 12@22':
                return [S('ARM_KIND_A32_FP_LIST', 12, '22', 'ARM_A32_LIST_VFP_D')]
            if lay == '1-7@1 8-11@12':
                return [S('ARM_KIND_A32_FP_LIST', 12, '255', 'ARM_A32_LIST_VFP_X')]
        raise Unmapped('dpr_reglist')
    if cls == 'spr_reglist':
        return [S('ARM_KIND_A32_FP_LIST', 12, '22', 'ARM_A32_LIST_VFP_S')]
    if cls == 'addrmode5':
        # (vldr / vstr: the A32 coprocessor addressing, offset form)
        return [S('ARM_KIND_A32_MEMORY', 16, '0', 'ARM_A32_ADDR_MODE5')]
    if cls == 'addrmode5fp16':
        return [S('ARM_KIND_A32_MEMORY', 16, '0', 'ARM_A32_SIMD_ADDR_MODE5_FP16')]
    return None


classes.PIECE_HANDLERS.append(piece_handler)
classes.EXTRA_HANDLERS.append(handler)
