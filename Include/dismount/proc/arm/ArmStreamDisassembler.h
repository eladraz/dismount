#ifndef __TBA_DISMOUNT_PROC_ARM_ARMSTREAMDISASSEMBLER_H
#define __TBA_DISMOUNT_PROC_ARM_ARMSTREAMDISASSEMBLER_H

/*
 * ArmStreamDisassembler.h
 *
 * The disassembler of the ARM processors: A32, T32 and A64.
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
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmEncodingTable.h"
#include "dismount/proc/arm/ArmOpcode.h"

/*
 * The disassembler of the ARM processors. See StreamDisassembler.
 *
 * Each call of next() reads one instruction: 4 bytes in A32 and A64, 2 or 4
 * bytes in T32 (a first halfword of 0b11101, 0b11110 or 0b11111 in its top
 * 5 bits starts a 32 bit instruction). The bytes are little endian (the
 * instructions of the big endian subsystems too, ARMv7 BE-8 and AArch64).
 *
 * The bits are matched against the tables of the instruction set in order
 * (see ArmEncodingEntry): the first entry whose bits, feature and condition
 * match is the instruction, and its operands are decoded by
 * ArmOperandCodec. Bits which no entry matches (or whose operands have a
 * reserved value in every matching entry) are an invalid instruction: with
 * the fault tolerance on, an InvalidOpcodeByte for each byte of the unit (4
 * bytes, 2 for T32), otherwise DisassemblerInvalidOpcodeException.
 *
 * T32: the disassembler follows the it blocks. After an it instruction the
 * next 1-4 instructions get the conditions of the block (ArmOpcode::
 * getCondition(), getItState()), so jumpToAddress() resets the it state.
 *
 * NOTE: This class is not thread-safe
 */
class ArmStreamDisassembler : public StreamDisassembler {
public:
    /*
     * Constructor.
     *
     * type               - The instruction set
     * disassemblerStream - The instruction pipe-line
     * shouldUseAddress   - Should the 'streamAddress' is valid and being
     *                      increased for each parsed instruction
     * streamAddress      - The address of the first instruction
     * shouldOpcodeFaultTolerantEnabled - See StreamDisassemblerFactory
     * features           - The features of the target: the encodings of
     *                      other features are invalid. The default decodes
     *                      everything
     */
    ArmStreamDisassembler(ArmInstructionSet::DisassemblerTypes type,
                          const BasicInputPtr& disassemblerStream,
                          bool shouldUseAddress,
                          const ProcessorAddress& streamAddress,
                          bool shouldOpcodeFaultTolerantEnabled,
                          const ArmFeatureSet& features = ArmFeatureSet::all());

    // See StreamDisassembler
    virtual OpcodePtr next();
    virtual void jumpToAddress(ProcessorAddress address, uint rawAddress = 0);
    virtual OpcodePtr jumpToAddressAndNext(ProcessorAddress address);
    virtual bool isEndOfStream();
    virtual bool getNextOpcodeLocation(ProcessorAddress& address);
    virtual OpcodeFormatterPtr getOpcodeFormat(const OpcodePtr& instruction,
                                               OpcodeDataFormatter& dataFormatter,
                                               int formatType = DEFAULT_FORMATTER);
    virtual OpcodeSubsystems::DisassemblerType getType() const;

    /*
     * Decode the bits of one instruction, without a stream (the assembler
     * checks its encodings with it, the tests use it)
     *
     * type     - The instruction set
     * encoding - The bits (see ArmEncodingEntry: the T32 hw1 in the high 16
     *            bits)
     * size     - 2 for a T16 instruction, 4 otherwise
     * address  - The address of the instruction
     * features - The features of the target
     * itState  - T32: the it state which applies to the instruction (0 for
     *            none)
     *
     * Return the instruction, or a NULL pointer if the bits are invalid
     */
    static ArmOpcodePtr decode(ArmInstructionSet::DisassemblerTypes type,
                               uint32 encoding,
                               uint size,
                               uint64 address,
                               const ArmFeatureSet& features,
                               uint itState = 0);

    /*
     * Return the tables of an instruction set, in the search order, ended by
     * a NULL pointer. T32 returns the T16 table for 'size' 2, and the T32
     * table then the A32 floating point / Advanced SIMD table (see
     * gArmA32SimdEncodingTable) for 4
     */
    static const armdis::ArmEncodingEntry* const* getTables(
                ArmInstructionSet::DisassemblerTypes type, uint size);

private:
    // The instruction set and the target features
    ArmInstructionSet::DisassemblerTypes m_type;
    ArmFeatureSet m_features;
    // The stream
    BasicInputPtr m_stream;
    // The address of the next instruction
    bool m_shouldUseAddress;
    ProcessorAddress m_address;
    // See StreamDisassemblerFactory
    bool m_shouldOpcodeFaultTolerantEnabled;
    // T32: the it state of the next instruction (0 outside of a block)
    uint m_itState;
    // The bytes of an invalid unit not returned yet (InvalidOpcodeByte one
    // at a time): the last m_pendingInvalidBytes bytes of m_pendingInvalid
    uint m_pendingInvalidBytes;
    uint8 m_pendingInvalid[4];
};

#endif // __TBA_DISMOUNT_PROC_ARM_ARMSTREAMDISASSEMBLER_H
