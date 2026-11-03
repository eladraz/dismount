/*
 * ArchitectureDescription.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/AMD64Architecture.h"

ArchitectureDescriptionPtr ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DisassemblerType type)
{
    switch (type)
    {
    case OpcodeSubsystems::DISASSEMBLER_INTEL_16:
    case OpcodeSubsystems::DISASSEMBLER_INTEL_32:
        return ArchitectureDescriptionPtr(new IA32Architecture(type));
    case OpcodeSubsystems::DISASSEMBLER_AMD_64:
        return ArchitectureDescriptionPtr(new AMD64Architecture());
    default:
        // No description for this processor
        CHECK_FAIL();
    }
}
