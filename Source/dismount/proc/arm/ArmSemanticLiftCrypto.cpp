/*
 * ArmSemanticLiftCrypto.cpp
 *
 * Implementation file
 *
 * The lifters of the AArch32 dot products (vsdot, vudot, vusdot, vsudot), the
 * 8 bit integer matrix multiplies (vsmmla, vummla, vusmmla) and the
 * cryptographic extensions: aese, aesd, aesmc, aesimc, the SHA-1 and SHA-256
 * instructions (written out round by round from the pseudocode of the Arm
 * ARM) and vmull.p64.
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

namespace {

// A 32 bit rotation
Val rotateLeft(armdis::a32::NeonBuilder& b, const Val& x, uint amount)
{
    return b.op(Op::OP_ROTATE_LEFT, 32, x, b.imm(amount, 32));
}

Val rotateRight(armdis::a32::NeonBuilder& b, const Val& x, uint amount)
{
    return b.op(Op::OP_ROTATE_RIGHT, 32, x, b.imm(amount, 32));
}

Val add32(armdis::a32::NeonBuilder& b, const Val& x, const Val& y)
{
    return b.op(Op::OP_ADD, 32, x, y);
}

Val xor32(armdis::a32::NeonBuilder& b, const Val& x, const Val& y)
{
    return b.op(Op::OP_XOR, 32, x, y);
}

Val majority(armdis::a32::NeonBuilder& b, const Val& x, const Val& y,
             const Val& z)
{
    return b.op(Op::OP_OR, 32, b.op(Op::OP_AND, 32, x, y),
                b.op(Op::OP_AND, 32, xor32(b, x, y), z));
}

// Return the operand 'index' of the instruction as a Q register value
Val quad(armdis::a32::NeonBuilder& b, uint index)
{
    return b.reg(index);
}

} // namespace

namespace armdis {
namespace a32 {

/////////////////////////////////////////////////////////////////////////////
// The dot products and the matrix multiplies
/////////////////////////////////////////////////////////////////////////////

void liftNeonDot(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    // The signedness of the first (Vn) and the second (Vm) operand
    bool isFirstSigned = true;
    bool isSecondSigned = true;
    if (n.is("vudot"))
    {
        isFirstSigned = false;
        isSecondSigned = false;
    } else if (n.is("vusdot"))
    {
        isFirstSigned = false;
    } else if (n.is("vsudot"))
    {
        isSecondSigned = false;
    }
    const uint size = b.regBits(0);
    const uint lanes = size / 32;
    const Val accumulator = b.reg(0);
    const Val first = b.reg(1);
    const ArmDecodedOperand& second = c.getOperand(2);
    const bool isByElement = second.m_elementIndex >= 0;
    Val element;
    Val other;
    if (isByElement)
        element = b.lane(c.getRegister(second.m_register),
                         (uint)second.m_elementIndex, 32);
    else
        other = b.reg(2);
    Val result[4];
    for (uint i = 0; i < lanes; i++)
    {
        Val sum = b.lane(accumulator, i, 32);
        const Val group = b.lane(first, i, 32);
        const Val secondGroup = isByElement ? element : b.lane(other, i, 32);
        for (uint j = 0; j < 4; j++)
        {
            const Val x = b.ext(b.lane(group, j, 8), 32, isFirstSigned);
            const Val y = b.ext(b.lane(secondGroup, j, 8), 32, isSecondSigned);
            sum = add32(b, sum, b.op(Op::OP_MULTIPLY, 32, x, y));
        }
        result[i] = sum;
    }
    c.write(0, b.pack(result, lanes, 32));
}

// vsmmla, vummla, vusmmla: Vd (2x2 of 32 bits) += Vn (2x8) * Vm (2x8)^T
void liftNeonMatrixMultiply(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const bool isFirstSigned = n.is("vsmmla");
    const bool isSecondSigned = !n.is("vummla");
    const Val accumulator = b.reg(0);
    const Val first = b.reg(1);
    const Val second = b.reg(2);
    Val result[4];
    for (uint i = 0; i < 2; i++)
    {
        for (uint j = 0; j < 2; j++)
        {
            Val sum = b.lane(accumulator, 2 * i + j, 32);
            for (uint k = 0; k < 8; k++)
            {
                const Val x = b.ext(b.lane(first, 8 * i + k, 8), 32, isFirstSigned);
                const Val y = b.ext(b.lane(second, 8 * j + k, 8), 32, isSecondSigned);
                sum = add32(b, sum, b.op(Op::OP_MULTIPLY, 32, x, y));
            }
            result[2 * i + j] = sum;
        }
    }
    c.write(0, b.pack(result, 4, 32));
}

/////////////////////////////////////////////////////////////////////////////
// AES
/////////////////////////////////////////////////////////////////////////////

void liftNeonCryptoAes(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    Val result;
    if (n.is("aese"))
        result = b.op(Op::OP_AES_SUB_SHIFT, 128,
                      b.op(Op::OP_XOR, 128, quad(b, 0), quad(b, 1)));
    else if (n.is("aesd"))
        result = b.op(Op::OP_AES_INVERSE_SUB_SHIFT, 128,
                      b.op(Op::OP_XOR, 128, quad(b, 0), quad(b, 1)));
    else if (n.is("aesmc"))
        result = b.op(Op::OP_AES_MIX_COLUMNS, 128, quad(b, 1));
    else
        result = b.op(Op::OP_AES_INVERSE_MIX_COLUMNS, 128, quad(b, 1));
    c.write(0, result);
}

/////////////////////////////////////////////////////////////////////////////
// SHA-1 and SHA-256
/////////////////////////////////////////////////////////////////////////////

namespace {

// The four 32 bit lanes of a q value
void split4(NeonBuilder& b, const Val& value, Val* lanes)
{
    for (uint i = 0; i < 4; i++)
        lanes[i] = b.lane(value, i, 32);
}

// sha256hash(X, Y, W, part1) of the Arm ARM
Val sha256Hash(NeonBuilder& b, const Val& xValue, const Val& yValue,
               const Val& wValue, bool isPart1)
{
    // The 256 bit Y:X as eight lanes, X in the low four
    Val l[8];
    split4(b, xValue, l);
    split4(b, yValue, l + 4);
    Val w[4];
    split4(b, wValue, w);
    for (uint e = 0; e < 4; e++)
    {
        const Val chs = b.op(Op::OP_BITWISE_SELECT, 32, l[4], l[5], l[6]);
        const Val maj = majority(b, l[0], l[1], l[2]);
        const Val sigma1 = xor32(b, xor32(b, rotateRight(b, l[4], 6),
                                          rotateRight(b, l[4], 11)),
                                 rotateRight(b, l[4], 25));
        const Val sigma0 = xor32(b, xor32(b, rotateRight(b, l[0], 2),
                                          rotateRight(b, l[0], 13)),
                                 rotateRight(b, l[0], 22));
        const Val t = add32(b, add32(b, add32(b, l[7], sigma1), chs), w[e]);
        l[3] = add32(b, t, l[3]);
        l[7] = add32(b, add32(b, t, sigma0), maj);
        // <Y, X> = ROL(Y : X, 32)
        const Val top = l[7];
        for (int k = 7; k > 0; k--)
            l[k] = l[k - 1];
        l[0] = top;
    }
    return b.pack(isPart1 ? l : l + 4, 4, 32);
}

} // namespace

void liftNeonCryptoSha(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    Val result;
    if (n.is("sha1h"))
    {
        result = b.ext(rotateLeft(b, b.lane(quad(b, 1), 0, 32), 30), 128, false);
    } else if (n.is("sha1su1"))
    {
        Val x[4], y[4], t[4], r[4];
        split4(b, quad(b, 0), x);
        split4(b, quad(b, 1), y);
        // T = X xor (Y >> 32)
        t[0] = xor32(b, x[0], y[1]);
        t[1] = xor32(b, x[1], y[2]);
        t[2] = xor32(b, x[2], y[3]);
        t[3] = x[3];
        r[0] = rotateLeft(b, t[0], 1);
        r[1] = rotateLeft(b, t[1], 1);
        r[2] = rotateLeft(b, t[2], 1);
        r[3] = xor32(b, rotateLeft(b, t[3], 1), rotateLeft(b, t[0], 2));
        result = b.pack(r, 4, 32);
    } else if (n.is("sha1su0"))
    {
        // (Y<63:0> : X<127:64>) xor X xor Z
        Val x[4], y[4], z[4], r[4];
        split4(b, quad(b, 0), x);
        split4(b, quad(b, 1), y);
        split4(b, quad(b, 2), z);
        r[0] = xor32(b, xor32(b, x[2], x[0]), z[0]);
        r[1] = xor32(b, xor32(b, x[3], x[1]), z[1]);
        r[2] = xor32(b, xor32(b, y[0], x[2]), z[2]);
        r[3] = xor32(b, xor32(b, y[1], x[3]), z[3]);
        result = b.pack(r, 4, 32);
    } else if (n.is("sha1c") || n.is("sha1p") || n.is("sha1m"))
    {
        Val x[4], w[4];
        split4(b, quad(b, 0), x);
        split4(b, quad(b, 2), w);
        Val y = b.lane(quad(b, 1), 0, 32);
        for (uint e = 0; e < 4; e++)
        {
            Val t;
            if (n.is("sha1c"))
                t = b.op(Op::OP_BITWISE_SELECT, 32, x[1], x[2], x[3]);
            else if (n.is("sha1p"))
                t = xor32(b, xor32(b, x[1], x[2]), x[3]);
            else
                t = majority(b, x[1], x[2], x[3]);
            y = add32(b, add32(b, add32(b, y, rotateLeft(b, x[0], 5)), t), w[e]);
            const Val rotated = rotateLeft(b, x[1], 30);
            // <Y, X> = the next state: a' = Y, b' = a, c' = ROL(b, 30), d' = c,
            // e' = d
            const Val d = x[3];
            x[3] = x[2];
            x[2] = rotated;
            x[1] = x[0];
            x[0] = y;
            y = d;
        }
        result = b.pack(x, 4, 32);
    } else if (n.is("sha256h"))
    {
        result = sha256Hash(b, quad(b, 0), quad(b, 1), quad(b, 2), true);
    } else if (n.is("sha256h2"))
    {
        result = sha256Hash(b, quad(b, 1), quad(b, 0), quad(b, 2), false);
    } else if (n.is("sha256su0"))
    {
        Val x[4], m[4], t[4], r[4];
        split4(b, quad(b, 0), x);
        split4(b, quad(b, 1), m);
        t[0] = x[1];
        t[1] = x[2];
        t[2] = x[3];
        t[3] = m[0];
        for (uint e = 0; e < 4; e++)
        {
            const Val s = xor32(b, xor32(b, rotateRight(b, t[e], 7),
                                         rotateRight(b, t[e], 18)),
                                b.op(Op::OP_SHIFT_RIGHT_LOGICAL, 32, t[e], b.imm(3, 32)));
            r[e] = add32(b, x[e], s);
        }
        result = b.pack(r, 4, 32);
    } else
    {
        // sha256su1
        Val x[4], y[4], z[4], t0[4], r[4];
        split4(b, quad(b, 0), x);
        split4(b, quad(b, 1), y);
        split4(b, quad(b, 2), z);
        t0[0] = y[1];
        t0[1] = y[2];
        t0[2] = y[3];
        t0[3] = z[0];
        Val t1[2] = { z[2], z[3] };
        for (uint e = 0; e < 4; e++)
        {
            if (e == 2)
            {
                t1[0] = r[0];
                t1[1] = r[1];
            }
            const Val elt = t1[e & 1];
            const Val s = xor32(b, xor32(b, rotateRight(b, elt, 17),
                                         rotateRight(b, elt, 19)),
                                b.op(Op::OP_SHIFT_RIGHT_LOGICAL, 32, elt, b.imm(10, 32)));
            r[e] = add32(b, add32(b, s, x[e]), t0[e]);
        }
        result = b.pack(r, 4, 32);
    }
    c.write(0, result);
}

// vmull.p64 Qd, Dn, Dm: the 128 bit carry-less product
void liftNeonPolynomial64(ArmLiftContext& c)
{
    NeonBuilder b(c);
    const Val x = b.reg(1);
    const Val y = b.reg(2);
    Val lanes[2];
    lanes[0] = b.op(Op::OP_CARRYLESS_MULTIPLY, 64, x, y);
    lanes[1] = b.op(Op::OP_CARRYLESS_MULTIPLY_HIGH, 64, x, y);
    c.write(0, b.pack(lanes, 2, 64));
}

} // namespace a32
} // namespace armdis
