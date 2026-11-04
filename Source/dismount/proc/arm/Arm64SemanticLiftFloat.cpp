/*
 * Arm64SemanticLiftFloat.cpp
 *
 * Implementation file
 *
 * The A64 lifter of the scalar and vector floating point instructions and the
 * conversions with the integers, see Arm64SemanticSimd.h. The semantics are the
 * ones of the Arm ARM pseudocode (FPAdd, FPMulAdd, FPMax, FPToFixed, ...): the
 * floating point operations of the IR take the rounding mode, flush to zero
 * and default NaN from FPCR and set the cumulative flags of FPSR
 * (Arm64Architecture::getFloatEnvironment() and SemanticFloat).
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
#include "dismount/proc/arm/Arm64SemanticSimd.h"

using namespace armdis;

typedef SemanticOperation Op;
typedef SimdLift::Val Val;

namespace {

/////////////////////////////////////////////////////////////////////////////
// Helpers
/////////////////////////////////////////////////////////////////////////////

// The 'index'th operand is an element (v0.s[1])
bool isElement(SimdLift& s, uint index)
{
    return s.operand(index).m_elementIndex >= 0;
}

// Read the register operand 'index' as the lanes of the destination shape:
// an element is broadcast to every lane (the "by element" forms)
Val readLanes(SimdLift& s, uint index, const SimdShape& destination)
{
    Val value = s.c().read(index);
    if (isElement(s, index) && destination.m_isVector)
        value = s.splat(SimdLift::V(destination.m_elementBits,
                                    destination.m_lanes), value);
    return value;
}

// The negation of floating point lanes (a sign flip, a NaN too)
Val negate(SimdLift& s, const Val& value)
{
    return s.fp(Op::OP_FLOAT_NEGATE, value);
}

// The zero of the type of the lanes
Val zeroLike(SimdLift& s, const Val& value)
{
    const SemanticType type = value.getType();
    if (type.isVector())
        return s.splat(type, 0);
    return s.imm(0, type);
}

// The rounding mode of the fcvt / frint mnemonics by their letter
Op::RoundingMode roundingOf(char letter)
{
    switch (letter)
    {
    case 'n': return Op::ROUND_NEAREST_EVEN;
    case 'm': return Op::ROUND_TOWARD_NEGATIVE;
    case 'a': return Op::ROUND_NEAREST_AWAY;
    case 'p': return Op::ROUND_TOWARD_POSITIVE;
    case 'z': return Op::ROUND_TOWARD_ZERO;
    default:  return Op::ROUND_DYNAMIC;
    }
}

// The value of the floating point immediate of fmov: VFPExpandImm
uint64 expandImmediate(uint imm8, uint bits)
{
    const uint exponentBits = (bits == 16) ? 5 : ((bits == 32) ? 8 : 11);
    const uint fractionBits = bits - exponentBits - 1;
    const uint sign = (imm8 >> 7) & 1;
    const uint b6 = (imm8 >> 6) & 1;
    uint64 exponent = (uint64)((b6 ^ 1) << (exponentBits - 1));
    for (uint i = 0; i < exponentBits - 3; i++)
        exponent |= (uint64)b6 << (exponentBits - 2 - i);
    exponent |= (imm8 >> 4) & 3;
    const uint64 fraction = (uint64)(imm8 & 15) << (fractionBits - 4);
    return ((uint64)sign << (bits - 1)) | (exponent << fractionBits) | fraction;
}

/////////////////////////////////////////////////////////////////////////////
// Arithmetic
/////////////////////////////////////////////////////////////////////////////

// The binary operations of the scalar and vector forms (and the "by element")
bool liftBinary(SimdLift& s)
{
    Op::Operation operation;
    Op::Behavior behavior = Op::BEHAVIOR_DEFAULT;
    bool isAbsoluteDifference = false;
    bool isNegated = false;
    if (s.is("fadd") || s.is("faddp"))
        operation = Op::OP_FLOAT_ADD;
    else if (s.is("fsub"))
        operation = Op::OP_FLOAT_SUBTRACT;
    else if (s.is("fmul") || s.is("fnmul"))
    {
        operation = Op::OP_FLOAT_MULTIPLY;
        isNegated = s.is("fnmul");
    } else if (s.is("fmulx"))
    {
        operation = Op::OP_FLOAT_MULTIPLY;
        behavior = Op::BEHAVIOR_MULTIPLY_EXTENDED;
    } else if (s.is("fdiv"))
        operation = Op::OP_FLOAT_DIVIDE;
    else if (s.is("fmax") || s.is("fmaxp"))
    {
        operation = Op::OP_FLOAT_MAXIMUM;
        behavior = Op::BEHAVIOR_MINMAX_PROPAGATE_NAN;
    } else if (s.is("fmin") || s.is("fminp"))
    {
        operation = Op::OP_FLOAT_MINIMUM;
        behavior = Op::BEHAVIOR_MINMAX_PROPAGATE_NAN;
    } else if (s.is("fmaxnm") || s.is("fmaxnmp"))
    {
        operation = Op::OP_FLOAT_MAXIMUM;
        behavior = Op::BEHAVIOR_MINMAX_NUMBER;
    } else if (s.is("fminnm") || s.is("fminnmp"))
    {
        operation = Op::OP_FLOAT_MINIMUM;
        behavior = Op::BEHAVIOR_MINMAX_NUMBER;
    } else if (s.is("fabd"))
    {
        operation = Op::OP_FLOAT_SUBTRACT;
        isAbsoluteDifference = true;
    } else
        return false;

    const bool isPairwise = s.is("faddp") || s.is("fmaxp") || s.is("fminp") ||
                            s.is("fmaxnmp") || s.is("fminnmp");
    SimdLift& l = s;
    const SimdShape d = l.shape(0);
    if (isPairwise)
    {
        if (s.count() == 2)
        {
            // The scalar form: faddp d0, v1.2d (h0, v1.2h too: the arrangement
            // of two halves is not a vector of the lifter)
            Val v = l.c().getRegister(l.operand(1).m_register);
            v = l.cast(SimdLift::V(d.m_elementBits, 128 / d.m_elementBits), v);
            Val first = l.lane(v, 0);
            Val second = l.lane(v, 1);
            l.c().write(0, l.fp(operation, first, second, Val(), behavior));
            return true;
        }
        // The pairs of Vn:Vm
        Val n = l.c().read(1);
        Val m = l.c().read(2);
        Val result = l.splat(n.getType(), 0);
        const uint lanes = d.m_lanes;
        for (uint i = 0; i < lanes; i++)
        {
            const Val& source = (i < lanes / 2) ? n : m;
            const uint j = (i < lanes / 2) ? i : (i - lanes / 2);
            result = l.setLane(result,
                    l.fp(operation, l.lane(source, 2 * j), l.lane(source, 2 * j + 1),
                         Val(), behavior), i);
        }
        l.c().write(0, result);
        return true;
    }

    Val n = readLanes(l, 1, d);
    Val m = readLanes(l, 2, d);
    Val result = l.fp(operation, n, m, Val(), behavior);
    if (isAbsoluteDifference)
        result = l.fp(Op::OP_FLOAT_ABSOLUTE, result);
    if (isNegated)
        result = negate(l, result);
    l.c().write(0, result);
    return true;
}

// fmadd, fmsub, fnmadd, fnmsub, fmla, fmls
bool liftFused(SimdLift& s)
{
    const SimdShape d = s.shape(0);
    if (s.is("fmadd") || s.is("fmsub") || s.is("fnmadd") || s.is("fnmsub"))
    {
        Val n = s.c().read(1);
        Val m = s.c().read(2);
        Val a = s.c().read(3);
        if (s.is("fmsub") || s.is("fnmadd"))
            n = negate(s, n);
        if (s.is("fnmadd") || s.is("fnmsub"))
            a = negate(s, a);
        s.c().write(0, s.fp(Op::OP_FLOAT_MULTIPLY_ADD, n, m, a));
        return true;
    }
    if (s.is("fmla") || s.is("fmls"))
    {
        Val n = readLanes(s, 1, d);
        if (s.count() == 3 && isElement(s, 2) && !d.m_isVector)
            n = s.c().read(1);
        Val m = readLanes(s, 2, d);
        Val acc = s.c().read(0);
        if (s.is("fmls"))
            n = negate(s, n);
        s.c().write(0, s.fp(Op::OP_FLOAT_MULTIPLY_ADD, n, m, acc));
        return true;
    }
    return false;
}

// The unary operations
bool liftUnary(SimdLift& s)
{
    const SimdShape d = s.shape(0);
    if (s.is("fabs") || s.is("fneg"))
    {
        Val n = s.c().read(1);
        s.c().write(0, s.fp(s.is("fabs") ? Op::OP_FLOAT_ABSOLUTE :
                                           Op::OP_FLOAT_NEGATE, n));
        return true;
    }
    if (s.is("fsqrt"))
    {
        s.c().write(0, s.fp(Op::OP_FLOAT_SQUARE_ROOT, s.c().read(1)));
        return true;
    }
    if (s.startsWith("frint"))
    {
        const char* name = s.name();
        Val n = s.c().read(1);
        Val result;
        if (s.is("frint32z") || s.is("frint32x") || s.is("frint64z") ||
            s.is("frint64x"))
        {
            const bool is32 = name[5] == '3';
            const bool isZero = name[7] == 'z';
            result = s.fp(Op::OP_FLOAT_ROUND, n, Val(), Val(),
                          is32 ? Op::BEHAVIOR_ROUND_INTEGER_32 :
                                 Op::BEHAVIOR_ROUND_INTEGER_64,
                          isZero ? Op::ROUND_TOWARD_ZERO : Op::ROUND_DYNAMIC);
        } else
        {
            const char letter = name[5];
            // frintx signals inexact, the others don't
            result = s.fp(Op::OP_FLOAT_ROUND, n, Val(), Val(),
                          Op::BEHAVIOR_DEFAULT, roundingOf(letter),
                          letter == 'x' ? Op::FLOAT_ROUND_SIGNAL_INEXACT :
                                          Op::FLOAT_ROUND_SUPPRESS_INEXACT);
        }
        s.c().write(0, result);
        return true;
    }
    if (s.is("frecpe") || s.is("frsqrte") || s.is("frecpx"))
    {
        const Op::Operation operation = s.is("frsqrte") ?
                Op::OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE :
                Op::OP_FLOAT_RECIPROCAL_ESTIMATE;
        s.c().write(0, s.fp(operation, s.c().read(1), Val(), Val(),
                    s.is("frecpx") ? Op::BEHAVIOR_ESTIMATE_ARM_EXPONENT :
                                     Op::BEHAVIOR_ESTIMATE_ARM));
        return true;
    }
    if (s.is("frecps") || s.is("frsqrts"))
    {
        Val n = readLanes(s, 1, d);
        Val m = readLanes(s, 2, d);
        s.c().write(0, s.fp(Op::OP_FLOAT_MULTIPLY_ADD, n, m, Val(),
                    s.is("frecps") ? Op::BEHAVIOR_STEP_RECIPROCAL :
                                     Op::BEHAVIOR_STEP_RECIPROCAL_SQRT));
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// Compares
/////////////////////////////////////////////////////////////////////////////

// fcmeq, fcmge, fcmgt, fcmle, fcmlt, facge, facgt, facle, faclt: a mask of
// all ones / zeros lanes. The second operand is a register or #0.0
bool liftVectorCompare(SimdLift& s)
{
    const char* name = s.name();
    const bool isAbsolute = s.startsWith("fac");
    if (!isAbsolute && !s.startsWith("fcm"))
        return false;
    if (s.is("fcmp") || s.is("fcmpe") || s.is("fcmla"))
        return false;
    const SimdShape d = s.shape(0);
    // The relation: eq, ge, gt, le, lt
    const char* relation = name + 3;
    Op::Condition condition;
    bool isSwapped = false;
    if (strcmp(relation, "eq") == 0)
        condition = Op::COND_FLOAT_EQUAL;
    else if (strcmp(relation, "ge") == 0)
        condition = Op::COND_FLOAT_GREATER_EQUAL;
    else if (strcmp(relation, "gt") == 0)
        condition = Op::COND_FLOAT_GREATER;
    else if (strcmp(relation, "le") == 0)
    {
        // a <= b is b >= a: the order of the operands decides the flags of
        // two NaNs only (the operands are equal in the processor)
        condition = Op::COND_FLOAT_GREATER_EQUAL;
        isSwapped = true;
    } else if (strcmp(relation, "lt") == 0)
    {
        condition = Op::COND_FLOAT_GREATER;
        isSwapped = true;
    } else
        return false;

    Val n = s.c().read(1);
    Val m;
    if (s.count() > 2 && s.operand(2).m_kind == ArmDecodedOperand::KIND_REGISTER)
        m = s.c().read(2);
    else
        m = zeroLike(s, n);
    if (isAbsolute)
    {
        n = s.fp(Op::OP_FLOAT_ABSOLUTE, n);
        m = s.fp(Op::OP_FLOAT_ABSOLUTE, m);
    }
    if (isSwapped)
    {
        Val t = n;
        n = m;
        m = t;
    }
    Val mask = s.fpCompare(condition, n, m, Op::BEHAVIOR_DEFAULT);
    s.c().write(0, s.maskToLanes(mask, d.m_elementBits, d.m_lanes));
    return true;
}

// Set N Z C V from the compare of two scalars (fcmp, fcmpe)
void setCompareFlags(SimdLift& s, const Val& n, const Val& m, bool isSignaling)
{
    const Op::Behavior behavior = isSignaling ? Op::BEHAVIOR_COMPARE_SIGNALING :
                                                Op::BEHAVIOR_COMPARE_QUIET;
    Val less = s.fpCompare(Op::COND_FLOAT_LESS, n, m, behavior);
    Val equal = s.fpCompare(Op::COND_FLOAT_EQUAL, n, m, behavior);
    Val unordered = s.fpCompare(Op::COND_FLOAT_UNORDERED, n, m, behavior);
    ArmLiftContext& c = s.c();
    c.move(c.getRegister(ARM64_REG_N), less);
    c.move(c.getRegister(ARM64_REG_Z), equal);
    c.move(c.getRegister(ARM64_REG_C), c.emit(Op::OP_NOT, SemanticType::integer(1), less));
    c.move(c.getRegister(ARM64_REG_V), unordered);
}

// fcmp, fcmpe, fccmp, fccmpe, fcsel
bool liftScalarCompare(SimdLift& s)
{
    ArmLiftContext& c = s.c();
    if (s.is("fcmp") || s.is("fcmpe"))
    {
        Val n = c.read(0);
        Val m = (s.operand(1).m_kind == ArmDecodedOperand::KIND_REGISTER) ?
                    c.read(1) : s.imm(0, n.getType());
        setCompareFlags(s, n, m, s.is("fcmpe"));
        return true;
    }
    if (s.is("fccmp") || s.is("fccmpe"))
    {
        // operands: Rn, Rm, #nzcv, cond
        const uint nzcv = (uint)s.operand(2).m_immediate;
        Val condition = c.testCondition(s.operand(3).m_condition, true);
        Val failed = c.newLabel();
        Val done = c.newLabel();
        c.branchIf(condition, failed);
        setCompareFlags(s, c.read(0), c.read(1), s.is("fccmpe"));
        c.branch(done);
        c.placeLabel(failed);
        const SemanticType flag = SemanticType::integer(1);
        c.move(c.getRegister(ARM64_REG_N), s.imm((nzcv >> 3) & 1, flag));
        c.move(c.getRegister(ARM64_REG_Z), s.imm((nzcv >> 2) & 1, flag));
        c.move(c.getRegister(ARM64_REG_C), s.imm((nzcv >> 1) & 1, flag));
        c.move(c.getRegister(ARM64_REG_V), s.imm(nzcv & 1, flag));
        c.placeLabel(done);
        return true;
    }
    if (s.is("fcsel"))
    {
        Val condition = c.testCondition(s.operand(3).m_condition);
        c.write(0, s.select(condition, c.read(1), c.read(2)));
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// Pairwise and across lanes
/////////////////////////////////////////////////////////////////////////////

// fmaxv, fminv, fmaxnmv, fminnmv: the tree of the Reduce() of the pseudocode
bool liftAcross(SimdLift& s)
{
    Op::Operation operation;
    Op::Behavior behavior;
    if (s.is("fmaxv"))
    {
        operation = Op::OP_FLOAT_MAXIMUM;
        behavior = Op::BEHAVIOR_MINMAX_PROPAGATE_NAN;
    } else if (s.is("fminv"))
    {
        operation = Op::OP_FLOAT_MINIMUM;
        behavior = Op::BEHAVIOR_MINMAX_PROPAGATE_NAN;
    } else if (s.is("fmaxnmv"))
    {
        operation = Op::OP_FLOAT_MAXIMUM;
        behavior = Op::BEHAVIOR_MINMAX_NUMBER;
    } else if (s.is("fminnmv"))
    {
        operation = Op::OP_FLOAT_MINIMUM;
        behavior = Op::BEHAVIOR_MINMAX_NUMBER;
    } else
        return false;
    Val v = s.c().read(1);
    const SimdShape n = s.shape(1);
    cArray<Val> items(n.m_lanes);
    for (uint i = 0; i < n.m_lanes; i++)
        items[i] = s.lane(v, i);
    uint count = n.m_lanes;
    while (count > 1)
    {
        for (uint i = 0; i < count / 2; i++)
            items[i] = s.fp(operation, items[2 * i], items[2 * i + 1], Val(),
                            behavior);
        count /= 2;
    }
    s.c().write(0, items[0]);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Complex numbers
/////////////////////////////////////////////////////////////////////////////

// fcadd (rotate 90, 270)
bool liftComplexAdd(SimdLift& s)
{
    if (!s.is("fcadd"))
        return false;
    const SimdShape d = s.shape(0);
    Val n = s.c().read(1);
    Val m = s.c().read(2);
    const bool is270 = s.operand(3).m_immediate == 270;
    Val result = s.splat(n.getType(), 0);
    for (uint i = 0; i < d.m_lanes; i += 2)
    {
        Val real2 = s.lane(m, i);
        Val imaginary2 = s.lane(m, i + 1);
        Val first, third;
        if (!is270)
        {
            first = negate(s, imaginary2);
            third = real2;
        } else
        {
            first = imaginary2;
            third = negate(s, real2);
        }
        result = s.setLane(result, s.fp(Op::OP_FLOAT_ADD, s.lane(n, i), first), i);
        result = s.setLane(result, s.fp(Op::OP_FLOAT_ADD, s.lane(n, i + 1), third), i + 1);
    }
    s.c().write(0, result);
    return true;
}

// fcmla (rotate 0, 90, 180, 270), with a register or an element pair
bool liftComplexMultiplyAdd(SimdLift& s)
{
    if (!s.is("fcmla"))
        return false;
    const SimdShape d = s.shape(0);
    Val n = s.c().read(1);
    const bool isIndexed = isElement(s, 2);
    Val m;
    uint index = 0;
    if (isIndexed)
    {
        // The element is a pair (real, imaginary): the register is read
        // whole
        index = (uint)s.operand(2).m_elementIndex;
        m = s.c().getRegister(s.operand(2).m_register);
        m = s.cast(SimdLift::V(d.m_elementBits, 128 / d.m_elementBits), m);
    } else
        m = s.c().read(2);
    const uint rotation = (uint)(s.operand(3).m_immediate / 90);
    Val acc = s.c().read(0);
    Val result = s.splat(n.getType(), 0);
    for (uint i = 0; i < d.m_lanes; i += 2)
    {
        const uint pair = isIndexed ? 2 * index : i;
        Val m0 = s.lane(m, pair);
        Val m1 = s.lane(m, pair + 1);
        Val n0 = s.lane(n, i);
        Val n1 = s.lane(n, i + 1);
        Val element1, element2, element3, element4;
        switch (rotation)
        {
        case 0:
            element1 = m0; element2 = n0; element3 = m1; element4 = n0;
            break;
        case 1:
            element1 = negate(s, m1); element2 = n1; element3 = m0; element4 = n1;
            break;
        case 2:
            element1 = negate(s, m0); element2 = n0; element3 = negate(s, m1);
            element4 = n0;
            break;
        default:
            element1 = m1; element2 = n1; element3 = negate(s, m0); element4 = n1;
            break;
        }
        result = s.setLane(result, s.fp(Op::OP_FLOAT_MULTIPLY_ADD, element2,
                           element1, s.lane(acc, i)), i);
        result = s.setLane(result, s.fp(Op::OP_FLOAT_MULTIPLY_ADD, element4,
                           element3, s.lane(acc, i + 1)), i + 1);
    }
    s.c().write(0, result);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Moves
/////////////////////////////////////////////////////////////////////////////

// fmov in all its forms
bool liftMove(SimdLift& s)
{
    if (!s.is("fmov"))
        return false;
    ArmLiftContext& c = s.c();
    const ArmDecodedOperand& destination = s.operand(0);
    const ArmDecodedOperand& source = s.operand(1);
    const ArchitectureDescriptionPtr& architecture = c.getArchitecture();

    if (source.m_kind == ArmDecodedOperand::KIND_FLOAT_IMMEDIATE)
    {
        // fmov s0, #1.0 and fmov v0.4s, #1.0 (imm8 is bits 20-13)
        const SimdShape d = s.shape(0);
        // The scalar form has imm8 in bits 20-13, the vector one is abc:defgh
        const uint imm8 = d.m_isVector ? ((s.bits(18, 16) << 5) | s.bits(9, 5))
                                       : s.bits(20, 13);
        const uint bits = d.m_elementBits;
        const uint64 value = expandImmediate(imm8, bits);
        Val scalar = s.imm((int64)value, SimdLift::I(bits));
        if (d.m_isVector)
            c.write(0, s.splat(SimdLift::V(bits, d.m_lanes), scalar));
        else
            c.write(0, scalar);
        return true;
    }
    const bool isDestinationGeneral = architecture->getRegister(
                destination.m_register).m_file ==
                ArchitectureRegister::FILE_GENERAL;
    const bool isSourceGeneral = architecture->getRegister(
                source.m_register).m_file == ArchitectureRegister::FILE_GENERAL;
    if (isDestinationGeneral)
    {
        // fmov w0, s1 / x0, d1 / w0, h1 / x0, h1 / x0, v1.d[1]
        Val value = c.read(1);
        const uint bits = architecture->getRegister(destination.m_register).m_bits;
        c.write(0, s.zext(bits, value));
        return true;
    }
    if (isSourceGeneral)
    {
        // fmov s0, w1 / d0, x1 / h0, w1 / h0, x1 / v0.d[1], x1
        Val value = c.read(1);
        const SimdShape d = s.shape(0);
        c.write(0, s.trunc(d.m_totalBits, value));
        return true;
    }
    // fmov d0, d1: a register copy
    c.write(0, c.read(1));
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Conversions
/////////////////////////////////////////////////////////////////////////////

// fcvt (precision), fcvtl, fcvtl2, fcvtn, fcvtn2, fcvtxn, fcvtxn2
bool liftPrecision(SimdLift& s)
{
    ArmLiftContext& c = s.c();
    if (s.is("fcvt"))
    {
        const SimdShape d = s.shape(0);
        c.write(0, s.fpConvert(c.read(1), d.m_elementBits, Op::ROUND_DYNAMIC));
        return true;
    }
    if (s.is("fcvtl") || s.is("fcvtl2"))
    {
        const SimdShape d = s.shape(0);
        const SimdShape n = s.shape(1);
        Val v = c.read(1);
        if (s.is("fcvtl2"))
            v = s.cast(SimdLift::V(n.m_elementBits, n.m_lanes / 2),
                       s.high64(s.cast(SimdLift::I(128), v)));
        c.write(0, s.fpConvert(v, d.m_elementBits, Op::ROUND_DYNAMIC));
        return true;
    }
    if (((s.is("fcvtn") || s.is("fcvtn2")) && s.count() == 2) ||
        s.is("fcvtxn") || s.is("fcvtxn2"))
    {
        const SimdShape d = s.shape(0);
        const bool isOdd = s.is("fcvtxn") || s.is("fcvtxn2");
        const bool isUpper = s.name()[strlen(s.name()) - 1] == '2';
        if (isOdd && d.m_isScalar)
        {
            // fcvtxn s0, d1
            c.write(0, s.fpConvert(c.read(1), 32, Op::ROUND_TO_ODD));
            return true;
        }
        Val narrowed = s.fpConvert(c.read(1), d.m_elementBits,
                                   isOdd ? Op::ROUND_TO_ODD : Op::ROUND_DYNAMIC);
        if (!isUpper)
        {
            c.write(0, narrowed);
            return true;
        }
        // The upper half of the destination
        Val old = s.readV(s.registerNumber(0));
        Val low = s.low64(old);
        c.write(0, s.join64(low, s.cast(SimdLift::I(64), narrowed)));
        return true;
    }
    return false;
}

// fcvt[nmapz][su] to an integer, scvtf, ucvtf
bool liftToFromInteger(SimdLift& s)
{
    ArmLiftContext& c = s.c();
    const char* name = s.name();
    if (strncmp(name, "fcvt", 4) == 0 && strlen(name) == 6 &&
        (name[4] == 'n' || name[4] == 'm' || name[4] == 'a' || name[4] == 'p' ||
         name[4] == 'z') && (name[5] == 's' || name[5] == 'u'))
    {
        const bool isSigned = name[5] == 's';
        const Op::RoundingMode rounding = roundingOf(name[4]);
        const ArmDecodedOperand& destination = s.operand(0);
        const uint fractionBits = (s.count() > 2) ?
                        (uint)s.operand(2).m_immediate : 0;
        // The size of the integer: the destination register, or the lanes
        uint integerBits;
        const ArchitectureRegister& reg = c.getArchitecture()->getRegister(
                                                    destination.m_register);
        const SimdShape d = s.shape(0);
        if (reg.m_file == ArchitectureRegister::FILE_GENERAL)
            integerBits = reg.m_bits;
        else
            integerBits = d.m_elementBits;
        Val value = c.read(1);
        c.write(0, s.fpToInteger(value, integerBits, isSigned, rounding,
                                 fractionBits));
        return true;
    }
    if (s.is("scvtf") || s.is("ucvtf"))
    {
        const SimdShape d = s.shape(0);
        const uint fractionBits = (s.count() > 2) ?
                        (uint)s.operand(2).m_immediate : 0;
        Val value = c.read(1);
        c.write(0, s.integerToFp(value, d.m_elementBits, s.is("scvtf"),
                                 fractionBits));
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// FEAT_BF16
/////////////////////////////////////////////////////////////////////////////

// bfcvt, bfcvtn, bfcvtn2, bfdot, bfmlalb, bfmlalt, bfmmla
bool liftBFloat(SimdLift& s)
{
    ArmLiftContext& c = s.c();
    if (s.is("bfcvt"))
    {
        c.write(0, s.fpConvertFormat(c.read(1), SemanticType::FLOAT_SINGLE,
                        SemanticType::FLOAT_BFLOAT16, Op::ROUND_DYNAMIC));
        return true;
    }
    if (s.is("bfcvtn") || s.is("bfcvtn2"))
    {
        Val narrowed = s.fpConvertFormat(c.read(1), SemanticType::FLOAT_SINGLE,
                        SemanticType::FLOAT_BFLOAT16, Op::ROUND_DYNAMIC);
        if (s.is("bfcvtn"))
        {
            c.write(0, narrowed);
            return true;
        }
        Val old = s.readV(s.registerNumber(0));
        c.write(0, s.join64(s.low64(old), s.cast(SimdLift::I(64), narrowed)));
        return true;
    }
    if (s.is("bfdot"))
    {
        const SimdShape d = s.shape(0);
        Val n = s.cast(SimdLift::V(32, d.m_lanes), c.read(1));
        Val accumulator = c.read(0);
        Val result = s.splat(accumulator.getType(), 0);
        Val indexed;
        if (isElement(s, 2))
        {
            Val m = s.cast(SimdLift::V(32, 4), c.getRegister(s.operand(2).m_register));
            indexed = s.lane(m, (uint)s.operand(2).m_elementIndex);
        }
        Val m;
        if (!isElement(s, 2))
            m = s.cast(SimdLift::V(32, d.m_lanes), c.read(2));
        for (uint l = 0; l < d.m_lanes; l++)
            result = s.setLane(result, s.bfloatDot(s.lane(accumulator, l),
                        s.lane(n, l), isElement(s, 2) ? indexed : s.lane(m, l)), l);
        c.write(0, result);
        return true;
    }
    if (s.is("bfmlalb") || s.is("bfmlalt"))
    {
        const uint selector = s.is("bfmlalt") ? 1 : 0;
        Val n = s.cast(SimdLift::V(16, 8), c.getRegister(s.operand(1).m_register));
        Val m;
        Val indexed;
        if (isElement(s, 2))
            indexed = c.read(2);
        else
            m = s.cast(SimdLift::V(16, 8), c.getRegister(s.operand(2).m_register));
        Val accumulator = c.read(0);
        Val result = s.splat(accumulator.getType(), 0);
        for (uint e = 0; e < 4; e++)
            result = s.setLane(result, s.fpMultiplyAddWidening(
                        s.lane(n, 2 * e + selector),
                        isElement(s, 2) ? indexed : s.lane(m, 2 * e + selector),
                        s.lane(accumulator, e), SemanticType::FLOAT_BFLOAT16, 32), e);
        c.write(0, result);
        return true;
    }
    if (s.is("bfmmla"))
    {
        Val n = s.cast(SimdLift::V(32, 4), c.read(1));
        Val m = s.cast(SimdLift::V(32, 4), c.read(2));
        Val accumulator = c.read(0);
        Val result = s.splat(accumulator.getType(), 0);
        for (uint i = 0; i < 2; i++)
            for (uint j = 0; j < 2; j++)
            {
                Val step = s.bfloatDot(s.lane(accumulator, 2 * i + j),
                                       s.lane(n, 2 * i), s.lane(m, 2 * j));
                step = s.bfloatDot(step, s.lane(n, 2 * i + 1), s.lane(m, 2 * j + 1));
                result = s.setLane(result, step, 2 * i + j);
            }
        c.write(0, result);
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// FEAT_FHM, FEAT_JSCVT
/////////////////////////////////////////////////////////////////////////////

// fmlal, fmlal2, fmlsl, fmlsl2: the half precision products added to single
// precision lanes with one rounding, from the low or the high half elements
bool liftHalfFused(SimdLift& s)
{
    if (!s.is("fmlal") && !s.is("fmlal2") && !s.is("fmlsl") && !s.is("fmlsl2"))
        return false;
    ArmLiftContext& c = s.c();
    const bool isSubtract = s.name()[3] == 's';
    const bool isHigh = s.name()[strlen(s.name()) - 1] == '2';
    const SimdShape d = s.shape(0);
    const uint elements = d.m_lanes;
    const uint offset = isHigh ? elements : 0;
    Val n = s.cast(SimdLift::V(16, 8), c.getRegister(s.operand(1).m_register));
    Val m;
    if (isElement(s, 2))
        m = c.read(2);
    else
        m = s.cast(SimdLift::V(16, 8), c.getRegister(s.operand(2).m_register));
    Val accumulator = c.read(0);
    Val result = s.splat(accumulator.getType(), 0);
    for (uint e = 0; e < elements; e++)
    {
        Val first = s.lane(n, e + offset);
        if (isSubtract)
            first = negate(s, first);
        Val second = isElement(s, 2) ? m : s.lane(m, e + offset);
        result = s.setLane(result, s.fpMultiplyAddWidening(first, second,
                    s.lane(accumulator, e), SemanticType::FLOAT_HALF, 32), e);
    }
    c.write(0, result);
    return true;
}

// A 1 bit: the comparison of two 64 bit values
Val is(SimdLift& s, Op::Condition condition, const Val& a, const Val& b)
{
    return s.compare(condition, SimdLift::I(64), a, b);
}

Val andBits(SimdLift& s, const Val& a, const Val& b)
{
    return s.op(Op::OP_AND, SimdLift::I(1), a, b);
}

Val orBits(SimdLift& s, const Val& a, const Val& b)
{
    return s.op(Op::OP_OR, SimdLift::I(1), a, b);
}

Val notBit(SimdLift& s, const Val& a)
{
    return s.op(Op::OP_NOT, SimdLift::I(1), a);
}

// fjcvtzs w0, d1: FPToFixedJS. The number is converted to an integer
// toward zero, modulo 2^32; Z is set when the conversion was exact
bool liftJavascriptConvert(SimdLift& s)
{
    if (!s.is("fjcvtzs"))
        return false;
    ArmLiftContext& c = s.c();
    const SemanticType word = SimdLift::I(64);
    Val value = c.read(1);
    Val sign = s.trunc(1, s.op(Op::OP_SHIFT_RIGHT_LOGICAL, word, value, s.imm(63, word)));
    Val exponent = s.op(Op::OP_AND, word, s.op(Op::OP_SHIFT_RIGHT_LOGICAL, word, value,
                                               s.imm(52, word)), s.imm(0x7FF, word));
    Val fraction = s.op(Op::OP_AND, word, value, s.imm((int64)0xFFFFFFFFFFFFFULL, word));
    Val flushEnabled = s.trunc(1, s.op(Op::OP_SHIFT_RIGHT_LOGICAL, SimdLift::I(32),
                                       c.getRegister(ARM64_REG_FPCR), s.imm(24, SimdLift::I(32))));
    Val zero = s.imm(0, word);

    Val isSpecial = is(s, Op::COND_EQUAL, exponent, s.imm(0x7FF, word));
    Val isZeroExponent = is(s, Op::COND_EQUAL, exponent, zero);
    Val hasFraction = is(s, Op::COND_NOT_EQUAL, fraction, zero);
    Val isDenormal = andBits(s, isZeroExponent, hasFraction);
    Val isFlushed = andBits(s, isDenormal, flushEnabled);
    Val isUnflushedDenormal = andBits(s, isDenormal, notBit(s, flushEnabled));
    Val isZero = orBits(s, andBits(s, isZeroExponent, notBit(s, hasFraction)), isFlushed);

    // The exponent E = e - 1023 as a signed number
    Val unbiased = s.op(Op::OP_SUBTRACT, word, exponent, s.imm(1023, word));
    Val isNegativeExponent = is(s, Op::COND_SIGNED_LESS, unbiased, zero);
    Val isFractionalRange = is(s, Op::COND_SIGNED_LESS_EQUAL, unbiased, s.imm(52, word));
    Val mantissa = s.op(Op::OP_OR, word, fraction, s.imm((int64)1 << 52, word));
    Val rightShift = s.op(Op::OP_SUBTRACT, word, s.imm(52, word), unbiased);
    Val leftShift = s.op(Op::OP_SUBTRACT, word, unbiased, s.imm(52, word));
    Val rightValue = s.op(Op::OP_SHIFT_RIGHT_LOGICAL, word, mantissa, rightShift);
    Val leftValue = s.op(Op::OP_SHIFT_LEFT, word, mantissa, leftShift);
    Val magnitude = s.select(isNegativeExponent, zero,
                             s.select(isFractionalRange, rightValue, leftValue));
    // The bits below the units
    Val lostMask = s.op(Op::OP_SUBTRACT, word,
                        s.op(Op::OP_SHIFT_LEFT, word, s.imm(1, word), rightShift),
                        s.imm(1, word));
    Val lost = s.op(Op::OP_AND, word, mantissa, lostMask);
    Val isInexactNormal = s.select(isNegativeExponent, s.imm(1, SimdLift::I(1)),
                                   s.select(isFractionalRange,
                                            is(s, Op::COND_NOT_EQUAL, lost, zero),
                                            s.imm(0, SimdLift::I(1))));
    Val isNormal = notBit(s, orBits(s, isSpecial, orBits(s, isZeroExponent, s.imm(0, SimdLift::I(1)))));
    Val isInexact = orBits(s, andBits(s, isNormal, isInexactNormal), isUnflushedDenormal);
    // The range of the 32 bit result
    Val isSmallEnough = is(s, Op::COND_SIGNED_LESS_EQUAL, unbiased, s.imm(62, word));
    Val positiveFits = is(s, Op::COND_UNSIGNED_LESS, magnitude, s.imm((int64)1 << 31, word));
    Val negativeFits = is(s, Op::COND_UNSIGNED_LESS_EQUAL, magnitude, s.imm((int64)1 << 31, word));
    Val fits = andBits(s, isSmallEnough,
                       s.select(sign, negativeFits, positiveFits));
    Val isOutOfRange = andBits(s, isNormal, notBit(s, fits));
    Val isInvalid = orBits(s, isSpecial, isOutOfRange);
    Val isInexactError = andBits(s, isInexact, notBit(s, isInvalid));

    // The result: the low 32 bits of the number, negated for a negative one
    Val low = s.trunc(32, magnitude);
    Val negated = s.op(Op::OP_NEGATE, SimdLift::I(32), low);
    Val result = s.select(sign, negated, low);
    result = s.select(isSpecial, s.imm(0, SimdLift::I(32)), result);
    c.write(0, result);

    s.setFpsrBit(0, isInvalid);
    s.setFpsrBit(4, isInexactError);
    s.setFpsrBit(7, isFlushed);
    // A denormal flushed to zero is not an exact conversion either
    Val zeroFlag = notBit(s, orBits(s, isFlushed, orBits(s, isInvalid,
                                    orBits(s, isInexactError, andBits(s, isZero, sign)))));
    const SemanticType flag = SimdLift::I(1);
    c.move(c.getRegister(ARM64_REG_N), s.imm(0, flag));
    c.move(c.getRegister(ARM64_REG_Z), zeroFlag);
    c.move(c.getRegister(ARM64_REG_C), s.imm(0, flag));
    c.move(c.getRegister(ARM64_REG_V), s.imm(0, flag));
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// FEAT_FAMINMAX, FEAT_FP8 (fscale), FEAT_LUT
/////////////////////////////////////////////////////////////////////////////

// famax, famin: the maximum / minimum of the absolute values (FPMax / FPMin of
// FPAbs of the elements: a NaN follows fmax / fmin)
bool liftAbsoluteMinMax(SimdLift& s)
{
    if (!s.is("famax") && !s.is("famin"))
        return false;
    Val n = s.fp(Op::OP_FLOAT_ABSOLUTE, s.c().read(1));
    Val m = s.fp(Op::OP_FLOAT_ABSOLUTE, s.c().read(2));
    s.c().write(0, s.fp(s.is("famax") ? Op::OP_FLOAT_MAXIMUM : Op::OP_FLOAT_MINIMUM,
                        n, m, Val(), Op::BEHAVIOR_MINMAX_PROPAGATE_NAN));
    return true;
}

// fscale: a * 2^b where b is the signed integer lane of the second operand
bool liftScale(SimdLift& s)
{
    if (!s.is("fscale"))
        return false;
    s.c().write(0, s.fp(Op::OP_FLOAT_SCALE, s.c().read(1), s.c().read(2), Val(),
                        Op::BEHAVIOR_SCALE_INTEGER));
    return true;
}

// The element 'index' of 'size' bits of a table held in 64 bit words, the
// index being a run time value (a 32 bit scalar)
Val tableElement(SimdLift& s, cArray<Val>& words, const Val& index, uint size)
{
    const SemanticType word = SimdLift::I(64);
    const uint perWord = 64 / size;
    Val wide = s.zext(64, index);
    Val wordIndex = s.op(Op::OP_SHIFT_RIGHT_LOGICAL, word, wide,
                         s.imm(perWord == 32 ? 5 : (perWord == 16 ? 4 : (perWord == 8 ? 3 : 2)), word));
    Val inside = s.op(Op::OP_AND, word, wide, s.imm(perWord - 1, word));
    // The word: a chain of selects on the word index
    Val selected = words[0];
    for (uint i = 1; i < words.getSize(); i++)
        selected = s.select(s.compare(Op::COND_EQUAL, word, wordIndex, s.imm(i, word)),
                            words[i], selected);
    Val shifted = s.op(Op::OP_SHIFT_RIGHT_LOGICAL, word, selected,
                       s.op(Op::OP_MULTIPLY, word, inside, s.imm(size, word)));
    return s.trunc(size, shifted);
}

// luti2, luti4: a table lookup of the 2 or 4 bit indices of a segment of the
// second register; the table is the register list
bool liftTableLookup(SimdLift& s)
{
    if (!s.is("luti2") && !s.is("luti4"))
        return false;
    ArmLiftContext& c = s.c();
    const uint indexBits = s.is("luti2") ? 2 : 4;
    const SimdShape d = s.shape(0);
    const uint size = d.m_elementBits;
    const uint elements = d.m_lanes;
    const uint segment = (uint)s.operand(2).m_elementIndex;
    const ArmDecodedOperand& list = s.operand(1);
    // The table: one register, or two for the 4 bit indices of halfwords
    cArray<Val> words(0);
    const uint registers = (list.m_count != 0) ? list.m_count : 1;
    const uint first = s.registerNumberOf(list.m_register);
    for (uint r = 0; r < registers; r++)
    {
        Val table = s.readV((first + r) & 31);
        words.append(s.low64(table));
        words.append(s.high64(table));
    }
    Val indexes = c.getRegister(s.operand(2).m_register);
    Val result = s.splat(SimdLift::V(size, elements), 0);
    for (uint e = 0; e < elements; e++)
    {
        Val index = s.op(Op::OP_EXTRACT, SimdLift::I(indexBits), indexes,
                         s.imm(segment * elements * indexBits + e * indexBits,
                               SimdLift::I(32)));
        result = s.setLane(result, tableElement(s, words, s.zext(32, index), size), e);
    }
    c.write(0, result);
    return true;
}

} // namespace

bool armdis::liftSimdFloat(SimdLift& s)
{
    return liftBinary(s) || liftFused(s) || liftUnary(s) ||
           liftVectorCompare(s) || liftScalarCompare(s) || liftAcross(s) ||
           liftComplexAdd(s) || liftComplexMultiplyAdd(s) || liftMove(s) ||
           liftPrecision(s) || liftToFromInteger(s) || liftHalfFused(s) ||
           liftJavascriptConvert(s) || liftBFloat(s) || liftAbsoluteMinMax(s) ||
           liftScale(s) || liftTableLookup(s);
}
