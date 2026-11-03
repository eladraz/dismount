/*
 * IA32SemanticTableSimd.cpp
 *
 * The semantics of the IA-32 MMX, SSE and SSE2 instructions, see
 * IA32Semantics.h.
 *
 * The registers are integers (mm: i64, xmm: i128): an instruction bitcasts
 * them to the vector type it works on (v16i8, v8i16, v4i32, v2i64, v4f32,
 * v2f64, and the 64 bit vectors of MMX) and back. The comparisons give a mask
 * (one bit per lane) which OP_SELECT turns into the all ones / all zeros
 * lanes of the processor.
 *
 * What a real processor does, and the lifters follow:
 *   - MMX: writing mm(i) writes the x87 register i as (value | 0xFFFF << 64).
 *     Every instruction with an mm operand sets TOP to 0 and all the tags
 *     valid, see IA32SemanticLifter::lift(). emms empties the tags.
 *   - A 128 bit memory operand must be 16 byte aligned, otherwise #GP (13);
 *     the unaligned moves (movups, movupd, movdqu) and the operands of 32,
 *     64 bits have no such rule.
 *   - The scalar SSE instructions change the low lane only: the upper lanes
 *     of the destination are kept (loading from memory zeroes them: movss,
 *     movsd, movd, movq).
 *   - The shifts by a count of the element size or more give 0 (the
 *     arithmetic right shifts give the sign), the count of the shifts by a
 *     register is its low 64 bits.
 *   - The floating point operations use MXCSR (the rounding mode, FTZ, DAZ,
 *     the exception flags and masks, an unmasked exception is #XM), the NaN
 *     rules are the SSE ones. A float to integer conversion of a NaN or a
 *     value out of range gives the "integer indefinite" (0x80000000).
 *   - rcpps / rsqrtps (and the scalar forms) are estimates from tables which
 *     Intel doesn't publish: they are helpers.
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
typedef SemanticType Type;

/////////////////////////////////////////////////////////////////////////////
// The lifters
/////////////////////////////////////////////////////////////////////////////

namespace {

// The exception vector of a misaligned operand and of a reserved MXCSR bit
const uint VECTOR_GENERAL_PROTECTION = 13;

// The reserved bits of MXCSR (bits 16..31)
const uint32 MXCSR_RESERVED = 0xFFFF0000;

// The flags written by the comiss family
const uint32 FALL = SemanticFlags::FLAGS_ARITHMETIC;

/////////////////////////////////////////////////////////////////////////////
// Types and small operations
/////////////////////////////////////////////////////////////////////////////

SemanticType integerType(uint bits)
{
    return SemanticType::integer(bits);
}

// A vector of integers of 'elementBits' filling 'totalBits'
SemanticType integerVector(uint elementBits, uint totalBits)
{
    return SemanticType(SemanticType::KIND_INTEGER, elementBits,
                        totalBits / elementBits);
}

// A float of 32 (single) or 64 (double) bits
SemanticType floatScalar(uint bits)
{
    return SemanticType::floating((bits == 32) ? SemanticType::FLOAT_SINGLE :
                                                 SemanticType::FLOAT_DOUBLE);
}

// A vector of floats of 'elementBits' filling 'totalBits'
SemanticType floatVector(uint elementBits, uint totalBits)
{
    return SemanticType(SemanticType::KIND_FLOAT, elementBits,
                        totalBits / elementBits,
                        (elementBits == 32) ? SemanticType::FLOAT_SINGLE :
                                              SemanticType::FLOAT_DOUBLE);
}

SemanticOperand immediate(Context& c, int64 value, uint bits)
{
    return c.getImmediate(value, integerType(bits));
}

// A vector constant whose bits are all ones / all zeros
SemanticOperand allOnes(Context& c, const SemanticType& type)
{
    return c.getImmediate(-1, type);
}

SemanticOperand allZeros(Context& c, const SemanticType& type)
{
    return c.getImmediate(0, type);
}

// Return 'value' as a value of 'type' (the same size, the same bits)
SemanticOperand cast(Context& c, const SemanticOperand& value,
                     const SemanticType& type)
{
    if (value.getType() == type)
        return value;
    CHECK(value.getType().getTotalBits() == type.getTotalBits());
    return c.emit(Op::OP_BITCAST, type, value);
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

// Compare the lanes of two vectors, and return the lanes of the type
// 'laneType' (all ones where the condition is true, all zeros otherwise)
SemanticOperand compareLanes(Context& c, Op::Condition condition,
                             const SemanticOperand& first,
                             const SemanticOperand& second,
                             const SemanticType& laneType,
                             bool isFloat, bool isNegated)
{
    SemanticOperand mask = c.newTemporary(
                    SemanticType::mask(first.getType().getLanes()));
    SemanticInstruction instruction(Op::OP_COMPARE, first.getType(), mask,
                                    first, second);
    instruction.setCondition(condition);
    if (isFloat)
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    c.emit(instruction);
    SemanticOperand ones = allOnes(c, laneType);
    SemanticOperand zeros = allZeros(c, laneType);
    return c.emit(Op::OP_SELECT, laneType, mask,
                  isNegated ? zeros : ones, isNegated ? ones : zeros);
}

// Emit a floating point operation: the rounding mode of MXCSR unless the
// instruction fixes it, and the exception flags of MXCSR
SemanticOperand emitFloat(Context& c, Op::Operation operation,
                          const SemanticType& type,
                          const SemanticOperand& source0,
                          const SemanticOperand& source1 = SemanticOperand(),
                          Op::RoundingMode rounding = Op::ROUND_DYNAMIC)
{
    SemanticOperand result = c.newTemporary(type);
    SemanticInstruction instruction(operation, type, result, source0, source1);
    instruction.setRoundingMode(rounding);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    c.emit(instruction);
    return result;
}

/////////////////////////////////////////////////////////////////////////////
// The operands
/////////////////////////////////////////////////////////////////////////////

bool isMmxRegister(uint registerId)
{
    return (registerId >= IA32_REG_MM0) && (registerId <= IA32_REG_MM7);
}

// Return true if operand 'index' is an mm register
bool isMmxOperand(Context& c, uint index)
{
    const IA32DecodedOperand& operand = c.getOperand(index);
    return (operand.m_kind == IA32DecodedOperand::KIND_REGISTER) &&
           isMmxRegister(operand.m_register);
}

// Return true if the instruction works on mm registers
bool usesMmx(Context& c)
{
    for (uint i = 0; i < c.getOperandsCount(); i++)
        if (isMmxOperand(c, i))
            return true;
    return false;
}

// The size of the vectors of the instruction: 64 bits for MMX, 128 for SSE
uint vectorBits(Context& c)
{
    return usesMmx(c) ? 64 : 128;
}

bool isMemoryOperand(Context& c, uint index)
{
    return c.getOperand(index).m_kind == IA32DecodedOperand::KIND_MEMORY;
}

// Raise #GP unless the memory operand 'index' is aligned to 16 bytes
void checkAlignment(Context& c, uint index)
{
    SemanticOperand address = c.getAddress(index);
    const uint bits = address.getType().getTotalBits();
    SemanticOperand low = c.emit(Op::OP_AND, integerType(bits), address,
                                 immediate(c, 15, bits));
    trapUnless(c, compare(c, Op::COND_EQUAL, low, immediate(c, 0, bits)),
               VECTOR_GENERAL_PROTECTION);
}

// Return the value of operand 'index' as an integer of its size: the register
// or the loaded memory. A 128 bit memory operand must be aligned, unless the
// instruction is an unaligned move
SemanticOperand readSimd(Context& c, uint index, bool isUnaligned = false)
{
    const IA32DecodedOperand& operand = c.getOperand(index);
    if (isMemoryOperand(c, index) && (operand.m_bits == 128))
    {
        if (isUnaligned)
        {
            // The load may be unaligned
            SemanticOperand result = c.newTemporary(integerType(128));
            SemanticInstruction load(Op::OP_LOAD, integerType(128), result,
                                     c.getAddress(index));
            load.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT |
                               SemanticInstruction::ATTRIBUTE_UNALIGNED);
            c.emit(load);
            return result;
        }
        checkAlignment(c, index);
    }
    return c.read(index);
}

// Write mm register 'registerId': the x87 register is written with the
// exponent and the sign set to ones
void writeMmx(Context& c, uint registerId, const SemanticOperand& value)
{
    SemanticOperand extended = c.emit(Op::OP_CONCAT, integerType(80),
                                      cast(c, value, integerType(64)),
                                      immediate(c, 0xFFFF, 16));
    c.move(c.getRegister(IA32_REG_FPR0 + (registerId - IA32_REG_MM0)), extended);
}

// Write operand 'index' (a register or memory). A value smaller than the
// register is zero extended
void writeSimd(Context& c, uint index, const SemanticOperand& value,
               bool isUnaligned = false)
{
    const IA32DecodedOperand& operand = c.getOperand(index);
    if (operand.m_kind == IA32DecodedOperand::KIND_REGISTER)
    {
        SemanticOperand integer = cast(c, resize(c, cast(c, value,
                    integerType(value.getType().getTotalBits())),
                    operand.m_bits), integerType(operand.m_bits));
        if (isMmxRegister(operand.m_register))
            writeMmx(c, operand.m_register, integer);
        else
            c.move(c.getRegister(operand.m_register), integer);
        return;
    }
    CHECK(operand.m_kind == IA32DecodedOperand::KIND_MEMORY);
    SemanticOperand integer = cast(c, value,
                    integerType(value.getType().getTotalBits()));
    if (operand.m_bits == 128)
    {
        if (isUnaligned)
        {
            SemanticInstruction store(Op::OP_STORE, integerType(128),
                                      SemanticOperand(), c.getAddress(index),
                                      integer);
            store.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT |
                                SemanticInstruction::ATTRIBUTE_UNALIGNED);
            c.emit(store);
            return;
        }
        checkAlignment(c, index);
    }
    c.store(c.getAddress(index), integer);
}

// Return the low 'bits' bits of operand 'index' (an xmm register or a memory
// operand of 'bits' bits) as a float of that size
SemanticOperand readScalarFloat(Context& c, uint index, uint bits)
{
    return cast(c, resize(c, c.read(index), bits), floatScalar(bits));
}

// Replace the low bits of the xmm register of operand 'index' by 'value',
// the upper lanes are kept
void writeLow(Context& c, uint index, const SemanticOperand& value)
{
    const uint bits = value.getType().getTotalBits();
    SemanticOperand merged = c.emit(Op::OP_INSERT, integerType(128),
            c.read(index), cast(c, value, integerType(bits)),
            immediate(c, 0, 32));
    c.write(index, merged);
}

// The number (0 = eax) of the register of a decoded general operand, which
// is 16 or 32 bits by the operand size
uint generalNumberOf(uint registerId)
{
    if ((registerId >= IA32_REG_AX) && (registerId <= IA32_REG_DI))
        return registerId - IA32_REG_AX;
    CHECK((registerId >= IA32_REG_EAX) && (registerId <= IA32_REG_EDI));
    return registerId - IA32_REG_EAX;
}

// The size of the operands r/m32 of the SSE instructions: 32 bits whatever
// the operand size is, 64 bits with REX.W in 64 bit mode
uint generalBits(Context& c, uint index)
{
    return c.is64BitMode() ? c.getOperand(index).m_bits : 32;
}

SemanticOperand readGeneral32(Context& c, uint index)
{
    const IA32DecodedOperand& operand = c.getOperand(index);
    if (operand.m_kind == IA32DecodedOperand::KIND_REGISTER)
    {
        if (c.is64BitMode())
            return c.getRegister(operand.m_register);
        return c.getGeneralRegister(generalNumberOf(operand.m_register), 32);
    }
    return c.load(c.getAddress(index), integerType(generalBits(c, index)));
}

void writeGeneral32(Context& c, uint index, const SemanticOperand& value)
{
    const IA32DecodedOperand& operand = c.getOperand(index);
    if (operand.m_kind == IA32DecodedOperand::KIND_REGISTER)
    {
        if (c.is64BitMode())
            c.move(c.getRegister(operand.m_register), value);
        else
            c.move(c.getGeneralRegister(generalNumberOf(operand.m_register), 32),
                   value);
    }
    else
        c.store(c.getAddress(index), value);
}

/////////////////////////////////////////////////////////////////////////////
// Moves
/////////////////////////////////////////////////////////////////////////////

// movaps, movapd, movdqa, movups, movupd, movdqu, movntps, movntpd, movntdq,
// movntq: the whole register / 128 or 64 bits of memory
template <bool IS_UNALIGNED>
void liftMove(Context& c)
{
    writeSimd(c, 0, readSimd(c, 1, IS_UNALIGNED), IS_UNALIGNED);
}

// movss, movsd: xmm <- xmm keeps the upper lanes, xmm <- memory zeroes them,
// memory <- xmm stores the low lane
template <uint BITS>
void liftMoveScalar(Context& c)
{
    if (isMemoryOperand(c, 0))
    {
        c.store(c.getAddress(0), resize(c, c.read(1), BITS));
    } else if (isMemoryOperand(c, 1))
    {
        c.write(0, resize(c, c.read(1), 128));
    } else
    {
        writeLow(c, 0, resize(c, c.read(1), BITS));
    }
}

// movlps, movlpd: the low 64 bits, from / to memory. The upper lanes are kept
void liftMoveLow(Context& c)
{
    if (isMemoryOperand(c, 0))
        c.store(c.getAddress(0), resize(c, c.read(1), 64));
    else
        writeLow(c, 0, c.read(1));
}

// movhps, movhpd: the high 64 bits, from / to memory
void liftMoveHigh(Context& c)
{
    if (isMemoryOperand(c, 0))
    {
        c.store(c.getAddress(0), c.emit(Op::OP_EXTRACT, integerType(64),
                                        c.read(1), immediate(c, 64, 32)));
        return;
    }
    SemanticOperand merged = c.emit(Op::OP_INSERT, integerType(128),
            c.read(0), c.read(1), immediate(c, 64, 32));
    c.write(0, merged);
}

// movhlps xmm1, xmm2: the low lane of xmm1 is the high lane of xmm2
void liftMoveHighToLow(Context& c)
{
    writeLow(c, 0, c.emit(Op::OP_EXTRACT, integerType(64), c.read(1),
                          immediate(c, 64, 32)));
}

// movlhps xmm1, xmm2: the high lane of xmm1 is the low lane of xmm2
void liftMoveLowToHigh(Context& c)
{
    SemanticOperand merged = c.emit(Op::OP_INSERT, integerType(128),
            c.read(0), resize(c, c.read(1), 64), immediate(c, 64, 32));
    c.write(0, merged);
}

// movd: between an mm / xmm register and r/m32. The upper bits of the
// register are zeroed
void liftMoveDword(Context& c)
{
    const IA32DecodedOperand& destination = c.getOperand(0);
    if ((destination.m_kind == IA32DecodedOperand::KIND_REGISTER) &&
        ((destination.m_register >= IA32_REG_MM0) &&
         (destination.m_register <= IA32_REG_XMM7)))
    {
        // mm / xmm <- r/m32
        writeSimd(c, 0, readGeneral32(c, 1));
    } else
    {
        // r/m32 <- mm / xmm
        writeGeneral32(c, 0, resize(c, c.read(1), generalBits(c, 0)));
    }
}

// movq: the low 64 bits between mm / xmm / memory. A register destination
// is zero extended (xmm <- xmm / m64), movq2dq and movdq2q included
void liftMoveQword(Context& c)
{
    writeSimd(c, 0, resize(c, c.read(1), 64));
}

// maskmovq, maskmovdqu: store the bytes of operand 0 whose byte of operand 1
// has the sign bit set, to ds:edi. The other bytes don't fault
void liftMaskedMove(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    const SemanticType type = integerVector(8, bits);
    SemanticOperand data = cast(c, c.read(0), type);
    SemanticOperand mask = c.emit(Op::OP_SIGN_MASK, integerType(type.getLanes()),
                                  cast(c, c.read(1), type));
    const uint segment = IA32OperandDecoder::getSegment(c.getInstruction(), IA32_REG_DS);
    const uint addressBits = c.getAddressSizeType().getTotalBits();
    SemanticOperand address = c.getLinearAddress(segment,
                c.getGeneralRegister(IA32_GP32_EDI, addressBits));
    SemanticInstruction store(Op::OP_STORE_MASKED, type, SemanticOperand(),
                              address, data, mask);
    store.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    c.emit(store);
}

/////////////////////////////////////////////////////////////////////////////
// Packed integers
/////////////////////////////////////////////////////////////////////////////

// d = op(d, s) on the lanes of ELEMENT_BITS (padd, psub, the saturating and
// the average, min, max and multiply)
template <Op::Operation OPERATION, uint ELEMENT_BITS>
void liftPackedBinary(Context& c)
{
    const SemanticType type = integerVector(ELEMENT_BITS, vectorBits(c));
    SemanticOperand source = cast(c, readSimd(c, 1), type);
    SemanticOperand destination = cast(c, c.read(0), type);
    writeSimd(c, 0, c.emit(OPERATION, type, destination, source));
}

// pcmpeq, pcmpgt: all ones in the lanes where the condition holds
template <Op::Condition CONDITION, uint ELEMENT_BITS>
void liftPackedCompare(Context& c)
{
    const SemanticType type = integerVector(ELEMENT_BITS, vectorBits(c));
    SemanticOperand source = cast(c, readSimd(c, 1), type);
    SemanticOperand destination = cast(c, c.read(0), type);
    writeSimd(c, 0, compareLanes(c, CONDITION, destination, source, type,
                                 false, false));
}

// Shifts by an immediate or by a count in a register or memory: a count of
// the element size or more gives 0 (the sign for psra)
template <Op::Operation OPERATION, uint ELEMENT_BITS>
void liftPackedShift(Context& c)
{
    const SemanticType type = integerVector(ELEMENT_BITS, vectorBits(c));
    SemanticOperand count;
    if (c.getOperand(1).m_kind == IA32DecodedOperand::KIND_IMMEDIATE)
    {
        count = immediate(c, c.getOperand(1).m_immediate & 0xFF, ELEMENT_BITS);
    } else
    {
        // The count is the low 64 bits of the source: a larger count than
        // any element is clamped to 255 before it is cut to the element
        SemanticOperand raw = resize(c, readSimd(c, 1), 64);
        SemanticOperand isLarge = compare(c, Op::COND_UNSIGNED_GREATER, raw,
                                          immediate(c, 255, 64));
        SemanticOperand clamped = c.emit(Op::OP_SELECT, integerType(64),
                                         isLarge, immediate(c, 255, 64), raw);
        count = resize(c, clamped, ELEMENT_BITS);
    }
    SemanticOperand counts = c.emit(Op::OP_BROADCAST, type, count);
    SemanticOperand destination = cast(c, c.read(0), type);
    writeSimd(c, 0, c.emit(OPERATION, type, destination, counts));
}

// pslldq, psrldq: shift the whole register by bytes
template <bool IS_LEFT>
void liftByteShift(Context& c)
{
    uint count = (uint)(c.getOperand(1).m_immediate & 0xFF);
    if (count > 16)
        count = 16;
    const SemanticType type = integerVector(8, 128);
    SemanticOperand value = cast(c, c.read(0), type);
    SemanticOperand zeros = allZeros(c, type);
    // The lanes of (first:second) starting at 'start'
    SemanticOperand result = IS_LEFT ?
        c.emit(Op::OP_SLIDE, type, zeros, value, immediate(c, 16 - count, 32)) :
        c.emit(Op::OP_SLIDE, type, value, zeros, immediate(c, count, 32));
    writeSimd(c, 0, result);
}

// pand, pandn, por, pxor and the float forms: bit by bit on the register
template <Op::Operation OPERATION, uint ELEMENT_BITS>
void liftLogic(Context& c)
{
    liftPackedBinary<OPERATION, ELEMENT_BITS>(c);
}

// pmullw/pmulhw/pmulhuw are liftPackedBinary. pmuludq: the low dwords of
// each qword are multiplied into a qword
void liftMultiplyUnsignedQword(Context& c)
{
    const SemanticType type = integerVector(64, vectorBits(c));
    SemanticOperand mask = c.emit(Op::OP_BROADCAST, type,
                                  immediate(c, 0xFFFFFFFF, 64));
    SemanticOperand source = c.emit(Op::OP_AND, type,
                                    cast(c, readSimd(c, 1), type), mask);
    SemanticOperand destination = c.emit(Op::OP_AND, type,
                                         cast(c, c.read(0), type), mask);
    writeSimd(c, 0, c.emit(Op::OP_MULTIPLY, type, destination, source));
}

// pmaddwd: the signed words are multiplied and the adjacent products added
void liftMultiplyAdd(Context& c)
{
    const uint bits = vectorBits(c);
    const SemanticType wordType = integerVector(16, bits);
    const SemanticType dwordType = integerVector(32, bits);
    SemanticOperand source = cast(c, readSimd(c, 1), wordType);
    SemanticOperand destination = cast(c, c.read(0), wordType);
    SemanticOperand result = c.newTemporary(dwordType);
    SemanticInstruction dot(Op::OP_DOT_PRODUCT, dwordType, result, destination,
                            source, allZeros(c, dwordType));
    dot.setBehavior(Op::BEHAVIOR_SIGNED);
    c.emit(dot);
    writeSimd(c, 0, result);
}

// psadbw: the sums of the absolute differences of the bytes, one 16 bit sum
// for each qword
void liftSumAbsoluteDifferences(Context& c)
{
    const uint bits = vectorBits(c);
    const SemanticType byteType = integerVector(8, bits);
    const SemanticType qwordType = integerVector(64, bits);
    SemanticOperand source = cast(c, readSimd(c, 1), byteType);
    SemanticOperand destination = cast(c, c.read(0), byteType);
    SemanticOperand differences = c.emit(Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED,
                                         byteType, destination, source);
    SemanticOperand ones = c.emit(Op::OP_BROADCAST, byteType, immediate(c, 1, 8));
    SemanticOperand result = c.newTemporary(qwordType);
    SemanticInstruction dot(Op::OP_DOT_PRODUCT, qwordType, result, differences,
                            ones, allZeros(c, qwordType));
    dot.setBehavior(Op::BEHAVIOR_UNSIGNED);
    c.emit(dot);
    writeSimd(c, 0, result);
}

// packsswb, packssdw, packuswb: the signed lanes of the destination then the
// source are narrowed with saturation
template <uint SOURCE_BITS, bool IS_UNSIGNED>
void liftPack(Context& c)
{
    const uint bits = vectorBits(c);
    const SemanticType sourceType = integerVector(SOURCE_BITS, bits);
    const SemanticType resultType = integerVector(SOURCE_BITS / 2, bits);
    SemanticOperand source = cast(c, readSimd(c, 1), sourceType);
    SemanticOperand destination = cast(c, c.read(0), sourceType);
    SemanticOperand result = c.newTemporary(resultType);
    SemanticInstruction narrow(Op::OP_NARROW, resultType, result, destination,
                               source);
    narrow.setAttributes(IS_UNSIGNED ? SemanticInstruction::ATTRIBUTE_SATURATE_UNSIGNED :
                                       SemanticInstruction::ATTRIBUTE_SATURATE_SIGNED);
    c.emit(narrow);
    writeSimd(c, 0, result);
}

// punpckl*, punpckh*, unpckl*, unpckh*: the lanes of the low / high halves
// interleaved
template <uint ELEMENT_BITS, bool IS_HIGH>
void liftUnpack(Context& c)
{
    const uint bits = vectorBits(c);
    const SemanticType type = integerVector(ELEMENT_BITS, bits);
    SemanticOperand source;
    if (usesMmx(c) && !IS_HIGH && isMemoryOperand(c, 1))
    {
        // The memory operand of the low unpacks of MMX is 32 bits
        source = cast(c, resize(c, c.load(c.getAddress(1), integerType(32)), 64),
                      type);
    } else
    {
        source = cast(c, readSimd(c, 1), type);
    }
    SemanticOperand destination = cast(c, c.read(0), type);
    const uint start = IS_HIGH ? (type.getLanes() / 2) : 0;
    writeSimd(c, 0, c.emit(Op::OP_INTERLEAVE, type, destination, source,
                           immediate(c, start, 32)));
}

// The lane indexes of a shuffle as a constant: 8 bits for each lane
SemanticOperand laneIndexes(Context& c, const uint* indexes, uint lanes)
{
    uint64 value = 0;
    for (uint i = 0; i < lanes; i++)
        value |= (uint64)(indexes[i] & 0xFF) << (8 * i);
    return c.getImmediate((int64)value, integerVector(8, lanes * 8));
}

// pshufd, pshufw, pshuflw, pshufhw: 4 lanes starting at FIRST are shuffled by
// the two bit fields of the immediate, the others are kept
template <uint ELEMENT_BITS, uint FIRST>
void liftShuffle(Context& c)
{
    const uint bits = vectorBits(c);
    const SemanticType type = integerVector(ELEMENT_BITS, bits);
    const uint lanes = type.getLanes();
    const uint control = (uint)(c.getOperand(2).m_immediate & 0xFF);
    uint indexes[16];
    for (uint i = 0; i < lanes; i++)
        indexes[i] = ((i >= FIRST) && (i < FIRST + 4)) ?
                        (FIRST + ((control >> (2 * (i - FIRST))) & 3)) : i;
    SemanticOperand source = cast(c, readSimd(c, 1), type);
    writeSimd(c, 0, c.emit(Op::OP_SHUFFLE, type, source, source,
                           laneIndexes(c, indexes, lanes)));
}

// shufps / shufpd: the low lanes of the result come from the destination, the
// high lanes from the source
template <uint ELEMENT_BITS>
void liftShuffleFloat(Context& c)
{
    const SemanticType type = integerVector(ELEMENT_BITS, 128);
    const uint lanes = type.getLanes();
    const uint control = (uint)(c.getOperand(2).m_immediate & 0xFF);
    const uint fieldBits = (lanes == 4) ? 2 : 1;
    uint indexes[4];
    for (uint i = 0; i < lanes; i++)
        indexes[i] = ((control >> (fieldBits * i)) & (lanes - 1)) +
                     ((i >= lanes / 2) ? lanes : 0);
    SemanticOperand source = cast(c, readSimd(c, 1), type);
    SemanticOperand destination = cast(c, c.read(0), type);
    writeSimd(c, 0, c.emit(Op::OP_SHUFFLE, type, destination, source,
                           laneIndexes(c, indexes, lanes)));
}

// pextrw r32, mm/xmm, imm8: the word, zero extended
void liftExtractWord(Context& c)
{
    const SemanticType type = integerVector(16, vectorBits(c));
    SemanticOperand lane = immediate(c, c.getOperand(2).m_immediate &
                                        (type.getLanes() - 1), 32);
    SemanticOperand word = c.emit(Op::OP_EXTRACT, integerType(16),
                                  cast(c, c.read(1), type), lane);
    c.write(0, c.emit(Op::OP_ZERO_EXTEND, integerType(32), word));
}

// pinsrw mm/xmm, r32/m16, imm8: the low word replaces the word of the lane
void liftInsertWord(Context& c)
{
    const SemanticType type = integerVector(16, vectorBits(c));
    SemanticOperand word;
    if (isMemoryOperand(c, 1))
        word = c.load(c.getAddress(1), integerType(16));
    else
        word = resize(c, readGeneral32(c, 1), 16);
    SemanticOperand lane = immediate(c, c.getOperand(2).m_immediate &
                                        (type.getLanes() - 1), 32);
    SemanticOperand result = c.emit(Op::OP_INSERT, type,
                                    cast(c, c.read(0), type), word, lane);
    writeSimd(c, 0, result);
}

// pmovmskb, movmskps, movmskpd: the sign bits of the lanes, zero extended
// into a general register
template <uint ELEMENT_BITS>
void liftMoveMask(Context& c)
{
    const SemanticType type = integerVector(ELEMENT_BITS, vectorBits(c));
    SemanticOperand mask = c.emit(Op::OP_SIGN_MASK, integerType(type.getLanes()),
                                  cast(c, c.read(1), type));
    c.write(0, c.emit(Op::OP_ZERO_EXTEND, integerType(32), mask));
}

/////////////////////////////////////////////////////////////////////////////
// Floating point arithmetic
/////////////////////////////////////////////////////////////////////////////

// addps, subps, mulps, divps, minps, maxps (and pd): d = op(d, s) on all lanes
template <Op::Operation OPERATION, uint ELEMENT_BITS>
void liftFloatPacked(Context& c)
{
    const SemanticType type = floatVector(ELEMENT_BITS, 128);
    SemanticOperand source = cast(c, readSimd(c, 1), type);
    SemanticOperand destination = cast(c, c.read(0), type);
    writeSimd(c, 0, emitFloat(c, OPERATION, type, destination, source));
}

// addss, subss, mulss, divss, minss, maxss (and sd): the low lane only
template <Op::Operation OPERATION, uint ELEMENT_BITS>
void liftFloatScalar(Context& c)
{
    SemanticOperand source = readScalarFloat(c, 1, ELEMENT_BITS);
    SemanticOperand destination = readScalarFloat(c, 0, ELEMENT_BITS);
    writeLow(c, 0, emitFloat(c, OPERATION, floatScalar(ELEMENT_BITS),
                             destination, source));
}

// sqrtps / sqrtpd: d = sqrt(s)
template <uint ELEMENT_BITS>
void liftSquareRootPacked(Context& c)
{
    const SemanticType type = floatVector(ELEMENT_BITS, 128);
    SemanticOperand source = cast(c, readSimd(c, 1), type);
    writeSimd(c, 0, emitFloat(c, Op::OP_FLOAT_SQUARE_ROOT, type, source));
}

// sqrtss / sqrtsd: the low lane of d = sqrt(s)
template <uint ELEMENT_BITS>
void liftSquareRootScalar(Context& c)
{
    SemanticOperand source = readScalarFloat(c, 1, ELEMENT_BITS);
    writeLow(c, 0, emitFloat(c, Op::OP_FLOAT_SQUARE_ROOT,
                             floatScalar(ELEMENT_BITS), source));
}

// rcpps, rsqrtps, rcpss, rsqrtss: an estimate from the table of the
// processor, a helper
template <uint HELPER, uint ELEMENT_BITS, bool IS_SCALAR>
void liftEstimate(Context& c)
{
    const SemanticType type = IS_SCALAR ? floatScalar(ELEMENT_BITS) :
                                          floatVector(ELEMENT_BITS, 128);
    SemanticOperand source = IS_SCALAR ? readScalarFloat(c, 1, ELEMENT_BITS) :
                                         cast(c, readSimd(c, 1), type);
    SemanticOperand result = c.newTemporary(type);
    c.emit(SemanticInstruction(Op::OP_HELPER, type, result,
                               immediate(c, HELPER, 32), source));
    if (IS_SCALAR)
        writeLow(c, 0, result);
    else
        writeSimd(c, 0, result);
}

// cmpps, cmppd, cmpss, cmpsd: the predicate is the immediate. EQ, UNORD, NEQ
// and ORD raise invalid for a signaling NaN only, the others for any NaN
template <uint ELEMENT_BITS, bool IS_SCALAR>
void liftFloatCompare(Context& c)
{
    const SemanticType type = IS_SCALAR ? floatScalar(ELEMENT_BITS) :
                                          floatVector(ELEMENT_BITS, 128);
    const SemanticType laneType = IS_SCALAR ? integerType(ELEMENT_BITS) :
                                              integerVector(ELEMENT_BITS, 128);
    Op::Condition condition;
    bool isNegated = false;
    switch (c.getOperand(2).m_immediate & 7)
    {
    case 0:  condition = Op::COND_FLOAT_EQUAL; break;
    case 1:  condition = Op::COND_FLOAT_LESS; break;
    case 2:  condition = Op::COND_FLOAT_LESS_EQUAL; break;
    case 3:  condition = Op::COND_FLOAT_UNORDERED; break;
    case 4:  condition = Op::COND_FLOAT_NOT_EQUAL; break;
    // not less / not less or equal are unordered or the opposite: they signal
    // like the predicates they negate
    case 5:  condition = Op::COND_FLOAT_LESS; isNegated = true; break;
    case 6:  condition = Op::COND_FLOAT_LESS_EQUAL; isNegated = true; break;
    default: condition = Op::COND_FLOAT_ORDERED; break;
    }
    SemanticOperand source = IS_SCALAR ? readScalarFloat(c, 1, ELEMENT_BITS) :
                                         cast(c, readSimd(c, 1), type);
    SemanticOperand destination = IS_SCALAR ? readScalarFloat(c, 0, ELEMENT_BITS) :
                                              cast(c, c.read(0), type);
    SemanticOperand result = compareLanes(c, condition, destination, source,
                                          laneType, true, isNegated);
    if (IS_SCALAR)
        writeLow(c, 0, result);
    else
        writeSimd(c, 0, result);
}

// comiss, comisd, ucomiss, ucomisd: ZF PF CF from the relation of the low
// lanes, OF SF AF cleared. comis raises invalid for a quiet NaN too
template <uint ELEMENT_BITS, bool IS_SIGNALING>
void liftFloatCompareFlags(Context& c)
{
    SemanticOperand source = readScalarFloat(c, 1, ELEMENT_BITS);
    SemanticOperand destination = readScalarFloat(c, 0, ELEMENT_BITS);
    SemanticInstruction flags(Op::OP_SET_FLAGS, floatScalar(ELEMENT_BITS),
                              SemanticOperand(), destination, source);
    flags.setFlags(Op::FLAGS_FLOAT_COMPARE, FALL);
    flags.setKind(IS_SIGNALING ? Op::FLOAT_COMPARE_SIGNALING :
                                 Op::FLOAT_COMPARE_QUIET);
    flags.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    c.emit(flags);
}

/////////////////////////////////////////////////////////////////////////////
// Conversions
/////////////////////////////////////////////////////////////////////////////

// Return the low 'bits' bits of operand 'index' (a register or memory) as a
// vector of type 'type'
SemanticOperand readLowAs(Context& c, uint index, const SemanticType& type)
{
    return cast(c, resize(c, readSimd(c, index), type.getTotalBits()), type);
}

// cvtdq2ps: 4 signed dwords to 4 singles
void liftConvertDwordsToSingles(Context& c)
{
    const SemanticType resultType = floatVector(32, 128);
    SemanticOperand source = cast(c, readSimd(c, 1), integerVector(32, 128));
    writeSimd(c, 0, emitFloat(c, Op::OP_SIGNED_TO_FLOAT, resultType, source));
}

// cvtdq2pd: the 2 low signed dwords to 2 doubles. cvtpi2pd: the same from mm
void liftConvertDwordsToDoubles(Context& c)
{
    SemanticOperand source = readLowAs(c, 1, integerVector(32, 64));
    writeSimd(c, 0, emitFloat(c, Op::OP_SIGNED_TO_FLOAT, floatVector(64, 128),
                              source));
}

// cvtpi2ps: the 2 signed dwords of mm / m64 to 2 singles, in the low lane of
// the xmm register (the upper lane is kept)
void liftConvertPackedIntegersToSingles(Context& c)
{
    SemanticOperand source = readLowAs(c, 1, integerVector(32, 64));
    writeLow(c, 0, emitFloat(c, Op::OP_SIGNED_TO_FLOAT, floatVector(32, 64),
                             source));
}

// cvtps2pd: the 2 low singles to 2 doubles
void liftConvertSinglesToDoubles(Context& c)
{
    SemanticOperand source = readLowAs(c, 1, floatVector(32, 64));
    writeSimd(c, 0, emitFloat(c, Op::OP_FLOAT_CONVERT, floatVector(64, 128),
                              source));
}

// cvtpd2ps: 2 doubles to 2 singles in the low lane, the upper lane is zeroed
void liftConvertDoublesToSingles(Context& c)
{
    SemanticOperand source = cast(c, readSimd(c, 1), floatVector(64, 128));
    writeSimd(c, 0, emitFloat(c, Op::OP_FLOAT_CONVERT, floatVector(32, 64),
                              source));
}

// cvtps2dq, cvttps2dq, cvtpd2dq, cvttpd2dq, cvtps2pi, cvttps2pi, cvtpd2pi,
// cvttpd2pi: SOURCE_LANES floats to signed dwords (the rest of the xmm
// destination is zeroed). The truncating forms round toward zero, the others
// by MXCSR. A NaN or a value out of range is the integer indefinite
template <uint SOURCE_BITS, uint SOURCE_LANES, bool IS_TRUNCATE>
void liftConvertFloatsToDwords(Context& c)
{
    const uint sourceTotal = SOURCE_BITS * SOURCE_LANES;
    SemanticOperand source = readLowAs(c, 1, floatVector(SOURCE_BITS, sourceTotal));
    writeSimd(c, 0, emitFloat(c, Op::OP_FLOAT_TO_SIGNED,
                              integerVector(32, 32 * SOURCE_LANES), source,
                              SemanticOperand(),
                              IS_TRUNCATE ? Op::ROUND_TOWARD_ZERO :
                                            Op::ROUND_DYNAMIC));
}

// cvtss2sd, cvtsd2ss: the low lane, the upper lanes are kept
template <uint SOURCE_BITS, uint RESULT_BITS>
void liftConvertScalarFloat(Context& c)
{
    SemanticOperand source = readScalarFloat(c, 1, SOURCE_BITS);
    writeLow(c, 0, emitFloat(c, Op::OP_FLOAT_CONVERT, floatScalar(RESULT_BITS),
                             source));
}

// cvtsi2ss, cvtsi2sd: a signed dword to the low lane
template <uint RESULT_BITS>
void liftConvertIntegerToScalar(Context& c)
{
    SemanticOperand source = readGeneral32(c, 1);
    writeLow(c, 0, emitFloat(c, Op::OP_SIGNED_TO_FLOAT, floatScalar(RESULT_BITS),
                             source));
}

// cvtss2si, cvtsd2si, cvttss2si, cvttsd2si: the low lane to a signed dword
template <uint SOURCE_BITS, bool IS_TRUNCATE>
void liftConvertScalarToInteger(Context& c)
{
    SemanticOperand source = readScalarFloat(c, 1, SOURCE_BITS);
    SemanticOperand result = emitFloat(c, Op::OP_FLOAT_TO_SIGNED,
                                       integerType(generalBits(c, 0)),
                                       source, SemanticOperand(),
                                       IS_TRUNCATE ? Op::ROUND_TOWARD_ZERO :
                                                     Op::ROUND_DYNAMIC);
    writeGeneral32(c, 0, result);
}

/////////////////////////////////////////////////////////////////////////////
// Carry-less multiplication
/////////////////////////////////////////////////////////////////////////////

// pclmulqdq: the carry-less product of a qword of each operand, selected by
// bit 0 and bit 4 of the immediate
void liftCarrylessMultiply(Context& c)
{
    const int64 control = c.getOperand(2).m_immediate;
    SemanticOperand source = readSimd(c, 1);
    SemanticOperand destination = c.read(0);
    SemanticOperand first = c.emit(Op::OP_EXTRACT, integerType(64), destination,
                                   immediate(c, (control & 0x01) ? 64 : 0, 32));
    SemanticOperand second = c.emit(Op::OP_EXTRACT, integerType(64), source,
                                    immediate(c, (control & 0x10) ? 64 : 0, 32));
    SemanticOperand low = c.emit(Op::OP_CARRYLESS_MULTIPLY, integerType(64),
                                 first, second);
    SemanticOperand high = c.emit(Op::OP_CARRYLESS_MULTIPLY_HIGH, integerType(64),
                                  first, second);
    writeSimd(c, 0, c.emit(Op::OP_CONCAT, integerType(128), low, high));
}

/////////////////////////////////////////////////////////////////////////////
// AES
/////////////////////////////////////////////////////////////////////////////

// aesenc, aesenclast, aesdec, aesdeclast: a round on the state in the
// destination with the round key of the source. The last round has no
// MixColumns
template <bool IS_INVERSE, bool IS_LAST>
void liftAesRound(Context& c)
{
    const SemanticType type = integerVector(8, 128);
    SemanticOperand key = cast(c, readSimd(c, 1), type);
    SemanticOperand state = c.emit(IS_INVERSE ? Op::OP_AES_INVERSE_SUB_SHIFT :
                                                Op::OP_AES_SUB_SHIFT,
                                   type, cast(c, c.read(0), type));
    if (!IS_LAST)
        state = c.emit(IS_INVERSE ? Op::OP_AES_INVERSE_MIX_COLUMNS :
                                    Op::OP_AES_MIX_COLUMNS, type, state);
    writeSimd(c, 0, c.emit(Op::OP_XOR, type, state, key));
}

// aesimc: InvMixColumns of the source
void liftAesInverseMixColumns(Context& c)
{
    const SemanticType type = integerVector(8, 128);
    writeSimd(c, 0, c.emit(Op::OP_AES_INVERSE_MIX_COLUMNS, type,
                           cast(c, readSimd(c, 1), type)));
}

// aeskeygenassist xmm1, xmm2/m128, imm8:
//   xmm1 = SubWord(X1), RotWord(SubWord(X1)) ^ rcon, SubWord(X3),
//          RotWord(SubWord(X3)) ^ rcon      (X is the source, dwords 3..0)
// SubWord is the SubBytes of a state whose four columns are the word: its
// rows are the same in every column, so ShiftRows keeps them
void liftAesKeyGenerationAssist(Context& c)
{
    const SemanticType dwords = integerVector(32, 128);
    const SemanticType bytes = integerVector(8, 128);
    const uint rcon = (uint)(c.getOperand(2).m_immediate & 0xFF);
    SemanticOperand source = cast(c, readSimd(c, 1), dwords);
    const uint firstColumns[4] = { 1, 1, 1, 1 };
    const uint thirdColumns[4] = { 3, 3, 3, 3 };
    SemanticOperand first = c.emit(Op::OP_SHUFFLE, dwords, source, source,
                                   laneIndexes(c, firstColumns, 4));
    SemanticOperand third = c.emit(Op::OP_SHUFFLE, dwords, source, source,
                                   laneIndexes(c, thirdColumns, 4));
    SemanticOperand subFirst = cast(c, c.emit(Op::OP_AES_SUB_SHIFT, bytes,
                                              cast(c, first, bytes)), dwords);
    SemanticOperand subThird = cast(c, c.emit(Op::OP_AES_SUB_SHIFT, bytes,
                                              cast(c, third, bytes)), dwords);
    // SubWord(X1), SubWord(X1), SubWord(X3), SubWord(X3)
    const uint pairs[4] = { 0, 0, 4, 4 };
    SemanticOperand words = c.emit(Op::OP_SHUFFLE, dwords, subFirst, subThird,
                                   laneIndexes(c, pairs, 4));
    SemanticOperand eight = c.emit(Op::OP_BROADCAST, dwords, immediate(c, 8, 32));
    SemanticOperand rotated = c.emit(Op::OP_ROTATE_RIGHT, dwords, words, eight);
    // The round constant in the dwords 1 and 3
    SemanticOperand constants = cast(c, c.emit(Op::OP_BROADCAST,
                integerVector(64, 128), immediate(c, (int64)rcon << 32, 64)), dwords);
    SemanticOperand rotatedConstant = c.emit(Op::OP_XOR, dwords, rotated, constants);
    const uint picks[4] = { 0, 5, 2, 7 };
    writeSimd(c, 0, c.emit(Op::OP_SHUFFLE, dwords, words, rotatedConstant,
                           laneIndexes(c, picks, 4)));
}

/////////////////////////////////////////////////////////////////////////////
// State, fences and the cache
/////////////////////////////////////////////////////////////////////////////

// ldmxcsr: a reserved bit is #GP
void liftLoadMxcsr(Context& c)
{
    SemanticOperand value = c.load(c.getAddress(0), integerType(32));
    SemanticOperand reserved = c.emit(Op::OP_AND, integerType(32), value,
                                      immediate(c, MXCSR_RESERVED, 32));
    trapUnless(c, compare(c, Op::COND_EQUAL, reserved, immediate(c, 0, 32)),
               VECTOR_GENERAL_PROTECTION);
    c.move(c.getRegister(IA32_REG_MXCSR), value);
}

void liftStoreMxcsr(Context& c)
{
    c.store(c.getAddress(0), c.getRegister(IA32_REG_MXCSR));
}

// emms: all the x87 registers are empty
void liftEmms(Context& c)
{
    c.move(c.getRegister(IA32_REG_FPU_TAG), immediate(c, 0, 8));
}

// lfence, sfence, mfence
template <uint ACCESS, Op::MemoryOrder ORDER>
void liftFence(Context& c)
{
    SemanticInstruction fence(Op::OP_FENCE, SemanticType(), SemanticOperand());
    fence.setMemoryOrder(ORDER);
    fence.setFenceAccess((uint8)ACCESS);
    c.emit(fence);
}

// prefetchnta, prefetcht0, prefetcht1, prefetcht2: a hint with the address
void liftPrefetch(Context& c)
{
    // With a register operand it's a nop
    if (isMemoryOperand(c, 0))
        c.emit(SemanticInstruction(Op::OP_PREFETCH, SemanticType(),
                                   SemanticOperand(), c.getAddress(0)));
}

// clflush: write the line back and invalidate it
void liftFlushLine(Context& c)
{
    SemanticInstruction flush(Op::OP_CACHE_MAINTENANCE, SemanticType(),
                              SemanticOperand(), c.getAddress(0));
    flush.setKind(Op::CACHE_FLUSH);
    c.emit(flush);
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// The table
/////////////////////////////////////////////////////////////////////////////

namespace ia32dis {

namespace {

// The categories
const uint8 CAT_MOVE        = Cl::CATEGORY_MOVE;
const uint8 CAT_STORE       = Cl::CATEGORY_STORE;
const uint8 CAT_COMPARE     = Cl::CATEGORY_COMPARE;
const uint8 CAT_CONVERT     = Cl::CATEGORY_CONVERT;
const uint8 CAT_FLOAT       = Cl::CATEGORY_FLOAT;
const uint8 CAT_VINT        = Cl::CATEGORY_VECTOR_INTEGER;
const uint8 CAT_VFLOAT      = Cl::CATEGORY_VECTOR_FLOAT;
const uint8 CAT_PERMUTE     = Cl::CATEGORY_VECTOR_PERMUTE;
const uint8 CAT_LOGIC       = Cl::CATEGORY_LOGIC;
const uint8 CAT_SHIFT       = Cl::CATEGORY_SHIFT;
const uint8 CAT_MULTIPLY    = Cl::CATEGORY_MULTIPLY;
const uint8 CAT_CRYPTO      = Cl::CATEGORY_CRYPTO;
const uint8 CAT_ATOMIC      = Cl::CATEGORY_ATOMIC;
const uint8 CAT_SYSTEM      = Cl::CATEGORY_SYSTEM;
const uint8 CAT_CACHE       = Cl::CATEGORY_CACHE;

// The execution units
const uint32 U_ALU          = Cl::UNIT_INTEGER_ALU;
const uint32 U_SHIFT        = Cl::UNIT_SHIFTER;
const uint32 U_MEMORY       = Cl::UNIT_LOAD | Cl::UNIT_STORE;
const uint32 U_LOAD         = Cl::UNIT_LOAD;
const uint32 U_STORE        = Cl::UNIT_STORE;
const uint32 U_VINT         = Cl::UNIT_VECTOR_INTEGER;
const uint32 U_VFLOAT       = Cl::UNIT_VECTOR_FLOAT;
const uint32 U_PERMUTE      = Cl::UNIT_VECTOR_PERMUTE;
const uint32 U_FADD         = Cl::UNIT_FLOAT_ADD;
const uint32 U_FMULTIPLY    = Cl::UNIT_FLOAT_MULTIPLY;
const uint32 U_FDIVIDE      = Cl::UNIT_FLOAT_DIVIDE;
const uint32 U_FCONVERT     = Cl::UNIT_FLOAT_CONVERT;
const uint32 U_CRYPTO       = Cl::UNIT_CRYPTO;
const uint32 U_SYSTEM       = Cl::UNIT_SYSTEM;
const uint32 U_MULTIPLY     = Cl::UNIT_INTEGER_MULTIPLY;

// The properties
const uint32 P_NONE         = Cl::PROPERTY_NONE;
const uint32 P_READ         = Cl::PROPERTY_READS_MEMORY;
const uint32 P_WRITE        = Cl::PROPERTY_WRITES_MEMORY;
const uint32 P_FAULT        = Cl::PROPERTY_MAY_FAULT;
const uint32 P_IMPLICIT     = Cl::PROPERTY_IMPLICIT_OPERANDS;
const uint32 P_SERIALIZING  = Cl::PROPERTY_SERIALIZING;
const uint32 P_HELPER       = Cl::PROPERTY_NEEDS_HELPER;

// The flags
const uint32 FALL           = SemanticFlags::FLAGS_ARITHMETIC;

// The data of an entry for the classification: kind, float format, the bits
// of an element and the lanes (0: fills the vector register, 1: scalar)
#define DATA_INT(bits)      Type::KIND_INTEGER, Type::FLOAT_NONE, bits, 0
#define DATA_SINGLE         Type::KIND_FLOAT, Type::FLOAT_SINGLE, 32, 0
#define DATA_DOUBLE         Type::KIND_FLOAT, Type::FLOAT_DOUBLE, 64, 0
#define DATA_SCALAR_SINGLE  Type::KIND_FLOAT, Type::FLOAT_SINGLE, 32, 1
#define DATA_SCALAR_DOUBLE  Type::KIND_FLOAT, Type::FLOAT_DOUBLE, 64, 1
#define DATA_SINGLE_PAIR    Type::KIND_FLOAT, Type::FLOAT_SINGLE, 32, 2
#define DATA_DOUBLE_PAIR    Type::KIND_FLOAT, Type::FLOAT_DOUBLE, 64, 2
#define DATA_NONE           Type::KIND_VOID, Type::FLOAT_NONE, 0, 0

// An entry with a hand written lifter. The flags are the ones of the integer
// table: read, written, undefined
#define ENTRY(name, lift, category, units, properties, read, written, undefined, data) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, category, units, properties, read, written, undefined }, \
      lift, data }

// The SIMD instructions have no flags, except for the comis family
#define SIMD(name, lift, category, units, properties, data) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, category, units, properties, 0, 0, 0 }, lift, data }

// The packed integer instructions of ELEMENT bits: category, units, the
// operation. The memory operand is read
#define PACKED(name, operation, bits) \
    SIMD(name, (liftPackedBinary<Op::operation, bits>), CAT_VINT, U_VINT | U_LOAD, \
         P_READ, DATA_INT(bits))
#define PACKED_MULTIPLY(name, operation, bits) \
    SIMD(name, (liftPackedBinary<Op::operation, bits>), CAT_MULTIPLY, \
         U_VINT | U_MULTIPLY | U_LOAD, P_READ, DATA_INT(bits))
#define COMPARE(name, condition, bits) \
    SIMD(name, (liftPackedCompare<Op::condition, bits>), CAT_COMPARE, \
         U_VINT | U_LOAD, P_READ, DATA_INT(bits))
#define SHIFT(name, operation, bits) \
    SIMD(name, (liftPackedShift<Op::operation, bits>), CAT_SHIFT, \
         U_SHIFT | U_VINT | U_LOAD, P_READ, DATA_INT(bits))
#define LOGIC(name, operation, bits) \
    SIMD(name, (liftLogic<Op::operation, bits>), CAT_LOGIC, U_VINT | U_LOAD, \
         P_READ, DATA_INT(bits))
#define LOGIC_FLOAT(name, operation, bits, data) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, CAT_LOGIC, U_VFLOAT | U_LOAD, P_READ, 0, 0, 0 }, \
      liftLogic<Op::operation, bits>, data }
#define PACK(name, bits, isUnsigned) \
    SIMD(name, (liftPack<bits, isUnsigned>), CAT_PERMUTE, U_PERMUTE | U_LOAD, \
         P_READ, DATA_INT(bits))
#define UNPACK(name, bits, isHigh) \
    SIMD(name, (liftUnpack<bits, isHigh>), CAT_PERMUTE, U_PERMUTE | U_LOAD, \
         P_READ, DATA_INT(bits))
#define UNPACK_FLOAT(name, bits, isHigh, data) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, CAT_PERMUTE, U_PERMUTE | U_LOAD, P_READ, 0, 0, 0 }, \
      liftUnpack<bits, isHigh>, data }
#define SHUFFLE(name, bits, first) \
    SIMD(name, (liftShuffle<bits, first>), CAT_PERMUTE, U_PERMUTE | U_LOAD, \
         P_READ, DATA_INT(bits))

// The packed / scalar float arithmetic
#define FLOAT_PACKED(name, operation, bits, unit, data) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, CAT_VFLOAT, U_VFLOAT | unit | U_LOAD, P_READ | P_FAULT, \
        0, 0, 0 }, \
      liftFloatPacked<Op::operation, bits>, data }
#define FLOAT_SCALAR(name, operation, bits, unit, data) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, CAT_FLOAT, unit | U_LOAD, P_READ | P_FAULT, 0, 0, 0 }, \
      liftFloatScalar<Op::operation, bits>, data }

} // namespace

const IA32SemanticEntry gIa32SimdSemanticTable[] = {
    //////////////////////////////////////////////////////////////////////////
    // Moves
    //////////////////////////////////////////////////////////////////////////
    // Name        Lifter                     Category  Units                Properties         Data
    SIMD("movaps",  liftMove<false>,           CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE | P_FAULT, DATA_SINGLE),
    SIMD("movapd",  liftMove<false>,           CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE | P_FAULT, DATA_DOUBLE),
    SIMD("movdqa",  liftMove<false>,           CAT_MOVE, U_VINT | U_MEMORY,   P_READ | P_WRITE | P_FAULT, DATA_INT(64)),
    SIMD("movups",  liftMove<true>,            CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE, DATA_SINGLE),
    SIMD("movupd",  liftMove<true>,            CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE, DATA_DOUBLE),
    SIMD("movdqu",  liftMove<true>,            CAT_MOVE, U_VINT | U_MEMORY,   P_READ | P_WRITE, DATA_INT(64)),
    SIMD("movntps", liftMove<false>,           CAT_STORE, U_VFLOAT | U_STORE, P_WRITE | P_FAULT, DATA_SINGLE),
    SIMD("movntpd", liftMove<false>,           CAT_STORE, U_VFLOAT | U_STORE, P_WRITE | P_FAULT, DATA_DOUBLE),
    SIMD("movntdq", liftMove<false>,           CAT_STORE, U_VINT | U_STORE,   P_WRITE | P_FAULT, DATA_INT(64)),
    SIMD("movntq",  liftMove<false>,           CAT_STORE, U_VINT | U_STORE,   P_WRITE, DATA_INT(64)),
    SIMD("movss",   liftMoveScalar<32>,        CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE, DATA_SCALAR_SINGLE),
    SIMD("movsd",   liftMoveScalar<64>,        CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE, DATA_SCALAR_DOUBLE),
    SIMD("movlps",  liftMoveLow,               CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE, DATA_SINGLE_PAIR),
    SIMD("movlpd",  liftMoveLow,               CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE, DATA_SCALAR_DOUBLE),
    SIMD("movhps",  liftMoveHigh,              CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE, DATA_SINGLE_PAIR),
    SIMD("movhpd",  liftMoveHigh,              CAT_MOVE, U_VFLOAT | U_MEMORY, P_READ | P_WRITE, DATA_SCALAR_DOUBLE),
    SIMD("movhlps", liftMoveHighToLow,         CAT_PERMUTE, U_PERMUTE,        P_NONE, DATA_SINGLE_PAIR),
    SIMD("movlhps", liftMoveLowToHigh,         CAT_PERMUTE, U_PERMUTE,        P_NONE, DATA_SINGLE_PAIR),
    SIMD("movd",    liftMoveDword,             CAT_MOVE, U_VINT | U_ALU | U_MEMORY, P_READ | P_WRITE, DATA_INT(32)),
    SIMD("movq",    liftMoveQword,             CAT_MOVE, U_VINT | U_MEMORY,   P_READ | P_WRITE, DATA_INT(64)),
    SIMD("movq2dq", liftMoveQword,             CAT_MOVE, U_VINT,              P_NONE, DATA_INT(64)),
    SIMD("movdq2q", liftMoveQword,             CAT_MOVE, U_VINT,              P_NONE, DATA_INT(64)),
    SIMD("maskmovq",    liftMaskedMove,        CAT_STORE, U_VINT | U_STORE,   P_WRITE | P_IMPLICIT, DATA_INT(8)),
    SIMD("maskmovdqu",  liftMaskedMove,        CAT_STORE, U_VINT | U_STORE,   P_WRITE | P_IMPLICIT, DATA_INT(8)),

    //////////////////////////////////////////////////////////////////////////
    // Packed integers
    //////////////////////////////////////////////////////////////////////////
    PACKED("paddb",   OP_ADD, 8),
    PACKED("paddw",   OP_ADD, 16),
    PACKED("paddd",   OP_ADD, 32),
    PACKED("paddq",   OP_ADD, 64),
    PACKED("psubb",   OP_SUBTRACT, 8),
    PACKED("psubw",   OP_SUBTRACT, 16),
    PACKED("psubd",   OP_SUBTRACT, 32),
    PACKED("psubq",   OP_SUBTRACT, 64),
    PACKED("paddsb",  OP_ADD_SATURATE_SIGNED, 8),
    PACKED("paddsw",  OP_ADD_SATURATE_SIGNED, 16),
    PACKED("paddusb", OP_ADD_SATURATE_UNSIGNED, 8),
    PACKED("paddusw", OP_ADD_SATURATE_UNSIGNED, 16),
    PACKED("psubsb",  OP_SUBTRACT_SATURATE_SIGNED, 8),
    PACKED("psubsw",  OP_SUBTRACT_SATURATE_SIGNED, 16),
    PACKED("psubusb", OP_SUBTRACT_SATURATE_UNSIGNED, 8),
    PACKED("psubusw", OP_SUBTRACT_SATURATE_UNSIGNED, 16),
    PACKED("pavgb",   OP_AVERAGE_UNSIGNED, 8),
    PACKED("pavgw",   OP_AVERAGE_UNSIGNED, 16),
    PACKED("pminub",  OP_MINIMUM_UNSIGNED, 8),
    PACKED("pmaxub",  OP_MAXIMUM_UNSIGNED, 8),
    PACKED("pminsw",  OP_MINIMUM_SIGNED, 16),
    PACKED("pmaxsw",  OP_MAXIMUM_SIGNED, 16),
    PACKED_MULTIPLY("pmullw",  OP_MULTIPLY, 16),
    PACKED_MULTIPLY("pmulhw",  OP_MULTIPLY_HIGH_SIGNED, 16),
    PACKED_MULTIPLY("pmulhuw", OP_MULTIPLY_HIGH_UNSIGNED, 16),
    SIMD("pmuludq", liftMultiplyUnsignedQword, CAT_MULTIPLY, U_VINT | U_MULTIPLY | U_LOAD, P_READ, DATA_INT(64)),
    SIMD("pmaddwd", liftMultiplyAdd,           CAT_MULTIPLY, U_VINT | U_MULTIPLY | U_LOAD, P_READ, DATA_INT(16)),
    SIMD("psadbw",  liftSumAbsoluteDifferences, CAT_VINT,    U_VINT | U_LOAD,              P_READ, DATA_INT(8)),
    COMPARE("pcmpeqb", COND_EQUAL, 8),
    COMPARE("pcmpeqw", COND_EQUAL, 16),
    COMPARE("pcmpeqd", COND_EQUAL, 32),
    COMPARE("pcmpgtb", COND_SIGNED_GREATER, 8),
    COMPARE("pcmpgtw", COND_SIGNED_GREATER, 16),
    COMPARE("pcmpgtd", COND_SIGNED_GREATER, 32),
    LOGIC("pand",  OP_AND, 64),
    LOGIC("pandn", OP_AND_NOT, 64),
    LOGIC("por",   OP_OR, 64),
    LOGIC("pxor",  OP_XOR, 64),
    SHIFT("psllw", OP_SHIFT_LEFT, 16),
    SHIFT("pslld", OP_SHIFT_LEFT, 32),
    SHIFT("psllq", OP_SHIFT_LEFT, 64),
    SHIFT("psrlw", OP_SHIFT_RIGHT_LOGICAL, 16),
    SHIFT("psrld", OP_SHIFT_RIGHT_LOGICAL, 32),
    SHIFT("psrlq", OP_SHIFT_RIGHT_LOGICAL, 64),
    SHIFT("psraw", OP_SHIFT_RIGHT_ARITHMETIC, 16),
    SHIFT("psrad", OP_SHIFT_RIGHT_ARITHMETIC, 32),
    SIMD("pslldq", liftByteShift<true>,  CAT_SHIFT, U_SHIFT | U_VINT, P_NONE, DATA_INT(8)),
    SIMD("psrldq", liftByteShift<false>, CAT_SHIFT, U_SHIFT | U_VINT, P_NONE, DATA_INT(8)),
    SIMD("pextrw", liftExtractWord,      CAT_PERMUTE, U_PERMUTE | U_ALU, P_NONE, DATA_INT(16)),
    SIMD("pinsrw", liftInsertWord,       CAT_PERMUTE, U_PERMUTE | U_ALU | U_LOAD, P_READ, DATA_INT(16)),
    SIMD("pmovmskb", liftMoveMask<8>,    CAT_VINT, U_VINT | U_ALU, P_NONE, DATA_INT(8)),

    //////////////////////////////////////////////////////////////////////////
    // Pack, unpack and shuffle
    //////////////////////////////////////////////////////////////////////////
    PACK("packsswb", 16, false),
    PACK("packssdw", 32, false),
    PACK("packuswb", 16, true),
    UNPACK("punpcklbw",  8, false),
    UNPACK("punpcklwd", 16, false),
    UNPACK("punpckldq", 32, false),
    UNPACK("punpcklqdq", 64, false),
    UNPACK("punpckhbw",  8, true),
    UNPACK("punpckhwd", 16, true),
    UNPACK("punpckhdq", 32, true),
    UNPACK("punpckhqdq", 64, true),
    UNPACK_FLOAT("unpcklps", 32, false, DATA_SINGLE),
    UNPACK_FLOAT("unpckhps", 32, true,  DATA_SINGLE),
    UNPACK_FLOAT("unpcklpd", 64, false, DATA_DOUBLE),
    UNPACK_FLOAT("unpckhpd", 64, true,  DATA_DOUBLE),
    SHUFFLE("pshufd",  32, 0),
    SHUFFLE("pshufw",  16, 0),
    SHUFFLE("pshuflw", 16, 0),
    SHUFFLE("pshufhw", 16, 4),
    SIMD("shufps", liftShuffleFloat<32>, CAT_PERMUTE, U_PERMUTE | U_LOAD, P_READ, DATA_SINGLE),
    SIMD("shufpd", liftShuffleFloat<64>, CAT_PERMUTE, U_PERMUTE | U_LOAD, P_READ, DATA_DOUBLE),

    //////////////////////////////////////////////////////////////////////////
    // Floating point
    //////////////////////////////////////////////////////////////////////////
    FLOAT_PACKED("addps", OP_FLOAT_ADD,      32, U_FADD,      DATA_SINGLE),
    FLOAT_PACKED("addpd", OP_FLOAT_ADD,      64, U_FADD,      DATA_DOUBLE),
    FLOAT_PACKED("subps", OP_FLOAT_SUBTRACT, 32, U_FADD,      DATA_SINGLE),
    FLOAT_PACKED("subpd", OP_FLOAT_SUBTRACT, 64, U_FADD,      DATA_DOUBLE),
    FLOAT_PACKED("mulps", OP_FLOAT_MULTIPLY, 32, U_FMULTIPLY, DATA_SINGLE),
    FLOAT_PACKED("mulpd", OP_FLOAT_MULTIPLY, 64, U_FMULTIPLY, DATA_DOUBLE),
    FLOAT_PACKED("divps", OP_FLOAT_DIVIDE,   32, U_FDIVIDE,   DATA_SINGLE),
    FLOAT_PACKED("divpd", OP_FLOAT_DIVIDE,   64, U_FDIVIDE,   DATA_DOUBLE),
    FLOAT_PACKED("minps", OP_FLOAT_MINIMUM,  32, U_FADD,      DATA_SINGLE),
    FLOAT_PACKED("minpd", OP_FLOAT_MINIMUM,  64, U_FADD,      DATA_DOUBLE),
    FLOAT_PACKED("maxps", OP_FLOAT_MAXIMUM,  32, U_FADD,      DATA_SINGLE),
    FLOAT_PACKED("maxpd", OP_FLOAT_MAXIMUM,  64, U_FADD,      DATA_DOUBLE),
    FLOAT_SCALAR("addss", OP_FLOAT_ADD,      32, U_FADD,      DATA_SCALAR_SINGLE),
    FLOAT_SCALAR("addsd", OP_FLOAT_ADD,      64, U_FADD,      DATA_SCALAR_DOUBLE),
    FLOAT_SCALAR("subss", OP_FLOAT_SUBTRACT, 32, U_FADD,      DATA_SCALAR_SINGLE),
    FLOAT_SCALAR("subsd", OP_FLOAT_SUBTRACT, 64, U_FADD,      DATA_SCALAR_DOUBLE),
    FLOAT_SCALAR("mulss", OP_FLOAT_MULTIPLY, 32, U_FMULTIPLY, DATA_SCALAR_SINGLE),
    FLOAT_SCALAR("mulsd", OP_FLOAT_MULTIPLY, 64, U_FMULTIPLY, DATA_SCALAR_DOUBLE),
    FLOAT_SCALAR("divss", OP_FLOAT_DIVIDE,   32, U_FDIVIDE,   DATA_SCALAR_SINGLE),
    FLOAT_SCALAR("divsd", OP_FLOAT_DIVIDE,   64, U_FDIVIDE,   DATA_SCALAR_DOUBLE),
    FLOAT_SCALAR("minss", OP_FLOAT_MINIMUM,  32, U_FADD,      DATA_SCALAR_SINGLE),
    FLOAT_SCALAR("minsd", OP_FLOAT_MINIMUM,  64, U_FADD,      DATA_SCALAR_DOUBLE),
    FLOAT_SCALAR("maxss", OP_FLOAT_MAXIMUM,  32, U_FADD,      DATA_SCALAR_SINGLE),
    FLOAT_SCALAR("maxsd", OP_FLOAT_MAXIMUM,  64, U_FADD,      DATA_SCALAR_DOUBLE),
    SIMD("sqrtps", liftSquareRootPacked<32>, CAT_VFLOAT, U_VFLOAT | U_FDIVIDE | U_LOAD, P_READ | P_FAULT, DATA_SINGLE),
    SIMD("sqrtpd", liftSquareRootPacked<64>, CAT_VFLOAT, U_VFLOAT | U_FDIVIDE | U_LOAD, P_READ | P_FAULT, DATA_DOUBLE),
    SIMD("sqrtss", liftSquareRootScalar<32>, CAT_FLOAT,  U_FDIVIDE | U_LOAD,            P_READ | P_FAULT, DATA_SCALAR_SINGLE),
    SIMD("sqrtsd", liftSquareRootScalar<64>, CAT_FLOAT,  U_FDIVIDE | U_LOAD,            P_READ | P_FAULT, DATA_SCALAR_DOUBLE),
    SIMD("rcpps",   (liftEstimate<IA32_HELPER_RECIPROCAL_ESTIMATE, 32, false>),      CAT_VFLOAT, U_VFLOAT | U_LOAD, P_READ | P_HELPER, DATA_SINGLE),
    SIMD("rcpss",   (liftEstimate<IA32_HELPER_RECIPROCAL_ESTIMATE, 32, true>),       CAT_FLOAT,  U_FDIVIDE | U_LOAD, P_READ | P_HELPER, DATA_SCALAR_SINGLE),
    SIMD("rsqrtps", (liftEstimate<IA32_HELPER_RECIPROCAL_SQRT_ESTIMATE, 32, false>), CAT_VFLOAT, U_VFLOAT | U_LOAD, P_READ | P_HELPER, DATA_SINGLE),
    SIMD("rsqrtss", (liftEstimate<IA32_HELPER_RECIPROCAL_SQRT_ESTIMATE, 32, true>),  CAT_FLOAT,  U_FDIVIDE | U_LOAD, P_READ | P_HELPER, DATA_SCALAR_SINGLE),
    LOGIC_FLOAT("andps",  OP_AND,     32, DATA_SINGLE),
    LOGIC_FLOAT("andpd",  OP_AND,     64, DATA_DOUBLE),
    LOGIC_FLOAT("andnps", OP_AND_NOT, 32, DATA_SINGLE),
    LOGIC_FLOAT("andnpd", OP_AND_NOT, 64, DATA_DOUBLE),
    LOGIC_FLOAT("orps",   OP_OR,      32, DATA_SINGLE),
    LOGIC_FLOAT("orpd",   OP_OR,      64, DATA_DOUBLE),
    LOGIC_FLOAT("xorps",  OP_XOR,     32, DATA_SINGLE),
    LOGIC_FLOAT("xorpd",  OP_XOR,     64, DATA_DOUBLE),
    SIMD("movmskps", liftMoveMask<32>, CAT_VFLOAT, U_VFLOAT | U_ALU, P_NONE, DATA_SINGLE),
    SIMD("movmskpd", liftMoveMask<64>, CAT_VFLOAT, U_VFLOAT | U_ALU, P_NONE, DATA_DOUBLE),
    SIMD("cmpps", (liftFloatCompare<32, false>), CAT_VFLOAT, U_VFLOAT | U_FADD | U_LOAD, P_READ | P_FAULT, DATA_SINGLE),
    SIMD("cmppd", (liftFloatCompare<64, false>), CAT_VFLOAT, U_VFLOAT | U_FADD | U_LOAD, P_READ | P_FAULT, DATA_DOUBLE),
    SIMD("cmpss", (liftFloatCompare<32, true>),  CAT_FLOAT,  U_FADD | U_LOAD,            P_READ | P_FAULT, DATA_SCALAR_SINGLE),
    SIMD("cmpsd", (liftFloatCompare<64, true>),  CAT_FLOAT,  U_FADD | U_LOAD,            P_READ | P_FAULT, DATA_SCALAR_DOUBLE),
    ENTRY("comiss",  (liftFloatCompareFlags<32, true>),  CAT_COMPARE, U_FADD | U_LOAD, P_READ | P_FAULT, 0, FALL, 0, DATA_SCALAR_SINGLE),
    ENTRY("comisd",  (liftFloatCompareFlags<64, true>),  CAT_COMPARE, U_FADD | U_LOAD, P_READ | P_FAULT, 0, FALL, 0, DATA_SCALAR_DOUBLE),
    ENTRY("ucomiss", (liftFloatCompareFlags<32, false>), CAT_COMPARE, U_FADD | U_LOAD, P_READ | P_FAULT, 0, FALL, 0, DATA_SCALAR_SINGLE),
    ENTRY("ucomisd", (liftFloatCompareFlags<64, false>), CAT_COMPARE, U_FADD | U_LOAD, P_READ | P_FAULT, 0, FALL, 0, DATA_SCALAR_DOUBLE),

    //////////////////////////////////////////////////////////////////////////
    // Conversions
    //////////////////////////////////////////////////////////////////////////
    SIMD("cvtdq2ps",  liftConvertDwordsToSingles,   CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_INT(32)),
    SIMD("cvtdq2pd",  liftConvertDwordsToDoubles,   CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_INT(32)),
    SIMD("cvtpi2pd",  liftConvertDwordsToDoubles,   CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_INT(32)),
    SIMD("cvtpi2ps",  liftConvertPackedIntegersToSingles, CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_INT(32)),
    SIMD("cvtps2pd",  liftConvertSinglesToDoubles,  CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_SINGLE_PAIR),
    SIMD("cvtpd2ps",  liftConvertDoublesToSingles,  CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_DOUBLE),
    SIMD("cvtps2dq",  (liftConvertFloatsToDwords<32, 4, false>), CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_SINGLE),
    SIMD("cvttps2dq", (liftConvertFloatsToDwords<32, 4, true>),  CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_SINGLE),
    SIMD("cvtpd2dq",  (liftConvertFloatsToDwords<64, 2, false>), CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_DOUBLE),
    SIMD("cvttpd2dq", (liftConvertFloatsToDwords<64, 2, true>),  CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_DOUBLE),
    SIMD("cvtps2pi",  (liftConvertFloatsToDwords<32, 2, false>), CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_SINGLE_PAIR),
    SIMD("cvttps2pi", (liftConvertFloatsToDwords<32, 2, true>),  CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_SINGLE_PAIR),
    SIMD("cvtpd2pi",  (liftConvertFloatsToDwords<64, 2, false>), CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_DOUBLE),
    SIMD("cvttpd2pi", (liftConvertFloatsToDwords<64, 2, true>),  CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_DOUBLE),
    SIMD("cvtss2sd",  (liftConvertScalarFloat<32, 64>), CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_SCALAR_SINGLE),
    SIMD("cvtsd2ss",  (liftConvertScalarFloat<64, 32>), CAT_CONVERT, U_FCONVERT | U_LOAD, P_READ | P_FAULT, DATA_SCALAR_DOUBLE),
    SIMD("cvtsi2ss",  liftConvertIntegerToScalar<32>,   CAT_CONVERT, U_FCONVERT | U_ALU | U_LOAD, P_READ | P_FAULT, DATA_SCALAR_SINGLE),
    SIMD("cvtsi2sd",  liftConvertIntegerToScalar<64>,   CAT_CONVERT, U_FCONVERT | U_ALU | U_LOAD, P_READ | P_FAULT, DATA_SCALAR_DOUBLE),
    SIMD("cvtss2si",  (liftConvertScalarToInteger<32, false>), CAT_CONVERT, U_FCONVERT | U_ALU | U_LOAD, P_READ | P_FAULT, DATA_SCALAR_SINGLE),
    SIMD("cvttss2si", (liftConvertScalarToInteger<32, true>),  CAT_CONVERT, U_FCONVERT | U_ALU | U_LOAD, P_READ | P_FAULT, DATA_SCALAR_SINGLE),
    SIMD("cvtsd2si",  (liftConvertScalarToInteger<64, false>), CAT_CONVERT, U_FCONVERT | U_ALU | U_LOAD, P_READ | P_FAULT, DATA_SCALAR_DOUBLE),
    SIMD("cvttsd2si", (liftConvertScalarToInteger<64, true>),  CAT_CONVERT, U_FCONVERT | U_ALU | U_LOAD, P_READ | P_FAULT, DATA_SCALAR_DOUBLE),

    //////////////////////////////////////////////////////////////////////////
    // Carry-less multiplication
    //////////////////////////////////////////////////////////////////////////
    SIMD("pclmulqdq", liftCarrylessMultiply, CAT_CRYPTO, U_CRYPTO | U_LOAD, P_READ | P_FAULT, DATA_INT(64)),
    SIMD("aesenc",      (liftAesRound<false, false>), CAT_CRYPTO, U_CRYPTO | U_LOAD, P_READ | P_FAULT, DATA_INT(8)),
    SIMD("aesenclast",  (liftAesRound<false, true>),  CAT_CRYPTO, U_CRYPTO | U_LOAD, P_READ | P_FAULT, DATA_INT(8)),
    SIMD("aesdec",      (liftAesRound<true, false>),  CAT_CRYPTO, U_CRYPTO | U_LOAD, P_READ | P_FAULT, DATA_INT(8)),
    SIMD("aesdeclast",  (liftAesRound<true, true>),   CAT_CRYPTO, U_CRYPTO | U_LOAD, P_READ | P_FAULT, DATA_INT(8)),
    SIMD("aesimc",      liftAesInverseMixColumns,     CAT_CRYPTO, U_CRYPTO | U_LOAD, P_READ | P_FAULT, DATA_INT(8)),
    SIMD("aeskeygenassist", liftAesKeyGenerationAssist, CAT_CRYPTO, U_CRYPTO | U_LOAD, P_READ | P_FAULT, DATA_INT(32)),

    //////////////////////////////////////////////////////////////////////////
    // State, fences and the cache
    //////////////////////////////////////////////////////////////////////////
    SIMD("ldmxcsr", liftLoadMxcsr,  CAT_SYSTEM, U_SYSTEM | U_LOAD,  P_READ | P_FAULT, DATA_NONE),
    SIMD("stmxcsr", liftStoreMxcsr, CAT_SYSTEM, U_SYSTEM | U_STORE, P_WRITE, DATA_NONE),
    SIMD("emms",    liftEmms,       CAT_SYSTEM, U_SYSTEM,           P_NONE, DATA_NONE),
    SIMD("lfence",  (liftFence<Op::FENCE_BEFORE_LOADS | Op::FENCE_AFTER_LOADS, Op::ORDER_ACQUIRE>),   CAT_ATOMIC, U_SYSTEM, P_SERIALIZING, DATA_NONE),
    SIMD("sfence",  (liftFence<Op::FENCE_BEFORE_STORES | Op::FENCE_AFTER_STORES, Op::ORDER_RELEASE>), CAT_ATOMIC, U_SYSTEM, P_SERIALIZING, DATA_NONE),
    SIMD("mfence",  (liftFence<Op::FENCE_ALL, Op::ORDER_SEQUENTIAL>),                               CAT_ATOMIC, U_SYSTEM, P_SERIALIZING, DATA_NONE),
    SIMD("prefetchnta", liftPrefetch, CAT_CACHE, U_LOAD, P_READ, DATA_NONE),
    SIMD("prefetcht0",  liftPrefetch, CAT_CACHE, U_LOAD, P_READ, DATA_NONE),
    SIMD("prefetcht1",  liftPrefetch, CAT_CACHE, U_LOAD, P_READ, DATA_NONE),
    SIMD("prefetcht2",  liftPrefetch, CAT_CACHE, U_LOAD, P_READ, DATA_NONE),
    SIMD("prefetchw",   liftPrefetch, CAT_CACHE, U_LOAD, P_READ, DATA_NONE),
    SIMD("clflush",     liftFlushLine, CAT_CACHE, U_STORE, P_WRITE, DATA_NONE),

    // End marker
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0 }
};

#undef DATA_INT
#undef DATA_SINGLE
#undef DATA_DOUBLE
#undef DATA_SCALAR_SINGLE
#undef DATA_SCALAR_DOUBLE
#undef DATA_SINGLE_PAIR
#undef DATA_DOUBLE_PAIR
#undef DATA_NONE
#undef ENTRY
#undef SIMD
#undef PACKED
#undef PACKED_MULTIPLY
#undef COMPARE
#undef SHIFT
#undef LOGIC
#undef LOGIC_FLOAT
#undef PACK
#undef UNPACK
#undef UNPACK_FLOAT
#undef SHUFFLE
#undef FLOAT_PACKED
#undef FLOAT_SCALAR

} // namespace ia32dis
