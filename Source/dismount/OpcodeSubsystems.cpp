#include "dismount/dismount.h"
/*
 * OpcodeSubsystems.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "dismount/OpcodeSubsystems.h"

bool OpcodeSubsystems::isLittleEndian(DisassemblerType subSystem)
{
    switch (subSystem)
    {
    // Intel
    // All Intel processors are in little endian format
    case DISASSEMBLER_INTEL_16:
    case DISASSEMBLER_INTEL_32:
    case DISASSEMBLER_AMD_64:
    case DISASSEMBLER_INTEL_64:

    // ARM
    case DISASSEMBLER_ARM_32_LE:
    case DISASSEMBLER_THUMB_LE:
        return true;

    // ARM
    case DISASSEMBLER_ARM_32_BE:
    case DISASSEMBLER_THUMB_BE:
        return false;
    default:
        // Unknown type!!
        CHECK_FAIL();
    }
}

bool OpcodeSubsystems::isOverwriteRelativeLocations(
    DisassemblerType subSystem)
{
    switch (subSystem)
    {
    case DISASSEMBLER_INTEL_16:
    case DISASSEMBLER_INTEL_32:
    case DISASSEMBLER_AMD_64:
    case DISASSEMBLER_INTEL_64:
        return false;

    case DISASSEMBLER_ARM_32_LE:
    case DISASSEMBLER_ARM_32_BE:
    case DISASSEMBLER_THUMB_BE:
    case DISASSEMBLER_THUMB_LE:
        return true;
    default:
        // Unknown type!!
        CHECK_FAIL();
    }
}

