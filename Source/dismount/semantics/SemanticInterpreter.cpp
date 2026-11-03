/*
 * SemanticInterpreter.cpp
 *
 * The reference semantics of the semantic IR.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/data/array.h"
#include "xStl/data/sarray.h"
#include "xStl/data/list.h"
#include "xStl/data/smartptr.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticFloat.h"
#include "dismount/semantics/SemanticInterpreter.h"

/////////////////////////////////////////////////////////////////////////////
// SemanticValue
/////////////////////////////////////////////////////////////////////////////

SemanticValue::SemanticValue(uint64 value)
{
    m_words[0] = value;
    for (uint i = 1; i < WORDS_COUNT; i++)
        m_words[i] = 0;
}

uint64 SemanticValue::getWord(uint index) const
{
    CHECK(index < WORDS_COUNT);
    return m_words[index];
}

void SemanticValue::setWord(uint index, uint64 value)
{
    CHECK(index < WORDS_COUNT);
    m_words[index] = value;
}

namespace {

// Return a mask of the low 'bits' bits (1 to 64)
uint64 maskOf(uint bits)
{
    return (bits >= 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
}

} // namespace

uint64 SemanticValue::getBits(uint offset, uint bits) const
{
    CHECK((bits >= 1) && (bits <= 64) && (offset + bits <= MAX_BITS));
    const uint word = offset / 64;
    const uint shift = offset % 64;
    uint64 value = m_words[word] >> shift;
    // The field continues in the next word
    if ((shift + bits > 64) && (word + 1 < WORDS_COUNT))
        value |= m_words[word + 1] << (64 - shift);
    return value & maskOf(bits);
}

void SemanticValue::setBits(uint offset, uint bits, uint64 value)
{
    CHECK((bits >= 1) && (bits <= 64) && (offset + bits <= MAX_BITS));
    const uint word = offset / 64;
    const uint shift = offset % 64;
    const uint64 mask = maskOf(bits);
    value &= mask;
    m_words[word] = (m_words[word] & ~(mask << shift)) | (value << shift);
    if ((shift + bits > 64) && (word + 1 < WORDS_COUNT))
    {
        // The part of the field in the next word
        const uint highShift = 64 - shift;
        m_words[word + 1] = (m_words[word + 1] & ~(mask >> highShift)) |
                            (value >> highShift);
    }
}

void SemanticValue::truncate(uint bits)
{
    if (bits >= MAX_BITS)
        return;
    const uint word = bits / 64;
    const uint shift = bits % 64;
    if (shift != 0)
    {
        m_words[word] &= maskOf(shift);
    }
    for (uint i = (shift != 0) ? word + 1 : word; i < WORDS_COUNT; i++)
        m_words[i] = 0;
}

bool SemanticValue::operator == (const SemanticValue& other) const
{
    for (uint i = 0; i < WORDS_COUNT; i++)
    {
        if (m_words[i] != other.m_words[i])
            return false;
    }
    return true;
}

bool SemanticValue::operator != (const SemanticValue& other) const
{
    return !(*this == other);
}

/////////////////////////////////////////////////////////////////////////////
// Helpers
/////////////////////////////////////////////////////////////////////////////

namespace {

// The result of evaluating an operation
enum EvalStatus {
    // The result is valid
    EVAL_OK,
    // The operation raises a guest exception (a division by zero)
    EVAL_FAULT,
    // The interpreter doesn't implement the operation (yet)
    EVAL_UNDEFINED
};

// Copy 'count' bits, of any count
void copyBits(SemanticValue& destination, uint destinationOffset,
              const SemanticValue& source, uint sourceOffset, uint count)
{
    while (count > 0)
    {
        const uint chunk = (count > 64) ? 64 : count;
        destination.setBits(destinationOffset, chunk,
                            source.getBits(sourceOffset, chunk));
        destinationOffset += chunk;
        sourceOffset += chunk;
        count -= chunk;
    }
}

// Return 'count' bits of 'value' from 'offset'. The bits beyond the end of the
// value are 0
SemanticValue extractBits(const SemanticValue& value, uint offset, uint count)
{
    SemanticValue result;
    if (offset >= SemanticValue::MAX_BITS)
        return result;
    if (offset + count > SemanticValue::MAX_BITS)
        count = SemanticValue::MAX_BITS - offset;
    copyBits(result, 0, value, offset, count);
    return result;
}

// Set the bits from 'from' to 'to' (not included)
void fillOnes(SemanticValue& value, uint from, uint to)
{
    while (from < to)
    {
        const uint chunk = ((to - from) > 64) ? 64 : (to - from);
        value.setBits(from, chunk, ~(uint64)0);
        from += chunk;
    }
}

// Return 'value' (of 'bits' bits) as a signed number
int64 signExtend(uint64 value, uint bits)
{
    const uint64 sign = (uint64)1 << (bits - 1);
    return (int64)((value ^ sign) - sign);
}

// Return the MSB of a 'bits' wide value
bool isNegative(uint64 value, uint bits)
{
    return ((value >> (bits - 1)) & 1) != 0;
}

// Shift right with any count. An arithmetic shift fills with the sign
uint64 shiftRight(uint64 value, uint64 count, bool isArithmetic)
{
    const bool isSigned = isArithmetic && ((value >> 63) != 0);
    if (count >= 64)
        return isSigned ? ~(uint64)0 : 0;
    if (isSigned)
        return ~((~value) >> count);
    return value >> count;
}

// The number of one bits
uint countOnes(uint64 value)
{
    uint count = 0;
    for (; value != 0; value &= value - 1)
        count++;
    return count;
}

// The number of zero bits above the highest one bit of 'value' (of 'bits'
// bits), 'bits' for 0
uint countLeadingZeros(uint64 value, uint bits)
{
    uint count = 0;
    while ((count < bits) && (((value >> (bits - 1 - count)) & 1) == 0))
        count++;
    return count;
}

// The number of zero bits below the lowest one bit, 'bits' for 0
uint countTrailingZeros(uint64 value, uint bits)
{
    uint count = 0;
    while ((count < bits) && (((value >> count) & 1) == 0))
        count++;
    return count;
}

// The unsigned product of two 64 bit numbers, as a 128 bit number
void multiply64(uint64 a, uint64 b, uint64& high, uint64& low)
{
    const uint64 low32 = 0xFFFFFFFF;
    const uint64 a0 = a & low32, a1 = a >> 32;
    const uint64 b0 = b & low32, b1 = b >> 32;
    const uint64 p00 = a0 * b0;
    const uint64 p01 = a0 * b1;
    const uint64 p10 = a1 * b0;
    const uint64 p11 = a1 * b1;
    const uint64 middle = (p00 >> 32) + (p01 & low32) + (p10 & low32);
    low = (p00 & low32) | (middle << 32);
    high = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}

// Return the 128 bit number 'high:low' shifted right by 'bits' (1 to 64),
// truncated to 'bits' bits: the high half of a 2 * bits product
uint64 highHalf(uint64 high, uint64 low, uint bits)
{
    if (bits >= 64)
        return high;
    return ((high << (64 - bits)) | (low >> bits)) & maskOf(bits);
}

// The high half of the product of two 'bits' bit numbers
uint64 multiplyHigh(uint64 a, uint64 b, uint bits, bool isSignedA,
                    bool isSignedB)
{
    uint64 high, low;
    // The operands are extended to 64 bits, so the 128 bit product is correct
    // after the correction of the negative operands
    const bool negativeA = isSignedA && isNegative(a, bits);
    const bool negativeB = isSignedB && isNegative(b, bits);
    if (negativeA)
        a = (uint64)signExtend(a, bits);
    if (negativeB)
        b = (uint64)signExtend(b, bits);
    multiply64(a, b, high, low);
    if (negativeA)
        high -= b;
    if (negativeB)
        high -= a;
    return highHalf(high, low, bits);
}

// The carry-less product of two 64 bit numbers, as a 128 bit number
void carrylessMultiply64(uint64 a, uint64 b, uint64& high, uint64& low)
{
    high = 0;
    low = 0;
    for (uint i = 0; i < 64; i++)
    {
        if (((b >> i) & 1) == 0)
            continue;
        low ^= a << i;
        if (i != 0)
            high ^= a >> (64 - i);
    }
}

// Multiply two numbers of GF(2^8) modulo x^8 + x^4 + x^3 + x + 1 (AES)
uint8 aesMultiply(uint8 a, uint8 b)
{
    uint8 result = 0;
    for (uint i = 0; i < 8; i++)
    {
        if ((b & 1) != 0)
            result ^= a;
        const bool isCarry = (a & 0x80) != 0;
        a = (uint8)(a << 1);
        if (isCarry)
            a ^= 0x1B;
        b >>= 1;
    }
    return result;
}

// The multiplicative inverse in GF(2^8), 0 for 0 (a^254)
uint8 aesInvert(uint8 a)
{
    uint8 result = 1;
    for (uint i = 0; i < 254; i++)
        result = aesMultiply(result, a);
    return (a == 0) ? 0 : result;
}

uint8 rotateLeft8(uint8 value, uint count)
{
    return (uint8)((value << count) | (value >> (8 - count)));
}

// SubBytes of one byte: the inverse in GF(2^8) then the affine transform
uint8 aesSubstitute(uint8 value)
{
    const uint8 b = aesInvert(value);
    return (uint8)(b ^ rotateLeft8(b, 1) ^ rotateLeft8(b, 2) ^
                   rotateLeft8(b, 3) ^ rotateLeft8(b, 4) ^ 0x63);
}

// InvSubBytes of one byte: the inverse affine transform then the inverse
uint8 aesInverseSubstitute(uint8 value)
{
    const uint8 b = (uint8)(rotateLeft8(value, 1) ^ rotateLeft8(value, 3) ^
                            rotateLeft8(value, 6) ^ 0x05);
    return aesInvert(b);
}

/*
 * The AES steps on a 128 bit state. The state is the sixteen bytes of the
 * value, byte r + 4 * c is the row r of column c.
 *
 * isInverse - InvSubBytes with InvShiftRows (they commute), or InvMixColumns
 * isMix     - MixColumns, or SubBytes with ShiftRows
 */
void aesStep(const SemanticValue& source, bool isInverse, bool isMix,
             SemanticValue& result)
{
    uint8 state[16];
    uint8 output[16];
    for (uint i = 0; i < 16; i++)
        state[i] = (uint8)source.getBits(i * 8, 8);
    for (uint column = 0; column < 4; column++)
    {
        for (uint row = 0; row < 4; row++)
        {
            if (!isMix)
            {
                // Row r moves left by r columns (right for the inverse)
                const uint from = isInverse ? ((column + 4 - row) % 4) :
                                              ((column + row) % 4);
                const uint8 value = state[row + 4 * from];
                output[row + 4 * column] = isInverse ?
                            aesInverseSubstitute(value) : aesSubstitute(value);
            } else
            {
                static const uint8 forward[4] = { 2, 3, 1, 1 };
                static const uint8 inverse[4] = { 14, 11, 13, 9 };
                const uint8* factors = isInverse ? inverse : forward;
                uint8 sum = 0;
                for (uint k = 0; k < 4; k++)
                    sum ^= aesMultiply(state[(k) + 4 * column],
                                       factors[(k + 4 - row) % 4]);
                output[row + 4 * column] = sum;
            }
        }
    }
    result = SemanticValue();
    for (uint i = 0; i < 16; i++)
        result.setBits(i * 8, 8, output[i]);
}

// Reverse the order of the bytes of a 'bits' wide value
uint64 swapBytes(uint64 value, uint bits)
{
    uint64 result = 0;
    for (uint i = 0; i < bits; i += 8)
        result |= ((value >> i) & 0xFF) << (bits - 8 - i);
    return result;
}

// Reverse the order of the bits of a 'bits' wide value
uint64 reverseBits(uint64 value, uint bits)
{
    uint64 result = 0;
    for (uint i = 0; i < bits; i++)
        result |= ((value >> i) & 1) << (bits - 1 - i);
    return result;
}

// Return the type of the source 'index': the type of its operand, or the type
// of the operation when the operand has none
SemanticType getSourceType(const SemanticInstruction& instruction, uint index)
{
    const SemanticOperand& operand = instruction.getSource(index);
    if (operand.isValid() && !operand.getType().isVoid())
        return operand.getType();
    return instruction.getType();
}

// The number of bits of the result: the type of the operation or, when it has
// none, the type of the destination
uint getResultBits(const SemanticInstruction& instruction)
{
    if (!instruction.getType().isVoid())
        return instruction.getType().getTotalBits();
    return instruction.getDestination().getType().getTotalBits();
}

// Evaluate the condition of OP_COMPARE on two integers of 'bits' bits.
// Return false for the conditions which aren't a relation of two values
bool compareValues(SemanticOperation::Condition condition, uint bits,
                   uint64 a, uint64 b, bool& result)
{
    const int64 signedA = signExtend(a, bits);
    const int64 signedB = signExtend(b, bits);
    switch (condition)
    {
    case SemanticOperation::COND_ALWAYS:
        result = true; break;
    case SemanticOperation::COND_NEVER:
        result = false; break;
    case SemanticOperation::COND_EQUAL:
        result = (a == b); break;
    case SemanticOperation::COND_NOT_EQUAL:
        result = (a != b); break;
    case SemanticOperation::COND_SIGNED_LESS:
        result = (signedA < signedB); break;
    case SemanticOperation::COND_SIGNED_LESS_EQUAL:
        result = (signedA <= signedB); break;
    case SemanticOperation::COND_SIGNED_GREATER:
        result = (signedA > signedB); break;
    case SemanticOperation::COND_SIGNED_GREATER_EQUAL:
        result = (signedA >= signedB); break;
    case SemanticOperation::COND_UNSIGNED_LESS:
        result = (a < b); break;
    case SemanticOperation::COND_UNSIGNED_LESS_EQUAL:
        result = (a <= b); break;
    case SemanticOperation::COND_UNSIGNED_GREATER:
        result = (a > b); break;
    case SemanticOperation::COND_UNSIGNED_GREATER_EQUAL:
        result = (a >= b); break;
    case SemanticOperation::COND_NEGATIVE:
        result = (signedA < 0); break;
    case SemanticOperation::COND_NOT_NEGATIVE:
        result = (signedA >= 0); break;
    default:
        // The flag conditions (carry, overflow, parity) don't describe a
        // relation of two values, and the float conditions are not
        // interpreted yet
        return false;
    }
    return true;
}

// Evaluate an operation on a single lane of 'bits' bits (1 to 64)
EvalStatus evaluateLane(const SemanticInstruction& instruction,
                        SemanticOperation::Operation operation,
                        uint bits, uint64 a, uint64 b, uint64 c, uint64& r)
{
    const uint64 m = maskOf(bits);
    const uint64 signBit = (uint64)1 << (bits - 1);
    switch (operation)
    {
    //////////////////////////////////////////////////////////////////
    // Integer arithmetic
    //////////////////////////////////////////////////////////////////
    case SemanticOperation::OP_ADD:
        r = a + b; break;
    case SemanticOperation::OP_ADD_CARRY:
        r = a + b + (c & 1); break;
    case SemanticOperation::OP_SUBTRACT:
        r = a - b; break;
    case SemanticOperation::OP_SUBTRACT_BORROW:
        r = a - b - (c & 1); break;
    case SemanticOperation::OP_NEGATE:
        r = (uint64)0 - a; break;
    case SemanticOperation::OP_MULTIPLY:
        r = a * b; break;
    case SemanticOperation::OP_MULTIPLY_HIGH_SIGNED:
        r = multiplyHigh(a, b, bits, true, true); break;
    case SemanticOperation::OP_MULTIPLY_HIGH_UNSIGNED:
        r = multiplyHigh(a, b, bits, false, false); break;
    case SemanticOperation::OP_MULTIPLY_HIGH_SIGNED_UNSIGNED:
        r = multiplyHigh(a, b, bits, true, false); break;
    case SemanticOperation::OP_DIVIDE_UNSIGNED:
    case SemanticOperation::OP_REMAINDER_UNSIGNED:
    {
        const bool isRemainder =
                    (operation == SemanticOperation::OP_REMAINDER_UNSIGNED);
        if (b == 0)
        {
            if (instruction.getBehavior() ==
                SemanticOperation::BEHAVIOR_DIVIDE_FAULT)
                return EVAL_FAULT;
            if (isRemainder)
                r = a;
            else
                r = (instruction.getBehavior() ==
                     SemanticOperation::BEHAVIOR_DIVIDE_ZERO) ? 0 : m;
        } else
        {
            r = isRemainder ? (a % b) : (a / b);
        }
        break;
    }
    case SemanticOperation::OP_DIVIDE_SIGNED:
    case SemanticOperation::OP_REMAINDER_SIGNED:
    {
        const bool isRemainder =
                    (operation == SemanticOperation::OP_REMAINDER_SIGNED);
        const int64 signedA = signExtend(a, bits);
        const int64 signedB = signExtend(b, bits);
        // The minimum / -1 doesn't fit the result
        const bool isOverflow = (a == signBit) && (signedB == -1);
        if ((b == 0) || isOverflow)
        {
            if (instruction.getBehavior() ==
                SemanticOperation::BEHAVIOR_DIVIDE_FAULT)
                return EVAL_FAULT;
            if (b == 0)
            {
                if (isRemainder)
                    r = a;
                else
                    r = (instruction.getBehavior() ==
                         SemanticOperation::BEHAVIOR_DIVIDE_ZERO) ? 0 : m;
            } else
            {
                r = isRemainder ? 0 : a;
            }
        } else
        {
            r = (uint64)(isRemainder ? (signedA % signedB)
                                     : (signedA / signedB));
        }
        break;
    }
    case SemanticOperation::OP_ABSOLUTE_DIFFERENCE_SIGNED:
    {
        const int64 signedA = signExtend(a, bits);
        const int64 signedB = signExtend(b, bits);
        r = (signedA > signedB) ? ((uint64)signedA - (uint64)signedB)
                                : ((uint64)signedB - (uint64)signedA);
        break;
    }
    case SemanticOperation::OP_ABSOLUTE_DIFFERENCE_UNSIGNED:
        r = (a > b) ? (a - b) : (b - a); break;
    case SemanticOperation::OP_CARRYLESS_MULTIPLY:
    case SemanticOperation::OP_CARRYLESS_MULTIPLY_HIGH:
    {
        uint64 high, low;
        carrylessMultiply64(a, b, high, low);
        r = (operation == SemanticOperation::OP_CARRYLESS_MULTIPLY) ?
                    low : highHalf(high, low, bits);
        break;
    }

    //////////////////////////////////////////////////////////////////
    // Logic, shifts and bits
    //////////////////////////////////////////////////////////////////
    case SemanticOperation::OP_AND:
        r = a & b; break;
    case SemanticOperation::OP_OR:
        r = a | b; break;
    case SemanticOperation::OP_XOR:
        r = a ^ b; break;
    case SemanticOperation::OP_NOT:
        r = ~a; break;
    case SemanticOperation::OP_AND_NOT:
        r = ~a & b; break;
    case SemanticOperation::OP_BITWISE_SELECT:
        r = (a & b) | (~a & c); break;
    case SemanticOperation::OP_SHIFT_LEFT:
        r = (b >= bits) ? 0 : (a << b); break;
    case SemanticOperation::OP_SHIFT_RIGHT_LOGICAL:
        r = (b >= bits) ? 0 : (a >> b); break;
    case SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC:
        r = shiftRight((uint64)signExtend(a, bits), b, true); break;
    case SemanticOperation::OP_ROTATE_LEFT:
    case SemanticOperation::OP_ROTATE_RIGHT:
    {
        // The size may be odd (rotate through carry is a 33 bit rotate)
        uint64 count = b % bits;
        if (operation == SemanticOperation::OP_ROTATE_RIGHT)
            count = (bits - count) % bits;
        r = (count == 0) ? a : ((a << count) | (a >> (bits - count)));
        break;
    }
    case SemanticOperation::OP_COUNT_LEADING_ZEROS:
        r = countLeadingZeros(a, bits); break;
    case SemanticOperation::OP_COUNT_TRAILING_ZEROS:
        r = countTrailingZeros(a, bits); break;
    case SemanticOperation::OP_COUNT_LEADING_SIGN_BITS:
        // The leading zeros of the value, or of its complement for a
        // negative one, without the sign bit itself
        r = countLeadingZeros(isNegative(a, bits) ? (~a & m) : a, bits) - 1;
        break;
    case SemanticOperation::OP_POPULATION_COUNT:
        r = countOnes(a); break;
    case SemanticOperation::OP_BYTE_SWAP:
        if ((bits % 8) != 0)
            return EVAL_UNDEFINED;
        r = swapBytes(a, bits);
        break;
    case SemanticOperation::OP_BIT_REVERSE:
        r = reverseBits(a, bits); break;
    case SemanticOperation::OP_BIT_DEPOSIT:
    {
        // The low bits of a go to the positions of the one bits of the mask
        r = 0;
        uint next = 0;
        for (uint i = 0; i < bits; i++)
        {
            if (((b >> i) & 1) != 0)
            {
                r |= ((a >> next) & 1) << i;
                next++;
            }
        }
        break;
    }
    case SemanticOperation::OP_BIT_GATHER:
    {
        r = 0;
        uint next = 0;
        for (uint i = 0; i < bits; i++)
        {
            if (((b >> i) & 1) != 0)
            {
                r |= ((a >> i) & 1) << next;
                next++;
            }
        }
        break;
    }

    //////////////////////////////////////////////////////////////////
    // Integer vector operations
    //////////////////////////////////////////////////////////////////
    case SemanticOperation::OP_ADD_SATURATE_SIGNED:
    {
        r = (a + b) & m;
        if ((isNegative(a, bits) == isNegative(b, bits)) &&
            (isNegative(r, bits) != isNegative(a, bits)))
            r = isNegative(a, bits) ? signBit : (m >> 1);
        break;
    }
    case SemanticOperation::OP_SUBTRACT_SATURATE_SIGNED:
    {
        r = (a - b) & m;
        if ((isNegative(a, bits) != isNegative(b, bits)) &&
            (isNegative(r, bits) != isNegative(a, bits)))
            r = isNegative(a, bits) ? signBit : (m >> 1);
        break;
    }
    case SemanticOperation::OP_ADD_SATURATE_UNSIGNED:
    {
        const uint64 sum = a + b;
        const bool isCarry = (bits == 64) ? (sum < a) : (sum > m);
        r = isCarry ? m : sum;
        break;
    }
    case SemanticOperation::OP_SUBTRACT_SATURATE_UNSIGNED:
        r = (a < b) ? 0 : (a - b); break;
    case SemanticOperation::OP_AVERAGE_UNSIGNED:
        // (a + b + 1) >> 1 without the overflow of the sum
        r = (a >> 1) + (b >> 1) + ((a | b) & 1); break;
    case SemanticOperation::OP_MINIMUM_SIGNED:
        r = (signExtend(a, bits) < signExtend(b, bits)) ? a : b; break;
    case SemanticOperation::OP_MAXIMUM_SIGNED:
        r = (signExtend(a, bits) > signExtend(b, bits)) ? a : b; break;
    case SemanticOperation::OP_MINIMUM_UNSIGNED:
        r = (a < b) ? a : b; break;
    case SemanticOperation::OP_MAXIMUM_UNSIGNED:
        r = (a > b) ? a : b; break;
    case SemanticOperation::OP_ABSOLUTE:
        r = isNegative(a, bits) ? ((uint64)0 - a) : a; break;
    case SemanticOperation::OP_MULTIPLY_HIGH_ROUNDING:
    {
        // The signed product has to fit 64 bits
        if ((bits < 2) || (bits > 32))
            return EVAL_UNDEFINED;
        const int64 product = signExtend(a, bits) * signExtend(b, bits);
        int64 value = (int64)shiftRight(
                        (uint64)(product + ((int64)1 << (bits - 2))),
                        bits - 1, true);
        // Only the minimum * the minimum overflows
        const int64 maximum = (int64)(m >> 1);
        if (value > maximum)
            value = maximum;
        r = (uint64)value;
        break;
    }
    case SemanticOperation::OP_SHIFT_RIGHT_ROUNDING:
    {
        // floor(a / 2^b) + bit (b - 1) of a, which doesn't overflow
        const bool isArithmetic = (instruction.getBehavior() ==
                                   SemanticOperation::BEHAVIOR_SIGNED);
        const uint64 value = isArithmetic ? (uint64)signExtend(a, bits) : a;
        r = shiftRight(value, b, isArithmetic);
        if (b != 0)
            r += shiftRight(value, b - 1, isArithmetic) & 1;
        break;
    }

    default:
        // TODO: The floating point, the cryptography and the lane
        // permutation operations are not interpreted yet
        return EVAL_UNDEFINED;
    }
    r &= m;
    return EVAL_OK;
}

/////////////////////////////////////////////////////////////////////////////
// Floating point operations (through SemanticFloat)
/////////////////////////////////////////////////////////////////////////////

// The floating point environment of an operation and the exceptions it raised
struct FloatContext {
    FloatContext() : m_flags(0) {};

    SemanticFloat::Environment m_env;
    uint32 m_flags;
};

typedef SemanticFloat::Bits FloatBits;

// Return the number as the bits of a lane
FloatBits loadFloat(const SemanticValue& value, uint offset,
                    SemanticType::FloatFormat format)
{
    const uint total = SemanticFloat::getTotalBits(format);
    FloatBits bits;
    bits.m_low = value.getBits(offset, (total > 64) ? 64 : total);
    if (total > 64)
        bits.m_high = value.getBits(offset + 64, total - 64);
    return bits;
}

void storeFloat(SemanticValue& value, uint offset,
                SemanticType::FloatFormat format, const FloatBits& bits)
{
    const uint total = SemanticFloat::getTotalBits(format);
    value.setBits(offset, (total > 64) ? 64 : total, bits.m_low);
    if (total > 64)
        value.setBits(offset + 64, total - 64, bits.m_high);
}

bool isFloatType(const SemanticType& type)
{
    return type.getKind() == SemanticType::KIND_FLOAT;
}

// The floating point type of the operation: its type or its destination's
SemanticType getFloatType(const SemanticInstruction& instruction)
{
    if (isFloatType(instruction.getType()))
        return instruction.getType();
    return instruction.getDestination().getType();
}

// The floating point type of the source 'index'
SemanticType getFloatSourceType(const SemanticInstruction& instruction,
                                uint index)
{
    const SemanticOperand& operand = instruction.getSource(index);
    if (operand.isValid() && isFloatType(operand.getType()))
        return operand.getType();
    return getFloatType(instruction);
}

bool isFloatOperation(SemanticOperation::Operation operation)
{
    return (operation >= SemanticOperation::OP_FLOAT_ADD) &&
           (operation <= SemanticOperation::OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE);
}

bool isFloatCombine(SemanticOperation::Operation operation)
{
    return (operation == SemanticOperation::OP_FLOAT_ADD) ||
           (operation == SemanticOperation::OP_FLOAT_MINIMUM) ||
           (operation == SemanticOperation::OP_FLOAT_MAXIMUM);
}

// Return true if the instruction is computed by SemanticFloat
bool usesFloat(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    if (isFloatOperation(operation))
        return true;
    switch (operation)
    {
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_PAIRWISE_ADD:
        return isFloatType(getSourceType(instruction, 0));
    case SemanticOperation::OP_REDUCE:
        return isFloatCombine(instruction.getCombineOperation());
    default:
        return false;
    }
}

// The environment of an operation without a state: the instruction's
// rounding mode, dynamic is nearest even
void defaultFloatEnvironment(const SemanticInstruction& instruction,
                             SemanticFloat::Environment& environment)
{
    environment = SemanticFloat::Environment();
    if (instruction.getRoundingMode() != SemanticOperation::ROUND_DYNAMIC)
        environment.m_rounding = instruction.getRoundingMode();
    if ((instruction.getAttributes() &
         SemanticInstruction::ATTRIBUTE_X87) != 0)
        environment.m_nanRule = FloatEnvironment::NAN_LARGER_SIGNIFICAND;
}

// Evaluate the relation of the condition of a float OP_COMPARE
bool isRelationTrue(SemanticOperation::Condition condition,
                    SemanticFloat::Relation relation)
{
    const bool isUnordered = (relation == SemanticFloat::RELATION_UNORDERED);
    const bool isLess = (relation == SemanticFloat::RELATION_LESS);
    const bool isEqual = (relation == SemanticFloat::RELATION_EQUAL);
    const bool isGreater = (relation == SemanticFloat::RELATION_GREATER);
    switch (condition)
    {
    case SemanticOperation::COND_FLOAT_ORDERED:
        return !isUnordered;
    case SemanticOperation::COND_FLOAT_UNORDERED:
        return isUnordered;
    case SemanticOperation::COND_FLOAT_EQUAL:
        return isEqual;
    case SemanticOperation::COND_FLOAT_NOT_EQUAL:
        return !isEqual;
    case SemanticOperation::COND_FLOAT_LESS:
        return isLess;
    case SemanticOperation::COND_FLOAT_LESS_EQUAL:
        return isLess || isEqual;
    case SemanticOperation::COND_FLOAT_GREATER:
        return isGreater;
    case SemanticOperation::COND_FLOAT_GREATER_EQUAL:
        return isGreater || isEqual;
    case SemanticOperation::COND_FLOAT_UNORDERED_OR_EQUAL:
        return isUnordered || isEqual;
    case SemanticOperation::COND_FLOAT_UNORDERED_OR_LESS:
        return isUnordered || isLess;
    case SemanticOperation::COND_FLOAT_UNORDERED_OR_LESS_EQUAL:
        return isUnordered || isLess || isEqual;
    case SemanticOperation::COND_ALWAYS:
        return true;
    default:
        return false;
    }
}

// The conditions which don't signal for a quiet NaN
bool isQuietCondition(SemanticOperation::Condition condition)
{
    return (condition == SemanticOperation::COND_FLOAT_ORDERED) ||
           (condition == SemanticOperation::COND_FLOAT_UNORDERED) ||
           (condition == SemanticOperation::COND_FLOAT_EQUAL) ||
           (condition == SemanticOperation::COND_FLOAT_NOT_EQUAL) ||
           (condition == SemanticOperation::COND_FLOAT_UNORDERED_OR_EQUAL);
}

// The flags of FLAGS_FLOAT_COMPARE
uint32 floatCompareFlags(SemanticFloat::Relation relation)
{
    switch (relation)
    {
    case SemanticFloat::RELATION_UNORDERED:
        return SemanticFlags::FLAG_ZERO | SemanticFlags::FLAG_PARITY |
               SemanticFlags::FLAG_CARRY;
    case SemanticFloat::RELATION_LESS:
        return SemanticFlags::FLAG_CARRY;
    case SemanticFloat::RELATION_EQUAL:
        return SemanticFlags::FLAG_ZERO;
    default:
        return 0;
    }
}

// Combine two float lanes with the combine operation of OP_REDUCE
FloatBits combineFloat(SemanticOperation::Operation operation,
                       SemanticType::FloatFormat format,
                       const SemanticFloat::Environment& environment,
                       SemanticOperation::Behavior behavior,
                       const FloatBits& a, const FloatBits& b, uint32& flags)
{
    if (operation == SemanticOperation::OP_FLOAT_ADD)
        return SemanticFloat::add(format, environment, a, b, flags);
    return SemanticFloat::minimumMaximum(
            format, environment,
            operation == SemanticOperation::OP_FLOAT_MAXIMUM, behavior,
            a, b, flags);
}

// Evaluate the floating point operations, lane by lane
EvalStatus evaluateFloat(const SemanticInstruction& instruction,
                         const SemanticValue* sources, SemanticValue& result,
                         FloatContext& context)
{
    typedef SemanticFloat SF;
    const SemanticOperation::Operation operation = instruction.getOperation();
    const SF::Environment& env = context.m_env;
    uint32& flags = context.m_flags;
    SemanticValue value;

    switch (operation)
    {
    case SemanticOperation::OP_COMPARE:
    {
        const SemanticType sourceType = getSourceType(instruction, 0);
        const SemanticType::FloatFormat format = sourceType.getFloatFormat();
        if (!SF::isSupported(format))
            return EVAL_UNDEFINED;
        const SemanticOperation::Condition condition =
                                                instruction.getCondition();
        for (uint lane = 0; lane < sourceType.getLanes(); lane++)
        {
            const uint offset = lane * sourceType.getElementBits();
            const SF::Relation relation = SF::compare(
                    format, env, loadFloat(sources[0], offset, format),
                    loadFloat(sources[1], offset, format),
                    !isQuietCondition(condition), flags);
            value.setBits(lane, 1,
                          isRelationTrue(condition, relation) ? 1 : 0);
        }
        result = value;
        return EVAL_OK;
    }
    case SemanticOperation::OP_REDUCE:
    {
        const SemanticType sourceType = getSourceType(instruction, 0);
        const SemanticType::FloatFormat format = sourceType.getFloatFormat();
        if (!SF::isSupported(format))
            return EVAL_UNDEFINED;
        const uint stride = sourceType.getElementBits();
        const uint lanes = sourceType.getLanes();
        cArray<FloatBits> items(lanes);
        for (uint lane = 0; lane < lanes; lane++)
            items[lane] = loadFloat(sources[0], lane * stride, format);
        uint count = lanes;
        const SemanticOperation::Operation combine =
                                        instruction.getCombineOperation();
        if (instruction.getBehavior() ==
            SemanticOperation::BEHAVIOR_REDUCE_PAIRWISE)
        {
            while (count > 1)
            {
                uint next = 0;
                for (uint i = 0; i + 1 < count; i += 2)
                    items[next++] = combineFloat(combine, format, env,
                                    instruction.getBehavior(), items[i],
                                    items[i + 1], flags);
                if ((count % 2) != 0)
                    items[next++] = items[count - 1];
                count = next;
            }
        } else
        {
            for (uint i = 1; i < count; i++)
                items[0] = combineFloat(combine, format, env,
                                instruction.getBehavior(), items[0], items[i],
                                flags);
        }
        storeFloat(value, 0, format, items[0]);
        result = value;
        return EVAL_OK;
    }
    case SemanticOperation::OP_PAIRWISE_ADD:
    {
        const SemanticType type = getFloatType(instruction);
        const SemanticType::FloatFormat format = type.getFloatFormat();
        if (!SF::isSupported(format))
            return EVAL_UNDEFINED;
        const uint stride = type.getElementBits();
        const uint half = type.getLanes() / 2;
        for (uint i = 0; i < type.getLanes(); i++)
        {
            const SemanticValue& source = (i < half) ? sources[0] : sources[1];
            const uint j = (i < half) ? i : (i - half);
            storeFloat(value, i * stride, format,
                SF::add(format, env,
                        loadFloat(source, 2 * j * stride, format),
                        loadFloat(source, (2 * j + 1) * stride, format),
                        flags));
        }
        result = value;
        return EVAL_OK;
    }
    case SemanticOperation::OP_FLOAT_RECIPROCAL_ESTIMATE:
    case SemanticOperation::OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE:
        // The estimate tables of the processors are not interpreted
        return EVAL_UNDEFINED;
    default:
        break;
    }

    // The conversions have different source and destination types
    if (operation == SemanticOperation::OP_FLOAT_CONVERT)
    {
        const SemanticType toType = getFloatType(instruction);
        const SemanticType fromType = getFloatSourceType(instruction, 0);
        if (!SF::isSupported(toType.getFloatFormat()) ||
            !SF::isSupported(fromType.getFloatFormat()))
            return EVAL_UNDEFINED;
        for (uint lane = 0; lane < toType.getLanes(); lane++)
            storeFloat(value, lane * toType.getElementBits(),
                toType.getFloatFormat(),
                SF::convert(fromType.getFloatFormat(), toType.getFloatFormat(),
                    env, loadFloat(sources[0],
                                   lane * fromType.getElementBits(),
                                   fromType.getFloatFormat()), flags));
        result = value;
        return EVAL_OK;
    }
    if ((operation == SemanticOperation::OP_SIGNED_TO_FLOAT) ||
        (operation == SemanticOperation::OP_UNSIGNED_TO_FLOAT))
    {
        const SemanticType toType = getFloatType(instruction);
        const SemanticOperand& operand = instruction.getSource(0);
        uint integerBits = toType.getElementBits();
        if (operand.isValid() &&
            (operand.getType().getKind() == SemanticType::KIND_INTEGER))
            integerBits = operand.getType().getElementBits();
        if (!SF::isSupported(toType.getFloatFormat()) || (integerBits > 64))
            return EVAL_UNDEFINED;
        for (uint lane = 0; lane < toType.getLanes(); lane++)
            storeFloat(value, lane * toType.getElementBits(),
                toType.getFloatFormat(),
                SF::fromInteger(toType.getFloatFormat(), env,
                    sources[0].getBits(lane * integerBits, integerBits),
                    integerBits,
                    operation == SemanticOperation::OP_SIGNED_TO_FLOAT,
                    flags));
        result = value;
        return EVAL_OK;
    }
    if ((operation == SemanticOperation::OP_FLOAT_TO_SIGNED) ||
        (operation == SemanticOperation::OP_FLOAT_TO_UNSIGNED) ||
        (operation == SemanticOperation::OP_FLOAT_CLASSIFY))
    {
        SemanticType fromType = getFloatSourceType(instruction, 0);
        // The integer type: the type of the operation or the destination
        SemanticType toType = instruction.getType();
        if (toType.getKind() != SemanticType::KIND_INTEGER)
            toType = instruction.getDestination().getType();
        const uint integerBits = toType.getElementBits();
        if (!SF::isSupported(fromType.getFloatFormat()) ||
            (integerBits < 1) || (integerBits > 64))
            return EVAL_UNDEFINED;
        for (uint lane = 0; lane < fromType.getLanes(); lane++)
        {
            const FloatBits a = loadFloat(sources[0],
                    lane * fromType.getElementBits(), fromType.getFloatFormat());
            uint64 r;
            if (operation == SemanticOperation::OP_FLOAT_CLASSIFY)
                r = SF::classify(fromType.getFloatFormat(), a);
            else
                r = SF::toInteger(fromType.getFloatFormat(), env, a,
                        integerBits,
                        operation == SemanticOperation::OP_FLOAT_TO_SIGNED,
                        instruction.getBehavior(), flags);
            value.setBits(lane * integerBits, integerBits, r);
        }
        result = value;
        return EVAL_OK;
    }

    // The operations with the type of the operation
    const SemanticType type = getFloatType(instruction);
    const SemanticType::FloatFormat format = type.getFloatFormat();
    if (!SF::isSupported(format))
        return EVAL_UNDEFINED;
    const uint stride = type.getElementBits();
    for (uint lane = 0; lane < type.getLanes(); lane++)
    {
        const uint offset = lane * stride;
        const FloatBits a = loadFloat(sources[0], offset, format);
        const FloatBits b = loadFloat(sources[1], offset, format);
        const FloatBits c = loadFloat(sources[2], offset, format);
        FloatBits r;
        switch (operation)
        {
        case SemanticOperation::OP_FLOAT_ADD:
            r = SF::add(format, env, a, b, flags); break;
        case SemanticOperation::OP_FLOAT_SUBTRACT:
            r = SF::subtract(format, env, a, b, flags); break;
        case SemanticOperation::OP_FLOAT_MULTIPLY:
            r = SF::multiply(format, env, a, b, flags); break;
        case SemanticOperation::OP_FLOAT_DIVIDE:
            r = SF::divide(format, env, a, b, flags); break;
        case SemanticOperation::OP_FLOAT_MULTIPLY_ADD:
            r = SF::multiplyAdd(format, env, a, b, c, flags); break;
        case SemanticOperation::OP_FLOAT_SQUARE_ROOT:
            r = SF::squareRoot(format, env, a, flags); break;
        case SemanticOperation::OP_FLOAT_MINIMUM:
        case SemanticOperation::OP_FLOAT_MAXIMUM:
            r = SF::minimumMaximum(format, env,
                    operation == SemanticOperation::OP_FLOAT_MAXIMUM,
                    instruction.getBehavior(), a, b, flags);
            break;
        case SemanticOperation::OP_FLOAT_ABSOLUTE:
            r = SF::absolute(format, a); break;
        case SemanticOperation::OP_FLOAT_NEGATE:
            r = SF::negate(format, a); break;
        case SemanticOperation::OP_FLOAT_COPY_SIGN:
            r = SF::copySign(format, a, b); break;
        case SemanticOperation::OP_FLOAT_ROUND:
            r = SF::roundToIntegral(format, env, a,
                    instruction.getKind() ==
                        SemanticOperation::FLOAT_ROUND_SIGNAL_INEXACT,
                    flags);
            break;
        case SemanticOperation::OP_FLOAT_SCALE:
            r = SF::scale(format, env, a, b, flags); break;
        case SemanticOperation::OP_FLOAT_GET_EXPONENT:
            r = SF::getExponent(format, env, a, flags); break;
        default:
            return EVAL_UNDEFINED;
        }
        storeFloat(value, offset, format, r);
    }
    result = value;
    return EVAL_OK;
}

// Evaluate an operation over values. See SemanticInterpreter::evaluate()
EvalStatus evaluateOperation(const SemanticInstruction& instruction,
                             const SemanticValue* sources,
                             SemanticValue& result,
                             FloatContext* context = NULL)
{
    if (usesFloat(instruction))
    {
        FloatContext local;
        if (context == NULL)
        {
            context = &local;
            defaultFloatEnvironment(instruction, local.m_env);
        }
        return evaluateFloat(instruction, sources, result, *context);
    }
    const SemanticOperation::Operation operation = instruction.getOperation();
    const SemanticType type = instruction.getType();
    const uint totalBits = type.getTotalBits();
    const uint sourcesCount = SemanticOperation::getSourcesCount(operation);
    SemanticValue value;

    switch (operation)
    {
    case SemanticOperation::OP_NOP:
        return EVAL_OK;

    //////////////////////////////////////////////////////////////////
    // Data movement and wide values
    //////////////////////////////////////////////////////////////////
    case SemanticOperation::OP_MOVE:
    case SemanticOperation::OP_BITCAST:
    case SemanticOperation::OP_TRUNCATE:
        value = sources[0];
        break;
    case SemanticOperation::OP_ZERO_EXTEND:
    case SemanticOperation::OP_SIGN_EXTEND:
    {
        const SemanticType extendFrom = getSourceType(instruction, 0);
        if (type.isVector() && extendFrom.isVector() &&
            (extendFrom.getLanes() == type.getLanes()) &&
            (extendFrom.getElementBits() < type.getElementBits()) &&
            (type.getElementBits() <= 64))
        {
            // pmovzx / pmovsx: each lane is extended
            const uint from = extendFrom.getElementBits();
            const uint to = type.getElementBits();
            for (uint lane = 0; lane < type.getLanes(); lane++)
            {
                uint64 v = sources[0].getBits(lane * from, from);
                if (operation == SemanticOperation::OP_SIGN_EXTEND)
                    v = (uint64)signExtend(v, from);
                value.setBits(lane * to, to, v);
            }
            break;
        }
        if (operation == SemanticOperation::OP_ZERO_EXTEND)
        {
            value = sources[0];
            value.truncate(extendFrom.getTotalBits());
            break;
        }
        const uint sourceBits = getSourceType(instruction, 0).getTotalBits();
        value = sources[0];
        value.truncate(sourceBits);
        if ((sourceBits >= 1) && (sourceBits < totalBits) &&
            (sources[0].getBits(sourceBits - 1, 1) != 0))
            fillOnes(value, sourceBits, totalBits);
        break;
    }
    case SemanticOperation::OP_EXTRACT:
    {
        const SemanticType sourceType = getSourceType(instruction, 0);
        uint offset = (uint)sources[1].getWord(0);
        // A lane index for a vector, a bit offset for the rest
        if (sourceType.isVector() && !type.isVector())
            offset *= sourceType.getElementBits();
        value = extractBits(sources[0], offset, totalBits);
        break;
    }
    case SemanticOperation::OP_INSERT:
    {
        const SemanticType insertedType = getSourceType(instruction, 1);
        uint offset = (uint)sources[2].getWord(0);
        if (type.isVector() && !insertedType.isVector())
            offset *= type.getElementBits();
        value = sources[0];
        const uint bits = insertedType.getTotalBits();
        if (offset < totalBits)
            copyBits(value, offset, sources[1],
                     0, (offset + bits > totalBits) ? (totalBits - offset)
                                                    : bits);
        break;
    }
    case SemanticOperation::OP_CONCAT:
    {
        const uint lowBits = getSourceType(instruction, 0).getTotalBits();
        value = sources[0];
        value.truncate(lowBits);
        if (lowBits < totalBits)
            copyBits(value, lowBits, sources[1], 0, totalBits - lowBits);
        break;
    }
    case SemanticOperation::OP_AND:
    case SemanticOperation::OP_OR:
    case SemanticOperation::OP_XOR:
    case SemanticOperation::OP_NOT:
    case SemanticOperation::OP_AND_NOT:
    case SemanticOperation::OP_BITWISE_SELECT:
        // Bit by bit, so any size
        for (uint i = 0; i < SemanticValue::WORDS_COUNT; i++)
        {
            const uint64 a = sources[0].getWord(i);
            const uint64 b = (sourcesCount > 1) ? sources[1].getWord(i) : 0;
            const uint64 c = (sourcesCount > 2) ? sources[2].getWord(i) : 0;
            uint64 r;
            switch (operation)
            {
            case SemanticOperation::OP_AND:     r = a & b; break;
            case SemanticOperation::OP_OR:      r = a | b; break;
            case SemanticOperation::OP_XOR:     r = a ^ b; break;
            case SemanticOperation::OP_NOT:     r = ~a; break;
            case SemanticOperation::OP_AND_NOT: r = ~a & b; break;
            default:                            r = (a & b) | (~a & c); break;
            }
            value.setWord(i, r);
        }
        break;
    case SemanticOperation::OP_SELECT:
        if (!type.isVector())
        {
            value = (sources[0] != SemanticValue()) ? sources[1] : sources[2];
        } else
        {
            // The mask has a bit for each lane
            const uint elementBits = type.getElementBits();
            for (uint lane = 0; lane < type.getLanes(); lane++)
            {
                const SemanticValue& chosen =
                            (sources[0].getBits(lane, 1) != 0) ? sources[1]
                                                               : sources[2];
                copyBits(value, lane * elementBits, chosen,
                         lane * elementBits, elementBits);
            }
        }
        break;
    case SemanticOperation::OP_BROADCAST:
        for (uint lane = 0; lane < type.getLanes(); lane++)
            copyBits(value, lane * type.getElementBits(), sources[0], 0,
                     type.getElementBits());
        break;
    case SemanticOperation::OP_SIGN_MASK:
    {
        const SemanticType sourceType = getSourceType(instruction, 0);
        for (uint lane = 0; lane < sourceType.getLanes(); lane++)
            value.setBits(lane, 1, sources[0].getBits(
                (lane + 1) * sourceType.getElementBits() - 1, 1));
        break;
    }

    //////////////////////////////////////////////////////////////////
    // Compare and reduce, lane by lane
    //////////////////////////////////////////////////////////////////
    case SemanticOperation::OP_COMPARE:
    {
        // The operands are the type of the compare, the result is a bit for
        // each lane
        const SemanticType sourceType = getSourceType(instruction, 0);
        const uint bits = sourceType.getElementBits();
        if ((bits < 1) || (bits > 64))
            return EVAL_UNDEFINED;
        for (uint lane = 0; lane < sourceType.getLanes(); lane++)
        {
            bool isTrue;
            if (!compareValues(instruction.getCondition(), bits,
                               sources[0].getBits(lane * bits, bits),
                               sources[1].getBits(lane * bits, bits),
                               isTrue))
                return EVAL_UNDEFINED;
            value.setBits(lane, 1, isTrue ? 1 : 0);
        }
        break;
    }
    case SemanticOperation::OP_REDUCE:
    {
        const SemanticType sourceType = getSourceType(instruction, 0);
        const uint bits = sourceType.getElementBits();
        if ((bits < 1) || (bits > 64))
            return EVAL_UNDEFINED;
        uint64 accumulator = sources[0].getBits(0, bits);
        for (uint lane = 1; lane < sourceType.getLanes(); lane++)
        {
            const EvalStatus status = evaluateLane(
                        instruction, instruction.getCombineOperation(), bits,
                        accumulator, sources[0].getBits(lane * bits, bits), 0,
                        accumulator);
            if (status != EVAL_OK)
                return status;
        }
        value.setWord(0, accumulator);
        break;
    }

    //////////////////////////////////////////////////////////////////
    // Lane permutations and the vector reductions
    //////////////////////////////////////////////////////////////////
    case SemanticOperation::OP_SHUFFLE:
    {
        const uint bits = type.getElementBits();
        const uint lanes = type.getLanes();
        uint indexBits = getSourceType(instruction, 2).getElementBits();
        if ((indexBits < 1) || (indexBits > 64))
            indexBits = 64;
        for (uint lane = 0; lane < lanes; lane++)
        {
            uint64 index = sources[2].getBits(lane * indexBits, indexBits);
            if (index >= 2 * (uint64)lanes)
            {
                if (instruction.getBehavior() ==
                    SemanticOperation::BEHAVIOR_SHUFFLE_ZERO)
                    continue;
                index %= 2 * (uint64)lanes;
            }
            const SemanticValue& source =
                            (index < lanes) ? sources[0] : sources[1];
            copyBits(value, lane * bits, source,
                     (uint)(index % lanes) * bits, bits);
        }
        break;
    }
    case SemanticOperation::OP_INTERLEAVE:
    {
        const uint bits = type.getElementBits();
        const uint lanes = type.getLanes();
        const uint start = (uint)sources[2].getWord(0);
        for (uint k = 0; k < lanes / 2; k++)
        {
            if (start + k >= lanes)
                break;
            copyBits(value, 2 * k * bits, sources[0], (start + k) * bits, bits);
            copyBits(value, (2 * k + 1) * bits, sources[1], (start + k) * bits,
                     bits);
        }
        break;
    }
    case SemanticOperation::OP_SLIDE:
    {
        const uint bits = type.getElementBits();
        const uint lanes = type.getLanes();
        const uint64 start = sources[2].getWord(0);
        for (uint lane = 0; lane < lanes; lane++)
        {
            const uint64 index = start + lane;
            if (index >= 2 * (uint64)lanes)
                continue;
            if (index < lanes)
                copyBits(value, lane * bits, sources[0], (uint)index * bits,
                         bits);
            else
                copyBits(value, lane * bits, sources[1],
                         (uint)(index - lanes) * bits, bits);
        }
        break;
    }
    case SemanticOperation::OP_NARROW:
    {
        const uint bits = type.getElementBits();
        if (bits > 64)
            return EVAL_UNDEFINED;
        SemanticType sourceType = getSourceType(instruction, 0);
        uint sourceBits = sourceType.getElementBits();
        uint sourceLanes = sourceType.getLanes();
        if (sourceBits <= bits)
        {
            // The operand has no type: the elements are twice as wide
            sourceBits = bits * 2;
            sourceLanes = type.getLanes() / 2;
        }
        if (sourceBits > 64)
            return EVAL_UNDEFINED;
        const uint attributes = instruction.getAttributes();
        for (uint lane = 0; lane < sourceLanes * 2; lane++)
        {
            const SemanticValue& source = (lane < sourceLanes) ? sources[0]
                                                               : sources[1];
            const uint index = (lane < sourceLanes) ? lane
                                                    : (lane - sourceLanes);
            // The source elements are signed numbers
            int64 v = signExtend(source.getBits(index * sourceBits,
                                                sourceBits), sourceBits);
            if ((attributes &
                 SemanticInstruction::ATTRIBUTE_SATURATE_SIGNED) != 0)
            {
                const int64 high = (int64)(maskOf(bits) >> 1);
                const int64 low = -high - 1;
                v = (v > high) ? high : ((v < low) ? low : v);
            } else if ((attributes &
                        SemanticInstruction::ATTRIBUTE_SATURATE_UNSIGNED) != 0)
            {
                const int64 high = (bits >= 63) ? (int64)(maskOf(63))
                                                : (int64)maskOf(bits);
                v = (v > high) ? high : ((v < 0) ? 0 : v);
            }
            value.setBits(lane * bits, bits, (uint64)v);
        }
        break;
    }
    case SemanticOperation::OP_PAIRWISE_ADD:
    {
        const uint bits = type.getElementBits();
        if (bits > 64)
            return EVAL_UNDEFINED;
        const uint half = type.getLanes() / 2;
        for (uint lane = 0; lane < type.getLanes(); lane++)
        {
            const SemanticValue& source = (lane < half) ? sources[0]
                                                        : sources[1];
            const uint j = (lane < half) ? lane : (lane - half);
            value.setBits(lane * bits, bits,
                          source.getBits(2 * j * bits, bits) +
                          source.getBits((2 * j + 1) * bits, bits));
        }
        break;
    }
    case SemanticOperation::OP_DOT_PRODUCT:
    {
        const uint bits = type.getElementBits();
        if (bits > 64)
            return EVAL_UNDEFINED;
        uint sourceBits = getSourceType(instruction, 0).getElementBits();
        if ((sourceBits >= bits) || (sourceBits < 1))
            sourceBits = bits / 2;
        const uint ratio = bits / sourceBits;
        const SemanticOperation::Behavior behavior = instruction.getBehavior();
        for (uint lane = 0; lane < type.getLanes(); lane++)
        {
            uint64 sum = sources[2].getBits(lane * bits, bits);
            for (uint j = 0; j < ratio; j++)
            {
                const uint offset = (lane * ratio + j) * sourceBits;
                uint64 a = sources[0].getBits(offset, sourceBits);
                uint64 b = sources[1].getBits(offset, sourceBits);
                if (behavior != SemanticOperation::BEHAVIOR_UNSIGNED)
                {
                    if (behavior == SemanticOperation::BEHAVIOR_SIGNED)
                        a = (uint64)signExtend(a, sourceBits);
                    b = (uint64)signExtend(b, sourceBits);
                }
                sum += a * b;
            }
            value.setBits(lane * bits, bits, sum);
        }
        break;
    }
    case SemanticOperation::OP_COMPRESS:
    case SemanticOperation::OP_EXPAND:
    {
        const uint bits = type.getElementBits();
        uint next = 0;
        for (uint lane = 0; lane < type.getLanes(); lane++)
        {
            if (sources[1].getBits(lane, 1) == 0)
                continue;
            if (operation == SemanticOperation::OP_COMPRESS)
                copyBits(value, next * bits, sources[0], lane * bits, bits);
            else
                copyBits(value, lane * bits, sources[0], next * bits, bits);
            next++;
        }
        break;
    }
    case SemanticOperation::OP_LANE_INDEX:
    {
        const uint bits = type.getElementBits();
        if (bits > 64)
            return EVAL_UNDEFINED;
        for (uint lane = 0; lane < type.getLanes(); lane++)
            value.setBits(lane * bits, bits,
                          sources[0].getWord(0) +
                          (uint64)lane * sources[1].getWord(0));
        break;
    }
    case SemanticOperation::OP_ACTIVE_LANE_MASK:
    {
        uint sourceBits = getSourceType(instruction, 0).getTotalBits();
        if ((sourceBits < 1) || (sourceBits > 64))
            sourceBits = 64;
        const uint64 first = sources[0].getBits(0, sourceBits);
        const uint64 limit = sources[1].getBits(0, sourceBits);
        for (uint lane = 0; lane < type.getLanes(); lane++)
            value.setBits(lane, 1, ((first + lane) < limit) ? 1 : 0);
        break;
    }
    case SemanticOperation::OP_AES_SUB_SHIFT:
    case SemanticOperation::OP_AES_INVERSE_SUB_SHIFT:
    case SemanticOperation::OP_AES_MIX_COLUMNS:
    case SemanticOperation::OP_AES_INVERSE_MIX_COLUMNS:
        if (totalBits != 128)
            return EVAL_UNDEFINED;
        aesStep(sources[0],
                (operation == SemanticOperation::OP_AES_INVERSE_SUB_SHIFT) ||
                (operation == SemanticOperation::OP_AES_INVERSE_MIX_COLUMNS),
                (operation == SemanticOperation::OP_AES_MIX_COLUMNS) ||
                (operation == SemanticOperation::OP_AES_INVERSE_MIX_COLUMNS),
                value);
        break;
    case SemanticOperation::OP_MASK_COUNT:
    case SemanticOperation::OP_MASK_FIRST:
    {
        const uint lanes = getSourceType(instruction, 0).getTotalBits();
        uint count = 0;
        int64 first = -1;
        for (uint lane = 0; lane < lanes; lane++)
        {
            if (sources[0].getBits(lane, 1) == 0)
                continue;
            if (first < 0)
                first = lane;
            count++;
        }
        value.setWord(0, (operation == SemanticOperation::OP_MASK_COUNT) ?
                                (uint64)count : (uint64)first);
        break;
    }

    //////////////////////////////////////////////////////////////////
    // The rest work on each lane of a number of up to 64 bits
    //////////////////////////////////////////////////////////////////
    default:
    {
        const uint bits = type.getElementBits();
        if ((bits < 1) || (bits > 64) ||
            (totalBits > SemanticValue::MAX_BITS))
            return EVAL_UNDEFINED;
        for (uint lane = 0; lane < type.getLanes(); lane++)
        {
            uint64 operands[SemanticInstruction::MAX_SOURCES] = { 0, 0, 0 };
            for (uint i = 0; i < sourcesCount; i++)
                operands[i] = sources[i].getBits(lane * bits, bits);
            uint64 r;
            const EvalStatus status = evaluateLane(
                        instruction, operation, bits,
                        operands[0], operands[1], operands[2], r);
            if (status != EVAL_OK)
                return status;
            value.setBits(lane * bits, bits, r);
        }
        break;
    }
    }

    value.truncate(totalBits);
    result = value;
    return EVAL_OK;
}

// Return the value of a register, a sub-register is read from its parent
SemanticValue loadRegister(const ArchitectureDescription& architecture,
                           const cArray<SemanticValue>& registers,
                           uint registerId)
{
    const ArchitectureRegister& reg = architecture.getRegister(registerId);
    if (reg.m_isZero)
        return SemanticValue();
    if (reg.m_parent == ArchitectureRegister::NO_PARENT)
        return registers[registerId];
    return extractBits(registers[reg.m_parent], reg.m_bitOffset, reg.m_bits);
}

// Change a register, a sub-register changes its bits of the parent
void storeRegister(const ArchitectureDescription& architecture,
                   cArray<SemanticValue>& registers,
                   uint registerId, const SemanticValue& value,
                   bool shouldZeroParent)
{
    const ArchitectureRegister& reg = architecture.getRegister(registerId);
    if (reg.m_isZero)
        return;

    SemanticValue truncated(value);
    truncated.truncate(reg.m_bits);
    if (reg.m_parent == ArchitectureRegister::NO_PARENT)
    {
        registers[registerId] = truncated;
        return;
    }

    SemanticValue parent = registers[reg.m_parent];
    if (reg.m_zeroExtendsParent || shouldZeroParent)
        parent = SemanticValue();
    copyBits(parent, reg.m_bitOffset, truncated, 0, reg.m_bits);
    registers[reg.m_parent] = parent;
}

// Read 'bits' bits of memory, in the byte order of the architecture
bool readMemory(SemanticMemory& memory, bool isLittleEndian, uint64 address,
                uint bits, SemanticValue& value)
{
    CHECK((bits >= 8) && ((bits % 8) == 0) &&
          (bits <= SemanticValue::MAX_BITS));
    const uint size = bits / 8;
    cBuffer buffer(size);
    if (!memory.read(address, buffer.getBuffer(), size))
        return false;
    value = SemanticValue();
    for (uint i = 0; i < size; i++)
        value.setBits((isLittleEndian ? i : (size - 1 - i)) * 8, 8, buffer[i]);
    return true;
}

// Write 'bits' bits of memory
bool writeMemory(SemanticMemory& memory, bool isLittleEndian, uint64 address,
                 uint bits, const SemanticValue& value)
{
    CHECK((bits >= 8) && ((bits % 8) == 0) &&
          (bits <= SemanticValue::MAX_BITS));
    const uint size = bits / 8;
    cBuffer buffer(size);
    for (uint i = 0; i < size; i++)
        buffer[i] = (uint8)value.getBits(
                        (isLittleEndian ? i : (size - 1 - i)) * 8, 8);
    return memory.write(address, buffer.getBuffer(), size);
}

// All the flags
const uint32 gAllFlags = SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_ZERO |
                         SemanticFlags::FLAG_SIGN |
                         SemanticFlags::FLAG_OVERFLOW |
                         SemanticFlags::FLAG_PARITY |
                         SemanticFlags::FLAG_ADJUST |
                         SemanticFlags::FLAG_DIRECTION;

} // namespace

/////////////////////////////////////////////////////////////////////////////
// SemanticInterpreter
/////////////////////////////////////////////////////////////////////////////

SemanticInterpreter::SemanticInterpreter(
                            const ArchitectureDescriptionPtr& architecture,
                            SemanticMemory& memory) :
    m_architecture(architecture),
    m_memory(memory),
    m_registers(architecture->getRegistersCount()),
    m_block(NULL),
    m_resumeIndex(0),
    m_stepsLeft(0),
    m_nextAddress(0),
    m_trapKind(0),
    m_trapNumber(0),
    m_faultAddress(0),
    m_helperNumber(0)
{
}

SemanticValue SemanticInterpreter::getRegister(uint registerId) const
{
    return loadRegister(*m_architecture, m_registers, registerId);
}

void SemanticInterpreter::setRegister(uint registerId,
                                      const SemanticValue& value)
{
    storeRegister(*m_architecture, m_registers, registerId, value, false);
}

uint64 SemanticInterpreter::getNextAddress() const
{
    return m_nextAddress;
}

uint SemanticInterpreter::getTrapKind() const
{
    return m_trapKind;
}

uint64 SemanticInterpreter::getTrapNumber() const
{
    return m_trapNumber;
}

uint64 SemanticInterpreter::getFaultAddress() const
{
    return m_faultAddress;
}

uint SemanticInterpreter::getHelperNumber() const
{
    return m_helperNumber;
}

const SemanticValue& SemanticInterpreter::getHelperArgument(uint index) const
{
    CHECK(index < 2);
    return m_helperArguments[index];
}

bool SemanticInterpreter::evaluate(const SemanticInstruction& instruction,
                                   const SemanticValue* sources,
                                   SemanticValue& result)
{
    return evaluateOperation(instruction, sources, result) == EVAL_OK;
}

/////////////////////////////////////////////////////////////////////////////
// Flags
/////////////////////////////////////////////////////////////////////////////

uint32 SemanticInterpreter::evaluateFlags(SemanticOperation::FlagRecipe recipe,
                                          const SemanticType& type,
                                          const SemanticValue& s0,
                                          const SemanticValue& s1,
                                          const SemanticValue& s2)
{
    if ((recipe == SemanticOperation::FLAGS_FLOAT_COMPARE) &&
        isFloatType(type) &&
        SemanticFloat::isSupported(type.getFloatFormat()))
    {
        // A quiet compare in the default environment
        uint32 ignored = 0;
        return floatCompareFlags(SemanticFloat::compare(
                type.getFloatFormat(), SemanticFloat::Environment(),
                loadFloat(s0, 0, type.getFloatFormat()),
                loadFloat(s1, 0, type.getFloatFormat()), false, ignored));
    }
    const uint n = type.getTotalBits();
    CHECK((n >= 1) && (n <= 64));
    const uint64 mask = maskOf(n);
    const uint64 a = s0.getBits(0, n);
    const uint64 b = s1.getBits(0, n);
    const uint64 r = s2.getBits(0, n);
    // The shift count can be wider than the shifted value (shl eax, cl)
    const uint64 count = s1.getWord(0);
    uint32 flags = 0;

    // The flags of a result, for the recipes which have one
    switch (recipe)
    {
    case SemanticOperation::FLAGS_ADD:
    case SemanticOperation::FLAGS_SUBTRACT:
    case SemanticOperation::FLAGS_LOGIC:
    case SemanticOperation::FLAGS_SHIFT_LEFT:
    case SemanticOperation::FLAGS_SHIFT_RIGHT_LOGICAL:
    case SemanticOperation::FLAGS_SHIFT_RIGHT_ARITHMETIC:
        if (r == 0)
            flags |= SemanticFlags::FLAG_ZERO;
        if (isNegative(r, n))
            flags |= SemanticFlags::FLAG_SIGN;
        if ((countOnes(r & 0xFF) & 1) == 0)
            flags |= SemanticFlags::FLAG_PARITY;
        break;
    default:
        break;
    }

    switch (recipe)
    {
    case SemanticOperation::FLAGS_ADD:
        if (isNegative((a & b) | ((a | b) & ~r), n))
            flags |= SemanticFlags::FLAG_CARRY;
        if (isNegative((a ^ r) & (b ^ r), n))
            flags |= SemanticFlags::FLAG_OVERFLOW;
        if (((a ^ b ^ r) & 0x10) != 0)
            flags |= SemanticFlags::FLAG_ADJUST;
        break;
    case SemanticOperation::FLAGS_SUBTRACT:
        if (isNegative((~a & b) | ((~a | b) & r), n))
            flags |= SemanticFlags::FLAG_CARRY;
        if (isNegative((a ^ b) & (a ^ r), n))
            flags |= SemanticFlags::FLAG_OVERFLOW;
        if (((a ^ b ^ r) & 0x10) != 0)
            flags |= SemanticFlags::FLAG_ADJUST;
        break;
    case SemanticOperation::FLAGS_LOGIC:
        // The carry, the overflow and the adjust are 0
        break;
    case SemanticOperation::FLAGS_SHIFT_LEFT:
    {
        const bool isCarry = (count >= 1) && (count <= n) &&
                             (((a >> (n - count)) & 1) != 0);
        if (isCarry)
            flags |= SemanticFlags::FLAG_CARRY;
        if (isNegative(r, n) != isCarry)
            flags |= SemanticFlags::FLAG_OVERFLOW;
        break;
    }
    case SemanticOperation::FLAGS_SHIFT_RIGHT_LOGICAL:
        if ((count <= n) && (count >= 1) && (((a >> (count - 1)) & 1) != 0))
            flags |= SemanticFlags::FLAG_CARRY;
        if (isNegative(a, n))
            flags |= SemanticFlags::FLAG_OVERFLOW;
        break;
    case SemanticOperation::FLAGS_SHIFT_RIGHT_ARITHMETIC:
    {
        const uint64 position = ((count < n) ? count : n) - 1;
        if ((count >= 1) && (((a >> position) & 1) != 0))
            flags |= SemanticFlags::FLAG_CARRY;
        break;
    }
    case SemanticOperation::FLAGS_ROTATE_LEFT:
    {
        const bool isCarry = (r & 1) != 0;
        if (isCarry)
            flags |= SemanticFlags::FLAG_CARRY;
        if (isNegative(r, n) != isCarry)
            flags |= SemanticFlags::FLAG_OVERFLOW;
        break;
    }
    case SemanticOperation::FLAGS_ROTATE_RIGHT:
    {
        if (isNegative(r, n))
            flags |= SemanticFlags::FLAG_CARRY;
        if (isNegative(r, n) != (n >= 2 && (((r >> (n - 2)) & 1) != 0)))
            flags |= SemanticFlags::FLAG_OVERFLOW;
        break;
    }
    case SemanticOperation::FLAGS_MULTIPLY_UNSIGNED:
        if (b != 0)
            flags |= SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW;
        break;
    case SemanticOperation::FLAGS_MULTIPLY_SIGNED:
        // The high half is not the sign extension of the low half
        if (b != (isNegative(a, n) ? mask : 0))
            flags |= SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW;
        break;
    case SemanticOperation::FLAGS_EXPLICIT:
        // The caller keeps the flags of its mask
        if ((a & 1) != 0)
            flags = gAllFlags;
        break;
    default:
        // TODO: FLAGS_FLOAT_COMPARE and FLAGS_PREDICATE_TEST are not
        // interpreted yet (run() ends with EXIT_UNDEFINED)
        break;
    }
    return flags;
}

uint32 SemanticInterpreter::getFlags() const
{
    uint32 flags = 0;
    for (uint32 flag = 1; flag <= SemanticFlags::FLAG_DIRECTION; flag <<= 1)
    {
        uint registerId;
        if (m_architecture->getFlagRegister(flag, registerId) &&
            (getRegister(registerId).getBits(0, 1) != 0))
            flags |= flag;
    }
    return flags;
}

bool SemanticInterpreter::evaluateCondition(
                                    SemanticOperation::Condition condition,
                                    uint32 flags)
{
    const bool carry = (flags & SemanticFlags::FLAG_CARRY) != 0;
    const bool zero = (flags & SemanticFlags::FLAG_ZERO) != 0;
    const bool sign = (flags & SemanticFlags::FLAG_SIGN) != 0;
    const bool overflow = (flags & SemanticFlags::FLAG_OVERFLOW) != 0;
    const bool parity = (flags & SemanticFlags::FLAG_PARITY) != 0;
    switch (condition)
    {
    case SemanticOperation::COND_NONE:
    case SemanticOperation::COND_ALWAYS:
        return true;
    case SemanticOperation::COND_NEVER:
        return false;
    case SemanticOperation::COND_EQUAL:
        return zero;
    case SemanticOperation::COND_NOT_EQUAL:
        return !zero;
    case SemanticOperation::COND_UNSIGNED_LESS:
    case SemanticOperation::COND_CARRY:
        return carry;
    case SemanticOperation::COND_UNSIGNED_GREATER_EQUAL:
    case SemanticOperation::COND_NOT_CARRY:
        return !carry;
    case SemanticOperation::COND_UNSIGNED_LESS_EQUAL:
        return carry || zero;
    case SemanticOperation::COND_UNSIGNED_GREATER:
        return !carry && !zero;
    case SemanticOperation::COND_SIGNED_LESS:
        return sign != overflow;
    case SemanticOperation::COND_SIGNED_GREATER_EQUAL:
        return sign == overflow;
    case SemanticOperation::COND_SIGNED_LESS_EQUAL:
        return zero || (sign != overflow);
    case SemanticOperation::COND_SIGNED_GREATER:
        return !zero && (sign == overflow);
    case SemanticOperation::COND_NEGATIVE:
        return sign;
    case SemanticOperation::COND_NOT_NEGATIVE:
        return !sign;
    case SemanticOperation::COND_OVERFLOW:
        return overflow;
    case SemanticOperation::COND_NOT_OVERFLOW:
        return !overflow;
    case SemanticOperation::COND_PARITY_EVEN:
        return parity;
    case SemanticOperation::COND_PARITY_ODD:
        return !parity;
    default:
        // The float conditions are for OP_COMPARE of floats only
        return false;
    }
}

/////////////////////////////////////////////////////////////////////////////
// Execution
/////////////////////////////////////////////////////////////////////////////

SemanticValue SemanticInterpreter::read(const SemanticOperand& operand) const
{
    SemanticValue value;
    switch (operand.getKind())
    {
    case SemanticOperand::KIND_REGISTER:
        value = loadRegister(*m_architecture, m_registers, operand.getIndex());
        break;
    case SemanticOperand::KIND_TEMPORARY:
        CHECK(operand.getIndex() < m_temporaries.getSize());
        value = m_temporaries[operand.getIndex()];
        break;
    case SemanticOperand::KIND_IMMEDIATE:
    case SemanticOperand::KIND_GUEST_ADDRESS:
        value.setWord(0, (uint64)operand.getValue());
        // The immediate is a signed 64 bit number
        if (operand.getValue() < 0)
        {
            for (uint i = 1; i < SemanticValue::WORDS_COUNT; i++)
                value.setWord(i, ~(uint64)0);
        }
        break;
    default:
        // KIND_NONE and KIND_LABEL are not values
        return value;
    }
    if (!operand.getType().isVoid())
        value.truncate(operand.getType().getTotalBits());
    return value;
}

void SemanticInterpreter::write(const SemanticOperand& operand,
                                const SemanticValue& value,
                                uint32 attributes)
{
    SemanticValue truncated(value);
    if (!operand.getType().isVoid())
        truncated.truncate(operand.getType().getTotalBits());
    switch (operand.getKind())
    {
    case SemanticOperand::KIND_REGISTER:
        storeRegister(*m_architecture, m_registers, operand.getIndex(),
                      truncated,
                      (attributes & SemanticInstruction::ATTRIBUTE_ZERO_PARENT)
                                                                        != 0);
        break;
    case SemanticOperand::KIND_TEMPORARY:
        CHECK(operand.getIndex() < m_temporaries.getSize());
        m_temporaries[operand.getIndex()] = truncated;
        break;
    default:
        // Only registers and temporaries are written
        CHECK_FAIL();
    }
}

uint64 SemanticInterpreter::getGuestAddress(uint index) const
{
    // The last guest instruction which starts at or before the instruction
    uint64 address = m_block->getFallThroughAddress().getAddress();
    bool isFound = false;
    const cList<SemanticBlock::GuestInstruction>& guests =
                                        m_block->getGuestInstructions();
    for (cList<SemanticBlock::GuestInstruction>::iterator i = guests.begin();
         i != guests.end(); i++)
    {
        if ((*i).m_firstInstruction > index)
            break;
        address = (*i).m_address.getAddress();
        isFound = true;
    }
    // Before the first marker (or without markers) the block has no better
    // address than its first guest instruction
    if (!isFound && (guests.begin() != guests.end()))
        address = (*guests.begin()).m_address.getAddress();
    return address;
}

/////////////////////////////////////////////////////////////////////////////
// Floating point environment
/////////////////////////////////////////////////////////////////////////////

void SemanticInterpreter::buildFloatEnvironment(
                            const SemanticInstruction& instruction,
                            SemanticFloat::Environment& environment) const
{
    defaultFloatEnvironment(instruction, environment);
    FloatEnvironment guest;
    if (!m_architecture->getFloatEnvironment(instruction.getAttributes(),
                                             guest))
        return;
    const uint64 control = getRegister(guest.m_controlRegister).getWord(0);
    if (instruction.getRoundingMode() == SemanticOperation::ROUND_DYNAMIC)
    {
        if (guest.m_roundingOffset != FloatEnvironment::NO_FIELD)
            environment.m_rounding = (SemanticOperation::RoundingMode)
                guest.m_roundingModes[(control >> guest.m_roundingOffset) & 3];
    }
    if (guest.m_precisionOffset != FloatEnvironment::NO_FIELD)
    {
        const uint field = (uint)((control >> guest.m_precisionOffset) & 3);
        environment.m_precision = (field == 0) ? 24 : ((field == 2) ? 53 : 64);
    }
    if (guest.m_flushToZeroOffset != FloatEnvironment::NO_FIELD)
        environment.m_flushToZero =
                ((control >> guest.m_flushToZeroOffset) & 1) != 0;
    if (guest.m_denormalsAreZeroOffset != FloatEnvironment::NO_FIELD)
        environment.m_denormalsAreZero =
                ((control >> guest.m_denormalsAreZeroOffset) & 1) != 0;
    environment.m_nanRule = guest.m_nanRule;
    environment.m_isDefaultNanNegative = guest.m_isDefaultNanNegative;
    // The underflow mask
    const uint underflow = 4;
    const bool maskBit =
        ((control >> guest.m_maskOffsets[underflow]) & 1) != 0;
    environment.m_isUnderflowUnmasked =
        (guest.m_trapMode != FloatEnvironment::TRAP_NONE) &&
        (guest.m_isMaskSetDisabled ? !maskBit : maskBit);
}

SemanticInterpreter::FloatCommit SemanticInterpreter::commitFloatFlags(
                            const SemanticInstruction& instruction,
                            uint32 flags, uint index)
{
    FloatEnvironment guest;
    if (((instruction.getAttributes() &
          SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS) == 0) ||
        (flags == 0) ||
        !m_architecture->getFloatEnvironment(instruction.getAttributes(),
                                             guest))
        return FLOAT_COMMIT_OK;

    SemanticValue status = getRegister(guest.m_statusRegister);
    const uint64 control = getRegister(guest.m_controlRegister).getWord(0);
    uint32 unmasked = 0;
    for (uint i = 0; i < SemanticFloatFlags::FLOAT_FLAGS_COUNT; i++)
    {
        if ((flags & ((uint32)1 << i)) == 0)
            continue;
        status.setBits(guest.m_flagOffsets[i], 1, 1);
        const bool maskBit = ((control >> guest.m_maskOffsets[i]) & 1) != 0;
        const bool isMasked = guest.m_isMaskSetDisabled ? maskBit : !maskBit;
        if (!isMasked)
            unmasked |= (uint32)1 << i;
    }
    if ((unmasked != 0) && (guest.m_trapMode == FloatEnvironment::TRAP_DEFERRED))
    {
        if (guest.m_summaryOffset != FloatEnvironment::NO_FIELD)
            status.setBits(guest.m_summaryOffset, 1, 1);
        if (guest.m_busyOffset != FloatEnvironment::NO_FIELD)
            status.setBits(guest.m_busyOffset, 1, 1);
    }
    setRegister(guest.m_statusRegister, status);
    if (unmasked == 0)
        return FLOAT_COMMIT_OK;
    if (guest.m_trapMode == FloatEnvironment::TRAP_IMMEDIATE)
    {
        m_trapKind = SemanticOperation::TRAP_FAULT;
        m_trapNumber = guest.m_trapNumber;
        m_nextAddress = getGuestAddress(index);
        return FLOAT_COMMIT_TRAP;
    }
    if (guest.m_trapMode == FloatEnvironment::TRAP_DEFERRED)
    {
        // x87: the invalid operation, denormal operand and zero divide
        // exceptions are faults, the instruction doesn't write its result. The
        // overflow, underflow and precision exceptions are traps: the result
        // is written (the masked response, not the scaled result of the
        // processor for an unmasked overflow or underflow)
        const uint32 faults = SemanticFloatFlags::FLOAT_INVALID |
                              SemanticFloatFlags::FLOAT_DENORMAL |
                              SemanticFloatFlags::FLOAT_DIVIDE_BY_ZERO;
        if ((unmasked & faults) != 0)
            return FLOAT_COMMIT_SUPPRESS;
    }
    return FLOAT_COMMIT_OK;
}


SemanticInterpreter::ExitReason SemanticInterpreter::run(
                                            const SemanticBlock& block,
                                            uint maxSteps)
{
    m_block = &block;
    m_stepsLeft = maxSteps;

    // The instructions by index, and the positions of the labels
    m_instructions = cArray<const SemanticInstruction*>(
                                    block.getInstructions().length());
    m_labels = cArray<uint>(block.getLabelsCount());
    for (uint i = 0; i < m_labels.getSize(); i++)
        m_labels[i] = 0xFFFFFFFF;
    uint index = 0;
    for (cList<SemanticInstruction>::iterator i =
                    block.getInstructions().begin();
         i != block.getInstructions().end(); i++)
    {
        const SemanticInstruction& instruction = *i;
        m_instructions[index] = &instruction;
        if (instruction.getOperation() == SemanticOperation::OP_LABEL)
        {
            const uint label = instruction.getSource(0).getIndex();
            CHECK(label < m_labels.getSize());
            m_labels[label] = index;
        }
        index++;
    }

    // The temporaries are written once, in every run
    m_temporaries = cArray<SemanticValue>(block.getTemporariesCount());

    const ExitReason reason = execute(0);
    setRegister(m_architecture->getProgramCounter(),
                SemanticValue(m_nextAddress));
    return reason;
}

SemanticInterpreter::ExitReason SemanticInterpreter::resume(
                                            const SemanticValue& result)
{
    CHECK((m_block != NULL) && (m_resumeIndex > 0));
    // The helper is the instruction before the resume point
    const SemanticInstruction& helper = *m_instructions[m_resumeIndex - 1];
    if (helper.getDestination().isValid())
        write(helper.getDestination(), result, helper.getAttributes());

    const ExitReason reason = execute(m_resumeIndex);
    setRegister(m_architecture->getProgramCounter(),
                SemanticValue(m_nextAddress));
    return reason;
}

SemanticInterpreter::ExitReason SemanticInterpreter::execute(uint index)
{
    const bool isLittleEndian = m_architecture->isLittleEndian();
    const uint count = m_instructions.getSize();

    while (index < count)
    {
        if (m_stepsLeft == 0)
        {
            m_nextAddress = getGuestAddress(index);
            return EXIT_STEPS_LIMIT;
        }
        m_stepsLeft--;

        const SemanticInstruction& instruction = *m_instructions[index];
        const SemanticOperation::Operation operation =
                                                instruction.getOperation();
        const SemanticOperand& destination = instruction.getDestination();
        const uint32 attributes = instruction.getAttributes();
        uint next = index + 1;

        switch (operation)
        {
        case SemanticOperation::OP_NOP:
        case SemanticOperation::OP_LABEL:
        case SemanticOperation::OP_FENCE:
        case SemanticOperation::OP_FENCE_INSTRUCTION:
        case SemanticOperation::OP_PREFETCH:
        case SemanticOperation::OP_HINT:
            // Nothing is observable in a single processor
            break;
        case SemanticOperation::OP_CACHE_MAINTENANCE:
            // TODO: CACHE_ZERO is a store of a cache line, which depends on
            // the size of the line of the processor
            if (instruction.getKind() == SemanticOperation::CACHE_ZERO)
            {
                m_nextAddress = getGuestAddress(index);
                return EXIT_UNDEFINED;
            }
            break;
        case SemanticOperation::OP_UNDEFINED:
            m_nextAddress = getGuestAddress(index);
            return EXIT_UNDEFINED;

        //////////////////////////////////////////////////////////////////
        // Control flow
        //////////////////////////////////////////////////////////////////
        case SemanticOperation::OP_BRANCH:
        case SemanticOperation::OP_BRANCH_INDIRECT:
        case SemanticOperation::OP_CALL:
        case SemanticOperation::OP_RETURN:
        case SemanticOperation::OP_BRANCH_CONDITIONAL:
        {
            const bool isConditional =
                    (operation == SemanticOperation::OP_BRANCH_CONDITIONAL);
            if (isConditional &&
                (read(instruction.getSource(0)) == SemanticValue()))
                break;

            const SemanticOperand& target =
                    instruction.getSource(isConditional ? 1 : 0);
            if (target.getKind() == SemanticOperand::KIND_LABEL)
            {
                CHECK(target.getIndex() < m_labels.getSize());
                next = m_labels[target.getIndex()];
                CHECK(next != 0xFFFFFFFF);
                break;
            }
            m_nextAddress = read(target).getWord(0);
            return EXIT_BRANCH;
        }
        case SemanticOperation::OP_TRAP:
            m_trapKind = instruction.getKind();
            m_trapNumber = read(instruction.getSource(0)).getWord(0);
            m_nextAddress = getGuestAddress(index);
            return EXIT_TRAP;
        case SemanticOperation::OP_HELPER:
            m_helperNumber = (uint)read(instruction.getSource(0)).getWord(0);
            m_helperArguments[0] = read(instruction.getSource(1));
            m_helperArguments[1] = read(instruction.getSource(2));
            m_resumeIndex = index + 1;
            m_nextAddress = getGuestAddress(index);
            return EXIT_HELPER;

        //////////////////////////////////////////////////////////////////
        // Memory
        //////////////////////////////////////////////////////////////////
        case SemanticOperation::OP_LOAD:
        case SemanticOperation::OP_ATOMIC_EXCHANGE:
        case SemanticOperation::OP_ATOMIC_COMPARE_EXCHANGE:
        case SemanticOperation::OP_ATOMIC_RMW:
        {
            const uint64 address = read(instruction.getSource(0)).getWord(0);
            const uint bits = getResultBits(instruction);
            SemanticValue old;
            if (!readMemory(m_memory, isLittleEndian, address, bits, old))
            {
                m_faultAddress = address;
                m_nextAddress = getGuestAddress(index);
                return EXIT_MEMORY_FAULT;
            }

            // The value to write, if any
            bool shouldWrite = true;
            SemanticValue newValue;
            if (operation == SemanticOperation::OP_LOAD)
            {
                shouldWrite = false;
            } else if (operation == SemanticOperation::OP_ATOMIC_EXCHANGE)
            {
                newValue = read(instruction.getSource(1));
            } else if (operation ==
                       SemanticOperation::OP_ATOMIC_COMPARE_EXCHANGE)
            {
                SemanticValue expected = read(instruction.getSource(1));
                expected.truncate(bits);
                shouldWrite = (old == expected);
                newValue = read(instruction.getSource(2));
            } else
            {
                // memory = old <combine operation> s1. OP_AND_NOT clears
                // the bits of s1: old & ~s1, so s1 goes first
                const SemanticOperation::Operation combineOperation =
                    instruction.getCombineOperation();
                const SemanticInstruction combine(combineOperation,
                            instruction.getType(), SemanticOperand());
                const bool isClear =
                    (combineOperation == SemanticOperation::OP_AND_NOT);
                const SemanticValue value = read(instruction.getSource(1));
                const SemanticValue operands[3] = {
                    isClear ? value : old, isClear ? old : value,
                    SemanticValue() };
                if (evaluateOperation(combine, operands, newValue) != EVAL_OK)
                {
                    m_nextAddress = getGuestAddress(index);
                    return EXIT_UNDEFINED;
                }
            }

            if (shouldWrite &&
                !writeMemory(m_memory, isLittleEndian, address, bits, newValue))
            {
                m_faultAddress = address;
                m_nextAddress = getGuestAddress(index);
                return EXIT_MEMORY_FAULT;
            }
            if (destination.isValid())
                write(destination, old, attributes);
            break;
        }
        case SemanticOperation::OP_STORE:
        {
            const uint64 address = read(instruction.getSource(0)).getWord(0);
            const uint bits = getSourceType(instruction, 1).getTotalBits();
            if (!writeMemory(m_memory, isLittleEndian, address, bits,
                             read(instruction.getSource(1))))
            {
                m_faultAddress = address;
                m_nextAddress = getGuestAddress(index);
                return EXIT_MEMORY_FAULT;
            }
            break;
        }

        case SemanticOperation::OP_LOAD_MASKED:
        case SemanticOperation::OP_STORE_MASKED:
        {
            // The lanes of the mask are accessed, the others don't fault
            const bool isLoad =
                        (operation == SemanticOperation::OP_LOAD_MASKED);
            const uint64 address = read(instruction.getSource(0)).getWord(0);
            const SemanticType vectorType = isLoad ?
                    ((instruction.getType().isVoid()) ?
                        destination.getType() : instruction.getType()) :
                    getSourceType(instruction, 1);
            const SemanticValue mask =
                    read(instruction.getSource(isLoad ? 1 : 2));
            const SemanticValue stored = isLoad ? SemanticValue() :
                                         read(instruction.getSource(1));
            const uint laneBits = vectorType.getElementBits();
            CHECK((laneBits >= 8) && ((laneBits % 8) == 0));
            const uint laneBytes = laneBits / 8;
            SemanticValue loaded;
            // The old bytes of the written lanes, to undo a partial store
            cList<uint> written;
            cArray<SemanticValue> olds(vectorType.getLanes());
            bool isFailed = false;
            uint64 failedAddress = address;
            for (uint lane = 0; (lane < vectorType.getLanes()) && !isFailed;
                 lane++)
            {
                if (mask.getBits(lane, 1) == 0)
                    continue;
                const uint64 laneAddress = address + (uint64)lane * laneBytes;
                SemanticValue old;
                if (!readMemory(m_memory, isLittleEndian, laneAddress,
                                laneBits, old))
                {
                    isFailed = true;
                    failedAddress = laneAddress;
                    break;
                }
                if (isLoad)
                {
                    copyBits(loaded, lane * laneBits, old, 0, laneBits);
                    continue;
                }
                SemanticValue laneValue;
                copyBits(laneValue, 0, stored, lane * laneBits, laneBits);
                if (!writeMemory(m_memory, isLittleEndian, laneAddress,
                                 laneBits, laneValue))
                {
                    isFailed = true;
                    failedAddress = laneAddress;
                    break;
                }
                olds[lane] = old;
                written.append(lane);
            }
            if (isFailed)
            {
                // Undo the lanes already written
                for (cList<uint>::iterator i = written.begin();
                     i != written.end(); i++)
                    writeMemory(m_memory, isLittleEndian,
                                address + (uint64)(*i) * laneBytes, laneBits,
                                olds[*i]);
                m_faultAddress = failedAddress;
                m_nextAddress = getGuestAddress(index);
                return EXIT_MEMORY_FAULT;
            }
            if (isLoad && destination.isValid())
                write(destination, loaded, attributes);
            break;
        }

        //////////////////////////////////////////////////////////////////
        // Flags and system registers
        //////////////////////////////////////////////////////////////////
        case SemanticOperation::OP_SET_FLAGS:
        {
            const SemanticOperation::FlagRecipe recipe =
                                                instruction.getFlagRecipe();
            const uint bits = instruction.getType().getTotalBits();
            const bool isFloatCompare =
                (recipe == SemanticOperation::FLAGS_FLOAT_COMPARE) &&
                isFloatType(instruction.getType()) &&
                SemanticFloat::isSupported(
                            instruction.getType().getFloatFormat());
            if (!isFloatCompare &&
                ((recipe == SemanticOperation::FLAGS_FLOAT_COMPARE) ||
                 (recipe == SemanticOperation::FLAGS_PREDICATE_TEST) ||
                 (bits < 1) || (bits > 64)))
            {
                m_nextAddress = getGuestAddress(index);
                return EXIT_UNDEFINED;
            }

            uint32 flags;
            if ((instruction.getCondition() != SemanticOperation::COND_NONE) &&
                !evaluateCondition(instruction.getCondition(), getFlags()))
            {
                // The condition of a conditional compare failed
                flags = instruction.getFalseFlags();
            } else if (isFloatCompare)
            {
                // Kind 1: a signaling compare (comisd, fcomi) raises
                // FLOAT_INVALID for a quiet NaN too
                SemanticFloat::Environment environment;
                buildFloatEnvironment(instruction, environment);
                uint32 floatFlags = 0;
                const SemanticType::FloatFormat format =
                                    instruction.getType().getFloatFormat();
                flags = floatCompareFlags(SemanticFloat::compare(
                    format, environment,
                    loadFloat(read(instruction.getSource(0)), 0, format),
                    loadFloat(read(instruction.getSource(1)), 0, format),
                    instruction.getKind() ==
                        SemanticOperation::FLOAT_COMPARE_SIGNALING,
                    floatFlags));
                const FloatCommit commit = commitFloatFlags(
                                        instruction, floatFlags, index);
                if (commit == FLOAT_COMMIT_TRAP)
                    return EXIT_TRAP;
                if (commit == FLOAT_COMMIT_SUPPRESS)
                    break;
            } else
            {
                flags = evaluateFlags(recipe, instruction.getType(),
                                      read(instruction.getSource(0)),
                                      read(instruction.getSource(1)),
                                      read(instruction.getSource(2)));
            }
            for (uint32 flag = 1; flag <= SemanticFlags::FLAG_DIRECTION;
                 flag <<= 1)
            {
                uint registerId;
                if (((instruction.getFlagMask() & flag) != 0) &&
                    m_architecture->getFlagRegister(flag, registerId))
                    setRegister(registerId, SemanticValue(
                                                (flags & flag) != 0 ? 1 : 0));
            }
            break;
        }
        case SemanticOperation::OP_TEST_FLAGS:
        {
            const SemanticOperation::Condition condition =
                                                instruction.getCondition();
            if (condition >= SemanticOperation::COND_FLOAT_ORDERED)
            {
                m_nextAddress = getGuestAddress(index);
                return EXIT_UNDEFINED;
            }
            write(destination, SemanticValue(
                    evaluateCondition(condition, getFlags()) ? 1 : 0),
                  attributes);
            break;
        }
        case SemanticOperation::OP_READ_SYSTEM_REGISTER:
            write(destination,
                  getRegister((uint)instruction.getSource(0).getValue()),
                  attributes);
            break;
        case SemanticOperation::OP_WRITE_SYSTEM_REGISTER:
            setRegister((uint)instruction.getSource(0).getValue(),
                        read(instruction.getSource(1)));
            break;
        case SemanticOperation::OP_READ_REGISTER_INDEXED:
        {
            // register (s0 + (s1 mod s2))
            const uint count = (uint)read(instruction.getSource(2)).getWord(0);
            CHECK(count != 0);
            const uint index =
                (uint)(read(instruction.getSource(1)).getWord(0) % count);
            write(destination,
                  getRegister(instruction.getSource(0).getIndex() + index),
                  attributes);
            break;
        }
        case SemanticOperation::OP_WRITE_REGISTER_INDEXED:
        {
            // register (d + (s0 mod s1)) = s2
            const uint count = (uint)read(instruction.getSource(1)).getWord(0);
            CHECK(count != 0);
            const uint index =
                (uint)(read(instruction.getSource(0)).getWord(0) % count);
            setRegister(destination.getIndex() + index,
                        read(instruction.getSource(2)));
            break;
        }

        //////////////////////////////////////////////////////////////////
        // The operations which compute a value
        //////////////////////////////////////////////////////////////////
        default:
        {
            SemanticValue sources[SemanticInstruction::MAX_SOURCES];
            const uint sourcesCount =
                                SemanticOperation::getSourcesCount(operation);
            for (uint i = 0; i < sourcesCount; i++)
                sources[i] = read(instruction.getSource(i));

            SemanticValue result;
            FloatContext floatContext;
            const bool isFloat = usesFloat(instruction);
            if (isFloat)
                buildFloatEnvironment(instruction, floatContext.m_env);
            const EvalStatus status = evaluateOperation(instruction, sources,
                        result, isFloat ? &floatContext : NULL);
            if (status == EVAL_FAULT)
            {
                // The divide error exception
                m_trapKind = SemanticOperation::TRAP_FAULT;
                m_trapNumber = 0;
                m_nextAddress = getGuestAddress(index);
                return EXIT_TRAP;
            }
            if (status == EVAL_UNDEFINED)
            {
                m_nextAddress = getGuestAddress(index);
                return EXIT_UNDEFINED;
            }
            if (isFloat)
            {
                if ((attributes & SemanticInstruction::ATTRIBUTE_IGNORE_DENORMAL)
                    != 0)
                    floatContext.m_flags &= ~(uint32)
                                SemanticFloatFlags::FLOAT_DENORMAL;
                const FloatCommit commit = commitFloatFlags(
                                instruction, floatContext.m_flags, index);
                if (commit == FLOAT_COMMIT_TRAP)
                    return EXIT_TRAP;
                if (commit == FLOAT_COMMIT_SUPPRESS)
                    break;
            }
            if (SemanticOperation::hasDestination(operation) &&
                destination.isValid())
                write(destination, result, attributes);
            break;
        }
        }
        index = next;
    }

    m_nextAddress = m_block->getFallThroughAddress().getAddress();
    return EXIT_FALL_THROUGH;
}
