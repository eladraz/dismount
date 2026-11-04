#include "dismount/dismount.h"
/*
 * ArmNotation.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"
#include "dismount/proc/arm/ArmOperandCodecA32.h"
#include "dismount/proc/arm/ArmNotation.h"

using namespace armdis;

namespace {

typedef ArmDecodedOperand Operand;
typedef ArmInstructionSet::DisassemblerTypes InstructionSet;

// The suffix of an arrangement, by Operand::Arrangement
const char* const gArrangements[] = {
    "", ".8b", ".16b", ".4h", ".8h", ".2s", ".4s", ".1d", ".2d", ".1q",
    ".b", ".h", ".s", ".d", ".q", ".2h", ".4b", ".2b"
};

const char* const gShifts[] = {
    "", "lsl", "lsr", "asr", "ror", "rrx", "msl",
    "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx",
    "mul vl", "mul"
};

// The registers of a SIMD / SVE group wrap around after the last one
struct RegisterGroup {
    uint m_first;
    uint m_count;
};

const RegisterGroup gA64Groups[] = {
    { ARM64_REG_Z0, 32 }, { ARM64_REG_V0, 32 }, { ARM64_REG_Q0, 32 },
    { ARM64_REG_D0, 32 }, { ARM64_REG_S0, 32 }, { ARM64_REG_H0, 32 },
    { ARM64_REG_B0, 32 }, { ARM64_REG_P0, 16 }
};

// Return the text of a signed number, in hexadecimal ("-0x10") or decimal
cString formatNumber(int64 value, bool isHex)
{
    cString text;
    if (value < 0)
        text = cString("-");
    uint64 magnitude = (value < 0) ? (uint64)0 - (uint64)value : (uint64)value;
    if (isHex)
        return text + "0x" + hexText(magnitude);
    return text + cString(magnitude);
}

// Return the text of an immediate: #16, #0x10, #-0x10, ", lsl #12"
cString formatImmediate(const Operand& operand)
{
    cString text("#");
    if (operand.m_isUnsigned && operand.m_isExplicit)
    {
        // The 64 bit immediate of movi (%#016llx): zero has no prefix
        cString digits = hexText((uint64)operand.m_immediate);
        if (operand.m_immediate == 0)
            return text + cString::dup("0", 16);
        if (digits.length() < 14)
            digits = cString::dup("0", 14 - digits.length()) + digits;
        return text + "0x" + digits;
    }
    if (operand.m_isUnsigned)
        text += cString("0x") + hexText((uint64)operand.m_immediate);
    else
        text += formatNumber(operand.m_immediate, operand.m_isHex);
    if (operand.m_shift != Operand::SHIFT_NONE)
    {
        text += cString(", ") + gShifts[operand.m_shift];
        if (operand.m_shift != Operand::SHIFT_MUL_VL)
            text += cString(" #") + cString((uint32)operand.m_shiftAmount);
    }
    return text;
}

// Return the text of a floating point with 8 decimal digits ("1.50000000").
// The values of the 8 bit immediates are exact in this format
cString formatFloat(double value)
{
    cString text;
    if (value < 0)
    {
        text = cString("-");
        value = -value;
    }
    uint64 whole = (uint64)value;
    uint64 fraction = (uint64)((value - (double)whole) * 100000000.0 + 0.5);
    if (fraction >= 100000000)
    {
        whole++;
        fraction -= 100000000;
    }
    cString digits(fraction);
    return text + cString(whole) + "." + cString::dup("0", 8 - digits.length()) + digits;
}

// Return the register name, with a fallback for the ids which have none
cString formatRegisterName(InstructionSet type, uint registerId)
{
    const char* name = getRegisterName(type, registerId);
    if (name == NULL)
        return cString("?") + cString((uint32)registerId);
    return cString(name);
}

// Return the text of a register: name, arrangement, element and predicate
cString formatRegister(InstructionSet type, const Operand& operand)
{
    cString text = formatRegisterName(type, operand.m_register);
    if (operand.m_isPredicateCounter)
        text = cString("pn") + text.part(1, text.length());
    text += gArrangements[operand.m_arrangement];
    if (operand.m_elementIndex >= 0)
        text += cString("[") + cString((uint32)operand.m_elementIndex) + "]";
    if (operand.m_predicateQualifier == Operand::PREDICATE_MERGING)
        text += "/m";
    else if (operand.m_predicateQualifier == Operand::PREDICATE_ZEROING)
        text += "/z";
    if (operand.m_addressing == Operand::ADDRESSING_PRE_INDEX)
        text += "!";
    return text;
}

// Return the register which is 'distance' registers after the first
uint getListRegister(uint first, uint distance)
{
    for (uint i = 0; i < sizeof(gA64Groups) / sizeof(gA64Groups[0]); i++)
        if ((first >= gA64Groups[i].m_first) &&
            (first < gA64Groups[i].m_first + gA64Groups[i].m_count))
            return gA64Groups[i].m_first +
                   ((first - gA64Groups[i].m_first + distance) % gA64Groups[i].m_count);
    return first + distance;
}

// True if the id is a general purpose register of A64
bool isA64GeneralRegister(uint registerId)
{
    return registerId <= ARM64_REG_WZR;
}

cString formatList(InstructionSet type, const Operand& operand)
{
    // The mask of the AArch32 general purpose registers: {r0, r1, lr}
    if (type != ArmInstructionSet::ARM_A64)
    {
        cString text("{");
        bool isFirst = true;
        for (uint i = 0; i < 16; i++)
            if (((operand.m_registerMask >> i) & 1) != 0)
            {
                if (!isFirst)
                    text += ", ";
                text += formatRegisterName(type, ARM_REG_R0 + i);
                isFirst = false;
            }
        return text + "}";
    }

    uint stride = (operand.m_stride == 0) ? 1 : operand.m_stride;
    bool isGeneral = isA64GeneralRegister(operand.m_register);
    // The pair of general purpose registers has no braces (casp x0, x1, ...)
    cString text = isGeneral ? cString() : cString("{ ");
    for (uint i = 0; i < operand.m_count; i++)
    {
        if (i != 0)
            text += ", ";
        uint registerId = isGeneral ?
            (operand.m_register + i * stride) :
            getListRegister(operand.m_register, i * stride);
        // The register after w30 / x30 is the zero register
        if (isGeneral && (i != 0))
        {
            if ((operand.m_register == ARM64_REG_XZR) || (operand.m_register == ARM64_REG_WZR) ||
                (registerId == ARM64_REG_SP) || (registerId == ARM64_REG_WSP))
                registerId = ((operand.m_register >= ARM64_REG_W0) ||
                              (operand.m_register == ARM64_REG_WZR)) ? ARM64_REG_WZR : ARM64_REG_XZR;
        }
        Operand single;
        single.m_register = registerId;
        single.m_arrangement = operand.m_arrangement;
        single.m_isPredicateCounter = operand.m_isPredicateCounter;
        text += formatRegister(type, single);
    }
    if (!isGeneral)
    {
        text += " }";
        if (operand.m_elementIndex >= 0)
            text += cString("[") + cString((uint32)operand.m_elementIndex) + "]";
    }
    return text;
}

// Return the text of the shift or extend of a register: ", lsl #3"
cString formatShift(const Operand& operand)
{
    if (operand.m_shift == Operand::SHIFT_NONE)
        return cString();
    cString text = cString(", ") + gShifts[operand.m_shift];
    bool isExtend = (operand.m_shift >= Operand::EXTEND_UXTB) &&
                    (operand.m_shift <= Operand::EXTEND_SXTX);
    if (operand.m_shift == Operand::SHIFT_RRX)
        return text;
    // lsl #0 is not written, the extends don't write a zero amount
    if (((operand.m_shift == Operand::SHIFT_LSL) || isExtend) && (operand.m_shiftAmount == 0))
        return (operand.m_shift == Operand::SHIFT_LSL) ? cString() : text;
    return text + " #" + cString((uint32)operand.m_shiftAmount);
}

cString formatMemory(InstructionSet type, const Operand& operand)
{
    cString text = cString("[") + formatRegisterName(type, operand.m_base);
    if (operand.m_index != ARM_NO_REGISTER)
    {
        // [x1, x2, lsl #3], [x1, w2, uxtw #2]: the amount is written when
        // the S bit is set
        text += cString(", ") + formatRegisterName(type, operand.m_index);
        text += gArrangements[operand.m_indexArrangement];
        if (operand.m_shift != Operand::SHIFT_NONE)
        {
            bool isLsl = (operand.m_shift == Operand::SHIFT_LSL);
            if (!isLsl || operand.m_isExplicit)
                text += cString(", ") + gShifts[operand.m_shift];
            if (operand.m_isExplicit)
                text += cString(" #") + cString((uint32)operand.m_shiftAmount);
        }
        return text + "]";
    }

    switch (operand.m_addressing)
    {
    case Operand::ADDRESSING_POST_INDEX:
        text += "]";
        if (operand.m_postIndexRegister != ARM_NO_REGISTER)
            return text + ", " + formatRegisterName(type, operand.m_postIndexRegister);
        return text + ", #" + formatNumber(operand.m_offset, operand.m_isHex);
    case Operand::ADDRESSING_PRE_INDEX:
        // The memory instructions write the offset, even a zero one (the
        // memory copy and set instructions have none)
        if (operand.m_isHex || (operand.m_offset != 0))
            text += cString(", #") + formatNumber(operand.m_offset, operand.m_isHex);
        return text + "]!";
    default:
        if (operand.m_offset != 0)
            text += cString(", #") + formatNumber(operand.m_offset, true);
        return text + "]";
    }
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////

ArmNotation::ArmNotation(const OpcodePtr& instruction,
                         OpcodeDataFormatter& dataFormatter) :
    m_data(instruction),
    m_opcode(NULL),
    m_dataFormatter(dataFormatter)
{
    CHECK(!m_data.isEmpty());
    switch (m_data->getType())
    {
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_BE:
    case OpcodeSubsystems::DISASSEMBLER_THUMB_LE:
    case OpcodeSubsystems::DISASSEMBLER_THUMB_BE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_BE:
        break;
    default:
        // Unsupported instruction
        CHECK_FAIL();
    }
    m_opcode = static_cast<const ArmOpcode*>(m_data.getPointer());
}

cString ArmNotation::string(OpcodeFormatStruct* formatStruct) const
{
    // Get the instruction address
    ProcessorAddress ipAddress(gNullPointerProcessorAddress);
    bool shouldUseIp = m_data->getOpcodeAddress(ipAddress);

    // Mark new instruction
    m_dataFormatter.newInstruction(shouldUseIp, ipAddress);

    if (formatStruct != NULL)
        formatStruct->m_opcodeNameStart = 0;

    uint count = m_opcode->getOperandsCount();
    if (count == 0)
    {
        // No operands, no padding
        cString name(m_opcode->getMnemonic());
        if (formatStruct != NULL)
            formatStruct->m_opcodeOperandsStart = name.length();
        return m_dataFormatter.endInstruction(name);
    }

    cString ret = m_dataFormatter.reparseOpcode(m_opcode->getMnemonic());
    // The operands are separated from the name, whatever the alignment is
    if ((ret.length() == 0) || (ret[ret.length() - 1] != XSTL_CHAR(' ')))
        ret += " ";
    if (formatStruct != NULL)
        formatStruct->m_opcodeOperandsStart = ret.length();

    for (uint i = 0; i < count; i++)
    {
        if (i != 0)
            ret += m_dataFormatter.getOpcodesSeparator(ret);
        cString text = formatOperand(m_opcode->getInstructionSet(),
                                     m_opcode->getOperand(i));
        switch (i)
        {
        case 0:
            text = m_dataFormatter.reparseFirstOperand(text);
            break;
        case 1:
            text = m_dataFormatter.reparseSecondOperand(text);
            break;
        case 2:
            text = m_dataFormatter.reparseThirdOperand(text);
            break;
        }
        ret += text;
    }
    return m_dataFormatter.endInstruction(ret);
}

uint ArmNotation::parseOperandAddress(ProcessorAddress& address)
{
    address = ProcessorAddress(gNullPointerProcessorAddress);

    // The branch target is the first pc relative operand
    for (uint i = 0; i < m_opcode->getOperandsCount(); i++)
    {
        const Operand& operand = m_opcode->getOperand(i);
        if (operand.m_kind != Operand::KIND_BRANCH_TARGET)
            continue;
        if (!m_data->getOpcodeAddress(address))
            return 0;
        address = ProcessorAddress(
            (m_opcode->getInstructionSet() == ArmInstructionSet::ARM_A64) ?
                ProcessorAddress::PROCESSOR_64 : ProcessorAddress::PROCESSOR_32,
            (ProcessorAddress::uintAddress)operand.m_immediate);
        return ARM_KIND_BRANCH_TARGET + 0x1000;
    }
    return 0;
}

cString ArmNotation::formatOperand(ArmInstructionSet::DisassemblerTypes type,
                                   const ArmDecodedOperand& operand)
{
    if (type != ArmInstructionSet::ARM_A64)
        return formatOperandA32(operand);
    switch (operand.m_kind)
    {
    case Operand::KIND_REGISTER:
        return formatRegister(type, operand);
    case Operand::KIND_REGISTER_LIST:
        return formatList(type, operand);
    case Operand::KIND_IMMEDIATE:
        return formatImmediate(operand);
    case Operand::KIND_FLOAT_IMMEDIATE:
        // The explicit zero of fcmp / fcmeq is written "#0.0"
        if (operand.m_isExplicit)
            return cString("#0.0");
        return cString("#") + formatFloat(operand.m_floatImmediate);
    case Operand::KIND_SHIFTED_REGISTER:
    {
        cString text = formatRegisterName(type, operand.m_register);
        if (operand.m_shift == Operand::SHIFT_NONE)
            return text;
        if (operand.m_shiftRegister != ARM_NO_REGISTER)
            return text + ", " + gShifts[operand.m_shift] + " " +
                   formatRegisterName(type, operand.m_shiftRegister);
        return text + formatShift(operand);
    }
    case Operand::KIND_MEMORY:
        return formatMemory(type, operand);
    case Operand::KIND_BRANCH_TARGET:
        return cString("0x") + hexText((uint64)operand.m_immediate);
    case Operand::KIND_CONDITION:
        return cString(getConditionName(operand.m_condition));
    case Operand::KIND_SYSTEM_REGISTER:
    {
        if (operand.m_name.length() != 0)
            return operand.m_name;
        // S<op0>_<op1>_C<n>_C<m>_<op2>
        uint32 packed = operand.m_systemRegister;
        return cString("S") + cString((uint32)(packed >> 14)) + "_" +
               cString((uint32)((packed >> 11) & 7)) + "_C" +
               cString((uint32)((packed >> 7) & 15)) + "_C" +
               cString((uint32)((packed >> 3) & 15)) + "_" +
               cString((uint32)(packed & 7));
    }
    case Operand::KIND_OPTION:
    {
        if (operand.m_optionScheme == ARM_OPT_CREG)
            return cString("c") + cString((uint32)operand.m_option);
        const char* name = getOptionName(operand.m_optionScheme, operand.m_option);
        if (name != NULL)
            return cString(name);
        return cString("#0x") + hexText(operand.m_option);
    }
    case Operand::KIND_NAME:
        return operand.m_name;
    default:
        // TODO: the matrix operands of SME
        return cString("?");
    }
}
