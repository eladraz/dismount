/*
 * RiscvSemanticLiftCrypto.cpp
 *
 * Implementation file
 *
 * The lifters of the scalar cryptography: Zknd, Zkne (AES), Zknh (SHA-2),
 * Zksh (SM3) and Zksed (SM4), see RiscvSemanticInternal.h. AES is composed
 * of the IR's AES steps (a single S-box lookup is a state of 16 bytes with
 * only its first byte used: ShiftRows doesn't move the first row), SHA-2 and
 * SM3 are rotates and shifts. The SM4 S-box has no IR operation: it is the
 * helper RISCV_HELPER_INSTRUCTION, which RiscvHelpers executes.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"

namespace riscvdis {

namespace {

// The round constants of the AES key schedule, by the round number
const uint8 gRoundConstants[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

// Return the S-box (or the inverse one) of a byte
SemanticOperand lookupSbox(RiscvLiftContext& context,
                           const SemanticOperand& byte, bool isInverse)
{
    SemanticOperand state = context.emit(Op::OP_ZERO_EXTEND, integerType(128),
                                         byte);
    SemanticOperand substituted = context.emit(
            isInverse ? Op::OP_AES_INVERSE_SUB_SHIFT : Op::OP_AES_SUB_SHIFT,
            integerType(128), state);
    return makeTruncate(context, substituted, 8);
}

// Return the bits [shift, shift + 8) of the 32 bit value
SemanticOperand getByte(RiscvLiftContext& context, const SemanticOperand& word,
                        uint shift)
{
    SemanticOperand moved = word;
    if (shift != 0)
        moved = context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, word.getType(), word,
                    makeConstant(context, shift, word.getType()));
    return makeTruncate(context, moved, 8);
}

// Return rotl(value, amount) / the right rotate of a value
SemanticOperand rotateLeft(RiscvLiftContext& context, const SemanticOperand& value,
                           uint amount)
{
    return context.emit(Op::OP_ROTATE_LEFT, value.getType(), value,
                        makeConstant(context, amount, value.getType()));
}
SemanticOperand rotateRight(RiscvLiftContext& context, const SemanticOperand& value,
                            uint amount)
{
    return context.emit(Op::OP_ROTATE_RIGHT, value.getType(), value,
                        makeConstant(context, amount, value.getType()));
}
SemanticOperand shiftRight(RiscvLiftContext& context, const SemanticOperand& value,
                           uint amount)
{
    return context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, value.getType(), value,
                        makeConstant(context, amount, value.getType()));
}
SemanticOperand shiftLeft(RiscvLiftContext& context, const SemanticOperand& value,
                          uint amount)
{
    return context.emit(Op::OP_SHIFT_LEFT, value.getType(), value,
                        makeConstant(context, amount, value.getType()));
}
SemanticOperand exclusiveOr(RiscvLiftContext& context, const SemanticOperand& a,
                            const SemanticOperand& b)
{
    return context.emit(Op::OP_XOR, a.getType(), a, b);
}

} // namespace

void liftAes32(RiscvLiftContext& context)
{
    // aes32esi / aes32esmi / aes32dsi / aes32dsmi: bit 0 of the parameter is
    // the decryption and bit 1 the MixColumns
    const uint64 parameter = context.getParameter();
    const bool isInverse = (parameter & 1) != 0;
    const bool isMix = (parameter & 2) != 0;
    const SemanticType word = integerType(32);
    const uint shift = 8 * (uint)(context.getOperand(3).m_immediate & 3);
    SemanticOperand a = context.read(1, word);
    SemanticOperand b = context.read(2, word);
    SemanticOperand substituted = lookupSbox(context, getByte(context, b, shift),
                                             isInverse);
    SemanticOperand value;
    if (isMix)
    {
        // The column [u, 0, 0, 0] mixed: 2u, u, u, 3u (encryption)
        SemanticOperand column = context.emit(Op::OP_ZERO_EXTEND, integerType(128),
                                              substituted);
        SemanticOperand mixed = context.emit(
                isInverse ? Op::OP_AES_INVERSE_MIX_COLUMNS : Op::OP_AES_MIX_COLUMNS,
                integerType(128), column);
        value = makeTruncate(context, mixed, 32);
    } else
    {
        value = context.emit(Op::OP_ZERO_EXTEND, word, substituted);
    }
    context.writeSignExtended(0, exclusiveOr(context, a,
                                             rotateLeft(context, value, shift)));
}

void liftAes64(RiscvLiftContext& context)
{
    // The parameter: 0 aes64es, 1 aes64esm, 2 aes64ds, 3 aes64dsm, 4 aes64im
    const uint64 parameter = context.getParameter();
    const SemanticType wide = integerType(128);
    SemanticOperand result;
    if (parameter == 4)
    {
        SemanticOperand state = context.emit(Op::OP_ZERO_EXTEND, wide,
                                             context.read(1, integerType(64)));
        result = context.emit(Op::OP_AES_INVERSE_MIX_COLUMNS, wide, state);
    } else
    {
        SemanticOperand state = context.emit(Op::OP_CONCAT, wide,
                context.read(1, integerType(64)), context.read(2, integerType(64)));
        const bool isInverse = (parameter >= 2);
        result = context.emit(isInverse ? Op::OP_AES_INVERSE_SUB_SHIFT :
                                          Op::OP_AES_SUB_SHIFT, wide, state);
        if ((parameter & 1) != 0)
            result = context.emit(isInverse ? Op::OP_AES_INVERSE_MIX_COLUMNS :
                                              Op::OP_AES_MIX_COLUMNS, wide, result);
    }
    context.write(0, makeTruncate(context, result, 64));
}

void liftAes64KeySchedule(RiscvLiftContext& context)
{
    const SemanticType word = integerType(32);
    if (context.getParameter() == 1)
    {
        // aes64ks2: {rs1[63:32] ^ rs2[31:0] ^ rs2[63:32], rs1[63:32] ^ rs2[31:0]}
        SemanticOperand a = context.read(1, integerType(64));
        SemanticOperand b = context.read(2, integerType(64));
        SemanticOperand first = exclusiveOr(context,
                makeTruncate(context, shiftRight(context, a, 32), 32),
                makeTruncate(context, b, 32));
        SemanticOperand second = exclusiveOr(context, first,
                makeTruncate(context, shiftRight(context, b, 32), 32));
        context.write(0, context.emit(Op::OP_CONCAT, integerType(64), first,
                                      second));
        return;
    }

    // aes64ks1i: SubWord(RotWord(rs1[63:32])) ^ rcon, in both halves
    const uint rnum = (uint)context.getOperand(2).m_immediate;
    SemanticOperand a = context.read(1, integerType(64));
    SemanticOperand temp = makeTruncate(context, shiftRight(context, a, 32), 32);
    if (rnum != 0xA)
        temp = rotateRight(context, temp, 8);
    SemanticOperand substituted;
    for (uint i = 0; i < 4; i++)
    {
        SemanticOperand byte = lookupSbox(context, getByte(context, temp, 8 * i),
                                          false);
        SemanticOperand moved = shiftLeft(context,
                context.emit(Op::OP_ZERO_EXTEND, word, byte), 8 * i);
        substituted = (i == 0) ? moved : context.emit(Op::OP_OR, word,
                                                      substituted, moved);
    }
    if (rnum < 10)
        substituted = exclusiveOr(context, substituted,
                    makeConstant(context, gRoundConstants[rnum], word));
    context.write(0, context.emit(Op::OP_CONCAT, integerType(64), substituted,
                                  substituted));
}

void liftSha256(RiscvLiftContext& context)
{
    // The parameter: 0 sig0, 1 sig1, 2 sum0, 3 sum1
    const SemanticType word = integerType(32);
    SemanticOperand x = context.read(1, word);
    SemanticOperand result;
    switch (context.getParameter())
    {
    case 0:
        result = exclusiveOr(context, exclusiveOr(context,
                    rotateRight(context, x, 7), rotateRight(context, x, 18)),
                    shiftRight(context, x, 3));
        break;
    case 1:
        result = exclusiveOr(context, exclusiveOr(context,
                    rotateRight(context, x, 17), rotateRight(context, x, 19)),
                    shiftRight(context, x, 10));
        break;
    case 2:
        result = exclusiveOr(context, exclusiveOr(context,
                    rotateRight(context, x, 2), rotateRight(context, x, 13)),
                    rotateRight(context, x, 22));
        break;
    default:
        result = exclusiveOr(context, exclusiveOr(context,
                    rotateRight(context, x, 6), rotateRight(context, x, 11)),
                    rotateRight(context, x, 25));
        break;
    }
    context.writeSignExtended(0, result);
}

void liftSha512(RiscvLiftContext& context)
{
    // RV64. The parameter: 0 sig0, 1 sig1, 2 sum0, 3 sum1
    const SemanticType type = integerType(64);
    SemanticOperand x = context.read(1, type);
    SemanticOperand result;
    switch (context.getParameter())
    {
    case 0:
        result = exclusiveOr(context, exclusiveOr(context,
                    rotateRight(context, x, 1), rotateRight(context, x, 8)),
                    shiftRight(context, x, 7));
        break;
    case 1:
        result = exclusiveOr(context, exclusiveOr(context,
                    rotateRight(context, x, 19), rotateRight(context, x, 61)),
                    shiftRight(context, x, 6));
        break;
    case 2:
        result = exclusiveOr(context, exclusiveOr(context,
                    rotateRight(context, x, 28), rotateRight(context, x, 34)),
                    rotateRight(context, x, 39));
        break;
    default:
        result = exclusiveOr(context, exclusiveOr(context,
                    rotateRight(context, x, 14), rotateRight(context, x, 18)),
                    rotateRight(context, x, 41));
        break;
    }
    context.write(0, result);
}

namespace {

// A term of the RV32 SHA-512 instructions: operand 'source' (1 or 2) shifted
struct ShiftTerm {
    uint8 m_source;
    bool m_isLeft;
    uint8 m_amount;
};

// The terms of sig0l, sig0h, sig1l, sig1h, sum0r, sum1r
const ShiftTerm gSig0l[] = { {1, false, 1}, {1, false, 7}, {1, false, 8},
                             {2, true, 31}, {2, true, 25}, {2, true, 24} };
const ShiftTerm gSig0h[] = { {1, false, 1}, {1, false, 7}, {1, false, 8},
                             {2, true, 31}, {2, true, 24}, {0, false, 0} };
const ShiftTerm gSig1l[] = { {1, true, 3}, {1, false, 6}, {1, false, 19},
                             {2, false, 29}, {2, true, 26}, {2, true, 13} };
const ShiftTerm gSig1h[] = { {1, true, 3}, {1, false, 6}, {1, false, 19},
                             {2, false, 29}, {2, true, 13}, {0, false, 0} };
const ShiftTerm gSum0r[] = { {1, true, 25}, {1, true, 30}, {1, false, 28},
                             {2, false, 7}, {2, false, 2}, {2, true, 4} };
const ShiftTerm gSum1r[] = { {1, true, 23}, {1, false, 14}, {1, false, 18},
                             {2, false, 9}, {2, true, 18}, {2, true, 14} };

} // namespace

void liftSha512Rv32(RiscvLiftContext& context)
{
    // The parameter: 0 sig0l, 1 sig0h, 2 sig1l, 3 sig1h, 4 sum0r, 5 sum1r
    const ShiftTerm* tables[] = { gSig0l, gSig0h, gSig1l, gSig1h, gSum0r,
                                  gSum1r };
    const uint64 parameter = context.getParameter();
    CHECK(parameter < 6);
    const ShiftTerm* terms = tables[parameter];
    const SemanticType word = integerType(32);
    SemanticOperand sources[3];
    sources[1] = context.read(1, word);
    sources[2] = context.read(2, word);
    SemanticOperand result;
    for (uint i = 0; i < 6; i++)
    {
        if (terms[i].m_source == 0)
            continue;
        SemanticOperand term = terms[i].m_isLeft ?
                shiftLeft(context, sources[terms[i].m_source], terms[i].m_amount) :
                shiftRight(context, sources[terms[i].m_source], terms[i].m_amount);
        result = result.isValid() ? exclusiveOr(context, result, term) : term;
    }
    context.write(0, result);
}

void liftSm3(RiscvLiftContext& context)
{
    // sm3p0: x ^ (x <<< 9) ^ (x <<< 17), sm3p1: x ^ (x <<< 15) ^ (x <<< 23)
    const SemanticType word = integerType(32);
    SemanticOperand x = context.read(1, word);
    const bool isFirst = context.getParameter() == 0;
    context.writeSignExtended(0, exclusiveOr(context, exclusiveOr(context, x,
                rotateLeft(context, x, isFirst ? 9 : 15)),
                rotateLeft(context, x, isFirst ? 17 : 23)));
}

void liftSm4(RiscvLiftContext& context)
{
    // The S-box has no IR operation: the helper does the whole instruction
    const SemanticType type = context.getNativeType();
    context.emit(SemanticInstruction(Op::OP_HELPER, type, SemanticOperand(),
            makeConstant(context, RISCV_HELPER_INSTRUCTION, integerType(32)),
            makeConstant(context, context.getInstruction().getEncoding(), type)));
}

} // namespace riscvdis
