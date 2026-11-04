/*
 * Arm64SemanticLiftSmeOuter.cpp
 *
 * Implementation file
 *
 * The SME outer products (fmopa / fmops, bfmopa / bfmops, smopa, umopa, sumopa, usmopa and the
 * s forms, bmopa / bmops) and addha / addva, see Arm64SemanticSme.h. A tile is updated by
 * rows: the row r of the accumulator takes the element r (or the group r) of the first
 * source against the whole second source.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include <string.h>
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64SemanticInternal.h"
#include "dismount/proc/arm/Arm64SemanticSme.h"

using namespace armdis;

typedef SemanticOperation Op;
typedef SmeLift::Val Val;

namespace {

uint elementBitsOf(uint arrangement)
{
    switch (arrangement)
    {
    case ArmDecodedOperand::ARRANGEMENT_B: return 8;
    case ArmDecodedOperand::ARRANGEMENT_H: return 16;
    case ArmDecodedOperand::ARRANGEMENT_S: return 32;
    case ArmDecodedOperand::ARRANGEMENT_D: return 64;
    default: return 0;
    }
}

// The mask of the columns of the row r of a widening outer product of 16 bit elements: the
// column c is updated when a pair element of the row and of the column are both active
Val pairColumns(SmeLift& m, const Val& rowBits, uint r, const Val& colBits)
{
    Val result;
    for (uint k = 0; k < 2; k++)
    {
        Val cols = m.deinterleaveBits(colBits, k);
        Val gated = m.c().emit(Op::OP_SELECT, cols.getType(), m.bitOf(rowBits, 2 * r + k),
                               cols, m.imm(0, cols.getType().getTotalBits()));
        result = result.isValid() ?
            m.c().emit(Op::OP_OR, cols.getType(), result, gated) : gated;
    }
    return m.bitsToMask(result);
}

void liftFloatOuter(SmeLift& m)
{
    ArmLiftContext& c = m.c();
    const bool isSubtract = m.name()[strlen(m.name()) - 1] == 's';
    const bool isBf = m.name()[0] == 'b';
    const SmeZa za = m.za(0);
    const uint pn = m.operand(1).m_register - ARM64_REG_P0;
    const uint pm = m.operand(2).m_register - ARM64_REG_P0;
    const uint zn = m.operand(3).m_register - ARM64_REG_Z0;
    const uint zm = m.operand(4).m_register - ARM64_REG_Z0;
    const uint esize = za.m_elementBits;
    const uint sourceBits = elementBitsOf(m.operand(3).m_arrangement);
    const uint dim = m.lanes(esize);
    m.begin(true, true);

    if (sourceBits == esize)
    {
        // fmopa za.s, z.s, z.s / za.d, z.d, z.d: a fused multiply add of the element
        Val a = m.zv(zn, esize);
        Val b = m.zv(zm, esize);
        Val colMask = m.laneMask(pm, esize);
        Val rowBits = m.maskToBits(m.laneMask(pn, esize));
        for (uint r = 0; r < dim; r++)
        {
            Val e1 = m.lane(a, r);
            if (isSubtract)
                e1 = m.flipSign(e1, esize);
            Val acc = m.tileRow(za, r);
            Val updated = m.fp(Op::OP_FLOAT_MULTIPLY_ADD, esize, m.splat(e1, esize),
                               b, acc, true, Op::BEHAVIOR_DEFAULT, isBf);
            Val mask = m.gateMask(m.bitOf(rowBits, r), colMask);
            m.setTileRow(za, r, m.s().select(mask, updated, acc));
        }
        m.end();
        return;
    }

    // widening: the pair of 16 bit elements (2 r, 2 r + 1) against the pairs of the columns
    CHECK((sourceBits == 16) && (esize == 32));
    const uint n16 = m.lanes(16);
    Val a16 = m.zv(zn, 16);
    Val b16 = m.zv(zm, 16);
    Val rowBits = m.maskToBits(m.laneMask(pn, 16));
    Val colBits = m.maskToBits(m.laneMask(pm, 16));
    if (isSubtract)
        a16 = m.flipSign(a16, 16);
    // the inactive elements are zero
    if (!isBf)
    {
        // the half precision inputs of the products are flushed by FPCR.FZ16
        a16 = m.flushHalf(a16);
        b16 = m.flushHalf(b16);
    }
    Val aMasked = m.s().select(m.laneMask(pn, 16), a16, m.zeroVec(16));
    Val bMasked = m.s().select(m.laneMask(pm, 16), b16, m.zeroVec(16));
    Val aPairs = m.s().cast(SimdLift::V(32, dim), aMasked);

    const SemanticType bfVec = SemanticType::vector(
        SemanticType::floating(SemanticType::FLOAT_BFLOAT16), n16);
    const SemanticType accType = SimdLift::FV(32, dim);
    for (uint r = 0; r < dim; r++)
    {
        Val acc = m.tileRow(za, r);
        Val columns = pairColumns(m, rowBits, r, colBits);
        Val result;
        if (isBf)
        {
            // the dot product of the standard BFloat16 behaviors
            Val pair = m.lane(aPairs, r);
            Val rep = m.s().cast(SimdLift::V(16, n16),
                                 m.s().splat(SimdLift::V(32, dim), pair));
            Val dotResult = c.newTemporary(accType);
            c.emit(SemanticInstruction(Op::OP_DOT_PRODUCT, accType, dotResult,
                                       m.s().cast(bfVec, rep), m.s().cast(bfVec, bMasked),
                                       m.s().cast(accType, acc)));
            result = m.s().cast(m.vecType(32), dotResult);
        } else
        {
            // the products are exact in single precision; the sum of the two products is rounded
            // and added to the accumulator
            Val products[2];
            for (uint k = 0; k < 2; k++)
            {
                Val eA = m.lane(aMasked, 2 * r + k);
                Val sA = m.fpConvert(eA, SemanticType::FLOAT_HALF, SemanticType::FLOAT_SINGLE,
                                     1, true);
                Val bK = m.deinterleave(bMasked, k);
                Val sB = m.fpConvert(bK, SemanticType::FLOAT_HALF, SemanticType::FLOAT_SINGLE,
                                     dim, true);
                products[k] = m.fp(Op::OP_FLOAT_MULTIPLY, 32, m.splat(sA, 32), sB, Val(), true);
            }
            Val total = m.fp(Op::OP_FLOAT_ADD, 32, products[0], products[1], Val(), true);
            result = m.fp(Op::OP_FLOAT_ADD, 32, acc, total, Val(), true);
        }
        m.setTileRow(za, r, m.s().select(columns, result, acc));
    }
    m.end();
}

void liftIntegerOuter(SmeLift& m)
{
    ArmLiftContext& c = m.c();
    const char* name = m.name();
    const bool isSubtract = name[strlen(name) - 1] == 's';
    // smopa umopa sumopa usmopa: the signedness of the first and second source
    const bool firstSigned = (name[0] == 's');
    const bool secondSigned = (name[0] == 's' && name[1] == 'm') || (name[0] == 'u' && name[1] == 's');
    const SmeZa za = m.za(0);
    const uint pn = m.operand(1).m_register - ARM64_REG_P0;
    const uint pm = m.operand(2).m_register - ARM64_REG_P0;
    const uint zn = m.operand(3).m_register - ARM64_REG_Z0;
    const uint zm = m.operand(4).m_register - ARM64_REG_Z0;
    const uint esize = za.m_elementBits;
    const uint ssize = elementBitsOf(m.operand(3).m_arrangement);
    const uint ways = esize / ssize;
    const uint dim = m.lanes(esize);
    m.begin(true, true);
    Val a = m.s().select(m.laneMask(pn, ssize), m.zv(zn, ssize), m.zeroVec(ssize));
    Val b = m.s().select(m.laneMask(pm, ssize), m.zv(zm, ssize), m.zeroVec(ssize));
    Val groups = m.s().cast(SimdLift::V(ways * ssize, dim), a);
    Op::Behavior behavior;
    bool swap = false;
    if (firstSigned && secondSigned)
        behavior = Op::BEHAVIOR_SIGNED;
    else if (!firstSigned && !secondSigned)
        behavior = Op::BEHAVIOR_UNSIGNED;
    else
    {
        behavior = Op::BEHAVIOR_UNSIGNED_SIGNED;
        // the first source of the dot product is the unsigned one
        swap = firstSigned;
    }
    const SemanticType accType = SimdLift::V(esize, dim);
    for (uint r = 0; r < dim; r++)
    {
        Val acc = m.tileRow(za, r);
        Val group = m.lane(groups, r);
        Val rep = m.s().cast(m.vecType(ssize), m.s().splat(SimdLift::V(ways * ssize, dim), group));
        Val x = swap ? b : rep;
        Val y = swap ? rep : b;
        Val sum = c.newTemporary(accType);
        SemanticInstruction dot(Op::OP_DOT_PRODUCT, accType, sum, x, y, m.zeroVec(esize));
        dot.setBehavior(behavior);
        c.emit(dot);
        m.setTileRow(za, r, c.emit(isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD, accType, acc, sum));
    }
    m.end();
}

void liftBinaryOuter(SmeLift& m)
{
    ArmLiftContext& c = m.c();
    const bool isSubtract = m.is("bmops");
    const SmeZa za = m.za(0);
    const uint pn = m.operand(1).m_register - ARM64_REG_P0;
    const uint pm = m.operand(2).m_register - ARM64_REG_P0;
    const uint zn = m.operand(3).m_register - ARM64_REG_Z0;
    const uint zm = m.operand(4).m_register - ARM64_REG_Z0;
    const uint dim = m.lanes(32);
    m.begin(true, true);
    Val a = m.zv(zn, 32);
    Val b = m.zv(zm, 32);
    Val colMask = m.laneMask(pm, 32);
    Val rowBits = m.maskToBits(m.laneMask(pn, 32));
    for (uint r = 0; r < dim; r++)
    {
        Val acc = m.tileRow(za, r);
        Val same = c.emit(Op::OP_NOT, m.vecType(32),
                          c.emit(Op::OP_XOR, m.vecType(32), m.splat(m.lane(a, r), 32), b));
        Val count = c.emit(Op::OP_POPULATION_COUNT, m.vecType(32), same);
        Val updated = c.emit(isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD, m.vecType(32), acc, count);
        m.setTileRow(za, r, m.s().select(m.gateMask(m.bitOf(rowBits, r), colMask), updated, acc));
    }
    m.end();
}

void liftAddha(SmeLift& m)
{
    ArmLiftContext& c = m.c();
    const bool isHorizontal = m.is("addha");
    const SmeZa za = m.za(0);
    const uint pn = m.operand(1).m_register - ARM64_REG_P0;
    const uint pm = m.operand(2).m_register - ARM64_REG_P0;
    const uint zn = m.operand(3).m_register - ARM64_REG_Z0;
    const uint esize = za.m_elementBits;
    const uint dim = m.lanes(esize);
    m.begin(true, true);
    Val a = m.zv(zn, esize);
    Val colMask = m.laneMask(pm, esize);
    Val rowBits = m.maskToBits(m.laneMask(pn, esize));
    for (uint r = 0; r < dim; r++)
    {
        Val acc = m.tileRow(za, r);
        Val addend = isHorizontal ? a : m.splat(m.lane(a, r), esize);
        Val updated = c.emit(Op::OP_ADD, m.vecType(esize), acc, addend);
        m.setTileRow(za, r, m.s().select(m.gateMask(m.bitOf(rowBits, r), colMask), updated, acc));
    }
    m.end();
}

} // namespace

bool armdis::liftSmeOuter(SmeLift& m)
{
    const char* name = m.name();
    if ((m.count() == 5) && (m.operand(0).m_kind == ArmDecodedOperand::KIND_MATRIX) &&
        (m.operand(3).m_kind == ArmDecodedOperand::KIND_REGISTER))
    {
        const uint esize = elementBitsOf(m.operand(0).m_arrangement);
        const uint sourceBits = elementBitsOf(m.operand(3).m_arrangement);
        if (m.is("fmopa") || m.is("fmops") || m.is("bfmopa") || m.is("bfmops"))
        {
            // the same size (single, double, half, BFloat16 with the FEAT_SME_B16B16) or the
            // pair of 16 bit elements of a single precision tile; the 8 bit floating point
            // sources are not lifted
            const bool isBf = m.name()[0] == 'b';
            const bool isSame = (sourceBits == esize) && (esize >= 16) && (!isBf || (esize == 16));
            const bool isWidening = (sourceBits == 16) && (esize == 32);
            if (!isSame && !isWidening)
                return false;
            liftFloatOuter(m);
            return true;
        }
        if (m.is("smopa") || m.is("smops") || m.is("umopa") || m.is("umops") ||
            m.is("sumopa") || m.is("sumops") || m.is("usmopa") || m.is("usmops"))
        {
            if (!(((sourceBits == 8) && (esize == 32)) || ((sourceBits == 16) && (esize == 32)) ||
                  ((sourceBits == 16) && (esize == 64))))
                return false;
            liftIntegerOuter(m);
            return true;
        }
    }
    if ((m.is("bmopa") || m.is("bmops")) && (m.count() == 5))
    {
        liftBinaryOuter(m);
        return true;
    }
    if ((m.is("addha") || m.is("addva")) && (m.count() == 4))
    {
        liftAddha(m);
        return true;
    }
    (void)name;
    return false;
}
