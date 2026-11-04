/*
 * RiscvSemanticTable.cpp
 *
 * Implementation file
 *
 * The semantic table of the scalar unprivileged RISC-V instructions (see
 * RiscvSemantics.h): the mnemonics of gRiscvBaseEncodingTable which the
 * lifter lifts, with their classification. Every instruction is lifted by a
 * hand written function over the RiscvLiftContext.
 *
 * NOTE: This file holds the first instructions only, to run the pipeline
 *       end to end: add, sub, the logic operations, addi, addw, addiw, lui,
 *       auipc, the loads and stores of the integer registers, the
 *       conditional branches, jal, jalr, fadd.s and fadd.d. The mnemonics
 *       which are not here are lifted to OP_UNDEFINED.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include <string.h>

#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvSemantics.h"

namespace riscvdis {

typedef SemanticOperation Op;

namespace {

// Return true if the mnemonic of the lifted instruction is 'name'
bool isMnemonic(RiscvLiftContext& context, const char* name)
{
    return strcmp(context.getInstruction().getEntry()->m_mnemonic, name) == 0;
}

// add, sub, and, or, xor, addi, andi, ori, xori
void liftArithmetic(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    Op::Operation operation = Op::OP_ADD;
    if (isMnemonic(context, "sub"))
        operation = Op::OP_SUBTRACT;
    else if (isMnemonic(context, "and") || isMnemonic(context, "andi"))
        operation = Op::OP_AND;
    else if (isMnemonic(context, "or") || isMnemonic(context, "ori"))
        operation = Op::OP_OR;
    else if (isMnemonic(context, "xor") || isMnemonic(context, "xori"))
        operation = Op::OP_XOR;
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    context.write(0, context.emit(operation, type, a, b));
}

// addw, subw, addiw: the 32 bit operation, sign extended
void liftArithmeticWord(RiscvLiftContext& context)
{
    const SemanticType type = SemanticType::integer(32);
    const Op::Operation operation = isMnemonic(context, "subw") ?
                                    Op::OP_SUBTRACT : Op::OP_ADD;
    SemanticOperand a = context.read(1, type);
    SemanticOperand b = context.read(2, type);
    context.writeSignExtended(0, context.emit(operation, type, a, b));
}

// lui, auipc
void liftUpper(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    // The operand is the 20 bit field: the value is sign extended from 32 bits
    const int64 value = (int64)(int32)((uint32)context.getOperand(1).m_immediate
                                       << 12);
    SemanticOperand upper = context.getImmediate(value, type);
    if (isMnemonic(context, "auipc"))
        upper = context.emit(Op::OP_ADD, type, context.getCurrentAddress(),
                             upper);
    context.write(0, upper);
}

// The size in bits of the memory access of a load or a store, by its mnemonic
uint getAccessBits(RiscvLiftContext& context)
{
    const char* mnemonic = context.getInstruction().getEntry()->m_mnemonic;
    switch (mnemonic[1])
    {
    case 'b': return 8;
    case 'h': return 16;
    case 'w': return 32;
    default:  return 64;
    }
}

// lb, lbu, lh, lhu, lw, lwu, ld
void liftLoad(RiscvLiftContext& context)
{
    const SemanticType type = SemanticType::integer(getAccessBits(context));
    SemanticOperand value = context.load(context.getAddress(1), type);
    const char* mnemonic = context.getInstruction().getEntry()->m_mnemonic;
    const bool isUnsigned = (mnemonic[strlen(mnemonic) - 1] == 'u');
    if (type.getTotalBits() < context.getXlen())
        value = context.emit(isUnsigned ? Op::OP_ZERO_EXTEND :
                                          Op::OP_SIGN_EXTEND,
                             context.getNativeType(), value);
    context.write(0, value);
}

// sb, sh, sw, sd
void liftStore(RiscvLiftContext& context)
{
    const SemanticType type = SemanticType::integer(getAccessBits(context));
    context.store(context.getAddress(1), context.read(0, type));
}

// beq, bne, blt, bge, bltu, bgeu
void liftBranch(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    Op::Condition condition = Op::COND_EQUAL;
    if (isMnemonic(context, "bne"))
        condition = Op::COND_NOT_EQUAL;
    else if (isMnemonic(context, "blt"))
        condition = Op::COND_SIGNED_LESS;
    else if (isMnemonic(context, "bge"))
        condition = Op::COND_SIGNED_GREATER_EQUAL;
    else if (isMnemonic(context, "bltu"))
        condition = Op::COND_UNSIGNED_LESS;
    else if (isMnemonic(context, "bgeu"))
        condition = Op::COND_UNSIGNED_GREATER_EQUAL;
    SemanticOperand a = context.read(0, type);
    SemanticOperand b = context.read(1, type);
    SemanticOperand taken = context.newTemporary(SemanticType::integer(1));
    SemanticInstruction compare(Op::OP_COMPARE, type, taken, a, b);
    compare.setCondition(condition);
    context.emit(compare);
    context.branchIf(taken, context.read(2, type));
}

// Return true if the register is ra or t0, the ones of the return address
// stack hints
bool isLinkRegister(uint registerId)
{
    return (registerId == RISCV_REG_X0 + 1) || (registerId == RISCV_REG_X0 + 5);
}

// jal rd, target
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

// jalr rd, rs1, offset (or rd, offset(rs1))
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
                          context.getImmediate(-2, type));
    if (rd != RISCV_REG_X0)
        context.write(0, context.getNextAddress());
    if ((rd == RISCV_REG_X0) && isLinkRegister(base) && (offset == 0))
        context.returnTo(target);
    else if (isLinkRegister(rd))
        context.call(target);
    else
        context.branchIndirect(target);
}

// fadd.s, fadd.d
void liftFloatAdd(RiscvLiftContext& context)
{
    const SemanticType type = SemanticType::floating(
                isMnemonic(context, "fadd.d") ? SemanticType::FLOAT_DOUBLE :
                                                SemanticType::FLOAT_SINGLE);
    SemanticOperand a = context.readFloat(1, type);
    SemanticOperand b = context.readFloat(2, type);
    const Op::RoundingMode rounding = (Op::RoundingMode)context.getRoundingMode(3);
    SemanticOperand result = context.newTemporary(type);
    SemanticInstruction instruction(Op::OP_FLOAT_ADD, type, result, a, b);
    instruction.setRoundingMode(rounding);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    context.emit(instruction);
    context.writeFloat(0, result);
}

#define U_(x) OpcodeClassification::UNIT_##x
#define P_(x) OpcodeClassification::PROPERTY_##x

// An entry: the mnemonic, the lifter, the category, the units and the
// properties
#define RV(name, lift, category, units, properties) \
    { RiscvSemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, OpcodeClassification::CATEGORY_##category, units, \
        properties, 0, 0, 0, 0 }, \
      lift, 0, 0, 0, 0 }
// The same for the floating point: the data type is the format
#define RVF(name, lift, category, units, properties, format, bits) \
    { RiscvSemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, OpcodeClassification::CATEGORY_##category, units, \
        properties, 0, 0, 0, 0 }, \
      lift, SemanticType::KIND_FLOAT, SemanticType::FLOAT_##format, bits, 1 }

} // namespace

const RiscvSemanticEntry gRiscvSemanticTable[] =
{
    RV("add", liftArithmetic, ARITHMETIC, U_(INTEGER_ALU), 0),
    RV("sub", liftArithmetic, ARITHMETIC, U_(INTEGER_ALU), 0),
    RV("and", liftArithmetic, LOGIC, U_(INTEGER_ALU), 0),
    RV("or", liftArithmetic, LOGIC, U_(INTEGER_ALU), 0),
    RV("xor", liftArithmetic, LOGIC, U_(INTEGER_ALU), 0),
    RV("addi", liftArithmetic, ARITHMETIC, U_(INTEGER_ALU), 0),
    RV("andi", liftArithmetic, LOGIC, U_(INTEGER_ALU), 0),
    RV("ori", liftArithmetic, LOGIC, U_(INTEGER_ALU), 0),
    RV("xori", liftArithmetic, LOGIC, U_(INTEGER_ALU), 0),
    RV("addw", liftArithmeticWord, ARITHMETIC, U_(INTEGER_ALU), 0),
    RV("subw", liftArithmeticWord, ARITHMETIC, U_(INTEGER_ALU), 0),
    RV("addiw", liftArithmeticWord, ARITHMETIC, U_(INTEGER_ALU), 0),
    RV("lui", liftUpper, MOVE, U_(INTEGER_ALU), 0),
    RV("auipc", liftUpper, ARITHMETIC, U_(INTEGER_ALU), 0),
    RV("lb", liftLoad, LOAD, U_(LOAD), P_(READS_MEMORY)),
    RV("lbu", liftLoad, LOAD, U_(LOAD), P_(READS_MEMORY)),
    RV("lh", liftLoad, LOAD, U_(LOAD), P_(READS_MEMORY)),
    RV("lhu", liftLoad, LOAD, U_(LOAD), P_(READS_MEMORY)),
    RV("lw", liftLoad, LOAD, U_(LOAD), P_(READS_MEMORY)),
    RV("lwu", liftLoad, LOAD, U_(LOAD), P_(READS_MEMORY)),
    RV("ld", liftLoad, LOAD, U_(LOAD), P_(READS_MEMORY)),
    RV("sb", liftStore, STORE, U_(STORE), P_(WRITES_MEMORY)),
    RV("sh", liftStore, STORE, U_(STORE), P_(WRITES_MEMORY)),
    RV("sw", liftStore, STORE, U_(STORE), P_(WRITES_MEMORY)),
    RV("sd", liftStore, STORE, U_(STORE), P_(WRITES_MEMORY)),
    RV("beq", liftBranch, BRANCH, U_(BRANCH), P_(CONDITIONAL)),
    RV("bne", liftBranch, BRANCH, U_(BRANCH), P_(CONDITIONAL)),
    RV("blt", liftBranch, BRANCH, U_(BRANCH), P_(CONDITIONAL)),
    RV("bge", liftBranch, BRANCH, U_(BRANCH), P_(CONDITIONAL)),
    RV("bltu", liftBranch, BRANCH, U_(BRANCH), P_(CONDITIONAL)),
    RV("bgeu", liftBranch, BRANCH, U_(BRANCH), P_(CONDITIONAL)),
    RV("jal", liftJal, BRANCH, U_(BRANCH), 0),
    RV("jalr", liftJalr, BRANCH, U_(BRANCH), 0),
    RVF("fadd.s", liftFloatAdd, FLOAT, U_(FLOAT_ADD), 0, SINGLE, 32),
    RVF("fadd.d", liftFloatAdd, FLOAT, U_(FLOAT_ADD), 0, DOUBLE, 64),
    // The end of the table
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0 }
};

} // namespace riscvdis
