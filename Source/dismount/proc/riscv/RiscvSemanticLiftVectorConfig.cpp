/*
 * RiscvSemanticLiftVectorConfig.cpp
 *
 * Implementation file
 *
 * The configuration instructions vsetvli, vsetivli and vsetvl, and the
 * helpers that compute the validity and VLMAX of a vtype value at runtime.
 *
 * The decisions where the specification leaves a choice:
 *   - vl = min(AVL, VLMAX) (the specification allows ceil(AVL / 2) <= vl <=
 *     VLMAX for AVL < 2 * VLMAX; both reference processors use the minimum).
 *   - A vtype with a reserved value (vsew > 3, vlmul = 4, SEW > ELEN *
 *     min(LMUL, 1), any bit above bit 7 set, including bit 8, the altfmt of
 *     Zvfbfa which isn't modeled here) sets vill: vtype is the vill bit only
 *     and vl is 0.
 *   - vsetvl{i} x0, x0 (keep vl) with a vtype whose VLMAX differs from the
 *     VLMAX of the current vtype (SEW / LMUL ratio changes) is reserved:
 *     vill is set, as Spike does (the specification allows an illegal
 *     instruction exception too). QEMU keeps vl if it fits instead, see
 *     tests/riscv/semantics/oracle_bugs.txt. With the current vtype illegal
 *     (VLMAX 0) any valid new vtype changes the ratio: vill again.
 *   - vstart is set to 0.
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

SemanticOperand RvvLift::vtypeIsValid(const SemanticOperand& vtype)
{
    const SemanticType x = xlenType();
    // The bits above bit 7 are zero (vill, the reserved bits, altfmt)
    SemanticOperand upper = emit(Op::OP_SHIFT_RIGHT_LOGICAL, x, vtype, immX(8));
    SemanticOperand isUpperZero = compare(Op::COND_EQUAL, x, upper, immX(0));
    SemanticOperand lmul = emit(Op::OP_AND, x, vtype, immX(7));
    SemanticOperand sew = emit(Op::OP_AND, x,
            emit(Op::OP_SHIFT_RIGHT_LOGICAL, x, vtype, immX(3)), immX(7));
    SemanticOperand isLmulLegal = compare(Op::COND_NOT_EQUAL, x, lmul, immX(4));
    // SEW <= ELEN * min(LMUL, 1): the log2(SEW / 8) is at most 3 for LMUL
    // >= 1, and 0, 1, 2 for LMUL 1/8, 1/4, 1/2
    SemanticOperand isFraction = compare(Op::COND_UNSIGNED_GREATER_EQUAL, x,
                                         lmul, immX(5));
    SemanticOperand limit = emit(Op::OP_SELECT, x, isFraction,
            emit(Op::OP_SUBTRACT, x, lmul, immX(5)), immX(3));
    SemanticOperand isSewLegal = compare(Op::COND_UNSIGNED_LESS_EQUAL, x, sew,
                                         limit);
    const SemanticType b = integerType(1);
    return emit(Op::OP_AND, b, emit(Op::OP_AND, b, isUpperZero, isLmulLegal),
                isSewLegal);
}

SemanticOperand RvvLift::vtypeVlmax(const SemanticOperand& vtype)
{
    const SemanticType x = xlenType();
    SemanticOperand lmul = emit(Op::OP_AND, x, vtype, immX(7));
    SemanticOperand sew = emit(Op::OP_AND, x,
            emit(Op::OP_SHIFT_RIGHT_LOGICAL, x, vtype, immX(3)), immX(7));
    // VLEN / SEW = (VLEN / 8) >> vsew
    SemanticOperand perRegister = emit(Op::OP_SHIFT_RIGHT_LOGICAL, x,
                                       immX(m_vlen / 8), sew);
    // LMUL: 1 .. 8 shift left by the code, 1/8 .. 1/2 shift right by 8 - code
    SemanticOperand isWhole = compare(Op::COND_UNSIGNED_LESS, x, lmul, immX(4));
    SemanticOperand left = emit(Op::OP_SELECT, x, isWhole, lmul, immX(0));
    SemanticOperand right = emit(Op::OP_SELECT, x, isWhole, immX(0),
                                 emit(Op::OP_SUBTRACT, x, immX(8), lmul));
    SemanticOperand scaled = emit(Op::OP_SHIFT_LEFT, x, perRegister, left);
    return emit(Op::OP_SHIFT_RIGHT_LOGICAL, x, scaled, right);
}

// vsetvli (0), vsetivli (1), vsetvl (2)
void liftVsetvl(RiscvLiftContext& context)
{
    RvvLift lift(context);
    const uint kind = (uint)context.getParameter();
    const SemanticType x = lift.xlenType();
    const uint xlen = lift.xlen();
    const uint rd = lift.field(7, 5);
    const uint rs1 = lift.field(15, 5);
    const bool isKeep = (kind != 1) && (rd == 0) && (rs1 == 0);
    const bool isMax = (kind != 1) && (rd != 0) && (rs1 == 0);

    // The requested length and the new type
    SemanticOperand avl;
    if (kind == 1)
        avl = lift.immX(lift.field(15, 5));
    else if (!isKeep && !isMax)
        avl = lift.readX(rs1);
    SemanticOperand newType;
    if (kind == 0)
        newType = lift.immX(lift.field(20, 11));
    else if (kind == 1)
        newType = lift.immX(lift.field(20, 10));
    else
        newType = lift.readX(lift.field(20, 5));
    SemanticOperand oldType = lift.emit(Op::OP_MOVE, x, lift.vtypeRegister());

    SemanticOperand vlmax = lift.vtypeVlmax(newType);
    SemanticOperand isValid = lift.vtypeIsValid(newType);
    if (isKeep)
    {
        // The ratio must not change; the current vlmax is 0 when the current
        // vtype is illegal
        SemanticOperand wasValid = lift.compare(Op::COND_SIGNED_GREATER_EQUAL,
                                                x, oldType, lift.immX(0));
        SemanticOperand oldMax = lift.emit(Op::OP_SELECT, x, wasValid,
                                           lift.vtypeVlmax(oldType),
                                           lift.immX(0));
        SemanticOperand isSame = lift.compare(Op::COND_EQUAL, x, oldMax, vlmax);
        isValid = lift.emit(Op::OP_AND, lift.integerType(1), isValid, isSame);
    }
    // The illegal configuration is the vill bit alone
    SemanticOperand vill = lift.immX((int64)1 << (xlen - 1));
    SemanticOperand storedType = lift.emit(Op::OP_SELECT, x, isValid, newType,
                                           vill);
    lift.move(lift.vtypeRegister(), storedType);

    // vl
    SemanticOperand length;
    if (isKeep)
    {
        // vl stays, but an illegal configuration clears it
        SemanticOperand current = lift.emit(Op::OP_MOVE, x, lift.vlRegister());
        length = lift.emit(Op::OP_SELECT, x, isValid, current, lift.immX(0));
    } else if (isMax)
    {
        length = lift.emit(Op::OP_SELECT, x, isValid, vlmax, lift.immX(0));
    } else
    {
        SemanticOperand isBelow = lift.compare(Op::COND_UNSIGNED_LESS, x, avl,
                                               vlmax);
        SemanticOperand minimum = lift.emit(Op::OP_SELECT, x, isBelow, avl,
                                            vlmax);
        length = lift.emit(Op::OP_SELECT, x, isValid, minimum, lift.immX(0));
    }
    lift.move(lift.vlRegister(), length);
    lift.writeX(rd, length);
    lift.finish();
}

} // namespace riscvdis
