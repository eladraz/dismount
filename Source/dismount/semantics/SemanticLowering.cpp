/*
 * SemanticLowering.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/exceptions.h"
#include "dismount/semantics/SemanticLowering.h"
#include "dismount/proc/ia32/AMD64Lowering.h"

SemanticLoweringPtr SemanticLoweringFactory::getLowering(
                                OpcodeSubsystems::DisassemblerType target)
{
    switch (target)
    {
    case OpcodeSubsystems::DISASSEMBLER_AMD_64:
        return SemanticLoweringPtr(new AMD64Lowering());
    default:
        // No lowering for this processor
        XSTL_THROW(cException,
                   cString("SemanticLoweringFactory: the processor has no "
                           "lowering").getBuffer(),
                   EXCEPTION_FAILED);
    }
}
