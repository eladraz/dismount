/*
 * Arm64SemanticLiftSmeVector.cpp
 *
 * Implementation file
 *
 * The multi vector instructions of the z registers of SME2 (see Arm64SemanticSme.h): the
 * destructive add / min / max / fmax / fmin... / srshl / urshl / sqdmulh with a vector group,
 * the clamps.
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

// The number of registers and the first register number of a register or a group
uint groupCount(SmeLift& m, uint index)
{
    const ArmDecodedOperand& o = m.operand(index);
    return (o.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ? o.m_count : 1;
}

uint groupRegister(SmeLift& m, uint index, uint r)
{
    const ArmDecodedOperand& o = m.operand(index);
    const uint first = o.m_register - ARM64_REG_Z0;
    return (o.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ?
        ((first + r * o.m_stride) & 31) : first;
}

// The elementwise operation of a group instruction: dst = op(first, second)
typedef Val (*ElementOperation)(SmeLift& m, uint elementBits, const Val& a, const Val& b);

// The group instruction: the destination group, the first source (the same registers), the second
// source (a group of the same size, or a single register). All the sources are read before the
// destination is written
void liftGroupOperation(SmeLift& m, ElementOperation operation)
{
    const uint nreg = groupCount(m, 0);
    const uint esize = elementBitsOf(m.operand(0).m_arrangement);
    const bool isSingle = m.operand(2).m_kind != ArmDecodedOperand::KIND_REGISTER_LIST;
    m.begin(true, false);
    Val results[4];
    for (uint r = 0; r < nreg; r++)
    {
        Val a = m.zv(groupRegister(m, 1, r), esize);
        Val b = m.zv(groupRegister(m, 2, isSingle ? 0 : r), esize);
        results[r] = operation(m, esize, a, b);
    }
    for (uint r = 0; r < nreg; r++)
        m.setZ(groupRegister(m, 0, r), results[r]);
    m.end();
}

Val opAdd(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.c().emit(Op::OP_ADD, m.vecType(e), a, b);
}

Val opSmax(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.c().emit(Op::OP_MAXIMUM_SIGNED, m.vecType(e), a, b);
}

Val opUmax(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.c().emit(Op::OP_MAXIMUM_UNSIGNED, m.vecType(e), a, b);
}

Val opSmin(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.c().emit(Op::OP_MINIMUM_SIGNED, m.vecType(e), a, b);
}

Val opUmin(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.c().emit(Op::OP_MINIMUM_UNSIGNED, m.vecType(e), a, b);
}

Val opFmul(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.fp(Op::OP_FLOAT_MULTIPLY, e, a, b, Val(), false, Op::BEHAVIOR_DEFAULT,
                m.name()[0] == 'b');
}

// famax / famin: the maximum / minimum of the absolute values (a NaN follows fmax / fmin)
Val opFamax(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.fp(Op::OP_FLOAT_MAXIMUM, e, m.fp(Op::OP_FLOAT_ABSOLUTE, e, a, Val(), Val(), false),
                m.fp(Op::OP_FLOAT_ABSOLUTE, e, b, Val(), Val(), false), Val(), false,
                Op::BEHAVIOR_MINMAX_PROPAGATE_NAN);
}

Val opFamin(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.fp(Op::OP_FLOAT_MINIMUM, e, m.fp(Op::OP_FLOAT_ABSOLUTE, e, a, Val(), Val(), false),
                m.fp(Op::OP_FLOAT_ABSOLUTE, e, b, Val(), Val(), false), Val(), false,
                Op::BEHAVIOR_MINMAX_PROPAGATE_NAN);
}

// fscale / bfscale: a * 2^b, b the signed integer lane
Val opFscale(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.fp(Op::OP_FLOAT_SCALE, e, a, b, Val(), false, Op::BEHAVIOR_SCALE_INTEGER,
                m.name()[0] == 'b');
}

Val opFmax(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.fp(Op::OP_FLOAT_MAXIMUM, e, a, b, Val(), false, Op::BEHAVIOR_MINMAX_PROPAGATE_NAN,
                m.name()[0] == 'b');
}

Val opFmin(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.fp(Op::OP_FLOAT_MINIMUM, e, a, b, Val(), false, Op::BEHAVIOR_MINMAX_PROPAGATE_NAN,
                m.name()[0] == 'b');
}

Val opFmaxnm(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.fp(Op::OP_FLOAT_MAXIMUM, e, a, b, Val(), false, Op::BEHAVIOR_MINMAX_NUMBER,
                m.name()[0] == 'b');
}

Val opFminnm(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return m.fp(Op::OP_FLOAT_MINIMUM, e, a, b, Val(), false, Op::BEHAVIOR_MINMAX_NUMBER,
                m.name()[0] == 'b');
}

// srshl / urshl: the signed element of b is the shift, a negative one is a rounding right shift
Val roundingShift(SmeLift& m, uint e, const Val& a, const Val& b, bool isSigned)
{
    const SemanticType type = m.vecType(e);
    ArmLiftContext& c = m.c();
    Val left = c.emit(Op::OP_SHIFT_LEFT, type, a, b);
    Val negated = c.emit(Op::OP_NEGATE, type, b);
    SemanticInstruction shift(Op::OP_SHIFT_RIGHT_ROUNDING, type, c.newTemporary(type), a, negated);
    shift.setBehavior(isSigned ? Op::BEHAVIOR_SIGNED : Op::BEHAVIOR_UNSIGNED);
    Val right = shift.getDestination();
    c.emit(shift);
    Val isRight = m.s().compare(Op::COND_SIGNED_LESS, type, b, m.zeroVec(e));
    return m.s().select(isRight, right, left);
}

Val opSrshl(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return roundingShift(m, e, a, b, true);
}

Val opUrshl(SmeLift& m, uint e, const Val& a, const Val& b)
{
    return roundingShift(m, e, a, b, false);
}

// sqdmulh: the high half of the doubled product, saturated
Val opSqdmulh(SmeLift& m, uint e, const Val& a, const Val& b)
{
    ArmLiftContext& c = m.c();
    const SemanticType type = m.vecType(e);
    Val high = c.emit(Op::OP_MULTIPLY_HIGH_SIGNED, type, a, b);
    Val low = c.emit(Op::OP_MULTIPLY, type, a, b);
    Val doubledHigh = c.emit(Op::OP_SHIFT_LEFT, type, high, m.s().splat(type, 1));
    Val lowBit = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, low, m.s().splat(type, e - 1));
    Val result = c.emit(Op::OP_OR, type, doubledHigh, lowBit);
    // the minimum against the minimum is the only overflow
    const int64 minimum = (e == 64) ? (int64)0x8000000000000000ULL : ((int64)1 << (e - 1));
    Val minimumVector = m.s().splat(type, m.imm(minimum, e));
    Val bothMinimum = m.andMask(m.s().compare(Op::COND_EQUAL, type, a, minimumVector),
                                m.s().compare(Op::COND_EQUAL, type, b, minimumVector));
    Val maximum = m.s().splat(type, m.imm(~minimum, e));
    return m.s().select(bothMinimum, maximum, result);
}

// sclamp / uclamp / fclamp: dst = min(max(dst, lo), hi) (the floating point one with the number
// preferring minimum and maximum, the lower bound first)
void liftClamp(SmeLift& m)
{
    const uint nreg = m.groupCount(0);
    const uint esize = m.elementBits(0);
    const bool isFloat = m.is("fclamp") || m.is("bfclamp");
    const bool isBf = m.is("bfclamp");
    const bool isSigned = m.is("sclamp");
    m.begin(true, false);
    Val lo = m.zv(m.zNumber(1), esize);
    Val hi = m.zv(m.zNumber(2), esize);
    Val results[4];
    for (uint r = 0; r < nreg; r++)
    {
        Val x = m.zv(m.groupRegister(0, r), esize);
        if (isFloat)
        {
            Val raised = m.fp(Op::OP_FLOAT_MAXIMUM, esize, lo, x, Val(), false, Op::BEHAVIOR_MINMAX_NUMBER, isBf);
            results[r] = m.fp(Op::OP_FLOAT_MINIMUM, esize, raised, hi, Val(), false, Op::BEHAVIOR_MINMAX_NUMBER, isBf);
        } else
        {
            Val raised = m.c().emit(isSigned ? Op::OP_MAXIMUM_SIGNED : Op::OP_MAXIMUM_UNSIGNED,
                                    m.vecType(esize), x, lo);
            results[r] = m.c().emit(isSigned ? Op::OP_MINIMUM_SIGNED : Op::OP_MINIMUM_UNSIGNED,
                                    m.vecType(esize), raised, hi);
        }
    }
    for (uint r = 0; r < nreg; r++)
        m.setZ(m.groupRegister(0, r), results[r]);
    m.end();
}

// The byte granular predicate (SVL / 8 bits) of the vector r of a group of 'nvec' vectors for the
// predicate as counter (the low 16 bits of the predicate register): the bit 15 inverts, the lowest
// set bit k of the bits 3..0 gives the element size 1 << k bytes, the count is the bits 8..k+1 (as
// the M4 implements it); the elements 0..count - 1 of the group (the first of each vector counts the
// elements of the earlier vectors) are active
Val counterBytes(SmeLift& m, const Val& counter16, uint r)
{
    ArmLiftContext& c = m.c();
    const uint pl = m.svl() / 8;
    const SemanticType t64 = SemanticType::integer(64);
    const SemanticType tp = SemanticType::integer(pl);
    Val v = c.emit(Op::OP_ZERO_EXTEND, t64, counter16);
    Val invert = c.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                        c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t64, v, m.imm(15, 64)));
    Val candidates[4];
    for (uint k = 0; k < 4; k++)
    {
        const uint ec = 1u << k;
        const uint perVector = pl / ec;
        Val count = c.emit(Op::OP_AND, t64,
                           c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t64, v, m.imm(k + 1, 64)),
                           m.imm(0xFF >> k, 64));
        // t = clamp(count - r * perVector, 0, perVector)
        Val remaining = c.emit(Op::OP_SUBTRACT, t64, count, m.imm((int64)r * perVector, 64));
        Val isNegative = m.s().compare(Op::COND_SIGNED_LESS, t64, remaining, m.imm(0, 64));
        Val isFull = m.s().compare(Op::COND_SIGNED_GREATER, t64, remaining, m.imm(perVector, 64));
        Val t = c.emit(Op::OP_SELECT, t64, isNegative, m.imm(0, 64),
                       c.emit(Op::OP_SELECT, t64, isFull, m.imm(perVector, 64), remaining));
        Val bitsCount = c.emit(Op::OP_SHIFT_LEFT, t64, t, m.imm(k, 64));
        Val one = m.imm(1, pl);
        Val low = c.emit(Op::OP_SUBTRACT, tp,
                         c.emit(Op::OP_SHIFT_LEFT, tp, one,
                                c.emit(Op::OP_TRUNCATE, tp, bitsCount)), one);
        uint64 pattern = 0;
        for (uint t2 = 0; t2 < perVector; t2++)
            pattern |= (uint64)1 << (t2 * ec);
        Val selected = c.emit(Op::OP_SELECT, tp, invert, c.emit(Op::OP_NOT, tp, low), low);
        candidates[k] = c.emit(Op::OP_AND, tp, selected, m.imm((int64)pattern, pl));
    }
    Val result = m.imm(0, pl);
    for (int k = 3; k >= 0; k--)
    {
        Val isSet = m.s().compare(Op::COND_NOT_EQUAL, SemanticType::integer(64),
                                  c.emit(Op::OP_AND, t64, v, m.imm(1 << k, 64)), m.imm(0, 64));
        result = c.emit(Op::OP_SELECT, tp, isSet, candidates[k], result);
    }
    return result;
}

// The lanes of 'elementBits' of a byte granular predicate
Val bytesToLaneMask(SmeLift& m, const Val& bytes, uint elementBits)
{
    const uint n = m.lanes(elementBits);
    const uint pl = m.svl() / 8;
    if (elementBits == 8)
        return m.s().cast(SimdLift::M(n), bytes);
    uint64 pattern = 0;
    for (uint i = 0; i < n; i++)
        pattern |= (uint64)1 << (i * (elementBits / 8));
    Val gathered = m.c().emit(Op::OP_BIT_GATHER, SemanticType::integer(pl), bytes,
                              m.imm((int64)pattern, pl));
    if (pl != n)
        gathered = m.c().emit(Op::OP_TRUNCATE, SemanticType::integer(n), gathered);
    return m.s().cast(SimdLift::M(n), gathered);
}

// sel {dst}, pn, {first}, {second}
void liftSel(SmeLift& m)
{
    const uint nreg = m.groupCount(0);
    const uint esize = m.elementBits(0);
    m.begin(true, false);
    Val counter = m.c().emit(Op::OP_TRUNCATE, SemanticType::integer(16),
                             m.c().getRegister(ARM64_REG_P0 + m.pNumber(1)));
    Val results[4];
    for (uint r = 0; r < nreg; r++)
    {
        Val mask = bytesToLaneMask(m, counterBytes(m, counter, r), esize);
        results[r] = m.s().select(mask, m.zv(m.groupRegister(2, r), esize),
                                  m.zv(m.groupRegister(3, r), esize));
    }
    for (uint r = 0; r < nreg; r++)
        m.setZ(m.groupRegister(0, r), results[r]);
    m.end();
}

} // namespace

bool armdis::liftSmeVector(SmeLift& m)
{
    if (m.is("sel") && (m.count() == 4))
    {
        liftSel(m);
        return true;
    }
    if (m.is("sclamp") || m.is("uclamp") || m.is("fclamp") || m.is("bfclamp"))
    {
        liftClamp(m);
        return true;
    }
    if (m.count() < 3)
        return false;
    struct Entry {
        const char* m_name;
        ElementOperation m_operation;
    };
    static const Entry groupOperations[] = {
        { "add", opAdd }, { "smax", opSmax }, { "umax", opUmax }, { "smin", opSmin },
        { "umin", opUmin }, { "fmax", opFmax }, { "fmin", opFmin }, { "fmaxnm", opFmaxnm },
        { "fminnm", opFminnm }, { "famax", opFamax }, { "famin", opFamin },
        { "fscale", opFscale }, { "bfscale", opFscale }, { "fmul", opFmul }, { "bfmul", opFmul }, { "bfmax", opFmax },
        { "bfmin", opFmin }, { "bfmaxnm", opFmaxnm }, { "bfminnm", opFminnm }, { "srshl", opSrshl }, { "urshl", opUrshl },
        { "sqdmulh", opSqdmulh }
    };
    const ArmDecodedOperand& o0 = m.operand(0);
    if ((o0.m_kind != ArmDecodedOperand::KIND_REGISTER_LIST) ||
        (m.operand(1).m_kind != ArmDecodedOperand::KIND_REGISTER_LIST) ||
        (elementBitsOf(o0.m_arrangement) == 0))
        return false;
    for (uint i = 0; i < sizeof(groupOperations) / sizeof(groupOperations[0]); i++)
    {
        if (m.is(groupOperations[i].m_name) && (m.count() == 3))
        {
            liftGroupOperation(m, groupOperations[i].m_operation);
            return true;
        }
    }
    return false;
}
