/*
 * AMD64LoweringFloat.cpp
 *
 * The lowering of the scalar floating point operations. Not implemented yet: every
 * operation is left to the other lowerings.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include "AMD64BlockLowering.h"

bool amd64lowering::BlockLowering::lowerFloat(const SemanticInstruction&)
{
    return false;
}
