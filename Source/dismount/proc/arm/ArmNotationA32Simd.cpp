#include "dismount/dismount.h"
/*
 * ArmNotationA32Simd.cpp
 *
 * Implementation file. The formatter of the AArch32 floating point and
 * Advanced SIMD operands that the common formatter doesn't know: the lists
 * of vector registers.
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

// The name of a register of a list. The numbers past d31 are the registers
// that follow d31 in the register enumeration of LLVM, which it prints
cString listRegisterName(const ArmDecodedOperand& operand, uint number)
{
    static const char* const past[] = { "fpinst2", "mvfr0", "mvfr1", "mvfr2" };
    bool isSingle = (operand.m_register >= ARM_REG_S0) && (operand.m_register <= ARM_REG_S31);
    uint base = isSingle ? ARM_REG_S0 : ARM_REG_D0;
    uint limit = isSingle ? 32 : 32;
    if (number >= limit)
        return cString(past[(number - limit) % 4]);
    const char* name = getRegisterName(ArmInstructionSet::ARM_A32, base + number);
    return cString(name);
}

} // anonymous namespace

cString formatSimdOperandA32(const ArmDecodedOperand& operand)
{
    if (operand.m_kind != ArmDecodedOperand::KIND_REGISTER_LIST)
        return cString("?");
    bool isSingle = (operand.m_register >= ARM_REG_S0) && (operand.m_register <= ARM_REG_S31);
    uint first = operand.m_register - (isSingle ? ARM_REG_S0 : ARM_REG_D0);
    uint stride = (operand.m_stride == 0) ? 1 : operand.m_stride;
    cString text("{");
    for (uint i = 0; i < operand.m_count; i++)
    {
        if (i != 0)
            text += ", ";
        uint number = first + i * stride;
        // (the lists of three and four registers wrap around d31)
        if (operand.m_index == 32)
            number %= 32;
        text += listRegisterName(operand, number);
        if (operand.m_elementIndex >= 0)
            text += cString("[") + cString((uint32)operand.m_elementIndex) + "]";
        else if (operand.m_isExplicit)
            text += "[]";
    }
    return text + "}";
}

} // namespace armdis
