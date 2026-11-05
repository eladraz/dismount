/*
 * RiscvSemanticLiftVectorBits.cpp
 *
 * Implementation file
 *
 * The vector bit manipulation: Zvbb and Zvkb (vandn, vbrev, vbrev8, vrev8,
 * vclz, vctz, vcpop.v, vrol, vror, vwsll) and the carry-less multiplies of
 * Zvbc (vclmul, vclmulh: SEW = 64 only). Each is a RvvPlan
 * (RiscvSemanticVector.h) and an element operation on the lane vectors of a
 * chunk; the parameter of the entries is built by RVV_PARAM() with an
 * RvvBitsOp as the operation.
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

SemanticOperand bitsElementOp(RvvChunk& c)
{
    RvvLift& l = c.m_lift;
    const SemanticType t = c.m_vectorType;
    const uint bits = c.m_bits;
    SemanticOperand a = c.m_a;
    SemanticOperand b = c.m_b;
    switch (c.m_plan.m_operation)
    {
    case RVB_OP_ANDN:
        return l.emit(Op::OP_AND_NOT, t, b, a);
    case RVB_OP_BREV:
        return l.emit(Op::OP_BIT_REVERSE, t, a);
    case RVB_OP_BREV8:
        // The bits of the element reversed, then the bytes: the bits of
        // each byte reversed in place
        return l.emit(Op::OP_BYTE_SWAP, t, l.emit(Op::OP_BIT_REVERSE, t, a));
    case RVB_OP_REV8:
        return l.emit(Op::OP_BYTE_SWAP, t, a);
    case RVB_OP_CLZ:
        return l.emit(Op::OP_COUNT_LEADING_ZEROS, t, a);
    case RVB_OP_CTZ:
        return l.emit(Op::OP_COUNT_TRAILING_ZEROS, t, a);
    case RVB_OP_CPOP:
        return l.emit(Op::OP_POPULATION_COUNT, t, a);
    case RVB_OP_ROL:
    case RVB_OP_ROR:
    {
        // vror.vi has a 6 bit immediate: bit 5 is bit 26 of the encoding
        if ((l.funct3() == 3) && (c.m_plan.m_operation == RVB_OP_ROR) &&
            (l.field(26, 1) != 0))
            b = l.emit(Op::OP_OR, t, b, l.splat(32, t));
        SemanticOperand count = l.emit(Op::OP_AND, t, b, l.splat(bits - 1, t));
        return l.emit((c.m_plan.m_operation == RVB_OP_ROL) ?
                      Op::OP_ROTATE_LEFT : Op::OP_ROTATE_RIGHT, t, a, count);
    }
    case RVB_OP_WSLL:
    {
        SemanticOperand count = l.emit(Op::OP_AND, t, b, l.splat(bits - 1, t));
        return l.emit(Op::OP_SHIFT_LEFT, t, a, count);
    }
    case RVB_OP_CLMUL:
        return l.emit(Op::OP_CARRYLESS_MULTIPLY, t, a, b);
    case RVB_OP_CLMULH:
        return l.emit(Op::OP_CARRYLESS_MULTIPLY_HIGH, t, a, b);
    default:
        CHECK_FAIL();
    }
}

namespace {

void bitsBody(RvvLift& lift, const RvvType& type)
{
    RvvPlan plan = makeArithmeticPlan(lift.context().getParameter());
    plan.m_op = bitsElementOp;
    // Zvbc works on 64 bit elements only
    if (((plan.m_operation == RVB_OP_CLMUL) ||
         (plan.m_operation == RVB_OP_CLMULH)) && (type.m_sew != 64))
    {
        lift.trapIllegal();
        return;
    }
    lift.elementwise(plan, type);
}

} // namespace

void liftVectorBits(RiscvLiftContext& context)
{
    RvvLift lift(context);
    lift.dispatch(bitsBody);
    lift.finish();
}

} // namespace riscvdis
