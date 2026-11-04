#include "dismount/dismount.h"
/*
 * ArmOperandCodecA32Thumb.cpp
 *
 * Implementation file. The Thumb (T16 and T32) addressing modes and branch
 * targets of ArmOperandCodec. The decoder follows the one of LLVM
 * (ARMDisassembler.cpp): see ArmOperandCodecA32.cpp.
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

bool registerNumber(uint registerId, uint& number)
{
    if ((registerId < ARM_REG_R0) || (registerId > ARM_REG_PC))
        return false;
    number = registerId - ARM_REG_R0;
    return true;
}

void startMemory(Operand& operand, uint base)
{
    operand.m_kind = Operand::KIND_MEMORY;
    operand.m_base = ARM_REG_R0 + base;
}

// True if the memory operand is a base register and an immediate
bool isImmediateForm(const Operand& operand)
{
    return (operand.m_kind == Operand::KIND_MEMORY) && (operand.m_index == ARM_NO_REGISTER) &&
           (operand.m_postIndexRegister == ARM_NO_REGISTER) && (operand.m_alignment == 0) &&
           (operand.m_shift == Operand::SHIFT_NONE) && (operand.m_offset >= 0);
}

// The offset of an immediate form scaled down, false if it can't be encoded
bool scaledOffset(const Operand& operand, uint scale, uint maximum, uint& value)
{
    if (!isImmediateForm(operand))
        return false;
    if ((operand.m_offset & (((int64)1 << scale) - 1)) != 0)
        return false;
    if ((operand.m_offset >> scale) > (int64)maximum)
        return false;
    value = (uint)(operand.m_offset >> scale);
    return true;
}

} // anonymous namespace

bool decodeA32ThumbMemory(const ArmOperandSpec& spec, uint32 encoding,
                          const ArmCodecContext& context, ArmDecodedOperand& operand)
{
    bool isPre = (spec.m_flags & ARM_A32_MEM_PRE) != 0;
    switch (spec.m_extra)
    {
    case ARM_A32_ADDR_T16_IMM5:
        startMemory(operand, field(encoding, spec.m_position, 3));
        operand.m_offset = (int64)field(encoding, spec.m_position + 3, 5) << (spec.m_flags & ARM_SPEC_SCALE_MASK);
        operand.m_isHex = true;
        return true;
    case ARM_A32_ADDR_T16_REG:
        startMemory(operand, field(encoding, spec.m_position, 3));
        operand.m_index = ARM_REG_R0 + field(encoding, spec.m_position + 3, 3);
        return true;
    case ARM_A32_ADDR_T16_SP:
    case ARM_A32_ADDR_T16_PC:
        startMemory(operand, (spec.m_extra == ARM_A32_ADDR_T16_SP) ? 13 : 15);
        operand.m_offset = (int64)field(encoding, 0, 8) * 4;
        operand.m_isHex = true;
        // The literal load prints the offset always
        operand.m_isExplicit = (spec.m_extra == ARM_A32_ADDR_T16_PC);
        return true;
    case ARM_A32_ADDR_T32_IMM12:
    {
        uint base = field(encoding, spec.m_position, 4);
        if (((spec.m_flags & ARM_A32_MEM_NO_PC) != 0) && (base == 15))
            return false;
        startMemory(operand, base);
        operand.m_offset = field(encoding, 0, 12);
        operand.m_isHex = true;
        return true;
    }
    case ARM_A32_ADDR_T32_IMM8:
    case ARM_A32_ADDR_T32_IMM8_POST:
    {
        uint base = field(encoding, spec.m_position, 4);
        if (((spec.m_flags & ARM_A32_MEM_NO_PC) != 0) && (base == 15))
            return false;
        startMemory(operand, base);
        bool isAdd;
        if ((spec.m_flags & ARM_A32_MEM_POS) != 0)
            isAdd = true;
        else if ((spec.m_flags & ARM_A32_MEM_NEG) != 0)
            isAdd = false;
        else
            isAdd = (field(encoding, 9, 1) != 0);
        operand.m_offset = field(encoding, 0, 8);
        operand.m_isSubtract = !isAdd;
        if (spec.m_extra == ARM_A32_ADDR_T32_IMM8_POST)
        {
            operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        } else if (isPre)
        {
            operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
            operand.m_isExplicit = true;
        }
        return true;
    }
    case ARM_A32_ADDR_T32_REG:
    {
        uint base = field(encoding, spec.m_position, 4);
        if (((spec.m_flags & ARM_A32_MEM_NO_PC) != 0) && (base == 15))
            return false;
        startMemory(operand, base);
        operand.m_index = ARM_REG_R0 + field(encoding, 0, 4);
        uint amount = field(encoding, 4, 2);
        if (amount != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = amount;
        }
        return true;
    }
    case ARM_A32_ADDR_T32_IMM8S4:
    case ARM_A32_ADDR_T32_IMM8S4_POST:
    {
        startMemory(operand, field(encoding, spec.m_position, 4));
        operand.m_offset = (int64)field(encoding, 0, 8) * 4;
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        if (spec.m_extra == ARM_A32_ADDR_T32_IMM8S4_POST)
        {
            operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        } else if (isPre)
        {
            operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
            operand.m_isExplicit = true;
        }
        return true;
    }
    case ARM_A32_ADDR_T32_IMM0_1020S4:
        startMemory(operand, field(encoding, spec.m_position, 4));
        operand.m_offset = (int64)field(encoding, 0, 8) * 4;
        operand.m_isHex = true;
        return true;
    case ARM_A32_ADDR_T32_TBB:
    case ARM_A32_ADDR_T32_TBH:
        startMemory(operand, field(encoding, spec.m_position, 4));
        operand.m_index = ARM_REG_R0 + field(encoding, 0, 4);
        if (spec.m_extra == ARM_A32_ADDR_T32_TBH)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = 1;
        }
        return true;
    case ARM_A32_ADDR_T32_LITERAL:
        startMemory(operand, 15);
        operand.m_offset = field(encoding, 0, 12);
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        operand.m_isHex = true;
        operand.m_isExplicit = true;
        return true;
    default:
        return false;
    }
}

bool encodeA32ThumbMemory(const ArmOperandSpec& spec, const ArmDecodedOperand& operand,
                          const ArmCodecContext& context, uint32& encoding)
{
    if (operand.m_kind != Operand::KIND_MEMORY)
        return false;
    uint base;
    if (!registerNumber(operand.m_base, base))
        return false;
    bool isPre = (spec.m_flags & ARM_A32_MEM_PRE) != 0;
    uint value;
    switch (spec.m_extra)
    {
    case ARM_A32_ADDR_T16_IMM5:
        if (operand.m_addressing != Operand::ADDRESSING_OFFSET || operand.m_isSubtract || base > 7 ||
            !scaledOffset(operand, spec.m_flags & ARM_SPEC_SCALE_MASK, 31, value))
            return false;
        setBits(encoding, spec.m_position, 3, base);
        setBits(encoding, spec.m_position + 3, 5, value);
        return true;
    case ARM_A32_ADDR_T16_REG:
    {
        uint index;
        if (operand.m_addressing != Operand::ADDRESSING_OFFSET || operand.m_isSubtract ||
            operand.m_index == ARM_NO_REGISTER || !registerNumber(operand.m_index, index) ||
            operand.m_shift != Operand::SHIFT_NONE || base > 7 || index > 7)
            return false;
        setBits(encoding, spec.m_position, 3, base);
        setBits(encoding, spec.m_position + 3, 3, index);
        return true;
    }
    case ARM_A32_ADDR_T16_SP:
    case ARM_A32_ADDR_T16_PC:
        if (operand.m_addressing != Operand::ADDRESSING_OFFSET || operand.m_isSubtract ||
            base != ((spec.m_extra == ARM_A32_ADDR_T16_SP) ? 13u : 15u) ||
            !scaledOffset(operand, 2, 255, value))
            return false;
        setBits(encoding, 0, 8, value);
        return true;
    case ARM_A32_ADDR_T32_IMM12:
        if (operand.m_addressing != Operand::ADDRESSING_OFFSET || operand.m_isSubtract ||
            !scaledOffset(operand, 0, 4095, value))
            return false;
        if (((spec.m_flags & ARM_A32_MEM_NO_PC) != 0) && (base == 15))
            return false;
        setBits(encoding, 0, 12, value);
        setBits(encoding, spec.m_position, 4, base);
        return true;
    case ARM_A32_ADDR_T32_IMM8:
    case ARM_A32_ADDR_T32_IMM8_POST:
    {
        if (((spec.m_flags & ARM_A32_MEM_NO_PC) != 0) && (base == 15))
            return false;
        bool isPost = (spec.m_extra == ARM_A32_ADDR_T32_IMM8_POST);
        Operand::Addressing expected = isPost ? Operand::ADDRESSING_POST_INDEX :
                                       isPre ? Operand::ADDRESSING_PRE_INDEX : Operand::ADDRESSING_OFFSET;
        if (operand.m_addressing != expected || !scaledOffset(operand, 0, 255, value))
            return false;
        if ((spec.m_flags & ARM_A32_MEM_POS) != 0)
        {
            if (operand.m_isSubtract)
                return false;
        } else if ((spec.m_flags & ARM_A32_MEM_NEG) != 0)
        {
            if (!operand.m_isSubtract)
                return false;
        } else
        {
            setBits(encoding, 9, 1, operand.m_isSubtract ? 0 : 1);
        }
        setBits(encoding, 0, 8, value);
        setBits(encoding, spec.m_position, 4, base);
        return true;
    }
    case ARM_A32_ADDR_T32_REG:
    {
        uint index;
        if (((spec.m_flags & ARM_A32_MEM_NO_PC) != 0) && (base == 15))
            return false;
        if (operand.m_addressing != Operand::ADDRESSING_OFFSET || operand.m_isSubtract ||
            operand.m_index == ARM_NO_REGISTER || !registerNumber(operand.m_index, index))
            return false;
        uint amount = 0;
        if (operand.m_shift == Operand::SHIFT_LSL)
            amount = operand.m_shiftAmount;
        else if (operand.m_shift != Operand::SHIFT_NONE)
            return false;
        if (amount > 3)
            return false;
        setBits(encoding, 0, 4, index);
        setBits(encoding, 4, 2, amount);
        setBits(encoding, spec.m_position, 4, base);
        return true;
    }
    case ARM_A32_ADDR_T32_IMM8S4:
    case ARM_A32_ADDR_T32_IMM8S4_POST:
    {
        bool isPost = (spec.m_extra == ARM_A32_ADDR_T32_IMM8S4_POST);
        Operand::Addressing expected = isPost ? Operand::ADDRESSING_POST_INDEX :
                                       isPre ? Operand::ADDRESSING_PRE_INDEX : Operand::ADDRESSING_OFFSET;
        if (operand.m_addressing != expected || !scaledOffset(operand, 2, 255, value))
            return false;
        setBits(encoding, 0, 8, value);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        setBits(encoding, spec.m_position, 4, base);
        return true;
    }
    case ARM_A32_ADDR_T32_IMM0_1020S4:
        if (operand.m_addressing != Operand::ADDRESSING_OFFSET || operand.m_isSubtract ||
            !scaledOffset(operand, 2, 255, value))
            return false;
        setBits(encoding, 0, 8, value);
        setBits(encoding, spec.m_position, 4, base);
        return true;
    case ARM_A32_ADDR_T32_TBB:
    case ARM_A32_ADDR_T32_TBH:
    {
        uint index;
        if (operand.m_addressing != Operand::ADDRESSING_OFFSET || operand.m_isSubtract ||
            operand.m_index == ARM_NO_REGISTER || !registerNumber(operand.m_index, index))
            return false;
        if (spec.m_extra == ARM_A32_ADDR_T32_TBH)
        {
            if ((operand.m_shift != Operand::SHIFT_LSL) || (operand.m_shiftAmount != 1))
                return false;
        } else if (operand.m_shift != Operand::SHIFT_NONE)
        {
            return false;
        }
        setBits(encoding, 0, 4, index);
        setBits(encoding, spec.m_position, 4, base);
        return true;
    }
    case ARM_A32_ADDR_T32_LITERAL:
        if (base != 15 || operand.m_addressing != Operand::ADDRESSING_OFFSET ||
            !scaledOffset(operand, 0, 4095, value))
            return false;
        setBits(encoding, 0, 12, value);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        return true;
    default:
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// The branch targets

namespace {

// The offset of S:J1:J2:imm10:imm11 of bl / b.w (the J bits are inverted
// with S)
int64 decodeLongOffset(uint32 encoding)
{
    uint s = field(encoding, 26, 1);
    uint i1 = !(field(encoding, 13, 1) ^ s);
    uint i2 = !(field(encoding, 11, 1) ^ s);
    uint value = (s << 23) | (i1 << 22) | (i2 << 21) | (field(encoding, 16, 10) << 11) |
                 field(encoding, 0, 11);
    return signExtend((uint64)value << 1, 25);
}

// Encode the 25 bit offset, false if out of range
bool encodeLongOffset(int64 offset, uint32& encoding)
{
    if ((offset < -((int64)1 << 24)) || (offset >= ((int64)1 << 24)))
        return false;
    uint value = (uint)(offset >> 1) & 0xFFFFFF;
    uint s = (value >> 23) & 1;
    uint i1 = (value >> 22) & 1;
    uint i2 = (value >> 21) & 1;
    setBits(encoding, 26, 1, s);
    setBits(encoding, 13, 1, !(i1 ^ s));
    setBits(encoding, 11, 1, !(i2 ^ s));
    setBits(encoding, 16, 10, (value >> 11) & 0x3FF);
    setBits(encoding, 0, 11, value & 0x7FF);
    return true;
}

} // anonymous namespace

bool decodeA32ThumbBranch(const ArmOperandSpec& spec, uint32 encoding,
                          const ArmCodecContext& context, ArmDecodedOperand& operand)
{
    int64 target;
    switch (spec.m_flags)
    {
    case ARM_A32_BRANCH_T16_B:
        target = context.m_address + signExtend((uint64)field(encoding, 0, 11) << 1, 12) + 4;
        break;
    case ARM_A32_BRANCH_T16_BCC:
        target = context.m_address + signExtend((uint64)field(encoding, 0, 8) << 1, 9) + 4;
        break;
    case ARM_A32_BRANCH_T16_CB:
        target = context.m_address + (((int64)field(encoding, 9, 1) << 6) |
                                      ((int64)field(encoding, 3, 5) << 1)) + 4;
        break;
    case ARM_A32_BRANCH_T32_BL:
        target = context.m_address + decodeLongOffset(encoding) + 4;
        break;
    case ARM_A32_BRANCH_T32_BLX:
        target = (int64)((context.m_address & ~(uint64)2) + 4) + (decodeLongOffset(encoding) & ~(int64)3);
        break;
    case ARM_A32_BRANCH_T32_BCC:
    {
        uint value = (field(encoding, 0, 11) << 1) | (field(encoding, 11, 1) << 19) |
                     (field(encoding, 13, 1) << 18) | (field(encoding, 16, 6) << 12) |
                     (field(encoding, 26, 1) << 20);
        target = context.m_address + signExtend(value, 21) + 4;
        break;
    }
    default:
        return false;
    }
    operand.m_kind = Operand::KIND_BRANCH_TARGET;
    operand.m_immediate = (int64)((uint64)target & 0xFFFFFFFFu);
    return true;
}

bool encodeA32ThumbBranch(const ArmOperandSpec& spec, const ArmDecodedOperand& operand,
                          const ArmCodecContext& context, uint32& encoding)
{
    uint64 address;
    if (!getBranchTarget(operand, context, address))
        return false;
    int64 offset = (int64)(int32)(uint32)(address - context.m_address) - 4;
    switch (spec.m_flags)
    {
    case ARM_A32_BRANCH_T16_B:
        if (((offset & 1) != 0) || (offset < -2048) || (offset >= 2048))
            return false;
        setBits(encoding, 0, 11, (uint)(offset >> 1) & 0x7FF);
        return true;
    case ARM_A32_BRANCH_T16_BCC:
        if (((offset & 1) != 0) || (offset < -256) || (offset >= 256))
            return false;
        setBits(encoding, 0, 8, (uint)(offset >> 1) & 0xFF);
        return true;
    case ARM_A32_BRANCH_T16_CB:
        if (((offset & 1) != 0) || (offset < 0) || (offset > 126))
            return false;
        setBits(encoding, 9, 1, (uint)(offset >> 6) & 1);
        setBits(encoding, 3, 5, (uint)(offset >> 1) & 31);
        return true;
    case ARM_A32_BRANCH_T32_BL:
        return ((offset & 1) == 0) && encodeLongOffset(offset, encoding);
    case ARM_A32_BRANCH_T32_BLX:
    {
        // The offset is from the aligned pc
        // (clang drops bit 1 of a target which is not word aligned)
        offset = (int64)(int32)(uint32)(address - ((context.m_address & ~(uint64)2) + 4));
        return ((offset & 1) == 0) && encodeLongOffset(offset & ~(int64)3, encoding);
    }
    case ARM_A32_BRANCH_T32_BCC:
    {
        if (((offset & 1) != 0) || (offset < -((int64)1 << 20)) || (offset >= ((int64)1 << 20)))
            return false;
        uint value = (uint)(offset >> 1) & 0xFFFFF;
        setBits(encoding, 0, 11, value & 0x7FF);
        setBits(encoding, 11, 1, (value >> 18) & 1);
        setBits(encoding, 13, 1, (value >> 17) & 1);
        setBits(encoding, 16, 6, (value >> 11) & 0x3F);
        setBits(encoding, 26, 1, (value >> 19) & 1);
        return true;
    }
    default:
        return false;
    }
}

} // namespace armdis
