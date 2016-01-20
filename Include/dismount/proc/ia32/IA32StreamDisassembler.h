#ifndef __TBA_DISMOUNT_PROC_IA32_IA32STREAMDISASSEMBLER_H
#define __TBA_DISMOUNT_PROC_IA32_IA32STREAMDISASSEMBLER_H

/*
 * IA32StreamDisassembler.h
 *
 * The disassembler implementation to x86 processors.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/stream/basicIO.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeFormatter.h"
#include "dismount/OpcodeDataFormatter.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/IntegerEncoding.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"
#include "dismount/proc/ia32/IA32OpcodeDatastruct.h"

/*
 * The disassembler implementation to x86 processors.
 * See StreamDisassembler for more information.
 *
 * NOTE: This class is not thread-safe
 */
class IA32StreamDisassembler : public StreamDisassembler {
public:
    /*
     * Constructor.
     *
     * type               - The type of the instruction set
     * disassemblerStream - The instruction pipe-line
     * shouldUseAddress   - Should the 'streamAddress' is valid and being
     *                      increase for each parsed instruction.
     * streamAddress      - The address of the first instruction in the stream
     * shouldOpcodeFaultTolerantEnabled - See StreamDisassemblerFactory
     *
     * Throw exception if the streamAddress format is different than the
     * disassembler address mode (For example 64bit address supplied for 32bit)
     */
    IA32StreamDisassembler(IA32eInstructionSet::DisassemblerTypes type,
                           const BasicInputPtr& disassemblerStream,
                           bool shouldUseAddress,
                           const ProcessorAddress& streamAddress,
                           bool shouldOpcodeFaultTolerantEnabled);

    /*
     * See StreamDisassembler::next
     * IA32 processor are CISC processor. Each opcode have a different length
     * of bytes. When constructed over a stream, this class read only the opcode
     * part and disassemble it.
     *
     * Here is a simple abstract fllow of the instruction format
     * try {
     *     Start reading the opcode format:
     *      [ - Decode REX prefixs ] - TODO!
     *        - Decode prefixs
     *        - Decode floating-point extension/two-byte opcode table
     *        - Decode instruction number
     *        - Read modrm if needed
     *        - Read sib if needed
     *        - Read displacement if needed
     *        - Read immediate if needed
     *     Construct IA32Opcode
     *     Return it.
     * } catch () {
     *    revertOpcodeToOneByteRead();
     *    return InvalidOpcodeByte(newReadByte);
     * }
     */
    virtual OpcodePtr next();

    /*
     * See StreamDisassembler::jumpToAddress
     */
    virtual void jumpToAddress(ProcessorAddress address,
                               uint rawAddress = 0);

    /*
     * See StreamDisassembler::jumpToAddressAndNext
     */
    virtual OpcodePtr jumpToAddressAndNext(ProcessorAddress address);

    /*
     * See StreamDisassembler::isEndOfStream
     */
    virtual bool isEndOfStream();

    /*
     * See StreamDisassembler::getNextOpcodeLocation
     */
    virtual bool getNextOpcodeLocation(ProcessorAddress& address);

    /*
     * See StreamDisassembler::getOpcodeFormat
     */
    virtual OpcodeFormatterPtr getOpcodeFormat(
        const OpcodePtr& instruction,
        OpcodeDataFormatter& dataFormatter,
        int formatType = DEFAULT_FORMATTER);

    /*
     * Return one of the following:
     *  - DISASSEMBLER_INTEL_16 for INTEL_16
     *  - DISASSEMBLER_INTEL_32 for INTEL_32
     *  - DISASSEMBLER_AMD_64 for AMD_64
     */
    virtual OpcodeSubsystems::DisassemblerType getType() const;

private:
    // The type of the instruction set
    IA32eInstructionSet::DisassemblerTypes m_type;
    // The instruction pipe-line.
    BasicInputPtr m_stream;

    // Should throw exception if invalid opcode is reached
    bool m_shouldOpcodeFaultTolerantEnabled;
    // The address of the stream
    bool m_shouldUseAddress;
    // The address of the stream
    ProcessorAddress m_streamAddress;

    /*
     * Called each time the stream is about to be read bytes from, test that
     * stream is not reach it's end, if so throw exception.
     */
    void testEOS();

    /*
     * This function takes the current processor mode (16,32 or 64 bit) and the
     * override prefix and calculate the correct address/operand size.
     *
     * operandSizeOverride - Set to true if the operand-size override prefix is
     *                       found (Group 3, 0x66)
     * addressSizeOverride - Set to true if the address-size override prefix is
     *                       found (Group 4, 0x67)
     *
     * Change the m_operandSize and the m_addressSize
     */
    void calculateAddressAndOperandSize(bool operandSizeOverride,
                                        bool addressSizeOverride);

    /*
     * Filter the modrm according to the opcode information
     *
     * modrm - The modrm of the opcode
     * opcode - The opcode which matches the prefix
     *
     * Return true to indicate that the command is valid
     */
    bool filterModRM(const IA32OpcodeDatastruct::MODRM& modrm,
                     const ia32dis::OpcodeEntry* opcode) const;

    // Buffering members and method for opcode parsing

    /*
     * Appends new byte into "m_opcodeData" opcode cached value
     */
    void appendByte(uint8 ops);

    /*
     * Appends new bytes into "m_opcodeData" opcode cached value
     */
    void appendBuffer(uint8* ops, uint length);

    /*
     * Read the displacement and append it to the 'm_opcodeData' information
     *
     * data - The stream which point to the displacement information
     * displacementLength - The number of bytes of
     *
     * Return the displacement as a value
     */
    IA32OpcodeDatastruct::DisplacementType
        readDisplacement(basicInput& data, uint displacementLength);

    /*
     * Read the immediate and append it to the 'm_opcodeData' information
     *
     * data            - The stream which point to the displacement information
     * type            - The operand to be read
     * immediateLength - Only if type contains immediate data (The return value
     *                   is true), will be set to the size of the immediate in
     *                   bytes.
     * immediate       - Only if type contains immediate data (The return value
     *                   is true), will be set to the immediate value
     *
     * Return true if the operand contains immediate value
     */
    bool readImmediate(basicInput& data,
                       ia32dis::OperandType type,
                       uint&   immediateLength,
                       IA32OpcodeDatastruct::ImmediateType& immediate);

    /*
     * Private helper function, for both 'readImmediate' and 'readDisplacement'
     * Use for atomic reading of numbers and appending them into 'm_opcodeData'
     *
     * data - The stream to read the information from
     *
     * Throw exception if the stream reaches the end-of-stream.
     */
    uint8 read8bit(basicInput& data);
    uint16 read16bit(basicInput& data);
    uint32 read32bit(basicInput& data);

    // The complete opcode characters
    cBuffer m_opcodeData;
    // Store whether the operand extension prefix was detected [0x66]
    IntegerEncoding::IntegerEncodingType m_operandSize;
    // Store whether the address extension prefix was detected [0x67]
    IntegerEncoding::IntegerEncodingType m_addressSize;
    // TODO! REX cache should be here
};

#endif // __TBA_DISMOUNT_PROC_IA32_IA32STREAMDISASSEMBLER_H
