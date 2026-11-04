/*
 * RiscvSemanticLiftSystem.cpp
 *
 * Implementation file
 *
 * The lifters of Zicsr, the fences, the cache block operations and the
 * prefetches, ecall and ebreak, Zicfilp and Zicfiss and the privileged
 * instructions (see RiscvSemanticInternal.h).
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

// The CSRs of the description
enum {
    CSR_FFLAGS = 0x001,
    CSR_FRM = 0x002,
    CSR_FCSR = 0x003,
    CSR_VSTART = 0x008,
    CSR_VXSAT = 0x009,
    CSR_VXRM = 0x00A,
    CSR_VCSR = 0x00F,
    CSR_SSP = 0x011,
    CSR_VL = 0xC20,
    CSR_VTYPE = 0xC21,
    CSR_VLENB = 0xC22
};

// The size of a block of the cache block operations (Zic64b)
enum { CACHE_BLOCK_BYTES = 64 };

// Return the register of an inline CSR, or false. 'bits' is the width of
// the field of the CSR, 'isReadOnly' tells the CSR can't be written
bool findInlineCsr(RiscvLiftContext& context, uint csr, uint& registerId,
                   uint& bits, bool& isReadOnly)
{
    isReadOnly = false;
    switch (csr)
    {
    case CSR_FFLAGS: registerId = RISCV_REG_FFLAGS; bits = 5; return true;
    case CSR_FRM:    registerId = RISCV_REG_FRM; bits = 3; return true;
    case CSR_FCSR:   registerId = RISCV_REG_FCSR; bits = 8; return true;
    case CSR_VSTART:
    {
        // Only the bits that hold the largest element index (VLMAX is at
        // most VLEN, with LMUL = 8 and SEW = 8): log2(VLEN)
        registerId = RISCV_REG_VSTART;
        bits = 0;
        while (((uint)1 << (bits + 1)) <= context.getVectorBits())
            bits++;
        return true;
    }
    case CSR_VXSAT:  registerId = RISCV_REG_VXSAT; bits = 1; return true;
    case CSR_VXRM:   registerId = RISCV_REG_VXRM; bits = 2; return true;
    case CSR_VCSR:   registerId = RISCV_REG_VCSR; bits = 3; return true;
    case CSR_SSP:
        if (!context.isControlFlowIntegrity())
            return false;
        registerId = RISCV_REG_SSP; bits = context.getXlen(); return true;
    case CSR_VL:     registerId = RISCV_REG_VL; bits = context.getXlen();
                     isReadOnly = true; return true;
    case CSR_VTYPE:  registerId = RISCV_REG_VTYPE; bits = context.getXlen();
                     isReadOnly = true; return true;
    default:
        return false;
    }
}

// Read an inline CSR as a XLEN value
SemanticOperand readInlineCsr(RiscvLiftContext& context, uint registerId,
                              uint bits)
{
    // (a copy: the instruction writes the CSR before it writes rd)
    SemanticOperand value = context.emit(Op::OP_MOVE,
            context.getRegister(registerId).getType(),
            context.getRegister(registerId));
    // (fcsr is a 32 bit register of which 8 bits are defined, vcsr 3)
    if (value.getType().getTotalBits() > bits)
        value = makeTruncate(context, value, bits);
    return makeZeroExtend(context, value, context.getXlen());
}

// Return the FenceAccess bits of a fence set of the instruction (i o r w)
uint getFenceBits(uint set, bool isBefore)
{
    uint bits = 0;
    if ((set & 0x0A) != 0)
        bits |= isBefore ? Op::FENCE_BEFORE_LOADS : Op::FENCE_AFTER_LOADS;
    if ((set & 0x05) != 0)
        bits |= isBefore ? Op::FENCE_BEFORE_STORES : Op::FENCE_AFTER_STORES;
    return bits;
}

// Add a fence
void emitFence(RiscvLiftContext& context, uint access)
{
    SemanticInstruction instruction(Op::OP_FENCE, SemanticType(),
                                    SemanticOperand());
    instruction.setFenceAccess((uint8)access);
    context.emit(instruction);
}

// Return the address aligned to the cache block
SemanticOperand getBlockAddress(RiscvLiftContext& context, uint index)
{
    const SemanticType type = context.getNativeType();
    return context.emit(Op::OP_AND, type, context.getAddress(index),
                        makeConstant(context, -(int64)CACHE_BLOCK_BYTES, type));
}

// Add a helper call: d = helper number(s1, s2)
SemanticOperand emitHelper(RiscvLiftContext& context, uint number,
                           int64 argument0, const SemanticOperand& argument1)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand result = context.newTemporary(type);
    context.emit(SemanticInstruction(Op::OP_HELPER, type, result,
                makeConstant(context, number, integerType(32)),
                makeConstant(context, argument0, type), argument1));
    return result;
}

} // namespace

void liftCsr(RiscvLiftContext& context)
{
    const uint64 parameter = context.getParameter();
    const uint kind = (uint)(parameter & 0xFF);
    const bool isImmediate = (parameter & CSR_IMMEDIATE) != 0;
    const SemanticType type = context.getNativeType();
    const uint csr = context.getOperand(1).m_csr;
    const RiscvDecodedOperand& source = context.getOperand(2);

    // Whether the instruction writes the CSR: csrrw always, csrrs and csrrc
    // when the source isn't x0 / 0
    bool isWrite = true;
    if (kind != 0)
        isWrite = isImmediate ? (source.m_immediate != 0) :
                                (source.m_register != RISCV_REG_X0);
    // Whether it reads it: csrrw to x0 doesn't
    const bool isRead = (kind != 0) ||
                        (context.getOperand(0).m_register != RISCV_REG_X0);
    const bool isReadOnlyAddress = (csr >> 10) == 3;

    SemanticOperand operandValue = context.read(2, type);

    uint registerId = 0;
    uint bits = 0;
    bool isReadOnly = false;
    const bool isInline = findInlineCsr(context, csr, registerId, bits,
                                        isReadOnly);
    if ((csr == CSR_SSP) && !context.isControlFlowIntegrity())
    {
        // The shadow stack pointer is not accessible until Zicfiss is enabled
        context.trap(Op::TRAP_UNDEFINED, RISCV_TRAP_ILLEGAL_INSTRUCTION);
        return;
    }
    if (csr == CSR_VLENB)
    {
        // A constant: VLEN / 8
        if (isWrite)
        {
            context.trap(Op::TRAP_UNDEFINED, RISCV_TRAP_ILLEGAL_INSTRUCTION);
            return;
        }
        context.write(0, makeConstant(context, context.getVectorBits() / 8, type));
        return;
    }
    if ((isReadOnly || isReadOnlyAddress) && isWrite)
    {
        // A write to a read only CSR
        context.trap(Op::TRAP_UNDEFINED, RISCV_TRAP_ILLEGAL_INSTRUCTION);
        return;
    }

    SemanticOperand old;
    if (isInline)
    {
        old = isRead ? readInlineCsr(context, registerId, bits) :
                       makeConstant(context, 0, type);
    } else
    {
        // The counters and the CSRs outside of the description: a helper
        old = emitHelper(context, RISCV_HELPER_READ_CSR, csr,
                         makeConstant(context, 0, type));
    }

    if (isWrite)
    {
        SemanticOperand value = operandValue;
        if (kind == 1)
            value = context.emit(Op::OP_OR, type, old, operandValue);
        else if (kind == 2)
            value = context.emit(Op::OP_AND_NOT, type, operandValue, old);
        if (isInline)
        {
            // The field keeps the low bits; fcsr / vcsr have 8 / 3 bits
            const ArchitectureRegister& field =
                    context.getArchitecture()->getRegister(registerId);
            const uint fieldBits = (bits < field.m_bits) ? bits : field.m_bits;
            SemanticOperand narrow = makeTruncate(context, value, fieldBits);
            if (fieldBits < field.m_bits)
                narrow = makeZeroExtend(context, narrow, field.m_bits);
            context.move(context.getRegister(registerId), narrow);
        } else
        {
            emitHelper(context, RISCV_HELPER_WRITE_CSR, csr, value);
        }
    }
    context.write(0, old);
}

void liftFence(RiscvLiftContext& context)
{
    const uint access = getFenceBits(context.getOperand(0).m_fenceSet, true) |
                        getFenceBits(context.getOperand(1).m_fenceSet, false);
    emitFence(context, access);
}

void liftFenceTso(RiscvLiftContext& context)
{
    // Loads before ordered against loads and stores after, stores against
    // stores: the specification's fence.tso (rw, rw without the store to
    // load ordering) has no finer description than the full fence
    emitFence(context, Op::FENCE_ALL);
}

void liftFenceInstruction(RiscvLiftContext& context)
{
    context.emit(SemanticInstruction(Op::OP_FENCE_INSTRUCTION, SemanticType(),
                                     SemanticOperand()));
}

void liftPause(RiscvLiftContext& context)
{
    SemanticInstruction instruction(Op::OP_HINT, SemanticType(),
                                    SemanticOperand());
    instruction.setKind(Op::HINT_SPIN);
    context.emit(instruction);
}

void liftWaitReservation(RiscvLiftContext& context)
{
    SemanticInstruction instruction(Op::OP_HINT, SemanticType(),
                                    SemanticOperand());
    instruction.setKind(Op::HINT_WAIT);
    context.emit(instruction);
}

void liftPrefetch(RiscvLiftContext& context)
{
    context.emit(SemanticInstruction(Op::OP_PREFETCH, SemanticType(),
                                     SemanticOperand(), context.getAddress(0)));
}

void liftCacheBlock(RiscvLiftContext& context)
{
    SemanticOperand address = getBlockAddress(context, 0);
    // The block must be accessible, else the instruction faults
    makeLoad(context, address, integerType(8), Op::ORDER_RELAXED);
    SemanticInstruction instruction(Op::OP_CACHE_MAINTENANCE, SemanticType(),
                                    SemanticOperand(), address);
    instruction.setKind((uint)context.getParameter());
    context.emit(instruction);
}

void liftCacheBlockZero(RiscvLiftContext& context)
{
    // The whole aligned block is written with zeros
    const SemanticType word = integerType(64);
    SemanticOperand address = getBlockAddress(context, 0);
    for (uint offset = 0; offset < CACHE_BLOCK_BYTES; offset += 8)
    {
        SemanticOperand target = address;
        if (offset != 0)
            target = context.emit(Op::OP_ADD, context.getNativeType(), address,
                    makeConstant(context, offset, context.getNativeType()));
        makeStore(context, target, makeConstant(context, 0, word),
                  Op::ORDER_RELAXED);
    }
}

void liftEcall(RiscvLiftContext& context)
{
    context.trap(Op::TRAP_SYSTEM_CALL, RISCV_TRAP_ECALL);
}

void liftEbreak(RiscvLiftContext& context)
{
    context.trap(Op::TRAP_BREAKPOINT, RISCV_TRAP_BREAKPOINT);
}

void liftUnimp(RiscvLiftContext& context)
{
    context.trap(Op::TRAP_UNDEFINED, RISCV_TRAP_ILLEGAL_INSTRUCTION);
}

void liftLandingPad(RiscvLiftContext& context)
{
    // Without Zicfilp enabled lpad is an auipc to x0
    if (!context.isControlFlowIntegrity())
    {
        context.emit(SemanticInstruction(Op::OP_NOP, SemanticType(),
                                         SemanticOperand()));
        return;
    }

    // While a landing pad is expected: the pc must be aligned to 4 bytes,
    // the label must be 0 or equal to x7[31:12]
    const SemanticType type = context.getNativeType();
    const int64 label = context.getOperand(0).m_immediate & 0xFFFFF;
    SemanticOperand skip = context.newLabel();
    SemanticOperand notExpected = context.emit(Op::OP_XOR, integerType(1),
                context.getRegister(RISCV_REG_ELP),
                makeConstant(context, 1, integerType(1)));
    context.branchIf(notExpected, skip);
    if ((context.getInstruction().getAddress() & 3) != 0)
        context.trap(Op::TRAP_FAULT, RISCV_TRAP_SOFTWARE_CHECK);
    if (label != 0)
    {
        SemanticOperand x7 = context.getRegister(RISCV_REG_X0 + 7);
        SemanticOperand upper = context.emit(Op::OP_AND, integerType(32),
            makeTruncate(context, context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type,
                    x7, makeConstant(context, 12, type)), 32),
            makeConstant(context, 0xFFFFF, integerType(32)));
        SemanticOperand isMismatch = makeCompare(context, Op::COND_NOT_EQUAL,
                integerType(32), upper, makeConstant(context, label, integerType(32)));
        makeTrapIf(context, isMismatch, Op::TRAP_FAULT, RISCV_TRAP_SOFTWARE_CHECK);
    }
    context.move(context.getRegister(RISCV_REG_ELP),
                 makeConstant(context, 0, integerType(1)));
    context.placeLabel(skip);
}

// The shadow stack accesses need a natural alignment: a misaligned ssp raises
// an access fault
void checkShadowStackAlignment(RiscvLiftContext& context)
{
    const SemanticType type = context.getNativeType();
    SemanticOperand offset = context.emit(Op::OP_AND, type,
            context.getRegister(RISCV_REG_SSP),
            makeConstant(context, context.getXlen() / 8 - 1, type));
    makeTrapIf(context, makeCompare(context, Op::COND_NOT_EQUAL, type, offset,
                                    makeConstant(context, 0, type)),
               Op::TRAP_FAULT, RISCV_TRAP_ACCESS_FAULT);
}

void liftShadowStackPush(RiscvLiftContext& context)
{
    if (!context.isControlFlowIntegrity())
        return;
    const SemanticType type = context.getNativeType();
    const uint bytes = context.getXlen() / 8;
    checkShadowStackAlignment(context);
    SemanticOperand pointer = context.emit(Op::OP_SUBTRACT, type,
            context.getRegister(RISCV_REG_SSP), makeConstant(context, bytes, type));
    makeStore(context, pointer, context.read(0, type), Op::ORDER_RELAXED);
    context.move(context.getRegister(RISCV_REG_SSP), pointer);
}

void liftShadowStackPopCheck(RiscvLiftContext& context)
{
    if (!context.isControlFlowIntegrity())
        return;
    const SemanticType type = context.getNativeType();
    const uint bytes = context.getXlen() / 8;
    checkShadowStackAlignment(context);
    SemanticOperand pointer = context.getRegister(RISCV_REG_SSP);
    SemanticOperand top = makeLoad(context, pointer, type, Op::ORDER_RELAXED);
    SemanticOperand isMismatch = makeCompare(context, Op::COND_NOT_EQUAL, type,
                                             top, context.read(0, type));
    makeTrapIf(context, isMismatch, Op::TRAP_FAULT, RISCV_TRAP_SOFTWARE_CHECK);
    context.move(pointer, context.emit(Op::OP_ADD, type, pointer,
                                       makeConstant(context, bytes, type)));
}

void liftShadowStackRead(RiscvLiftContext& context)
{
    // Without Zicfiss enabled ssrdp is a Zimop instruction
    if (context.isControlFlowIntegrity())
        context.write(0, context.getRegister(RISCV_REG_SSP));
    else
        context.write(0, makeConstant(context, 0, context.getNativeType()));
}

void liftPrivileged(RiscvLiftContext& context)
{
    // The whole instruction on the guest state: s1 is the encoding
    const SemanticType type = context.getNativeType();
    context.emit(SemanticInstruction(Op::OP_HELPER, type, SemanticOperand(),
            makeConstant(context, RISCV_HELPER_PRIVILEGED, integerType(32)),
            makeConstant(context, context.getInstruction().getEncoding(), type)));
}

} // namespace riscvdis
