/*
 * RiscvSemanticLiftCommon.cpp
 *
 * Implementation file
 *
 * The helpers the hand written lifters of the scalar RISC-V instructions
 * share (see RiscvSemanticInternal.h).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"

namespace riscvdis {

const char* getMnemonic(RiscvLiftContext& context)
{
    return context.getInstruction().getEntry()->m_mnemonic;
}

uint64 maskOf(uint bits)
{
    return (bits >= 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
}

SemanticOperand makeConstant(RiscvLiftContext& context, int64 value,
                             const SemanticType& type)
{
    return context.getImmediate(value, type);
}

SemanticOperand makeCompare(RiscvLiftContext& context,
                            SemanticOperation::Condition condition,
                            const SemanticType& type,
                            const SemanticOperand& a,
                            const SemanticOperand& b)
{
    SemanticOperand result = context.newTemporary(integerType(1));
    SemanticInstruction compare(Op::OP_COMPARE, type, result, a, b);
    compare.setCondition(condition);
    context.emit(compare);
    return result;
}

SemanticOperand makeSelect(RiscvLiftContext& context, const SemanticType& type,
                           const SemanticOperand& condition,
                           const SemanticOperand& a,
                           const SemanticOperand& b)
{
    return context.emit(Op::OP_SELECT, type, condition, a, b);
}

SemanticOperand makeZeroExtend(RiscvLiftContext& context,
                               const SemanticOperand& value, uint bits)
{
    const uint valueBits = value.getType().getTotalBits();
    if (valueBits == bits)
        return value;
    if (valueBits > bits)
        return context.emit(Op::OP_TRUNCATE, integerType(bits), value);
    return context.emit(Op::OP_ZERO_EXTEND, integerType(bits), value);
}

SemanticOperand makeSignExtend(RiscvLiftContext& context,
                               const SemanticOperand& value, uint bits)
{
    const uint valueBits = value.getType().getTotalBits();
    if (valueBits == bits)
        return value;
    if (valueBits > bits)
        return context.emit(Op::OP_TRUNCATE, integerType(bits), value);
    return context.emit(Op::OP_SIGN_EXTEND, integerType(bits), value);
}

SemanticOperand makeTruncate(RiscvLiftContext& context,
                             const SemanticOperand& value, uint bits)
{
    if (value.getType().getTotalBits() == bits)
        return value;
    return context.emit(Op::OP_TRUNCATE, integerType(bits), value);
}

void makeTrap(RiscvLiftContext& context, SemanticOperation::TrapKind kind,
              uint number)
{
    context.trap(kind, number);
}

void makeTrapIf(RiscvLiftContext& context, const SemanticOperand& condition,
                SemanticOperation::TrapKind kind, uint number)
{
    SemanticOperand skip = context.newLabel();
    SemanticOperand inverted = context.emit(Op::OP_XOR, integerType(1),
                                    condition, makeConstant(context, 1,
                                                            integerType(1)));
    context.branchIf(inverted, skip);
    context.trap(kind, number);
    context.placeLabel(skip);
}

SemanticOperand makeBitcast(RiscvLiftContext& context,
                            const SemanticOperand& value,
                            const SemanticType& type)
{
    return context.emit(Op::OP_BITCAST, type, value);
}

SemanticOperand makeLoad(RiscvLiftContext& context,
                         const SemanticOperand& address,
                         const SemanticType& type,
                         SemanticOperation::MemoryOrder order)
{
    SemanticOperand result = context.newTemporary(type);
    SemanticInstruction instruction(Op::OP_LOAD, type, result, address);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    instruction.setMemoryOrder(order);
    context.emit(instruction);
    return result;
}

void makeStore(RiscvLiftContext& context, const SemanticOperand& address,
               const SemanticOperand& value,
               SemanticOperation::MemoryOrder order)
{
    SemanticInstruction instruction(Op::OP_STORE, value.getType(),
                                    SemanticOperand(), address, value);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    instruction.setMemoryOrder(order);
    context.emit(instruction);
}

SemanticOperation::MemoryOrder getAtomicOrder(RiscvLiftContext& context)
{
    // The aq bit is 26 and the rl bit 25
    const uint32 encoding = context.getInstruction().getEncoding();
    const bool isAcquire = ((encoding >> 26) & 1) != 0;
    const bool isRelease = ((encoding >> 25) & 1) != 0;
    if (isAcquire && isRelease)
        return Op::ORDER_SEQUENTIAL;
    if (isAcquire)
        return Op::ORDER_ACQUIRE;
    if (isRelease)
        return Op::ORDER_RELEASE;
    return Op::ORDER_RELAXED;
}

void liftHint(RiscvLiftContext& context)
{
    // The hints and the instructions with a destination of x0 and no effect
    context.emit(SemanticInstruction(Op::OP_NOP, SemanticType(),
                                     SemanticOperand()));
}

void liftLandingPadCheck(RiscvLiftContext& context)
{
    makeTrapIf(context, context.getRegister(RISCV_REG_ELP),
               Op::TRAP_FAULT, RISCV_TRAP_SOFTWARE_CHECK);
}

} // namespace riscvdis
