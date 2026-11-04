/*
 * ArmSemanticLift32.cpp
 *
 * Implementation file
 *
 * The A32 / T32 integer lifters: data processing with the shifter operand
 * and its carry out, the multiplies, the saturating and the parallel
 * arithmetic, the bit fields, the extends, the branches, the hints, the
 * exceptions and the APSR. See ArmSemanticInternal.h and the table
 * ArmSemanticTable.cpp. The semantics are the pseudocode of the Arm ARM
 * (AddWithCarry, Shift_C, ThumbExpandImm_C, SignedSatQ...); the condition of
 * the instruction wraps the template (ArmSemanticLifter::lift()), so the
 * handlers don't test it.
 *
 * The flags are the registers N Z C V (ARM carry: the inverted borrow of a
 * subtraction), Q and GE0-GE3. A subtraction a - b is a + ~b + 1 for the
 * flags (FLAGS_ADD of the inverted operand), like the A64 lifter does.
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
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/ArmSemanticInternal.h"

using namespace armdis;

typedef SemanticOperation Op;

namespace armdis {
namespace a32 {

const char* getMnemonic(const ArmLiftContext& context)
{
    return context.getInstruction().getEntry()->m_mnemonic;
}

bool isMnemonic(const ArmLiftContext& context, const char* name)
{
    const char* mnemonic = getMnemonic(context);
    size_t length = strlen(mnemonic);
    if ((length > 2) && (mnemonic[length - 2] == '.') &&
        (mnemonic[length - 1] == 'w'))
        length -= 2;
    return (strlen(name) == length) && (strncmp(mnemonic, name, length) == 0);
}

void undefined(ArmLiftContext& context)
{
    context.emit(SemanticInstruction(Op::OP_UNDEFINED, SemanticType(),
                                     SemanticOperand()));
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

void accumulateSaturation(ArmLiftContext& context, const SemanticOperand& flag)
{
    context.move(context.getRegister(ARM_REG_Q),
                 context.emit(Op::OP_OR, SemanticType::integer(1),
                              context.getRegister(ARM_REG_Q), flag));
}

SemanticOperand getRegisterValue(ArmLiftContext& context, uint registerId)
{
    if (registerId == ARM_REG_PC)
        return context.getPcValue();
    return context.getRegister(registerId);
}

} // namespace a32
} // namespace armdis

using namespace armdis::a32;

/////////////////////////////////////////////////////////////////////////////
// The helpers
/////////////////////////////////////////////////////////////////////////////

namespace {

SemanticType word()
{
    return SemanticType::integer(32);
}

SemanticType bitType()
{
    return SemanticType::integer(1);
}

SemanticType doubleWord()
{
    return SemanticType::integer(64);
}

SemanticOperand k32(ArmLiftContext& c, uint64 value)
{
    return c.getImmediate((int64)(value & 0xFFFFFFFFULL), word());
}

SemanticOperand k64(ArmLiftContext& c, uint64 value)
{
    return c.getImmediate((int64)value, doubleWord());
}

SemanticOperand b32(ArmLiftContext& c, Op::Operation operation,
                    const SemanticOperand& a, const SemanticOperand& b)
{
    return c.emit(operation, word(), a, b);
}

SemanticOperand b32(ArmLiftContext& c, Op::Operation operation,
                    const SemanticOperand& a, uint64 value)
{
    return c.emit(operation, word(), a, k32(c, value));
}

SemanticOperand zext(ArmLiftContext& c, const SemanticOperand& value,
                     uint bits)
{
    return c.emit(Op::OP_ZERO_EXTEND, SemanticType::integer(bits), value);
}

SemanticOperand sext(ArmLiftContext& c, const SemanticOperand& value,
                     uint bits)
{
    return c.emit(Op::OP_SIGN_EXTEND, SemanticType::integer(bits), value);
}

SemanticOperand truncTo(ArmLiftContext& c, const SemanticOperand& value,
                        uint bits)
{
    return c.emit(Op::OP_TRUNCATE, SemanticType::integer(bits), value);
}

// Return the bits 'position'..'position + bits - 1' of the 32 bit 'value'
// as an integer of 'bits' bits
SemanticOperand field(ArmLiftContext& c, const SemanticOperand& value,
                      uint position, uint bits)
{
    SemanticOperand shifted = value;
    if (position != 0)
        shifted = b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value, position);
    return truncTo(c, shifted, bits);
}

// Return a copy of the carry flag in a temporary
SemanticOperand carryIn(ArmLiftContext& c)
{
    return c.emit(Op::OP_MOVE, bitType(), c.getRegister(ARM_REG_C));
}

// Return a 32 bit value of the 1 bit 'value'
SemanticOperand bitToWord(ArmLiftContext& c, const SemanticOperand& value)
{
    return zext(c, value, 32);
}

// Return the register id of operand 'index' (a register or a fixed name)
uint registerOf(ArmLiftContext& c, uint index)
{
    const ArmDecodedOperand& operand = c.getOperand(index);
    if (operand.m_kind == ArmDecodedOperand::KIND_NAME)
    {
        if (operand.m_name == cString("sp"))
            return ARM_REG_SP;
        if (operand.m_name == cString("lr"))
            return ARM_REG_LR;
        if (operand.m_name == cString("pc"))
            return ARM_REG_PC;
        return ARM_REG_R12;
    }
    return operand.m_register;
}

// Read operand 'index' as a 32 bit value
SemanticOperand rd(ArmLiftContext& c, uint index)
{
    return c.read(index, word());
}

// Return true if the entry of the instruction has an operand of the kind
bool hasSpecKind(ArmLiftContext& c, uint kind)
{
    const ArmEncodingEntry* entry = c.getInstruction().getEntry();
    for (uint i = 0; i < ARM_MAX_OPERAND_SPECS; i++)
    {
        if (entry->m_operands[i].m_kind == ARM_KIND_NONE)
            break;
        if (entry->m_operands[i].m_kind == kind)
            return true;
    }
    return false;
}

// The mnemonic of the instruction without the ".w" of the wide T32 encodings
struct BaseName {
    explicit BaseName(const ArmLiftContext& c)
    {
        const char* name = getMnemonic(c);
        size_t length = strlen(name);
        if ((length > 2) && (name[length - 2] == '.') && (name[length - 1] == 'w'))
            length -= 2;
        if (length > sizeof(m_text) - 1)
            length = sizeof(m_text) - 1;
        memcpy(m_text, name, length);
        m_text[length] = 0;
    }

    char m_text[32];
};

bool isThumb(ArmLiftContext& c)
{
    return c.getInstruction().getInstructionSet() == ArmInstructionSet::ARM_T32;
}

// Return true if the immediate operand is a modified immediate with a
// rotation, which makes the carry out of the shifter its bit 31 (the
// ARMExpandImm_C and ThumbExpandImm_C of the Arm ARM)
bool isRotatedImmediate(ArmLiftContext& c)
{
    if (!hasSpecKind(c, ARM_KIND_A32_MODIFIED_IMMEDIATE))
        return false;
    const uint32 e = c.getInstruction().getEncoding();
    if (isThumb(c))
    {
        // i:imm3:imm8, the rotation is made of imm12<11:7>: not 00xxx
        const uint imm12 = (((e >> 26) & 1) << 11) | (((e >> 12) & 7) << 8) |
                           (e & 0xFF);
        return (imm12 >> 10) != 0;
    }
    return ((e >> 8) & 15) != 0;
}

/////////////////////////////////////////////////////////////////////////////
// The shifter operand
/////////////////////////////////////////////////////////////////////////////

// Return the 32 bit value of an immediate operand. The A32 modified immediate
// whose rotation is not the smallest one is the pair "#bits, #rotation"
uint64 getImmediateValue(const ArmDecodedOperand& operand)
{
    const uint64 bits = (uint64)operand.m_immediate & 0xFFFFFFFFULL;
    if (!operand.m_isExplicit || (operand.m_shiftAmount == 0))
        return bits;
    const uint amount = operand.m_shiftAmount & 31;
    return ((bits >> amount) | (bits << (32 - amount))) & 0xFFFFFFFFULL;
}

// The result of a shift and its carry out (1 bit, invalid when not wanted)
struct Shifted {
    SemanticOperand m_value;
    SemanticOperand m_carry;
};

enum ShiftKind { SHIFT_LSL, SHIFT_LSR, SHIFT_ASR, SHIFT_ROR, SHIFT_RRX };

/*
 * Shift_C of the Arm ARM by a constant amount (1 to 32 for lsr / asr, 1 to 31
 * for lsl / ror), or rrx
 */
Shifted shiftByConstant(ArmLiftContext& c, const SemanticOperand& value,
                        ShiftKind kind, uint amount, bool wantCarry,
                        const SemanticOperand& carry)
{
    Shifted result;
    switch (kind)
    {
    case SHIFT_LSL:
        result.m_value = b32(c, Op::OP_SHIFT_LEFT, value, amount);
        if (wantCarry)
            result.m_carry = truncTo(c, b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value,
                                          32 - amount), 1);
        break;
    case SHIFT_LSR:
        result.m_value = b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value, amount);
        if (wantCarry)
            result.m_carry = truncTo(c, b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value,
                                          amount - 1), 1);
        break;
    case SHIFT_ASR:
        result.m_value = b32(c, Op::OP_SHIFT_RIGHT_ARITHMETIC, value, amount);
        if (wantCarry)
            result.m_carry = truncTo(c, b32(c, Op::OP_SHIFT_RIGHT_ARITHMETIC,
                                          value, amount - 1), 1);
        break;
    case SHIFT_ROR:
        result.m_value = b32(c, Op::OP_ROTATE_RIGHT, value, amount);
        if (wantCarry)
            result.m_carry = truncTo(c, b32(c, Op::OP_SHIFT_RIGHT_LOGICAL,
                                          result.m_value, 31), 1);
        break;
    default:
        // rrx: the carry in goes to bit 31, bit 0 is the carry out
        result.m_value = b32(c, Op::OP_OR,
                             b32(c, Op::OP_SHIFT_LEFT, bitToWord(c, carry), 31),
                             b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value, 1));
        if (wantCarry)
            result.m_carry = truncTo(c, value, 1);
        break;
    }
    return result;
}

/*
 * Shift_C by the low byte of a register (a shift by a register): a zero
 * amount keeps the value and the carry, an amount of 32 or more follows the
 * semantic shifts (0, or the sign for asr, or the rotation modulo 32)
 */
Shifted shiftByRegister(ArmLiftContext& c, const SemanticOperand& value,
                        ShiftKind kind, const SemanticOperand& registerValue,
                        bool wantCarry, const SemanticOperand& carry)
{
    Shifted result;
    SemanticOperand amount = b32(c, Op::OP_AND, registerValue, 0xFF);
    SemanticOperand isZero = compare(c, Op::COND_EQUAL, word(), amount,
                                     k32(c, 0));
    SemanticOperand carryOut;
    switch (kind)
    {
    case SHIFT_LSL:
        result.m_value = b32(c, Op::OP_SHIFT_LEFT, value, amount);
        if (wantCarry)
            carryOut = truncTo(c, b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value,
                                    b32(c, Op::OP_SUBTRACT, k32(c, 32),
                                        amount)), 1);
        break;
    case SHIFT_LSR:
        result.m_value = b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value, amount);
        if (wantCarry)
            carryOut = truncTo(c, b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value,
                                    b32(c, Op::OP_SUBTRACT, amount,
                                        k32(c, 1))), 1);
        break;
    case SHIFT_ASR:
        result.m_value = b32(c, Op::OP_SHIFT_RIGHT_ARITHMETIC, value, amount);
        if (wantCarry)
            carryOut = truncTo(c, b32(c, Op::OP_SHIFT_RIGHT_ARITHMETIC, value,
                                    b32(c, Op::OP_SUBTRACT, amount,
                                        k32(c, 1))), 1);
        break;
    default:
        result.m_value = b32(c, Op::OP_ROTATE_RIGHT, value, amount);
        if (wantCarry)
            carryOut = truncTo(c, b32(c, Op::OP_SHIFT_RIGHT_LOGICAL,
                                    result.m_value, 31), 1);
        break;
    }
    if (wantCarry)
        result.m_carry = c.emit(Op::OP_SELECT, bitType(), isZero, carry,
                                carryOut);
    return result;
}

// Return the ShiftKind of a decoded shift
ShiftKind getShiftKind(uint shift)
{
    switch (shift)
    {
    case ArmDecodedOperand::SHIFT_LSR: return SHIFT_LSR;
    case ArmDecodedOperand::SHIFT_ASR: return SHIFT_ASR;
    case ArmDecodedOperand::SHIFT_ROR: return SHIFT_ROR;
    case ArmDecodedOperand::SHIFT_RRX: return SHIFT_RRX;
    default: return SHIFT_LSL;
    }
}

// Return true if the shifted register operand shifts by a register (the
// shift type with a zero amount: the immediate shifts have an amount, or none)
bool isRegisterShift(const ArmDecodedOperand& operand)
{
    return (operand.m_kind == ArmDecodedOperand::KIND_SHIFTED_REGISTER) &&
           (operand.m_shiftAmount == 0) &&
           ((operand.m_shift == ArmDecodedOperand::SHIFT_LSL) ||
            (operand.m_shift == ArmDecodedOperand::SHIFT_LSR) ||
            (operand.m_shift == ArmDecodedOperand::SHIFT_ASR) ||
            (operand.m_shift == ArmDecodedOperand::SHIFT_ROR));
}

/*
 * Evaluate the second operand of a data processing instruction: the value
 * and, when wanted, the carry out of the shifter
 */
Shifted evaluateShifter(ArmLiftContext& c, uint index, bool wantCarry)
{
    const ArmDecodedOperand& operand = c.getOperand(index);
    Shifted result;
    SemanticOperand cin;
    if (wantCarry || (operand.m_shift == ArmDecodedOperand::SHIFT_RRX))
        cin = carryIn(c);

    if (operand.m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
    {
        result.m_value = k32(c, getImmediateValue(operand));
        if (wantCarry)
        {
            if (isRotatedImmediate(c))
                result.m_carry = c.getImmediate(
                    (int64)((getImmediateValue(operand) >> 31) & 1), bitType());
            else
                result.m_carry = cin;
        }
        return result;
    }
    if (operand.m_kind != ArmDecodedOperand::KIND_SHIFTED_REGISTER)
    {
        // A register (or a fixed operand)
        result.m_value = rd(c, index);
        result.m_carry = cin;
        return result;
    }

    // (the A64 extends are not an AArch32 operand: the context applies them)
    if (operand.m_shift >= ArmDecodedOperand::EXTEND_UXTB)
    {
        result.m_value = rd(c, index);
        result.m_carry = cin;
        return result;
    }
    SemanticOperand value = getRegisterValue(c, operand.m_register);
    if (isRegisterShift(operand))
        return shiftByRegister(c, value, getShiftKind(operand.m_shift),
                               getRegisterValue(c, operand.m_shiftRegister),
                               wantCarry, cin);
    if (operand.m_shift == ArmDecodedOperand::SHIFT_NONE)
    {
        result.m_value = value;
        result.m_carry = cin;
        return result;
    }
    if (operand.m_shift == ArmDecodedOperand::SHIFT_RRX)
        return shiftByConstant(c, value, SHIFT_RRX, 1, wantCarry, cin);
    return shiftByConstant(c, value, getShiftKind(operand.m_shift),
                           operand.m_shiftAmount, wantCarry, cin);
}

// Set N and Z from the result (and C from the carry out of the shifter if
// 'carry' is valid)
void setLogicFlags(ArmLiftContext& c, const SemanticOperand& result,
                   const SemanticOperand& carry)
{
    c.setFlags(Op::FLAGS_LOGIC, SemanticFlags::FLAG_SIGN |
               SemanticFlags::FLAG_ZERO, word(), result, result, result);
    if (carry.isValid())
        c.move(c.getRegister(ARM_REG_C), carry);
}

// The NZCV flags
const uint32 FLAGS_NZCV = SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO |
                          SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW;

} // namespace

/////////////////////////////////////////////////////////////////////////////
// Data processing
/////////////////////////////////////////////////////////////////////////////

namespace armdis {
namespace a32 {

/*
 * and, eor, sub, rsb, add, adc, sbc, rsc, orr, bic, orn, mov, mvn, tst, teq,
 * cmp, cmn (and, in the unified syntax of T16, movs and the two operand
 * forms where the destination is the first source; addw / subw have no flags)
 */
void liftDataProcessing(ArmLiftContext& c)
{
    const bool isCompare = isMnemonic(c, "tst") || isMnemonic(c, "teq") ||
                           isMnemonic(c, "cmp") || isMnemonic(c, "cmn");
    const bool isMove = isMnemonic(c, "mov") || isMnemonic(c, "movs") ||
                        isMnemonic(c, "mvn");
    const bool isPlain = isMnemonic(c, "addw") || isMnemonic(c, "subw");
    const bool isSetFlags = isCompare || isMnemonic(c, "movs") ||
                            c.getInstruction().isSettingFlags();

    int destination = 0, first = 0, second = 1;
    if (isCompare)
    {
        destination = -1;
        first = 0;
        second = 1;
    } else if (isMove)
    {
        first = -1;
        second = 1;
    } else if (c.getOperandsCount() >= 3)
    {
        first = 1;
        second = 2;
    }

    // Writing the pc with the flags is an exception return (SPSR): not modeled
    if (isSetFlags && !isCompare && (destination >= 0) &&
        (registerOf(c, destination) == ARM_REG_PC))
    {
        undefined(c);
        return;
    }

    // The name without the "s" suffix of the T16 "movs"
    const bool isLogic = isMnemonic(c, "and") || isMnemonic(c, "eor") ||
                         isMnemonic(c, "orr") || isMnemonic(c, "bic") ||
                         isMnemonic(c, "orn") || isMnemonic(c, "mov") ||
                         isMnemonic(c, "movs") || isMnemonic(c, "mvn") ||
                         isMnemonic(c, "tst") || isMnemonic(c, "teq");
    const Shifted shifter = evaluateShifter(c, second, isSetFlags && isLogic);
    SemanticOperand a;
    if (first >= 0)
    {
        // addw / subw / adr with the pc: the word aligned pc
        if (isPlain && (registerOf(c, first) == ARM_REG_PC))
            a = k32(c, (c.getInstruction().getAddress() + 4) & ~(uint64)3);
        else
            a = rd(c, first);
    }
    const SemanticOperand b = shifter.m_value;

    SemanticOperand result;
    // For the arithmetic: the operands of the flags (a + b' + carry)
    SemanticOperand flagsA, flagsB;
    bool isArithmetic = false;
    if (isMnemonic(c, "and") || isMnemonic(c, "tst"))
        result = b32(c, Op::OP_AND, a, b);
    else if (isMnemonic(c, "eor") || isMnemonic(c, "teq"))
        result = b32(c, Op::OP_XOR, a, b);
    else if (isMnemonic(c, "orr"))
        result = b32(c, Op::OP_OR, a, b);
    else if (isMnemonic(c, "bic"))
        result = c.emit(Op::OP_AND_NOT, word(), b, a);
    else if (isMnemonic(c, "orn"))
        result = b32(c, Op::OP_OR, a, b32(c, Op::OP_XOR, b, 0xFFFFFFFFULL));
    else if (isMnemonic(c, "mov") || isMnemonic(c, "movs"))
        result = b;
    else if (isMnemonic(c, "mvn"))
        result = b32(c, Op::OP_XOR, b, 0xFFFFFFFFULL);
    else
    {
        isArithmetic = true;
        const bool isReverse = isMnemonic(c, "rsb") || isMnemonic(c, "rsc");
        const bool isSubtract = isMnemonic(c, "sub") || isMnemonic(c, "sbc") ||
                                isMnemonic(c, "rsb") || isMnemonic(c, "rsc") ||
                                isMnemonic(c, "cmp") || isMnemonic(c, "subw");
        const bool isCarry = isMnemonic(c, "adc") || isMnemonic(c, "sbc") ||
                             isMnemonic(c, "rsc");
        SemanticOperand x = isReverse ? b : a;
        SemanticOperand y = isReverse ? a : b;
        // x + y (add), x + ~y + 1 (sub), x + y + c (adc), x + ~y + c (sbc)
        flagsA = x;
        flagsB = isSubtract ? b32(c, Op::OP_XOR, y, 0xFFFFFFFFULL) : y;
        if (isCarry)
            result = c.emit(Op::OP_ADD_CARRY, word(), flagsA, flagsB,
                            carryIn(c));
        else if (isSubtract)
            result = b32(c, Op::OP_SUBTRACT, x, y);
        else
            result = b32(c, Op::OP_ADD, x, y);
    }

    if (isSetFlags && !isPlain)
    {
        if (isArithmetic)
            c.setFlags(Op::FLAGS_ADD, FLAGS_NZCV, word(), flagsA, flagsB, result);
        else
            setLogicFlags(c, result, shifter.m_carry);
    }
    if (destination >= 0)
        c.write(destination, result);
}

/*
 * lsl, lsr, asr, ror (immediate and register amount) and rrx: Rd = shift(Rm),
 * the T16 two operand forms are Rdn = shift(Rdn, Rm)
 */
void liftShift(ArmLiftContext& c)
{
    const bool isSetFlags = c.getInstruction().isSettingFlags();
    // Writing the pc with the flags is an exception return (SPSR): not modeled
    if (isSetFlags && (registerOf(c, 0) == ARM_REG_PC))
    {
        undefined(c);
        return;
    }
    const uint count = c.getOperandsCount();
    const bool isRrx = isMnemonic(c, "rrx");
    const uint source = (count >= 3) ? 1 : 0;
    ShiftKind kind = SHIFT_LSL;
    if (isMnemonic(c, "lsr"))
        kind = SHIFT_LSR;
    else if (isMnemonic(c, "asr"))
        kind = SHIFT_ASR;
    else if (isMnemonic(c, "ror"))
        kind = SHIFT_ROR;
    else if (isRrx)
        kind = SHIFT_RRX;

    SemanticOperand value = rd(c, isRrx ? 1 : source);
    SemanticOperand cin = carryIn(c);
    Shifted shifted;
    if (isRrx)
    {
        shifted = shiftByConstant(c, value, SHIFT_RRX, 1, isSetFlags, cin);
    } else
    {
        const uint amountIndex = (count >= 3) ? 2 : 1;
        const ArmDecodedOperand& amount = c.getOperand(amountIndex);
        if (amount.m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
        {
            if (amount.m_immediate == 0)
            {
                shifted.m_value = value;
                shifted.m_carry = cin;
            } else
            {
                shifted = shiftByConstant(c, value, kind, (uint)amount.m_immediate,
                                          isSetFlags, cin);
            }
        } else
        {
            shifted = shiftByRegister(c, value, kind, rd(c, amountIndex),
                                      isSetFlags, cin);
        }
    }
    if (isSetFlags)
        setLogicFlags(c, shifted.m_value, shifted.m_carry);
    c.write(0, shifted.m_value);
}

// adr (T16, T32): Rd = Align(pc, 4) +/- the immediate
void liftAdr(ArmLiftContext& c)
{
    const uint64 base = (c.getInstruction().getAddress() + 4) & ~(uint64)3;
    c.write(0, k32(c, base + (uint64)c.getOperand(1).m_immediate));
}

// movw / movt
void liftMoveWide(ArmLiftContext& c)
{
    const uint64 value = (uint64)c.getOperand(1).m_immediate & 0xFFFF;
    if (isMnemonic(c, "movw"))
    {
        c.write(0, k32(c, value));
        return;
    }
    SemanticOperand low = b32(c, Op::OP_AND, rd(c, 0), 0xFFFF);
    c.write(0, b32(c, Op::OP_OR, low, value << 16));
}

/////////////////////////////////////////////////////////////////////////////
// Multiplies
/////////////////////////////////////////////////////////////////////////////

// mul, mla, mls
void liftMultiply(ArmLiftContext& c)
{
    SemanticOperand product = b32(c, Op::OP_MULTIPLY, rd(c, 1), rd(c, 2));
    SemanticOperand result = product;
    if (isMnemonic(c, "mla"))
        result = b32(c, Op::OP_ADD, product, rd(c, 3));
    else if (isMnemonic(c, "mls"))
        result = b32(c, Op::OP_SUBTRACT, rd(c, 3), product);
    // mul in the T16 form has the destination as one of the sources: the
    // operands are Rd, Rn, Rm in all the encodings
    if (c.getInstruction().isSettingFlags())
        c.setFlags(Op::FLAGS_LOGIC, SemanticFlags::FLAG_SIGN |
                   SemanticFlags::FLAG_ZERO, word(), result, result, result);
    c.write(0, result);
}

// Write the 64 bit value to RdLo (operand 0) and RdHi (operand 1)
void writeLongResult(ArmLiftContext& c, const SemanticOperand& value,
                     bool isSetFlags)
{
    SemanticOperand low = truncTo(c, value, 32);
    SemanticOperand high = truncTo(c, c.emit(Op::OP_SHIFT_RIGHT_LOGICAL,
                                           doubleWord(), value, k64(c, 32)), 32);
    if (isSetFlags)
        c.setFlags(Op::FLAGS_LOGIC, SemanticFlags::FLAG_SIGN |
                   SemanticFlags::FLAG_ZERO, doubleWord(), value, value, value);
    c.write(0, low);
    c.write(1, high);
}

// Return RdHi:RdLo (operands 0 and 1) as a 64 bit value
SemanticOperand readLongAccumulator(ArmLiftContext& c)
{
    SemanticOperand low = zext(c, rd(c, 0), 64);
    SemanticOperand high = c.emit(Op::OP_SHIFT_LEFT, doubleWord(),
                                  zext(c, rd(c, 1), 64), k64(c, 32));
    return c.emit(Op::OP_OR, doubleWord(), high, low);
}

// umull, smull, umlal, smlal, umaal
void liftLongMultiply(ArmLiftContext& c)
{
    const bool isSigned = isMnemonic(c, "smull") || isMnemonic(c, "smlal");
    SemanticOperand a = isSigned ? sext(c, rd(c, 2), 64) : zext(c, rd(c, 2), 64);
    SemanticOperand b = isSigned ? sext(c, rd(c, 3), 64) : zext(c, rd(c, 3), 64);
    SemanticOperand product = c.emit(Op::OP_MULTIPLY, doubleWord(), a, b);
    SemanticOperand result = product;
    if (isMnemonic(c, "umlal") || isMnemonic(c, "smlal"))
        result = c.emit(Op::OP_ADD, doubleWord(), product,
                        readLongAccumulator(c));
    else if (isMnemonic(c, "umaal"))
        result = c.emit(Op::OP_ADD, doubleWord(),
                        c.emit(Op::OP_ADD, doubleWord(), product,
                               zext(c, rd(c, 0), 64)), zext(c, rd(c, 1), 64));
    writeLongResult(c, result, c.getInstruction().isSettingFlags());
}

// Q |= (the 64 bit 'wide' is not the sign extension of its low 32 bits)
void setQOnOverflow(ArmLiftContext& c, const SemanticOperand& wide)
{
    SemanticOperand narrow = sext(c, truncTo(c, wide, 32), 64);
    accumulateSaturation(c, compare(c, Op::COND_NOT_EQUAL, doubleWord(), wide,
                                    narrow));
}

// Return the signed halfword of 'value': the top one if 'isTop'
SemanticOperand halfword(ArmLiftContext& c, const SemanticOperand& value,
                         bool isTop)
{
    return sext(c, field(c, value, isTop ? 16 : 0, 16), 32);
}

// smulxy, smlaxy, smulwy, smlawy, smlalxy
void liftHalfwordMultiply(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    // smul<x><y>, smla<x><y>, smlal<x><y>, smulw<y>, smlaw<y>
    const bool isLong = strncmp(name, "smlal", 5) == 0;
    const bool isWord = (strncmp(name, "smulw", 5) == 0) ||
                        (strncmp(name, "smlaw", 5) == 0);
    const bool isAccumulate = strncmp(name, "smla", 4) == 0;
    const size_t length = strlen(name);
    const bool yTop = name[length - 1] == 't';
    const bool xTop = (!isWord) && (name[length - 2] == 't');

    if (isLong)
    {
        // smlal<x><y> RdLo, RdHi, Rn, Rm
        SemanticOperand product = c.emit(Op::OP_MULTIPLY, doubleWord(),
                sext(c, halfword(c, rd(c, 2), xTop), 64),
                sext(c, halfword(c, rd(c, 3), yTop), 64));
        SemanticOperand result = c.emit(Op::OP_ADD, doubleWord(), product,
                                        readLongAccumulator(c));
        writeLongResult(c, result, false);
        return;
    }
    SemanticOperand y = halfword(c, rd(c, 2), yTop);
    SemanticOperand product;
    if (isWord)
    {
        // (Rn * halfword) >> 16, 48 bits of the 64 bit product
        SemanticOperand wide = c.emit(Op::OP_MULTIPLY, doubleWord(),
                                      sext(c, rd(c, 1), 64), sext(c, y, 64));
        product = truncTo(c, c.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, doubleWord(),
                                  wide, k64(c, 16)), 32);
    } else
    {
        product = b32(c, Op::OP_MULTIPLY, halfword(c, rd(c, 1), xTop), y);
    }
    if (!isAccumulate)
    {
        c.write(0, product);
        return;
    }
    SemanticOperand wideSum = c.emit(Op::OP_ADD, doubleWord(),
                                     sext(c, product, 64), sext(c, rd(c, 3), 64));
    setQOnOverflow(c, wideSum);
    c.write(0, truncTo(c, wideSum, 32));
}

// smuad, smusd, smlad, smlsd (and the exchanging x forms), smlald, smlsld
void liftDualMultiply(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    const size_t length = strlen(name);
    const bool isExchange = name[length - 1] == 'x';
    const bool isLong = (strncmp(name, "smlald", 6) == 0) ||
                        (strncmp(name, "smlsld", 6) == 0);
    const bool isSubtract = (strncmp(name, "smusd", 5) == 0) ||
                            (strncmp(name, "smlsd", 5) == 0) ||
                            (strncmp(name, "smlsld", 6) == 0);
    const bool isAccumulate = strncmp(name, "smla", 4) == 0 ||
                              strncmp(name, "smls", 4) == 0;
    const uint n = isLong ? 2 : 1;
    const uint m = isLong ? 3 : 2;
    SemanticOperand second = rd(c, m);
    if (isExchange)
        second = b32(c, Op::OP_ROTATE_RIGHT, second, 16);
    SemanticOperand first = rd(c, n);
    SemanticOperand p1 = c.emit(Op::OP_MULTIPLY, doubleWord(),
                                sext(c, halfword(c, first, false), 64),
                                sext(c, halfword(c, second, false), 64));
    SemanticOperand p2 = c.emit(Op::OP_MULTIPLY, doubleWord(),
                                sext(c, halfword(c, first, true), 64),
                                sext(c, halfword(c, second, true), 64));
    SemanticOperand sum = c.emit(isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD,
                                 doubleWord(), p1, p2);
    if (isLong)
    {
        writeLongResult(c, c.emit(Op::OP_ADD, doubleWord(), sum,
                                  readLongAccumulator(c)), false);
        return;
    }
    if (isAccumulate)
        sum = c.emit(Op::OP_ADD, doubleWord(), sum, sext(c, rd(c, 3), 64));
    // smusd cannot overflow; the others set Q when the sum doesn't fit
    if (!isMnemonic(c, "smusd") && !isMnemonic(c, "smusdx"))
        setQOnOverflow(c, sum);
    c.write(0, truncTo(c, sum, 32));
}

// smmul, smmla, smmls (and the rounding r forms)
void liftMostSignificantMultiply(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    const size_t length = strlen(name);
    const bool isRound = name[length - 1] == 'r';
    SemanticOperand product = c.emit(Op::OP_MULTIPLY, doubleWord(),
                                     sext(c, rd(c, 1), 64), sext(c, rd(c, 2), 64));
    SemanticOperand value = product;
    if (strncmp(name, "smmla", 5) == 0 || strncmp(name, "smmls", 5) == 0)
    {
        SemanticOperand accumulator = c.emit(Op::OP_SHIFT_LEFT, doubleWord(),
                                             zext(c, rd(c, 3), 64), k64(c, 32));
        value = c.emit(strncmp(name, "smmla", 5) == 0 ? Op::OP_ADD :
                       Op::OP_SUBTRACT, doubleWord(), accumulator, product);
    }
    if (isRound)
        value = c.emit(Op::OP_ADD, doubleWord(), value, k64(c, 0x80000000ULL));
    c.write(0, truncTo(c, c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, doubleWord(), value,
                               k64(c, 32)), 32));
}

// usad8, usada8
void liftSumAbsoluteDifferences(ArmLiftContext& c)
{
    SemanticOperand a = rd(c, 1);
    SemanticOperand b = rd(c, 2);
    SemanticOperand sum;
    for (uint i = 0; i < 4; i++)
    {
        SemanticOperand difference = c.emit(Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED,
                SemanticType::integer(8), field(c, a, 8 * i, 8),
                field(c, b, 8 * i, 8));
        SemanticOperand wide = zext(c, difference, 32);
        sum = sum.isValid() ? b32(c, Op::OP_ADD, sum, wide) : wide;
    }
    if (isMnemonic(c, "usada8"))
        sum = b32(c, Op::OP_ADD, sum, rd(c, 3));
    c.write(0, sum);
}

// sdiv, udiv: a division by zero gives 0
void liftDivide(ArmLiftContext& c)
{
    SemanticOperand result = c.newTemporary(word());
    SemanticInstruction instruction(isMnemonic(c, "sdiv") ?
                                        Op::OP_DIVIDE_SIGNED : Op::OP_DIVIDE_UNSIGNED,
                                    word(), result, rd(c, 1), rd(c, 2));
    instruction.setBehavior(Op::BEHAVIOR_DIVIDE_ZERO);
    c.emit(instruction);
    c.write(0, result);
}

/////////////////////////////////////////////////////////////////////////////
// Saturating arithmetic
/////////////////////////////////////////////////////////////////////////////

// qadd, qsub, qdadd, qdsub: Rd = sat(Rm op sat(2 * Rn)); Q is set on any
// saturation
void liftSaturatingAddSub(ArmLiftContext& c)
{
    SemanticOperand a = rd(c, 1);
    SemanticOperand b = rd(c, 2);
    const bool isDouble = isMnemonic(c, "qdadd") || isMnemonic(c, "qdsub");
    const bool isSubtract = isMnemonic(c, "qsub") || isMnemonic(c, "qdsub");
    if (isDouble)
    {
        SemanticOperand doubled = c.emit(Op::OP_ADD_SATURATE_SIGNED, word(), b, b);
        accumulateSaturation(c, compare(c, Op::COND_NOT_EQUAL, word(), doubled,
                                        b32(c, Op::OP_ADD, b, b)));
        b = doubled;
    }
    SemanticOperand saturated = c.emit(isSubtract ?
                Op::OP_SUBTRACT_SATURATE_SIGNED : Op::OP_ADD_SATURATE_SIGNED,
                word(), a, b);
    SemanticOperand wrapped = b32(c, isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD,
                                  a, b);
    accumulateSaturation(c, compare(c, Op::COND_NOT_EQUAL, word(), saturated,
                                    wrapped));
    c.write(0, saturated);
}

// Saturate the 32 bit signed 'value' to [minimum, maximum] (signed
// constants), and return the result; 'isSaturated' is the 1 bit saturation
SemanticOperand saturate(ArmLiftContext& c, const SemanticOperand& value,
                         int64 minimum, int64 maximum,
                         SemanticOperand& isSaturated)
{
    SemanticOperand clamped = b32(c, Op::OP_MINIMUM_SIGNED,
            b32(c, Op::OP_MAXIMUM_SIGNED, value, (uint64)minimum), (uint64)maximum);
    isSaturated = compare(c, Op::COND_NOT_EQUAL, word(), clamped, value);
    return clamped;
}

// ssat, usat: Rd = saturate(shift(Rn)) to a signed / unsigned range
void liftSaturate(ArmLiftContext& c)
{
    const bool isSigned = isMnemonic(c, "ssat");
    const uint bits = (uint)c.getOperand(1).m_immediate;
    SemanticOperand value = rd(c, 2);
    SemanticOperand isSaturated;
    SemanticOperand result;
    if (isSigned)
    {
        const int64 maximum = (int64)(((uint64)1 << (bits - 1)) - 1);
        result = saturate(c, value, -maximum - 1, maximum, isSaturated);
    } else
    {
        const int64 maximum = (int64)(((uint64)1 << bits) - 1);
        result = saturate(c, value, 0, maximum, isSaturated);
    }
    accumulateSaturation(c, isSaturated);
    c.write(0, result);
}

// ssat16, usat16: the two halfwords
void liftSaturate16(ArmLiftContext& c)
{
    const bool isSigned = isMnemonic(c, "ssat16");
    const uint bits = (uint)c.getOperand(1).m_immediate;
    SemanticOperand value = rd(c, 2);
    SemanticOperand result;
    for (uint half = 0; half < 2; half++)
    {
        SemanticOperand lane = halfword(c, value, half != 0);
        SemanticOperand isSaturated;
        SemanticOperand clamped;
        if (isSigned)
        {
            const int64 maximum = (int64)(((uint64)1 << (bits - 1)) - 1);
            clamped = saturate(c, lane, -maximum - 1, maximum, isSaturated);
        } else
        {
            clamped = saturate(c, lane, 0, (int64)(((uint64)1 << bits) - 1),
                               isSaturated);
        }
        accumulateSaturation(c, isSaturated);
        SemanticOperand part = b32(c, Op::OP_AND, clamped, 0xFFFF);
        if (half != 0)
            part = b32(c, Op::OP_SHIFT_LEFT, part, 16);
        result = result.isValid() ? b32(c, Op::OP_OR, result, part) : part;
    }
    c.write(0, result);
}

/*
 * The parallel add and subtract: [s q sh u uq uh] x [add16 add8 sub16 sub8
 * asx sax]. The lanes are computed in 32 bits; s and u set the GE flags,
 * q / uq saturate, sh / uh halve
 */
void liftParallel(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    bool isUnsigned = false, isSaturating = false, isHalving = false;
    const char* operation = name;
    if (strncmp(name, "uq", 2) == 0)
    {
        isUnsigned = isSaturating = true;
        operation += 2;
    } else if (strncmp(name, "uh", 2) == 0)
    {
        isUnsigned = isHalving = true;
        operation += 2;
    } else if (strncmp(name, "sh", 2) == 0)
    {
        isHalving = true;
        operation += 2;
    } else if (name[0] == 'q')
    {
        isSaturating = true;
        operation += 1;
    } else if (name[0] == 'u')
    {
        isUnsigned = true;
        operation += 1;
    } else
    {
        operation += 1;
    }
    const bool is8 = strstr(operation, "8") != NULL;
    const uint width = is8 ? 8 : 16;
    const uint lanes = is8 ? 4 : 2;
    // Per lane: the lane of Rn, the lane of Rm and the operation
    bool isAdd[4];
    uint secondLane[4];
    for (uint i = 0; i < lanes; i++)
    {
        secondLane[i] = i;
        isAdd[i] = strncmp(operation, "add", 3) == 0;
    }
    if (strcmp(operation, "asx") == 0)
    {
        // Rd[15:0] = Rn[15:0] - Rm[31:16], Rd[31:16] = Rn[31:16] + Rm[15:0]
        secondLane[0] = 1;
        secondLane[1] = 0;
        isAdd[0] = false;
        isAdd[1] = true;
    } else if (strcmp(operation, "sax") == 0)
    {
        // Rd[15:0] = Rn[15:0] + Rm[31:16], Rd[31:16] = Rn[31:16] - Rm[15:0]
        secondLane[0] = 1;
        secondLane[1] = 0;
        isAdd[0] = true;
        isAdd[1] = false;
    }

    SemanticOperand a = rd(c, 1);
    SemanticOperand b = rd(c, 2);
    SemanticOperand result;
    SemanticOperand ge[4];
    for (uint i = 0; i < lanes; i++)
    {
        SemanticOperand x = field(c, a, width * i, width);
        SemanticOperand y = field(c, b, width * secondLane[i], width);
        x = isUnsigned ? zext(c, x, 32) : sext(c, x, 32);
        y = isUnsigned ? zext(c, y, 32) : sext(c, y, 32);
        SemanticOperand exact = b32(c, isAdd[i] ? Op::OP_ADD : Op::OP_SUBTRACT, x, y);
        SemanticOperand lane;
        if (isSaturating)
        {
            const int64 maximum = isUnsigned ? (int64)(((uint64)1 << width) - 1) :
                                  (int64)(((uint64)1 << (width - 1)) - 1);
            const int64 minimum = isUnsigned ? 0 : -maximum - 1;
            SemanticOperand ignored;
            lane = saturate(c, exact, minimum, maximum, ignored);
        } else if (isHalving)
        {
            lane = b32(c, Op::OP_SHIFT_RIGHT_ARITHMETIC, exact, 1);
        } else
        {
            lane = exact;
            // GE: the signed result is not negative; the unsigned sum carries
            // out, the unsigned difference doesn't borrow
            if (isUnsigned && isAdd[i])
                ge[i] = truncTo(c, b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, exact, width), 1);
            else
                ge[i] = compare(c, Op::COND_SIGNED_GREATER_EQUAL, word(), exact,
                                k32(c, 0));
        }
        SemanticOperand part = b32(c, Op::OP_AND, lane, (width == 8) ? 0xFF : 0xFFFF);
        if (i != 0)
            part = b32(c, Op::OP_SHIFT_LEFT, part, width * i);
        result = result.isValid() ? b32(c, Op::OP_OR, result, part) : part;
    }
    if (!isSaturating && !isHalving)
    {
        for (uint i = 0; i < 4; i++)
            c.move(c.getRegister(ARM_REG_GE0 + i), ge[is8 ? i : (i / 2)]);
    }
    c.write(0, result);
}

// sel: each byte of Rn if its GE flag is set, otherwise of Rm
void liftSelect(ArmLiftContext& c)
{
    SemanticOperand a = rd(c, 1);
    SemanticOperand b = rd(c, 2);
    SemanticOperand result;
    for (uint i = 0; i < 4; i++)
    {
        SemanticOperand lane = c.emit(Op::OP_SELECT, SemanticType::integer(8),
                                      c.getRegister(ARM_REG_GE0 + i),
                                      field(c, a, 8 * i, 8), field(c, b, 8 * i, 8));
        SemanticOperand part = zext(c, lane, 32);
        if (i != 0)
            part = b32(c, Op::OP_SHIFT_LEFT, part, 8 * i);
        result = result.isValid() ? b32(c, Op::OP_OR, result, part) : part;
    }
    c.write(0, result);
}

/////////////////////////////////////////////////////////////////////////////
// Extends, packs, bit fields and bit operations
/////////////////////////////////////////////////////////////////////////////

// sxtb, sxth, uxtb, uxth, sxtb16, uxtb16 and the adding sxtab, sxtah, uxtab,
// uxtah, sxtab16, uxtab16: the operand is rotated (read() applies the ror)
void liftExtend(ArmLiftContext& c)
{
    const BaseName base(c);
    const char* name = base.m_text;
    const bool isSigned = name[0] == 's';
    const bool isAdd = strncmp(name + 1, "xta", 3) == 0;
    const bool is16Pair = strstr(name, "16") != NULL;
    const bool isHalf = (!is16Pair) && (name[strlen(name) - 1] == 'h');
    const uint operand = isAdd ? 2 : 1;
    SemanticOperand rotated = rd(c, operand);
    SemanticOperand result;
    if (is16Pair)
    {
        for (uint half = 0; half < 2; half++)
        {
            SemanticOperand lane = field(c, rotated, 16 * half, 8);
            SemanticOperand extended = isSigned ? sext(c, lane, 16) : zext(c, lane, 16);
            if (isAdd)
                extended = c.emit(Op::OP_ADD, SemanticType::integer(16), extended,
                                  field(c, rd(c, 1), 16 * half, 16));
            SemanticOperand part = zext(c, extended, 32);
            if (half != 0)
                part = b32(c, Op::OP_SHIFT_LEFT, part, 16);
            result = result.isValid() ? b32(c, Op::OP_OR, result, part) : part;
        }
        c.write(0, result);
        return;
    }
    const uint bits = isHalf ? 16 : 8;
    SemanticOperand lane = field(c, rotated, 0, bits);
    result = isSigned ? sext(c, lane, 32) : zext(c, lane, 32);
    if (isAdd)
        result = b32(c, Op::OP_ADD, rd(c, 1), result);
    c.write(0, result);
}

// pkhbt Rd, Rn, Rm{, lsl #n}; pkhtb Rd, Rn, Rm{, asr #n}
void liftPack(ArmLiftContext& c)
{
    SemanticOperand n = rd(c, 1);
    SemanticOperand m = rd(c, 2);
    if (isMnemonic(c, "pkhbt"))
    {
        c.write(0, b32(c, Op::OP_OR, b32(c, Op::OP_AND, n, 0xFFFF),
                       b32(c, Op::OP_AND, m, 0xFFFF0000ULL)));
        return;
    }
    c.write(0, b32(c, Op::OP_OR, b32(c, Op::OP_AND, n, 0xFFFF0000ULL),
                   b32(c, Op::OP_AND, m, 0xFFFF)));
}

// bfc Rd, #lsb, #width; bfi Rd, Rn, #lsb, #width
void liftBitFieldInsert(ArmLiftContext& c)
{
    const bool isClear = isMnemonic(c, "bfc");
    const uint lsbIndex = isClear ? 1 : 2;
    const uint lsb = (uint)c.getOperand(lsbIndex).m_immediate;
    const uint width = (uint)c.getOperand(lsbIndex + 1).m_immediate;
    const uint64 mask = ((width >= 32) ? 0xFFFFFFFFULL :
                         (((uint64)1 << width) - 1)) << lsb;
    SemanticOperand kept = b32(c, Op::OP_AND, rd(c, 0), ~mask & 0xFFFFFFFFULL);
    if (isClear)
    {
        c.write(0, kept);
        return;
    }
    SemanticOperand inserted = b32(c, Op::OP_AND,
                                   b32(c, Op::OP_SHIFT_LEFT, rd(c, 1), lsb), mask);
    c.write(0, b32(c, Op::OP_OR, kept, inserted));
}

// ubfx, sbfx Rd, Rn, #lsb, #width
void liftBitFieldExtract(ArmLiftContext& c)
{
    const uint lsb = (uint)c.getOperand(2).m_immediate;
    const uint width = (uint)c.getOperand(3).m_immediate;
    SemanticOperand value = rd(c, 1);
    if (isMnemonic(c, "ubfx"))
    {
        SemanticOperand shifted = b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value, lsb);
        const uint64 mask = (width >= 32) ? 0xFFFFFFFFULL :
                            (((uint64)1 << width) - 1);
        c.write(0, b32(c, Op::OP_AND, shifted, mask));
        return;
    }
    SemanticOperand up = b32(c, Op::OP_SHIFT_LEFT, value, 32 - lsb - width);
    c.write(0, b32(c, Op::OP_SHIFT_RIGHT_ARITHMETIC, up, 32 - width));
}

// rev, rev16, revsh, rbit, clz
void liftReverse(ArmLiftContext& c)
{
    SemanticOperand value = rd(c, 1);
    SemanticOperand result;
    if (isMnemonic(c, "rev"))
    {
        result = c.emit(Op::OP_BYTE_SWAP, word(), value);
    } else if (isMnemonic(c, "rbit"))
    {
        result = c.emit(Op::OP_BIT_REVERSE, word(), value);
    } else if (isMnemonic(c, "clz"))
    {
        result = c.emit(Op::OP_COUNT_LEADING_ZEROS, word(), value);
    } else if (isMnemonic(c, "rev16"))
    {
        result = b32(c, Op::OP_OR,
                     b32(c, Op::OP_SHIFT_LEFT, b32(c, Op::OP_AND, value,
                                                    0x00FF00FFULL), 8),
                     b32(c, Op::OP_AND, b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value,
                                            8), 0x00FF00FFULL));
    } else
    {
        // revsh: the byte swapped low halfword, sign extended
        SemanticOperand swapped = b32(c, Op::OP_OR,
                b32(c, Op::OP_SHIFT_LEFT, b32(c, Op::OP_AND, value, 0xFF), 8),
                b32(c, Op::OP_AND, b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value, 8),
                    0xFF));
        result = sext(c, truncTo(c, swapped, 16), 32);
    }
    c.write(0, result);
}

// crc32{b,h,w}, crc32c{b,h,w} Rd, Rn, Rm: the accumulator is Rn
void liftCrc32(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    const size_t length = strlen(name);
    const bool isC = name[5] == 'c';
    const uint bits = (name[length - 1] == 'b') ? 8 :
                      (name[length - 1] == 'h') ? 16 : 32;
    SemanticOperand data = rd(c, 2);
    if (bits < 32)
        data = truncTo(c, data, bits);
    SemanticOperand result = c.newTemporary(word());
    SemanticInstruction crc(Op::OP_CRC32, word(), result, data, rd(c, 1));
    crc.setBehavior(isC ? Op::BEHAVIOR_CRC32C : Op::BEHAVIOR_CRC32_IEEE);
    c.emit(crc);
    c.write(0, result);
}

/////////////////////////////////////////////////////////////////////////////
// Branches
/////////////////////////////////////////////////////////////////////////////

namespace {

// The link register value of a call: the address of the next instruction,
// with bit 0 set in T32 (the return interworks back)
SemanticOperand getReturnAddress(ArmLiftContext& c)
{
    const uint64 next = c.getInstruction().getAddress() +
                        c.getInstruction().getOpcodeSize();
    return k32(c, next | (isThumb(c) ? 1 : 0));
}

SemanticOperand target(ArmLiftContext& c)
{
    return SemanticOperand::guestAddress(
        (uint64)c.getOperand(0).m_immediate & 0xFFFFFFFFULL, word());
}

} // namespace

// b
void liftBranch(ArmLiftContext& c)
{
    c.branch(target(c));
}

// bl
void liftBranchLink(ArmLiftContext& c)
{
    c.move(c.getRegister(ARM_REG_LR), getReturnAddress(c));
    c.call(target(c));
}

// blx label: calls and switches the instruction set
void liftBranchLinkExchangeImmediate(ArmLiftContext& c)
{
    c.move(c.getRegister(ARM_REG_LR), getReturnAddress(c));
    c.move(c.getRegister(ARM_REG_T),
           c.getImmediate(isThumb(c) ? 0 : 1, bitType()));
    c.call(target(c));
}

// bx Rm, bxj Rm (no Jazelle): interworking branch
void liftBranchExchange(ArmLiftContext& c)
{
    c.branchWritePc(rd(c, 0), true);
}

// blx Rm: the target is read before the link register is written
void liftBranchLinkExchange(ArmLiftContext& c)
{
    const SemanticOperand destination = c.emit(Op::OP_MOVE, word(), rd(c, 0));
    c.move(c.getRegister(ARM_REG_LR), getReturnAddress(c));
    c.branchWritePc(destination, true);
}

// blx label or blx Rm
void liftBlx(ArmLiftContext& c)
{
    if (c.getOperand(0).m_kind == ArmDecodedOperand::KIND_BRANCH_TARGET)
        liftBranchLinkExchangeImmediate(c);
    else
        liftBranchLinkExchange(c);
}

// cbz, cbnz Rn, label
void liftCompareBranch(ArmLiftContext& c)
{
    SemanticOperand condition = compare(c, isMnemonic(c, "cbz") ?
            Op::COND_EQUAL : Op::COND_NOT_EQUAL, word(), rd(c, 0), k32(c, 0));
    c.branchIf(condition, SemanticOperand::guestAddress(
        (uint64)c.getOperand(1).m_immediate & 0xFFFFFFFFULL, word()));
}

/////////////////////////////////////////////////////////////////////////////
// Hints, barriers, exceptions and the APSR
/////////////////////////////////////////////////////////////////////////////

// nop and the hints without an effect (the it instruction is applied by the
// decoder to the instructions of its block)
void liftNop(ArmLiftContext& c)
{
    c.emit(SemanticInstruction(Op::OP_NOP, SemanticType(), SemanticOperand()));
}

// yield, wfe, wfi, sev, sevl
void liftHint(ArmLiftContext& c)
{
    SemanticInstruction instruction(Op::OP_HINT, SemanticType(),
                                    SemanticOperand());
    if (isMnemonic(c, "yield"))
        instruction.setKind(Op::HINT_SPIN);
    else if (isMnemonic(c, "wfe") || isMnemonic(c, "wfi"))
        instruction.setKind(Op::HINT_WAIT);
    else
        instruction.setKind(Op::HINT_SEND_EVENT);
    c.emit(instruction);
}

// dmb, dsb, isb, ssbb, pssbb, sb
void liftBarrier(ArmLiftContext& c)
{
    if (isMnemonic(c, "isb"))
    {
        c.emit(SemanticInstruction(Op::OP_FENCE_INSTRUCTION, SemanticType(),
                                   SemanticOperand()));
        return;
    }
    SemanticInstruction fence(Op::OP_FENCE, SemanticType(), SemanticOperand());
    fence.setMemoryOrder(Op::ORDER_SEQUENTIAL);
    fence.setFenceAccess(Op::FENCE_ALL);
    c.emit(fence);
}

// svc, bkpt, udf, hlt, trap
void liftException(ArmLiftContext& c)
{
    uint number = 0;
    if ((c.getOperandsCount() > 0) &&
        (c.getOperand(0).m_kind == ArmDecodedOperand::KIND_IMMEDIATE))
        number = (uint)c.getOperand(0).m_immediate;
    if (isMnemonic(c, "svc"))
        c.trap(Op::TRAP_SYSTEM_CALL, number);
    else if (isMnemonic(c, "bkpt"))
        c.trap(Op::TRAP_BREAKPOINT, number);
    else
        c.trap(Op::TRAP_UNDEFINED, number);
}

// mrc / mcr p15, #0, Rt, c13, c0, #2 and #3: the thread id registers of the user mode
// (TPIDRURW read / write, TPIDRURO read only). The other coprocessor registers are
// privileged or belong to the system: not modeled
void liftCoprocessorMove(ArmLiftContext& c)
{
    if (c.getOperandsCount() != 6)
    {
        undefined(c);
        return;
    }
    const uint coprocessor = c.getOperand(0).m_option;
    const int64 opcode1 = c.getOperand(1).m_immediate;
    const uint registerN = c.getOperand(3).m_option;
    const uint registerM = c.getOperand(4).m_option;
    const int64 opcode2 = c.getOperand(5).m_immediate;
    if ((coprocessor != 15) || (opcode1 != 0) || (registerN != 13) || (registerM != 0) ||
        ((opcode2 != 2) && (opcode2 != 3)) || (registerOf(c, 2) == ARM_REG_PC))
    {
        undefined(c);
        return;
    }
    const uint threadRegister = (opcode2 == 2) ? ARM_REG_TPIDRURW : ARM_REG_TPIDRURO;
    if (isMnemonic(c, "mrc"))
        c.write(2, c.getRegister(threadRegister));
    else if (opcode2 == 2)
        c.move(c.getRegister(ARM_REG_TPIDRURW), rd(c, 2));
    else
        c.trap(Op::TRAP_UNDEFINED, 0);
}

// Pack the APSR: N Z C V Q at bits 31-27 and GE3-GE0 at bits 19-16
static SemanticOperand packApsr(ArmLiftContext& c)
{
    static const struct { uint m_register; uint m_bit; } bits[] = {
        { ARM_REG_N, 31 }, { ARM_REG_Z, 30 }, { ARM_REG_C, 29 },
        { ARM_REG_V, 28 }, { ARM_REG_Q, 27 }, { ARM_REG_GE0, 16 },
        { ARM_REG_GE1, 17 }, { ARM_REG_GE2, 18 }, { ARM_REG_GE3, 19 }
    };
    SemanticOperand result;
    for (uint i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
    {
        SemanticOperand bit = zext(c, c.getRegister(bits[i].m_register), 32);
        bit = b32(c, Op::OP_SHIFT_LEFT, bit, bits[i].m_bit);
        result = result.isValid() ? b32(c, Op::OP_OR, result, bit) : bit;
    }
    return result;
}

// mrs Rd, apsr / cpsr
void liftReadApsr(ArmLiftContext& c)
{
    // The SPSR and the banked registers are not visible in user mode
    const ArmDecodedOperand& source = c.getOperand(1);
    if ((source.m_kind != ArmDecodedOperand::KIND_NAME) ||
        !(source.m_name == cString("apsr")))
    {
        undefined(c);
        return;
    }
    // The CPSR of the user mode: the mode field is 0b10000, the execution
    // state bits (T, IT) read as zero (the mask of the MRS in the Arm ARM)
    SemanticOperand endian = b32(c, Op::OP_SHIFT_LEFT,
                                 zext(c, c.getRegister(ARM_REG_E), 32), 9);
    c.write(0, b32(c, Op::OP_OR, b32(c, Op::OP_OR, packApsr(c), endian), 0x10));
}

// msr apsr_<fields>, Rn / #imm: the NZCVQ bits (mask bit 3) and the GE bits
// (mask bit 2); the control fields are privileged and ignored in user mode
void liftWriteApsr(ArmLiftContext& c)
{
    const ArmDecodedOperand& option = c.getOperand(0);
    // The SPSR and the banked registers (r12_fiq...) are not visible in user
    // mode; the mask of the APSR / CPSR has R (bit 4) clear
    cString fieldName(option.m_name);
    fieldName.makeLower();
    if (!(fieldName.left(4) == cString("apsr") ||
          fieldName.left(4) == cString("cpsr")) || ((option.m_option & 16) != 0))
    {
        undefined(c);
        return;
    }
    SemanticOperand value = (c.getOperand(1).m_kind == ArmDecodedOperand::KIND_IMMEDIATE) ?
                            k32(c, getImmediateValue(c.getOperand(1))) : rd(c, 1);
    static const struct { uint m_register; uint m_bit; uint m_mask; } bits[] = {
        { ARM_REG_N, 31, 8 }, { ARM_REG_Z, 30, 8 }, { ARM_REG_C, 29, 8 },
        { ARM_REG_V, 28, 8 }, { ARM_REG_Q, 27, 8 }, { ARM_REG_GE0, 16, 4 },
        { ARM_REG_GE1, 17, 4 }, { ARM_REG_GE2, 18, 4 }, { ARM_REG_GE3, 19, 4 }
    };
    for (uint i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
    {
        if ((option.m_option & bits[i].m_mask) == 0)
            continue;
        c.move(c.getRegister(bits[i].m_register),
               field(c, value, bits[i].m_bit, 1));
    }
}

} // namespace a32
} // namespace armdis
