/*
 * RiscvSemanticLiftVectorFloat.cpp
 *
 * Implementation file
 *
 * The vector floating point instructions (OPFVV and OPFVF: Zve32f, Zve64f,
 * Zve64d, Zvfh, Zvfhmin and the BFloat16 ones of Zvfbfmin and Zvfbfwma): the
 * arithmetic, the multiply-add family, the widening and narrowing forms, the
 * square root and the estimates, the minimum and maximum, the sign
 * injection, the class, the compares into masks, vfmerge / vfmv.v.f, the
 * conversions and the reductions. The moves between the f registers and the
 * vector (vfmv.f.s, vfmv.s.f) and vfslide1up / vfslide1down are in
 * RiscvSemanticLiftVectorPermute.cpp.
 *
 * Each is a RvvPlan (RiscvSemanticVector.h) and an element operation on the
 * lane vectors of a chunk. A lane which is not active (masked off, tail or
 * prestart) takes a value that raises nothing before the operation, so the
 * flags of fflags come only from the active elements. The rounding mode is
 * frm (a reserved value is an illegal instruction), except for the
 * conversions which round toward zero or to odd.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"
#include "dismount/proc/riscv/RiscvSemanticVector.h"

namespace riscvdis {

typedef SemanticOperation Op;

namespace {

// The format of a floating point lane
SemanticType::FloatFormat formatOf(uint bits, bool isBfloat)
{
    if (isBfloat)
        return SemanticType::FLOAT_BFLOAT16;
    if (bits == 16)
        return SemanticType::FLOAT_HALF;
    if (bits == 32)
        return SemanticType::FLOAT_SINGLE;
    CHECK(bits == 64);
    return SemanticType::FLOAT_DOUBLE;
}

SemanticType floatLanes(uint bits, uint lanes, bool isBfloat = false)
{
    return SemanticType::vector(SemanticType::floating(formatOf(bits, isBfloat)),
                                lanes);
}

// The bits of 1.0 in a lane (the neutral divisor and the neutral operand of
// the estimates)
int64 oneOf(uint bits, bool isBfloat)
{
    if (bits == 16)
        return isBfloat ? 0x3F80 : 0x3C00;
    if (bits == 32)
        return 0x3F800000;
    return 0x3FF0000000000000LL;
}

// The scalar operand of the .vf forms: the NaN boxed f register
SemanticOperand readBoxedScalar(RvvLift& lift, uint bits)
{
    uint64 canonical = 0x7FF8000000000000ULL;
    if (bits == 16)
        canonical = 0x7E00;
    else if (bits == 32)
        canonical = 0x7FC00000;
    return lift.readFloatScalar(bits, canonical);
}

// (the 16 bit scalar of the BFloat16 instructions reads a bad box as the
// canonical NaN of BFloat16)
SemanticOperand readBoxedBfloatScalar(RvvLift& lift, uint bits)
{
    CHECK(bits == 16);
    return lift.readFloatScalar(16, 0x7FC0);
}

// Add a floating point operation which raises exceptions
SemanticOperand emitFloat(RvvChunk& c, Op::Operation operation,
                          const SemanticType& type, Op::RoundingMode rounding,
                          Op::Behavior behavior, const SemanticOperand& a,
                          const SemanticOperand& b = SemanticOperand(),
                          const SemanticOperand& d = SemanticOperand())
{
    RiscvLiftContext& context = c.m_lift.context();
    SemanticOperand result = context.newTemporary(type);
    SemanticInstruction instruction(operation, type, result, a, b, d);
    instruction.setRoundingMode(rounding);
    instruction.setBehavior(behavior);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    context.emit(instruction);
    return result;
}

// The lanes as floats of the type
SemanticOperand asFloat(RvvChunk& c, const SemanticOperand& lanes,
                        const SemanticType& type)
{
    return c.m_lift.bitcast(lanes, type);
}

// The exact conversion of narrow lanes to the wide float type
SemanticOperand widenLanes(RvvChunk& c, const SemanticOperand& narrow,
                           const SemanticType& wide)
{
    return emitFloat(c, Op::OP_FLOAT_CONVERT, wide, Op::ROUND_NEAREST_EVEN,
                     Op::BEHAVIOR_DEFAULT, narrow);
}

// The flags of the instruction
uint flagsOf(const RvvChunk& c)
{
    return (uint)c.m_plan.m_argument;
}

bool isWidening(const RvvPlan& plan)
{
    return (plan.m_shape == RvvPlan::SHAPE_WIDEN) ||
           (plan.m_shape == RvvPlan::SHAPE_WIDEN_W);
}

// The conversions
SemanticOperand convertLanes(RvvChunk& c)
{
    RvvLift& l = c.m_lift;
    const RvvPlan& plan = c.m_plan;
    const uint flags = flagsOf(c);
    const bool isBfloat = (flags & RVF_FLAG_BFLOAT16) != 0;
    const bool isNarrow = plan.m_shape == RvvPlan::SHAPE_NARROW;
    const uint sourceBits = isWidening(plan) ? c.m_bits / 2 : c.m_bits;
    const uint destinationBits = isNarrow ? c.m_bits / 2 : c.m_bits;
    Op::RoundingMode rounding = Op::ROUND_DYNAMIC;
    if ((flags & RVF_FLAG_RTZ) != 0)
        rounding = Op::ROUND_TOWARD_ZERO;
    else if ((flags & RVF_FLAG_ROD) != 0)
        rounding = Op::ROUND_TO_ODD;
    const uint lanes = c.m_lanes;
    SemanticOperand result;
    switch (plan.m_operation)
    {
    case RVF_OP_TO_INTEGER:
    {
        const SemanticType source = floatLanes(sourceBits, lanes);
        const SemanticType destination = l.vectorType(destinationBits, lanes);
        result = emitFloat(c, ((flags & RVF_FLAG_UNSIGNED) != 0) ?
                           Op::OP_FLOAT_TO_UNSIGNED : Op::OP_FLOAT_TO_SIGNED,
                           destination, rounding,
                           Op::BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM,
                           asFloat(c, c.m_a, source));
        break;
    }
    case RVF_OP_FROM_INTEGER:
    {
        const SemanticType destination = floatLanes(destinationBits, lanes);
        result = emitFloat(c, ((flags & RVF_FLAG_UNSIGNED) != 0) ?
                           Op::OP_UNSIGNED_TO_FLOAT : Op::OP_SIGNED_TO_FLOAT,
                           destination, rounding, Op::BEHAVIOR_DEFAULT, c.m_a);
        break;
    }
    default:
    {
        // (the BFloat16 side is the narrow one)
        const SemanticType source = floatLanes(sourceBits, lanes,
                                       isBfloat && isWidening(plan));
        const SemanticType destination = floatLanes(destinationBits, lanes,
                                       isBfloat && isNarrow);
        result = emitFloat(c, Op::OP_FLOAT_CONVERT, destination, rounding,
                           Op::BEHAVIOR_DEFAULT, asFloat(c, c.m_a, source));
        break;
    }
    }
    // A narrowing result has narrow lanes: the engine truncates the compute
    // lanes
    result = l.bitcast(result, l.vectorType(destinationBits, lanes));
    if (isNarrow)
        result = l.extendLanes(result, c.m_bits, false);
    return result;
}

// -value (a sign flip, no exceptions)
SemanticOperand emitNegate(RvvChunk& c, const SemanticOperand& value,
                           const SemanticType& type)
{
    return c.m_lift.emit(Op::OP_FLOAT_NEGATE, type, value);
}

// The compares into a mask
SemanticOperand compareLanes(RvvChunk& c, SemanticOperation::Condition condition,
                             SemanticOperation::Behavior behavior,
                             const SemanticOperand& x, const SemanticOperand& y)
{
    RiscvLiftContext& context = c.m_lift.context();
    const SemanticType type = floatLanes(c.m_bits, c.m_lanes);
    SemanticOperand result = context.newTemporary(c.m_maskType);
    SemanticInstruction instruction(Op::OP_COMPARE, type, result, x, y);
    instruction.setCondition(condition);
    instruction.setBehavior(behavior);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    context.emit(instruction);
    return result;
}

// The sign injection on the bits of the lanes
SemanticOperand injectSign(RvvChunk& c, uint operation)
{
    RvvLift& l = c.m_lift;
    const SemanticType t = c.m_vectorType;
    const int64 sign = (int64)((uint64)1 << (c.m_bits - 1));
    SemanticOperand signMask = l.splat(sign, t);
    SemanticOperand signOfB = l.emit(Op::OP_AND, t, c.m_b, signMask);
    switch (operation)
    {
    case RVF_OP_SGNJ:
        return l.emit(Op::OP_OR, t, l.emit(Op::OP_AND_NOT, t, signMask, c.m_a),
                      signOfB);
    case RVF_OP_SGNJN:
        return l.emit(Op::OP_OR, t, l.emit(Op::OP_AND_NOT, t, signMask, c.m_a),
                      l.emit(Op::OP_XOR, t, signOfB, signMask));
    default:
        return l.emit(Op::OP_XOR, t, c.m_a, signOfB);
    }
}

} // namespace

SemanticOperand floatElementOp(RvvChunk& c)
{
    RvvLift& l = c.m_lift;
    const RvvPlan& plan = c.m_plan;
    const uint operation = plan.m_operation;
    const uint flags = flagsOf(c);
    const bool isBfloat = (flags & RVF_FLAG_BFLOAT16) != 0;
    const uint bits = c.m_bits;

    switch (operation)
    {
    case RVF_OP_MERGE:
        return l.select(c.m_carry, c.m_b, c.m_a);
    case RVF_OP_MOVE:
        return c.m_b;
    case RVF_OP_SGNJ:
    case RVF_OP_SGNJN:
    case RVF_OP_SGNJX:
        return injectSign(c, operation);
    case RVF_OP_TO_INTEGER:
    case RVF_OP_FROM_INTEGER:
    case RVF_OP_CONVERT:
        // (the inactive lanes are 0: no flags)
        c.m_a = c.sanitize(c.m_a, 0);
        return convertLanes(c);
    case RVF_OP_CLASS:
    {
        const SemanticType type = floatLanes(bits, c.m_lanes);
        SemanticOperand result = l.context().newTemporary(c.m_vectorType);
        l.context().emit(SemanticInstruction(Op::OP_FLOAT_CLASSIFY,
                c.m_vectorType, result, asFloat(c, c.m_a, type)));
        return result;
    }
    default:
        break;
    }

    // The inactive lanes take values which raise nothing: 0, and 1.0 for the
    // divisor and for the operand of the estimates
    const bool isWiden = isWidening(plan);
    const uint narrowBits = isWiden ? bits / 2 : bits;
    SemanticOperand a = c.m_a;
    SemanticOperand b = c.m_b;
    SemanticOperand old = c.m_old;
    const bool isEstimate = (operation == RVF_OP_REC7) ||
                            (operation == RVF_OP_RSQRT7);
    a = c.sanitize(a, (isEstimate || (operation == RVF_OP_RDIV)) ?
                      oneOf(bits, false) : 0);
    if (b.isValid())
        b = c.sanitize(b, (operation == RVF_OP_DIV) ? oneOf(bits, false) : 0);
    if (old.isValid())
        old = c.sanitize(old, 0);

    const SemanticType type = floatLanes(bits, c.m_lanes);
    const SemanticType narrowType = floatLanes(narrowBits, c.m_lanes, isBfloat);
    SemanticOperand x;
    SemanticOperand y;
    if (plan.m_shape == RvvPlan::SHAPE_WIDEN)
    {
        x = widenLanes(c, asFloat(c, a, narrowType), type);
        if (b.isValid())
            y = widenLanes(c, asFloat(c, b, narrowType), type);
    } else if (plan.m_shape == RvvPlan::SHAPE_WIDEN_W)
    {
        x = asFloat(c, a, type);
        y = widenLanes(c, asFloat(c, b, narrowType), type);
    } else
    {
        x = asFloat(c, a, type);
        if (b.isValid())
            y = asFloat(c, b, type);
    }
    SemanticOperand z;
    if (old.isValid())
        z = asFloat(c, old, type);

    SemanticOperand result;
    const Op::RoundingMode dynamic = Op::ROUND_DYNAMIC;
    switch (operation)
    {
    case RVF_OP_ADD:
        result = emitFloat(c, Op::OP_FLOAT_ADD, type, dynamic,
                           Op::BEHAVIOR_DEFAULT, x, y);
        break;
    case RVF_OP_SUB:
        result = emitFloat(c, Op::OP_FLOAT_SUBTRACT, type, dynamic,
                           Op::BEHAVIOR_DEFAULT, x, y);
        break;
    case RVF_OP_RSUB:
        result = emitFloat(c, Op::OP_FLOAT_SUBTRACT, type, dynamic,
                           Op::BEHAVIOR_DEFAULT, y, x);
        break;
    case RVF_OP_MUL:
        result = emitFloat(c, Op::OP_FLOAT_MULTIPLY, type, dynamic,
                           Op::BEHAVIOR_DEFAULT, x, y);
        break;
    case RVF_OP_DIV:
        result = emitFloat(c, Op::OP_FLOAT_DIVIDE, type, dynamic,
                           Op::BEHAVIOR_DEFAULT, x, y);
        break;
    case RVF_OP_RDIV:
        result = emitFloat(c, Op::OP_FLOAT_DIVIDE, type, dynamic,
                           Op::BEHAVIOR_DEFAULT, y, x);
        break;
    case RVF_OP_MIN:
        result = emitFloat(c, Op::OP_FLOAT_MINIMUM, type, dynamic,
                           Op::BEHAVIOR_MINMAX_NUMBER, x, y);
        break;
    case RVF_OP_MAX:
        result = emitFloat(c, Op::OP_FLOAT_MAXIMUM, type, dynamic,
                           Op::BEHAVIOR_MINMAX_NUMBER, x, y);
        break;
    case RVF_OP_MACC:
    {
        // vfmacc: vs1 * vs2 + vd, vfmadd: vs1 * vd + vs2; the negations of
        // the product and of the addend make the other six
        SemanticOperand multiplicand = x;
        SemanticOperand addend = z;
        if ((flags & RVF_FLAG_MULTIPLY_DESTINATION) != 0)
        {
            multiplicand = z;
            addend = x;
        }
        if ((flags & RVF_FLAG_NEGATE_PRODUCT) != 0)
            multiplicand = emitNegate(c, multiplicand, type);
        if ((flags & RVF_FLAG_NEGATE_ADDEND) != 0)
            addend = emitNegate(c, addend, type);
        result = emitFloat(c, Op::OP_FLOAT_MULTIPLY_ADD, type, dynamic,
                           Op::BEHAVIOR_DEFAULT, multiplicand, y, addend);
        break;
    }
    case RVF_OP_SQRT:
        result = emitFloat(c, Op::OP_FLOAT_SQUARE_ROOT, type, dynamic,
                           Op::BEHAVIOR_DEFAULT, x);
        break;
    case RVF_OP_REC7:
        result = emitFloat(c, Op::OP_FLOAT_RECIPROCAL_ESTIMATE, type, dynamic,
                           Op::BEHAVIOR_ESTIMATE_RISCV, x);
        break;
    case RVF_OP_RSQRT7:
        result = emitFloat(c, Op::OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE, type,
                           dynamic, Op::BEHAVIOR_ESTIMATE_RISCV, x);
        break;
    case RVF_OP_EQ:
        return compareLanes(c, Op::COND_FLOAT_EQUAL, Op::BEHAVIOR_COMPARE_QUIET,
                            x, y);
    case RVF_OP_NE:
        return compareLanes(c, Op::COND_FLOAT_NOT_EQUAL,
                            Op::BEHAVIOR_COMPARE_QUIET, x, y);
    case RVF_OP_LT:
        return compareLanes(c, Op::COND_FLOAT_LESS,
                            Op::BEHAVIOR_COMPARE_SIGNALING, x, y);
    case RVF_OP_LE:
        return compareLanes(c, Op::COND_FLOAT_LESS_EQUAL,
                            Op::BEHAVIOR_COMPARE_SIGNALING, x, y);
    case RVF_OP_GT:
        return compareLanes(c, Op::COND_FLOAT_GREATER,
                            Op::BEHAVIOR_COMPARE_SIGNALING, x, y);
    case RVF_OP_GE:
        return compareLanes(c, Op::COND_FLOAT_GREATER_EQUAL,
                            Op::BEHAVIOR_COMPARE_SIGNALING, x, y);
    default:
        CHECK_FAIL();
    }
    return l.bitcast(result, c.m_vectorType);
}
namespace {

// The plan of an entry from its parameter (RVF_PARAM())
RvvPlan makeFloatPlan(uint64 parameter)
{
    RvvPlan plan;
    plan.m_operation = (uint)(parameter & 0xFF);
    plan.m_shape = (RvvPlan::Shape)((parameter >> 8) & 0xF);
    plan.m_mask = (RvvPlan::MaskMode)((parameter >> 12) & 3);
    plan.m_argument = (int64)((parameter >> 16) & 0xFF);
    const uint flags = (uint)plan.m_argument;
    const bool isBfloat = (flags & RVF_FLAG_BFLOAT16) != 0;
    // The narrow operands of the widening and narrowing instructions reach
    // the element operation as they are
    const bool isReshaped = (plan.m_shape == RvvPlan::SHAPE_WIDEN) ||
                            (plan.m_shape == RvvPlan::SHAPE_WIDEN_W) ||
                            (plan.m_shape == RvvPlan::SHAPE_NARROW);
    plan.m_extendA = isReshaped ? RvvPlan::EXTEND_NONE : RvvPlan::EXTEND_ZERO;
    plan.m_extendB = isReshaped ? RvvPlan::EXTEND_NONE : RvvPlan::EXTEND_ZERO;
    switch (plan.m_operation)
    {
    case RVF_OP_SQRT:
    case RVF_OP_REC7:
    case RVF_OP_RSQRT7:
    case RVF_OP_CLASS:
    case RVF_OP_TO_INTEGER:
    case RVF_OP_FROM_INTEGER:
    case RVF_OP_CONVERT:
        plan.m_hasNoB = true;
        break;
    case RVF_OP_MACC:
        plan.m_readsDestination = true;
        break;
    default:
        break;
    }
    plan.m_readScalar = isBfloat ? readBoxedBfloatScalar : readBoxedScalar;
    plan.m_op = floatElementOp;
    return plan;
}

// true if the instruction exists for the element width SEW: the floating
// point formats are 16 (Zvfh), 32 and 64 bits wide, the integers of the
// conversions 8 to 64
bool isSewLegal(const RvvPlan& plan, uint sew)
{
    const uint flags = (uint)plan.m_argument;
    if ((flags & RVF_FLAG_BFLOAT16) != 0)
        return sew == 16;
    const bool isFloatSide = (sew == 16) || (sew == 32) || (sew == 64);
    switch (plan.m_operation)
    {
    case RVF_OP_TO_INTEGER:
    case RVF_OP_FROM_INTEGER:
    {
        // The float side is SEW, 2 * SEW (narrowing source, widening
        // result)
        const bool isNarrow = plan.m_shape == RvvPlan::SHAPE_NARROW;
        const bool isWiden = plan.m_shape == RvvPlan::SHAPE_WIDEN;
        const bool isFloatWide = (plan.m_operation == RVF_OP_TO_INTEGER) ?
                                 isNarrow : isWiden;
        const uint floatBits = isFloatWide ? sew * 2 : sew;
        return (floatBits == 16) || (floatBits == 32) || (floatBits == 64);
    }
    default:
        return isFloatSide;
    }
}

void floatBody(RvvLift& lift, const RvvType& type)
{
    RvvPlan plan = makeFloatPlan(lift.context().getParameter());
    if (!isSewLegal(plan, type.m_sew))
    {
        lift.trapIllegal();
        return;
    }
    // The dynamic rounding mode: a reserved frm is an illegal instruction
    if ((plan.m_argument & RVF_FLAG_NO_FRM) == 0)
        lift.context().getRoundingMode(0);
    lift.elementwise(plan, type);
}

} // namespace

void liftVectorFloat(RiscvLiftContext& context)
{
    RvvLift lift(context);
    lift.dispatch(floatBody);
    lift.finish();
}

/////////////////////////////////////////////////////////////////////////////
// The reductions
/////////////////////////////////////////////////////////////////////////////

namespace {

void reduceBody(RvvLift& lift, const RvvType& type)
{
    const uint64 parameter = lift.context().getParameter();
    const uint kind = (uint)(parameter & 0xFF);
    const bool isWide = ((parameter >> 8) & 1) != 0;
    const uint sew = type.m_sew;
    const uint bits = isWide ? sew * 2 : sew;
    const SemanticType x = lift.xlenType();
    // The vector reductions need vstart = 0
    lift.trapIllegalIf(lift.compare(Op::COND_NOT_EQUAL, x, lift.vstart(),
                                    lift.immX(0)));
    const RvvGroup s = RvvLift::makeGroupLmul(type, lift.vs2(), sew,
                                              type.m_lmulLog2);
    const bool isSewLegal = (sew == 16) || (sew == 32) || (sew == 64);
    if (!RvvLift::isGroupLegal(s) || !isSewLegal || (bits > 64))
    {
        lift.trapIllegal();
        return;
    }
    // (every vector floating point instruction checks frm)
    lift.context().getRoundingMode(0);

    RiscvLiftContext& context = lift.context();
    const SemanticType narrowType = SemanticType::floating(formatOf(sew, false));
    const SemanticType wideType = SemanticType::floating(formatOf(bits, false));
    const SemanticType resultType = lift.integerType(bits);
    // The accumulator: vs1[0]
    SemanticOperand accumulator = context.newTemporary(wideType);
    lift.move(accumulator, lift.bitcast(lift.readElement(lift.vs1(), bits,
                                                         lift.immX(0)), wideType));
    SemanticOperand skip = lift.newLabel();
    SemanticOperand i = lift.beginLoop(lift.vl());
    if (lift.isMasked())
        lift.skipElementIf(lift.compare(Op::COND_EQUAL, lift.integerType(1),
                lift.maskBit(i), lift.imm(0, lift.integerType(1))));
    SemanticOperand element = lift.bitcast(lift.readElement(lift.vs2(), sew, i),
                                           narrowType);
    if (isWide)
    {
        SemanticOperand converted = context.newTemporary(wideType);
        SemanticInstruction convert(Op::OP_FLOAT_CONVERT, wideType, converted,
                                    element);
        convert.setRoundingMode(Op::ROUND_NEAREST_EVEN);
        convert.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
        context.emit(convert);
        element = converted;
    }
    Op::Operation combine = Op::OP_FLOAT_ADD;
    Op::Behavior behavior = Op::BEHAVIOR_DEFAULT;
    if (kind == RVF_REDUCE_MIN)
    {
        combine = Op::OP_FLOAT_MINIMUM;
        behavior = Op::BEHAVIOR_MINMAX_NUMBER;
    } else if (kind == RVF_REDUCE_MAX)
    {
        combine = Op::OP_FLOAT_MAXIMUM;
        behavior = Op::BEHAVIOR_MINMAX_NUMBER;
    }
    SemanticOperand combined = context.newTemporary(wideType);
    SemanticInstruction instruction(combine, wideType, combined, accumulator,
                                    element);
    instruction.setRoundingMode(Op::ROUND_DYNAMIC);
    instruction.setBehavior(behavior);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    context.emit(instruction);
    lift.move(accumulator, combined);
    lift.endLoop();
    // vd[0] = the result, if vl > 0
    lift.branchIf(lift.compare(Op::COND_EQUAL, x, lift.vl(), lift.immX(0)), skip);
    SemanticOperand reg = lift.readRegister(lift.vd());
    lift.writeRegister(lift.vd(), lift.insertBits(reg,
            lift.bitcast(accumulator, resultType), lift.immX(0)));
    lift.placeLabel(skip);
}

} // namespace

void liftVectorFloatReduce(RiscvLiftContext& context)
{
    RvvLift lift(context);
    lift.dispatch(reduceBody);
    lift.finish();
}

} // namespace riscvdis
