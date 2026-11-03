#include "dismount/dismount.h"
/*
 * IA32IntelNotation.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/proc/ia32/IA32IntelNotation.h"

/*
 * Return true for the opcode names which take a size suffix when the operand
 * size is not the default one (nasm: retw, iretd, pushfw, ...)
 */
static bool isSizeSuffixed(const cString& name)
{
    static const char* names[] = {"ret", "retf", "iret", "leave", "enter",
                                  "pushf#d", "popf#d", "pusha#d", "popa#d"};
    for (uint i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (name == names[i])
            return true;
    return false;
}

/*
 * Return true for the string instructions, which address the memory with
 * rsi / rdi / rbx and have no operands
 */
static bool isStringInstruction(const cString& name)
{
    static const char* names[] = {"movsb", "movs##", "cmpsb", "cmps##", "stosb",
                                  "stos##", "lodsb", "lods##", "scasb", "scas##",
                                  "insb", "ins##", "outsb", "outs##", "xlatb"};
    for (uint i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (name == names[i])
            return true;
    return false;
}

IA32IntelNotation::IA32IntelNotation(const OpcodePtr& instruction,
                                     OpcodeDataFormatter& dataFormatter) :
    m_dataFormatter(dataFormatter),
    m_data(instruction),
    m_opcode(NULL)
{
    switch (m_data->getType())
    {
    case OpcodeSubsystems::DISASSEMBLER_INTEL_16:
    case OpcodeSubsystems::DISASSEMBLER_INTEL_32:
    case OpcodeSubsystems::DISASSEMBLER_AMD_64:
        break;
    default:
        // Unsupported instruction
        CHECK_FAIL();
    }

    // Cast opcode
    m_opcode = (IA32Opcode*)m_data.getPointer();
}

cString IA32IntelNotation::string(OpcodeFormatStruct* formatStruct) const
{
    // Get the instruction address
    ProcessorAddress ipAddress(gNullPointerProcessorAddress);
    bool shouldUseIp = m_data->getOpcodeAddress(ipAddress);

    // Mark new instruction
    m_dataFormatter.newInstruction(shouldUseIp, ipAddress);

    // Add all the prepost-prefix name
    cString ret;

    // Add all the prepost-prefix name
    for (uint i = 0; i < m_opcode->m_prefixsCount; i++)
        for (uint j = 0; j < ia32dis::IA32_NUMBER_OF_PREFIXS; j++)
            if ((m_opcode->m_prefixs[i] == ia32dis::gIa32PrefixTable[j].m_opcode) &&
                (!ia32dis::gIa32PrefixTable[j].m_isSegmentSelector) &&
                (ia32dis::gIa32PrefixTable[j].m_isOpcodeNameValid))
            {
                ret+= ia32dis::gIa32PrefixTable[j].m_prefixName;
                ret+= " ";
            }

    // The segment override of a string instruction (movs, lods...) has no
    // memory operand to attach to, so it's written as a prefix: "fs movsb"
    if (m_opcode->m_opcode->m_firstOperand == ia32dis::OPND_NO_OPERAND)
    {
        cString segment = getSegmentSelector();
        if (segment.length() != 0)
            ret+= segment.left(segment.length() - 1) + " ";
    }

    // The 32 bit address of a string instruction in 64 bit mode has no
    // register to take the size from: "a32 movsb"
    if (is64bit() && (m_opcode->m_addressSize == IntegerEncoding::INTEGER_32BIT) &&
        isStringInstruction(m_opcode->m_opcode->m_opcodeName))
        ret+= "a32 ";

    if (formatStruct != NULL)
        formatStruct->m_opcodeNameStart = ret.length();

    // Add the opcode name
    cString opcodeName = m_opcode->m_opcode->m_opcodeName;
    // The name conventions follow the operand size ('/', '#d' and '##') and
    // the address size ('#e')
    const uint operandBits = getOperandBits();
    const uint addressBits = getAddressBits();
    // Replace name convenstion. The parts are the names of the 16, 32 and 64
    // bit operand size (the 64 bit name is optional)
    if (opcodeName.find("/") < opcodeName.length())
    {
        cString first = opcodeName.left(opcodeName.find("/"));
        cString rest = opcodeName.part(opcodeName.find("/") + 1,
                                       opcodeName.length());
        cString second = rest;
        cString third;
        if (rest.find("/") < rest.length())
        {
            second = rest.left(rest.find("/"));
            third = rest.part(rest.find("/") + 1, rest.length());
        }
        if (operandBits == 16)
            opcodeName = first;
        else if ((operandBits == 64) && (third.length() != 0))
            opcodeName = third;
        else
            opcodeName = second;
    }

    uint namePos = opcodeName.find("#");
    while (namePos != opcodeName.length()) {
        switch (opcodeName[namePos + 1])
        {
        case XSTL_CHAR('d'):
            opcodeName = opcodeName.left(namePos) +
                ((operandBits == 64) ? "q" : (operandBits == 32) ? "d" : "") +
                opcodeName.part(namePos + 2, opcodeName.length());
            break;
        case XSTL_CHAR('e'):
            opcodeName = opcodeName.left(namePos) +
                ((addressBits == 64) ? "r" : (addressBits == 32) ? "e" : "") +
                opcodeName.part(namePos + 2, opcodeName.length());
            break;
        case XSTL_CHAR('#'):
            opcodeName = opcodeName.left(namePos) +
                ((operandBits == 64) ? "q" : (operandBits == 32) ? "d" : "w") +
                opcodeName.part(namePos + 2, opcodeName.length());
            break;
        }
        namePos = opcodeName.find("#");
    }

    // The stack frame and flags instructions with an operand size which is
    // not the default one are written with the size suffix (66 C3 is 'retw').
    // The '#d' names already end with 'd' or 'q' for 32 and 64 bit.
    cString tableName(m_opcode->m_opcode->m_opcodeName);
    if ((operandBits != getDefaultOperandBits()) && isSizeSuffixed(tableName) &&
        ((operandBits == 16) || (tableName.find("#d") == tableName.length())))
        opcodeName+= (operandBits == 64) ? "q" : (operandBits == 32) ? "d" : "w";

    ret+= m_dataFormatter.reparseOpcode(opcodeName);

    if (formatStruct != NULL)
        formatStruct->m_opcodeOperandsStart = ret.length();

    // Start adding operands
    // Add operands
    if (m_opcode->m_opcode->m_firstOperand == ia32dis::OPND_NO_OPERAND)
    {
        ASSERT(m_opcode->m_opcode->m_secondOperand == ia32dis::OPND_NO_OPERAND);
        ASSERT(m_opcode->m_opcode->m_thridOperand == ia32dis::OPND_NO_OPERAND);
        return m_dataFormatter.endInstruction(ret);
    }

    ret+= m_dataFormatter.reparseFirstOperand(
                    stringOperand(m_opcode->m_opcode->m_firstOperand));

    // D8 C0 is written 'fadd st0' (nasm assembles 'fadd st0, st0' as DC C0)
    bool isSt0St0 = (m_opcode->m_opcode->m_prefix == 0xD8) &&
                    (m_opcode->m_opcode->m_secondOperand == ia32dis::OPND_FPU_ST_RM) &&
                    (m_opcode->m_modrm.m_bits.m_rm == 0);

    if ((m_opcode->m_opcode->m_secondOperand == ia32dis::OPND_NO_OPERAND) || isSt0St0)
    {
        ASSERT(m_opcode->m_opcode->m_thridOperand == ia32dis::OPND_NO_OPERAND);
        // loop/loopz/loopnz/jecxz (E0-E3) with an address size which is not the
        // default one count with cx or ecx. In 64 bit mode the jrcxz / jecxz
        // name has the register
        const uint defaultAddressBits = is64bit() ? 64 : (is32bit() ? 32 : 16);
        if ((m_opcode->m_opcode->m_firstOperand == ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8) &&
            (m_opcode->m_opcode->m_prefix >= 0xE0) &&
            (m_opcode->m_opcode->m_prefix <= (is64bit() ? 0xE2 : 0xE3)) &&
            (addressBits != defaultAddressBits))
            ret+= m_dataFormatter.getOpcodesSeparator(ret) +
                  getGeneralRegister(ia32dis::IA32_GP32_ECX, addressBits);
        return m_dataFormatter.endInstruction(ret);
    }

    // Pad with space and first operand
    ret+= m_dataFormatter.getOpcodesSeparator(ret);
    ret+= m_dataFormatter.reparseSecondOperand(
                    stringOperand(m_opcode->m_opcode->m_secondOperand));

    if (m_opcode->m_opcode->m_thridOperand == ia32dis::OPND_NO_OPERAND)
        return m_dataFormatter.endInstruction(ret);

    ret+= m_dataFormatter.getOpcodesSeparator(ret);
    ret+= m_dataFormatter.reparseThirdOperand(
                    stringOperand(m_opcode->m_opcode->m_thridOperand));

    // Build the instruction
    return m_dataFormatter.endInstruction(ret);
}

uint IA32IntelNotation::parseOperandAddress(ProcessorAddress& address)
{
    address = ProcessorAddress(gNullPointerProcessorAddress);

    if (is64bit())
    {
        // The branch targets and the absolute / rip relative addresses are
        // 64 bit addresses
        const uint length = m_opcode->m_opcodeData.getSize();
        switch (m_opcode->m_opcode->m_firstOperand)
        {
        case ia32dis::OPND_MODRM_dWORDPTR:
        {
            const uint mod = m_opcode->m_modrm.m_bits.m_mod;
            const uint rm = m_opcode->m_modrm.m_bits.m_rm;
            if (mod != 0)
                return ia32dis::OPND_NO_OPERAND;
            int64 displacement = (int32)m_opcode->m_displacement;
            if (rm == ia32dis::IA32_GP32_EBP)
            {
                // [rip + disp32]
                if (!m_opcode->m_shouldUseAddress)
                    return ia32dis::OPND_NO_OPERAND;
                address = ProcessorAddress(ProcessorAddress::PROCESSOR_64,
                                           m_opcode->m_opcodeAddress.getAddress() +
                                           length + displacement);
            } else if ((rm == ia32dis::IA32_GP32_ESP) &&
                       (m_opcode->m_sib.m_bits.m_base == ia32dis::IA32_GP32_EBP) &&
                       (m_opcode->m_sib.m_bits.m_index == ia32dis::IA32_GP32_ESP) &&
                       (m_opcode->m_rex.m_bits.x == 0))
            {
                // [disp32]
                address = ProcessorAddress(ProcessorAddress::PROCESSOR_64,
                                           (uint64)displacement);
            } else
            {
                return ia32dis::OPND_NO_OPERAND;
            }
            break;
        }
        case ia32dis::OPND_MEMREF_OFFSET_DS:
            address = ProcessorAddress(
                (m_opcode->m_addressSize == IntegerEncoding::INTEGER_64BIT) ?
                    ProcessorAddress::PROCESSOR_64 : ProcessorAddress::PROCESSOR_32,
                m_opcode->m_immediate.offset);
            break;
        case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
        case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
        case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
            if (!m_opcode->m_shouldUseAddress)
                return ia32dis::OPND_NO_OPERAND;
            address = ProcessorAddress(ProcessorAddress::PROCESSOR_64,
                (uint64)((m_opcode->m_opcode->m_firstOperand ==
                          ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8) ?
                         (int64)(int8)m_opcode->m_immediate.offset :
                         (int64)(int32)m_opcode->m_immediate.offset) +
                length + m_opcode->m_opcodeAddress.getAddress());
            break;
        default:
            return ia32dis::OPND_NO_OPERAND;
        }
        return m_opcode->m_opcode->m_firstOperand;
    }

    switch (m_opcode->m_opcode->m_firstOperand)
    {
    case ia32dis::OPND_MODRM_dWORDPTR:
        // Verify the operand is only a displacement
        if ((0 == m_opcode->m_modrm.m_bits.m_mod) &&
            ((ia32dis::IA32_GP32_EBP == m_opcode->m_modrm.m_bits.m_rm) ||
                ((ia32dis::IA32_GP32_ESP == m_opcode->m_modrm.m_bits.m_rm) &&
                 (ia32dis::IA32_GP32_ESP == m_opcode->m_sib.m_bits.m_index) &&
                 (ia32dis::IA32_GP32_EBP == m_opcode->m_sib.m_bits.m_base))))
                address = ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                           m_opcode->m_displacement);
        else
            return ia32dis::OPND_NO_OPERAND;
        break;

    case ia32dis::OPND_MEMREF_OFFSET_DS:
        switch (m_opcode->m_addressSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            address = ProcessorAddress(ProcessorAddress::PROCESSOR_16,
                                       m_opcode->m_immediate.offset);
            break;
        case IntegerEncoding::INTEGER_32BIT:
            address = ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                       m_opcode->m_immediate.offset);
            break;
        default:
            return ia32dis::OPND_NO_OPERAND;
        }
        break;

    case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
        if (m_opcode->m_shouldUseAddress)
            address = ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                       (int32)m_opcode->m_immediate.offset +
                                       m_opcode->m_opcodeData.getSize() +
                                       m_opcode->m_opcodeAddress.getAddress());
        else
            return ia32dis::OPND_NO_OPERAND;
        break;

    case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
        if (m_opcode->m_shouldUseAddress)
            address = ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                       (int8)m_opcode->m_immediate.offset +
                                       m_opcode->m_opcodeData.getSize() +
                                       m_opcode->m_opcodeAddress.getAddress());
        else
            return ia32dis::OPND_NO_OPERAND;
        break;

    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            if (m_opcode->m_shouldUseAddress)
                address = ProcessorAddress(ProcessorAddress::PROCESSOR_16,
                                           (int16)m_opcode->m_immediate.offset +
                                           m_opcode->m_opcodeData.getSize() +
                                           m_opcode->m_opcodeAddress.getAddress());
            else
                return ia32dis::OPND_NO_OPERAND;
            break;

        case IntegerEncoding::INTEGER_32BIT:
            if (m_opcode->m_shouldUseAddress)
                address = ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                           (int32)m_opcode->m_immediate.offset +
                                           m_opcode->m_opcodeData.getSize() +
                                           m_opcode->m_opcodeAddress.getAddress());
            else
                return ia32dis::OPND_NO_OPERAND;
            break;

        default:
            CHECK_FAIL();
        }
        break;
    default:
        // Not ready yet!!
        return ia32dis::OPND_NO_OPERAND;
    }

    return m_opcode->m_opcode->m_firstOperand;
}

bool IA32IntelNotation::is32bit() const
{
    return m_opcode->m_type == IA32eInstructionSet::INTEL_32;
}

bool IA32IntelNotation::is64bit() const
{
    return m_opcode->m_type == IA32eInstructionSet::AMD_64;
}

uint IA32IntelNotation::getDefaultOperandBits() const
{
    if (is64bit())
    {
        // The opcodes which default to the 64 bit operand size
        if ((m_opcode->m_opcode->m_mode64 == ia32dis::MODE64_DEFAULT_64) ||
            (m_opcode->m_opcode->m_mode64 == ia32dis::MODE64_FORCE_64))
            return 64;
        return 32;
    }
    return is32bit() ? 32 : 16;
}

cString IA32IntelNotation::getOperandSizeKeyword() const
{
    if (getOperandBits() == getDefaultOperandBits())
        return cString();
    switch (getOperandBits())
    {
    case 16: return "word ";
    case 32: return "dword ";
    default: return "qword ";
    }
}

uint IA32IntelNotation::getOperandBits() const
{
    switch (m_opcode->m_operandSize)
    {
    case IntegerEncoding::INTEGER_16BIT: return 16;
    case IntegerEncoding::INTEGER_32BIT: return 32;
    case IntegerEncoding::INTEGER_64BIT: return 64;
    default:
        CHECK_FAIL();
    }
}

uint IA32IntelNotation::getAddressBits() const
{
    switch (m_opcode->m_addressSize)
    {
    case IntegerEncoding::INTEGER_16BIT: return 16;
    case IntegerEncoding::INTEGER_32BIT: return 32;
    case IntegerEncoding::INTEGER_64BIT: return 64;
    default:
        CHECK_FAIL();
    }
}

cString IA32IntelNotation::getGeneralRegister(uint number, uint bits) const
{
    if (!is64bit())
    {
        CHECK(number < ia32dis::NUMBER_OF_REGISTERS);
        switch (bits)
        {
        case 8:  return ia32dis::gIa8Registers[number].m_name;
        case 16: return ia32dis::gIa16Registers[number].m_name;
        case 32: return ia32dis::gIa32Registers[number].m_name;
        default:
            CHECK_FAIL();
        }
    }

    CHECK(number < ia32dis::AMD64_NUMBER_OF_REGISTERS);
    switch (bits)
    {
    case 8:
        // Without a REX prefix 4..7 are ah, ch, dh, bh
        if ((number < ia32dis::NUMBER_OF_REGISTERS) && !m_opcode->hasRex())
            return ia32dis::gIa8Registers[number].m_name;
        return ia32dis::gAmd64Registers8[number].m_name;
    case 16: return ia32dis::gAmd64Registers16[number].m_name;
    case 32: return ia32dis::gAmd64Registers32[number].m_name;
    case 64: return ia32dis::gAmd64Registers[number].m_name;
    default:
        CHECK_FAIL();
    }
}

cString IA32IntelNotation::getSegmentSelector() const
{
    cString ret;
    // Add the segment selector. When the segment prefix is repeated the last
    // one is used (as the processors do).
    for (uint i = 0; i < m_opcode->m_prefixsCount; i++)
        for (uint j = 0; j < ia32dis::IA32_NUMBER_OF_PREFIXS; j++)
            if ((m_opcode->m_prefixs[i] == ia32dis::gIa32PrefixTable[j].m_opcode) &&
                (ia32dis::gIa32PrefixTable[j].m_isSegmentSelector))
            {
                ret = ia32dis::gIa32PrefixTable[j].m_prefixName;
            }

    return ret;
}

cString IA32IntelNotation::getModrmString64(ia32dis::OperandType type) const
{
    const uint mod = m_opcode->m_modrm.m_bits.m_mod;
    const uint rm = m_opcode->m_modrm.m_bits.m_rm;
    const uint rmExtended = rm | (m_opcode->m_rex.m_bits.b << 3);
    const uint operandBits = getOperandBits();

    // Direct register access (11)
    if (mod == 3)
    {
        switch (type)
        {
        case ia32dis::OPND_MODRM_dWORDPTR:
            return getGeneralRegister(rmExtended, operandBits);
        case ia32dis::OPND_MODRM_FIXED_DWORDPTR:
            return getGeneralRegister(rmExtended, 32);
        case ia32dis::OPND_MODRM_WORDPTR:
            // mov r32, sreg writes the whole register
            if (m_opcode->m_opcode->m_secondOperand == ia32dis::OPND_GP_SEGMENT_MODRM)
                return getGeneralRegister(rmExtended, operandBits);
            return getGeneralRegister(rmExtended, 16);
        case ia32dis::OPND_MODRM_BYTEPTR:
            return getGeneralRegister(rmExtended, 8);
        default:
            // lea, cmpxchg16b... with a register: not a valid instruction,
            // written as the register of the operand size (as in 32 bit mode)
            return getGeneralRegister(rmExtended, operandBits);
        }
    }

    // The size of the memory access
    cString ret;
    switch (type)
    {
    case ia32dis::OPND_MODRM_dWORDPTR:
        ret+= (operandBits == 64) ? "qword ptr " :
              (operandBits == 32) ? "dword ptr " : "word ptr ";
        break;
    case ia32dis::OPND_MODRM_FIXED_DWORDPTR: ret+= "dword ptr "; break;
    case ia32dis::OPND_MODRM_WORDPTR:        ret+= "word ptr ";  break;
    case ia32dis::OPND_MODRM_BYTEPTR:        ret+= "byte ptr ";  break;
    case ia32dis::OPND_MODRM_QWORDPTR:       ret+= "qword ptr "; break;
    case ia32dis::OPND_MODRM_TWORDPTR:       ret+= "tword ptr "; break;
    case ia32dis::OPND_MODRM_DOUBLE_MEM:
        ret+= (operandBits == 64) ? "oword ptr " : "qword ptr ";
        break;
    default:
        break;
    }

    ret+= getSegmentSelector();
    ret+= "[";

    const uint addressBits = getAddressBits();
    // A 32 bit address is written with the a32 keyword when it has no
    // register (nasm takes the address size from the registers)
    const char* addressKeyword = (addressBits == 32) ? "a32 " : "";
    cString body;
    // True if the address has a register
    bool hasRegisters = false;
    // The displacement is relative to the next instruction (rip)
    bool ripRelative = false;

    if (rm == ia32dis::IA32_GP32_ESP)
    {
        const IA32OpcodeDatastruct::SIB& sib = m_opcode->m_sib;
        const uint base = sib.m_bits.m_base | (m_opcode->m_rex.m_bits.b << 3);
        const uint index = sib.m_bits.m_index | (m_opcode->m_rex.m_bits.x << 3);
        // Mod 0 with the base 5 has no base (a disp32 is used)
        bool hasBase = (mod != 0) || (sib.m_bits.m_base != ia32dis::IA32_GP32_EBP);
        // The index 4 (rsp, but not r12) means no index
        bool hasIndex = (index != ia32dis::AMD64_GP64_RSP);
        if (hasBase)
            body+= getGeneralRegister(base, addressBits);
        if (hasIndex)
        {
            if (hasBase)
                body+= "+";
            body+= getGeneralRegister(index, addressBits);
            switch (sib.m_bits.m_scale)
            {
            // With an rbp or r13 base the index is written with its scale: an
            // assembler swaps [rbp+rax] into the shorter [rax+rbp]
            case 0: if (hasBase && (sib.m_bits.m_base == ia32dis::IA32_GP32_EBP))
                        body+= "*1";
                    break;
            case 1: body+= "*2"; break;
            case 2: body+= "*4"; break;
            case 3: body+= "*8"; break;
            }
        }
        hasRegisters = hasBase || hasIndex;
    } else if ((mod == 0) && (rm == ia32dis::IA32_GP32_EBP))
    {
        ripRelative = true;
    } else
    {
        body+= getGeneralRegister(rmExtended, addressBits);
        hasRegisters = true;
    }

    // The sign extended displacement
    int64 displacement = 0;
    switch (m_opcode->m_displacementLength)
    {
    case 0: break;
    case 1: displacement = (int8)(m_opcode->m_displacement); break;
    case 4: displacement = (int32)(m_opcode->m_displacement); break;
    default:
        CHECK_FAIL();
    }

    if (ripRelative)
    {
        // nasm writes the address relative to the rip with [rel $+N], where
        // $ is the address of the instruction
        body = cString("rel ") + m_dataFormatter.translateRelativeAddress(
                    (ProcessorAddress::intAddress)displacement +
                        m_opcode->m_opcodeData.getSize(),
                    m_opcode->m_shouldUseAddress,
                    m_opcode->m_opcodeAddress);
        ret+= addressKeyword;
    } else if (!hasRegisters)
    {
        // [disp32]: an absolute address
        ret+= addressKeyword;
        if (addressBits == 32)
            body = m_dataFormatter.translateAbsoluteAddress(
                        ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                         (uint32)displacement));
        else
            body = m_dataFormatter.translateAbsoluteAddress(
                        ProcessorAddress(ProcessorAddress::PROCESSOR_64,
                                         (uint64)displacement));
    } else if (m_opcode->m_displacementLength > 0)
    {
        body+= m_dataFormatter.translateRelativeDisplacement(displacement);
    }

    return ret + body + "]";
}

cString IA32IntelNotation::getModrmString(ia32dis::OperandType type) const
{
    if (is64bit())
        return getModrmString64(type);

    cString ret;
    uint reg = m_opcode->m_modrm.m_bits.m_rm;
    uint mod = m_opcode->m_modrm.m_bits.m_mod;

    ia32dis::ModRMTranslationType* modrmTranslator = NULL;
    ia32dis::RegisterDescription* registersDescriptor = NULL;

    // Load appropriate MOD/RM table. Determined by address size attribute.
    switch (m_opcode->m_addressSize)
    {
    case IntegerEncoding::INTEGER_16BIT:
        // 16 bit
        modrmTranslator = &ia32dis::gIa32ModRM16;
        break;
    case IntegerEncoding::INTEGER_32BIT:
        // 32 bit
        modrmTranslator = &ia32dis::gIa32ModRM32;
        break;
    default:
        // The 64 bit mode is getModrmString64
        CHECK_FAIL();
    }

    // Load appropriate Register table. Determined by MODR/M mode (Memory
    // reference or direct register access) and MODR/M operand type
    if ((*modrmTranslator)[mod][reg].m_isReference ||
        (type == ia32dis::OPND_MODRM_MEM))
    {
        // MODR/M reference mode. Load MODR/M table based on address size
        // attribute
        switch (m_opcode->m_addressSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            registersDescriptor = ia32dis::gIa16Registers;
            break;
        case IntegerEncoding::INTEGER_32BIT:
            registersDescriptor = ia32dis::gIa32Registers;
            break;
        default:
            CHECK_FAIL();
        }
    } else
    {
        // MODR/M Direct register mode (11).
        // NOTE: Check if it truly is either reference (00, 01, 10) or direct
        //       register(11) or not. If not, then redesign is required
        switch (type)
        {
        case ia32dis::OPND_MODRM_dWORDPTR:
            switch (m_opcode->m_operandSize)
            {
            case IntegerEncoding::INTEGER_16BIT:
                registersDescriptor = ia32dis::gIa16Registers;
                break;
            case IntegerEncoding::INTEGER_32BIT:
                registersDescriptor = ia32dis::gIa32Registers;
                break;
            default:
                CHECK_FAIL();
            }
            break;
        case ia32dis::OPND_MODRM_FIXED_DWORDPTR:
            registersDescriptor = ia32dis::gIa32Registers;
            break;
        case ia32dis::OPND_MODRM_WORDPTR:
            registersDescriptor = ia32dis::gIa16Registers;
            break;
        case ia32dis::OPND_MODRM_BYTEPTR:
            registersDescriptor = ia32dis::gIa8Registers;
            break;
        case ia32dis::OPND_MODRM_MEM: // Handled in the previous case.
        default:
            CHECK_FAIL();
        }
    }

    if ((*modrmTranslator)[mod][reg].m_isReference)
    {
        switch (type)
        {
        case ia32dis::OPND_MODRM_dWORDPTR:
            switch (m_opcode->m_operandSize)
            {
            case IntegerEncoding::INTEGER_16BIT: ret+= "word ptr "; break;
            case IntegerEncoding::INTEGER_32BIT: ret+= "dword ptr ";  break;
            default: break;
            }
            break;
        case ia32dis::OPND_MODRM_FIXED_DWORDPTR:
            ret+= "dword ptr "; break;
        case ia32dis::OPND_MODRM_WORDPTR:
            ret+= "word ptr "; break;
        case ia32dis::OPND_MODRM_BYTEPTR:
            ret+= "byte ptr "; break;
        case ia32dis::OPND_MODRM_QWORDPTR:
            ret+= "qword ptr "; break;
        case ia32dis::OPND_MODRM_TWORDPTR:
            ret+= "tword ptr "; break;
        default:
            break;
        }

        ret+= getSegmentSelector();
        ret+= "[";
    }

    uint firstReg = (*modrmTranslator)[mod][reg].m_firstRegisterPointer;
    uint secondReg = (*modrmTranslator)[mod][reg].m_secondRegisterPointer;

    if (firstReg != ia32dis::NO_REGISTER)
        ret+= registersDescriptor[firstReg].m_name;

    if (secondReg != ia32dis::NO_REGISTER)
    {
        ret+= "+";
        ret+= registersDescriptor[secondReg].m_name;
    }

    if ((*modrmTranslator)[mod][reg].m_forceSib)
    {
        // Start with the base register. Mod 0 with base EBP has no base (a
        // disp32 is used instead)
        bool hasBase = (mod != 0) ||
                       (m_opcode->m_sib.m_bits.m_base != ia32dis::IA32_GP32_EBP);
        if (hasBase)
            ret+= registersDescriptor[m_opcode->m_sib.m_bits.m_base].m_name;
        // Add the scaled index register. Index ESP means no index.
        if (m_opcode->m_sib.m_bits.m_index != ia32dis::IA32_GP32_ESP)
        {
            if (hasBase)
                ret+= "+";
            ret+= registersDescriptor[m_opcode->m_sib.m_bits.m_index].m_name;
            switch (m_opcode->m_sib.m_bits.m_scale)
            {
            // With an EBP base the index is written with its scale: an
            // assembler swaps [ebp+eax] into the shorter [eax+ebp]
            case 0: if (hasBase &&
                        (m_opcode->m_sib.m_bits.m_base == ia32dis::IA32_GP32_EBP))
                        ret+= "*1";
                    break;
            case 1: ret+= "*2"; break;
            case 2: ret+= "*4"; break;
            case 3: ret+= "*8"; break;
            }
        }
    }

    if (m_opcode->m_displacementLength > 0)
    {
        int32 displacement = 0;
        switch (m_opcode->m_displacementLength)
        {
        case 1: displacement = (int8)(m_opcode->m_displacement); break;
        case 2: displacement = (int16)(m_opcode->m_displacement); break;
        case 4: displacement = (int32)(m_opcode->m_displacement); break;
        default:
            CHECK_FAIL();
        }

        if ((*modrmTranslator)[mod][reg].m_displacementRelative)
        {
            /*
             * TODO! How should I treat the displacement? As an address or as a
             *       const?, For now I think I will use it as an address relative+
             *       to 0...
             *
            ret+= m_dataFormatter.translateRelativeAddress(displacement,
                            false,
                            gNullPointerProcessorAddress);
            */

            ret+= m_dataFormatter.translateRelativeDisplacement(displacement);
        } else
        {
            // An absolute displacement which follows the SIB registers
            if (ret[ret.length() - 1] != XSTL_CHAR('['))
                ret+= "+";
            switch (m_opcode->m_displacementLength)
            {
            case 1: ret+= m_dataFormatter.translateUint8(displacement); break;
            case 2: if (is32bit())
                        ret+= m_dataFormatter.translateUint16(displacement);
                    else
                        ret+= m_dataFormatter.translateAbsoluteAddress(
                            ProcessorAddress(ProcessorAddress::PROCESSOR_16,
                                             displacement));
                    break;
            case 4: if (is32bit())
                        ret+= m_dataFormatter.translateAbsoluteAddress(
                            ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                             displacement));
                    else
                        ret+= m_dataFormatter.translateUint32(displacement);
                    break;
            }
        }
    }

    if ((*modrmTranslator)[mod][reg].m_isReference)
        ret+= "]";

    return ret;
}

cString IA32IntelNotation::stringOperand(ia32dis::OperandType type) const
{
    // Dummy uint object
    uint tempu;

    // The operand size and the register fields. In 64 bit mode the REX
    // prefix extends the registers
    const uint operandBits = getOperandBits();
    const uint regField = m_opcode->m_modrm.m_bits.m_regOpcode |
                          (m_opcode->m_rex.m_bits.r << 3);
    const uint rmField = m_opcode->m_modrm.m_bits.m_rm |
                         (m_opcode->m_rex.m_bits.b << 3);
    const uint opcodeField = (m_opcode->m_lastOpcodeByte & 7) |
                             (m_opcode->m_rex.m_bits.b << 3);

    switch (type)
    {
    case ia32dis::OPND_GP_16_32BIT:
        return getGeneralRegister(regField, operandBits);
    case ia32dis::OPND_GP_8BIT_MODRM:
        return getGeneralRegister(regField, 8);
    case ia32dis::OPND_GP_16BIT_MODRM:
        return getGeneralRegister(regField, 16);
    case ia32dis::OPND_SIMD_MODRM:
        return is64bit() ? ia32dis::gAmd64SIMDRegisters[regField].m_name :
            ia32dis::gIa32SIMDRegisters[m_opcode->m_modrm.m_bits.m_regOpcode].
                m_name;
    case ia32dis::OPND_CTRL_MODRM:
        return is64bit() ? ia32dis::gAmd64ControlRegisters[regField].m_name :
            ia32dis::gIa32ControlRegisters[m_opcode->m_modrm.m_bits.m_regOpcode].
                m_name;
    case ia32dis::OPND_DBG_MODRM:
        return ia32dis::gIa32DebugRegisters[m_opcode->m_modrm.m_bits.m_regOpcode].
            m_name;
    case ia32dis::OPND_GP_NATIVE_RM:
        return getGeneralRegister(rmField, is64bit() ? 64 : 32);

    case ia32dis::OPND_MODRM_MEM:
        // The 66 prefix selects the m16&32 pointer of the descriptor tables,
        // which nasm writes with the word keyword
        if (is64bit() && (operandBits == 16) &&
            (m_opcode->m_opcode->m_prefix == 0x01))
            return cString("word ") + getModrmString(type);
        return getModrmString(type);
    case ia32dis::OPND_MODRM_dWORDPTR:
    case ia32dis::OPND_MODRM_FIXED_DWORDPTR:
    case ia32dis::OPND_MODRM_WORDPTR:
    case ia32dis::OPND_MODRM_BYTEPTR:
    case ia32dis::OPND_MODRM_QWORDPTR:
    case ia32dis::OPND_MODRM_TWORDPTR:
    case ia32dis::OPND_MODRM_DOUBLE_MEM:
        return getModrmString(type);

    case ia32dis::OPND_MMX_MODRM:
        return ia32dis::gIa32MMXRegisters[m_opcode->m_modrm.m_bits.m_regOpcode].
            m_name;
    case ia32dis::OPND_MMX_RM:
        if (m_opcode->m_modrm.m_bits.m_mod == 3)
            return ia32dis::gIa32MMXRegisters[m_opcode->m_modrm.m_bits.m_rm].m_name;
        return cString("qword ptr ") + getModrmString(ia32dis::OPND_MODRM_MEM);
    case ia32dis::OPND_SIMD_RM:
    case ia32dis::OPND_SIMD_RM_QWORD:
    case ia32dis::OPND_SIMD_RM_DWORD:
        if (m_opcode->m_modrm.m_bits.m_mod == 3)
            return is64bit() ? ia32dis::gAmd64SIMDRegisters[rmField].m_name :
                ia32dis::gIa32SIMDRegisters[m_opcode->m_modrm.m_bits.m_rm].m_name;
        return cString((type == ia32dis::OPND_SIMD_RM) ? "oword ptr " :
                       (type == ia32dis::OPND_SIMD_RM_QWORD) ? "qword ptr " :
                                                               "dword ptr ") +
               getModrmString(ia32dis::OPND_MODRM_MEM);
    case ia32dis::OPND_GP_32BIT_MODRM:
        // 64 bits with REX.W
        return getGeneralRegister(regField,
                                  (m_opcode->m_rex.m_bits.w != 0) ? 64 : 32);
    case ia32dis::OPND_MODRM_R32_WORDPTR:
        if (m_opcode->m_modrm.m_bits.m_mod == 3)
            return getGeneralRegister(rmField, 32);
        return cString("word ptr ") + getModrmString(ia32dis::OPND_MODRM_MEM);

    case ia32dis::OPND_FPU_ST0:
        return "st0";
    case ia32dis::OPND_FPU_ST_RM:
        return cString("st") + cString((int32)m_opcode->m_modrm.m_bits.m_rm);

    case ia32dis::OPND_MODRM_FAR_OFFSET:
        // jmp/call far through a m16:16 or m16:32 pointer
        return cString("far ") + getModrmString(ia32dis::OPND_MODRM_dWORDPTR);

    case ia32dis::OPND_GP_SEGMENT_MODRM:
        tempu = m_opcode->m_modrm.m_bits.m_regOpcode;
        CHECK(tempu < ia32dis::NUMBER_OF_SEGMENTS_REGISTERS);
        return ia32dis::gIa32SegmentsRegisters[tempu].m_name;

    case ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32:
        return getGeneralRegister(opcodeField, operandBits);

    case ia32dis::OPND_ONEBYTES_OPCODE_GP_8:
        return getGeneralRegister(opcodeField, 8);

    case ia32dis::OPND_AL:
        return ia32dis::gIa8Registers[ia32dis::IA32_GP8_AL].m_name;
    case ia32dis::OPND_AX:
        return ia32dis::gIa16Registers[ia32dis::IA32_GP16_AX].m_name;
    case ia32dis::OPND_CL:
        return ia32dis::gIa8Registers[ia32dis::IA32_GP8_CL].m_name;
    case ia32dis::OPND_eAX:
        return getGeneralRegister(ia32dis::IA32_GP32_EAX, operandBits);
    case ia32dis::OPND_eBX:
        return getGeneralRegister(ia32dis::IA32_GP32_EBX, operandBits);
    case ia32dis::OPND_eBP:
        return getGeneralRegister(ia32dis::IA32_GP32_EBP, operandBits);
    case ia32dis::OPND_eSI:
        return getGeneralRegister(ia32dis::IA32_GP32_ESI, operandBits);
    case ia32dis::OPND_eDI:
        return getGeneralRegister(ia32dis::IA32_GP32_EDI, operandBits);
    case ia32dis::OPND_DX:
        return ia32dis::gIa16Registers[ia32dis::IA32_GP16_DX].m_name;
    case ia32dis::OPND_FS:
        return ia32dis::gIa32SegmentsRegisters[ia32dis::IA32_SEG_FS].m_name;
    case ia32dis::OPND_GS:
        return ia32dis::gIa32SegmentsRegisters[ia32dis::IA32_SEG_GS].m_name;
    case ia32dis::OPND_CS:
        return ia32dis::gIa32SegmentsRegisters[ia32dis::IA32_SEG_CS].m_name;
    case ia32dis::OPND_DS:
        return ia32dis::gIa32SegmentsRegisters[ia32dis::IA32_SEG_DS].m_name;
    case ia32dis::OPND_SS:
        return ia32dis::gIa32SegmentsRegisters[ia32dis::IA32_SEG_SS].m_name;
    case ia32dis::OPND_ES:
        return ia32dis::gIa32SegmentsRegisters[ia32dis::IA32_SEG_ES].m_name;

    case ia32dis::OPND_ONE:
        return "1";

    case ia32dis::OPND_THREE:
        return "3";

    case ia32dis::OPND_IMMEDIATE_8BIT:
        ASSERT(m_opcode->m_immediateLength == 1);
        // The second immediate of 'enter imm16, imm8'
        if (m_opcode->m_opcode->m_firstOperand == ia32dis::OPND_IMMEDIATE_16BIT)
            return m_dataFormatter.translateUint8(
                        (uint8)m_opcode->m_immediate.secondOffset);
        // The sign-extended imm8 (83 /digit ib, 6B /r ib, 6A ib) is shown as a
        // signed value. 'push imm8' with an operand size which isn't the
        // default of the mode has the size keyword (push word, push dword)
        if ((m_opcode->m_opcode->m_prefix == 0x83) ||
            (m_opcode->m_opcode->m_prefix == 0x6B) ||
            (m_opcode->m_opcode->m_prefix == 0x6A))
        {
            cString size;
            if (m_opcode->m_opcode->m_prefix == 0x6A)
                size = getOperandSizeKeyword();
            if ((int8)m_opcode->m_immediate.offset < 0)
                return size + "-" + m_dataFormatter.translateUint8(
                        (uint8)(-(int8)m_opcode->m_immediate.offset));
            return size + m_dataFormatter.translateUint8(
                        (uint8)m_opcode->m_immediate.offset);
        }
        return m_dataFormatter.translateUint8(
                        (uint8)m_opcode->m_immediate.offset);
    case ia32dis::OPND_IMMEDIATE_16BIT:
        return m_dataFormatter.translateUint16(
                        (uint16)m_opcode->m_immediate.offset);
    case ia32dis::OPND_IMMEDIATE_DS:
        switch (operandBits)
        {
        case 16:
            return ((m_opcode->m_opcode->m_prefix == 0x68) ?
                        getOperandSizeKeyword() : cString()) +
                m_dataFormatter.translateUint16(
                        (uint16)m_opcode->m_immediate.offset);
        case 32:
            return ((m_opcode->m_opcode->m_prefix == 0x68) ?
                        getOperandSizeKeyword() : cString()) +
                m_dataFormatter.translateUint32(
                        (uint32)m_opcode->m_immediate.offset);
        case 64:
            // The immediate is 32 bits, sign extended. A negative one is
            // written as a negative number, nasm doesn't take the 64 bit
            // constant 0xFFFFFFFFxxxxxxxx as a sign extended immediate
            if ((int32)m_opcode->m_immediate.offset < 0)
                return cString("-") + m_dataFormatter.translateUint32(
                        (uint32)(0 - (uint32)m_opcode->m_immediate.offset));
            return m_dataFormatter.translateUint32(
                        (uint32)m_opcode->m_immediate.offset);
        default:
            CHECK_FAIL();
        }
    case ia32dis::OPND_IMMEDIATE_DQ:
        switch (operandBits)
        {
        case 16:
            return m_dataFormatter.translateUint16(
                        (uint16)m_opcode->m_immediate.offset);
        case 32:
            return m_dataFormatter.translateUint32(
                        (uint32)m_opcode->m_immediate.offset);
        case 64:
            return m_dataFormatter.translateUint64(m_opcode->m_immediate.offset);
        default:
            CHECK_FAIL();
        }

    case ia32dis::OPND_MEMREF_OFFSET_DS:
        switch (m_opcode->m_addressSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            return getSegmentSelector() + cString("[") +
                m_dataFormatter.translateAbsoluteAddress(
                    ProcessorAddress(ProcessorAddress::PROCESSOR_16,
                                     m_opcode->m_immediate.offset))
                + "]";
        case IntegerEncoding::INTEGER_32BIT:
            return getSegmentSelector() + cString("[") +
                (is64bit() ? "a32 " : "") +
                m_dataFormatter.translateAbsoluteAddress(
                    ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                     m_opcode->m_immediate.offset))
                + "]";
        case IntegerEncoding::INTEGER_64BIT:
            // mov al, [moffs64]: the qword keyword selects the 64 bit offset
            return getSegmentSelector() + cString("[qword ") +
                m_dataFormatter.translateAbsoluteAddress(
                    ProcessorAddress(ProcessorAddress::PROCESSOR_64,
                                     m_opcode->m_immediate.offset))
                + "]";
        default:
            CHECK_FAIL();
        }


    case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
        // The operand size, if it isn't the default of the mode (66 jz)
        return getOperandSizeKeyword() + m_dataFormatter.translateRelativeAddress(
            (ProcessorAddress::intAddress)((int8)m_opcode->m_immediate.offset) +
                // NOTE: All ia32 relative calculate are from the next operation
                m_opcode->m_opcodeData.getSize(),
            m_opcode->m_shouldUseAddress,
            m_opcode->m_opcodeAddress);

    case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
    {
        // rel16 or rel32 according to the operand size (rel32 in 64 bit mode)
        ProcessorAddress::intAddress relative =
            (m_opcode->m_operandSize == IntegerEncoding::INTEGER_16BIT) ?
                (ProcessorAddress::intAddress)((int16)m_opcode->m_immediate.offset) :
                (ProcessorAddress::intAddress)((int32)m_opcode->m_immediate.offset);
        // NOTE: All ia32 relative calculate are from the next operation
        relative+= m_opcode->m_opcodeData.getSize();
        // A near jcc/jmp whose target fits in the 2 bytes short form is marked
        // 'near', otherwise it's assembled as short (call has no short form)
        cString hint;
        if ((m_opcode->m_opcode->m_prefix != 0xE8) &&
            (relative - 2 >= -0x80) && (relative - 2 <= 0x7F))
            hint = "near ";
        // The operand size, if it isn't the default of the mode ('jmp word
        // $+N' in 32 bit mode, 'call dword $+N' in 16 bit mode)
        return hint + getOperandSizeKeyword() +
            m_dataFormatter.translateRelativeAddress(relative,
                m_opcode->m_shouldUseAddress,
                m_opcode->m_opcodeAddress);
    }

    case ia32dis::OPND_IMMEDIATE_OFFSET_FAR:
        // An absolute segment:offset, the offset is operand sized
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
        return m_dataFormatter.translateUint16(m_opcode->m_immediate.segment) +
               ":" + m_dataFormatter.translateUint16(
                (uint16)m_opcode->m_immediate.offset);
        case IntegerEncoding::INTEGER_32BIT:
        return m_dataFormatter.translateUint16(m_opcode->m_immediate.segment) +
                ":" + m_dataFormatter.translateUint32(
                (uint32)m_opcode->m_immediate.offset);
        default:
            CHECK_FAIL();
        }

    default:
        // Not ready yet!!
        CHECK_FAIL();
    }
}
