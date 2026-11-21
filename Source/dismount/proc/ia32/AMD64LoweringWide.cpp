/*
 * AMD64LoweringWide.cpp
 *
 * The lowering of the wide values (the operations of the 128 bit and wider
 * values). Not implemented yet: every operation is left to the other lowerings.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include "AMD64BlockLowering.h"

bool amd64lowering::BlockLowering::lowerWide(const SemanticInstruction&)
{
    return false;
}
