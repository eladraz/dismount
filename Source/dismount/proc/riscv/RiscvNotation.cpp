#include "dismount/dismount.h"
/*
 * RiscvNotation.cpp
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
#include "dismount/proc/riscv/RiscvOperandCodec.h"
#include "dismount/proc/riscv/RiscvOperandCodecInternal.h"
#include "dismount/proc/riscv/RiscvNotation.h"

using namespace riscvdis;

namespace {

typedef RiscvDecodedOperand Operand;

// The ABI names of x0-x31 and f0-f31
const char* const gXNames[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3",
    "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
    "t3", "t4", "t5", "t6"
};
const char* const gFNames[32] = {
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0", "fa1",
    "fa2", "fa3", "fa4", "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
    "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"
};
const char* const gRoundingNames[8] = { "rne", "rtz", "rdn", "rup", "rmm", "", "", "dyn" };

// The text of a signed number
cString decimalText(int64 value)
{
    if (value < 0)
        return cString("-") + cString((uint64)0 - (uint64)value);
    return cString((uint64)value);
}

// The text of a number in hexadecimal, "0x1f"
cString hexText(uint64 value)
{
    static const char digits[] = "0123456789abcdef";
    char text[19];
    uint position = sizeof(text);
    text[--position] = 0;
    do
    {
        text[--position] = digits[value & 0xf];
        value >>= 4;
    } while (value != 0);
    text[--position] = 'x';
    text[--position] = '0';
    return cString(&text[position]);
}

// The text of a register id
cString registerText(uint registerId, int formatType)
{
    const char* name = RiscvNotation::getRegisterName(registerId, formatType);
    if (name == NULL)
        return cString("?") + cString((uint32)registerId);
    return cString(name);
}

} // anonymous namespace

const char* RiscvNotation::getRegisterName(uint registerId, int formatType)
{
    // (the numeric names are formatted into a static table)
    static char numeric[32 * 3][5];
    static bool isReady = false;
    if (!isReady)
    {
        for (uint i = 0; i < 32; i++)
        {
            // "x31"
            const char prefixes[3] = { 'x', 'f', 'v' };
            for (uint k = 0; k < 3; k++)
            {
                char* text = numeric[32 * k + i];
                uint length = 0;
                text[length++] = prefixes[k];
                if (i >= 10)
                    text[length++] = (char)('0' + i / 10);
                text[length++] = (char)('0' + i % 10);
                text[length] = 0;
            }
        }
        isReady = true;
    }
    bool isNumeric = (formatType & RISCV_FORMAT_NUMERIC) != 0;
    if (registerId <= RISCV_REG_X31)
        return isNumeric ? numeric[registerId - RISCV_REG_X0] : gXNames[registerId - RISCV_REG_X0];
    if (registerId == RISCV_REG_PC)
        return "pc";
    if ((registerId >= RISCV_REG_F0) && (registerId <= RISCV_REG_F31))
        return isNumeric ? numeric[32 + registerId - RISCV_REG_F0] : gFNames[registerId - RISCV_REG_F0];
    if ((registerId >= RISCV_REG_V0) && (registerId <= RISCV_REG_V31))
        return numeric[64 + registerId - RISCV_REG_V0];
    return NULL;
}

RiscvNotation::RiscvNotation(const OpcodePtr& instruction,
                             OpcodeDataFormatter& dataFormatter,
                             int formatType) :
    m_data(instruction),
    m_opcode(NULL),
    m_dataFormatter(dataFormatter),
    m_formatType(formatType)
{
    CHECK(!m_data.isEmpty());
    switch (m_data->getType())
    {
    case OpcodeSubsystems::DISASSEMBLER_RISCV_32:
    case OpcodeSubsystems::DISASSEMBLER_RISCV_64:
        break;
    default:
        // Unsupported instruction
        CHECK_FAIL();
    }
    m_opcode = static_cast<const RiscvOpcode*>(m_data.getPointer());
}

cString RiscvNotation::formatOperand(const RiscvDecodedOperand& operand,
                                     const RiscvCodecContext& context,
                                     int formatType)
{
    switch (operand.m_kind)
    {
    case Operand::KIND_REGISTER:
        return registerText(operand.m_register, formatType);
    case Operand::KIND_IMMEDIATE:
        if (operand.m_isHex)
            return hexText((uint64)operand.m_immediate);
        if (operand.m_isUnsigned)
            return cString((uint64)operand.m_immediate);
        return decimalText(operand.m_immediate);
    case Operand::KIND_FLOAT_IMMEDIATE:
        return cString(gRiscvFliValues[operand.m_fliIndex].m_text);
    case Operand::KIND_MEMORY:
    {
        cString text;
        if (operand.m_hasOffset)
            text = decimalText(operand.m_offset);
        return text + "(" + registerText(operand.m_base, formatType) + ")";
    }
    case Operand::KIND_BRANCH_TARGET:
        return hexText((uint64)operand.m_immediate);
    case Operand::KIND_CSR:
    {
        const char* name = RiscvOperandCodec::getCsrName(operand.m_csr, context);
        if (name != NULL)
            return cString(name);
        return cString((uint32)operand.m_csr);
    }
    case Operand::KIND_ROUNDING_MODE:
        return cString(gRoundingNames[operand.m_roundingMode & 7]);
    case Operand::KIND_FENCE_SET:
    {
        if (operand.m_fenceSet == 0)
            return cString("0");
        cString text;
        if ((operand.m_fenceSet & 8) != 0)
            text += "i";
        if ((operand.m_fenceSet & 4) != 0)
            text += "o";
        if ((operand.m_fenceSet & 2) != 0)
            text += "r";
        if ((operand.m_fenceSet & 1) != 0)
            text += "w";
        return text;
    }
    case Operand::KIND_VTYPE:
        return formatVtype(operand.m_vtype);
    case Operand::KIND_VECTOR_MASK:
        return cString("v0.t");
    case Operand::KIND_NAME:
        return operand.m_name;
    default:
        CHECK_FAIL();
    }
}

cString RiscvNotation::string(OpcodeFormatStruct* formatStruct) const
{
    // Get the instruction address
    ProcessorAddress ipAddress(gNullPointerProcessorAddress);
    bool shouldUseIp = m_data->getOpcodeAddress(ipAddress);

    // Mark new instruction
    m_dataFormatter.newInstruction(shouldUseIp, ipAddress);

    if (formatStruct != NULL)
        formatStruct->m_opcodeNameStart = 0;

    // The instruction which is printed: the compressed ones print the 32 bit
    // instruction they expand to (and the aliases of it), unless the
    // aliases are off
    const RiscvOpcode* opcode = m_opcode;
    if ((m_formatType & RISCV_FORMAT_NO_ALIASES) != 0)
    {
        if (!opcode->getNoAliasForm().isEmpty())
            opcode = opcode->getNoAliasForm().getPointer();
    } else if (!opcode->getExpansion().isEmpty())
    {
        opcode = opcode->getExpansion().getPointer();
    }

    RiscvCodecContext context;
    context.m_type = opcode->getInstructionSet();
    context.m_features = RiscvFeatureSet::all();
    context.m_address = opcode->getAddress();

    uint count = opcode->getOperandsCount();
    if (count == 0)
    {
        // No operands, no padding
        cString name(opcode->getMnemonic());
        if (formatStruct != NULL)
            formatStruct->m_opcodeOperandsStart = name.length();
        return m_dataFormatter.endInstruction(name);
    }

    cString ret = m_dataFormatter.reparseOpcode(opcode->getMnemonic());
    // The operands are separated from the name, whatever the alignment is
    if ((ret.length() == 0) || (ret[ret.length() - 1] != XSTL_CHAR(' ')))
        ret += " ";
    if (formatStruct != NULL)
        formatStruct->m_opcodeOperandsStart = ret.length();

    for (uint i = 0; i < count; i++)
    {
        if (i != 0)
            ret += m_dataFormatter.getOpcodesSeparator(ret);
        cString text = formatOperand(opcode->getOperand(i), context, m_formatType);
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

uint RiscvNotation::parseOperandAddress(ProcessorAddress& address)
{
    address = ProcessorAddress(gNullPointerProcessorAddress);

    const RiscvOpcode* opcode = m_opcode;
    if (!opcode->getExpansion().isEmpty())
        opcode = opcode->getExpansion().getPointer();
    // The branch target is the first pc relative operand
    for (uint i = 0; i < opcode->getOperandsCount(); i++)
    {
        const Operand& operand = opcode->getOperand(i);
        if (operand.m_kind != Operand::KIND_BRANCH_TARGET)
            continue;
        if (!m_data->getOpcodeAddress(address))
            return 0;
        address = ProcessorAddress(
            (opcode->getInstructionSet() == RiscvInstructionSet::RISCV_RV64) ?
                ProcessorAddress::PROCESSOR_64 : ProcessorAddress::PROCESSOR_32,
            (ProcessorAddress::uintAddress)operand.m_immediate);
        return RISCV_KIND_BRANCH_TARGET + 0x1000;
    }
    return 0;
}
