#ifndef __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICINTERNAL_H
#define __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICINTERNAL_H

/*
 * ArmSemanticInternal.h
 *
 * The shared declarations of the AArch32 (A32 and T32) lifters: the integer
 * instructions (ArmSemanticLift32*.cpp), the VFP and floating point ones
 * (ArmSemanticLiftVfp.cpp) and the Advanced SIMD ones (ArmSemanticLiftNeon.cpp).
 * Not a public header: the semantic tables are the interface (ArmSemantics.h).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/ArmSemantics.h"

/*
 * An entry of an AArch32 semantic table: the mnemonic of the encoding entry
 * (with the data type of the Advanced SIMD and VFP ones, "vadd.f32"; the ".w"
 * of the wide T32 encodings is ignored), the hand written lifter, the
 * category, the units and the properties of OpcodeClassification, and the
 * SemanticFlags the instruction reads and writes.
 */
#define ARM32_ENTRY(name, handler, category, units, properties, read, written) \
    { ArmSemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, OpcodeClassification::CATEGORY_##category, \
        units, properties, read, written, 0, 0 }, \
      handler, 0, 0, 0, 0 }

/*
 * The same with the data of OpcodeClassification::getDataType(): the
 * SemanticType::Kind (KIND_INTEGER, KIND_FLOAT), the FloatFormat, the bits of
 * an element and the lanes (0: from the size of the first operand)
 */
#define ARM32_ENTRY_DATA(name, handler, category, units, properties, read, \
                         written, kind, format, bits, lanes) \
    { ArmSemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, OpcodeClassification::CATEGORY_##category, \
        units, properties, read, written, 0, 0 }, \
      handler, kind, format, bits, lanes }

// The entry which ends a table
#define ARM32_END \
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0 }

namespace armdis {
namespace a32 {

// Return the mnemonic of the encoding entry of the instruction ("add.w")
const char* getMnemonic(const ArmLiftContext& context);

// Return true if the mnemonic of the instruction is 'name', ignoring the
// ".w" of the wide T32 encodings
bool isMnemonic(const ArmLiftContext& context, const char* name);

// Emit an OP_UNDEFINED: the instruction is not lifted (UNPREDICTABLE...)
void undefined(ArmLiftContext& context);

// Return a 1 bit temporary: 'a' <condition> 'b' compared as 'type'
SemanticOperand compare(ArmLiftContext& context,
                        SemanticOperation::Condition condition,
                        const SemanticType& type,
                        const SemanticOperand& a, const SemanticOperand& b);

// Q = Q | flag (the sticky saturation flag of the APSR, a 1 bit value)
void accumulateSaturation(ArmLiftContext& context, const SemanticOperand& flag);

// Return the value of register 'registerId', the pc as an instruction reads
// it (the address + 8 in A32, + 4 in T32)
SemanticOperand getRegisterValue(ArmLiftContext& context, uint registerId);

/*
 * The lifters of the integer instructions, the handlers of the entries of
 * ArmSemanticTable.cpp (ArmSemanticLift32.cpp, ArmSemanticLift32Memory.cpp)
 */
void liftDataProcessing(ArmLiftContext& context);
void liftShift(ArmLiftContext& context);
void liftAdr(ArmLiftContext& context);
void liftMoveWide(ArmLiftContext& context);
void liftMultiply(ArmLiftContext& context);
void liftLongMultiply(ArmLiftContext& context);
void liftHalfwordMultiply(ArmLiftContext& context);
void liftDualMultiply(ArmLiftContext& context);
void liftMostSignificantMultiply(ArmLiftContext& context);
void liftSumAbsoluteDifferences(ArmLiftContext& context);
void liftDivide(ArmLiftContext& context);
void liftSaturatingAddSub(ArmLiftContext& context);
void liftSaturate(ArmLiftContext& context);
void liftSaturate16(ArmLiftContext& context);
void liftParallel(ArmLiftContext& context);
void liftSelect(ArmLiftContext& context);
void liftExtend(ArmLiftContext& context);
void liftPack(ArmLiftContext& context);
void liftBitFieldInsert(ArmLiftContext& context);
void liftBitFieldExtract(ArmLiftContext& context);
void liftReverse(ArmLiftContext& context);
void liftCrc32(ArmLiftContext& context);
void liftBranch(ArmLiftContext& context);
void liftBranchLink(ArmLiftContext& context);
void liftBranchLinkExchangeImmediate(ArmLiftContext& context);
void liftBranchExchange(ArmLiftContext& context);
void liftBranchLinkExchange(ArmLiftContext& context);
void liftBlx(ArmLiftContext& context);
void liftCompareBranch(ArmLiftContext& context);
void liftTableBranch(ArmLiftContext& context);
void liftNop(ArmLiftContext& context);
void liftHint(ArmLiftContext& context);
void liftBarrier(ArmLiftContext& context);
void liftException(ArmLiftContext& context);
void liftReadApsr(ArmLiftContext& context);
void liftWriteApsr(ArmLiftContext& context);
void liftCoprocessorMove(ArmLiftContext& context);
void liftLoadStore(ArmLiftContext& context);
void liftLoadStoreDouble(ArmLiftContext& context);
void liftLoadStoreMultiple(ArmLiftContext& context);
void liftExclusive(ArmLiftContext& context);
void liftClearExclusive(ArmLiftContext& context);
void liftPrefetch(ArmLiftContext& context);

} // namespace a32
} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICINTERNAL_H
