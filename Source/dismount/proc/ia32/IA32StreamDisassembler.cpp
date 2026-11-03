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

// mov to / from a control or a debug register: the processor ignores the mod
// field of the ModR/M byte and takes it as 11 (Intel SDM, MOV CR / MOV DR)
static bool isControlOrDebugMove(const ia32dis::OpcodeEntry* opcode)
{
    return (opcode->m_firstOperand == ia32dis::OPND_CTRL_MODRM) ||
           (opcode->m_secondOperand == ia32dis::OPND_CTRL_MODRM) ||
           (opcode->m_firstOperand == ia32dis::OPND_DBG_MODRM) ||
           (opcode->m_secondOperand == ia32dis::OPND_DBG_MODRM);
}

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
        break;
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
    m_rex.m_packed = 0;

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

        // The REX prefix (64 bit mode) follows the legacy prefixes. When
        // there are several REX prefixes the first one wins, as in ndisasm
        // (the processors ignore a REX prefix which does not immediately
        // precede the opcode, which would make the last one win). A legacy prefix
        // after a REX prefix leaves the REX prefix alone, as an instruction
        // by its own, which is not an instruction: the processor ignores
        // it, ndisasm decodes it separately
        uint rexCount = 0;
        // The mandatory prefix which is a part of the opcode (64 bit mode)
        uint removedMandatory = 0;
        if ((m_type == IA32eInstructionSet::AMD_64) &&
            ((nextByte & ia32dis::AMD64_REX_PREFIX_MASK) == ia32dis::AMD64_REX_PREFIX))
        {
            m_rex.m_packed = nextByte;
            do
            {
                rexCount++;
                testEOS();
                m_stream->streamReadUint8(nextByte);
                appendByte(nextByte);
            } while ((nextByte & ia32dis::AMD64_REX_PREFIX_MASK) ==
                     ia32dis::AMD64_REX_PREFIX);

            for (uint i = 0; i < ia32dis::IA32_NUMBER_OF_PREFIXS; i++)
                if (nextByte == ia32dis::gIa32PrefixTable[i].m_opcode)
                    CHECK_FAIL();
        }

        // Change the encoding type of the operand-size and the address-size
        calculateAddressAndOperandSize(operandSizeOverride, addressSizeOverride,
                                       m_rex, ia32dis::MODE64_LEGACY);

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
            // The three-bytes opcodes: 0F 38 xx and 0F 3A xx
            if ((nextByte == ia32dis::gIa32ThreeBytes38EscapeCharacter) ||
                (nextByte == ia32dis::gIa32ThreeBytes3AEscapeCharacter))
            {
                table = (nextByte == ia32dis::gIa32ThreeBytes38EscapeCharacter) ?
                    ia32dis::gIa32ThreeBytes38OpcodeTable :
                    ia32dis::gIa32ThreeBytes3AOpcodeTable;
                testEOS();
                m_stream->streamReadUint8(nextByte);
                appendByte(nextByte);
                opcodeLength = 3;
            }
        }
        // Scan FPU opcode table
        else if ((nextByte >= ia32dis::gIa32FPUStartEscapeCharacter) &&
                 (nextByte <= ia32dis::gIa32FPUEndEscapeCharacter))
        {
            table = ia32dis::gIa32FPUOpcodeTable;
            testEOS();
            opcodeLength = 1;
        }

        // Scan opcode table. In 64 bit mode the entries of the AMD64 table
        // replace the entries of the IA-32 table which have the same encoding
        const ia32dis::OpcodeEntry* amd64Table = NULL;
        // The memory operands of the FPU have a fixed size in 64 bit mode:
        // neither REX.W nor 66 change them
        const bool isFpu64 = (m_type == IA32eInstructionSet::AMD_64) &&
                             (table == ia32dis::gIa32FPUOpcodeTable);
        if (m_type == IA32eInstructionSet::AMD_64)
        {
            if (table == ia32dis::gIa32OneByteOpcodeTable)
                amd64Table = ia32dis::gAmd64OneByteOpcodeTable;
            else if (table == ia32dis::gIa32TwoBytesOpcodeTable)
                amd64Table = ia32dis::gAmd64TwoBytesOpcodeTable;
        }
        const ia32dis::OpcodeEntry* opcode = NULL;
        // Pre-read of the modrm byte
        IA32OpcodeDatastruct::MODRM modrm;
        modrm.m_packed = 0;
        bool modrmFound = false;

        // Try to find the opcode inside the tables
        for (uint pass = 0; (pass < 2) && (opcode == NULL); pass++)
        {
            const ia32dis::OpcodeEntry* current = (pass == 0) ? amd64Table : table;
            if (current == NULL)
                continue;
            for (uint i = 0; strcmp(current[i].m_opcodeName,
                                    ia32dis::OPCODEEOT) != 0;
                 i++)
            {
                // The entries which are invalid in the processor mode
                if (!isValidInMode(&(current[i])))
                    continue;
                // 90 is nop, but 66 90 is 'xchg ax, ax'. In 64 bit mode REX.B
                // selects r8 ('xchg r8, rax') and REX.W is 'xchg rax, rax'
                if ((table == ia32dis::gIa32OneByteOpcodeTable) &&
                    (operandSizeOverride || (m_rex.m_bits.b != 0) ||
                     (m_rex.m_bits.w != 0)) &&
                    (current[i].m_prefix == 0x90) &&
                    (current[i].m_firstOperand == ia32dis::OPND_NO_OPERAND))
                    continue;
                // An opcode with a mandatory prefix is valid only with the prefix
                uint mandatory = prefixsCount;
                if (current[i].m_mandatoryPrefix != ia32dis::MANDATORY_NONE)
                {
                    for (mandatory = 0; mandatory < prefixsCount; mandatory++)
                        if (prefixs[mandatory] == current[i].m_mandatoryPrefix)
                            break;
                    if (mandatory == prefixsCount)
                        continue;
                }
                if ((current[i].m_prefixMask & nextByte) !=
                    current[i].m_prefix)
                    continue;

                // Parse opcode
                const ia32dis::OpcodeEntry* candidate = &(current[i]);
                // Check for modrm
                if (candidate->m_modrm != ia32dis::MODRM_NO_MODRM)
                {
                    if (!modrmFound)
                    {
                        testEOS();
                        m_stream->streamReadUint8(modrm.m_packed);
                        appendByte(modrm.m_packed);
                        modrmFound = true;
                    }

                    if (!filterModRM(modrm, candidate))
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

                // The mandatory prefix is a part of the opcode, it is not an
                // operand-size or a repeat prefix
                if (mandatory != prefixsCount)
                {
                    prefixsCount--;
                    for (uint j = mandatory; j < prefixsCount; j++)
                        prefixs[j] = prefixs[j + 1];
                    operandSizeOverride = false;
                    for (uint j = 0; j < prefixsCount; j++)
                        if (prefixs[j] == ia32dis::gIa32PrefixGroup3OperandSize)
                            operandSizeOverride = true;
                    calculateAddressAndOperandSize(operandSizeOverride && !isFpu64,
                                                   addressSizeOverride,
                                                   m_rex, isFpu64 ? ia32dis::MODE64_NO_REXW : candidate->m_mode64);
                    if (m_type == IA32eInstructionSet::AMD_64)
                        removedMandatory = 1;
                } else if ((m_type == IA32eInstructionSet::AMD_64) ||
                           (candidate->m_mode64 == ia32dis::MODE64_OPERAND_32_64))
                {
                    // The operand size of 64 bit mode depends on the opcode
                    calculateAddressAndOperandSize(operandSizeOverride && !isFpu64,
                                                   addressSizeOverride,
                                                   m_rex, isFpu64 ? ia32dis::MODE64_NO_REXW : candidate->m_mode64);
                }
                opcode = candidate;
                break;
            }
        }

        // Test whether the opcode is not found
        if (opcode == NULL)
            CHECK_FAIL();

        // The m32 memory operand of the FPU (fadd, fld, fiadd, fistp...) is
        // 32 bits whatever the 66 prefix or the 16 bit mode say. The operand
        // size still selects the layout of the environment (fldenv, fnsave)
        if ((table == ia32dis::gIa32FPUOpcodeTable) &&
            (opcode->m_firstOperand == ia32dis::OPND_MODRM_dWORDPTR))
            m_operandSize = IntegerEncoding::INTEGER_32BIT;

        // The mod field of mov cr / dr is ignored: the register form, which
        // has neither SIB nor displacement
        if (modrmFound && isControlOrDebugMove(opcode))
            modrm.m_bits.m_mod = 3;

        // Try to read displacement and SIB.
        IA32OpcodeDatastruct::SIB sib;
        sib.m_packed = 0;
        bool sibExist = false;
        uint displacementLength = 0;
        IA32OpcodeDatastruct::DisplacementType displacement = 0;
        if (modrmFound)
        {
            ia32dis::ModRMTranslationType* modrmTable;
            // TODO!!!
            // Is any prefix changes this tables?
            // The address size (67 prefix) selects the table. The 64 bit
            // addressing has the layout of the 32 bit one (the registers
            // are the only difference)
            switch (m_addressSize) {
            case IntegerEncoding::INTEGER_16BIT:
                modrmTable = &ia32dis::gIa32ModRM16;
                break;
            case IntegerEncoding::INTEGER_32BIT:
            case IntegerEncoding::INTEGER_64BIT:
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
        IA32OpcodeDatastruct::ImmediateType immediate = {0,0,0};
        bool firstImmediate =
            readImmediate(*m_stream, opcode->m_firstOperand,  immediateLength, immediate);
        // A second immediate (enter imm16, imm8) has its own slot
        IA32OpcodeDatastruct::ImmediateType secondImmediate = {0,0,0};
        readImmediate(*m_stream, opcode->m_secondOperand, immediateLength,
                      firstImmediate ? secondImmediate : immediate);
        immediate.secondOffset = secondImmediate.offset;
        readImmediate(*m_stream, opcode->m_thridOperand,  immediateLength, immediate);

        // Generate the IA32Opcode. The opcode size counts all the bytes
        // between the legacy prefixes and the last byte of the opcode (the
        // REX prefixes and the mandatory prefix are among them)
        // TODO: Fix the way this handles instructions with ModR\M (LEA etc, DS:\SS: + proper effective address calculations)
        ret = OpcodePtr(new IA32Opcode(m_type,
                                m_shouldUseAddress, m_streamAddress,
                                m_opcodeData,
                                prefixs, prefixsCount,
                                m_rex,
                                opcode,
                                opcodeLength + rexCount + removedMandatory,
                                modrm, sibExist, sib,
                                displacementLength, displacement,
                                immediateLength, immediate,
                                m_operandSize, m_addressSize));
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
                                    bool addressSizeOverride,
                                    const IA32OpcodeDatastruct::REX& rex,
                                    uint mode64)
{
    switch (m_type)
    {
    case IA32eInstructionSet::INTEL_16:
        if (operandSizeOverride || (mode64 == ia32dis::MODE64_OPERAND_32_64))
            m_operandSize = IntegerEncoding::INTEGER_32BIT;
        else
            m_operandSize = IntegerEncoding::INTEGER_16BIT;

        if (addressSizeOverride)
            m_addressSize = IntegerEncoding::INTEGER_32BIT;
        else
            m_addressSize = IntegerEncoding::INTEGER_16BIT;

        break;
    case IA32eInstructionSet::INTEL_32:
        if (operandSizeOverride && (mode64 != ia32dis::MODE64_OPERAND_32_64))
            m_operandSize = IntegerEncoding::INTEGER_16BIT;
        else
            m_operandSize = IntegerEncoding::INTEGER_32BIT;

        if (addressSizeOverride)
            m_addressSize = IntegerEncoding::INTEGER_16BIT;
        else
            m_addressSize = IntegerEncoding::INTEGER_32BIT;
        break;
    case IA32eInstructionSet::AMD_64:
        // The address size is 64 bits, 32 bits with the 67 prefix
        if (addressSizeOverride)
            m_addressSize = IntegerEncoding::INTEGER_32BIT;
        else
            m_addressSize = IntegerEncoding::INTEGER_64BIT;

        // The operand size is 32 bits, 64 bits with REX.W (which wins over
        // the 66 prefix) and 16 bits with the 66 prefix. Some opcodes have
        // another default
        switch (mode64)
        {
        case ia32dis::MODE64_FORCE_64:
            m_operandSize = IntegerEncoding::INTEGER_64BIT;
            break;
        case ia32dis::MODE64_DEFAULT_64:
            // REX.W wins over the 66 prefix here too
            m_operandSize = (operandSizeOverride && (rex.m_bits.w == 0)) ?
                IntegerEncoding::INTEGER_16BIT : IntegerEncoding::INTEGER_64BIT;
            break;
        case ia32dis::MODE64_NO_REXW:
            m_operandSize = operandSizeOverride ?
                IntegerEncoding::INTEGER_16BIT : IntegerEncoding::INTEGER_32BIT;
            break;
        case ia32dis::MODE64_OPERAND_32_64:
            // 66 does not matter
            m_operandSize = (rex.m_bits.w != 0) ?
                IntegerEncoding::INTEGER_64BIT : IntegerEncoding::INTEGER_32BIT;
            break;
        default:
            if (rex.m_bits.w != 0)
                m_operandSize = IntegerEncoding::INTEGER_64BIT;
            else if (operandSizeOverride)
                m_operandSize = IntegerEncoding::INTEGER_16BIT;
            else
                m_operandSize = IntegerEncoding::INTEGER_32BIT;
            break;
        }
        break;
    default:
        CHECK_FAIL();
    }
}

bool IA32StreamDisassembler::filterModRM(
                const IA32OpcodeDatastruct::MODRM& modrm,
                const ia32dis::OpcodeEntry* opcode) const
{
    if ((opcode->m_modrm & ia32dis::MODRM_EXACT_FLAG) != 0)
        return modrm.m_packed == ((opcode->m_modrm >> 16) & 0xFF);

    bool ok = true;
    switch (isControlOrDebugMove(opcode) ? 3 : modrm.m_bits.m_mod)
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

    // A reg field which names no segment or control register (8C/8E with reg
    // 6-7, 0F 20/22 with cr1 or cr5-cr7) is an undefined encoding
    if ((opcode->m_firstOperand == ia32dis::OPND_GP_SEGMENT_MODRM) ||
        (opcode->m_secondOperand == ia32dis::OPND_GP_SEGMENT_MODRM))
        ok = ok && (modrm.m_bits.m_regOpcode < ia32dis::NUMBER_OF_SEGMENTS_REGISTERS);
    if ((opcode->m_firstOperand == ia32dis::OPND_CTRL_MODRM) ||
        (opcode->m_secondOperand == ia32dis::OPND_CTRL_MODRM))
    {
        if (m_type == IA32eInstructionSet::AMD_64)
        {
            // cr0, cr2, cr3, cr4 and cr8 (REX.R and the number 0)
            uint number = modrm.m_bits.m_regOpcode | (m_rex.m_bits.r << 3);
            ok = ok && ((number == 0) || (number == 2) || (number == 3) ||
                        (number == 4) || (number == 8));
        } else
        {
            ok = ok && (modrm.m_bits.m_regOpcode != 1) &&
                 (modrm.m_bits.m_regOpcode < ia32dis::NUMBER_OF_CONTROL_REGISTERS);
        }
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
    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
    case ia32dis::OPND_IMMEDIATE_DS:
        // rel16/rel32, according to the operand size. With a 64 bit operand
        // size the immediate is 32 bits (sign extended)
        simpleImmediate = m_operandSize;
        if (simpleImmediate == IntegerEncoding::INTEGER_64BIT)
            simpleImmediate = IntegerEncoding::INTEGER_32BIT;
        break;

    case ia32dis::OPND_IMMEDIATE_DQ:
        simpleImmediate = m_operandSize;
        break;

    case ia32dis::OPND_MEMREF_OFFSET_DS:
        simpleImmediate = m_addressSize;
        break;

    case ia32dis::OPND_IMMEDIATE_OFFSET_FAR:
        // offset16/32 (operand size) and the segment
        simpleImmediate = m_operandSize;
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
    case IntegerEncoding::INTEGER_64BIT:
        immediateLength = 8;
        immediate.offset = read64bit(data);
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

uint64 IA32StreamDisassembler::read64bit(basicInput& data)
{
    uint8 buffer[sizeof(uint64)];
    if (data.pipeRead(&buffer, sizeof(buffer)) != sizeof(buffer))
        XSTL_THROW(DisassemblerEndOfStreamException);
    appendBuffer(buffer, sizeof(buffer));
    // The IA32Disassembler is little-endian engine
    return cLittleEndian::readUint64(buffer);
}

bool IA32StreamDisassembler::isValidInMode(const ia32dis::OpcodeEntry* opcode) const
{
    // The 16 bit operand size is not an instruction of some opcodes (bswap)
    if ((opcode->m_mode64 == ia32dis::MODE64_NO_16BIT) &&
        (m_operandSize == IntegerEncoding::INTEGER_16BIT))
        return false;
    if (m_type == IA32eInstructionSet::AMD_64)
        return opcode->m_mode64 != ia32dis::MODE64_INVALID;
    return opcode->m_mode64 != ia32dis::MODE64_ONLY;
}
