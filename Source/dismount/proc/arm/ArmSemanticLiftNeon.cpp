/*
 * ArmSemanticLiftNeon.cpp
 *
 * Implementation file
 *
 * The lifters of the AArch32 Advanced SIMD integer instructions, see
 * ArmSemanticTableNeon.inc. Every instruction works lane by lane on scalar
 * integers: a lane is extracted from the register, computed in the width the
 * Arm ARM pseudocode uses (the exact integer of the pseudocode is an i64 for
 * the lanes of up to 32 bits, the 64 bit lanes use the saturating and
 * rounding operations of the IR), and the lanes are packed back. The
 * saturating instructions accumulate "some lane saturated" into the QC bit
 * of fpscr.
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
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/ArmSemanticInternal.h"
#include "dismount/proc/arm/ArmSemanticNeon.h"

using namespace armdis;

typedef SemanticOperation Op;
typedef SemanticOperand Val;

namespace armdis {
namespace a32 {

/////////////////////////////////////////////////////////////////////////////
// NeonName
/////////////////////////////////////////////////////////////////////////////

NeonName::NeonName(const ArmLiftContext& context) :
    m_kind(0),
    m_bits(0)
{
    const char* mnemonic = getMnemonic(context);
    const char* dot = strchr(mnemonic, '.');
    size_t length = (dot == NULL) ? strlen(mnemonic) : (size_t)(dot - mnemonic);
    if (length >= sizeof(m_base))
        length = sizeof(m_base) - 1;
    memcpy(m_base, mnemonic, length);
    m_base[length] = 0;
    if (dot == NULL)
        return;
    // The last component: "s16" of "vcvt.s32.s16", "8" of "vcnt.8"
    const char* last = strrchr(mnemonic, '.') + 1;
    if ((*last >= 'a') && (*last <= 'z'))
    {
        m_kind = *last;
        last++;
    }
    while ((*last >= '0') && (*last <= '9'))
    {
        m_bits = m_bits * 10 + (uint)(*last - '0');
        last++;
    }
}

bool NeonName::is(const char* name) const
{
    return strcmp(m_base, name) == 0;
}

/////////////////////////////////////////////////////////////////////////////
// NeonBuilder
/////////////////////////////////////////////////////////////////////////////

int64 signedMaximum(uint bits)
{
    if (bits >= 64)
        return (int64)(((uint64)1 << 63) - 1);
    return (int64)(((uint64)1 << (bits - 1)) - 1);
}

int64 signedMinimum(uint bits)
{
    return -signedMaximum(bits) - 1;
}

uint64 unsignedMaximum(uint bits)
{
    if (bits >= 64)
        return ~(uint64)0;
    return ((uint64)1 << bits) - 1;
}

NeonBuilder::NeonBuilder(ArmLiftContext& context) :
    m_context(context)
{
}

SemanticType NeonBuilder::type(uint bits)
{
    return SemanticType::integer(bits);
}

Val NeonBuilder::imm(int64 value, uint bits)
{
    return m_context.getImmediate(value, type(bits));
}

Val NeonBuilder::op(Op::Operation operation, uint bits, const Val& a,
                    const Val& b, const Val& c)
{
    return m_context.emit(operation, type(bits), a, b, c);
}

Val NeonBuilder::ext(const Val& value, uint bits, bool isSigned)
{
    const uint from = value.getType().getTotalBits();
    if (from == bits)
        return value;
    if (from > bits)
        return m_context.emit(Op::OP_TRUNCATE, type(bits), value);
    return m_context.emit(isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                          type(bits), value);
}

Val NeonBuilder::compare(Op::Condition condition, const Val& a, const Val& b)
{
    Val result = m_context.newTemporary(SemanticType::integer(1));
    SemanticInstruction instruction(Op::OP_COMPARE, a.getType(), result, a, b);
    instruction.setCondition(condition);
    m_context.emit(instruction);
    return result;
}

Val NeonBuilder::copy(const Val& value)
{
    return m_context.emit(Op::OP_MOVE, value.getType(), value);
}

Val NeonBuilder::lane(const Val& value, uint index, uint bits)
{
    if ((value.getType().getTotalBits() == bits) && (index == 0))
        return value;
    return m_context.emit(Op::OP_EXTRACT, type(bits), value,
                          m_context.getImmediate((int64)index * bits,
                                                 SemanticType::integer(32)));
}

Val NeonBuilder::pack(const Val* lanes, uint count, uint bits)
{
    if (count == 1)
        return lanes[0];
    const uint total = count * bits;
    Val value = imm(0, total);
    for (uint i = 0; i < count; i++)
        value = op(Op::OP_INSERT, total, value, lanes[i],
                   m_context.getImmediate((int64)i * bits,
                                          SemanticType::integer(32)));
    return value;
}

Val NeonBuilder::insert(const Val& value, uint index, uint bits,
                        const Val& laneValue)
{
    return op(Op::OP_INSERT, value.getType().getTotalBits(), value, laneValue,
              m_context.getImmediate((int64)index * bits,
                                     SemanticType::integer(32)));
}

Val NeonBuilder::reg(uint index)
{
    return m_context.read(index);
}

uint NeonBuilder::regBits(uint index)
{
    const uint id = m_context.getOperand(index).m_register;
    return ((id >= ARM_REG_Q0) && (id <= ARM_REG_Q15)) ? 128 : 64;
}

Val NeonBuilder::clampSigned(const Val& value, int64 low, int64 high)
{
    const uint bits = value.getType().getTotalBits();
    Val r = op(Op::OP_MAXIMUM_SIGNED, bits, value, imm(low, bits));
    r = op(Op::OP_MINIMUM_SIGNED, bits, r, imm(high, bits));
    orSaturation(compare(Op::COND_NOT_EQUAL, r, value));
    return r;
}

Val NeonBuilder::clampUnsigned(const Val& value, uint64 high)
{
    // 'value' is the exact result as a signed number of the width
    const uint bits = value.getType().getTotalBits();
    Val r = op(Op::OP_MAXIMUM_SIGNED, bits, value, imm(0, bits));
    r = op(Op::OP_MINIMUM_UNSIGNED, bits, r, imm((int64)high, bits));
    orSaturation(compare(Op::COND_NOT_EQUAL, r, value));
    return r;
}

void NeonBuilder::orSaturation(const Val& bit)
{
    if (!m_saturation.isValid())
        m_saturation = bit;
    else
        m_saturation = op(Op::OP_OR, 1, m_saturation, bit);
}

void NeonBuilder::commitSaturation()
{
    if (!m_saturation.isValid())
        return;
    // QC is bit 27 of fpscr and sticky
    Val flag = op(Op::OP_ZERO_EXTEND, 32, m_saturation);
    flag = op(Op::OP_SHIFT_LEFT, 32, flag, imm(27, 32));
    Val fpscr = m_context.getRegister(ARM_REG_FPSCR);
    m_context.move(fpscr, op(Op::OP_OR, 32, fpscr, flag));
}

} // namespace a32
} // namespace armdis

/////////////////////////////////////////////////////////////////////////////
// The lifters
/////////////////////////////////////////////////////////////////////////////

using namespace armdis::a32;

namespace {

// True if the operand is a scalar element (d0[1]) and not a whole register
bool isScalar(const ArmDecodedOperand& operand)
{
    return (operand.m_kind == ArmDecodedOperand::KIND_REGISTER) &&
           (operand.m_elementIndex >= 0);
}

// The value of the scalar operand as a lane of 'bits' bits
Val readScalar(NeonBuilder& b, uint index, uint bits)
{
    const ArmDecodedOperand& operand = b.m_context.getOperand(index);
    return b.lane(b.m_context.getRegister(operand.m_register),
                  (uint)operand.m_elementIndex, bits);
}

// Write the lanes to the destination operand 0
void writeLanes(NeonBuilder& b, const Val* lanes, uint count, uint bits)
{
    b.m_context.write(0, b.pack(lanes, count, bits));
}

// The sign: all ones for a true compare
Val compareMask(NeonBuilder& b, const Val& flag, uint bits)
{
    return b.ext(flag, bits, true);
}

// The result of a lane operation of the same width
Val sameWidthLane(NeonBuilder& b, const NeonName& n, uint e, bool isSigned,
                  const Val& x, const Val& y, const Val& accumulator)
{
    if (n.is("vadd"))
        return b.op(Op::OP_ADD, e, x, y);
    if (n.is("vsub"))
        return b.op(Op::OP_SUBTRACT, e, x, y);
    if (n.is("vmul"))
        return b.op(n.m_kind == 'p' ? Op::OP_CARRYLESS_MULTIPLY : Op::OP_MULTIPLY,
                    e, x, y);
    if (n.is("vmla"))
        return b.op(Op::OP_ADD, e, accumulator, b.op(Op::OP_MULTIPLY, e, x, y));
    if (n.is("vmls"))
        return b.op(Op::OP_SUBTRACT, e, accumulator,
                    b.op(Op::OP_MULTIPLY, e, x, y));
    if (n.is("vabd"))
        return b.op(isSigned ? Op::OP_ABSOLUTE_DIFFERENCE_SIGNED :
                               Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, e, x, y);
    if (n.is("vaba"))
        return b.op(Op::OP_ADD, e, accumulator,
                    b.op(isSigned ? Op::OP_ABSOLUTE_DIFFERENCE_SIGNED :
                                    Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, e, x, y));
    if (n.is("vhadd") || n.is("vrhadd") || n.is("vhsub"))
    {
        // The exact sum / difference (the lanes are at most 32 bits), halved
        Val sum = b.op(n.is("vhsub") ? Op::OP_SUBTRACT : Op::OP_ADD, 64,
                       b.ext(x, 64, isSigned), b.ext(y, 64, isSigned));
        if (n.is("vrhadd"))
            sum = b.op(Op::OP_ADD, 64, sum, b.imm(1, 64));
        return b.ext(b.op(Op::OP_SHIFT_RIGHT_ARITHMETIC, 64, sum, b.imm(1, 64)),
                     e, false);
    }
    if (n.is("vmax"))
        return b.op(isSigned ? Op::OP_MAXIMUM_SIGNED : Op::OP_MAXIMUM_UNSIGNED,
                    e, x, y);
    if (n.is("vmin"))
        return b.op(isSigned ? Op::OP_MINIMUM_SIGNED : Op::OP_MINIMUM_UNSIGNED,
                    e, x, y);
    if (n.is("vqadd") || n.is("vqsub"))
    {
        const bool isAdd = n.is("vqadd");
        Val saturated = b.op(isAdd ?
            (isSigned ? Op::OP_ADD_SATURATE_SIGNED : Op::OP_ADD_SATURATE_UNSIGNED) :
            (isSigned ? Op::OP_SUBTRACT_SATURATE_SIGNED :
                        Op::OP_SUBTRACT_SATURATE_UNSIGNED), e, x, y);
        Val wrapped = b.op(isAdd ? Op::OP_ADD : Op::OP_SUBTRACT, e, x, y);
        b.orSaturation(b.compare(Op::COND_NOT_EQUAL, saturated, wrapped));
        return saturated;
    }
    if (n.is("vtst"))
        return compareMask(b, b.compare(Op::COND_NOT_EQUAL,
                           b.op(Op::OP_AND, e, x, y), b.imm(0, e)), e);
    if (n.is("vceq"))
        return compareMask(b, b.compare(Op::COND_EQUAL, x, y), e);
    if (n.is("vcge"))
        return compareMask(b, b.compare(isSigned ? Op::COND_SIGNED_GREATER_EQUAL :
                                        Op::COND_UNSIGNED_GREATER_EQUAL, x, y), e);
    if (n.is("vcgt"))
        return compareMask(b, b.compare(isSigned ? Op::COND_SIGNED_GREATER :
                                        Op::COND_UNSIGNED_GREATER, x, y), e);
    if (n.is("vcle"))
        return compareMask(b, b.compare(Op::COND_SIGNED_LESS_EQUAL, x, y), e);
    if (n.is("vclt"))
        return compareMask(b, b.compare(Op::COND_SIGNED_LESS, x, y), e);
    CHECK_FAIL();
}

} // namespace

namespace armdis {
namespace a32 {

/////////////////////////////////////////////////////////////////////////////
// Same size operations: vadd, vsub, vmul, vmla, vmls, vabd, vaba, vhadd,
// vrhadd, vhsub, vmax, vmin, vqadd, vqsub, vtst, vceq, vcge, vcgt, vcle, vclt
// (also by scalar for vmul, vmla, vmls)
/////////////////////////////////////////////////////////////////////////////

void liftNeonBinary(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint e = n.m_bits;
    const bool isSigned = n.isSigned();
    const uint size = b.regBits(0);
    const uint lanes = size / e;
    const ArmDecodedOperand& second = c.getOperand(2);
    const bool isByScalar = isScalar(second);
    const bool isZero = second.m_kind == ArmDecodedOperand::KIND_NAME;

    Val first = b.reg(1);
    Val other;
    if (!isByScalar && !isZero)
        other = b.reg(2);
    Val accumulator = b.reg(0);
    Val scalar;
    if (isByScalar)
        scalar = readScalar(b, 2, e);

    Val result[16];
    for (uint i = 0; i < lanes; i++)
    {
        const Val x = b.lane(first, i, e);
        Val y;
        if (isByScalar)
            y = scalar;
        else if (isZero)
            y = b.imm(0, e);
        else
            y = b.lane(other, i, e);
        result[i] = sameWidthLane(b, n, e, isSigned, x, y, b.lane(accumulator, i, e));
    }
    writeLanes(b, result, lanes, e);
    b.commitSaturation();
}

/////////////////////////////////////////////////////////////////////////////
// Whole register logic: vand, vbic, vorr, vorn, veor, vbsl, vbit, vbif
/////////////////////////////////////////////////////////////////////////////

void liftNeonLogic(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint size = b.regBits(0);
    // The immediate forms of vorr and vbic
    if (c.getOperand(1).m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
    {
        liftNeonImmediateLogic(c);
        return;
    }
    const Val d = b.reg(0);
    const Val x = b.reg(1);
    const Val y = b.reg(2);
    Val result;
    if (n.is("vand"))
        result = b.op(Op::OP_AND, size, x, y);
    else if (n.is("vbic"))
        result = b.op(Op::OP_AND_NOT, size, y, x);
    else if (n.is("vorr"))
        result = b.op(Op::OP_OR, size, x, y);
    else if (n.is("vorn"))
        result = b.op(Op::OP_OR, size, x, b.op(Op::OP_NOT, size, y));
    else if (n.is("veor"))
        result = b.op(Op::OP_XOR, size, x, y);
    else if (n.is("vbsl"))
        // (d & n) | (~d & m)
        result = b.op(Op::OP_BITWISE_SELECT, size, d, x, y);
    else if (n.is("vbit"))
        // (n & m) | (d & ~m)
        result = b.op(Op::OP_BITWISE_SELECT, size, y, x, d);
    else
        // vbif: (d & m) | (n & ~m)
        result = b.op(Op::OP_BITWISE_SELECT, size, y, d, x);
    c.write(0, result);
}

// vorr, vbic and vmvn with a modified immediate
void liftNeonImmediateLogic(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint size = b.regBits(0);
    const uint e = (n.m_bits == 0) ? 32 : n.m_bits;
    const uint64 value = (uint64)c.getOperand(1).m_immediate;
    const Val element = b.imm((int64)(value & unsignedMaximum(e)), e);
    Val lanes[16];
    const uint count = size / e;
    for (uint i = 0; i < count; i++)
        lanes[i] = element;
    const Val pattern = b.pack(lanes, count, e);
    Val result;
    if (n.is("vorr"))
        result = b.op(Op::OP_OR, size, b.reg(0), pattern);
    else if (n.is("vbic"))
        result = b.op(Op::OP_AND_NOT, size, pattern, b.reg(0));
    else
        result = b.op(Op::OP_NOT, size, pattern);
    c.write(0, result);
}

/////////////////////////////////////////////////////////////////////////////
// Two register operations: vabs, vneg, vqabs, vqneg, vcls, vclz, vcnt, vmvn
/////////////////////////////////////////////////////////////////////////////

void liftNeonUnary(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint size = b.regBits(0);
    if (n.is("vmvn"))
    {
        if (c.getOperand(1).m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
        {
            liftNeonImmediateLogic(c);
            return;
        }
        c.write(0, b.op(Op::OP_NOT, size, b.reg(1)));
        return;
    }
    const uint e = n.m_bits;
    const uint lanes = size / e;
    const Val source = b.reg(1);
    Val result[16];
    for (uint i = 0; i < lanes; i++)
    {
        const Val x = b.lane(source, i, e);
        if (n.is("vabs"))
        {
            result[i] = b.op(Op::OP_ABSOLUTE, e, x);
        } else if (n.is("vneg"))
        {
            result[i] = b.op(Op::OP_NEGATE, e, x);
        } else if (n.is("vqabs") || n.is("vqneg"))
        {
            const Val isMinimum = b.compare(Op::COND_EQUAL, x,
                                            b.imm(signedMinimum(e), e));
            b.orSaturation(isMinimum);
            const Val plain = b.op(n.is("vqabs") ? Op::OP_ABSOLUTE : Op::OP_NEGATE,
                                   e, x);
            result[i] = b.op(Op::OP_SELECT, e, isMinimum,
                             b.imm(signedMaximum(e), e), plain);
        } else if (n.is("vcls"))
        {
            result[i] = b.op(Op::OP_COUNT_LEADING_SIGN_BITS, e, x);
        } else if (n.is("vclz"))
        {
            result[i] = b.op(Op::OP_COUNT_LEADING_ZEROS, e, x);
        } else
        {
            // vcnt
            result[i] = b.op(Op::OP_POPULATION_COUNT, e, x);
        }
    }
    writeLanes(b, result, lanes, e);
    b.commitSaturation();
}

/////////////////////////////////////////////////////////////////////////////
// The long, wide and narrow three register operations
/////////////////////////////////////////////////////////////////////////////

void liftNeonLong(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint e = n.m_bits;
    const bool isSigned = n.isSigned();
    const uint w = (n.m_kind == 'p' && e == 64) ? 128 : 2 * e;
    const ArmDecodedOperand& second = c.getOperand(2);
    const bool isByScalar = isScalar(second);

    // vmull.p64: the 64 x 64 carry-less product
    if (n.m_kind == 'p' && e == 64)
    {
        liftNeonPolynomial64(c);
        return;
    }
    const bool isWide = n.is("vaddw") || n.is("vsubw");
    const Val first = b.reg(1);
    const Val other = isByScalar ? Val() : b.reg(2);
    const Val accumulator = b.reg(0);
    const Val scalar = isByScalar ? readScalar(b, 2, e) : Val();
    const uint lanes = 64 / e;
    Val result[8];
    for (uint i = 0; i < lanes; i++)
    {
        // The first operand is a q register for the wide forms
        Val x = isWide ? b.lane(first, i, w) : b.ext(b.lane(first, i, e), w, isSigned);
        Val y = isByScalar ? b.ext(scalar, w, isSigned) :
                             b.ext(b.lane(other, i, e), w, isSigned);
        const Val acc = b.lane(accumulator, i, w);
        if (n.is("vaddl") || n.is("vaddw"))
        {
            result[i] = b.op(Op::OP_ADD, w, x, y);
        } else if (n.is("vsubl") || n.is("vsubw"))
        {
            result[i] = b.op(Op::OP_SUBTRACT, w, x, y);
        } else if (n.is("vabdl") || n.is("vabal"))
        {
            Val d = b.op(isSigned ? Op::OP_ABSOLUTE_DIFFERENCE_SIGNED :
                                    Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, w, x, y);
            result[i] = n.is("vabal") ? b.op(Op::OP_ADD, w, acc, d) : d;
        } else if (n.is("vmull") && n.m_kind == 'p')
        {
            result[i] = b.op(Op::OP_CARRYLESS_MULTIPLY, w, x, y);
        } else
        {
            // vmull, vmlal, vmlsl
            Val product = b.op(Op::OP_MULTIPLY, w, x, y);
            if (n.is("vmlal"))
                result[i] = b.op(Op::OP_ADD, w, acc, product);
            else if (n.is("vmlsl"))
                result[i] = b.op(Op::OP_SUBTRACT, w, acc, product);
            else
                result[i] = product;
        }
    }
    writeLanes(b, result, lanes, w);
}

// vaddhn, vraddhn, vsubhn, vrsubhn: the high half of the sum, narrowed
void liftNeonNarrowHigh(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint w = n.m_bits;
    const uint e = w / 2;
    const bool isRounding = n.is("vraddhn") || n.is("vrsubhn");
    const bool isSubtract = n.is("vsubhn") || n.is("vrsubhn");
    const Val first = b.reg(1);
    const Val other = b.reg(2);
    const uint lanes = 128 / w;
    Val result[8];
    for (uint i = 0; i < lanes; i++)
    {
        Val sum = b.op(isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD, w,
                       b.lane(first, i, w), b.lane(other, i, w));
        if (isRounding)
            sum = b.op(Op::OP_ADD, w, sum, b.imm((int64)1 << (e - 1), w));
        result[i] = b.ext(b.op(Op::OP_SHIFT_RIGHT_LOGICAL, w, sum, b.imm(e, w)),
                          e, false);
    }
    writeLanes(b, result, lanes, e);
}

/////////////////////////////////////////////////////////////////////////////
// vqdmulh, vqrdmulh, vqrdmlah, vqrdmlsh, vqdmull, vqdmlal, vqdmlsl
/////////////////////////////////////////////////////////////////////////////

void liftNeonSaturatingDouble(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint e = n.m_bits;
    const bool isLong = n.is("vqdmull") || n.is("vqdmlal") || n.is("vqdmlsl");
    const ArmDecodedOperand& second = c.getOperand(2);
    const bool isByScalar = isScalar(second);
    const uint size = isLong ? 64 : b.regBits(0);
    const uint lanes = size / e;
    const Val first = b.reg(1);
    const Val other = isByScalar ? Val() : b.reg(2);
    const Val scalar = isByScalar ? readScalar(b, 2, e) : Val();
    const Val accumulator = b.reg(0);
    const uint w = 2 * e;
    Val result[8];
    for (uint i = 0; i < lanes; i++)
    {
        const Val x = b.ext(b.lane(first, i, e), 64, true);
        const Val y = b.ext(isByScalar ? scalar : b.lane(other, i, e), 64, true);
        const Val product = b.op(Op::OP_MULTIPLY, 64, x, y);
        if (isLong)
        {
            // 2 * a * b saturated to 2e bits: only minimum * minimum of the
            // 32 bit lanes overflows the 64 bits of the doubling
            Val doubled;
            if (e == 32)
            {
                const Val isOverflow = b.compare(Op::COND_EQUAL, product,
                                                 b.imm((int64)1 << 62, 64));
                b.orSaturation(isOverflow);
                doubled = b.op(Op::OP_SELECT, 64, isOverflow,
                               b.imm(signedMaximum(64), 64),
                               b.op(Op::OP_SHIFT_LEFT, 64, product, b.imm(1, 64)));
            } else
            {
                doubled = b.clampSigned(b.op(Op::OP_SHIFT_LEFT, 64, product,
                                             b.imm(1, 64)),
                                        signedMinimum(w), signedMaximum(w));
            }
            Val value = b.ext(doubled, w, false);
            if (!n.is("vqdmull"))
            {
                const Val acc = b.lane(accumulator, i, w);
                const bool isAdd = n.is("vqdmlal");
                const Val saturated = b.op(isAdd ? Op::OP_ADD_SATURATE_SIGNED :
                                           Op::OP_SUBTRACT_SATURATE_SIGNED, w,
                                           acc, value);
                const Val wrapped = b.op(isAdd ? Op::OP_ADD : Op::OP_SUBTRACT, w,
                                         acc, value);
                b.orSaturation(b.compare(Op::COND_NOT_EQUAL, saturated, wrapped));
                value = saturated;
            }
            result[i] = value;
        } else
        {
            Val exact;
            if (n.is("vqdmulh"))
            {
                // (2 * a * b) >> e = (a * b) >> (e - 1)
                exact = b.op(Op::OP_SHIFT_RIGHT_ARITHMETIC, 64, product,
                             b.imm(e - 1, 64));
            } else if (n.is("vqrdmulh"))
            {
                exact = b.op(Op::OP_SHIFT_RIGHT_ARITHMETIC, 64,
                             b.op(Op::OP_ADD, 64, product,
                                  b.imm((int64)1 << (e - 2), 64)),
                             b.imm(e - 1, 64));
            } else
            {
                // vqrdmlah / vqrdmlsh: ((acc << (e - 1)) +/- a * b + 2^(e-2))
                // >> (e - 1)
                const Val acc = b.ext(b.lane(accumulator, i, e), 64, true);
                Val value = b.op(Op::OP_SHIFT_LEFT, 64, acc, b.imm(e - 1, 64));
                value = b.op(n.is("vqrdmlah") ? Op::OP_ADD : Op::OP_SUBTRACT, 64,
                             value, product);
                value = b.op(Op::OP_ADD, 64, value, b.imm((int64)1 << (e - 2), 64));
                exact = b.op(Op::OP_SHIFT_RIGHT_ARITHMETIC, 64, value,
                             b.imm(e - 1, 64));
            }
            result[i] = b.ext(b.clampSigned(exact, signedMinimum(e),
                                            signedMaximum(e)), e, false);
        }
    }
    writeLanes(b, result, lanes, isLong ? w : e);
    b.commitSaturation();
}

/////////////////////////////////////////////////////////////////////////////
// The shifts by an immediate
/////////////////////////////////////////////////////////////////////////////

namespace {

// The exact shifted value (an i64) and the rounding, for the right shifts
Val shiftRightExact(NeonBuilder& b, const Val& x, uint amount, bool isRounding,
                    bool isSigned)
{
    if (isRounding)
    {
        SemanticInstruction instruction(Op::OP_SHIFT_RIGHT_ROUNDING,
                                        NeonBuilder::type(64),
                                        b.m_context.newTemporary(NeonBuilder::type(64)),
                                        x, b.imm(amount, 64));
        instruction.setBehavior(isSigned ? Op::BEHAVIOR_SIGNED :
                                           Op::BEHAVIOR_UNSIGNED);
        b.m_context.emit(instruction);
        return instruction.getDestination();
    }
    return b.op(isSigned ? Op::OP_SHIFT_RIGHT_ARITHMETIC :
                           Op::OP_SHIFT_RIGHT_LOGICAL, 64, x, b.imm(amount, 64));
}

// The left shift with saturation: the result of 'e' bits, and QC. 'x' is the
// lane extended to 64 bits (signed: isSigned), 'amount' an i64 >= 0
Val shiftLeftSaturating(NeonBuilder& b, const Val& x, const Val& amount, uint e,
                        bool isSigned, bool isUnsignedResult, Val* isSaturated)
{
    const Val shifted = b.op(Op::OP_SHIFT_LEFT, 64, x, amount);
    // (vqshlu: the result is unsigned, a positive value may use all 64 bits)
    const Val back = b.op((isSigned && !isUnsignedResult) ?
                          Op::OP_SHIFT_RIGHT_ARITHMETIC :
                          Op::OP_SHIFT_RIGHT_LOGICAL, 64, shifted, amount);
    // The shift lost bits of the 64: overflow
    const Val isOverflow = b.compare(Op::COND_NOT_EQUAL, back, x);
    Val clamped;
    Val outOfRange;
    if (isSigned && !isUnsignedResult)
    {
        clamped = b.op(Op::OP_MINIMUM_SIGNED, 64,
                       b.op(Op::OP_MAXIMUM_SIGNED, 64, shifted,
                            b.imm(signedMinimum(e), 64)), b.imm(signedMaximum(e), 64));
        const Val isNegative = b.compare(Op::COND_SIGNED_LESS, x, b.imm(0, 64));
        const Val limit = b.op(Op::OP_SELECT, 64, isNegative,
                               b.imm(signedMinimum(e), 64),
                               b.imm(signedMaximum(e), 64));
        outOfRange = b.compare(Op::COND_NOT_EQUAL, clamped, shifted);
        clamped = b.op(Op::OP_SELECT, 64, isOverflow, limit, clamped);
    } else if (isSigned)
    {
        // vqshlu: a signed input, an unsigned result
        const Val isNegative = b.compare(Op::COND_SIGNED_LESS, x, b.imm(0, 64));
        const Val positive = b.op(Op::OP_MINIMUM_UNSIGNED, 64, shifted,
                                  b.imm((int64)unsignedMaximum(e), 64));
        // x < 0 gives 0 (the saturation) unless x is 0
        clamped = b.op(Op::OP_SELECT, 64, isNegative, b.imm(0, 64),
                       b.op(Op::OP_SELECT, 64, isOverflow,
                            b.imm((int64)unsignedMaximum(e), 64), positive));
        outOfRange = b.compare(Op::COND_NOT_EQUAL, clamped, shifted);
        // A negative nonzero input saturates; the exact check above is
        // enough since clamped is 0 while shifted is negative
    } else
    {
        clamped = b.op(Op::OP_MINIMUM_UNSIGNED, 64, shifted,
                       b.imm((int64)unsignedMaximum(e), 64));
        outOfRange = b.compare(Op::COND_NOT_EQUAL, clamped, shifted);
        clamped = b.op(Op::OP_SELECT, 64, isOverflow,
                       b.imm((int64)unsignedMaximum(e), 64), clamped);
    }
    *isSaturated = b.op(Op::OP_OR, 1, isOverflow, outOfRange);
    return clamped;
}

} // namespace

void liftNeonShiftImmediate(ArmLiftContext& c)
{
    // vshl and vqshl by a register
    if (c.getOperand(2).m_kind == ArmDecodedOperand::KIND_REGISTER)
    {
        liftNeonShiftRegister(c);
        return;
    }
    NeonBuilder b(c);
    NeonName n(c);
    const uint e = n.m_bits;
    const bool isSigned = n.isSigned();
    const uint size = b.regBits(0);
    const uint lanes = size / e;
    const uint amount = (uint)c.getOperand(2).m_immediate;
    const Val source = b.reg(1);
    const Val destination = b.reg(0);
    Val result[16];
    for (uint i = 0; i < lanes; i++)
    {
        const Val x = b.lane(source, i, e);
        if (n.is("vshr") || n.is("vsra") || n.is("vrshr") || n.is("vrsra"))
        {
            const bool isRounding = n.is("vrshr") || n.is("vrsra");
            Val shifted = b.ext(shiftRightExact(b, b.ext(x, 64, isSigned), amount,
                                                isRounding, isSigned), e, false);
            if (n.is("vsra") || n.is("vrsra"))
                shifted = b.op(Op::OP_ADD, e, b.lane(destination, i, e), shifted);
            result[i] = shifted;
        } else if (n.is("vshl"))
        {
            result[i] = b.op(Op::OP_SHIFT_LEFT, e, x, b.imm(amount, e));
        } else if (n.is("vqshl") || n.is("vqshlu"))
        {
            Val saturated;
            const Val value = shiftLeftSaturating(b, b.ext(x, 64, isSigned),
                                                  b.imm(amount, 64), e, isSigned,
                                                  n.is("vqshlu"), &saturated);
            b.orSaturation(saturated);
            result[i] = b.ext(value, e, false);
        } else if (n.is("vsli"))
        {
            // The shifted bits replace the ones of the destination above the
            // shift
            const uint64 mask = (unsignedMaximum(e) << amount) & unsignedMaximum(e);
            const Val old = b.lane(destination, i, e);
            const Val shifted = b.op(Op::OP_SHIFT_LEFT, e, x, b.imm(amount, e));
            result[i] = b.op(Op::OP_OR, e,
                             b.op(Op::OP_AND, e, old, b.imm((int64)~mask, e)),
                             b.op(Op::OP_AND, e, shifted, b.imm((int64)mask, e)));
        } else
        {
            // vsri
            const uint64 mask = (amount >= e) ? 0 : (unsignedMaximum(e) >> amount);
            const Val old = b.lane(destination, i, e);
            const Val shifted = b.op(Op::OP_SHIFT_RIGHT_LOGICAL, e, x,
                                     b.imm(amount, e));
            result[i] = b.op(Op::OP_OR, e,
                             b.op(Op::OP_AND, e, old, b.imm((int64)~mask, e)),
                             b.op(Op::OP_AND, e, shifted, b.imm((int64)mask, e)));
        }
    }
    writeLanes(b, result, lanes, e);
    b.commitSaturation();
}

/////////////////////////////////////////////////////////////////////////////
// The shifts by a register: vshl, vrshl, vqshl, vqrshl
/////////////////////////////////////////////////////////////////////////////

void liftNeonShiftRegister(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint e = n.m_bits;
    const bool isSigned = n.isSigned();
    const bool isRounding = n.is("vrshl") || n.is("vqrshl");
    const bool isSaturating = n.is("vqshl") || n.is("vqrshl");
    const uint size = b.regBits(0);
    const uint lanes = size / e;
    const Val value = b.reg(1);
    const Val amounts = b.reg(2);
    Val result[16];
    for (uint i = 0; i < lanes; i++)
    {
        const Val x = b.ext(b.lane(value, i, e), 64, isSigned);
        // The shift is the signed low byte of the lane of the second operand
        const Val s = b.ext(b.ext(b.lane(amounts, i, e), 8, false), 64, true);
        const Val isLeft = b.compare(Op::COND_SIGNED_GREATER_EQUAL, s, b.imm(0, 64));
        const Val rightAmount = b.op(Op::OP_NEGATE, 64, s);
        // The right shift
        Val right;
        if (isRounding)
        {
            SemanticInstruction instruction(Op::OP_SHIFT_RIGHT_ROUNDING,
                                            NeonBuilder::type(64),
                                            c.newTemporary(NeonBuilder::type(64)),
                                            x, rightAmount);
            instruction.setBehavior(isSigned ? Op::BEHAVIOR_SIGNED :
                                               Op::BEHAVIOR_UNSIGNED);
            c.emit(instruction);
            right = instruction.getDestination();
        } else
        {
            right = b.op(isSigned ? Op::OP_SHIFT_RIGHT_ARITHMETIC :
                                    Op::OP_SHIFT_RIGHT_LOGICAL, 64, x, rightAmount);
        }
        // The left shift
        Val left;
        if (isSaturating)
        {
            Val saturated;
            left = shiftLeftSaturating(b, x, s, e, isSigned, false, &saturated);
            b.orSaturation(b.op(Op::OP_AND, 1, isLeft, saturated));
        } else
        {
            left = b.op(Op::OP_SHIFT_LEFT, 64, x, s);
        }
        result[i] = b.ext(b.op(Op::OP_SELECT, 64, isLeft, left, right), e, false);
    }
    writeLanes(b, result, lanes, e);
    b.commitSaturation();
}

/////////////////////////////////////////////////////////////////////////////
// vmovn, vmovl, vqmovn, vqmovun, vshll, vshrn, vrshrn, vqshrn, vqrshrn,
// vqshrun, vqrshrun
/////////////////////////////////////////////////////////////////////////////

void liftNeonNarrowMove(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint s = n.m_bits;
    // Widening: d -> q
    if (n.is("vmovl") || n.is("vshll"))
    {
        const uint w = 2 * s;
        const bool isSigned = n.isSigned();
        const uint amount = n.is("vshll") ?
            ((c.getOperand(2).m_kind == ArmDecodedOperand::KIND_IMMEDIATE) ?
                (uint)c.getOperand(2).m_immediate : s) : 0;
        const Val source = b.reg(1);
        Val result[8];
        const uint lanes = 64 / s;
        for (uint i = 0; i < lanes; i++)
        {
            Val v = b.ext(b.lane(source, i, s), w, isSigned);
            if (amount != 0)
                v = b.op(Op::OP_SHIFT_LEFT, w, v, b.imm(amount, w));
            result[i] = v;
        }
        writeLanes(b, result, lanes, w);
        return;
    }
    // Narrowing: q -> d
    const uint r = s / 2;
    const uint lanes = 128 / s;
    const bool isSigned = n.isSigned();
    const Val source = b.reg(1);
    Val result[8];
    const bool isShift = c.getOperandsCount() > 2;
    const uint amount = isShift ? (uint)c.getOperand(2).m_immediate : 0;
    const bool isRounding = n.is("vrshrn") || n.is("vqrshrn") || n.is("vqrshrun");
    for (uint i = 0; i < lanes; i++)
    {
        Val x = b.lane(source, i, s);
        if (n.is("vmovn"))
        {
            result[i] = b.ext(x, r, false);
        } else if (n.is("vshrn") || n.is("vrshrn"))
        {
            Val v = x;
            if (isRounding)
                v = shiftRightExact(b, b.ext(x, 64, false), amount, true, false);
            else
                v = b.op(Op::OP_SHIFT_RIGHT_LOGICAL, 64, b.ext(x, 64, false),
                         b.imm(amount, 64));
            result[i] = b.ext(v, r, false);
        } else
        {
            // The saturating narrows: the value (shifted) in 64 bits
            const bool sourceSigned = (n.m_kind != 'u');
            Val v = b.ext(x, 64, sourceSigned);
            if (isShift)
                v = shiftRightExact(b, v, amount, isRounding, sourceSigned);
            const bool resultUnsigned = n.is("vqmovun") || n.is("vqshrun") ||
                                        n.is("vqrshrun") || (n.m_kind == 'u');
            Val clamped;
            if (!resultUnsigned)
            {
                clamped = b.clampSigned(v, signedMinimum(r), signedMaximum(r));
            } else if (sourceSigned)
            {
                clamped = b.clampUnsigned(v, unsignedMaximum(r));
            } else
            {
                clamped = b.op(Op::OP_MINIMUM_UNSIGNED, 64, v,
                               b.imm((int64)unsignedMaximum(r), 64));
                b.orSaturation(b.compare(Op::COND_NOT_EQUAL, clamped, v));
            }
            result[i] = b.ext(clamped, r, false);
        }
    }
    writeLanes(b, result, lanes, r);
    b.commitSaturation();
}

/////////////////////////////////////////////////////////////////////////////
// vpadd, vpmax, vpmin (d registers), vpaddl, vpadal
/////////////////////////////////////////////////////////////////////////////

void liftNeonPairwise(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint e = n.m_bits;
    const bool isSigned = n.isSigned();
    if (n.is("vpaddl") || n.is("vpadal"))
    {
        const uint size = b.regBits(0);
        const uint w = 2 * e;
        const uint lanes = size / w;
        const Val source = b.reg(1);
        const Val accumulator = b.reg(0);
        Val result[8];
        for (uint i = 0; i < lanes; i++)
        {
            Val sum = b.op(Op::OP_ADD, w,
                           b.ext(b.lane(source, 2 * i, e), w, isSigned),
                           b.ext(b.lane(source, 2 * i + 1, e), w, isSigned));
            if (n.is("vpadal"))
                sum = b.op(Op::OP_ADD, w, b.lane(accumulator, i, w), sum);
            result[i] = sum;
        }
        writeLanes(b, result, lanes, w);
        return;
    }
    const uint lanes = 64 / e;
    const Val first = b.reg(1);
    const Val other = b.reg(2);
    Val result[8];
    for (uint i = 0; i < lanes; i++)
    {
        const Val& source = (i < lanes / 2) ? first : other;
        const uint j = (i < lanes / 2) ? i : (i - lanes / 2);
        const Val x = b.lane(source, 2 * j, e);
        const Val y = b.lane(source, 2 * j + 1, e);
        if (n.is("vpadd"))
            result[i] = b.op(Op::OP_ADD, e, x, y);
        else if (n.is("vpmax"))
            result[i] = b.op(isSigned ? Op::OP_MAXIMUM_SIGNED :
                                        Op::OP_MAXIMUM_UNSIGNED, e, x, y);
        else
            result[i] = b.op(isSigned ? Op::OP_MINIMUM_SIGNED :
                                        Op::OP_MINIMUM_UNSIGNED, e, x, y);
    }
    writeLanes(b, result, lanes, e);
}

/////////////////////////////////////////////////////////////////////////////
// vrev16/32/64, vswp, vtrn, vuzp, vzip, vext
/////////////////////////////////////////////////////////////////////////////

void liftNeonPermute(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint size = b.regBits(0);
    if (n.is("vswp"))
    {
        const Val x = b.copy(b.reg(0));
        const Val y = b.copy(b.reg(1));
        c.write(0, y);
        c.write(1, x);
        return;
    }
    const uint e = n.m_bits;
    const uint lanes = size / e;
    if (n.is("vrev16") || n.is("vrev32") || n.is("vrev64"))
    {
        const uint container = (n.m_base[4] == '1') ? 16 : ((n.m_base[4] == '3') ? 32 : 64);
        const uint group = container / e;
        const Val source = b.reg(1);
        Val result[16];
        for (uint i = 0; i < lanes; i++)
            result[i] = b.lane(source, i ^ (group - 1), e);
        writeLanes(b, result, lanes, e);
        return;
    }
    if (n.is("vext"))
    {
        const uint index = (uint)c.getOperand(3).m_immediate;
        const Val x = b.reg(1);
        const Val y = b.reg(2);
        Val result[16];
        for (uint i = 0; i < lanes; i++)
        {
            const uint k = i + index;
            result[i] = (k < lanes) ? b.lane(x, k, e) : b.lane(y, k - lanes, e);
        }
        writeLanes(b, result, lanes, e);
        return;
    }
    // vtrn, vuzp, vzip: two destinations, read both first
    const Val x = b.copy(b.reg(0));
    const Val y = b.copy(b.reg(1));
    Val first[16];
    Val second[16];
    if (n.is("vtrn"))
    {
        for (uint i = 0; i < lanes; i += 2)
        {
            first[i] = b.lane(x, i, e);
            first[i + 1] = b.lane(y, i, e);
            second[i] = b.lane(x, i + 1, e);
            second[i + 1] = b.lane(y, i + 1, e);
        }
    } else if (n.is("vuzp"))
    {
        for (uint i = 0; i < 2 * lanes; i++)
        {
            const Val v = (i < lanes) ? b.lane(x, i, e) : b.lane(y, i - lanes, e);
            if ((i & 1) == 0)
                first[i / 2] = v;
            else
                second[i / 2] = v;
        }
    } else
    {
        // vzip: the interleave of x and y, the low half to x
        for (uint i = 0; i < lanes; i++)
        {
            const uint k = 2 * i;
            const Val a = b.lane(x, i, e);
            const Val d = b.lane(y, i, e);
            if (k < lanes)
            {
                first[k] = a;
                first[k + 1] = d;
            } else
            {
                second[k - lanes] = a;
                second[k - lanes + 1] = d;
            }
        }
    }
    c.write(0, b.pack(first, lanes, e));
    c.write(1, b.pack(second, lanes, e));
}

/////////////////////////////////////////////////////////////////////////////
// vtbl, vtbx
/////////////////////////////////////////////////////////////////////////////

void liftNeonTable(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const ArmDecodedOperand& list = c.getOperand(1);
    const uint count = list.m_count;
    // The table bytes
    Val table[32];
    for (uint r = 0; r < count; r++)
    {
        const Val v = c.getRegister(ARM_REG_D0 + (list.m_register - ARM_REG_D0 + r) % 32);
        for (uint k = 0; k < 8; k++)
            table[8 * r + k] = b.lane(v, k, 8);
    }
    const Val indexes = b.reg(2);
    const Val old = b.reg(0);
    Val result[8];
    for (uint i = 0; i < 8; i++)
    {
        const Val index = b.lane(indexes, i, 8);
        Val value = n.is("vtbx") ? b.lane(old, i, 8) : b.imm(0, 8);
        for (uint k = 0; k < 8 * count; k++)
            value = b.op(Op::OP_SELECT, 8, b.compare(Op::COND_EQUAL, index,
                                                      b.imm(k, 8)),
                         table[k], value);
        result[i] = value;
    }
    writeLanes(b, result, 8, 8);
}

/////////////////////////////////////////////////////////////////////////////
// vdup
/////////////////////////////////////////////////////////////////////////////

void liftNeonDuplicate(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint e = n.m_bits;
    const uint size = b.regBits(0);
    const uint lanes = size / e;
    Val value;
    if (isScalar(c.getOperand(1)))
        value = readScalar(b, 1, e);
    else
        value = b.ext(c.read(1), e, false);
    Val result[16];
    for (uint i = 0; i < lanes; i++)
        result[i] = value;
    writeLanes(b, result, lanes, e);
}

/////////////////////////////////////////////////////////////////////////////
// vrecpe.u32, vrsqrte.u32: the integer estimates
/////////////////////////////////////////////////////////////////////////////

void liftNeonEstimate(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const uint size = b.regBits(0);
    const uint lanes = size / 32;
    const Val source = b.reg(1);
    const bool isReciprocal = n.is("vrecpe");
    Val result[4];
    for (uint i = 0; i < lanes; i++)
    {
        const Val x = b.lane(source, i, 32);
        const Val wide = b.ext(x, 64, false);
        // The 9 bit number operand<31:23>
        const Val a = b.op(Op::OP_SHIFT_RIGHT_LOGICAL, 64, wide, b.imm(23, 64));
        Val estimate;
        Val isSmall;
        if (isReciprocal)
        {
            // RecipEstimate: a = a * 2 + 1; b = 2^19 DIV a; r = (b + 1) DIV 2
            const Val odd = b.op(Op::OP_ADD, 64,
                                 b.op(Op::OP_SHIFT_LEFT, 64, a, b.imm(1, 64)),
                                 b.imm(1, 64));
            const Val quotient = b.op(Op::OP_DIVIDE_UNSIGNED, 64,
                                      b.imm(1 << 19, 64), odd);
            estimate = b.op(Op::OP_SHIFT_RIGHT_LOGICAL, 64,
                            b.op(Op::OP_ADD, 64, quotient, b.imm(1, 64)),
                            b.imm(1, 64));
            isSmall = b.compare(Op::COND_UNSIGNED_LESS, wide, b.imm(0x80000000LL, 64));
        } else
        {
            // RecipSqrtEstimate: a in 128..511
            const Val isLow = b.compare(Op::COND_UNSIGNED_LESS, a, b.imm(256, 64));
            const Val lowForm = b.op(Op::OP_ADD, 64,
                                     b.op(Op::OP_SHIFT_LEFT, 64, a, b.imm(1, 64)),
                                     b.imm(1, 64));
            const Val highForm = b.op(Op::OP_SHIFT_LEFT, 64,
                b.op(Op::OP_ADD, 64,
                     b.op(Op::OP_SHIFT_LEFT, 64,
                          b.op(Op::OP_SHIFT_RIGHT_LOGICAL, 64, a, b.imm(1, 64)),
                          b.imm(1, 64)),
                     b.imm(1, 64)), b.imm(1, 64));
            const Val scaled = b.op(Op::OP_SELECT, 64, isLow, lowForm, highForm);
            // The largest t with scaled * t * t < 2^28, found bit by bit
            Val t = b.imm(0, 64);
            for (int bit = 10; bit >= 0; bit--)
            {
                const Val candidate = b.op(Op::OP_OR, 64, t, b.imm((int64)1 << bit, 64));
                const Val square = b.op(Op::OP_MULTIPLY, 64, scaled,
                                        b.op(Op::OP_MULTIPLY, 64, candidate, candidate));
                const Val isBelow = b.compare(Op::COND_UNSIGNED_LESS, square,
                                              b.imm((int64)1 << 28, 64));
                t = b.op(Op::OP_SELECT, 64, isBelow, candidate, t);
            }
            // b = max(512, t); r = (b + 1) DIV 2
            const Val bValue = b.op(Op::OP_MAXIMUM_UNSIGNED, 64, t, b.imm(512, 64));
            estimate = b.op(Op::OP_SHIFT_RIGHT_LOGICAL, 64,
                            b.op(Op::OP_ADD, 64, bValue, b.imm(1, 64)), b.imm(1, 64));
            isSmall = b.compare(Op::COND_UNSIGNED_LESS, wide, b.imm(0x40000000LL, 64));
        }
        // estimate<8:0> : Zeros(23)
        const Val shifted = b.op(Op::OP_SHIFT_LEFT, 32,
                                 b.ext(b.op(Op::OP_AND, 64, estimate, b.imm(0x1ff, 64)),
                                       32, false), b.imm(23, 32));
        result[i] = b.op(Op::OP_SELECT, 32, isSmall, b.imm(-1, 32), shifted);
    }
    writeLanes(b, result, lanes, 32);
}

} // namespace a32
} // namespace armdis
