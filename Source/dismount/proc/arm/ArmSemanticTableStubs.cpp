/*
 * ArmSemanticTableStubs.cpp
 *
 * Implementation file
 *
 * The semantic tables of the areas whose lifters are not written yet (the A64
 * SVE and SME): empty tables, so a mnemonic of these areas is lifted to
 * OP_UNDEFINED. When an area is filled, delete its table from this file and
 * define it in its own file (see ArmSemantics.h).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/proc/arm/ArmSemantics.h"

namespace armdis {

// The entry which ends a table
#define ARM_SEMANTIC_END \
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0 }

const ArmSemanticEntry gArm64SmeSemanticTable[] = { ARM_SEMANTIC_END };

} // namespace armdis
