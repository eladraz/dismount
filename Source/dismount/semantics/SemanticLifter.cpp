/*
 * SemanticLifter.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticLifter.h"
#include "dismount/proc/ia32/IA32Semantics.h"
#include "dismount/proc/arm/ArmSemantics.h"

SemanticLifterPtr SemanticLifterFactory::getLifter(
                                    OpcodeSubsystems::DisassemblerType type)
{
    switch (type)
    {
    case OpcodeSubsystems::DISASSEMBLER_INTEL_16:
    case OpcodeSubsystems::DISASSEMBLER_INTEL_32:
    case OpcodeSubsystems::DISASSEMBLER_AMD_64:
        return SemanticLifterPtr(new IA32SemanticLifter(type));
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_LE:
    case OpcodeSubsystems::DISASSEMBLER_THUMB_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_LE:
        return SemanticLifterPtr(new ArmSemanticLifter(type));
    default:
        // No semantic description for this processor
        CHECK_FAIL();
    }
}
