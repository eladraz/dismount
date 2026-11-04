/*
 * Arm64SemanticLiftSveMisc.cpp
 *
 * Implementation file
 *
 * The A64 SVE lifters of the predicate selects and extracts (psel, pext with
 * the predicate as counter, pmov), the pointer conflict masks (whilewr,
 * whilerw), expand, revd and the checked pointer arithmetic (addpt, subpt,
 * madpt, mlapt).
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

// A mask of 'count' lanes made of the 1 bit condition: all or none of them
SemanticOperand gateMask(SveLift& s, const SemanticOperand& condition, uint count)
{
    return s.bitcast(s.emit(Op::OP_SELECT, SemanticType::integer(count), condition,
                            s.integer(-1, count), s.integer(0, count)),
                     SemanticType::mask(count));
}

/////////////////////////////////////////////////////////////////////////////
// psel, pext, pmov
/////////////////////////////////////////////////////////////////////////////

// psel pd, pn, pm.t[wv, imm]: pn if the element (wv + imm) of pm is set, else
// zeros. The index is in the encoding: tszh:tszl:i bits 23-22, 20-19, 18
void liftPsel(SveLift& s, const SveDescriptor& d)
{
    const uint32 encoding = s.context().getInstruction().getEncoding();
    const uint e = SveLift::elementBitsOf(operandOf(s, 2));
    const uint field = ((encoding >> 22) & 3) << 3 | ((encoding >> 19) & 3) << 1 | ((encoding >> 18) & 1);
    uint shift = 1;
    for (uint size = e; size > 8; size /= 2)
        shift++;
    const uint immediate = field >> shift;
    const uint elements = s.lanes(e);
    SemanticOperand w = s.context().getRegister(
        getArm64GeneralRegister(12 + ((encoding >> 16) & 3), false, false));
    const SemanticType i64 = SemanticType::integer(64);
    SemanticOperand index = s.emit(Op::OP_AND, i64,
        s.emit(Op::OP_ADD, i64, s.zeroExtend(w, 64), s.integer(immediate, 64)),
        s.integer(elements - 1, 64));
    SemanticOperand bitPosition = s.emit(Op::OP_MULTIPLY, i64, index, s.integer(e / 8, 64));
    SemanticOperand pm = s.zeroExtend(s.readPredicateBits(pregister(s, 2)), 64);
    SemanticOperand bit = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                                 s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64, pm, bitPosition));
    SemanticOperand pn = s.readPredicateBits(pregister(s, 1));
    s.writePredicateBits(pregister(s, 0),
                         s.select(bit, pn, s.integer(0, s.predicateBits())));
}

// pext pd.t, pn[i] | {pd.t, pd+1.t}, pn[i]: a predicate of a predicate as
// counter. The counter: bit 15 inverts, bits 3-0 hold the element size (the
// lowest set bit: b, h, w, d; none: the result is zero) and the bits above it
// down to bit 14 hold the count of the elements of that size (modulo 4
// vectors). The count covers count * size bytes from the start of the 4
// vectors; the elements of the result have the larger of the size of the
// counter and of the instruction, and element e of the vector 'index * pairs
// + k' is active if its first byte is below the count, inverted by bit 15
void liftPext(SveLift& s, const SveDescriptor& d)
{
    const ArmDecodedOperand& destination = operandOf(s, 0);
    const uint pairs = (destination.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ?
                       destination.m_count : 1;
    const uint index = (uint)operandOf(s, 1).m_elementIndex;
    const uint instructionBits = SveLift::elementBitsOf(destination);
    const SemanticType i64 = SemanticType::integer(64);
    SemanticOperand counter = s.zeroExtend(s.readPredicateBits(pregister(s, 1)), 64);
    SemanticOperand low = s.emit(Op::OP_AND, i64, counter, s.integer(0xF, 64));
    SemanticOperand lowest = s.emit(Op::OP_COUNT_TRAILING_ZEROS, i64, low);
    SemanticOperand isValid = s.compareScalar(Op::COND_NOT_EQUAL, low, s.integer(0, 64));
    SemanticOperand invert = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
        s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64, counter, s.integer(15, 64)));
    const uint vectorBytes = s.vectorBits() / 8;
    for (uint k = 0; k < pairs; k++)
    {
        SemanticOperand result = s.integer(0, s.predicateBits());
        for (uint size = 0; size < 4; size++)
        {
            const uint counterBits = 8u << size;
            const uint e = (counterBits > instructionBits) ? counterBits : instructionBits;
            const uint elements = s.lanes(e);
            uint shift = 0;
            for (uint bits = e; bits > 8; bits /= 2)
                shift++;
            // The count in elements of the counter, modulo 4 vectors, in bytes
            const uint counterElements = s.lanes(counterBits);
            SemanticOperand count = s.emit(Op::OP_AND, i64,
                s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64, counter, s.integer(size + 1, 64)),
                s.integer(4 * counterElements - 1, 64));
            SemanticOperand bytes = s.emit(Op::OP_SHIFT_LEFT, i64, count, s.integer(size, 64));
            const uint base = (index * pairs + k) * vectorBytes;
            SemanticOperand isAbove = s.compareScalar(Op::COND_UNSIGNED_GREATER, bytes, s.integer(base, 64));
            SemanticOperand remaining = s.select(isAbove, s.emit(Op::OP_SUBTRACT, i64, bytes, s.integer(base, 64)),
                                                 s.integer(0, 64));
            SemanticOperand limit = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64,
                s.emit(Op::OP_ADD, i64, remaining, s.integer((1u << shift) - 1, 64)), s.integer(shift, 64));
            SemanticOperand mask = s.emit(Op::OP_ACTIVE_LANE_MASK, SemanticType::mask(elements),
                                          s.integer(0, 64), limit);
            mask = s.emit(Op::OP_XOR, SemanticType::mask(elements), mask, gateMask(s, invert, elements));
            SemanticOperand chosen = s.emit(Op::OP_AND, SemanticType::integer(1), isValid,
                s.compareScalar(Op::COND_EQUAL, lowest, s.integer(size, 64)));
            result = s.select(chosen, s.maskToPredicate(mask, e), result);
        }
        s.writePredicateBits(SveLift::registerNumber(destination) + k, result);
    }
}

// pmov pd.t, zn[i] and pmov zd[i], pn.t
void liftPmov(SveLift& s, const SveDescriptor& d)
{
    const bool toPredicate = SveLift::isP(operandOf(s, 0));
    const ArmDecodedOperand& predicate = operandOf(s, toPredicate ? 0 : 1);
    const ArmDecodedOperand& vector = operandOf(s, toPredicate ? 1 : 0);
    const uint e = SveLift::elementBitsOf(predicate);
    const uint elements = s.lanes(e);
    const uint index = (vector.m_elementIndex >= 0) ? (uint)vector.m_elementIndex : 0;
    const uint z = SveLift::registerNumber(vector);
    if (toPredicate)
    {
        SemanticOperand bits = s.emit(Op::OP_EXTRACT, SemanticType::integer(elements),
                                      s.readZWhole(z), s.integer((int64)index * elements, 32));
        s.writePredicateBits(SveLift::registerNumber(predicate),
                             s.maskToPredicate(s.bitcast(bits, SemanticType::mask(elements)), e));
        return;
    }
    SemanticOperand bits = s.bitcast(s.readLaneMask(SveLift::registerNumber(predicate), e),
                                     SemanticType::integer(elements));
    if (vector.m_elementIndex < 0)
    {
        s.writeZ(z, s.zeroExtend(bits, s.vectorBits()));
        return;
    }
    s.writeZ(z, s.emit(Op::OP_INSERT, s.wholeType(), s.readZWhole(z), bits,
                       s.integer((int64)index * elements, 32)));
}

/////////////////////////////////////////////////////////////////////////////
// whilewr, whilerw
/////////////////////////////////////////////////////////////////////////////

// m_a: 0 whilewr, 1 whilerw
void liftWhileConflict(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i64 = SemanticType::integer(64);
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint elements = s.lanes(e);
    SemanticOperand n = s.readGprOperand(1, 64);
    SemanticOperand m = s.readGprOperand(2, 64);
    SemanticOperand diff = s.emit(Op::OP_SUBTRACT, i64, m, n);
    uint shift = 0;
    for (uint size = e; size > 8; size /= 2)
        shift++;
    SemanticOperand maximum = s.integer(-1, 64);
    SemanticOperand limit;
    if (d.m_a == 0)
    {
        // the addresses are unsigned: m <= n: every element; else the elements below (m - n) / bytes
        SemanticOperand isPositive = s.compareScalar(Op::COND_UNSIGNED_GREATER, m, n);
        limit = s.select(isPositive, s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64, diff, s.integer(shift, 64)),
                         maximum);
    } else
    {
        SemanticOperand isNegative = s.compareScalar(Op::COND_UNSIGNED_LESS, m, n);
        SemanticOperand absolute = s.select(isNegative, s.emit(Op::OP_NEGATE, i64, diff), diff);
        limit = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64, absolute, s.integer(shift, 64));
        limit = s.select(s.compareScalar(Op::COND_EQUAL, absolute, s.integer(0, 64)), maximum, limit);
    }
    // An element count of zero (a difference smaller than an element): every element
    limit = s.select(s.compareScalar(Op::COND_EQUAL, limit, s.integer(0, 64)), maximum, limit);
    SemanticOperand mask = s.emit(Op::OP_ACTIVE_LANE_MASK, SemanticType::mask(elements),
                                  s.integer(0, 64), limit);
    SemanticOperand result = s.maskToPredicate(mask, e);
    s.writePredicateBits(pregister(s, 0), result);
    s.setPredicateFlags(s.integer((int64)s.elementBitsMask(e), s.predicateBits()), result, e);
}

/////////////////////////////////////////////////////////////////////////////
// expand, revd
/////////////////////////////////////////////////////////////////////////////

// expand zd.t, pg, zn.t
void liftExpand(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    s.writeZ(pregister(s, 0), s.emit(Op::OP_EXPAND, s.integerType(e), s.readZOperand(2, e),
                                     s.readMaskOperand(1, e)));
}

// revd zd.q, pg/m|z, zn.q: the doublewords of every quadword swap
void liftRevd(SveLift& s, const SveDescriptor& d)
{
    const SemanticType wide = s.integerType(64);
    SemanticOperand n = s.readZOperand(2, 64);
    SemanticOperand index = s.emit(Op::OP_XOR, wide, s.laneIndex(0, 1, 64), s.splatInteger(1, 64));
    SemanticOperand swapped = s.bitcast(s.emit(Op::OP_SHUFFLE, wide, n, n, index), s.integerType(128));
    SemanticOperand old = s.readZ(pregister(s, 0), 128);
    SemanticOperand mask = s.readMaskOperand(1, 128);
    const bool isZeroing = operandOf(s, 1).m_predicateQualifier == ArmDecodedOperand::PREDICATE_ZEROING;
    s.writeZ(pregister(s, 0), s.predicate(mask, swapped, old, isZeroing));
}

/////////////////////////////////////////////////////////////////////////////
// The checked pointer arithmetic: plain 64 bit arithmetic
/////////////////////////////////////////////////////////////////////////////

// addpt, subpt (m_a: the operation): predicated [zdn, pg/m, zdn, zm] or [zd, zn, zm]
void liftPointerAdd(SveLift& s, const SveDescriptor& d)
{
    const SemanticType type = s.integerType(64);
    const bool isPredicated = SveLift::isP(operandOf(s, 1));
    const uint first = isPredicated ? 2 : 1;
    SemanticOperand result = s.emit((Op::Operation)d.m_a, type, s.readZOperand(first, 64),
                                    s.readZOperand(first + 1, 64));
    const uint reg = pregister(s, 0);
    if (isPredicated)
        result = s.select(s.readMaskOperand(1, 64), result, s.readZ(reg, 64));
    s.writeZ(reg, result);
}

// madpt zdn, zm, za: za + zdn * zm; mlapt zda, zn, zm: zda + zn * zm
void liftPointerMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    const SemanticType type = s.integerType(64);
    const uint reg = pregister(s, 0);
    SemanticOperand result;
    if (d.m_a == 0)
        result = s.emit(Op::OP_ADD, type, s.readZOperand(2, 64),
                        s.emit(Op::OP_MULTIPLY, type, s.readZ(reg, 64), s.readZOperand(1, 64)));
    else
        result = s.emit(Op::OP_ADD, type, s.readZ(reg, 64),
                        s.emit(Op::OP_MULTIPLY, type, s.readZOperand(1, 64), s.readZOperand(2, 64)));
    s.writeZ(reg, result);
}

const SveDescriptor gTable[] =
{
    { "psel", liftPsel, 0, 0 },
    { "pext", liftPext, 0, 0 },
    { "pmov", liftPmov, 0, 0 },
    { "whilewr", liftWhileConflict, 0, 0 },
    { "whilerw", liftWhileConflict, 1, 0 },
    { "expand", liftExpand, 0, 0 },
    { "revd", liftRevd, 0, 0 },
    { "addpt", liftPointerAdd, Op::OP_ADD, 0 },
    { "subpt", liftPointerAdd, Op::OP_SUBTRACT, 0 },
    { "madpt", liftPointerMultiplyAdd, 0, 0 },
    { "mlapt", liftPointerMultiplyAdd, 1, 0 },
    { NULL, NULL, 0, 0 }
};

} // namespace

void armdis::a64::liftSveMisc(ArmLiftContext& context)
{
    runSveTable(context, gTable);
}
