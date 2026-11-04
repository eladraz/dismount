#!/usr/bin/env python3
"""The Thumb operand classes of the LLVM ARM backend (T16 and T32), mapped to
the operand specs of dismount. Registered with aarch32_classes.EXTRA_HANDLERS
by aarch32_thumb.py; the A32 classes which have the same fields in Thumb (the
barrier options, the coprocessor numbers...) are handled by aarch32_classes.
"""
import aarch32_classes as classes
from aarch32_classes import S, Unmapped, need, first_bit


def t_gpr(ctx, op, piece):
    """Registers of the T16 forms (3 bit fields, the high registers)."""
    layout = op.layout()
    if op.cls == 'tGPR':
        position, width = first_bit(op)
        return [S('ARM_KIND_GPR', position, 'ARM_A32_GPR_ANY', width)]
    if op.cls in ('GPR', 'GPRnopc') and layout == '0-2@0 3@7':
        return [S('ARM_KIND_T16_GPR_HIGH', 0, 'ARM_A32_GPR_ANY', 7)]
    return None


def plain_hex(position, width, scale=0):
    flags = 'ARM_SPEC_HEX' + (' | %d' % scale if scale else '')
    return S('ARM_KIND_IMMEDIATE', position, flags, width)


# Operands tied to another one (not in the encoding): printed again
TIED = {('tMUL', 'Rm'): 'Rd', ('tADDrSP', 'Rn'): 'Rdn'}


def handle(ctx, op, piece, records):
    cls = op.cls
    name = ctx.ins.name
    layout = op.layout()
    if (name, op.name) in TIED:
        return handle(ctx, ctx.ins.operand(TIED[(name, op.name)]), piece, records)
    if cls.startswith('(addrmode_tbb'):
        return [S('ARM_KIND_A32_MEMORY', 16, '0', 'ARM_A32_ADDR_T32_TBB')]
    if cls.startswith('(addrmode_tbh'):
        return [S('ARM_KIND_A32_MEMORY', 16, '0', 'ARM_A32_ADDR_T32_TBH')]
    # ---- T16 registers
    if ctx.ins.size == 2 or cls == 'tGPR':
        if cls == 'tGPR' and name == 'tLDMIA' and op.name == 'Rn':
            position, width = first_bit(op)
            return [S('ARM_KIND_GPR', position, 'ARM_A32_GPR_WRITEBACK_UNLESS_LISTED', width)]
        if cls == 'tGPR' and piece.writeback:
            position, width = first_bit(op)
            return [S('ARM_KIND_GPR', position, 'ARM_A32_GPR_ANY | ARM_A32_GPR_WRITEBACK', width)]
        result = t_gpr(ctx, op, piece)
        if result is not None:
            return result
        if cls in ('GPR', 'GPRnopc') and layout == '0-3@3':
            return [S('ARM_KIND_GPR', 3, 'ARM_A32_GPR_ANY', 4)]
    if cls == 'GPRsp' and not op.bits:
        return [S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_FIXED_SP')]
    if cls == 'GPRlr' and not op.bits:
        return [S('ARM_KIND_A32_FIXED', 0, '0', 'ARM_A32_FIXED_LR')]
    if cls in ('rGPR', 'GPR', 'GPRnopc') and any(len(v) > 1 for v in op.copies.values()):
        positions = sorted(p for v in op.copies.values() for p in v)
        if cls in ('rGPR', 'GPR', 'GPRnopc') and positions == [0, 1, 2, 3, 16, 17, 18, 19] and len(op.copies) == 4:
            return [S('ARM_KIND_GPR', 0, 'ARM_A32_GPR_ANY | ARM_A32_GPR_DUPLICATE', 4)]
        raise Unmapped('copies of %s' % op)
    if cls == 'rGPR' or (cls in ('GPR', 'GPRnopc', 'GPRnosp', 'GPRwithAPSR') and ctx.ins.size == 4):
        if not op.bits:
            raise Unmapped('register class %s without bits' % cls)
        position, width = first_bit(op)
        flag = 'ARM_A32_GPR_APSR' if cls == 'GPRwithAPSR' else 'ARM_A32_GPR_ANY'
        if piece.writeback:
            flag += ' | ARM_A32_GPR_WRITEBACK'
        return [S('ARM_KIND_GPR', position, flag, width)]

    # ---- immediates
    if cls == 't2_so_imm':
        need(op, '0-7@0 8-10@12 11@26')
        return [S('ARM_KIND_A32_MODIFIED_IMMEDIATE', 0, 'ARM_A32_MODIMM_T32', '0')]
    if cls == 'imm0_4095':
        need(op, '0-7@0 8-10@12 11@26')
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_T32_IMM12')]
    if cls == 'imm0_65535_expr' and ctx.ins.size == 4:
        need(op, '0-7@0 8-10@12 11@26 12-15@16')
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_T32_MOVW_IMM16')]
    if cls == 'imm0_65535' and ctx.ins.size == 4:
        need(op, '0-11@0 12-15@16')
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_T32_IMM16')]
    if cls in ('imm_sr', 'imm1_31') and layout == '0-1@6 2-4@12':
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_T32_SHIFT_AMOUNT')]
    if cls == 'imm_sr' and layout == '0-4@6':
        return [S('ARM_KIND_A32_FIELD', 6, '0', 'ARM_A32_FIELD_T16_SHIFT_AMOUNT')]
    if cls == 'imm0_31' and layout == '0-1@6 2-4@12':
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_T32_LSB')]
    if cls == 'bf_inv_mask_imm':
        need(op, '0-1@6 2-4@12 5-9@0')
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_T32_BF_LSB'),
                S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_T32_BF_WIDTH')]
    if cls == 'imm1_32' and ctx.ins.size == 4 and layout == '0-4@0':
        return [S('ARM_KIND_IMMEDIATE', 0, 'ARM_SPEC_HEX | ARM_SPEC_PLUS_ONE', 5)]
    if cls == 't_imm0_1020s4':
        need(op, '0-7@0')
        return [plain_hex(0, 8, 2)]
    if cls == 't_imm0_508s4':
        need(op, '0-6@0')
        return [plain_hex(0, 7, 2)]
    if cls == 't_adrlabel':
        need(op, '0-7@0')
        return [S('ARM_KIND_IMMEDIATE', 0, '2', 8)]
    if cls == 'imm0_255_expr':
        return [plain_hex(first_bit(op)[0], 8)]
    if cls == 'imm0_63':
        return [plain_hex(0, 6)]
    if cls == 'imm0_15' and ctx.ins.size == 2:
        return [plain_hex(first_bit(op)[0], 4)]
    if cls == 'i32imm':
        need(op, '0-4@0')
        return [S('ARM_KIND_IMMEDIATE', 0, 'ARM_SPEC_HEX', 5)]

    # ---- shifted registers and shift operands
    if cls == 't2_so_reg':
        need(op, '0-3@0 5-8@4 9-11@12')
        return [S('ARM_KIND_SHIFTED_REGISTER', 0, '0', 'ARM_A32_SHIFT_T2_IMM')]
    if cls == 'pkh_lsl_amt':
        return [('SHIFT', 'ARM_A32_SHIFT_T2_PKH_LSL')]
    if cls == 'pkh_asr_amt':
        return [('SHIFT', 'ARM_A32_SHIFT_T2_PKH_ASR')]
    if cls == 't2_shift_imm':
        return [('SHIFT', 'ARM_A32_SHIFT_T2_SAT')]
    if cls == 'rot_imm' and layout == '0-1@4':
        return [('SHIFT', 'ARM_A32_SHIFT_T2_ROTATE')]

    # ---- register lists
    if cls == 'reglist':
        if layout == '0-15@0':
            return [S('ARM_KIND_GPR_LIST', 0, 'ARM_A32_LIST_T32', 16)]
        if layout == '0-12@0 14@14':
            return [S('ARM_KIND_GPR_LIST', 0, 'ARM_A32_LIST_T32 | ARM_A32_LIST_T32_STORE', 16)]
        if layout == '0-7@0':
            return [S('ARM_KIND_GPR_LIST', 0, '0', 8)]
        if layout == '0-7@0 15@8':
            return [S('ARM_KIND_GPR_LIST', 0, 'ARM_A32_LIST_EXTRA | (15 << 4)', 8)]
        if layout == '0-7@0 14@8':
            return [S('ARM_KIND_GPR_LIST', 0, 'ARM_A32_LIST_EXTRA | (14 << 4)', 8)]

    # ---- options
    if cls == 'banked_reg':
        if layout == '0-3@16 4@4 5@20':
            return [S('ARM_KIND_A32_OPTION', 16, 'ARM_A32_BANKED_T32_MRS', 'ARM_A32_OPT_BANKED')]
        if layout == '0-3@8 4@4 5@20':
            return [S('ARM_KIND_A32_OPTION', 16, 'ARM_A32_BANKED_T32_MSR', 'ARM_A32_OPT_BANKED')]
    if cls == 'msr_mask' and layout == '0-3@8 4@20':
        return [S('ARM_KIND_A32_OPTION', 8, 'ARM_A32_MSR_T32', 'ARM_A32_OPT_MSR_MASK')]
    if cls == 'iflags_op':
        position, width = first_bit(op)
        return [S('ARM_KIND_A32_OPTION', position, '0', 'ARM_A32_OPT_IFLAGS')]
    if cls == 'imod_op' and layout == '0@4':
        return None
    if cls == 'setend_op' and layout == '0@3':
        return [S('ARM_KIND_A32_OPTION', 3, '0', 'ARM_A32_OPT_ENDIAN')]
    if cls == 'it_pred':
        need(op, '0-3@4')
        return [S('ARM_KIND_CONDITION', 4, 'ARM_A32_COND_FIRSTCOND', 0)]

    # ---- branch targets
    branch = {
        't_brtarget': ('ARM_A32_BRANCH_T16_B', '0-10@0'),
        'thumb_bcc_target': ('ARM_A32_BRANCH_T16_BCC', '0-7@0'),
        'thumb_cb_target': ('ARM_A32_BRANCH_T16_CB', '0-4@3 5@9'),
        'thumb_bl_target': ('ARM_A32_BRANCH_T32_BL', '0-10@0 11-20@16 21@11 22@13 23@26'),
        'thumb_blx_target': ('ARM_A32_BRANCH_T32_BLX', '1-10@1 11-20@16 21@11 22@13 23@26'),
        'thumb_br_target': ('ARM_A32_BRANCH_T32_BL', '0-10@0 11-20@16 21@11 22@13 23@26'),
        'brtarget': ('ARM_A32_BRANCH_T32_BCC', '1-11@0 12-17@16 18@13 19@11 20@26'),
    }
    if cls in branch:
        scheme, expected = branch[cls]
        need(op, expected)
        return [S('ARM_KIND_BRANCH_TARGET', 0, scheme, 0)]

    # ---- T16 addressing
    t16 = {
        't_addrmode_is1': (0, '0-7@3'), 't_addrmode_is2': (1, '0-7@3'), 't_addrmode_is4': (2, '0-7@3'),
    }
    if cls in t16:
        scale, expected = t16[cls]
        need(op, expected)
        return [S('ARM_KIND_A32_MEMORY', 3, str(scale), 'ARM_A32_ADDR_T16_IMM5')]
    if cls in ('t_addrmode_rr', 't_addrmode_rr_sext'):
        need(op, '0-5@3')
        return [S('ARM_KIND_A32_MEMORY', 3, '0', 'ARM_A32_ADDR_T16_REG')]
    if cls == 't_addrmode_sp':
        need(op, '0-7@0')
        return [S('ARM_KIND_A32_MEMORY', 0, '0', 'ARM_A32_ADDR_T16_SP')]
    if cls == 't_addrmode_pc':
        need(op, '0-7@0')
        return [S('ARM_KIND_A32_MEMORY', 0, '0', 'ARM_A32_ADDR_T16_PC')]

    # ---- T32 addressing
    if cls == 't2addrmode_imm12':
        need(op, '0-11@0 13-16@16')
        return [S('ARM_KIND_A32_MEMORY', 16, '0', 'ARM_A32_ADDR_T32_IMM12')]
    if cls in ('t2addrmode_negimm8', 't2addrmode_posimm8', 't2addrmode_imm8_pre'):
        flags = {'t2addrmode_negimm8': 'ARM_A32_MEM_NEG', 't2addrmode_posimm8': 'ARM_A32_MEM_POS',
                 't2addrmode_imm8_pre': 'ARM_A32_MEM_PRE'}[cls]
        return [S('ARM_KIND_A32_MEMORY', 16, flags, 'ARM_A32_ADDR_T32_IMM8')]
    if cls == 't2addrmode_so_reg':
        need(op, '0-1@4 2-5@0 6-9@16')
        return [S('ARM_KIND_A32_MEMORY', 16, '0', 'ARM_A32_ADDR_T32_REG')]
    if cls in ('t2addrmode_imm8s4', 't2addrmode_imm8s4_pre'):
        pre = 'ARM_A32_MEM_PRE' if cls.endswith('_pre') else '0'
        return [S('ARM_KIND_A32_MEMORY', 16, pre, 'ARM_A32_ADDR_T32_IMM8S4')]
    if cls == 't2addrmode_imm0_1020s4':
        need(op, '0-7@0 8-11@16')
        return [S('ARM_KIND_A32_MEMORY', 16, '2', 'ARM_A32_ADDR_T32_IMM0_1020S4')]
    if cls == 't2ldrlabel':
        need(op, '0-11@0 12@23')
        return [S('ARM_KIND_A32_MEMORY', 0, '0', 'ARM_A32_ADDR_T32_LITERAL')]
    if cls == 't2adrlabel':
        return [S('ARM_KIND_A32_FIELD', 0, '0', 'ARM_A32_FIELD_T32_ADR')]
    return None
