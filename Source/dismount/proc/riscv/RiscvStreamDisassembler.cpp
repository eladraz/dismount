#include "dismount/dismount.h"
/*
 * RiscvStreamDisassembler.cpp
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
#include "dismount/proc/riscv/RiscvOperandCodec.h"
#include "dismount/proc/riscv/RiscvNotation.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"

using namespace riscvdis;

namespace {

/*
 * The tables of the instruction sizes, in the search order
 */
const RiscvEncodingEntry* const gCompressedTables[] = {
    gRiscvCompressedEncodingTable, NULL
};
const RiscvEncodingEntry* const gWideTables[] = {
    gRiscvBaseEncodingTable, gRiscvVectorEncodingTable, gRiscvPrivilegedEncodingTable, NULL
};

/*
 * An index of the tables of one size by the bits which every entry fixes: the
 * major opcode (bits 0-6) of the 32 bit instructions, and the quadrant with the
 * funct3 (bits 0-1 and 13-15) of the 16 bit ones. The entries which can match
 * a key, in the order of the tables: the first matching entry of a bucket is
 * the first matching entry of the tables.
 */
class DecodeIndex {
public:
    DecodeIndex(const RiscvEncodingEntry* const* tables, bool isCompressed) :
        m_isCompressed(isCompressed)
    {
        const uint buckets = isCompressed ? 32 : 128;
        for (uint key = 0; key < buckets; key++)
        {
            for (const RiscvEncodingEntry* const* table = tables; *table != NULL; table++)
            {
                for (const RiscvEncodingEntry* entry = *table; entry->m_mnemonic != NULL; entry++)
                {
                    if ((entry->m_flags & RISCV_ENTRY_ASSEMBLE_ONLY) != 0)
                        continue;
                    uint keyMask = getKey(entry->m_mask);
                    uint keyValue = getKey(entry->m_value & entry->m_mask);
                    if ((key & keyMask) == keyValue)
                        m_buckets[key].append(entry);
                }
            }
        }
    }

    const cArray<const RiscvEncodingEntry*>& getBucket(uint32 encoding) const
    {
        return m_buckets[getKey(encoding)];
    }

private:
    uint getKey(uint32 bits) const
    {
        if (m_isCompressed)
            return (bits & 3) | (((bits >> 13) & 7) << 2);
        return bits & 0x7f;
    }

    bool m_isCompressed;
    cArray<const RiscvEncodingEntry*> m_buckets[128];
};

const DecodeIndex& getIndex(uint size)
{
    if (size == 2)
    {
        static const DecodeIndex index(gCompressedTables, true);
        return index;
    }
    static const DecodeIndex index(gWideTables, false);
    return index;
}

ProcessorAddress makeAddress(RiscvInstructionSet::DisassemblerTypes type, uint64 address)
{
    return ProcessorAddress(
        (type == RiscvInstructionSet::RISCV_RV64) ? ProcessorAddress::PROCESSOR_64 :
                                                    ProcessorAddress::PROCESSOR_32,
        (ProcessorAddress::uintAddress)address);
}

// Decode the bits into a new opcode (NULL if they are invalid), without the
// expansion and the alias forms
// Match the bits against one entry: its mask, features, instruction set and
// condition, then its operands. Return true and fill 'mnemonic', 'operands'
// and 'operandsCount' if the bits are this entry
bool matchEntry(const RiscvEncodingEntry* entry,
                RiscvInstructionSet::DisassemblerTypes type,
                uint32 encoding,
                uint64 address,
                const RiscvFeatureSet& features,
                uint aliasMode,
                cString& mnemonic,
                RiscvDecodedOperand* operands,
                uint& operandsCount,
                bool ignoreCondition = false)
{
    RiscvCodecContext context;
    context.m_type = type;
    context.m_address = address;
    context.m_features = features;
    if ((encoding & entry->m_mask) != (entry->m_value & entry->m_mask))
        return false;
    if (((entry->m_flags & RISCV_ENTRY_RV32_ONLY) != 0) &&
        (type != RiscvInstructionSet::RISCV_RV32))
        return false;
    if (((entry->m_flags & RISCV_ENTRY_RV64_ONLY) != 0) &&
        (type != RiscvInstructionSet::RISCV_RV64))
        return false;
    if ((((entry->m_flags & RISCV_ENTRY_DECODE_UNGATED) == 0) &&
         !features.has((RiscvFeatures::Feature)entry->m_feature)) ||
        !features.has((RiscvFeatures::Feature)entry->m_feature2))
        return false;
    if (!ignoreCondition && (entry->m_condition != NULL) && !entry->m_condition(encoding))
        return false;

    // Decode the operands first: an operand with a reserved field means
    // that the encoding is not this entry
    operandsCount = 0;
    mnemonic = cString(entry->m_mnemonic);
    for (uint j = 0; j < RISCV_MAX_OPERAND_SPECS; j++)
    {
        const RiscvOperandSpec& spec = entry->m_operands[j];
        if (spec.m_kind == RISCV_KIND_NONE)
            break;
        if (RiscvOperandCodec::isMnemonicPart(spec))
        {
            mnemonic += RiscvOperandCodec::decodeMnemonicPart(spec, encoding);
            continue;
        }
        RiscvDecodedOperand operand;
        if (!RiscvOperandCodec::decode(spec, encoding, context, operand))
            return false;
        if (RiscvOperandCodec::isHidden(spec))
            continue;
        // The optional operands which print the default are left out
        // (without the aliases LLVM prints the dyn rounding mode too)
        if (RiscvOperandCodec::isOptional(spec) &&
            !RiscvOperandCodec::isPrinted(spec, encoding) &&
            !((aliasMode == 0) && (spec.m_kind == RISCV_KIND_ROUNDING_MODE) &&
              ((spec.m_flags & RISCV_ROUNDING_DEFAULT_RNE) == 0)))
            continue;
        operands[operandsCount++] = operand;
    }
    return true;
}

RiscvOpcode* decodeEntry(RiscvInstructionSet::DisassemblerTypes type,
                         uint32 encoding,
                         uint size,
                         uint64 address,
                         const RiscvFeatureSet& features,
                         uint aliasMode)
{
    // aliasMode: 0 no alias, 1 the aliases, 2 the aliases of the expansion of a
    // compressed instruction too
    const bool isAliasAllowed = (aliasMode != 0);
    const cArray<const RiscvEncodingEntry*>& bucket = getIndex(size).getBucket(encoding);

    for (uint i = 0; i < bucket.getSize(); i++)
    {
        const RiscvEncodingEntry* entry = bucket[i];
        if (!isAliasAllowed && ((entry->m_flags & RISCV_ENTRY_ALIAS) != 0))
            continue;
        if ((aliasMode != 2) && ((entry->m_flags & RISCV_ENTRY_EXPANSION_ALIAS) != 0))
            continue;
        RiscvDecodedOperand operands[RiscvOpcode::MAX_OPERANDS];
        uint operandsCount = 0;
        cString mnemonic;
        if (!matchEntry(entry, type, encoding, address, features, aliasMode, mnemonic, operands,
                        operandsCount))
            continue;

        RiscvOpcode* opcode = new RiscvOpcode(type, true, makeAddress(type, address),
                                              encoding, size, entry);
        for (uint j = 0; j < operandsCount; j++)
            opcode->addOperand(operands[j]);
        opcode->setMnemonic(mnemonic);
        return opcode;
    }
    return NULL;
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////

RiscvStreamDisassembler::RiscvStreamDisassembler(
    RiscvInstructionSet::DisassemblerTypes type,
    const BasicInputPtr& disassemblerStream,
    bool shouldUseAddress,
    const ProcessorAddress& streamAddress,
    bool shouldOpcodeFaultTolerantEnabled,
    const RiscvFeatureSet& features) :
        m_type(type),
        m_features(features),
        m_stream(disassemblerStream),
        m_shouldUseAddress(shouldUseAddress),
        m_address(streamAddress),
        m_shouldOpcodeFaultTolerantEnabled(shouldOpcodeFaultTolerantEnabled),
        m_pendingInvalidBytes(0)
{
    CHECK(!m_stream.isEmpty());
    for (uint i = 0; i < PENDING_SIZE; i++)
        m_pendingInvalid[i] = 0;
    if (m_shouldUseAddress)
    {
        ProcessorAddress::ProcessorAddressType expected =
            (type == RiscvInstructionSet::RISCV_RV64) ? ProcessorAddress::PROCESSOR_64 :
                                                        ProcessorAddress::PROCESSOR_32;
        CHECK(streamAddress.getAddressType() == expected);
    }
}

const RiscvEncodingEntry* const* RiscvStreamDisassembler::getTables(uint size)
{
    return (size == 2) ? gCompressedTables : gWideTables;
}

namespace {

// True if the entry has a rounding mode which RISCV_FORMAT_NO_ALIASES prints
// when it is dyn (LLVM leaves it out otherwise)
bool hasDynRounding(const RiscvEncodingEntry* entry)
{
    for (uint i = 0; i < RISCV_MAX_OPERAND_SPECS; i++)
    {
        if (entry->m_operands[i].m_kind == RISCV_KIND_NONE)
            break;
        if ((entry->m_operands[i].m_kind == RISCV_KIND_ROUNDING_MODE) &&
            ((entry->m_operands[i].m_flags & RISCV_ROUNDING_DEFAULT_RNE) == 0))
            return true;
    }
    return false;
}

// Decode the bits into a new opcode, with its expansion and its alias form.
// The caller owns the opcode. NULL if the bits are invalid
RiscvOpcode* decodeRaw(RiscvInstructionSet::DisassemblerTypes type,
                       uint32 encoding,
                       uint size,
                       uint64 address,
                       const RiscvFeatureSet& features,
                       uint aliasMode)
{
    RiscvOpcode* opcode = decodeEntry(type, encoding, size, address, features, aliasMode);
    if (opcode == NULL)
        return NULL;

    const RiscvEncodingEntry* entry = opcode->getEntry();
    // An alias prints the instruction it stands for with
    // RISCV_FORMAT_NO_ALIASES
    if ((aliasMode != 0) && (((entry->m_flags & RISCV_ENTRY_ALIAS) != 0) || hasDynRounding(entry)))
        opcode->setNoAliasForm(RiscvOpcodePtr(
            decodeRaw(type, encoding, size, address, features, 0)));

    // A compressed instruction carries the 32 bit instruction it expands to,
    // except the hints (LLVM prints those by their compressed name)
    if ((size == 2) && ((entry->m_flags & RISCV_ENTRY_HINT) == 0))
    {
        uint32 expanded = 0;
        if (RiscvOperandCodec::expandCompressed(type, (uint16)encoding, expanded))
            opcode->setExpansion(RiscvOpcodePtr(
                decodeRaw(type, expanded, 4, address, features,
                          (aliasMode != 0) ? 2 : 0)));
    }
    return opcode;
}

} // anonymous namespace

RiscvOpcodePtr RiscvStreamDisassembler::decode(RiscvInstructionSet::DisassemblerTypes type,
                                               uint32 encoding,
                                               uint size,
                                               uint64 address,
                                               const RiscvFeatureSet& features,
                                               bool isAliasAllowed)
{
    CHECK((size == 2) || (size == 4));
    return RiscvOpcodePtr(decodeRaw(type, encoding, size, address, features, isAliasAllowed ? 1 : 0));
}

bool RiscvStreamDisassembler::matches(const riscvdis::RiscvEncodingEntry* entry,
                                      RiscvInstructionSet::DisassemblerTypes type,
                                      uint32 encoding,
                                      uint64 address,
                                      const RiscvFeatureSet& features)
{
    RiscvDecodedOperand operands[RiscvOpcode::MAX_OPERANDS];
    uint operandsCount = 0;
    cString mnemonic;
    return matchEntry(entry, type, encoding, address, features, 1, mnemonic, operands,
                      operandsCount, true);
}

OpcodePtr RiscvStreamDisassembler::next()
{
    // The bytes of an invalid unit, one at a time
    if (m_pendingInvalidBytes > 0)
    {
        uint8 data = m_pendingInvalid[PENDING_SIZE - m_pendingInvalidBytes];
        m_pendingInvalidBytes--;
        OpcodePtr invalid(new InvalidOpcodeByte(data, m_shouldUseAddress, m_address));
        if (m_shouldUseAddress)
            m_address = m_address + 1;
        return invalid;
    }

    if (m_stream->isEOS())
        XSTL_THROW(DisassemblerEndOfStreamException);

    uint streamPointer = m_stream->getPointer();
    uint8 bytes[RiscvStreamDisassembler::PENDING_SIZE];
    for (uint i = 0; i < sizeof(bytes); i++)
        bytes[i] = 0;
    uint size = 2;
    uint32 encoding = 0;
    bool isReservedFormat = false;

    XSTL_TRY
    {
        m_stream->streamReadUint8(bytes[0]);
        m_stream->streamReadUint8(bytes[1]);
        uint16 first = (uint16)(bytes[0] | ((uint32)bytes[1] << 8));
        size = RiscvInstructionSet::getInstructionSize(first);
        if (size == 0)
        {
            // A longer format: an invalid unit of its length, 1 byte for the
            // reserved one
            isReservedFormat = true;
            if ((first & 0x20) == 0)
                size = 6;
            else if ((first & 0x40) == 0)
                size = 8;
            else if (((first >> 12) & 7) != 7)
                size = 10 + 2 * ((first >> 12) & 7);
            else
                size = 1;
            if (size == 1)
            {
                // (only the first byte is skipped)
                m_stream->seek(streamPointer + 1, basicInput::IO_SEEK_SET);
            } else
            {
                for (uint i = 2; i < size; i++)
                    m_stream->streamReadUint8(bytes[i]);
            }
        }
        encoding = first;
        if (size == 4)
        {
            m_stream->streamReadUint8(bytes[2]);
            m_stream->streamReadUint8(bytes[3]);
            encoding |= ((uint32)bytes[2] << 16) | ((uint32)bytes[3] << 24);
        }
    }
    XSTL_CATCH_ALL
    {
        // A truncated instruction: with the fault tolerance on the first byte
        // is an invalid byte (as llvm-objdump lists it), else the end of the
        // stream
        m_stream->seek(streamPointer, basicInput::IO_SEEK_SET);
        if (m_shouldOpcodeFaultTolerantEnabled)
        {
            uint8 data = 0;
            m_stream->streamReadUint8(data);
            OpcodePtr invalid(new InvalidOpcodeByte(data, m_shouldUseAddress, m_address));
            if (m_shouldUseAddress)
                m_address = m_address + 1;
            return invalid;
        }
        XSTL_THROW(DisassemblerEndOfStreamException);
    }

    RiscvOpcode* opcode = NULL;
    if (!isReservedFormat)
        opcode = decodeRaw(m_type, encoding, size,
                           m_shouldUseAddress ? m_address.getAddress() : 0,
                           m_features, 1);
    if (opcode == NULL)
    {
        // Invalid bits
        if (!m_shouldOpcodeFaultTolerantEnabled)
        {
            m_stream->seek(streamPointer, basicInput::IO_SEEK_SET);
            XSTL_THROW(DisassemblerInvalidOpcodeException);
        }
        for (uint i = 0; i < size; i++)
            m_pendingInvalid[PENDING_SIZE - size + i] = bytes[i];
        m_pendingInvalidBytes = size;
        return next();
    }

    if (m_shouldUseAddress)
        m_address = m_address + size;
    return OpcodePtr(opcode);
}

void RiscvStreamDisassembler::jumpToAddress(ProcessorAddress address, uint rawAddress)
{
    // Seek the stream to either the absolute raw offset or the calculated
    // difference
    if (0 != rawAddress)
        m_stream->seek(rawAddress, basicInput::IO_SEEK_SET);
    else
        m_stream->seek((int)(address.getAddress() - m_address.getAddress()),
                       basicInput::IO_SEEK_CUR);

    m_address.setAddress(address.getAddress());
    // The new location starts a new unit
    m_pendingInvalidBytes = 0;
}

OpcodePtr RiscvStreamDisassembler::jumpToAddressAndNext(ProcessorAddress address)
{
    jumpToAddress(address);
    return next();
}

bool RiscvStreamDisassembler::isEndOfStream()
{
    return (m_pendingInvalidBytes == 0) && m_stream->isEOS();
}

bool RiscvStreamDisassembler::getNextOpcodeLocation(ProcessorAddress& address)
{
    if (!m_shouldUseAddress)
        return false;
    address = m_address;
    return true;
}

OpcodeFormatterPtr RiscvStreamDisassembler::getOpcodeFormat(
                                const OpcodePtr& instruction,
                                OpcodeDataFormatter& dataFormatter,
                                int formatType)
{
    // Handle invalid opcode format
    if (instruction->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE)
        return OpcodeFormatterPtr(new InvalidOpcodeFormatter(instruction, dataFormatter));

    CHECK(RiscvInstructionSet::fromSubsystem(instruction->getType()) == m_type);
    return OpcodeFormatterPtr(new RiscvNotation(instruction, dataFormatter, formatType));
}

OpcodeSubsystems::DisassemblerType RiscvStreamDisassembler::getType() const
{
    return RiscvInstructionSet::toSubsystem(m_type);
}
