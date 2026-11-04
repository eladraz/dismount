/*
 * ArmSemanticTableSimd.cpp
 *
 * Implementation file
 *
 * The semantic table of the AArch32 VFP and Advanced SIMD instructions
 * (gArmA32SimdEncodingTable, see ArmSemantics.h). The entries are in two
 * files, one per lifter: the floating point instructions (every mnemonic
 * with a floating point data type, vmov, vmrs, vmsr, vldr, vstr, vldm,
 * vstm, vpush and vpop, ArmSemanticLiftVfp.cpp) and the rest of the
 * Advanced SIMD and the cryptographic extensions (ArmSemanticLiftNeon.cpp).
 * A mnemonic without an entry is lifted to OP_UNDEFINED and its feature is
 * not marked SUPPORT_LIFT (ArmFeatures.cpp).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/ArmSemanticInternal.h"
#include "dismount/proc/arm/ArmSemanticVfp.h"
#include "dismount/proc/arm/ArmSemanticNeon.h"

namespace armdis {

namespace {

#define U_(x) OpcodeClassification::UNIT_##x
#define P_(x) OpcodeClassification::PROPERTY_##x

} // namespace

const ArmSemanticEntry gArmSimdSemanticTable[] =
{
#include "ArmSemanticTableVfp.inc"
#include "ArmSemanticTableNeon.inc"
    ARM32_END
};

} // namespace armdis
