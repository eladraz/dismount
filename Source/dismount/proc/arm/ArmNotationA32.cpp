#include "dismount/dismount.h"
/*
 * ArmNotationA32.cpp
 *
 * Implementation file. The formatter of the AArch32 operands, in the LLVM
 * syntax (see ArmNotation.h and ArmOperandCodecA32.h for the decoded forms).
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

const ArmInstructionSet::DisassemblerTypes SET = ArmInstructionSet::ARM_A32;

cString registerName(uint registerId)
{
    const char* name = getRegisterName(SET, registerId);
    if (name == NULL)
        return cString("?") + cString((uint32)registerId);
    return cString(name);
}

// The text of a number, in hexadecimal or decimal, with a minus sign if needed
cString numberText(int64 value, bool isHex)
{
    cString text;
    uint64 magnitude = (uint64)value;
    if (value < 0)
    {
        text = cString("-");
        magnitude = (uint64)0 - (uint64)value;
    }
    if (isHex)
        return text + "0x" + hexText(magnitude);
    return text + cString(magnitude);
}

// The text of a shift: ", lsl #2", ", rrx", ", ror #8"; nothing without one
cString shiftText(const Operand& operand)
{
    static const char* const names[] = {
        "", "lsl", "lsr", "asr", "ror", "rrx"
    };
    if (operand.m_shift == Operand::SHIFT_NONE)
        return cString();
    if (operand.m_shift == Operand::SHIFT_RRX)
        return cString(", rrx");
    if (operand.m_shiftRegister != ARM_NO_REGISTER)
        return cString(", ") + names[operand.m_shift] + " " + registerName(operand.m_shiftRegister);
    return cString(", ") + names[operand.m_shift] + " #" + cString((uint32)operand.m_shiftAmount);
}

cString formatMemory(const Operand& operand)
{
    cString text = cString("[") + registerName(operand.m_base);
    const char* sign = operand.m_isSubtract ? "-" : "";
    if (operand.m_addressing == Operand::ADDRESSING_POST_INDEX)
    {
        if (operand.m_alignment != 0)
            text += cString(":") + cString((uint32)operand.m_alignment);
        text += "]";
        if (operand.m_optionScheme == ARM_A32_OPT_COPROC_OPTION)
            return text + ", {" + cString((uint64)operand.m_offset) + "}";
        if (operand.m_postIndexRegister != ARM_NO_REGISTER)
            return text + ", " + sign + registerName(operand.m_postIndexRegister) + shiftText(operand);
        return text + ", #" + sign + numberText(operand.m_offset, operand.m_isHex);
    }
    if (operand.m_alignment != 0)
        text += cString(":") + cString((uint32)operand.m_alignment);
    if (operand.m_index != ARM_NO_REGISTER)
    {
        text += cString(", ") + sign + registerName(operand.m_index) + shiftText(operand);
    } else if (operand.m_isSubtract)
    {
        text += cString(", #-") + numberText(operand.m_offset, operand.m_isHex);
    } else if ((operand.m_offset != 0) || operand.m_isExplicit)
    {
        text += cString(", #") + numberText(operand.m_offset, operand.m_isHex);
    }
    text += "]";
    if (operand.m_addressing == Operand::ADDRESSING_PRE_INDEX)
        text += "!";
    return text;
}

cString formatList(const Operand& operand)
{
    cString text;
    if (operand.m_registerMask == 0)
    {
        // A pair of registers: "r0, r1"
        for (uint i = 0; i < operand.m_count; i++)
        {
            if (i != 0)
                text += ", ";
            text += registerName(operand.m_register + i * ((operand.m_stride == 0) ? 1 : operand.m_stride));
        }
        return text;
    }
    text = cString("{");
    bool isFirst = true;
    for (uint i = 0; i < 16; i++)
        if (((operand.m_registerMask >> i) & 1) != 0)
        {
            if (!isFirst)
                text += ", ";
            text += registerName(ARM_REG_R0 + i);
            isFirst = false;
        }
    text += "}";
    if (operand.m_isExplicit)
        text += " ^";
    return text;
}

} // anonymous namespace

cString formatOperandA32(const ArmDecodedOperand& operand)
{
    switch (operand.m_kind)
    {
    case Operand::KIND_REGISTER:
    {
        cString text = registerName(operand.m_register);
        if (operand.m_elementIndex >= 0)
            text += cString("[") + cString((uint32)operand.m_elementIndex) + "]";
        if (operand.m_addressing == Operand::ADDRESSING_PRE_INDEX)
            text += "!";
        return text;
    }
    case Operand::KIND_REGISTER_LIST:
        if (operand.m_optionScheme == ARM_A32_OPT_VECTOR_LIST)
            return formatSimdOperandA32(operand);
        return formatList(operand);
    case Operand::KIND_IMMEDIATE:
    {
        if (operand.m_isUnsigned)
            return cString("#0x") + hexText((uint64)operand.m_immediate);
        cString text = cString("#") + numberText(operand.m_immediate, operand.m_isHex);
        if (operand.m_isExplicit)
            text += cString(", #") + cString((uint32)operand.m_shiftAmount);
        return text;
    }
    case Operand::KIND_FLOAT_IMMEDIATE:
        return cString("#") + cString(operand.m_name);
    case Operand::KIND_SHIFTED_REGISTER:
        return registerName(operand.m_register) + shiftText(operand);
    case Operand::KIND_MEMORY:
        return formatMemory(operand);
    case Operand::KIND_BRANCH_TARGET:
        return cString("0x") + hexText((uint64)operand.m_immediate & 0xFFFFFFFFu);
    case Operand::KIND_CONDITION:
        return cString(getConditionName(operand.m_condition));
    case Operand::KIND_OPTION:
    case Operand::KIND_NAME:
        return operand.m_name;
    default:
        return cString("?");
    }
}

} // namespace armdis
