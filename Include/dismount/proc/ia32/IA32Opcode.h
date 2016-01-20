#ifndef __TBA_DISMOUNT_PROC_IA32_IA32OPCODE_H
#define __TBA_DISMOUNT_PROC_IA32_IA32OPCODE_H

/*
 * IA32Opcode.h
 *
 * Handles the ia32 processor opcode parser. Implement the 'Opcode' interface
 * according to intel ia32 spesfication.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/stream/basicIO.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/IntegerEncoding.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/proc/ia32/IA32OpcodeDatastruct.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"

class IA32Opcode : public Opcode {
public:
    /*
     * Constructor. Fill out the class members.
     * See this class members for detailed information regarding the argument
     * value.
     * See IA32StreamDisassembler for more information about IA32 opcode.
     */
    IA32Opcode(IA32eInstructionSet::DisassemblerTypes type,
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
               IntegerEncoding::IntegerEncodingType addressSize);

    /*
     * See Opcode::getOpcodeSize.
     * Calculate the size of the opcode by adding:
     *   prefix-size + opcode-size + modrm + sib + displacment + immediate
     */
    virtual uint getOpcodeSize() const;

    /*
     * See Opcode::getOpcode
     */
    virtual inline cBuffer getOpcode() const { return m_opcodeData;};

    /*
     * See Opcode::isBranch
     */
    virtual bool isBranch() const;

    /*
     * See Opcode::isSwitch
     */
    virtual bool isSwitch() const;

    /*
     * See Opcode::getSwitchTableOffset
     */
    virtual uint32 getSwitchTableOffset() const;

    /*
     * See Opcode::getAlterProperty
     */
    virtual int getAlterProperty() const;

    /*
     * Returns the opcode entry from opcode table.
     */
    virtual inline const ia32dis::OpcodeEntry* getOpcodeEntry() const { return m_opcode;};

    /*
     * Returns the opcode immediate value
     */
    inline const IA32OpcodeDatastruct::ImmediateType getImmediate() const { return m_immediate;};

    /*
     * Calculates and returns the address of a 32bit offset from the
     * current opcode address
     */
    const uint32 getImmediateAddress32() const;

    /*
     * Returns the ModR\M data
     */
    inline const IA32OpcodeDatastruct::MODRM getModrm() const { return m_modrm;};

    /*
     * Returns the SIB byte, if it exists
     */
    inline const IA32OpcodeDatastruct::SIB getSIB() const { return m_sib;};

    /*
     * Returns wheter the SIB byte is valid
     */
    inline const bool sibExist() const { return m_isSibExist;};

    /*
     * Returns the displacement data
     */
    inline const IA32OpcodeDatastruct::DisplacementType getDisplacement() const { return m_displacement;};

    /*
     * Returns the displacement length
     */
    inline const uint getDisplacementLength() const {return m_displacementLength;};

    /*
     * See Opcode::getOpcodeAddress
     */
    virtual bool getOpcodeAddress(ProcessorAddress& address) const;

    /*
     * Adds an address value to the opcode address
     * (Used for fixing the address to a real memory address -
     * adding the module base address)
     *
     * address - The address value to add
     *
     * Returns false if the opcode doesn't contain an address,
     * false otherwise.
     */
    bool addBaseAddress(addressNumericValue address);

    /*
     * Return one of the following:
     *  - DISASSEMBLER_INTEL_16 for INTEL_16
     *  - DISASSEMBLER_INTEL_32 for INTEL_32
     *  - DISASSEMBLER_AMD_64 for AMD_64
     */
    virtual OpcodeSubsystems::DisassemblerType getType() const;

private:
    // The OpcodeFormatter is the only class for now which can get access to
    // opcode's data.
    friend class IA32IntelNotation;

    // The assembler type
    IA32eInstructionSet::DisassemblerTypes m_type;

    // Set to true if opcodeAddress is valid
    bool m_shouldUseAddress;
    // The address of the instruction. See 'm_shouldUseAddress'
    ProcessorAddress m_opcodeAddress;

    // The complete opcode characters
    cBuffer m_opcodeData;
    // The last byte of the opcode might contains relevant argument. This value
    // is a simple cache to m_opcodeData[-1]
    uint8 m_lastOpcodeByte;

    // TODO! 64 bit, add REX prefix

    // The prefixs for the opcode
    uint8 m_prefixs[ia32dis::MAX_PREFIX];
    uint8 m_prefixsCount;

    // Pointer to the data of the opcode
    const ia32dis::OpcodeEntry* m_opcode;

    // Contains the modrm data if applied. OpcodeEntry->m_modrm tells whether
    // This value is valid.
    IA32OpcodeDatastruct::MODRM m_modrm;

    // Set to true if there is SIB
    bool m_isSibExist;

    // Contains the SIB data if applied
    IA32OpcodeDatastruct::SIB m_sib;

    // Contains the displacement data if applied, this variable have the maximum
    // length for the displacement
    IA32OpcodeDatastruct::DisplacementType m_displacement;
    uint m_displacementLength;
    // Contains the immediate value
    IA32OpcodeDatastruct::ImmediateType m_immediate;
    uint m_immediateLength;

    // Holds the size of the operand-size, register size and immediate size
    IntegerEncoding::IntegerEncodingType m_operandSize;
    // Holds the size of the addressing mode, address registered pointers and
    // immediates holding the address content
    IntegerEncoding::IntegerEncodingType m_addressSize;
};

typedef cSmartPtr<IA32Opcode> IA32OpcodePtr;

#endif // __TBA_DISMOUNT_PROC_IA32_IA32OPCODE_H
