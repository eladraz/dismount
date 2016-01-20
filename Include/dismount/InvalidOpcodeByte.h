#ifndef __TBA_DISMOUNT_INVALIDOPCODEBYTE_H
#define __TBA_DISMOUNT_INVALIDOPCODEBYTE_H

/*
 * InvalidOpcodeByte.h
 *
 * If the disassembler riches an opcode which is unable to decode, it's return
 * an opcode in a special sub-system named "InvalidOpcodeByte". This opcode
 * contains the byte which fault the disassembler.
 * All OpcodeFormatter should have the ability to decode this instruction
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "dismount/Opcode.h"
#include "dismount/ProcessorAddress.h"

/*
 * A single byte instruction which represent an unknown instruction.
 * All the OpcodeFormatter should know how to disassemble this instruction.
 */
class InvalidOpcodeByte : public Opcode {
public:
    /*
     * Constructor. Store the opcode data.
     *
     * invalidOpcode    - The 8 bit which cause the disassembler failure.
     * shouldUseAddress - Set by the StreamDisassembler. Set to true if
     *                    opcodeAddress is valid.
     * opcodeAddress    - Set by the StreamDisassembler. The address of the
     *                    instruction. See 'shouldUseAddress'
     */
    InvalidOpcodeByte(uint8 invalidOpcode,
                      bool shouldUseAddress,
                      const ProcessorAddress& opcodeAddress);


    /*
     * See Opcode::getType. Returns DISASSEMBLER_INVALID_OPCODE.
     */
    virtual OpcodeSubsystems::DisassemblerType getType() const;

    /*
     * See Opcode::getOpcode. Returns a single byte buffer contains the
     * illegal instruction.
     */
    virtual cBuffer getOpcode() const;

    /*
     * See Opcode::getOpcodeSize. Returns 1.
     */
    virtual uint getOpcodeSize() const;

    /*
     * See Opcode::getOpcodeAddress.
     * Return the address of the opcode only if this kind of information was
     * supplied.
     */
    virtual bool getOpcodeAddress(ProcessorAddress& address) const;

    /*
     * See Opcode::isBranch.
     */
    virtual bool isBranch() const;

    /*
     * See Opcode::isSwitch
     */
    virtual bool isSwitch() const;

    /*
     * See Opcode::isSwitch
     */
    virtual uint32 getSwitchTableOffset() const;

    /*
     * See Opcode::getAlterProperty.
     */
    virtual int getAlterProperty() const;

private:
    // The invalid opcode
    uint8 m_invalidOpcode;
    // Set to true if opcodeAddress is valid
    bool m_shouldUseAddress;
    // The address of the instruction. See 'm_shouldUseAddress'
    ProcessorAddress m_opcodeAddress;
};

#endif // __TBA_DISMOUNT_INVALIDOPCODEBYTE_H
