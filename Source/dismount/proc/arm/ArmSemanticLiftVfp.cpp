/*
 * ArmSemanticLiftVfp.cpp
 *
 * Implementation file
 *
 * The lifters of the AArch32 floating point instructions (VFP, and the
 * floating point data types of Advanced SIMD), see ArmSemanticTableVfp.inc.
 *
 * The semantics are the ones of the Arm ARM pseudocode:
 *   - The VFP instructions (the s registers, and the d registers with f64)
 *     use the FPSCR as it is: rounding mode, flush to zero, default NaN, and
 *     they accumulate the cumulative exception flags in it.
 *   - The Advanced SIMD instructions (the d and q registers with f32 / f16)
 *     use the "standard FPSCR value" (round to nearest, FZ, DN set, FZ16
 *     clear): ATTRIBUTE_FLOAT_STANDARD, and accumulate the flags too.
 *   - The reciprocal steps (vrecps, vrsqrts) are OP_FLOAT_MULTIPLY_ADD with
 *     BEHAVIOR_STEP_RECIPROCAL_UNFUSED / _SQRT_UNFUSED, and the estimates
 *     are the OP_FLOAT_RECIPROCAL_*_ESTIMATE operations with
 *     BEHAVIOR_ESTIMATE_ARM (see SemanticInterpreter.cpp).
 *   - The fixed point conversions give the number of fraction bits as the
 *     immediate source 1 of the conversion operations.
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
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/ArmSemanticInternal.h"
#include "dismount/proc/arm/ArmSemanticVfp.h"

using namespace armdis;
using namespace armdis::a32;

typedef SemanticOperation Op;
typedef SemanticType::FloatFormat Format;

/////////////////////////////////////////////////////////////////////////////
// The mnemonic and the shape of the operands
/////////////////////////////////////////////////////////////////////////////

namespace {

// The parts of a mnemonic: "vcvt.s32.f32" is the base vcvt and the parts
// s32 and f32
struct Mnemonic {
    char m_base[12];
    char m_parts[3][8];
    uint m_count;
};

Mnemonic parseMnemonic(const ArmLiftContext& c)
{
    Mnemonic m;
    m.m_base[0] = 0;
    m.m_count = 0;
    for (uint i = 0; i < 3; i++)
        m.m_parts[i][0] = 0;
    const char* text = getMnemonic(c);
    uint length = 0;
    while ((text[length] != 0) && (text[length] != '.') && (length < 11))
    {
        m.m_base[length] = text[length];
        length++;
    }
    m.m_base[length] = 0;
    const char* p = text + length;
    while ((*p == '.') && (m.m_count < 3))
    {
        p++;
        uint n = 0;
        while ((*p != 0) && (*p != '.') && (n < 7))
            m.m_parts[m.m_count][n++] = *p++;
        m.m_parts[m.m_count][n] = 0;
        m.m_count++;
    }
    return m;
}

bool equals(const char* a, const char* b)
{
    return strcmp(a, b) == 0;
}

// Return the floating point format of a part ("f32"), false if it isn't one
bool formatOf(const char* part, Format& format)
{
    if (equals(part, "f16"))
        format = SemanticType::FLOAT_HALF;
    else if (equals(part, "f32"))
        format = SemanticType::FLOAT_SINGLE;
    else if (equals(part, "f64"))
        format = SemanticType::FLOAT_DOUBLE;
    else if (equals(part, "bf16"))
        format = SemanticType::FLOAT_BFLOAT16;
    else
        return false;
    return true;
}

uint bitsOf(Format format)
{
    switch (format)
    {
    case SemanticType::FLOAT_DOUBLE: return 64;
    case SemanticType::FLOAT_SINGLE: return 32;
    default:                         return 16;
    }
}

// Return the integer type of a part ("s32", "u16"): the signedness and bits
bool integerOf(const char* part, bool& isSigned, uint& bits)
{
    if (((part[0] != 's') && (part[0] != 'u') && (part[0] != 'i')) ||
        (part[1] < '0') || (part[1] > '9'))
        return false;
    isSigned = part[0] == 's';
    bits = 0;
    for (const char* p = part + 1; *p != 0; p++)
        bits = bits * 10 + (uint)(*p - '0');
    return true;
}

SemanticType integerType(uint bits)
{
    return SemanticType::integer(bits);
}

// The size of the register operand 'index'
uint registerBits(ArmLiftContext& c, uint index)
{
    return c.getArchitecture()->getRegister(c.getOperand(index).m_register).m_bits;
}

// The type of a floating point value of 'format' in a register of 'bits':
// a scalar in an s register and a d register of f64, a vector otherwise
SemanticType floatType(Format format, bool isScalar, uint bits)
{
    if (isScalar)
        return SemanticType::floating(format);
    return SemanticType::vector(SemanticType::floating(format),
                                bits / bitsOf(format));
}

/*
 * The shape of an instruction: the format, the type of the values and
 * whether the Advanced SIMD environment (the standard FPSCR value) applies
 */
struct Shape {
    Format m_format;
    uint m_registerBits;
    bool m_isScalar;
    bool m_isStandard;
    SemanticType m_type;
};

// The shape from the operand 'index' (the destination, usually) and the format
Shape shapeOf(ArmLiftContext& c, uint index, Format format)
{
    Shape s;
    s.m_format = format;
    s.m_registerBits = registerBits(c, index);
    s.m_isScalar = (s.m_registerBits == 32) || (format == SemanticType::FLOAT_DOUBLE);
    s.m_isStandard = !s.m_isScalar;
    s.m_type = floatType(format, s.m_isScalar, s.m_registerBits);
    return s;
}

// An integer constant
SemanticOperand constant(ArmLiftContext& c, uint64 value, uint bits)
{
    return c.getImmediate((int64)value, integerType(bits));
}

// Return the bits [offset, offset + bits) of the integer 'value'
SemanticOperand extractBits(ArmLiftContext& c, const SemanticOperand& value,
                            uint offset, uint bits)
{
    if ((offset == 0) && (value.getType().getTotalBits() == bits))
        return value;
    return c.emit(Op::OP_EXTRACT, integerType(bits), value,
                  constant(c, offset, 32));
}

SemanticOperand bitcast(ArmLiftContext& c, const SemanticType& type,
                        const SemanticOperand& value)
{
    if (value.getType() == type)
        return value;
    return c.emit(Op::OP_BITCAST, type, value);
}

// Return the value of the register operand 'index' as an integer of the size
// of the register
SemanticOperand readBits(ArmLiftContext& c, uint index)
{
    return c.read(index, integerType(registerBits(c, index)));
}

// Return the value of operand 'index' as the type of 'shape': the register,
// or the lane of a scalar operand (d0[1]) replicated in all the lanes
SemanticOperand readFloat(ArmLiftContext& c, uint index, const Shape& shape)
{
    const ArmDecodedOperand& operand = c.getOperand(index);
    const uint elementBits = bitsOf(shape.m_format);
    if (operand.m_kind == ArmDecodedOperand::KIND_NAME)
    {
        // The #0 of the compares
        return c.getImmediate(0, shape.m_type);
    }
    SemanticOperand bits = readBits(c, index);
    if (operand.m_elementIndex >= 0)
    {
        SemanticOperand lane = extractBits(c, bits,
                            (uint)operand.m_elementIndex * elementBits, elementBits);
        lane = bitcast(c, SemanticType::floating(shape.m_format), lane);
        if (shape.m_isScalar)
            return lane;
        return c.emit(Op::OP_BROADCAST, shape.m_type, lane);
    }
    if (shape.m_isScalar)
    {
        bits = extractBits(c, bits, 0, elementBits);
        return bitcast(c, SemanticType::floating(shape.m_format), bits);
    }
    return bitcast(c, shape.m_type, bits);
}

// Write a floating point value (a scalar or a vector) into the register
// operand 'index': the bits, zero extended to the register
void writeFloat(ArmLiftContext& c, uint index, const SemanticOperand& value)
{
    const uint bits = value.getType().getTotalBits();
    c.write(index, bitcast(c, integerType(bits), value));
}

// Emit a floating point operation
SemanticOperand fp(ArmLiftContext& c, Op::Operation operation,
                   const SemanticType& type, bool isStandard,
                   const SemanticOperand& a = SemanticOperand(),
                   const SemanticOperand& b = SemanticOperand(),
                   const SemanticOperand& d = SemanticOperand(),
                   Op::RoundingMode mode = Op::ROUND_DYNAMIC,
                   uint kind = 0, bool hasExceptions = true,
                   Op::Behavior behavior = Op::BEHAVIOR_DEFAULT)
{
    SemanticOperand result = c.newTemporary(type);
    SemanticInstruction instruction(operation, type, result, a, b, d);
    uint32 attributes = 0;
    if (hasExceptions)
        attributes |= SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS;
    if (isStandard)
        attributes |= SemanticInstruction::ATTRIBUTE_FLOAT_STANDARD;
    instruction.setAttributes(attributes);
    instruction.setRoundingMode(mode);
    instruction.setBehavior(behavior);
    if (kind != 0)
        instruction.setKind(kind);
    c.emit(instruction);
    return result;
}

SemanticOperand negate(ArmLiftContext& c, const Shape& shape,
                       const SemanticOperand& a)
{
    return fp(c, Op::OP_FLOAT_NEGATE, shape.m_type, shape.m_isStandard, a,
              SemanticOperand(), SemanticOperand(), Op::ROUND_DYNAMIC, 0, false);
}

// The lanes of a floating point vector value as scalars (the register 'bits'
// integer), and back
void splitLanes(ArmLiftContext& c, const SemanticOperand& bits, Format format,
                uint count, SemanticOperand* lanes)
{
    const uint elementBits = bitsOf(format);
    for (uint i = 0; i < count; i++)
        lanes[i] = bitcast(c, SemanticType::floating(format),
                           extractBits(c, bits, i * elementBits, elementBits));
}

SemanticOperand joinLanes(ArmLiftContext& c, const SemanticOperand* lanes,
                          uint count, uint elementBits)
{
    SemanticOperand result = bitcast(c, integerType(elementBits), lanes[0]);
    for (uint i = 1; i < count; i++)
        result = c.emit(Op::OP_CONCAT, integerType(elementBits * (i + 1)),
                        result, bitcast(c, integerType(elementBits), lanes[i]));
    return result;
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// Arithmetic
/////////////////////////////////////////////////////////////////////////////

namespace armdis {
namespace a32 {

// vadd vsub vmul vdiv vnmul vabd vmax vmin vmaxnm vminnm vmla vmls vnmla
// vnmls vfma vfms vfnma vfnms vrecps vrsqrts: the operands are the
// destination (also the accumulator), the first and the second operand (a
// scalar lane of a d register for the by scalar forms)
void liftFpArithmetic(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    Format format;
    CHECK(formatOf(m.m_parts[0], format));
    const Shape s = shapeOf(c, 0, format);
    const char* name = m.m_base;
    const SemanticOperand a = readFloat(c, 1, s);
    const SemanticOperand b = readFloat(c, 2, s);
    const bool isStd = s.m_isStandard;
    const SemanticType& t = s.m_type;
    SemanticOperand result;

    if (equals(name, "vadd"))
        result = fp(c, Op::OP_FLOAT_ADD, t, isStd, a, b);
    else if (equals(name, "vsub"))
        result = fp(c, Op::OP_FLOAT_SUBTRACT, t, isStd, a, b);
    else if (equals(name, "vmul"))
        result = fp(c, Op::OP_FLOAT_MULTIPLY, t, isStd, a, b);
    else if (equals(name, "vdiv"))
        result = fp(c, Op::OP_FLOAT_DIVIDE, t, isStd, a, b);
    else if (equals(name, "vnmul"))
        result = negate(c, s, fp(c, Op::OP_FLOAT_MULTIPLY, t, isStd, a, b));
    else if (equals(name, "vabd"))
        result = fp(c, Op::OP_FLOAT_ABSOLUTE, t, isStd,
                    fp(c, Op::OP_FLOAT_SUBTRACT, t, isStd, a, b),
                    SemanticOperand(), SemanticOperand(), Op::ROUND_DYNAMIC, 0,
                    false);
    else if (equals(name, "vmax") || equals(name, "vmin"))
        result = fp(c, equals(name, "vmax") ? Op::OP_FLOAT_MAXIMUM :
                                              Op::OP_FLOAT_MINIMUM, t, isStd, a, b,
                    SemanticOperand(), Op::ROUND_DYNAMIC, 0, true,
                    Op::BEHAVIOR_MINMAX_PROPAGATE_NAN);
    else if (equals(name, "vmaxnm") || equals(name, "vminnm"))
        result = fp(c, equals(name, "vmaxnm") ? Op::OP_FLOAT_MAXIMUM :
                                                Op::OP_FLOAT_MINIMUM, t, isStd, a, b,
                    SemanticOperand(), Op::ROUND_DYNAMIC, 0, true,
                    Op::BEHAVIOR_MINMAX_NUMBER);
    else if (equals(name, "vrecps") || equals(name, "vrsqrts"))
        result = fp(c, Op::OP_FLOAT_MULTIPLY_ADD, t, isStd, a, b,
                    c.getImmediate(0, t), Op::ROUND_DYNAMIC, 0, true,
                    equals(name, "vrecps") ? Op::BEHAVIOR_STEP_RECIPROCAL_UNFUSED :
                                             Op::BEHAVIOR_STEP_RECIPROCAL_SQRT_UNFUSED);
    else
    {
        // The accumulating forms: the destination is the addend
        const SemanticOperand d = readFloat(c, 0, s);
        if (equals(name, "vmla"))
            result = fp(c, Op::OP_FLOAT_ADD, t, isStd, d,
                        fp(c, Op::OP_FLOAT_MULTIPLY, t, isStd, a, b));
        else if (equals(name, "vmls"))
        {
            const SemanticOperand product = fp(c, Op::OP_FLOAT_MULTIPLY, t, isStd, a, b);
            // The Advanced SIMD vmls subtracts, the VFP one adds the
            // negated product (the NaN sign differs)
            if (isStd)
                result = fp(c, Op::OP_FLOAT_SUBTRACT, t, isStd, d, product);
            else
                result = fp(c, Op::OP_FLOAT_ADD, t, isStd, d, negate(c, s, product));
        } else if (equals(name, "vnmla"))
        {
            const SemanticOperand product = fp(c, Op::OP_FLOAT_MULTIPLY, t, isStd, a, b);
            result = fp(c, Op::OP_FLOAT_ADD, t, isStd, negate(c, s, d),
                        negate(c, s, product));
        } else if (equals(name, "vnmls"))
        {
            const SemanticOperand product = fp(c, Op::OP_FLOAT_MULTIPLY, t, isStd, a, b);
            result = fp(c, Op::OP_FLOAT_ADD, t, isStd, negate(c, s, d), product);
        } else if (equals(name, "vfma"))
            result = fp(c, Op::OP_FLOAT_MULTIPLY_ADD, t, isStd, a, b, d);
        else if (equals(name, "vfms"))
            result = fp(c, Op::OP_FLOAT_MULTIPLY_ADD, t, isStd, negate(c, s, a), b, d);
        else if (equals(name, "vfnma"))
            result = fp(c, Op::OP_FLOAT_MULTIPLY_ADD, t, isStd, negate(c, s, a), b,
                        negate(c, s, d));
        else if (equals(name, "vfnms"))
            result = fp(c, Op::OP_FLOAT_MULTIPLY_ADD, t, isStd, a, b, negate(c, s, d));
        else
        {
            undefined(c);
            return;
        }
    }
    writeFloat(c, 0, result);
}

// vpadd vpmax vpmin: the pairs of the first operand, then the ones of the
// second, in a d register
void liftFpPairwise(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    Format format;
    CHECK(formatOf(m.m_parts[0], format));
    const uint elementBits = bitsOf(format);
    const uint count = 64 / elementBits;
    const SemanticType scalar = SemanticType::floating(format);
    SemanticOperand n[4], mm[4], lanes[4];
    splitLanes(c, readBits(c, 1), format, count, n);
    splitLanes(c, readBits(c, 2), format, count, mm);
    for (uint i = 0; i < count / 2; i++)
    {
        for (uint half = 0; half < 2; half++)
        {
            const SemanticOperand* source = (half == 0) ? n : mm;
            const SemanticOperand x = source[2 * i];
            const SemanticOperand y = source[2 * i + 1];
            SemanticOperand r;
            if (equals(m.m_base, "vpadd"))
                r = fp(c, Op::OP_FLOAT_ADD, scalar, true, x, y);
            else
                r = fp(c, equals(m.m_base, "vpmax") ? Op::OP_FLOAT_MAXIMUM :
                                                      Op::OP_FLOAT_MINIMUM, scalar,
                       true, x, y, SemanticOperand(), Op::ROUND_DYNAMIC, 0, true,
                       Op::BEHAVIOR_MINMAX_PROPAGATE_NAN);
            lanes[half * (count / 2) + i] = r;
        }
    }
    writeFloat(c, 0, joinLanes(c, lanes, count, elementBits));
}

// vabs vneg vsqrt vrecpe vrsqrte vrint*
void liftFpUnary(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    Format format;
    CHECK(formatOf(m.m_parts[0], format));
    const Shape s = shapeOf(c, 0, format);
    const char* name = m.m_base;
    const SemanticOperand a = readFloat(c, 1, s);
    const SemanticType& t = s.m_type;
    SemanticOperand result;
    if (equals(name, "vabs"))
        result = fp(c, Op::OP_FLOAT_ABSOLUTE, t, s.m_isStandard, a,
                    SemanticOperand(), SemanticOperand(), Op::ROUND_DYNAMIC, 0, false);
    else if (equals(name, "vneg"))
        result = negate(c, s, a);
    else if (equals(name, "vsqrt"))
        result = fp(c, Op::OP_FLOAT_SQUARE_ROOT, t, s.m_isStandard, a);
    else if (equals(name, "vrecpe"))
        result = fp(c, Op::OP_FLOAT_RECIPROCAL_ESTIMATE, t, s.m_isStandard, a,
                    SemanticOperand(), SemanticOperand(), Op::ROUND_DYNAMIC, 0,
                    true, Op::BEHAVIOR_ESTIMATE_ARM);
    else if (equals(name, "vrsqrte"))
        result = fp(c, Op::OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE, t, s.m_isStandard, a,
                    SemanticOperand(), SemanticOperand(), Op::ROUND_DYNAMIC, 0,
                    true, Op::BEHAVIOR_ESTIMATE_ARM);
    else
    {
        // vrintz (toward zero), vrintr (FPSCR), vrintx (FPSCR, inexact),
        // vrinta (ties away), vrintn (ties even), vrintp, vrintm
        Op::RoundingMode mode = Op::ROUND_DYNAMIC;
        if (equals(name, "vrintz"))
            mode = Op::ROUND_TOWARD_ZERO;
        else if (equals(name, "vrinta"))
            mode = Op::ROUND_NEAREST_AWAY;
        else if (equals(name, "vrintn"))
            mode = Op::ROUND_NEAREST_EVEN;
        else if (equals(name, "vrintp"))
            mode = Op::ROUND_TOWARD_POSITIVE;
        else if (equals(name, "vrintm"))
            mode = Op::ROUND_TOWARD_NEGATIVE;
        result = fp(c, Op::OP_FLOAT_ROUND, t, s.m_isStandard, a,
                    SemanticOperand(), SemanticOperand(), mode,
                    equals(name, "vrintx") ? Op::FLOAT_ROUND_SIGNAL_INEXACT :
                                             Op::FLOAT_ROUND_SUPPRESS_INEXACT);
        // (the kind 0 is FLOAT_ROUND_SIGNAL_INEXACT)
    }
    writeFloat(c, 0, result);
}

} // namespace a32
} // namespace armdis

/////////////////////////////////////////////////////////////////////////////
// Compares and selects
/////////////////////////////////////////////////////////////////////////////

namespace {

// Return the 1 bit result of a floating point compare of 'a' and 'b'. The
// flags of the exceptions are added to the FPSCR only when 'hasExceptions'
SemanticOperand compareFloat(ArmLiftContext& c, Op::Condition condition,
                             const SemanticType& type,
                             const SemanticOperand& a, const SemanticOperand& b,
                             bool isStandard, bool hasExceptions)
{
    const SemanticType booleanType = SemanticType::integer(1);
    SemanticOperand result = c.newTemporary(booleanType);
    SemanticInstruction instruction(Op::OP_COMPARE, type, result, a, b);
    instruction.setCondition(condition);
    uint32 attributes = 0;
    if (hasExceptions)
        attributes |= SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS;
    if (isStandard)
        attributes |= SemanticInstruction::ATTRIBUTE_FLOAT_STANDARD;
    instruction.setAttributes(attributes);
    c.emit(instruction);
    return result;
}

// Return a 1 bit value placed at 'offset' of a 32 bit value
SemanticOperand placeBit(ArmLiftContext& c, const SemanticOperand& bit,
                         uint offset)
{
    SemanticOperand wide = c.emit(Op::OP_ZERO_EXTEND, integerType(32), bit);
    return c.emit(Op::OP_SHIFT_LEFT, integerType(32), wide,
                  constant(c, offset, 32));
}

// Write the flags N Z C V (1 bit values) into bits 31-28 of the FPSCR
void writeFpscrFlags(ArmLiftContext& c, const SemanticOperand& n,
                     const SemanticOperand& z, const SemanticOperand& carry,
                     const SemanticOperand& v)
{
    const SemanticOperand fpscr = c.getRegister(ARM_REG_FPSCR);
    SemanticOperand value = c.emit(Op::OP_AND, integerType(32), fpscr,
                                   constant(c, 0x0FFFFFFFu, 32));
    value = c.emit(Op::OP_OR, integerType(32), value, placeBit(c, n, 31));
    value = c.emit(Op::OP_OR, integerType(32), value, placeBit(c, z, 30));
    value = c.emit(Op::OP_OR, integerType(32), value, placeBit(c, carry, 29));
    value = c.emit(Op::OP_OR, integerType(32), value, placeBit(c, v, 28));
    c.move(fpscr, value);
}

} // namespace

namespace armdis {
namespace a32 {

// vcmp, vcmpe: the result is in the flags of the FPSCR (vmrs APSR_nzcv, fpscr
// copies them): less 1000, equal 0110, greater 0010, unordered 0011
void liftFpCompare(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    Format format;
    CHECK(formatOf(m.m_parts[0], format));
    const Shape s = shapeOf(c, 0, format);
    const bool isSignaling = equals(m.m_base, "vcmpe");
    const SemanticOperand a = readFloat(c, 0, s);
    const SemanticOperand b = readFloat(c, 1, s);
    // vcmp raises the invalid operation for a signaling NaN, vcmpe for any NaN.
    // Only one of the compares adds the exceptions
    const SemanticOperand less = compareFloat(c, Op::COND_FLOAT_LESS, s.m_type,
                                              a, b, false, isSignaling);
    const SemanticOperand equal = compareFloat(c, Op::COND_FLOAT_EQUAL, s.m_type,
                                               a, b, false, !isSignaling);
    const SemanticOperand unordered = compareFloat(c, Op::COND_FLOAT_UNORDERED,
                                                   s.m_type, a, b, false, false);
    const SemanticOperand notLess = c.emit(Op::OP_XOR, integerType(1), less,
                                           c.getImmediate(1, integerType(1)));
    writeFpscrFlags(c, less, equal, notLess, unordered);
}

// vceq vcge vcgt vcle vclt vacge vacgt: a mask of all ones in the lanes
// where the relation holds
void liftFpVectorCompare(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    Format format;
    CHECK(formatOf(m.m_parts[0], format));
    const Shape s = shapeOf(c, 0, format);
    const char* name = m.m_base;
    const uint elementBits = bitsOf(format);
    const uint count = s.m_registerBits / elementBits;
    const bool isAbsolute = (name[1] == 'a');
    Op::Condition condition;
    if (equals(name, "vceq"))
        condition = Op::COND_FLOAT_EQUAL;
    else if (equals(name, "vcge") || equals(name, "vacge"))
        condition = Op::COND_FLOAT_GREATER_EQUAL;
    else if (equals(name, "vcgt") || equals(name, "vacgt"))
        condition = Op::COND_FLOAT_GREATER;
    else if (equals(name, "vcle"))
        condition = Op::COND_FLOAT_LESS_EQUAL;
    else
        condition = Op::COND_FLOAT_LESS;

    const SemanticType scalar = SemanticType::floating(format);
    SemanticOperand n[8], mm[8], lanes[8];
    splitLanes(c, readBits(c, 1), format, count, n);
    if (c.getOperand(2).m_kind == ArmDecodedOperand::KIND_NAME)
    {
        for (uint i = 0; i < count; i++)
            mm[i] = c.getImmediate(0, scalar);
    } else
    {
        splitLanes(c, readBits(c, 2), format, count, mm);
    }
    for (uint i = 0; i < count; i++)
    {
        SemanticOperand x = n[i], y = mm[i];
        if (isAbsolute)
        {
            x = fp(c, Op::OP_FLOAT_ABSOLUTE, scalar, true, x, SemanticOperand(),
                   SemanticOperand(), Op::ROUND_DYNAMIC, 0, false);
            y = fp(c, Op::OP_FLOAT_ABSOLUTE, scalar, true, y, SemanticOperand(),
                   SemanticOperand(), Op::ROUND_DYNAMIC, 0, false);
        }
        const SemanticOperand r = compareFloat(c, condition, scalar, x, y, true, true);
        lanes[i] = c.emit(Op::OP_SIGN_EXTEND, integerType(elementBits), r);
    }
    SemanticOperand result = lanes[0];
    for (uint i = 1; i < count; i++)
        result = c.emit(Op::OP_CONCAT, integerType(elementBits * (i + 1)), result,
                        lanes[i]);
    c.write(0, result);
}

// vseleq vselge vselgt vselvs: the first or the second operand by the flags
void liftFpSelect(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    Format format;
    CHECK(formatOf(m.m_parts[0], format));
    const uint elementBits = bitsOf(format);
    Op::Condition condition;
    if (equals(m.m_base, "vseleq"))
        condition = Op::COND_EQUAL;
    else if (equals(m.m_base, "vselge"))
        condition = Op::COND_SIGNED_GREATER_EQUAL;
    else if (equals(m.m_base, "vselgt"))
        condition = Op::COND_SIGNED_GREATER;
    else
        condition = Op::COND_OVERFLOW;
    const SemanticOperand flag = c.testFlags(condition);
    const SemanticOperand a = extractBits(c, readBits(c, 1), 0, elementBits);
    const SemanticOperand b = extractBits(c, readBits(c, 2), 0, elementBits);
    c.write(0, c.emit(Op::OP_SELECT, integerType(elementBits), flag, a, b));
}

} // namespace a32
} // namespace armdis

/////////////////////////////////////////////////////////////////////////////
// Conversions
/////////////////////////////////////////////////////////////////////////////

namespace {

// The rounding mode of a vcvt* mnemonic base
Op::RoundingMode conversionMode(const char* base)
{
    if (equals(base, "vcvtr"))
        return Op::ROUND_DYNAMIC;
    if (equals(base, "vcvta"))
        return Op::ROUND_NEAREST_AWAY;
    if (equals(base, "vcvtn"))
        return Op::ROUND_NEAREST_EVEN;
    if (equals(base, "vcvtp"))
        return Op::ROUND_TOWARD_POSITIVE;
    if (equals(base, "vcvtm"))
        return Op::ROUND_TOWARD_NEGATIVE;
    return Op::ROUND_TOWARD_ZERO;
}

// vcvtb, vcvtt: the conversions of a half precision (or bfloat16) number in
// the low (b) or the high (t) half of an s register
void liftConvertHalf(ArmLiftContext& c, const Mnemonic& m)
{
    Format to, from;
    CHECK(formatOf(m.m_parts[0], to) && formatOf(m.m_parts[1], from));
    const uint offset = equals(m.m_base, "vcvtt") ? 16 : 0;
    const Shape source = shapeOf(c, 1, from);
    const SemanticOperand value = readFloat(c, 1, source);
    if (bitsOf(to) == 16)
    {
        // Narrow into one half of the destination, the other is kept
        // (the bfloat16 conversions always round to nearest even)
        const SemanticOperand narrow = fp(c, Op::OP_FLOAT_CONVERT,
            SemanticType::floating(to), false, value, SemanticOperand(),
            SemanticOperand(),
            (to == SemanticType::FLOAT_BFLOAT16) ? Op::ROUND_NEAREST_EVEN :
                                                   Op::ROUND_DYNAMIC);
        const SemanticOperand half = bitcast(c, integerType(16), narrow);
        const SemanticOperand old = readBits(c, 0);
        c.write(0, c.emit(Op::OP_INSERT, integerType(32), old, half,
                          constant(c, offset, 32)));
    } else
    {
        // Widen the half
        const SemanticOperand bits = extractBits(c, readBits(c, 1), offset, 16);
        const SemanticOperand half = bitcast(c, SemanticType::floating(from), bits);
        writeFloat(c, 0, fp(c, Op::OP_FLOAT_CONVERT, SemanticType::floating(to),
                            false, half));
    }
}

} // namespace

namespace armdis {
namespace a32 {

// vcvt, vcvtr, vcvta, vcvtn, vcvtp, vcvtm, vcvtb, vcvtt: the conversions
// between integers (and fixed point numbers) and floating point numbers, and
// between the floating point formats
void liftFpConvert(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    if (equals(m.m_base, "vcvtb") || equals(m.m_base, "vcvtt"))
    {
        liftConvertHalf(c, m);
        return;
    }
    bool isSigned = true;
    uint integerBits = 0;
    Format sourceFormat, destinationFormat;
    const bool hasFraction = (c.getOperandsCount() == 3) &&
        (c.getOperand(2).m_kind == ArmDecodedOperand::KIND_IMMEDIATE);
    const SemanticOperand fraction = hasFraction ?
        constant(c, (uint64)c.getOperand(2).m_immediate, 32) : SemanticOperand();
    const Op::RoundingMode mode = conversionMode(m.m_base);

    if (integerOf(m.m_parts[0], isSigned, integerBits) &&
        formatOf(m.m_parts[1], sourceFormat))
    {
        // Floating point to integer, saturating, NaN is zero
        const Shape s = shapeOf(c, 1, sourceFormat);
        const SemanticOperand value = readFloat(c, 1, s);
        const uint lanes = s.m_isScalar ? 1 : s.m_registerBits / bitsOf(sourceFormat);
        const SemanticType type = s.m_isScalar ? integerType(integerBits) :
            SemanticType::vector(integerType(integerBits), lanes);
        SemanticOperand result = c.newTemporary(type);
        SemanticInstruction instruction(
            isSigned ? Op::OP_FLOAT_TO_SIGNED : Op::OP_FLOAT_TO_UNSIGNED, type,
            result, value, fraction);
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS |
            (s.m_isStandard ? SemanticInstruction::ATTRIBUTE_FLOAT_STANDARD : 0));
        instruction.setRoundingMode(mode);
        instruction.setBehavior(Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO);
        c.emit(instruction);
        // A fixed point number narrower than the register is extended
        const uint registerSize = registerBits(c, 0);
        if (s.m_isScalar && (integerBits < registerSize))
            result = c.emit(isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                            integerType(registerSize), result);
        c.write(0, bitcast(c, integerType(result.getType().getTotalBits()), result));
        return;
    }
    if (formatOf(m.m_parts[0], destinationFormat) &&
        integerOf(m.m_parts[1], isSigned, integerBits))
    {
        // Integer (or fixed point) to floating point
        const Shape s = shapeOf(c, 0, destinationFormat);
        SemanticOperand source = readBits(c, 1);
        SemanticType sourceType;
        if (s.m_isScalar)
        {
            sourceType = integerType(integerBits);
            source = extractBits(c, source, 0, integerBits);
        } else
        {
            sourceType = SemanticType::vector(integerType(integerBits),
                                s.m_registerBits / integerBits);
            source = bitcast(c, sourceType, source);
        }
        SemanticOperand result = c.newTemporary(s.m_type);
        SemanticInstruction instruction(
            isSigned ? Op::OP_SIGNED_TO_FLOAT : Op::OP_UNSIGNED_TO_FLOAT, s.m_type,
            result, source, fraction);
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS |
            (s.m_isStandard ? SemanticInstruction::ATTRIBUTE_FLOAT_STANDARD : 0));
        instruction.setRoundingMode(Op::ROUND_DYNAMIC);
        c.emit(instruction);
        writeFloat(c, 0, result);
        return;
    }
    if (formatOf(m.m_parts[0], destinationFormat) &&
        formatOf(m.m_parts[1], sourceFormat))
    {
        // Between the floating point formats
        const Shape d = shapeOf(c, 0, destinationFormat);
        const Shape s = shapeOf(c, 1, sourceFormat);
        const SemanticOperand value = readFloat(c, 1, s);
        const bool isBfloat = destinationFormat == SemanticType::FLOAT_BFLOAT16;
        writeFloat(c, 0, fp(c, Op::OP_FLOAT_CONVERT, d.m_type,
                            d.m_isStandard, value, SemanticOperand(),
                            SemanticOperand(),
                            isBfloat ? Op::ROUND_NEAREST_EVEN : Op::ROUND_DYNAMIC));
        return;
    }
    undefined(c);
}

// vins.f16, vmovx.f16
void liftFpHalfMove(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    const SemanticOperand source = readBits(c, 1);
    if (equals(m.m_base, "vins"))
    {
        const SemanticOperand low = extractBits(c, source, 0, 16);
        c.write(0, c.emit(Op::OP_INSERT, integerType(32), readBits(c, 0), low,
                          constant(c, 16, 32)));
    } else
    {
        c.write(0, c.emit(Op::OP_ZERO_EXTEND, integerType(32),
                          extractBits(c, source, 16, 16)));
    }
}

} // namespace a32
} // namespace armdis

/////////////////////////////////////////////////////////////////////////////
// Complex numbers, bfloat16 and the half precision widening multiplies
/////////////////////////////////////////////////////////////////////////////

namespace {

// Convert a half precision number to single precision as the IEEE format,
// whatever the FPSCR.AHP says
SemanticOperand convertIeeeHalf(ArmLiftContext& c, const SemanticType& type,
                                const SemanticOperand& value)
{
    SemanticOperand result = c.newTemporary(type);
    SemanticInstruction instruction(Op::OP_FLOAT_CONVERT, type, result, value);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS |
                              SemanticInstruction::ATTRIBUTE_FLOAT_STANDARD |
                              SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
    c.emit(instruction);
    return result;
}

// Return the lanes of 'count' elements of operand 'index' of a register
// (the whole register), or for a scalar operand (d0[1]) the pair 'pairBits'
// wide at the index, replicated
void readLanes(ArmLiftContext& c, uint index, Format format, uint count,
               uint pairBits, SemanticOperand* lanes)
{
    const ArmDecodedOperand& operand = c.getOperand(index);
    const uint elementBits = bitsOf(format);
    SemanticOperand bits = readBits(c, index);
    if (operand.m_elementIndex >= 0)
    {
        const SemanticOperand pair = extractBits(c, bits,
                        (uint)operand.m_elementIndex * pairBits, pairBits);
        SemanticOperand pairLanes[2];
        splitLanes(c, pair, format, pairBits / elementBits, pairLanes);
        for (uint i = 0; i < count; i++)
            lanes[i] = pairLanes[i % (pairBits / elementBits)];
        return;
    }
    splitLanes(c, bits, format, count, lanes);
}

} // namespace

namespace armdis {
namespace a32 {

// vcadd, vcmla (FEAT_FCMA): the complex numbers are pairs of lanes
void liftFpComplex(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    Format format;
    CHECK(formatOf(m.m_parts[0], format));
    const Shape s = shapeOf(c, 0, format);
    const uint elementBits = bitsOf(format);
    const uint count = s.m_registerBits / elementBits;
    const uint rotation = (uint)c.getOperand(c.getOperandsCount() - 1).m_immediate;
    const SemanticType scalar = SemanticType::floating(format);
    SemanticOperand n[8], mm[8], d[8], lanes[8];
    splitLanes(c, readBits(c, 1), format, count, n);
    readLanes(c, 2, format, count, 2 * elementBits, mm);
    const bool isAccumulating = equals(m.m_base, "vcmla");
    if (isAccumulating)
        splitLanes(c, readBits(c, 0), format, count, d);
    for (uint e = 0; e < count / 2; e++)
    {
        const uint re = 2 * e, im = 2 * e + 1;
        if (!isAccumulating)
        {
            if (rotation == 90)
            {
                lanes[re] = fp(c, Op::OP_FLOAT_SUBTRACT, scalar, true, n[re], mm[im]);
                lanes[im] = fp(c, Op::OP_FLOAT_ADD, scalar, true, n[im], mm[re]);
            } else
            {
                lanes[re] = fp(c, Op::OP_FLOAT_ADD, scalar, true, n[re], mm[im]);
                lanes[im] = fp(c, Op::OP_FLOAT_SUBTRACT, scalar, true, n[im], mm[re]);
            }
            continue;
        }
        SemanticOperand element1, element2, element3, element4;
        switch (rotation)
        {
        case 0:
            element1 = mm[re]; element2 = n[re];
            element3 = mm[im]; element4 = n[re];
            break;
        case 90:
            element1 = negate(c, s, mm[im]); element2 = n[im];
            element3 = mm[re]; element4 = n[im];
            break;
        case 180:
            element1 = negate(c, s, mm[re]); element2 = n[re];
            element3 = negate(c, s, mm[im]); element4 = n[re];
            break;
        default:
            element1 = mm[im]; element2 = n[im];
            element3 = negate(c, s, mm[re]); element4 = n[im];
            break;
        }
        lanes[re] = fp(c, Op::OP_FLOAT_MULTIPLY_ADD, scalar, true, element2,
                       element1, d[re]);
        lanes[im] = fp(c, Op::OP_FLOAT_MULTIPLY_ADD, scalar, true, element4,
                       element3, d[im]);
    }
    writeFloat(c, 0, joinLanes(c, lanes, count, elementBits));
}

// vfmal, vfmsl (FEAT_FHM): half precision products added to single precision
// accumulators
void liftFpWidenMultiply(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    const bool isSubtract = equals(m.m_base, "vfmsl");
    const uint count = registerBits(c, 0) / 32;
    const SemanticType single = SemanticType::floating(SemanticType::FLOAT_SINGLE);
    SemanticOperand n[4], mm[4], d[4], lanes[4];
    splitLanes(c, readBits(c, 1), SemanticType::FLOAT_HALF, count, n);
    if (c.getOperand(2).m_elementIndex >= 0)
    {
        readLanes(c, 2, SemanticType::FLOAT_HALF, count, 16, mm);
    } else
    {
        splitLanes(c, readBits(c, 2), SemanticType::FLOAT_HALF, count, mm);
    }
    splitLanes(c, readBits(c, 0), SemanticType::FLOAT_SINGLE, count, d);
    for (uint e = 0; e < count; e++)
    {
        SemanticOperand a = convertIeeeHalf(c, single, n[e]);
        const SemanticOperand b = convertIeeeHalf(c, single, mm[e]);
        if (isSubtract)
            a = fp(c, Op::OP_FLOAT_NEGATE, single, true, a, SemanticOperand(),
                   SemanticOperand(), Op::ROUND_DYNAMIC, 0, false);
        lanes[e] = fp(c, Op::OP_FLOAT_MULTIPLY_ADD, single, true, a, b, d[e]);
    }
    writeFloat(c, 0, joinLanes(c, lanes, count, 32));
}

} // namespace a32
} // namespace armdis

namespace {

// The bfloat16 'bits' (16 bit integer) widened to a single precision number
SemanticOperand widenBfloat(ArmLiftContext& c, const SemanticOperand& bfloat)
{
    const SemanticOperand wide = c.emit(Op::OP_ZERO_EXTEND, integerType(32), bfloat);
    return bitcast(c, SemanticType::floating(SemanticType::FLOAT_SINGLE),
                   c.emit(Op::OP_SHIFT_LEFT, integerType(32), wide, constant(c, 16, 32)));
}

// BFDotAdd: addend + (a1 * b1 + a2 * b2), the sum of the two products rounded
// to odd
SemanticOperand bfloatDotAdd(ArmLiftContext& c, const SemanticOperand& addend,
                             const SemanticOperand& a1, const SemanticOperand& a2,
                             const SemanticOperand& b1, const SemanticOperand& b2)
{
    const SemanticType single = SemanticType::floating(SemanticType::FLOAT_SINGLE);
    const SemanticOperand p1 = fp(c, Op::OP_FLOAT_MULTIPLY, single, true,
                                  widenBfloat(c, a1), widenBfloat(c, b1));
    const SemanticOperand p2 = fp(c, Op::OP_FLOAT_MULTIPLY, single, true,
                                  widenBfloat(c, a2), widenBfloat(c, b2));
    const SemanticOperand sum = fp(c, Op::OP_FLOAT_ADD, single, true, p1, p2,
                                   SemanticOperand(), Op::ROUND_TO_ODD);
    return fp(c, Op::OP_FLOAT_ADD, single, true, addend, sum);
}

// The 16 bit lanes of an operand as integers (a scalar operand d0[3] gives
// its lane replicated)
void readBfloatLanes(ArmLiftContext& c, uint index, uint count,
                     SemanticOperand* lanes)
{
    const ArmDecodedOperand& operand = c.getOperand(index);
    if (operand.m_elementIndex >= 0)
    {
        // The lane may be in the upper d register of the pair
        uint offset = (uint)operand.m_elementIndex * 16;
        SemanticOperand source;
        if (offset >= 64)
        {
            const uint number = operand.m_register - ARM_REG_D0;
            source = c.getRegister(ARM_REG_Q0 + number / 2);
            offset += (number % 2) * 64;
        } else
        {
            source = readBits(c, index);
        }
        const SemanticOperand lane = extractBits(c, source, offset, 16);
        for (uint i = 0; i < count; i++)
            lanes[i] = lane;
        return;
    }
    const SemanticOperand bits = readBits(c, index);
    for (uint i = 0; i < count; i++)
        lanes[i] = extractBits(c, bits, i * 16, 16);
}

} // namespace

namespace armdis {
namespace a32 {

// vdot.bf16, vmmla.bf16, vfmab.bf16, vfmat.bf16 (FEAT_BF16)
void liftBfloat(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    const SemanticType single = SemanticType::floating(SemanticType::FLOAT_SINGLE);
    const uint bits = registerBits(c, 0);
    const uint count = bits / 32;
    SemanticOperand d[4], lanes[4];
    splitLanes(c, readBits(c, 0), SemanticType::FLOAT_SINGLE, count, d);
    if (equals(m.m_base, "vdot"))
    {
        SemanticOperand n[8], mm[8];
        readBfloatLanes(c, 1, count * 2, n);
        // The by element form replicates a pair
        if (c.getOperand(2).m_elementIndex >= 0)
        {
            const SemanticOperand pair = extractBits(c, readBits(c, 2),
                        (uint)c.getOperand(2).m_elementIndex * 32, 32);
            for (uint i = 0; i < count * 2; i++)
                mm[i] = extractBits(c, pair, (i % 2) * 16, 16);
        } else
        {
            readBfloatLanes(c, 2, count * 2, mm);
        }
        for (uint e = 0; e < count; e++)
            lanes[e] = bfloatDotAdd(c, d[e], n[2 * e], n[2 * e + 1], mm[2 * e],
                                    mm[2 * e + 1]);
    } else if (equals(m.m_base, "vmmla"))
    {
        SemanticOperand n[8], mm[8];
        readBfloatLanes(c, 1, 8, n);
        readBfloatLanes(c, 2, 8, mm);
        for (uint i = 0; i < 2; i++)
            for (uint j = 0; j < 2; j++)
            {
                SemanticOperand sum = d[2 * i + j];
                for (uint k = 0; k < 2; k++)
                    sum = bfloatDotAdd(c, sum, n[4 * i + 2 * k], n[4 * i + 2 * k + 1],
                                       mm[4 * j + 2 * k], mm[4 * j + 2 * k + 1]);
                lanes[2 * i + j] = sum;
            }
    } else
    {
        // vfmab (even lanes), vfmat (odd lanes)
        const uint select = equals(m.m_base, "vfmat") ? 1 : 0;
        SemanticOperand n[8], mm[8];
        readBfloatLanes(c, 1, 8, n);
        readBfloatLanes(c, 2, 8, mm);
        for (uint e = 0; e < count; e++)
        {
            // (the by element form has the same lane everywhere)
            const SemanticOperand b = mm[2 * e + select];
            lanes[e] = fp(c, Op::OP_FLOAT_MULTIPLY_ADD, single, true,
                          widenBfloat(c, n[2 * e + select]), widenBfloat(c, b), d[e]);
        }
    }
    writeFloat(c, 0, joinLanes(c, lanes, count, 32));
}

} // namespace a32
} // namespace armdis

/////////////////////////////////////////////////////////////////////////////
// Moves, the status registers and the memory instructions
/////////////////////////////////////////////////////////////////////////////

namespace {

// VFPExpandImm: the bits of the 8 bit floating point immediate in a format
uint64 expandFloatImmediate8(Format format, uint imm8)
{
    const uint64 sign = (imm8 >> 7) & 1;
    const uint64 b6 = (imm8 >> 6) & 1;
    const uint64 low = (imm8 >> 4) & 3;
    const uint64 fraction = imm8 & 15;
    switch (format)
    {
    case SemanticType::FLOAT_DOUBLE:
        return (sign << 63) |
               ((((b6 ^ 1) << 10) | ((b6 ? 0xFFull : 0) << 2) | low) << 52) |
               (fraction << 48);
    case SemanticType::FLOAT_SINGLE:
        return (sign << 31) |
               ((((b6 ^ 1) << 7) | ((b6 ? 0x1Full : 0) << 2) | low) << 23) |
               (fraction << 19);
    default:
        return (sign << 15) |
               ((((b6 ^ 1) << 4) | ((b6 ? 3ull : 0) << 2) | low) << 10) |
               (fraction << 6);
    }
}

// A constant 'lane' of 'laneBits' replicated to fill a register of 'bits'
SemanticOperand replicate(ArmLiftContext& c, uint64 lane, uint laneBits, uint bits)
{
    SemanticOperand result = constant(c, lane, laneBits);
    for (uint filled = laneBits; filled < bits; filled += laneBits)
        result = c.emit(Op::OP_CONCAT, integerType(filled + laneBits), result,
                        constant(c, lane, laneBits));
    return result;
}

// Return the lower case text of a name operand
cString nameOf(ArmLiftContext& c, uint index)
{
    cString name(c.getOperand(index).m_name);
    name.makeLower();
    return name;
}

} // namespace

namespace armdis {
namespace a32 {

// vmov: the register moves, the immediates, the transfers to and from the
// core registers and the scalars of the d registers
void liftFpMove(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    const uint count = c.getOperandsCount();
    const ArmDecodedOperand& destination = c.getOperand(0);

    // The transfers of two core registers
    if (count == 3)
    {
        if (c.getOperand(1).m_kind == ArmDecodedOperand::KIND_REGISTER &&
            c.getOperand(0).m_register >= ARM_REG_Q0)
        {
            // vmov Dm, Rt, Rt2
            c.write(0, c.emit(Op::OP_CONCAT, integerType(64), c.read(1, integerType(32)),
                              c.read(2, integerType(32))));
        } else
        {
            // vmov Rt, Rt2, Dm
            const SemanticOperand d = readBits(c, 2);
            c.write(0, extractBits(c, d, 0, 32));
            c.write(1, extractBits(c, d, 32, 32));
        }
        return;
    }
    if (count == 4)
    {
        if (destination.m_register >= ARM_REG_Q0)
        {
            // vmov Sm, Sm1, Rt, Rt2
            c.write(0, c.read(2, integerType(32)));
            c.write(1, c.read(3, integerType(32)));
        } else
        {
            // vmov Rt, Rt2, Sm, Sm1
            const SemanticOperand a = readBits(c, 2);
            const SemanticOperand b = readBits(c, 3);
            c.write(0, a);
            c.write(1, b);
        }
        return;
    }

    const ArmDecodedOperand& source = c.getOperand(1);
    // The scalars of the d registers
    if (destination.m_elementIndex >= 0)
    {
        // vmov.32 / .16 / .8 Dd[x], Rt
        uint bits = 32;
        if (equals(m.m_parts[0], "16"))
            bits = 16;
        else if (equals(m.m_parts[0], "8"))
            bits = 8;
        const SemanticOperand lane = extractBits(c, c.read(1, integerType(32)), 0, bits);
        c.write(0, c.emit(Op::OP_INSERT, integerType(64), readBits(c, 0), lane,
                          constant(c, (uint)destination.m_elementIndex * bits, 32)));
        return;
    }
    if (source.m_elementIndex >= 0)
    {
        // vmov.32 Rt, Dn[x]; vmov.s8 / .u8 / .s16 / .u16 Rt, Dn[x]
        const char* type = m.m_parts[0];
        const bool isSigned = type[0] == 's';
        const char* digits = ((type[0] == 's') || (type[0] == 'u')) ? type + 1 : type;
        const uint bits = equals(digits, "8") ? 8 : (equals(digits, "16") ? 16 : 32);
        SemanticOperand lane = extractBits(c, readBits(c, 1),
                                           (uint)source.m_elementIndex * bits, bits);
        if (bits < 32)
            lane = c.emit(isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                          integerType(32), lane);
        c.write(0, lane);
        return;
    }

    // The immediates
    if (source.m_kind == ArmDecodedOperand::KIND_FLOAT_IMMEDIATE)
    {
        Format format;
        CHECK(formatOf(m.m_parts[0], format));
        const uint bits = bitsOf(format);
        const uint64 value = expandFloatImmediate8(format, (uint)source.m_immediate);
        c.write(0, replicate(c, value, bits, registerBits(c, 0) == 32 ? bits :
                                                 registerBits(c, 0)));
        return;
    }
    if (source.m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
    {
        bool isSigned;
        uint bits;
        CHECK(integerOf(m.m_parts[0], isSigned, bits));
        c.write(0, replicate(c, (uint64)source.m_immediate, bits, registerBits(c, 0)));
        return;
    }

    // The core register and the floating point register: the f16 form moves
    // the low half
    const bool isHalf = equals(m.m_parts[0], "f16");
    const bool isToCore = (destination.m_register >= ARM_REG_R0) &&
                          (destination.m_register <= ARM_REG_PC);
    if (isToCore)
    {
        SemanticOperand value = readBits(c, 1);
        if (isHalf)
            value = c.emit(Op::OP_ZERO_EXTEND, integerType(32),
                           extractBits(c, value, 0, 16));
        c.write(0, value);
        return;
    }
    if ((source.m_register >= ARM_REG_R0) && (source.m_register <= ARM_REG_PC))
    {
        SemanticOperand value = c.read(1, integerType(32));
        if (isHalf)
            value = c.emit(Op::OP_ZERO_EXTEND, integerType(32),
                           extractBits(c, value, 0, 16));
        c.write(0, value);
        return;
    }
    // vmov Sd, Sm / Dd, Dm
    c.write(0, readBits(c, 1));
}

// vmrs, vmsr
void liftFpStatus(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    if (equals(m.m_base, "vmrs"))
    {
        const cString special = nameOf(c, 1);
        uint registerId;
        if (special == cString("fpscr"))
        {
            registerId = ARM_REG_FPSCR;
        } else
        {
            // fpexc, fpsid and mvfr0-2 are not accessible from the user mode
            // (the privileged level is not modeled): UNDEFINED
            c.trap(Op::TRAP_UNDEFINED, 0);
            return;
        }
        const SemanticOperand value = c.getRegister(registerId);
        if (c.getOperand(0).m_kind == ArmDecodedOperand::KIND_NAME)
        {
            // vmrs APSR_nzcv, fpscr
            static const uint flags[4][2] = {
                { ARM_REG_N, 31 }, { ARM_REG_Z, 30 }, { ARM_REG_C, 29 }, { ARM_REG_V, 28 }
            };
            const SemanticOperand copy = c.emit(Op::OP_MOVE, integerType(32), value);
            for (uint i = 0; i < 4; i++)
                c.move(c.getRegister(flags[i][0]),
                       c.emit(Op::OP_TRUNCATE, integerType(1),
                              c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, integerType(32), copy,
                                     constant(c, flags[i][1], 32))));
            return;
        }
        c.write(0, value);
        return;
    }

    // vmsr special, Rt
    const cString special = nameOf(c, 0);
    SemanticOperand value = c.read(1, integerType(32));
    if (special == cString("fpscr"))
    {
        // The bits the processor implements: NZCV, QC, AHP, DN, FZ, RMode,
        // FZ16 and the cumulative exception flags
        c.move(c.getRegister(ARM_REG_FPSCR),
               c.emit(Op::OP_AND, integerType(32), value, constant(c, 0xFFC8009Fu, 32)));
    } else
    {
        // fpexc and fpsid are not accessible from the user mode
        c.trap(Op::TRAP_UNDEFINED, 0);
    }
}

// vldr, vstr
void liftFpMemory(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    const bool isLoad = equals(m.m_base, "vldr");
    uint bits = registerBits(c, 0);
    if (equals(m.m_parts[0], "16"))
        bits = 16;
    if (isLoad)
    {
        c.write(0, c.read(1, integerType(bits)));
    } else
    {
        c.write(1, extractBits(c, readBits(c, 0), 0, bits));
    }
}

// vldmia, vldmdb, vstmia, vstmdb, vpush, vpop, fldmiax, fldmdbx, fstmiax,
// fstmdbx
void liftFpMultiple(ArmLiftContext& c)
{
    const Mnemonic m = parseMnemonic(c);
    const char* name = m.m_base;
    const bool isLoad = equals(name, "vldmia") || equals(name, "vldmdb") ||
                        equals(name, "vpop") || equals(name, "fldmiax") ||
                        equals(name, "fldmdbx");
    const bool isDecrement = equals(name, "vldmdb") || equals(name, "vstmdb") ||
                             equals(name, "vpush") || equals(name, "fldmdbx") ||
                             equals(name, "fstmdbx");
    const bool isExtra = (name[0] == 'f');
    const bool isStack = equals(name, "vpush") || equals(name, "vpop");
    const uint listIndex = isStack ? 0 : 1;
    const ArmDecodedOperand& list = c.getOperand(listIndex);
    const uint baseRegister = isStack ? (uint)ARM_REG_SP : c.getOperand(0).m_register;
    const bool isWriteback = isStack ||
        (c.getOperand(0).m_addressing == ArmDecodedOperand::ADDRESSING_PRE_INDEX);
    const bool isSingle = list.m_register >= ARM_REG_S0;
    const uint size = isSingle ? 4 : 8;
    const uint regs = list.m_count;
    const uint total = size * regs + (isExtra ? 4 : 0);

    const SemanticType address = integerType(32);
    SemanticOperand base = c.emit(Op::OP_MOVE, address,
                                  baseRegister == ARM_REG_PC ? c.getPcValue() :
                                  c.getRegister(baseRegister));
    SemanticOperand start = base;
    if (isDecrement)
        start = c.emit(Op::OP_SUBTRACT, address, base, constant(c, total, 32));
    for (uint i = 0; i < regs; i++)
    {
        const SemanticOperand at = (i == 0) ? start :
            c.emit(Op::OP_ADD, address, start, constant(c, i * size, 32));
        const uint registerId = list.m_register + i * list.m_stride;
        if (isLoad)
        {
            const SemanticOperand value = c.load(at, integerType(size * 8));
            c.move(c.getRegister(registerId), value);
        } else
        {
            c.store(at, c.getRegister(registerId));
        }
    }
    if (isWriteback)
        c.move(c.getRegister(baseRegister),
               c.emit(isDecrement ? Op::OP_SUBTRACT : Op::OP_ADD, address, base,
                      constant(c, total, 32)));
}

} // namespace a32
} // namespace armdis

/////////////////////////////////////////////////////////////////////////////
// vjcvt
/////////////////////////////////////////////////////////////////////////////

namespace {

SemanticOperand op2(ArmLiftContext& c, Op::Operation operation, uint bits,
                    const SemanticOperand& a, const SemanticOperand& b)
{
    return c.emit(operation, integerType(bits), a, b);
}

SemanticOperand cmp(ArmLiftContext& c, Op::Condition condition, uint bits,
                    const SemanticOperand& a, const SemanticOperand& b)
{
    SemanticOperand result = c.newTemporary(integerType(1));
    SemanticInstruction instruction(Op::OP_COMPARE, integerType(bits), result, a, b);
    instruction.setCondition(condition);
    c.emit(instruction);
    return result;
}

SemanticOperand bnot(ArmLiftContext& c, const SemanticOperand& a)
{
    return c.emit(Op::OP_XOR, integerType(1), a, c.getImmediate(1, integerType(1)));
}

} // namespace

namespace armdis {
namespace a32 {

// vjcvt.s32.f64: the JavaScript conversion of a double to a 32 bit integer
// (FPToFixedJS): the value truncated, modulo 2^32. Z of the FPSCR flags says
// that the conversion was exact and in range; an inexact result raises IXC,
// a NaN, an infinity or an out of range one IOC
void liftFpJavascript(ArmLiftContext& c)
{
    const SemanticType i64 = integerType(64);
    const SemanticOperand bits = readBits(c, 1);
    const SemanticOperand e = op2(c, Op::OP_AND, 64,
        op2(c, Op::OP_SHIFT_RIGHT_LOGICAL, 64, bits, constant(c, 52, 64)),
        constant(c, 0x7FF, 64));
    const SemanticOperand frac = op2(c, Op::OP_AND, 64, bits,
                                     constant(c, ((uint64)1 << 52) - 1, 64));
    const SemanticOperand sign = c.emit(Op::OP_TRUNCATE, integerType(1),
        op2(c, Op::OP_SHIFT_RIGHT_LOGICAL, 64, bits, constant(c, 63, 64)));
    const SemanticOperand fpscr = c.getRegister(ARM_REG_FPSCR);
    const SemanticOperand flushEnabled = c.emit(Op::OP_TRUNCATE, integerType(1),
        op2(c, Op::OP_SHIFT_RIGHT_LOGICAL, 32, fpscr, constant(c, 24, 32)));

    const SemanticOperand isZeroExponent = cmp(c, Op::COND_EQUAL, 64, e, constant(c, 0, 64));
    const SemanticOperand fractionNonZero = cmp(c, Op::COND_NOT_EQUAL, 64, frac,
                                                constant(c, 0, 64));
    const SemanticOperand isDenormal = op2(c, Op::OP_AND, 1, isZeroExponent, fractionNonZero);
    const SemanticOperand isFlushed = op2(c, Op::OP_AND, 1, isDenormal, flushEnabled);
    const SemanticOperand nonZero = op2(c, Op::OP_OR, 1, bnot(c, isZeroExponent),
                                        fractionNonZero);
    const SemanticOperand effectiveNonZero = op2(c, Op::OP_AND, 1, nonZero,
                                                 bnot(c, isFlushed));
    const SemanticOperand mantissa = op2(c, Op::OP_OR, 64, frac,
                                         constant(c, (uint64)1 << 52, 64));
    const SemanticOperand isSmall = cmp(c, Op::COND_UNSIGNED_LESS, 64, e, constant(c, 1023, 64));
    const SemanticOperand shift = op2(c, Op::OP_SUBTRACT, 64, e, constant(c, 1023, 64));
    const SemanticOperand isRight = cmp(c, Op::COND_UNSIGNED_LESS_EQUAL, 64, shift,
                                        constant(c, 52, 64));
    // The magnitude: shifted right (the value has a fraction) or left
    const SemanticOperand rightCount = op2(c, Op::OP_SUBTRACT, 64, constant(c, 52, 64), shift);
    const SemanticOperand magnitudeRight = op2(c, Op::OP_SHIFT_RIGHT_LOGICAL, 64,
                                               mantissa, rightCount);
    const SemanticOperand lostMask = op2(c, Op::OP_SUBTRACT, 64,
        op2(c, Op::OP_SHIFT_LEFT, 64, constant(c, 1, 64), rightCount), constant(c, 1, 64));
    const SemanticOperand lost = cmp(c, Op::COND_NOT_EQUAL, 64,
        op2(c, Op::OP_AND, 64, mantissa, lostMask), constant(c, 0, 64));
    const SemanticOperand magnitudeLeft = op2(c, Op::OP_SHIFT_LEFT, 64, mantissa,
        op2(c, Op::OP_SUBTRACT, 64, shift, constant(c, 52, 64)));
    SemanticOperand magnitude = c.emit(Op::OP_SELECT, i64, isRight, magnitudeRight, magnitudeLeft);
    magnitude = c.emit(Op::OP_SELECT, i64, isSmall, constant(c, 0, 64), magnitude);
    const SemanticOperand falseBit = c.getImmediate(0, integerType(1));
    SemanticOperand error = c.emit(Op::OP_SELECT, integerType(1), isRight, lost, falseBit);
    error = c.emit(Op::OP_SELECT, integerType(1), isSmall, effectiveNonZero, error);

    // The range: the truncated value has to fit a signed 32 bit integer
    const SemanticOperand isBig = cmp(c, Op::COND_UNSIGNED_GREATER_EQUAL, 64, shift,
                                      constant(c, 32, 64));
    const SemanticOperand is31 = cmp(c, Op::COND_EQUAL, 64, shift, constant(c, 31, 64));
    const SemanticOperand isMinimum = op2(c, Op::OP_AND, 1, sign,
        cmp(c, Op::COND_EQUAL, 64,
            op2(c, Op::OP_SHIFT_RIGHT_LOGICAL, 64, mantissa, constant(c, 21, 64)),
            constant(c, (uint64)1 << 31, 64)));
    const SemanticOperand overflow = op2(c, Op::OP_AND, 1, bnot(c, isSmall),
        op2(c, Op::OP_OR, 1, isBig, op2(c, Op::OP_AND, 1, is31, bnot(c, isMinimum))));
    const SemanticOperand isSpecial = cmp(c, Op::COND_EQUAL, 64, e, constant(c, 0x7FF, 64));

    const SemanticOperand low = c.emit(Op::OP_TRUNCATE, integerType(32), magnitude);
    SemanticOperand result = c.emit(Op::OP_SELECT, integerType(32), sign,
                                    c.emit(Op::OP_NEGATE, integerType(32), low), low);
    result = c.emit(Op::OP_SELECT, integerType(32), isSpecial, constant(c, 0, 32), result);

    const SemanticOperand inexact = op2(c, Op::OP_AND, 1, error, bnot(c, overflow));
    const SemanticOperand exact = bnot(c, op2(c, Op::OP_OR, 1, overflow, error));
    SemanticOperand value = op2(c, Op::OP_AND, 32, fpscr, constant(c, 0x0FFFFFFFu, 32));
    value = op2(c, Op::OP_OR, 32, value, placeBit(c, exact, 30));
    value = op2(c, Op::OP_OR, 32, value, placeBit(c, overflow, 0));
    value = op2(c, Op::OP_OR, 32, value, placeBit(c, inexact, 4));
    value = op2(c, Op::OP_OR, 32, value, placeBit(c, isFlushed, 7));
    c.move(fpscr, value);
    c.write(0, result);
}

} // namespace a32
} // namespace armdis
