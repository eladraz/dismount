/*
 * Arm64SemanticLift.cpp
 *
 * Implementation file
 *
 * The A64 base lifters: data processing, branches, exceptions and system
 * instructions. See Arm64SemanticInternal.h. The semantics are the ones of
 * the pseudocode of the Arm ARM (AddWithCarry, DecodeBitMasks, ...). The flag
 * registers hold N Z C V as the processor does, so the
 * subtraction flags are the ones of the addition of the inverted operand
 * (AddWithCarry), see ArmLiftContext::testCondition().
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64SemanticInternal.h"

using namespace armdis;
using namespace armdis::a64;

typedef SemanticOperation Op;

/////////////////////////////////////////////////////////////////////////////
// The helpers
/////////////////////////////////////////////////////////////////////////////

namespace armdis {
namespace a64 {

const char* getMnemonic(const ArmLiftContext& context)
{
    return context.getInstruction().getEntry()->m_mnemonic;
}

bool isMnemonic(const ArmLiftContext& context, const char* name)
{
    return strcmp(getMnemonic(context), name) == 0;
}

SemanticOperand readGpr(ArmLiftContext& context, uint number, bool is64,
                        bool isSp)
{
    if ((number == 31) && !isSp)
        return context.getImmediate(0, typeOf(is64));
    return context.getRegister(getArm64GeneralRegister(number, is64, isSp));
}

void writeGpr(ArmLiftContext& context, uint number, bool is64, bool isSp,
              const SemanticOperand& value)
{
    if ((number == 31) && !isSp)
        return;
    context.move(context.getRegister(getArm64GeneralRegister(number, is64, isSp)),
                 value);
}

SemanticOperand compare(ArmLiftContext& context,
                        SemanticOperation::Condition condition,
                        const SemanticType& type,
                        const SemanticOperand& a, const SemanticOperand& b)
{
    SemanticOperand result = context.newTemporary(SemanticType::integer(1));
    SemanticInstruction instruction(Op::OP_COMPARE, type, result, a, b);
    instruction.setCondition(condition);
    context.emit(instruction);
    return result;
}

SemanticOperand booleanConstant(ArmLiftContext& context, bool value)
{
    return context.getImmediate(value ? 1 : 0, SemanticType::integer(1));
}

SemanticOperand emitImmediate(ArmLiftContext& context,
                              SemanticOperation::Operation operation,
                              const SemanticType& type,
                              const SemanticOperand& a, int64 value)
{
    return context.emit(operation, type, a, context.getImmediate(value, type));
}

SemanticOperand extend(ArmLiftContext& context, const SemanticType& type,
                       const SemanticOperand& value, bool isSigned)
{
    if (value.getType().getTotalBits() == type.getTotalBits())
        return value;
    if (value.getType().getTotalBits() > type.getTotalBits())
        return context.emit(Op::OP_TRUNCATE, type, value);
    return context.emit(isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                        type, value);
}

SemanticOperand invert(ArmLiftContext& context, const SemanticOperand& value)
{
    if (value.getKind() == SemanticOperand::KIND_IMMEDIATE)
        return context.getImmediate(~value.getValue(), value.getType());
    return context.emit(Op::OP_NOT, value.getType(), value);
}

SemanticOperand packNzcv(ArmLiftContext& context)
{
    const SemanticType type = SemanticType::integer(64);
    static const uint registers[4] = { ARM64_REG_N, ARM64_REG_Z, ARM64_REG_C,
                                       ARM64_REG_V };
    SemanticOperand result;
    for (uint i = 0; i < 4; i++)
    {
        SemanticOperand bit = context.emit(Op::OP_ZERO_EXTEND, type,
                                           context.getRegister(registers[i]));
        bit = emitImmediate(context, Op::OP_SHIFT_LEFT, type, bit, 31 - i);
        result = result.isValid() ? context.emit(Op::OP_OR, type, result, bit)
                                  : bit;
    }
    return result;
}

void unpackNzcv(ArmLiftContext& context, const SemanticOperand& value)
{
    static const uint registers[4] = { ARM64_REG_N, ARM64_REG_Z, ARM64_REG_C,
                                       ARM64_REG_V };
    const SemanticType type = value.getType();
    for (uint i = 0; i < 4; i++)
    {
        SemanticOperand shifted = emitImmediate(context,
                        Op::OP_SHIFT_RIGHT_LOGICAL, type, value, 31 - i);
        context.move(context.getRegister(registers[i]),
                     context.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                                  shifted));
    }
}

void clearExclusiveMonitor(ArmLiftContext& context)
{
    context.move(context.getRegister(ARM64_REG_EXCLUSIVE_SIZE),
                 context.getImmediate(0, SemanticType::integer(64)));
}

void undefined(ArmLiftContext& context)
{
    context.emit(SemanticInstruction(Op::OP_UNDEFINED, SemanticType(),
                                     SemanticOperand()));
}

} // namespace a64
} // namespace armdis

/////////////////////////////////////////////////////////////////////////////
// Data processing
/////////////////////////////////////////////////////////////////////////////

namespace {

// Shift types of the shifted register forms
enum { SHIFT_LSL, SHIFT_LSR, SHIFT_ASR, SHIFT_ROR };

// Return 'value' shifted by the constant 'amount' (the shifted register)
SemanticOperand shiftValue(ArmLiftContext& c, const SemanticType& type,
                           const SemanticOperand& value, uint shiftType,
                           uint amount)
{
    if (amount == 0)
        return value;
    static const Op::Operation operations[4] = {
        Op::OP_SHIFT_LEFT, Op::OP_SHIFT_RIGHT_LOGICAL,
        Op::OP_SHIFT_RIGHT_ARITHMETIC, Op::OP_ROTATE_RIGHT };
    return emitImmediate(c, operations[shiftType & 3], type, value, amount);
}

// Return the extended register of the add / sub extended register form
// (ExtendReg): the low bits by the extend type, extended, then shifted left
SemanticOperand extendRegister(ArmLiftContext& c, bool is64, uint number,
                               uint option, uint amount)
{
    const SemanticType type = typeOf(is64);
    SemanticOperand value = readGpr(c, number, is64, false);
    uint fromBits = 8u << (option & 3);
    if (fromBits > type.getTotalBits())
        fromBits = type.getTotalBits();
    if (fromBits < type.getTotalBits())
    {
        value = c.emit(Op::OP_TRUNCATE, SemanticType::integer(fromBits), value);
        value = extend(c, type, value, option >= 4);
    }
    if (amount != 0)
        value = emitImmediate(c, Op::OP_SHIFT_LEFT, type, value, amount);
    return value;
}

/*
 * The addition and the subtraction, with the carry in (adc / sbc) and the
 * flags (AddWithCarry of the Arm ARM). A subtraction a - b is a + ~b + 1: the
 * flags are those of the addition of the inverted operand, which is the ARM
 * carry (the inverted borrow).
 *
 * carry - The 1 bit carry in for adc / sbc, an invalid operand for the plain
 *         forms
 */
SemanticOperand addSub(ArmLiftContext& c, bool is64, bool isSub,
                       bool isSetFlags, const SemanticOperand& a,
                       const SemanticOperand& b, const SemanticOperand& carry)
{
    const SemanticType type = typeOf(is64);
    SemanticOperand result;
    SemanticOperand flagsB = b;
    if (isSub)
        flagsB = invert(c, b);
    if (!carry.isValid())
        result = c.emit(isSub ? Op::OP_SUBTRACT : Op::OP_ADD, type, a, b);
    else
        result = c.emit(Op::OP_ADD_CARRY, type, a, flagsB, carry);
    if (isSetFlags)
        c.setFlags(Op::FLAGS_ADD, FLAGS_NZCV, type, a, flagsB, result);
    return result;
}

// Add / subtract (immediate, shifted register, extended register) and
// add / subtract with carry
void liftAddSub(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const bool isSub = bits(e, 30, 30) != 0;
    const bool isSetFlags = bits(e, 29, 29) != 0;
    const uint rd = bits(e, 4, 0);
    const uint rn = bits(e, 9, 5);
    const uint rm = bits(e, 20, 16);
    const SemanticType type = typeOf(is64);

    SemanticOperand a, b, carry;
    bool isDestinationSp = false;
    if (bits(e, 28, 24) == 0x11)
    {
        // Immediate: imm12, shifted by 12 when sh is set
        int64 value = bits(e, 21, 10);
        if (bits(e, 22, 22) != 0)
            value <<= 12;
        a = readGpr(c, rn, is64, true);
        b = c.getImmediate(value, type);
        isDestinationSp = !isSetFlags;
    } else if (bits(e, 28, 21) == 0xD0)
    {
        // With carry: C is the carry in (for sbc the inverted borrow)
        a = readGpr(c, rn, is64, false);
        b = readGpr(c, rm, is64, false);
        carry = c.getRegister(ARM64_REG_C);
    } else if (bits(e, 21, 21) == 0)
    {
        // Shifted register
        a = readGpr(c, rn, is64, false);
        b = shiftValue(c, type, readGpr(c, rm, is64, false), bits(e, 23, 22),
                       bits(e, 15, 10));
    } else
    {
        // Extended register
        a = readGpr(c, rn, is64, true);
        b = extendRegister(c, is64, rm, bits(e, 15, 13), bits(e, 12, 10));
        isDestinationSp = !isSetFlags;
    }
    SemanticOperand result = addSub(c, is64, isSub, isSetFlags, a, b, carry);
    writeGpr(c, rd, is64, isDestinationSp, result);
}

// and, orr, eor, ands, bic, orn, eon, bics (immediate and shifted register)
void liftLogical(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const uint opc = bits(e, 30, 29);
    const uint rd = bits(e, 4, 0);
    const uint rn = bits(e, 9, 5);
    const SemanticType type = typeOf(is64);

    SemanticOperand b;
    bool isImmediate = bits(e, 28, 23) == 0x24;
    if (isImmediate)
    {
        uint64 value;
        if (!decodeLogicalImmediateValue(bits(e, 22, 22), bits(e, 21, 16),
                                         bits(e, 15, 10), is64 ? 64 : 32, value))
        {
            undefined(c);
            return;
        }
        b = c.getImmediate((int64)value, type);
    } else
    {
        b = shiftValue(c, type, readGpr(c, bits(e, 20, 16), is64, false),
                       bits(e, 23, 22), bits(e, 15, 10));
        if (bits(e, 21, 21) != 0)
            b = invert(c, b);
    }
    const SemanticOperand a = readGpr(c, rn, is64, false);
    static const Op::Operation operations[4] = {
        Op::OP_AND, Op::OP_OR, Op::OP_XOR, Op::OP_AND };
    SemanticOperand result = c.emit(operations[opc], type, a, b);
    if (opc == 3)
        c.setFlags(Op::FLAGS_LOGIC, FLAGS_NZCV, type, a, b, result);
    // The immediate forms write the stack pointer, but ands
    writeGpr(c, rd, is64, isImmediate && (opc != 3), result);
}

// movn, movz, movk
void liftMoveWide(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const uint opc = bits(e, 30, 29);
    const uint rd = bits(e, 4, 0);
    const uint shift = bits(e, 22, 21) * 16;
    const SemanticType type = typeOf(is64);
    const uint64 immediate = (uint64)bits(e, 20, 5) << shift;
    if (opc == 0)
    {
        writeGpr(c, rd, is64, false,
                 c.getImmediate((int64)(~immediate & maskOf(is64 ? 64 : 32)),
                                type));
    } else if (opc == 2)
    {
        writeGpr(c, rd, is64, false, c.getImmediate((int64)immediate, type));
    } else
    {
        // movk keeps the other bits
        SemanticOperand old = readGpr(c, rd, is64, false);
        SemanticOperand cleared = c.emit(Op::OP_AND, type, old,
                    c.getImmediate((int64)~((uint64)0xFFFF << shift), type));
        writeGpr(c, rd, is64, false,
                 c.emit(Op::OP_OR, type, cleared,
                        c.getImmediate((int64)immediate, type)));
    }
}

// adr, adrp: the decoder gives the absolute address
void liftPcRelative(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    writeGpr(c, bits(e, 4, 0), true, false,
             c.getImmediate(c.getOperand(1).m_immediate,
                            SemanticType::integer(64)));
}

// DecodeBitMasks of the Arm ARM for the bitfield instructions (immediate
// is false). Return false for the reserved encodings
bool decodeBitMasks(uint n, uint imms, uint immr, uint dataSize,
                    uint64& wmask, uint64& tmask)
{
    const uint value = (n << 6) | ((~imms) & 0x3F);
    uint length = 0;
    for (uint i = 0; i < 7; i++)
        if ((value >> i) & 1)
            length = i;
    if (value == 0 || length < 1)
        return false;
    const uint levels = (1u << length) - 1;
    const uint s = imms & levels;
    const uint r = immr & levels;
    const uint esize = 1u << length;
    if (esize > dataSize)
        return false;
    const uint d = (s - r) & (esize - 1);
    uint64 welem = maskOf(s + 1);
    const uint64 telem = maskOf(d + 1);
    // ROR(welem, r) in esize bits
    if (r != 0)
        welem = ((welem >> r) | (welem << (esize - r))) & maskOf(esize);
    wmask = 0;
    tmask = 0;
    for (uint i = 0; i < dataSize; i += esize)
    {
        wmask |= welem << i;
        tmask |= telem << i;
    }
    return true;
}

// sbfm, ubfm, bfm and all their aliases
void liftBitfield(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const uint opc = bits(e, 30, 29);
    const uint immr = bits(e, 21, 16);
    const uint imms = bits(e, 15, 10);
    const uint rd = bits(e, 4, 0);
    const uint rn = bits(e, 9, 5);
    const uint dataSize = is64 ? 64 : 32;
    const SemanticType type = typeOf(is64);
    uint64 wmask, tmask;
    if ((opc == 3) || (bits(e, 22, 22) != (is64 ? 1u : 0u)) ||
        !decodeBitMasks(bits(e, 22, 22), imms, immr, dataSize, wmask, tmask))
    {
        undefined(c);
        return;
    }
    const uint r = immr & (dataSize - 1);
    const uint s = imms & (dataSize - 1);
    const SemanticOperand source = readGpr(c, rn, is64, false);

    // ROR(src, R)
    SemanticOperand rotated = source;
    if (r != 0)
        rotated = emitImmediate(c, Op::OP_ROTATE_RIGHT, type, source, r);

    SemanticOperand result;
    if (opc == 2)
    {
        // ubfm: ROR(src, R) & wmask & tmask
        result = emitImmediate(c, Op::OP_AND, type, rotated,
                               (int64)(wmask & tmask));
    } else if (opc == 0)
    {
        // sbfm: (top & ~tmask) | (bot & tmask), top = src<S> replicated
        SemanticOperand bot = emitImmediate(c, Op::OP_AND, type, rotated,
                                            (int64)wmask);
        bot = emitImmediate(c, Op::OP_AND, type, bot, (int64)tmask);
        SemanticOperand top = emitImmediate(c, Op::OP_SHIFT_LEFT, type, source,
                                            dataSize - 1 - s);
        top = emitImmediate(c, Op::OP_SHIFT_RIGHT_ARITHMETIC, type, top,
                            dataSize - 1);
        top = emitImmediate(c, Op::OP_AND, type, top,
                            (int64)(~tmask & maskOf(dataSize)));
        result = c.emit(Op::OP_OR, type, top, bot);
    } else
    {
        // bfm: bot = (dst & ~wmask) | (ROR(src, R) & wmask)
        //      dst = (dst & ~tmask) | (bot & tmask)
        SemanticOperand dst = readGpr(c, rd, is64, false);
        SemanticOperand bot = c.emit(Op::OP_OR, type,
            emitImmediate(c, Op::OP_AND, type, dst,
                          (int64)(~wmask & maskOf(dataSize))),
            emitImmediate(c, Op::OP_AND, type, rotated, (int64)wmask));
        result = c.emit(Op::OP_OR, type,
            emitImmediate(c, Op::OP_AND, type, dst,
                          (int64)(~tmask & maskOf(dataSize))),
            emitImmediate(c, Op::OP_AND, type, bot, (int64)tmask));
    }
    writeGpr(c, rd, is64, false, result);
}

// extr and its alias ror (immediate)
void liftExtract(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const uint lsb = bits(e, 15, 10);
    const SemanticType type = typeOf(is64);
    if (bits(e, 22, 22) != (is64 ? 1u : 0u) || (!is64 && lsb >= 32))
    {
        undefined(c);
        return;
    }
    const SemanticOperand high = readGpr(c, bits(e, 9, 5), is64, false);
    const SemanticOperand low = readGpr(c, bits(e, 20, 16), is64, false);
    SemanticOperand result = low;
    if (lsb != 0)
        result = c.emit(Op::OP_OR, type,
            emitImmediate(c, Op::OP_SHIFT_RIGHT_LOGICAL, type, low, lsb),
            emitImmediate(c, Op::OP_SHIFT_LEFT, type, high,
                          (is64 ? 64 : 32) - lsb));
    writeGpr(c, bits(e, 4, 0), is64, false, result);
}

// The data processing (immediate) and (register) of CSSC: smax, smin, umax,
// umin
void liftMinMax(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const SemanticType type = typeOf(is64);
    const char* name = getMnemonic(c);
    const bool isSigned = (name[0] == 's');
    const bool isMax = (name[1] == 'm') && (name[2] == 'a');
    SemanticOperand a = readGpr(c, bits(e, 9, 5), is64, false);
    SemanticOperand b;
    if (bits(e, 28, 23) == 0x23)
    {
        // The immediate: signed for smax / smin, unsigned for umax / umin
        int64 value = bits(e, 17, 10);
        if (isSigned && (value >= 128))
            value -= 256;
        b = c.getImmediate(value, type);
    } else
    {
        b = readGpr(c, bits(e, 20, 16), is64, false);
    }
    Op::Operation operation = isSigned ?
        (isMax ? Op::OP_MAXIMUM_SIGNED : Op::OP_MINIMUM_SIGNED) :
        (isMax ? Op::OP_MAXIMUM_UNSIGNED : Op::OP_MINIMUM_UNSIGNED);
    writeGpr(c, bits(e, 4, 0), is64, false, c.emit(operation, type, a, b));
}

// The data processing (2 source): udiv, sdiv, the variable shifts, crc32,
// and the CSSC min / max
void liftDataProcessing2(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const uint opcode = bits(e, 15, 10);
    const uint rd = bits(e, 4, 0);
    const SemanticType type = typeOf(is64);
    const uint size = is64 ? 64 : 32;
    const SemanticOperand a = readGpr(c, bits(e, 9, 5), is64, false);

    switch (opcode)
    {
    case 0x02:
    case 0x03:
    {
        // udiv, sdiv: a division by zero is 0, the minimum / -1 the minimum
        const SemanticOperand b = readGpr(c, bits(e, 20, 16), is64, false);
        SemanticOperand result = c.newTemporary(type);
        SemanticInstruction division(opcode == 2 ? Op::OP_DIVIDE_UNSIGNED :
                                     Op::OP_DIVIDE_SIGNED, type, result, a, b);
        division.setBehavior(Op::BEHAVIOR_DIVIDE_ZERO);
        c.emit(division);
        writeGpr(c, rd, is64, false, result);
        return;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    {
        // lslv, lsrv, asrv, rorv: the amount modulo the size
        SemanticOperand amount = emitImmediate(c, Op::OP_AND, type,
            readGpr(c, bits(e, 20, 16), is64, false), size - 1);
        static const Op::Operation operations[4] = {
            Op::OP_SHIFT_LEFT, Op::OP_SHIFT_RIGHT_LOGICAL,
            Op::OP_SHIFT_RIGHT_ARITHMETIC, Op::OP_ROTATE_RIGHT };
        writeGpr(c, rd, is64, false,
                 c.emit(operations[opcode - 8], type, a, amount));
        return;
    }
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    {
        // crc32{b,h,w,x}, crc32c{b,h,w,x}: the accumulator is Rn (a w
        // register), the data is Rm
        const uint dataBits = 8u << (opcode & 3);
        SemanticOperand data = readGpr(c, bits(e, 20, 16), dataBits == 64, false);
        if (dataBits < 32)
            data = c.emit(Op::OP_TRUNCATE, SemanticType::integer(dataBits), data);
        const SemanticType word = SemanticType::integer(32);
        SemanticOperand accumulator = readGpr(c, bits(e, 9, 5), false, false);
        SemanticOperand result = c.newTemporary(word);
        SemanticInstruction crc(Op::OP_CRC32, word, result, data, accumulator);
        crc.setBehavior((opcode & 4) != 0 ? Op::BEHAVIOR_CRC32C :
                                            Op::BEHAVIOR_CRC32_IEEE);
        c.emit(crc);
        writeGpr(c, rd, false, false, result);
        return;
    }
    case 0x18: case 0x19: case 0x1A: case 0x1B:
        liftMinMax(c);
        return;
    default:
        undefined(c);
    }
}

// Return the pointer without its authentication code (xpaci, xpacd, xpaclri):
// Strip() of the Arm ARM for the user mode of macOS (47 bit virtual addresses),
// so the bits 63-47 are copies of bit 55. The data pointers of the lower half
// (bit 55 clear) have the top byte ignored (TBI0): xpacd keeps it. The M4 does
// the same
SemanticOperand stripPointer(ArmLiftContext& c, const SemanticOperand& value,
                             bool isData)
{
    const SemanticType word = SemanticType::integer(64);
    const uint64 lowMask = ((uint64)1 << 47) - 1;
    SemanticOperand isHigh = compare(c, Op::COND_NOT_EQUAL, word,
        emitImmediate(c, Op::OP_AND, word, value, (int64)1 << 55),
        c.getImmediate(0, word));
    SemanticOperand low = emitImmediate(c, Op::OP_AND, word, value, (int64)lowMask);
    SemanticOperand upper = emitImmediate(c, Op::OP_OR, word, low,
                                          (int64)~lowMask);
    SemanticOperand lower = low;
    if (isData)
        lower = c.emit(Op::OP_OR, word, low, emitImmediate(c, Op::OP_AND, word,
                                value, (int64)0xFF00000000000000ull));
    return c.emit(Op::OP_SELECT, word, isHigh, upper, lower);
}

// The data processing (1 source): rbit, rev16, rev32, rev, clz, cls, and
// the CSSC ctz, cnt, abs
void liftDataProcessing1(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const uint opcode = bits(e, 15, 10);
    const uint rd = bits(e, 4, 0);
    const SemanticType type = typeOf(is64);
    const SemanticOperand a = readGpr(c, bits(e, 9, 5), is64, false);
    if ((bits(e, 20, 16) == 1) && ((opcode == 0x10) || (opcode == 0x11)))
    {
        // xpaci, xpacd: strip the pointer authentication code of Xd
        writeGpr(c, rd, true, false,
                 stripPointer(c, readGpr(c, rd, true, false), opcode == 0x11));
        return;
    }
    if (bits(e, 20, 16) != 0)
    {
        undefined(c);
        return;
    }

    SemanticOperand result;
    switch (opcode)
    {
    case 0:
        result = c.emit(Op::OP_BIT_REVERSE, type, a);
        break;
    case 1:
    {
        // rev16: the bytes of each halfword
        const int64 mask = is64 ? (int64)0x00FF00FF00FF00FFull : 0x00FF00FFll;
        result = c.emit(Op::OP_OR, type,
            emitImmediate(c, Op::OP_SHIFT_LEFT, type,
                          emitImmediate(c, Op::OP_AND, type, a, mask), 8),
            emitImmediate(c, Op::OP_AND, type,
                emitImmediate(c, Op::OP_SHIFT_RIGHT_LOGICAL, type, a, 8), mask));
        break;
    }
    case 2:
        // rev (32 bit) / rev32 (64 bit): the bytes of each word
        result = c.emit(Op::OP_BYTE_SWAP, type, a);
        if (is64)
            result = emitImmediate(c, Op::OP_ROTATE_RIGHT, type, result, 32);
        break;
    case 3:
        // rev (64 bit)
        if (!is64)
        {
            undefined(c);
            return;
        }
        result = c.emit(Op::OP_BYTE_SWAP, type, a);
        break;
    case 4:
        result = c.emit(Op::OP_COUNT_LEADING_ZEROS, type, a);
        break;
    case 5:
        result = c.emit(Op::OP_COUNT_LEADING_SIGN_BITS, type, a);
        break;
    case 6:
        result = c.emit(Op::OP_COUNT_TRAILING_ZEROS, type, a);
        break;
    case 7:
        result = c.emit(Op::OP_POPULATION_COUNT, type, a);
        break;
    case 8:
    {
        // abs: the absolute value, the minimum stays
        SemanticOperand isNegative = compare(c, Op::COND_SIGNED_LESS, type, a,
                                             c.getImmediate(0, type));
        result = c.emit(Op::OP_SELECT, type, isNegative,
                        c.emit(Op::OP_NEGATE, type, a), a);
        break;
    }
    default:
        undefined(c);
        return;
    }
    writeGpr(c, rd, is64, false, result);
}

// madd, msub and the long / high multiplies
void liftMultiply(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const uint op31 = bits(e, 23, 21);
    const bool isSubtract = bits(e, 15, 15) != 0;
    const uint rd = bits(e, 4, 0);
    const uint rn = bits(e, 9, 5);
    const uint rm = bits(e, 20, 16);
    const uint ra = bits(e, 14, 10);
    const SemanticType wide = SemanticType::integer(64);

    if (op31 == 0)
    {
        // madd, msub (mul, mneg)
        const SemanticType type = typeOf(is64);
        SemanticOperand product = c.emit(Op::OP_MULTIPLY, type,
                    readGpr(c, rn, is64, false), readGpr(c, rm, is64, false));
        SemanticOperand addend = readGpr(c, ra, is64, false);
        writeGpr(c, rd, is64, false,
                 c.emit(isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD, type,
                        addend, product));
        return;
    }
    if (!is64)
    {
        undefined(c);
        return;
    }
    if ((op31 == 1) || (op31 == 5))
    {
        // smaddl, smsubl, umaddl, umsubl (smull, umull, smnegl, umnegl)
        const bool isSigned = op31 == 1;
        SemanticOperand a = extend(c, wide, readGpr(c, rn, false, false),
                                   isSigned);
        SemanticOperand b = extend(c, wide, readGpr(c, rm, false, false),
                                   isSigned);
        SemanticOperand product = c.emit(Op::OP_MULTIPLY, wide, a, b);
        SemanticOperand addend = readGpr(c, ra, true, false);
        writeGpr(c, rd, true, false,
                 c.emit(isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD, wide,
                        addend, product));
        return;
    }
    if ((op31 == 2) || (op31 == 6))
    {
        // smulh, umulh
        writeGpr(c, rd, true, false,
                 c.emit(op31 == 2 ? Op::OP_MULTIPLY_HIGH_SIGNED :
                                    Op::OP_MULTIPLY_HIGH_UNSIGNED, wide,
                        readGpr(c, rn, true, false),
                        readGpr(c, rm, true, false)));
        return;
    }
    undefined(c);
}

// csel, csinc, csinv, csneg and their aliases cset, csetm, cinc, cinv, cneg
void liftConditionalSelect(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const bool op = bits(e, 30, 30) != 0;
    const bool o2 = bits(e, 10, 10) != 0;
    const SemanticType type = typeOf(is64);
    const SemanticOperand condition = c.testCondition(bits(e, 15, 12));
    const SemanticOperand a = readGpr(c, bits(e, 9, 5), is64, false);
    SemanticOperand b = readGpr(c, bits(e, 20, 16), is64, false);
    if (op)
        b = c.emit(Op::OP_NOT, type, b);
    if (o2)
        b = emitImmediate(c, Op::OP_ADD, type, b, 1);
    writeGpr(c, bits(e, 4, 0), is64, false,
             c.emit(Op::OP_SELECT, type, condition, a, b));
}

// ccmp, ccmn (register and immediate)
void liftConditionalCompare(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const bool isCompare = bits(e, 30, 30) != 0;
    const uint condition = bits(e, 15, 12);
    const uint nzcv = bits(e, 3, 0);
    const SemanticType type = typeOf(is64);
    const SemanticOperand a = readGpr(c, bits(e, 9, 5), is64, false);
    SemanticOperand b;
    if (bits(e, 11, 11) != 0)
        b = c.getImmediate(bits(e, 20, 16), type);
    else
        b = readGpr(c, bits(e, 20, 16), is64, false);

    // The flags of the comparison. ccmp is a - b, ccmn is a + b
    uint32 falseFlags = 0;
    if (nzcv & 8) falseFlags |= SemanticFlags::FLAG_SIGN;
    if (nzcv & 4) falseFlags |= SemanticFlags::FLAG_ZERO;
    if (nzcv & 2) falseFlags |= SemanticFlags::FLAG_CARRY;
    if (nzcv & 1) falseFlags |= SemanticFlags::FLAG_OVERFLOW;

    SemanticOperand flagsB = isCompare ? invert(c, b) : b;
    SemanticOperand result = isCompare ? c.emit(Op::OP_SUBTRACT, type, a, b) :
                                         c.emit(Op::OP_ADD, type, a, b);
    // hi and ls are not a single condition on the registers: branch over
    const bool isComposite = (condition == 8) || (condition == 9);
    if (!isComposite)
    {
        SemanticInstruction instruction(Op::OP_SET_FLAGS, type,
                                        SemanticOperand(), a, flagsB, result);
        instruction.setFlags(Op::FLAGS_ADD, FLAGS_NZCV);
        instruction.setCondition(ArmLiftContext::convertCondition(condition));
        instruction.setFalseFlags(falseFlags);
        c.emit(instruction);
        return;
    }
    SemanticOperand isTrue = c.testCondition(condition);
    SemanticOperand compareLabel = c.newLabel();
    SemanticOperand endLabel = c.newLabel();
    c.branchIf(isTrue, compareLabel);
    for (uint i = 0; i < 4; i++)
    {
        static const uint32 flags[4] = { SemanticFlags::FLAG_SIGN,
            SemanticFlags::FLAG_ZERO, SemanticFlags::FLAG_CARRY,
            SemanticFlags::FLAG_OVERFLOW };
        c.setFlags(flags[i], booleanConstant(c, (falseFlags & flags[i]) != 0));
    }
    c.branch(endLabel);
    c.placeLabel(compareLabel);
    c.setFlags(Op::FLAGS_ADD, FLAGS_NZCV, type, a, flagsB, result);
    c.placeLabel(endLabel);
}

// rmif: rotate the bits of Xn into the flags selected by the mask
void liftRmif(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const SemanticType type = SemanticType::integer(64);
    const uint shift = bits(e, 20, 15);
    const uint mask = bits(e, 3, 0);
    SemanticOperand value = readGpr(c, bits(e, 9, 5), true, false);
    if (shift != 0)
        value = emitImmediate(c, Op::OP_ROTATE_RIGHT, type, value, shift);
    static const uint32 flags[4] = { SemanticFlags::FLAG_OVERFLOW,
        SemanticFlags::FLAG_CARRY, SemanticFlags::FLAG_ZERO,
        SemanticFlags::FLAG_SIGN };
    for (uint i = 0; i < 4; i++)
    {
        if (((mask >> i) & 1) == 0)
            continue;
        SemanticOperand bit = c.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                    emitImmediate(c, Op::OP_SHIFT_RIGHT_LOGICAL, type, value, i));
        c.setFlags(flags[i], bit);
    }
}

// setf8, setf16: N = bit 7 (15), Z = low byte (halfword) zero, V = bit 8 (16)
// xor bit 7 (15)
void liftSetf(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const uint size = (bits(e, 14, 14) != 0) ? 16 : 8;
    const SemanticType word = SemanticType::integer(32);
    const SemanticType small = SemanticType::integer(size);
    const SemanticOperand value = readGpr(c, bits(e, 9, 5), false, false);
    const SemanticType booleanType = SemanticType::integer(1);
    SemanticOperand narrow = c.emit(Op::OP_TRUNCATE, small, value);
    SemanticOperand sign = c.emit(Op::OP_TRUNCATE, booleanType,
        emitImmediate(c, Op::OP_SHIFT_RIGHT_LOGICAL, word, value, size - 1));
    SemanticOperand next = c.emit(Op::OP_TRUNCATE, booleanType,
        emitImmediate(c, Op::OP_SHIFT_RIGHT_LOGICAL, word, value, size));
    c.setFlags(SemanticFlags::FLAG_SIGN, sign);
    c.setFlags(SemanticFlags::FLAG_ZERO,
               compare(c, Op::COND_EQUAL, small, narrow,
                       c.getImmediate(0, small)));
    c.setFlags(SemanticFlags::FLAG_OVERFLOW,
               c.emit(Op::OP_XOR, booleanType, next, sign));
}

// cfinv, axflag, xaflag
void liftFlagManipulation(ArmLiftContext& c, uint op2)
{
    const SemanticType booleanType = SemanticType::integer(1);
    const SemanticOperand z = c.getRegister(ARM64_REG_Z);
    const SemanticOperand carry = c.getRegister(ARM64_REG_C);
    const SemanticOperand v = c.getRegister(ARM64_REG_V);
    if (op2 == 0)
    {
        // cfinv
        c.setFlags(SemanticFlags::FLAG_CARRY,
                   c.emit(Op::OP_XOR, booleanType, carry,
                          booleanConstant(c, true)));
    } else if (op2 == 2)
    {
        // axflag: Z = Z | V, C = C & ~V, N = 0, V = 0
        SemanticOperand newZ = c.emit(Op::OP_OR, booleanType, z, v);
        SemanticOperand newC = c.emit(Op::OP_AND, booleanType, carry,
                    c.emit(Op::OP_XOR, booleanType, v, booleanConstant(c, true)));
        c.setFlags(SemanticFlags::FLAG_ZERO, newZ);
        c.setFlags(SemanticFlags::FLAG_CARRY, newC);
        c.setFlags(SemanticFlags::FLAG_SIGN, booleanConstant(c, false));
        c.setFlags(SemanticFlags::FLAG_OVERFLOW, booleanConstant(c, false));
    } else
    {
        // xaflag: N = ~C & ~Z, Z = Z & C, C = C | Z, V = ~C & Z
        SemanticOperand notC = c.emit(Op::OP_XOR, booleanType, carry,
                                      booleanConstant(c, true));
        SemanticOperand notZ = c.emit(Op::OP_XOR, booleanType, z,
                                      booleanConstant(c, true));
        SemanticOperand newN = c.emit(Op::OP_AND, booleanType, notC, notZ);
        SemanticOperand newZ = c.emit(Op::OP_AND, booleanType, z, carry);
        SemanticOperand newC = c.emit(Op::OP_OR, booleanType, carry, z);
        SemanticOperand newV = c.emit(Op::OP_AND, booleanType, notC, z);
        c.setFlags(SemanticFlags::FLAG_SIGN, newN);
        c.setFlags(SemanticFlags::FLAG_ZERO, newZ);
        c.setFlags(SemanticFlags::FLAG_CARRY, newC);
        c.setFlags(SemanticFlags::FLAG_OVERFLOW, newV);
    }
}

} // namespace

void armdis::liftArm64DataProcessing(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const char* name = getMnemonic(c);

    // The instructions whose encodings the bits below don't separate
    if (strcmp(name, "rmif") == 0)
    {
        liftRmif(c);
        return;
    }
    if ((strcmp(name, "setf8") == 0) || (strcmp(name, "setf16") == 0))
    {
        liftSetf(c);
        return;
    }
    if ((strcmp(name, "smax") == 0) || (strcmp(name, "smin") == 0) ||
        (strcmp(name, "umax") == 0) || (strcmp(name, "umin") == 0))
    {
        liftMinMax(c);
        return;
    }

    if (bits(e, 28, 26) == 4)
    {
        // Data processing (immediate)
        switch (bits(e, 25, 23))
        {
        case 0:
        case 1:
            liftPcRelative(c);
            return;
        case 2:
            liftAddSub(c);
            return;
        case 4:
            liftLogical(c);
            return;
        case 5:
            liftMoveWide(c);
            return;
        case 6:
            liftBitfield(c);
            return;
        case 7:
            liftExtract(c);
            return;
        default:
            undefined(c);
            return;
        }
    }

    // Data processing (register)
    if (bits(e, 28, 28) == 0)
    {
        if (bits(e, 24, 24) == 0)
            liftLogical(c);
        else
            liftAddSub(c);
        return;
    }
    if (bits(e, 28, 24) == 0x1B)
    {
        liftMultiply(c);
        return;
    }
    switch (bits(e, 23, 21))
    {
    case 0:
        // add / subtract with carry
        liftAddSub(c);
        return;
    case 2:
        liftConditionalCompare(c);
        return;
    case 4:
        liftConditionalSelect(c);
        return;
    case 6:
        if (bits(e, 30, 30) == 0)
            liftDataProcessing2(c);
        else
            liftDataProcessing1(c);
        return;
    default:
        undefined(c);
    }
}

/////////////////////////////////////////////////////////////////////////////
// Branches, exceptions and system
/////////////////////////////////////////////////////////////////////////////

namespace {

// The system registers (the packed op0:op1:CRn:CRm:op2 of mrs and msr) which
// the lifter reads and writes as registers of the description
enum {
    SYSREG_NZCV         = 0xDA10,
    SYSREG_FPCR         = 0xDA20,
    SYSREG_FPSR         = 0xDA21,
    SYSREG_TPIDR_EL0    = 0xDE82,
    SYSREG_TPIDRRO_EL0  = 0xDE83,
    SYSREG_DCZID_EL0    = 0xD807
};

// The bits of fpcr and fpsr which exist
const uint32 FPCR_MASK = 0x07FF9F07;
const uint32 FPSR_MASK = 0x0800009F;

// The size of the block of dc zva in bytes (DCZID_EL0.BS = 4: 16 words)
const uint ZERO_BLOCK_BYTES = 64;

// Add an OP_HELPER and return its result (a temporary of 'type')
SemanticOperand helper(ArmLiftContext& c, uint number, const SemanticType& type,
                       int64 argument0, const SemanticOperand& argument1)
{
    const SemanticType word = SemanticType::integer(64);
    SemanticOperand result = c.newTemporary(type);
    c.emit(SemanticInstruction(Op::OP_HELPER, type, result,
                               c.getImmediate(number, word),
                               c.getImmediate(argument0, word), argument1));
    return result;
}

// b, bl
void liftBranchImmediate(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const SemanticType word = SemanticType::integer(64);
    const SemanticOperand target = SemanticOperand::guestAddress(
                                    c.getOperand(0).m_immediate, word);
    if (bits(e, 31, 31) != 0)
    {
        // bl: the return address in the link register
        writeGpr(c, 30, true, false, c.getImmediate(
            (int64)(c.getInstruction().getAddress() + 4), word));
        c.call(target);
        return;
    }
    c.branch(target);
}

// b.cond, bc.cond
void liftBranchCondition(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const SemanticOperand target = SemanticOperand::guestAddress(
                    c.getOperand(0).m_immediate, SemanticType::integer(64));
    c.branchIf(c.testCondition(bits(e, 3, 0)), target);
}

// cbz, cbnz
void liftCompareBranch(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool is64 = bits(e, 31, 31) != 0;
    const SemanticType type = typeOf(is64);
    const SemanticOperand target = SemanticOperand::guestAddress(
                    c.getOperand(1).m_immediate, SemanticType::integer(64));
    c.branchIf(compare(c, bits(e, 24, 24) != 0 ? Op::COND_NOT_EQUAL :
                                                 Op::COND_EQUAL, type,
                       readGpr(c, bits(e, 4, 0), is64, false),
                       c.getImmediate(0, type)), target);
}

// tbz, tbnz
void liftTestBranch(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const uint bit = (bits(e, 31, 31) << 5) | bits(e, 23, 19);
    const SemanticType type = SemanticType::integer(64);
    const SemanticOperand target = SemanticOperand::guestAddress(
                    c.getOperand(2).m_immediate, type);
    SemanticOperand masked = emitImmediate(c, Op::OP_AND, type,
                    readGpr(c, bits(e, 4, 0), true, false),
                    (int64)((uint64)1 << bit));
    c.branchIf(compare(c, bits(e, 24, 24) != 0 ? Op::COND_NOT_EQUAL :
                                                 Op::COND_EQUAL, type, masked,
                       c.getImmediate(0, type)), target);
}

// FEAT_CMPBR: cb<cc> Rt, Rm | #imm, label (and cbb<cc>, cbh<cc> on the low byte
// or halfword): compare and branch. The compare is signed for gt, ge and lt,
// unsigned for hi, hs and lo; the immediate is an unsigned 6 bit number
void liftCompareRegisterBranch(ArmLiftContext& c)
{
    const char* name = getMnemonic(c) + 2;
    uint bits = c.getArchitecture()->getRegister(c.getOperand(0).m_register).m_bits;
    static const struct {
        const char* m_name;
        Op::Condition m_condition;
    } conditions[] = {
        { "gt", Op::COND_SIGNED_GREATER }, { "ge", Op::COND_SIGNED_GREATER_EQUAL },
        { "lt", Op::COND_SIGNED_LESS }, { "hi", Op::COND_UNSIGNED_GREATER },
        { "hs", Op::COND_UNSIGNED_GREATER_EQUAL }, { "lo", Op::COND_UNSIGNED_LESS },
        { "eq", Op::COND_EQUAL }, { "ne", Op::COND_NOT_EQUAL }
    };
    const uint conditionsCount = sizeof(conditions) / sizeof(conditions[0]);
    // cbhi is cb + hi, cbhhi is cbh + hi: the whole name is tried first
    uint index = 0;
    while ((index < conditionsCount) && (strcmp(name, conditions[index].m_name) != 0))
        index++;
    if (index == conditionsCount)
    {
        if (*name == 'b')
            bits = 8;
        else if (*name == 'h')
            bits = 16;
        name++;
        index = 0;
        while ((index < conditionsCount) && (strcmp(name, conditions[index].m_name) != 0))
            index++;
    }
    if (index == conditionsCount)
    {
        undefined(c);
        return;
    }
    const SemanticType type = SemanticType::integer(bits);
    SemanticOperand a = c.read(0);
    SemanticOperand b;
    if (c.getOperand(1).m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
        b = c.getImmediate(c.getOperand(1).m_immediate, type);
    else
        b = c.read(1);
    if (a.getType().getTotalBits() > bits)
    {
        a = c.emit(Op::OP_TRUNCATE, type, a);
        if (b.getKind() != SemanticOperand::KIND_IMMEDIATE)
            b = c.emit(Op::OP_TRUNCATE, type, b);
    }
    c.branchIf(compare(c, conditions[index].m_condition, type, a, b),
               SemanticOperand::guestAddress(c.getOperand(2).m_immediate,
                                             SemanticType::integer(64)));
}

// br, blr, ret
void liftBranchRegister(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const uint opc = bits(e, 24, 21);
    const SemanticType word = SemanticType::integer(64);
    if ((bits(e, 20, 16) != 0x1F) || (bits(e, 15, 10) != 0) ||
        (bits(e, 4, 0) != 0) || (opc > 2))
    {
        undefined(c);
        return;
    }
    // The target is read before the link register is written (blr x30)
    SemanticOperand target = c.emit(Op::OP_MOVE, word,
                                    readGpr(c, bits(e, 9, 5), true, false));
    if (opc == 0)
    {
        c.branchIndirect(target);
    } else if (opc == 1)
    {
        writeGpr(c, 30, true, false, c.getImmediate(
            (int64)(c.getInstruction().getAddress() + 4), word));
        c.call(target);
    } else
    {
        c.returnTo(target);
    }
}

// svc, brk, hlt, udf
void liftException(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const uint immediate = bits(e, 20, 5);
    const char* name = getMnemonic(c);
    if (strcmp(name, "svc") == 0)
        c.trap(Op::TRAP_SYSTEM_CALL, immediate);
    else if (strcmp(name, "brk") == 0)
        c.trap(Op::TRAP_BREAKPOINT, immediate);
    else if (strcmp(name, "hlt") == 0)
        // The halt instruction is undefined outside of the debug state
        c.trap(Op::TRAP_UNDEFINED, ARM_TRAP_HALT);
    else
        c.trap(Op::TRAP_UNDEFINED, immediate);
}

// Add a memory barrier of the option (CRm) of dmb and dsb
void liftBarrier(ArmLiftContext& c, uint option)
{
    uint access;
    switch (option & 3)
    {
    case 1:
        // ld: the loads before, the loads and the stores after
        access = Op::FENCE_BEFORE_LOADS | Op::FENCE_AFTER_LOADS |
                 Op::FENCE_AFTER_STORES;
        break;
    case 2:
        // st: the stores before the stores
        access = Op::FENCE_BEFORE_STORES | Op::FENCE_AFTER_STORES;
        break;
    default:
        access = Op::FENCE_ALL;
        break;
    }
    SemanticInstruction fence(Op::OP_FENCE, SemanticType(), SemanticOperand());
    fence.setMemoryOrder(Op::ORDER_SEQUENTIAL);
    fence.setFenceAccess((uint8)access);
    c.emit(fence);
}

// Add an OP_HINT
void liftHint(ArmLiftContext& c, Op::HintKind kind)
{
    SemanticInstruction hint(Op::OP_HINT, SemanticType(), SemanticOperand());
    hint.setKind(kind);
    c.emit(hint);
}

// The hint space (nop, yield, wfe, wfi, sev, sevl, bti...)
void liftHintSpace(ArmLiftContext& c, uint number)
{
    switch (number)
    {
    case 1:
        liftHint(c, Op::HINT_SPIN);
        break;
    case 2:
    case 3:
        liftHint(c, Op::HINT_WAIT);
        break;
    case 4:
    case 5:
        liftHint(c, Op::HINT_SEND_EVENT);
        break;
    case 7:
        // xpaclri: strip the pointer authentication code of x30
        writeGpr(c, 30, true, false,
                 stripPointer(c, readGpr(c, 30, true, false), false));
        break;
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
        liftHint(c, Op::HINT_LANDING_PAD);
        break;
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
        // The pointer authentication hints (pacia1716, paciasp, autiasp...):
        // the keys are not modeled
        c.emit(SemanticInstruction(Op::OP_HELPER, SemanticType::integer(64),
            c.newTemporary(SemanticType::integer(64)),
            c.getImmediate(ARM_HELPER_INSTRUCTION, SemanticType::integer(64)),
            c.getImmediate(c.getInstruction().getEncoding(),
                           SemanticType::integer(64))));
        break;
    default:
        // nop, dgh, esb, psb, tsb, csdb, clrbhb, chkfeat and the unallocated
        // hints
        c.emit(SemanticInstruction(Op::OP_NOP, SemanticType(),
                                   SemanticOperand()));
        break;
    }
}

// mrs
void liftReadSystemRegister(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const uint packed = bits(e, 20, 5);
    const uint rt = bits(e, 4, 0);
    const SemanticType word = SemanticType::integer(64);
    SemanticOperand value;
    switch (packed)
    {
    case SYSREG_NZCV:
        value = packNzcv(c);
        break;
    case SYSREG_FPCR:
        value = c.emit(Op::OP_ZERO_EXTEND, word,
                       c.getRegister(ARM64_REG_FPCR));
        break;
    case SYSREG_FPSR:
        value = c.emit(Op::OP_ZERO_EXTEND, word,
                       c.getRegister(ARM64_REG_FPSR));
        break;
    case SYSREG_TPIDR_EL0:
        value = c.emit(Op::OP_MOVE, word, c.getRegister(ARM64_REG_TPIDR_EL0));
        break;
    case SYSREG_TPIDRRO_EL0:
        value = c.emit(Op::OP_MOVE, word, c.getRegister(ARM64_REG_TPIDRRO_EL0));
        break;
    case SYSREG_DCZID_EL0:
        // DZP = 0 (dc zva allowed), BS = log2(words) = 4
        value = c.getImmediate(ZERO_BLOCK_BYTES / 16, word);
        break;
    default:
        value = helper(c, ARM_HELPER_READ_SYSTEM_REGISTER, word, packed,
                       SemanticOperand::immediate(0, word));
        break;
    }
    writeGpr(c, rt, true, false, value);
}

// msr (register)
void liftWriteSystemRegister(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const uint packed = bits(e, 20, 5);
    const SemanticType word = SemanticType::integer(64);
    const SemanticType half = SemanticType::integer(32);
    SemanticOperand value = readGpr(c, bits(e, 4, 0), true, false);
    switch (packed)
    {
    case SYSREG_NZCV:
        unpackNzcv(c, value);
        break;
    case SYSREG_FPCR:
        c.move(c.getRegister(ARM64_REG_FPCR),
               emitImmediate(c, Op::OP_AND, half,
                             c.emit(Op::OP_TRUNCATE, half, value), FPCR_MASK));
        break;
    case SYSREG_FPSR:
        c.move(c.getRegister(ARM64_REG_FPSR),
               emitImmediate(c, Op::OP_AND, half,
                             c.emit(Op::OP_TRUNCATE, half, value), FPSR_MASK));
        break;
    case SYSREG_TPIDR_EL0:
        c.move(c.getRegister(ARM64_REG_TPIDR_EL0), value);
        break;
    default:
        helper(c, ARM_HELPER_WRITE_SYSTEM_REGISTER, word, packed, value);
        break;
    }
}

// sys: the data cache and instruction cache maintenance by address
void liftSystem(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const uint op1 = bits(e, 18, 16);
    const uint crn = bits(e, 15, 12);
    const uint crm = bits(e, 11, 8);
    const uint op2 = bits(e, 7, 5);
    const SemanticType word = SemanticType::integer(64);
    if ((bits(e, 21, 21) != 0) || (op1 != 3) || (crn != 7))
    {
        undefined(c);
        return;
    }
    if (crm == 3)
    {
        // cfp, dvp, cpp (FEAT_SPECRES): restrict the speculation, nothing
        // observable
        c.emit(SemanticInstruction(Op::OP_NOP, SemanticType(),
                                   SemanticOperand()));
        return;
    }
    SemanticOperand address = readGpr(c, bits(e, 4, 0), true, false);
    Op::CacheOperation operation;
    if ((crm == 4) && (op2 == 1))
    {
        // dc zva: zero the block that contains the address
        SemanticOperand base = emitImmediate(c, Op::OP_AND, word, address,
                                             -(int64)ZERO_BLOCK_BYTES);
        for (uint offset = 0; offset < ZERO_BLOCK_BYTES; offset += 8)
            c.store(emitImmediate(c, Op::OP_ADD, word, base, offset),
                    c.getImmediate(0, word));
        return;
    } else if ((crm == 5) && (op2 == 1))
    {
        // ic ivau
        c.load(address, SemanticType::integer(8));
        c.emit(SemanticInstruction(Op::OP_FENCE_INSTRUCTION, SemanticType(),
                                   SemanticOperand()));
        return;
    } else if (((crm == 10) || (crm == 11) || (crm == 12) || (crm == 13)) &&
               (op2 == 1))
    {
        // dc cvac, cvau, cvap, cvadp
        operation = Op::CACHE_CLEAN;
    } else if ((crm == 14) && (op2 == 1))
    {
        // dc civac
        operation = Op::CACHE_FLUSH;
    } else
    {
        undefined(c);
        return;
    }
    // The maintenance by address faults when the address is not mapped
    c.load(address, SemanticType::integer(8));
    SemanticInstruction maintenance(Op::OP_CACHE_MAINTENANCE, SemanticType(),
                                    SemanticOperand(), address);
    maintenance.setKind(operation);
    c.emit(maintenance);
}

// The system instructions
void liftSystemClass(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const bool isRead = bits(e, 21, 21) != 0;
    const uint op0 = bits(e, 20, 19);
    const uint crn = bits(e, 15, 12);
    const uint crm = bits(e, 11, 8);
    const uint op2 = bits(e, 7, 5);

    if (op0 >= 2)
    {
        if (isRead)
            liftReadSystemRegister(c);
        else
            liftWriteSystemRegister(c);
        return;
    }
    if (op0 == 1)
    {
        liftSystem(c);
        return;
    }
    if (isRead)
    {
        undefined(c);
        return;
    }
    if (crn == 1)
    {
        // wfet, wfit: wait for an event with a timeout
        liftHint(c, Op::HINT_WAIT);
    } else if (crn == 9)
    {
        // stshh: the store shareability hint (FEAT_PCDPHINT)
        c.emit(SemanticInstruction(Op::OP_NOP, SemanticType(),
                                   SemanticOperand()));
    } else if (crn == 2)
    {
        liftHintSpace(c, (crm << 3) | op2);
    } else if (crn == 3)
    {
        switch (op2)
        {
        case 2:
            // clrex
            clearExclusiveMonitor(c);
            break;
        case 1:
            // dsb nXS: the barrier of every access
            liftBarrier(c, 3);
            break;
        case 4:
        case 5:
            // dsb, dmb (ssbb and pssbb are dsb #0 and #4)
            liftBarrier(c, crm);
            break;
        case 6:
            // isb
            c.emit(SemanticInstruction(Op::OP_FENCE_INSTRUCTION,
                                       SemanticType(), SemanticOperand()));
            break;
        case 7:
            // sb: the speculation barrier, nothing observable
            liftBarrier(c, 3);
            break;
        default:
            undefined(c);
        }
    } else if (crn == 4)
    {
        // msr (immediate): the PSTATE fields (op1:op2)
        const uint field = (bits(e, 18, 16) << 3) | op2;
        if (field == 0)
            liftFlagManipulation(c, 0);
        else if (field == 1)
            liftFlagManipulation(c, 1);
        else if (field == 2)
            liftFlagManipulation(c, 2);
        else
            undefined(c);
    } else
    {
        undefined(c);
    }
}

} // namespace

void armdis::liftArm64BranchSystem(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    const char* name = getMnemonic(c);
    if ((name[0] == 'c') && (name[1] == 'b') && (strcmp(name, "cbz") != 0) &&
        (strcmp(name, "cbnz") != 0))
        liftCompareRegisterBranch(c);
    else if (bits(e, 30, 26) == 5)
        liftBranchImmediate(c);
    else if (bits(e, 30, 25) == 0x1A)
        liftCompareBranch(c);
    else if (bits(e, 30, 25) == 0x1B)
        liftTestBranch(c);
    else if (bits(e, 31, 25) == 0x2A)
        liftBranchCondition(c);
    else if (bits(e, 31, 24) == 0xD4)
        liftException(c);
    else if (bits(e, 31, 22) == 0x354)
        liftSystemClass(c);
    else if (bits(e, 31, 25) == 0x6B)
        liftBranchRegister(c);
    else
        undefined(c);
}

/////////////////////////////////////////////////////////////////////////////
// The entry point
/////////////////////////////////////////////////////////////////////////////

void armdis::liftArm64Base(ArmLiftContext& c)
{
    const uint32 e = c.getInstruction().getEncoding();
    if (bits(e, 31, 16) == 0)
        // udf #imm16 (the reserved space)
        c.trap(Op::TRAP_UNDEFINED, bits(e, 15, 0));
    else if ((bits(e, 27, 27) != 0) && (bits(e, 25, 25) == 0))
        liftArm64LoadStore(c);
    else if (bits(e, 28, 26) == 5)
        liftArm64BranchSystem(c);
    else
        liftArm64DataProcessing(c);
}
