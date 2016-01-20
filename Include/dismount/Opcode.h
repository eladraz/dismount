#ifndef __TBA_DISMOUNT_OPCODE_H
#define __TBA_DISMOUNT_OPCODE_H

/*
 * Opcode.h
 *
 * The opcode class is a repository containing the content of a single CPU
 * instruction. See StreamDisassmbler for more information.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/OpcodeSubsystems.h"

/*
 * Hols the content of a single CPU instruction and provides logical information
 * regarding the type of the instruction.
 * Also holds
 */
class Opcode {
public:
    /*
     *
     */
    typedef enum {
        // Indicate that the opcode does not affect the flow of the program
        FLOW_NO_ALTER       = 0,

        // Indicate that a condition needs to be fulfilled for the jump to take place
        FLOW_COND_ALWAYS    = 0x0001,
        FLOW_COND_ZERO      = 0x0002,
        FLOW_COND_NEGATIVE  = 0x0004,
        FLOW_COND_PARITY    = 0x0008,
        FLOW_COND_OVERFLOW  = 0x0010,
        FLOW_COND_CARRY     = 0x0020,
        FLOW_COND_BIGGER    = 0x0040,
        FLOW_COND_LOWER     = 0x0080,
        FLOW_COND_NOT       = 0x0100,

        // Indicate that the opcode performs a change to the stack before or after jumping vs. no change in stack
        FLOW_STACK_CHANGE   = 0x0200,

        // Indicate that the jump is to a relative value vs. an absolute value
        FLOW_RELATIVE_JMP   = 0x0400,

        // Indicate that this is a ret command
        FLOW_RET            = 0x0800,

        // Invalid opcodes that should stop the parsing
        FLOW_INVALID        = 0x1000,

        // Indicate that this is a retf command
        FLOW_RETF           = 0x2000,

        // Used for debug purposes
        FLOW_DEBUG          = 0x4000,

        // Opcodes that we can handle with the "OpcodeAction" class
        FLOW_ACTION         = 0x8000,
    } FlowAlter;

    /*
     * Virtual destructor. You can inherit from me.
     */
    virtual ~Opcode() {};

    /*
     * Return run-time information regarding the class implementation, used for
     * downcast variables.
     */
    virtual OpcodeSubsystems::DisassemblerType getType() const = 0;

    /*
     * Return the content of the opcode
     */
    virtual cBuffer getOpcode() const = 0;

    /*
     * Return the size of the opcode in bytes
     */
    virtual uint getOpcodeSize() const = 0;

    /*
     * Return the opcode instruction location in the memory. This value is
     * optional and set by the disassembler.
     *
     * address - Will be filled with the address of the instruction. Valid if
     *           and only if the return value is 'true'
     *
     * Return true if the instruction contain address and 'address' filled with
     * the address. Return false if the instruction don't contains address and
     * 'address' content is invalid
     */
    virtual bool getOpcodeAddress(ProcessorAddress& address) const = 0;

    /*
     * Return true if the opcode may cause the normal flow of the application
     * to jump to a different location
     */
    virtual bool isBranch() const = 0;

    /*
     * Returns true if the opcode represents an instruction for jumping to
     * functions in a switch jump table (The opcode is a near JMP, with
     * a base displacement value - the top of the jump table,
     * and a register index inside the table)
     */
    virtual bool isSwitch() const = 0;

    /*
     * For 'switch' opcodes, returns the top of the switch jump table -
     * the 4 byte displacement
     */
    virtual uint32 getSwitchTableOffset() const = 0;

    /*
     * Returns the flow altering properties of the opcode
     */
    virtual int getAlterProperty() const = 0;

    // TODO!

    /*
     * Return true if the opcode might read from the physical memory
     *
    virtual bool isReadFromMemory() = 0;

     *
     * Return true if the opcode might write to the physical memory
     *
    virtual bool isWriteToMemory() = 0;

     *
     * Return the number of bytes that the opcode causes the stack to be change.
     *  - Zero means that the stack don't change when this opcode is executed.
     *  - Positive number means that new variable at size 'stackIncreasment'
     *    was pushed to the stack
     *  - Negative number means that new variable at size 'stackIncreasment'
     *    was poped from the stack
     * NOTE: This value can be negative.
     *
    virtual int stackIncreasment() = 0;
    **/
};

// The opcode reference-counter object
typedef cSmartPtr<Opcode> OpcodePtr;

#endif // __TBA_DISMOUNT_OPCODE_H

