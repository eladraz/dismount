/*
 * AMD64LoweringInteger.cpp
 *
 * The lowering of the integer operations which the main lowering doesn't cover:
 *
 *   OP_DIVIDE_SIGNED/UNSIGNED, OP_REMAINDER_SIGNED/UNSIGNED, 8 to 64 bits
 *       The behaviour of the guest for a division by zero (and the signed
 *       minimum / -1) is the one of the instruction: BEHAVIOR_DIVIDE_FAULT
 *       leaves the code with a divide error trap, BEHAVIOR_DIVIDE_ZERO gives
 *       a zero quotient, BEHAVIOR_DIVIDE_ALL_ONES all ones; the remainder of
 *       a division by zero is the dividend. The host never takes #DE: the
 *       divisor of a div / idiv is never zero, and minimum / -1 never reaches
 *       idiv. Uses local labels, so it is lowered by lowerBlock() only.
 *   OP_COUNT_LEADING_ZEROS, OP_COUNT_TRAILING_ZEROS, 8 to 64 bits
 *       bsr / bsf with a conditional move for the zero input (no lzcnt, tzcnt).
 *   OP_POPULATION_COUNT, 8 to 64 bits
 *       The bit sums of the SWAR method (no popcnt).
 *   OP_BYTE_SWAP, 8, 16, 32, 64 bits
 *   OP_BIT_REVERSE, 8 to 64 bits
 *       The bit reversal of 64 bits (SWAR, then bswap), shifted down to the width.
 *   OP_MULTIPLY_HIGH_SIGNED_UNSIGNED, 16, 32, 64 bits
 *       The high half of the signed by unsigned product: the unsigned product
 *       minus the unsigned operand when the signed one is negative.
 *   OP_ADD_CARRY, 8 to 64 bits (adc and the flags of adds with carry, A64)
 *       d = s0 + s1 + (s2 & 1), the carry is a boolean (bit 0 only, as the
 *       interpreter reads it).
 *
 * The scratch registers are rax, rcx and rdx only. The operands are read again
 * from the state when a value is needed after the registers were overwritten.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include <stdio.h>
#include "AMD64BlockLowering.h"

using namespace amd64lowering;

namespace {

// The label of the IR instruction 'index' which the division uses: 'z' is the
// zero divisor (the trap, for a fault), 'd' the division, 'e' the end
cString getDivideLabel(uint index, char tag)
{
    char text[40];
    snprintf(text, sizeof(text), "L_d%u_%c", (unsigned)index, tag);
    return cString(text);
}

// Return the 64 bit two's complement of the minimum of a 'bits' signed number
uint64 getMinimum(uint bits)
{
    return (bits == 64) ? (uint64)0x8000000000000000ULL :
                          ~(((uint64)1 << (bits - 1)) - 1);
}

// The masks of the bit operations (the SWAR methods), 64 bit constants
const char* const gMask55 = "0x5555555555555555";
const char* const gMask33 = "0x3333333333333333";
const char* const gMask0F = "0x0f0f0f0f0f0f0f0f";
const char* const gMask01 = "0x0101010101010101";

} // namespace

bool BlockLowering::lowerIntegerExtra(const SemanticInstruction& instruction)
{
    switch (instruction.getOperation())
    {
    case SemanticOperation::OP_DIVIDE_SIGNED:
    case SemanticOperation::OP_DIVIDE_UNSIGNED:
    case SemanticOperation::OP_REMAINDER_SIGNED:
    case SemanticOperation::OP_REMAINDER_UNSIGNED:
        lowerDivide(instruction);
        return true;
    case SemanticOperation::OP_COUNT_LEADING_ZEROS:
    case SemanticOperation::OP_COUNT_TRAILING_ZEROS:
        lowerCountZeros(instruction);
        return true;
    case SemanticOperation::OP_POPULATION_COUNT:
        lowerPopulationCount(instruction);
        return true;
    case SemanticOperation::OP_BYTE_SWAP:
        lowerByteSwap(instruction);
        return true;
    case SemanticOperation::OP_BIT_REVERSE:
        lowerBitReverse(instruction);
        return true;
    case SemanticOperation::OP_MULTIPLY_HIGH_SIGNED_UNSIGNED:
        lowerHighSignedUnsigned(instruction);
        return true;
    case SemanticOperation::OP_ADD_CARRY:
        lowerAddCarry(instruction);
        return true;
    default:
        return false;
    }
}

void BlockLowering::defineLocalLabel(const cString& name)
{
    // The label is at the end of the code written so far
    flush();
    AMD64Lowering::LabelDefinition definition;
    definition.m_name = name;
    definition.m_offset = getOffset();
    m_localLabels.append(definition);
}

void BlockLowering::writeDivideFault()
{
    // The same exit as OP_TRAP of the fault kind, number 0
    char text[120];
    snprintf(text, sizeof(text), "mov rax, 0x%llx",
             (unsigned long long)getGuestAddress(m_index));
    line(text);
    snprintf(text, sizeof(text), "mov qword [%s+0x%x], rax",
             m_state.getASCIIstring().getBuffer(),
             (unsigned)m_map.getProgramCounterOffset());
    line(text);
    const uint64 reason = (uint64)AMD64Lowering::EXIT_TRAP |
        ((uint64)SemanticOperation::TRAP_FAULT << AMD64Lowering::EXIT_TRAP_KIND_SHIFT);
    line(getExitReasonStore(reason));
    line(cString("jmp ") + AMD64Lowering::getExitLabel());
}

void BlockLowering::lowerDivide(const SemanticInstruction& instruction)
{
    if (!m_allowLocalLabels)
        fail("a division needs its local labels: lower the block with lowerBlock()");
    const SemanticOperation::Operation operation = instruction.getOperation();
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    const SemanticOperand& dividend = instruction.getSource(0);
    const SemanticOperand& divisor = instruction.getSource(1);
    if ((getWidth(dividend.getType(), WIDTH_INTEGERS) != bits) ||
        (getWidth(divisor.getType(), WIDTH_INTEGERS) != bits))
    {
        fail("the operands have another width than the result");
    }
    const bool isSigned = (operation == SemanticOperation::OP_DIVIDE_SIGNED) ||
                          (operation == SemanticOperation::OP_REMAINDER_SIGNED);
    const bool isRemainder =
                (operation == SemanticOperation::OP_REMAINDER_SIGNED) ||
                (operation == SemanticOperation::OP_REMAINDER_UNSIGNED);
    const SemanticOperation::Behavior behavior = instruction.getBehavior();
    const bool isFault = (behavior == SemanticOperation::BEHAVIOR_DIVIDE_FAULT);
    if (!isFault && (behavior != SemanticOperation::BEHAVIOR_DIVIDE_ZERO) &&
        (behavior != SemanticOperation::BEHAVIOR_DIVIDE_ALL_ONES))
    {
        fail("the behaviour of a division by zero is not a divide behaviour");
    }

    const cString zeroLabel = getDivideLabel(m_index, 'z');
    const cString divideLabel = getDivideLabel(m_index, 'd');
    const cString endLabel = getDivideLabel(m_index, 'e');

    load(SCRATCH_A, dividend, isSigned ? EXTEND_SIGN : EXTEND_ZERO);
    load(SCRATCH_C, divisor, isSigned ? EXTEND_SIGN : EXTEND_ZERO);
    line("test rcx, rcx");
    line(cString("jz ") + zeroLabel);
    if (isSigned)
    {
        // minimum / -1 overflows the idiv: the result is the minimum (the
        // negation of rax wraps) and the remainder 0, unless it is a fault
        line("cmp rcx, -1");
        line(cString("jne ") + divideLabel);
        if (isFault)
        {
            char text[60];
            snprintf(text, sizeof(text), "mov rdx, 0x%llx",
                     (unsigned long long)getMinimum(bits));
            line(text);
            line("cmp rax, rdx");
            line(cString("je ") + zeroLabel);
        }
        line("neg rax");
        line("xor edx, edx");
        line(cString("jmp ") + endLabel);
        defineLocalLabel(divideLabel);
        line("cqo");
        line("idiv rcx");
    } else
    {
        line("xor edx, edx");
        line("div rcx");
    }
    line(cString("jmp ") + endLabel);

    // The divisor is zero
    defineLocalLabel(zeroLabel);
    if (isFault)
    {
        writeDivideFault();
    } else
    {
        // The remainder is the dividend, the quotient 0 or all ones
        line("mov rdx, rax");
        line((behavior == SemanticOperation::BEHAVIOR_DIVIDE_ZERO) ?
                                        "xor eax, eax" : "mov rax, -1");
    }
    defineLocalLabel(endLabel);
    store(destination, isRemainder ? SCRATCH_D : SCRATCH_A);
}

void BlockLowering::lowerCountZeros(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    if (getWidth(instruction.getSource(0).getType(), WIDTH_INTEGERS) != bits)
        fail("the operand has another width than the result");
    load(SCRATCH_C, instruction.getSource(0), EXTEND_ZERO);
    char text[60];
    // The zero input gives the width. The index of the lowest one bit (bsf) is
    // the count; the index of the highest (bsr) is the count of the leading
    // zeros counted from the top: bits - 1 - index = (~index) + bits
    if (instruction.getOperation() == SemanticOperation::OP_COUNT_TRAILING_ZEROS)
    {
        snprintf(text, sizeof(text), "mov eax, %u", (unsigned)bits);
        line(text);
        line("bsf rdx, rcx");
        line("cmovnz rax, rdx");
    } else
    {
        line("bsr rdx, rcx");
        line("not rdx");
        snprintf(text, sizeof(text), "lea rax, [rdx+%u]", (unsigned)bits);
        line(text);
        snprintf(text, sizeof(text), "mov ecx, %u", (unsigned)bits);
        line(text);
        line("cmovz rax, rcx");
    }
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerPopulationCount(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    if (getWidth(instruction.getSource(0).getType(), WIDTH_INTEGERS) != bits)
        fail("the operand has another width than the result");
    // The zero extended value: the sums of the SWAR method, in 2 bit, 4 bit
    // and 8 bit fields, then the sum of the bytes by a multiplication
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    char text[120];
    line("mov rdx, rax");
    line("shr rdx, 1");
    snprintf(text, sizeof(text), "mov rcx, %s", gMask55);
    line(text);
    line("and rdx, rcx");
    line("sub rax, rdx");
    line("mov rdx, rax");
    line("shr rdx, 2");
    snprintf(text, sizeof(text), "mov rcx, %s", gMask33);
    line(text);
    line("and rdx, rcx");
    line("and rax, rcx");
    line("add rax, rdx");
    line("mov rdx, rax");
    line("shr rdx, 4");
    line("add rax, rdx");
    snprintf(text, sizeof(text), "mov rcx, %s", gMask0F);
    line(text);
    line("and rax, rcx");
    snprintf(text, sizeof(text), "mov rcx, %s", gMask01);
    line(text);
    line("imul rax, rcx");
    line("shr rax, 56");
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerByteSwap(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    if (getWidth(instruction.getSource(0).getType(), WIDTH_INTEGERS) != bits)
        fail("the operand has another width than the result");
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    switch (bits)
    {
    case 8:
        // A byte is its own reversal
        break;
    case 16:
        line("rol ax, 8");
        break;
    case 32:
        line("bswap eax");
        break;
    default:
        line("bswap rax");
        break;
    }
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerBitReverse(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    if (getWidth(instruction.getSource(0).getType(), WIDTH_INTEGERS) != bits)
        fail("the operand has another width than the result");
    // The 64 bit reversal of the zero extended value: the bits are swapped in
    // pairs, pairs of pairs and nibbles, then the bytes are reversed; the
    // value is at the top of the 64 bits, so it is shifted down to its width
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    struct Step {
        uint shift;
        const char* mask;
    };
    const Step steps[3] = {
        { 1, gMask55 }, { 2, gMask33 }, { 4, gMask0F }
    };
    char text[120];
    for (uint i = 0; i < 3; i++)
    {
        line("mov rdx, rax");
        snprintf(text, sizeof(text), "shr rdx, %u", (unsigned)steps[i].shift);
        line(text);
        snprintf(text, sizeof(text), "mov rcx, %s", steps[i].mask);
        line(text);
        line("and rdx, rcx");
        line("and rax, rcx");
        snprintf(text, sizeof(text), "shl rax, %u", (unsigned)steps[i].shift);
        line(text);
        line("or rax, rdx");
    }
    line("bswap rax");
    if (bits < 64)
    {
        snprintf(text, sizeof(text), "shr rax, %u", 64u - (unsigned)bits);
        line(text);
    }
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerHighSignedUnsigned(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    // The same widths as OP_MULTIPLY_HIGH_SIGNED (the pattern of 8 bits is not
    // a native multiplication)
    if (bits < 16)
        fail("this width is not implemented");
    if ((getWidth(instruction.getSource(0).getType(), WIDTH_INTEGERS) != bits) ||
        (getWidth(instruction.getSource(1).getType(), WIDTH_INTEGERS) != bits))
    {
        fail("the operands have another width than the result");
    }
    // The high half of s * u (s signed, u unsigned) is the high half of the
    // unsigned product minus u when s is negative. The sign of s is read from
    // the operand again, once the product has overwritten rax and rdx; u is
    // still in rcx
    char text[200];
    snprintf(text, sizeof(text),
             "mov %%a, $0\nmov %%c, $1\nmul %%c\n"
             "mov %%a, $0\nsar %%a, %u\nand %%a, %%c\nsub %%x, %%a",
             (unsigned)bits - 1);
    SemanticOperand sources[SemanticInstruction::MAX_SOURCES];
    for (uint s = 0; s < SemanticInstruction::MAX_SOURCES; s++)
        sources[s] = instruction.getSource(s);
    expand(text, bits, destination, sources);
    store(destination, SCRATCH_D);
}

void BlockLowering::lowerAddCarry(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    if ((getWidth(instruction.getSource(0).getType(), WIDTH_INTEGERS) != bits) ||
        (getWidth(instruction.getSource(1).getType(), WIDTH_INTEGERS) != bits))
    {
        fail("the operands have another width than the result");
    }
    // The carry is a boolean (a byte holding 0 or 1, or a constant). Only its
    // bit 0 counts, as in the interpreter. The sum is written in full 64 bit
    // registers: the store takes the low bits of the destination
    getWidth(instruction.getSource(2).getType(), WIDTH_1);
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    load(SCRATCH_C, instruction.getSource(1), EXTEND_ZERO);
    load(SCRATCH_D, instruction.getSource(2), EXTEND_ZERO);
    line("and rdx, 1");
    line("add rax, rcx");
    line("add rax, rdx");
    store(destination, SCRATCH_A);
}
