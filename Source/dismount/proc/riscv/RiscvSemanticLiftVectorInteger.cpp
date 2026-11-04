/*
 * RiscvSemanticLiftVectorInteger.cpp
 *
 * Implementation file
 *
 * The vector integer arithmetic (OPIVV, OPIVX, OPIVI and the integer OPMVV,
 * OPMVX): add and subtract, logic, shifts, min and max, multiply and divide,
 * the widening and narrowing forms, the carries, the compares into masks,
 * the multiply-add forms, vmerge and vmv.v.*, vzext and vsext. Each is a
 * RvvPlan (RiscvSemanticVector.h) and an element operation on the lane
 * vectors of a chunk. The fixed point instructions are in
 * RiscvSemanticLiftVectorFixed.cpp.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"
#include "dismount/proc/riscv/RiscvSemanticVector.h"

namespace riscvdis {

typedef SemanticOperation Op;

RvvPlan makeArithmeticPlan(uint64 parameter)
{
    RvvPlan plan;
    plan.m_operation = (uint)(parameter & 0xFF);
    plan.m_shape = (RvvPlan::Shape)((parameter >> 8) & 0xF);
    plan.m_extendA = (RvvPlan::Extend)((parameter >> 12) & 3);
    plan.m_extendB = (RvvPlan::Extend)((parameter >> 14) & 3);
    plan.m_mask = (RvvPlan::MaskMode)((parameter >> 16) & 3);
    const uint flags = (uint)((parameter >> 20) & 0xFF);
    plan.m_isImmediateUnsigned = (flags & RVV_FLAG_IMMEDIATE_UNSIGNED) != 0;
    plan.m_readsDestination = (flags & RVV_FLAG_READS_DESTINATION) != 0;
    plan.m_setsSaturation = (flags & RVV_FLAG_SATURATES) != 0;
    plan.m_hasNoB = (flags & RVV_FLAG_NO_B) != 0;
    plan.m_factor = (uint)1 << ((parameter >> 28) & 7);
    plan.m_op = (plan.m_operation >= RVV_OP_FIXED_FIRST) ? fixedElementOp
                                                          : integerElementOp;
    return plan;
}

namespace {

void integerBody(RvvLift& lift, const RvvType& type)
{
    RvvPlan plan = makeArithmeticPlan(lift.context().getParameter());
    lift.elementwise(plan, type);
}

} // namespace

void liftVectorInteger(RiscvLiftContext& context)
{
    RvvLift lift(context);
    lift.dispatch(integerBody);
    lift.finish();
}

SemanticOperand integerElementOp(RvvChunk& c)
{
    RvvLift& l = c.m_lift;
    const SemanticType t = c.m_vectorType;
    const uint bits = c.m_bits;
    const SemanticOperand& a = c.m_a;
    const SemanticOperand& b = c.m_b;
    // The shift count: the low log2(bits) bits
    switch (c.m_plan.m_operation)
    {
    case RVV_OP_ADD:
        return l.emit(Op::OP_ADD, t, a, b);
    case RVV_OP_SUB:
        return l.emit(Op::OP_SUBTRACT, t, a, b);
    case RVV_OP_RSUB:
        return l.emit(Op::OP_SUBTRACT, t, b, a);
    case RVV_OP_MINU:
        return l.emit(Op::OP_MINIMUM_UNSIGNED, t, a, b);
    case RVV_OP_MIN:
        return l.emit(Op::OP_MINIMUM_SIGNED, t, a, b);
    case RVV_OP_MAXU:
        return l.emit(Op::OP_MAXIMUM_UNSIGNED, t, a, b);
    case RVV_OP_MAX:
        return l.emit(Op::OP_MAXIMUM_SIGNED, t, a, b);
    case RVV_OP_AND:
        return l.emit(Op::OP_AND, t, a, b);
    case RVV_OP_OR:
        return l.emit(Op::OP_OR, t, a, b);
    case RVV_OP_XOR:
        return l.emit(Op::OP_XOR, t, a, b);
    case RVV_OP_SLL:
    case RVV_OP_SRL:
    case RVV_OP_SRA:
    case RVV_OP_NSRL:
    case RVV_OP_NSRA:
    {
        SemanticOperand count = l.emit(Op::OP_AND, t, b, l.splat(bits - 1, t));
        Op::Operation operation = Op::OP_SHIFT_LEFT;
        if ((c.m_plan.m_operation == RVV_OP_SRL) ||
            (c.m_plan.m_operation == RVV_OP_NSRL))
            operation = Op::OP_SHIFT_RIGHT_LOGICAL;
        else if ((c.m_plan.m_operation == RVV_OP_SRA) ||
                 (c.m_plan.m_operation == RVV_OP_NSRA))
            operation = Op::OP_SHIFT_RIGHT_ARITHMETIC;
        return l.emit(operation, t, a, count);
    }
    case RVV_OP_MUL:
        return l.emit(Op::OP_MULTIPLY, t, a, b);
    case RVV_OP_MULH:
        return l.emit(Op::OP_MULTIPLY_HIGH_SIGNED, t, a, b);
    case RVV_OP_MULHU:
        return l.emit(Op::OP_MULTIPLY_HIGH_UNSIGNED, t, a, b);
    case RVV_OP_MULHSU:
        // vs2 signed, vs1 / rs1 unsigned
        return l.emit(Op::OP_MULTIPLY_HIGH_SIGNED_UNSIGNED, t, a, b);
    case RVV_OP_DIVU:
        return l.emitBehavior(Op::OP_DIVIDE_UNSIGNED, t, a, b,
                              Op::BEHAVIOR_DIVIDE_ALL_ONES);
    case RVV_OP_DIV:
        return l.emitBehavior(Op::OP_DIVIDE_SIGNED, t, a, b,
                              Op::BEHAVIOR_DIVIDE_ALL_ONES);
    case RVV_OP_REMU:
        return l.emitBehavior(Op::OP_REMAINDER_UNSIGNED, t, a, b,
                              Op::BEHAVIOR_DIVIDE_ALL_ONES);
    case RVV_OP_REM:
        return l.emitBehavior(Op::OP_REMAINDER_SIGNED, t, a, b,
                              Op::BEHAVIOR_DIVIDE_ALL_ONES);
    case RVV_OP_ADC:
    case RVV_OP_SBC:
    {
        SemanticOperand carry = l.select(c.m_carry, l.splat(1, t), l.splat(0, t));
        if (c.m_plan.m_operation == RVV_OP_ADC)
            return l.emit(Op::OP_ADD, t, l.emit(Op::OP_ADD, t, a, b), carry);
        return l.emit(Op::OP_SUBTRACT, t, l.emit(Op::OP_SUBTRACT, t, a, b),
                      carry);
    }
    case RVV_OP_MADC:
    case RVV_OP_MSBC:
    {
        // The carry (borrow) out of a + b + carry (a - b - borrow)
        SemanticOperand carry = c.m_carry.isValid() ?
                    l.select(c.m_carry, l.splat(1, t), l.splat(0, t)) :
                    l.splat(0, t);
        const bool isAdd = c.m_plan.m_operation == RVV_OP_MADC;
        SemanticOperand first = l.emit(isAdd ? Op::OP_ADD : Op::OP_SUBTRACT,
                                       t, a, b);
        SemanticOperand firstOut = l.compare(isAdd ? Op::COND_UNSIGNED_LESS :
                                             Op::COND_UNSIGNED_LESS, t,
                                             isAdd ? first : a,
                                             isAdd ? a : b);
        SemanticOperand second = l.emit(isAdd ? Op::OP_ADD : Op::OP_SUBTRACT,
                                        t, first, carry);
        SemanticOperand secondOut = l.compare(Op::COND_UNSIGNED_LESS, t,
                                              isAdd ? second : first,
                                              isAdd ? first : carry);
        return l.emit(Op::OP_OR, c.m_maskType, firstOut, secondOut);
    }
    case RVV_OP_MERGE:
        // v0 selects vs1 / the scalar
        return l.select(c.m_carry, b, a);
    case RVV_OP_MOVE:
        return b;
    case RVV_OP_SEQ:
        return l.compare(Op::COND_EQUAL, t, a, b);
    case RVV_OP_SNE:
        return l.compare(Op::COND_NOT_EQUAL, t, a, b);
    case RVV_OP_SLTU:
        return l.compare(Op::COND_UNSIGNED_LESS, t, a, b);
    case RVV_OP_SLT:
        return l.compare(Op::COND_SIGNED_LESS, t, a, b);
    case RVV_OP_SLEU:
        return l.compare(Op::COND_UNSIGNED_LESS_EQUAL, t, a, b);
    case RVV_OP_SLE:
        return l.compare(Op::COND_SIGNED_LESS_EQUAL, t, a, b);
    case RVV_OP_SGTU:
        return l.compare(Op::COND_UNSIGNED_GREATER, t, a, b);
    case RVV_OP_SGT:
        return l.compare(Op::COND_SIGNED_GREATER, t, a, b);
    case RVV_OP_MACC:
        return l.emit(Op::OP_ADD, t, l.emit(Op::OP_MULTIPLY, t, b, a), c.m_old);
    case RVV_OP_NMSAC:
        return l.emit(Op::OP_SUBTRACT, t, c.m_old,
                      l.emit(Op::OP_MULTIPLY, t, b, a));
    case RVV_OP_MADD:
        return l.emit(Op::OP_ADD, t, l.emit(Op::OP_MULTIPLY, t, b, c.m_old), a);
    case RVV_OP_NMSUB:
        return l.emit(Op::OP_SUBTRACT, t, a,
                      l.emit(Op::OP_MULTIPLY, t, b, c.m_old));
    case RVV_OP_EXTEND:
        return a;
    case RVV_OP_ID:
        // The element numbers
        return l.emit(Op::OP_LANE_INDEX, t, l.imm(c.m_firstElement, l.xlenType()),
                      l.immX(1));
    default:
        CHECK_FAIL();
    }
}

} // namespace riscvdis
