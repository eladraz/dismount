/*
 * AMD64LoweringFloat.cpp
 *
 * The lowering of the scalar floating point operations (single and double
 * precision, one lane) onto SSE2. The code uses xmm0 and xmm1 for the values
 * and rax, rcx for the bit operations; the state and the memory base are the
 * registers of AMD64Lowering.h.
 *
 *   Section 1  arithmetic: OP_FLOAT_ADD, SUBTRACT, MULTIPLY, DIVIDE,
 *              SQUARE_ROOT (addsd/subsd/mulsd/divsd/sqrtsd and the ss forms),
 *              NEGATE and ABSOLUTE (the sign bit of the integer image).
 *              OP_FLOAT_MULTIPLY_ADD is refused: build the guest with
 *              -ffp-contract=off.
 *   Section 2  compare: OP_COMPARE of floats with the COND_FLOAT_* conditions
 *              (ucomisd/ucomiss and the parity-aware setcc).
 *   Section 3  select: OP_SELECT of floats (a bit select, cmov on the images).
 *   Section 4  conversions:
 *              OP_SIGNED_TO_FLOAT / OP_UNSIGNED_TO_FLOAT from 8, 16, 32 and
 *              64 bit integers to f32 / f64 (cvtsi2ss/sd; the unsigned 64 bit
 *              values at or above 2^63 are halved with the low bit kept as a
 *              sticky bit, converted and doubled: one rounding). The rounding
 *              is nearest even (the MXCSR default), dynamic, or a directed
 *              mode (toward -inf, +inf, zero: the rounding control of MXCSR
 *              is set around the one conversion; the sticky bit keeps the
 *              directed rounding exact too). Nearest away throws.
 *              OP_FLOAT_TO_SIGNED / OP_FLOAT_TO_UNSIGNED from f32 / f64 to 32
 *              and 64 bit integers, toward zero (cvtt), with the three
 *              out-of-range behaviors: the integer indefinite (IA-32, the
 *              default), saturation with a NaN of zero (ARM) and saturation
 *              with a NaN of the maximum (RISC-V). The bounds are exact.
 *              OP_FLOAT_CONVERT between f32 and f64 (cvtss2sd/cvtsd2ss).
 *              The fixed point counts (source 1, the fraction bits of ucvtf /
 *              fcvtzs with #fbits, 0 to 64) are lowered: an integer to float
 *              conversion is followed by the exact product with 2^-fbits (one
 *              rounding, as the interpreter's exponent shift); a float to
 *              integer conversion takes the exact product with 2^fbits before
 *              the truncation. The Lua guest uses ucvtf d0, x9, #0x35.
 *              The rounding of the float to integer conversions: the
 *              value is first rounded to an integral value in the mode of
 *              the instruction (see section 6), then truncated as above. The
 *              round is exact at the fixed point scale too (the product with
 *              2^fbits is exact), so it matches the interpreter.
 *              Not lowered: the conversions to 8 and 16 bit integers, and the
 *              rounding of the integer to float conversion nearest away
 *              throws.
 *
 *   Section 5  classify: OP_FLOAT_CLASSIFY of f32/f64 into the 10 bit mask of
 *              the RISC-V fclass instruction, bit for bit as
 *              SemanticFloat::classify (the default environment: no flush).
 *              The mask is one bit: -inf 0x001, -normal 0x002, -subnormal
 *              0x004, -0 0x008, +0 0x010, +subnormal 0x020, +normal 0x040,
 *              +inf 0x080, sNaN 0x100, qNaN 0x200. Branch free: the
 *              fraction and the exponent are compared by shifts, the class
 *              index is a cmov chain, and the result is 1 << index.
 *
 *   Section 6  round: OP_FLOAT_ROUND of f32/f64 to an integral value (frint*
 *              of A64, the rounding modes of the interpreter): nearest even
 *              (roundsd imm 8), toward -inf (9), toward +inf (10), toward zero
 *              (11), dynamic = nearest even. Nearest away (ties away from
 *              zero, frinta) is a sequence on the truncated value: the
 *              fraction x - trunc(x) is exact, and it is compared with 0.5.
 *              The round uses SSE4.1 roundsd/roundss: the host must be
 *              x86-64-v2 (the lowered code faults with SIGILL otherwise).
 *              Round to odd throws.
 *              The NaN, the zeros (with their sign), the infinities and the
 *              integral values pass unchanged in every mode.
 *
 *   Section 7  minimum and maximum: OP_FLOAT_MINIMUM / MAXIMUM with
 *              BEHAVIOR_MINMAX_NUMBER, the number rule of the signed zeros
 *              (-0 below +0) and the NaN rule of the source ISA (the block's
 *              architecture, as the interpreter's environment):
 *                RISC-V (fmin, fmax; IEEE 754-2019 minimumNumber): a NaN
 *                  operand gives the other one, both NaN the canonical NaN.
 *                ARM (FMINNM, FMAXNM with FPCR.DN = 0): both NaN give the
 *                  first signaling NaN, else the first; a single signaling
 *                  NaN gives itself quieted; a single quiet NaN gives the
 *                  number. The winner keeps its sign and payload.
 *              The branch free code is ucomis and cmov on the bit images,
 *              with bt for the quiet bit.
 *
 * Not implemented here: the x87 extended format, the rounding modes other
 * than nearest even for the arithmetic (only OP_FLOAT_ROUND and the float to
 * integer conversions take the directed modes), the behaviors other than the
 * default and BEHAVIOR_MINMAX_NUMBER (BEHAVIOR_MULTIPLY_EXTENDED, the ARM
 * minimum and maximum propagation rules BEHAVIOR_MINMAX_PROPAGATE_NAN: they
 * throw), the vector floats, and the float moves, loads and
 * stores (BITCAST and the 128 bit values). The refused ones throw
 * "AMD64 lowering: operation ...".
 *
 * The float to integer conversions are branch free (the saturation and the
 * selections are cmov), so they are lowered by lower() and by lowerBlock().
 * The rounding helper (section 6) is branch free too; it clobbers rax, rcx,
 * rdx and xmm1, xmm2, so the conversions round before they use any of them.
 * The minimum and maximum (section 7) clobber rax, rcx, rdx, r8, r9 and
 * xmm0 to xmm3 (see lowerArmNaNMinMax).
 *
 * Deviations from the SemanticInterpreter (documented, and tested only up to
 * these points):
 *   - NaN sign and payload: an invalid operation gives the x86 "default NaN"
 *     (negative, quiet). The interpreter follows the guest (ARM: positive
 *     default NaN) and the propagation rules of its environment. The tests
 *     compare NaN results by class only. The native oracle of the Lua guest
 *     normalizes the NaN sign the same way. OP_FLOAT_CONVERT of a NaN keeps
 *     the sign and the top of the payload (cvtss2sd/cvtsd2ss) and quiets a
 *     signaling NaN, as the x86 processor does.
 *   - FPSR / MXCSR flags are not updated: the invalid, divide by zero,
 *     overflow, underflow and inexact flags are not accumulated in the guest
 *     (OP_FLOAT_* with ATTRIBUTE_FLOAT_EXCEPTIONS). The exceptions are
 *     masked, so no host trap is ever raised.
 *   - Signaling compares: ucomisd/ucomiss signal only for a signaling NaN
 *     (the quiet compare). A signaling OP_COMPARE (BEHAVIOR_COMPARE_SIGNALING)
 *     gives the same boolean: only the exception flags differ, and those are
 *     not kept (see above).
 *   - Rounding: nearest even, the default of MXCSR. ROUND_DYNAMIC is taken as
 *     nearest even too, as the SemanticInterpreter does. A static directed
 *     rounding mode is refused by the arithmetic and the float conversions
 *     between formats; OP_FLOAT_ROUND, the float to integer conversions and
 *     the integer to float conversions take the directed modes (sections 4
 *     and 6).
 *   - Denormals: gradual underflow (MXCSR without FTZ and DAZ), the same as
 *     the default environment of the SemanticInterpreter.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include <stdio.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "AMD64BlockLowering.h"

using namespace amd64lowering;

namespace {

// A scalar float of single or double precision
bool isScalarSingleOrDouble(const SemanticType& type)
{
    return (type.getKind() == SemanticType::KIND_FLOAT) && !type.isVector() &&
           !type.isScalable() &&
           ((type.getFloatFormat() == SemanticType::FLOAT_SINGLE) ||
            (type.getFloatFormat() == SemanticType::FLOAT_DOUBLE));
}

// A constant operand (an immediate or a guest address)
bool isConstantOperand(const SemanticOperand& operand)
{
    return (operand.getKind() == SemanticOperand::KIND_IMMEDIATE) ||
           (operand.getKind() == SemanticOperand::KIND_GUEST_ADDRESS);
}

// The mnemonic of a two operand SSE operation of 'isDouble' precision (sd or ss)
const char* getBinaryMnemonic(SemanticOperation::Operation operation,
                              bool isDouble)
{
    switch (operation)
    {
    case SemanticOperation::OP_FLOAT_ADD:
        return isDouble ? "addsd" : "addss";
    case SemanticOperation::OP_FLOAT_SUBTRACT:
        return isDouble ? "subsd" : "subss";
    case SemanticOperation::OP_FLOAT_MULTIPLY:
        return isDouble ? "mulsd" : "mulss";
    default:
        return isDouble ? "divsd" : "divss";
    }
}

} // namespace

uint BlockLowering::getFloatWidth(const SemanticType& type) const
{
    if ((type.getKind() == SemanticType::KIND_FLOAT) && !type.isVector() &&
        !type.isScalable())
    {
        switch (type.getFloatFormat())
        {
        case SemanticType::FLOAT_SINGLE: return 32;
        case SemanticType::FLOAT_DOUBLE: return 64;
        default: break;
        }
    }
    char text[120];
    snprintf(text, sizeof(text), "the float type with %llu bits (kind %d) is "
             "not implemented", (unsigned long long)type.getTotalBits(),
             (int)type.getKind());
    fail(text);
    return 0;
}

void BlockLowering::checkFloatBehavior(const SemanticInstruction& instruction) const
{
    if (instruction.getBehavior() != SemanticOperation::BEHAVIOR_DEFAULT)
        fail("this behavior of the float operation is not implemented");
    if ((instruction.getAttributes() & SemanticInstruction::ATTRIBUTE_X87) != 0)
        fail("the x87 extended precision is not implemented");
}

void BlockLowering::checkFloatRounding(const SemanticInstruction& instruction) const
{
    // Every mode but round to odd: OP_FLOAT_ROUND and the float to integer
    // conversions take the directed and the nearest away modes too
    if (instruction.getRoundingMode() == SemanticOperation::ROUND_TO_ODD)
        fail("the rounding to odd is not implemented");
}

void BlockLowering::checkIntegerToFloatRounding(const SemanticInstruction& instruction) const
{
    switch (instruction.getRoundingMode())
    {
    case SemanticOperation::ROUND_DYNAMIC:
    case SemanticOperation::ROUND_NEAREST_EVEN:
    case SemanticOperation::ROUND_TOWARD_NEGATIVE:
    case SemanticOperation::ROUND_TOWARD_POSITIVE:
    case SemanticOperation::ROUND_TOWARD_ZERO:
        return;
    default:
        fail("the nearest away rounding of the integer to float conversion is not implemented");
    }
}

void BlockLowering::lineConvertToFloat(const char* convert, SemanticOperation::RoundingMode mode)
{
    // The rounding control of MXCSR (bits 14:13): 0 nearest even, 1 down,
    // 2 up, 3 toward zero. The original control word is saved at [rsp], the
    // changed one is loaded from [rsp+4]; both are restored after the convert
    uint control = 0;
    switch (mode)
    {
    case SemanticOperation::ROUND_DYNAMIC:
    case SemanticOperation::ROUND_NEAREST_EVEN:
        line(convert);
        return;
    case SemanticOperation::ROUND_TOWARD_NEGATIVE: control = 1; break;
    case SemanticOperation::ROUND_TOWARD_POSITIVE: control = 2; break;
    case SemanticOperation::ROUND_TOWARD_ZERO: control = 3; break;
    default:
        fail("the rounding mode of this integer to float conversion is not implemented");
    }
    char text[120];
    line("sub rsp, 8");
    line("stmxcsr dword ptr [rsp]");
    line("mov ecx, dword ptr [rsp]");
    snprintf(text, sizeof(text), "and ecx, 0x%x", 0xFFFF9FFFu);
    line(text);
    snprintf(text, sizeof(text), "or ecx, 0x%x", control << 13);
    line(text);
    line("mov dword ptr [rsp+4], ecx");
    line("ldmxcsr dword ptr [rsp+4]");
    line(convert);
    line("ldmxcsr dword ptr [rsp]");
    line("add rsp, 8");
}

void BlockLowering::checkFloatInstruction(const SemanticInstruction& instruction) const
{
    checkFloatBehavior(instruction);
    const SemanticOperation::RoundingMode rounding =
                                        instruction.getRoundingMode();
    if ((rounding != SemanticOperation::ROUND_DYNAMIC) &&
        (rounding != SemanticOperation::ROUND_NEAREST_EVEN))
    {
        fail("only the rounding to nearest even is implemented");
    }
}

void BlockLowering::roundXmm0(SemanticOperation::RoundingMode mode, uint bits)
{
    const bool isDouble = (bits == 64);
    char text[120];
    // The immediate of roundsd/roundss: the mode in bits 1:0 (nearest, down,
    // up, truncate), bit 3 suppresses the precision exception
    uint immediate = 0;
    switch (mode)
    {
    case SemanticOperation::ROUND_DYNAMIC:
    case SemanticOperation::ROUND_NEAREST_EVEN:
        immediate = 8; break;
    case SemanticOperation::ROUND_TOWARD_NEGATIVE:
        immediate = 9; break;
    case SemanticOperation::ROUND_TOWARD_POSITIVE:
        immediate = 10; break;
    case SemanticOperation::ROUND_TOWARD_ZERO:
        immediate = 11; break;
    case SemanticOperation::ROUND_NEAREST_AWAY:
        break;
    default:
        fail("the rounding mode of this float operation is not implemented");
    }
    if (mode != SemanticOperation::ROUND_NEAREST_AWAY)
    {
        snprintf(text, sizeof(text), "%s xmm0, xmm0, %u",
                 isDouble ? "roundsd" : "roundss", immediate);
        line(text);
        return;
    }

    // Nearest away (ties away from zero). t = trunc(x) (xmm1), d = x - t
    // (xmm0, exact: the fraction of a float is representable). The result is
    // t plus the sign of d when |d| >= 0.5, else t. rax: the bits of d, then
    // of t; rcx: |d|; rdx: copysign(1, d); xmm2: the increment, then t + inc
    const char* gpr = isDouble ? "rax" : "eax";
    const char* other = isDouble ? "rdx" : "edx";
    const char* counter = isDouble ? "rcx" : "ecx";
    const char* move = isDouble ? "movq" : "movd";
    const uint64 magnitudeMask = isDouble ? 0x7FFFFFFFFFFFFFFFULL : 0x7FFFFFFFULL;
    const uint64 one = isDouble ? 0x3FF0000000000000ULL : 0x3F800000ULL;
    const uint64 half = isDouble ? 0x3FE0000000000000ULL : 0x3F000000ULL;
    const uint shift = isDouble ? 63 : 31;
    snprintf(text, sizeof(text), "%s xmm1, xmm0, 11", isDouble ? "roundsd" : "roundss");
    line(text);
    line(isDouble ? "subsd xmm0, xmm1" : "subss xmm0, xmm1");
    snprintf(text, sizeof(text), "%s %s, xmm0", move, gpr);
    line(text);
    snprintf(text, sizeof(text), "mov %s, 0x%llx", counter, (unsigned long long)magnitudeMask);
    line(text);
    snprintf(text, sizeof(text), "and %s, %s", counter, gpr);       // |d|
    line(text);
    snprintf(text, sizeof(text), "shr %s, %u", gpr, shift);
    line(text);
    snprintf(text, sizeof(text), "shl %s, %u", gpr, shift);         // the sign of d
    line(text);
    snprintf(text, sizeof(text), "mov %s, 0x%llx", other, (unsigned long long)one);
    line(text);
    snprintf(text, sizeof(text), "add %s, %s", other, gpr);         // copysign(1, d)
    line(text);
    snprintf(text, sizeof(text), "mov %s, 0x%llx", gpr, (unsigned long long)half);
    line(text);
    snprintf(text, sizeof(text), "cmp %s, %s", counter, gpr);       // |d| against 0.5
    line(text);
    snprintf(text, sizeof(text), "%s %s, xmm1", move, gpr);         // t (no flags)
    line(text);
    snprintf(text, sizeof(text), "%s xmm2, %s", move, other);       // the increment
    line(text);
    line(isDouble ? "addsd xmm2, xmm1" : "addss xmm2, xmm1");       // t + inc
    snprintf(text, sizeof(text), "%s %s, xmm2", move, other);
    line(text);
    snprintf(text, sizeof(text), "cmovae %s, %s", gpr, other);      // |d| >= 0.5
    line(text);
    snprintf(text, sizeof(text), "%s xmm0, %s", move, gpr);
    line(text);
}

void BlockLowering::loadXmm(uint xmm, const SemanticOperand& operand, uint bits)
{
    char text[120];
    if (isConstantOperand(operand))
    {
        // The bits of the constant in rax, then the move to the register
        snprintf(text, sizeof(text), "mov rax, 0x%llx",
                 (unsigned long long)getConstant(operand, bits));
        line(text);
        snprintf(text, sizeof(text), "%s xmm%d, %s",
                 (bits == 64) ? "movq" : "movd", (int)xmm,
                 (bits == 64) ? "rax" : "eax");
        line(text);
        return;
    }
    cString current((bits == 64) ? "movsd " : "movss ");
    current += cString(xmm == 0 ? "xmm0, " : "xmm1, ");
    current += getMemory(operand, bits);
    line(current);
}

void BlockLowering::storeXmm(const SemanticOperand& destination, uint bits)
{
    cString current((bits == 64) ? "movsd " : "movss ");
    current += getMemory(destination, bits);
    current += cString(", xmm0");
    line(current);
}

void BlockLowering::loadBits(Scratch scratch, const SemanticOperand& operand,
                             uint bits)
{
    char text[120];
    if (isConstantOperand(operand))
    {
        snprintf(text, sizeof(text), "mov %s, 0x%llx",
                 getRegister(scratch, 64),
                 (unsigned long long)getConstant(operand, bits));
        line(text);
        return;
    }
    cString current("mov ");
    current += cString(getRegister(scratch, bits));
    current += cString(", ");
    current += getMemory(operand, bits);
    line(current);
}

void BlockLowering::storeBits(const SemanticOperand& destination,
                              Scratch scratch, uint bits)
{
    cString current("mov ");
    current += getMemory(destination, bits);
    current += cString(", ");
    current += cString(getRegister(scratch, bits));
    line(current);
}

/////////////////////////////////////////////////////////////////////////////
// Section 1: arithmetic
/////////////////////////////////////////////////////////////////////////////

void BlockLowering::lowerFloatArithmetic(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getFloatWidth(destination.getType());
    checkFloatInstruction(instruction);

    const bool isBinary = (operation == SemanticOperation::OP_FLOAT_ADD) ||
                          (operation == SemanticOperation::OP_FLOAT_SUBTRACT) ||
                          (operation == SemanticOperation::OP_FLOAT_MULTIPLY) ||
                          (operation == SemanticOperation::OP_FLOAT_DIVIDE);
    const uint sources = isBinary ? 2 : 1;
    for (uint s = 0; s < sources; s++)
    {
        if (getFloatWidth(instruction.getSource(s).getType()) != bits)
            fail("a source is not of the width of the destination");
    }

    // The sign bit of the integer image: toggled by a negation, cleared by
    // an absolute value. A NaN is changed the same way, as the interpreter does
    if ((operation == SemanticOperation::OP_FLOAT_NEGATE) ||
        (operation == SemanticOperation::OP_FLOAT_ABSOLUTE))
    {
        const bool isNegate = (operation == SemanticOperation::OP_FLOAT_NEGATE);
        loadBits(SCRATCH_A, instruction.getSource(0), bits);
        if (bits == 64)
            line(isNegate ? "btc rax, 63" : "btr rax, 63");
        else
            line(isNegate ? "btc eax, 31" : "btr eax, 31");
        storeBits(destination, SCRATCH_A, bits);
        return;
    }

    if (operation == SemanticOperation::OP_FLOAT_SQUARE_ROOT)
    {
        loadXmm(1, instruction.getSource(0), bits);
        line((bits == 64) ? "sqrtsd xmm0, xmm1" : "sqrtss xmm0, xmm1");
        storeXmm(destination, bits);
        return;
    }

    // d = s0 <op> s1. Both sources are loaded before the destination is
    // written: the destination may be one of them
    loadXmm(0, instruction.getSource(0), bits);
    loadXmm(1, instruction.getSource(1), bits);
    cString current(getBinaryMnemonic(operation, bits == 64));
    current += cString(" xmm0, xmm1");
    line(current);
    storeXmm(destination, bits);
}

/////////////////////////////////////////////////////////////////////////////
// Section 2: compare
/////////////////////////////////////////////////////////////////////////////

void BlockLowering::lowerFloatCompare(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& s0 = instruction.getSource(0);
    const SemanticOperand& s1 = instruction.getSource(1);
    const SemanticOperation::Condition condition = instruction.getCondition();
    // The result is a boolean (a mask of one lane)
    if (destination.getType().getTotalBits() != 1)
        fail("the result of a float compare is a single boolean");
    const uint bits = getFloatWidth(s0.getType());
    if (getFloatWidth(s1.getType()) != bits)
        fail("the operands of a float compare have different widths");

    if ((condition == SemanticOperation::COND_ALWAYS) ||
        (condition == SemanticOperation::COND_NEVER))
    {
        cString current("mov ");
        current += getMemory(destination, 8);
        current += cString((condition == SemanticOperation::COND_ALWAYS) ?
                           ", 1" : ", 0");
        line(current);
        return;
    }

    loadXmm(0, s0, bits);
    loadXmm(1, s1, bits);
    // ucomisd sets ZF, PF, CF: unordered 1 1 1, less 0 0 1, equal 1 0 0,
    // greater 0 0 0. "Less" is the "greater" of the swapped operands, so an
    // unordered pair (CF set) is never "greater" or "greater or equal"
    const bool isDouble = (bits == 64);
    cString compare01(isDouble ? "ucomisd xmm0, xmm1" : "ucomiss xmm0, xmm1");
    cString compare10(isDouble ? "ucomisd xmm1, xmm0" : "ucomiss xmm1, xmm0");
    switch (condition)
    {
    case SemanticOperation::COND_FLOAT_ORDERED:
        line(compare01); line("setnp al");
        break;
    case SemanticOperation::COND_FLOAT_UNORDERED:
        line(compare01); line("setp al");
        break;
    case SemanticOperation::COND_FLOAT_EQUAL:
        // equal: ZF and not PF (the unordered operands set ZF too)
        line(compare01); line("sete al"); line("setnp cl"); line("and al, cl");
        break;
    case SemanticOperation::COND_FLOAT_NOT_EQUAL:
        line(compare01); line("setne al"); line("setp cl"); line("or al, cl");
        break;
    case SemanticOperation::COND_FLOAT_LESS:
        line(compare10); line("seta al");
        break;
    case SemanticOperation::COND_FLOAT_LESS_EQUAL:
        line(compare10); line("setae al");
        break;
    case SemanticOperation::COND_FLOAT_GREATER:
        line(compare01); line("seta al");
        break;
    case SemanticOperation::COND_FLOAT_GREATER_EQUAL:
        line(compare01); line("setae al");
        break;
    case SemanticOperation::COND_FLOAT_UNORDERED_OR_EQUAL:
        line(compare01); line("sete al");
        break;
    case SemanticOperation::COND_FLOAT_UNORDERED_OR_LESS:
        line(compare01); line("setb al");
        break;
    case SemanticOperation::COND_FLOAT_UNORDERED_OR_LESS_EQUAL:
        line(compare01); line("setbe al");
        break;
    default:
        fail("this condition is not a float compare");
        return;
    }

    // A boolean is one byte: every reader tests the byte (see lowerSelect())
    storeBits(destination, SCRATCH_A, 8);
}

/////////////////////////////////////////////////////////////////////////////
// Section 3: select
/////////////////////////////////////////////////////////////////////////////

void BlockLowering::lowerFloatSelect(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& condition = instruction.getSource(0);
    const SemanticOperand& ifTrue = instruction.getSource(1);
    const SemanticOperand& ifFalse = instruction.getSource(2);
    const uint bits = getFloatWidth(destination.getType());
    if ((getFloatWidth(ifTrue.getType()) != bits) ||
        (getFloatWidth(ifFalse.getType()) != bits))
    {
        fail("a value of the select is not of the width of the destination");
    }

    if (isConstantOperand(condition))
    {
        loadBits(SCRATCH_A, (getConstant(condition, 1) != 0) ? ifTrue : ifFalse,
                 bits);
    } else
    {
        // The bits of both values, then the conditional move of the 64 bit
        // images (both loads zero the upper half of a 32 bit value)
        loadBits(SCRATCH_A, ifFalse, bits);
        loadBits(SCRATCH_C, ifTrue, bits);
        // (the condition is a boolean: its byte, the other bytes of the slot
        // are not part of the value)
        cString compare("cmp ");
        compare += getMemory(condition, 8);
        compare += cString(", 0");
        line(compare);
        line("cmovne rax, rcx");
    }
    storeBits(destination, SCRATCH_A, bits);
}

/////////////////////////////////////////////////////////////////////////////
// Section 4: conversions
/////////////////////////////////////////////////////////////////////////////

namespace {

// The bit image of a constant in the float format of 'bits' (32 or 64)
uint64 floatPattern(double value, uint bits)
{
    if (bits == 64)
    {
        union { double d; uint64 u; } image;
        image.d = value;
        return image.u;
    }
    union { float f; uint32 u; } image;
    image.f = (float)value;
    return (uint64)image.u;
}

// 2^exponent (|exponent| <= 1023): the products by two are exact
double powerOfTwo(int exponent)
{
    double value = 1.0;
    for (int i = 0; i < exponent; i++)
        value *= 2.0;
    for (int i = 0; i > exponent; i--)
        value *= 0.5;
    return value;
}

// The mask of the low 'bits' bits of an integer (32 or 64)
uint64 integerMask(uint bits)
{
    return (bits == 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
}

} // namespace

void BlockLowering::loadPatternXmm(uint xmm, uint64 pattern, uint bits)
{
    char text[120];
    snprintf(text, sizeof(text), "mov rcx, 0x%llx", (unsigned long long)pattern);
    line(text);
    snprintf(text, sizeof(text), "%s xmm%u, %s", (bits == 64) ? "movq" : "movd",
             (unsigned)xmm, (bits == 64) ? "rcx" : "ecx");
    line(text);
}

uint BlockLowering::getFractionBits(const SemanticInstruction& instruction) const
{
    const SemanticOperand& fraction = instruction.getSource(1);
    if (!fraction.isValid())
        return 0;
    if (!isConstantOperand(fraction))
        fail("the fixed point count of a conversion is not a constant");
    const uint64 count = getConstant(fraction, 64);
    if (count > 64)
        fail("the fixed point count of a conversion is more than 64 bits");
    return (uint)count;
}

void BlockLowering::loadScaledXmm(const SemanticOperand& source, uint sourceBits,
                                  uint fraction,
                                  SemanticOperation::RoundingMode mode)
{
    // The value times 2^fraction is exact: a float to integer conversion
    // takes it instead of the scaling of the interpreter's exponent. An
    // overflow to infinity gives the same out of range result. The round
    // (a mode other than toward zero) is at the integer position of the
    // scaled value, where the interpreter rounds
    loadXmm(0, source, sourceBits);
    if (fraction != 0)
    {
        loadPatternXmm(1, floatPattern(powerOfTwo((int)fraction), sourceBits),
                       sourceBits);
        line((sourceBits == 64) ? "mulsd xmm0, xmm1" : "mulss xmm0, xmm1");
    }
    if (mode != SemanticOperation::ROUND_TOWARD_ZERO)
        roundXmm0(mode, sourceBits);
}

void BlockLowering::lowerIntegerToFloat(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& source = instruction.getSource(0);
    const uint bits = getFloatWidth(destination.getType());
    const uint sourceBits = getWidth(source.getType(), WIDTH_INTEGERS);
    checkFloatBehavior(instruction);
    checkIntegerToFloatRounding(instruction);
    const SemanticOperation::RoundingMode mode = instruction.getRoundingMode();
    // The fixed point count: the result is the integer times 2^-fraction.
    // The scaling by a power of two is exact (no subnormal result: the count
    // is at most 64), so rounding then scaling is the one rounding of the
    // interpreter
    const uint fraction = getFractionBits(instruction);
    const double scale = powerOfTwo(-(int)fraction);
    const bool isSigned = (operation == SemanticOperation::OP_SIGNED_TO_FLOAT);
    const bool isDouble = (bits == 64);
    if (isSigned || (sourceBits < 64))
    {
        // The value is below 2^63 (a narrower unsigned integer is zero
        // extended): its 64 bit form converts with one rounding
        load(SCRATCH_A, source, isSigned ? EXTEND_SIGN : EXTEND_ZERO);
        lineConvertToFloat(isDouble ? "cvtsi2sd xmm0, rax" : "cvtsi2ss xmm0, rax", mode);
        if (fraction != 0)
        {
            loadPatternXmm(1, floatPattern(scale, bits), bits);
            line(isDouble ? "mulsd xmm0, xmm1" : "mulss xmm0, xmm1");
        }
        storeXmm(destination, bits);
        return;
    }

    // An unsigned 64 bit value. The halved value, with its low bit or'ed
    // back (a sticky bit), converts with one rounding; the value is doubled
    // again when its sign bit (the bit 63) was set. Both factors are exact
    char text[120];
    load(SCRATCH_A, source, EXTEND_ZERO);
    line("mov rdx, rax");
    line("shr rdx, 1");
    line("mov rcx, rax");
    line("and rcx, 1");
    line("or rdx, rcx");
    line("test rax, rax");
    line("cmovns rdx, rax");
    lineConvertToFloat(isDouble ? "cvtsi2sd xmm0, rdx" : "cvtsi2ss xmm0, rdx", mode);
    // The factor (1 or 2) by the sign of the test: the test is repeated
    // (the MXCSR change of the convert clobbers the flags); the movs keep them
    const uint64 one = floatPattern(scale, bits);
    const uint64 two = floatPattern(2.0 * scale, bits);
    snprintf(text, sizeof(text), "mov rcx, 0x%llx", (unsigned long long)one);
    line(text);
    snprintf(text, sizeof(text), "mov rdx, 0x%llx", (unsigned long long)two);
    line(text);
    line("test rax, rax");
    line("cmovs rcx, rdx");
    line(isDouble ? "movq xmm1, rcx" : "movd xmm1, ecx");
    line(isDouble ? "mulsd xmm0, xmm1" : "mulss xmm0, xmm1");
    storeXmm(destination, bits);
}

void BlockLowering::lowerFloatToInteger(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& source = instruction.getSource(0);
    const uint bits = getWidth(destination.getType(), WIDTH_32 | WIDTH_64);
    const uint sourceBits = getFloatWidth(source.getType());
    const SemanticOperation::Behavior behavior = instruction.getBehavior();
    if ((behavior != SemanticOperation::BEHAVIOR_CONVERT_INDEFINITE) &&
        (behavior != SemanticOperation::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO) &&
        (behavior != SemanticOperation::BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM))
    {
        fail("this behavior of the float to integer conversion is not implemented");
    }
    checkFloatRounding(instruction);
    if ((instruction.getAttributes() & SemanticInstruction::ATTRIBUTE_X87) != 0)
        fail("the x87 extended precision is not implemented");
    // The round of the mode (none for toward zero) comes with the scaled load
    const SemanticOperation::RoundingMode mode = instruction.getRoundingMode();
    const uint fraction = getFractionBits(instruction);

    const bool isSigned = (operation == SemanticOperation::OP_FLOAT_TO_SIGNED);
    const bool isDouble = (sourceBits == 64);
    const bool isSaturating =
                (behavior != SemanticOperation::BEHAVIOR_CONVERT_INDEFINITE);
    const bool isNanZero =
                (behavior == SemanticOperation::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO);
    const uint64 mask = integerMask(bits);
    // The results of the cases the cvtt doesn't give: the value of a
    // negative overflow (the minimum of an unsigned integer is zero), of a
    // NaN, and of a positive overflow (the maximum)
    const uint64 negativeValue = isSaturating ? 0 : mask;
    const uint64 nanValue = isNanZero ? 0 : mask;
    char text[120];
    const char* compare = isDouble ? "ucomisd xmm0, xmm1" : "ucomiss xmm0, xmm1";
    const char* compareSelf = isDouble ? "ucomisd xmm0, xmm0" : "ucomiss xmm0, xmm0";
    const char* subtract = isDouble ? "subsd xmm0, xmm1" : "subss xmm0, xmm1";
    const char* convert64 = isDouble ? "cvttsd2si rax, xmm0" : "cvttss2si rax, xmm0";

    if (isSigned)
    {
        // cvtt gives the integer indefinite (the minimum) for a NaN, a
        // positive overflow and a negative overflow. The default behavior is
        // that result. The saturation changes only the positive overflow
        // (the maximum, from 2^(bits-1) on) and the NaN, by cmov
        snprintf(text, sizeof(text), "%s %s, xmm0",
                 isDouble ? "cvttsd2si" : "cvttss2si",
                 (bits == 64) ? "rax" : "eax");
        const cString signedConvert(text);
        loadScaledXmm(source, sourceBits, fraction, mode);
        if (!isSaturating)
        {
            line(signedConvert);
            store(destination, SCRATCH_A);
            return;
        }
        const uint64 maximum = mask >> 1;
        const double bound = (bits == 64) ? 9223372036854775808.0 : 2147483648.0;
        loadPatternXmm(1, floatPattern(bound, sourceBits), sourceBits);
        snprintf(text, sizeof(text), "mov rdx, 0x%llx", (unsigned long long)maximum);
        line(text);
        line(signedConvert);
        line(compare);
        line("cmovae rax, rdx");
        snprintf(text, sizeof(text), "mov rcx, 0x%llx",
                 (unsigned long long)(isNanZero ? 0 : maximum));
        line(text);
        line(compareSelf);
        line("cmovp rax, rcx");
        store(destination, SCRATCH_A);
        return;
    }

    if (bits == 32)
    {
        // The 64 bit cvtt is exact below 2^63, so a value of 2^32 or more
        // (cmovae) is the maximum, and a negative value (cmovs) the negative
        // value. A NaN is the last cmov
        loadScaledXmm(source, sourceBits, fraction, mode);
        loadPatternXmm(1, floatPattern(4294967296.0, sourceBits), sourceBits);
        snprintf(text, sizeof(text), "mov rdx, 0x%llx", (unsigned long long)mask);
        line(text);
        line(convert64);
        line(compare);
        line("cmovae rax, rdx");
        snprintf(text, sizeof(text), "mov rcx, 0x%llx", (unsigned long long)negativeValue);
        line(text);
        line("test rax, rax");
        line("cmovs rax, rcx");
        snprintf(text, sizeof(text), "mov rcx, 0x%llx", (unsigned long long)nanValue);
        line(text);
        line(compareSelf);
        line("cmovp rax, rcx");
        store(destination, SCRATCH_A);
        return;
    }

    // An unsigned 64 bit integer, without a branch (so that lower() can
    // lower it too). The candidate of the values from 2^63 on is the signed
    // conversion of x - 2^63 (exact) with its sign bit set again: an overflow
    // (x - 2^63 >= 2^63, the integer indefinite) is the maximum. The candidate
    // of the others is the signed conversion of x, and a negative one is the
    // negative value. The compare with 2^63 selects, and a NaN is the last cmov.
    // The value is loaded (and rounded) once: xmm2 keeps it for the subtraction
    loadScaledXmm(source, sourceBits, fraction, mode);
    snprintf(text, sizeof(text), "%s %s, xmm0", isDouble ? "movq" : "movd",
             isDouble ? "rax" : "eax");
    line(text);
    snprintf(text, sizeof(text), "%s xmm2, %s", isDouble ? "movq" : "movd",
             isDouble ? "rax" : "eax");
    line(text);
    loadPatternXmm(1, floatPattern(9223372036854775808.0, sourceBits), sourceBits);
    line(isDouble ? "subsd xmm2, xmm1" : "subss xmm2, xmm1");
    line(isDouble ? "cvttsd2si rdx, xmm2" : "cvttss2si rdx, xmm2");
    line("mov rcx, 0x8000000000000000");
    line("xor rdx, rcx");            // ZF: the indefinite gave zero
    snprintf(text, sizeof(text), "mov rcx, 0x%llx", (unsigned long long)mask);
    line(text);
    line("cmovz rdx, rcx");
    line(convert64);
    snprintf(text, sizeof(text), "mov rcx, 0x%llx", (unsigned long long)negativeValue);
    line(text);
    line("test rax, rax");
    line("cmovs rax, rcx");
    loadPatternXmm(1, floatPattern(9223372036854775808.0, sourceBits), sourceBits);
    line(compare);
    line("cmovae rax, rdx");
    snprintf(text, sizeof(text), "mov rcx, 0x%llx", (unsigned long long)nanValue);
    line(text);
    line(compareSelf);
    line("cmovp rax, rcx");
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerFloatConvert(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& source = instruction.getSource(0);
    const uint bits = getFloatWidth(destination.getType());
    const uint sourceBits = getFloatWidth(source.getType());
    if (bits == sourceBits)
        fail("a float conversion between formats of the same width");
    checkFloatInstruction(instruction);
    loadXmm(0, source, sourceBits);
    line((bits == 64) ? "cvtss2sd xmm0, xmm0" : "cvtsd2ss xmm0, xmm0");
    storeXmm(destination, bits);
}

/////////////////////////////////////////////////////////////////////////////
// Section 6: round to an integral value
/////////////////////////////////////////////////////////////////////////////

void BlockLowering::lowerFloatRound(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& source = instruction.getSource(0);
    const uint bits = getFloatWidth(destination.getType());
    if (getFloatWidth(source.getType()) != bits)
        fail("a source is not of the width of the destination");
    checkFloatBehavior(instruction);
    checkFloatRounding(instruction);
    loadXmm(0, source, bits);
    roundXmm0(instruction.getRoundingMode(), bits);
    storeXmm(destination, bits);
}

/////////////////////////////////////////////////////////////////////////////
// Section 5: classify
/////////////////////////////////////////////////////////////////////////////

void BlockLowering::lowerFloatClassify(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& source = instruction.getSource(0);
    const uint bits = getFloatWidth(source.getType());
    // An integer of 8 to 64 bits: store() truncates the one bit mask as the
    // interpreter's setBits does (an 8 or 16 bit NaN class is zero)
    getWidth(destination.getType(), WIDTH_INTEGERS);

    // The fields of the format. The fraction is compared through a left
    // shift (it leaves the bits above the fraction behind), the exponent
    // through a right shift and a mask
    const uint fractionBits = (bits == 64) ? 52 : 23;
    const uint maxExponent = (bits == 64) ? 0x7FF : 0xFF;
    const uint fractionShift = 64 - fractionBits;
    const uint quietBit = fractionBits - 1;
    const uint signBit = bits - 1;

    // The class index of the interpreter's bit: -inf 0, -normal 1,
    // -subnormal 2, -0 3, +0 4, +subnormal 5, +normal 6, +inf 7, sNaN 8,
    // qNaN 9. The positive index of a finite number or infinity is 4 + its
    // class (zero 0, subnormal 1, normal 2, infinity 3), and the negative
    // index is 7 minus it. A NaN keeps 8 + its quiet bit whatever its sign.
    // The image is kept in xmm0 while rax, rcx and rdx are the scratch
    // registers (every compare is followed at once by its cmov or bit test)
    char text[120];
    loadBits(SCRATCH_A, source, bits);
    if (bits == 32)
        line("mov eax, eax");          // (a constant is not masked)
    line("movq xmm0, rax");

    // rcx = 4 for a zero, 5 for a subnormal: the fraction is zero when the
    // bits above it are shifted out to zero
    line("mov rdx, rax");
    snprintf(text, sizeof(text), "shl rdx, %llu", (unsigned long long)fractionShift);
    line(text);
    line("cmp rdx, 1");                // CF: the fraction is zero
    line("mov ecx, 5");
    line("sbb ecx, 0");

    // A nonzero exponent is a normal number: rcx = 6
    line("mov rdx, rax");
    snprintf(text, sizeof(text), "shr rdx, %llu", (unsigned long long)fractionBits);
    line(text);
    snprintf(text, sizeof(text), "and edx, %llu", (unsigned long long)maxExponent);
    line(text);                        // ZF: the exponent is zero
    line("mov edx, 6");
    line("cmovnz ecx, edx");

    // An all ones exponent: infinity (7) when the fraction is zero, else a
    // NaN (8 + the quiet bit). rdx gets the candidate, then the exponent
    // compare selects it
    snprintf(text, sizeof(text), "bt rax, %llu", (unsigned long long)quietBit);
    line(text);                        // CF: the quiet bit
    line("mov edx, 8");
    line("adc edx, 0");                // rdx = 8 + quiet bit
    snprintf(text, sizeof(text), "shl rax, %llu", (unsigned long long)fractionShift);
    line(text);                        // ZF: the fraction is zero
    line("mov eax, 7");
    line("cmovz edx, eax");            // infinity
    line("movq rax, xmm0");
    snprintf(text, sizeof(text), "shr rax, %llu", (unsigned long long)fractionBits);
    line(text);
    snprintf(text, sizeof(text), "and eax, %llu", (unsigned long long)maxExponent);
    line(text);
    snprintf(text, sizeof(text), "cmp eax, %llu", (unsigned long long)maxExponent);
    line(text);                        // ZF: the exponent is all ones
    line("cmovz ecx, edx");

    // The negative numbers: 7 minus the index, unless the index is a NaN
    // (8 or 9, kept). rdx = 7 - index, or the index for a NaN
    line("mov edx, 7");
    line("sub edx, ecx");
    line("cmp ecx, 8");                // CF: not a NaN
    line("cmovae edx, ecx");
    line("movq rax, xmm0");
    snprintf(text, sizeof(text), "bt rax, %llu", (unsigned long long)signBit);
    line(text);                        // CF: the sign
    line("cmovc ecx, edx");

    // The one bit of the class
    line("mov eax, 1");
    line("shl eax, cl");
    store(destination, SCRATCH_A);
}

/////////////////////////////////////////////////////////////////////////////
// Section 7: minimum and maximum (BEHAVIOR_MINMAX_NUMBER)
/////////////////////////////////////////////////////////////////////////////

// The number rule (branch free): rax = a<b ? a : b (a>b for the maximum; an
// unordered pair takes b), rax = a|b (a&b for the maximum) when a == b, which
// orders the signed zeros (-0 wins the minimum, +0 the maximum). The NaN part
// is the source ISA's rule (lowerRiscvNaNMinMax, lowerArmNaNMinMax), which
// overrides rax when an operand is a NaN. The loads of the constant operands
// use rax, so the xmm loads come first.
void BlockLowering::lowerFloatMinMax(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& s0 = instruction.getSource(0);
    const SemanticOperand& s1 = instruction.getSource(1);
    const uint bits = getFloatWidth(destination.getType());
    if ((getFloatWidth(s0.getType()) != bits) || (getFloatWidth(s1.getType()) != bits))
        fail("a source is not of the width of the destination");
    if (instruction.getBehavior() != SemanticOperation::BEHAVIOR_MINMAX_NUMBER)
        fail("this behavior of the minimum or maximum is not implemented");
    if ((instruction.getAttributes() & SemanticInstruction::ATTRIBUTE_X87) != 0)
        fail("the x87 extended precision is not implemented");
    const bool isMinimum = (operation == SemanticOperation::OP_FLOAT_MINIMUM);
    const bool isDouble = (bits == 64);

    // xmm0 = a, xmm1 = b (before the bit loads, which may use rax)
    loadXmm(0, s0, bits);
    loadXmm(1, s1, bits);
    // rax = a, rcx = b, rdx = a|b (a&b): the combined value of equal operands
    loadBits(SCRATCH_A, s0, bits);
    loadBits(SCRATCH_C, s1, bits);
    loadBits(SCRATCH_D, s0, bits);
    line(isMinimum ? "or rdx, rcx" : "and rdx, rcx");
    // The ordered pick: rax keeps a when a is strictly the one wanted (a < b
    // for the minimum, a > b for the maximum), else it takes b. An unordered
    // pair sets CF and ZF, so it takes b, and the NaN rules below fix it
    line(isDouble ? (isMinimum ? "ucomisd xmm1, xmm0" : "ucomisd xmm0, xmm1")
                  : (isMinimum ? "ucomiss xmm1, xmm0" : "ucomiss xmm0, xmm1"));
    line("cmovbe rax, rcx");
    // Equal operands (ZF; an unordered pair also sets ZF and is fixed below)
    line(isDouble ? "ucomisd xmm0, xmm1" : "ucomiss xmm0, xmm1");
    line("cmove rax, rdx");
    // The NaN operands. The rule is the environment's (the source ISA of the
    // block): the RISC-V one (a NaN operand gives the other one, both NaN the
    // canonical NaN) or the ARM one (FMINNM/FMAXNM with FPCR.DN = 0: the first
    // signaling NaN, else the first NaN, is the result, quieted; a number
    // gives the other NaN-free operand)
    // The rule is chosen by the source ISA (both ARM rules are the same
    // signaling-first rule, ArmArchitecture.cpp); any other ISA is refused
    switch (m_block.getArchitecture())
    {
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_BE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_BE:
        lowerArmNaNMinMax(s0, s1, bits);
        break;
    case OpcodeSubsystems::DISASSEMBLER_RISCV_64:
        lowerRiscvNaNMinMax(s0, s1, bits);
        break;
    default:
        fail("the NaN rule of this source ISA for the minimum and maximum is not implemented");
        break;
    }
    storeBits(destination, SCRATCH_A, bits);
}

// RISC-V minimumNumber / maximumNumber, the NaN part: rax holds the number
// rule's result (xmm0 = a, xmm1 = b, rcx = b bits, rdx = the combined value)
void BlockLowering::lowerRiscvNaNMinMax(const SemanticOperand& s0,
                                        const SemanticOperand& s1, uint bits)
{
    const bool isDouble = (bits == 64);
    // A NaN a gives b
    line(isDouble ? "ucomisd xmm0, xmm0" : "ucomiss xmm0, xmm0");
    line("cmovp rax, rcx");
    // rdx = a, or the canonical NaN when a is a NaN: the value that a NaN b
    // gives (both NaN: canonical). The flags of a's compare are read first
    loadBits(SCRATCH_D, s0, bits);
    line(isDouble ? "mov rcx, 0x7ff8000000000000" : "mov ecx, 0x7fc00000");
    line(isDouble ? "ucomisd xmm0, xmm0" : "ucomiss xmm0, xmm0");
    line("cmovp rdx, rcx");
    // A NaN b gives rdx
    line(isDouble ? "ucomisd xmm1, xmm1" : "ucomiss xmm1, xmm1");
    line("cmovp rax, rdx");
}

// ARM FMINNM / FMAXNM with FPCR.DN = 0 (the interpreter's NAN_SIGNALING_FIRST
// with m_isMinMaxNanSignaling). With nx, ny the NaN bits of a and b and qx, qy
// their quiet bits, the operand that wins is
//   W = nx ? (!qx | (ny & qy)) : qy
// (both NaN: the first signaling NaN, else a; one NaN: a signaling NaN wins,
// a quiet NaN gives the other, a number) and the result is that operand, with
// the quiet bit set when it is a NaN. With no NaN the number rule gives rax.
// Registers: rax, rcx, rdx, r8, r9 (scratch: not guest state); xmm2 keeps the
// number rule's result and xmm3 the NaN flag while the NaN part runs.
void BlockLowering::lowerArmNaNMinMax(const SemanticOperand& s0,
                                      const SemanticOperand& s1, uint bits)
{
    const bool isDouble = (bits == 64);
    const uint quietBit = isDouble ? 51 : 22;
    char text[120];
    // xmm2 = the number rule's result (rax)
    line(isDouble ? "movq xmm2, rax" : "movd xmm2, eax");
    // r8 = nx, r9 = ny (ucomis is unordered: PF)
    line("xor r8d, r8d");
    line(isDouble ? "ucomisd xmm0, xmm0" : "ucomiss xmm0, xmm0");
    line("setp r8b");
    line("xor r9d, r9d");
    line(isDouble ? "ucomisd xmm1, xmm1" : "ucomiss xmm1, xmm1");
    line("setp r9b");
    // xmm3 = nx | ny (a NaN in either operand)
    line("mov rcx, r8");
    line("or rcx, r9");
    line("movq xmm3, rcx");
    // rcx = qx
    loadBits(SCRATCH_C, s0, bits);
    snprintf(text, sizeof(text), "bt rcx, %u", quietBit);
    line(text);
    line("setc cl");
    line("movzx ecx, cl");
    // rax = !qx | (ny & qy): the winner when a is a NaN
    line("mov eax, ecx");
    line("xor eax, 1");
    loadBits(SCRATCH_D, s1, bits);
    snprintf(text, sizeof(text), "bt rdx, %u", quietBit);
    line(text);
    line("setc dl");
    line("movzx edx, dl");
    line("and rdx, r9");
    line("or rax, rdx");
    // rdx = qy: the winner when a is a number (nx = 0)
    loadBits(SCRATCH_D, s1, bits);
    snprintf(text, sizeof(text), "bt rdx, %u", quietBit);
    line(text);
    line("setc dl");
    line("movzx edx, dl");
    line("test r8, r8");
    line("cmovz rax, rdx");                  // rax = W (1: a wins)
    // rcx = the winner is a NaN: W ? nx : ny, shifted to the quiet bit
    line("mov rcx, r9");
    line("test rax, rax");
    line("cmovnz rcx, r8");
    snprintf(text, sizeof(text), "shl rcx, %u", quietBit);
    line(text);
    // rdx = W ? a : b (the raw bits), then the quiet bit of a NaN winner
    loadBits(SCRATCH_D, s0, bits);
    line("mov r9, rdx");
    loadBits(SCRATCH_D, s1, bits);
    line("test rax, rax");
    line("cmovnz rdx, r9");
    line("or rdx, rcx");
    // rax = the NaN winner when any operand is a NaN (xmm3), else the number
    // rule's result (xmm2)
    line("movq r8, xmm3");
    line("test r8, r8");
    line(isDouble ? "movq rax, xmm2" : "movd eax, xmm2");
    line("cmovnz rax, rdx");
}

/////////////////////////////////////////////////////////////////////////////
// Dispatch
/////////////////////////////////////////////////////////////////////////////

bool BlockLowering::lowerFloat(const SemanticInstruction& instruction)
{
    switch (instruction.getOperation())
    {
    case SemanticOperation::OP_FLOAT_ADD:
    case SemanticOperation::OP_FLOAT_SUBTRACT:
    case SemanticOperation::OP_FLOAT_MULTIPLY:
    case SemanticOperation::OP_FLOAT_DIVIDE:
    case SemanticOperation::OP_FLOAT_SQUARE_ROOT:
    case SemanticOperation::OP_FLOAT_NEGATE:
    case SemanticOperation::OP_FLOAT_ABSOLUTE:
        lowerFloatArithmetic(instruction);
        return true;
    case SemanticOperation::OP_FLOAT_MULTIPLY_ADD:
        fail("fused multiply-add is not implemented: build the guest with "
             "-ffp-contract=off");
        return true;
    case SemanticOperation::OP_COMPARE:
        if (instruction.getSource(0).getType().getKind() !=
                                                SemanticType::KIND_FLOAT)
            return false;
        lowerFloatCompare(instruction);
        return true;
    case SemanticOperation::OP_SELECT:
        // Only the scalar f32/f64 are selected here; any other value is
        // selected as its bits by lowerSelect()
        if (!isScalarSingleOrDouble(instruction.getDestination().getType()))
            return false;
        lowerFloatSelect(instruction);
        return true;
    case SemanticOperation::OP_SIGNED_TO_FLOAT:
    case SemanticOperation::OP_UNSIGNED_TO_FLOAT:
        lowerIntegerToFloat(instruction);
        return true;
    case SemanticOperation::OP_FLOAT_TO_SIGNED:
    case SemanticOperation::OP_FLOAT_TO_UNSIGNED:
        lowerFloatToInteger(instruction);
        return true;
    case SemanticOperation::OP_FLOAT_CONVERT:
        lowerFloatConvert(instruction);
        return true;
    case SemanticOperation::OP_FLOAT_CLASSIFY:
        lowerFloatClassify(instruction);
        return true;
    case SemanticOperation::OP_FLOAT_ROUND:
        lowerFloatRound(instruction);
        return true;
    case SemanticOperation::OP_FLOAT_MINIMUM:
    case SemanticOperation::OP_FLOAT_MAXIMUM:
        lowerFloatMinMax(instruction);
        return true;
    default:
        return false;
    }
}
