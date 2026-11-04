/*
 * RiscvSemanticLiftAtomic.cpp
 *
 * Implementation file
 *
 * The lifters of the atomic instructions: Zalrsc (lr, sc), Zaamo (the amos),
 * Zabha (the byte and halfword amos) and Zacas (amocas), see
 * RiscvSemanticInternal.h. The reservation is modeled by the registers
 * RISCV_REG_RESERVATION_ADDRESS and RISCV_REG_RESERVATION_SIZE (the size is 0
 * when there is none), as the exclusive monitor of ARM.
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
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"

namespace riscvdis {

namespace {

// Zama16b: a misaligned atomic access completes when it stays inside a
// naturally aligned region of 16 bytes, else it raises an access fault
void checkGranule(RiscvLiftContext& context, const SemanticOperand& address,
                  uint bytes)
{
    if (bytes <= 1)
        return;
    const SemanticType type = context.getNativeType();
    SemanticOperand offset = context.emit(Op::OP_AND, type, address,
                                          makeConstant(context, 15, type));
    SemanticOperand end = context.emit(Op::OP_ADD, type, offset,
                                       makeConstant(context, bytes, type));
    SemanticOperand isCrossing = makeCompare(context, Op::COND_UNSIGNED_GREATER,
                            type, end, makeConstant(context, 16, type));
    makeTrapIf(context, isCrossing, Op::TRAP_FAULT, RISCV_TRAP_ACCESS_FAULT);
}

// A natural alignment is required (lr, sc, the shadow stack): a misaligned
// access raises the exception
void checkAlignment(RiscvLiftContext& context, const SemanticOperand& address,
                    uint bytes)
{
    if (bytes <= 1)
        return;
    const SemanticType type = context.getNativeType();
    SemanticOperand offset = context.emit(Op::OP_AND, type, address,
                                          makeConstant(context, bytes - 1, type));
    SemanticOperand isMisaligned = makeCompare(context, Op::COND_NOT_EQUAL,
                            type, offset, makeConstant(context, 0, type));
    makeTrapIf(context, isMisaligned, Op::TRAP_FAULT, RISCV_TRAP_ACCESS_FAULT);
}

// Return the value of the register pair which starts at the register of
// operand 'index' (the even register: the low part), 2 XLEN bits wide. The
// zero register is a zero pair
SemanticOperand readPair(RiscvLiftContext& context, uint index)
{
    const uint reg = context.getOperand(index).m_register;
    if (reg == RISCV_REG_X0)
        return makeConstant(context, 0, integerType(2 * context.getXlen()));
    return context.emit(Op::OP_CONCAT, integerType(2 * context.getXlen()),
                        context.getRegister(reg), context.getRegister(reg + 1));
}

// Write the pair
void writePair(RiscvLiftContext& context, uint index,
               const SemanticOperand& value)
{
    const uint reg = context.getOperand(index).m_register;
    if (reg == RISCV_REG_X0)
        return;
    const uint xlen = context.getXlen();
    context.move(context.getRegister(reg),
                 context.emit(Op::OP_TRUNCATE, integerType(xlen), value));
    context.move(context.getRegister(reg + 1),
        context.emit(Op::OP_EXTRACT, integerType(xlen), value,
                     makeConstant(context, xlen, integerType(32))));
}

} // namespace

void liftLoadReserved(RiscvLiftContext& context)
{
    const uint bits = (uint)context.getParameter();
    const SemanticType type = integerType(bits);
    SemanticOperand address = context.getAddress(1);
    checkAlignment(context, address, bits / 8);
    SemanticOperand value = makeLoad(context, address, type,
                                     getAtomicOrder(context));
    // The reservation
    context.move(context.getRegister(RISCV_REG_RESERVATION_ADDRESS), address);
    context.move(context.getRegister(RISCV_REG_RESERVATION_SIZE),
                 makeConstant(context, bits / 8, context.getNativeType()));
    context.writeSignExtended(0, value);
}

void liftStoreConditional(RiscvLiftContext& context)
{
    const uint bits = (uint)context.getParameter();
    const SemanticType type = integerType(bits);
    const SemanticType native = context.getNativeType();
    SemanticOperand address = context.getAddress(2);
    SemanticOperand value = context.read(1, type);
    checkAlignment(context, address, bits / 8);
    // The access is checked whether the sc succeeds or not
    makeLoad(context, address, type, Op::ORDER_RELAXED);

    SemanticOperand isReserved = makeCompare(context, Op::COND_EQUAL, native,
            context.getRegister(RISCV_REG_RESERVATION_SIZE),
            makeConstant(context, bits / 8, native));
    SemanticOperand isSame = makeCompare(context, Op::COND_EQUAL, native,
            context.getRegister(RISCV_REG_RESERVATION_ADDRESS), address);
    SemanticOperand isPassed = context.emit(Op::OP_AND, integerType(1),
                                            isReserved, isSame);
    SemanticOperand failLabel = context.newLabel();
    SemanticOperand endLabel = context.newLabel();
    context.branchIf(context.emit(Op::OP_XOR, integerType(1), isPassed,
                        makeConstant(context, 1, integerType(1))), failLabel);
    makeStore(context, address, value, getAtomicOrder(context));
    context.write(0, makeConstant(context, 0, native));
    context.branch(endLabel);
    context.placeLabel(failLabel);
    context.write(0, makeConstant(context, 1, native));
    context.placeLabel(endLabel);
    // An sc ends the reservation, whether it succeeded or not
    context.move(context.getRegister(RISCV_REG_RESERVATION_SIZE),
                 makeConstant(context, 0, native));
}

void liftAtomicOperation(RiscvLiftContext& context)
{
    const uint64 parameter = context.getParameter();
    const uint bits = (uint)(parameter & 0xFF);
    const Op::Operation operation = (Op::Operation)(parameter >> 8);
    const SemanticType type = integerType(bits);
    SemanticOperand address = context.getAddress(2);
    SemanticOperand value = context.read(1, type);
    // The shadow stack AMO needs a natural alignment
    if (strcmp(getMnemonic(context), "ssamoswap.w") == 0 ||
        strcmp(getMnemonic(context), "ssamoswap.d") == 0)
        checkAlignment(context, address, bits / 8);
    else
        checkGranule(context, address, bits / 8);

    SemanticOperand result = context.newTemporary(type);
    if (operation == Op::OP_ATOMIC_EXCHANGE)
    {
        SemanticInstruction instruction(Op::OP_ATOMIC_EXCHANGE, type, result,
                                        address, value);
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        instruction.setMemoryOrder(getAtomicOrder(context));
        context.emit(instruction);
    } else
    {
        SemanticInstruction instruction(Op::OP_ATOMIC_RMW, type, result,
                                        address, value);
        instruction.setCombineOperation((Op::Operation)(parameter >> 8));
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        instruction.setMemoryOrder(getAtomicOrder(context));
        context.emit(instruction);
    }
    context.writeSignExtended(0, result);
}

void liftShadowStackSwap(RiscvLiftContext& context)
{
    // ssamoswap is an illegal instruction until the supervisor enables the
    // shadow stack; otherwise it is an amoswap on the shadow stack
    if (!context.isControlFlowIntegrity())
    {
        context.trap(Op::TRAP_UNDEFINED, RISCV_TRAP_ILLEGAL_INSTRUCTION);
        return;
    }
    liftAtomicOperation(context);
}

void liftCompareAndSwap(RiscvLiftContext& context)
{
    const uint bits = (uint)context.getParameter();
    const SemanticType type = integerType(bits);
    SemanticOperand address = context.getAddress(2);
    checkGranule(context, address, bits / 8);

    SemanticOperand expected;
    SemanticOperand replacement;
    const bool isPair = bits > context.getXlen();
    if (isPair)
    {
        expected = readPair(context, 0);
        replacement = readPair(context, 1);
    } else
    {
        expected = context.read(0, type);
        replacement = context.read(1, type);
    }
    SemanticOperand result = context.newTemporary(type);
    SemanticInstruction instruction(Op::OP_ATOMIC_COMPARE_EXCHANGE, type,
                                    result, address, expected, replacement);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    instruction.setMemoryOrder(getAtomicOrder(context));
    context.emit(instruction);
    if (isPair)
        writePair(context, 0, result);
    else
        context.writeSignExtended(0, result);
}

} // namespace riscvdis
