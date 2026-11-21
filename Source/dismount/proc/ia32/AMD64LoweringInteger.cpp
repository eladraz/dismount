/*
 * AMD64LoweringInteger.cpp
 *
 * The lowering of the integer operations which the main lowering doesn't cover (bit
 * counts, divisions, byte reversal). Not implemented yet: every operation is
 * left to the other lowerings.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include "AMD64BlockLowering.h"

bool amd64lowering::BlockLowering::lowerIntegerExtra(const SemanticInstruction&)
{
    return false;
}
