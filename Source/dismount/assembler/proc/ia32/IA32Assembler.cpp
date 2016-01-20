#include "dismount/dismount.h"
/*
 * IA32Assembler.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/endian.h"
#include "xStl/except/trace.h"
#include "xStl/parser/parser.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/IntegerEncoding.h"
#include "dismount/proc/ia32/opcodeTable.h"

#ifdef _DEBUG
#include "xStl/stream/traceStream.h"
#endif

IA32Assembler::IA32Assembler(FirstPassBinaryPtr binary,
                             IA32eInstructionSet::DisassemblerTypes processorMode) :
    m_binary(binary),
    m_processorMode(processorMode)
{
}

cStringerStreamPtr IA32Assembler::getAssembler()
{
    return cStringerStreamPtr(new IA32AssemblerStream(*m_binary,
                                                      m_processorMode));
}

FirstPassBinaryPtr IA32Assembler::getFirstPass()
{
    return m_binary;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

IA32AssemblerStream::IA32AssemblerStream(FirstPassBinary& binary,
                           IA32eInstructionSet::DisassemblerTypes processorMode) :
    m_binary(binary),
    m_processorMode(processorMode)
{
    // TODO! This module was only tested on 32 bit machines. When running in a
    //       different context this mode should be tested.
    CHECK(m_processorMode == IA32eInstructionSet::INTEL_32);
}

IA32AssemblerStream::~IA32AssemblerStream()
{
    // Check whether the data should be ignored
    if (getData().length() == 0)
        return;

    // Start parsing data
    cSArray<char> ascii = getData().getASCIIstring();
    // Remove comments insert for debugging
    Parser::easyParsing(ascii.getBuffer(), ascii.getSize());
    Parser parser(ascii.getBuffer(),
                  ascii.getBuffer(),
                  ascii.getSize() - 1, // Reduce the null terminate character!
                  1);

    // And start compiling
    parser.readBlanks();
    while (!parser.isEOS())
    {
        // Read opcode
        cString opcode = parser.readWord();

        // The first operand and whether it exist
        Operand firstOperand;
        // The second operand and whether it exist
        Operand secondOperand;
        // The third operand and whether it exist
        Operand thirdOperand;

        // Reading operands
        if (!parser.readBlanksUntilEOL())
            // Read first argument
            firstOperand.readOperand(parser);
        if (!parser.readBlanksUntilEOL())
            // Read first argument
            secondOperand.readOperand(parser);
        if (!parser.readBlanksUntilEOL())
            // Read first argument
            thirdOperand.readOperand(parser);

        // Find matching entry and encode...
        writeOpcode(opcode, firstOperand, secondOperand, thirdOperand);

        // Read new-line symbol
        parser.readBlanks();
    }
}

void IA32AssemblerStream::writeOpcode(const cString& name,
                               const IA32AssemblerStream::Operand& first,
                               const IA32AssemblerStream::Operand& second,
                               const IA32AssemblerStream::Operand& third)
{
    uint selectedTable = (uint)-1;
    uint selectedOpcodeSize = (uint)-1;
    const ia32dis::OpcodeEntry* selectedOpcode = NULL;
    bool selectedOperandSizePrefix = false;
    bool selectedAddressSizePrefix = false;
    bool selectedModrm = false;
    IA32OpcodeDatastruct::MODRM selectedModrmMod;

    // There are two tables opcodes so far: One-byte opcode table and two byte
    // opcode table
    // Tries to find best opcode...
    for (uint tableIndex = 0; tableIndex < 2; tableIndex++)
    {
        // Find the table
        const ia32dis::OpcodeEntry* opcode = NULL;
        switch(tableIndex)
        {
        // One byte opcode table
        case 0: opcode = ia32dis::gIa32OneByteOpcodeTable; break;
        // Two bytes opcode table
        case 1: opcode = ia32dis::gIa32TwoBytesOpcodeTable; break;
        default:
            // Cannot find the table!!!
            CHECK_FAIL();
        }

        // And scan the table for a match
        bool operandSizePrefix = false;
        bool addressSizePrefix = false;
        bool modrm = false;
        IA32OpcodeDatastruct::MODRM modrmMod;
        modrmMod.m_packed = 0;
        while (opcode->m_opcodeName != ia32dis::OPCODEEOT)
        {
            // TODO! Extend this code to support special 16/32 bit opcode
            //       wildcard  (e.g. pusha/pushad...)
            if (name == opcode->m_opcodeName)
            {
                // Try to find a match with arguments
                if (matchOpcode(opcode, first, second, third,
                                operandSizePrefix, addressSizePrefix,
                                modrm, modrmMod))
                {
                    // Calculate the length of the opcode
                    uint opcodeSize = calculateOpcodeSize(tableIndex, opcode,
                                                          operandSizePrefix,
                                                          addressSizePrefix,
                                                          modrm, modrmMod);
                    if (opcodeSize < selectedOpcodeSize)
                    {
                        // Switch opcode
                        selectedOpcodeSize = opcodeSize;
                        selectedOpcode = opcode;
                        selectedOperandSizePrefix = operandSizePrefix;
                        selectedAddressSizePrefix = addressSizePrefix;
                        selectedTable = tableIndex;
                        selectedModrm = modrm;
                        selectedModrmMod = modrmMod;
                    }
                }
            }

            // Continue until the table ends
            opcode++;
        }
    }

    // Check that we have found an opcode
    CHECK(selectedOpcode != NULL);

    // Encode it
    encodeOpcode(selectedTable, selectedOpcode, first, second, third,
                 selectedOperandSizePrefix, selectedAddressSizePrefix,
                 selectedModrm, selectedModrmMod);
}

bool IA32AssemblerStream::matchOpcode(const ia32dis::OpcodeEntry* opcode,
                               const IA32AssemblerStream::Operand& first,
                               const IA32AssemblerStream::Operand& second,
                               const IA32AssemblerStream::Operand& third,
                               bool& operandSizePrefix,
                               bool& addressSizePrefix,
                               bool& modrm,
                               IA32OpcodeDatastruct::MODRM& modrmMod) const
{
    // Don't even try matching irrelevant opcodes
    if (opcode->m_opcodeName == ia32dis::INVALID)
        return false;

    operandSizePrefix = false;
    addressSizePrefix = false;
    modrm = false;
    modrmMod.m_packed = 0;

    // The comparison is simple try to match each type into the encoding process
    if (!matchOperand(opcode->m_firstOperand,  first,
                      operandSizePrefix, addressSizePrefix,
                      modrm, modrmMod,
                      opcode->m_isUnsigned))
        return false;
    if (!matchOperand(opcode->m_secondOperand, second,
                      operandSizePrefix, addressSizePrefix,
                      modrm, modrmMod,
                      opcode->m_isUnsigned))
        return false;
    if (!matchOperand(opcode->m_thridOperand,  third,
                      operandSizePrefix, addressSizePrefix,
                      modrm, modrmMod,
                      opcode->m_isUnsigned))
        return false;

    // Find a match
    return true;
}

bool IA32AssemblerStream::matchOperand(ia32dis::OperandType ia32disType,
                                const Operand& compilerOperand,
                                bool& operandSizePrefix,
                                bool& addressSizePrefix,
                                bool& modrm,
                                IA32OpcodeDatastruct::MODRM& modrmMod,
                                bool isUnsigedAllowed) const
{
    // DONT RESET PREFIXES!
    //     (operandSizePrefix != false) && (addressSizePrefix != false)

    if (compilerOperand.getType() == Operand::NO_OPERAND)
    {
        if (ia32disType == ia32dis::OPND_NO_OPERAND)
            return true;
        else
            return false;
    }

    switch (compilerOperand.getType())
    {
    case Operand::OPERAND_8BIT_REG:
        // 8 bit for both modrm and GP
        if (ia32disType == ia32dis::OPND_ONEBYTES_OPCODE_GP_8)
            return true;

        if (ia32disType == ia32dis::OPND_GP_8BIT_MODRM)
        {
            modrm = true;
            return true;
        }

        // Custom modes
        if ((ia32disType == ia32dis::OPND_CL) &&
            (compilerOperand.getReference1() == ia32dis::IA32_GP8_CL))
            return true;
        if ((ia32disType == ia32dis::OPND_AL) &&
            (compilerOperand.getReference1() == ia32dis::IA32_GP8_AL))
            return true;

        // Marking the modrm as normal register transfer (mode 3)
        if (ia32disType == ia32dis::OPND_MODRM_BYTEPTR)
        {
            modrmMod.m_bits.m_mod = 3;
            modrm = true;
            return true;
        }


        return false;

    case Operand::OPERAND_16BIT_REG:
        if (m_processorMode == IA32eInstructionSet::INTEL_32)
            operandSizePrefix = true;

        // 32 bit for both modrm and GP
        if (ia32disType == ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32)
            return true;

        if (ia32disType == ia32dis::OPND_GP_16_32BIT)
        {
            modrm = true;
            return true;
        }

        // Marking the modrm as normal register transfer (mode 3)
        if ((ia32disType == ia32dis::OPND_MODRM_dWORDPTR) ||
            (ia32disType == ia32dis::OPND_MODRM_WORDPTR)
            /*TODO!(ia32disType == ia32dis::OPND_MODRM_MEM)*/)
        {
            // Change the modrm mod into 3
            modrmMod.m_bits.m_mod = 3;
            modrm = true;
            return true;
        }

        // Custom modes
        if ((ia32disType == ia32dis::OPND_eAX) &&
            (compilerOperand.getReference1() == ia32dis::IA32_GP16_AX))
            return true;

        return false;

    case Operand::OPERAND_32BIT_REG:
        if (m_processorMode == IA32eInstructionSet::INTEL_16)
            operandSizePrefix = true;

        // 32 bit for both modrm and GP
        if (ia32disType == ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32)
            return true;

        if (ia32disType == ia32dis::OPND_GP_16_32BIT)
        {
            modrm = true;
            return true;
        }

        // Marking the modrm as normal register transfer (mode 3)
        if ((ia32disType == ia32dis::OPND_MODRM_dWORDPTR)
            /*TODO!||(ia32disType == ia32dis::OPND_MODRM_MEM)*/)
        {
            // Change the modrm mod into 3
            modrmMod.m_bits.m_mod  = 3;
            modrm = true;
            return true;
        }

        // Custom modes
        if ((ia32disType == ia32dis::OPND_eAX) &&
            (compilerOperand.getReference1() == ia32dis::IA32_GP32_EAX))
            return true;

        return false;

    case Operand::OPERAND_SEGMENT_REG:
        if ((ia32disType == ia32dis::OPND_ES) &&
            (compilerOperand.getReference1() == ia32dis::IA32_SEG_ES))
            return true;
        if ((ia32disType == ia32dis::OPND_CS) &&
            (compilerOperand.getReference1() == ia32dis::IA32_SEG_CS))
            return true;
        if ((ia32disType == ia32dis::OPND_SS) &&
            (compilerOperand.getReference1() == ia32dis::IA32_SEG_SS))
            return true;
        if ((ia32disType == ia32dis::OPND_DS) &&
            (compilerOperand.getReference1() == ia32dis::IA32_SEG_DS))
            return true;
        if ((ia32disType == ia32dis::OPND_FS) &&
            (compilerOperand.getReference1() == ia32dis::IA32_SEG_FS))
            return true;
        if ((ia32disType == ia32dis::OPND_GS) &&
            (compilerOperand.getReference1() == ia32dis::IA32_SEG_GS))
            return true;
        return false;

    case Operand::OPERAND_IMMEDIATE_8BIT:
        if ((ia32disType == ia32dis::OPND_IMMEDIATE_8BIT) ||
            (ia32disType == ia32dis::OPND_IMMEDIATE_16BIT) ||
            (ia32disType == ia32dis::OPND_IMMEDIATE_DS))
            return true;

        return false;

    case Operand::OPERAND_IMMEDIATE_16BIT:
        if ((ia32disType == ia32dis::OPND_IMMEDIATE_16BIT) ||
            (ia32disType == ia32dis::OPND_IMMEDIATE_DS))
            return true;

        if ((isUnsigedAllowed) &&
            (ia32disType == ia32dis::OPND_IMMEDIATE_8BIT) &&
            (compilerOperand.getReference1() <= 0xFF))
            return true;

        return false;

    case Operand::OPERAND_IMMEDIATE_32BIT:
        if ((ia32disType == ia32dis::OPND_IMMEDIATE_DS) &&
            (m_processorMode == IA32eInstructionSet::INTEL_32))
            return true;

        if ((isUnsigedAllowed) &&
            (ia32disType == ia32dis::OPND_IMMEDIATE_16BIT) &&
            (compilerOperand.getReference1() <= 0xFFFF))
            return true;

        return false;

    case Operand::OPERAND_OFFSET_MASK:
        if (ia32disType == ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8)
        {
            // Check for displacement sizes
            if (checkSignedImmediateSize8(compilerOperand.getDisplacement()))
                return true;
            // Wrong size
            return false;
        }

        if ((ia32disType == ia32dis::OPND_IMMEDIATE_OFFSET_DS) ||
            // TODO! OPND_IMMEDIATE_OFFSET_LONG_32 is valid for 16 bit mode.
            (ia32disType == ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32))
        {
            CHECK(m_processorMode == IA32eInstructionSet::INTEL_32);
            return true;
        }

        return false;
    }

    // Check for memory operand
    uint memorySize = compilerOperand.getType() & Operand::OPERAND_MEMORY_ACCESS_MASK;
    CHECK(memorySize != 0);
    // Filter size
    switch (memorySize)
    {
    case Operand::OPERAND_MEMORY_32BIT:
        if ((ia32disType == ia32dis::OPND_MEMREF_OFFSET_DS) ||
            (ia32disType == ia32dis::OPND_MODRM_dWORDPTR) ||
            (ia32disType == ia32dis::OPND_MODRM_MEM))
            break;

        // For all other values
        return false;

    case Operand::OPERAND_MEMORY_16BIT:
        if ((ia32disType == ia32dis::OPND_MEMREF_OFFSET_DS) ||
            (ia32disType == ia32dis::OPND_MODRM_dWORDPTR))
            break;

        if (ia32disType == ia32dis::OPND_MODRM_WORDPTR)
            break;

        // For all other values
        return false;
    case Operand::OPERAND_MEMORY_8BIT:
        if (ia32disType != ia32dis::OPND_MODRM_BYTEPTR)
            return false;
        break;

    default:
        // Not a memory? Check operand
        CHECK_FAIL();
    }

    // Update operand size
    if (!updateOperandSize(memorySize, ia32disType, operandSizePrefix))
        return false;

    // Check for opcode data-size
    uint memoryType = compilerOperand.getType() & Operand::OPERAND_MEMORY_TYPE_MASK;
    switch (memoryType)
    {
    case Operand::OPERAND_MEMORY_DIRECT:
        // TODO! What about 16 bit table?!
        CHECK(is32operand(addressSizePrefix));
        if (ia32disType != ia32dis::OPND_MEMREF_OFFSET_DS)
        {
            // MODRM reference memory
            // Direct memory reference, mod 0 subcode 5
            modrm = true;
            modrmMod.m_bits.m_mod = 0;
            modrmMod.m_bits.m_rm = 5;
        }
        return true;

    case Operand::OPERAND_MEMORY_REG:
        if (ia32disType == ia32dis::OPND_MEMREF_OFFSET_DS)
            return false;
        // TODO! What about 16 bit table?!
        CHECK(is32operand(addressSizePrefix));
        CHECK((compilerOperand.getType() & Operand::OPERAND_32BIT_REG));

        // Direct registry reference, mod group 0.
        modrm = true;
        modrmMod.m_bits.m_mod = 0;
        fillModrmRM(modrmMod, operandSizePrefix, compilerOperand.getType(),
                    compilerOperand.getReference1());
        return true;

    case Operand::OPERAND_MEMORY_REG_PLUS_DISPLACEMENT:
        if (ia32disType == ia32dis::OPND_MEMREF_OFFSET_DS)
            return false;
        // Check register. TODO! Change this code into the modrm mapping table!
        if (is32operand(operandSizePrefix))
        {
            CHECK((compilerOperand.getType() & Operand::OPERAND_32BIT_REG) != 0)
        } else
        {
            ASSERT(is16operand(operandSizePrefix));
            CHECK((compilerOperand.getType() & Operand::OPERAND_16BIT_REG) != 0);
        }
        // Change according to displacement
        modrm = true;
        if (compilerOperand.getDisplacementPacking() ==
            IntegerEncoding::INTEGER_8BIT)
        {
            // Displacement of 8 bit
            modrmMod.m_bits.m_mod = 1;
            // Fill r/m of the modrm according to register and table
            fillModrmRM(modrmMod, operandSizePrefix, compilerOperand.getType(),
                        compilerOperand.getReference1());
        } else
        {
            // Displacement of 32 bit
            modrmMod.m_bits.m_mod = 2;
            // Fill r/m of the modrm according to register and table
            fillModrmRM(modrmMod, operandSizePrefix, compilerOperand.getType(),
                        compilerOperand.getReference1());
        }
        return true;

    default:
        // Not ready yet
        CHECK_FAIL();
    }

    // Default will not find a match. note to developer. Add type.
    ASSERT(false);
    return false;
}

bool IA32AssemblerStream::updateOperandSize(uint memorySize,
                                            ia32dis::OperandType ia32disType,
                                            bool& operandSizePrefix) const
{
    if (memorySize == Operand::OPERAND_MEMORY_32BIT)
    {
        if (m_processorMode == IA32eInstructionSet::INTEL_32)
        {
            // Processor mode: 32 bit, access 32 bit value
            CHECK(!operandSizePrefix);
        } else
        {
            // Processor mode: 16 bit, access 32 bit value
            // Should update the operand size prefix
            operandSizePrefix = true;
        }
    } else if (memorySize == Operand::OPERAND_MEMORY_16BIT)
    {
        if (m_processorMode == IA32eInstructionSet::INTEL_32)
        {
            // Processor mode: 32 bit, access 16 bit value
            if (ia32disType != ia32dis::OPND_MODRM_WORDPTR)
            {
                // The operand reference is illigal. TODO! Check all cases
                if (ia32disType != ia32dis::OPND_MODRM_dWORDPTR)
                    return false;
                // Change prefix
                operandSizePrefix = true;
            }
        } else
        {
            // Processor mode: 16 bit, access 16 bit value
            CHECK(!operandSizePrefix);
        }
    }

    return true;
}

uint IA32AssemblerStream::calculateOpcodeSize(uint tableIndex,
                                       const ia32dis::OpcodeEntry* opcode,
                                       bool operandSizePrefix,
                                       bool addressSizePrefix,
                                       bool modrm,
                                       IA32OpcodeDatastruct::MODRM modrmMod) const
{
    uint size = 0;
    if (tableIndex == 1) size++;
    if (operandSizePrefix) size++;
    if (addressSizePrefix) size++;

    // Calculate operation size
    IntegerEncoding::IntegerEncodingType operandSize =
        getProcessorOperandSize(operandSizePrefix);
    IntegerEncoding::IntegerEncodingType addressSize =
        getProcessorOperandSize(addressSizePrefix);

    // Add opcode base size
    size++;
    // Add modrm, SIB and displacement
    if (modrm)
    {
        ASSERT(opcode->m_modrm != ia32dis::MODRM_NO_MODRM);
        // ASSERT(filter modrm)
        // Add modRM byte
        size++;

        // TODO!!!
        // Is any prefix changes this tables?
        ia32dis::ModRMTranslationType* modrmTable;
        switch (m_processorMode) {
        case IA32eInstructionSet::INTEL_16:
            modrmTable = &ia32dis::gIa32ModRM16;
            break;
        case IA32eInstructionSet::INTEL_32:
            modrmTable = &ia32dis::gIa32ModRM32;
            break;
        default:
            CHECK_FAIL();
        }

        size+= (*modrmTable)[modrmMod.m_bits.m_mod][modrmMod.m_bits.m_rm].
                m_displacementLength;
        if ((*modrmTable)[modrmMod.m_bits.m_mod][modrmMod.m_bits.m_rm].
                m_forceSib)
                // The SIB exist
                size++;
    }

    // Add immediate sizes
    size+= getOperandSize(operandSize, addressSize, opcode->m_firstOperand);
    size+= getOperandSize(operandSize, addressSize, opcode->m_secondOperand);
    size+= getOperandSize(operandSize, addressSize, opcode->m_thridOperand);

    return size;
}

IntegerEncoding::IntegerEncodingType IA32AssemblerStream::getOperandSize(
    IntegerEncoding::IntegerEncodingType processorOperandSize,
    IntegerEncoding::IntegerEncodingType processorAddressSize,
    ia32dis::OperandType ia32disType)
{
    switch (ia32disType)
    {
    case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
    case ia32dis::OPND_IMMEDIATE_8BIT:
        return IntegerEncoding::INTEGER_8BIT;
    case ia32dis::OPND_IMMEDIATE_16BIT:
        return  IntegerEncoding::INTEGER_16BIT;
    case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
        return IntegerEncoding::INTEGER_32BIT;

    case ia32dis::OPND_IMMEDIATE_DS:
        return processorOperandSize;

    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
    case ia32dis::OPND_MEMREF_OFFSET_DS:
        return processorAddressSize;

    case ia32dis::OPND_IMMEDIATE_OFFSET_FAR:
        // TODO!
        CHECK_FAIL();
        // simpleImmediate = m_addressSize;
        // shouldReadSegment = true;

    default:
        // No immediate
        return IntegerEncoding::INTEGER_NOT_EXIST;
    }
}

IntegerEncoding::IntegerEncodingType IA32AssemblerStream::getProcessorOperandSize(
                                                        bool sizePrefix) const
{
    switch (m_processorMode)
    {
    case IA32eInstructionSet::INTEL_32:
        if (sizePrefix)
            return IntegerEncoding::INTEGER_16BIT;
        return IntegerEncoding::INTEGER_32BIT;

    case IA32eInstructionSet::INTEL_16:
        if (sizePrefix)
            return IntegerEncoding::INTEGER_32BIT;
        return IntegerEncoding::INTEGER_16BIT;

    default:
        // Stop operation
        CHECK_FAIL();
    }
}

void IA32AssemblerStream::encodeOpcode(uint table,
                                const ia32dis::OpcodeEntry* opcode,
                                const Operand& first,
                                const Operand& second,
                                const Operand& third,
                                bool operandSizePrefix,
                                bool addressSizePrefix,
                                bool modrm,
                                IA32OpcodeDatastruct::MODRM modrmMod)
{
    // Write the operand-size/address size opcode and decide on activity mode
    // Calculate operation size
    IntegerEncoding::IntegerEncodingType operandSize =
        getProcessorOperandSize(operandSizePrefix);
    IntegerEncoding::IntegerEncodingType addressSize =
        getProcessorOperandSize(addressSizePrefix);
    if (operandSizePrefix)
        m_binary.appendUint8(ia32dis::gIa32PrefixGroup3OperandSize);
    if (addressSizePrefix)
        m_binary.appendUint8(ia32dis::gIa32PrefixGroup4AddressSize);

    // Write the table index prefix. Used only for two bytes table
    if (table == 1)
        m_binary.appendUint8(ia32dis::gIa32TwoByteEscapeCharacter);

    // Start by encoding opcode
    uint8 opcodeData = opcode->m_prefix;

    // Add internal opcode information (if any)
    // For example push reg32 encoded as 9X
    if ((opcode->m_firstOperand == ia32dis::OPND_ONEBYTES_OPCODE_GP_8) ||
        (opcode->m_firstOperand == ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32))
    {
        CHECK((first.getType() & Operand::OPERAND_REGISTRY_MASK) != 0);
        opcodeData|= (first.getReference1() & (~opcode->m_prefixMask));
    }
    m_binary.appendUint8(opcodeData);

    // Add ModRM (if any)
    if (modrm)
    {
        // TODO!!!
        // Is any prefix changes this tables?
        ia32dis::ModRMTranslationType* modrmTable;
        switch (m_processorMode) {
        case IA32eInstructionSet::INTEL_16:
            modrmTable = &ia32dis::gIa32ModRM16;
            break;
        case IA32eInstructionSet::INTEL_32:
            modrmTable = &ia32dis::gIa32ModRM32;
            break;
        default:
            CHECK_FAIL();
        }

        // Start by filling the modrm data
        IA32OpcodeDatastruct::MODRM modrm =
            fillModrm(modrmMod, opcode, first, second, third);

        // Fix modrm according to filter opcode name
        if (opcode->m_modrm != ia32dis::MODRM_ALL_FILTER)
        {
            if ((opcode->m_modrm & ia32dis::MODRM_MOD_000) != 0)
                modrm.m_bits.m_regOpcode = 0;
            if ((opcode->m_modrm & ia32dis::MODRM_MOD_001) != 0)
                modrm.m_bits.m_regOpcode = 1;
            if ((opcode->m_modrm & ia32dis::MODRM_MOD_010) != 0)
                modrm.m_bits.m_regOpcode = 2;
            if ((opcode->m_modrm & ia32dis::MODRM_MOD_011) != 0)
                modrm.m_bits.m_regOpcode = 3;
            if ((opcode->m_modrm & ia32dis::MODRM_MOD_100) != 0)
                modrm.m_bits.m_regOpcode = 4;
            if ((opcode->m_modrm & ia32dis::MODRM_MOD_101) != 0)
                modrm.m_bits.m_regOpcode = 5;
            if ((opcode->m_modrm & ia32dis::MODRM_MOD_110) != 0)
                modrm.m_bits.m_regOpcode = 6;
            if ((opcode->m_modrm & ia32dis::MODRM_MOD_111) != 0)
                modrm.m_bits.m_regOpcode = 7;
        }

        // Write down the modrm
        m_binary.appendUint8(modrm.m_packed);

        // Write displacement
        uint displacement = (*modrmTable)[modrmMod.m_bits.m_mod][modrmMod.m_bits.m_rm].
                m_displacementLength;

        if (displacement != 0)
        {
            writeDisplacement(first,  displacement);
            writeDisplacement(second, displacement);
            writeDisplacement(third,  displacement);
        }

        if ((*modrmTable)[modrmMod.m_bits.m_mod][modrmMod.m_bits.m_rm].
                m_forceSib)
        {
            // The SIB exist
            // TODO!
            CHECK_FAIL();
        }
    }

    // Add immediate
    writeImmediate(operandSize, addressSize, opcode->m_firstOperand, first);
    writeImmediate(operandSize, addressSize, opcode->m_secondOperand, second);
    writeImmediate(operandSize, addressSize, opcode->m_thridOperand, third);
}

IA32OpcodeDatastruct::MODRM IA32AssemblerStream::fillModrm(
                                      IA32OpcodeDatastruct::MODRM modrmMod,
                                      const ia32dis::OpcodeEntry* opcode,
                                      const Operand& first,
                                      const Operand& second,
                                      const Operand& third)
{
    // Reset modrm
    IA32OpcodeDatastruct::MODRM ret(modrmMod);

    for (uint i = 0; i < 3; i++)
    {
        // Choose the operand number
        ia32dis::OperandType type;
        const Operand* operand = NULL;
        switch (i)
        {
        case 0: type = opcode->m_firstOperand;  operand = &first; break;
        case 1: type = opcode->m_secondOperand; operand = &second; break;
        case 2: type = opcode->m_thridOperand;  operand = &third;  break;
        default: { ASSERT(false); }
        }
        // Another assertion
        ASSERT(operand != NULL);

        // Fill the modrm
        switch (type)
        {
        case ia32dis::OPND_GP_8BIT_MODRM:
        case ia32dis::OPND_GP_16BIT_MODRM:
        case ia32dis::OPND_GP_16_32BIT:
        // case ia32dis::OPND_GP_SEGMENT_MODRM
            CHECK(operand->getReference1() < 8);
            ret.m_bits.m_regOpcode = operand->getReference1();
            break;

        case ia32dis::OPND_MODRM_dWORDPTR:
        case ia32dis::OPND_MODRM_WORDPTR:
        case ia32dis::OPND_MODRM_BYTEPTR:
        case ia32dis::OPND_MODRM_MEM:
            if ((operand->getType() & Operand::OPERAND_MEMORY_TYPE_MASK) !=
                Operand::OPERAND_MEMORY_DIRECT)
            {
                // Use reference value
                CHECK(operand->getReference1() < 8);
                ret.m_bits.m_rm = operand->getReference1();;
            }
            break;
        default:
            break;
        }
    }

    return ret;
}

void IA32AssemblerStream::fillModrmRM(IA32OpcodeDatastruct::MODRM& modrm,
                                      bool operandSizePrefix,
                                      uint regType,
                                      uint registerReference) const
{
    CHECK(modrm.m_bits.m_mod != 3);
    if (((regType & Operand::OPERAND_REGISTRY_MASK) ==
          Operand::OPERAND_32BIT_REG))
    {
        // Check that the registry is correct
        CHECK(registerReference < 8);
        // 32 bit modrm table
        // Mod 0 - Register without displacement
        // Mod 1 - Register with 8 bit displacement
        // Mod 2 - Register with 32 bit displacement
        // Mod 3 - Doesn't channel throw this function
        modrm.m_bits.m_rm = registerReference;

        // R/M #4 stand for SIB
        CHECK(registerReference != 4);
        // In mod #0 R/M #5 is commited for 32 bit displacement
        if (modrm.m_bits.m_mod == 0)
            CHECK(registerReference != 5);
    } else
    {
        // 16 bit modrm table
        CHECK_FAIL();
    }
}

void IA32AssemblerStream::writeImmediate(IntegerEncoding::IntegerEncodingType processorOperandSize,
                                  IntegerEncoding::IntegerEncodingType processorAddressSize,
                                  ia32dis::OperandType ia32disType,
                                  const Operand& compilerOperand)
{
    IntegerEncoding::IntegerEncodingType simpleImmediate =
                            getOperandSize(processorOperandSize,
                                           processorAddressSize,
                                           ia32disType);
    // bool shouldReadSegment = false;

    // Check the operand compiler data and write the immediate
    switch (simpleImmediate)
    {
    case IntegerEncoding::INTEGER_NOT_EXIST:
        return;
    case IntegerEncoding::INTEGER_8BIT:
        if (ia32disType == ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8)
        {
            m_binary.appendUint8((uint8)((int8)compilerOperand.getDisplacement()));
        } else
        {
            m_binary.appendUint8(compilerOperand.getReference1());
        }
        break;
    case IntegerEncoding::INTEGER_16BIT:
        writeUint16(compilerOperand.getReference1());
        break;
    case IntegerEncoding::INTEGER_32BIT:
        if (ia32disType == ia32dis::OPND_IMMEDIATE_OFFSET_DS)
        {
            writeUint32(compilerOperand.getDisplacement());
        } else
        {
            writeUint32(compilerOperand.getReference1());
        }
        break;
    default:
        // Not ready yet!
        CHECK_FAIL();
    }

    // if (shouldReadSegment)
    //    immediate.segment = read16bit(data);
}

void IA32AssemblerStream::writeDisplacement(const IA32AssemblerStream::Operand& compilerOperand,
                                     uint displacementSize)
{
    switch (displacementSize)
    {
    case 1:
        if (compilerOperand.getDisplacementPacking() == IntegerEncoding::INTEGER_8BIT)
        {
            int8 data = (int8)compilerOperand.getDisplacement();
            m_binary.appendUint8((uint8)data);
        }
        break;
    case 2:
        if (compilerOperand.getDisplacementPacking() == IntegerEncoding::INTEGER_16BIT)
        {
            int16 data = (int16)compilerOperand.getDisplacement();
            writeUint16((uint16)data);
        }
        break;
    case 4:
        if (compilerOperand.getDisplacementPacking() == IntegerEncoding::INTEGER_32BIT)
            writeUint32((uint32)compilerOperand.getDisplacement());
        break;
    default:
        // ??
        CHECK_FAIL();
    }

}

void IA32AssemblerStream::writeUint16(uint16 data)
{
    uint8 buffer[sizeof(uint16)];
    cLittleEndian::writeUint16(buffer, data);
    m_binary.appendBuffer(buffer, sizeof(buffer));
}

void IA32AssemblerStream::writeUint32(uint32 data)
{
    uint8 buffer[sizeof(uint32)];
    cLittleEndian::writeUint32(buffer, data);
    m_binary.appendBuffer(buffer, sizeof(buffer));
}

bool IA32AssemblerStream::is32operand(bool operandPrefix) const
{
    ASSERT(m_processorMode == IA32eInstructionSet::INTEL_32);
    return !operandPrefix;
}

bool IA32AssemblerStream::is16operand(bool operandPrefix) const
{
    ASSERT(m_processorMode == IA32eInstructionSet::INTEL_32);
    return operandPrefix;
}

bool IA32AssemblerStream::checkSignedImmediateSize8(int32 value)
{
    return (value >= -0x80) && (value <= 0x7F);
}

bool IA32AssemblerStream::checkSignedImmediateSize16(int32 value)
{
    return (value >= -0x8000) && (value <= 0x7FFF);
}


//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////
// IA32AssemblerStream::Operand

IA32AssemblerStream::Operand::Operand() :
    m_type(NO_OPERAND),
    m_reference1(0),
    m_displacement(0),
    m_displacementEncodingType(IntegerEncoding::INTEGER_NOT_EXIST)
{
}

void IA32AssemblerStream::Operand::readOperand(Parser& parser)
{
    // Read the first word and start comparing it to known values
    // There are couple of operand modes:
    if (parser.isCUnsignedInteger())
    {
        // Unsigned immediate
        m_reference1 = parser.readCUnsignedInteger();
        limitedImmediate();
        readUntilOpcode(parser);
        return;
    }

    // Try to read mangled names

    // Try to read offset locations
    if (parser.peekChar() == '$')
    {
        // Read relative addressing
        parser.readChar();
        parser.readBlanks();
        bool negative = parser.readSignedValue();
        parser.readBlanks();
        m_type = OPERAND_OFFSET_MASK;
        m_displacement = parser.readCUnsignedInteger();
        if (negative)
            m_displacement= -m_displacement;
        // And done.
        readUntilOpcode(parser);
        return;
    }

    // Try to find memory address
    cString word;
    if (parser.peekChar() == '[')
    {
        parser.readChar();
        // TODO!
        //m_type = (m_processorMode == IA32eInstructionSet::INTEL_32) ?
        //            OPERAND_MEMORY_32BIT : OPERAND_MEMORY_16BIT;
        m_type = OPERAND_MEMORY_32BIT;
    } else
    {
        // Otherwise just read a CString word for registers,
        word = parser.readCString();
        parser.readBlanksUntilEOL();

        //////////////////////////////////////////////////////////////////////////
        // Check for memory reference
        // Memory reference must be started with access token: dword/word/byte ptr
        if (word == "dword")
            m_type = OPERAND_MEMORY_32BIT;
        if (word == "word")
            m_type = OPERAND_MEMORY_16BIT;
        if (word == "byte")
            m_type = OPERAND_MEMORY_8BIT;

        // Memory operation
        if (m_type != NO_OPERAND)
        {
            CHECK(parser.readCString() == "ptr");
            parser.readBlanksUntilEOL();
            CHECK(parser.readChar() == '[');
        }
    }


    if (m_type != NO_OPERAND)
    {
        // Read opening memory token [
        parser.readBlanksUntilEOL();

        // There are several of options in memory transaction
        bool isNegative = parser.readSignedValue();
        if (parser.isCUnsignedInteger())
        {
            // Just displacement
            m_type|= OPERAND_MEMORY_DIRECT;
            m_displacement = parser.readCUnsignedInteger();
            // Negiate number
            if (isNegative)
                m_displacement = -m_displacement;
            limitedDisplacement();
            // Change reference1, used in immediates
            m_reference1 = m_displacement;
        } else
        {
            // reg+disp, reg+reg+disp, SIB+disp..
            word = parser.readCString();
            parser.readBlanksUntilEOL();
            // Read the first register
            uint type;
            CHECK(checkRegister(word, type, m_reference1));
            m_type|= type;
            // Check for more elements
            if (parser.peekChar() != ']')
            {
                // Check for appending mark
                char mark = parser.readChar();
                parser.readBlanksUntilEOL();
                CHECK((mark == '+') || (mark == '-'));
                // Check for another register or displacement
                if (parser.isCUnsignedInteger())
                {
                    m_type|= OPERAND_MEMORY_REG_PLUS_DISPLACEMENT;
                    m_displacement = parser.readCUnsignedInteger();
                    // Negiate number
                    if (mark == '-')
                        m_displacement = -m_displacement;
                    limitedDisplacement();
                    parser.readBlanksUntilEOL();
                } else
                {
                    // Not ready yet
                    CHECK_FAIL();
                }
            } else
            {
                // Only a register
                m_type|= OPERAND_MEMORY_REG;
            }
        }

        // Read modrm mod + register in use

        // Read SIB

        CHECK(parser.readChar() == ']');
        parser.readBlanksUntilEOL();

        // And done
        readUntilOpcode(parser);
        return;
    }

    //////////////////////////////////////////////////////////////////////////
    // Registers:  (EAX, EBX, ECX, EDX)
    if (checkRegister(word, m_type, m_reference1))
    {
        // And done
        readUntilOpcode(parser);
        return;
    }

    // Couldn't find the operand
    CHECK_FAIL();
}

uint IA32AssemblerStream::Operand::getType() const
{
    return m_type;
}

uint32 IA32AssemblerStream::Operand::getReference1() const
{
    return m_reference1;
}

int32 IA32AssemblerStream::Operand::getDisplacement() const
{
    return m_displacement;
}

IntegerEncoding::IntegerEncodingType
            IA32AssemblerStream::Operand::getDisplacementPacking() const
{
    return m_displacementEncodingType;
}

void IA32AssemblerStream::Operand::readUntilOpcode(Parser& parser)
{
    if (parser.readBlanksUntilEOL())
        return;

    // TODO! Add comments support
    CHECK(parser.readChar() == ',');

    parser.readBlanksUntilEOL();
}

bool IA32AssemblerStream::Operand::checkRegister(const cString& regname,
                                                 uint& regType,
                                                 uint32& regIndex)
{
    uint32 i = 0;
    for (; i < ia32dis::NUMBER_OF_REGISTERS; i++)
    {
        if (regname == ia32dis::gIa8Registers[i].m_name)
        {
            regType = OPERAND_8BIT_REG;
            regIndex = i;
            return true;
        }

        if (regname == ia32dis::gIa16Registers[i].m_name)
        {
            regType = OPERAND_16BIT_REG;
            regIndex = i;
            return true;
        }

        if (regname == ia32dis::gIa32Registers[i].m_name)
        {
            regType = OPERAND_32BIT_REG;
            regIndex = i;
            return true;
        }

        if (regname == ia32dis::gIa32SIMDRegisters[i].m_name)
        {
            regType = OPERAND_SIMD_REG;
            regIndex = i;
            return true;
        }

        if (regname == ia32dis::gIa32MMXRegisters[i].m_name)
        {
            regType = OPERAND_XMM_REG;
            regIndex = i;
            return true;
        }
    }

    for (i = 0; i < ia32dis::NUMBER_OF_SEGMENTS_REGISTERS; i++)
    {
        if (regname == ia32dis::gIa32SegmentsRegisters[i].m_name)
        {
            regType = OPERAND_SEGMENT_REG;
            regIndex = i;
            return true;
        }
    }

    return false;
}

void IA32AssemblerStream::Operand::limitedImmediate()
{
    if (m_reference1 < 0x80)
        m_type = Operand::OPERAND_IMMEDIATE_8BIT;
    else if (m_reference1 < 0x8000)
        m_type = Operand::OPERAND_IMMEDIATE_16BIT;
    else
        m_type = Operand::OPERAND_IMMEDIATE_32BIT;
}

void IA32AssemblerStream::Operand::limitedDisplacement()
{
    if ((m_displacement < 128) && (m_displacement > -129))
        // -128..127
        m_displacementEncodingType = IntegerEncoding::INTEGER_8BIT;
    // else if ((m_displacement < 128) || (m_displacement > -129))
    //    m_displacementEncodingType = IntegerEncoding::INTEGER_16BIT;
    else
        m_displacementEncodingType = IntegerEncoding::INTEGER_32BIT;
}
