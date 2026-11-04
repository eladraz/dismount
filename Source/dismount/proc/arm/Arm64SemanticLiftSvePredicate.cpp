/*
 * Arm64SemanticLiftSvePredicate.cpp
 *
 * Implementation file
 *
 * The A64 SVE lifters of the predicates: ptrue, while, the break
 * instructions, the first fault register, the counts of the elements and the
 * vector length (cnt, inc, dec and the saturating forms, rdvl, addvl), the
 * predicate counts, the last element extractions, splice and compact.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
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
#include "dismount/proc/arm/Arm64SemanticSve.h"

using namespace armdis;
using namespace armdis::a64;

typedef SemanticOperation Op;

namespace {

const ArmDecodedOperand& operandOf(SveLift& s, uint index)
{
    return s.context().getOperand(index);
}

uint pregister(SveLift& s, uint index)
{
    return SveLift::registerNumber(operandOf(s, index));
}

// DecodePredCount: the number of elements a pattern selects out of 'elements'
uint patternCount(uint pattern, uint elements)
{
    uint count = 0;
    switch (pattern)
    {
    case 0:
        count = 1;
        while ((count * 2) <= elements)
            count *= 2;
        return (elements == 0) ? 0 : count;
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        count = pattern;
        break;
    case 9: count = 16; break;
    case 10: count = 32; break;
    case 11: count = 64; break;
    case 12: count = 128; break;
    case 13: count = 256; break;
    case 29: return elements - (elements % 4);
    case 30: return elements - (elements % 3);
    case 31: return elements;
    default: return 0;
    }
    return (count <= elements) ? count : 0;
}

// The pattern and the multiplier operands, from operand 'first' on
void readPattern(SveLift& s, uint first, uint& pattern, uint& multiplier)
{
    pattern = 31;
    multiplier = 1;
    for (uint i = first; i < s.context().getOperandsCount(); i++)
    {
        const ArmDecodedOperand& operand = operandOf(s, i);
        if (operand.m_kind == ArmDecodedOperand::KIND_OPTION)
            pattern = operand.m_option;
        else if (operand.m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
            multiplier = (uint)operand.m_immediate;
    }
}

/////////////////////////////////////////////////////////////////////////////
// ptrue, pfalse, pfirst, pnext, ptest
/////////////////////////////////////////////////////////////////////////////

// ptrue, ptrues. m_a: 1 sets the flags
void liftPtrue(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    if (operandOf(s, 0).m_isPredicateCounter)
    {
        // ptrue pn8.t: all the elements are active
        uint size = 0;
        while ((8u << size) < e)
            size++;
        s.writePredicateBits(pregister(s, 0), s.integer(0x8000 | (1 << size), s.predicateBits()));
        return;
    }
    uint pattern, multiplier;
    readPattern(s, 1, pattern, multiplier);
    const uint count = patternCount(pattern, s.lanes(e));
    uint64 value = 0;
    for (uint i = 0; i < count; i++)
        value |= (uint64)1 << (i * (e / 8));
    SemanticOperand result = s.integer((int64)value, s.predicateBits());
    s.writePredicateBits(pregister(s, 0), result);
    if (d.m_a != 0)
        s.setPredicateFlags(result, result, e);
}

void liftPfalse(SveLift& s, const SveDescriptor& d)
{
    s.writePredicateBits(pregister(s, 0), s.integer(0, s.predicateBits()));
}

// The lowest and the highest set bit of the 64 bit value (0 if none)
SemanticOperand lowestBit(SveLift& s, const SemanticOperand& value)
{
    const SemanticType i64 = SemanticType::integer(64);
    return s.emit(Op::OP_AND, i64, value, s.emit(Op::OP_NEGATE, i64, value));
}

SemanticOperand highestBit(SveLift& s, const SemanticOperand& value)
{
    const SemanticType i64 = SemanticType::integer(64);
    return s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64,
                  s.integer((int64)0x8000000000000000ULL, 64),
                  s.emit(Op::OP_COUNT_LEADING_ZEROS, i64, value));
}

SemanticOperand isNonZero(SveLift& s, const SemanticOperand& value)
{
    return s.compareScalar(Op::COND_NOT_EQUAL, value, s.integer(0, 64));
}

// pfirst pdn.b, pg, pdn.b
void liftPfirst(SveLift& s, const SveDescriptor& d)
{
    SemanticOperand g = s.readPredicateBits(pregister(s, 1));
    SemanticOperand n = s.readPredicateBits(pregister(s, 0));
    SemanticOperand first = s.truncate(lowestBit(s, s.zeroExtend(g, 64)), s.predicateBits());
    SemanticOperand result = s.emit(Op::OP_OR, s.predicateType(), n, first);
    s.writePredicateBits(pregister(s, 0), result);
    s.setPredicateFlags(g, result, 8);
}

// pnext pdn.t, pv, pdn.t
void liftPnext(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    SemanticOperand g = s.readPredicateBits(pregister(s, 1));
    SemanticOperand n = s.readPredicateBits(pregister(s, 0));
    SemanticOperand g64 = s.zeroExtend(s.elementPredicate(g, e), 64);
    SemanticOperand o64 = s.zeroExtend(s.elementPredicate(n, e), 64);
    SemanticOperand high = highestBit(s, o64);
    SemanticOperand below = s.emit(Op::OP_SUBTRACT, i64,
        s.emit(Op::OP_SHIFT_LEFT, i64, high, s.integer(1, 64)), s.integer(1, 64));
    below = s.select(isNonZero(s, o64), below, s.integer(0, 64));
    SemanticOperand candidates = s.emit(Op::OP_AND_NOT, i64, below, g64);
    SemanticOperand next = s.truncate(lowestBit(s, candidates), s.predicateBits());
    s.writePredicateBits(pregister(s, 0), next);
    s.setPredicateFlags(g, next, e);
}

// ptest pg, pn.b
void liftPtest(SveLift& s, const SveDescriptor& d)
{
    s.setPredicateFlags(s.readPredicateBits(pregister(s, 0)),
                        s.readPredicateBits(pregister(s, 1)), 8);
}

/////////////////////////////////////////////////////////////////////////////
// The first fault register
/////////////////////////////////////////////////////////////////////////////

// rdffr pd.b | pd.b, pg/z; rdffrs pd.b, pg/z
void liftRdffr(SveLift& s, const SveDescriptor& d)
{
    SemanticOperand ffr = s.readFfr();
    if (s.context().getOperandsCount() == 1)
    {
        s.writePredicateBits(pregister(s, 0), ffr);
        return;
    }
    SemanticOperand g = s.readPredicateBits(pregister(s, 1));
    SemanticOperand result = s.emit(Op::OP_AND, s.predicateType(), ffr, g);
    s.writePredicateBits(pregister(s, 0), result);
    if (d.m_a != 0)
        s.setPredicateFlags(g, result, 8);
}

void liftSetffr(SveLift& s, const SveDescriptor& d)
{
    s.writeFfr(s.integer(-1, s.predicateBits()));
}

void liftWrffr(SveLift& s, const SveDescriptor& d)
{
    s.writeFfr(s.readPredicateBits(pregister(s, 0)));
}

/////////////////////////////////////////////////////////////////////////////
// The breaks
/////////////////////////////////////////////////////////////////////////////

// brka, brkb, brkas, brkbs. m_a: 1 break before (brkb), m_b: 1 sets flags
void liftBreak(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    const ArmDecodedOperand& pg = operandOf(s, 1);
    SemanticOperand g = s.readPredicateBits(pregister(s, 1));
    SemanticOperand n = s.readPredicateBits(pregister(s, 2));
    SemanticOperand g64 = s.zeroExtend(g, 64);
    SemanticOperand t = s.emit(Op::OP_AND, i64, g64, s.zeroExtend(n, 64));
    SemanticOperand lowest = lowestBit(s, t);
    SemanticOperand limit;
    if (d.m_a != 0)
        limit = s.emit(Op::OP_SUBTRACT, i64, lowest, s.integer(1, 64));
    else
        limit = s.emit(Op::OP_SUBTRACT, i64,
                       s.emit(Op::OP_SHIFT_LEFT, i64, lowest, s.integer(1, 64)),
                       s.integer(1, 64));
    SemanticOperand result = s.truncate(s.emit(Op::OP_AND, i64, limit, g64),
                                        s.predicateBits());
    if (pg.m_predicateQualifier == ArmDecodedOperand::PREDICATE_MERGING)
    {
        SemanticOperand old = s.readPredicateBits(pregister(s, 0));
        result = s.emit(Op::OP_OR, s.predicateType(), result,
                        s.emit(Op::OP_AND_NOT, s.predicateType(), g, old));
    }
    s.writePredicateBits(pregister(s, 0), result);
    if (d.m_b != 0)
        s.setPredicateFlags(g, result, 8);
}

// brkn, brkns pdm.b, pg/z, pn.b, pdm.b
void liftBreakNext(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    SemanticOperand g = s.readPredicateBits(pregister(s, 1));
    SemanticOperand n = s.readPredicateBits(pregister(s, 2));
    SemanticOperand m = s.readPredicateBits(pregister(s, 3));
    SemanticOperand hit = isNonZero(s, s.emit(Op::OP_AND, i64,
        highestBit(s, s.zeroExtend(g, 64)), s.zeroExtend(n, 64)));
    SemanticOperand result = s.select(hit, m, s.integer(0, s.predicateBits()));
    s.writePredicateBits(pregister(s, 0), result);
    if (d.m_b != 0)
        s.setPredicateFlags(s.integer(-1, s.predicateBits()), result, 8);
}

// brkpa, brkpb, brkpas, brkpbs pd.b, pg/z, pn.b, pm.b. m_a: 1 brkpb
void liftBreakPropagate(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    SemanticOperand g = s.readPredicateBits(pregister(s, 1));
    SemanticOperand n = s.readPredicateBits(pregister(s, 2));
    SemanticOperand m = s.readPredicateBits(pregister(s, 3));
    SemanticOperand g64 = s.zeroExtend(g, 64);
    SemanticOperand hit = isNonZero(s, s.emit(Op::OP_AND, i64,
        highestBit(s, g64), s.zeroExtend(n, 64)));
    SemanticOperand t = s.emit(Op::OP_AND, i64, g64, s.zeroExtend(m, 64));
    SemanticOperand lowest = lowestBit(s, t);
    SemanticOperand limit;
    if (d.m_a != 0)
        limit = s.emit(Op::OP_SUBTRACT, i64, lowest, s.integer(1, 64));
    else
        limit = s.emit(Op::OP_SUBTRACT, i64,
                       s.emit(Op::OP_SHIFT_LEFT, i64, lowest, s.integer(1, 64)),
                       s.integer(1, 64));
    SemanticOperand result = s.select(hit, s.truncate(s.emit(Op::OP_AND, i64, limit, g64),
                                                      s.predicateBits()),
                                      s.integer(0, s.predicateBits()));
    s.writePredicateBits(pregister(s, 0), result);
    if (d.m_b != 0)
        s.setPredicateFlags(g, result, 8);
}

/////////////////////////////////////////////////////////////////////////////
// while
/////////////////////////////////////////////////////////////////////////////

// whilelt, whilele, whilelo, whilels, whilegt, whilege, whilehi, whilehs of a
// predicate, a pair of predicates and a counter. m_a: 0 lt, 1 le, 2 gt, 3 ge;
// m_b: 1 unsigned.
//
// The elements of the sequence take the values of the first operand, counting
// up (lt, le) or down (gt, ge) with the wrap around of the register width (32
// bits for a w register); an element is active while its value satisfies the
// condition, and every one after the first which doesn't is inactive. A
// descending sequence is laid out from the end: its last element has the
// first value and the active elements are the last ones of the sequence
void liftWhile(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    const SemanticType bitType = SemanticType::integer(1);
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint operandIndex = (operandOf(s, 0).m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ? 1 : 1;
    const ArmDecodedOperand& reg = operandOf(s, operandIndex);
    const bool is64 = (reg.m_register < ARM64_REG_W0) ||
                      (reg.m_register == ARM64_REG_SP) ||
                      (reg.m_register == ARM64_REG_XZR);
    const uint width = is64 ? 64 : 32;
    const SemanticType type = SemanticType::integer(width);
    SemanticOperand a = s.readGprOperand(operandIndex, width);
    SemanticOperand b = s.readGprOperand(operandIndex + 1, width);
    const bool isUnsigned = d.m_b != 0;
    const bool isDown = d.m_a >= 2;
    const bool isInclusive = (d.m_a & 1) != 0;
    Op::Condition condition;
    if (isDown)
        condition = isUnsigned ? (isInclusive ? Op::COND_UNSIGNED_GREATER_EQUAL : Op::COND_UNSIGNED_GREATER)
                               : (isInclusive ? Op::COND_SIGNED_GREATER_EQUAL : Op::COND_SIGNED_GREATER);
    else
        condition = isUnsigned ? (isInclusive ? Op::COND_UNSIGNED_LESS_EQUAL : Op::COND_UNSIGNED_LESS)
                               : (isInclusive ? Op::COND_SIGNED_LESS_EQUAL : Op::COND_SIGNED_LESS);
    SemanticOperand holds = s.compareScalar(condition, a, b);
    // The number of the elements that succeed: the distance to the second
    // operand (and the element on it, if inclusive); everything when the
    // value that ends the sequence does not exist (it wraps)
    SemanticOperand distance = isDown ? s.emit(Op::OP_SUBTRACT, type, a, b)
                                      : s.emit(Op::OP_SUBTRACT, type, b, a);
    SemanticOperand limit = s.emit(Op::OP_ZERO_EXTEND, i64, distance);
    SemanticOperand endless = SemanticOperand();
    if (isInclusive)
    {
        limit = s.emit(Op::OP_ADD, i64, limit, s.integer(1, 64));
        // x2 = the largest (le, ls) or the smallest (ge, hs) value of the width
        uint64 edge;
        if (isDown)
            edge = isUnsigned ? 0 : ((uint64)1 << (width - 1));
        else
            edge = isUnsigned ? (is64 ? ~(uint64)0 : 0xFFFFFFFFULL) : (((uint64)1 << (width - 1)) - 1);
        endless = s.compareScalar(Op::COND_EQUAL, b, s.integer((int64)edge, width));
    }
    uint vectors = 1;
    const ArmDecodedOperand& destination = operandOf(s, 0);
    const bool isCounter = destination.m_isPredicateCounter;
    const bool isPair = destination.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST;
    if (isCounter || isPair)
        vectors = 2;
    for (uint i = 3; i < s.context().getOperandsCount(); i++)
        if (operandOf(s, i).m_kind == ArmDecodedOperand::KIND_OPTION)
            vectors = (operandOf(s, i).m_option == 0) ? 2 : 4;
    const uint lanes = s.lanes(e);
    const uint total = vectors * lanes;
    SemanticOperand totalValue = s.integer(total, 64);
    SemanticOperand active = s.select(s.compareScalar(Op::COND_UNSIGNED_GREATER, limit, totalValue),
                                      totalValue, limit);
    if (endless.isValid())
        active = s.select(endless, totalValue, active);
    active = s.select(holds, active, s.integer(0, 64));

    // The result
    if (isCounter)
    {
        s.writePredicateBits(pregister(s, 0), s.encodeCounter(active, total, e, isDown));
    } else
    {
        SemanticOperand boundary = isDown ? s.emit(Op::OP_SUBTRACT, i64, totalValue, active) : active;
        const SemanticType maskType = SemanticType::mask(lanes);
        for (uint r = 0; r < vectors; r++)
        {
            SemanticOperand mask = s.emit(Op::OP_ACTIVE_LANE_MASK, maskType,
                                          s.integer(r * lanes, 64), boundary);
            if (isDown)
                mask = s.emit(Op::OP_XOR, maskType, mask, s.bitcast(s.integer(-1, lanes), maskType));
            s.writePredicateBits((pregister(s, 0) + r) & 15, s.maskToPredicate(mask, e));
        }
    }
    // The flags test the sequence as it is laid out
    SemanticOperand none = s.compareScalar(Op::COND_EQUAL, active, s.integer(0, 64));
    SemanticOperand any = s.emit(Op::OP_XOR, bitType, none, s.integer(1, 1));
    SemanticOperand full = s.compareScalar(Op::COND_EQUAL, active, totalValue);
    SemanticOperand isFirst = isDown ? full : any;
    SemanticOperand isLast = isDown ? any : full;
    s.context().setFlags(SemanticFlags::FLAG_SIGN, isFirst);
    s.context().setFlags(SemanticFlags::FLAG_ZERO, none);
    s.context().setFlags(SemanticFlags::FLAG_CARRY, s.emit(Op::OP_XOR, bitType, isLast, s.integer(1, 1)));
    s.context().setFlags(SemanticFlags::FLAG_OVERFLOW, booleanConstant(s.context(), false));
}

// ctermeq, ctermne xn, xm
void liftCterm(SveLift& s, const SveDescriptor& d)
{
    const ArmDecodedOperand& reg = operandOf(s, 0);
    const bool is64 = (reg.m_register < ARM64_REG_W0) ||
                      (reg.m_register == ARM64_REG_XZR);
    SemanticOperand a = s.readGprOperand(0, is64 ? 64 : 32);
    SemanticOperand b = s.readGprOperand(1, is64 ? 64 : 32);
    SemanticOperand term = s.compareScalar(d.m_a == 0 ? Op::COND_EQUAL : Op::COND_NOT_EQUAL, a, b);
    SemanticOperand carry = s.context().testFlags(Op::COND_CARRY);
    SemanticOperand notCarry = s.emit(Op::OP_XOR, SemanticType::integer(1), carry, s.integer(1, 1));
    SemanticOperand overflow = s.select(term, s.integer(0, 1), notCarry);
    s.context().setFlags(SemanticFlags::FLAG_SIGN, term);
    s.context().setFlags(SemanticFlags::FLAG_OVERFLOW, overflow);
}

/////////////////////////////////////////////////////////////////////////////
// The counts of the elements and of the vector
/////////////////////////////////////////////////////////////////////////////

// The element size of cnt[bhwd], inc..., from the mnemonic suffix
uint suffixBits(const char* name)
{
    switch (name[strlen(name) - 1])
    {
    case 'b': return 8;
    case 'h': return 16;
    case 'w': return 32;
    default: return 64;
    }
}

enum {
    // cnt, inc, dec, sqinc, sqdec, uqinc, uqdec
    K_CNT, K_INC, K_DEC, K_SQINC, K_SQDEC, K_UQINC, K_UQDEC
};

// Add 'amount' (negative for decrements) to the scalar or vector operand 0
// of an inc / dec / saturating instruction. 'count' is the (signed) number
// of elements
void applyCount(SveLift& s, uint kind, uint elementBits, bool isVector,
                const SemanticOperand& count)
{
    const bool isSaturating = kind >= K_SQINC;
    const bool isSigned = (kind == K_SQINC) || (kind == K_SQDEC);
    const bool isDecrement = (kind == K_DEC) || (kind == K_SQDEC) || (kind == K_UQDEC);
    if (isVector)
    {
        const SemanticType type = s.integerType(elementBits);
        const uint reg = pregister(s, 0);
        SemanticOperand a = s.readZ(reg, elementBits);
        SemanticOperand b = s.splat(s.truncate(count, elementBits), type);
        Op::Operation op;
        if (!isSaturating)
            op = isDecrement ? Op::OP_SUBTRACT : Op::OP_ADD;
        else if (isSigned)
            op = isDecrement ? Op::OP_SUBTRACT_SATURATE_SIGNED : Op::OP_ADD_SATURATE_SIGNED;
        else
            op = isDecrement ? Op::OP_SUBTRACT_SATURATE_UNSIGNED : Op::OP_ADD_SATURATE_UNSIGNED;
        // The count is unsigned, but a signed add of an element of 8 bits
        // would see 128..255 as negative: widen the count's sign by the
        // saturation rules of the Arm ARM (the count fits the element or the
        // value saturates)
        s.writeZ(reg, s.emit(op, type, a, b));
        return;
    }
    // A scalar: x (64 bit), or the x register with a w register source
    // (sqinc xd, wn: the 32 bit saturating form, sign extended; uqinc wd)
    const ArmDecodedOperand& destination = operandOf(s, 0);
    int wOperand = -1;
    for (uint i = 1; i < s.context().getOperandsCount(); i++)
    {
        const ArmDecodedOperand& operand = operandOf(s, i);
        if ((operand.m_kind == ArmDecodedOperand::KIND_REGISTER) &&
            (operand.m_register >= ARM64_REG_W0) && (operand.m_register <= ARM64_REG_WZR))
            wOperand = (int)i;
    }
    const bool has32 = wOperand >= 0;
    const bool is32 = has32 || (destination.m_register >= ARM64_REG_W0 &&
                                destination.m_register <= ARM64_REG_WZR);
    const uint bits = is32 ? 32 : 64;
    SemanticOperand a = s.readGprOperand(has32 ? (uint)wOperand : 0, bits);
    SemanticOperand b = s.truncate(count, bits);
    SemanticOperand result;
    if (!isSaturating)
        result = s.emit(isDecrement ? Op::OP_SUBTRACT : Op::OP_ADD, SemanticType::integer(bits), a, b);
    else
    {
        Op::Operation op;
        if (isSigned)
            op = isDecrement ? Op::OP_SUBTRACT_SATURATE_SIGNED : Op::OP_ADD_SATURATE_SIGNED;
        else
            op = isDecrement ? Op::OP_SUBTRACT_SATURATE_UNSIGNED : Op::OP_ADD_SATURATE_UNSIGNED;
        result = s.emit(op, SemanticType::integer(bits), a, b);
    }
    // sqinc xd, wn sign extends the 32 bit result into the x register
    if (has32 && isSigned)
        result = s.emit(Op::OP_SIGN_EXTEND, SemanticType::integer(64), result);
    s.writeGprOperand(0, result);
}

// cntb, cnth, cntw, cntd; incb...; decb...; sqincb...; uqincb... m_a: K_*
void liftElementCount(SveLift& s, const SveDescriptor& d)
{
    const uint e = suffixBits(d.m_name);
    uint pattern, multiplier;
    readPattern(s, 1, pattern, multiplier);
    const int64 count = (int64)patternCount(pattern, s.lanes(e)) * multiplier;
    if (d.m_a == K_CNT)
    {
        s.writeGprOperand(0, s.integer(count, 64));
        return;
    }
    const bool isVector = SveLift::isZ(operandOf(s, 0));
    applyCount(s, d.m_a, e, isVector, s.integer(count, 64));
}

// cntp xd, pg, pn.t | xd, pn.t, vlx2|vlx4
void liftCntp(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    if (operandOf(s, 2).m_kind == ArmDecodedOperand::KIND_OPTION)
    {
        const uint e = SveLift::elementBitsOf(operandOf(s, 1));
        const uint vectors = (operandOf(s, 2).m_option == 0) ? 2 : 4;
        s.writeGprOperand(0, s.counterActive(pregister(s, 1), e, vectors));
        return;
    }
    const uint e = SveLift::elementBitsOf(operandOf(s, 2));
    SemanticOperand g = s.readPredicateBits(pregister(s, 1));
    SemanticOperand n = s.readPredicateBits(pregister(s, 2));
    SemanticOperand active = s.elementPredicate(s.emit(Op::OP_AND, s.predicateType(), g, n), e);
    s.writeGprOperand(0, s.emit(Op::OP_POPULATION_COUNT, i64, s.zeroExtend(active, 64)));
}

// incp, decp, sqincp, sqdecp, uqincp, uqdecp. m_a: K_*
void liftPredicateCount(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    uint e;
    uint predicateOperand;
    const bool isVector = SveLift::isZ(operandOf(s, 0));
    if (isVector)
    {
        e = SveLift::elementBitsOf(operandOf(s, 0));
        predicateOperand = 1;
    } else
    {
        e = SveLift::elementBitsOf(operandOf(s, 1));
        predicateOperand = 1;
    }
    SemanticOperand n = s.readPredicateBits(pregister(s, predicateOperand));
    SemanticOperand count = s.emit(Op::OP_POPULATION_COUNT, i64,
                                   s.zeroExtend(s.elementPredicate(n, e), 64));
    applyCount(s, d.m_a, e, isVector, count);
}

// rdvl xd, #imm; addvl xd, xn, #imm; addpl xd, xn, #imm; and the streaming
// rdsvl, addsvl, addspl. m_a: 0 rdvl, 1 addvl, 2 addpl; m_b: 1 streaming
void liftVectorLength(SveLift& s, const SveDescriptor& d)
{
    const Arm64Architecture* architecture =
        (const Arm64Architecture*)s.context().getArchitecture().getPointer();
    const uint bits = (d.m_b != 0) ? architecture->getStreamingVectorBits()
                                    : s.vectorBits();
    const uint scale = (d.m_a == 2) ? (bits / 64) : (bits / 8);
    if (d.m_a == 0)
    {
        s.writeGprOperand(0, s.integer(operandOf(s, 1).m_immediate * (int64)scale, 64));
        return;
    }
    SemanticOperand base = s.readGprOperand(1, 64);
    s.writeGprOperand(0, s.emit(Op::OP_ADD, SemanticType::integer(64), base,
                                s.integer(operandOf(s, 2).m_immediate * (int64)scale, 64)));
}

/////////////////////////////////////////////////////////////////////////////
// lasta, lastb, clasta, clastb, splice, compact
/////////////////////////////////////////////////////////////////////////////

// The index of the last active element of the governing predicate
// (element predicate, 64 bit), -1 as 0xFFFF... when none
SemanticOperand lastActiveIndex(SveLift& s, const SemanticOperand& g64, uint e)
{
    const SemanticType i64 = SemanticType::integer(64);
    SemanticOperand leading = s.emit(Op::OP_COUNT_LEADING_ZEROS, i64, g64);
    SemanticOperand bit = s.emit(Op::OP_SUBTRACT, i64, s.integer(63, 64), leading);
    uint shift = 0;
    while ((1u << shift) < (e / 8))
        shift++;
    return s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64, bit, s.integer(shift, 64));
}

// The element 'index' (0 .. lanes - 1) of the vector as a scalar of 'e' bits
SemanticOperand dynamicElement(SveLift& s, const SemanticOperand& vector,
                               const SemanticOperand& index, uint e)
{
    SemanticOperand moved = s.emit(Op::OP_SLIDE, vector.getType(), vector,
                                   s.zero(vector.getType()), index);
    return s.extractLane(moved, 0);
}

// lasta, lastb, clasta, clastb. m_a: 0 lasta, 1 lastb, 2 clasta, 3 clastb
void liftLast(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    const bool isConditional = d.m_a >= 2;
    const bool isAfter = (d.m_a & 1) == 0;
    // lasta rd, pg, zn / clasta zdn, pg, zdn, zm / clasta rdn, pg, rdn, zm
    const uint vectorOperand = isConditional ? 3 : 2;
    const uint e = SveLift::elementBitsOf(operandOf(s, vectorOperand));
    const uint count = s.lanes(e);
    SemanticOperand g = s.zeroExtend(s.elementPredicate(
        s.readPredicateBits(pregister(s, isConditional ? 1 : 1)), e), 64);
    SemanticOperand vector = s.readZOperand(vectorOperand, e);
    SemanticOperand last = lastActiveIndex(s, g, e);
    SemanticOperand any = isNonZero(s, g);
    SemanticOperand index;
    if (isAfter)
    {
        // last + 1, wrapping to 0; no active element: 0 (lasta)
        SemanticOperand next = s.emit(Op::OP_ADD, i64, last, s.integer(1, 64));
        SemanticOperand wraps = s.compareScalar(Op::COND_EQUAL, next, s.integer(count, 64));
        index = s.select(wraps, s.integer(0, 64), next);
        if (!isConditional)
            index = s.select(any, index, s.integer(0, 64));
    } else
    {
        index = last;
        if (!isConditional)
            index = s.select(any, index, s.integer(count - 1, 64));
    }
    SemanticOperand element = dynamicElement(s, vector, index, e);
    const ArmDecodedOperand& destination = operandOf(s, 0);
    if (SveLift::isZ(destination))
    {
        // clasta zdn, pg, zdn, zm: the element in every lane, if any active
        const SemanticType type = s.integerType(e);
        SemanticOperand old = s.readZ(pregister(s, 0), e);
        SemanticOperand enable = s.emit(Op::OP_SELECT, SemanticType::integer(count), any,
                                        s.integer(-1, count), s.integer(0, count));
        s.writeZ(pregister(s, 0), s.select(s.bitcast(enable, SemanticType::mask(count)),
                                           s.splat(element, type), old));
        return;
    }
    // A general purpose register or a SIMD scalar register
    if (destination.m_register >= ARM64_REG_V0)
    {
        SemanticOperand value = element;
        if (isConditional)
            value = s.select(any, element, s.truncate(s.context().getRegister(destination.m_register), e));
        s.context().move(s.context().getRegister(destination.m_register), value);
        return;
    }
    const ArchitectureRegister& reg =
        s.context().getArchitecture()->getRegister(destination.m_register);
    SemanticOperand value = s.zeroExtend(element, reg.m_bits);
    if (isConditional)
        value = s.select(any, value, s.zeroExtend(s.truncate(s.readGprOperand(0, reg.m_bits), e), reg.m_bits));
    s.writeGprOperand(0, value);
}

// firstp, lastp xd, pg, pn.t: the index of the first / last element active in
// pn and pg, -1 if none. m_a: 1 last
void liftFirstLastPredicate(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    const uint e = SveLift::elementBitsOf(operandOf(s, 2));
    SemanticOperand both = s.emit(Op::OP_AND, s.predicateType(), s.readPredicateBits(pregister(s, 1)),
                                  s.readPredicateBits(pregister(s, 2)));
    SemanticOperand g = s.zeroExtend(s.elementPredicate(both, e), 64);
    uint shift = 0;
    while ((1u << shift) < (e / 8))
        shift++;
    SemanticOperand index;
    if (d.m_a != 0)
        index = lastActiveIndex(s, g, e);
    else
        index = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64,
                       s.emit(Op::OP_COUNT_TRAILING_ZEROS, i64, g), s.integer(shift, 64));
    s.writeGprOperand(0, s.select(isNonZero(s, g), index, s.integer(-1, 64)));
}

// splice zdn, pg, zdn, zm | zd, pg, {zn, zn+1}
void liftSplice(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    const uint count = s.lanes(e);
    SemanticOperand g = s.zeroExtend(s.elementPredicate(s.readPredicateBits(pregister(s, 1)), e), 64);
    SemanticOperand first, second;
    if (operandOf(s, 2).m_kind == ArmDecodedOperand::KIND_REGISTER_LIST)
    {
        const ArmDecodedOperand& list = operandOf(s, 2);
        first = s.readZ(SveLift::registerNumber(list), e);
        second = s.readZ((SveLift::registerNumber(list) + list.m_stride) & 31, e);
    } else
    {
        first = s.readZOperand(2, e);
        second = s.readZOperand(3, e);
    }
    uint shift = 0;
    while ((1u << shift) < (e / 8))
        shift++;
    SemanticOperand any = isNonZero(s, g);
    SemanticOperand firstIndex = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64,
        s.emit(Op::OP_COUNT_TRAILING_ZEROS, i64, g), s.integer(shift, 64));
    SemanticOperand lastIndex = lastActiveIndex(s, g, e);
    SemanticOperand length = s.emit(Op::OP_ADD, i64,
        s.emit(Op::OP_SUBTRACT, i64, lastIndex, firstIndex), s.integer(1, 64));
    length = s.select(any, length, s.integer(0, 64));
    firstIndex = s.select(any, firstIndex, s.integer(0, 64));
    // The segment moved to lane 0, then followed by the low lanes of zm
    SemanticOperand moved = s.emit(Op::OP_SLIDE, type, first, s.zero(type), firstIndex);
    SemanticOperand tail = s.emit(Op::OP_SLIDE, type, s.zero(type), second,
        s.emit(Op::OP_SUBTRACT, i64, s.integer(count, 64), length));
    SemanticOperand inSegment = s.emit(Op::OP_ACTIVE_LANE_MASK, SemanticType::mask(count),
                                       s.integer(0, 64), length);
    s.writeZ(pregister(s, 0), s.select(inSegment, moved, tail));
}

// compact zd, pg, zn
void liftCompact(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    s.writeZ(pregister(s, 0),
             s.emit(Op::OP_COMPRESS, s.integerType(e), s.readZOperand(2, e),
                    s.readMaskOperand(1, e)));
}

/////////////////////////////////////////////////////////////////////////////
// The tables
/////////////////////////////////////////////////////////////////////////////

const SveDescriptor gPredicateTable[] =
{
    { "ptrue", liftPtrue, 0, 0 },
    { "ptrues", liftPtrue, 1, 0 },
    { "pfalse", liftPfalse, 0, 0 },
    { "pfirst", liftPfirst, 0, 0 },
    { "pnext", liftPnext, 0, 0 },
    { "ptest", liftPtest, 0, 0 },
    { "rdffr", liftRdffr, 0, 0 },
    { "rdffrs", liftRdffr, 1, 0 },
    { "setffr", liftSetffr, 0, 0 },
    { "wrffr", liftWrffr, 0, 0 },
    { "brka", liftBreak, 0, 0 },
    { "brkas", liftBreak, 0, 1 },
    { "brkb", liftBreak, 1, 0 },
    { "brkbs", liftBreak, 1, 1 },
    { "brkn", liftBreakNext, 0, 0 },
    { "brkns", liftBreakNext, 0, 1 },
    { "brkpa", liftBreakPropagate, 0, 0 },
    { "brkpas", liftBreakPropagate, 0, 1 },
    { "brkpb", liftBreakPropagate, 1, 0 },
    { "brkpbs", liftBreakPropagate, 1, 1 },
    { "whilelt", liftWhile, 0, 0 },
    { "whilele", liftWhile, 1, 0 },
    { "whilegt", liftWhile, 2, 0 },
    { "whilege", liftWhile, 3, 0 },
    { "whilelo", liftWhile, 0, 1 },
    { "whilels", liftWhile, 1, 1 },
    { "whilehi", liftWhile, 2, 1 },
    { "whilehs", liftWhile, 3, 1 },
    { "ctermeq", liftCterm, 0, 0 },
    { "ctermne", liftCterm, 1, 0 },
    { "cntb", liftElementCount, K_CNT, 0 },
    { "cnth", liftElementCount, K_CNT, 0 },
    { "cntw", liftElementCount, K_CNT, 0 },
    { "cntd", liftElementCount, K_CNT, 0 },
    { "incb", liftElementCount, K_INC, 0 },
    { "inch", liftElementCount, K_INC, 0 },
    { "incw", liftElementCount, K_INC, 0 },
    { "incd", liftElementCount, K_INC, 0 },
    { "decb", liftElementCount, K_DEC, 0 },
    { "dech", liftElementCount, K_DEC, 0 },
    { "decw", liftElementCount, K_DEC, 0 },
    { "decd", liftElementCount, K_DEC, 0 },
    { "sqincb", liftElementCount, K_SQINC, 0 },
    { "sqinch", liftElementCount, K_SQINC, 0 },
    { "sqincw", liftElementCount, K_SQINC, 0 },
    { "sqincd", liftElementCount, K_SQINC, 0 },
    { "sqdecb", liftElementCount, K_SQDEC, 0 },
    { "sqdech", liftElementCount, K_SQDEC, 0 },
    { "sqdecw", liftElementCount, K_SQDEC, 0 },
    { "sqdecd", liftElementCount, K_SQDEC, 0 },
    { "uqincb", liftElementCount, K_UQINC, 0 },
    { "uqinch", liftElementCount, K_UQINC, 0 },
    { "uqincw", liftElementCount, K_UQINC, 0 },
    { "uqincd", liftElementCount, K_UQINC, 0 },
    { "uqdecb", liftElementCount, K_UQDEC, 0 },
    { "uqdech", liftElementCount, K_UQDEC, 0 },
    { "uqdecw", liftElementCount, K_UQDEC, 0 },
    { "uqdecd", liftElementCount, K_UQDEC, 0 },
    { "cntp", liftCntp, 0, 0 },
    { "incp", liftPredicateCount, K_INC, 0 },
    { "decp", liftPredicateCount, K_DEC, 0 },
    { "sqincp", liftPredicateCount, K_SQINC, 0 },
    { "sqdecp", liftPredicateCount, K_SQDEC, 0 },
    { "uqincp", liftPredicateCount, K_UQINC, 0 },
    { "uqdecp", liftPredicateCount, K_UQDEC, 0 },
    { "rdvl", liftVectorLength, 0, 0 },
    { "addvl", liftVectorLength, 1, 0 },
    { "addpl", liftVectorLength, 2, 0 },
    { "rdsvl", liftVectorLength, 0, 1 },
    { "addsvl", liftVectorLength, 1, 1 },
    { "addspl", liftVectorLength, 2, 1 },
    { "lasta", liftLast, 0, 0 },
    { "lastb", liftLast, 1, 0 },
    { "clasta", liftLast, 2, 0 },
    { "clastb", liftLast, 3, 0 },
    { "splice", liftSplice, 0, 0 },
    { "compact", liftCompact, 0, 0 },
    { "firstp", liftFirstLastPredicate, 0, 0 },
    { "lastp", liftFirstLastPredicate, 1, 0 },
    { NULL, NULL, 0, 0 }
};

} // namespace

void armdis::a64::liftSvePredicate(ArmLiftContext& context)
{
    runSveTable(context, gPredicateTable);
}
