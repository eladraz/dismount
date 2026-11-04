#include "dismount/dismount.h"
/*
 * ArmStreamDisassembler.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/array.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/stream/basicIO.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/InvalidOpcodeByte.h"
#include "dismount/InvalidOpcodeFormatter.h"
#include "dismount/DisassemblerEndOfStreamException.h"
#include "dismount/DisassemblerInvalidOpcodeException.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmNotation.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"

using namespace armdis;

namespace {

/*
 * The tables of the instruction sets, in the search order
 */
const ArmEncodingEntry* const gA64Tables[] = {
    gArm64BaseEncodingTable, gArm64SimdEncodingTable, gArm64SveEncodingTable,
    gArm64SmeEncodingTable, NULL
};
const ArmEncodingEntry* const gA32Tables[] = {
    gArmA32EncodingTable, gArmA32SimdEncodingTable, NULL
};
const ArmEncodingEntry* const gT16Tables[] = {
    gArmT16EncodingTable, NULL
};
const ArmEncodingEntry* const gT32Tables[] = {
    gArmT32EncodingTable, gArmA32SimdEncodingTable, NULL
};

/*
 * An index of the tables of one instruction set (and instruction size) by
 * the top byte of the instruction: the entries which can match a byte value,
 * in the order of the tables. The first matching entry of a bucket is the
 * first matching entry of the tables.
 */
class DecodeIndex {
public:
    DecodeIndex(const ArmEncodingEntry* const* tables, uint shift) :
        m_shift(shift)
    {
        for (uint key = 0; key < BUCKETS; key++)
        {
            for (const ArmEncodingEntry* const* table = tables; *table != NULL; table++)
            {
                for (const ArmEncodingEntry* entry = *table; entry->m_mnemonic != NULL; entry++)
                {
                    if ((entry->m_flags & ARM_ENTRY_ASSEMBLE_ONLY) != 0)
                        continue;
                    uint32 keyMask = (entry->m_mask >> shift) & 0xff;
                    uint32 keyValue = ((entry->m_value & entry->m_mask) >> shift) & 0xff;
                    if ((key & keyMask) == keyValue)
                        m_buckets[key].append(entry);
                }
            }
        }
    }

    // Return the entries which can match the bits
    const cArray<const ArmEncodingEntry*>& getBucket(uint32 encoding) const
    {
        return m_buckets[(encoding >> m_shift) & 0xff];
    }

private:
    enum { BUCKETS = 256 };
    // The position of the key byte
    uint m_shift;
    cArray<const ArmEncodingEntry*> m_buckets[BUCKETS];
};

// The index of each table group, built on the first use
const DecodeIndex& getIndex(ArmInstructionSet::DisassemblerTypes type, uint size)
{
    switch (type)
    {
    case ArmInstructionSet::ARM_A64:
    {
        static const DecodeIndex index(gA64Tables, 24);
        return index;
    }
    case ArmInstructionSet::ARM_A32:
    {
        static const DecodeIndex index(gA32Tables, 20);
        return index;
    }
    case ArmInstructionSet::ARM_T32:
        if (size == 2)
        {
            static const DecodeIndex index(gT16Tables, 8);
            return index;
        } else
        {
            static const DecodeIndex index(gT32Tables, 24);
            return index;
        }
    }
    CHECK_FAIL();
}

// Return the condition of an instruction: the condition part of its entry
// (the A64 b.cond), AL for the others
// TODO: A32 takes it from bits 28-31 and T32 from the it block
uint getCondition(const ArmEncodingEntry* entry, uint32 encoding)
{
    for (uint i = 0; i < ARM_MAX_OPERAND_SPECS; i++)
    {
        if (entry->m_operands[i].m_kind == ARM_KIND_NONE)
            break;
        if (entry->m_operands[i].m_kind == ARM_KIND_CONDITION_MNEMONIC)
            return ArmOperandCodec::getField(encoding, entry->m_operands[i].m_position, 4);
    }
    return ArmOpcode::CONDITION_ALWAYS;
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////

ArmStreamDisassembler::ArmStreamDisassembler(
    ArmInstructionSet::DisassemblerTypes type,
    const BasicInputPtr& disassemblerStream,
    bool shouldUseAddress,
    const ProcessorAddress& streamAddress,
    bool shouldOpcodeFaultTolerantEnabled,
    const ArmFeatureSet& features) :
        m_type(type),
        m_features(features),
        m_stream(disassemblerStream),
        m_shouldUseAddress(shouldUseAddress),
        m_address(streamAddress),
        m_shouldOpcodeFaultTolerantEnabled(shouldOpcodeFaultTolerantEnabled),
        m_itState(0),
        m_pendingInvalidBytes(0)
{
    CHECK(!m_stream.isEmpty());
    if (m_shouldUseAddress)
    {
        ProcessorAddress::ProcessorAddressType expected =
            (type == ArmInstructionSet::ARM_A64) ? ProcessorAddress::PROCESSOR_64 :
                                                   ProcessorAddress::PROCESSOR_32;
        CHECK(streamAddress.getAddressType() == expected);
    }
}

const ArmEncodingEntry* const* ArmStreamDisassembler::getTables(
                ArmInstructionSet::DisassemblerTypes type, uint size)
{
    switch (type)
    {
    case ArmInstructionSet::ARM_A64:
        return gA64Tables;
    case ArmInstructionSet::ARM_A32:
        return gA32Tables;
    case ArmInstructionSet::ARM_T32:
        return (size == 2) ? gT16Tables : gT32Tables;
    }
    CHECK_FAIL();
}

// Decode the bits into a new opcode, NULL if they are invalid. The caller owns
// the opcode
static ArmOpcode* decodeRaw(ArmInstructionSet::DisassemblerTypes type,
                            uint32 encoding,
                            uint size,
                            uint64 address,
                            const ArmFeatureSet& features,
                            uint itState)
{
    const DecodeIndex& index = getIndex(type, size);
    const cArray<const ArmEncodingEntry*>& bucket = index.getBucket(encoding);

    ArmCodecContext context;
    context.m_type = type;
    context.m_address = address;
    context.m_features = features;

    ProcessorAddress opcodeAddress(
        (type == ArmInstructionSet::ARM_A64) ? ProcessorAddress::PROCESSOR_64 :
                                               ProcessorAddress::PROCESSOR_32,
        (ProcessorAddress::uintAddress)address);

    for (uint i = 0; i < bucket.getSize(); i++)
    {
        const ArmEncodingEntry* entry = bucket[i];
        if ((encoding & entry->m_mask) != (entry->m_value & entry->m_mask))
            continue;
        if (!features.has((ArmFeatures::Feature)entry->m_feature))
            continue;
        if ((entry->m_condition != NULL) && !entry->m_condition(encoding))
            continue;
        // TODO: T32 searches the A32 floating point and Advanced SIMD table
        // with the A32 form of the encoding

        context.m_entry = entry;
        // Decode the operands first: an operand with a reserved field means
        // that the encoding is not this entry
        ArmDecodedOperand operands[ArmOpcode::MAX_OPERANDS];
        uint operandsCount = 0;
        cString mnemonic(entry->m_mnemonic);
        bool isValid = true;
        for (uint j = 0; j < ARM_MAX_OPERAND_SPECS; j++)
        {
            const ArmOperandSpec& spec = entry->m_operands[j];
            if (spec.m_kind == ARM_KIND_NONE)
                break;
            if (ArmOperandCodec::isMnemonicPart(spec))
            {
                mnemonic += ArmOperandCodec::decodeMnemonicPart(spec, encoding, context);
                continue;
            }
            if (ArmOperandCodec::isHidden(spec))
                continue;
            if (!ArmOperandCodec::decode(spec, encoding, context, operands[operandsCount]))
            {
                isValid = false;
                break;
            }
            operandsCount++;
        }
        if (!isValid)
            continue;

        ArmOpcode* opcode = new ArmOpcode(type, true, opcodeAddress, encoding, size,
                                          entry, getCondition(entry, encoding),
                                          itState);
        for (uint j = 0; j < operandsCount; j++)
            opcode->addOperand(operands[j]);
        opcode->setMnemonic(mnemonic);
        return opcode;
    }
    return NULL;
}

ArmOpcodePtr ArmStreamDisassembler::decode(ArmInstructionSet::DisassemblerTypes type,
                                           uint32 encoding,
                                           uint size,
                                           uint64 address,
                                           const ArmFeatureSet& features,
                                           uint itState)
{
    return ArmOpcodePtr(decodeRaw(type, encoding, size, address, features, itState));
}

OpcodePtr ArmStreamDisassembler::next()
{
    // The bytes of an invalid unit, one at a time
    if (m_pendingInvalidBytes > 0)
    {
        uint8 data = m_pendingInvalid[4 - m_pendingInvalidBytes];
        m_pendingInvalidBytes--;
        OpcodePtr invalid(new InvalidOpcodeByte(data, m_shouldUseAddress, m_address));
        if (m_shouldUseAddress)
            m_address = m_address + 1;
        return invalid;
    }

    if (m_stream->isEOS())
        XSTL_THROW(DisassemblerEndOfStreamException);

    uint streamPointer = m_stream->getPointer();
    uint8 bytes[4] = { 0, 0, 0, 0 };
    uint size = ArmInstructionSet::getInstructionAlignment(m_type);
    uint32 encoding = 0;

    XSTL_TRY
    {
        m_stream->streamReadUint8(bytes[0]);
        m_stream->streamReadUint8(bytes[1]);
        if (m_type == ArmInstructionSet::ARM_T32)
        {
            // The first halfword: 0b11101, 0b11110 and 0b11111 start a 32 bit
            // instruction
            uint32 first = bytes[0] | ((uint32)bytes[1] << 8);
            if ((first >> 11) >= 0x1D)
            {
                size = 4;
                m_stream->streamReadUint8(bytes[2]);
                m_stream->streamReadUint8(bytes[3]);
                encoding = (first << 16) | bytes[2] | ((uint32)bytes[3] << 8);
            } else
            {
                encoding = first;
            }
        } else
        {
            m_stream->streamReadUint8(bytes[2]);
            m_stream->streamReadUint8(bytes[3]);
            encoding = bytes[0] | ((uint32)bytes[1] << 8) |
                       ((uint32)bytes[2] << 16) | ((uint32)bytes[3] << 24);
        }
    }
    XSTL_CATCH_ALL
    {
        // A truncated instruction
        m_stream->seek(streamPointer, basicInput::IO_SEEK_SET);
        XSTL_THROW(DisassemblerEndOfStreamException);
    }

    ArmOpcode* opcode = decodeRaw(m_type, encoding, size,
                                  m_shouldUseAddress ? m_address.getAddress() : 0,
                                  m_features, m_itState);
    if (opcode == NULL)
    {
        // Invalid bits
        if (!m_shouldOpcodeFaultTolerantEnabled)
        {
            m_stream->seek(streamPointer, basicInput::IO_SEEK_SET);
            XSTL_THROW(DisassemblerInvalidOpcodeException);
        }
        for (uint i = 0; i < size; i++)
            m_pendingInvalid[4 - size + i] = bytes[i];
        m_pendingInvalidBytes = size;
        return next();
    }

    // TODO: T32 it blocks. An it instruction sets m_itState to its
    // firstcond:mask and the next instructions take their condition from it,
    // advancing the state after each one
    if (m_shouldUseAddress)
        m_address = m_address + size;
    return OpcodePtr(opcode);
}

void ArmStreamDisassembler::jumpToAddress(ProcessorAddress address, uint rawAddress)
{
    // Seek the stream to either the absolute raw offset or the calculated
    // difference
    if (0 != rawAddress)
        m_stream->seek(rawAddress, basicInput::IO_SEEK_SET);
    else
        m_stream->seek((int)(address.getAddress() - m_address.getAddress()),
                       basicInput::IO_SEEK_CUR);

    m_address.setAddress(address.getAddress());
    // The new location starts a new it block and a new unit
    m_itState = 0;
    m_pendingInvalidBytes = 0;
}

OpcodePtr ArmStreamDisassembler::jumpToAddressAndNext(ProcessorAddress address)
{
    jumpToAddress(address);
    return next();
}

bool ArmStreamDisassembler::isEndOfStream()
{
    return (m_pendingInvalidBytes == 0) && m_stream->isEOS();
}

bool ArmStreamDisassembler::getNextOpcodeLocation(ProcessorAddress& address)
{
    if (!m_shouldUseAddress)
        return false;
    address = m_address;
    return true;
}

OpcodeFormatterPtr ArmStreamDisassembler::getOpcodeFormat(
                                const OpcodePtr& instruction,
                                OpcodeDataFormatter& dataFormatter,
                                int formatType)
{
    // Handle invalid opcode format
    if (instruction->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE)
        return OpcodeFormatterPtr(new InvalidOpcodeFormatter(instruction, dataFormatter));

    CHECK(ArmInstructionSet::fromSubsystem(instruction->getType()) == m_type);
    return OpcodeFormatterPtr(new ArmNotation(instruction, dataFormatter));
}

OpcodeSubsystems::DisassemblerType ArmStreamDisassembler::getType() const
{
    return ArmInstructionSet::toSubsystem(m_type);
}
