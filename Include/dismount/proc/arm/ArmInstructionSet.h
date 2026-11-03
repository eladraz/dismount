#ifndef __TBA_DISMOUNT_PROC_ARM_ARMINSTRUCTIONSET_H
#define __TBA_DISMOUNT_PROC_ARM_ARMINSTRUCTIONSET_H

/*
 * ArmInstructionSet.h
 *
 * The instruction sets of the ARM processor family:
 *    - A32, the 32 bit ARM instruction set (ARMv4T to ARMv8 AArch32)
 *    - T32, the Thumb instruction set: the 16 bit Thumb of ARMv4T and the
 *      16 / 32 bit Thumb-2 of ARMv6T2 and up
 *    - A64, the instruction set of ARMv8 / ARMv9 AArch64
 *
 * A32 and T32 share the registers, the semantics and most of the mnemonics
 * (the unified syntax): they are two modes of AArch32, selected by the T bit
 * of the CPSR (bx / blx switch it). A64 is a different architecture state.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/OpcodeSubsystems.h"

class ArmInstructionSet {
public:
    enum DisassemblerTypes {
        // The 32 bit ARM instruction set, 4 bytes aligned instructions
        ARM_A32,
        // Thumb, 2 or 4 bytes instructions, 2 bytes aligned
        ARM_T32,
        // AArch64, 4 bytes aligned instructions
        ARM_A64
    };

    /*
     * Return the instruction set of a subsystem
     *
     * type - DISASSEMBLER_ARM_32_LE / BE, DISASSEMBLER_THUMB_LE / BE or
     *        DISASSEMBLER_ARM_64_LE / BE
     *
     * Throw exception for the other subsystems
     */
    static DisassemblerTypes fromSubsystem(OpcodeSubsystems::DisassemblerType type);

    /*
     * Return the little endian subsystem of an instruction set
     * (DISASSEMBLER_ARM_32_LE, DISASSEMBLER_THUMB_LE, DISASSEMBLER_ARM_64_LE)
     */
    static OpcodeSubsystems::DisassemblerType toSubsystem(DisassemblerTypes type);

    /*
     * Return true for A32 and T32 (the AArch32 state)
     */
    static bool isAArch32(DisassemblerTypes type);

    /*
     * Return the size of the smallest instruction in bytes (the alignment):
     * 4 for A32 and A64, 2 for T32
     */
    static uint getInstructionAlignment(DisassemblerTypes type);
};

#endif // __TBA_DISMOUNT_PROC_ARM_ARMINSTRUCTIONSET_H
