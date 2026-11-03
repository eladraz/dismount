/*
 * IA32SemanticTable.cpp
 *
 * The semantics of the IA-32 integer instructions: the template steps of the
 * regular mnemonics, the lifters of the others, and the table which
 * classifies them all, see IA32Semantics.h.
 *
 * The flags of a table entry: 'written' are all the flags the instruction may
 * change, the 'undefined' flags among them have no defined value (the SDM
 * says "undefined"). The result of an instruction which leaves flags
 * undefined still follows the recipe it uses, but nothing depends on it.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/proc/ia32/IA32Opcode.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/IA32OperandDecoder.h"
#include "dismount/proc/ia32/IA32Semantics.h"

using namespace ia32dis;

typedef SemanticOperation Op;
typedef OpcodeClassification Cl;
typedef IA32LiftContext Context;

/////////////////////////////////////////////////////////////////////////////
// The lifters
/////////////////////////////////////////////////////////////////////////////

namespace {

// The prefix bytes of rep / repne, and of lock
const uint8 PREFIX_REPNE = 0xF2;
const uint8 PREFIX_REPE  = 0xF3;
const uint8 PREFIX_LOCK  = 0xF0;

// The flags
const uint32 FC = SemanticFlags::FLAG_CARRY;
const uint32 FZ = SemanticFlags::FLAG_ZERO;
const uint32 FS = SemanticFlags::FLAG_SIGN;
const uint32 FO = SemanticFlags::FLAG_OVERFLOW;
const uint32 FP = SemanticFlags::FLAG_PARITY;
const uint32 FA = SemanticFlags::FLAG_ADJUST;
const uint32 FD = SemanticFlags::FLAG_DIRECTION;
const uint32 FALL = SemanticFlags::FLAGS_ARITHMETIC;

SemanticType integerType(uint bits)
{
    return SemanticType::integer(bits);
}

SemanticOperand immediate(Context& c, int64 value, uint bits)
{
    return c.getImmediate(value, integerType(bits));
}

uint operandBitsOf(Context& c)
{
    return IA32OperandDecoder::getOperandBits(c.getInstruction());
}

// Return the stack pointer register: rsp in 64 bit mode, esp in protected
// mode, sp in real mode
SemanticOperand stackPointerOf(Context& c)
{
    return c.getStackPointer();
}

// Return the frame pointer register of the stack size
SemanticOperand framePointerOf(Context& c)
{
    const uint stackBits = c.getStackAddressType().getTotalBits();
    return c.getRegister(stackBits == 64 ? AMD64_REG_RBP :
                         stackBits == 32 ? IA32_REG_EBP : IA32_REG_BP);
}

// Return the value of a register operand in a temporary, so a later write
// to the register doesn't change it. Other operands are returned as is
SemanticOperand snapshot(Context& c, const SemanticOperand& value)
{
    if (value.getKind() != SemanticOperand::KIND_REGISTER)
        return value;
    return c.emit(Op::OP_MOVE, value.getType(), value);
}

// Return a 1 bit temporary: 'first' <condition> 'second'
SemanticOperand compare(Context& c, Op::Condition condition,
                        const SemanticOperand& first,
                        const SemanticOperand& second)
{
    SemanticOperand result = c.newTemporary(integerType(1));
    SemanticInstruction instruction(Op::OP_COMPARE, first.getType(), result,
                                    first, second);
    instruction.setCondition(condition);
    c.emit(instruction);
    return result;
}

// Continue only if 'isOk' is true, otherwise raise the exception 'number'
void trapUnless(Context& c, const SemanticOperand& isOk, uint number)
{
    SemanticOperand ok = c.newLabel();
    c.branchIf(isOk, ok);
    c.trap(Op::TRAP_FAULT, number);
    c.placeLabel(ok);
}

// Return log2 of a power of two
uint log2Of(uint value)
{
    uint result = 0;
    while ((((uint)1 << result) < value) && (result < 31))
        result++;
    return result;
}

// Return the integer 'value' in 'bits' bits: truncated or zero extended
SemanticOperand resize(Context& c, const SemanticOperand& value, uint bits)
{
    const uint valueBits = value.getType().getTotalBits();
    if (valueBits == bits)
        return value;
    return c.emit(valueBits > bits ? Op::OP_TRUNCATE : Op::OP_ZERO_EXTEND,
                  integerType(bits), value);
}

/////////////////////////////////////////////////////////////////////////////
// Data movement
/////////////////////////////////////////////////////////////////////////////

// lea r, m: the offset of the memory operand, in the size of the register
void liftLea(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    SemanticOperand offset = c.getOffset(1);
    const uint addressBits = offset.getType().getTotalBits();
    if (addressBits > bits)
        offset = c.emit(Op::OP_TRUNCATE, integerType(bits), offset);
    else if (addressBits < bits)
        offset = c.emit(Op::OP_ZERO_EXTEND, integerType(bits), offset);
    c.write(0, offset);
}

// xchg: with a memory operand the exchange is always atomic
void liftXchg(Context& c)
{
    const bool isMemoryFirst =
        (c.getOperand(0).m_kind == IA32DecodedOperand::KIND_MEMORY);
    const bool isMemorySecond =
        (c.getOperand(1).m_kind == IA32DecodedOperand::KIND_MEMORY);
    if (!isMemoryFirst && !isMemorySecond)
    {
        SemanticOperand first = snapshot(c, c.read(0));
        SemanticOperand second = snapshot(c, c.read(1));
        c.write(0, second);
        c.write(1, first);
        return;
    }

    const uint memoryIndex = isMemoryFirst ? 0 : 1;
    const uint registerIndex = 1 - memoryIndex;
    const SemanticType type = c.getOperandType(registerIndex);
    SemanticOperand address = c.getAddress(memoryIndex);
    SemanticOperand old = c.newTemporary(type);
    SemanticInstruction exchange(Op::OP_ATOMIC_EXCHANGE, type, old, address,
                                 c.read(registerIndex));
    exchange.setMemoryOrder(Op::ORDER_SEQUENTIAL);
    exchange.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    c.emit(exchange);
    c.write(registerIndex, old);
}

// cbw / cwde: sign extend al into ax, or ax into eax
void liftConvertAccumulator(Context& c)
{
    const uint bits = operandBitsOf(c);
    SemanticOperand narrow = c.getGeneralRegister(0, bits / 2);
    c.move(c.getGeneralRegister(0, bits),
           c.emit(Op::OP_SIGN_EXTEND, integerType(bits), narrow));
}

// cwd / cdq: the sign of ax / eax fills dx / edx
void liftConvertAccumulatorToData(Context& c)
{
    const uint bits = operandBitsOf(c);
    SemanticOperand sign = c.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC,
                                  integerType(bits),
                                  c.getGeneralRegister(0, bits),
                                  immediate(c, bits - 1, bits));
    c.move(c.getGeneralRegister(2, bits), sign);
}

// xlat: al = [seg:bx + al]
void liftXlat(Context& c)
{
    const SemanticType addressType = c.getAddressSizeType();
    SemanticOperand index = c.emit(Op::OP_ZERO_EXTEND, addressType,
                                   c.getRegister(IA32_REG_AL));
    SemanticOperand offset = c.emit(Op::OP_ADD, addressType,
        c.getGeneralRegister(IA32_GP32_EBX, addressType.getTotalBits()), index);
    const uint segment = IA32OperandDecoder::getSegment(c.getInstruction(),
                                                        IA32_REG_DS);
    SemanticOperand value = c.load(c.getLinearAddress(segment, offset),
                                   integerType(8));
    c.move(c.getRegister(IA32_REG_AL), value);
}

/////////////////////////////////////////////////////////////////////////////
// The stack
/////////////////////////////////////////////////////////////////////////////

void liftPush(Context& c)
{
    SemanticOperand value = c.read(0);
    // A segment register is pushed in the operand size
    const SemanticType operandType = c.getOperandSizeType();
    if (value.getType().getTotalBits() < operandType.getTotalBits())
        value = c.emit(Op::OP_ZERO_EXTEND, operandType, value);
    c.push(value);
}

void liftPop(Context& c)
{
    // The destination is written after the stack pointer: pop [esp] and
    // pop esp
    SemanticOperand value = c.pop(c.getOperandType(0));
    c.write(0, value);
}

void liftPusha(Context& c)
{
    const uint bits = operandBitsOf(c);
    const uint bytes = bits / 8;
    const SemanticType stackType = c.getStackAddressType();
    SemanticOperand stackPointer = stackPointerOf(c);
    // The registers are stored with the value they had before the pusha
    // (including the stack pointer), and the stack pointer changes last
    for (uint i = 0; i < 8; i++)
    {
        SemanticOperand offset = c.emit(Op::OP_SUBTRACT, stackType,
                stackPointer, immediate(c, (i + 1) * bytes, stackType.getTotalBits()));
        c.store(c.getLinearAddress(IA32_REG_SS, offset),
                c.getGeneralRegister(i, bits));
    }
    c.move(stackPointer, c.emit(Op::OP_SUBTRACT, stackType, stackPointer,
                    immediate(c, 8 * bytes, stackType.getTotalBits())));
}

void liftPopa(Context& c)
{
    const uint bits = operandBitsOf(c);
    const uint bytes = bits / 8;
    const SemanticType stackType = c.getStackAddressType();
    SemanticOperand stackPointer = stackPointerOf(c);
    // The values are read first, edi at the top of the stack. The stack
    // pointer in the frame is skipped
    SemanticOperand values[8];
    for (uint i = 0; i < 8; i++)
    {
        const uint registerNumber = 7 - i;
        SemanticOperand offset = c.emit(Op::OP_ADD, stackType, stackPointer,
                    immediate(c, i * bytes, stackType.getTotalBits()));
        values[registerNumber] = c.load(c.getLinearAddress(IA32_REG_SS, offset),
                                        integerType(bits));
    }
    c.move(stackPointer, c.emit(Op::OP_ADD, stackType, stackPointer,
                    immediate(c, 8 * bytes, stackType.getTotalBits())));
    for (uint i = 0; i < 8; i++)
        if (i != IA32_GP32_ESP)
            c.move(c.getGeneralRegister(i, bits), values[i]);
}

// The bits of EFLAGS that user code reads and writes: the flag registers and
// the system flags
struct FlagBit {
    uint m_register;
    uint32 m_flag;
    uint m_bit;
};
const FlagBit gEflagsBits[] = {
    { IA32_REG_CF, SemanticFlags::FLAG_CARRY,     0 },
    { IA32_REG_PF, SemanticFlags::FLAG_PARITY,    2 },
    { IA32_REG_AF, SemanticFlags::FLAG_ADJUST,    4 },
    { IA32_REG_ZF, SemanticFlags::FLAG_ZERO,      6 },
    { IA32_REG_SF, SemanticFlags::FLAG_SIGN,      7 },
    { IA32_REG_IF, 0,                             9 },
    { IA32_REG_DF, SemanticFlags::FLAG_DIRECTION, 10 },
    { IA32_REG_OF, SemanticFlags::FLAG_OVERFLOW,  11 },
    { IA32_REG_AC, 0,                             18 },
    { IA32_REG_ID, 0,                             21 }
};
const uint gEflagsBitsCount = sizeof(gEflagsBits) / sizeof(gEflagsBits[0]);

// Return the 32 bit EFLAGS value, built from the flag registers (bit 1 is
// always set)
SemanticOperand composeEflags(Context& c)
{
    const SemanticType type = integerType(32);
    SemanticOperand result = immediate(c, 2, 32);
    for (uint i = 0; i < gEflagsBitsCount; i++)
    {
        SemanticOperand bit = c.emit(Op::OP_ZERO_EXTEND, type,
                                     c.getRegister(gEflagsBits[i].m_register));
        if (gEflagsBits[i].m_bit != 0)
            bit = c.emit(Op::OP_SHIFT_LEFT, type, bit,
                         immediate(c, gEflagsBits[i].m_bit, 32));
        result = c.emit(Op::OP_OR, type, result, bit);
    }
    return result;
}

// Return bit 'bit' of 'value' as a 1 bit value
SemanticOperand extractBit(Context& c, const SemanticOperand& value, uint bit)
{
    SemanticOperand shifted = value;
    if (bit != 0)
        shifted = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, value.getType(), value,
                         immediate(c, bit, value.getType().getTotalBits()));
    return c.emit(Op::OP_TRUNCATE, integerType(1), shifted);
}

void liftPushf(Context& c)
{
    SemanticOperand flags = composeEflags(c);
    c.push(resize(c, flags, operandBitsOf(c)));
}

void liftPopf(Context& c)
{
    const uint bits = operandBitsOf(c);
    SemanticOperand value = c.pop(integerType(bits));
    for (uint i = 0; i < gEflagsBitsCount; i++)
    {
        // A 16 bit popf doesn't change the AC and ID bits
        if (gEflagsBits[i].m_bit >= bits)
            continue;
        SemanticOperand bit = extractBit(c, value, gEflagsBits[i].m_bit);
        if (gEflagsBits[i].m_flag != 0)
            c.setFlags(gEflagsBits[i].m_flag, bit);
        else
            c.move(c.getRegister(gEflagsBits[i].m_register), bit);
    }
}

// The flags of the ah register: sf zf 0 af 0 pf 1 cf
void liftLahf(Context& c)
{
    const SemanticType type = integerType(8);
    SemanticOperand result = immediate(c, 2, 8);
    static const uint registers[] = { IA32_REG_CF, IA32_REG_PF, IA32_REG_AF,
                                      IA32_REG_ZF, IA32_REG_SF };
    static const uint bits[] = { 0, 2, 4, 6, 7 };
    for (uint i = 0; i < 5; i++)
    {
        SemanticOperand bit = c.emit(Op::OP_ZERO_EXTEND, type,
                                     c.getRegister(registers[i]));
        if (bits[i] != 0)
            bit = c.emit(Op::OP_SHIFT_LEFT, type, bit, immediate(c, bits[i], 8));
        result = c.emit(Op::OP_OR, type, result, bit);
    }
    c.move(c.getRegister(IA32_REG_AH), result);
}

void liftSahf(Context& c)
{
    SemanticOperand ah = c.getRegister(IA32_REG_AH);
    c.setFlags(SemanticFlags::FLAG_CARRY, extractBit(c, ah, 0));
    c.setFlags(SemanticFlags::FLAG_PARITY, extractBit(c, ah, 2));
    c.setFlags(SemanticFlags::FLAG_ADJUST, extractBit(c, ah, 4));
    c.setFlags(SemanticFlags::FLAG_ZERO, extractBit(c, ah, 6));
    c.setFlags(SemanticFlags::FLAG_SIGN, extractBit(c, ah, 7));
}

// enter size, level. The frame pointer is pushed, the pointers of the outer
// frames are copied, and the frame is allocated
void liftEnter(Context& c)
{
    const uint bits = operandBitsOf(c);
    const uint bytes = bits / 8;
    const SemanticType stackType = c.getStackAddressType();
    const uint stackBits = stackType.getTotalBits();
    const uint level = (uint)(c.getOperand(1).m_immediate & 0x1F);
    const int64 size = c.getOperand(0).m_immediate;
    SemanticOperand stackPointer = stackPointerOf(c);
    SemanticOperand framePointer = framePointerOf(c);

    // push ebp: the new frame is where it's stored
    SemanticOperand frame = c.emit(Op::OP_SUBTRACT, stackType, stackPointer,
                                   immediate(c, bytes, stackBits));
    c.store(c.getLinearAddress(IA32_REG_SS, frame), c.getGeneralRegister(IA32_GP32_EBP, bits));
    if (level != 0)
    {
        // Copy the level - 1 pointers of the outer frame
        for (uint i = 1; i < level; i++)
        {
            SemanticOperand from = c.emit(Op::OP_SUBTRACT, stackType, framePointer,
                                          immediate(c, bytes * i, stackBits));
            SemanticOperand value = c.load(c.getLinearAddress(IA32_REG_SS, from),
                                           integerType(bits));
            SemanticOperand to = c.emit(Op::OP_SUBTRACT, stackType, frame,
                                        immediate(c, bytes * i, stackBits));
            c.store(c.getLinearAddress(IA32_REG_SS, to), value);
        }
        // The last level is the new frame pointer
        SemanticOperand to = c.emit(Op::OP_SUBTRACT, stackType, frame,
                                    immediate(c, bytes * level, stackBits));
        c.store(c.getLinearAddress(IA32_REG_SS, to), resize(c, frame, bits));
    }
    c.move(framePointer, frame);
    c.move(stackPointer, c.emit(Op::OP_SUBTRACT, stackType, frame,
                                immediate(c, size + bytes * level, stackBits)));
}

// leave: esp = ebp, ebp = pop
void liftLeave(Context& c)
{
    const uint bits = operandBitsOf(c);
    const SemanticType stackType = c.getStackAddressType();
    const uint stackBits = stackType.getTotalBits();
    SemanticOperand framePointer = framePointerOf(c);
    SemanticOperand value = c.load(c.getLinearAddress(IA32_REG_SS, framePointer),
                                   integerType(bits));
    SemanticOperand newPointer = c.emit(Op::OP_ADD, stackType, framePointer,
                                        immediate(c, bits / 8, stackBits));
    c.move(c.getGeneralRegister(IA32_GP32_EBP, bits), value);
    c.move(stackPointerOf(c), newPointer);
}

/////////////////////////////////////////////////////////////////////////////
// Control flow
/////////////////////////////////////////////////////////////////////////////

// Return true if the operand 0 of a jmp / call is a far pointer, which isn't
// lifted: it loads cs
bool isFarTarget(Context& c)
{
    const IA32DecodedOperand& target = c.getOperand(0);
    return (target.m_kind == IA32DecodedOperand::KIND_FAR_POINTER) ||
           ((target.m_kind == IA32DecodedOperand::KIND_MEMORY) &&
            (target.m_bits != operandBitsOf(c)));
}

void liftUndefined(Context& c)
{
    c.emit(SemanticInstruction(Op::OP_UNDEFINED, SemanticType(),
                               SemanticOperand()));
}

void liftJmp(Context& c)
{
    if (isFarTarget(c))
    {
        liftUndefined(c);
        return;
    }
    if (c.getOperand(0).m_kind == IA32DecodedOperand::KIND_BRANCH_TARGET)
    {
        c.branch(c.read(0));
        return;
    }
    c.emit(SemanticInstruction(Op::OP_BRANCH_INDIRECT, SemanticType(),
                               SemanticOperand(), c.read(0)));
}

void liftCall(Context& c)
{
    if (isFarTarget(c))
    {
        liftUndefined(c);
        return;
    }
    // The target is read before the return address is pushed (call [esp])
    SemanticOperand target = snapshot(c, c.read(0));
    const uint bits = operandBitsOf(c);
    c.push(SemanticOperand::guestAddress(
                IA32OperandDecoder::getNextAddress(c.getInstruction()),
                integerType(bits)));
    c.emit(SemanticInstruction(Op::OP_CALL, SemanticType(), SemanticOperand(),
                               target));
}

// ret and ret imm16 (near)
void liftRet(Context& c)
{
    SemanticOperand target = c.pop(c.getOperandSizeType());
    if (c.getOperandsCount() == 1)
    {
        const SemanticType stackType = c.getStackAddressType();
        SemanticOperand stackPointer = stackPointerOf(c);
        c.move(stackPointer, c.emit(Op::OP_ADD, stackType, stackPointer,
                    immediate(c, c.getOperand(0).m_immediate & 0xFFFF,
                              stackType.getTotalBits())));
    }
    c.emit(SemanticInstruction(Op::OP_RETURN, SemanticType(), SemanticOperand(),
                               target));
}

// The loops decrement cx / ecx (by the address size) and jump while it's not
// zero; loopz and loopnz look at the zero flag too. jcxz jumps if it's zero
enum LoopKind {
    LOOP_ALWAYS,
    LOOP_WHILE_EQUAL,
    LOOP_WHILE_NOT_EQUAL,
    LOOP_IF_ZERO
};

template <int KIND>
void liftLoop(Context& c)
{
    const SemanticType type = c.getAddressSizeType();
    SemanticOperand counter = c.getGeneralRegister(IA32_GP32_ECX,
                                                   type.getTotalBits());
    SemanticOperand condition;
    if (KIND == LOOP_IF_ZERO)
    {
        condition = compare(c, Op::COND_EQUAL, counter,
                            c.getImmediate(0, type));
    } else
    {
        SemanticOperand decremented = c.emit(Op::OP_SUBTRACT, type, counter,
                                             c.getImmediate(1, type));
        c.move(counter, decremented);
        condition = compare(c, Op::COND_NOT_EQUAL, decremented,
                            c.getImmediate(0, type));
        if (KIND != LOOP_ALWAYS)
        {
            SemanticOperand flag = c.testFlags(KIND == LOOP_WHILE_EQUAL ?
                                    Op::COND_EQUAL : Op::COND_NOT_EQUAL);
            condition = c.emit(Op::OP_AND, integerType(1), condition, flag);
        }
    }
    c.branchIf(condition, c.read(0));
}

void liftInt(Context& c)
{
    c.trap(Op::TRAP_INTERRUPT, (uint)c.getOperand(0).m_immediate);
}

void liftInt3(Context& c)
{
    c.trap(Op::TRAP_BREAKPOINT, 3);
}

// int1 (icebp) raises the debug exception
void liftInt1(Context& c)
{
    c.trap(Op::TRAP_FAULT, 1);
}

// ud0, ud1, ud2: the invalid opcode exception, the operands aren't accessed
void liftUndefinedInstruction(Context& c)
{
    c.trap(Op::TRAP_UNDEFINED, 6);
}

// salc: al = carry ? 0xFF : 0
void liftSalc(Context& c)
{
    c.move(c.getGeneralRegister(IA32_GP32_EAX, 8),
           c.emit(Op::OP_SELECT, integerType(8), c.testFlags(Op::COND_CARRY),
                  immediate(c, 0xFF, 8), immediate(c, 0, 8)));
}

void liftInto(Context& c)
{
    SemanticOperand skip = c.newLabel();
    c.branchIf(c.testFlags(Op::COND_NOT_OVERFLOW), skip);
    c.trap(Op::TRAP_INTERRUPT, 4);
    c.placeLabel(skip);
}

// bound r, m: the index must be in [lower, upper] (signed)
void liftBound(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerType(bits);
    SemanticOperand index = c.read(0);
    // The upper bound follows the lower bound in memory
    SemanticOperand lowerAddress = c.getAddress(1);
    SemanticOperand lower = c.load(lowerAddress, type);
    SemanticOperand upperAddress = c.emit(Op::OP_ADD, integerType(32),
                                          lowerAddress, immediate(c, bits / 8, 32));
    SemanticOperand upper = c.load(upperAddress, type);
    SemanticOperand isAbove = compare(c, Op::COND_SIGNED_GREATER_EQUAL, index, lower);
    SemanticOperand isBelow = compare(c, Op::COND_SIGNED_LESS_EQUAL, index, upper);
    trapUnless(c, c.emit(Op::OP_AND, integerType(1), isAbove, isBelow), 5);
}

/////////////////////////////////////////////////////////////////////////////
// The string instructions
/////////////////////////////////////////////////////////////////////////////

enum StringKind {
    STRING_MOVS,
    STRING_CMPS,
    STRING_STOS,
    STRING_LODS,
    STRING_SCAS
};

// The body of a string instruction: a single iteration
void liftStringIteration(Context& c, int kind, uint bits)
{
    const SemanticType type = integerType(bits);
    const SemanticType addressType = c.getAddressSizeType();
    const uint addressBits = addressType.getTotalBits();
    SemanticOperand source = c.getGeneralRegister(IA32_GP32_ESI, addressBits);
    SemanticOperand destination = c.getGeneralRegister(IA32_GP32_EDI, addressBits);
    const uint sourceSegment = IA32OperandDecoder::getSegment(
                                        c.getInstruction(), IA32_REG_DS);
    SemanticOperand accumulator = c.getGeneralRegister(0, bits);

    // The pointers move by the size of the element, backward if DF is set
    SemanticOperand step = c.emit(Op::OP_SELECT, addressType,
                    c.getRegister(IA32_REG_DF),
                    c.getImmediate(-(int64)(bits / 8), addressType),
                    c.getImmediate(bits / 8, addressType));

    switch (kind)
    {
    case STRING_MOVS:
    {
        SemanticOperand value = c.load(c.getLinearAddress(sourceSegment, source), type);
        c.store(c.getLinearAddress(IA32_REG_ES, destination), value);
        break;
    }
    case STRING_CMPS:
    case STRING_SCAS:
    {
        SemanticOperand first = accumulator;
        if (kind == STRING_CMPS)
            first = c.load(c.getLinearAddress(sourceSegment, source), type);
        SemanticOperand second = c.load(c.getLinearAddress(IA32_REG_ES, destination), type);
        SemanticOperand difference = c.emit(Op::OP_SUBTRACT, type, first, second);
        c.setFlags(Op::FLAGS_SUBTRACT, FALL, type, first, second, difference);
        break;
    }
    case STRING_STOS:
        c.store(c.getLinearAddress(IA32_REG_ES, destination), accumulator);
        break;
    case STRING_LODS:
    {
        SemanticOperand value = c.load(c.getLinearAddress(sourceSegment, source), type);
        c.move(accumulator, value);
        break;
    }
    }

    if ((kind == STRING_MOVS) || (kind == STRING_CMPS) || (kind == STRING_LODS))
        c.move(source, c.emit(Op::OP_ADD, addressType, source, step));
    if (kind != STRING_LODS)
        c.move(destination, c.emit(Op::OP_ADD, addressType, destination, step));
}

// A string instruction, repeated by the rep prefixes: while cx / ecx is not
// zero. cmps and scas stop when the zero flag differs from the prefix
template <int KIND>
void liftString(Context& c)
{
    // The byte forms have an even opcode (A4, A6, AA, AC, AE)
    const uint bits = ((IA32OperandDecoder::getLastOpcodeByte(c.getInstruction()) & 1) == 0) ?
                      8 : operandBitsOf(c);
    const bool isRepe = IA32OperandDecoder::hasPrefix(c.getInstruction(), PREFIX_REPE);
    const bool isRepne = IA32OperandDecoder::hasPrefix(c.getInstruction(), PREFIX_REPNE);
    if (!isRepe && !isRepne)
    {
        liftStringIteration(c, KIND, bits);
        return;
    }

    const SemanticType addressType = c.getAddressSizeType();
    SemanticOperand counter = c.getGeneralRegister(IA32_GP32_ECX,
                                                   addressType.getTotalBits());
    SemanticOperand top = c.newLabel();
    SemanticOperand end = c.newLabel();
    c.placeLabel(top);
    c.branchIf(compare(c, Op::COND_EQUAL, counter, c.getImmediate(0, addressType)),
               end);
    liftStringIteration(c, KIND, bits);
    c.move(counter, c.emit(Op::OP_SUBTRACT, addressType, counter,
                           c.getImmediate(1, addressType)));
    if ((KIND == STRING_CMPS) || (KIND == STRING_SCAS))
    {
        // repe stops when the operands differ, repne when they're equal
        c.branchIf(c.testFlags(isRepe ? Op::COND_NOT_EQUAL : Op::COND_EQUAL),
                   end);
    }
    c.branch(top);
    c.placeLabel(end);
}

/////////////////////////////////////////////////////////////////////////////
// Multiply and divide
/////////////////////////////////////////////////////////////////////////////

// Write the double width result of mul / imul: ax, or dx:ax, or edx:eax
void writeProduct(Context& c, uint bits, const SemanticOperand& low,
                  const SemanticOperand& high)
{
    if (bits == 8)
    {
        c.move(c.getRegister(IA32_REG_AL), low);
        c.move(c.getRegister(IA32_REG_AH), high);
    } else
    {
        c.move(c.getGeneralRegister(0, bits), low);
        c.move(c.getGeneralRegister(2, bits), high);
    }
}

// mul / imul r/m: al, ax or eax times the operand
template <bool IS_SIGNED>
void liftMultiplyOneOperand(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerType(bits);
    SemanticOperand factor = c.read(0);
    SemanticOperand accumulator = c.getGeneralRegister(0, bits);
    SemanticOperand low = c.emit(Op::OP_MULTIPLY, type, accumulator, factor);
    SemanticOperand high = c.emit(IS_SIGNED ? Op::OP_MULTIPLY_HIGH_SIGNED :
                                              Op::OP_MULTIPLY_HIGH_UNSIGNED,
                                  type, accumulator, factor);
    c.setFlags(IS_SIGNED ? Op::FLAGS_MULTIPLY_SIGNED : Op::FLAGS_MULTIPLY_UNSIGNED,
               FC | FO, type, low, high, SemanticOperand());
    writeProduct(c, bits, low, high);
}

// imul r, r/m and imul r, r/m, imm
void liftImulTruncated(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerType(bits);
    const bool isThreeOperands = (c.getOperandsCount() == 3);
    SemanticOperand first = c.read(isThreeOperands ? 1 : 0);
    SemanticOperand second = c.read(isThreeOperands ? 2 : 1);
    SemanticOperand low = c.emit(Op::OP_MULTIPLY, type, first, second);
    SemanticOperand high = c.emit(Op::OP_MULTIPLY_HIGH_SIGNED, type, first, second);
    c.setFlags(Op::FLAGS_MULTIPLY_SIGNED, FC | FO, type, low, high,
               SemanticOperand());
    c.write(0, low);
}

// div / idiv r/m: ax / r/m8, dx:ax / r/m16, edx:eax / r/m32. The divide error
// is raised before any register changes
template <bool IS_SIGNED>
void liftDivide(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerType(bits);
    const SemanticType wide = integerType(2 * bits);
    SemanticOperand divisor = c.read(0);

    SemanticOperand dividend;
    if (bits == 8)
        dividend = c.getRegister(IA32_REG_AX);
    else
        dividend = c.emit(Op::OP_CONCAT, wide, c.getGeneralRegister(0, bits),
                          c.getGeneralRegister(2, bits));

    // A division by zero
    trapUnless(c, compare(c, Op::COND_NOT_EQUAL, divisor, c.getImmediate(0, type)), 0);

    SemanticOperand wideDivisor = c.emit(IS_SIGNED ? Op::OP_SIGN_EXTEND :
                                                     Op::OP_ZERO_EXTEND,
                                         wide, divisor);
    SemanticOperand quotient = c.newTemporary(wide);
    SemanticOperand remainder = c.newTemporary(wide);
    SemanticInstruction divide(IS_SIGNED ? Op::OP_DIVIDE_SIGNED :
                                           Op::OP_DIVIDE_UNSIGNED,
                               wide, quotient, dividend, wideDivisor);
    divide.setBehavior(Op::BEHAVIOR_DIVIDE_FAULT);
    divide.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    c.emit(divide);
    SemanticInstruction modulo(IS_SIGNED ? Op::OP_REMAINDER_SIGNED :
                                           Op::OP_REMAINDER_UNSIGNED,
                               wide, remainder, dividend, wideDivisor);
    modulo.setBehavior(Op::BEHAVIOR_DIVIDE_FAULT);
    modulo.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    c.emit(modulo);

    // The quotient must fit the register
    SemanticOperand narrowQuotient = c.emit(Op::OP_TRUNCATE, type, quotient);
    SemanticOperand fits;
    if (bits == 64)
    {
        // The comparisons are on numbers of up to 64 bits: the high half of
        // the quotient is the sign (or zero)
        SemanticOperand high = c.emit(Op::OP_EXTRACT, type, quotient,
                                      immediate(c, 64, 32));
        SemanticOperand expectedHigh = immediate(c, 0, 64);
        if (IS_SIGNED)
            expectedHigh = c.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, type,
                                  narrowQuotient, immediate(c, 63, 64));
        fits = compare(c, Op::COND_EQUAL, high, expectedHigh);
    } else if (IS_SIGNED)
    {
        fits = compare(c, Op::COND_EQUAL, quotient,
                       c.emit(Op::OP_SIGN_EXTEND, wide, narrowQuotient));
    } else
    {
        fits = compare(c, Op::COND_EQUAL, quotient,
                       c.emit(Op::OP_ZERO_EXTEND, wide, narrowQuotient));
    }
    trapUnless(c, fits, 0);

    writeProduct(c, bits, narrowQuotient,
                 c.emit(Op::OP_TRUNCATE, type, remainder));
}

/////////////////////////////////////////////////////////////////////////////
// Shifts and rotates
/////////////////////////////////////////////////////////////////////////////

enum ShiftKind {
    SHIFT_LEFT,
    SHIFT_RIGHT,
    SHIFT_RIGHT_ARITHMETIC,
    ROTATE_LEFT,
    ROTATE_RIGHT,
    ROTATE_LEFT_CARRY,
    ROTATE_RIGHT_CARRY
};

// Return the count masked to 5 bits, 6 bits for a 64 bit operand (an 8 bit
// value)
SemanticOperand maskCount(Context& c, const SemanticOperand& count, uint bits)
{
    return c.emit(Op::OP_AND, integerType(8), count,
                  immediate(c, (bits == 64) ? 63 : 31, 8));
}

// Return the 8 bit count in the size of the operation
SemanticOperand widenCount(Context& c, const SemanticOperand& count, uint bits)
{
    if (bits == 8)
        return count;
    return c.emit(Op::OP_ZERO_EXTEND, integerType(bits), count);
}

// The end of a shift or rotate whose count is zero: nothing changes, flags
// included. But in 64 bit mode the write of a 32 bit register clears the
// upper half of the 64 bit register even then
void placeZeroCountLabel(Context& c, const SemanticOperand& skip,
                         const SemanticOperand& value)
{
    if (c.is64BitMode() && (value.getType().getTotalBits() == 32) &&
        (c.getOperand(0).m_kind == IA32DecodedOperand::KIND_REGISTER))
    {
        SemanticOperand done = c.newLabel();
        c.branch(done);
        c.placeLabel(skip);
        c.write(0, value);
        c.placeLabel(done);
        return;
    }
    c.placeLabel(skip);
}

// shl, shr, sar, rol, ror, rcl, rcr: nothing changes, flags included, when
// the count (masked to 5 bits, 6 for 64 bit operands) is zero
template <int KIND>
void liftShift(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerType(bits);
    SemanticOperand value = c.read(0);
    SemanticOperand count = maskCount(c, c.read(1), bits);
    SemanticOperand wideCount = widenCount(c, count, bits);
    SemanticOperand skip = c.newLabel();
    c.branchIf(compare(c, Op::COND_EQUAL, count, immediate(c, 0, 8)), skip);

    switch (KIND)
    {
    case SHIFT_LEFT:
    case SHIFT_RIGHT:
    case SHIFT_RIGHT_ARITHMETIC:
    {
        const Op::Operation operation = (KIND == SHIFT_LEFT) ? Op::OP_SHIFT_LEFT :
                (KIND == SHIFT_RIGHT) ? Op::OP_SHIFT_RIGHT_LOGICAL :
                                        Op::OP_SHIFT_RIGHT_ARITHMETIC;
        const Op::FlagRecipe recipe = (KIND == SHIFT_LEFT) ? Op::FLAGS_SHIFT_LEFT :
                (KIND == SHIFT_RIGHT) ? Op::FLAGS_SHIFT_RIGHT_LOGICAL :
                                        Op::FLAGS_SHIFT_RIGHT_ARITHMETIC;
        SemanticOperand result = c.emit(operation, type, value, wideCount);
        c.setFlags(recipe, FALL, type, value, wideCount, result);
        c.write(0, result);
        break;
    }
    case ROTATE_LEFT:
    case ROTATE_RIGHT:
    {
        SemanticOperand result = c.emit(KIND == ROTATE_LEFT ?
                        Op::OP_ROTATE_LEFT : Op::OP_ROTATE_RIGHT,
                        type, value, wideCount);
        c.setFlags(KIND == ROTATE_LEFT ? Op::FLAGS_ROTATE_LEFT :
                                         Op::FLAGS_ROTATE_RIGHT,
                   FC | FO, type, value, wideCount, result);
        c.write(0, result);
        break;
    }
    default:
    {
        if (bits == 64)
        {
            // The rotate of a 65 bit value is done with shifts
            const SemanticOperand carry = c.emit(Op::OP_ZERO_EXTEND, type,
                                                 c.getRegister(IA32_REG_CF));
            SemanticOperand complement = c.emit(Op::OP_SUBTRACT, type,
                                                immediate(c, 64, 64), wideCount);
            SemanticOperand result;
            SemanticOperand newCarry;
            if (KIND == ROTATE_LEFT_CARRY)
            {
                // (value << n) | (CF << (n - 1)) | (value >> (65 - n))
                SemanticOperand left = c.emit(Op::OP_SHIFT_LEFT, type, value, wideCount);
                SemanticOperand carryBit = c.emit(Op::OP_SHIFT_LEFT, type, carry,
                    c.emit(Op::OP_SUBTRACT, type, wideCount, immediate(c, 1, 64)));
                SemanticOperand right = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type,
                    c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, value, complement),
                    immediate(c, 1, 64));
                result = c.emit(Op::OP_OR, type, c.emit(Op::OP_OR, type, left, carryBit), right);
                newCarry = c.emit(Op::OP_TRUNCATE, integerType(1),
                    c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, value, complement));
                c.setFlags(FC, newCarry);
                c.setFlags(FO, c.emit(Op::OP_XOR, integerType(1),
                                      extractBit(c, result, 63), newCarry));
            } else
            {
                // (value >> n) | (CF << (64 - n)) | (value << (65 - n))
                SemanticOperand right = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, value, wideCount);
                SemanticOperand carryBit = c.emit(Op::OP_SHIFT_LEFT, type, carry, complement);
                SemanticOperand left = c.emit(Op::OP_SHIFT_LEFT, type,
                    c.emit(Op::OP_SHIFT_LEFT, type, value, complement),
                    immediate(c, 1, 64));
                result = c.emit(Op::OP_OR, type, c.emit(Op::OP_OR, type, right, carryBit), left);
                newCarry = c.emit(Op::OP_TRUNCATE, integerType(1),
                    c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, value,
                           c.emit(Op::OP_SUBTRACT, type, wideCount, immediate(c, 1, 64))));
                c.setFlags(FC, newCarry);
                c.setFlags(FO, c.emit(Op::OP_XOR, integerType(1),
                                      extractBit(c, value, 63), extractBit(c, result, 63)));
            }
            c.write(0, result);
            break;
        }
        // Through the carry: a rotate of the bits + 1 bit value, whose
        // lowest bit is the carry flag
        const SemanticType wide = integerType(bits + 1);
        SemanticOperand extended = c.emit(Op::OP_ZERO_EXTEND, wide, value);
        SemanticOperand shifted = c.emit(Op::OP_SHIFT_LEFT, wide, extended,
                                         immediate(c, 1, bits + 1));
        SemanticOperand carry = c.emit(Op::OP_ZERO_EXTEND, wide,
                                       c.getRegister(IA32_REG_CF));
        SemanticOperand combined = c.emit(Op::OP_OR, wide, shifted, carry);
        SemanticOperand rotateCount = c.emit(Op::OP_ZERO_EXTEND, wide, count);
        SemanticOperand rotated = c.emit(KIND == ROTATE_LEFT_CARRY ?
                        Op::OP_ROTATE_LEFT : Op::OP_ROTATE_RIGHT,
                        wide, combined, rotateCount);
        SemanticOperand result = c.emit(Op::OP_TRUNCATE, type,
                    c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, wide, rotated,
                           immediate(c, 1, bits + 1)));
        if (KIND == ROTATE_LEFT_CARRY)
        {
            c.setFlags(Op::FLAGS_ROTATE_LEFT, FC | FO, wide, combined,
                       rotateCount, rotated);
        } else
        {
            // The carry is the lowest bit, the overflow compares the sign
            // before and after
            c.setFlags(FC, c.emit(Op::OP_TRUNCATE, integerType(1), rotated));
            SemanticOperand oldSign = extractBit(c, value, bits - 1);
            SemanticOperand newSign = extractBit(c, result, bits - 1);
            c.setFlags(FO, c.emit(Op::OP_XOR, integerType(1), oldSign, newSign));
        }
        c.write(0, result);
        break;
    }
    }
    placeZeroCountLabel(c, skip, value);
}

// shld / shrd r/m, r, imm8|cl: the bits come from the second operand. For a 16
// bit operand the count can be up to 31, and the processors shift the 48 bit
// value dest:src:dest (the result is undefined in the manuals)
template <bool IS_RIGHT>
void liftDoubleShift(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerType(bits);
    SemanticOperand value = c.read(0);
    SemanticOperand filler = c.read(1);
    SemanticOperand count = maskCount(c, c.read(2), bits);
    SemanticOperand skip = c.newLabel();
    c.branchIf(compare(c, Op::COND_EQUAL, count, immediate(c, 0, 8)), skip);

    if (bits == 64)
    {
        // The shifts of a 128 bit value are done with 64 bit shifts (the
        // count is 1 to 63)
        SemanticOperand wideCount = c.emit(Op::OP_ZERO_EXTEND, type, count);
        SemanticOperand complement = c.emit(Op::OP_SUBTRACT, type,
                                            immediate(c, 64, 64), wideCount);
        SemanticOperand result;
        SemanticOperand carry;
        if (IS_RIGHT)
        {
            result = c.emit(Op::OP_OR, type,
                            c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, value, wideCount),
                            c.emit(Op::OP_SHIFT_LEFT, type, filler, complement));
            carry = c.emit(Op::OP_TRUNCATE, integerType(1),
                           c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, value,
                                  c.emit(Op::OP_SUBTRACT, type, wideCount,
                                         immediate(c, 1, 64))));
        } else
        {
            result = c.emit(Op::OP_OR, type,
                            c.emit(Op::OP_SHIFT_LEFT, type, value, wideCount),
                            c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, filler, complement));
            carry = c.emit(Op::OP_TRUNCATE, integerType(1),
                           c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, value, complement));
        }
        c.setFlags(FC, carry);
        c.setFlags(FO, c.emit(Op::OP_XOR, integerType(1),
                              extractBit(c, value, 63), extractBit(c, result, 63)));
        c.setFlags(Op::FLAGS_LOGIC, FZ | FS | FP, type, result, result, result);
        c.write(0, result);
        c.placeLabel(skip);
        return;
    }

    // The value which is shifted, from the high bits to the low bits: dest:src
    // for shld, src:dest for shrd, and for 16 bits dest:src:dest
    uint wideBits = 2 * bits;
    SemanticOperand combined;
    if (bits == 16)
    {
        wideBits = 48;
        combined = c.emit(Op::OP_CONCAT, integerType(wideBits),
                          c.emit(Op::OP_CONCAT, integerType(32), value, filler),
                          value);
    } else if (IS_RIGHT)
    {
        combined = c.emit(Op::OP_CONCAT, integerType(wideBits), value, filler);
    } else
    {
        combined = c.emit(Op::OP_CONCAT, integerType(wideBits), filler, value);
    }
    const SemanticType wide = integerType(wideBits);
    SemanticOperand wideCount = c.emit(Op::OP_ZERO_EXTEND, wide, count);

    SemanticOperand result;
    if (IS_RIGHT)
    {
        SemanticOperand shifted = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, wide,
                                         combined, wideCount);
        c.setFlags(Op::FLAGS_SHIFT_RIGHT_LOGICAL, FC, wide, combined,
                   wideCount, shifted);
        result = c.emit(Op::OP_TRUNCATE, type, shifted);
    } else
    {
        // The high half is the result
        SemanticOperand shifted = c.emit(Op::OP_SHIFT_LEFT, wide, combined,
                                         wideCount);
        c.setFlags(Op::FLAGS_SHIFT_LEFT, FC, wide, combined, wideCount, shifted);
        result = c.emit(Op::OP_EXTRACT, type, shifted,
                        immediate(c, wideBits - bits, 32));
    }
    // The overflow flag is set when the sign changed
    SemanticOperand oldSign = extractBit(c, value, bits - 1);
    SemanticOperand newSign = extractBit(c, result, bits - 1);
    c.setFlags(FO, c.emit(Op::OP_XOR, integerType(1), oldSign, newSign));
    c.setFlags(Op::FLAGS_LOGIC, FZ | FS | FP, type, result, result, result);
    c.write(0, result);
    placeZeroCountLabel(c, skip, value);
}

/////////////////////////////////////////////////////////////////////////////
// Bits
/////////////////////////////////////////////////////////////////////////////

enum BitTestKind {
    BIT_TEST,
    BIT_TEST_SET,
    BIT_TEST_RESET,
    BIT_TEST_COMPLEMENT
};

// bt, bts, btr, btc: the carry flag is the bit. A register offset addresses
// memory beyond the operand: the offset is signed and counts bits from the
// address
template <int KIND>
void liftBitTest(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerType(bits);
    const bool isMemory = (c.getOperand(0).m_kind == IA32DecodedOperand::KIND_MEMORY);
    const bool isImmediate = (c.getOperand(1).m_kind == IA32DecodedOperand::KIND_IMMEDIATE);
    const bool isLocked = IA32OperandDecoder::hasPrefix(c.getInstruction(), PREFIX_LOCK);

    // The bit inside the operand
    SemanticOperand offset = c.read(1, type);
    SemanticOperand index;
    if (isImmediate)
        index = immediate(c, c.getOperand(1).m_immediate & (bits - 1), bits);
    else
        index = c.emit(Op::OP_AND, type, offset, immediate(c, bits - 1, bits));
    SemanticOperand mask = c.emit(Op::OP_SHIFT_LEFT, type, immediate(c, 1, bits), index);

    SemanticOperand address;
    SemanticOperand value;
    if (isMemory)
    {
        if (isImmediate)
        {
            address = c.getAddress(0);
        } else
        {
            // The signed bit offset moves the address by whole operands
            const SemanticType addressType = c.getAddressSizeType();
            const uint addressBits = addressType.getTotalBits();
            SemanticOperand words = c.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, type,
                        offset, immediate(c, log2Of(bits), bits));
            SemanticOperand bytes = c.emit(Op::OP_SHIFT_LEFT, type, words,
                        immediate(c, log2Of(bits / 8), bits));
            if (addressBits > bits)
                bytes = c.emit(Op::OP_SIGN_EXTEND, addressType, bytes);
            else if (addressBits < bits)
                bytes = c.emit(Op::OP_TRUNCATE, addressType, bytes);
            SemanticOperand moved = c.emit(Op::OP_ADD, addressType,
                                           c.getOffset(0), bytes);
            address = c.getLinearAddress(c.getOperand(0).m_segment, moved);
        }
        if (isLocked && (KIND != BIT_TEST))
        {
            // The atomic operation returns the old value
            value = c.newTemporary(type);
            SemanticInstruction atomic(Op::OP_ATOMIC_RMW, type, value, address,
                                       mask);
            atomic.setCombineOperation(KIND == BIT_TEST_SET ? Op::OP_OR :
                                       KIND == BIT_TEST_RESET ? Op::OP_AND_NOT :
                                                                Op::OP_XOR);
            atomic.setMemoryOrder(Op::ORDER_SEQUENTIAL);
            atomic.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
            c.emit(atomic);
        } else
        {
            value = c.load(address, type);
        }
    } else
    {
        value = c.read(0);
    }

    SemanticOperand shifted = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, value, index);
    c.setFlags(FC, c.emit(Op::OP_TRUNCATE, integerType(1), shifted));
    if (KIND == BIT_TEST)
        return;
    if (isMemory && isLocked)
        return;

    SemanticOperand changed;
    if (KIND == BIT_TEST_SET)
        changed = c.emit(Op::OP_OR, type, value, mask);
    else if (KIND == BIT_TEST_RESET)
        changed = c.emit(Op::OP_AND_NOT, type, mask, value);
    else
        changed = c.emit(Op::OP_XOR, type, value, mask);
    if (isMemory)
        c.store(address, changed);
    else
        c.write(0, changed);
}

// bsf / bsr: the index of the lowest / highest set bit. A zero source sets
// the zero flag and leaves the destination
template <bool IS_REVERSE>
void liftBitScan(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerType(bits);
    SemanticOperand source = c.read(1);
    SemanticOperand destination = c.read(0);
    SemanticOperand isZero = compare(c, Op::COND_EQUAL, source,
                                     immediate(c, 0, bits));
    SemanticOperand index;
    if (IS_REVERSE)
        index = c.emit(Op::OP_SUBTRACT, type, immediate(c, bits - 1, bits),
                       c.emit(Op::OP_COUNT_LEADING_ZEROS, type, source));
    else
        index = c.emit(Op::OP_COUNT_TRAILING_ZEROS, type, source);
    c.setFlags(FZ, isZero);
    c.write(0, c.emit(Op::OP_SELECT, type, isZero, destination, index));
}

// popcnt: the number of set bits. The zero flag tells that the source is
// zero, the other flags are cleared
void liftPopulationCount(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    SemanticOperand source = c.read(1);
    SemanticOperand isZero = compare(c, Op::COND_EQUAL, source,
                                     immediate(c, 0, bits));
    SemanticOperand count = c.emit(Op::OP_POPULATION_COUNT,
                                   integerType(bits), source);
    c.setFlags(FALL & ~FZ, immediate(c, 0, 1));
    c.setFlags(FZ, isZero);
    c.write(0, count);
}

// lzcnt / tzcnt: the number of leading / trailing zero bits, the operand size
// for a zero source. The carry flag tells that the source is zero and the zero
// flag that the result is zero
template <bool IS_LEADING>
void liftZeroCount(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    SemanticOperand source = c.read(1);
    SemanticOperand isZero = compare(c, Op::COND_EQUAL, source,
                                     immediate(c, 0, bits));
    SemanticOperand count = c.emit(IS_LEADING ? Op::OP_COUNT_LEADING_ZEROS :
                                                Op::OP_COUNT_TRAILING_ZEROS,
                                   integerType(bits), source);
    SemanticOperand isCountZero = compare(c, Op::COND_EQUAL, count,
                                          immediate(c, 0, bits));
    c.setFlags(FC, isZero);
    c.setFlags(FZ, isCountZero);
    c.write(0, count);
}

/////////////////////////////////////////////////////////////////////////////
// Exchange and add, compare and exchange
/////////////////////////////////////////////////////////////////////////////

void liftXadd(Context& c)
{
    const SemanticType type = c.getOperandType(0);
    const bool isLocked = IA32OperandDecoder::hasPrefix(c.getInstruction(), PREFIX_LOCK);
    SemanticOperand addend = c.read(1);
    SemanticOperand old;
    if (isLocked)
    {
        old = c.newTemporary(type);
        SemanticInstruction atomic(Op::OP_ATOMIC_RMW, type, old,
                                   c.getAddress(0), addend);
        atomic.setCombineOperation(Op::OP_ADD);
        atomic.setMemoryOrder(Op::ORDER_SEQUENTIAL);
        atomic.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        c.emit(atomic);
    } else
    {
        old = c.read(0);
    }
    SemanticOperand sum = c.emit(Op::OP_ADD, type, old, addend);
    c.setFlags(Op::FLAGS_ADD, FALL, type, old, addend, sum);
    // The source gets the old value and the destination the sum (the sum
    // wins when it's the same register)
    c.write(1, snapshot(c, old));
    if (!isLocked)
        c.write(0, sum);
}

void liftCmpxchg(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerType(bits);
    const bool isLocked = IA32OperandDecoder::hasPrefix(c.getInstruction(), PREFIX_LOCK);
    SemanticOperand accumulator = c.getGeneralRegister(0, bits);
    SemanticOperand expected = snapshot(c, accumulator);
    SemanticOperand source = snapshot(c, c.read(1));

    SemanticOperand old;
    if (isLocked)
    {
        old = c.newTemporary(type);
        SemanticInstruction exchange(Op::OP_ATOMIC_COMPARE_EXCHANGE, type, old,
                                     c.getAddress(0), expected, source);
        exchange.setMemoryOrder(Op::ORDER_SEQUENTIAL);
        exchange.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        c.emit(exchange);
    } else
    {
        old = snapshot(c, c.read(0));
    }

    SemanticOperand difference = c.emit(Op::OP_SUBTRACT, type, expected, old);
    c.setFlags(Op::FLAGS_SUBTRACT, FALL, type, expected, old, difference);
    SemanticOperand isEqual = compare(c, Op::COND_EQUAL, expected, old);
    // The accumulator gets the old value, and the destination the source if
    // they were equal
    SemanticOperand newDestination = c.emit(Op::OP_SELECT, type, isEqual, source, old);
    c.move(accumulator, old);
    if (!isLocked)
        c.write(0, newDestination);
}

void liftCmpxchg8b(Context& c)
{
    const SemanticType type = integerType(64);
    const bool isLocked = IA32OperandDecoder::hasPrefix(c.getInstruction(), PREFIX_LOCK);
    SemanticOperand address = c.getAddress(0);
    SemanticOperand expected = c.emit(Op::OP_CONCAT, type, c.getRegister(IA32_REG_EAX),
                                      c.getRegister(IA32_REG_EDX));
    SemanticOperand source = c.emit(Op::OP_CONCAT, type, c.getRegister(IA32_REG_EBX),
                                    c.getRegister(IA32_REG_ECX));
    SemanticOperand old;
    if (isLocked)
    {
        old = c.newTemporary(type);
        SemanticInstruction exchange(Op::OP_ATOMIC_COMPARE_EXCHANGE, type, old,
                                     address, expected, source);
        exchange.setMemoryOrder(Op::ORDER_SEQUENTIAL);
        exchange.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        c.emit(exchange);
    } else
    {
        old = c.load(address, type);
    }
    SemanticOperand isEqual = compare(c, Op::COND_EQUAL, expected, old);
    if (!isLocked)
        c.store(address, c.emit(Op::OP_SELECT, type, isEqual, source, old));
    c.setFlags(FZ, isEqual);
    // The old value goes to edx:eax if they were different. In 64 bit mode
    // the write of the registers clears their upper half, so they are not
    // written if they were equal
    SemanticOperand skip;
    if (c.is64BitMode())
    {
        skip = c.newLabel();
        c.branchIf(isEqual, skip);
    }
    SemanticOperand low = c.emit(Op::OP_TRUNCATE, integerType(32), old);
    SemanticOperand high = c.emit(Op::OP_EXTRACT, integerType(32), old,
                                  immediate(c, 32, 32));
    c.move(c.getRegister(IA32_REG_EAX), c.emit(Op::OP_SELECT, integerType(32),
                isEqual, c.getRegister(IA32_REG_EAX), low));
    c.move(c.getRegister(IA32_REG_EDX), c.emit(Op::OP_SELECT, integerType(32),
                isEqual, c.getRegister(IA32_REG_EDX), high));
    if (c.is64BitMode())
        c.placeLabel(skip);
}

/////////////////////////////////////////////////////////////////////////////
// Decimal adjust
/////////////////////////////////////////////////////////////////////////////

// Return the 1 bit value: (low nibble of al > 9) or AF
SemanticOperand needsAdjust(Context& c, const SemanticOperand& al)
{
    SemanticOperand low = c.emit(Op::OP_AND, integerType(8), al, immediate(c, 0xF, 8));
    SemanticOperand isLarge = compare(c, Op::COND_UNSIGNED_GREATER, low, immediate(c, 9, 8));
    return c.emit(Op::OP_OR, integerType(1), isLarge, c.getRegister(IA32_REG_AF));
}

// daa and das: adjust al after an addition / subtraction of packed decimals
template <bool IS_SUBTRACT>
void liftDecimalAdjust(Context& c)
{
    const SemanticType type = integerType(8);
    const Op::Operation operation = IS_SUBTRACT ? Op::OP_SUBTRACT : Op::OP_ADD;
    SemanticOperand oldAl = snapshot(c, c.getRegister(IA32_REG_AL));
    SemanticOperand oldCarry = snapshot(c, c.getRegister(IA32_REG_CF));
    SemanticOperand adjustLow = needsAdjust(c, oldAl);
    SemanticOperand al = c.emit(Op::OP_SELECT, type, adjustLow,
                    c.emit(operation, type, oldAl, immediate(c, 6, 8)), oldAl);
    SemanticOperand adjustHigh = c.emit(Op::OP_OR, integerType(1),
        compare(c, Op::COND_UNSIGNED_GREATER, oldAl, immediate(c, 0x99, 8)), oldCarry);
    al = c.emit(Op::OP_SELECT, type, adjustHigh,
                c.emit(operation, type, al, immediate(c, 0x60, 8)), al);

    SemanticOperand carry = adjustHigh;
    if (IS_SUBTRACT)
    {
        // A borrow out of the first adjustment is a carry too
        SemanticOperand borrow = compare(c, Op::COND_UNSIGNED_LESS, oldAl,
                                         immediate(c, 6, 8));
        SemanticOperand first = c.emit(Op::OP_AND, integerType(1), adjustLow,
                    c.emit(Op::OP_OR, integerType(1), oldCarry, borrow));
        carry = c.emit(Op::OP_OR, integerType(1), adjustHigh, first);
    }
    c.setFlags(Op::FLAGS_LOGIC, FZ | FS | FP, type, al, al, al);
    c.setFlags(FA, adjustLow);
    c.setFlags(FC, carry);
    c.move(c.getRegister(IA32_REG_AL), al);
}

// aaa and aas: adjust al after an addition / subtraction of unpacked decimals
template <bool IS_SUBTRACT>
void liftAsciiAdjust(Context& c)
{
    const SemanticType type = integerType(16);
    SemanticOperand adjust = needsAdjust(c, c.getRegister(IA32_REG_AL));
    SemanticOperand ax = c.getRegister(IA32_REG_AX);
    SemanticOperand adjusted = c.emit(IS_SUBTRACT ? Op::OP_SUBTRACT : Op::OP_ADD,
                                      type, ax, immediate(c, 0x106, 16));
    SemanticOperand result = c.emit(Op::OP_SELECT, type, adjust, adjusted, ax);
    c.setFlags(FA, adjust);
    c.setFlags(FC, adjust);
    c.move(ax, c.emit(Op::OP_AND, type, result, immediate(c, 0xFF0F, 16)));
}

// aam imm8: ah = al / imm, al = al % imm
void liftAam(Context& c)
{
    const SemanticType type = integerType(8);
    const int64 divisor = c.getOperand(0).m_immediate;
    if (divisor == 0)
    {
        c.trap(Op::TRAP_FAULT, 0);
        return;
    }
    SemanticOperand al = c.getRegister(IA32_REG_AL);
    SemanticOperand divisorOperand = immediate(c, divisor, 8);
    SemanticOperand quotient = c.emit(Op::OP_DIVIDE_UNSIGNED, type, al, divisorOperand);
    SemanticOperand remainder = c.emit(Op::OP_REMAINDER_UNSIGNED, type, al, divisorOperand);
    c.setFlags(Op::FLAGS_LOGIC, FZ | FS | FP, type, remainder, remainder, remainder);
    c.move(c.getRegister(IA32_REG_AH), quotient);
    c.move(al, remainder);
}

// aad imm8: al = al + ah * imm, ah = 0
void liftAad(Context& c)
{
    const SemanticType type = integerType(8);
    SemanticOperand product = c.emit(Op::OP_MULTIPLY, type,
                    c.getRegister(IA32_REG_AH),
                    immediate(c, c.getOperand(0).m_immediate, 8));
    SemanticOperand al = c.emit(Op::OP_ADD, type, c.getRegister(IA32_REG_AL), product);
    c.setFlags(Op::FLAGS_LOGIC, FZ | FS | FP, type, al, al, al);
    c.move(c.getRegister(IA32_REG_AX), c.emit(Op::OP_ZERO_EXTEND, integerType(16), al));
}

/////////////////////////////////////////////////////////////////////////////
// The runtime helpers
/////////////////////////////////////////////////////////////////////////////

void liftCpuid(Context& c)
{
    c.emit(SemanticInstruction(Op::OP_HELPER, SemanticType(), SemanticOperand(),
                               immediate(c, IA32_HELPER_CPUID, 32)));
}

void liftRdtsc(Context& c)
{
    c.emit(SemanticInstruction(Op::OP_HELPER, SemanticType(), SemanticOperand(),
                               immediate(c, IA32_HELPER_RDTSC, 32)));
}

// in al|ax|eax, imm8|dx
void liftIn(Context& c)
{
    const SemanticType type = c.getOperandType(0);
    SemanticOperand result = c.newTemporary(type);
    c.emit(SemanticInstruction(Op::OP_HELPER, type, result,
                               immediate(c, IA32_HELPER_IN, 32), c.read(1),
                               immediate(c, type.getTotalBits() / 8, 32)));
    c.write(0, result);
}

// out imm8|dx, al|ax|eax
void liftOut(Context& c)
{
    const SemanticType type = c.getOperandType(1);
    c.emit(SemanticInstruction(Op::OP_HELPER, type, SemanticOperand(),
                               immediate(c, IA32_HELPER_OUT, 32), c.read(0),
                               c.read(1)));
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// The template steps
/////////////////////////////////////////////////////////////////////////////

namespace {

// The references of a step
#define NO_REF          { SemanticTemplateReference::KIND_NONE, 0, 0 }
#define OPERAND(n)      { SemanticTemplateReference::KIND_OPERAND, n, 0 }
#define TEMP(n)         { SemanticTemplateReference::KIND_TEMPORARY, n, 0 }
#define REG(id)         { SemanticTemplateReference::KIND_REGISTER, 0, id }
#define IMM(value)      { SemanticTemplateReference::KIND_IMMEDIATE, 0, value }

// The types of a step: the type of an operand, a boolean, a fixed integer
#define TYPE_OF(n)      { SemanticTemplateType::SOURCE_OPERAND, 0, n, 0 }
#define TYPE_BOOLEAN    { SemanticTemplateType::SOURCE_BOOLEAN, 0, 0, 0 }
#define TYPE_INT(bits)  { SemanticTemplateType::SOURCE_FIXED, \
                          SemanticType::KIND_INTEGER, bits, 1 }

// A step: destination = operation(source0, source1, source2)
#define STEP(operation, type, destination, source0, source1, source2) \
    { Op::operation, type, destination, { source0, source1, source2 }, \
      Op::COND_NONE, Op::FLAGS_NONE, 0 }
// A step which sets flags: the operands are the sources of the recipe
#define FLAGS_STEP(type, source0, source1, source2, recipe, mask) \
    { Op::OP_SET_FLAGS, type, NO_REF, { source0, source1, source2 }, \
      Op::COND_NONE, Op::recipe, mask }
// A step with a condition (OP_TEST_FLAGS, OP_COMPARE)
#define CONDITION_STEP(operation, type, destination, source0, source1, condition) \
    { Op::operation, type, destination, { source0, source1, NO_REF }, \
      condition, Op::FLAGS_NONE, 0 }

const uint8 FROM_OPCODE = SemanticTemplate::CONDITION_FROM_OPCODE;

// mov r/m, r | r, r/m | r/m, imm | r, imm | r/m, sreg
const SemanticTemplateStep gMoveSteps[] = {
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), OPERAND(1), NO_REF, NO_REF)
};

// movzx / movsx r, r/m
const SemanticTemplateStep gMoveZeroExtendSteps[] = {
    STEP(OP_ZERO_EXTEND, TYPE_OF(0), OPERAND(0), OPERAND(1), NO_REF, NO_REF)
};
const SemanticTemplateStep gMoveSignExtendSteps[] = {
    STEP(OP_SIGN_EXTEND, TYPE_OF(0), OPERAND(0), OPERAND(1), NO_REF, NO_REF)
};

// add: t0 = op0 + op1, the flags, op0 = t0
const SemanticTemplateStep gAddSteps[] = {
    STEP(OP_ADD, TYPE_OF(0), TEMP(0), OPERAND(0), OPERAND(1), NO_REF),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), OPERAND(1), TEMP(0), FLAGS_ADD, FALL),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};

// adc: the carry flag is added too
const SemanticTemplateStep gAddCarrySteps[] = {
    STEP(OP_ADD_CARRY, TYPE_OF(0), TEMP(0), OPERAND(0), OPERAND(1), REG(IA32_REG_CF)),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), OPERAND(1), TEMP(0), FLAGS_ADD, FALL),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};

// sub
const SemanticTemplateStep gSubtractSteps[] = {
    STEP(OP_SUBTRACT, TYPE_OF(0), TEMP(0), OPERAND(0), OPERAND(1), NO_REF),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), OPERAND(1), TEMP(0), FLAGS_SUBTRACT, FALL),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};

// sbb: the borrow is the carry flag
const SemanticTemplateStep gSubtractBorrowSteps[] = {
    STEP(OP_SUBTRACT_BORROW, TYPE_OF(0), TEMP(0), OPERAND(0), OPERAND(1), REG(IA32_REG_CF)),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), OPERAND(1), TEMP(0), FLAGS_SUBTRACT, FALL),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};

// cmp: sub without the result
const SemanticTemplateStep gCompareSteps[] = {
    STEP(OP_SUBTRACT, TYPE_OF(0), TEMP(0), OPERAND(0), OPERAND(1), NO_REF),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), OPERAND(1), TEMP(0), FLAGS_SUBTRACT, FALL)
};

// and / or / xor
const SemanticTemplateStep gAndSteps[] = {
    STEP(OP_AND, TYPE_OF(0), TEMP(0), OPERAND(0), OPERAND(1), NO_REF),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), OPERAND(1), TEMP(0), FLAGS_LOGIC, FALL),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};
const SemanticTemplateStep gOrSteps[] = {
    STEP(OP_OR, TYPE_OF(0), TEMP(0), OPERAND(0), OPERAND(1), NO_REF),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), OPERAND(1), TEMP(0), FLAGS_LOGIC, FALL),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};
const SemanticTemplateStep gXorSteps[] = {
    STEP(OP_XOR, TYPE_OF(0), TEMP(0), OPERAND(0), OPERAND(1), NO_REF),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), OPERAND(1), TEMP(0), FLAGS_LOGIC, FALL),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};

// test: and without the result
const SemanticTemplateStep gTestSteps[] = {
    STEP(OP_AND, TYPE_OF(0), TEMP(0), OPERAND(0), OPERAND(1), NO_REF),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), OPERAND(1), TEMP(0), FLAGS_LOGIC, FALL)
};

// inc / dec: add / sub of 1 which leaves the carry flag
const SemanticTemplateStep gIncrementSteps[] = {
    STEP(OP_ADD, TYPE_OF(0), TEMP(0), OPERAND(0), IMM(1), NO_REF),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), IMM(1), TEMP(0), FLAGS_ADD, FALL & ~FC),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};
const SemanticTemplateStep gDecrementSteps[] = {
    STEP(OP_SUBTRACT, TYPE_OF(0), TEMP(0), OPERAND(0), IMM(1), NO_REF),
    FLAGS_STEP(TYPE_OF(0), OPERAND(0), IMM(1), TEMP(0), FLAGS_SUBTRACT, FALL & ~FC),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};

// not: no flags. neg: 0 - op0
const SemanticTemplateStep gNotSteps[] = {
    STEP(OP_NOT, TYPE_OF(0), TEMP(0), OPERAND(0), NO_REF, NO_REF),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};
const SemanticTemplateStep gNegateSteps[] = {
    STEP(OP_SUBTRACT, TYPE_OF(0), TEMP(0), IMM(0), OPERAND(0), NO_REF),
    FLAGS_STEP(TYPE_OF(0), IMM(0), OPERAND(0), TEMP(0), FLAGS_SUBTRACT, FALL),
    STEP(OP_MOVE, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};

// bswap
const SemanticTemplateStep gByteSwapSteps[] = {
    STEP(OP_BYTE_SWAP, TYPE_OF(0), OPERAND(0), OPERAND(0), NO_REF, NO_REF)
};

// setcc r/m8: the condition is in the opcode
const SemanticTemplateStep gSetConditionSteps[] = {
    CONDITION_STEP(OP_TEST_FLAGS, TYPE_BOOLEAN, TEMP(0), NO_REF, NO_REF, FROM_OPCODE),
    STEP(OP_ZERO_EXTEND, TYPE_OF(0), OPERAND(0), TEMP(0), NO_REF, NO_REF)
};

// cmovcc r, r/m: the destination is always written, with its own value when
// the condition is false (a 32 bit destination of 64 bit mode is zero extended
// either way), and the source is always read
const SemanticTemplateStep gConditionalMoveSteps[] = {
    CONDITION_STEP(OP_TEST_FLAGS, TYPE_BOOLEAN, TEMP(0), NO_REF, NO_REF, FROM_OPCODE),
    STEP(OP_SELECT, TYPE_OF(0), OPERAND(0), TEMP(0), OPERAND(1), OPERAND(0))
};

// movbe r, m | m, r
const SemanticTemplateStep gMoveByteSwappedSteps[] = {
    STEP(OP_BYTE_SWAP, TYPE_OF(0), OPERAND(0), OPERAND(1), NO_REF, NO_REF)
};

// jcc rel: jump if the condition holds
const SemanticTemplateStep gJumpConditionSteps[] = {
    CONDITION_STEP(OP_TEST_FLAGS, TYPE_BOOLEAN, TEMP(0), NO_REF, NO_REF, FROM_OPCODE),
    STEP(OP_BRANCH_CONDITIONAL, TYPE_BOOLEAN, NO_REF, TEMP(0), OPERAND(0), NO_REF)
};

// nop
const SemanticTemplateStep gNopSteps[] = {
    STEP(OP_NOP, TYPE_BOOLEAN, NO_REF, NO_REF, NO_REF, NO_REF)
};

// clc, stc, cld, std: the flag is a constant. cmc inverts the carry
const SemanticTemplateStep gClearCarrySteps[] = {
    FLAGS_STEP(TYPE_INT(1), IMM(0), NO_REF, NO_REF, FLAGS_EXPLICIT, SemanticFlags::FLAG_CARRY)
};
const SemanticTemplateStep gSetCarrySteps[] = {
    FLAGS_STEP(TYPE_INT(1), IMM(1), NO_REF, NO_REF, FLAGS_EXPLICIT, SemanticFlags::FLAG_CARRY)
};
const SemanticTemplateStep gComplementCarrySteps[] = {
    CONDITION_STEP(OP_TEST_FLAGS, TYPE_BOOLEAN, TEMP(0), NO_REF, NO_REF, Op::COND_NOT_CARRY),
    FLAGS_STEP(TYPE_INT(1), TEMP(0), NO_REF, NO_REF, FLAGS_EXPLICIT, SemanticFlags::FLAG_CARRY)
};
const SemanticTemplateStep gClearDirectionSteps[] = {
    FLAGS_STEP(TYPE_INT(1), IMM(0), NO_REF, NO_REF, FLAGS_EXPLICIT, SemanticFlags::FLAG_DIRECTION)
};
const SemanticTemplateStep gSetDirectionSteps[] = {
    FLAGS_STEP(TYPE_INT(1), IMM(1), NO_REF, NO_REF, FLAGS_EXPLICIT, SemanticFlags::FLAG_DIRECTION)
};

// cli, sti: the interrupt flag
const SemanticTemplateStep gClearInterruptSteps[] = {
    STEP(OP_MOVE, TYPE_INT(1), REG(IA32_REG_IF), IMM(0), NO_REF, NO_REF)
};
const SemanticTemplateStep gSetInterruptSteps[] = {
    STEP(OP_MOVE, TYPE_INT(1), REG(IA32_REG_IF), IMM(1), NO_REF, NO_REF)
};

#undef STEP
#undef FLAGS_STEP
#undef CONDITION_STEP

/////////////////////////////////////////////////////////////////////////////
// The classification shortcuts of the table
/////////////////////////////////////////////////////////////////////////////

// The categories
const uint8 CAT_MOVE        = Cl::CATEGORY_MOVE;
const uint8 CAT_NOP         = Cl::CATEGORY_NOP;
const uint8 CAT_STACK       = Cl::CATEGORY_STACK;
const uint8 CAT_ARITHMETIC  = Cl::CATEGORY_ARITHMETIC;
const uint8 CAT_LOGIC       = Cl::CATEGORY_LOGIC;
const uint8 CAT_SHIFT       = Cl::CATEGORY_SHIFT;
const uint8 CAT_MULTIPLY    = Cl::CATEGORY_MULTIPLY;
const uint8 CAT_DIVIDE      = Cl::CATEGORY_DIVIDE;
const uint8 CAT_BITS        = Cl::CATEGORY_BIT_MANIPULATION;
const uint8 CAT_COMPARE     = Cl::CATEGORY_COMPARE;
const uint8 CAT_CONDITIONAL = Cl::CATEGORY_CONDITIONAL_MOVE;
const uint8 CAT_CONVERT     = Cl::CATEGORY_CONVERT;
const uint8 CAT_BRANCH      = Cl::CATEGORY_BRANCH;
const uint8 CAT_CALL        = Cl::CATEGORY_CALL;
const uint8 CAT_RETURN      = Cl::CATEGORY_RETURN;
const uint8 CAT_STRING      = Cl::CATEGORY_STRING;
const uint8 CAT_ATOMIC      = Cl::CATEGORY_ATOMIC;
const uint8 CAT_INTERRUPT   = Cl::CATEGORY_INTERRUPT;
const uint8 CAT_IO          = Cl::CATEGORY_IO;
const uint8 CAT_SYSTEM      = Cl::CATEGORY_SYSTEM;

// The execution units
const uint32 U_NONE   = Cl::UNIT_NONE;
const uint32 U_ALU    = Cl::UNIT_INTEGER_ALU;
const uint32 U_SHIFT  = Cl::UNIT_SHIFTER;
const uint32 U_MUL    = Cl::UNIT_INTEGER_MULTIPLY;
const uint32 U_DIV    = Cl::UNIT_INTEGER_DIVIDE;
const uint32 U_BRANCH = Cl::UNIT_BRANCH;
const uint32 U_MEMORY = Cl::UNIT_LOAD | Cl::UNIT_STORE;
const uint32 U_SYSTEM = Cl::UNIT_SYSTEM;

// The properties. The flags properties follow the flags sets
const uint32 P_NONE     = Cl::PROPERTY_NONE;
const uint32 P_READ     = Cl::PROPERTY_READS_MEMORY;
const uint32 P_WRITE    = Cl::PROPERTY_WRITES_MEMORY;
const uint32 P_READ_WRITE = Cl::PROPERTY_READS_MEMORY | Cl::PROPERTY_WRITES_MEMORY;
const uint32 P_STACK    = Cl::PROPERTY_CHANGES_STACK;
const uint32 P_CONDITIONAL = Cl::PROPERTY_CONDITIONAL;
const uint32 P_REPEATED = Cl::PROPERTY_REPEATED;
const uint32 P_ATOMIC   = Cl::PROPERTY_ATOMIC;
const uint32 P_IMPLICIT = Cl::PROPERTY_IMPLICIT_OPERANDS;
const uint32 P_HELPER   = Cl::PROPERTY_NEEDS_HELPER;

} // namespace

/////////////////////////////////////////////////////////////////////////////
// The table
/////////////////////////////////////////////////////////////////////////////

namespace ia32dis {

// An entry of a template: the steps, the classification and the flags
// read / written / undefined
#define TEMPLATE(name, steps, category, units, properties, read, written, undefined) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, steps, sizeof(steps) / sizeof(steps[0]), category, units, \
        properties, read, written, undefined }, NULL }

// An entry of a hand written lifter
#define LIFTER(name, lift, category, units, properties, read, written, undefined) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, category, units, properties, read, written, undefined }, \
      lift }

// The same, for the mnemonic whose forms differ by the number of operands
#define LIFTER_OPERANDS(count, name, lift, category, units, properties, read, written, undefined) \
    { count, \
      { name, NULL, 0, category, units, properties, read, written, undefined }, \
      lift }

// The conditional jumps and sets: the flags the condition reads
#define JUMP(name, flags) \
    TEMPLATE(name, gJumpConditionSteps, CAT_BRANCH, U_BRANCH, P_CONDITIONAL, flags, 0, 0)
#define SET(name, flags) \
    TEMPLATE(name, gSetConditionSteps, CAT_CONDITIONAL, U_ALU | U_MEMORY, \
             P_CONDITIONAL | P_WRITE, flags, 0, 0)
#define CMOV(name, flags) \
    TEMPLATE(name, gConditionalMoveSteps, CAT_CONDITIONAL, U_ALU | U_MEMORY, \
             P_CONDITIONAL | P_READ, flags, 0, 0)

// The bits and shifts: the count and the sizes of the other mnemonics
#define SHIFT(name, kind, read, written, undefined) \
    LIFTER(name, liftShift<kind>, CAT_SHIFT, U_SHIFT | U_MEMORY, P_READ_WRITE, \
           read, written, undefined)

const IA32SemanticEntry gIa32SemanticTable[] = {
    //////////////////////////////////////////////////////////////////////////
    // Data movement
    //////////////////////////////////////////////////////////////////////////
    // Name       Steps / lifter        Category      Units            Properties   Flags read  written   undefined
    TEMPLATE("mov",    gMoveSteps,           CAT_MOVE,    U_ALU | U_MEMORY, P_READ_WRITE, 0, 0, 0),
    TEMPLATE("movnti", gMoveSteps,           CAT_MOVE,    U_ALU | U_MEMORY, P_WRITE,      0, 0, 0),
    TEMPLATE("movzx",  gMoveZeroExtendSteps, CAT_CONVERT, U_ALU | U_MEMORY, P_READ,       0, 0, 0),
    TEMPLATE("movsx",  gMoveSignExtendSteps, CAT_CONVERT, U_ALU | U_MEMORY, P_READ,       0, 0, 0),
    LIFTER("lea",      liftLea,              CAT_MOVE,    U_ALU,            P_NONE,       0, 0, 0),
    LIFTER("xchg",     liftXchg,             CAT_ATOMIC,  U_ALU | U_MEMORY, P_READ_WRITE | P_ATOMIC, 0, 0, 0),
    TEMPLATE("bswap",  gByteSwapSteps,       CAT_BITS,    U_ALU,            P_NONE,       0, 0, 0),
    LIFTER("cbw/cwde", liftConvertAccumulator, CAT_CONVERT, U_ALU,          P_IMPLICIT,   0, 0, 0),
    LIFTER("cwd/cdq",  liftConvertAccumulatorToData, CAT_CONVERT, U_ALU,    P_IMPLICIT,   0, 0, 0),
    LIFTER("xlatb",    liftXlat,             CAT_MOVE,    U_ALU | U_MEMORY, P_READ | P_IMPLICIT, 0, 0, 0),
    TEMPLATE("movbe",  gMoveByteSwappedSteps, CAT_MOVE,   U_ALU | U_MEMORY, P_READ_WRITE, 0, 0, 0),
    TEMPLATE("nop",    gNopSteps,            CAT_NOP,     U_NONE,           P_NONE,       0, 0, 0),
    TEMPLATE("pause",  gNopSteps,            CAT_NOP,     U_NONE,           P_NONE,       0, 0, 0),
    TEMPLATE("endbr32", gNopSteps,           CAT_NOP,     U_NONE,           P_NONE,       0, 0, 0),
    TEMPLATE("endbr64", gNopSteps,           CAT_NOP,     U_NONE,           P_NONE,       0, 0, 0),

    //////////////////////////////////////////////////////////////////////////
    // Arithmetic and logic
    //////////////////////////////////////////////////////////////////////////
    TEMPLATE("add",  gAddSteps,           CAT_ARITHMETIC, U_ALU | U_MEMORY, P_READ_WRITE, 0,  FALL, 0),
    TEMPLATE("adc",  gAddCarrySteps,      CAT_ARITHMETIC, U_ALU | U_MEMORY, P_READ_WRITE, FC, FALL, 0),
    TEMPLATE("sub",  gSubtractSteps,      CAT_ARITHMETIC, U_ALU | U_MEMORY, P_READ_WRITE, 0,  FALL, 0),
    TEMPLATE("sbb",  gSubtractBorrowSteps, CAT_ARITHMETIC, U_ALU | U_MEMORY, P_READ_WRITE, FC, FALL, 0),
    TEMPLATE("cmp",  gCompareSteps,       CAT_COMPARE,    U_ALU | U_MEMORY, P_READ,       0,  FALL, 0),
    TEMPLATE("and",  gAndSteps,           CAT_LOGIC,      U_ALU | U_MEMORY, P_READ_WRITE, 0,  FALL, FA),
    TEMPLATE("or",   gOrSteps,            CAT_LOGIC,      U_ALU | U_MEMORY, P_READ_WRITE, 0,  FALL, FA),
    TEMPLATE("xor",  gXorSteps,           CAT_LOGIC,      U_ALU | U_MEMORY, P_READ_WRITE, 0,  FALL, FA),
    TEMPLATE("test", gTestSteps,          CAT_COMPARE,    U_ALU | U_MEMORY, P_READ,       0,  FALL, FA),
    TEMPLATE("inc",  gIncrementSteps,     CAT_ARITHMETIC, U_ALU | U_MEMORY, P_READ_WRITE, 0,  FALL & ~FC, 0),
    TEMPLATE("dec",  gDecrementSteps,     CAT_ARITHMETIC, U_ALU | U_MEMORY, P_READ_WRITE, 0,  FALL & ~FC, 0),
    TEMPLATE("not",  gNotSteps,           CAT_LOGIC,      U_ALU | U_MEMORY, P_READ_WRITE, 0,  0,    0),
    TEMPLATE("neg",  gNegateSteps,        CAT_ARITHMETIC, U_ALU | U_MEMORY, P_READ_WRITE, 0,  FALL, 0),
    LIFTER("mul",    liftMultiplyOneOperand<false>, CAT_MULTIPLY, U_MUL | U_MEMORY, P_READ | P_IMPLICIT, 0, FALL, FZ | FS | FA | FP),
    LIFTER_OPERANDS(1, "imul", liftMultiplyOneOperand<true>, CAT_MULTIPLY, U_MUL | U_MEMORY, P_READ | P_IMPLICIT, 0, FALL, FZ | FS | FA | FP),
    LIFTER_OPERANDS(2, "imul", liftImulTruncated, CAT_MULTIPLY, U_MUL | U_MEMORY, P_READ, 0, FALL, FZ | FS | FA | FP),
    LIFTER_OPERANDS(3, "imul", liftImulTruncated, CAT_MULTIPLY, U_MUL | U_MEMORY, P_READ, 0, FALL, FZ | FS | FA | FP),
    LIFTER("div",    liftDivide<false>,   CAT_DIVIDE,     U_DIV | U_MEMORY, P_READ | P_IMPLICIT, 0, FALL, FALL),
    LIFTER("idiv",   liftDivide<true>,    CAT_DIVIDE,     U_DIV | U_MEMORY, P_READ | P_IMPLICIT, 0, FALL, FALL),

    //////////////////////////////////////////////////////////////////////////
    // Shifts and rotates
    //////////////////////////////////////////////////////////////////////////
    SHIFT("shl", SHIFT_LEFT,             0,  FALL, FA | FO),
    SHIFT("sal", SHIFT_LEFT,             0,  FALL, FA | FO),
    SHIFT("shr", SHIFT_RIGHT,            0,  FALL, FA | FO),
    SHIFT("sar", SHIFT_RIGHT_ARITHMETIC, 0,  FALL, FA | FO),
    SHIFT("rol", ROTATE_LEFT,            0,  FC | FO, FO),
    SHIFT("ror", ROTATE_RIGHT,           0,  FC | FO, FO),
    SHIFT("rcl", ROTATE_LEFT_CARRY,      FC, FC | FO, FO),
    SHIFT("rcr", ROTATE_RIGHT_CARRY,     FC, FC | FO, FO),
    LIFTER("shld", liftDoubleShift<false>, CAT_SHIFT, U_SHIFT | U_MEMORY, P_READ_WRITE, 0, FALL, FA | FO),
    LIFTER("shrd", liftDoubleShift<true>,  CAT_SHIFT, U_SHIFT | U_MEMORY, P_READ_WRITE, 0, FALL, FA | FO),

    //////////////////////////////////////////////////////////////////////////
    // Bits
    //////////////////////////////////////////////////////////////////////////
    LIFTER("bt",  liftBitTest<BIT_TEST>,            CAT_BITS, U_ALU | U_MEMORY, P_READ,       0, FALL & ~FZ, FO | FS | FA | FP),
    LIFTER("bts", liftBitTest<BIT_TEST_SET>,        CAT_BITS, U_ALU | U_MEMORY, P_READ_WRITE, 0, FALL & ~FZ, FO | FS | FA | FP),
    LIFTER("btr", liftBitTest<BIT_TEST_RESET>,      CAT_BITS, U_ALU | U_MEMORY, P_READ_WRITE, 0, FALL & ~FZ, FO | FS | FA | FP),
    LIFTER("btc", liftBitTest<BIT_TEST_COMPLEMENT>, CAT_BITS, U_ALU | U_MEMORY, P_READ_WRITE, 0, FALL & ~FZ, FO | FS | FA | FP),
    LIFTER("bsf", liftBitScan<false>, CAT_BITS, U_ALU | U_MEMORY, P_READ, 0, FALL, FALL & ~FZ),
    LIFTER("bsr", liftBitScan<true>,  CAT_BITS, U_ALU | U_MEMORY, P_READ, 0, FALL, FALL & ~FZ),
    LIFTER("popcnt", liftPopulationCount, CAT_BITS, U_ALU | U_MEMORY, P_READ, 0, FALL, 0),
    LIFTER("lzcnt", liftZeroCount<true>,  CAT_BITS, U_ALU | U_MEMORY, P_READ, 0, FALL, FALL & ~(FC | FZ)),
    LIFTER("tzcnt", liftZeroCount<false>, CAT_BITS, U_ALU | U_MEMORY, P_READ, 0, FALL, FALL & ~(FC | FZ)),

    //////////////////////////////////////////////////////////////////////////
    // Atomic read-modify-write
    //////////////////////////////////////////////////////////////////////////
    LIFTER("xadd",      liftXadd,      CAT_ATOMIC, U_ALU | U_MEMORY, P_READ_WRITE, 0, FALL, 0),
    LIFTER("cmpxchg",   liftCmpxchg,   CAT_ATOMIC, U_ALU | U_MEMORY, P_READ_WRITE | P_IMPLICIT, 0, FALL, 0),
    LIFTER("cmpxchg8b", liftCmpxchg8b, CAT_ATOMIC, U_ALU | U_MEMORY, P_READ_WRITE | P_IMPLICIT, 0, FZ, 0),

    //////////////////////////////////////////////////////////////////////////
    // Flags
    //////////////////////////////////////////////////////////////////////////
    TEMPLATE("clc", gClearCarrySteps,      CAT_MOVE, U_ALU, P_NONE, 0,  FC, 0),
    TEMPLATE("stc", gSetCarrySteps,        CAT_MOVE, U_ALU, P_NONE, 0,  FC, 0),
    TEMPLATE("cmc", gComplementCarrySteps, CAT_MOVE, U_ALU, P_NONE, FC, FC, 0),
    TEMPLATE("cld", gClearDirectionSteps,  CAT_MOVE, U_ALU, P_NONE, 0,  FD, 0),
    TEMPLATE("std", gSetDirectionSteps,    CAT_MOVE, U_ALU, P_NONE, 0,  FD, 0),
    TEMPLATE("cli", gClearInterruptSteps,  CAT_SYSTEM, U_SYSTEM, P_NONE, 0, 0, 0),
    TEMPLATE("sti", gSetInterruptSteps,    CAT_SYSTEM, U_SYSTEM, P_NONE, 0, 0, 0),
    LIFTER("lahf",  liftLahf, CAT_MOVE, U_ALU, P_IMPLICIT, FC | FP | FA | FZ | FS, 0, 0),
    LIFTER("sahf",  liftSahf, CAT_MOVE, U_ALU, P_IMPLICIT, 0, FC | FP | FA | FZ | FS, 0),

    //////////////////////////////////////////////////////////////////////////
    // The stack
    //////////////////////////////////////////////////////////////////////////
    LIFTER("push",     liftPush,  CAT_STACK, U_ALU | U_MEMORY, P_STACK | P_WRITE | P_READ, 0, 0, 0),
    LIFTER("pop",      liftPop,   CAT_STACK, U_ALU | U_MEMORY, P_STACK | P_READ | P_WRITE, 0, 0, 0),
    LIFTER("pusha#d",  liftPusha, CAT_STACK, U_ALU | U_MEMORY, P_STACK | P_WRITE | P_IMPLICIT, 0, 0, 0),
    LIFTER("popa#d",   liftPopa,  CAT_STACK, U_ALU | U_MEMORY, P_STACK | P_READ | P_IMPLICIT, 0, 0, 0),
    LIFTER("pushf#d",  liftPushf, CAT_STACK, U_ALU | U_MEMORY, P_STACK | P_WRITE | P_IMPLICIT, FALL | FD, 0, 0),
    LIFTER("popf#d",   liftPopf,  CAT_STACK, U_ALU | U_MEMORY, P_STACK | P_READ | P_IMPLICIT, 0, FALL | FD, 0),
    LIFTER("enter",    liftEnter, CAT_STACK, U_ALU | U_MEMORY, P_STACK | P_WRITE | P_IMPLICIT, 0, 0, 0),
    LIFTER("leave",    liftLeave, CAT_STACK, U_ALU | U_MEMORY, P_STACK | P_READ | P_IMPLICIT, 0, 0, 0),

    //////////////////////////////////////////////////////////////////////////
    // Control flow
    //////////////////////////////////////////////////////////////////////////
    LIFTER("jmp",    liftJmp,  CAT_BRANCH, U_BRANCH | U_MEMORY, P_READ, 0, 0, 0),
    LIFTER("call",   liftCall, CAT_CALL,   U_BRANCH | U_MEMORY, P_STACK | P_WRITE | P_READ | P_IMPLICIT, 0, 0, 0),
    LIFTER("ret",    liftRet,  CAT_RETURN, U_BRANCH | U_MEMORY, P_STACK | P_READ | P_IMPLICIT, 0, 0, 0),
    LIFTER("loop",   liftLoop<LOOP_ALWAYS>,         CAT_BRANCH, U_BRANCH, P_CONDITIONAL | P_IMPLICIT, 0, 0, 0),
    LIFTER("loopz",  liftLoop<LOOP_WHILE_EQUAL>,    CAT_BRANCH, U_BRANCH, P_CONDITIONAL | P_IMPLICIT, FZ, 0, 0),
    LIFTER("loope",  liftLoop<LOOP_WHILE_EQUAL>,    CAT_BRANCH, U_BRANCH, P_CONDITIONAL | P_IMPLICIT, FZ, 0, 0),
    LIFTER("loopnz", liftLoop<LOOP_WHILE_NOT_EQUAL>, CAT_BRANCH, U_BRANCH, P_CONDITIONAL | P_IMPLICIT, FZ, 0, 0),
    LIFTER("loopne", liftLoop<LOOP_WHILE_NOT_EQUAL>, CAT_BRANCH, U_BRANCH, P_CONDITIONAL | P_IMPLICIT, FZ, 0, 0),
    LIFTER("j#ecxz", liftLoop<LOOP_IF_ZERO>,        CAT_BRANCH, U_BRANCH, P_CONDITIONAL | P_IMPLICIT, 0, 0, 0),
    JUMP("jo",   FO),
    JUMP("jno",  FO),
    JUMP("jb",   FC),
    JUMP("jc",   FC),
    JUMP("jnae", FC),
    JUMP("jnb",  FC),
    JUMP("jnc",  FC),
    JUMP("jae",  FC),
    JUMP("jz",   FZ),
    JUMP("je",   FZ),
    JUMP("jnz",  FZ),
    JUMP("jne",  FZ),
    JUMP("jbe",  FC | FZ),
    JUMP("jna",  FC | FZ),
    JUMP("ja",   FC | FZ),
    JUMP("jnbe", FC | FZ),
    JUMP("js",   FS),
    JUMP("jns",  FS),
    JUMP("jp",   FP),
    JUMP("jpe",  FP),
    JUMP("jnp",  FP),
    JUMP("jpo",  FP),
    JUMP("jl",   FS | FO),
    JUMP("jnge", FS | FO),
    JUMP("jnl",  FS | FO),
    JUMP("jge",  FS | FO),
    JUMP("jng",  FZ | FS | FO),
    JUMP("jle",  FZ | FS | FO),
    JUMP("jg",   FZ | FS | FO),
    JUMP("jnle", FZ | FS | FO),
    SET("seto",   FO),
    SET("setno",  FO),
    SET("setb",   FC),
    SET("setc",   FC),
    SET("setnae", FC),
    SET("setae",  FC),
    SET("setnb",  FC),
    SET("setnc",  FC),
    SET("setz",   FZ),
    SET("sete",   FZ),
    SET("setnz",  FZ),
    SET("setne",  FZ),
    SET("setna",  FC | FZ),
    SET("setbe",  FC | FZ),
    SET("seta",   FC | FZ),
    SET("setnbe", FC | FZ),
    SET("sets",   FS),
    SET("setns",  FS),
    SET("setp",   FP),
    SET("setpe",  FP),
    SET("setnp",  FP),
    SET("setpo",  FP),
    SET("setl",   FS | FO),
    SET("setnge", FS | FO),
    SET("setnl",  FS | FO),
    SET("setge",  FS | FO),
    SET("setng",  FZ | FS | FO),
    SET("setle",  FZ | FS | FO),
    SET("setg",   FZ | FS | FO),
    SET("setnle", FZ | FS | FO),
    CMOV("cmovo",   FO),
    CMOV("cmovno",  FO),
    CMOV("cmovb",   FC),
    CMOV("cmovc",   FC),
    CMOV("cmovnae", FC),
    CMOV("cmovae",  FC),
    CMOV("cmovnb",  FC),
    CMOV("cmovnc",  FC),
    CMOV("cmovz",   FZ),
    CMOV("cmove",   FZ),
    CMOV("cmovnz",  FZ),
    CMOV("cmovne",  FZ),
    CMOV("cmovna",  FC | FZ),
    CMOV("cmovbe",  FC | FZ),
    CMOV("cmova",   FC | FZ),
    CMOV("cmovnbe", FC | FZ),
    CMOV("cmovs",   FS),
    CMOV("cmovns",  FS),
    CMOV("cmovp",   FP),
    CMOV("cmovpe",  FP),
    CMOV("cmovnp",  FP),
    CMOV("cmovpo",  FP),
    CMOV("cmovl",   FS | FO),
    CMOV("cmovnge", FS | FO),
    CMOV("cmovnl",  FS | FO),
    CMOV("cmovge",  FS | FO),
    CMOV("cmovng",  FZ | FS | FO),
    CMOV("cmovle",  FZ | FS | FO),
    CMOV("cmovg",   FZ | FS | FO),
    CMOV("cmovnle", FZ | FS | FO),
    LIFTER("int",  liftInt,  CAT_INTERRUPT, U_BRANCH, P_NONE, 0,  0, 0),
    LIFTER("int3", liftInt3, CAT_INTERRUPT, U_BRANCH, P_NONE, 0,  0, 0),
    LIFTER("int1", liftInt1, CAT_INTERRUPT, U_BRANCH, P_NONE, 0,  0, 0),
    LIFTER("ud0",  liftUndefinedInstruction, CAT_INTERRUPT, U_BRANCH, P_NONE, 0, 0, 0),
    LIFTER("ud1",  liftUndefinedInstruction, CAT_INTERRUPT, U_BRANCH, P_NONE, 0, 0, 0),
    LIFTER("ud2",  liftUndefinedInstruction, CAT_INTERRUPT, U_BRANCH, P_NONE, 0, 0, 0),
    LIFTER("salc", liftSalc, CAT_MOVE, U_ALU, P_IMPLICIT, FC, 0, 0),
    LIFTER("into", liftInto, CAT_INTERRUPT, U_BRANCH, P_CONDITIONAL, FO, 0, 0),
    LIFTER("bound", liftBound, CAT_COMPARE, U_ALU | U_MEMORY, P_READ, 0, 0, 0),

    //////////////////////////////////////////////////////////////////////////
    // The string instructions
    //////////////////////////////////////////////////////////////////////////
    LIFTER("movsb",  liftString<STRING_MOVS>, CAT_STRING, U_ALU | U_MEMORY, P_READ | P_WRITE | P_REPEATED | P_IMPLICIT, FD, 0, 0),
    LIFTER("movs##", liftString<STRING_MOVS>, CAT_STRING, U_ALU | U_MEMORY, P_READ | P_WRITE | P_REPEATED | P_IMPLICIT, FD, 0, 0),
    LIFTER("cmpsb",  liftString<STRING_CMPS>, CAT_STRING, U_ALU | U_MEMORY, P_READ | P_REPEATED | P_IMPLICIT, FD | FZ, FALL, 0),
    LIFTER("cmps##", liftString<STRING_CMPS>, CAT_STRING, U_ALU | U_MEMORY, P_READ | P_REPEATED | P_IMPLICIT, FD | FZ, FALL, 0),
    LIFTER("stosb",  liftString<STRING_STOS>, CAT_STRING, U_ALU | U_MEMORY, P_WRITE | P_REPEATED | P_IMPLICIT, FD, 0, 0),
    LIFTER("stos##", liftString<STRING_STOS>, CAT_STRING, U_ALU | U_MEMORY, P_WRITE | P_REPEATED | P_IMPLICIT, FD, 0, 0),
    LIFTER("lodsb",  liftString<STRING_LODS>, CAT_STRING, U_ALU | U_MEMORY, P_READ | P_REPEATED | P_IMPLICIT, FD, 0, 0),
    LIFTER("lods##", liftString<STRING_LODS>, CAT_STRING, U_ALU | U_MEMORY, P_READ | P_REPEATED | P_IMPLICIT, FD, 0, 0),
    LIFTER("scasb",  liftString<STRING_SCAS>, CAT_STRING, U_ALU | U_MEMORY, P_READ | P_REPEATED | P_IMPLICIT, FD | FZ, FALL, 0),
    LIFTER("scas##", liftString<STRING_SCAS>, CAT_STRING, U_ALU | U_MEMORY, P_READ | P_REPEATED | P_IMPLICIT, FD | FZ, FALL, 0),

    //////////////////////////////////////////////////////////////////////////
    // Decimal adjust
    //////////////////////////////////////////////////////////////////////////
    LIFTER("daa", liftDecimalAdjust<false>, CAT_ARITHMETIC, U_ALU, P_IMPLICIT, FC | FA, FALL, FO),
    LIFTER("das", liftDecimalAdjust<true>,  CAT_ARITHMETIC, U_ALU, P_IMPLICIT, FC | FA, FALL, FO),
    LIFTER("aaa", liftAsciiAdjust<false>,   CAT_ARITHMETIC, U_ALU, P_IMPLICIT, FA, FALL, FO | FS | FZ | FP),
    LIFTER("aas", liftAsciiAdjust<true>,    CAT_ARITHMETIC, U_ALU, P_IMPLICIT, FA, FALL, FO | FS | FZ | FP),
    LIFTER("aam", liftAam, CAT_DIVIDE,   U_DIV, P_IMPLICIT, 0, FALL, FC | FA | FO),
    LIFTER("aad", liftAad, CAT_MULTIPLY, U_MUL, P_IMPLICIT, 0, FALL, FC | FA | FO),

    //////////////////////////////////////////////////////////////////////////
    // The runtime helpers
    //////////////////////////////////////////////////////////////////////////
    LIFTER("cpuid", liftCpuid, CAT_SYSTEM, U_SYSTEM, P_HELPER | P_IMPLICIT, 0, 0, 0),
    LIFTER("rdtsc", liftRdtsc, CAT_SYSTEM, U_SYSTEM, P_HELPER | P_IMPLICIT, 0, 0, 0),
    LIFTER("in",    liftIn,    CAT_IO,     U_SYSTEM, P_HELPER, 0, 0, 0),
    LIFTER("out",   liftOut,   CAT_IO,     U_SYSTEM, P_HELPER, 0, 0, 0),

    // End marker
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }, NULL }
};

#undef TEMPLATE
#undef LIFTER
#undef LIFTER_OPERANDS
#undef JUMP
#undef SET
#undef CMOV
#undef SHIFT
#undef NO_REF
#undef OPERAND
#undef TEMP
#undef REG
#undef IMM
#undef TYPE_OF
#undef TYPE_BOOLEAN
#undef TYPE_INT

} // namespace ia32dis
