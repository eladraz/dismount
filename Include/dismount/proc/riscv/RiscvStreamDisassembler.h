#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVSTREAMDISASSEMBLER_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVSTREAMDISASSEMBLER_H

/*
 * RiscvStreamDisassembler.h
 *
 * The disassembler of the RISC-V processors: RV32 and RV64.
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
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvEncodingTable.h"
#include "dismount/proc/riscv/RiscvOpcode.h"

/*
 * The disassembler of the RISC-V processors. See StreamDisassembler.
 *
 * Each call of next() reads one instruction: the first 16 bit parcel gives
 * the size (RiscvInstructionSet::getInstructionSize()), 2 or 4 bytes. The
 * bytes are little endian.
 *
 * The bits are matched against the tables of the size in order (see
 * RiscvEncodingEntry): the first entry whose bits, features, instruction set
 * and condition match is the instruction, and its operands are decoded by
 * RiscvOperandCodec. A compressed instruction gets its expansion
 * (RiscvOpcode::getExpansion()), decoded from the 32 bit tables.
 *
 * Bits which no entry matches (or whose operands have a reserved value in
 * every matching entry), a compressed instruction without the Zca feature,
 * and the longer formats are an invalid instruction: with the fault
 * tolerance on, an InvalidOpcodeByte for each byte of the unit, otherwise
 * DisassemblerInvalidOpcodeException. The unit is what llvm-objdump skips: 2
 * bytes for a 16 bit parcel, 4 for a 32 bit instruction, and the whole length
 * of a longer format (6 bytes for the 48 bit one, 8 for the 64 bit one, 10 +
 * 2 * nnn for the bits[14:12] = nnn < 7 of the (80 + 16 * nnn) bit formats);
 * the reserved format (bits[6:0] = 1111111 with nnn = 7) is one byte.
 *
 * NOTE: This class is not thread-safe
 */
class RiscvStreamDisassembler : public StreamDisassembler {
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
    RiscvStreamDisassembler(RiscvInstructionSet::DisassemblerTypes type,
                            const BasicInputPtr& disassemblerStream,
                            bool shouldUseAddress,
                            const ProcessorAddress& streamAddress,
                            bool shouldOpcodeFaultTolerantEnabled,
                            const RiscvFeatureSet& features = RiscvFeatureSet::all());

    // See StreamDisassembler. The formatType of getOpcodeFormat() is a
    // combination of the RiscvNotation::FormatFlags
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
     * type           - The instruction set
     * encoding       - The bits (a 16 bit instruction in the low bits)
     * size           - 2 or 4
     * address        - The address of the instruction
     * features       - The features of the target
     * isAliasAllowed - False to skip the RISCV_ENTRY_ALIAS entries (the
     *                  formatter of RISCV_FORMAT_NO_ALIASES decodes again
     *                  without them)
     *
     * Return the instruction, or a NULL pointer if the bits are invalid
     */
    static RiscvOpcodePtr decode(RiscvInstructionSet::DisassemblerTypes type,
                                 uint32 encoding,
                                 uint size,
                                 uint64 address,
                                 const RiscvFeatureSet& features,
                                 bool isAliasAllowed = true);

    /*
     * Return true if the bits are this entry (its mask, features, instruction
     * set and operands), whatever the entries before it are and without its
     * m_condition (the reserved encodings which the decoder rejects but llvm-mc
     * assembles: mop.r.28 zero, a1): the assembler uses it to validate an
     * encoding, which another entry may shadow ("jr ra" is printed "ret")
     */
    static bool matches(const riscvdis::RiscvEncodingEntry* entry,
                        RiscvInstructionSet::DisassemblerTypes type,
                        uint32 encoding,
                        uint64 address,
                        const RiscvFeatureSet& features);

    /*
     * Return the tables of an instruction size, in the search order, ended
     * by a NULL pointer: the compressed table for 2, the base, the vector
     * and the privileged tables for 4
     */
    static const riscvdis::RiscvEncodingEntry* const* getTables(uint size);

private:
    // The instruction set and the target features
    RiscvInstructionSet::DisassemblerTypes m_type;
    RiscvFeatureSet m_features;
    // The stream
    BasicInputPtr m_stream;
    // The address of the next instruction
    bool m_shouldUseAddress;
    ProcessorAddress m_address;
    // See StreamDisassemblerFactory
    bool m_shouldOpcodeFaultTolerantEnabled;
    // The bytes of an invalid unit not returned yet (InvalidOpcodeByte one
    // at a time): the last m_pendingInvalidBytes bytes of m_pendingInvalid
    // (the longest unit is the 22 bytes of a 176 bit format)
    enum { PENDING_SIZE = 32 };
    uint m_pendingInvalidBytes;
    uint8 m_pendingInvalid[PENDING_SIZE];
};

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVSTREAMDISASSEMBLER_H
