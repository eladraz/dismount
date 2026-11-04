#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVINSTRUCTIONSET_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVINSTRUCTIONSET_H

/*
 * RiscvInstructionSet.h
 *
 * The instruction sets of the RISC-V processor family:
 *    - RV32, XLEN = 32 (the RV32I base and its extensions)
 *    - RV64, XLEN = 64 (the RV64I base and its extensions, the base of the
 *      RVA23 profiles)
 *
 * RV32 and RV64 share the encodings, the syntax and most of the semantics:
 * they differ by the width of the integer registers, by the instructions which
 * exist in one of them only (addw / ld / c.ld are RV64, c.jal / c.flw / zip
 * are RV32) and by the shift amounts (5 bits in RV32, 6 bits in RV64). The
 * encoding tables are shared, an entry says when it belongs to one of them
 * (RISCV_ENTRY_RV32_ONLY / RISCV_ENTRY_RV64_ONLY, RiscvEncodingTable.h).
 *
 * The instructions are 16 bits (the compressed extension, the low two bits
 * are not 11) or 32 bits, 2 bytes aligned when a compressed extension is
 * enabled and 4 bytes aligned otherwise. They are always little endian. The
 * reserved longer formats (48 bits and up, the low bits xx11111) are invalid
 * instructions.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/OpcodeSubsystems.h"

class RiscvInstructionSet {
public:
    enum DisassemblerTypes {
        // XLEN = 32
        RISCV_RV32,
        // XLEN = 64
        RISCV_RV64
    };

    /*
     * Return the instruction set of a subsystem
     *
     * type - DISASSEMBLER_RISCV_32 or DISASSEMBLER_RISCV_64
     *
     * Throw exception for the other subsystems
     */
    static DisassemblerTypes fromSubsystem(OpcodeSubsystems::DisassemblerType type);

    /*
     * Return the subsystem of an instruction set (DISASSEMBLER_RISCV_32,
     * DISASSEMBLER_RISCV_64)
     */
    static OpcodeSubsystems::DisassemblerType toSubsystem(DisassemblerTypes type);

    /*
     * Return XLEN, the width of the integer registers in bits: 32 or 64
     */
    static uint getXlen(DisassemblerTypes type);

    /*
     * Return the size in bytes of an instruction from its first 16 bit parcel
     * (the low bits of the instruction): 2 when bits 0-1 are not 11, 4 when
     * bits 0-1 are 11 and bits 2-4 are not 111, and 0 for the reserved
     * longer formats (an invalid instruction)
     */
    static uint getInstructionSize(uint16 firstParcel);
};

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVINSTRUCTIONSET_H
