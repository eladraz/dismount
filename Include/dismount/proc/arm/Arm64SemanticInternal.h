#ifndef __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICINTERNAL_H
#define __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICINTERNAL_H

/*
 * Arm64SemanticInternal.h
 *
 * The parts of the A64 lifter which are split per area (Arm64SemanticLift.cpp:
 * data processing, branches and system; Arm64SemanticLiftMemory.cpp: loads,
 * stores, exclusives and atomics), and the helpers they share. Not a public
 * header: the lifter is ArmSemanticLifter (ArmSemantics.h).
 *
 * The lifters of the A64 base table decode the bits of the instruction
 * (ArmOpcode::getEncoding()) and not the operands, since the aliases (mov,
 * cmp, lsl, cset...) present their own operands: one lifter serves a
 * mnemonic and all its aliases. The memory operands are the exception: the
 * decoded address (ArmLiftContext::getAddress()) is used.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/arm/ArmSemantics.h"

namespace armdis {

/*
 * The lifter of the instructions of the A64 base table: data processing,
 * branches, exceptions, system, loads and stores. Every entry of
 * gArm64SemanticTable is this function.
 */
void liftArm64Base(ArmLiftContext& context);

// Arm64SemanticLift.cpp: the data processing, the branches and the system
// instructions
void liftArm64DataProcessing(ArmLiftContext& context);
void liftArm64BranchSystem(ArmLiftContext& context);
// Arm64SemanticLiftMemory.cpp: the loads and the stores
void liftArm64LoadStore(ArmLiftContext& context);

namespace a64 {

// The SemanticFlags of N Z C V
enum {
    FLAGS_NZCV = SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO |
                 SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW
};

// Return bits 'high' to 'low' of the encoding
inline uint32 bits(uint32 encoding, uint high, uint low)
{
    return (encoding >> low) & ((uint32)((1ull << (high - low + 1)) - 1));
}

// Return the integer type of a 32 / 64 bit register
inline SemanticType typeOf(bool is64)
{
    return SemanticType::integer(is64 ? 64 : 32);
}

// Return the mask of 'bits' bits (1 to 64)
inline uint64 maskOf(uint bits)
{
    return (bits >= 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
}

// Return true if the entry mnemonic of the instruction is 'name'
bool isMnemonic(const ArmLiftContext& context, const char* name);
// Return the entry mnemonic of the instruction
const char* getMnemonic(const ArmLiftContext& context);

// Read general purpose register 'number' (0-31) as x / w. Register 31 is the
// stack pointer if 'isSp', the zero register otherwise
SemanticOperand readGpr(ArmLiftContext& context, uint number, bool is64,
                        bool isSp);
// Write a general purpose register. A write to the zero register is dropped
void writeGpr(ArmLiftContext& context, uint number, bool is64, bool isSp,
              const SemanticOperand& value);

// Add an OP_COMPARE of the values 'a' and 'b' of 'type' and return the 1 bit
// result
SemanticOperand compare(ArmLiftContext& context,
                        SemanticOperation::Condition condition,
                        const SemanticType& type,
                        const SemanticOperand& a, const SemanticOperand& b);

// Return the 1 bit value of a boolean constant
SemanticOperand booleanConstant(ArmLiftContext& context, bool value);

// Return an operation on 'type' with the immediate 'value' (convenience)
SemanticOperand emitImmediate(ArmLiftContext& context,
                              SemanticOperation::Operation operation,
                              const SemanticType& type,
                              const SemanticOperand& a, int64 value);

// Return the sign extension / zero extension of 'value' (of a type narrower
// than 'type') into 'type'
SemanticOperand extend(ArmLiftContext& context, const SemanticType& type,
                       const SemanticOperand& value, bool isSigned);

// Return an OP_NOT of 'value' (a constant for an immediate)
SemanticOperand invert(ArmLiftContext& context, const SemanticOperand& value);

// Return the value of NZCV as the bits of mrs (N 31, Z 30, C 29, V 28)
SemanticOperand packNzcv(ArmLiftContext& context);
// Set N Z C V from the bits 31-28 of the 64 bit value
void unpackNzcv(ArmLiftContext& context, const SemanticOperand& value);

// The clear of the exclusive monitor
void clearExclusiveMonitor(ArmLiftContext& context);

// Add an OP_UNDEFINED
void undefined(ArmLiftContext& context);

} // namespace a64

} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICINTERNAL_H
