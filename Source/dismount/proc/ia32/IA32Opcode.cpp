#include "dismount/dismount.h"
/*
 * IA32Opcode.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/string.h"
#include "xStl/data/datastream.h"
#include "xStl/except/trace.h"
#include "dismount/Opcode.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/proc/ia32/IA32Opcode.h"

IA32Opcode::IA32Opcode(IA32eInstructionSet::DisassemblerTypes type,
                       bool shouldUseAddress,
                       const ProcessorAddress& opcodeAddress,
                       const cBuffer& opcodeData,
                       uint8* prefixs,
                       uint prefixsCount,
                       const ia32dis::OpcodeEntry* opcode,
                       uint8 opcodeSize,
                       const IA32OpcodeDatastruct::MODRM& modrm,
                       bool isSib,
                       const IA32OpcodeDatastruct::SIB& sib,
                       uint displacementLength,
                       const IA32OpcodeDatastruct::DisplacementType& displacement,
                       uint immediateLength,
                       const IA32OpcodeDatastruct::ImmediateType& immediate,
                       IntegerEncoding::IntegerEncodingType operandSize,
                       IntegerEncoding::IntegerEncodingType addressSize) :
    m_type(type),
    m_shouldUseAddress(shouldUseAddress),
    m_opcodeAddress(opcodeAddress),
    m_opcodeData(opcodeData),
    m_prefixsCount(prefixsCount),
    m_opcode(opcode),
    m_modrm(modrm),
    m_isSibExist(isSib),
    m_sib(sib),
    m_displacement(displacement),
    m_displacementLength(displacementLength),
    m_immediate(immediate),
    m_immediateLength(immediateLength),
    m_operandSize(operandSize),
    m_addressSize(addressSize)
{
    CHECK(opcode != NULL);
    // Cache the last byte of the opcode
    CHECK(m_opcodeData.getSize() > 0);
    m_lastOpcodeByte = m_opcodeData[opcodeSize - 1];
    // Test the address notation.
    switch (m_type)
    {
    case IA32eInstructionSet::INTEL_16:
        if (m_shouldUseAddress)
            CHECK(opcodeAddress.getAddressType() == ProcessorAddress::PROCESSOR_20);
        break;
    case IA32eInstructionSet::INTEL_32:
        if (m_shouldUseAddress)
            CHECK(opcodeAddress.getAddressType() == ProcessorAddress::PROCESSOR_32);
        break;
    case IA32eInstructionSet::AMD_64:
        if (m_shouldUseAddress)
            CHECK(opcodeAddress.getAddressType() == ProcessorAddress::PROCESSOR_64);
        // TODO! Not ready yet.
        CHECK_FAIL();
    default:
        CHECK_FAIL();
    }

    // Copy the prefixs
    //CHECK(m_prefixsCount <= ia32dis::MAX_PREFIX);
    if (!(m_prefixsCount <= ia32dis::MAX_PREFIX)) CHECK_FAIL();
    cOS::memcpy(m_prefixs, prefixs, m_prefixsCount);
}

uint IA32Opcode::getOpcodeSize() const
{
    return m_opcodeData.getSize();
}

bool IA32Opcode::isBranch() const
{
    return Opcode::FLOW_NO_ALTER != getOpcodeEntry()->m_alterProperty;
}

bool IA32Opcode::isSwitch() const
{
    return ((0xFF == m_opcodeData[0]) && (0x24 == m_modrm.m_packed) && // Near JMP
            (2 == m_sib.m_bits.m_scale) &&                             // Scaled index is DWORD
            (ia32dis::IA32_GP32_EBP == m_sib.m_bits.m_base) &&         // disp32
            (ia32dis::IA32_GP32_ESP != m_sib.m_bits.m_index));         // Register valid
}

uint32 IA32Opcode::getSwitchTableOffset() const
{
    if (4 == m_displacementLength)
        return m_displacement;
    return 0;
}

int IA32Opcode::getAlterProperty() const
{
    return getOpcodeEntry()->m_alterProperty;
}

const uint32 IA32Opcode::getImmediateAddress32() const
{
    return (uint32)((int32)m_immediate.offset + m_opcodeData.getSize() + m_opcodeAddress.getAddress());
}

bool IA32Opcode::getOpcodeAddress(ProcessorAddress& address) const
{
    if (!m_shouldUseAddress)
        return false;
    address = m_opcodeAddress;
    return true;
}

bool IA32Opcode::addBaseAddress(addressNumericValue address)
{
    if (!m_shouldUseAddress)
        return false;
    m_opcodeAddress.setAddress(m_opcodeAddress.getAddress() + address);
    return true;
}

OpcodeSubsystems::DisassemblerType IA32Opcode::getType() const
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
