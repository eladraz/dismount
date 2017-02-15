#include "dismount/dismount.h"
/*
 * IA32StreamDisassembler.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/stream/basicIO.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/InvalidOpcodeByte.h"
#include "dismount/InvalidOpcodeFormatter.h"
#include "dismount/DisassemblerEndOfStreamException.h"
#include "dismount/DisassemblerInvalidOpcodeException.h"
#include "dismount/proc/ia32/IA32Opcode.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"
#include "dismount/proc/ia32/IA32StreamDisassembler.h"
#include "dismount/proc/ia32/IA32OpcodeDatastruct.h"
#include "dismount/proc/ia32/IA32IntelNotation.h"

IA32StreamDisassembler::IA32StreamDisassembler(
    IA32eInstructionSet::DisassemblerTypes type,
    const BasicInputPtr& disassemblerStream,
    bool shouldUseAddress,
    const ProcessorAddress& streamAddress,
    bool shouldOpcodeFaultTolerantEnabled) :
        m_type(type),
        m_stream(disassemblerStream),
        m_shouldOpcodeFaultTolerantEnabled(shouldOpcodeFaultTolerantEnabled),
        m_shouldUseAddress(shouldUseAddress),
        m_streamAddress(streamAddress)
{
    CHECK(!m_stream.isEmpty());
    switch (m_type)
    {
    case IA32eInstructionSet::INTEL_16:
        if (m_shouldUseAddress)
            CHECK(streamAddress.getAddressType() == ProcessorAddress::PROCESSOR_20);
        break;
    case IA32eInstructionSet::INTEL_32:
        if (m_shouldUseAddress)
            CHECK(streamAddress.getAddressType() == ProcessorAddress::PROCESSOR_32);
        break;
    case IA32eInstructionSet::AMD_64:
        if (m_shouldUseAddress)
            CHECK(streamAddress.getAddressType() == ProcessorAddress::PROCESSOR_64);
        // TODO! Not ready yet.
        CHECK_FAIL();
    default:
        CHECK_FAIL();
    }

    // Change the page-size into the average opcode-length
    m_opcodeData.setPageSize(6);
}

OpcodePtr IA32StreamDisassembler::next()
{
    // Erase all previous buffering
    m_opcodeData.changeSize(0);
    m_operandSize = IntegerEncoding::INTEGER_NOT_EXIST;
    m_addressSize = IntegerEncoding::INTEGER_NOT_EXIST;

    // Get the position of the stream
    uint streamInstructionPointer = m_stream->getPointer();
    // Construct new IA32 opcode
    OpcodePtr ret(NULL);

    XSTL_TRY
    {
        // Cache for next byte reading, use to share between mode transactions
        uint8 nextByte;

        // Start by reading all prefixes
        uint8 prefixs[ia32dis::MAX_PREFIX];
        uint  prefixsCount = 0;

        // Decode prefixs
        bool operandSizeOverride = false;
        bool addressSizeOverride = false;
        bool endPrefix;

        do
        {
            testEOS();
            m_stream->streamReadUint8(nextByte);
            appendByte(nextByte);

            endPrefix = true;
            for (uint i = 0; i < ia32dis::IA32_NUMBER_OF_PREFIXS; i++)
                if (nextByte == ia32dis::gIa32PrefixTable[i].m_opcode)
                {
                    prefixs[prefixsCount] = nextByte;
                    prefixsCount++;
                    // Eat the prefix which change the instruction encoding
                    if (nextByte == ia32dis::gIa32PrefixGroup3OperandSize)
                        operandSizeOverride = true;
                    if (nextByte == ia32dis::gIa32PrefixGroup4AddressSize)
                        addressSizeOverride = true;
                    endPrefix = false;
                }
        } while (!endPrefix);

        // TODO!
        // 64bit processor might encode the REX prefix here.

        // Change the encoding type of the operand-size and the address-size
        calculateAddressAndOperandSize(operandSizeOverride, addressSizeOverride);

        // The next character mark which decoding table the disassembler should
        // use. 0x0F marks two-byte opcode table, and 0xD8-0xDF marks an FPU opcode.
        // TODO! There are also floating-point processor opcode table and other
        //       CPU instruction-set extension.
        const ia32dis::OpcodeEntry* table = ia32dis::gIa32OneByteOpcodeTable;
        uint8 opcodeLength = 1;
        // Scan two-word opcode table.
        if (nextByte == ia32dis::gIa32TwoByteEscapeCharacter)
        {
            table = ia32dis::gIa32TwoBytesOpcodeTable;
            testEOS();
            m_stream->streamReadUint8(nextByte);
            appendByte(nextByte);
            opcodeLength = 2;
        }
        // Scan FPU opcode table
        else if ((nextByte >= ia32dis::gIa32FPUStartEscapeCharacter) &&
                 (nextByte <= ia32dis::gIa32FPUEndEscapeCharacter))
        {
            table = ia32dis::gIa32FPUOpcodeTable;
            testEOS();
            opcodeLength = 1;
        }

        // Scan opcode table
        uint i = 0;
        const ia32dis::OpcodeEntry* opcode;
        // Pre-read of the modrm byte
        IA32OpcodeDatastruct::MODRM modrm;
        bool modrmFound = false;

        // Try to find the opcode inside the table
        for (; strcmp(table[i].m_opcodeName,
                      ia32dis::OPCODEEOT) != 0;
             i++)
        {
            if ((table[i].m_prefixMask & nextByte) ==
                table[i].m_prefix)
            {
                // Parse opcode
                opcode = &(table[i]);
                // Check for modrm
                if (opcode->m_modrm != ia32dis::MODRM_NO_MODRM)
                {
                    if (!modrmFound)
                    {
                        testEOS();
                        m_stream->streamReadUint8(modrm.m_packed);
                        appendByte(modrm.m_packed);
                        modrmFound = true;
                    }

                    if (!filterModRM(modrm, opcode))
                        continue;
                } else
                {
                    if (modrmFound)
                    {
                        // The opcode doesn't contain modrm. There is an error
                        // with the table!
                        ASSERT(false);
                        CHECK_FAIL();
                    }
                }

                // Try to read displacement and SIB.
                IA32OpcodeDatastruct::SIB sib;
                bool sibExist = false;
                uint displacementLength = 0;
                IA32OpcodeDatastruct::DisplacementType displacement = 0;
                if (modrmFound)
                {
                    ia32dis::ModRMTranslationType* modrmTable;
                    // TODO!!!
                    // Is any prefix changes this tables?
                    switch (m_type) {
                    case IA32eInstructionSet::INTEL_16:
                        modrmTable = &ia32dis::gIa32ModRM16;
                        break;
                    case IA32eInstructionSet::INTEL_32:
                        modrmTable = &ia32dis::gIa32ModRM32;
                        break;
                    default:
                        CHECK_FAIL();
                    }

                    displacementLength =
                        (*modrmTable)[modrm.m_bits.m_mod][modrm.m_bits.m_rm].
                            m_displacementLength;
                    sibExist =
                        (*modrmTable)[modrm.m_bits.m_mod][modrm.m_bits.m_rm].
                            m_forceSib;

                    // Read SIB (Scale Index Base) if needed
                    if (sibExist)
                    {
                        testEOS();
                        m_stream->streamReadUint8(sib.m_packed);
                        appendByte(sib.m_packed);

                        // Check for MOD 00
                        if (modrm.m_bits.m_mod == 0)
                        {
                            // Check for SIB with extra displacement: Base = 5
                            if (sib.m_bits.m_base == ia32dis::IA32_GP32_EBP)
                            {
                                // [scaled index] + disp32
                                displacementLength = 4;
                            }
                        }
                    }

                    // Read displacement
                    displacement =
                        readDisplacement(*m_stream, displacementLength);
                }

                // Read immediate, cast all possible operands
                uint immediateLength = 0;
                IA32OpcodeDatastruct::ImmediateType immediate = {0,0};
                readImmediate(*m_stream, opcode->m_firstOperand,  immediateLength, immediate);
                readImmediate(*m_stream, opcode->m_secondOperand, immediateLength, immediate);
                readImmediate(*m_stream, opcode->m_thridOperand,  immediateLength, immediate);

                // Generate the IA32Opcode
                // TODO: Fix the way this handles instructions with ModR\M (LEA etc, DS:\SS: + proper effective address calculations)
                ret = OpcodePtr(new IA32Opcode(m_type,
                                        m_shouldUseAddress, m_streamAddress,
                                        m_opcodeData,
                                        prefixs, prefixsCount,
                                        opcode,
                                        opcodeLength,
                                        modrm, sibExist, sib,
                                        displacementLength, displacement,
                                        immediateLength, immediate,
                                        m_operandSize, m_addressSize));
                // Opcode found, terminate try-catch block
                break;
            }
        }

        // Test whether the opcode is not found
        if (strcmp(table[i].m_opcodeName, ia32dis::OPCODEEOT) == 0)
            CHECK_FAIL();
    }
    XSTL_CATCH(DisassemblerEndOfStreamException&)
    {
        // Rethorw the exception. NOTE: The stream come to it's end.
        XSTL_THROW(DisassemblerEndOfStreamException);
    }
    XSTL_CATCH_ALL
    {
        // There was an error reading the opcode. Seeking back all read
        // characters.
        m_stream->seek(streamInstructionPointer, basicInput::IO_SEEK_SET);

        // Should I throw exception or should I generate InvalidOpcodeByte class
        if (!m_shouldOpcodeFaultTolerantEnabled)
        {
            XSTL_THROW(DisassemblerInvalidOpcodeException);
        }

        uint8 data;
        m_opcodeData.changeSize(0);
        // Read a single character and generate invalid opcode, if the stream
        // reach EOS exception will be raised here.
        m_stream->streamReadUint8(data);
        m_opcodeData.changeSize(1);
        m_opcodeData[0] = data;
        ret = OpcodePtr(new InvalidOpcodeByte(data,
                            m_shouldUseAddress,
                            m_streamAddress));
    }

    // End of instruction parsing.

    // Increase instruction address if needed
    if (m_shouldUseAddress)
    {
        m_streamAddress = m_streamAddress + m_opcodeData.getSize();
    }

    // Done!
    return ret;
}

void IA32StreamDisassembler::jumpToAddress(ProcessorAddress address, uint rawAddress)
{
    // Seek the stream to either the absolute raw offset or the calculated difference
    if (0 != rawAddress)
        m_stream->seek(rawAddress, basicInput::IO_SEEK_SET);
    else
        m_stream->seek((uint)(address.getAddress() - m_streamAddress.getAddress()), basicInput::IO_SEEK_CUR);

    // Set the stream address
    m_streamAddress.setAddress(address.getAddress());
}

OpcodePtr IA32StreamDisassembler::jumpToAddressAndNext(ProcessorAddress address)
{
    jumpToAddress(address);
    return next();
}

bool IA32StreamDisassembler::isEndOfStream()
{
    return m_stream->isEOS();
}

bool IA32StreamDisassembler::getNextOpcodeLocation(ProcessorAddress& address)
{
    if (!m_shouldUseAddress)
        return false;
    address = m_streamAddress;
    return true;
}

OpcodeFormatterPtr IA32StreamDisassembler::getOpcodeFormat(
                                const OpcodePtr& instruction,
                                OpcodeDataFormatter& dataFormatter,
                                int formatType)
{
    // Handle invalid opcode format
    if (instruction->getType() ==
        OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE)
        return OpcodeFormatterPtr(new InvalidOpcodeFormatter(instruction,
                                                             dataFormatter));

    CHECK(instruction->getType() == getType());
    return OpcodeFormatterPtr(new IA32IntelNotation(instruction,
                                                    dataFormatter));
}

OpcodeSubsystems::DisassemblerType IA32StreamDisassembler::getType() const
{
    switch (m_type)
    {
    case IA32eInstructionSet::INTEL_16:
        return OpcodeSubsystems::DISASSEMBLER_INTEL_16;
    case IA32eInstructionSet::INTEL_32:
        return OpcodeSubsystems::DISASSEMBLER_INTEL_32;
    case IA32eInstructionSet::AMD_64:
        return OpcodeSubsystems::DISASSEMBLER_AMD_64;
    }
    // Should never reach here
    CHECK_FAIL();
}

void IA32StreamDisassembler::testEOS()
{
    if (m_stream->isEOS())
        XSTL_THROW(DisassemblerEndOfStreamException);
}

void IA32StreamDisassembler::calculateAddressAndOperandSize(
                                    bool operandSizeOverride,
                                    bool addressSizeOverride)
{
    switch (m_type)
    {
    case IA32eInstructionSet::INTEL_16:
        if (operandSizeOverride)
            m_operandSize = IntegerEncoding::INTEGER_32BIT;
        else
            m_operandSize = IntegerEncoding::INTEGER_16BIT;

        if (addressSizeOverride)
            m_addressSize = IntegerEncoding::INTEGER_32BIT;
        else
            m_addressSize = IntegerEncoding::INTEGER_16BIT;

        break;
    case IA32eInstructionSet::INTEL_32:
        if (operandSizeOverride)
            m_operandSize = IntegerEncoding::INTEGER_16BIT;
        else
            m_operandSize = IntegerEncoding::INTEGER_32BIT;

        if (addressSizeOverride)
            m_addressSize = IntegerEncoding::INTEGER_16BIT;
        else
            m_addressSize = IntegerEncoding::INTEGER_32BIT;
        break;
    case IA32eInstructionSet::AMD_64:
    default:
        // TODO! Not ready yet.
        CHECK_FAIL();
    }
}

bool IA32StreamDisassembler::filterModRM(
                const IA32OpcodeDatastruct::MODRM& modrm,
                const ia32dis::OpcodeEntry* opcode) const
{
    bool ok = true;
    switch (modrm.m_bits.m_mod)
    {
    case 0:
    case 1:
    case 2:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_MEMORY) != 0; break;
    case 3:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_REGISTERS) != 0; break;
    }

    if (ok) switch (modrm.m_bits.m_regOpcode)
    {
    case 0:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_000) != 0; break;
    case 1:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_001) != 0; break;
    case 2:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_010) != 0; break;
    case 3:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_011) != 0; break;
    case 4:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_100) != 0; break;
    case 5:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_101) != 0; break;
    case 6:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_110) != 0; break;
    case 7:
        ok = (opcode->m_modrm & ia32dis::MODRM_MOD_111) != 0; break;
    }

    return ok;
}

void IA32StreamDisassembler::appendByte(uint8 ops)
{
    m_opcodeData.changeSize(m_opcodeData.getSize() + 1);
    m_opcodeData[m_opcodeData.getSize() - 1] = ops;
}

void IA32StreamDisassembler::appendBuffer(uint8* ops, uint length)
{
    // Append the displacement
    uint pos = m_opcodeData.getSize();
    m_opcodeData.changeSize(pos + length);
    for (uint j = 0; j < length; j++)
        m_opcodeData[pos + j] = ops[j];
}

IA32OpcodeDatastruct::DisplacementType
    IA32StreamDisassembler::readDisplacement(basicInput& data,
                                             uint displacementLength)
{
    switch (displacementLength)
    {
    case 0: return 0;
    case 1: return read8bit(data);
    case 2: return read16bit(data);
    case 4: return read32bit(data);
    case 8:
    default:
        // Not ready yet, and as far as I remember displacement
        // is relative to RIP and limited to 32bit only.
        CHECK_FAIL();
    }
}

bool IA32StreamDisassembler::readImmediate(basicInput& data,
                   ia32dis::OperandType type,
                   uint&   immediateLength,
                   IA32OpcodeDatastruct::ImmediateType& immediate)
{
    bool shouldReadSegment = false;
    IntegerEncoding::IntegerEncodingType simpleImmediate =
        IntegerEncoding::INTEGER_NOT_EXIST;

    switch (type)
    {
    case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
    case ia32dis::OPND_IMMEDIATE_8BIT:
        simpleImmediate = IntegerEncoding::INTEGER_8BIT;
        break;
    case ia32dis::OPND_IMMEDIATE_16BIT:
        simpleImmediate = IntegerEncoding::INTEGER_16BIT;
        break;
    case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
        simpleImmediate = IntegerEncoding::INTEGER_32BIT;
        break;

    case ia32dis::OPND_IMMEDIATE_DS:
        simpleImmediate = m_operandSize;
        break;

    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
    case ia32dis::OPND_MEMREF_OFFSET_DS:
        simpleImmediate = m_addressSize;
        break;

    case ia32dis::OPND_IMMEDIATE_OFFSET_FAR:
        simpleImmediate = m_addressSize;
        shouldReadSegment = true;
        break;
    default:
        //CHECK_FAIL();
        break;
    }

    // Read the immediate
    switch (simpleImmediate)
    {
    case IntegerEncoding::INTEGER_NOT_EXIST: return false;
    case IntegerEncoding::INTEGER_8BIT:
        immediateLength = 1;
        immediate.offset = read8bit(data);
        break;
    case IntegerEncoding::INTEGER_16BIT:
        immediateLength = 2;
        immediate.offset = read16bit(data);
        break;
    case IntegerEncoding::INTEGER_32BIT:
        immediateLength = 4;
        immediate.offset = read32bit(data);
        break;
    default:
        // Not ready yet!
        CHECK_FAIL();
    }

    if (shouldReadSegment)
        immediate.segment = read16bit(data);

    return true;
}

uint8 IA32StreamDisassembler::read8bit(basicInput& data)
{
    uint8 ret;
    if (data.pipeRead(&ret, sizeof(uint8)) == 0)
        XSTL_THROW(DisassemblerEndOfStreamException);
    appendByte(ret);
    return ret;
}

uint16 IA32StreamDisassembler::read16bit(basicInput& data)
{
    uint8 buffer[sizeof(uint16)];
    if (data.pipeRead(&buffer, sizeof(buffer)) != sizeof(buffer))
        XSTL_THROW(DisassemblerEndOfStreamException);
    appendBuffer(buffer, sizeof(buffer));
    return cLittleEndian::readUint16(buffer);
}

uint32 IA32StreamDisassembler::read32bit(basicInput& data)
{
    uint8 buffer[sizeof(uint32)];
    if (data.pipeRead(&buffer, sizeof(buffer)) != sizeof(buffer))
        XSTL_THROW(DisassemblerEndOfStreamException);
    appendBuffer(buffer, sizeof(buffer));
    // The IA32Disassembler is little-endian engine
    return cLittleEndian::readUint32(buffer);
}
