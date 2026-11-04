/*
 * Arm64SemanticLiftSmeArray.cpp
 *
 * Implementation file
 *
 * The SME2 instructions which write the vectors of the za array: add / sub (accumulating, and
 * the array results forms), fadd / fsub, fmla / fmls, the dot products (sdot, udot, usdot,
 * sudot, bfdot, fdot), the vertical dot products (svdot, uvdot, suvdot, usvdot, fvdot,
 * bfvdot) and the long multiply accumulates (smlal, umlal, smlsl, umlsl, smlall, umlall,
 * smlsll, umlsll, sumlall, usmlall, fmlal, fmlsl, bfmlal, bfmlsl), see Arm64SemanticSme.h.
 *
 * The vectors of the array a group writes are (w + offset) mod (rows / group) + r * rows /
 * group, for r the register of the group.
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

// The decoded operands of an array instruction: za, the first source (a register or a group), the
// optional second source (a group, a register or an indexed register)
struct ArrayInstruction {
    SmeZa m_za;
    uint m_nreg;
    uint m_zn[4];
    uint m_sourceBits;
    bool m_hasSecond;
    bool m_isGroup;
    bool m_isIndexed;
    uint m_index;
    uint m_zm[4];
};

void listOf(SmeLift& m, uint operandIndex, uint& count, uint* regs, uint& bits)
{
    const ArmDecodedOperand& o = m.operand(operandIndex);
    count = (o.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ? o.m_count : 1;
    for (uint r = 0; r < count; r++)
        regs[r] = (o.m_register - ARM64_REG_Z0 +
                   ((o.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ? r * o.m_stride : 0)) & 31;
    bits = elementBitsOf(o.m_arrangement);
}

void decode(SmeLift& m, ArrayInstruction& a)
{
    a.m_za = m.za(0);
    listOf(m, 1, a.m_nreg, a.m_zn, a.m_sourceBits);
    a.m_hasSecond = m.count() > 2;
    a.m_isGroup = false;
    a.m_isIndexed = false;
    a.m_index = 0;
    if (a.m_hasSecond)
    {
        uint count, bits;
        listOf(m, 2, count, a.m_zm, bits);
        a.m_isGroup = m.operand(2).m_kind == ArmDecodedOperand::KIND_REGISTER_LIST;
        if (m.operand(2).m_elementIndex >= 0)
        {
            a.m_isIndexed = true;
            a.m_index = (uint)m.operand(2).m_elementIndex;
        }
    }
}

// The first row of the register r of the group: (w + offset) mod stride (rounded down to a multiple of
// 'round' before the offset is added) + r * stride + plus
Val rowOf(SmeLift& m, const ArrayInstruction& a, uint r, uint plus, uint round)
{
    const uint stride = m.rows() / a.m_nreg;
    Val base = m.arraySelect(a.m_za, 0, round);
    if (r * stride + plus == 0)
        return base;
    return m.c().emit(Op::OP_ADD, SemanticType::integer(32), base, m.imm(r * stride + plus, 32));
}

// The second source of the register r: the vector of 'elementBits' lanes
Val secondVector(SmeLift& m, const ArrayInstruction& a, uint r, uint elementBits)
{
    if (a.m_isIndexed)
        return m.indexedVector(m.z(a.m_zm[0]), elementBits, a.m_index);
    return m.zv(a.m_zm[a.m_isGroup ? r : 0], elementBits);
}

void liftAddSub(SmeLift& m)
{
    ArrayInstruction a;
    decode(m, a);
    const bool isSub = m.is("sub");
    const uint esize = a.m_za.m_elementBits;
    m.begin(true, true);
    for (uint r = 0; r < a.m_nreg; r++)
    {
        Val row = rowOf(m, a, r, 0, 1);
        Val zn = m.zv(a.m_zn[r], esize);
        Val result;
        if (a.m_hasSecond)
            // the array results forms: zn +/- zm
            result = m.c().emit(isSub ? Op::OP_SUBTRACT : Op::OP_ADD, m.vecType(esize), zn,
                                secondVector(m, a, r, esize));
        else
            result = m.c().emit(isSub ? Op::OP_SUBTRACT : Op::OP_ADD, m.vecType(esize),
                                m.asVec(m.zaRowAt(row), esize), zn);
        m.setZaRowAt(row, result);
    }
    m.end();
}

void liftFloatAdd(SmeLift& m)
{
    ArrayInstruction a;
    decode(m, a);
    const uint esize = a.m_za.m_elementBits;
    m.begin(true, true);
    for (uint r = 0; r < a.m_nreg; r++)
    {
        Val row = rowOf(m, a, r, 0, 1);
        Val acc = m.asVec(m.zaRowAt(row), esize);
        Val result = m.fp(m.name()[strlen(m.name()) - 3] == 'a' ? Op::OP_FLOAT_ADD :
                          Op::OP_FLOAT_SUBTRACT, esize, acc, m.zv(a.m_zn[r], esize), Val(), true,
                          Op::BEHAVIOR_DEFAULT, m.name()[0] == 'b');
        m.setZaRowAt(row, result);
    }
    m.end();
}

void liftFloatMla(SmeLift& m)
{
    ArrayInstruction a;
    decode(m, a);
    const bool isSub = m.is("fmls") || m.is("bfmls");
    const uint esize = a.m_za.m_elementBits;
    m.begin(true, true);
    for (uint r = 0; r < a.m_nreg; r++)
    {
        Val row = rowOf(m, a, r, 0, 1);
        Val acc = m.asVec(m.zaRowAt(row), esize);
        Val zn = m.zv(a.m_zn[r], esize);
        if (isSub)
            zn = m.flipSign(zn, esize);
        Val result = m.fp(Op::OP_FLOAT_MULTIPLY_ADD, esize, zn, secondVector(m, a, r, esize),
                          acc, true, Op::BEHAVIOR_DEFAULT, m.name()[0] == 'b');
        m.setZaRowAt(row, result);
    }
    m.end();
}

// acc + the sum of the pair of half precision products (as the widening fmopa)
Val halfDot(SmeLift& m, const Val& acc, const Val& a16, const Val& b16, uint dim)
{
    Val products[2];
    for (uint k = 0; k < 2; k++)
    {
        Val x = m.fpConvert(m.deinterleave(m.flushHalf(a16), k), SemanticType::FLOAT_HALF,
                            SemanticType::FLOAT_SINGLE, dim, true);
        Val y = m.fpConvert(m.deinterleave(m.flushHalf(b16), k), SemanticType::FLOAT_HALF,
                            SemanticType::FLOAT_SINGLE, dim, true);
        products[k] = m.fp(Op::OP_FLOAT_MULTIPLY, 32, x, y, Val(), true);
    }
    Val total = m.fp(Op::OP_FLOAT_ADD, 32, products[0], products[1], Val(), true);
    return m.fp(Op::OP_FLOAT_ADD, 32, acc, total, Val(), true);
}

// acc + the dot product of the BFloat16 pairs (the standard behaviors)
Val bfloatDot(SmeLift& m, const Val& acc, const Val& a16, const Val& b16, uint dim)
{
    const SemanticType accType = SimdLift::FV(32, dim);
    const SemanticType bfVec = SemanticType::vector(
        SemanticType::floating(SemanticType::FLOAT_BFLOAT16), 2 * dim);
    Val result = m.c().newTemporary(accType);
    m.c().emit(SemanticInstruction(Op::OP_DOT_PRODUCT, accType, result,
                                   m.s().cast(bfVec, a16), m.s().cast(bfVec, b16),
                                   m.s().cast(accType, acc)));
    return m.s().cast(m.vecType(32), result);
}

void liftDot(SmeLift& m)
{
    ArrayInstruction a;
    decode(m, a);
    const char* name = m.name();
    const uint esize = a.m_za.m_elementBits;
    const uint ssize = a.m_sourceBits;
    const uint ways = esize / ssize;
    const uint dim = m.lanes(esize);
    const bool isFloat = m.is("fdot") || m.is("bfdot");
    m.begin(true, true);
    for (uint r = 0; r < a.m_nreg; r++)
    {
        Val row = rowOf(m, a, r, 0, 1);
        Val acc = m.asVec(m.zaRowAt(row), esize);
        Val zn = m.zv(a.m_zn[r], ssize);
        // the second source: the groups of 'ways' elements are the 'esize' bit lanes
        Val zm = a.m_isIndexed ? m.indexedVector(m.z(a.m_zm[0]), esize, a.m_index)
                               : m.asVec(m.z(a.m_zm[a.m_isGroup ? r : 0]), esize);
        zm = m.s().cast(m.vecType(ssize), zm);
        Val result;
        if (isFloat)
        {
            if (m.is("bfdot"))
                result = bfloatDot(m, acc, zn, zm, dim);
            else
                result = halfDot(m, acc, zn, zm, dim);
        } else
        {
            // sdot udot usdot sudot
            Op::Behavior behavior;
            bool swap = false;
            if (name[0] == 's' && name[1] == 'd')
                behavior = Op::BEHAVIOR_SIGNED;
            else if (name[0] == 'u' && name[1] == 'd')
                behavior = Op::BEHAVIOR_UNSIGNED;
            else
            {
                behavior = Op::BEHAVIOR_UNSIGNED_SIGNED;
                // usdot: zn unsigned, zm signed; sudot: zn signed, zm unsigned
                swap = (name[0] == 's');
            }
            Val x = swap ? zm : zn;
            Val y = swap ? zn : zm;
            const SemanticType accType = SimdLift::V(esize, dim);
            result = m.c().newTemporary(accType);
            SemanticInstruction dot(Op::OP_DOT_PRODUCT, accType, result, x, y, acc);
            dot.setBehavior(behavior);
            m.c().emit(dot);
        }
        m.setZaRowAt(row, result);
    }
    (void)ways;
    m.end();
}

// svdot uvdot suvdot usvdot fvdot bfvdot
void liftVerticalDot(SmeLift& m)
{
    ArrayInstruction a;
    decode(m, a);
    const char* name = m.name();
    const uint esize = a.m_za.m_elementBits;
    const uint ssize = a.m_sourceBits;
    const uint ways = a.m_nreg;
    const uint dim = m.lanes(esize);
    m.begin(true, true);
    Val chunks = m.s().cast(m.vecType(ssize),
                            m.indexedVector(m.z(a.m_zm[0]), ways * ssize, a.m_index));
    const bool isFloat = m.is("fvdot") || m.is("bfvdot");
    bool firstSigned = false, secondSigned = false;
    if (!isFloat)
    {
        firstSigned = (name[0] == 's');
        secondSigned = (name[0] == 's' && name[1] == 'v') || (name[0] == 'u' && name[1] == 's');
    }
    for (uint r = 0; r < ways; r++)
    {
        Val row = rowOf(m, a, r, 0, 1);
        Val acc = m.asVec(m.zaRowAt(row), esize);
        // the elements (ways * i + r) of the registers of the group
        Val columns[4];
        Val weights[4];
        for (uint k = 0; k < ways; k++)
        {
            columns[k] = m.deinterleave(m.zv(a.m_zn[k], ssize), r, ways);
            weights[k] = m.deinterleave(chunks, k, ways);
        }
        Val result;
        if (isFloat && m.is("bfvdot"))
        {
            // the pairs (columns 0, columns 1) against the pairs of the chunks
            Val e0 = m.s().cast(SemanticType::integer(m.svl() / 2), columns[0]);
            Val e1 = m.s().cast(SemanticType::integer(m.svl() / 2), columns[1]);
            const SemanticType wide = SimdLift::V(16, 2 * dim);
            Val x0 = m.s().cast(wide, m.c().emit(Op::OP_ZERO_EXTEND, SemanticType::integer(m.svl()), e0));
            Val x1 = m.s().cast(wide, m.c().emit(Op::OP_ZERO_EXTEND, SemanticType::integer(m.svl()), e1));
            Val pairs = m.c().emit(Op::OP_INTERLEAVE, wide, x0, x1, m.imm(0, 32));
            result = bfloatDot(m, acc, pairs, chunks, dim);
        } else if (isFloat)
        {
            Val products[2];
            for (uint k = 0; k < 2; k++)
            {
                Val x = m.fpConvert(m.flushHalf(columns[k]), SemanticType::FLOAT_HALF,
                                    SemanticType::FLOAT_SINGLE, dim, true);
                Val y = m.fpConvert(m.flushHalf(weights[k]), SemanticType::FLOAT_HALF,
                                    SemanticType::FLOAT_SINGLE, dim, true);
                products[k] = m.fp(Op::OP_FLOAT_MULTIPLY, 32, x, y, Val(), true);
            }
            Val total = m.fp(Op::OP_FLOAT_ADD, 32, products[0], products[1], Val(), true);
            result = m.fp(Op::OP_FLOAT_ADD, 32, acc, total, Val(), true);
        } else
        {
            result = acc;
            for (uint k = 0; k < ways; k++)
            {
                Val x = m.extendLanes(columns[k], esize, firstSigned);
                Val y = m.extendLanes(weights[k], esize, secondSigned);
                result = m.c().emit(Op::OP_ADD, m.vecType(esize), result,
                                    m.c().emit(Op::OP_MULTIPLY, m.vecType(esize), x, y));
            }
        }
        m.setZaRowAt(row, result);
    }
    m.end();
}

// The long multiply accumulates: smlal... (integer), fmlal / bfmlal (floating point)
void liftLong(SmeLift& m)
{
    ArrayInstruction a;
    decode(m, a);
    const char* name = m.name();
    const uint esize = a.m_za.m_elementBits;
    const uint ssize = a.m_sourceBits;
    const uint ways = esize / ssize;
    const uint dim = m.lanes(esize);
    const bool isFloat = (name[0] == 'f') || (name[0] == 'b');
    const bool isBf = name[0] == 'b';
    // smlsl umlsl smlsll umlsll fmlsl bfmlsl subtract
    const bool isSubtract = strstr(name, "mls") != NULL;
    // the signedness of the first and second source of the integer forms
    const bool firstSigned = (name[0] == 's');
    const bool secondSigned = (name[0] == 's' && name[1] == 'm') || (name[0] == 'u' && name[1] == 's');
    m.begin(true, true);
    for (uint r = 0; r < a.m_nreg; r++)
    {
        Val zn = m.zv(a.m_zn[r], ssize);
        Val zm;
        if (a.m_isIndexed)
            zm = m.indexedVector(m.z(a.m_zm[0]), ssize, a.m_index);
        else
            zm = m.zv(a.m_zm[a.m_isGroup ? r : 0], ssize);
        for (uint j = 0; j < ways; j++)
        {
            Val row = rowOf(m, a, r, j, ways);
            Val acc = m.asVec(m.zaRowAt(row), esize);
            Val x = m.deinterleave(zn, j, ways);
            Val y = m.deinterleave(zm, j, ways);
            Val result;
            if (isFloat)
            {
                if (!isBf)
                {
                    x = m.flushHalf(x);
                    y = m.flushHalf(y);
                }
                const SemanticType::FloatFormat from = isBf ? SemanticType::FLOAT_BFLOAT16
                                                            : SemanticType::FLOAT_HALF;
                Val fx = m.fpConvert(x, from, SemanticType::FLOAT_SINGLE, dim, true);
                Val fy = m.fpConvert(y, from, SemanticType::FLOAT_SINGLE, dim, true);
                if (isSubtract)
                    fx = m.flipSign(fx, 32);
                result = m.fp(Op::OP_FLOAT_MULTIPLY_ADD, 32, fx, fy, acc, true);
            } else
            {
                Val product = m.c().emit(Op::OP_MULTIPLY, m.vecType(esize),
                                         m.extendLanes(x, esize, firstSigned),
                                         m.extendLanes(y, esize, secondSigned));
                result = m.c().emit(isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD,
                                    m.vecType(esize), acc, product);
            }
            m.setZaRowAt(row, result);
        }
    }
    (void)dim;
    m.end();
}

} // namespace

bool armdis::liftSmeArray(SmeLift& m)
{
    if (m.count() < 2 || m.operand(0).m_kind != ArmDecodedOperand::KIND_MATRIX)
        return false;
    const SmeZa za = m.za(0);
    if (!za.m_isArray)
        return false;
    if (m.is("add") || m.is("sub"))
    {
        liftAddSub(m);
        return true;
    }
    if (m.is("fadd") || m.is("fsub") || m.is("bfadd") || m.is("bfsub"))
    {
        liftFloatAdd(m);
        return true;
    }
    if (m.is("fmla") || m.is("fmls") || m.is("bfmla") || m.is("bfmls"))
    {
        liftFloatMla(m);
        return true;
    }
    const uint esize = za.m_elementBits;
    const uint ssize = elementBitsOf(m.operand(1).m_arrangement);
    if (ssize == 0)
        return false;
    const bool isFloatDot = m.is("bfdot") || m.is("fdot");
    if (isFloatDot || m.is("sdot") || m.is("udot") || m.is("usdot") || m.is("sudot"))
    {
        // the 16 bit floating point pairs of a single precision tile, the integer groups of 4
        // (8 bit to 32 bit, 16 bit to 64 bit) and the pairs of 16 bit integers of a 32 bit tile
        if (isFloatDot ? !((ssize == 16) && (esize == 32))
                       : !(((ssize == 8) && (esize == 32)) || ((ssize == 16) && ((esize == 32) || (esize == 64)))))
            return false;
        liftDot(m);
        return true;
    }
    if (m.is("svdot") || m.is("uvdot") || m.is("suvdot") || m.is("usvdot") ||
        m.is("fvdot") || m.is("bfvdot"))
    {
        const bool isFloat = m.is("fvdot") || m.is("bfvdot");
        if (isFloat ? !((ssize == 16) && (esize == 32))
                    : !(((ssize == 8) && (esize == 32)) || ((ssize == 16) && ((esize == 32) || (esize == 64)))))
            return false;
        liftVerticalDot(m);
        return true;
    }
    if (m.is("fmlal") || m.is("fmlsl") || m.is("bfmlal") || m.is("bfmlsl"))
    {
        // (the 8 bit floating point forms are not lifted)
        if (!((ssize == 16) && (esize == 32)))
            return false;
        liftLong(m);
        return true;
    }
    if (m.is("smlal") || m.is("umlal") || m.is("smlsl") || m.is("umlsl") || m.is("smlall") ||
        m.is("umlall") || m.is("smlsll") || m.is("umlsll") || m.is("sumlall") ||
        m.is("usmlall"))
    {
        if (!(((ssize == 16) && (esize == 32)) || ((ssize == 8) && (esize == 32)) ||
              ((ssize == 16) && (esize == 64))))
            return false;
        liftLong(m);
        return true;
    }
    return false;
}
