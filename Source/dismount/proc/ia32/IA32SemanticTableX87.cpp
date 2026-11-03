/*
 * IA32SemanticTableX87.cpp
 *
 * The semantics of the IA-32 x87 instructions, see IA32Semantics.h.
 *
 * The model:
 *   - ST(i) is the physical register FPR[(TOP + i) mod 8] (OP_READ/WRITE_
 *     REGISTER_INDEXED). The abridged tag word IA32_REG_FPU_TAG has a bit
 *     for each physical register, set when it isn't empty.
 *   - A push writes the register TOP - 1 and decrements TOP, a pop empties
 *     ST(0) and increments TOP.
 *   - Reading an empty register is a stack underflow, pushing onto a
 *     register which isn't empty is a stack overflow. Both are an invalid
 *     operation (IE) with the stack fault bit (SF) and C1 (0 for underflow, 1
 *     for overflow). When the exception is masked the result is the real
 *     indefinite (the default NaN, the integer indefinite for the integer
 *     stores), otherwise ES and B are set and the instruction ends before it
 *     changes anything.
 *   - The arithmetic runs on FLOAT_EXTENDED with the precision and rounding
 *     control of the control word (ATTRIBUTE_X87). The invalid operation,
 *     denormal operand and zero divide exceptions are faults: when one is
 *     unmasked the interpreter sets the flag, ES and B and doesn't write the
 *     result, and the lifter ends the instruction right after the operation
 *     (abortIfPending): nothing is stored, nothing is popped. The overflow,
 *     underflow and precision exceptions are traps: the result is stored and
 *     the instruction completes, ES and B are set.
 *   - Every x87 instruction but the "no wait" ones (fnstsw, fnstcw, fnstenv,
 *     fnsave, fnclex, fninit, and the fxsave pair) first checks ES for an
 *     earlier unmasked exception and raises #MF (16).
 *   - C1 of the operations which round is set when the result was rounded
 *     up (away from zero), by comparing the result with the one rounded toward
 *     zero. The loads, compares and stack operations clear it, a stack fault
 *     sets it as the SDM says.
 *   - The precision control applies to fadd, fsub, fmul, fdiv, fsqrt and
 *     their forms only (not to fscale, frndint or the conversions).
 *   - ES and B are derived from the exception flags and the masks whenever a
 *     control word or a status word is loaded (fldcw, fldenv, frstor,
 *     fxrstor); the control word keeps its defined bits (0x1F3F, bit 6 reads
 *     1).
 *   - The instruction, data pointers and opcode registers (FIP, FDP, FOP)
 *     aren't kept: the environment images store zeros.
 *   - The unmasked overflow and underflow exceptions store the masked
 *     response (infinity, denormal, zero) and not the scaled result of the
 *     processor.
 *   - An empty register that an instruction reads is the stack underflow, an
 *     fcmovcc moves into an empty ST(0) (it becomes valid).
 *
 * Not lifted exactly (OP_HELPER): the transcendental functions f2xm1, fyl2x,
 * fyl2xp1, fpatan, fptan, fsin, fcos and fsincos, whose results depend on
 * the implementation. fprem and fprem1 are exact and lifted (a loop of
 * long division steps, the partial remainder reduces by N = 32 + D mod 32
 * bits like the processors).
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
typedef SemanticInstruction Insn;

namespace {

// The bits of the status word
const uint32 SW_IE = 0x0001;
const uint32 SW_DE = 0x0002;
const uint32 SW_ZE = 0x0004;
const uint32 SW_SF = 0x0040;
const uint32 SW_ES = 0x0080;
const uint32 SW_C0 = 0x0100;
const uint32 SW_C1 = 0x0200;
const uint32 SW_C2 = 0x0400;
const uint32 SW_C3 = 0x4000;
const uint32 SW_B  = 0x8000;
// The exception flags, and the exception masks of the control word
const uint32 SW_EXCEPTIONS = 0x003F;

// The attributes of the x87 operations
const uint32 X87_ATTRIBUTES = Insn::ATTRIBUTE_X87 |
                              Insn::ATTRIBUTE_FLOAT_EXCEPTIONS;

SemanticType integerType(uint bits)
{
    return SemanticType::integer(bits);
}

SemanticType floatType(SemanticType::FloatFormat format)
{
    return SemanticType::floating(format);
}

SemanticType extendedType()
{
    return floatType(SemanticType::FLOAT_EXTENDED);
}

SemanticOperand immediate(Context& c, int64 value, uint bits)
{
    return c.getImmediate(value, integerType(bits));
}

/////////////////////////////////////////////////////////////////////////////
// Bits and flags
/////////////////////////////////////////////////////////////////////////////

SemanticOperand notOf(Context& c, const SemanticOperand& value)
{
    return c.emit(Op::OP_XOR, integerType(1), value, immediate(c, 1, 1));
}

SemanticOperand logic(Context& c, Op::Operation operation,
                      const SemanticOperand& first, const SemanticOperand& second)
{
    return c.emit(operation, integerType(1), first, second);
}

// Return a 1 bit temporary: 'first' <condition> 'second'
SemanticOperand compare(Context& c, Op::Condition condition,
                        const SemanticOperand& first,
                        const SemanticOperand& second)
{
    SemanticOperand result = c.newTemporary(integerType(1));
    Insn instruction(Op::OP_COMPARE, first.getType(), result, first, second);
    instruction.setCondition(condition);
    c.emit(instruction);
    return result;
}

// Return true if any of the 'mask' bits of the 16 bit 'value' is set
SemanticOperand isSet(Context& c, const SemanticOperand& value, uint32 mask)
{
    return compare(c, Op::COND_NOT_EQUAL,
                   c.emit(Op::OP_AND, integerType(16), value,
                          immediate(c, mask, 16)),
                   immediate(c, 0, 16));
}

SemanticOperand statusWord(Context& c)
{
    return c.getRegister(IA32_REG_FPU_STATUS);
}

SemanticOperand controlWord(Context& c)
{
    return c.getRegister(IA32_REG_FPU_CONTROL);
}

// status |= mask
void setStatus(Context& c, uint32 mask)
{
    SemanticOperand status = statusWord(c);
    c.move(status, c.emit(Op::OP_OR, integerType(16), status,
                          immediate(c, mask, 16)));
}

// status &= ~mask
void clearStatus(Context& c, uint32 mask)
{
    SemanticOperand status = statusWord(c);
    c.move(status, c.emit(Op::OP_AND, integerType(16), status,
                          immediate(c, (~mask) & 0xFFFF, 16)));
}

// C1 = value (a 1 bit operand)
void setC1(Context& c, const SemanticOperand& value)
{
    c.move(c.getRegister(IA32_REG_FPU_C1), value);
}

void setC1(Context& c, uint value)
{
    setC1(c, immediate(c, value, 1));
}

// The control word as the processor stores it: the reserved bits 7 and 13 to
// 15 read as 0, bit 6 as 1
SemanticOperand normalizeControlWord(Context& c, const SemanticOperand& value)
{
    return c.emit(Op::OP_OR, integerType(16),
                  c.emit(Op::OP_AND, integerType(16), value,
                         immediate(c, 0x1F3F, 16)),
                  immediate(c, 0x0040, 16));
}

// An x87 instruction first checks for an unmasked exception of an earlier
// instruction: #MF
void waitCheck(Context& c)
{
    SemanticOperand skip = c.newLabel();
    c.branchIf(notOf(c, isSet(c, statusWord(c), SW_ES)), skip);
    c.trap(Op::TRAP_FAULT, 16);
    c.placeLabel(skip);
}

// End the instruction if the last operation raised an unmasked invalid
// operation, denormal operand or zero divide exception (the faults: the
// instruction changes nothing). The overflow, underflow and precision
// exceptions are traps, the instruction completes. ES was clear when the
// instruction started, so no unmasked exception flag was set
void abortIfPending(Context& c, const SemanticOperand& done)
{
    SemanticOperand faults = c.emit(Op::OP_AND, integerType(16),
        c.emit(Op::OP_AND, integerType(16), statusWord(c),
               c.emit(Op::OP_NOT, integerType(16), controlWord(c))),
        immediate(c, SW_IE | SW_DE | SW_ZE, 16));
    c.branchIf(compare(c, Op::COND_NOT_EQUAL, faults, immediate(c, 0, 16)), done);
}

/*
 * Record a stack fault: IE and SF are set, C1 is 1 for an overflow and 0 for
 * an underflow. If the invalid operation exception is unmasked ES and B are
 * set and the instruction ends at 'done'. Otherwise the code continues with
 * the masked response
 */
void raiseStackFault(Context& c, bool isOverflow, const SemanticOperand& done)
{
    setStatus(c, SW_IE | SW_SF);
    setC1(c, isOverflow ? 1 : 0);
    SemanticOperand masked = c.newLabel();
    c.branchIf(isSet(c, controlWord(c), 1), masked);
    setStatus(c, SW_ES | SW_B);
    c.branch(done);
    c.placeLabel(masked);
}

/*
 * Record an exception which isn't a stack fault: its flag is set, and if the
 * exception is not masked (the mask bit of the control word) ES and B are set
 * and the instruction ends at 'done'
 */
void raiseException(Context& c, uint32 flag, uint32 mask,
                    const SemanticOperand& done)
{
    setStatus(c, flag);
    SemanticOperand masked = c.newLabel();
    c.branchIf(isSet(c, controlWord(c), mask), masked);
    setStatus(c, SW_ES | SW_B);
    c.branch(done);
    c.placeLabel(masked);
}

/////////////////////////////////////////////////////////////////////////////
// The register stack
/////////////////////////////////////////////////////////////////////////////

// Return the physical number of ST(index) in a 3 bit temporary
SemanticOperand physical(Context& c, uint index)
{
    SemanticOperand top = c.getRegister(IA32_REG_FPU_TOP);
    if (index == 0)
        return c.emit(Op::OP_MOVE, integerType(3), top);
    return c.emit(Op::OP_ADD, integerType(3), top, immediate(c, index, 3));
}

// Return the tag bit (an 8 bit mask) of the physical register
SemanticOperand tagBit(Context& c, const SemanticOperand& physicalRegister)
{
    SemanticOperand wide = c.emit(Op::OP_ZERO_EXTEND, integerType(8),
                                  physicalRegister);
    return c.emit(Op::OP_SHIFT_LEFT, integerType(8), immediate(c, 1, 8), wide);
}

// Return a 1 bit value: the physical register is empty
SemanticOperand isEmpty(Context& c, const SemanticOperand& physicalRegister)
{
    return compare(c, Op::COND_EQUAL,
                   c.emit(Op::OP_AND, integerType(8),
                          c.getRegister(IA32_REG_FPU_TAG),
                          tagBit(c, physicalRegister)),
                   immediate(c, 0, 8));
}

void setValid(Context& c, const SemanticOperand& physicalRegister)
{
    SemanticOperand tag = c.getRegister(IA32_REG_FPU_TAG);
    c.move(tag, c.emit(Op::OP_OR, integerType(8), tag,
                       tagBit(c, physicalRegister)));
}

void setEmpty(Context& c, const SemanticOperand& physicalRegister)
{
    SemanticOperand tag = c.getRegister(IA32_REG_FPU_TAG);
    c.move(tag, c.emit(Op::OP_AND, integerType(8), tag,
                       c.emit(Op::OP_NOT, integerType(8),
                              tagBit(c, physicalRegister))));
}

// Return the 80 bit float of a physical register
SemanticOperand readRegister(Context& c, const SemanticOperand& physicalRegister)
{
    SemanticOperand bits = c.newTemporary(integerType(80));
    c.emit(Insn(Op::OP_READ_REGISTER_INDEXED, integerType(80), bits,
                c.getRegister(IA32_REG_FPR0), physicalRegister,
                immediate(c, 8, 32)));
    return c.emit(Op::OP_BITCAST, extendedType(), bits);
}

// Write an 80 bit float into a physical register (the tag isn't changed)
void writeRegister(Context& c, const SemanticOperand& physicalRegister,
                   const SemanticOperand& value)
{
    SemanticOperand bits = c.emit(Op::OP_BITCAST, integerType(80), value);
    c.emit(Insn(Op::OP_WRITE_REGISTER_INDEXED, integerType(80),
                c.getRegister(IA32_REG_FPR0), physicalRegister,
                immediate(c, 8, 32), bits));
}

// Return the 80 bit float given by its sign and exponent word, and its
// significand
SemanticOperand makeExtended(Context& c, uint32 signExponent, uint64 significand)
{
    SemanticOperand bits = c.emit(Op::OP_CONCAT, integerType(80),
                                  immediate(c, (int64)significand, 64),
                                  immediate(c, signExponent, 16));
    return c.emit(Op::OP_BITCAST, extendedType(), bits);
}

// The real indefinite: the negative quiet NaN with only the quiet bit
SemanticOperand indefinite(Context& c)
{
    return makeExtended(c, 0xFFFF, 0xC000000000000000ULL);
}

// TOP = TOP - 1, TOP = TOP + 1
void decrementTop(Context& c)
{
    SemanticOperand top = c.getRegister(IA32_REG_FPU_TOP);
    c.move(top, c.emit(Op::OP_SUBTRACT, integerType(3), top, immediate(c, 1, 3)));
}

void incrementTop(Context& c)
{
    SemanticOperand top = c.getRegister(IA32_REG_FPU_TOP);
    c.move(top, c.emit(Op::OP_ADD, integerType(3), top, immediate(c, 1, 3)));
}

// Push a value. The caller checked the overflow
void pushValue(Context& c, const SemanticOperand& value)
{
    SemanticOperand newTop = physical(c, 7);
    writeRegister(c, newTop, value);
    setValid(c, newTop);
    decrementTop(c);
}

// Pop the register stack
void popStack(Context& c)
{
    setEmpty(c, physical(c, 0));
    incrementTop(c);
}

// Return the index i of the operand 'operand', which is ST(i)
uint stIndex(Context& c, uint operand)
{
    return c.getOperand(operand).m_register - IA32_REG_FPR0;
}

bool isMemoryOperand(Context& c, uint operand)
{
    return (c.getOperandsCount() > operand) &&
           (c.getOperand(operand).m_kind == IA32DecodedOperand::KIND_MEMORY);
}

/*
 * Return the size in bits of the memory operand 0 of an x87 instruction: 16
 * (m16int), 32 (m32fp, m32int), 64 (m64fp, m64int) or 80. The decoder gives
 * the operand size of the instruction (16 in real mode, or with the 66
 * prefix) to the "dword" operands, which are always 32 bits
 */
uint memoryBits(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    if ((bits == 64) || (bits == 80))
        return bits;
    const uint opcode = IA32OperandDecoder::getLastOpcodeByte(c.getInstruction());
    return ((opcode == 0xDE) || (opcode == 0xDF)) ? 16 : 32;
}

/////////////////////////////////////////////////////////////////////////////
// Floating point operations
/////////////////////////////////////////////////////////////////////////////

/*
 * Emit a floating point operation of the type 'type' in the x87 environment.
 * 'raises' selects whether the exceptions it raises are recorded in the
 * status word
 */
SemanticOperand floatOperation(Context& c, Op::Operation operation,
                               const SemanticType& type,
                               const SemanticOperand& first,
                               const SemanticOperand& second = SemanticOperand(),
                               Op::RoundingMode mode = Op::ROUND_DYNAMIC,
                               bool raises = true)
{
    SemanticOperand result = c.newTemporary(type);
    Insn instruction(operation, type, result, first, second);
    instruction.setRoundingMode(mode);
    instruction.setAttributes(raises ? X87_ATTRIBUTES :
                                       (uint32)Insn::ATTRIBUTE_X87);
    c.emit(instruction);
    return result;
}

SemanticOperand floatCompare(Context& c, Op::Condition condition,
                             const SemanticOperand& first,
                             const SemanticOperand& second, bool raises)
{
    SemanticOperand result = c.newTemporary(integerType(1));
    Insn instruction(Op::OP_COMPARE, first.getType(), result, first, second);
    instruction.setCondition(condition);
    instruction.setAttributes(raises ? X87_ATTRIBUTES :
                                       (uint32)Insn::ATTRIBUTE_X87);
    c.emit(instruction);
    return result;
}

// Return a 1 bit value: |rounded| > |truncated|, the result was rounded up
SemanticOperand isRoundedUp(Context& c, const SemanticOperand& rounded,
                            const SemanticOperand& truncated)
{
    const SemanticType type = rounded.getType();
    return floatCompare(c, Op::COND_FLOAT_GREATER,
            floatOperation(c, Op::OP_FLOAT_ABSOLUTE, type, rounded,
                           SemanticOperand(), Op::ROUND_DYNAMIC, false),
            floatOperation(c, Op::OP_FLOAT_ABSOLUTE, type, truncated,
                           SemanticOperand(), Op::ROUND_DYNAMIC, false),
            false);
}

/*
 * Compute 'operation' of first and second, and set C1 to the rounded up
 * indication. The result is returned. If the operation raised an unmasked
 * exception the code continues at 'done'
 */
SemanticOperand roundingOperation(Context& c, Op::Operation operation,
                                  const SemanticType& type,
                                  const SemanticOperand& first,
                                  const SemanticOperand& second,
                                  const SemanticOperand& done)
{
    SemanticOperand result = floatOperation(c, operation, type, first, second);
    abortIfPending(c, done);
    SemanticOperand truncated = floatOperation(c, operation, type, first,
                    second, Op::ROUND_TOWARD_ZERO, false);
    setC1(c, isRoundedUp(c, result, truncated));
    return result;
}

// Convert a float from memory (32 or 64 bit) to the extended format
SemanticOperand loadFloatOperand(Context& c, const SemanticOperand& address,
                                 uint bits)
{
    if (bits == 80)
        return c.emit(Op::OP_BITCAST, extendedType(),
                      c.load(address, integerType(80)));
    SemanticOperand value = c.load(address, floatType(bits == 32 ?
                    SemanticType::FLOAT_SINGLE : SemanticType::FLOAT_DOUBLE));
    return floatOperation(c, Op::OP_FLOAT_CONVERT, extendedType(), value);
}

/*
 * Convert a float from memory to the extended format for an arithmetic
 * operation. A NaN keeps its payload and its signaling bit (the operation
 * raises the invalid exception for a signaling NaN and chooses between the
 * NaN operands with the signaling NaN as it was: a quiet NaN wins over a
 * signaling one), which the converting load (fld) does not
 */
SemanticOperand loadArithmeticOperand(Context& c, const SemanticOperand& address,
                                      uint bits)
{
    const SemanticType type = floatType(bits == 32 ?
                    SemanticType::FLOAT_SINGLE : SemanticType::FLOAT_DOUBLE);
    SemanticOperand value = c.load(address, type);
    SemanticOperand converted = floatOperation(c, Op::OP_FLOAT_CONVERT,
                                               extendedType(), value);
    SemanticOperand cls = c.newTemporary(integerType(32));
    c.emit(Insn(Op::OP_FLOAT_CLASSIFY, integerType(32), cls, value));
    SemanticOperand isNan = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(32), cls, immediate(c, 0x300, 32)),
        immediate(c, 0, 32));

    // The same NaN in the extended format: the sign, the maximum exponent,
    // the integer bit and the fraction shifted to the top
    SemanticOperand raw = c.emit(Op::OP_BITCAST, integerType(bits), value);
    SemanticOperand wide = c.emit(Op::OP_ZERO_EXTEND, integerType(64), raw);
    const uint fractionBits = (bits == 32) ? 23 : 52;
    SemanticOperand fraction = c.emit(Op::OP_AND, integerType(64), wide,
        immediate(c, (((int64)1) << fractionBits) - 1, 64));
    SemanticOperand significand = c.emit(Op::OP_OR, integerType(64),
        c.emit(Op::OP_SHIFT_LEFT, integerType(64), fraction,
               immediate(c, 63 - fractionBits, 64)),
        immediate(c, (int64)0x8000000000000000ULL, 64));
    SemanticOperand sign = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, integerType(16),
        c.emit(Op::OP_TRUNCATE, integerType(16),
               c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, integerType(bits), raw,
                      immediate(c, bits - 16, bits))),
        immediate(c, 15, 16));
    SemanticOperand signExponent = c.emit(Op::OP_OR, integerType(16),
        c.emit(Op::OP_SHIFT_LEFT, integerType(16), sign, immediate(c, 15, 16)),
        immediate(c, 0x7FFF, 16));
    SemanticOperand nanBits = c.emit(Op::OP_CONCAT, integerType(80),
                                     significand, signExponent);
    return c.emit(Op::OP_SELECT, extendedType(), isNan,
                  c.emit(Op::OP_BITCAST, extendedType(), nanBits), converted);
}

// Convert an integer from memory (16, 32 or 64 bit) to the extended format
SemanticOperand loadIntegerOperand(Context& c, const SemanticOperand& address,
                                   uint bits)
{
    SemanticOperand value = c.load(address, integerType(bits));
    return floatOperation(c, Op::OP_SIGNED_TO_FLOAT, extendedType(), value,
                          SemanticOperand(), Op::ROUND_DYNAMIC, false);
}

/////////////////////////////////////////////////////////////////////////////
// Data transfer
/////////////////////////////////////////////////////////////////////////////

// fld m32 / m64 / m80 / ST(i)
void liftLoad(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand overflow = c.newLabel();
    const bool isMemory = isMemoryOperand(c, 0);
    SemanticOperand address;
    if (isMemory)
        address = c.getAddress(0);

    SemanticOperand source;
    SemanticOperand underflow;
    if (!isMemory)
    {
        source = physical(c, stIndex(c, 0));
        underflow = c.newLabel();
    }
    c.branchIf(notOf(c, isEmpty(c, physical(c, 7))), overflow);
    if (!isMemory)
        c.branchIf(isEmpty(c, source), underflow);

    SemanticOperand value = isMemory ?
            loadFloatOperand(c, address, memoryBits(c)) :
            readRegister(c, source);
    abortIfPending(c, done);
    setC1(c, 0);
    pushValue(c, value);
    c.branch(done);

    c.placeLabel(overflow);
    raiseStackFault(c, true, done);
    pushValue(c, indefinite(c));
    c.branch(done);

    if (!isMemory)
    {
        c.placeLabel(underflow);
        raiseStackFault(c, false, done);
        pushValue(c, indefinite(c));
    }
    c.placeLabel(done);
}

// The significand of the real indefinite stored by fst/fstp m32/m64
SemanticOperand narrowIndefinite(Context& c, uint bits)
{
    if (bits == 32)
        return c.emit(Op::OP_BITCAST, floatType(SemanticType::FLOAT_SINGLE),
                      immediate(c, 0xFFC00000, 32));
    return c.emit(Op::OP_BITCAST, floatType(SemanticType::FLOAT_DOUBLE),
                  immediate(c, (int64)0xFFF8000000000000ULL, 64));
}

// fst / fstp m32 / m64 / m80 / ST(i)
template<bool IS_POP>
void liftStore(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    const bool isMemory = isMemoryOperand(c, 0);
    SemanticOperand address;
    uint bits = 80;
    SemanticOperand destination;
    if (isMemory)
    {
        address = c.getAddress(0);
        bits = memoryBits(c);
    } else
    {
        destination = physical(c, stIndex(c, 0));
    }
    SemanticOperand top = physical(c, 0);
    c.branchIf(isEmpty(c, top), underflow);
    SemanticOperand value = readRegister(c, top);

    if (!isMemory)
    {
        writeRegister(c, destination, value);
        setValid(c, destination);
        setC1(c, 0);
    } else if (bits == 80)
    {
        c.store(address, c.emit(Op::OP_BITCAST, integerType(80), value));
        setC1(c, 0);
    } else
    {
        const SemanticType type = floatType(bits == 32 ?
                    SemanticType::FLOAT_SINGLE : SemanticType::FLOAT_DOUBLE);
        SemanticOperand narrow = c.newTemporary(type);
        {
            // The store of a denormal source doesn't raise the denormal exception
            Insn instruction(Op::OP_FLOAT_CONVERT, type, narrow, value);
            instruction.setRoundingMode(Op::ROUND_DYNAMIC);
            instruction.setAttributes(X87_ATTRIBUTES |
                                      Insn::ATTRIBUTE_IGNORE_DENORMAL);
            c.emit(instruction);
        }
        abortIfPending(c, done);
        SemanticOperand truncated = floatOperation(c, Op::OP_FLOAT_CONVERT,
                type, value, SemanticOperand(), Op::ROUND_TOWARD_ZERO, false);
        setC1(c, isRoundedUp(c, narrow, truncated));
        c.store(address, narrow);
    }
    if (IS_POP)
        popStack(c);
    c.branch(done);

    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    if (!isMemory)
    {
        writeRegister(c, destination, indefinite(c));
        setValid(c, destination);
    } else if (bits == 80)
    {
        c.store(address, c.emit(Op::OP_BITCAST, integerType(80),
                                indefinite(c)));
    } else
    {
        c.store(address, narrowIndefinite(c, bits));
    }
    if (IS_POP)
        popStack(c);
    c.placeLabel(done);
}

// fild m16 / m32 / m64
void liftLoadInteger(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand overflow = c.newLabel();
    SemanticOperand address = c.getAddress(0);
    c.branchIf(notOf(c, isEmpty(c, physical(c, 7))), overflow);
    SemanticOperand value = loadIntegerOperand(c, address,
                                               memoryBits(c));
    setC1(c, 0);
    pushValue(c, value);
    c.branch(done);
    c.placeLabel(overflow);
    raiseStackFault(c, true, done);
    pushValue(c, indefinite(c));
    c.placeLabel(done);
}

// Return the integer indefinite: the most negative number of 'bits' bits
SemanticOperand integerIndefinite(Context& c, uint bits)
{
    return c.getImmediate(-((int64)1 << (bits - 1)), integerType(bits));
}

// fist / fistp / fisttp m16 / m32 / m64
template<bool IS_POP, bool IS_TRUNCATING>
void liftStoreInteger(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand address = c.getAddress(0);
    const uint bits = memoryBits(c);
    const SemanticType type = integerType(bits);
    const Op::RoundingMode mode = IS_TRUNCATING ? Op::ROUND_TOWARD_ZERO :
                                                  Op::ROUND_DYNAMIC;
    SemanticOperand top = physical(c, 0);
    c.branchIf(isEmpty(c, top), underflow);
    SemanticOperand value = readRegister(c, top);

    SemanticOperand result = floatOperation(c, Op::OP_FLOAT_TO_SIGNED, type,
                                            value, SemanticOperand(), mode);
    abortIfPending(c, done);
    if (IS_TRUNCATING)
    {
        setC1(c, 0);
    } else
    {
        // Rounded up: the value rounded by the control word is larger than
        // the truncated one, and the result is valid (it converts back to the
        // rounded value)
        SemanticOperand rounded = floatOperation(c, Op::OP_FLOAT_ROUND,
                extendedType(), value, SemanticOperand(), Op::ROUND_DYNAMIC,
                false);
        SemanticOperand truncated = floatOperation(c, Op::OP_FLOAT_ROUND,
                extendedType(), value, SemanticOperand(),
                Op::ROUND_TOWARD_ZERO, false);
        SemanticOperand back = floatOperation(c, Op::OP_SIGNED_TO_FLOAT,
                extendedType(), result, SemanticOperand(), Op::ROUND_DYNAMIC,
                false);
        SemanticOperand isValid = floatCompare(c, Op::COND_FLOAT_EQUAL, back,
                                               rounded, false);
        setC1(c, logic(c, Op::OP_AND, isValid,
                       isRoundedUp(c, rounded, truncated)));
    }
    c.store(address, result);
    if (IS_POP || IS_TRUNCATING)
        popStack(c);
    c.branch(done);

    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    c.store(address, integerIndefinite(c, bits));
    if (IS_POP || IS_TRUNCATING)
        popStack(c);
    c.placeLabel(done);
}

// fxch ST(i)
void liftExchange(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand ok = c.newLabel();
    SemanticOperand first = physical(c, 0);
    SemanticOperand second = physical(c, stIndex(c, 0));
    SemanticOperand firstEmpty = isEmpty(c, first);
    SemanticOperand secondEmpty = isEmpty(c, second);
    setC1(c, 0);
    c.branchIf(notOf(c, logic(c, Op::OP_OR, firstEmpty, secondEmpty)), ok);
    // An empty register gets the indefinite
    raiseStackFault(c, false, done);
    c.placeLabel(ok);
    SemanticOperand a = c.emit(Op::OP_SELECT, extendedType(), firstEmpty,
                               indefinite(c), readRegister(c, first));
    SemanticOperand b = c.emit(Op::OP_SELECT, extendedType(), secondEmpty,
                               indefinite(c), readRegister(c, second));
    writeRegister(c, first, b);
    writeRegister(c, second, a);
    setValid(c, first);
    setValid(c, second);
    c.placeLabel(done);
}

// fcmovcc ST(0), ST(i)
template<int CONDITION>
void liftConditionalMove(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand first = physical(c, 0);
    SemanticOperand second = physical(c, stIndex(c, 1));
    // The source must not be empty (the destination may be, it is overwritten)
    c.branchIf(isEmpty(c, second), underflow);
    setC1(c, 0);
    SemanticOperand skip = c.newLabel();
    c.branchIf(notOf(c, c.testFlags((Op::Condition)CONDITION)), skip);
    writeRegister(c, first, readRegister(c, second));
    setValid(c, first);
    c.placeLabel(skip);
    c.branch(done);
    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    writeRegister(c, first, indefinite(c));
    setValid(c, first);
    c.placeLabel(done);
}

// ffree ST(i), ffreep ST(i)
template<bool IS_POP>
void liftFree(Context& c)
{
    waitCheck(c);
    setEmpty(c, physical(c, stIndex(c, 0)));
    if (IS_POP)
        popStack(c);
}

// fincstp, fdecstp
template<bool IS_INCREMENT>
void liftStepTop(Context& c)
{
    waitCheck(c);
    setC1(c, 0);
    if (IS_INCREMENT)
        incrementTop(c);
    else
        decrementTop(c);
}

/*
 * The constants. The values are those of the "round to nearest" control
 * word. The processor rounds the constant by the rounding control:
 *   l2e, pi, lg2, ln2 are rounded up by the nearest mode, so toward zero and
 *     toward negative infinity give the significand - 1
 *   l2t is rounded down by the nearest mode, so toward positive infinity
 *     gives the significand + 1
 */
struct X87Constant {
    uint32 m_signExponent;
    uint64 m_significand;
    // True if the nearest mode rounds the constant up
    bool m_isRoundedUp;
};

const X87Constant gConstantOne = { 0x3FFF, 0x8000000000000000ULL, false };
const X87Constant gConstantZero = { 0x0000, 0x0000000000000000ULL, false };
const X87Constant gConstantL2T = { 0x4000, 0xD49A784BCD1B8AFEULL, false };
const X87Constant gConstantL2E = { 0x3FFF, 0xB8AA3B295C17F0BCULL, true };
const X87Constant gConstantPi  = { 0x4000, 0xC90FDAA22168C235ULL, true };
const X87Constant gConstantLg2 = { 0x3FFD, 0x9A209A84FBCFF799ULL, true };
const X87Constant gConstantLn2 = { 0x3FFE, 0xB17217F7D1CF79ACULL, true };

template<const X87Constant* CONSTANT, bool IS_EXACT>
void liftLoadConstant(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand overflow = c.newLabel();
    c.branchIf(notOf(c, isEmpty(c, physical(c, 7))), overflow);
    setC1(c, 0);
    SemanticOperand significand = immediate(c, (int64)CONSTANT->m_significand,
                                            64);
    if (!IS_EXACT)
    {
        // rc = control word bits 10..11: 0 nearest, 1 down, 2 up, 3 zero
        SemanticOperand rc = c.emit(Op::OP_AND, integerType(16),
                c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, integerType(16),
                       controlWord(c), immediate(c, 10, 16)),
                immediate(c, 3, 16));
        SemanticOperand adjust;
        if (CONSTANT->m_isRoundedUp)
        {
            // down (1) and toward zero (3): one less
            SemanticOperand isLower = compare(c, Op::COND_NOT_EQUAL,
                c.emit(Op::OP_AND, integerType(16), rc, immediate(c, 1, 16)),
                immediate(c, 0, 16));
            adjust = c.emit(Op::OP_SELECT, integerType(64), isLower,
                            immediate(c, -1, 64), immediate(c, 0, 64));
        } else
        {
            // up (2): one more
            SemanticOperand isUp = compare(c, Op::COND_EQUAL, rc,
                                           immediate(c, 2, 16));
            adjust = c.emit(Op::OP_SELECT, integerType(64), isUp,
                            immediate(c, 1, 64), immediate(c, 0, 64));
        }
        significand = c.emit(Op::OP_ADD, integerType(64), significand, adjust);
    }
    SemanticOperand bits = c.emit(Op::OP_CONCAT, integerType(80), significand,
                        immediate(c, CONSTANT->m_signExponent, 16));
    pushValue(c, c.emit(Op::OP_BITCAST, extendedType(), bits));
    c.branch(done);
    c.placeLabel(overflow);
    raiseStackFault(c, true, done);
    pushValue(c, indefinite(c));
    c.placeLabel(done);
}

// fchs, fabs, and the other unary operations which don't round
template<int OPERATION>
void liftSign(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand top = physical(c, 0);
    c.branchIf(isEmpty(c, top), underflow);
    setC1(c, 0);
    writeRegister(c, top, floatOperation(c, (Op::Operation)OPERATION,
            extendedType(), readRegister(c, top), SemanticOperand(),
            Op::ROUND_DYNAMIC, false));
    c.branch(done);
    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    writeRegister(c, top, indefinite(c));
    setValid(c, top);
    c.placeLabel(done);
}

/////////////////////////////////////////////////////////////////////////////
// Arithmetic
/////////////////////////////////////////////////////////////////////////////

/*
 * fadd, fsub, fsubr, fmul, fdiv, fdivr and their pop and integer forms.
 *   one operand:  ST(0) = ST(0) op memory   (m32, m64, m16int, m32int)
 *   two operands: op0 = op0 op op1          (reversed: op1 op op0)
 */
template<int OPERATION, bool IS_REVERSE, bool IS_POP, bool IS_INTEGER>
void liftArithmetic(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    const bool isMemory = isMemoryOperand(c, 0);
    SemanticOperand address;
    if (isMemory)
        address = c.getAddress(0);
    SemanticOperand destination = physical(c, isMemory ? 0 : stIndex(c, 0));
    SemanticOperand source;
    SemanticOperand isUnderflow = isEmpty(c, destination);
    if (!isMemory)
    {
        source = physical(c, stIndex(c, 1));
        isUnderflow = logic(c, Op::OP_OR, isUnderflow, isEmpty(c, source));
    }
    c.branchIf(isUnderflow, underflow);

    SemanticOperand a = readRegister(c, destination);
    SemanticOperand b;
    if (!isMemory)
    {
        b = readRegister(c, source);
    } else if (IS_INTEGER)
    {
        b = loadIntegerOperand(c, address, memoryBits(c));
    } else
    {
        b = loadArithmeticOperand(c, address, memoryBits(c));
        abortIfPending(c, done);
    }
    SemanticOperand result = roundingOperation(c, (Op::Operation)OPERATION,
            extendedType(), IS_REVERSE ? b : a, IS_REVERSE ? a : b, done);
    writeRegister(c, destination, result);
    if (IS_POP)
        popStack(c);
    c.branch(done);

    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    writeRegister(c, destination, indefinite(c));
    setValid(c, destination);
    if (IS_POP)
        popStack(c);
    c.placeLabel(done);
}

// fsqrt
void liftSquareRoot(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand top = physical(c, 0);
    c.branchIf(isEmpty(c, top), underflow);
    SemanticOperand result = roundingOperation(c, Op::OP_FLOAT_SQUARE_ROOT,
            extendedType(), readRegister(c, top), SemanticOperand(), done);
    writeRegister(c, top, result);
    c.branch(done);
    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    writeRegister(c, top, indefinite(c));
    setValid(c, top);
    c.placeLabel(done);
}

// frndint
void liftRoundToInteger(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand top = physical(c, 0);
    c.branchIf(isEmpty(c, top), underflow);
    SemanticOperand value = readRegister(c, top);
    SemanticOperand result = c.newTemporary(extendedType());
    {
        Insn instruction(Op::OP_FLOAT_ROUND, extendedType(), result, value);
        instruction.setKind(Op::FLOAT_ROUND_SIGNAL_INEXACT);
        instruction.setRoundingMode(Op::ROUND_DYNAMIC);
        instruction.setAttributes(X87_ATTRIBUTES);
        c.emit(instruction);
    }
    abortIfPending(c, done);
    SemanticOperand truncated = c.newTemporary(extendedType());
    {
        Insn instruction(Op::OP_FLOAT_ROUND, extendedType(), truncated, value);
        instruction.setKind(Op::FLOAT_ROUND_SUPPRESS_INEXACT);
        instruction.setRoundingMode(Op::ROUND_TOWARD_ZERO);
        instruction.setAttributes(Insn::ATTRIBUTE_X87);
        c.emit(instruction);
    }
    setC1(c, isRoundedUp(c, result, truncated));
    writeRegister(c, top, result);
    c.branch(done);
    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    writeRegister(c, top, indefinite(c));
    setValid(c, top);
    c.placeLabel(done);
}

// fscale: ST(0) = ST(0) * 2^trunc(ST(1))
void liftScale(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand first = physical(c, 0);
    SemanticOperand second = physical(c, 1);
    c.branchIf(logic(c, Op::OP_OR, isEmpty(c, first), isEmpty(c, second)),
               underflow);
    SemanticOperand a = readRegister(c, first);
    SemanticOperand b = readRegister(c, second);
    // The exceptions of the operands (a signaling NaN, an unsupported format
    // or a denormal number), none if one of them is a NaN
    floatCompare(c, Op::COND_FLOAT_UNORDERED, a, b, true);
    abortIfPending(c, done);
    // The scale is the integer part of ST(1)
    SemanticOperand count = c.newTemporary(extendedType());
    {
        Insn instruction(Op::OP_FLOAT_ROUND, extendedType(), count, b);
        instruction.setKind(Op::FLOAT_ROUND_SUPPRESS_INEXACT);
        instruction.setRoundingMode(Op::ROUND_TOWARD_ZERO);
        instruction.setAttributes(Insn::ATTRIBUTE_X87);
        c.emit(instruction);
    }
    SemanticOperand result = roundingOperation(c, Op::OP_FLOAT_SCALE,
                                    extendedType(), a, count, done);
    writeRegister(c, first, result);
    c.branch(done);
    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    writeRegister(c, first, indefinite(c));
    setValid(c, first);
    c.placeLabel(done);
}

// The transcendental functions: a runtime helper does the instruction
template<int HELPER>
void liftHelper(Context& c)
{
    waitCheck(c);
    c.emit(Insn(Op::OP_HELPER, SemanticType(), SemanticOperand(),
                immediate(c, HELPER, 32)));
}


/////////////////////////////////////////////////////////////////////////////
// Compare
/////////////////////////////////////////////////////////////////////////////

// Set C0, C2 and C3 from the relation of a and b (an unordered relation sets
// all three). 'isQuiet' selects fucom: only a signaling NaN raises the
// invalid operation exception, fcom raises it for a quiet NaN too. If the
// compare raised an unmasked exception the code continues at 'done'
void compareToConditionCodes(Context& c, const SemanticOperand& a,
                             const SemanticOperand& b, bool isQuiet,
                             const SemanticOperand& done)
{
    // The compare which records the exceptions (IE, DE)
    floatCompare(c, isQuiet ? Op::COND_FLOAT_UNORDERED : Op::COND_FLOAT_LESS,
                 a, b, true);
    abortIfPending(c, done);
    SemanticOperand isLess = floatCompare(c, Op::COND_FLOAT_LESS, a, b, false);
    SemanticOperand isEqual = floatCompare(c, Op::COND_FLOAT_EQUAL, a, b, false);
    SemanticOperand isUnordered = floatCompare(c, Op::COND_FLOAT_UNORDERED, a, b,
                                               false);
    setC1(c, 0);
    c.move(c.getRegister(IA32_REG_FPU_C0),
           logic(c, Op::OP_OR, isLess, isUnordered));
    c.move(c.getRegister(IA32_REG_FPU_C2), isUnordered);
    c.move(c.getRegister(IA32_REG_FPU_C3),
           logic(c, Op::OP_OR, isEqual, isUnordered));
}

// The condition codes of the masked stack underflow: unordered
void setUnorderedConditionCodes(Context& c)
{
    c.move(c.getRegister(IA32_REG_FPU_C0), immediate(c, 1, 1));
    c.move(c.getRegister(IA32_REG_FPU_C2), immediate(c, 1, 1));
    c.move(c.getRegister(IA32_REG_FPU_C3), immediate(c, 1, 1));
}

/*
 * fcom, fcomp, fcompp, fucom, fucomp, fucompp, ficom, ficomp, ftst:
 *   no operand:   ST(0) with ST(1) (fcompp, fucompp)
 *   a register:   ST(0) with ST(i)
 *   memory:       ST(0) with m32, m64, m16int or m32int
 */
template<bool IS_QUIET, bool IS_INTEGER, uint POPS, bool IS_ZERO>
void liftCompare(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    const bool isMemory = isMemoryOperand(c, 0);
    SemanticOperand address;
    if (isMemory)
        address = c.getAddress(0);
    SemanticOperand first = physical(c, 0);
    SemanticOperand second;
    SemanticOperand isUnderflow = isEmpty(c, first);
    if (!IS_ZERO && !isMemory)
    {
        second = physical(c, (c.getOperandsCount() == 0) ? 1 : stIndex(c, 0));
        isUnderflow = logic(c, Op::OP_OR, isUnderflow, isEmpty(c, second));
    }
    c.branchIf(isUnderflow, underflow);

    SemanticOperand a = readRegister(c, first);
    SemanticOperand b;
    if (IS_ZERO)
        b = makeExtended(c, 0, 0);
    else if (isMemory && IS_INTEGER)
        b = loadIntegerOperand(c, address, memoryBits(c));
    else if (isMemory)
        b = loadFloatOperand(c, address, memoryBits(c));
    else
        b = readRegister(c, second);
    if (isMemory && !IS_INTEGER)
        abortIfPending(c, done);
    compareToConditionCodes(c, a, b, IS_QUIET, done);
    for (uint i = 0; i < POPS; i++)
        popStack(c);
    c.branch(done);

    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    setUnorderedConditionCodes(c);
    for (uint i = 0; i < POPS; i++)
        popStack(c);
    c.placeLabel(done);
}

// fcomi, fcomip, fucomi, fucomip: ST(0) with ST(i), into ZF, PF and CF
template<bool IS_QUIET, bool IS_POP>
void liftCompareToFlags(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand first = physical(c, 0);
    SemanticOperand second = physical(c, stIndex(c, 1));
    c.branchIf(logic(c, Op::OP_OR, isEmpty(c, first), isEmpty(c, second)),
               underflow);
    SemanticOperand a = readRegister(c, first);
    SemanticOperand b = readRegister(c, second);
    // OF, SF and AF are cleared even if the compare is an unmasked exception
    c.setFlags(SemanticFlags::FLAG_OVERFLOW | SemanticFlags::FLAG_SIGN |
               SemanticFlags::FLAG_ADJUST, immediate(c, 0, 1));
    {
        Insn instruction(Op::OP_SET_FLAGS, extendedType(), SemanticOperand(),
                         a, b);
        instruction.setFlags(Op::FLAGS_FLOAT_COMPARE,
                             SemanticFlags::FLAGS_ARITHMETIC);
        instruction.setKind(IS_QUIET ? Op::FLOAT_COMPARE_QUIET :
                                       Op::FLOAT_COMPARE_SIGNALING);
        instruction.setAttributes(X87_ATTRIBUTES);
        c.emit(instruction);
    }
    abortIfPending(c, done);
    setC1(c, 0);
    if (IS_POP)
        popStack(c);
    c.branch(done);

    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    c.setFlags(SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_ZERO |
               SemanticFlags::FLAG_PARITY, immediate(c, 1, 1));
    c.setFlags(SemanticFlags::FLAG_OVERFLOW | SemanticFlags::FLAG_SIGN |
               SemanticFlags::FLAG_ADJUST, immediate(c, 0, 1));
    if (IS_POP)
        popStack(c);
    c.placeLabel(done);
}

// fxam: the class of ST(0), and its sign in C1. An empty register is a class
// too, there is no stack fault
void liftExamine(Context& c)
{
    waitCheck(c);
    SemanticOperand top = physical(c, 0);
    SemanticOperand isRegisterEmpty = isEmpty(c, top);
    SemanticOperand value = readRegister(c, top);
    SemanticOperand bits = c.emit(Op::OP_BITCAST, integerType(80), value);
    c.move(c.getRegister(IA32_REG_FPU_C1),
           c.emit(Op::OP_EXTRACT, integerType(1), bits, immediate(c, 79, 32)));

    // The bits of OP_FLOAT_CLASSIFY: 0 for an unsupported encoding
    SemanticOperand cls = c.newTemporary(integerType(32));
    c.emit(Insn(Op::OP_FLOAT_CLASSIFY, integerType(32), cls, value));
    SemanticOperand isNan = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(32), cls, immediate(c, 0x300, 32)),
        immediate(c, 0, 32));
    SemanticOperand isInfinity = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(32), cls, immediate(c, 0x081, 32)),
        immediate(c, 0, 32));
    SemanticOperand isNormal = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(32), cls, immediate(c, 0x042, 32)),
        immediate(c, 0, 32));
    SemanticOperand isZero = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(32), cls, immediate(c, 0x018, 32)),
        immediate(c, 0, 32));
    SemanticOperand isDenormal = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(32), cls, immediate(c, 0x024, 32)),
        immediate(c, 0, 32));

    // C3 C2 C0: unsupported 000, NaN 001, normal 010, infinity 011, zero
    // 100, empty 101, denormal 110
    c.move(c.getRegister(IA32_REG_FPU_C0),
           logic(c, Op::OP_OR, isRegisterEmpty,
                 logic(c, Op::OP_OR, isNan, isInfinity)));
    c.move(c.getRegister(IA32_REG_FPU_C2),
           logic(c, Op::OP_AND, notOf(c, isRegisterEmpty),
                 logic(c, Op::OP_OR, isNormal,
                       logic(c, Op::OP_OR, isInfinity, isDenormal))));
    c.move(c.getRegister(IA32_REG_FPU_C3),
           logic(c, Op::OP_OR, isRegisterEmpty,
                 logic(c, Op::OP_OR, isZero, isDenormal)));
}

/////////////////////////////////////////////////////////////////////////////
// fxtract
/////////////////////////////////////////////////////////////////////////////

// fxtract: ST(0) = the exponent, then push the significand (so ST(1) is the
// exponent and ST(0) the significand)
void liftExtract(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand overflow = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand top = physical(c, 0);
    SemanticOperand newTop = physical(c, 7);
    c.branchIf(notOf(c, isEmpty(c, newTop)), overflow);
    c.branchIf(isEmpty(c, top), underflow);
    SemanticOperand value = readRegister(c, top);

    SemanticOperand cls = c.newTemporary(integerType(32));
    c.emit(Insn(Op::OP_FLOAT_CLASSIFY, integerType(32), cls, value));
    SemanticOperand isFinite = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(32), cls, immediate(c, 0x066, 32)),
        immediate(c, 0, 32));
    SemanticOperand isInfinity = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(32), cls, immediate(c, 0x081, 32)),
        immediate(c, 0, 32));
    SemanticOperand isZero = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(32), cls, immediate(c, 0x018, 32)),
        immediate(c, 0, 32));

    // The unbiased exponent as a number. A zero gives -infinity and the divide
    // by zero exception, a denormal the denormal exception, a NaN the NaN
    SemanticOperand exponent = floatOperation(c, Op::OP_FLOAT_GET_EXPONENT,
                                              extendedType(), value);
    abortIfPending(c, done);
    {
        // A zero operand: the zero divide exception
        SemanticOperand notZero = c.newLabel();
        c.branchIf(notOf(c, isZero), notZero);
        raiseException(c, SW_ZE, 4, done);
        c.placeLabel(notZero);
    }

    // The significand: the same number with the exponent 0 (a denormal is
    // normalized first). Exact, the precision control doesn't apply
    SemanticOperand bits = c.emit(Op::OP_BITCAST, integerType(80), value);
    SemanticOperand significandBits = c.emit(Op::OP_TRUNCATE, integerType(64), bits);
    SemanticOperand shift = c.emit(Op::OP_COUNT_LEADING_ZEROS, integerType(64),
                                   significandBits);
    SemanticOperand normalized = c.emit(Op::OP_SHIFT_LEFT, integerType(64),
                                        significandBits, shift);
    SemanticOperand sign = c.emit(Op::OP_AND, integerType(16),
        c.emit(Op::OP_EXTRACT, integerType(16), bits, immediate(c, 64, 32)),
        immediate(c, 0x8000, 16));
    SemanticOperand scaled = c.emit(Op::OP_BITCAST, extendedType(),
        c.emit(Op::OP_CONCAT, integerType(80), normalized,
               c.emit(Op::OP_OR, integerType(16), sign, immediate(c, 0x3FFF, 16))));
    // A zero and an infinity keep their value, a NaN is the quiet NaN of the
    // exponent operation
    SemanticOperand significand = c.emit(Op::OP_SELECT, extendedType(),
        isFinite, scaled,
        c.emit(Op::OP_SELECT, extendedType(),
               logic(c, Op::OP_OR, isZero, isInfinity), value, exponent));
    SemanticOperand positiveInfinity = makeExtended(c, 0x7FFF,
                                                    0x8000000000000000ULL);
    SemanticOperand exponentResult = c.emit(Op::OP_SELECT, extendedType(),
                                            isInfinity, positiveInfinity,
                                            exponent);
    setC1(c, 0);
    writeRegister(c, top, exponentResult);
    pushValue(c, significand);
    c.branch(done);

    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    writeRegister(c, top, indefinite(c));
    setValid(c, top);
    pushValue(c, indefinite(c));
    c.branch(done);

    c.placeLabel(overflow);
    raiseStackFault(c, true, done);
    writeRegister(c, top, indefinite(c));
    setValid(c, top);
    pushValue(c, indefinite(c));
    c.placeLabel(done);
}

/////////////////////////////////////////////////////////////////////////////
// Control
/////////////////////////////////////////////////////////////////////////////

// fwait, fnop and the instructions of the 8087 that are nops: they only check
// for a pending exception
void liftWait(Context& c)
{
    waitCheck(c);
}

void liftNop(Context& c)
{
    (void)c;
}

// fninit
void liftInitialize(Context& c)
{
    c.move(controlWord(c), immediate(c, 0x037F, 16));
    c.move(statusWord(c), immediate(c, 0, 16));
    c.move(c.getRegister(IA32_REG_FPU_TAG), immediate(c, 0, 8));
}

// fnclex: the exception flags, the stack fault, ES and B
void liftClearExceptions(Context& c)
{
    clearStatus(c, 0x80FF);
}

// fnstsw m16 / ax
void liftStoreStatus(Context& c)
{
    c.write(0, statusWord(c));
}

// fnstcw m16
void liftStoreControl(Context& c)
{
    c.write(0, controlWord(c));
}

// ES summarizes the exceptions: it is set when an exception flag is set and
// its mask is clear, and B is a copy of ES. Recomputed after the control word
// or the status word is loaded
void synchronizeSummary(Context& c)
{
    SemanticOperand status = statusWord(c);
    SemanticOperand pending = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(16),
               c.emit(Op::OP_AND, integerType(16), status,
                      immediate(c, SW_EXCEPTIONS, 16)),
               c.emit(Op::OP_NOT, integerType(16), controlWord(c))),
        immediate(c, 0, 16));
    SemanticOperand summary = c.emit(Op::OP_SELECT, integerType(16), pending,
                                     immediate(c, SW_ES | SW_B, 16),
                                     immediate(c, 0, 16));
    c.move(status, c.emit(Op::OP_OR, integerType(16),
            c.emit(Op::OP_AND, integerType(16), status,
                   immediate(c, ~(SW_ES | SW_B) & 0xFFFF, 16)), summary));
}

// fldcw m16. ES and B follow the exceptions that the new masks leave unmasked
void liftLoadControl(Context& c)
{
    waitCheck(c);
    c.move(controlWord(c), normalizeControlWord(c, c.read(0, integerType(16))));
    synchronizeSummary(c);
}

// Return the address 'address' + offset
SemanticOperand offsetAddress(Context& c, const SemanticOperand& address,
                              uint offset)
{
    if (offset == 0)
        return address;
    return c.emit(Op::OP_ADD, integerType(32), address, immediate(c, offset, 32));
}

// Return the 2 bit tag of the physical register i for the tag word: 0 valid, 1
// zero, 2 special (NaN, infinity, denormal, unsupported), 3 empty
SemanticOperand registerTag(Context& c, uint index)
{
    SemanticOperand number = immediate(c, index, 3);
    SemanticOperand empty = isEmpty(c, number);
    SemanticOperand bits = c.newTemporary(integerType(80));
    c.emit(Insn(Op::OP_READ_REGISTER_INDEXED, integerType(80), bits,
                c.getRegister(IA32_REG_FPR0), number, immediate(c, 8, 32)));
    SemanticOperand high = c.emit(Op::OP_EXTRACT, integerType(16), bits,
                                  immediate(c, 64, 32));
    SemanticOperand low = c.emit(Op::OP_TRUNCATE, integerType(64), bits);
    SemanticOperand exponent = c.emit(Op::OP_AND, integerType(16), high,
                                      immediate(c, 0x7FFF, 16));
    SemanticOperand isExponentZero = compare(c, Op::COND_EQUAL, exponent,
                                             immediate(c, 0, 16));
    SemanticOperand isExponentMax = compare(c, Op::COND_EQUAL, exponent,
                                            immediate(c, 0x7FFF, 16));
    SemanticOperand isMantissaZero = compare(c, Op::COND_EQUAL, low,
                                             immediate(c, 0, 64));
    SemanticOperand hasIntegerBit = compare(c, Op::COND_SIGNED_LESS, low,
                                            immediate(c, 0, 64));
    const SemanticType two = integerType(2);
    SemanticOperand normalTag = c.emit(Op::OP_SELECT, two, hasIntegerBit,
                                       immediate(c, 0, 2), immediate(c, 2, 2));
    SemanticOperand zeroTag = c.emit(Op::OP_SELECT, two, isMantissaZero,
                                     immediate(c, 1, 2), immediate(c, 2, 2));
    SemanticOperand tag = c.emit(Op::OP_SELECT, two, isExponentZero, zeroTag,
                                 normalTag);
    tag = c.emit(Op::OP_SELECT, two, isExponentMax, immediate(c, 2, 2), tag);
    return c.emit(Op::OP_SELECT, two, empty, immediate(c, 3, 2), tag);
}

// Return the full 16 bit tag word
SemanticOperand fullTagWord(Context& c)
{
    SemanticOperand word = immediate(c, 0, 16);
    for (uint i = 0; i < 8; i++)
    {
        SemanticOperand tag = c.emit(Op::OP_ZERO_EXTEND, integerType(16),
                                     registerTag(c, i));
        if (i != 0)
            tag = c.emit(Op::OP_SHIFT_LEFT, integerType(16), tag,
                         immediate(c, 2 * i, 16));
        word = c.emit(Op::OP_OR, integerType(16), word, tag);
    }
    return word;
}

// Store a value of 'bits' bits (16 or 32) at address + offset
void storeField(Context& c, const SemanticOperand& address, uint offset,
                uint bits, const SemanticOperand& value)
{
    SemanticOperand sized = value;
    const uint valueBits = value.getType().getTotalBits();
    if (valueBits < bits)
        sized = c.emit(Op::OP_ZERO_EXTEND, integerType(bits), value);
    c.store(offsetAddress(c, address, offset), sized);
}

/*
 * The environment image: the control word, the status word, the tag word and
 * the instruction and data pointers (which are not kept: zeros). The 32 bit
 * format has 28 bytes, the 16 bit one 14 bytes. The words of the 32 bit
 * format are padded to dwords with zeros
 */
void storeEnvironment(Context& c, const SemanticOperand& address, bool is32)
{
    SemanticOperand tagWord = fullTagWord(c);
    const uint size = is32 ? 32 : 16;
    const uint step = is32 ? 4 : 2;
    storeField(c, address, 0 * step, size, controlWord(c));
    storeField(c, address, 1 * step, size, statusWord(c));
    storeField(c, address, 2 * step, size, tagWord);
    for (uint i = 3; i < 7; i++)
        storeField(c, address, i * step, size, immediate(c, 0, size));
}

// Load the environment image: the control word, the status word and the tags.
// The tag word becomes the abridged one: a register is empty if its tag is 3
void loadEnvironment(Context& c, const SemanticOperand& address, bool is32)
{
    const uint step = is32 ? 4 : 2;
    SemanticOperand control = c.load(offsetAddress(c, address, 0), integerType(is32 ? 32 : 16));
    SemanticOperand status = c.load(offsetAddress(c, address, step), integerType(is32 ? 32 : 16));
    SemanticOperand tags = c.load(offsetAddress(c, address, 2 * step), integerType(is32 ? 32 : 16));
    if (is32)
    {
        control = c.emit(Op::OP_TRUNCATE, integerType(16), control);
        status = c.emit(Op::OP_TRUNCATE, integerType(16), status);
        tags = c.emit(Op::OP_TRUNCATE, integerType(16), tags);
    }
    c.move(controlWord(c), normalizeControlWord(c, control));
    c.move(statusWord(c), status);
    synchronizeSummary(c);
    SemanticOperand abridged = immediate(c, 0, 8);
    for (uint i = 0; i < 8; i++)
    {
        SemanticOperand field = c.emit(Op::OP_AND, integerType(16),
            c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, integerType(16), tags,
                   immediate(c, 2 * i, 16)), immediate(c, 3, 16));
        SemanticOperand isNotEmpty = compare(c, Op::COND_NOT_EQUAL, field,
                                             immediate(c, 3, 16));
        SemanticOperand bit = c.emit(Op::OP_SELECT, integerType(8), isNotEmpty,
                                     immediate(c, 1 << i, 8), immediate(c, 0, 8));
        abridged = c.emit(Op::OP_OR, integerType(8), abridged, bit);
    }
    c.move(c.getRegister(IA32_REG_FPU_TAG), abridged);
}

// fnstenv: store the environment, then mask all the exceptions
void liftStoreEnvironment(Context& c)
{
    SemanticOperand address = c.getAddress(0);
    storeEnvironment(c, address, IA32OperandDecoder::getOperandBits(
                                    c.getInstruction()) == 32);
    SemanticOperand control = controlWord(c);
    c.move(control, c.emit(Op::OP_OR, integerType(16), control,
                           immediate(c, SW_EXCEPTIONS, 16)));
}

// fldenv
void liftLoadEnvironment(Context& c)
{
    waitCheck(c);
    loadEnvironment(c, c.getAddress(0), IA32OperandDecoder::getOperandBits(
                                    c.getInstruction()) == 32);
}

// fnsave: the environment, ST(0) to ST(7), then fninit
void liftSave(Context& c)
{
    const bool is32 = IA32OperandDecoder::getOperandBits(c.getInstruction()) == 32;
    SemanticOperand address = c.getAddress(0);
    storeEnvironment(c, address, is32);
    const uint base = is32 ? 28 : 14;
    for (uint i = 0; i < 8; i++)
    {
        SemanticOperand bits = c.emit(Op::OP_BITCAST, integerType(80),
                                      readRegister(c, physical(c, i)));
        c.store(offsetAddress(c, address, base + 10 * i), bits);
    }
    liftInitialize(c);
}

// frstor
void liftRestore(Context& c)
{
    waitCheck(c);
    const bool is32 = IA32OperandDecoder::getOperandBits(c.getInstruction()) == 32;
    SemanticOperand address = c.getAddress(0);
    loadEnvironment(c, address, is32);
    const uint base = is32 ? 28 : 14;
    for (uint i = 0; i < 8; i++)
    {
        SemanticOperand bits = c.load(offsetAddress(c, address, base + 10 * i),
                                      integerType(80));
        writeRegister(c, physical(c, i), c.emit(Op::OP_BITCAST,
                                                extendedType(), bits));
    }
}

/*
 * fxsave / fxrstor, the 512 byte image (no wait, the area must be 16 byte
 * aligned: #GP):
 *    0 fcw   2 fsw   4 abridged tag (1 byte)   6 fop   8 fip   12 fcs
 *   16 fdp  20 fds  24 mxcsr  28 mxcsr mask  32 ST(0)..ST(7), 16 bytes each
 *  160 xmm0..xmm7
 * The pointers are stored as zeros, the opcode and the padding of the
 * registers (bytes 10 to 15 of each) is not written
 */
void checkAlignment(Context& c, const SemanticOperand& address)
{
    SemanticOperand isAligned = compare(c, Op::COND_EQUAL,
        c.emit(Op::OP_AND, integerType(32), address, immediate(c, 0xF, 32)),
        immediate(c, 0, 32));
    SemanticOperand ok = c.newLabel();
    c.branchIf(isAligned, ok);
    c.trap(Op::TRAP_FAULT, 13);
    c.placeLabel(ok);
}

void liftFxsave(Context& c)
{
    SemanticOperand address = c.getAddress(0);
    checkAlignment(c, address);
    c.store(offsetAddress(c, address, 0), controlWord(c));
    c.store(offsetAddress(c, address, 2), statusWord(c));
    c.store(offsetAddress(c, address, 4), c.getRegister(IA32_REG_FPU_TAG));
    // The reserved byte, the opcode and the instruction and data pointers
    c.store(offsetAddress(c, address, 5), immediate(c, 0, 8));
    c.store(offsetAddress(c, address, 6), immediate(c, 0, 16));
    for (uint i = 8; i < 24; i += 4)
        c.store(offsetAddress(c, address, i), immediate(c, 0, 32));
    c.store(offsetAddress(c, address, 24), c.getRegister(IA32_REG_MXCSR));
    c.store(offsetAddress(c, address, 28), immediate(c, 0xFFFF, 32));
    for (uint i = 0; i < 8; i++)
    {
        SemanticOperand bits = c.emit(Op::OP_BITCAST, integerType(80),
                                      readRegister(c, physical(c, i)));
        c.store(offsetAddress(c, address, 32 + 16 * i), bits);
        // The reserved bytes of the register slot
        c.store(offsetAddress(c, address, 42 + 16 * i), immediate(c, 0, 16));
        c.store(offsetAddress(c, address, 44 + 16 * i), immediate(c, 0, 32));
        c.store(offsetAddress(c, address, 160 + 16 * i),
                c.getRegister(IA32_REG_XMM0 + i));
    }
}

void liftFxrstor(Context& c)
{
    SemanticOperand address = c.getAddress(0);
    checkAlignment(c, address);
    SemanticOperand mxcsr = c.load(offsetAddress(c, address, 24), integerType(32));
    // Reserved bits of MXCSR: #GP
    SemanticOperand isValid = compare(c, Op::COND_EQUAL,
        c.emit(Op::OP_AND, integerType(32), mxcsr, immediate(c, 0xFFFF0000, 32)),
        immediate(c, 0, 32));
    SemanticOperand ok = c.newLabel();
    c.branchIf(isValid, ok);
    c.trap(Op::TRAP_FAULT, 13);
    c.placeLabel(ok);
    c.move(controlWord(c), normalizeControlWord(c,
            c.load(offsetAddress(c, address, 0), integerType(16))));
    c.move(statusWord(c), c.load(offsetAddress(c, address, 2), integerType(16)));
    synchronizeSummary(c);
    c.move(c.getRegister(IA32_REG_FPU_TAG),
           c.load(offsetAddress(c, address, 4), integerType(8)));
    c.move(c.getRegister(IA32_REG_MXCSR), mxcsr);
    for (uint i = 0; i < 8; i++)
    {
        SemanticOperand bits = c.load(offsetAddress(c, address, 32 + 16 * i),
                                      integerType(80));
        writeRegister(c, physical(c, i), c.emit(Op::OP_BITCAST,
                                                extendedType(), bits));
        c.move(c.getRegister(IA32_REG_XMM0 + i),
               c.load(offsetAddress(c, address, 160 + 16 * i), integerType(128)));
    }
}

/////////////////////////////////////////////////////////////////////////////
// Packed BCD
/////////////////////////////////////////////////////////////////////////////

// fbld m80: 18 packed BCD digits (the bytes 0 to 8, the low digits first) and
// the sign in bit 7 of byte 9. A byte is hi * 10 + lo (a digit above 9 is
// undefined, it adds up as a number)
void liftLoadBcd(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand overflow = c.newLabel();
    SemanticOperand address = c.getAddress(0);
    c.branchIf(notOf(c, isEmpty(c, physical(c, 7))), overflow);

    const SemanticType wide = integerType(64);
    SemanticOperand value = immediate(c, 0, 64);
    for (int i = 8; i >= 0; i--)
    {
        SemanticOperand byte = c.emit(Op::OP_ZERO_EXTEND, wide,
            c.load(offsetAddress(c, address, i), integerType(8)));
        SemanticOperand high = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, wide, byte,
                                      immediate(c, 4, 64));
        SemanticOperand low = c.emit(Op::OP_AND, wide, byte, immediate(c, 15, 64));
        SemanticOperand digits = c.emit(Op::OP_ADD, wide,
            c.emit(Op::OP_MULTIPLY, wide, high, immediate(c, 10, 64)), low);
        value = c.emit(Op::OP_ADD, wide,
            c.emit(Op::OP_MULTIPLY, wide, value, immediate(c, 100, 64)), digits);
    }
    SemanticOperand number = floatOperation(c, Op::OP_SIGNED_TO_FLOAT,
            extendedType(), value, SemanticOperand(), Op::ROUND_DYNAMIC, false);
    SemanticOperand signByte = c.load(offsetAddress(c, address, 9),
                                      integerType(8));
    SemanticOperand isNegative = compare(c, Op::COND_NOT_EQUAL,
        c.emit(Op::OP_AND, integerType(8), signByte, immediate(c, 0x80, 8)),
        immediate(c, 0, 8));
    number = c.emit(Op::OP_SELECT, extendedType(), isNegative,
        floatOperation(c, Op::OP_FLOAT_NEGATE, extendedType(), number,
                       SemanticOperand(), Op::ROUND_DYNAMIC, false), number);
    setC1(c, 0);
    pushValue(c, number);
    c.branch(done);

    c.placeLabel(overflow);
    raiseStackFault(c, true, done);
    pushValue(c, indefinite(c));
    c.placeLabel(done);
}

// fbstp m80: ST(0) rounded to an integer (the rounding control), stored as 18
// packed BCD digits and the sign. A value of 19 digits or more, a NaN or an
// infinity store the packed BCD indefinite
void liftStoreBcd(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand address = c.getAddress(0);
    SemanticOperand top = physical(c, 0);
    c.branchIf(isEmpty(c, top), underflow);
    SemanticOperand value = readRegister(c, top);

    // The magnitude: the rounded integer fits 64 bits (converted as a signed
    // number, the indefinite 0x8000000000000000 is out of the BCD range)
    const SemanticType wide = integerType(64);
    SemanticOperand integer = floatOperation(c, Op::OP_FLOAT_TO_SIGNED, wide,
                                             value);
    abortIfPending(c, done);
    // The sign is the sign of the number (a small negative number rounds to
    // -0)
    SemanticOperand isNegative = c.emit(Op::OP_EXTRACT, integerType(1),
        c.emit(Op::OP_BITCAST, integerType(80), value), immediate(c, 79, 32));
    SemanticOperand isIntegerNegative = compare(c, Op::COND_SIGNED_LESS,
                                        integer, immediate(c, 0, 64));
    SemanticOperand magnitude = c.emit(Op::OP_SELECT, wide, isIntegerNegative,
                                       c.emit(Op::OP_NEGATE, wide, integer),
                                       integer);
    // 10^18 - 1 is the largest: anything else (including the indefinite,
    // whose magnitude 2^63 wraps negative) is invalid
    SemanticOperand isInRange = compare(c, Op::COND_UNSIGNED_LESS, magnitude,
                                        immediate(c, 1000000000000000000LL, 64));
    SemanticOperand invalid = c.newLabel();
    c.branchIf(notOf(c, isInRange), invalid);

    SemanticOperand rest = magnitude;
    for (uint i = 0; i < 9; i++)
    {
        SemanticOperand pair = c.emit(Op::OP_REMAINDER_UNSIGNED, wide, rest,
                                      immediate(c, 100, 64));
        rest = c.emit(Op::OP_DIVIDE_UNSIGNED, wide, rest, immediate(c, 100, 64));
        SemanticOperand high = c.emit(Op::OP_DIVIDE_UNSIGNED, wide, pair,
                                      immediate(c, 10, 64));
        SemanticOperand low = c.emit(Op::OP_REMAINDER_UNSIGNED, wide, pair,
                                     immediate(c, 10, 64));
        SemanticOperand byte = c.emit(Op::OP_OR, wide,
            c.emit(Op::OP_SHIFT_LEFT, wide, high, immediate(c, 4, 64)), low);
        c.store(offsetAddress(c, address, i),
                c.emit(Op::OP_TRUNCATE, integerType(8), byte));
    }
    c.store(offsetAddress(c, address, 9),
            c.emit(Op::OP_SELECT, integerType(8), isNegative,
                   immediate(c, 0x80, 8), immediate(c, 0, 8)));
    setC1(c, 0);
    popStack(c);
    c.branch(done);

    c.placeLabel(invalid);
    raiseException(c, SW_IE, 1, done);
    // The packed BCD indefinite: FFFF C000 0000 0000 0000
    for (uint i = 0; i < 7; i++)
        c.store(offsetAddress(c, address, i), immediate(c, 0, 8));
    c.store(offsetAddress(c, address, 7), immediate(c, 0xC0, 8));
    c.store(offsetAddress(c, address, 8), immediate(c, 0xFF, 8));
    c.store(offsetAddress(c, address, 9), immediate(c, 0xFF, 8));
    setC1(c, 0);
    popStack(c);
    c.branch(done);

    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    for (uint i = 0; i < 7; i++)
        c.store(offsetAddress(c, address, i), immediate(c, 0, 8));
    c.store(offsetAddress(c, address, 7), immediate(c, 0xC0, 8));
    c.store(offsetAddress(c, address, 8), immediate(c, 0xFF, 8));
    c.store(offsetAddress(c, address, 9), immediate(c, 0xFF, 8));
    popStack(c);
    c.placeLabel(done);
}


/////////////////////////////////////////////////////////////////////////////
// fprem, fprem1
/////////////////////////////////////////////////////////////////////////////

// Write 'dest' (a temporary, assigned more than once) = operation(s0, s1)
void assign(Context& c, const SemanticOperand& dest, Op::Operation operation,
            const SemanticOperand& source0,
            const SemanticOperand& source1 = SemanticOperand())
{
    c.emit(Insn(operation, dest.getType(), dest, source0, source1));
}

// Return true when the bits of 'cls' (OP_FLOAT_CLASSIFY) intersect 'mask'
SemanticOperand hasClass(Context& c, const SemanticOperand& cls, uint32 mask)
{
    return compare(c, Op::COND_NOT_EQUAL,
                   c.emit(Op::OP_AND, integerType(32), cls, immediate(c, mask, 32)),
                   immediate(c, 0, 32));
}

// The unbiased exponent (32 bit) of a finite non zero number, and its
// significand shifted so that the integer bit is set (a denormal number is
// normalized). 'bits' is the 80 bit register
void normalizeOperand(Context& c, const SemanticOperand& bits,
                      SemanticOperand& significand, SemanticOperand& exponent)
{
    SemanticOperand raw = c.emit(Op::OP_TRUNCATE, integerType(64), bits);
    SemanticOperand field = c.emit(Op::OP_ZERO_EXTEND, integerType(32),
        c.emit(Op::OP_AND, integerType(16),
               c.emit(Op::OP_EXTRACT, integerType(16), bits, immediate(c, 64, 32)),
               immediate(c, 0x7FFF, 16)));
    // The exponent field 0 is the exponent 1
    SemanticOperand isDenormalField = compare(c, Op::COND_EQUAL, field,
                                              immediate(c, 0, 32));
    field = c.emit(Op::OP_SELECT, integerType(32), isDenormalField,
                   immediate(c, 1, 32), field);
    SemanticOperand shift = c.emit(Op::OP_COUNT_LEADING_ZEROS, integerType(64), raw);
    significand = c.emit(Op::OP_SHIFT_LEFT, integerType(64), raw, shift);
    exponent = c.emit(Op::OP_SUBTRACT, integerType(32),
        c.emit(Op::OP_SUBTRACT, integerType(32), field, immediate(c, 0x3FFF, 32)),
        c.emit(Op::OP_TRUNCATE, integerType(32), shift));
}

// Return the number as the arithmetic stores it: a pseudo-denormal (exponent
// 0, integer bit 1) is the normal number with the exponent 1
SemanticOperand canonicalize(Context& c, const SemanticOperand& number)
{
    SemanticOperand bits = c.emit(Op::OP_BITCAST, integerType(80), number);
    SemanticOperand significand = c.emit(Op::OP_TRUNCATE, integerType(64), bits);
    SemanticOperand high = c.emit(Op::OP_EXTRACT, integerType(16), bits,
                                  immediate(c, 64, 32));
    SemanticOperand isPseudo = logic(c, Op::OP_AND,
        compare(c, Op::COND_EQUAL, c.emit(Op::OP_AND, integerType(16), high,
                                          immediate(c, 0x7FFF, 16)),
                immediate(c, 0, 16)),
        compare(c, Op::COND_SIGNED_LESS, significand, immediate(c, 0, 64)));
    SemanticOperand fixed = c.emit(Op::OP_SELECT, integerType(16), isPseudo,
        c.emit(Op::OP_OR, integerType(16), high, immediate(c, 1, 16)), high);
    return c.emit(Op::OP_BITCAST, extendedType(),
                  c.emit(Op::OP_CONCAT, integerType(80), significand, fixed));
}

/*
 * Return the extended number R * 2^lsbExponent, with the sign. R is an
 * unsigned 64 bit integer (zero gives a zero), lsbExponent is a signed 32
 * bit integer. The result is exact (a remainder is)
 */
SemanticOperand makeFromInteger(Context& c, const SemanticOperand& value,
                                const SemanticOperand& lsbExponent,
                                const SemanticOperand& sign)
{
    SemanticOperand shift = c.emit(Op::OP_COUNT_LEADING_ZEROS, integerType(64), value);
    SemanticOperand significand = c.emit(Op::OP_SHIFT_LEFT, integerType(64),
                                         value, shift);
    // field = exponent + 16383 = lsb + 63 - shift + 16383
    SemanticOperand field = c.emit(Op::OP_SUBTRACT, integerType(32),
        c.emit(Op::OP_ADD, integerType(32), lsbExponent,
               immediate(c, 63 + 0x3FFF, 32)),
        c.emit(Op::OP_TRUNCATE, integerType(32), shift));
    // A denormal result: the exponent field 0 and the significand shifted
    // right (exact)
    SemanticOperand isDenormal = compare(c, Op::COND_SIGNED_LESS, field,
                                         immediate(c, 1, 32));
    SemanticOperand denormalShift = c.emit(Op::OP_SUBTRACT, integerType(32),
                                           immediate(c, 1, 32), field);
    SemanticOperand shiftedSignificand = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL,
        integerType(64), significand,
        c.emit(Op::OP_ZERO_EXTEND, integerType(64), denormalShift));
    SemanticOperand finalSignificand = c.emit(Op::OP_SELECT, integerType(64),
        isDenormal, shiftedSignificand, significand);
    SemanticOperand finalField = c.emit(Op::OP_SELECT, integerType(32),
        isDenormal, immediate(c, 0, 32), field);
    SemanticOperand signExponent = c.emit(Op::OP_OR, integerType(16),
        c.emit(Op::OP_TRUNCATE, integerType(16), finalField),
        c.emit(Op::OP_SELECT, integerType(16), sign, immediate(c, 0x8000, 16),
               immediate(c, 0, 16)));
    SemanticOperand isZero = compare(c, Op::COND_EQUAL, value, immediate(c, 0, 64));
    SemanticOperand number = c.emit(Op::OP_BITCAST, extendedType(),
        c.emit(Op::OP_CONCAT, integerType(80), finalSignificand, signExponent));
    SemanticOperand zero = c.emit(Op::OP_BITCAST, extendedType(),
        c.emit(Op::OP_CONCAT, integerType(80), immediate(c, 0, 64),
               c.emit(Op::OP_SELECT, integerType(16), sign,
                      immediate(c, 0x8000, 16), immediate(c, 0, 16))));
    return c.emit(Op::OP_SELECT, extendedType(), isZero, zero, number);
}

/*
 * fprem (the quotient truncated toward zero) and fprem1 (the IEEE remainder:
 * the quotient rounded to nearest). The remainder of two finite numbers is
 * exact:
 *   ST(0) = a, ST(1) = b, D = exp(a) - exp(b)
 *   D < 64: Q = the integer quotient, ST(0) = a - b * Q, C2 = 0, and
 *           C0 C3 C1 = Q2 Q1 Q0
 *   D >= 64: a partial remainder: the reduction by 2^(D - N), N = 32 + D mod 32,
 *           C2 = 1 (and C0, C1, C3 are cleared)
 * An invalid operation (a NaN, an unsupported format, an infinite dividend or a
 * zero divisor) or a stack fault clears the condition codes
 */
template<bool IS_NEAREST>
void liftRemainder(Context& c)
{
    waitCheck(c);
    SemanticOperand done = c.newLabel();
    SemanticOperand underflow = c.newLabel();
    SemanticOperand first = physical(c, 0);
    SemanticOperand second = physical(c, 1);
    c.branchIf(logic(c, Op::OP_OR, isEmpty(c, first), isEmpty(c, second)),
               underflow);
    SemanticOperand a = readRegister(c, first);
    SemanticOperand b = readRegister(c, second);
    // The exceptions of the operands: a signaling NaN, an unsupported format
    // (IE), a denormal number (DE), none if one of them is a NaN
    floatCompare(c, Op::COND_FLOAT_UNORDERED, a, b, true);
    abortIfPending(c, done);

    SemanticOperand result = c.newTemporary(extendedType());
    // The condition codes of the result: C0 C1 C2 C3
    SemanticOperand c0 = c.newTemporary(integerType(1));
    SemanticOperand c1 = c.newTemporary(integerType(1));
    SemanticOperand c2 = c.newTemporary(integerType(1));
    SemanticOperand c3 = c.newTemporary(integerType(1));
    assign(c, c0, Op::OP_MOVE, immediate(c, 0, 1));
    assign(c, c1, Op::OP_MOVE, immediate(c, 0, 1));
    assign(c, c2, Op::OP_MOVE, immediate(c, 0, 1));
    assign(c, c3, Op::OP_MOVE, immediate(c, 0, 1));
    SemanticOperand store = c.newLabel();

    SemanticOperand classA = c.newTemporary(integerType(32));
    SemanticOperand classB = c.newTemporary(integerType(32));
    c.emit(Insn(Op::OP_FLOAT_CLASSIFY, integerType(32), classA, a));
    c.emit(Insn(Op::OP_FLOAT_CLASSIFY, integerType(32), classB, b));

    // A NaN or an unsupported format (the class 0): the result of an operation
    SemanticOperand isBadA = logic(c, Op::OP_OR, hasClass(c, classA, 0x300),
        compare(c, Op::COND_EQUAL, classA, immediate(c, 0, 32)));
    SemanticOperand isBadB = logic(c, Op::OP_OR, hasClass(c, classB, 0x300),
        compare(c, Op::COND_EQUAL, classB, immediate(c, 0, 32)));
    SemanticOperand notBad = c.newLabel();
    c.branchIf(notOf(c, logic(c, Op::OP_OR, isBadA, isBadB)), notBad);
    assign(c, result, Op::OP_MOVE, floatOperation(c, Op::OP_FLOAT_ADD,
            extendedType(), a, b, Op::ROUND_DYNAMIC, false));
    c.branch(store);
    c.placeLabel(notBad);

    // An infinite dividend or a zero divisor: invalid
    SemanticOperand isValid = c.newLabel();
    c.branchIf(notOf(c, logic(c, Op::OP_OR, hasClass(c, classA, 0x081),
                              hasClass(c, classB, 0x018))), isValid);
    raiseException(c, SW_IE, 1, done);
    assign(c, result, Op::OP_MOVE, indefinite(c));
    c.branch(store);
    c.placeLabel(isValid);

    // An infinite divisor, or a zero dividend: the dividend is the remainder
    SemanticOperand isFinite = c.newLabel();
    c.branchIf(notOf(c, logic(c, Op::OP_OR, hasClass(c, classB, 0x081),
                              hasClass(c, classA, 0x018))), isFinite);
    assign(c, result, Op::OP_MOVE, canonicalize(c, a));
    c.branch(store);
    c.placeLabel(isFinite);

    // Two finite non zero numbers
    SemanticOperand bitsA = c.emit(Op::OP_BITCAST, integerType(80), a);
    SemanticOperand bitsB = c.emit(Op::OP_BITCAST, integerType(80), b);
    SemanticOperand dividend, exponentA, divisor, exponentB;
    normalizeOperand(c, bitsA, dividend, exponentA);
    normalizeOperand(c, bitsB, divisor, exponentB);
    SemanticOperand signA = c.emit(Op::OP_EXTRACT, integerType(1), bitsA,
                                   immediate(c, 79, 32));
    SemanticOperand difference = c.emit(Op::OP_SUBTRACT, integerType(32),
                                        exponentA, exponentB);
    // The exponent of the least significant bit of the divisor
    SemanticOperand unitB = c.emit(Op::OP_SUBTRACT, integerType(32), exponentB,
                                   immediate(c, 63, 32));

    // D < -1, or D = -1 for fprem: |a| < |b| (|a| < |b| / 2 for the nearest
    // quotient), the quotient is 0 and a is the remainder
    SemanticOperand notSmall = c.newLabel();
    c.branchIf(compare(c, Op::COND_SIGNED_GREATER_EQUAL, difference,
                       immediate(c, IS_NEAREST ? -1 : 0, 32)), notSmall);
    assign(c, result, Op::OP_MOVE, canonicalize(c, a));
    c.branch(store);
    c.placeLabel(notSmall);

    // fprem1 with D = -1: a / b = A / (2 B), the quotient is 1 if A > B (a
    // tie goes to the even quotient 0). The remainder a - b is negative
    SemanticOperand quotient = c.newTemporary(integerType(64));
    SemanticOperand remainder = c.newTemporary(integerType(64));
    SemanticOperand lsbExponent = c.newTemporary(integerType(32));
    SemanticOperand resultSign = c.newTemporary(integerType(1));
    assign(c, resultSign, Op::OP_MOVE, signA);
    SemanticOperand notHalf = c.newLabel();
    if (IS_NEAREST)
    {
        c.branchIf(compare(c, Op::COND_SIGNED_GREATER_EQUAL, difference,
                           immediate(c, 0, 32)), notHalf);
        SemanticOperand keep = c.newLabel();
        c.branchIf(compare(c, Op::COND_UNSIGNED_LESS_EQUAL, dividend, divisor),
                   keep);
        // |R| = 2B - A in units of 2^(exponent(b) - 64)
        assign(c, remainder, Op::OP_SUBTRACT,
               c.emit(Op::OP_SHIFT_LEFT, integerType(64), divisor,
                      immediate(c, 1, 64)), dividend);
        // 2B - A < B fits 64 bits, but 2B itself is 65 bits: the difference is
        // exact modulo 2^64
        assign(c, lsbExponent, Op::OP_SUBTRACT, unitB, immediate(c, 1, 32));
        assign(c, resultSign, Op::OP_XOR, signA, immediate(c, 1, 1));
        assign(c, c1, Op::OP_MOVE, immediate(c, 1, 1));
        assign(c, result, Op::OP_MOVE, makeFromInteger(c, remainder, lsbExponent,
                                                       resultSign));
        c.branch(store);
        c.placeLabel(keep);
        assign(c, result, Op::OP_MOVE, canonicalize(c, a));
        c.branch(store);
    }
    c.placeLabel(notHalf);

    // D >= 0. The number of reduction steps N is D, or for D >= 64 a partial
    // remainder with N = 32 + D mod 32
    SemanticOperand isPartial = compare(c, Op::COND_SIGNED_GREATER_EQUAL,
                                        difference, immediate(c, 64, 32));
    SemanticOperand steps = c.newTemporary(integerType(32));
    c.emit(Insn(Op::OP_SELECT, integerType(32), steps, isPartial,
                c.emit(Op::OP_ADD, integerType(32),
                       c.emit(Op::OP_AND, integerType(32), difference,
                              immediate(c, 31, 32)), immediate(c, 32, 32)),
                difference));
    // The exponent of the least significant bit of the remainder
    c.emit(Insn(Op::OP_ADD, integerType(32), lsbExponent, unitB,
                c.emit(Op::OP_SUBTRACT, integerType(32), difference, steps)));

    // r = A, q = 0, the first step: A < 2B
    assign(c, remainder, Op::OP_MOVE, dividend);
    assign(c, quotient, Op::OP_MOVE, immediate(c, 0, 64));
    SemanticOperand noFirst = c.newLabel();
    c.branchIf(compare(c, Op::COND_UNSIGNED_LESS, remainder, divisor), noFirst);
    assign(c, remainder, Op::OP_SUBTRACT, remainder, divisor);
    assign(c, quotient, Op::OP_MOVE, immediate(c, 1, 64));
    c.placeLabel(noFirst);

    // N steps of the long division: r = 2r mod B, q = 2q + bit
    SemanticOperand loop = c.newLabel();
    SemanticOperand loopEnd = c.newLabel();
    c.placeLabel(loop);
    c.branchIf(compare(c, Op::COND_EQUAL, steps, immediate(c, 0, 32)), loopEnd);
    {
        SemanticOperand carry = compare(c, Op::COND_SIGNED_LESS, remainder,
                                        immediate(c, 0, 64));
        assign(c, remainder, Op::OP_SHIFT_LEFT, remainder, immediate(c, 1, 64));
        assign(c, quotient, Op::OP_SHIFT_LEFT, quotient, immediate(c, 1, 64));
        SemanticOperand isSubtract = logic(c, Op::OP_OR, carry,
            compare(c, Op::COND_UNSIGNED_GREATER_EQUAL, remainder, divisor));
        SemanticOperand skip = c.newLabel();
        c.branchIf(notOf(c, isSubtract), skip);
        assign(c, remainder, Op::OP_SUBTRACT, remainder, divisor);
        assign(c, quotient, Op::OP_OR, quotient, immediate(c, 1, 64));
        c.placeLabel(skip);
        assign(c, steps, Op::OP_SUBTRACT, steps, immediate(c, 1, 32));
        c.branch(loop);
    }
    c.placeLabel(loopEnd);

    if (IS_NEAREST)
    {
        // The nearest quotient: if 2R > B, or 2R = B and Q is odd, Q + 1 and
        // the remainder R - B (negative). A partial remainder isn't adjusted
        SemanticOperand noAdjust = c.newLabel();
        c.branchIf(isPartial, noAdjust);
        SemanticOperand highBit = compare(c, Op::COND_SIGNED_LESS, remainder,
                                          immediate(c, 0, 64));
        SemanticOperand doubled = c.emit(Op::OP_SHIFT_LEFT, integerType(64),
                                         remainder, immediate(c, 1, 64));
        SemanticOperand isGreater = logic(c, Op::OP_OR, highBit,
            compare(c, Op::COND_UNSIGNED_GREATER, doubled, divisor));
        SemanticOperand isTie = logic(c, Op::OP_AND, notOf(c, highBit),
            compare(c, Op::COND_EQUAL, doubled, divisor));
        SemanticOperand isOdd = compare(c, Op::COND_NOT_EQUAL,
            c.emit(Op::OP_AND, integerType(64), quotient, immediate(c, 1, 64)),
            immediate(c, 0, 64));
        c.branchIf(notOf(c, logic(c, Op::OP_OR, isGreater,
                                  logic(c, Op::OP_AND, isTie, isOdd))),
                   noAdjust);
        assign(c, remainder, Op::OP_SUBTRACT, divisor, remainder);
        assign(c, quotient, Op::OP_ADD, quotient, immediate(c, 1, 64));
        assign(c, resultSign, Op::OP_XOR, signA, immediate(c, 1, 1));
        c.placeLabel(noAdjust);
    }
    assign(c, result, Op::OP_MOVE, makeFromInteger(c, remainder, lsbExponent,
                                                   resultSign));
    // C2 = 1 for a partial remainder, otherwise C0 C3 C1 = Q2 Q1 Q0
    assign(c, c2, Op::OP_MOVE, isPartial);
    SemanticOperand complete = c.newLabel();
    c.branchIf(isPartial, store);
    assign(c, c0, Op::OP_EXTRACT, quotient, immediate(c, 2, 32));
    assign(c, c3, Op::OP_EXTRACT, quotient, immediate(c, 1, 32));
    assign(c, c1, Op::OP_EXTRACT, quotient, immediate(c, 0, 32));
    c.placeLabel(complete);

    c.placeLabel(store);
    writeRegister(c, first, result);
    c.move(c.getRegister(IA32_REG_FPU_C0), c0);
    c.move(c.getRegister(IA32_REG_FPU_C1), c1);
    c.move(c.getRegister(IA32_REG_FPU_C2), c2);
    c.move(c.getRegister(IA32_REG_FPU_C3), c3);
    c.branch(done);

    c.placeLabel(underflow);
    raiseStackFault(c, false, done);
    c.move(c.getRegister(IA32_REG_FPU_C0), immediate(c, 0, 1));
    c.move(c.getRegister(IA32_REG_FPU_C2), immediate(c, 0, 1));
    c.move(c.getRegister(IA32_REG_FPU_C3), immediate(c, 0, 1));
    writeRegister(c, first, indefinite(c));
    setValid(c, first);
    c.placeLabel(done);
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// The table
/////////////////////////////////////////////////////////////////////////////

namespace ia32dis {

namespace {

const uint8 CAT_FLOAT = Cl::CATEGORY_FLOAT;

const uint32 U_FADD  = Cl::UNIT_FLOAT_ADD;
const uint32 U_FMUL  = Cl::UNIT_FLOAT_MULTIPLY;
const uint32 U_FDIV  = Cl::UNIT_FLOAT_DIVIDE;
const uint32 U_FCVT  = Cl::UNIT_FLOAT_CONVERT;
const uint32 U_LOAD  = Cl::UNIT_LOAD;
const uint32 U_STORE = Cl::UNIT_STORE;
const uint32 U_SYSTEM = Cl::UNIT_SYSTEM;
const uint32 U_NONE  = Cl::UNIT_NONE;

const uint32 P_NONE  = Cl::PROPERTY_MAY_FAULT;
const uint32 P_READ  = Cl::PROPERTY_READS_MEMORY | Cl::PROPERTY_MAY_FAULT;
const uint32 P_WRITE = Cl::PROPERTY_WRITES_MEMORY | Cl::PROPERTY_MAY_FAULT;
// The x87 stack is an implicit operand
const uint32 P_STACK = Cl::PROPERTY_IMPLICIT_OPERANDS;
const uint32 P_HELPER = Cl::PROPERTY_NEEDS_HELPER;

// The condition codes the instructions leave undefined
const uint32 UC023 = SW_C0 | SW_C2 | SW_C3;
const uint32 UC0123 = SW_C0 | SW_C1 | SW_C2 | SW_C3;
const uint32 UC03 = SW_C0 | SW_C3;

// The flags of fcomi
const uint32 FALL = SemanticFlags::FLAGS_ARITHMETIC;

} // namespace

#define X87(name, lift, units, properties, readFlags, writeFlags, undefinedStatus) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, CAT_FLOAT, units, properties, readFlags, writeFlags, 0, \
        undefinedStatus }, lift }

// An entry of another category (the instructions which are not arithmetic)
#define X87_CATEGORY(name, lift, category, units, properties, readFlags, writeFlags, undefinedStatus) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, category, units, properties, readFlags, writeFlags, 0, \
        undefinedStatus }, lift }

#define X87_OPERANDS(count, name, lift, units, properties, readFlags, writeFlags, undefinedStatus) \
    { count, \
      { name, NULL, 0, CAT_FLOAT, units, properties, readFlags, writeFlags, 0, \
        undefinedStatus }, lift }

// The four arithmetic operations in their forms
#define X87_ARITHMETIC(name, nameP, nameI, operation, isReverse, units) \
    X87(name,  (liftArithmetic<operation, isReverse, false, false>), units | U_LOAD, P_READ | P_STACK, 0, 0, UC023), \
    X87(nameP, (liftArithmetic<operation, isReverse, true,  false>), units, P_STACK, 0, 0, UC023), \
    X87(nameI, (liftArithmetic<operation, isReverse, false, true>),  units | U_LOAD, P_READ | P_STACK, 0, 0, UC023)

const IA32SemanticEntry gIa32X87SemanticTable[] = {
    //////////////////////////////////////////////////////////////////////////
    // Data transfer
    //////////////////////////////////////////////////////////////////////////
    X87("fld",    liftLoad,                    U_LOAD, P_READ | P_STACK, 0, 0, UC023),
    X87("fst",    liftStore<false>,            U_STORE, P_WRITE | P_STACK, 0, 0, UC023),
    X87("fstp",   liftStore<true>,             U_STORE, P_WRITE | P_STACK, 0, 0, UC023),
    X87("fild",   liftLoadInteger,             U_LOAD | U_FCVT, P_READ | P_STACK, 0, 0, UC023),
    X87("fist",   (liftStoreInteger<false, false>), U_STORE | U_FCVT, P_WRITE | P_STACK, 0, 0, UC023),
    X87("fistp",  (liftStoreInteger<true, false>),  U_STORE | U_FCVT, P_WRITE | P_STACK, 0, 0, UC023),
    X87("fisttp", (liftStoreInteger<true, true>),   U_STORE | U_FCVT, P_WRITE | P_STACK, 0, 0, UC023),
    X87("fxch",   liftExchange,                U_FADD, P_STACK, 0, 0, UC023),
    X87("fcmovb",   (liftConditionalMove<Op::COND_CARRY>),                U_FADD, P_STACK | Cl::PROPERTY_CONDITIONAL, SemanticFlags::FLAG_CARRY, 0, UC023),
    X87("fcmove",   (liftConditionalMove<Op::COND_EQUAL>),                U_FADD, P_STACK | Cl::PROPERTY_CONDITIONAL, SemanticFlags::FLAG_ZERO, 0, UC023),
    X87("fcmovbe",  (liftConditionalMove<Op::COND_UNSIGNED_LESS_EQUAL>),  U_FADD, P_STACK | Cl::PROPERTY_CONDITIONAL, SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_ZERO, 0, UC023),
    X87("fcmovu",   (liftConditionalMove<Op::COND_PARITY_EVEN>),          U_FADD, P_STACK | Cl::PROPERTY_CONDITIONAL, SemanticFlags::FLAG_PARITY, 0, UC023),
    X87("fcmovnb",  (liftConditionalMove<Op::COND_NOT_CARRY>),            U_FADD, P_STACK | Cl::PROPERTY_CONDITIONAL, SemanticFlags::FLAG_CARRY, 0, UC023),
    X87("fcmovne",  (liftConditionalMove<Op::COND_NOT_EQUAL>),            U_FADD, P_STACK | Cl::PROPERTY_CONDITIONAL, SemanticFlags::FLAG_ZERO, 0, UC023),
    X87("fcmovnbe", (liftConditionalMove<Op::COND_UNSIGNED_GREATER>),     U_FADD, P_STACK | Cl::PROPERTY_CONDITIONAL, SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_ZERO, 0, UC023),
    X87("fcmovnu",  (liftConditionalMove<Op::COND_PARITY_ODD>),           U_FADD, P_STACK | Cl::PROPERTY_CONDITIONAL, SemanticFlags::FLAG_PARITY, 0, UC023),
    X87("ffree",  liftFree<false>,             U_FADD, P_STACK, 0, 0, UC0123),
    X87("ffreep", liftFree<true>,              U_FADD, P_STACK, 0, 0, UC0123),
    X87("fincstp", liftStepTop<true>,          U_FADD, P_STACK, 0, 0, UC023),
    X87("fdecstp", liftStepTop<false>,         U_FADD, P_STACK, 0, 0, UC023),
    X87("fld1",    (liftLoadConstant<&gConstantOne, true>),  U_LOAD, P_STACK, 0, 0, UC023),
    X87("fldz",    (liftLoadConstant<&gConstantZero, true>), U_LOAD, P_STACK, 0, 0, UC023),
    X87("fldl2t",  (liftLoadConstant<&gConstantL2T, false>), U_LOAD, P_STACK, 0, 0, UC023),
    X87("fldl2e",  (liftLoadConstant<&gConstantL2E, false>), U_LOAD, P_STACK, 0, 0, UC023),
    X87("fldpi",   (liftLoadConstant<&gConstantPi, false>),  U_LOAD, P_STACK, 0, 0, UC023),
    X87("fldlg2",  (liftLoadConstant<&gConstantLg2, false>), U_LOAD, P_STACK, 0, 0, UC023),
    X87("fldln2",  (liftLoadConstant<&gConstantLn2, false>), U_LOAD, P_STACK, 0, 0, UC023),
    X87("fchs",    (liftSign<Op::OP_FLOAT_NEGATE>),   U_FADD, P_STACK, 0, 0, UC023),
    X87("fabs",    (liftSign<Op::OP_FLOAT_ABSOLUTE>), U_FADD, P_STACK, 0, 0, UC023),

    //////////////////////////////////////////////////////////////////////////
    // Arithmetic
    //////////////////////////////////////////////////////////////////////////
    X87_ARITHMETIC("fadd",  "faddp",  "fiadd",  Op::OP_FLOAT_ADD,      false, U_FADD),
    X87_ARITHMETIC("fsub",  "fsubp",  "fisub",  Op::OP_FLOAT_SUBTRACT, false, U_FADD),
    X87_ARITHMETIC("fsubr", "fsubrp", "fisubr", Op::OP_FLOAT_SUBTRACT, true,  U_FADD),
    X87_ARITHMETIC("fmul",  "fmulp",  "fimul",  Op::OP_FLOAT_MULTIPLY, false, U_FMUL),
    X87_ARITHMETIC("fdiv",  "fdivp",  "fidiv",  Op::OP_FLOAT_DIVIDE,   false, U_FDIV),
    X87_ARITHMETIC("fdivr", "fdivrp", "fidivr", Op::OP_FLOAT_DIVIDE,   true,  U_FDIV),
    X87("fsqrt",   liftSquareRoot,   U_FDIV, P_STACK, 0, 0, UC023),
    X87("frndint", liftRoundToInteger, U_FCVT, P_STACK, 0, 0, UC023),
    X87("fscale",  liftScale,        U_FMUL, P_STACK, 0, 0, UC023),

    X87("fxtract", liftExtract,  U_FADD, P_STACK, 0, 0, UC023),
    X87("fbld",    liftLoadBcd,  U_LOAD | U_FCVT, P_READ | P_STACK, 0, 0, UC023),
    X87("fbstp",   liftStoreBcd, U_STORE | U_FCVT, P_WRITE | P_STACK, 0, 0, UC023),

    //////////////////////////////////////////////////////////////////////////
    // Compare
    //////////////////////////////////////////////////////////////////////////
    X87("fcom",    (liftCompare<false, false, 0, false>), U_FADD, P_READ | P_STACK, 0, 0, 0),
    X87("fcomp",   (liftCompare<false, false, 1, false>), U_FADD, P_READ | P_STACK, 0, 0, 0),
    X87("fcompp",  (liftCompare<false, false, 2, false>), U_FADD, P_STACK, 0, 0, 0),
    X87("fucom",   (liftCompare<true,  false, 0, false>), U_FADD, P_STACK, 0, 0, 0),
    X87("fucomp",  (liftCompare<true,  false, 1, false>), U_FADD, P_STACK, 0, 0, 0),
    X87("fucompp", (liftCompare<true,  false, 2, false>), U_FADD, P_STACK, 0, 0, 0),
    X87("ficom",   (liftCompare<false, true,  0, false>), U_FADD | U_LOAD, P_READ | P_STACK, 0, 0, 0),
    X87("ficomp",  (liftCompare<false, true,  1, false>), U_FADD | U_LOAD, P_READ | P_STACK, 0, 0, 0),
    X87("ftst",    (liftCompare<false, false, 0, true>),  U_FADD, P_STACK, 0, 0, 0),
    X87("fxam",    liftExamine,                           U_FADD, P_STACK, 0, 0, 0),
    X87("fcomi",   (liftCompareToFlags<false, false>), U_FADD, P_STACK, 0, FALL, 0),
    X87("fcomip",  (liftCompareToFlags<false, true>),  U_FADD, P_STACK, 0, FALL, 0),
    X87("fucomi",  (liftCompareToFlags<true,  false>), U_FADD, P_STACK, 0, FALL, 0),
    X87("fucomip", (liftCompareToFlags<true,  true>),  U_FADD, P_STACK, 0, FALL, 0),

    //////////////////////////////////////////////////////////////////////////
    // Control
    //////////////////////////////////////////////////////////////////////////
    X87("fwait",   liftWait,             U_NONE, P_NONE, 0, 0, 0),
    X87("fnop",    liftWait,             U_NONE, P_NONE, 0, 0, UC0123),
    X87("fneni",   liftNop,              U_NONE, P_NONE, 0, 0, 0),
    X87("fndisi",  liftNop,              U_NONE, P_NONE, 0, 0, 0),
    X87("fsetpm",  liftNop,              U_NONE, P_NONE, 0, 0, 0),
    X87("fninit",  liftInitialize,       U_SYSTEM, P_STACK, 0, 0, 0),
    X87("fnclex",  liftClearExceptions,  U_SYSTEM, P_NONE, 0, 0, UC0123),
    X87("fnstsw",  liftStoreStatus,      U_STORE, P_WRITE, 0, 0, 0),
    X87("fnstcw",  liftStoreControl,     U_STORE, P_WRITE, 0, 0, 0),
    X87("fldcw",   liftLoadControl,      U_LOAD, P_READ, 0, 0, UC0123),
    X87("fnstenv", liftStoreEnvironment, U_STORE, P_WRITE | P_STACK, 0, 0, UC0123),
    X87("fldenv",  liftLoadEnvironment,  U_LOAD, P_READ | P_STACK, 0, 0, 0),
    X87("fnsave",  liftSave,             U_STORE, P_WRITE | P_STACK, 0, 0, 0),
    X87("frstor",  liftRestore,          U_LOAD, P_READ | P_STACK, 0, 0, 0),
    X87_CATEGORY("fxsave",  liftFxsave,  Cl::CATEGORY_SYSTEM, U_STORE, P_WRITE | P_STACK, 0, 0, 0),
    X87_CATEGORY("fxrstor", liftFxrstor, Cl::CATEGORY_SYSTEM, U_LOAD, P_READ | P_STACK, 0, 0, 0),

    // The transcendental functions
    X87("fprem",   liftRemainder<false>, U_FDIV, P_STACK, 0, 0, 0),
    X87("fprem1",  liftRemainder<true>,  U_FDIV, P_STACK, 0, 0, 0),
    X87("f2xm1",   liftHelper<IA32_HELPER_X87_F2XM1>,   U_FMUL, P_STACK | P_HELPER, 0, 0, UC023),
    X87("fyl2x",   liftHelper<IA32_HELPER_X87_FYL2X>,   U_FMUL, P_STACK | P_HELPER, 0, 0, UC023),
    X87("fyl2xp1", liftHelper<IA32_HELPER_X87_FYL2XP1>, U_FMUL, P_STACK | P_HELPER, 0, 0, UC023),
    X87("fpatan",  liftHelper<IA32_HELPER_X87_FPATAN>,  U_FMUL, P_STACK | P_HELPER, 0, 0, UC023),
    X87("fptan",   liftHelper<IA32_HELPER_X87_FPTAN>,   U_FMUL, P_STACK | P_HELPER, 0, 0, UC03),
    X87("fsin",    liftHelper<IA32_HELPER_X87_FSIN>,    U_FMUL, P_STACK | P_HELPER, 0, 0, UC03),
    X87("fcos",    liftHelper<IA32_HELPER_X87_FCOS>,    U_FMUL, P_STACK | P_HELPER, 0, 0, UC03),
    X87("fsincos", liftHelper<IA32_HELPER_X87_FSINCOS>, U_FMUL, P_STACK | P_HELPER, 0, 0, UC03),

    // End marker
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL }
};

} // namespace ia32dis
