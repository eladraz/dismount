/*
 * RiscvSemanticLiftVectorCrypto.cpp
 *
 * Implementation file
 *
 * The vector cryptography: Zvkg (vghsh, vgmul), Zvkned (the AES rounds and
 * key schedules), Zvknha / Zvknhb (SHA-2), Zvksed (SM4) and Zvksh (SM3).
 * The instructions work on element groups: EGS elements of 32 bits (or 64
 * for SHA-512) form a group of EGW = 128 or 256 bits, vl and vstart count
 * elements and must be multiples of EGS (else illegal instruction), and the
 * groups are taken from the register groups in order, the same way for any
 * VLEN (a group of 256 bits spans two registers when VLEN is 128). The SEW
 * of vtype must be 32 (64 for SHA-512), the group must fit the register
 * group (EGW <= LMUL * VLEN), and the register numbers follow the rules of
 * the specification (aligned to LMUL, the vd of the .vs forms and of SHA-2
 * and SM3 not overlapping the sources). There is no mask: vm is 1 in the
 * encodings.
 *
 * The groups of the register groups are unrolled at lift time (their number
 * is a constant of VLEN and LMUL); an element group is active when
 * vstart <= its first element < vl. All the groups are computed before any
 * is written. AES is composed of the IR's AES steps, SHA-2 and SM3 are
 * rotates and shifts of the words, the GHASH multiply is composed of
 * carry-less multiplies, and the SM4 S-box (which the IR has no operation
 * for) is the helper RISCV_HELPER_SM4_SBOX.
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

// The round constants of the AES-128 key schedule (by round - 1)
const uint32 gKeyRoundConstants[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

// The SM4 constant key (CK), section 7.3.2 of the IETF draft
const uint32 gSm4ConstantKey[32] = {
    0x00070E15, 0x1C232A31, 0x383F464D, 0x545B6269,
    0x70777E85, 0x8C939AA1, 0xA8AFB6BD, 0xC4CBD2D9,
    0xE0E7EEF5, 0xFC030A11, 0x181F262D, 0x343B4249,
    0x50575E65, 0x6C737A81, 0x888F969D, 0xA4ABB2B9,
    0xC0C7CED5, 0xDCE3EAF1, 0xF8FF060D, 0x141B2229,
    0x30373E45, 0x4C535A61, 0x686F767D, 0x848B9299,
    0xA0A7AEB5, 0xBCC3CAD1, 0xD8DFE6ED, 0xF4FB0209,
    0x10171E25, 0x2C333A41, 0x484F565D, 0x646B7279
};

/*
 * The description of an instruction: the element width, the group size in
 * elements, the operands
 */
struct CryptoSpec {
    // SEW 32 (and 64 for SHA-512)
    bool m_isSha2;
    // EGS: the elements of a group
    uint m_egs;
    // EGW for SEW = 32
    uint m_egw;
    // vs1 is a source
    bool m_hasVs1;
    // vs2 is a source
    bool m_hasVs2;
    // The .vs forms: vs2 is the first group only
    bool m_isScalar;
};

const CryptoSpec& specOf(uint operation)
{
    static const CryptoSpec aesVv = { false, 4, 128, false, true, false };
    static const CryptoSpec aesVs = { false, 4, 128, false, true, true };
    static const CryptoSpec ghash = { false, 4, 128, true, true, false };
    static const CryptoSpec gmul = { false, 4, 128, false, true, false };
    static const CryptoSpec sha2 = { true, 4, 128, true, true, false };
    static const CryptoSpec sm3me = { false, 8, 256, true, true, false };
    static const CryptoSpec sm3c = { false, 8, 256, false, true, false };
    switch (operation)
    {
    case RVK_AESEF_VV: case RVK_AESEM_VV: case RVK_AESDF_VV: case RVK_AESDM_VV:
    case RVK_AESKF1: case RVK_AESKF2: case RVK_SM4K: case RVK_SM4R_VV:
        return aesVv;
    case RVK_AESEF_VS: case RVK_AESEM_VS: case RVK_AESDF_VS: case RVK_AESDM_VS:
    case RVK_AESZ_VS: case RVK_SM4R_VS:
        return aesVs;
    case RVK_GHSH:
        return ghash;
    case RVK_GMUL:
        return gmul;
    case RVK_SHA2MS: case RVK_SHA2CH: case RVK_SHA2CL:
        return sha2;
    case RVK_SM3ME:
        return sm3me;
    default:
        return sm3c;
    }
}

/*
 * Scalar helpers on the words of the groups
 */
class Words {
public:
    explicit Words(RvvLift& lift) : m_l(lift) {}

    SemanticType type(uint bits) { return m_l.integerType(bits); }
    SemanticOperand constant(int64 value, uint bits)
    {
        return m_l.imm(value, type(bits));
    }
    SemanticOperand xorOf(const SemanticOperand& a, const SemanticOperand& b)
    {
        return m_l.emit(Op::OP_XOR, a.getType(), a, b);
    }
    SemanticOperand add(const SemanticOperand& a, const SemanticOperand& b)
    {
        return m_l.emit(Op::OP_ADD, a.getType(), a, b);
    }
    SemanticOperand andOf(const SemanticOperand& a, const SemanticOperand& b)
    {
        return m_l.emit(Op::OP_AND, a.getType(), a, b);
    }
    // ~a & b
    SemanticOperand andNot(const SemanticOperand& a, const SemanticOperand& b)
    {
        return m_l.emit(Op::OP_AND_NOT, a.getType(), a, b);
    }
    SemanticOperand orOf(const SemanticOperand& a, const SemanticOperand& b)
    {
        return m_l.emit(Op::OP_OR, a.getType(), a, b);
    }
    SemanticOperand rol(const SemanticOperand& a, uint amount)
    {
        return m_l.emit(Op::OP_ROTATE_LEFT, a.getType(), a,
                        m_l.imm(amount, a.getType()));
    }
    SemanticOperand ror(const SemanticOperand& a, uint amount)
    {
        return m_l.emit(Op::OP_ROTATE_RIGHT, a.getType(), a,
                        m_l.imm(amount, a.getType()));
    }
    SemanticOperand shiftRight(const SemanticOperand& a, uint amount)
    {
        return m_l.emit(Op::OP_SHIFT_RIGHT_LOGICAL, a.getType(), a,
                        m_l.imm(amount, a.getType()));
    }
    SemanticOperand shiftLeft(const SemanticOperand& a, uint amount)
    {
        return m_l.emit(Op::OP_SHIFT_LEFT, a.getType(), a,
                        m_l.imm(amount, a.getType()));
    }
    SemanticOperand xor3(const SemanticOperand& a, const SemanticOperand& b,
                         const SemanticOperand& c)
    {
        return xorOf(xorOf(a, b), c);
    }
    SemanticOperand byteSwap(const SemanticOperand& a)
    {
        return m_l.emit(Op::OP_BYTE_SWAP, a.getType(), a);
    }
    // The word 'index' of 'bits' bits of a group
    SemanticOperand word(const SemanticOperand& group, uint index, uint bits)
    {
        return m_l.extractBits(group, m_l.immX(index * bits), type(bits));
    }
    // The group of the words (the first is the lowest)
    SemanticOperand join(const SemanticOperand* words, uint count, uint bits)
    {
        SemanticOperand result = words[0];
        for (uint i = 1; i < count; i++)
            result = m_l.emit(Op::OP_CONCAT, type((i + 1) * bits), result,
                              words[i]);
        return result;
    }
    SemanticOperand zeroExtend(const SemanticOperand& a, uint bits)
    {
        return m_l.emit(Op::OP_ZERO_EXTEND, type(bits), a);
    }
    SemanticOperand truncate(const SemanticOperand& a, uint bits)
    {
        return m_l.emit(Op::OP_TRUNCATE, type(bits), a);
    }

    // The bits of each byte of the 128 bit value reversed (brev8)
    SemanticOperand reverseBitsInBytes(const SemanticOperand& a)
    {
        const SemanticType bytes = m_l.vectorType(8, 16);
        SemanticOperand lanes = m_l.bitcast(a, bytes);
        return m_l.bitcast(m_l.emit(Op::OP_BIT_REVERSE, bytes, lanes), type(128));
    }

    // SubWord of AES: the S-box of the 4 bytes of a word
    SemanticOperand subWord(const SemanticOperand& w)
    {
        // (a state of four equal columns: ShiftRows doesn't change it)
        SemanticOperand words[4] = { w, w, w, w };
        SemanticOperand state = join(words, 4, 32);
        SemanticOperand substituted = m_l.emit(Op::OP_AES_SUB_SHIFT, type(128),
                                               state);
        return word(substituted, 0, 32);
    }

    // The SM4 S-box of the 4 bytes of a word (the helper)
    SemanticOperand sm4SubBytes(const SemanticOperand& w)
    {
        const SemanticType native = m_l.xlenType();
        SemanticOperand argument = zeroExtend(w, m_l.xlen());
        SemanticOperand result = m_l.context().newTemporary(native);
        m_l.context().emit(SemanticInstruction(Op::OP_HELPER, native, result,
                m_l.imm(RISCV_HELPER_SM4_SBOX, type(32)), argument));
        return truncateTo32(result);
    }
    SemanticOperand truncateTo32(const SemanticOperand& a)
    {
        return m_l.truncate(a, 32);
    }

    // The product of two polynomials of GF(2)[x] modulo x^128 + x^7 + x^2 +
    // x + 1 (the multiplication of GHASH), the 128 bit values being the
    // coefficients from the bit 0 up. Computed on 64 bit words
    SemanticOperand multiplyGalois(const SemanticOperand& a,
                                   const SemanticOperand& b)
    {
        SemanticOperand al = word(a, 0, 64);
        SemanticOperand ah = word(a, 1, 64);
        SemanticOperand bl = word(b, 0, 64);
        SemanticOperand bh = word(b, 1, 64);
        SemanticOperand ll[2], lh[2], hl[2], hh[2];
        product(al, bl, ll);
        product(al, bh, lh);
        product(ah, bl, hl);
        product(ah, bh, hh);
        // The 256 bit product p0..p3
        SemanticOperand p0 = ll[0];
        SemanticOperand p1 = xor3(ll[1], lh[0], hl[0]);
        SemanticOperand p2 = xor3(hh[0], lh[1], hl[1]);
        SemanticOperand p3 = hh[1];
        // x^128 = x^7 + x^2 + x + 1: the high half times it is added to the
        // low half (a 135 bit value, t0..t2), and the top 7 bits again
        static const uint shifts[4] = { 0, 1, 2, 7 };
        SemanticOperand t0, t1, t2;
        for (uint i = 0; i < 4; i++)
        {
            const uint s = shifts[i];
            SemanticOperand w0 = p2;
            SemanticOperand w1 = p3;
            SemanticOperand w2 = constant(0, 64);
            if (s != 0)
            {
                w0 = shiftLeft(p2, s);
                w1 = orOf(shiftLeft(p3, s), shiftRight(p2, 64 - s));
                w2 = shiftRight(p3, 64 - s);
            }
            if (i == 0)
            {
                t0 = w0;
                t1 = w1;
                t2 = w2;
            } else
            {
                t0 = xorOf(t0, w0);
                t1 = xorOf(t1, w1);
                t2 = xorOf(t2, w2);
            }
        }
        SemanticOperand again = xor3(xorOf(t2, shiftLeft(t2, 1)),
                                     shiftLeft(t2, 2), shiftLeft(t2, 7));
        SemanticOperand result[2] = { xor3(p0, t0, again), xorOf(p1, t1) };
        return join(result, 2, 64);
    }

private:
    // The 128 bit carry-less product of two 64 bit values (low, high)
    void product(const SemanticOperand& a, const SemanticOperand& b,
                 SemanticOperand* result)
    {
        result[0] = m_l.emit(Op::OP_CARRYLESS_MULTIPLY, type(64), a, b);
        result[1] = m_l.emit(Op::OP_CARRYLESS_MULTIPLY_HIGH, type(64), a, b);
    }

    RvvLift& m_l;
};

/*
 * The registers of the groups
 */

// The element group 'index' (of 'egw' bits) of the register group that starts
// at 'first'
SemanticOperand readElementGroup(RvvLift& l, uint first, uint index, uint egw)
{
    const uint vlen = l.vlen();
    const uint offset = index * egw;
    const uint number = first + offset / vlen;
    if (egw == vlen)
        return l.readRegister(number);
    if (egw < vlen)
        return l.extractBits(l.readRegister(number), l.immX(offset % vlen),
                             l.integerType(egw));
    SemanticOperand result = l.readRegister(number);
    for (uint i = 1; i < egw / vlen; i++)
        result = l.emit(SemanticOperation::OP_CONCAT,
                        l.integerType((i + 1) * vlen), result,
                        l.readRegister(number + i));
    return result;
}

void writeElementGroup(RvvLift& l, uint first, uint index, uint egw,
                       const SemanticOperand& value)
{
    const uint vlen = l.vlen();
    const uint offset = index * egw;
    const uint number = first + offset / vlen;
    if (egw == vlen)
    {
        l.writeRegister(number, value);
    } else if (egw < vlen)
    {
        l.writeRegister(number, l.insertBits(l.readRegister(number), value,
                                             l.immX(offset % vlen)));
    } else
    {
        for (uint i = 0; i < egw / vlen; i++)
            l.writeRegister(number + i, l.extractBits(value, l.immX(i * vlen),
                                                      l.integerType(vlen)));
    }
}

// log2 of a power of two, and VLEN * LMUL
uint registerGroupBits(const RvvLift& l, const RvvType& type)
{
    return (type.m_lmulLog2 >= 0) ? (l.vlen() << type.m_lmulLog2)
                                  : (l.vlen() >> (-type.m_lmulLog2));
}

// true if 'number' is aligned to the register group of LMUL (no alignment for
// a fractional LMUL)
bool isAligned(uint number, const RvvType& type)
{
    return (number % type.registers()) == 0;
}

/*
 * The new value of an element group
 */
struct Operands {
    // The old vd, vs2 and vs1 groups (vs2 is the first group for the .vs forms)
    SemanticOperand m_d;
    SemanticOperand m_s2;
    SemanticOperand m_s1;
};

SemanticOperand aesRound(RvvLift& l, Words& w, uint operation,
                         const Operands& o)
{
    typedef SemanticOperation Op;
    const SemanticType t = l.integerType(128);
    SemanticOperand state = o.m_d;
    switch (operation)
    {
    case RVK_AESEF_VV: case RVK_AESEF_VS:
        return w.xorOf(l.emit(Op::OP_AES_SUB_SHIFT, t, state), o.m_s2);
    case RVK_AESEM_VV: case RVK_AESEM_VS:
        return w.xorOf(l.emit(Op::OP_AES_MIX_COLUMNS, t,
                              l.emit(Op::OP_AES_SUB_SHIFT, t, state)), o.m_s2);
    case RVK_AESDF_VV: case RVK_AESDF_VS:
        return w.xorOf(l.emit(Op::OP_AES_INVERSE_SUB_SHIFT, t, state), o.m_s2);
    case RVK_AESDM_VV: case RVK_AESDM_VS:
        return l.emit(Op::OP_AES_INVERSE_MIX_COLUMNS, t,
                      w.xorOf(l.emit(Op::OP_AES_INVERSE_SUB_SHIFT, t, state),
                              o.m_s2));
    default:
        // vaesz.vs
        return w.xorOf(state, o.m_s2);
    }
}

// vaeskf1.vi / vaeskf2.vi: the next round key of the key in vs2 (and, for
// AES-256, the previous one in vd)
SemanticOperand aesKeySchedule(RvvLift& l, Words& w, uint operation,
                               const Operands& o)
{
    const uint immediate = l.uimm5() & 0xF;
    uint round;
    SemanticOperand temp = w.word(o.m_s2, 3, 32);
    SemanticOperand previous;
    if (operation == RVK_AESKF1)
    {
        round = ((immediate >= 1) && (immediate <= 10)) ? immediate
                                                         : (immediate ^ 8);
        temp = w.ror(temp, 8);
        temp = w.subWord(temp);
        temp = w.xorOf(temp, w.constant(gKeyRoundConstants[round - 1], 32));
        previous = o.m_s2;
    } else
    {
        round = ((immediate >= 2) && (immediate <= 14)) ? immediate
                                                         : (immediate ^ 8);
        const bool isEven = (round & 1) == 0;
        if (isEven)
            temp = w.ror(temp, 8);
        temp = w.subWord(temp);
        if (isEven)
            temp = w.xorOf(temp, w.constant(gKeyRoundConstants[(round >> 1) - 1], 32));
        previous = o.m_d;
    }
    SemanticOperand words[4];
    words[0] = w.xorOf(w.word(previous, 0, 32), temp);
    words[1] = w.xorOf(w.word(previous, 1, 32), words[0]);
    words[2] = w.xorOf(w.word(previous, 2, 32), words[1]);
    words[3] = w.xorOf(w.word(previous, 3, 32), words[2]);
    return w.join(words, 4, 32);
}

SemanticOperand ghash(RvvLift& l, Words& w, uint operation, const Operands& o)
{
    (void)l;
    if (operation == RVK_GHSH)
    {
        // Z = brev8(brev8(Y ^ X) * brev8(H))
        SemanticOperand s = w.reverseBitsInBytes(w.xorOf(o.m_d, o.m_s1));
        SemanticOperand h = w.reverseBitsInBytes(o.m_s2);
        return w.reverseBitsInBytes(w.multiplyGalois(s, h));
    }
    SemanticOperand y = w.reverseBitsInBytes(o.m_d);
    SemanticOperand h = w.reverseBitsInBytes(o.m_s2);
    return w.reverseBitsInBytes(w.multiplyGalois(y, h));
}

// SHA-2 (32 or 64 bit words)
struct Sha2Functions {
    uint m_bits;
    uint m_sum0[3];
    uint m_sum1[3];
    uint m_sig0[2];
    uint m_shift0;
    uint m_sig1[2];
    uint m_shift1;
};

SemanticOperand sha2Sum(Words& w, const SemanticOperand& x, const uint* rotates)
{
    return w.xor3(w.ror(x, rotates[0]), w.ror(x, rotates[1]),
                  w.ror(x, rotates[2]));
}

SemanticOperand sha2Sigma(Words& w, const SemanticOperand& x,
                          const uint* rotates, uint shift)
{
    return w.xor3(w.ror(x, rotates[0]), w.ror(x, rotates[1]),
                  w.shiftRight(x, shift));
}

SemanticOperand sha2(RvvLift& l, Words& w, uint operation, const Operands& o,
                     uint bits)
{
    (void)l;
    static const Sha2Functions f256 = { 32, {2, 13, 22}, {6, 11, 25}, {7, 18},
                                        3, {17, 19}, 10 };
    static const Sha2Functions f512 = { 64, {28, 34, 39}, {14, 18, 41}, {1, 8},
                                        7, {19, 61}, 6 };
    const Sha2Functions& f = (bits == 32) ? f256 : f512;
    SemanticOperand d[4];
    SemanticOperand s2[4];
    SemanticOperand s1[4];
    for (uint i = 0; i < 4; i++)
    {
        d[i] = w.word(o.m_d, i, bits);
        s2[i] = w.word(o.m_s2, i, bits);
        s1[i] = w.word(o.m_s1, i, bits);
    }
    if (operation == RVK_SHA2MS)
    {
        // {w3..w0} = vd, {w11, w10, w9, w4} = vs2, {w15, w14, _, w12} = vs1
        SemanticOperand wd[20];
        for (uint i = 0; i < 4; i++)
            wd[i] = d[i];
        wd[4] = s2[0];
        wd[9] = s2[1];
        wd[10] = s2[2];
        wd[11] = s2[3];
        wd[12] = s1[0];
        wd[14] = s1[2];
        wd[15] = s1[3];
        for (uint t = 16; t < 20; t++)
        {
            // W[t] = sig1(W[t - 2]) + W[t - 7] + sig0(W[t - 15]) + W[t - 16]
            wd[t] = w.add(w.add(sha2Sigma(w, wd[t - 2], f.m_sig1, f.m_shift1),
                                wd[t - 7]),
                          w.add(sha2Sigma(w, wd[t - 15], f.m_sig0, f.m_shift0),
                                wd[t - 16]));
        }
        SemanticOperand result[4] = { wd[16], wd[17], wd[18], wd[19] };
        return w.join(result, 4, bits);
    }
    // {c, d, g, h} = vd (c is the element 3), {a, b, e, f} = vs2
    SemanticOperand h = d[0];
    SemanticOperand g = d[1];
    SemanticOperand dd = d[2];
    SemanticOperand c = d[3];
    SemanticOperand ff = s2[0];
    SemanticOperand e = s2[1];
    SemanticOperand b = s2[2];
    SemanticOperand a = s2[3];
    // vsha2ch adds the elements 2 and 3 of vs1, vsha2cl the elements 0 and 1
    const uint first = (operation == RVK_SHA2CH) ? 2 : 0;
    for (uint round = 0; round < 2; round++)
    {
        const SemanticOperand& kw = s1[first + round];
        SemanticOperand ch = w.xorOf(w.andOf(e, ff), w.andNot(e, g));
        SemanticOperand maj = w.xor3(w.andOf(a, b), w.andOf(a, c), w.andOf(b, c));
        SemanticOperand t1 = w.add(w.add(h, sha2Sum(w, e, f.m_sum1)),
                                   w.add(ch, kw));
        SemanticOperand t2 = w.add(sha2Sum(w, a, f.m_sum0), maj);
        h = g;
        g = ff;
        ff = e;
        e = w.add(dd, t1);
        dd = c;
        c = b;
        b = a;
        a = w.add(t1, t2);
    }
    // vd = {a, b, e, f}
    SemanticOperand result[4] = { ff, e, b, a };
    return w.join(result, 4, bits);
}

// SM3 (the words of the groups are byte swapped)
SemanticOperand sm3Permutation1(Words& w, const SemanticOperand& x)
{
    return w.xor3(x, w.rol(x, 15), w.rol(x, 23));
}

SemanticOperand sm3Permutation0(Words& w, const SemanticOperand& x)
{
    return w.xor3(x, w.rol(x, 9), w.rol(x, 17));
}

SemanticOperand sm3Expand(Words& w, const SemanticOperand& m16,
                          const SemanticOperand& m9, const SemanticOperand& m3,
                          const SemanticOperand& m13, const SemanticOperand& m6)
{
    return w.xor3(sm3Permutation1(w, w.xor3(m16, m9, w.rol(m3, 15))),
                  w.rol(m13, 7), m6);
}

SemanticOperand sm3MessageExpansion(RvvLift& l, Words& w, const Operands& o)
{
    (void)l;
    SemanticOperand m[24];
    for (uint i = 0; i < 8; i++)
    {
        m[i] = w.byteSwap(w.word(o.m_s1, i, 32));
        m[8 + i] = w.byteSwap(w.word(o.m_s2, i, 32));
    }
    m[16] = sm3Expand(w, m[0], m[7], m[13], m[3], m[10]);
    m[17] = sm3Expand(w, m[1], m[8], m[14], m[4], m[11]);
    m[18] = sm3Expand(w, m[2], m[9], m[15], m[5], m[12]);
    m[19] = sm3Expand(w, m[3], m[10], m[16], m[6], m[13]);
    m[20] = sm3Expand(w, m[4], m[11], m[17], m[7], m[14]);
    m[21] = sm3Expand(w, m[5], m[12], m[18], m[8], m[15]);
    m[22] = sm3Expand(w, m[6], m[13], m[19], m[9], m[16]);
    m[23] = sm3Expand(w, m[7], m[14], m[20], m[10], m[17]);
    SemanticOperand result[8];
    for (uint i = 0; i < 8; i++)
        result[i] = w.byteSwap(m[16 + i]);
    return w.join(result, 8, 32);
}

// One round of the compression of SM3: the state is updated in place, 'j' is
// the round number (a constant), 'x' and 'wj' the message words
void sm3Round(Words& w, uint j, SemanticOperand* state,
              const SemanticOperand& x, const SemanticOperand& wj)
{
    SemanticOperand& a = state[0];
    SemanticOperand& b = state[1];
    SemanticOperand& c = state[2];
    SemanticOperand& d = state[3];
    SemanticOperand& e = state[4];
    SemanticOperand& f = state[5];
    SemanticOperand& g = state[6];
    SemanticOperand& h = state[7];
    const uint32 t = (j <= 15) ? 0x79CC4519u : 0x7A879D8Au;
    const uint amount = j % 32;
    const uint32 rotated = (amount == 0) ? t : ((t << amount) | (t >> (32 - amount)));
    SemanticOperand a12 = w.rol(a, 12);
    SemanticOperand ss1 = w.rol(w.add(w.add(a12, e), w.constant(rotated, 32)), 7);
    SemanticOperand ss2 = w.xorOf(ss1, a12);
    SemanticOperand ff;
    SemanticOperand gg;
    if (j <= 15)
    {
        ff = w.xor3(a, b, c);
        gg = w.xor3(e, f, g);
    } else
    {
        ff = w.orOf(w.orOf(w.andOf(a, b), w.andOf(a, c)), w.andOf(b, c));
        gg = w.orOf(w.andOf(e, f), w.andNot(e, g));
    }
    SemanticOperand tt1 = w.add(w.add(ff, d), w.add(ss2, x));
    SemanticOperand tt2 = w.add(w.add(gg, h), w.add(ss1, wj));
    d = c;
    c = w.rol(b, 9);
    b = a;
    a = tt1;
    h = g;
    g = w.rol(f, 19);
    f = e;
    e = sm3Permutation0(w, tt2);
}

SemanticOperand sm3Compress(RvvLift& l, Words& w, const Operands& o)
{
    const uint round = l.uimm5();
    // {H..A} = vd (A is the element 0), the words byte swapped
    SemanticOperand state[8];
    for (uint i = 0; i < 8; i++)
        state[i] = w.byteSwap(w.word(o.m_d, i, 32));
    SemanticOperand w0 = w.byteSwap(w.word(o.m_s2, 0, 32));
    SemanticOperand w1 = w.byteSwap(w.word(o.m_s2, 1, 32));
    SemanticOperand w4 = w.byteSwap(w.word(o.m_s2, 4, 32));
    SemanticOperand w5 = w.byteSwap(w.word(o.m_s2, 5, 32));
    SemanticOperand x0 = w.xorOf(w0, w4);
    SemanticOperand x1 = w.xorOf(w1, w5);
    sm3Round(w, 2 * round, state, x0, w0);
    // (the state after the first round: A1 = a, C1 = c, E1 = e, G1 = g)
    SemanticOperand a1 = state[0];
    SemanticOperand c1 = state[2];
    SemanticOperand e1 = state[4];
    SemanticOperand g1 = state[6];
    sm3Round(w, 2 * round + 1, state, x1, w1);
    // vd = {G1, G2, E1, E2, C1, C2, A1, A2} from the element 7 down
    SemanticOperand result[8] = {
        w.byteSwap(state[0]), w.byteSwap(a1), w.byteSwap(state[2]), w.byteSwap(c1),
        w.byteSwap(state[4]), w.byteSwap(e1), w.byteSwap(state[6]), w.byteSwap(g1)
    };
    return w.join(result, 8, 32);
}

// SM4
SemanticOperand sm4Transform(Words& w, const SemanticOperand& s, bool isKey)
{
    if (isKey)
        return w.xor3(s, w.rol(s, 13), w.rol(s, 23));
    return w.xorOf(w.xor3(s, w.rol(s, 2), w.rol(s, 10)),
                   w.xorOf(w.rol(s, 18), w.rol(s, 24)));
}

SemanticOperand sm4KeySchedule(RvvLift& l, Words& w, const Operands& o)
{
    const uint round = l.uimm5() & 7;
    SemanticOperand rk[8];
    for (uint i = 0; i < 4; i++)
        rk[i] = w.word(o.m_s2, i, 32);
    for (uint i = 0; i < 4; i++)
    {
        SemanticOperand b = w.xorOf(w.xor3(rk[i + 1], rk[i + 2], rk[i + 3]),
                                    w.constant(gSm4ConstantKey[4 * round + i], 32));
        rk[i + 4] = w.xorOf(rk[i], sm4Transform(w, w.sm4SubBytes(b), true));
    }
    return w.join(rk + 4, 4, 32);
}

SemanticOperand sm4Rounds(RvvLift& l, Words& w, const Operands& o)
{
    (void)l;
    SemanticOperand x[8];
    SemanticOperand rk[4];
    for (uint i = 0; i < 4; i++)
    {
        x[i] = w.word(o.m_d, i, 32);
        rk[i] = w.word(o.m_s2, i, 32);
    }
    for (uint i = 0; i < 4; i++)
    {
        SemanticOperand b = w.xorOf(w.xor3(x[i + 1], x[i + 2], x[i + 3]), rk[i]);
        x[i + 4] = w.xorOf(x[i], sm4Transform(w, w.sm4SubBytes(b), false));
    }
    return w.join(x + 4, 4, 32);
}

SemanticOperand newGroup(RvvLift& l, Words& w, uint operation,
                         const Operands& o, uint elementBits)
{
    switch (operation)
    {
    case RVK_AESKF1:
    case RVK_AESKF2:
        return aesKeySchedule(l, w, operation, o);
    case RVK_GHSH:
    case RVK_GMUL:
        return ghash(l, w, operation, o);
    case RVK_SHA2MS:
    case RVK_SHA2CH:
    case RVK_SHA2CL:
        return sha2(l, w, operation, o, elementBits);
    case RVK_SM3ME:
        return sm3MessageExpansion(l, w, o);
    case RVK_SM3C:
        return sm3Compress(l, w, o);
    case RVK_SM4K:
        return sm4KeySchedule(l, w, o);
    case RVK_SM4R_VV:
    case RVK_SM4R_VS:
        return sm4Rounds(l, w, o);
    default:
        return aesRound(l, w, operation, o);
    }
}

void cryptoBody(RvvLift& lift, const RvvType& type)
{
    const uint operation = (uint)(lift.context().getParameter() & 0xFF);
    const CryptoSpec& spec = specOf(operation);
    const uint sew = type.m_sew;
    // The SEW: 32, and 64 for SHA-512
    if (!((sew == 32) || (spec.m_isSha2 && (sew == 64))))
    {
        lift.trapIllegal();
        return;
    }
    const uint egw = (spec.m_isSha2 && (sew == 64)) ? 256 : spec.m_egw;
    const uint groupBits = registerGroupBits(lift, type);
    bool isLegal = (egw <= groupBits);
    const uint vd = lift.vd();
    const uint vs2 = lift.vs2();
    const uint vs1 = lift.vs1();
    // The alignment of the groups to LMUL
    isLegal = isLegal && isAligned(vd, type);
    if (!spec.m_isScalar)
        isLegal = isLegal && isAligned(vs2, type);
    if (spec.m_hasVs1)
        isLegal = isLegal && isAligned(vs1, type);
    if (spec.m_isScalar)
    {
        // vs2 is a single element group (aligned to its registers) which the
        // destination group doesn't contain
        const uint scalarRegisters = (egw > lift.vlen()) ? (egw / lift.vlen()) : 1;
        isLegal = isLegal && ((vs2 % scalarRegisters) == 0);
        const uint emul = type.registers();
        isLegal = isLegal && ((vd / emul) != (vs2 / emul));
    }
    if (spec.m_isSha2)
        isLegal = isLegal && (vd != vs1) && (vd != vs2);
    if ((operation == RVK_SM3ME) || (operation == RVK_SM3C))
        isLegal = isLegal && (vd != vs2);
    // The register groups fit the file
    const uint registers = type.registers();
    isLegal = isLegal && (vd + registers <= 32) && (vs2 + registers <= 32) &&
              (!spec.m_hasVs1 || (vs1 + registers <= 32));
    if (!isLegal)
    {
        lift.trapIllegal();
        return;
    }

    // vstart and vl are multiples of EGS
    const SemanticType x = lift.xlenType();
    SemanticOperand bitsOfBoth = lift.emit(Op::OP_OR, x, lift.vstart(), lift.vl());
    SemanticOperand remainder = lift.emit(Op::OP_AND, x, bitsOfBoth,
                                          lift.immX(spec.m_egs - 1));
    lift.trapIllegalIf(lift.compare(Op::COND_NOT_EQUAL, x, remainder, lift.immX(0)));

    Words words(lift);
    const uint groups = groupBits / egw;
    cArray<SemanticOperand> results(groups);
    cArray<SemanticOperand> olds(groups);
    SemanticOperand scalarKey;
    if (spec.m_isScalar)
        scalarKey = readElementGroup(lift, vs2, 0, egw);
    for (uint g = 0; g < groups; g++)
    {
        Operands operands;
        operands.m_d = readElementGroup(lift, vd, g, egw);
        olds[g] = operands.m_d;
        if (spec.m_isScalar)
            operands.m_s2 = scalarKey;
        else
            operands.m_s2 = readElementGroup(lift, vs2, g, egw);
        if (spec.m_hasVs1)
            operands.m_s1 = readElementGroup(lift, vs1, g, egw);
        SemanticOperand value = newGroup(lift, words, operation, operands,
                                         (egw == 256 && spec.m_isSha2) ? 64 : 32);
        // Active: vstart <= g * EGS < vl
        const uint first = g * spec.m_egs;
        SemanticOperand isAfterStart = lift.compare(Op::COND_UNSIGNED_LESS_EQUAL,
                x, lift.vstart(), lift.immX(first));
        SemanticOperand isBeforeEnd = lift.compare(Op::COND_UNSIGNED_GREATER, x,
                lift.vl(), lift.immX(first));
        SemanticOperand isActive = lift.emit(Op::OP_AND, lift.integerType(1),
                                             isAfterStart, isBeforeEnd);
        results[g] = lift.select(isActive, value, operands.m_d);
    }
    for (uint g = 0; g < groups; g++)
        writeElementGroup(lift, vd, g, egw, results[g]);
}

} // namespace

void liftVectorCrypto(RiscvLiftContext& context)
{
    RvvLift lift(context);
    lift.dispatch(cryptoBody);
    lift.finish();
}

} // namespace riscvdis
