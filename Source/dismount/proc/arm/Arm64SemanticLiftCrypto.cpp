/*
 * Arm64SemanticLiftCrypto.cpp
 *
 * Implementation file
 *
 * The A64 lifter of AES, SHA1, SHA256, SHA512, SHA3 (eor3, bcax, rax1, xar),
 * SM3, SM4 and the polynomial multiply (pmull), see Arm64SemanticSimd.h.
 * The semantics are the pseudocode of the Arm ARM (SHA1C, SHA256H, SHA512H,
 * SM3TT1A, SM4E...) written on the 32 / 64 bit words of the registers with
 * the rotate, logic and addition operations of the IR.
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

// The S-box of SM4
const uint8 gSm4Sbox[256] =
{
    0xd6, 0x90, 0xe9, 0xfe, 0xcc, 0xe1, 0x3d, 0xb7, 0x16, 0xb6, 0x14, 0xc2, 0x28, 0xfb, 0x2c, 0x05,
    0x2b, 0x67, 0x9a, 0x76, 0x2a, 0xbe, 0x04, 0xc3, 0xaa, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99,
    0x9c, 0x42, 0x50, 0xf4, 0x91, 0xef, 0x98, 0x7a, 0x33, 0x54, 0x0b, 0x43, 0xed, 0xcf, 0xac, 0x62,
    0xe4, 0xb3, 0x1c, 0xa9, 0xc9, 0x08, 0xe8, 0x95, 0x80, 0xdf, 0x94, 0xfa, 0x75, 0x8f, 0x3f, 0xa6,
    0x47, 0x07, 0xa7, 0xfc, 0xf3, 0x73, 0x17, 0xba, 0x83, 0x59, 0x3c, 0x19, 0xe6, 0x85, 0x4f, 0xa8,
    0x68, 0x6b, 0x81, 0xb2, 0x71, 0x64, 0xda, 0x8b, 0xf8, 0xeb, 0x0f, 0x4b, 0x70, 0x56, 0x9d, 0x35,
    0x1e, 0x24, 0x0e, 0x5e, 0x63, 0x58, 0xd1, 0xa2, 0x25, 0x22, 0x7c, 0x3b, 0x01, 0x21, 0x78, 0x87,
    0xd4, 0x00, 0x46, 0x57, 0x9f, 0xd3, 0x27, 0x52, 0x4c, 0x36, 0x02, 0xe7, 0xa0, 0xc4, 0xc8, 0x9e,
    0xea, 0xbf, 0x8a, 0xd2, 0x40, 0xc7, 0x38, 0xb5, 0xa3, 0xf7, 0xf2, 0xce, 0xf9, 0x61, 0x15, 0xa1,
    0xe0, 0xae, 0x5d, 0xa4, 0x9b, 0x34, 0x1a, 0x55, 0xad, 0x93, 0x32, 0x30, 0xf5, 0x8c, 0xb1, 0xe3,
    0x1d, 0xf6, 0xe2, 0x2e, 0x82, 0x66, 0xca, 0x60, 0xc0, 0x29, 0x23, 0xab, 0x0d, 0x53, 0x4e, 0x6f,
    0xd5, 0xdb, 0x37, 0x45, 0xde, 0xfd, 0x8e, 0x2f, 0x03, 0xff, 0x6a, 0x72, 0x6d, 0x6c, 0x5b, 0x51,
    0x8d, 0x1b, 0xaf, 0x92, 0xbb, 0xdd, 0xbc, 0x7f, 0x11, 0xd9, 0x5c, 0x41, 0x1f, 0x10, 0x5a, 0xd8,
    0x0a, 0xc1, 0x31, 0x88, 0xa5, 0xcd, 0x7b, 0xbd, 0x2d, 0x74, 0xd0, 0x12, 0xb8, 0xe5, 0xb4, 0xb0,
    0x89, 0x69, 0x97, 0x4a, 0x0c, 0x96, 0x77, 0x7e, 0x65, 0xb9, 0xf1, 0x09, 0xc5, 0x6e, 0xc6, 0x84,
    0x18, 0xf0, 0x7d, 0xec, 0x3a, 0xdc, 0x4d, 0x20, 0x79, 0xee, 0x5f, 0x3e, 0xd7, 0xcb, 0x39, 0x48,
};

const SemanticType I32 = SemanticType::integer(32);
const SemanticType I64 = SemanticType::integer(64);

// The 32 bit words of a 128 bit register value
Val words(SimdLift& s, const Val& value)
{
    return s.cast(SimdLift::V(32, 4), value);
}

Val dwords(SimdLift& s, const Val& value)
{
    return s.cast(SimdLift::V(64, 2), value);
}

// The register number of operand 'index'
Val readOperand(SimdLift& s, uint index)
{
    return s.readV(s.registerNumber(index));
}

// A 128 bit value of four 32 bit words
Val fromWords(SimdLift& s, const Val w[4])
{
    Val v = s.splat(SimdLift::V(32, 4), 0);
    for (uint i = 0; i < 4; i++)
        v = s.setLane(v, w[i], i);
    return v;
}

Val rol(SimdLift& s, const Val& x, uint n)
{
    return s.op(Op::OP_ROTATE_LEFT, x.getType(), x, s.imm(n, x.getType()));
}

Val ror(SimdLift& s, const Val& x, uint n)
{
    return s.op(Op::OP_ROTATE_RIGHT, x.getType(), x, s.imm(n, x.getType()));
}

Val lsr(SimdLift& s, const Val& x, uint n)
{
    return s.op(Op::OP_SHIFT_RIGHT_LOGICAL, x.getType(), x, s.imm(n, x.getType()));
}

Val xor2(SimdLift& s, const Val& a, const Val& b)
{
    return s.op(Op::OP_XOR, a.getType(), a, b);
}

Val xor3(SimdLift& s, const Val& a, const Val& b, const Val& c)
{
    return xor2(s, xor2(s, a, b), c);
}

Val and2(SimdLift& s, const Val& a, const Val& b)
{
    return s.op(Op::OP_AND, a.getType(), a, b);
}

Val or2(SimdLift& s, const Val& a, const Val& b)
{
    return s.op(Op::OP_OR, a.getType(), a, b);
}

Val add2(SimdLift& s, const Val& a, const Val& b)
{
    return s.op(Op::OP_ADD, a.getType(), a, b);
}

Val not1(SimdLift& s, const Val& a)
{
    return s.op(Op::OP_NOT, a.getType(), a);
}

// choose(x, y, z) = (x & y) | (~x & z)
Val choose(SimdLift& s, const Val& x, const Val& y, const Val& z)
{
    return or2(s, and2(s, x, y), and2(s, not1(s, x), z));
}

// majority(x, y, z)
Val majority(SimdLift& s, const Val& x, const Val& y, const Val& z)
{
    return or2(s, and2(s, x, y), and2(s, or2(s, x, y), z));
}

/////////////////////////////////////////////////////////////////////////////
// AES
/////////////////////////////////////////////////////////////////////////////

bool liftAes(SimdLift& s)
{
    Op::Operation operation;
    bool isRound = true;
    if (s.is("aese"))
        operation = Op::OP_AES_SUB_SHIFT;
    else if (s.is("aesd"))
        operation = Op::OP_AES_INVERSE_SUB_SHIFT;
    else if (s.is("aesmc"))
    {
        operation = Op::OP_AES_MIX_COLUMNS;
        isRound = false;
    } else if (s.is("aesimc"))
    {
        operation = Op::OP_AES_INVERSE_MIX_COLUMNS;
        isRound = false;
    } else
        return false;
    Val state;
    if (isRound)
        state = xor2(s, readOperand(s, 0), readOperand(s, 1));
    else
        state = readOperand(s, 1);
    s.c().write(0, s.op(operation, SimdLift::I(128), s.cast(SimdLift::I(128), state)));
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// SHA1
/////////////////////////////////////////////////////////////////////////////

bool liftSha1(SimdLift& s)
{
    ArmLiftContext& c = s.c();
    if (s.is("sha1h"))
    {
        Val x = s.trunc(32, readOperand(s, 1));
        c.write(0, rol(s, x, 30));
        return true;
    }
    if (s.is("sha1c") || s.is("sha1m") || s.is("sha1p"))
    {
        Val xv = words(s, readOperand(s, 0));
        Val x[4];
        for (uint i = 0; i < 4; i++)
            x[i] = s.lane(xv, i);
        Val y = s.trunc(32, readOperand(s, 1));
        Val w = words(s, readOperand(s, 2));
        for (uint e = 0; e < 4; e++)
        {
            Val t;
            if (s.is("sha1c"))
                t = choose(s, x[1], x[2], x[3]);
            else if (s.is("sha1p"))
                t = xor3(s, x[1], x[2], x[3]);
            else
                t = majority(s, x[1], x[2], x[3]);
            y = add2(s, add2(s, add2(s, y, rol(s, x[0], 5)), t), s.lane(w, e));
            x[1] = rol(s, x[1], 30);
            // <Y, X> = ROL(Y:X, 32)
            Val oldTop = x[3];
            x[3] = x[2];
            x[2] = x[1];
            x[1] = x[0];
            x[0] = y;
            y = oldTop;
        }
        c.write(0, fromWords(s, x));
        return true;
    }
    if (s.is("sha1su0"))
    {
        Val op1 = dwords(s, readOperand(s, 0));
        Val op2 = dwords(s, readOperand(s, 1));
        Val op3 = readOperand(s, 2);
        Val t = s.setLane(s.splat(SimdLift::V(64, 2), 0), s.lane(op1, 1), 0);
        t = s.setLane(t, s.lane(op2, 0), 1);
        Val r = xor3(s, s.cast(SimdLift::I(128), t), s.cast(SimdLift::I(128), op1), op3);
        c.write(0, r);
        return true;
    }
    if (s.is("sha1su1"))
    {
        Val op1 = words(s, readOperand(s, 0));
        Val op2 = words(s, readOperand(s, 1));
        Val t[4];
        for (uint i = 0; i < 4; i++)
        {
            // op1 EOR (op2 >> 32): the top word is not changed
            t[i] = (i < 3) ? xor2(s, s.lane(op1, i), s.lane(op2, i + 1))
                           : s.lane(op1, i);
        }
        Val r[4];
        for (uint i = 0; i < 3; i++)
            r[i] = rol(s, t[i], 1);
        r[3] = xor2(s, rol(s, t[3], 1), rol(s, t[0], 2));
        c.write(0, fromWords(s, r));
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// SHA256
/////////////////////////////////////////////////////////////////////////////

Val bigSigma0(SimdLift& s, const Val& x)
{
    return xor3(s, ror(s, x, 2), ror(s, x, 13), ror(s, x, 22));
}

Val bigSigma1(SimdLift& s, const Val& x)
{
    return xor3(s, ror(s, x, 6), ror(s, x, 11), ror(s, x, 25));
}

Val smallSigma0(SimdLift& s, const Val& x)
{
    return xor3(s, ror(s, x, 7), ror(s, x, 18), lsr(s, x, 3));
}

Val smallSigma1(SimdLift& s, const Val& x)
{
    return xor3(s, ror(s, x, 17), ror(s, x, 19), lsr(s, x, 10));
}

// SHA256hash(X, Y, W, part1)
void sha256Hash(SimdLift& s, const Val& xValue, const Val& yValue,
                const Val& wValue, bool isPart1)
{
    Val xv = words(s, xValue);
    Val yv = words(s, yValue);
    Val w = words(s, wValue);
    Val x[4], y[4];
    for (uint i = 0; i < 4; i++)
    {
        x[i] = s.lane(xv, i);
        y[i] = s.lane(yv, i);
    }
    for (uint e = 0; e < 4; e++)
    {
        Val chs = choose(s, y[0], y[1], y[2]);
        Val maj = majority(s, x[0], x[1], x[2]);
        Val t = add2(s, add2(s, add2(s, y[3], bigSigma1(s, y[0])), chs),
                     s.lane(w, e));
        x[3] = add2(s, t, x[3]);
        y[3] = add2(s, add2(s, t, bigSigma0(s, x[0])), maj);
        // <Y, X> = ROL(Y:X, 32)
        Val topY = y[3];
        Val topX = x[3];
        y[3] = y[2]; y[2] = y[1]; y[1] = y[0]; y[0] = topX;
        x[3] = x[2]; x[2] = x[1]; x[1] = x[0]; x[0] = topY;
    }
    s.c().write(0, fromWords(s, isPart1 ? x : y));
}

bool liftSha256(SimdLift& s)
{
    ArmLiftContext& c = s.c();
    if (s.is("sha256h"))
    {
        sha256Hash(s, readOperand(s, 0), readOperand(s, 1), readOperand(s, 2), true);
        return true;
    }
    if (s.is("sha256h2"))
    {
        sha256Hash(s, readOperand(s, 1), readOperand(s, 0), readOperand(s, 2), false);
        return true;
    }
    if (s.is("sha256su0"))
    {
        Val d = words(s, readOperand(s, 0));
        Val n = words(s, readOperand(s, 1));
        // T = n[0] : d[3..1]
        Val r[4];
        for (uint e = 0; e < 4; e++)
        {
            Val t = (e < 3) ? s.lane(d, e + 1) : s.lane(n, 0);
            r[e] = add2(s, smallSigma0(s, t), s.lane(d, e));
        }
        c.write(0, fromWords(s, r));
        return true;
    }
    if (s.is("sha256su1"))
    {
        Val d = words(s, readOperand(s, 0));
        Val n = words(s, readOperand(s, 1));
        Val m = words(s, readOperand(s, 2));
        Val t0[4];
        for (uint e = 0; e < 4; e++)
            t0[e] = (e < 3) ? s.lane(n, e + 1) : s.lane(m, 0);
        Val r[4];
        // T1 = m<127:64>
        r[0] = add2(s, add2(s, smallSigma1(s, s.lane(m, 2)), s.lane(d, 0)), t0[0]);
        r[1] = add2(s, add2(s, smallSigma1(s, s.lane(m, 3)), s.lane(d, 1)), t0[1]);
        r[2] = add2(s, add2(s, smallSigma1(s, r[0]), s.lane(d, 2)), t0[2]);
        r[3] = add2(s, add2(s, smallSigma1(s, r[1]), s.lane(d, 3)), t0[3]);
        c.write(0, fromWords(s, r));
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// SHA512 and SHA3
/////////////////////////////////////////////////////////////////////////////

Val sig512S0(SimdLift& s, const Val& x)
{
    return xor3(s, ror(s, x, 28), ror(s, x, 34), ror(s, x, 39));
}

Val sig512S1(SimdLift& s, const Val& x)
{
    return xor3(s, ror(s, x, 14), ror(s, x, 18), ror(s, x, 41));
}

Val sig512s0(SimdLift& s, const Val& x)
{
    return xor3(s, ror(s, x, 1), ror(s, x, 8), lsr(s, x, 7));
}

Val sig512s1(SimdLift& s, const Val& x)
{
    return xor3(s, ror(s, x, 19), ror(s, x, 61), lsr(s, x, 6));
}

Val fromDwords(SimdLift& s, const Val& low, const Val& high)
{
    return s.join64(low, high);
}

bool liftSha512(SimdLift& s)
{
    ArmLiftContext& c = s.c();
    if (s.is("sha512h") || s.is("sha512h2"))
    {
        Val d = dwords(s, readOperand(s, 0));
        Val n = dwords(s, readOperand(s, 1));
        Val m = dwords(s, readOperand(s, 2));
        Val d0 = s.lane(d, 0), d1 = s.lane(d, 1);
        Val n0 = s.lane(n, 0), n1 = s.lane(n, 1);
        Val m0 = s.lane(m, 0), m1 = s.lane(m, 1);
        if (s.is("sha512h"))
        {
            Val nd1 = add2(s, add2(s, d1, sig512S1(s, m1)), choose(s, m1, n0, n1));
            Val t = add2(s, nd1, m0);
            Val nd0 = add2(s, add2(s, d0, sig512S1(s, t)), choose(s, t, m1, n0));
            c.write(0, fromDwords(s, nd0, nd1));
        } else
        {
            Val nd1 = add2(s, add2(s, d1, sig512S0(s, m0)), majority(s, n0, m1, m0));
            Val nd0 = add2(s, add2(s, d0, sig512S0(s, nd1)), majority(s, nd1, m0, m1));
            c.write(0, fromDwords(s, nd0, nd1));
        }
        return true;
    }
    if (s.is("sha512su0"))
    {
        Val d = dwords(s, readOperand(s, 0));
        Val n = dwords(s, readOperand(s, 1));
        Val nd0 = add2(s, s.lane(d, 0), sig512s0(s, s.lane(d, 1)));
        Val nd1 = add2(s, s.lane(d, 1), sig512s0(s, s.lane(n, 0)));
        c.write(0, fromDwords(s, nd0, nd1));
        return true;
    }
    if (s.is("sha512su1"))
    {
        Val d = dwords(s, readOperand(s, 0));
        Val n = dwords(s, readOperand(s, 1));
        Val m = dwords(s, readOperand(s, 2));
        Val nd0 = add2(s, add2(s, s.lane(d, 0), sig512s1(s, s.lane(n, 0))), s.lane(m, 0));
        Val nd1 = add2(s, add2(s, s.lane(d, 1), sig512s1(s, s.lane(n, 1))), s.lane(m, 1));
        c.write(0, fromDwords(s, nd0, nd1));
        return true;
    }
    if (s.is("eor3"))
    {
        c.write(0, xor3(s, readOperand(s, 1), readOperand(s, 2), readOperand(s, 3)));
        return true;
    }
    if (s.is("bcax"))
    {
        Val n = readOperand(s, 1);
        Val m = readOperand(s, 2);
        Val a = readOperand(s, 3);
        c.write(0, xor2(s, n, and2(s, m, not1(s, a))));
        return true;
    }
    if (s.is("rax1"))
    {
        Val n = dwords(s, readOperand(s, 1));
        Val m = dwords(s, readOperand(s, 2));
        Val r[2];
        for (uint i = 0; i < 2; i++)
            r[i] = xor2(s, s.lane(n, i), rol(s, s.lane(m, i), 1));
        c.write(0, fromDwords(s, r[0], r[1]));
        return true;
    }
    if (s.is("xar"))
    {
        Val n = dwords(s, readOperand(s, 1));
        Val m = dwords(s, readOperand(s, 2));
        const uint amount = (uint)s.operand(3).m_immediate;
        Val r[2];
        for (uint i = 0; i < 2; i++)
            r[i] = ror(s, xor2(s, s.lane(n, i), s.lane(m, i)), amount);
        c.write(0, fromDwords(s, r[0], r[1]));
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// SM3 and SM4
/////////////////////////////////////////////////////////////////////////////

// P1(x) = x ^ ROL(x, 15) ^ ROL(x, 23)
Val sm3P1(SimdLift& s, const Val& x)
{
    return xor3(s, x, rol(s, x, 15), rol(s, x, 23));
}

bool liftSm3(SimdLift& s)
{
    ArmLiftContext& c = s.c();
    if (s.is("sm3ss1"))
    {
        Val n = words(s, readOperand(s, 1));
        Val m = words(s, readOperand(s, 2));
        Val a = words(s, readOperand(s, 3));
        Val t = add2(s, add2(s, rol(s, s.lane(n, 3), 12), s.lane(m, 3)), s.lane(a, 3));
        Val r[4];
        r[0] = r[1] = r[2] = s.imm(0, I32);
        r[3] = rol(s, t, 7);
        c.write(0, fromWords(s, r));
        return true;
    }
    if (s.is("sm3tt1a") || s.is("sm3tt1b") || s.is("sm3tt2a") || s.is("sm3tt2b"))
    {
        const bool isSecond = s.name()[5] == '2';
        const bool isB = s.name()[6] == 'b';
        Val d = words(s, readOperand(s, 0));
        Val n = words(s, readOperand(s, 1));
        Val m = words(s, readOperand(s, 2));
        Val wj = s.lane(m, (uint)s.operand(2).m_elementIndex);
        Val d0 = s.lane(d, 0), d1 = s.lane(d, 1), d2 = s.lane(d, 2), d3 = s.lane(d, 3);
        Val r[4];
        if (!isSecond)
        {
            Val ss2 = xor2(s, s.lane(n, 3), rol(s, d3, 12));
            Val t = isB ? majority(s, d3, d1, d2) : xor3(s, d1, d2, d3);
            Val tt1 = add2(s, add2(s, add2(s, t, d0), ss2), wj);
            r[0] = d1;
            r[1] = rol(s, d2, 9);
            r[2] = d3;
            r[3] = tt1;
        } else
        {
            Val t = isB ? choose(s, d3, d2, d1) : xor3(s, d1, d2, d3);
            Val tt2 = add2(s, add2(s, add2(s, t, d0), s.lane(n, 3)), wj);
            r[0] = d1;
            r[1] = rol(s, d2, 19);
            r[2] = d3;
            r[3] = xor3(s, tt2, rol(s, tt2, 9), rol(s, tt2, 17));
        }
        c.write(0, fromWords(s, r));
        return true;
    }
    if (s.is("sm3partw1"))
    {
        Val d = words(s, readOperand(s, 0));
        Val n = words(s, readOperand(s, 1));
        Val m = words(s, readOperand(s, 2));
        Val r[4];
        for (uint i = 0; i < 3; i++)
        {
            Val t = xor3(s, s.lane(d, i), s.lane(n, i), rol(s, s.lane(m, i + 1), 15));
            r[i] = sm3P1(s, t);
        }
        Val t = xor3(s, s.lane(d, 3), s.lane(n, 3), rol(s, r[0], 15));
        r[3] = sm3P1(s, t);
        c.write(0, fromWords(s, r));
        return true;
    }
    if (s.is("sm3partw2"))
    {
        Val d = words(s, readOperand(s, 0));
        Val n = words(s, readOperand(s, 1));
        Val m = words(s, readOperand(s, 2));
        Val tmp[4];
        for (uint i = 0; i < 4; i++)
            tmp[i] = xor2(s, s.lane(n, i), rol(s, s.lane(m, i), 7));
        Val r[4];
        for (uint i = 0; i < 3; i++)
            r[i] = xor2(s, s.lane(d, i), tmp[i]);
        Val t2 = rol(s, tmp[0], 15);
        t2 = xor3(s, t2, rol(s, t2, 15), rol(s, t2, 23));
        r[3] = xor3(s, s.lane(d, 3), tmp[3], t2);
        c.write(0, fromWords(s, r));
        return true;
    }
    return false;
}

// L(tau(t)) of SM4: the S-box on the bytes then the linear transform
Val sm4Round(SimdLift& s, const Val& t, bool isKey)
{
    // The S-box of the four bytes at once: a chain of selects by compare
    // (the IR has no table lookup)
    const SemanticType bytes = SimdLift::V(8, 4);
    Val b = s.cast(bytes, t);
    Val r = s.splat(bytes, 0);
    for (uint j = 0; j < 256; j++)
    {
        Val isEqual = s.compare(Op::COND_EQUAL, bytes, b, s.splat(bytes, j));
        r = s.select(isEqual, s.splat(bytes, gSm4Sbox[j]), r);
    }
    Val x = s.cast(I32, r);
    if (isKey)
        return xor3(s, x, rol(s, x, 13), rol(s, x, 23));
    return xor2(s, xor3(s, x, rol(s, x, 2), rol(s, x, 10)),
                xor2(s, rol(s, x, 18), rol(s, x, 24)));
}

bool liftSm4(SimdLift& s)
{
    ArmLiftContext& c = s.c();
    if (s.is("sm4e"))
    {
        Val d = words(s, readOperand(s, 0));
        Val n = words(s, readOperand(s, 1));
        Val x[4];
        for (uint i = 0; i < 4; i++)
            x[i] = s.lane(d, i);
        for (uint i = 0; i < 4; i++)
        {
            Val t = sm4Round(s, xor3(s, xor2(s, x[1], x[2]), x[3], s.lane(n, i)), false);
            Val next = xor2(s, x[0], t);
            x[0] = x[1]; x[1] = x[2]; x[2] = x[3]; x[3] = next;
        }
        c.write(0, fromWords(s, x));
        return true;
    }
    if (s.is("sm4ekey"))
    {
        Val n = words(s, readOperand(s, 1));
        Val m = words(s, readOperand(s, 2));
        Val x[4];
        for (uint i = 0; i < 4; i++)
            x[i] = s.lane(n, i);
        Val r[4];
        for (uint i = 0; i < 4; i++)
        {
            Val t = sm4Round(s, xor3(s, xor2(s, x[1], x[2]), x[3], s.lane(m, i)), true);
            Val next = xor2(s, x[0], t);
            r[i] = next;
            x[0] = x[1]; x[1] = x[2]; x[2] = x[3]; x[3] = next;
        }
        c.write(0, fromWords(s, r));
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// PMULL
/////////////////////////////////////////////////////////////////////////////

bool liftPmull(SimdLift& s)
{
    if (!s.is("pmull") && !s.is("pmull2"))
        return false;
    ArmLiftContext& c = s.c();
    const bool isUpper = s.is("pmull2");
    const uint nn = s.registerNumber(1);
    const uint mm = s.registerNumber(2);
    Val n = s.readV(nn);
    Val m = s.readV(mm);
    if (s.operand(0).m_arrangement == ArmDecodedOperand::ARRANGEMENT_1Q)
    {
        Val a = isUpper ? s.high64(n) : s.low64(n);
        Val b = isUpper ? s.high64(m) : s.low64(m);
        Val low = s.op(Op::OP_CARRYLESS_MULTIPLY, SimdLift::I(64), a, b);
        Val high = s.op(Op::OP_CARRYLESS_MULTIPLY_HIGH, SimdLift::I(64), a, b);
        c.write(0, s.join64(low, high));
        return true;
    }
    // 8 bit polynomials to 16 bit
    Val a = isUpper ? s.high64(n) : s.low64(n);
    Val b = isUpper ? s.high64(m) : s.low64(m);
    Val av = s.op(Op::OP_ZERO_EXTEND, SimdLift::V(16, 8),
                  s.cast(SimdLift::V(8, 8), a));
    Val bv = s.op(Op::OP_ZERO_EXTEND, SimdLift::V(16, 8),
                  s.cast(SimdLift::V(8, 8), b));
    c.write(0, s.op(Op::OP_CARRYLESS_MULTIPLY, SimdLift::V(16, 8), av, bv));
    return true;
}

} // namespace

bool armdis::liftSimdCrypto(SimdLift& s)
{
    return liftAes(s) || liftSha1(s) || liftSha256(s) || liftSha512(s) ||
           liftSm3(s) || liftSm4(s) || liftPmull(s);
}
