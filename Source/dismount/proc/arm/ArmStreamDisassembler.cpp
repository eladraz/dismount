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
#include "dismount/proc/arm/ArmOperandCodecA32.h"
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
// The tables T32 searches with the A32 form of the instruction
const ArmEncodingEntry* const gSimdTables[] = {
    gArmA32SimdEncodingTable, NULL
};
// The T32 integer table only (the first match of the raw bits)
const ArmEncodingEntry* const gT32IntegerTables[] = {
    gArmT32EncodingTable, NULL
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
            static const DecodeIndex index(gT32IntegerTables, 24);
            return index;
        }
    }
    CHECK_FAIL();
}

// True if an A32 entry has a condition field and the encoding has the 1111
// value in it
bool hasInvalidCondition(const ArmEncodingEntry* entry, uint32 encoding)
{
    for (uint i = 0; i < ARM_MAX_OPERAND_SPECS; i++)
    {
        if (entry->m_operands[i].m_kind == ARM_KIND_NONE)
            break;
        if (entry->m_operands[i].m_kind == ARM_KIND_CONDITION_MNEMONIC)
        {
            // (the condition of the it block is not a field)
            if ((entry->m_operands[i].m_flags & (ARM_A32_COND_IT | ARM_A32_COND_IT_ONLY)) != 0)
                return false;
            return ArmOperandCodec::getField(encoding, entry->m_operands[i].m_position, 4) == 15;
        }
    }
    return false;
}

// True if the entry has an A32 condition field at bit 28 (the floating point)
bool hasConditionField28(const ArmEncodingEntry* entry)
{
    for (uint i = 0; i < ARM_MAX_OPERAND_SPECS; i++)
    {
        const ArmOperandSpec& spec = entry->m_operands[i];
        if (spec.m_kind == ARM_KIND_NONE)
            break;
        if ((spec.m_kind == ARM_KIND_CONDITION_MNEMONIC) && (spec.m_position == 28) &&
            ((spec.m_flags & (ARM_A32_COND_IT | ARM_A32_COND_IT_ONLY)) == 0))
            return true;
    }
    return false;
}

// Return the condition of an instruction: the condition part of its entry
// (the A64 b.cond, the A32 field, the T32 b<c>), the one of the it block for
// the T32 instructions which take it from there, AL for the others
uint getCondition(const ArmEncodingEntry* entry, ArmInstructionSet::DisassemblerTypes type,
                  uint32 encoding, uint itState)
{
    for (uint i = 0; i < ARM_MAX_OPERAND_SPECS; i++)
    {
        const ArmOperandSpec& spec = entry->m_operands[i];
        if (spec.m_kind == ARM_KIND_NONE)
            break;
        if (spec.m_kind == ARM_KIND_CONDITION_MNEMONIC)
        {
            // (the A32 floating point entries have the field too, T32 has an
            // it block instead)
            if (((spec.m_flags & ARM_A32_COND_IT) != 0) || (itState != 0 && type == ArmInstructionSet::ARM_T32) ||
                ((spec.m_flags & ARM_A32_COND_IT_ONLY) != 0))
                return (itState == 0) ? ArmOpcode::CONDITION_ALWAYS : (itState >> 4);
            return ArmOperandCodec::getField(encoding, spec.m_position, 4);
        }
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

// The T32 form of the floating point and Advanced SIMD instructions is the A32
// form with another top byte (see gArmA32SimdEncodingTable). Return the A32
// encodings which a T32 word can be, in the order LLVM tries them. Return the
// number of them
static uint translateT32ToA32(uint32 encoding, uint32 candidates[3])
{
    uint count = 0;
    uint32 top = encoding >> 24;
    // The VFP (the condition is 1110) and the ARMv8 VFP (1111 1110) as they are
    if (((top >> 4) == 0xE) || (top == 0xFE) || (top == 0xFC) || (top == 0xFD))
        candidates[count++] = encoding;
    // The element and structure loads: 1111 1001 -> 1111 0100
    if (top == 0xF9)
        candidates[count++] = (encoding & 0xF0FFFFFF) | 0x04000000;
    // The data processing: 111U 1111 -> 1111 001U (and the ARMv8 ones)
    if ((top & 0xF) == 0xF)
    {
        uint32 data = encoding & 0xF0FFFFFF;
        data |= (data & 0x10000000) >> 4;
        data |= 0x12000000;
        candidates[count++] = data;
        candidates[count++] = encoding & 0xF3FFFFFF;
    }
    return count;
}

// Match the bits against the entries of a bucket: return the new opcode of the
// first entry which decodes, NULL if none does. 'encoding' is the form that
// the entries describe, 'original' the bits of the instruction
static ArmOpcode* decodeBucket(const cArray<const ArmEncodingEntry*>& bucket,
                               ArmInstructionSet::DisassemblerTypes type,
                               uint32 encoding,
                               uint32 original,
                               uint size,
                               uint64 address,
                               const ArmFeatureSet& features,
                               uint itState)
{
    ArmCodecContext context;
    context.m_type = type;
    context.m_address = address;
    context.m_features = features;
    context.m_itState = itState;

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
        if ((entry->m_feature2 != 0) &&
            !features.has((ArmFeatures::Feature)entry->m_feature2))
            continue;
        if ((entry->m_flags & ARM_ENTRY_PRE_V8) != 0 &&
            features.has(ArmFeatures::FEAT_V8_AA32))
            continue;
        if ((entry->m_condition != NULL) && !entry->m_condition(encoding))
            continue;
        // The A32 condition 1111 is not a condition of the instructions which
        // have one (they are in the unconditional space)
        if ((type == ArmInstructionSet::ARM_A32) && hasInvalidCondition(entry, encoding))
            continue;
        // T32: the floating point entries (their A32 condition field) are the
        // ones whose word starts with 1110
        if ((type == ArmInstructionSet::ARM_T32) && hasConditionField28(entry) &&
            ((encoding >> 28) != 0xE))
            continue;

        context.m_entry = entry;
        // Decode the operands first: an operand with a reserved field means
        // that the encoding is not this entry
        ArmDecodedOperand operands[ArmOpcode::MAX_OPERANDS];
        uint operandsCount = 0;
        // AArch32: the text of the entry up to its first '.' is the name, the
        // rest ("vadd.f32", "add.w") is a fixed suffix after the parts
        cString mnemonic(entry->m_mnemonic);
        cString fixedSuffix;
        if (type != ArmInstructionSet::ARM_A64)
        {
            uint dot = mnemonic.find(XSTL_STRING("."));
            if (dot < mnemonic.length())
            {
                fixedSuffix = mnemonic.part(dot, mnemonic.length());
                mnemonic = mnemonic.left(dot);
            }
        }
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
        mnemonic += fixedSuffix;
        // T32: the wide encoding of an instruction which has a narrow one
        if ((entry->m_flags & ARM_ENTRY_T32_WIDE) != 0)
            mnemonic += ".w";

        ArmOpcode* opcode = new ArmOpcode(type, true, opcodeAddress, original, size,
                                          entry, getCondition(entry, type, encoding, itState),
                                          itState);
        for (uint j = 0; j < operandsCount; j++)
            opcode->addOperand(operands[j]);
        opcode->setMnemonic(mnemonic);
        return opcode;
    }
    return NULL;
}

// The index of the Advanced SIMD and floating point table, which T32 searches
// with the A32 form of the instruction
static const DecodeIndex& getSimdIndex()
{
    static const DecodeIndex index(gSimdTables, 24);
    return index;
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
    ArmOpcode* opcode = decodeBucket(index.getBucket(encoding), type, encoding, encoding, size,
                                     address, features, itState);
    if ((opcode != NULL) || (type != ArmInstructionSet::ARM_T32) || (size != 4))
        return opcode;

    // T32: the floating point and Advanced SIMD instructions are the A32 ones
    uint32 candidates[3];
    uint count = translateT32ToA32(encoding, candidates);
    for (uint i = 0; i < count; i++)
    {
        opcode = decodeBucket(getSimdIndex().getBucket(candidates[i]), type, candidates[i],
                              encoding, size, address, features, itState);
        if (opcode != NULL)
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

    // T32: an it instruction sets the it state to its firstcond:mask, and each
    // instruction of the block advances it (ITAdvance)
    if (m_type == ArmInstructionSet::ARM_T32)
    {
        bool isIt = false;
        for (uint i = 0; i < ARM_MAX_OPERAND_SPECS; i++)
        {
            if (opcode->getEntry()->m_operands[i].m_kind == ARM_KIND_NONE)
                break;
            if (opcode->getEntry()->m_operands[i].m_kind == ARM_KIND_T16_IT_MASK)
                isIt = true;
        }
        if (isIt)
        {
            // (LLVM takes the firstcond 1111 as 1110)
            m_itState = encoding & 0xFF;
            if ((m_itState >> 4) == 15)
                m_itState = (m_itState & 0x0F) | 0xE0;
        }
        else if (m_itState != 0)
            m_itState = ((m_itState & 7) == 0) ? 0 : ((m_itState & 0xE0) | ((m_itState << 1) & 0x1F));
    }
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
