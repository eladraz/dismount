#ifndef __TBA_DISMOUNT_OPCODESUBSYSTEMS_H
#define __TBA_DISMOUNT_OPCODESUBSYSTEMS_H

/*
 * OpcodeSubsystems.h
 *
 * A list of all different subsystems and whether the processor is integer
 * formate is little or big endian format
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"

/*
 * TODO! Add x86 different CPU cases.
 */
class OpcodeSubsystems {
public:
    // The different subsystems
    enum DisassemblerType {
        // Intel 8086 assembly
        DISASSEMBLER_INTEL_16,
        // Intel 80386 assembly, The IA32
        DISASSEMBLER_INTEL_32,
        // I am sorry, IA32e (EMT) is immoral name! AMD develop the langauge
        DISASSEMBLER_AMD_64,
        // Itanium microprocessor instruction set
        DISASSEMBLER_INTEL_64,
        // Little-endian ARM microprocessor
        DISASSEMBLER_ARM_32_LE,
        // Big-endian ARM microprocessor
        DISASSEMBLER_ARM_32_BE,
        // Little-endian THUMB microprocessor
        DISASSEMBLER_THUMB_LE,
        // Big-endian THUMB microprocessor
        DISASSEMBLER_THUMB_BE,

        // Native stribg (C/Javascript)
        DISASSEMBLER_STRING,

        // Dummy processor for Optimizer
        DISASSEMBLER_DUMMY,

        // Used for all disassemblers. Store illegal opcode start byte.
        DISASSEMBLER_INVALID_OPCODE
    };

    /*
     * Return true if 'subSystem' is little endian machine
     * Return false otherwise.
     *
     * subSystem - The sub system format
     */
    static bool isLittleEndian(DisassemblerType subSystem);

    /*
     * Return true if 'subSystem' should overwrite relative locations
     * Return false otherwise
     *
     * subSystem - The sub system format
     */
    static bool isOverwriteRelativeLocations(DisassemblerType subSystem);
};

#endif // __TBA_DISMOUNT_OPCODESUBSYSTEMS_H
