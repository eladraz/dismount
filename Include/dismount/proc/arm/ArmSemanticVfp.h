#ifndef __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICVFP_H
#define __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICVFP_H

/*
 * ArmSemanticVfp.h
 *
 * The lifters of the AArch32 floating point instructions (VFP and the floating point ones of Advanced SIMD), declared for
 * the semantic table ArmSemanticTableVfp.inc. Implementation: ArmSemanticLiftVfp.cpp.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/arm/ArmSemantics.h"

namespace armdis {
namespace a32 {

// The lifters of ArmSemanticTableVfp.inc (ArmSemanticLiftVfp.cpp)
void liftFpArithmetic(ArmLiftContext& context);
void liftFpPairwise(ArmLiftContext& context);
void liftFpUnary(ArmLiftContext& context);
void liftFpCompare(ArmLiftContext& context);
void liftFpVectorCompare(ArmLiftContext& context);
void liftFpSelect(ArmLiftContext& context);
void liftFpJavascript(ArmLiftContext& context);
void liftFpConvert(ArmLiftContext& context);
void liftFpHalfMove(ArmLiftContext& context);
void liftFpComplex(ArmLiftContext& context);
void liftFpWidenMultiply(ArmLiftContext& context);
void liftBfloat(ArmLiftContext& context);
void liftFpMove(ArmLiftContext& context);
void liftFpStatus(ArmLiftContext& context);
void liftFpMemory(ArmLiftContext& context);
void liftFpMultiple(ArmLiftContext& context);

} // namespace a32
} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICVFP_H
