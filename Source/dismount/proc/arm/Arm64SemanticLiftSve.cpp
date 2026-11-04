/*
 * Arm64SemanticLiftSve.cpp
 *
 * Implementation file
 *
 * The A64 SVE lifters of the integer instructions: arithmetic, logic,
 * shifts, multiply add, saturating arithmetic, compares, copies, index, the
 * reductions and the permutes. The semantics are the ones of the pseudocode
 * of the Arm ARM; the predication is the merging / zeroing of the active
 * lanes (see Arm64SemanticSve.h).
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

// The flags of the descriptors
enum {
    // The operands of the operation are swapped (subr, sdivr, lslr...)
    F_REVERSE = 1,
    // A division: by zero gives zero
    F_DIVIDE = 2,
    // The operation is a shift by a vector
    F_SHIFT = 4,
    // The saturating operations of the SVE2 predicated forms
    F_SATURATE = 8
};

const ArmDecodedOperand& operandOf(SveLift& s, uint index)
{
    return s.context().getOperand(index);
}

bool isKind(SveLift& s, uint index, uint kind)
{
    return (index < s.context().getOperandsCount()) &&
           (operandOf(s, index).m_kind == kind);
}

bool isPredicateAt(SveLift& s, uint index)
{
    return (index < s.context().getOperandsCount()) &&
           SveLift::isP(operandOf(s, index));
}

// Write the result of an instruction whose destination is operand 0 and,
// when 'predicateIndex' is not -1, the predicate operand at that index:
// the inactive lanes keep the old value (merging) or become zero (zeroing)
void writeResult(SveLift& s, int predicateIndex, uint elementBits,
                 const SemanticOperand& computed)
{
    const ArmDecodedOperand& dst = operandOf(s, 0);
    const uint reg = SveLift::registerNumber(dst);
    SemanticOperand result = computed;
    if (predicateIndex >= 0)
    {
        const ArmDecodedOperand& pg = operandOf(s, predicateIndex);
        SemanticOperand mask = s.readMaskOperand(predicateIndex, elementBits);
        result = s.predicate(mask, computed, s.readZ(reg, elementBits),
                             pg.m_predicateQualifier ==
                                ArmDecodedOperand::PREDICATE_ZEROING);
    }
    s.writeZ(reg, result);
}

// The value of the immediate operand 'index' as a vector of 'elementBits'
SemanticOperand immediateVector(SveLift& s, uint index, uint elementBits)
{
    return s.splatInteger(operandOf(s, index).m_immediate, elementBits);
}

/////////////////////////////////////////////////////////////////////////////
// Predicate logic, shared with the vector mnemonics (and, orr, eor, bic...)
/////////////////////////////////////////////////////////////////////////////

// and, bic, eor, nand, nor, orn, orr, sel of predicates, and the S forms
// (m_a: a SveDescriptor::m_name selects the operation)
void liftPredicateLogic(SveLift& s, const SveDescriptor& d)
{
    ArmLiftContext& c = s.context();
    const char* name = d.m_name;
    const bool isFlags = (strcmp(name, "ands") == 0) || (strcmp(name, "bics") == 0) ||
                         (strcmp(name, "eors") == 0) || (strcmp(name, "nands") == 0) ||
                         (strcmp(name, "nors") == 0) || (strcmp(name, "orns") == 0) ||
                         (strcmp(name, "orrs") == 0) || (strcmp(name, "nots") == 0) ||
                         (strcmp(name, "movs") == 0);
    const SemanticType type = s.predicateType();
    const uint destination = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand governing, n, m;
    // The operand layouts: [pd, pg/z, pn, pm], the aliases mov [pd, pn] /
    // [pd, pg/z, pn] / [pd, pg/m, pn], not [pd, pg/z, pn]
    const uint count = c.getOperandsCount();
    if (strcmp(name, "sel") == 0)
    {
        governing = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 1)));
        n = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 2)));
        m = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 3)));
        SemanticOperand notGoverning = s.emit(Op::OP_NOT, type, governing);
        SemanticOperand result = s.emit(Op::OP_OR, type,
            s.emit(Op::OP_AND, type, n, governing),
            s.emit(Op::OP_AND, type, m, notGoverning));
        s.writePredicateBits(destination, result);
        return;
    }
    if (strcmp(name, "mov") == 0 || strcmp(name, "movs") == 0)
    {
        if (count == 2)
        {
            // mov pd, pn = orr pd, pn, pn, pn
            n = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 1)));
            governing = n;
            m = n;
        } else
        {
            governing = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 1)));
            n = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 2)));
            if (operandOf(s, 1).m_predicateQualifier ==
                ArmDecodedOperand::PREDICATE_MERGING)
            {
                // mov pd, pg/m, pn = sel pd, pg, pn, pd
                SemanticOperand old = s.readPredicateBits(destination);
                SemanticOperand result = s.emit(Op::OP_OR, type,
                    s.emit(Op::OP_AND, type, n, governing),
                    s.emit(Op::OP_AND, type, old,
                           s.emit(Op::OP_NOT, type, governing)));
                s.writePredicateBits(destination, result);
                return;
            }
            m = n;
        }
        SemanticOperand result = s.emit(Op::OP_AND, type, n, governing);
        s.writePredicateBits(destination, result);
        if (isFlags)
            s.setPredicateFlags(governing, result, 8);
        return;
    }
    if (strcmp(name, "not") == 0 || strcmp(name, "nots") == 0)
    {
        // not pd, pg/z, pn = eor pd, pg/z, pn, pg
        governing = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 1)));
        n = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 2)));
        SemanticOperand result = s.emit(Op::OP_AND, type,
                                        s.emit(Op::OP_XOR, type, n, governing),
                                        governing);
        s.writePredicateBits(destination, result);
        if (isFlags)
            s.setPredicateFlags(governing, result, 8);
        return;
    }
    governing = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 1)));
    n = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 2)));
    m = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 3)));
    SemanticOperand value;
    if (strncmp(name, "and", 3) == 0)
        value = s.emit(Op::OP_AND, type, n, m);
    else if (strncmp(name, "bic", 3) == 0)
        value = s.emit(Op::OP_AND, type, n, s.emit(Op::OP_NOT, type, m));
    else if (strncmp(name, "eor", 3) == 0)
        value = s.emit(Op::OP_XOR, type, n, m);
    else if (strncmp(name, "nand", 4) == 0)
        value = s.emit(Op::OP_NOT, type, s.emit(Op::OP_AND, type, n, m));
    else if (strncmp(name, "nor", 3) == 0)
        value = s.emit(Op::OP_NOT, type, s.emit(Op::OP_OR, type, n, m));
    else if (strncmp(name, "orn", 3) == 0)
        value = s.emit(Op::OP_OR, type, n, s.emit(Op::OP_NOT, type, m));
    else
        value = s.emit(Op::OP_OR, type, n, m);
    SemanticOperand result = s.emit(Op::OP_AND, type, value, governing);
    s.writePredicateBits(destination, result);
    if (isFlags)
        s.setPredicateFlags(governing, result, 8);
}

/////////////////////////////////////////////////////////////////////////////
// Binary integer arithmetic and logic
/////////////////////////////////////////////////////////////////////////////

// The vector whose lanes are the element 'index' of the 128 bit segment of
// the lane (the indexed multiplies)
SemanticOperand segmentElement(SveLift& s, const SemanticOperand& vector, uint e,
                               uint index)
{
    const uint segmentLanes = 128 / e;
    if (s.lanes(e) <= segmentLanes)
        return s.emit(Op::OP_SHUFFLE, vector.getType(), vector, vector,
                      s.splatInteger(index, e));
    SemanticOperand lanes = s.laneIndex(0, 1, e);
    SemanticOperand base = s.emit(Op::OP_AND, s.integerType(e), lanes,
                                  s.splatInteger(~(int64)(segmentLanes - 1), e));
    return s.emit(Op::OP_SHUFFLE, vector.getType(), vector, vector,
                  s.emit(Op::OP_ADD, s.integerType(e), base, s.splatInteger(index, e)));
}

// add, sub, subr, mul, smulh, umulh, sdiv, udiv, sdivr, udivr, smax, smin,
// umax, umin, sabd, uabd, and, orr, eor, bic, the saturating add / sub
// m_a: the operation, m_b: F_*
void liftBinary(SveLift& s, const SveDescriptor& d)
{
    ArmLiftContext& c = s.context();
    if (SveLift::isP(operandOf(s, 0)))
    {
        liftPredicateLogic(s, d);
        return;
    }
    const uint count = c.getOperandsCount();
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    int predicateIndex = -1;
    uint first = 1;
    if (isPredicateAt(s, 1))
    {
        predicateIndex = 1;
        first = 2;
    }
    if (count != first + 2)
    {
        s.undefined();
        return;
    }
    SemanticOperand a = s.readZOperand(first, e);
    SemanticOperand b;
    if (isKind(s, first + 1, ArmDecodedOperand::KIND_IMMEDIATE))
        b = immediateVector(s, first + 1, e);
    else if (operandOf(s, first + 1).m_elementIndex >= 0)
        b = segmentElement(s, s.readZOperand(first + 1, e), e, operandOf(s, first + 1).m_elementIndex);
    else
        b = s.readZOperand(first + 1, e);
    if ((d.m_b & F_REVERSE) != 0)
    {
        SemanticOperand t = a;
        a = b;
        b = t;
    }
    SemanticOperand computed;
    if (((d.m_b & F_SATURATE) != 0) && isKind(s, first + 1, ArmDecodedOperand::KIND_IMMEDIATE))
    {
        // The signed saturating add / subtract of an unsigned immediate: on the
        // values biased by the sign bit it is an unsigned saturating one
        SemanticOperand sign = s.splatInteger((int64)((uint64)1 << (e - 1)), e);
        const Op::Operation biased = ((Op::Operation)d.m_a == Op::OP_ADD_SATURATE_SIGNED) ?
            Op::OP_ADD_SATURATE_UNSIGNED : Op::OP_SUBTRACT_SATURATE_UNSIGNED;
        computed = s.emit(Op::OP_XOR, type,
            s.emit(biased, type, s.emit(Op::OP_XOR, type, a, sign), b), sign);
        writeResult(s, predicateIndex, e, computed);
        return;
    }
    if ((d.m_b & F_DIVIDE) != 0)
        computed = s.emitBehavior((Op::Operation)d.m_a, type, a, b,
                                  Op::BEHAVIOR_DIVIDE_ZERO);
    else
        computed = s.emit((Op::Operation)d.m_a, type, a, b);
    writeResult(s, predicateIndex, e, computed);
}

// bic z, z, #imm does not exist; the logical immediates of and, orr, eor
// and the dupm are handled by liftBinary (an immediate source)

/////////////////////////////////////////////////////////////////////////////
// Shifts
/////////////////////////////////////////////////////////////////////////////

// Broadcast the 64 bit shift counts of the wide forms into the lanes of
// 'e' bits of their 64 bit chunk, clamped to 'e' (the count of the size or
// more shifts everything out)
SemanticOperand wideShiftCounts(SveLift& s, uint index, uint e)
{
    const SemanticType wide = s.integerType(64);
    SemanticOperand counts = s.readZOperand(index, 64);
    SemanticOperand limit = s.splatInteger(e, 64);
    SemanticOperand isLarge = s.compareLanes(Op::COND_UNSIGNED_GREATER, wide,
                                             counts, limit);
    counts = s.select(isLarge, limit, counts);
    // The count in every lane of its chunk
    for (uint shift = e; shift < 64; shift *= 2)
        counts = s.emit(Op::OP_OR, wide, counts,
                        s.emit(Op::OP_SHIFT_LEFT, wide, counts,
                               s.splatInteger(shift, 64)));
    return s.bitcast(counts, s.integerType(e));
}

// asr, lsr, lsl, asrr, lsrr, lslr (the vector, wide and immediate forms),
// asrd. m_a: the operation (OP_SHIFT_*), m_b: F_REVERSE, 8: asrd
void liftShift(SveLift& s, const SveDescriptor& d)
{
    ArmLiftContext& c = s.context();
    const uint count = c.getOperandsCount();
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    int predicateIndex = -1;
    uint first = 1;
    if (isPredicateAt(s, 1))
    {
        predicateIndex = 1;
        first = 2;
    }
    if (count != first + 2)
    {
        s.undefined();
        return;
    }
    SemanticOperand a = s.readZOperand(first, e);
    SemanticOperand b;
    if (isKind(s, first + 1, ArmDecodedOperand::KIND_IMMEDIATE))
    {
        const int64 amount = operandOf(s, first + 1).m_immediate;
        if ((d.m_b & 16) != 0)
        {
            // asrd: the division by 2^amount rounding toward zero
            SemanticOperand sign = s.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, type, a,
                                          s.splatInteger(e - 1, e));
            SemanticOperand bias = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, sign,
                                          s.splatInteger(e - amount, e));
            SemanticOperand sum = s.emit(Op::OP_ADD, type, a, bias);
            writeResult(s, predicateIndex, e,
                        s.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, type, sum,
                               s.splatInteger(amount, e)));
            return;
        }
        b = s.splatInteger(amount, e);
    } else if (SveLift::elementBitsOf(operandOf(s, first + 1)) == 64 && e < 64)
    {
        b = wideShiftCounts(s, first + 1, e);
    } else
    {
        b = s.readZOperand(first + 1, e);
    }
    if ((d.m_b & F_REVERSE) != 0)
    {
        SemanticOperand t = a;
        a = b;
        b = t;
    }
    writeResult(s, predicateIndex, e, s.emit((Op::Operation)d.m_a, type, a, b));
}

/////////////////////////////////////////////////////////////////////////////
// Unary operations
/////////////////////////////////////////////////////////////////////////////

enum {
    U_ABS, U_NEG, U_CLS, U_CLZ, U_CNT, U_CNOT, U_NOT, U_RBIT,
    U_REVB, U_REVH, U_REVW, U_SXTB, U_SXTH, U_SXTW, U_UXTB, U_UXTH, U_UXTW
};

// m_a: U_*
void liftUnary(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    int predicateIndex = -1;
    uint source = 1;
    if (isPredicateAt(s, 1))
    {
        predicateIndex = 1;
        source = 2;
    }
    if (SveLift::isP(operandOf(s, 0)))
    {
        liftPredicateLogic(s, d);
        return;
    }
    SemanticOperand a = s.readZOperand(source, e);
    SemanticOperand result;
    switch (d.m_a)
    {
    case U_ABS: result = s.emit(Op::OP_ABSOLUTE, type, a); break;
    case U_NEG: result = s.emit(Op::OP_NEGATE, type, a); break;
    case U_CLS: result = s.emit(Op::OP_COUNT_LEADING_SIGN_BITS, type, a); break;
    case U_CLZ: result = s.emit(Op::OP_COUNT_LEADING_ZEROS, type, a); break;
    case U_CNT: result = s.emit(Op::OP_POPULATION_COUNT, type, a); break;
    case U_CNOT:
        result = s.maskToVector(s.compareLanes(Op::COND_EQUAL, type, a,
                                               s.zero(type)), e);
        break;
    case U_NOT: result = s.emit(Op::OP_NOT, type, a); break;
    case U_RBIT: result = s.emit(Op::OP_BIT_REVERSE, type, a); break;
    case U_REVB: result = s.emit(Op::OP_BYTE_SWAP, type, a); break;
    case U_REVH:
        if (e == 32)
            result = s.emit(Op::OP_ROTATE_LEFT, type, a, s.splatInteger(16, e));
        else
        {
            // 64 bit elements: the halfwords in reverse order
            SemanticOperand mask = s.splatInteger(0xFFFF, 64);
            SemanticOperand h0 = s.emit(Op::OP_AND, type, a, mask);
            SemanticOperand h1 = s.emit(Op::OP_AND, type,
                s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, a, s.splatInteger(16, 64)), mask);
            SemanticOperand h2 = s.emit(Op::OP_AND, type,
                s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, a, s.splatInteger(32, 64)), mask);
            SemanticOperand h3 = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, a, s.splatInteger(48, 64));
            result = s.emit(Op::OP_OR, type,
                s.emit(Op::OP_OR, type,
                    s.emit(Op::OP_SHIFT_LEFT, type, h0, s.splatInteger(48, 64)),
                    s.emit(Op::OP_SHIFT_LEFT, type, h1, s.splatInteger(32, 64))),
                s.emit(Op::OP_OR, type,
                    s.emit(Op::OP_SHIFT_LEFT, type, h2, s.splatInteger(16, 64)), h3));
        }
        break;
    case U_REVW:
        result = s.emit(Op::OP_ROTATE_LEFT, type, a, s.splatInteger(32, 64));
        break;
    case U_SXTB: case U_SXTH: case U_SXTW:
    {
        const uint from = (d.m_a == U_SXTB) ? 8 : ((d.m_a == U_SXTH) ? 16 : 32);
        result = s.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, type,
                        s.emit(Op::OP_SHIFT_LEFT, type, a, s.splatInteger(e - from, e)),
                        s.splatInteger(e - from, e));
        break;
    }
    default:
    {
        const uint from = (d.m_a == U_UXTB) ? 8 : ((d.m_a == U_UXTH) ? 16 : 32);
        result = s.emit(Op::OP_AND, type, a,
                        s.splatInteger((int64)(((uint64)1 << from) - 1), e));
        break;
    }
    }
    writeResult(s, predicateIndex, e, result);
}

/////////////////////////////////////////////////////////////////////////////
// Multiply add: mla, mls (the accumulator is the destination), mad, msb
/////////////////////////////////////////////////////////////////////////////

// m_a: 0 mla, 1 mls, 2 mad, 3 msb
void liftMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand product, addend;
    if ((d.m_a < 2) && (s.context().getOperandsCount() == 3))
    {
        // zda, zn, zm[i]
        SemanticOperand n = s.readZOperand(1, e);
        SemanticOperand m = segmentElement(s, s.readZOperand(2, e), e, operandOf(s, 2).m_elementIndex);
        SemanticOperand accumulated = s.readZ(reg, e);
        s.writeZ(reg, s.emit(d.m_a == 0 ? Op::OP_ADD : Op::OP_SUBTRACT, type, accumulated,
                             s.emit(Op::OP_MULTIPLY, type, n, m)));
        return;
    }
    if (d.m_a < 2)
    {
        // zda, pg/m, zn, zm
        SemanticOperand n = s.readZOperand(2, e);
        SemanticOperand m = s.readZOperand(3, e);
        product = s.emit(Op::OP_MULTIPLY, type, n, m);
        addend = s.readZ(reg, e);
    } else
    {
        // zdn, pg/m, zm, za: zdn * zm (+/-) za
        SemanticOperand dn = s.readZ(reg, e);
        SemanticOperand m = s.readZOperand(2, e);
        addend = s.readZOperand(3, e);
        product = s.emit(Op::OP_MULTIPLY, type, dn, m);
    }
    SemanticOperand result;
    if ((d.m_a & 1) == 0)
        result = s.emit(Op::OP_ADD, type, addend, product);
    else
        result = s.emit(Op::OP_SUBTRACT, type, addend, product);
    writeResult(s, 1, e, result);
}

// suqadd: signed + unsigned, saturated as signed; usqadd: unsigned + signed,
// saturated as unsigned. m_a: 0 suqadd, 1 usqadd. Predicated
void liftMixedSaturating(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    SemanticOperand a = s.readZOperand(2, e);
    SemanticOperand b = s.readZOperand(3, e);
    SemanticOperand result;
    if (d.m_a == 0)
    {
        // Biased by the sign bit the signed value is unsigned, and adding an
        // unsigned number saturates at the signed maximum
        SemanticOperand sign = s.splatInteger((int64)((uint64)1 << (e - 1)), e);
        result = s.emit(Op::OP_XOR, type, s.emit(Op::OP_ADD_SATURATE_UNSIGNED, type,
                                                 s.emit(Op::OP_XOR, type, a, sign), b), sign);
    } else
    {
        // b signed: an addition of its value, or a subtraction of its magnitude
        SemanticOperand isNegative = s.compareLanes(Op::COND_SIGNED_LESS, type, b, s.zero(type));
        SemanticOperand magnitude = s.emit(Op::OP_NEGATE, type, b);
        result = s.select(isNegative, s.emit(Op::OP_SUBTRACT_SATURATE_UNSIGNED, type, a, magnitude),
                          s.emit(Op::OP_ADD_SATURATE_UNSIGNED, type, a, b));
    }
    writeResult(s, 1, e, result);
}

/////////////////////////////////////////////////////////////////////////////
// Copies, index
/////////////////////////////////////////////////////////////////////////////

// dup (the vector, scalar, immediate and indexed forms) and the mov aliases
void liftMove(SveLift& s, const SveDescriptor& d)
{
    ArmLiftContext& c = s.context();
    const uint count = c.getOperandsCount();
    const ArmDecodedOperand& dst = operandOf(s, 0);
    if (SveLift::isP(dst))
    {
        liftPredicateLogic(s, d);
        return;
    }
    const uint e = SveLift::elementBitsOf(dst);
    const SemanticType type = s.integerType(e);
    const uint reg = SveLift::registerNumber(dst);
    if (count == 2)
    {
        const ArmDecodedOperand& source = operandOf(s, 1);
        if (source.m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
        {
            s.writeZ(reg, s.splatInteger(source.m_immediate, e));
        } else if (SveLift::isZ(source) && source.m_elementIndex >= 0)
        {
            // dup zd, zn[i]: zero when the index is beyond the vector
            if ((uint)source.m_elementIndex >= s.lanes(e))
                s.writeZ(reg, s.zero(type));
            else
                s.writeZ(reg, s.splat(s.readZElement(1), type));
        } else if (SveLift::isZ(source))
        {
            // mov zd, zn = orr zd, zn, zn
            s.writeZ(reg, s.readZWhole(SveLift::registerNumber(source)));
        } else if ((source.m_register >= ARM64_REG_V0) && (source.m_register <= ARM64_REG_B31))
        {
            // dup zd.q, qn (the SIMD scalar register)
            s.writeZ(reg, s.splat(s.truncate(s.context().getRegister(source.m_register), e), type));
        } else
        {
            // dup zd, wn / xn
            s.writeZ(reg, s.splat(s.truncate(s.readGprOperand(1, e == 64 ? 64 : 32), e),
                                  type));
        }
        return;
    }
    if (count != 3)
    {
        s.undefined();
        return;
    }
    // mov zd, pg/m|z, #imm | wn | vn | zn
    const ArmDecodedOperand& pg = operandOf(s, 1);
    const ArmDecodedOperand& source = operandOf(s, 2);
    SemanticOperand value;
    if (source.m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
    {
        value = s.splatInteger(source.m_immediate, e);
    } else if (SveLift::isZ(source))
    {
        value = s.readZOperand(2, e);
    } else if (source.m_register >= ARM64_REG_V0)
    {
        // The SIMD scalar register b, h, s, d
        value = s.splat(s.truncate(s.context().getRegister(source.m_register), e), type);
    } else
    {
        value = s.splat(s.truncate(s.readGprOperand(2, e == 64 ? 64 : 32), e), type);
    }
    SemanticOperand mask = s.readMaskOperand(1, e);
    s.writeZ(reg, s.predicate(mask, value, s.readZ(reg, e),
                              pg.m_predicateQualifier ==
                                ArmDecodedOperand::PREDICATE_ZEROING));
}

// sel zd, pg, zn, zm
void liftSelect(SveLift& s, const SveDescriptor& d)
{
    if (SveLift::isP(operandOf(s, 0)))
    {
        liftPredicateLogic(s, d);
        return;
    }
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    SemanticOperand mask = s.readMaskOperand(1, e);
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)),
             s.select(mask, s.readZOperand(2, e), s.readZOperand(3, e)));
}

// index zd, start, step
void liftIndex(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    SemanticOperand operands[2];
    for (uint i = 0; i < 2; i++)
    {
        if (isKind(s, 1 + i, ArmDecodedOperand::KIND_IMMEDIATE))
            operands[i] = s.integer(operandOf(s, 1 + i).m_immediate, 64);
        else
            operands[i] = s.zeroExtend(s.readGprOperand(1 + i, e == 64 ? 64 : 32), 64);
    }
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)),
             s.emit(Op::OP_LANE_INDEX, s.integerType(e), operands[0], operands[1]));
}

// movprfx zd, zn | zd, pg/z|m, zn
void liftMovePrefix(SveLift& s, const SveDescriptor& d)
{
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    if (isPredicateAt(s, 1))
    {
        const uint e = SveLift::elementBitsOf(operandOf(s, 0));
        writeResult(s, 1, e, s.readZOperand(2, e));
    } else
    {
        s.writeZ(reg, s.readZWhole(SveLift::registerNumber(operandOf(s, 1))));
    }
}

// dupm zd, #bitmask
void liftDupm(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)),
             s.splatInteger(operandOf(s, 1).m_immediate, e));
}

/////////////////////////////////////////////////////////////////////////////
// Compares
/////////////////////////////////////////////////////////////////////////////

// cmpXX pd, pg/z, zn, zm | zm.d | #imm. m_a: the condition
void liftCompare(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    const Op::Condition condition = (Op::Condition)d.m_a;
    SemanticOperand governing = s.readMaskOperand(1, e);
    SemanticOperand governingBits = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 1)));
    SemanticOperand a = s.readZOperand(2, e);
    SemanticOperand result;
    if (isKind(s, 3, ArmDecodedOperand::KIND_IMMEDIATE))
    {
        result = s.compareLanes(condition, type, a,
                                s.splatInteger(operandOf(s, 3).m_immediate, e));
    } else if (SveLift::elementBitsOf(operandOf(s, 3)) == 64 && e < 64)
    {
        // The wide compare: each lane against the 64 bit element of its
        // chunk, both widened to 64 bits (signed or unsigned by the condition)
        const bool isSigned = (condition == Op::COND_EQUAL) ||
            (condition == Op::COND_NOT_EQUAL) ||
            (condition == Op::COND_SIGNED_GREATER_EQUAL) ||
            (condition == Op::COND_SIGNED_GREATER) ||
            (condition == Op::COND_SIGNED_LESS) ||
            (condition == Op::COND_SIGNED_LESS_EQUAL);
        SemanticOperand b = s.readZOperand(3, 64);
        const uint count = s.lanes(e);
        const SemanticType i64 = SemanticType::integer(64);
        SemanticOperand bits = s.integer(0, count);
        for (uint lane = 0; lane < count; lane++)
        {
            SemanticOperand x = s.extractLane(a, lane);
            x = s.emit(isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND, i64, x);
            SemanticOperand y = s.extractLane(b, lane * e / 64);
            SemanticOperand flag = s.compareScalar(condition, x, y);
            SemanticOperand placed = s.emit(Op::OP_SHIFT_LEFT, SemanticType::integer(count),
                s.emit(Op::OP_ZERO_EXTEND, SemanticType::integer(count), flag),
                s.integer(lane, count));
            bits = s.emit(Op::OP_OR, SemanticType::integer(count), bits, placed);
        }
        result = s.bitcast(bits, SemanticType::mask(count));
    } else
    {
        result = s.compareLanes(condition, type, a, s.readZOperand(3, e));
    }
    result = s.emit(Op::OP_AND, SemanticType::mask(s.lanes(e)), result, governing);
    SemanticOperand predicate = s.maskToPredicate(result, e);
    s.writePredicateBits(SveLift::registerNumber(operandOf(s, 0)), predicate);
    s.setPredicateFlags(governingBits, predicate, e);
}

/////////////////////////////////////////////////////////////////////////////
// Reductions into a SIMD scalar register
/////////////////////////////////////////////////////////////////////////////

// saddv, uaddv, smaxv, sminv, umaxv, uminv, andv, eorv, orv
// m_a: the combine operation, m_b: 1 signed sum (saddv), 2 unsigned sum
void liftReduce(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 2));
    const SemanticType type = s.integerType(e);
    SemanticOperand mask = s.readMaskOperand(1, e);
    SemanticOperand a = s.readZOperand(2, e);
    const uint destination = operandOf(s, 0).m_register;
    SemanticOperand result;
    if (d.m_b != 0)
    {
        // The sum of the elements, in 64 bits
        const SemanticType i64 = SemanticType::integer(64);
        SemanticOperand sum = s.integer(0, 64);
        SemanticOperand maskBits = s.bitcast(mask, SemanticType::integer(s.lanes(e)));
        for (uint lane = 0; lane < s.lanes(e); lane++)
        {
            SemanticOperand x = s.emit(d.m_b == 1 ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                                       i64, s.extractLane(a, lane));
            SemanticOperand active = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, SemanticType::integer(s.lanes(e)),
                       maskBits, s.integer(lane, s.lanes(e))));
            x = s.select(active, x, s.integer(0, 64));
            sum = s.emit(Op::OP_ADD, i64, sum, x);
        }
        result = sum;
    } else
    {
        int64 identity = 0;
        switch (d.m_a)
        {
        case Op::OP_MAXIMUM_SIGNED: identity = SveLift::minimumSigned(e); break;
        case Op::OP_MINIMUM_SIGNED: identity = SveLift::maximumSigned(e); break;
        case Op::OP_MINIMUM_UNSIGNED:
        case Op::OP_AND: identity = -1; break;
        default: identity = 0; break;
        }
        SemanticOperand masked = s.select(mask, a, s.splatInteger(identity, e));
        SemanticInstruction reduce(Op::OP_REDUCE, type, SemanticOperand(), masked);
        SemanticOperand scalar = s.context().newTemporary(SemanticType::integer(e));
        reduce.setDestination(scalar);
        reduce.setCombineOperation((Op::Operation)d.m_a);
        s.context().emit(reduce);
        result = scalar;
    }
    // The SIMD scalar register of the destination (b, h, s or d) zeroes the
    // rest of the z register
    s.context().move(s.context().getRegister(destination), result);
}

/////////////////////////////////////////////////////////////////////////////
// Permutes
/////////////////////////////////////////////////////////////////////////////

// The data of the permutes: a vector register as lanes of 'e' bits, a
// predicate as lanes of e / 8 bits (the permutes move the whole predicate
// element, not only its first bit)
struct PermuteData {
    SemanticType m_type;
    SemanticType m_indexType;
    uint m_lanes;
};

PermuteData permuteData(SveLift& s, bool isPredicate, uint e)
{
    PermuteData data;
    data.m_lanes = s.lanes(e);
    if (isPredicate)
    {
        data.m_type = SemanticType::vector(SemanticType::integer(e / 8), data.m_lanes);
        data.m_indexType = SemanticType::vector(SemanticType::integer(8), data.m_lanes);
    } else
    {
        data.m_type = s.integerType(e);
        data.m_indexType = (e > 64) ? SemanticType::vector(SemanticType::integer(64), data.m_lanes)
                                    : data.m_type;
    }
    return data;
}

SemanticOperand permuteRead(SveLift& s, bool isPredicate, uint index,
                            const PermuteData& data, uint e)
{
    if (isPredicate)
        return s.bitcast(s.readPredicateBits(SveLift::registerNumber(operandOf(s, index))),
                         data.m_type);
    return s.readZOperand(index, e);
}

void permuteWrite(SveLift& s, bool isPredicate, const SemanticOperand& value)
{
    if (isPredicate)
        s.writePredicateBits(SveLift::registerNumber(operandOf(s, 0)),
                             s.bitcast(value, s.predicateType()));
    else
        s.writeZ(SveLift::registerNumber(operandOf(s, 0)), value);
}

SemanticOperand indexSplat(SveLift& s, const PermuteData& data, int64 value)
{
    return s.splat(s.integer(value, data.m_indexType.getElementBits()), data.m_indexType);
}

// zip1, zip2, uzp1, uzp2, trn1, trn2 of vectors and predicates
// m_a: 0 zip1, 1 zip2, 2 uzp1, 3 uzp2, 4 trn1, 5 trn2
void liftPermute(SveLift& s, const SveDescriptor& d)
{
    const bool isPredicate = SveLift::isP(operandOf(s, 0));
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const PermuteData data = permuteData(s, isPredicate, e);
    const uint count = data.m_lanes;
    SemanticOperand a = permuteRead(s, isPredicate, 1, data, e);
    SemanticOperand b = permuteRead(s, isPredicate, 2, data, e);
    const uint indexBits = data.m_indexType.getElementBits();
    SemanticOperand result;
    switch (d.m_a)
    {
    case 0: result = s.emit(Op::OP_INTERLEAVE, data.m_type, a, b, s.integer(0, 32)); break;
    case 1: result = s.emit(Op::OP_INTERLEAVE, data.m_type, a, b, s.integer(count / 2, 32)); break;
    case 2: result = s.emit(Op::OP_SHUFFLE, data.m_type, a, b, s.laneIndexWide(0, 2, count, indexBits)); break;
    case 3: result = s.emit(Op::OP_SHUFFLE, data.m_type, a, b, s.laneIndexWide(1, 2, count, indexBits)); break;
    default:
    {
        // trn1: lane i takes a[i & ~1] (even) or b[i & ~1] (odd); trn2 i | 1
        SemanticOperand index = s.laneIndexWide(0, 1, count, indexBits);
        SemanticOperand odd = s.emit(Op::OP_AND, data.m_indexType, index, indexSplat(s, data, 1));
        SemanticOperand base = (d.m_a == 4) ?
            s.emit(Op::OP_AND, data.m_indexType, index, indexSplat(s, data, ~1LL)) :
            s.emit(Op::OP_OR, data.m_indexType, index, indexSplat(s, data, 1));
        SemanticOperand offset = s.emit(Op::OP_MULTIPLY, data.m_indexType, odd, indexSplat(s, data, count));
        result = s.emit(Op::OP_SHUFFLE, data.m_type, a, b,
                        s.emit(Op::OP_ADD, data.m_indexType, base, offset));
        break;
    }
    }
    permuteWrite(s, isPredicate, result);
}

// rev zd, zn | pd, pn
void liftReverse(SveLift& s, const SveDescriptor& d)
{
    const bool isPredicate = SveLift::isP(operandOf(s, 0));
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const PermuteData data = permuteData(s, isPredicate, e);
    SemanticOperand a = permuteRead(s, isPredicate, 1, data, e);
    SemanticOperand index = s.emit(Op::OP_SUBTRACT, data.m_indexType,
        indexSplat(s, data, data.m_lanes - 1),
        s.laneIndexWide(0, 1, data.m_lanes, data.m_indexType.getElementBits()));
    permuteWrite(s, isPredicate, s.emit(Op::OP_SHUFFLE, data.m_type, a, a, index));
}

// tbl zd, zn, zm | {zn, zn+1}, zm (SVE2); tbx zd, zn, zm
void liftTable(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    const uint count = s.lanes(e);
    const ArmDecodedOperand& table = operandOf(s, 1);
    SemanticOperand first = s.readZ(SveLift::registerNumber(table), e);
    SemanticOperand second;
    const uint indexOperand = 2;
    if (table.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST && table.m_count == 2)
        second = s.readZ((SveLift::registerNumber(table) + table.m_stride) & 31, e);
    else
        second = s.zero(type);
    SemanticOperand index = s.readZOperand(indexOperand, e);
    SemanticOperand looked = s.emitBehavior(Op::OP_SHUFFLE, type, first, second,
                                            Op::BEHAVIOR_SHUFFLE_ZERO, index);
    if (strcmp(d.m_name, "tbx") == 0)
    {
        // Out of range indexes keep the destination
        SemanticOperand inRange = s.compareLanes(Op::COND_UNSIGNED_LESS, type, index,
            s.splatInteger((table.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST &&
                            table.m_count == 2) ? 2 * count : count, e));
        looked = s.select(inRange, looked,
                          s.readZ(SveLift::registerNumber(operandOf(s, 0)), e));
    }
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)), looked);
}

// sunpklo, sunpkhi, uunpklo, uunpkhi, punpklo, punpkhi. m_a: 1 signed, 2
// unsigned, 3 predicate. m_b: 1 high
void liftUnpack(SveLift& s, const SveDescriptor& d)
{
    if (d.m_a == 3)
    {
        // The low / high half of the bits of the byte predicate, spread to
        // an element of twice the size
        const uint e = SveLift::elementBitsOf(operandOf(s, 0));
        const uint half = s.predicateBits() / 2;
        SemanticOperand bits = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 1)));
        if (d.m_b != 0)
            bits = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, s.predicateType(), bits,
                          s.integer(half, s.predicateBits()));
        bits = s.truncate(bits, half);
        SemanticOperand wide = s.zeroExtend(bits, 64);
        SemanticOperand spread = s.emit(Op::OP_BIT_DEPOSIT, SemanticType::integer(64), wide,
                                        s.integer((int64)s.elementBitsMask(e), 64));
        s.writePredicateBits(SveLift::registerNumber(operandOf(s, 0)),
                             s.truncate(spread, s.predicateBits()));
        return;
    }
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint narrow = e / 2;
    SemanticOperand whole = s.readZWhole(SveLift::registerNumber(operandOf(s, 1)));
    SemanticOperand half = s.emit(Op::OP_EXTRACT, SemanticType::integer(s.vectorBits() / 2), whole,
                                  s.integer(d.m_b != 0 ? s.vectorBits() / 2 : 0, 32));
    SemanticOperand source = s.bitcast(half, SemanticType::vector(
                SemanticType::integer(narrow), s.lanes(e)));
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)),
             s.emit(d.m_a == 1 ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                    s.integerType(e), source));
}

// ext zdn, zdn, zm, #imm (bytes of the concatenation zdn:zm); the SVE2
// constructive form is ext zd, {zn, zn+1}, #imm
void liftExtract(SveLift& s, const SveDescriptor& d)
{
    const uint count = s.lanes(8);
    SemanticOperand first, second;
    uint immediateIndex;
    if (operandOf(s, 1).m_kind == ArmDecodedOperand::KIND_REGISTER_LIST)
    {
        const ArmDecodedOperand& list = operandOf(s, 1);
        const uint reg = SveLift::registerNumber(list);
        first = s.readZ(reg, 8);
        second = s.readZ((reg + list.m_stride) & 31, 8);
        immediateIndex = 2;
    } else
    {
        first = s.readZOperand(1, 8);
        second = s.readZOperand(2, 8);
        immediateIndex = 3;
    }
    int64 position = operandOf(s, immediateIndex).m_immediate;
    if ((uint)position >= count)
        position = 0;
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)),
             s.emit(Op::OP_SLIDE, s.integerType(8), first, second,
                    s.integer(position, 32)));
}

// insr zdn, scalar | simd scalar
void liftInsert(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    const ArmDecodedOperand& source = operandOf(s, 1);
    SemanticOperand scalar;
    if (source.m_register >= ARM64_REG_V0)
        scalar = s.truncate(s.context().getRegister(source.m_register), e);
    else
        scalar = s.truncate(s.readGprOperand(1, e == 64 ? 64 : 32), e);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand seed = s.insertLane(s.zero(type), scalar, s.lanes(e) - 1);
    s.writeZ(reg, s.emit(Op::OP_SLIDE, type, seed, s.readZ(reg, e),
                         s.integer(s.lanes(e) - 1, 32)));
}

/////////////////////////////////////////////////////////////////////////////
// The tables
/////////////////////////////////////////////////////////////////////////////

#define BIN(name, op, flags) { name, liftBinary, Op::op, flags }
#define SHIFT(name, op, flags) { name, liftShift, Op::op, flags }
#define UNARY(name, id) { name, liftUnary, id, 0 }

const SveDescriptor gIntegerTable[] =
{
    BIN("add", OP_ADD, 0),
    BIN("sub", OP_SUBTRACT, 0),
    BIN("subr", OP_SUBTRACT, F_REVERSE),
    BIN("mul", OP_MULTIPLY, 0),
    BIN("smulh", OP_MULTIPLY_HIGH_SIGNED, 0),
    BIN("umulh", OP_MULTIPLY_HIGH_UNSIGNED, 0),
    BIN("sdiv", OP_DIVIDE_SIGNED, F_DIVIDE),
    BIN("udiv", OP_DIVIDE_UNSIGNED, F_DIVIDE),
    BIN("sdivr", OP_DIVIDE_SIGNED, F_DIVIDE | F_REVERSE),
    BIN("udivr", OP_DIVIDE_UNSIGNED, F_DIVIDE | F_REVERSE),
    BIN("smax", OP_MAXIMUM_SIGNED, 0),
    BIN("smin", OP_MINIMUM_SIGNED, 0),
    BIN("umax", OP_MAXIMUM_UNSIGNED, 0),
    BIN("umin", OP_MINIMUM_UNSIGNED, 0),
    BIN("sabd", OP_ABSOLUTE_DIFFERENCE_SIGNED, 0),
    BIN("uabd", OP_ABSOLUTE_DIFFERENCE_UNSIGNED, 0),
    BIN("and", OP_AND, 0),
    BIN("orr", OP_OR, 0),
    BIN("eor", OP_XOR, 0),
    BIN("bic", OP_AND_NOT, F_REVERSE),
    BIN("sqadd", OP_ADD_SATURATE_SIGNED, F_SATURATE),
    BIN("uqadd", OP_ADD_SATURATE_UNSIGNED, 0),
    BIN("sqsub", OP_SUBTRACT_SATURATE_SIGNED, F_SATURATE),
    BIN("uqsub", OP_SUBTRACT_SATURATE_UNSIGNED, 0),
    BIN("sqsubr", OP_SUBTRACT_SATURATE_SIGNED, F_REVERSE),
    BIN("uqsubr", OP_SUBTRACT_SATURATE_UNSIGNED, F_REVERSE),
    { "ands", liftPredicateLogic, 0, 0 },
    { "bics", liftPredicateLogic, 0, 0 },
    { "eors", liftPredicateLogic, 0, 0 },
    { "nand", liftPredicateLogic, 0, 0 },
    { "nands", liftPredicateLogic, 0, 0 },
    { "nor", liftPredicateLogic, 0, 0 },
    { "nors", liftPredicateLogic, 0, 0 },
    { "orn", liftPredicateLogic, 0, 0 },
    { "orns", liftPredicateLogic, 0, 0 },
    { "orrs", liftPredicateLogic, 0, 0 },
    { "movs", liftPredicateLogic, 0, 0 },
    { "nots", liftPredicateLogic, 0, 0 },
    SHIFT("asr", OP_SHIFT_RIGHT_ARITHMETIC, 0),
    SHIFT("lsr", OP_SHIFT_RIGHT_LOGICAL, 0),
    SHIFT("lsl", OP_SHIFT_LEFT, 0),
    SHIFT("asrr", OP_SHIFT_RIGHT_ARITHMETIC, F_REVERSE),
    SHIFT("lsrr", OP_SHIFT_RIGHT_LOGICAL, F_REVERSE),
    SHIFT("lslr", OP_SHIFT_LEFT, F_REVERSE),
    SHIFT("asrd", OP_SHIFT_RIGHT_ARITHMETIC, 16),
    UNARY("abs", U_ABS),
    UNARY("neg", U_NEG),
    UNARY("cls", U_CLS),
    UNARY("clz", U_CLZ),
    UNARY("cnt", U_CNT),
    UNARY("cnot", U_CNOT),
    UNARY("not", U_NOT),
    UNARY("rbit", U_RBIT),
    UNARY("revb", U_REVB),
    UNARY("revh", U_REVH),
    UNARY("revw", U_REVW),
    UNARY("sxtb", U_SXTB),
    UNARY("sxth", U_SXTH),
    UNARY("sxtw", U_SXTW),
    UNARY("uxtb", U_UXTB),
    UNARY("uxth", U_UXTH),
    UNARY("uxtw", U_UXTW),
    { "mla", liftMultiplyAdd, 0, 0 },
    { "mls", liftMultiplyAdd, 1, 0 },
    { "mad", liftMultiplyAdd, 2, 0 },
    { "msb", liftMultiplyAdd, 3, 0 },
    { "suqadd", liftMixedSaturating, 0, 0 },
    { "usqadd", liftMixedSaturating, 1, 0 },
    { "mov", liftMove, 0, 0 },
    { "dup", liftMove, 0, 0 },
    { "sel", liftSelect, 0, 0 },
    { "index", liftIndex, 0, 0 },
    { "movprfx", liftMovePrefix, 0, 0 },
    { "dupm", liftDupm, 0, 0 },
    { "cmpeq", liftCompare, Op::COND_EQUAL, 0 },
    { "cmpne", liftCompare, Op::COND_NOT_EQUAL, 0 },
    { "cmpge", liftCompare, Op::COND_SIGNED_GREATER_EQUAL, 0 },
    { "cmpgt", liftCompare, Op::COND_SIGNED_GREATER, 0 },
    { "cmplt", liftCompare, Op::COND_SIGNED_LESS, 0 },
    { "cmple", liftCompare, Op::COND_SIGNED_LESS_EQUAL, 0 },
    { "cmphs", liftCompare, Op::COND_UNSIGNED_GREATER_EQUAL, 0 },
    { "cmphi", liftCompare, Op::COND_UNSIGNED_GREATER, 0 },
    { "cmplo", liftCompare, Op::COND_UNSIGNED_LESS, 0 },
    { "cmpls", liftCompare, Op::COND_UNSIGNED_LESS_EQUAL, 0 },
    { "saddv", liftReduce, Op::OP_ADD, 1 },
    { "uaddv", liftReduce, Op::OP_ADD, 2 },
    { "smaxv", liftReduce, Op::OP_MAXIMUM_SIGNED, 0 },
    { "sminv", liftReduce, Op::OP_MINIMUM_SIGNED, 0 },
    { "umaxv", liftReduce, Op::OP_MAXIMUM_UNSIGNED, 0 },
    { "uminv", liftReduce, Op::OP_MINIMUM_UNSIGNED, 0 },
    { "andv", liftReduce, Op::OP_AND, 0 },
    { "eorv", liftReduce, Op::OP_XOR, 0 },
    { "orv", liftReduce, Op::OP_OR, 0 },
    { "zip1", liftPermute, 0, 0 },
    { "zip2", liftPermute, 1, 0 },
    { "uzp1", liftPermute, 2, 0 },
    { "uzp2", liftPermute, 3, 0 },
    { "trn1", liftPermute, 4, 0 },
    { "trn2", liftPermute, 5, 0 },
    { "rev", liftReverse, 0, 0 },
    { "tbl", liftTable, 0, 0 },
    { "tbx", liftTable, 0, 0 },
    { "sunpklo", liftUnpack, 1, 0 },
    { "sunpkhi", liftUnpack, 1, 1 },
    { "uunpklo", liftUnpack, 2, 0 },
    { "uunpkhi", liftUnpack, 2, 1 },
    { "punpklo", liftUnpack, 3, 0 },
    { "punpkhi", liftUnpack, 3, 1 },
    { "ext", liftExtract, 0, 0 },
    { "insr", liftInsert, 0, 0 },
    { NULL, NULL, 0, 0 }
};

} // namespace

void armdis::a64::liftSveInteger(ArmLiftContext& context)
{
    runSveTable(context, gIntegerTable);
}
