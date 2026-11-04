/*
 * RiscvSemanticLiftInteger.cpp
 *
 * Implementation file
 *
 * The lifters of the base integer instructions (RV32I and RV64I), M and
 * Zmmul, the bit manipulation (Zba, Zbb, Zbs, Zbc, Zbkb, Zbkx), Zicond and
 * the Zimop instructions (see RiscvSemanticInternal.h).
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

// Return true if the register is ra or t0, the ones of the return address
// stack hints
bool isLinkRegister(uint registerId)
{
    return (registerId == RISCV_REG_X0 + 1) || (registerId == RISCV_REG_X0 + 5);
}

// Return the shift count of operand 2 (a register or the shamt) modulo the
// size of 'type'
SemanticOperand getShiftCount(RiscvLiftContext& context,
                              const SemanticType& type)
{
    SemanticOperand count = context.read(2, type);
    if (context.getOperand(2).m_kind == RiscvDecodedOperand::KIND_IMMEDIATE)
        return count;
    return context.emit(Op::OP_AND, type, count,
                makeConstant(context, type.getTotalBits() - 1, type));
}

// Return the register operand 1 as the low 32 bits zero extended to XLEN
SemanticOperand getUnsignedWord(RiscvLiftContext& context, uint index)
{
    SemanticOperand word = context.read(index, integerType(32));
    return makeZeroExtend(context, word, context.getXlen());
}

} // namespace

void liftBinary(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    context.write(0, context.emit((Op::Operation)context.getParameter(),
                                  type, a, b));
}

void liftBinaryWord(RiscvLiftContext& context)
{
    const SemanticType type = integerType(32);
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    context.writeSignExtended(0, context.emit(
                (Op::Operation)context.getParameter(), type, a, b));
}

void liftShift(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(1, type);
    SemanticOperand count = getShiftCount(context, type);
    context.write(0, context.emit((Op::Operation)context.getParameter(),
                                  type, a, count));
}

void liftShiftWord(RiscvLiftContext& context)
{
    const SemanticType type = integerType(32);
    SemanticOperand a = context.read(1, type);
    SemanticOperand count = getShiftCount(context, type);
    context.writeSignExtended(0, context.emit(
                (Op::Operation)context.getParameter(), type, a, count));
}

void liftSetLess(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    SemanticOperand less = makeCompare(context,
            (Op::Condition)context.getParameter(), type, a, b);
    context.write(0, makeZeroExtend(context, less, context.getXlen()));
}

void liftUpper(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    // The operand is the 20 bit field: the value is sign extended from 32 bits
    const int64 value = (int64)(int32)((uint32)context.getOperand(1).m_immediate
                                       << 12);
    SemanticOperand upper = context.getImmediate(value, type);
    if (context.getParameter() != 0)
        upper = context.emit(Op::OP_ADD, type, context.getCurrentAddress(),
                             upper);
    context.write(0, upper);
}

void liftLoad(RiscvLiftContext& context)
{
    const uint64 parameter = context.getParameter();
    const uint bits = (uint)(parameter & 0xFF);
    const SemanticType type = integerType(bits);
    SemanticOperand value = context.load(context.getAddress(1), type);
    if (bits < context.getXlen())
        value = context.emit(((parameter & LOAD_UNSIGNED) != 0) ?
                                Op::OP_ZERO_EXTEND : Op::OP_SIGN_EXTEND,
                             context.getNativeType(), value);
    context.write(0, value);
}

void liftStore(RiscvLiftContext& context)
{
    const SemanticType type = integerType((uint)context.getParameter());
    context.store(context.getAddress(1), context.read(0, type));
}

void liftBranch(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(0, type);
    SemanticOperand b = context.read(1, type);
    SemanticOperand taken = makeCompare(context,
            (Op::Condition)context.getParameter(), type, a, b);
    context.branchIf(taken, context.read(2, type));
}

void liftJal(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand target = context.read(1, type);
    const uint rd = context.getOperand(0).m_register;
    context.write(0, context.getNextAddress());
    if (isLinkRegister(rd))
        context.call(target);
    else
        context.branch(target);
}

void liftJalr(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    const uint rd = context.getOperand(0).m_register;
    uint base;
    int64 offset;
    SemanticOperand target;
    if (context.getOperand(1).m_kind == RiscvDecodedOperand::KIND_MEMORY)
    {
        base = context.getOperand(1).m_base;
        offset = context.getOperand(1).m_hasOffset ?
                 context.getOperand(1).m_offset : 0;
        target = context.getAddress(1);
    } else
    {
        base = context.getOperand(1).m_register;
        offset = context.getOperand(2).m_immediate;
        target = context.emit(Op::OP_ADD, type, context.read(1, type),
                              context.read(2, type));
    }
    // The target has bit 0 cleared, and is computed before rd is written
    target = context.emit(Op::OP_AND, type, target,
                          makeConstant(context, -2, type));
    if (rd != RISCV_REG_X0)
        context.write(0, context.getNextAddress());
    const bool isReturn = (rd == RISCV_REG_X0) && isLinkRegister(base) &&
                          (offset == 0);
    // Zicfilp: an indirect jump expects a landing pad at its target, but not
    // the ones through ra and t0 (the returns, and the calls the hints of
    // the return address stack treat as such: whatever rd is) and through t2
    // (the software guarded branches)
    if (context.isControlFlowIntegrity() && !isLinkRegister(base) &&
        (base != RISCV_REG_X0 + 7))
        context.move(context.getRegister(RISCV_REG_ELP),
                     makeConstant(context, 1, integerType(1)));
    if (isReturn)
        context.returnTo(target);
    else if (isLinkRegister(rd))
        context.call(target);
    else
        context.branchIndirect(target);
}

void liftDivide(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    SemanticOperand result = context.newTemporary(type);
    SemanticInstruction instruction((Op::Operation)context.getParameter(),
                                    type, result, a, b);
    instruction.setBehavior(Op::BEHAVIOR_DIVIDE_ALL_ONES);
    context.emit(instruction);
    context.write(0, result);
}

void liftDivideWord(RiscvLiftContext& context)
{
    const SemanticType type = integerType(32);
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    SemanticOperand result = context.newTemporary(type);
    SemanticInstruction instruction((Op::Operation)context.getParameter(),
                                    type, result, a, b);
    instruction.setBehavior(Op::BEHAVIOR_DIVIDE_ALL_ONES);
    context.emit(instruction);
    context.writeSignExtended(0, result);
}

void liftShiftAdd(RiscvLiftContext& context)
{
    const uint64 parameter = context.getParameter();
    const SemanticType type = context.getNativeType();
    SemanticOperand a = ((parameter & SHIFT_ADD_WORD) != 0) ?
                        getUnsignedWord(context, 1) : context.read(1, type);
    const uint shift = (uint)(parameter & 0xFF);
    if (shift != 0)
        a = context.emit(Op::OP_SHIFT_LEFT, type, a,
                         makeConstant(context, shift, type));
    context.write(0, context.emit(Op::OP_ADD, type, a, context.read(2, type)));
}

void liftSlliUw(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = getUnsignedWord(context, 1);
    context.write(0, context.emit(Op::OP_SHIFT_LEFT, type, a,
                                  context.read(2, type)));
}

void liftLogicNegated(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    SemanticOperand result;
    switch (context.getParameter())
    {
    case 0:
        // a & ~b
        result = context.emit(Op::OP_AND_NOT, type, b, a);
        break;
    case 1:
        // a | ~b
        result = context.emit(Op::OP_OR, type, a,
                              context.emit(Op::OP_NOT, type, b));
        break;
    default:
        // ~(a ^ b)
        result = context.emit(Op::OP_NOT, type,
                              context.emit(Op::OP_XOR, type, a, b));
        break;
    }
    context.write(0, result);
}

void liftRotate(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(1, type);
    SemanticOperand count = getShiftCount(context, type);
    context.write(0, context.emit((Op::Operation)context.getParameter(),
                                  type, a, count));
}

void liftRotateWord(RiscvLiftContext& context)
{
    const SemanticType type = integerType(32);
    SemanticOperand a = context.read(1, type);
    SemanticOperand count = getShiftCount(context, type);
    context.writeSignExtended(0, context.emit(
                (Op::Operation)context.getParameter(), type, a, count));
}

void liftByteSwap(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    context.write(0, context.emit(Op::OP_BYTE_SWAP, type,
                                  context.read(1, type)));
}

void liftBitCount(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    context.write(0, context.emit((Op::Operation)context.getParameter(),
                                  type, context.read(1, type)));
}

void liftBitCountWord(RiscvLiftContext& context)
{
    const SemanticType type = integerType(32);
    SemanticOperand result = context.emit(
                (Op::Operation)context.getParameter(), type,
                context.read(1, type));
    context.write(0, makeZeroExtend(context, result, context.getXlen()));
}

void liftSignExtendBits(RiscvLiftContext& context)
{
    const uint bits = (uint)context.getParameter();
    SemanticOperand low = context.read(1, integerType(bits));
    context.write(0, makeSignExtend(context, low, context.getXlen()));
}

void liftZeroExtendHalf(RiscvLiftContext& context)
{
    SemanticOperand low = context.read(1, integerType(16));
    context.write(0, makeZeroExtend(context, low, context.getXlen()));
}

void liftOrCombineBytes(RiscvLiftContext& context)
{
    // Every byte which isn't zero becomes 0xFF
    const SemanticType type = context.getNativeType();
    const int64 low7 = (int64)0x7F7F7F7F7F7F7F7FULL & (int64)maskOf(context.getXlen());
    const int64 high1 = (int64)0x8080808080808080ULL & (int64)maskOf(context.getXlen());
    SemanticOperand x = context.read(1, type);
    SemanticOperand sum = context.emit(Op::OP_ADD, type,
            context.emit(Op::OP_AND, type, x, makeConstant(context, low7, type)),
            makeConstant(context, low7, type));
    SemanticOperand marks = context.emit(Op::OP_AND, type,
            context.emit(Op::OP_OR, type, sum, x),
            makeConstant(context, high1, type));
    // 0x80 -> 0xFF: marks | (marks - (marks >> 7))
    SemanticOperand low = context.emit(Op::OP_SUBTRACT, type, marks,
            context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, marks,
                         makeConstant(context, 7, type)));
    context.write(0, context.emit(Op::OP_OR, type, marks, low));
}

void liftSingleBit(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(1, type);
    SemanticOperand index = getShiftCount(context, type);
    SemanticOperand result;
    switch (context.getParameter())
    {
    case 1:
        // bext: (a >> index) & 1
        result = context.emit(Op::OP_AND, type,
                    context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, a, index),
                    makeConstant(context, 1, type));
        break;
    default:
    {
        SemanticOperand bit = context.emit(Op::OP_SHIFT_LEFT, type,
                                           makeConstant(context, 1, type),
                                           index);
        if (context.getParameter() == 0)
            result = context.emit(Op::OP_AND_NOT, type, bit, a);
        else if (context.getParameter() == 2)
            result = context.emit(Op::OP_XOR, type, a, bit);
        else
            result = context.emit(Op::OP_OR, type, a, bit);
        break;
    }
    }
    context.write(0, result);
}

void liftCarryless(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    SemanticOperand result;
    switch (context.getParameter())
    {
    case 0:
        result = context.emit(Op::OP_CARRYLESS_MULTIPLY, type, a, b);
        break;
    case 1:
        result = context.emit(Op::OP_CARRYLESS_MULTIPLY_HIGH, type, a, b);
        break;
    default:
    {
        // The product shifted right by XLEN - 1: bits XLEN - 1 to 2 XLEN - 2
        SemanticOperand low = context.emit(Op::OP_CARRYLESS_MULTIPLY, type, a, b);
        SemanticOperand high = context.emit(Op::OP_CARRYLESS_MULTIPLY_HIGH,
                                            type, a, b);
        result = context.emit(Op::OP_OR, type,
            context.emit(Op::OP_SHIFT_LEFT, type, high,
                         makeConstant(context, 1, type)),
            context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, low,
                         makeConstant(context, context.getXlen() - 1, type)));
        break;
    }
    }
    context.write(0, result);
}

void liftPack(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    switch (context.getParameter())
    {
    case 0:
    {
        // pack: rs1 in the low half, rs2 in the high half
        const uint half = context.getXlen() / 2;
        SemanticOperand low = context.emit(Op::OP_AND, type, a,
                    makeConstant(context, (int64)maskOf(half), type));
        SemanticOperand high = context.emit(Op::OP_SHIFT_LEFT, type, b,
                    makeConstant(context, half, type));
        context.write(0, context.emit(Op::OP_OR, type, low, high));
        break;
    }
    case 1:
    {
        // packh: the low bytes
        SemanticOperand low = context.emit(Op::OP_AND, type, a,
                                           makeConstant(context, 0xFF, type));
        SemanticOperand high = context.emit(Op::OP_SHIFT_LEFT, type,
                context.emit(Op::OP_AND, type, b,
                             makeConstant(context, 0xFF, type)),
                makeConstant(context, 8, type));
        context.write(0, context.emit(Op::OP_OR, type, low, high));
        break;
    }
    default:
    {
        // packw: the low halfwords, sign extended
        const SemanticType word = integerType(32);
        SemanticOperand low = context.emit(Op::OP_AND, word,
                makeTruncate(context, a, 32), makeConstant(context, 0xFFFF, word));
        SemanticOperand high = context.emit(Op::OP_SHIFT_LEFT, word,
                makeTruncate(context, b, 32),
                makeConstant(context, 16, word));
        context.writeSignExtended(0, context.emit(Op::OP_OR, word, low, high));
        break;
    }
    }
}

void liftReverseBitsInBytes(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand reversed = context.emit(Op::OP_BIT_REVERSE, type,
                                            context.read(1, type));
    context.write(0, context.emit(Op::OP_BYTE_SWAP, type, reversed));
}

void liftZip(RiscvLiftContext& context)
{
    // RV32 only
    const SemanticType type = integerType(32);
    SemanticOperand x = context.read(1, type);
    SemanticOperand even = makeConstant(context, 0x55555555, type);
    SemanticOperand odd = makeConstant(context, (int64)0xAAAAAAAAULL, type);
    SemanticOperand result;
    if (context.getParameter() == 0)
    {
        // zip: the low half to the even bits, the high half to the odd bits
        SemanticOperand low = context.emit(Op::OP_BIT_DEPOSIT, type, x, even);
        SemanticOperand high = context.emit(Op::OP_BIT_DEPOSIT, type,
                context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, x,
                             makeConstant(context, 16, type)), odd);
        result = context.emit(Op::OP_OR, type, low, high);
    } else
    {
        // unzip: the even bits to the low half, the odd bits to the high half
        SemanticOperand low = context.emit(Op::OP_BIT_GATHER, type, x, even);
        SemanticOperand high = context.emit(Op::OP_SHIFT_LEFT, type,
                context.emit(Op::OP_BIT_GATHER, type, x, odd),
                makeConstant(context, 16, type));
        result = context.emit(Op::OP_OR, type, low, high);
    }
    context.write(0, result);
}

void liftCrossbar(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    const uint width = (uint)context.getParameter();
    const uint elements = context.getXlen() / width;
    SemanticOperand table = context.read(1, type);
    SemanticOperand indexes = context.read(2, type);
    SemanticOperand result = makeConstant(context, 0, type);
    const int64 mask = (int64)maskOf(width);
    for (uint i = 0; i < elements; i++)
    {
        SemanticOperand index = context.emit(Op::OP_AND, type,
                context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, indexes,
                             makeConstant(context, i * width, type)),
                makeConstant(context, mask, type));
        // An index beyond the elements gives 0: a shift of the table by the
        // size or more is 0
        SemanticOperand shifted = context.emit(Op::OP_SHIFT_RIGHT_LOGICAL,
                type, table,
                context.emit(Op::OP_MULTIPLY, type, index,
                             makeConstant(context, width, type)));
        SemanticOperand element = context.emit(Op::OP_AND, type, shifted,
                                               makeConstant(context, mask, type));
        result = context.emit(Op::OP_OR, type, result,
                context.emit(Op::OP_SHIFT_LEFT, type, element,
                             makeConstant(context, i * width, type)));
    }
    context.write(0, result);
}

void liftConditionalZero(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand value = context.read(1, type);
    SemanticOperand condition = context.read(2, type);
    // czero.eqz clears the value when the condition register is zero
    SemanticOperand isZero = makeCompare(context,
            (context.getParameter() == 0) ? Op::COND_EQUAL : Op::COND_NOT_EQUAL,
            type, condition, makeConstant(context, 0, type));
    context.write(0, makeSelect(context, type, isZero,
                                makeConstant(context, 0, type), value));
}

void liftMop(RiscvLiftContext& context)
{
    // (c.mop.N has no destination)
    if (context.getOperandsCount() == 0)
    {
        liftHint(context);
        return;
    }
    context.write(0, makeConstant(context, 0, context.getNativeType()));
}

} // namespace riscvdis
