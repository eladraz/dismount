/*
 * AMD64LoweringAtomic.cpp
 *
 * The lowering of the atomic operations of 8, 16, 32 and 64 bits:
 * OP_ATOMIC_EXCHANGE, OP_ATOMIC_COMPARE_EXCHANGE and OP_ATOMIC_RMW (the
 * combine operations ADD, SUBTRACT, AND, OR, XOR, AND_NOT, and the signed and
 * unsigned MINIMUM and MAXIMUM). The result of every one is the OLD value of
 * the memory, whatever the outcome of the operation, zero or sign extension
 * left to the IR (the lifter writes the sign extension of the 32 bit results).
 *
 *   Operation                       Code (the address is in rdx, the access is
 *                                   [r14+rdx]; the result is in rax)
 *   EXCHANGE                        xchg [mem], al            (locked by itself)
 *   COMPARE_EXCHANGE                lock cmpxchg [mem], cl    (rax = expected in,
 *                                                              old value out)
 *   RMW ADD                         lock xadd [mem], al       (rax = old)
 *   RMW SUBTRACT                    neg al; lock xadd [mem], al
 *   RMW AND OR XOR AND_NOT          loop: r8 = old <op> value;
 *   RMW MIN/MAX (signed, unsigned)  lock cmpxchg [mem], r8; jne loop
 *
 * Memory order: every access is a locked instruction (x86-64 lock prefixed
 * operations and xchg with memory are sequentially consistent), so the order
 * the IR asks for (acquire, release, sequential) is always provided. The
 * alignment of an access is not checked here: the lifter guards it with a
 * trap before the operation.
 *
 * Not lowered: a 128 bit compare-exchange (its cmpxchg16b takes rbx, which
 * the register contract doesn't give to the lowering). The loops of the
 * RMW combine operations use local labels, so they need lowerBlock().
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include <stdio.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "AMD64BlockLowering.h"

using namespace amd64lowering;

namespace {

// The local label of the loop of the IR instruction 'index'
void formatLoopLabel(char* text, uint size, uint index)
{
    snprintf(text, size, "L_a%u_l", (unsigned)index);
}

// The name of r8 in 'bits' (r8 is a scratch register of the lowering, see
// AMD64Lowering.h)
const char* getExtraRegister(uint bits)
{
    switch (bits)
    {
    case 64: return "r8";
    case 32: return "r8d";
    case 16: return "r8w";
    default: return "r8b";
    }
}

// The condition code of cmov and jcc which keeps the old value of a minimum
// or maximum: old is kept when it is the smaller (larger) of the two, after
// 'cmp old, value'. NULL for the other combine operations
const char* getKeepCondition(SemanticOperation::Operation operation)
{
    switch (operation)
    {
    case SemanticOperation::OP_MINIMUM_SIGNED:    return "l";
    case SemanticOperation::OP_MAXIMUM_SIGNED:    return "g";
    case SemanticOperation::OP_MINIMUM_UNSIGNED:  return "b";
    case SemanticOperation::OP_MAXIMUM_UNSIGNED:  return "a";
    default:                                      return NULL;
    }
}
} // namespace

void BlockLowering::checkAtomicWidth(const SemanticOperand& operand,
                                     uint bits) const
{
    if (getWidth(operand.getType(), WIDTH_INTEGERS) != bits)
        fail("the operands have another width than the result");
}

void BlockLowering::lineAtomicAccess(const char* mnemonic, uint bits,
                                     const char* reg)
{
    char text[160];
    snprintf(text, sizeof(text), "%s %s [%s+rdx], %s", mnemonic,
             getSizeKeyword(bits), m_base.getASCIIstring().getBuffer(), reg);
    line(text);
}

void BlockLowering::lowerAtomicLoop(const SemanticInstruction& instruction,
                                    uint bits)
{
    const SemanticOperation::Operation combine =
                                        instruction.getCombineOperation();
    const SemanticOperand& value = instruction.getSource(1);
    const char* keep = getKeepCondition(combine);
    if ((combine != SemanticOperation::OP_AND) &&
        (combine != SemanticOperation::OP_OR) &&
        (combine != SemanticOperation::OP_XOR) &&
        (combine != SemanticOperation::OP_AND_NOT) && (keep == NULL))
    {
        fail("the combine operation of this atomic read-modify-write is not implemented");
    }
    if (!m_allowLocalLabels)
        fail("an atomic loop needs its local labels: lower the block with lowerBlock()");

    // rdx: the guest address; rcx: the operand, zero extended
    load(SCRATCH_C, value, EXTEND_ZERO);
    // rax: the first old value (movzx for 8 and 16 bits, zero extended)
    char text[160];
    snprintf(text, sizeof(text), "%s %s, %s [%s+rdx]",
             (bits < 32) ? "movzx" : "mov",
             getRegister(SCRATCH_A, (bits == 64) ? 64 : 32),
             getSizeKeyword(bits), m_base.getASCIIstring().getBuffer());
    line(text);

    char label[40];
    formatLoopLabel(label, sizeof(label), m_index);
    defineLocalLabel(cString(label));
    const char* extra = getExtraRegister(bits);
    const char* reg = getRegister(SCRATCH_A, bits);
    // r8 = old <combine> value. The 64 bit operations are exact in the low
    // bits, the compare-exchange stores only 'bits' of r8
    if (keep != NULL)
    {
        line("mov r8, rcx");
        snprintf(text, sizeof(text), "cmp %s, %s", reg, getRegister(SCRATCH_C, bits));
        line(text);
        snprintf(text, sizeof(text), "cmov%s r8, rax", keep);
        line(text);
    } else if (combine == SemanticOperation::OP_AND_NOT)
    {
        line("mov r8, rcx");
        line("not r8");
        line("and r8, rax");
    } else
    {
        line("mov r8, rax");
        const char* op = (combine == SemanticOperation::OP_AND) ? "and" :
                         ((combine == SemanticOperation::OP_OR) ? "or" : "xor");
        snprintf(text, sizeof(text), "%s r8, rcx", op);
        line(text);
    }
    lineAtomicAccess("lock cmpxchg", bits, extra);
    snprintf(text, sizeof(text), "jne %s", label);
    line(text);
}

void BlockLowering::lowerAtomicRmw(const SemanticInstruction& instruction,
                                   uint bits)
{
    const SemanticOperation::Operation combine =
                                        instruction.getCombineOperation();
    const SemanticOperand& address = instruction.getSource(0);
    const SemanticOperand& value = instruction.getSource(1);
    checkAtomicWidth(value, bits);
    if ((combine == SemanticOperation::OP_ADD) ||
        (combine == SemanticOperation::OP_SUBTRACT))
    {
        // xadd leaves the old value in its register and adds the other
        load(SCRATCH_D, address, EXTEND_ZERO);
        load(SCRATCH_A, value, EXTEND_ZERO);
        if (combine == SemanticOperation::OP_SUBTRACT)
        {
            char text[40];
            snprintf(text, sizeof(text), "neg %s", getRegister(SCRATCH_A, bits));
            line(text);
        }
        lineAtomicAccess("lock xadd", bits, getRegister(SCRATCH_A, bits));
        return;
    }
    load(SCRATCH_D, address, EXTEND_ZERO);
    lowerAtomicLoop(instruction, bits);
}

void BlockLowering::lowerAtomic(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    // The width of the access: the type of the instruction (a void type
    // takes the width of the destination, as the interpreter does)
    const SemanticType& type = instruction.getType().isVoid() ?
                               destination.getType() : instruction.getType();
    const uint bits = getWidth(type, WIDTH_INTEGERS);
    const SemanticOperand& address = instruction.getSource(0);
    const SemanticOperand& value = instruction.getSource(1);

    switch (m_operation)
    {
    case SemanticOperation::OP_ATOMIC_EXCHANGE:
        checkAtomicWidth(value, bits);
        load(SCRATCH_D, address, EXTEND_ZERO);
        load(SCRATCH_A, value, EXTEND_ZERO);
        // xchg with memory is locked: rax = the old value
        lineAtomicAccess("xchg", bits, getRegister(SCRATCH_A, bits));
        break;
    case SemanticOperation::OP_ATOMIC_COMPARE_EXCHANGE:
    {
        const SemanticOperand& replacement = instruction.getSource(2);
        checkAtomicWidth(value, bits);
        checkAtomicWidth(replacement, bits);
        load(SCRATCH_D, address, EXTEND_ZERO);
        // rax = expected; cmpxchg leaves the old value in rax either way
        load(SCRATCH_A, value, EXTEND_ZERO);
        load(SCRATCH_C, replacement, EXTEND_ZERO);
        lineAtomicAccess("lock cmpxchg", bits, getRegister(SCRATCH_C, bits));
        break;
    }
    case SemanticOperation::OP_ATOMIC_RMW:
        lowerAtomicRmw(instruction, bits);
        break;
    default:
        fail("the operation is not an atomic operation");
        break;
    }
    if (destination.isValid())
        store(destination, SCRATCH_A);
}
