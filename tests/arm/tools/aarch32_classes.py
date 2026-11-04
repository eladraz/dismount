#!/usr/bin/env python3
"""The operand classes of the LLVM ARM backend, mapped to the operand specs of
dismount (ArmOperandKind / the schemes of ArmOperandCodecA32.h).

A handler gets the instruction, the operand (with the instruction bits of its
fields) and the way the operand is written in the assembly string, and returns
a list of specs, one for each comma separated operand of the text. A spec is a
tuple (kind, position, flags, extra) of C++ expressions.

The operands are described by the layout of their bits (Operand.layout()): an
assert on it checks the fixed positions which the C++ scheme assumes.
"""
import re


class Piece(object):
    """One comma separated operand of the assembly string."""

    def __init__(self, text):
        self.text = text
        self.names = re.findall(r'\$(\w+)', text)
        self.writeback = text.rstrip().endswith('!') or '!' in text.replace('$', ' ')
        self.caret = '^' in text
        # The literal text without the names
        self.literal = re.sub(r'\$\w+', '', text).strip()


class Unmapped(Exception):
    pass


def S(kind, position=0, flags='0', extra='0'):
    return (kind, position, flags, extra)


def need(op, layout):
    """Check the layout of the operand."""
    if op is None or op.layout() != layout:
        raise Unmapped('layout of %s is %s, expected %s' % (op, op.layout() if op else None, layout))


def first_bit(op):
    """The instruction bit of the low bit of a contiguous operand; width."""
    runs = op.runs()
    if len(runs) != 1 or runs[0][0] != 0:
        raise Unmapped('not contiguous: %r' % (op,))
    return runs[0][1], runs[0][2]


# ---------------------------------------------------------------------------
# The general purpose registers: class -> (ArmA32GprClass, width)

GPR_CLASSES = {
    'GPR': 'ARM_A32_GPR_ANY', 'GPRnopc': 'ARM_A32_GPR_ANY', 'rGPR': 'ARM_A32_GPR_ANY',
    'GPRnosp': 'ARM_A32_GPR_ANY', 'tGPR': 'ARM_A32_GPR_ANY',
    'GPRwithAPSR': 'ARM_A32_GPR_APSR', 'GPRwithZRnosp': 'ARM_A32_GPR_NO_SP',
    'GPRwithZR': 'ARM_A32_GPR_ZR', 'tcGPR': 'ARM_A32_GPR_TAIL_CALL',
    'GPRPairOp': 'ARM_A32_GPR_PAIR_CLASS',
}


def gpr(ctx, op, piece):
    cls = op.cls
    if cls == 'GPRsp' or cls == 'GPRlr' or cls == 'GPRsp_and_pc':
        if op.bits:
            if cls == 'GPRsp':
                position, width = first_bit(op)
                return [S('ARM_KIND_GPR', position, 'ARM_A32_GPR_SP_ONLY', width)]
            raise Unmapped(cls)
        return [S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_FIXED_SP' if cls == 'GPRsp' else 'ARM_A32_FIXED_LR')]
    position, width = first_bit(op)
    flags = GPR_CLASSES[cls]
    if piece.writeback:
        flags += ' | ARM_A32_GPR_WRITEBACK'
    return [S('ARM_KIND_GPR', position, flags, width)]


# ---------------------------------------------------------------------------
# Immediates: name -> (hex, plus_one) for the plain ones. The LLVM printer
# prints the printOperand ones in hexadecimal

PLAIN_IMM_PRINT = ('printOperand', 'printNoHashImmediate')


def plain_immediate(ctx, op, piece, records):
    position, width = first_bit(op)
    rec = records[op.cls]
    printer = rec['PrintMethod']
    flags = []
    plus = False
    if printer == 'printOperand':
        flags.append('ARM_SPEC_HEX')
    elif printer == 'printImmPlusOneOperand':
        flags.append('ARM_SPEC_HEX')
        flags.append('ARM_SPEC_PLUS_ONE')
    else:
        raise Unmapped('print method %s of %s' % (printer, op.cls))
    return [S('ARM_KIND_IMMEDIATE', position, ' | '.join(flags), width)]


# Handlers of the other areas (aarch32_thumb.py, aarch32_simd.py...): functions
# (ctx, op, piece, records) which return a list of specs, or None for the
# operand classes that are not theirs. They are tried before the ones below
EXTRA_HANDLERS = []
# Functions (ctx, pieces, index) which handle the piece at 'index' of the operand
# text, possibly with the next ones: return (specs, next index) or None
PIECE_HANDLERS = []


def handle(ctx, op, piece, records):
    """Return the list of specs of an operand of the assembly string."""
    for extra in EXTRA_HANDLERS:
        result = extra(ctx, op, piece, records)
        if result is not None:
            return result
    cls = op.cls
    if cls in GPR_CLASSES or cls in ('GPRsp', 'GPRlr'):
        return gpr(ctx, op, piece)
    if cls == 'mod_imm':
        need(op, '0-11@0')
        unsigned = 'ARM_A32_MODIMM_UNSIGNED' if ctx.ins.name == 'MSRi' else '0'
        return [S('ARM_KIND_A32_MODIFIED_IMMEDIATE', 0, unsigned, '0')]
    if cls in ('so_reg_imm', 'shift_so_reg_imm'):
        need(op, '0-3@0 5-11@5')
        return [S('ARM_KIND_SHIFTED_REGISTER', 0, '0', 'ARM_A32_SHIFT_SO_IMM')]
    if cls in ('so_reg_reg', 'shift_so_reg_reg'):
        need(op, '0-3@0 5-6@5 8-11@8')
        return [S('ARM_KIND_SHIFTED_REGISTER', 0, 'ARM_SPEC_REGISTER_SHIFT', 'ARM_A32_SHIFT_SO_REG')]
    if cls == 'reglist':
        flags = []
        if piece.caret:
            flags.append('ARM_A32_LIST_CARET')
        return [S('ARM_KIND_GPR_LIST', 0, ' | '.join(flags) if flags else '0', 16)]
    if cls == 'imm0_65535':
        need(op, '0-3@0 4-15@8')
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_IMM16_SPLIT')]
    if cls == 'imm0_65535_expr':
        need(op, '0-11@0 12-15@16')
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_MOVW_IMM16')]
    if cls == 'bf_inv_mask_imm':
        need(op, '0-4@7 5-9@16')
        return [S('ARM_KIND_A32_FIELD', 7, '0', 'ARM_A32_FIELD_BF_LSB'),
                S('ARM_KIND_A32_FIELD', 16, '0', 'ARM_A32_FIELD_BF_WIDTH')]
    if cls == 'arm_br_target' or cls == 'arm_bl_target':
        need(op, '0-23@0')
        return [S('ARM_KIND_BRANCH_TARGET', 0, 'ARM_A32_BRANCH_B', 24)]
    if cls == 'arm_blx_target':
        need(op, '0@24 1-24@0')
        return [S('ARM_KIND_BRANCH_TARGET', 0, 'ARM_A32_BRANCH_BLX', 24)]
    if cls == 'p_imm':
        need(op, '0-3@8')
        return [S('ARM_KIND_A32_OPTION', 8, '0', 'ARM_A32_OPT_COPROC')]
    if cls == 'c_imm':
        position, width = first_bit(op)
        return [S('ARM_KIND_A32_OPTION', position, '0', 'ARM_A32_OPT_CREG')]
    if cls == 'memb_opt':
        need(op, '0-3@0')
        return [S('ARM_KIND_A32_OPTION', 0, '0', 'ARM_A32_OPT_BARRIER')]
    if cls == 'instsyncb_opt':
        need(op, '0-3@0')
        return [S('ARM_KIND_A32_OPTION', 0, '0', 'ARM_A32_OPT_ISB')]
    if cls == 'iflags_op':
        need(op, '0-2@6')
        return [S('ARM_KIND_A32_OPTION', 6, '0', 'ARM_A32_OPT_IFLAGS')]
    if cls == 'setend_op':
        need(op, '0@9')
        return [S('ARM_KIND_A32_OPTION', 9, '0', 'ARM_A32_OPT_ENDIAN')]
    if cls == 'msr_mask':
        need(op, '0-3@16 4@22')
        return [S('ARM_KIND_A32_OPTION', 16, '0', 'ARM_A32_OPT_MSR_MASK')]
    if cls == 'banked_reg':
        need(op, '0-3@16 4@8 5@22')
        return [S('ARM_KIND_A32_OPTION', 16, '0', 'ARM_A32_OPT_BANKED')]
    if cls == 'tsb_opt':
        return [S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_FIXED_CSYNC')]
    if cls == 'pkh_lsl_amt':
        need(op, '0-4@7')
        return [('SHIFT', 'ARM_A32_SHIFT_PKH_LSL')]
    if cls == 'pkh_asr_amt':
        need(op, '0-4@7')
        return [('SHIFT', 'ARM_A32_SHIFT_PKH_ASR')]
    if cls == 'shift_imm':
        need(op, '0-4@7 5@6')
        return [('SHIFT', 'ARM_A32_SHIFT_SAT')]
    if cls == 'rot_imm':
        need(op, '0-1@10')
        return [('SHIFT', 'ARM_A32_SHIFT_ROTATE')]
    if cls.startswith('imm'):
        return plain_immediate(ctx, op, piece, records)
    # The addressing modes
    if cls == 'addr_offset_none':
        need(op, '0-3@16')
        return [S('ARM_KIND_A32_MEMORY', 16, '0', 'ARM_A32_ADDR_BASE')]
    if cls in ('addrmode_imm12', 'addrmode_imm12_pre'):
        need(op, '0-11@0 12@23 13-16@16')
        pre = 'ARM_A32_MEM_PRE' if cls.endswith('_pre') else '0'
        return [S('ARM_KIND_A32_MEMORY', 16, pre, 'ARM_A32_ADDR_IMM12')]
    if cls == 'ldst_so_reg':
        need(op, '0-3@0 5-11@5 12@23 13-16@16')
        pre = 'ARM_A32_MEM_PRE' if piece.writeback else '0'
        return [S('ARM_KIND_A32_MEMORY', 16, pre, 'ARM_A32_ADDR_SO_REG')]
    if cls in ('addrmode3', 'addrmode3_pre'):
        need(op, '0-3@0 4-7@8 8@23 9-12@16 13@22')
        pre = 'ARM_A32_MEM_PRE' if cls.endswith('_pre') else '0'
        return [S('ARM_KIND_A32_MEMORY', 16, pre, 'ARM_A32_ADDR_MODE3')]
    if cls in ('addrmode5', 'addrmode5_pre'):
        need(op, '0-7@0 8@23 9-12@16')
        pre = 'ARM_A32_MEM_PRE' if cls.endswith('_pre') else '0'
        return [S('ARM_KIND_A32_MEMORY', 16, pre, 'ARM_A32_ADDR_MODE5')]
    raise Unmapped('operand class %s' % cls)


# The classes which follow a "$addr" (addr_offset_none) in the text and make
# it a post indexed memory operand: class -> (scheme, layout)
POST_CLASSES = {
    'am2offset_imm': ('ARM_A32_ADDR_MODE2_POST_IMM', '0-11@0 12@23'),
    'am2offset_reg': ('ARM_A32_ADDR_MODE2_POST_REG', '0-3@0 5-11@5 12@23'),
    'am3offset': ('ARM_A32_ADDR_MODE3_POST', '0-3@0 4-7@8 8@23 9@22'),
    'postidx_imm8': ('ARM_A32_ADDR_POST_IMM8', '0-3@0 4-7@8 8@23'),
    'postidx_reg': ('ARM_A32_ADDR_POST_REG', '0-3@0 4@23'),
    'postidx_imm8s4': ('ARM_A32_ADDR_MODE5_POST', '0-7@0 8@23'),
    'coproc_option_imm': ('ARM_A32_ADDR_MODE5_OPTION', '0-7@0'),
}


# The Thumb offsets which follow the "[Rn]" in the text without a comma
THUMB_POST_CLASSES = {
    't2am_imm8_offset': 'ARM_A32_ADDR_T32_IMM8_POST',
    't2am_imm8s4_offset': 'ARM_A32_ADDR_T32_IMM8S4_POST',
}
