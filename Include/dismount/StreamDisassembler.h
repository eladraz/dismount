#ifndef __TBA_DISMOUNT_STREAMDISASSEMBLER_H
#define __TBA_DISMOUNT_STREAMDISASSEMBLER_H

/*
 * StreamDisassembler.h
 *
 * Parse a stream of bytes and return thier opcode representation.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeFormatter.h"
#include "dismount/OpcodeDataFormatter.h"
#include "dismount/ProcessorAddress.h"

/*
 * This class parse a stream of opcodes and return their instruction.
 * See StreamDisassemblerFactory for more information
 * Usage:
 *   OpcodePtr firstInstruction = dis.next();
 *   OpcodeFormatterPtr formatter = dis.getOpcodeView(firstInstruction,
 *                                                    formatter);
 *   cout << formatter.string() << endl;
 *
 * NOTE: This class is not thread-safe
 */
class StreamDisassembler {
public:
    /*
     * Default destructor. You can inherit from me.
     */
    virtual ~StreamDisassembler() {};

    /*
     * Returns the next opcode instruction.
     *
     * If the instruction is invalid and the StreamDisassemblerFactory generate
     * this class with opcode-fault-tolerant the return opcode is
     * InvalidOpcodeByte and the address will increase by one byte.
     * If the opcode-fault-tolerant option is off for this class than
     * DisassemblerInvalidOpcodeException will be throwed.
     *
     * Throw DisassemblerEndOfStreamException exception if there aren't any
     * more bytes in the stream.
     */
    virtual OpcodePtr next() = 0;

    /*
     * Seeks the stream to a given memory location.
     * Calculates the needed stream location and seeks accordingly.
     * It also sets the current memory address variable.
     *
     * address - Contains the memory address to jump to
     * rawAddress - If specified, the raw offset in the stream to
     *              seek to
     */
    virtual void jumpToAddress(ProcessorAddress address,
                               uint rawAddress = 0) = 0;

    /*
     * Seeks the stream to a given memory location, and returns the next opcode instruction.
     * The opcode validity checking and exception throwing is the same as  in next().
     *
     * address - Contains the memory address to jump to
     */
    virtual OpcodePtr jumpToAddressAndNext(ProcessorAddress address) = 0;

    /*
     * Return true if the stream reach it's end-of-stream, all calls to next
     * will cause in exception.
     */
    virtual bool isEndOfStream() = 0;

    /*
     * Return the next opcode instruction location in the memory. This value is
     * optional and set by the disassembler.
     *
     * address - Will be filled with the address of the instruction. Valid if
     *           and only if the return value is 'true'
     *
     * Return true if the instruction contain address and 'address' filled with
     * the address. Return false if the instruction don't contains address and
     * 'address' content is invalid
     */
    virtual bool getNextOpcodeLocation(ProcessorAddress& address) = 0;

    /*
     * The different formatter that might be used in the GUI
     */
    enum {
        // For all disassemblers, this value is valid.
        DEFAULT_FORMATTER = 0
    };

    /*
     * Return the formatter object to be used to translate opcode into a string.
     *
     * instruction   - The instruction to be formatted
     * dataFormatter - The formatter GUI information module
     * formatType    - The convention type module for the disassembler
     *
     * Return the formatter object.
     * Return NULL-pointer object if the arguments are invalid.
     */
    virtual OpcodeFormatterPtr getOpcodeFormat(const OpcodePtr& instruction,
                                               OpcodeDataFormatter& dataFormatter,
                                               int formatType = DEFAULT_FORMATTER) = 0;

    /*
     * Return run-time information regarding the class implementation, used for
     * downcast variables.
     */
    virtual OpcodeSubsystems::DisassemblerType getType() const = 0;
};
// The reference countable object
typedef cSmartPtr<StreamDisassembler> StreamDisassemblerPtr;

#endif // __TBA_DISMOUNT_STREAMDISASSEMBLER_H
