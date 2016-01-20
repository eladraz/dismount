#include "dismount/dismount.h"
/*
 * AssemblingFactory.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"

AssemblerInterfacePtr AssemblingFactory::generateAssembler(
                                        FirstPassBinaryPtr binary,
                                        OpcodeSubsystems::DisassemblerType type)
{
    switch (type)
    {
    case OpcodeSubsystems::DISASSEMBLER_INTEL_32:
        return AssemblerInterfacePtr(new IA32Assembler(
                                                binary,
                                                IA32eInstructionSet::INTEL_32));
    default:
        // Not ready yet...
        CHECK_FAIL();
    }
}

