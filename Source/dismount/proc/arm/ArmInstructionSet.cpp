#include "dismount/dismount.h"
/*
 * ArmInstructionSet.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/proc/arm/ArmInstructionSet.h"

ArmInstructionSet::DisassemblerTypes ArmInstructionSet::fromSubsystem(
                                    OpcodeSubsystems::DisassemblerType type)
{
    switch (type)
    {
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_BE:
        return ARM_A32;
    case OpcodeSubsystems::DISASSEMBLER_THUMB_LE:
    case OpcodeSubsystems::DISASSEMBLER_THUMB_BE:
        return ARM_T32;
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_BE:
        return ARM_A64;
    default:
        // Not an ARM subsystem
        CHECK_FAIL();
    }
}

OpcodeSubsystems::DisassemblerType ArmInstructionSet::toSubsystem(
                                                        DisassemblerTypes type)
{
    switch (type)
    {
    case ARM_A32:
        return OpcodeSubsystems::DISASSEMBLER_ARM_32_LE;
    case ARM_T32:
        return OpcodeSubsystems::DISASSEMBLER_THUMB_LE;
    case ARM_A64:
        return OpcodeSubsystems::DISASSEMBLER_ARM_64_LE;
    }
    // Should never reach here
    CHECK_FAIL();
}

bool ArmInstructionSet::isAArch32(DisassemblerTypes type)
{
    return (type == ARM_A32) || (type == ARM_T32);
}

uint ArmInstructionSet::getInstructionAlignment(DisassemblerTypes type)
{
    return (type == ARM_T32) ? 2 : 4;
}
