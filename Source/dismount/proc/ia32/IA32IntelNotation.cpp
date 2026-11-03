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
        break;
    case OpcodeSubsystems::DISASSEMBLER_AMD_64:
        // TODO!
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

    if (formatStruct != NULL)
        formatStruct->m_opcodeNameStart = ret.length();

    // Add the opcode name
    cString opcodeName = m_opcode->m_opcode->m_opcodeName;
    // The name conventions follow the operand size ('/', '#d' and '##') and
    // the address size ('#e')
    bool operand32 = (m_opcode->m_operandSize == IntegerEncoding::INTEGER_32BIT);
    bool address32 = (m_opcode->m_addressSize == IntegerEncoding::INTEGER_32BIT);
    // Replace name convenstion
    if (opcodeName.find("/") < opcodeName.length())
    {
        // TODO! 64bit
        if (!operand32)
            opcodeName = opcodeName.left(opcodeName.find("/"));
        else
            opcodeName = opcodeName.part(opcodeName.find("/") + 1,
                                         opcodeName.length());
    }

    uint namePos = opcodeName.find("#");
    while (namePos != opcodeName.length()) {
        switch (opcodeName[namePos + 1])
        {
        case XSTL_CHAR('d'):
            opcodeName = opcodeName.left(namePos) +
                (operand32 ? "d" : "") +
                opcodeName.part(namePos + 2, opcodeName.length());
            break;
        case XSTL_CHAR('e'):
            opcodeName = opcodeName.left(namePos) +
                (address32 ? "e" : "") +
                opcodeName.part(namePos + 2, opcodeName.length());
            break;
        case XSTL_CHAR('#'):
            opcodeName = opcodeName.left(namePos) +
                (operand32 ? "d" : "w") +
                opcodeName.part(namePos + 2, opcodeName.length());
            break;
        }
        namePos = opcodeName.find("#");
    }

    // The stack frame and flags instructions with an operand size which is
    // not the default one are written with the size suffix (66 C3 is 'retw').
    // The '#d' names already end with 'd' for 32 bit.
    cString tableName(m_opcode->m_opcode->m_opcodeName);
    if ((operand32 != is32bit()) && isSizeSuffixed(tableName) &&
        (!operand32 || (tableName.find("#d") == tableName.length())))
        opcodeName+= operand32 ? "d" : "w";

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
        // default one count with cx or ecx
        if ((m_opcode->m_opcode->m_firstOperand == ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8) &&
            (m_opcode->m_opcode->m_prefix >= 0xE0) && (m_opcode->m_opcode->m_prefix <= 0xE3) &&
            (address32 != is32bit()))
            ret+= m_dataFormatter.getOpcodesSeparator(ret) + (address32 ? "ecx" : "cx");
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

cString IA32IntelNotation::getModrmString(ia32dis::OperandType type) const
{
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
        // For 64bit and all other unknown value.
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
        // TODO! 64 bit table
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

    switch (type)
    {
    case ia32dis::OPND_GP_16_32BIT:
        // TODO! If the processor ever supports different endian types, change
        // this value
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            return ia32dis::gIa16Registers[m_opcode->m_modrm.m_bits.m_regOpcode].
                        m_name;
        case IntegerEncoding::INTEGER_32BIT:
            return ia32dis::gIa32Registers[m_opcode->m_modrm.m_bits.m_regOpcode].
                        m_name;
        default:
            // For 64bit and all other unknown value.
            CHECK_FAIL();
        }
    case ia32dis::OPND_GP_8BIT_MODRM:
        return ia32dis::gIa8Registers[m_opcode->m_modrm.m_bits.m_regOpcode].
            m_name;
    case ia32dis::OPND_GP_16BIT_MODRM:
        return ia32dis::gIa16Registers[m_opcode->m_modrm.m_bits.m_regOpcode].
            m_name;
    case ia32dis::OPND_SIMD_MODRM:
        return ia32dis::gIa32SIMDRegisters[m_opcode->m_modrm.m_bits.m_regOpcode].
            m_name;
    case ia32dis::OPND_CTRL_MODRM:
        return ia32dis::gIa32ControlRegisters[m_opcode->m_modrm.m_bits.m_regOpcode].
            m_name;
    case ia32dis::OPND_DBG_MODRM:
        return ia32dis::gIa32DebugRegisters[m_opcode->m_modrm.m_bits.m_regOpcode].
            m_name;

    case ia32dis::OPND_MODRM_dWORDPTR:
    case ia32dis::OPND_MODRM_WORDPTR:
    case ia32dis::OPND_MODRM_BYTEPTR:
    case ia32dis::OPND_MODRM_QWORDPTR:
    case ia32dis::OPND_MODRM_TWORDPTR:
    case ia32dis::OPND_MODRM_MEM:
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
            return ia32dis::gIa32SIMDRegisters[m_opcode->m_modrm.m_bits.m_rm].m_name;
        return cString((type == ia32dis::OPND_SIMD_RM) ? "oword ptr " :
                       (type == ia32dis::OPND_SIMD_RM_QWORD) ? "qword ptr " :
                                                               "dword ptr ") +
               getModrmString(ia32dis::OPND_MODRM_MEM);
    case ia32dis::OPND_GP_32BIT_MODRM:
        return ia32dis::gIa32Registers[m_opcode->m_modrm.m_bits.m_regOpcode].
            m_name;
    case ia32dis::OPND_MODRM_R32_WORDPTR:
        if (m_opcode->m_modrm.m_bits.m_mod == 3)
            return ia32dis::gIa32Registers[m_opcode->m_modrm.m_bits.m_rm].m_name;
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
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            return ia32dis::gIa16Registers[m_opcode->m_lastOpcodeByte & 7].m_name;
        case IntegerEncoding::INTEGER_32BIT:
            return ia32dis::gIa32Registers[m_opcode->m_lastOpcodeByte & 7].m_name;
        default:
            CHECK_FAIL();
        }

    case ia32dis::OPND_ONEBYTES_OPCODE_GP_8:
        return ia32dis::gIa8Registers[m_opcode->m_lastOpcodeByte & 7].m_name;

    case ia32dis::OPND_AL:
        return ia32dis::gIa8Registers[ia32dis::IA32_GP8_AL].m_name;
    case ia32dis::OPND_AX:
        return ia32dis::gIa16Registers[ia32dis::IA32_GP16_AX].m_name;
    case ia32dis::OPND_CL:
        return ia32dis::gIa8Registers[ia32dis::IA32_GP8_CL].m_name;
    case ia32dis::OPND_eAX:
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            return ia32dis::gIa16Registers[ia32dis::IA32_GP16_AX].m_name;
        case IntegerEncoding::INTEGER_32BIT:
            return ia32dis::gIa32Registers[ia32dis::IA32_GP32_EAX].m_name;
        default:
            // For 64bit and all other unknown value.
            CHECK_FAIL();
        }
    case ia32dis::OPND_eBX:
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            return ia32dis::gIa16Registers[ia32dis::IA32_GP16_BX].m_name;
        case IntegerEncoding::INTEGER_32BIT:
            return ia32dis::gIa32Registers[ia32dis::IA32_GP32_EBX].m_name;
        default:
            // For 64bit and all other unknown value.
            CHECK_FAIL();
        }
    case ia32dis::OPND_eBP:
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            return ia32dis::gIa16Registers[ia32dis::IA32_GP16_BP].m_name;
        case IntegerEncoding::INTEGER_32BIT:
            return ia32dis::gIa32Registers[ia32dis::IA32_GP32_EBP].m_name;
        default:
            // For 64bit and all other unknown value.
            CHECK_FAIL();
        }
    case ia32dis::OPND_eSI:
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            return ia32dis::gIa16Registers[ia32dis::IA32_GP16_SI].m_name;
        case IntegerEncoding::INTEGER_32BIT:
            return ia32dis::gIa32Registers[ia32dis::IA32_GP32_ESI].m_name;
        default:
            // For 64bit and all other unknown value.
            CHECK_FAIL();
        }
    case ia32dis::OPND_eDI:
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            return ia32dis::gIa16Registers[ia32dis::IA32_GP16_DI].m_name;
        case IntegerEncoding::INTEGER_32BIT:
            return ia32dis::gIa32Registers[ia32dis::IA32_GP32_EDI].m_name;
        default:
            // For 64bit and all other unknown value.
            CHECK_FAIL();
        }
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
        // signed value. 'push imm8' with a 16 bit operand size pushes a word.
        if ((m_opcode->m_opcode->m_prefix == 0x83) ||
            (m_opcode->m_opcode->m_prefix == 0x6B) ||
            (m_opcode->m_opcode->m_prefix == 0x6A))
        {
            cString size;
            if ((m_opcode->m_opcode->m_prefix == 0x6A) &&
                (m_opcode->m_operandSize == IntegerEncoding::INTEGER_16BIT))
                size = "word ";
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
        switch (m_opcode->m_operandSize)
        {
        case IntegerEncoding::INTEGER_16BIT:
            return m_dataFormatter.translateUint16(
                        (uint16)m_opcode->m_immediate.offset);
        case IntegerEncoding::INTEGER_32BIT:
            return m_dataFormatter.translateUint32(
                        (uint32)m_opcode->m_immediate.offset);
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
                m_dataFormatter.translateAbsoluteAddress(
                    ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                     m_opcode->m_immediate.offset))
                + "]";
        default:
            CHECK_FAIL();
        }


    case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
        return m_dataFormatter.translateRelativeAddress(
            (ProcessorAddress::intAddress)((int8)m_opcode->m_immediate.offset) +
                // NOTE: All ia32 relative calculate are from the next operation
                m_opcode->m_opcodeData.getSize(),
            m_opcode->m_shouldUseAddress,
            m_opcode->m_opcodeAddress);

    case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
    {
        // rel16 or rel32 according to the operand size
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
        return hint + m_dataFormatter.translateRelativeAddress(relative,
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
