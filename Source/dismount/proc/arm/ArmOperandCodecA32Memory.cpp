#include "dismount/dismount.h"
/*
 * ArmOperandCodecA32Memory.cpp
 *
 * Implementation file. The addressing modes of AArch32 (ARM_KIND_A32_MEMORY):
 * the A32 loads and stores, the coprocessor loads and stores and the Thumb
 * ones. See ArmOperandCodecA32.h for the decoded form.
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

uint field(uint32 encoding, uint position, uint width)
{
    return ArmOperandCodec::getField(encoding, position, width);
}

void setBits(uint32& encoding, uint position, uint width, uint value)
{
    ArmOperandCodec::setField(encoding, position, width, value);
}

// Return the register number of a plain register operand
bool registerNumber(uint registerId, uint& number)
{
    if ((registerId < ARM_REG_R0) || (registerId > ARM_REG_PC))
        return false;
    number = registerId - ARM_REG_R0;
    return true;
}

// The shift of an index register: the type 0-3 and the amount field of 5 bits
void setIndexShift(Operand& operand, uint type, uint amount)
{
    switch (type)
    {
    case 0:
        if (amount != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = amount;
        }
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
            operand.m_shift = Operand::SHIFT_RRX;
        else
        {
            operand.m_shift = Operand::SHIFT_ROR;
            operand.m_shiftAmount = amount;
        }
        break;
    }
}

bool getIndexShift(const Operand& operand, uint& type, uint& amount)
{
    uint count = operand.m_shiftAmount;
    switch (operand.m_shift)
    {
    case Operand::SHIFT_NONE:
        type = 0;
        amount = 0;
        return operand.m_shiftAmount == 0;
    case Operand::SHIFT_LSL:
        type = 0;
        amount = count;
        return (count >= 1) && (count <= 31);
    case Operand::SHIFT_LSR:
        type = 1;
        amount = count & 31;
        return (count >= 1) && (count <= 32);
    case Operand::SHIFT_ASR:
        type = 2;
        amount = count & 31;
        return (count >= 1) && (count <= 32);
    case Operand::SHIFT_ROR:
        type = 3;
        amount = count;
        return (count >= 1) && (count <= 31);
    case Operand::SHIFT_RRX:
        type = 3;
        amount = 0;
        return true;
    default:
        return false;
    }
}

// Start a memory operand with the base register
void startMemory(Operand& operand, uint base)
{
    operand.m_kind = Operand::KIND_MEMORY;
    operand.m_base = ARM_REG_R0 + base;
}

// True if the memory operand has the shape: base register and an immediate
// (no index register, no post index register)
bool isImmediateForm(const Operand& operand)
{
    return (operand.m_kind == Operand::KIND_MEMORY) && (operand.m_index == ARM_NO_REGISTER) &&
           (operand.m_postIndexRegister == ARM_NO_REGISTER) && (operand.m_alignment == 0) &&
           (operand.m_shift == Operand::SHIFT_NONE);
}

// Return the sign and the magnitude of the offset of an immediate form
bool isOffsetEncodable(const Operand& operand, uint maximum, uint scale)
{
    if (operand.m_offset < 0)
        return false;
    if ((operand.m_offset & (((int64)1 << scale) - 1)) != 0)
        return false;
    return (operand.m_offset >> scale) <= (int64)maximum;
}

} // anonymous namespace

bool decodeA32Memory(const ArmOperandSpec& spec,
                     uint32 encoding,
                     const ArmCodecContext& context,
                     ArmDecodedOperand& operand)
{
    uint base = field(encoding, spec.m_position, 4);
    bool isPre = (spec.m_flags & ARM_A32_MEM_PRE) != 0;
    startMemory(operand, base);
    if (isPre)
    {
        operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
        operand.m_isExplicit = true;
    }
    switch (spec.m_extra)
    {
    case ARM_A32_ADDR_IMM12:
        operand.m_offset = field(encoding, 0, 12);
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        operand.m_isHex = true;
        // (the offset form doesn't print a zero offset, but "#-0")
        operand.m_isExplicit = isPre;
        return true;
    case ARM_A32_ADDR_SO_REG:
        operand.m_index = ARM_REG_R0 + field(encoding, 0, 4);
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        setIndexShift(operand, field(encoding, 5, 2), field(encoding, 7, 5));
        return true;
    case ARM_A32_ADDR_MODE3:
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        if (field(encoding, 22, 1) != 0)
            operand.m_offset = (field(encoding, 8, 4) << 4) | field(encoding, 0, 4);
        else
            operand.m_index = ARM_REG_R0 + field(encoding, 0, 4);
        return true;
    case ARM_A32_ADDR_MODE3_POST:
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_isExplicit = false;
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        if (field(encoding, 22, 1) != 0)
        {
            operand.m_offset = (field(encoding, 8, 4) << 4) | field(encoding, 0, 4);
        } else
        {
            operand.m_postIndexRegister = ARM_REG_R0 + field(encoding, 0, 4);
        }
        return true;
    case ARM_A32_ADDR_MODE2_POST_IMM:
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_isExplicit = false;
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        operand.m_offset = field(encoding, 0, 12);
        return true;
    case ARM_A32_ADDR_MODE2_POST_REG:
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_isExplicit = false;
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        operand.m_postIndexRegister = ARM_REG_R0 + field(encoding, 0, 4);
        setIndexShift(operand, field(encoding, 5, 2), field(encoding, 7, 5));
        return true;
    case ARM_A32_ADDR_BASE:
        return true;
    case ARM_A32_ADDR_POST_IMM8:
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        operand.m_offset = (field(encoding, 8, 4) << 4) | field(encoding, 0, 4);
        return true;
    case ARM_A32_ADDR_POST_REG:
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        operand.m_postIndexRegister = ARM_REG_R0 + field(encoding, 0, 4);
        return true;
    case ARM_A32_ADDR_MODE5:
        operand.m_offset = field(encoding, 0, 8) * 4;
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        return true;
    case ARM_A32_ADDR_MODE5_POST:
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_offset = field(encoding, 0, 8) * 4;
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        return true;
    case ARM_A32_ADDR_MODE5_OPTION:
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_offset = field(encoding, 0, 8);
        operand.m_optionScheme = ARM_A32_OPT_COPROC_OPTION;
        return true;
    default:
        break;
    }
    return decodeA32ThumbMemory(spec, encoding, context, operand);
}

bool encodeA32Memory(const ArmOperandSpec& spec,
                     const ArmDecodedOperand& operand,
                     const ArmCodecContext& context,
                     uint32& encoding)
{
    if (operand.m_kind != Operand::KIND_MEMORY)
        return false;
    uint base;
    if (!registerNumber(operand.m_base, base))
        return false;
    bool isPre = (spec.m_flags & ARM_A32_MEM_PRE) != 0;
    switch (spec.m_extra)
    {
    case ARM_A32_ADDR_IMM12:
    case ARM_A32_ADDR_SO_REG:
    case ARM_A32_ADDR_MODE3:
    case ARM_A32_ADDR_MODE5:
        if (operand.m_addressing != (isPre ? Operand::ADDRESSING_PRE_INDEX :
                                             Operand::ADDRESSING_OFFSET))
            return false;
        break;
    case ARM_A32_ADDR_BASE:
        if (!isImmediateForm(operand) || (operand.m_offset != 0) || operand.m_isSubtract ||
            (operand.m_addressing != Operand::ADDRESSING_OFFSET))
            return false;
        break;
    case ARM_A32_ADDR_MODE3_POST:
    case ARM_A32_ADDR_MODE2_POST_IMM:
    case ARM_A32_ADDR_MODE2_POST_REG:
    case ARM_A32_ADDR_POST_IMM8:
    case ARM_A32_ADDR_POST_REG:
    case ARM_A32_ADDR_MODE5_POST:
    case ARM_A32_ADDR_MODE5_OPTION:
        if (operand.m_addressing != Operand::ADDRESSING_POST_INDEX)
            return false;
        break;
    default:
        return encodeA32ThumbMemory(spec, operand, context, encoding);
    }
    // The unwritten form of an immediate zero offset: an explicit "#0" in the
    // offset form is the same encoding
    switch (spec.m_extra)
    {
    case ARM_A32_ADDR_IMM12:
        if (!isImmediateForm(operand) || !isOffsetEncodable(operand, 4095, 0))
            return false;
        setBits(encoding, 0, 12, (uint)operand.m_offset);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    case ARM_A32_ADDR_SO_REG:
    {
        uint index, type, amount;
        if ((operand.m_index == ARM_NO_REGISTER) || !registerNumber(operand.m_index, index) ||
            !getIndexShift(operand, type, amount))
            return false;
        setBits(encoding, 0, 4, index);
        setBits(encoding, 5, 2, type);
        setBits(encoding, 7, 5, amount);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    }
    case ARM_A32_ADDR_MODE3:
        if (operand.m_index != ARM_NO_REGISTER)
        {
            uint index;
            if (!registerNumber(operand.m_index, index) || (operand.m_shift != Operand::SHIFT_NONE) ||
                (operand.m_offset != 0))
                return false;
            setBits(encoding, 22, 1, 0);
            setBits(encoding, 0, 4, index);
            setBits(encoding, 8, 4, 0);
        } else
        {
            if (!isImmediateForm(operand) || !isOffsetEncodable(operand, 255, 0))
                return false;
            setBits(encoding, 22, 1, 1);
            setBits(encoding, 8, 4, (uint)operand.m_offset >> 4);
            setBits(encoding, 0, 4, (uint)operand.m_offset & 15);
        }
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    case ARM_A32_ADDR_MODE3_POST:
        if (operand.m_postIndexRegister != ARM_NO_REGISTER)
        {
            uint index;
            if (!registerNumber(operand.m_postIndexRegister, index) ||
                (operand.m_shift != Operand::SHIFT_NONE) || (operand.m_index != ARM_NO_REGISTER))
                return false;
            setBits(encoding, 22, 1, 0);
            setBits(encoding, 0, 4, index);
            setBits(encoding, 8, 4, 0);
        } else
        {
            if ((operand.m_index != ARM_NO_REGISTER) || !isOffsetEncodable(operand, 255, 0))
                return false;
            setBits(encoding, 22, 1, 1);
            setBits(encoding, 8, 4, (uint)operand.m_offset >> 4);
            setBits(encoding, 0, 4, (uint)operand.m_offset & 15);
        }
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    case ARM_A32_ADDR_MODE2_POST_IMM:
        if ((operand.m_postIndexRegister != ARM_NO_REGISTER) ||
            (operand.m_index != ARM_NO_REGISTER) || !isOffsetEncodable(operand, 4095, 0))
            return false;
        setBits(encoding, 0, 12, (uint)operand.m_offset);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    case ARM_A32_ADDR_MODE2_POST_REG:
    {
        uint index, type, amount;
        if ((operand.m_postIndexRegister == ARM_NO_REGISTER) ||
            !registerNumber(operand.m_postIndexRegister, index) ||
            !getIndexShift(operand, type, amount))
            return false;
        setBits(encoding, 0, 4, index);
        setBits(encoding, 5, 2, type);
        setBits(encoding, 7, 5, amount);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    }
    case ARM_A32_ADDR_BASE:
        break;
    case ARM_A32_ADDR_POST_IMM8:
        if ((operand.m_postIndexRegister != ARM_NO_REGISTER) || !isOffsetEncodable(operand, 255, 0))
            return false;
        setBits(encoding, 8, 4, (uint)operand.m_offset >> 4);
        setBits(encoding, 0, 4, (uint)operand.m_offset & 15);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    case ARM_A32_ADDR_POST_REG:
    {
        uint index;
        if ((operand.m_postIndexRegister == ARM_NO_REGISTER) ||
            !registerNumber(operand.m_postIndexRegister, index) ||
            (operand.m_shift != Operand::SHIFT_NONE))
            return false;
        setBits(encoding, 0, 4, index);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    }
    case ARM_A32_ADDR_MODE5:
        if (!isImmediateForm(operand) || !isOffsetEncodable(operand, 255, 2))
            return false;
        setBits(encoding, 0, 8, (uint)operand.m_offset >> 2);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    case ARM_A32_ADDR_MODE5_POST:
        if (!isImmediateForm(operand) || !isOffsetEncodable(operand, 255, 2))
            return false;
        setBits(encoding, 0, 8, (uint)operand.m_offset >> 2);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        break;
    case ARM_A32_ADDR_MODE5_OPTION:
        if ((operand.m_optionScheme != ARM_A32_OPT_COPROC_OPTION) || (operand.m_offset < 0) ||
            (operand.m_offset > 255))
            return false;
        setBits(encoding, 0, 8, (uint)operand.m_offset);
        break;
    default:
        return false;
    }
    setBits(encoding, spec.m_position, 4, base);
    return true;
}

} // namespace armdis
