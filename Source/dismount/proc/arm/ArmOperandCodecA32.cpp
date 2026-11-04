#include "dismount/dismount.h"
/*
 * ArmOperandCodecA32.cpp
 *
 * Implementation file. The AArch32 (A32 and T32) kinds of ArmOperandCodec:
 * the registers, the shifted registers, the immediates, the addressing
 * modes, the named options and the mnemonic parts. The Advanced SIMD and
 * floating point kinds are in ArmOperandCodecA32Simd.cpp.
 *
 * The decoder follows the decoder of LLVM (ARMDisassembler.cpp): a value
 * which LLVM rejects (Fail) makes the decode return false, a value which LLVM
 * only soft fails is decoded.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"
#include "dismount/proc/arm/ArmOperandCodecA32.h"

namespace armdis {

namespace {

typedef ArmDecodedOperand Operand;

//////////////////////////////////////////////////////////////////////////
// Helpers

uint field(uint32 encoding, uint position, uint width)
{
    return ArmOperandCodec::getField(encoding, position, width);
}

void setBits(uint32& encoding, uint position, uint width, uint value)
{
    ArmOperandCodec::setField(encoding, position, width, value);
}

// True if the feature set is an ARMv8 one: the coprocessor instructions of
// ARMv8 are limited to cp14 and cp15
bool isV8(const ArmCodecContext& context)
{
    return context.m_features.has(ArmFeatures::FEAT_V8_AA32);
}

// Return a register operand
void makeRegister(Operand& operand, uint number)
{
    operand.m_kind = Operand::KIND_REGISTER;
    operand.m_register = ARM_REG_R0 + number;
}

// Return the register number of a register operand, false if it isn't one
bool getRegisterNumber(const Operand& operand, uint& number)
{
    if ((operand.m_kind != Operand::KIND_REGISTER) || (operand.m_register < ARM_REG_R0) ||
        (operand.m_register > ARM_REG_PC) || (operand.m_arrangement != Operand::ARRANGEMENT_NONE) ||
        (operand.m_elementIndex >= 0))
        return false;
    number = operand.m_register - ARM_REG_R0;
    return true;
}

// The shift of a register by an immediate: type 0-3 (lsl, lsr, asr, ror) and
// the amount field (0 means 32 for lsr and asr, and ror #0 is rrx)
void makeImmediateShift(Operand& operand, uint type, uint amount)
{
    switch (type)
    {
    case 0:
        operand.m_shift = (amount == 0) ? Operand::SHIFT_NONE : Operand::SHIFT_LSL;
        operand.m_shiftAmount = amount;
        break;
    case 1:
        operand.m_shift = Operand::SHIFT_LSR;
        operand.m_shiftAmount = (amount == 0) ? 32 : amount;
        break;
    case 2:
        operand.m_shift = Operand::SHIFT_ASR;
        operand.m_shiftAmount = (amount == 0) ? 32 : amount;
        break;
    default:
        if (amount == 0)
        {
            operand.m_shift = Operand::SHIFT_RRX;
            operand.m_shiftAmount = 0;
        } else
        {
            operand.m_shift = Operand::SHIFT_ROR;
            operand.m_shiftAmount = amount;
        }
        break;
    }
}

// The inverse of makeImmediateShift. 'maximum' is the largest amount of the
// field (31 for the A32 5 bit one). Return false if the shift can't be encoded
bool encodeImmediateShift(const Operand& operand, uint& type, uint& amount)
{
    uint count = operand.m_shiftAmount;
    switch (operand.m_shift)
    {
    case Operand::SHIFT_NONE:
        type = 0;
        amount = 0;
        return true;
    case Operand::SHIFT_LSL:
        if ((count < 1) || (count > 31))
            return false;
        type = 0;
        amount = count;
        return true;
    case Operand::SHIFT_LSR:
    case Operand::SHIFT_ASR:
        if ((count < 1) || (count > 32))
            return false;
        type = (operand.m_shift == Operand::SHIFT_LSR) ? 1 : 2;
        amount = count & 31;
        return true;
    case Operand::SHIFT_ROR:
        if ((count < 1) || (count > 31))
            return false;
        type = 3;
        amount = count;
        return true;
    case Operand::SHIFT_RRX:
        type = 3;
        amount = 0;
        return true;
    default:
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// General purpose registers

bool decodeGpr(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    uint number = field(encoding, spec.m_position, spec.m_extra);
    if ((spec.m_flags & ARM_A32_GPR_DUPLICATE) != 0)
        number |= field(encoding, 16, 4);
    uint registerClass = spec.m_flags & ARM_A32_GPR_CLASS_MASK;
    switch (registerClass)
    {
    case ARM_A32_GPR_SP_ONLY:
        if (number != 13)
            return false;
        break;
    case ARM_A32_GPR_TAIL_CALL:
        if (!((number <= 3) || (number == 9) || (number == 12)))
            return false;
        break;
    case ARM_A32_GPR_NO_SP:
        if (number == 13)
            return false;
        if (number == 15)
        {
            operand.m_kind = Operand::KIND_NAME;
            operand.m_name = cString("zr");
            return true;
        }
        break;
    case ARM_A32_GPR_ZR:
        if (number == 15)
        {
            operand.m_kind = Operand::KIND_NAME;
            operand.m_name = cString("zr");
            return true;
        }
        break;
    case ARM_A32_GPR_APSR:
        if (number == 15)
        {
            operand.m_kind = Operand::KIND_NAME;
            operand.m_name = cString("apsr_nzcv");
            return true;
        }
        break;
    case ARM_A32_GPR_PAIR_NEXT:
        if (number == 15)
            return false;
        operand.m_kind = Operand::KIND_REGISTER_LIST;
        operand.m_register = ARM_REG_R0 + number;
        operand.m_count = 2;
        operand.m_stride = 1;
        return true;
    case ARM_A32_GPR_PAIR_CLASS:
        if (number > 13)
            return false;
        operand.m_kind = Operand::KIND_REGISTER_LIST;
        operand.m_register = ARM_REG_R0 + (number & ~1u);
        operand.m_count = 2;
        operand.m_stride = 1;
        return true;
    default:
        break;
    }
    makeRegister(operand, number);
    if ((spec.m_flags & ARM_A32_GPR_WRITEBACK) != 0)
        operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
    // The T16 ldm: written back unless the base register is in the list
    if (((spec.m_flags & ARM_A32_GPR_WRITEBACK_UNLESS_LISTED) != 0) &&
        (field(encoding, number, 1) == 0 || number >= 8))
        operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
    return true;
}

bool encodeGpr(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    uint registerClass = spec.m_flags & ARM_A32_GPR_CLASS_MASK;
    uint number = 0;
    if (registerClass == ARM_A32_GPR_PAIR_NEXT || registerClass == ARM_A32_GPR_PAIR_CLASS)
    {
        if ((operand.m_kind != Operand::KIND_REGISTER_LIST) || (operand.m_count != 2) ||
            (operand.m_registerMask != 0) || (operand.m_register < ARM_REG_R0) ||
            (operand.m_register >= ARM_REG_PC))
            return false;
        number = operand.m_register - ARM_REG_R0;
        if ((registerClass == ARM_A32_GPR_PAIR_CLASS) && (((number & 1) != 0) || (number > 12)))
            return false;
    } else if (!getRegisterNumber(operand, number))
    {
        // apsr_nzcv and zr are the register 15
        if ((operand.m_kind != Operand::KIND_NAME) ||
            !(((registerClass == ARM_A32_GPR_APSR) && (operand.m_name == "apsr_nzcv")) ||
              (((registerClass == ARM_A32_GPR_ZR) || (registerClass == ARM_A32_GPR_NO_SP)) &&
               (operand.m_name == "zr"))))
            return false;
        number = 15;
    } else
    {
        if ((spec.m_flags & ARM_A32_GPR_WRITEBACK_UNLESS_LISTED) == 0 &&
            (operand.m_addressing == Operand::ADDRESSING_PRE_INDEX) !=
            ((spec.m_flags & ARM_A32_GPR_WRITEBACK) != 0))
            return false;
    }
    switch (registerClass)
    {
    case ARM_A32_GPR_SP_ONLY:
        if (number != 13)
            return false;
        break;
    case ARM_A32_GPR_TAIL_CALL:
        if (!((number <= 3) || (number == 9) || (number == 12)))
            return false;
        break;
    case ARM_A32_GPR_NO_SP:
        if (number == 13)
            return false;
        break;
    case ARM_A32_GPR_PAIR_NEXT:
        if (number == 15)
            return false;
        break;
    default:
        break;
    }
    if (spec.m_extra < 4 && number >= (1u << spec.m_extra))
        return false;
    setBits(encoding, spec.m_position, spec.m_extra, number);
    if ((spec.m_flags & ARM_A32_GPR_DUPLICATE) != 0)
        setBits(encoding, 16, 4, number);
    return true;
}

//////////////////////////////////////////////////////////////////////////
// Register lists

bool decodeList(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    uint mask = field(encoding, spec.m_position, spec.m_extra);
    // The T32 stores have no sp and pc in the list (bits 13 and 15)
    if ((spec.m_flags & ARM_A32_LIST_T32_STORE) != 0)
        mask = field(encoding, 0, 13) | (field(encoding, 14, 1) << 14);
    if ((spec.m_flags & ARM_A32_LIST_EXTRA) != 0)
    {
        // The T16 push / pop: bit 8 is lr or pc
        if (field(encoding, 8, 1) != 0)
            mask |= 1u << (spec.m_flags >> 4);
    }
    // Empty lists are invalid
    if (mask == 0)
        return false;
    operand.m_kind = Operand::KIND_REGISTER_LIST;
    operand.m_registerMask = mask;
    operand.m_isExplicit = (spec.m_flags & ARM_A32_LIST_CARET) != 0;
    return true;
}

bool encodeList(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_REGISTER_LIST) || (operand.m_registerMask == 0))
        return false;
    if (operand.m_isExplicit != ((spec.m_flags & ARM_A32_LIST_CARET) != 0))
        return false;
    uint32 mask = operand.m_registerMask;
    if ((spec.m_flags & ARM_A32_LIST_EXTRA) != 0)
    {
        uint extra = spec.m_flags >> 4;
        if ((mask & (1u << extra)) != 0)
        {
            setBits(encoding, 8, 1, 1);
            mask &= ~(1u << extra);
        } else
        {
            setBits(encoding, 8, 1, 0);
        }
    }
    if ((mask >> spec.m_extra) != 0)
        return false;
    if ((spec.m_flags & ARM_A32_LIST_T32_STORE) != 0)
    {
        if ((mask & 0xA000) != 0)
            return false;
        setBits(encoding, 0, 13, mask & 0x1FFF);
        setBits(encoding, 14, 1, (mask >> 14) & 1);
        return true;
    }
    setBits(encoding, spec.m_position, spec.m_extra, mask);
    return true;
}

//////////////////////////////////////////////////////////////////////////
// Shifted registers

bool decodeShifted(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    operand.m_kind = Operand::KIND_SHIFTED_REGISTER;
    operand.m_register = ARM_REG_R0 + field(encoding, spec.m_position, 4);
    switch (spec.m_extra)
    {
    case ARM_A32_SHIFT_SO_IMM:
        makeImmediateShift(operand, field(encoding, 5, 2), field(encoding, 7, 5));
        return true;
    case ARM_A32_SHIFT_SO_REG:
    {
        // The shift by register: LLVM's rrx can't be (type 3 is ror)
        static const uint types[] = { Operand::SHIFT_LSL, Operand::SHIFT_LSR,
                                      Operand::SHIFT_ASR, Operand::SHIFT_ROR };
        operand.m_shift = types[field(encoding, 5, 2)];
        operand.m_shiftRegister = ARM_REG_R0 + field(encoding, 8, 4);
        return true;
    }
    case ARM_A32_SHIFT_PKH_LSL:
    {
        uint amount = field(encoding, 7, 5);
        if (amount != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = amount;
        }
        return true;
    }
    case ARM_A32_SHIFT_PKH_ASR:
    {
        uint amount = field(encoding, 7, 5);
        operand.m_shift = Operand::SHIFT_ASR;
        operand.m_shiftAmount = (amount == 0) ? 32 : amount;
        return true;
    }
    case ARM_A32_SHIFT_SAT:
    {
        uint amount = field(encoding, 7, 5);
        if (field(encoding, 6, 1) != 0)
        {
            operand.m_shift = Operand::SHIFT_ASR;
            operand.m_shiftAmount = (amount == 0) ? 32 : amount;
        } else if (amount != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = amount;
        }
        return true;
    }
    case ARM_A32_SHIFT_ROTATE:
    {
        uint rotation = field(encoding, 10, 2);
        if (rotation != 0)
        {
            operand.m_shift = Operand::SHIFT_ROR;
            operand.m_shiftAmount = 8 * rotation;
        }
        return true;
    }
    case ARM_A32_SHIFT_T2_IMM:
        makeImmediateShift(operand, field(encoding, 4, 2),
                           (field(encoding, 12, 3) << 2) | field(encoding, 6, 2));
        return true;
    case ARM_A32_SHIFT_T2_PKH_LSL:
    {
        uint amount = (field(encoding, 12, 3) << 2) | field(encoding, 6, 2);
        if (amount != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = amount;
        }
        return true;
    }
    case ARM_A32_SHIFT_T2_PKH_ASR:
    {
        uint amount = (field(encoding, 12, 3) << 2) | field(encoding, 6, 2);
        operand.m_shift = Operand::SHIFT_ASR;
        operand.m_shiftAmount = (amount == 0) ? 32 : amount;
        return true;
    }
    case ARM_A32_SHIFT_T2_SAT:
    {
        // asr #32 (sh = 1, amount = 0) is not allowed in Thumb-2
        uint amount = (field(encoding, 12, 3) << 2) | field(encoding, 6, 2);
        if (field(encoding, 21, 1) != 0)
        {
            if (amount == 0)
                return false;
            operand.m_shift = Operand::SHIFT_ASR;
            operand.m_shiftAmount = amount;
        } else if (amount != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = amount;
        }
        return true;
    }
    case ARM_A32_SHIFT_T2_ROTATE:
    {
        uint rotation = field(encoding, 4, 2);
        if (rotation != 0)
        {
            operand.m_shift = Operand::SHIFT_ROR;
            operand.m_shiftAmount = 8 * rotation;
        }
        return true;
    }
    default:
        return false;
    }
}

bool encodeShifted(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    uint number;
    if (operand.m_kind == Operand::KIND_REGISTER)
    {
        if (!getRegisterNumber(operand, number))
            return false;
    } else if (operand.m_kind == Operand::KIND_SHIFTED_REGISTER)
    {
        if ((operand.m_register < ARM_REG_R0) || (operand.m_register > ARM_REG_PC))
            return false;
        number = operand.m_register - ARM_REG_R0;
    } else
    {
        return false;
    }
    bool isShifted = (operand.m_kind == Operand::KIND_SHIFTED_REGISTER);
    uint shift = isShifted ? operand.m_shift : (uint)Operand::SHIFT_NONE;
    uint amount = isShifted ? operand.m_shiftAmount : 0;
    bool hasShiftRegister = isShifted && (operand.m_shiftRegister != ARM_NO_REGISTER);

    switch (spec.m_extra)
    {
    case ARM_A32_SHIFT_SO_IMM:
    {
        if (hasShiftRegister)
            return false;
        Operand copy(operand);
        uint type, count;
        if (!encodeImmediateShift(copy, type, count))
            return false;
        setBits(encoding, 5, 2, type);
        setBits(encoding, 7, 5, count);
        break;
    }
    case ARM_A32_SHIFT_SO_REG:
    {
        if (!hasShiftRegister || (operand.m_shiftRegister < ARM_REG_R0) ||
            (operand.m_shiftRegister > ARM_REG_PC))
            return false;
        uint type;
        switch (shift)
        {
        case Operand::SHIFT_LSL: type = 0; break;
        case Operand::SHIFT_LSR: type = 1; break;
        case Operand::SHIFT_ASR: type = 2; break;
        case Operand::SHIFT_ROR: type = 3; break;
        default: return false;
        }
        setBits(encoding, 5, 2, type);
        setBits(encoding, 8, 4, operand.m_shiftRegister - ARM_REG_R0);
        break;
    }
    case ARM_A32_SHIFT_PKH_LSL:
        if (hasShiftRegister)
            return false;
        if (shift == Operand::SHIFT_NONE)
            amount = 0;
        else if ((shift != Operand::SHIFT_LSL) || (amount < 1) || (amount > 31))
            return false;
        setBits(encoding, 7, 5, amount);
        break;
    case ARM_A32_SHIFT_PKH_ASR:
        if (hasShiftRegister || (shift != Operand::SHIFT_ASR) || (amount < 1) || (amount > 32))
            return false;
        setBits(encoding, 7, 5, amount & 31);
        break;
    case ARM_A32_SHIFT_SAT:
        if (hasShiftRegister)
            return false;
        if (shift == Operand::SHIFT_NONE)
        {
            setBits(encoding, 6, 1, 0);
            setBits(encoding, 7, 5, 0);
        } else if ((shift == Operand::SHIFT_LSL) && (amount >= 1) && (amount <= 31))
        {
            setBits(encoding, 6, 1, 0);
            setBits(encoding, 7, 5, amount);
        } else if ((shift == Operand::SHIFT_ASR) && (amount >= 1) && (amount <= 32))
        {
            setBits(encoding, 6, 1, 1);
            setBits(encoding, 7, 5, amount & 31);
        } else
        {
            return false;
        }
        break;
    case ARM_A32_SHIFT_ROTATE:
    case ARM_A32_SHIFT_T2_ROTATE:
        if (hasShiftRegister)
            return false;
        if (shift == Operand::SHIFT_NONE)
            amount = 0;
        else if ((shift != Operand::SHIFT_ROR) || ((amount != 8) && (amount != 16) && (amount != 24)))
            return false;
        setBits(encoding, (spec.m_extra == ARM_A32_SHIFT_ROTATE) ? 10 : 4, 2, amount / 8);
        break;
    case ARM_A32_SHIFT_T2_IMM:
    {
        if (hasShiftRegister)
            return false;
        Operand copy(operand);
        uint type, count;
        if (!encodeImmediateShift(copy, type, count))
            return false;
        setBits(encoding, 4, 2, type);
        setBits(encoding, 12, 3, count >> 2);
        setBits(encoding, 6, 2, count & 3);
        break;
    }
    case ARM_A32_SHIFT_T2_PKH_LSL:
        if (hasShiftRegister)
            return false;
        if (shift == Operand::SHIFT_NONE)
            amount = 0;
        else if ((shift != Operand::SHIFT_LSL) || (amount < 1) || (amount > 31))
            return false;
        setBits(encoding, 12, 3, amount >> 2);
        setBits(encoding, 6, 2, amount & 3);
        break;
    case ARM_A32_SHIFT_T2_PKH_ASR:
        if (hasShiftRegister || (shift != Operand::SHIFT_ASR) || (amount < 1) || (amount > 32))
            return false;
        setBits(encoding, 12, 3, (amount & 31) >> 2);
        setBits(encoding, 6, 2, amount & 3);
        break;
    case ARM_A32_SHIFT_T2_SAT:
        if (hasShiftRegister)
            return false;
        if (shift == Operand::SHIFT_NONE)
        {
            setBits(encoding, 21, 1, 0);
            amount = 0;
        } else if ((shift == Operand::SHIFT_LSL) && (amount >= 1) && (amount <= 31))
        {
            setBits(encoding, 21, 1, 0);
        } else if ((shift == Operand::SHIFT_ASR) && (amount >= 1) && (amount <= 31))
        {
            setBits(encoding, 21, 1, 1);
        } else
        {
            return false;
        }
        setBits(encoding, 12, 3, amount >> 2);
        setBits(encoding, 6, 2, amount & 3);
        break;
    default:
        return false;
    }
    setBits(encoding, spec.m_position, 4, number);
    return true;
}

//////////////////////////////////////////////////////////////////////////
// The modified immediate

uint32 rotateRight(uint32 value, uint count)
{
    count &= 31;
    return count == 0 ? value : ((value >> count) | (value << (32 - count)));
}

// Return the encoding of a value as a modified immediate (the smallest
// rotation, as LLVM chooses), false if it has none
bool findModifiedImmediate(uint32 value, uint& immediate)
{
    for (uint rotation = 0; rotation < 32; rotation += 2)
    {
        uint32 bits = rotateRight(value, 32 - rotation);
        if (bits < 256)
        {
            immediate = ((rotation >> 1) << 8) | bits;
            return true;
        }
    }
    return false;
}

// The ThumbExpandImm of the T32 modified immediate: i at 26, imm3 at 12, imm8
// at 0
uint32 expandThumbImmediate(uint value)
{
    if ((value >> 10) == 0)
    {
        uint32 imm = value & 0xFF;
        switch ((value >> 8) & 3)
        {
        case 0: return imm;
        case 1: return (imm << 16) | imm;
        case 2: return (imm << 24) | (imm << 8);
        default: return (imm << 24) | (imm << 16) | (imm << 8) | imm;
        }
    }
    return rotateRight((value & 0x7F) | 0x80, (value >> 7) & 31);
}

// Return the 12 bit encoding of a value, false if it has none
bool findThumbImmediate(uint32 value, uint& immediate)
{
    uint32 imm = value & 0xFF;
    uint32 imm2 = (value >> 8) & 0xFF;
    if (value < 256)
        immediate = (uint)value;
    else if (value == ((imm << 16) | imm))
        immediate = 0x100 | imm;
    else if (value == ((imm2 << 24) | (imm2 << 8)))
        immediate = 0x200 | imm2;
    else if (value == ((imm << 24) | (imm << 16) | (imm << 8) | imm))
        immediate = 0x300 | imm;
    else
    {
        for (uint rotation = 8; rotation < 32; rotation++)
        {
            uint32 unrotated = rotateRight(value, 32 - rotation);
            if ((unrotated >= 0x80) && (unrotated <= 0xFF))
            {
                immediate = (rotation << 7) | (unrotated & 0x7F);
                return true;
            }
        }
        return false;
    }
    return true;
}

bool decodeModifiedImmediate(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    if ((spec.m_flags & ARM_A32_MODIMM_T32) != 0)
    {
        uint value = (field(encoding, 26, 1) << 11) | (field(encoding, 12, 3) << 8) |
                     field(encoding, 0, 8);
        operand.m_kind = Operand::KIND_IMMEDIATE;
        operand.m_immediate = (int64)expandThumbImmediate(value);
        operand.m_isHex = true;
        return true;
    }
    uint immediate = field(encoding, 0, 12);
    uint bits = immediate & 0xFF;
    uint rotation = (immediate & 0xF00) >> 7;
    int32 rotated = (int32)rotateRight(bits, rotation);
    uint canonical;
    operand.m_kind = Operand::KIND_IMMEDIATE;
    if (findModifiedImmediate((uint32)rotated, canonical) && (canonical == immediate))
    {
        // (the moves to pc and to the status registers print the unsigned value)
        if ((spec.m_flags & ARM_A32_MODIMM_UNSIGNED) != 0)
            operand.m_immediate = (int64)(uint32)rotated;
        else
            operand.m_immediate = rotated;
    } else
    {
        // "#bits, #rot": the rotation is not the smallest one
        operand.m_immediate = bits;
        operand.m_shiftAmount = rotation;
        operand.m_isExplicit = true;
    }
    return true;
}

bool encodeModifiedImmediate(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if (operand.m_kind != Operand::KIND_IMMEDIATE)
        return false;
    uint immediate;
    if ((spec.m_flags & ARM_A32_MODIMM_T32) != 0)
    {
        if (operand.m_isExplicit || (operand.m_immediate < -(int64)0x80000000LL) ||
            (operand.m_immediate > (int64)0xFFFFFFFFLL) ||
            !findThumbImmediate((uint32)operand.m_immediate, immediate))
            return false;
        setBits(encoding, 26, 1, immediate >> 11);
        setBits(encoding, 12, 3, (immediate >> 8) & 7);
        setBits(encoding, 0, 8, immediate & 0xFF);
        return true;
    }
    if (operand.m_isExplicit)
    {
        // "#bits, #rot"
        if ((operand.m_immediate < 0) || (operand.m_immediate > 255) ||
            (operand.m_shiftAmount > 30) || ((operand.m_shiftAmount & 1) != 0))
            return false;
        immediate = ((operand.m_shiftAmount >> 1) << 8) | (uint)operand.m_immediate;
    } else
    {
        int64 value = operand.m_immediate;
        if ((value < -(int64)0x80000000LL) || (value > (int64)0xFFFFFFFFLL))
            return false;
        if (!findModifiedImmediate((uint32)value, immediate))
            return false;
    }
    setBits(encoding, 0, 12, immediate);
    return true;
}


//////////////////////////////////////////////////////////////////////////
// The named options

// The names of the barrier options. The ones that have no name before ARMv8
// are printed as numbers
const char* getBarrierName(uint value, bool isV8)
{
    switch (value)
    {
    case 15: return "sy";
    case 14: return "st";
    case 13: return isV8 ? "ld" : NULL;
    case 11: return "ish";
    case 10: return "ishst";
    case 9:  return isV8 ? "ishld" : NULL;
    case 7:  return "nsh";
    case 6:  return "nshst";
    case 5:  return isV8 ? "nshld" : NULL;
    case 3:  return "osh";
    case 2:  return "oshst";
    case 1:  return isV8 ? "oshld" : NULL;
    default: return NULL;
    }
}

// The text of an unnamed option: "#0xd"
cString numberText(uint value)
{
    return cString("#0x") + hexText(value);
}

// Return the text of the mask of msr: apsr_nzcvq, cpsr_fc, spsr_x...
cString msrMaskText(uint value)
{
    uint spsr = value >> 4;
    uint mask = value & 15;
    if (!spsr && ((mask == 8) || (mask == 4) || (mask == 12)))
    {
        if (mask == 4)
            return cString("APSR_g");
        return (mask == 8) ? cString("APSR_nzcvq") : cString("APSR_nzcvqg");
    }
    cString text(spsr ? "SPSR" : "CPSR");
    if (mask != 0)
    {
        text += "_";
        if (mask & 8) text += "f";
        if (mask & 4) text += "s";
        if (mask & 2) text += "x";
        if (mask & 1) text += "c";
    }
    return text;
}

bool decodeOption(const ArmOperandSpec& spec, uint32 encoding, const ArmCodecContext& context,
                  Operand& operand)
{
    operand.m_kind = Operand::KIND_OPTION;
    operand.m_optionScheme = spec.m_extra;
    switch (spec.m_extra)
    {
    case ARM_A32_OPT_BARRIER:
    {
        uint value = field(encoding, spec.m_position, 4);
        operand.m_option = value;
        const char* name = getBarrierName(value, isV8(context));
        operand.m_name = (name != NULL) ? cString(name) : numberText(value);
        return true;
    }
    case ARM_A32_OPT_ISB:
    {
        uint value = field(encoding, spec.m_position, 4);
        operand.m_option = value;
        operand.m_name = (value == 15) ? cString("sy") : numberText(value);
        return true;
    }
    case ARM_A32_OPT_COPROC:
    {
        uint value = field(encoding, spec.m_position, 4);
        // cp10 and cp11 are the floating point, ARMv8 has only cp14 and cp15
        if ((value == 10) || (value == 11) || (isV8(context) && ((value & 14) != 14)))
            return false;
        if (((spec.m_flags & ARM_A32_COPROC_CP14_V8) != 0) && isV8(context) && (value != 14))
            return false;
        operand.m_option = value;
        operand.m_name = cString("p") + cString((uint32)value);
        return true;
    }
    case ARM_A32_OPT_CREG:
    {
        uint value = field(encoding, spec.m_position, 4);
        operand.m_option = value;
        operand.m_name = cString("c") + cString((uint32)value);
        return true;
    }
    case ARM_A32_OPT_IFLAGS:
    {
        uint value = field(encoding, spec.m_position, 3);
        operand.m_option = value;
        cString text;
        if (value & 4) text += "a";
        if (value & 2) text += "i";
        if (value & 1) text += "f";
        operand.m_name = (value == 0) ? cString("none") : text;
        return true;
    }
    case ARM_A32_OPT_ENDIAN:
    {
        uint value = field(encoding, spec.m_position, 1);
        operand.m_option = value;
        operand.m_name = cString(value ? "be" : "le");
        return true;
    }
    case ARM_A32_OPT_MSR_MASK:
    {
        uint value = ((spec.m_flags & ARM_A32_MSR_T32) != 0) ?
                        ((field(encoding, 20, 1) << 4) | field(encoding, 8, 4)) :
                        ((field(encoding, 22, 1) << 4) | field(encoding, 16, 4));
        // The A / R class: a zero mask is invalid
        if (value == 0)
            return false;
        operand.m_option = value;
        operand.m_name = msrMaskText(value);
        return true;
    }
    case ARM_A32_OPT_BANKED:
    {
        uint value;
        if ((spec.m_flags & ARM_A32_BANKED_T32_MRS) != 0)
            value = (field(encoding, 20, 1) << 5) | (field(encoding, 4, 1) << 4) | field(encoding, 16, 4);
        else if ((spec.m_flags & ARM_A32_BANKED_T32_MSR) != 0)
            value = (field(encoding, 20, 1) << 5) | (field(encoding, 4, 1) << 4) | field(encoding, 8, 4);
        else
            value = (field(encoding, 22, 1) << 5) | (field(encoding, 8, 1) << 4) |
                    field(encoding, 16, 4);
        const char* name = getA32BankedName(value);
        if (name == NULL)
            return false;
        operand.m_option = value;
        operand.m_name = cString(name);
        // The SPSR registers: spsr_fiq is printed SPSR_fiq
        if ((value & 0x20) != 0)
            operand.m_name = cString("SPSR") + operand.m_name.part(4, operand.m_name.length());
        return true;
    }
    default:
        return false;
    }
}

// Set the fields of the msr mask
void putMsrMask(const ArmOperandSpec& spec, uint32& encoding, uint value)
{
    if ((spec.m_flags & ARM_A32_MSR_T32) != 0)
    {
        setBits(encoding, 8, 4, value & 15);
        setBits(encoding, 20, 1, value >> 4);
        return;
    }
    setBits(encoding, 16, 4, value & 15);
    setBits(encoding, 22, 1, value >> 4);
}

bool encodeOption(const ArmOperandSpec& spec, const Operand& operand, const ArmCodecContext& context,
                  uint32& encoding)
{
    // The assembler reads the options as names (KIND_NAME) or numbers
    cString name;
    int64 number = -1;
    if (operand.m_kind == Operand::KIND_NAME)
        name = operand.m_name;
    else if (operand.m_kind == Operand::KIND_OPTION)
        name = operand.m_name;
    else if (operand.m_kind == Operand::KIND_IMMEDIATE)
        number = operand.m_immediate;
    else if (operand.m_kind == Operand::KIND_REGISTER)
    {
        const char* registerName = getRegisterName(ArmInstructionSet::ARM_A32, operand.m_register);
        if (registerName == NULL)
            return false;
        name = cString(registerName);
    } else
        return false;
    name.makeLower();

    switch (spec.m_extra)
    {
    case ARM_A32_OPT_BARRIER:
    {
        uint value = 0;
        bool isFound = false;
        for (uint i = 0; i < 16; i++)
        {
            const char* text = getBarrierName(i, isV8(context));
            if ((text != NULL) && (name == text))
            {
                value = i;
                isFound = true;
                break;
            }
        }
        if (!isFound)
        {
            if (number >= 0 && number <= 15)
                value = (uint)number;
            else if (name.length() > 2 && name.left(2) == "#0")
                value = 0xFFFF;
            else
                return false;
            if (value == 0xFFFF)
                return false;
        }
        setBits(encoding, spec.m_position, 4, value);
        return true;
    }
    case ARM_A32_OPT_ISB:
    {
        uint value;
        if (name == "sy")
            value = 15;
        else if (number >= 0 && number <= 15)
            value = (uint)number;
        else
            return false;
        setBits(encoding, spec.m_position, 4, value);
        return true;
    }
    case ARM_A32_OPT_COPROC:
    case ARM_A32_OPT_CREG:
    {
        uint value;
        const char* prefix = (spec.m_extra == ARM_A32_OPT_COPROC) ? "p" : "c";
        if (!parseNumberedName(name, prefix, 15, value))
            return false;
        if (spec.m_extra == ARM_A32_OPT_COPROC)
        {
            if ((value == 10) || (value == 11) || (isV8(context) && ((value & 14) != 14)))
                return false;
            if (((spec.m_flags & ARM_A32_COPROC_CP14_V8) != 0) && isV8(context) && (value != 14))
                return false;
        }
        setBits(encoding, spec.m_position, 4, value);
        return true;
    }
    case ARM_A32_OPT_IFLAGS:
    {
        uint value = 0;
        if (name == "none")
        {
            value = 0;
        } else
        {
            // a, i and f in this order
            uint last = 3;
            for (uint i = 0; i < name.length(); i++)
            {
                uint bit;
                if (name[i] == XSTL_CHAR('a')) bit = 2;
                else if (name[i] == XSTL_CHAR('i')) bit = 1;
                else if (name[i] == XSTL_CHAR('f')) bit = 0;
                else return false;
                if (bit >= last)
                    return false;
                last = bit;
                value |= 1u << bit;
            }
            if (value == 0)
                return false;
        }
        setBits(encoding, spec.m_position, 3, value);
        return true;
    }
    case ARM_A32_OPT_ENDIAN:
        if (name == "be")
            setBits(encoding, spec.m_position, 1, 1);
        else if (name == "le")
            setBits(encoding, spec.m_position, 1, 0);
        else
            return false;
        return true;
    case ARM_A32_OPT_MSR_MASK:
    {
        // The names of the 32 values
        for (uint value = 1; value < 32; value++)
        {
            if ((value & 15) == 0 && ((value >> 4) == 0))
                continue;
            cString text = msrMaskText(value);
            text.makeLower();
            if (text == name)
            {
                putMsrMask(spec, encoding, value);
                return true;
            }
        }
        // cpsr, cpsr_fsxc... in any order of the letters
        if ((name.length() >= 4) && ((name.left(4) == "cpsr") || (name.left(4) == "spsr")))
        {
            uint value = (name[0] == XSTL_CHAR('s')) ? 16 : 0;
            cString canonical(name);
            uint mask = 0;
            if (name.length() == 4)
                return false;
            if (name[4] != XSTL_CHAR('_'))
                return false;
            for (uint i = 5; i < name.length(); i++)
            {
                if (name[i] == XSTL_CHAR('f')) mask |= 8;
                else if (name[i] == XSTL_CHAR('s')) mask |= 4;
                else if (name[i] == XSTL_CHAR('x')) mask |= 2;
                else if (name[i] == XSTL_CHAR('c')) mask |= 1;
                else return false;
            }
            value |= mask;
            if (value == 0)
                return false;
            // The canonical spelling only: apsr_nzcvq is cpsr_f
            cString spelled = msrMaskText(value);
            spelled.makeLower();
            if (!(spelled == name))
                return false;
            putMsrMask(spec, encoding, value);
            return true;
        }
        return false;
    }
    case ARM_A32_OPT_BANKED:
    {
        uint value;
        if (!findA32BankedRegister(name, value))
            return false;
        if ((spec.m_flags & (ARM_A32_BANKED_T32_MRS | ARM_A32_BANKED_T32_MSR)) != 0)
        {
            setBits(encoding, 20, 1, value >> 5);
            setBits(encoding, 4, 1, (value >> 4) & 1);
            setBits(encoding, ((spec.m_flags & ARM_A32_BANKED_T32_MRS) != 0) ? 16 : 8, 4, value & 15);
            return true;
        }
        setBits(encoding, 22, 1, value >> 5);
        setBits(encoding, 8, 1, (value >> 4) & 1);
        setBits(encoding, 16, 4, value & 15);
        return true;
    }
    default:
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// The fixed operands, the fields and the branch targets

const char* const gFixedTexts[ARM_A32_FIXED_COUNT] = {
    "apsr", "spsr", "sp", "lr", "csync", "#0", "sp!", "#0x3!", "#0x1!", "#0x4!", "#0x2!", "pc", "r12", "#0x0"
};

bool decodeField(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    operand.m_kind = Operand::KIND_IMMEDIATE;
    switch (spec.m_extra)
    {
    case ARM_A32_FIELD_IMM16_SPLIT:
        operand.m_immediate = (field(encoding, 8, 12) << 4) | field(encoding, 0, 4);
        operand.m_isHex = true;
        return true;
    case ARM_A32_FIELD_MOVW_IMM16:
        operand.m_immediate = (field(encoding, 16, 4) << 12) | field(encoding, 0, 12);
        operand.m_isHex = true;
        return true;
    case ARM_A32_FIELD_BF_LSB:
    {
        // LLVM sets the lsb to the msb when lsb > msb
        uint msb = field(encoding, 16, 5);
        uint lsb = field(encoding, 7, 5);
        operand.m_immediate = (lsb > msb) ? msb : lsb;
        return true;
    }
    case ARM_A32_FIELD_BF_WIDTH:
    {
        // LLVM sets the lsb to the msb when lsb > msb
        uint msb = field(encoding, 16, 5);
        uint lsb = field(encoding, 7, 5);
        if (lsb > msb)
            lsb = msb;
        operand.m_immediate = msb - lsb + 1;
        return true;
    }
    case ARM_A32_FIELD_PLUS_ONE:
        operand.m_immediate = field(encoding, spec.m_position, spec.m_flags) + 1;
        operand.m_isHex = true;
        return true;
    case ARM_A32_FIELD_SHIFT_AMOUNT:
    {
        uint amount = field(encoding, 7, 5);
        operand.m_immediate = (amount == 0) ? 32 : amount;
        return true;
    }
    case ARM_A32_FIELD_T32_IMM12:
        operand.m_immediate = (field(encoding, 26, 1) << 11) | (field(encoding, 12, 3) << 8) |
                              field(encoding, 0, 8);
        operand.m_isHex = true;
        return true;
    case ARM_A32_FIELD_T32_MOVW_IMM16:
        operand.m_immediate = (field(encoding, 16, 4) << 12) | (field(encoding, 26, 1) << 11) |
                              (field(encoding, 12, 3) << 8) | field(encoding, 0, 8);
        operand.m_isHex = true;
        return true;
    case ARM_A32_FIELD_T32_IMM16:
        operand.m_immediate = (field(encoding, 16, 4) << 12) | field(encoding, 0, 12);
        operand.m_isHex = true;
        return true;
    case ARM_A32_FIELD_T32_SHIFT_AMOUNT:
    {
        uint amount = (field(encoding, 12, 3) << 2) | field(encoding, 6, 2);
        operand.m_immediate = (amount == 0) ? 32 : amount;
        operand.m_isHex = true;
        return true;
    }
    case ARM_A32_FIELD_T16_SHIFT_AMOUNT:
    {
        uint amount = field(encoding, spec.m_position, 5);
        operand.m_immediate = (amount == 0) ? 32 : amount;
        operand.m_isHex = true;
        return true;
    }
    case ARM_A32_FIELD_T32_LSB:
        operand.m_immediate = (field(encoding, 12, 3) << 2) | field(encoding, 6, 2);
        operand.m_isHex = true;
        return true;
    case ARM_A32_FIELD_T32_BF_LSB:
    {
        uint msb = field(encoding, 0, 5);
        uint lsb = (field(encoding, 12, 3) << 2) | field(encoding, 6, 2);
        operand.m_immediate = (lsb > msb) ? msb : lsb;
        return true;
    }
    case ARM_A32_FIELD_T32_BF_WIDTH:
    {
        uint msb = field(encoding, 0, 5);
        uint lsb = (field(encoding, 12, 3) << 2) | field(encoding, 6, 2);
        if (lsb > msb)
            lsb = msb;
        operand.m_immediate = msb - lsb + 1;
        return true;
    }
    case ARM_A32_FIELD_T32_ADR:
    {
        // The sign bits at 21 and 23 have to be equal
        uint sign = field(encoding, 21, 1);
        if (sign != field(encoding, 23, 1))
            return false;
        int64 value = (field(encoding, 26, 1) << 11) | (field(encoding, 12, 3) << 8) |
                      field(encoding, 0, 8);
        // (a subtraction of zero is subw, see the table)
        operand.m_immediate = sign ? -value : value;
        return true;
    }
    default:
        return false;
    }
}

bool encodeField(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if (operand.m_kind != Operand::KIND_IMMEDIATE)
        return false;
    int64 value = operand.m_immediate;
    switch (spec.m_extra)
    {
    case ARM_A32_FIELD_IMM16_SPLIT:
        if ((value < 0) || (value > 0xFFFF))
            return false;
        setBits(encoding, 8, 12, (uint)value >> 4);
        setBits(encoding, 0, 4, (uint)value & 15);
        return true;
    case ARM_A32_FIELD_MOVW_IMM16:
        if ((value < 0) || (value > 0xFFFF))
            return false;
        setBits(encoding, 16, 4, (uint)value >> 12);
        setBits(encoding, 0, 12, (uint)value & 0xFFF);
        return true;
    case ARM_A32_FIELD_BF_LSB:
        if ((value < 0) || (value > 31))
            return false;
        setBits(encoding, 7, 5, (uint)value);
        return true;
    case ARM_A32_FIELD_BF_WIDTH:
    {
        uint lsb = field(encoding, 7, 5);
        if ((value < 1) || (lsb + value > 32))
            return false;
        setBits(encoding, 16, 5, lsb + (uint)value - 1);
        return true;
    }
    case ARM_A32_FIELD_PLUS_ONE:
        if ((value < 1) || (value > ((int64)1 << spec.m_flags)))
            return false;
        setBits(encoding, spec.m_position, spec.m_flags, (uint)(value - 1));
        return true;
    case ARM_A32_FIELD_SHIFT_AMOUNT:
        if ((value < 1) || (value > 32))
            return false;
        setBits(encoding, 7, 5, (uint)value & 31);
        return true;
    case ARM_A32_FIELD_T32_IMM12:
        if ((value < 0) || (value > 0xFFF))
            return false;
        setBits(encoding, 26, 1, (uint)value >> 11);
        setBits(encoding, 12, 3, ((uint)value >> 8) & 7);
        setBits(encoding, 0, 8, (uint)value & 0xFF);
        return true;
    case ARM_A32_FIELD_T32_MOVW_IMM16:
        if ((value < 0) || (value > 0xFFFF))
            return false;
        setBits(encoding, 16, 4, (uint)value >> 12);
        setBits(encoding, 26, 1, ((uint)value >> 11) & 1);
        setBits(encoding, 12, 3, ((uint)value >> 8) & 7);
        setBits(encoding, 0, 8, (uint)value & 0xFF);
        return true;
    case ARM_A32_FIELD_T32_IMM16:
        if ((value < 0) || (value > 0xFFFF))
            return false;
        setBits(encoding, 16, 4, (uint)value >> 12);
        setBits(encoding, 0, 12, (uint)value & 0xFFF);
        return true;
    case ARM_A32_FIELD_T32_SHIFT_AMOUNT:
        if ((value < 1) || (value > 32))
            return false;
        setBits(encoding, 12, 3, ((uint)value & 31) >> 2);
        setBits(encoding, 6, 2, (uint)value & 3);
        return true;
    case ARM_A32_FIELD_T16_SHIFT_AMOUNT:
        if ((value < 1) || (value > 32))
            return false;
        setBits(encoding, spec.m_position, 5, (uint)value & 31);
        return true;
    case ARM_A32_FIELD_T32_LSB:
        if ((value < 0) || (value > 31))
            return false;
        setBits(encoding, 12, 3, (uint)value >> 2);
        setBits(encoding, 6, 2, (uint)value & 3);
        return true;
    case ARM_A32_FIELD_T32_BF_LSB:
        if ((value < 0) || (value > 31))
            return false;
        setBits(encoding, 12, 3, (uint)value >> 2);
        setBits(encoding, 6, 2, (uint)value & 3);
        return true;
    case ARM_A32_FIELD_T32_BF_WIDTH:
    {
        uint lsb = (field(encoding, 12, 3) << 2) | field(encoding, 6, 2);
        if ((value < 1) || (lsb + value > 32))
            return false;
        setBits(encoding, 0, 5, lsb + (uint)value - 1);
        return true;
    }
    case ARM_A32_FIELD_T32_ADR:
    {
        bool isSubtract = (value < 0);
        uint64 magnitude = isSubtract ? (uint64)(-value) : (uint64)value;
        if (magnitude > 0xFFF)
            return false;
        setBits(encoding, 21, 1, isSubtract ? 1 : 0);
        setBits(encoding, 23, 1, isSubtract ? 1 : 0);
        setBits(encoding, 26, 1, (uint)magnitude >> 11);
        setBits(encoding, 12, 3, ((uint)magnitude >> 8) & 7);
        setBits(encoding, 0, 8, (uint)magnitude & 0xFF);
        return true;
    }
    default:
        return false;
    }
}

bool decodeBranch(const ArmOperandSpec& spec, uint32 encoding, const ArmCodecContext& context,
                  Operand& operand)
{
    int64 offset;
    switch (spec.m_flags)
    {
    case ARM_A32_BRANCH_B:
        offset = signExtend(field(encoding, 0, 24), 24) * 4 + 8;
        break;
    case ARM_A32_BRANCH_BLX:
        offset = signExtend(field(encoding, 0, 24), 24) * 4 + field(encoding, 24, 1) * 2 + 8;
        break;
    default:
        return decodeA32ThumbBranch(spec, encoding, context, operand);
    }
    operand.m_kind = Operand::KIND_BRANCH_TARGET;
    operand.m_immediate = (int64)((context.m_address + (uint64)offset) & 0xFFFFFFFFu);
    return true;
}

bool encodeBranch(const ArmOperandSpec& spec, const Operand& operand, const ArmCodecContext& context,
                  uint32& encoding)
{
    uint64 target;
    if (!getBranchTarget(operand, context, target))
        return false;
    int64 offset = (int64)(int32)(uint32)(target - context.m_address) - 8;
    switch (spec.m_flags)
    {
    case ARM_A32_BRANCH_B:
        if ((offset & 3) != 0)
            return false;
        offset /= 4;
        if ((offset < -(int64)0x800000) || (offset >= (int64)0x800000))
            return false;
        setBits(encoding, 0, 24, (uint)offset & 0xFFFFFF);
        return true;
    case ARM_A32_BRANCH_BLX:
    {
        if ((offset & 1) != 0)
            return false;
        int64 words = offset >> 2;
        if ((words < -(int64)0x800000) || (words >= (int64)0x800000))
            return false;
        setBits(encoding, 0, 24, (uint)words & 0xFFFFFF);
        setBits(encoding, 24, 1, (uint)((offset >> 1) & 1));
        return true;
    }
    default:
        return encodeA32ThumbBranch(spec, operand, context, encoding);
    }
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////
// The entry points

const char* getA32FixedText(uint text)
{
    ASSERT(text < ARM_A32_FIXED_COUNT);
    return gFixedTexts[text];
}

bool decodeA32(const ArmOperandSpec& spec,
               uint32 encoding,
               const ArmCodecContext& context,
               ArmDecodedOperand& operand)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_GPR:
        return decodeGpr(spec, encoding, operand);
    case ARM_KIND_T16_GPR_HIGH:
        // The top bit (m_extra) and 3 bits at m_position
        makeRegister(operand, field(encoding, spec.m_position, 3) |
                              (field(encoding, spec.m_extra, 1) << 3));
        return true;
    case ARM_KIND_GPR_LIST:
        return decodeList(spec, encoding, operand);
    case ARM_KIND_SHIFTED_REGISTER:
        return decodeShifted(spec, encoding, operand);
    case ARM_KIND_A32_MODIFIED_IMMEDIATE:
        return decodeModifiedImmediate(spec, encoding, operand);
    case ARM_KIND_BRANCH_TARGET:
        return decodeBranch(spec, encoding, context, operand);
    case ARM_KIND_A32_MEMORY:
        if (spec.m_extra >= ARM_A32_SIMD_BASE)
            return decodeA32Simd(spec, encoding, context, operand);
        return decodeA32Memory(spec, encoding, context, operand);
    case ARM_KIND_A32_OPTION:
        return decodeOption(spec, encoding, context, operand);
    case ARM_KIND_A32_FIXED:
        if (spec.m_extra >= ARM_A32_SIMD_BASE)
            return decodeA32Simd(spec, encoding, context, operand);
        operand.m_kind = Operand::KIND_NAME;
        operand.m_name = cString(getA32FixedText(spec.m_extra));
        return true;
    case ARM_KIND_A32_FIELD:
        if (spec.m_extra >= ARM_A32_SIMD_BASE)
            return decodeA32Simd(spec, encoding, context, operand);
        return decodeField(spec, encoding, operand);
    default:
        return decodeA32Simd(spec, encoding, context, operand);
    }
}

bool encodeA32(const ArmOperandSpec& spec,
               const ArmDecodedOperand& operand,
               const ArmCodecContext& context,
               uint32& encoding)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_GPR:
        return encodeGpr(spec, operand, encoding);
    case ARM_KIND_T16_GPR_HIGH:
    {
        uint number;
        if (!getRegisterNumber(operand, number))
            return false;
        setBits(encoding, spec.m_position, 3, number & 7);
        setBits(encoding, spec.m_extra, 1, number >> 3);
        return true;
    }
    case ARM_KIND_GPR_LIST:
        return encodeList(spec, operand, encoding);
    case ARM_KIND_SHIFTED_REGISTER:
        return encodeShifted(spec, operand, encoding);
    case ARM_KIND_A32_MODIFIED_IMMEDIATE:
        return encodeModifiedImmediate(spec, operand, encoding);
    case ARM_KIND_BRANCH_TARGET:
        return encodeBranch(spec, operand, context, encoding);
    case ARM_KIND_A32_MEMORY:
        if (spec.m_extra >= ARM_A32_SIMD_BASE)
            return encodeA32Simd(spec, operand, context, encoding);
        return encodeA32Memory(spec, operand, context, encoding);
    case ARM_KIND_A32_OPTION:
        return encodeOption(spec, operand, context, encoding);
    case ARM_KIND_A32_FIXED:
    {
        if (spec.m_extra >= ARM_A32_SIMD_BASE)
            return encodeA32Simd(spec, operand, context, encoding);
        // The text of the operand: a register or a name
        cString text;
        if (operand.m_name.length() != 0)
            text = operand.m_name;
        else if (operand.m_kind == Operand::KIND_REGISTER)
        {
            const char* registerName = getRegisterName(ArmInstructionSet::ARM_A32, operand.m_register);
            if (registerName == NULL)
                return false;
            text = cString(registerName);
        } else if ((operand.m_kind == Operand::KIND_IMMEDIATE) && (operand.m_immediate == 0) &&
                   (spec.m_extra == ARM_A32_FIXED_ZERO))
            return true;
        else
            return false;
        text.makeLower();
        return text == getA32FixedText(spec.m_extra);
    }
    case ARM_KIND_A32_FIELD:
        if (spec.m_extra >= ARM_A32_SIMD_BASE)
            return encodeA32Simd(spec, operand, context, encoding);
        return encodeField(spec, operand, encoding);
    default:
        return encodeA32Simd(spec, operand, context, encoding);
    }
}

//////////////////////////////////////////////////////////////////////////
// The mnemonic parts

cString decodeMnemonicPartA32(const ArmOperandSpec& spec,
                              uint32 encoding,
                              const ArmCodecContext& context)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_CONDITION_MNEMONIC:
    {
        uint condition;
        bool isT32 = (context.m_type == ArmInstructionSet::ARM_T32);
        if ((spec.m_flags & ARM_A32_COND_IT_ONLY) != 0 && !isT32)
            return cString();
        if (((spec.m_flags & (ARM_A32_COND_IT | ARM_A32_COND_IT_ONLY)) != 0) ||
            (isT32 && (spec.m_position == 28)))
            condition = (context.m_itState == 0) ? 14 : (context.m_itState >> 4);
        else if (isT32 && (context.m_itState != 0))
            // (the branches of the it block take the condition of it)
            condition = context.m_itState >> 4;
        else
            condition = field(encoding, spec.m_position, 4);
        if (condition == 14)
            return cString();
        // (the else conditions of an it block that starts with 1111)
        if (condition == 15)
            return cString("<und>");
        return cString(getConditionName(condition));
    }
    case ARM_KIND_SETS_FLAGS:
        if ((spec.m_flags & ARM_A32_SETS_FLAGS_OUTSIDE_IT) != 0)
            return (context.m_itState == 0) ? cString("s") : cString();
        return (field(encoding, spec.m_position, 1) != 0) ? cString("s") : cString();
    case ARM_KIND_T16_IT_MASK:
    {
        // The letters: t if the bit of the instruction is the one of the first
        // condition, e otherwise
        uint mask = field(encoding, spec.m_position, 4);
        uint first = field(encoding, spec.m_extra, 4);
        // (LLVM takes the firstcond 1111 as 1110)
        first = (first == 15) ? 0 : (first & 1);
        cString letters;
        if (mask == 0)
            return letters;
        uint lowest = 0;
        while (((mask >> lowest) & 1) == 0)
            lowest++;
        for (int position = 3; position > (int)lowest; position--)
            letters += (((mask >> position) & 1) == first) ? "t" : "e";
        return letters;
    }
    case ARM_KIND_A32_NEON_DATA_TYPE:
        return decodeNeonDataType(spec, encoding);
    case ARM_KIND_A32_MNEMONIC_PART:
        switch (spec.m_extra)
        {
        case ARM_A32_PART_CPS_IMOD:
        {
            uint imod = field(encoding, 18, 2);
            return (imod == 2) ? cString("ie") : (imod == 3) ? cString("id") : cString();
        }
        case ARM_A32_PART_CPS_IMOD_T16:
            return (field(encoding, 4, 1) != 0) ? cString("id") : cString("ie");
        case ARM_A32_PART_CPS_IMOD_T32:
        {
            uint imod = field(encoding, 9, 2);
            return (imod == 2) ? cString("ie") : (imod == 3) ? cString("id") : cString();
        }
        default:
            break;
        }
        return cString();
    default:
        return cString();
    }
}

bool encodeMnemonicPartA32(const ArmOperandSpec& spec,
                           cString& suffix,
                           const ArmCodecContext& context,
                           uint32& encoding)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_CONDITION_MNEMONIC:
    {
        uint condition = 14;
        bool isT32 = (context.m_type == ArmInstructionSet::ARM_T32);
        if ((suffix.length() >= 2) && findCondition(suffix.left(2), condition))
        {
            // al is written "al", it is not an error
            suffix = suffix.part(2, suffix.length());
        } else
        {
            condition = 14;
        }
        if ((spec.m_flags & ARM_A32_COND_IT_ONLY) != 0 && !isT32)
        {
            // Unconditional in A32
            return condition == 14;
        }
        if (((spec.m_flags & (ARM_A32_COND_IT | ARM_A32_COND_IT_ONLY)) != 0) ||
            (isT32 && (spec.m_position == 28)))
        {
            // The condition of the it block
            uint expected = (context.m_itState == 0) ? 14 : (context.m_itState >> 4);
            if (spec.m_position == 28)
                setBits(encoding, 28, 4, 14);
            return condition == expected;
        }
        if (condition == 15)
            return false;
        setBits(encoding, spec.m_position, 4, condition);
        return true;
    }
    case ARM_KIND_SETS_FLAGS:
    {
        bool hasS = (suffix.length() > 0) && (suffix[0] == XSTL_CHAR('s'));
        if ((spec.m_flags & ARM_A32_SETS_FLAGS_OUTSIDE_IT) != 0)
        {
            // adds outside of an it block, add inside one
            if (hasS != (context.m_itState == 0))
                return false;
        } else
        {
            setBits(encoding, spec.m_position, 1, hasS ? 1 : 0);
        }
        if (hasS)
            suffix = suffix.part(1, suffix.length());
        return true;
    }
    case ARM_KIND_T16_IT_MASK:
    {
        // The t and e letters: the bits of the mask in the form where "then"
        // is zero. The condition operand flips the bits when the first
        // condition is odd (the mask has the low bit of each condition)
        uint mask = 0;
        uint count = 0;
        while ((suffix.length() > 0) && ((suffix[0] == XSTL_CHAR('t')) || (suffix[0] == XSTL_CHAR('e'))))
        {
            if (count == 3)
                return false;
            if (suffix[0] == XSTL_CHAR('e'))
                mask |= 1u << (3 - count);
            suffix = suffix.part(1, suffix.length());
            count++;
        }
        mask |= 1u << (3 - count);
        setBits(encoding, spec.m_position, 4, mask);
        return true;
    }
    case ARM_KIND_A32_NEON_DATA_TYPE:
        return encodeNeonDataType(spec, suffix, encoding);
    case ARM_KIND_A32_MNEMONIC_PART:
        switch (spec.m_extra)
        {
        case ARM_A32_PART_CPS_IMOD_T16:
            if ((suffix.length() >= 2) && (suffix.left(2) == "ie"))
            {
                setBits(encoding, 4, 1, 0);
                suffix = suffix.part(2, suffix.length());
                return true;
            }
            if ((suffix.length() >= 2) && (suffix.left(2) == "id"))
            {
                setBits(encoding, 4, 1, 1);
                suffix = suffix.part(2, suffix.length());
                return true;
            }
            return false;
        case ARM_A32_PART_CPS_IMOD_T32:
            if ((suffix.length() >= 2) && (suffix.left(2) == "ie"))
            {
                setBits(encoding, 9, 2, 2);
                suffix = suffix.part(2, suffix.length());
                return true;
            }
            if ((suffix.length() >= 2) && (suffix.left(2) == "id"))
            {
                setBits(encoding, 9, 2, 3);
                suffix = suffix.part(2, suffix.length());
                return true;
            }
            setBits(encoding, 9, 2, 0);
            return true;
        case ARM_A32_PART_CPS_IMOD:
            if ((suffix.length() >= 2) && (suffix.left(2) == "ie"))
            {
                setBits(encoding, 18, 2, 2);
                suffix = suffix.part(2, suffix.length());
                return true;
            }
            if ((suffix.length() >= 2) && (suffix.left(2) == "id"))
            {
                setBits(encoding, 18, 2, 3);
                suffix = suffix.part(2, suffix.length());
                return true;
            }
            setBits(encoding, 18, 2, 0);
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

} // namespace armdis
