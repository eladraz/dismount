/*
 * ArmSemanticLift32Memory.cpp
 *
 * Implementation file
 *
 * The A32 / T32 load and store lifters: every addressing mode, the multiple
 * registers, the exclusives and the table branches. See ArmSemanticInternal.h
 * and the table ArmSemanticTable.cpp.
 *
 * The order of an access with a writeback follows the Arm ARM: the address
 * (and the value of a store) is computed first, then the memory is accessed,
 * then the base register is written back, and the loaded registers last (the
 * loaded value wins when the base is loaded: UNPREDICTABLE anyway).
 *
 * The exclusive monitor is the pair of registers ARM_REG_EXCLUSIVE_*: a load
 * exclusive records the address and the size (non zero), a store exclusive
 * succeeds (writes 0) when the monitor is open and holds its address, writes
 * 1 otherwise, and closes the monitor in both cases.
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
using namespace armdis::a32;

typedef SemanticOperation Op;

namespace {

SemanticType word()
{
    return SemanticType::integer(32);
}

SemanticOperand k32(ArmLiftContext& c, uint64 value)
{
    return c.getImmediate((int64)(value & 0xFFFFFFFFULL), word());
}

SemanticOperand b32(ArmLiftContext& c, Op::Operation operation,
                    const SemanticOperand& a, const SemanticOperand& b)
{
    return c.emit(operation, word(), a, b);
}

SemanticOperand add32(ArmLiftContext& c, const SemanticOperand& a, int64 value)
{
    if (value == 0)
        return a;
    return c.emit(Op::OP_ADD, word(), a, k32(c, (uint64)value));
}

bool isThumb(ArmLiftContext& c)
{
    return c.getInstruction().getInstructionSet() == ArmInstructionSet::ARM_T32;
}

// The access size in bits of a load or store mnemonic suffix: "" is a word,
// b / sb a byte, h / sh a halfword
uint getAccessBits(const char* suffix)
{
    if ((strcmp(suffix, "b") == 0) || (strcmp(suffix, "sb") == 0))
        return 8;
    if ((strcmp(suffix, "h") == 0) || (strcmp(suffix, "sh") == 0))
        return 16;
    return 32;
}

// The value of an index register with the shift of the memory operand
SemanticOperand getShiftedIndex(ArmLiftContext& c, const ArmDecodedOperand& memory,
                                uint registerId)
{
    SemanticOperand value = getRegisterValue(c, registerId);
    const uint amount = memory.m_shiftAmount;
    switch (memory.m_shift)
    {
    case ArmDecodedOperand::SHIFT_LSL:
        return b32(c, Op::OP_SHIFT_LEFT, value, k32(c, amount));
    case ArmDecodedOperand::SHIFT_LSR:
        return b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value, k32(c, amount));
    case ArmDecodedOperand::SHIFT_ASR:
        return b32(c, Op::OP_SHIFT_RIGHT_ARITHMETIC, value, k32(c, amount));
    case ArmDecodedOperand::SHIFT_ROR:
        return b32(c, Op::OP_ROTATE_RIGHT, value, k32(c, amount));
    case ArmDecodedOperand::SHIFT_RRX:
    {
        SemanticOperand carry = c.emit(Op::OP_ZERO_EXTEND, word(),
                                       c.getRegister(ARM_REG_C));
        return b32(c, Op::OP_OR,
                   b32(c, Op::OP_SHIFT_LEFT, carry, k32(c, 31)),
                   b32(c, Op::OP_SHIFT_RIGHT_LOGICAL, value, k32(c, 1)));
    }
    default:
        return value;
    }
}

// An address of a memory operand: the address to access and the value of
// the base register after the access (invalid without a writeback)
struct MemoryAddress {
    SemanticOperand m_access;
    SemanticOperand m_writeBack;
    uint m_base;
};

/*
 * Resolve the memory operand 'index' (the offset, pre index and post index
 * forms of A32, T16 and T32, with an immediate or a shifted register offset,
 * added or subtracted). The base register is read once: a load into the
 * base register doesn't change the address
 */
MemoryAddress resolveMemory(ArmLiftContext& c, uint index)
{
    const ArmDecodedOperand& memory = c.getOperand(index);
    CHECK(memory.m_kind == ArmDecodedOperand::KIND_MEMORY);
    MemoryAddress result;
    result.m_base = memory.m_base;

    SemanticOperand base;
    if (memory.m_base == ARM_REG_PC)
        base = k32(c, (c.getInstruction().getAddress() + (isThumb(c) ? 4 : 8)) &
                      ~(uint64)3);
    else
        base = c.emit(Op::OP_MOVE, word(), c.getRegister(memory.m_base));

    // The offset: a (shifted) register or an immediate
    const bool isPost = memory.m_addressing ==
                        ArmDecodedOperand::ADDRESSING_POST_INDEX;
    const uint indexRegister = isPost ? memory.m_postIndexRegister :
                                        memory.m_index;
    SemanticOperand offsetAddress = base;
    if (indexRegister != ARM_NO_REGISTER)
    {
        SemanticOperand offset = getShiftedIndex(c, memory, indexRegister);
        offsetAddress = b32(c, memory.m_isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD,
                            base, offset);
    } else
    {
        offsetAddress = add32(c, base, memory.m_isSubtract ? -memory.m_offset :
                                                             memory.m_offset);
    }

    if (isPost)
    {
        result.m_access = base;
        result.m_writeBack = offsetAddress;
    } else
    {
        result.m_access = offsetAddress;
        if (memory.m_addressing == ArmDecodedOperand::ADDRESSING_PRE_INDEX)
            result.m_writeBack = offsetAddress;
    }
    return result;
}

void writeBack(ArmLiftContext& c, const MemoryAddress& address)
{
    if (address.m_writeBack.isValid())
        c.move(c.getRegister(address.m_base), address.m_writeBack);
}

SemanticOperand loadOrdered(ArmLiftContext& c, const SemanticOperand& address,
                            uint bits, Op::MemoryOrder order)
{
    const SemanticType type = SemanticType::integer(bits);
    SemanticOperand result = c.newTemporary(type);
    SemanticInstruction instruction(Op::OP_LOAD, type, result, address);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    instruction.setMemoryOrder(order);
    c.emit(instruction);
    return result;
}

void storeOrdered(ArmLiftContext& c, const SemanticOperand& address,
                  const SemanticOperand& value, Op::MemoryOrder order)
{
    SemanticInstruction instruction(Op::OP_STORE, value.getType(),
                                    SemanticOperand(), address, value);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    instruction.setMemoryOrder(order);
    c.emit(instruction);
}

// Write the loaded value into the register 'registerId'; the pc branches
// (LoadWritePC: the load of the pc interworks)
void writeLoaded(ArmLiftContext& c, uint registerId, const SemanticOperand& value)
{
    if (registerId == ARM_REG_PC)
        c.branchWritePc(value, true);
    else
        c.move(c.getRegister(registerId), value);
}

// Return the two registers of a pair operand: the A32 forms have one list
// operand (r0-r1), the T32 ones two registers. 'next' is the index of the
// operand which follows
void getPair(ArmLiftContext& c, uint operandIndex, uint& first, uint& second,
             uint& next)
{
    const ArmDecodedOperand& operand = c.getOperand(operandIndex);
    if (operand.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST)
    {
        first = operand.m_register;
        second = operand.m_register + 1;
        next = operandIndex + 1;
    } else
    {
        first = operand.m_register;
        second = c.getOperand(operandIndex + 1).m_register;
        next = operandIndex + 2;
    }
}

} // namespace

namespace armdis {
namespace a32 {

/*
 * The single register loads and stores: ldr, ldrb, ldrh, ldrsb, ldrsh and the
 * unprivileged ldrt, ldrbt, ldrht, ldrsbt, ldrsht, str, strb, strh, strt,
 * strbt, strht, and the acquire / release lda, ldab, ldah, stl, stlb, stlh
 */
void liftLoadStore(ArmLiftContext& c)
{
    const char* mnemonic = getMnemonic(c);
    char name[16];
    size_t length = strlen(mnemonic);
    if ((length > 2) && (mnemonic[length - 2] == '.') && (mnemonic[length - 1] == 'w'))
        length -= 2;
    memcpy(name, mnemonic, length);
    name[length] = 0;

    const bool isLoad = name[1] == 'd';
    const bool isOrdered = (strncmp(name, "lda", 3) == 0) ||
                           (strncmp(name, "stl", 3) == 0);
    char suffix[8];
    strcpy(suffix, name + 3);
    size_t suffixLength = strlen(suffix);
    if (!isOrdered && (suffixLength > 0) && (suffix[suffixLength - 1] == 't'))
        suffix[--suffixLength] = 0;
    const uint bits = getAccessBits(suffix);
    const bool isSigned = suffix[0] == 's';
    const Op::MemoryOrder order = !isOrdered ? Op::ORDER_RELAXED :
                                  (isLoad ? Op::ORDER_ACQUIRE : Op::ORDER_RELEASE);

    const MemoryAddress address = resolveMemory(c, 1);
    if (isLoad)
    {
        SemanticOperand value = loadOrdered(c, address.m_access, bits, order);
        if (bits < 32)
            value = c.emit(isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                           word(), value);
        writeBack(c, address);
        c.write(0, value);
        return;
    }
    SemanticOperand value = c.read(0, word());
    if (bits < 32)
        value = c.emit(Op::OP_TRUNCATE, SemanticType::integer(bits), value);
    storeOrdered(c, address.m_access, value, order);
    writeBack(c, address);
}

// ldrd, strd
void liftLoadStoreDouble(ArmLiftContext& c)
{
    uint first, second, next;
    getPair(c, 0, first, second, next);
    const MemoryAddress address = resolveMemory(c, next);
    const SemanticOperand high = add32(c, address.m_access, 4);
    if (isMnemonic(c, "ldrd"))
    {
        SemanticOperand lowValue = loadOrdered(c, address.m_access, 32,
                                               Op::ORDER_RELAXED);
        SemanticOperand highValue = loadOrdered(c, high, 32, Op::ORDER_RELAXED);
        writeBack(c, address);
        writeLoaded(c, first, lowValue);
        writeLoaded(c, second, highValue);
        return;
    }
    SemanticOperand lowValue = c.emit(Op::OP_MOVE, word(),
                                      getRegisterValue(c, first));
    SemanticOperand highValue = c.emit(Op::OP_MOVE, word(),
                                       getRegisterValue(c, second));
    storeOrdered(c, address.m_access, lowValue, Op::ORDER_RELAXED);
    storeOrdered(c, high, highValue, Op::ORDER_RELAXED);
    writeBack(c, address);
}

/*
 * ldm / stm in the four modes (ia, ib, da, db), push and pop. The registers
 * are accessed in the order of the numbers from the lowest address; the
 * loaded pc interworks
 */
void liftLoadStoreMultiple(ArmLiftContext& c)
{
    const char* mnemonic = getMnemonic(c);
    const bool isPush = strncmp(mnemonic, "push", 4) == 0;
    const bool isPop = strncmp(mnemonic, "pop", 3) == 0;
    const bool isLoad = (mnemonic[0] == 'l') || isPop;
    const bool isStack = isPush || isPop;
    uint baseRegister = ARM_REG_SP;
    uint listIndex = 0;
    bool hasWriteBack = true;
    if (!isStack)
    {
        const ArmDecodedOperand& base = c.getOperand(0);
        baseRegister = base.m_register;
        hasWriteBack = base.m_addressing == ArmDecodedOperand::ADDRESSING_PRE_INDEX;
        listIndex = 1;
    }
    // The mode: ldm / stm + ia, ib, da, db (T32 ldm.w has no mode: ia)
    bool isIncrement = true, isBefore = false;
    if (isPush)
    {
        isIncrement = false;
        isBefore = true;
    } else if (!isStack)
    {
        const char* suffix = mnemonic + 3;
        if (strncmp(suffix, "db", 2) == 0)
        {
            isIncrement = false;
            isBefore = true;
        } else if (strncmp(suffix, "da", 2) == 0)
        {
            isIncrement = false;
        } else if (strncmp(suffix, "ib", 2) == 0)
        {
            isBefore = true;
        }
    }

    // The forms with a caret (the user mode registers, the exception return) are
    // privileged
    if (c.getOperand(listIndex).m_isExplicit)
    {
        undefined(c);
        return;
    }
    const uint32 mask = c.getOperand(listIndex).m_registerMask;
    uint count = 0;
    for (uint i = 0; i < 16; i++)
        if ((mask & (1u << i)) != 0)
            count++;
    SemanticOperand base = c.emit(Op::OP_MOVE, word(), c.getRegister(baseRegister));
    // The lowest address accessed and the base after the instruction
    int64 startOffset, finalOffset;
    if (isIncrement)
    {
        startOffset = isBefore ? 4 : 0;
        finalOffset = 4 * (int64)count;
    } else
    {
        startOffset = isBefore ? -4 * (int64)count : -4 * (int64)count + 4;
        finalOffset = -4 * (int64)count;
    }
    SemanticOperand start = add32(c, base, startOffset);

    SemanticOperand values[16];
    uint position = 0;
    if (isLoad)
    {
        for (uint i = 0; i < 16; i++)
        {
            if ((mask & (1u << i)) == 0)
                continue;
            values[i] = loadOrdered(c, add32(c, start, 4 * (int64)position), 32,
                                    Op::ORDER_RELAXED);
            position++;
        }
        if (hasWriteBack)
            c.move(c.getRegister(baseRegister), add32(c, base, finalOffset));
        for (uint i = 0; i < 15; i++)
            if ((mask & (1u << i)) != 0)
                writeLoaded(c, ARM_REG_R0 + i, values[i]);
        if ((mask & 0x8000) != 0)
            writeLoaded(c, ARM_REG_PC, values[15]);
        return;
    }
    // The values first (the pc stores address + 8), then the stores
    for (uint i = 0; i < 16; i++)
        if ((mask & (1u << i)) != 0)
            values[i] = c.emit(Op::OP_MOVE, word(),
                               getRegisterValue(c, ARM_REG_R0 + i));
    for (uint i = 0; i < 16; i++)
    {
        if ((mask & (1u << i)) == 0)
            continue;
        storeOrdered(c, add32(c, start, 4 * (int64)position), values[i],
                     Op::ORDER_RELAXED);
        position++;
    }
    if (hasWriteBack)
        c.move(c.getRegister(baseRegister), add32(c, base, finalOffset));
}

/*
 * The exclusive loads and stores: ldrex, ldrexb, ldrexh, ldrexd, ldaex*, the
 * strex / stlex ones. A load records the address and the size in the
 * monitor; a store succeeds (status 0) when the monitor holds its address
 * and closes the monitor either way
 */
void liftExclusive(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    const bool isLoad = (name[1] == 'd');
    const bool isOrdered = (strncmp(name, "lda", 3) == 0) ||
                           (strncmp(name, "stl", 3) == 0);
    const size_t length = strlen(name);
    const char last = name[length - 1];
    const bool isDouble = last == 'd';
    const uint bits = isDouble ? 64 : ((last == 'b') ? 8 : ((last == 'h') ? 16 : 32));
    const uint elementBits = isDouble ? 32 : bits;
    const Op::MemoryOrder order = !isOrdered ? Op::ORDER_RELAXED :
                                  (isLoad ? Op::ORDER_ACQUIRE : Op::ORDER_RELEASE);

    if (isLoad)
    {
        uint first = c.getOperand(0).m_register, second = 0, memoryIndex = 1;
        if (isDouble)
            getPair(c, 0, first, second, memoryIndex);
        const MemoryAddress address = resolveMemory(c, memoryIndex);
        SemanticOperand lowValue = loadOrdered(c, address.m_access, elementBits,
                                               order);
        SemanticOperand highValue;
        if (isDouble)
            highValue = loadOrdered(c, add32(c, address.m_access, 4), 32, order);
        c.move(c.getRegister(ARM_REG_EXCLUSIVE_ADDRESS), address.m_access);
        c.move(c.getRegister(ARM_REG_EXCLUSIVE_SIZE), k32(c, bits / 8));
        if (elementBits < 32)
            lowValue = c.emit(Op::OP_ZERO_EXTEND, word(), lowValue);
        writeLoaded(c, first, lowValue);
        if (isDouble)
            writeLoaded(c, second, highValue);
        return;
    }

    // status, Rt [, Rt2], memory
    uint first = c.getOperand(1).m_register, second = 0, memoryIndex = 2;
    if (isDouble)
        getPair(c, 1, first, second, memoryIndex);
    const MemoryAddress address = resolveMemory(c, memoryIndex);
    SemanticOperand lowValue = c.emit(Op::OP_MOVE, word(),
                                      getRegisterValue(c, first));
    if (elementBits < 32)
        lowValue = c.emit(Op::OP_TRUNCATE, SemanticType::integer(elementBits),
                          lowValue);
    SemanticOperand highValue;
    if (isDouble)
        highValue = c.emit(Op::OP_MOVE, word(), getRegisterValue(c, second));
    SemanticOperand isOpen = a32::compare(c, Op::COND_NOT_EQUAL, word(),
            c.getRegister(ARM_REG_EXCLUSIVE_SIZE), k32(c, 0));
    SemanticOperand isSame = a32::compare(c, Op::COND_EQUAL, word(),
            c.getRegister(ARM_REG_EXCLUSIVE_ADDRESS), address.m_access);
    SemanticOperand isPassed = c.emit(Op::OP_AND, SemanticType::integer(1), isOpen,
                                      isSame);
    SemanticOperand failLabel = c.newLabel();
    SemanticOperand endLabel = c.newLabel();
    c.branchIf(c.emit(Op::OP_XOR, SemanticType::integer(1), isPassed,
                      c.getImmediate(1, SemanticType::integer(1))), failLabel);
    storeOrdered(c, address.m_access, lowValue, order);
    if (isDouble)
        storeOrdered(c, add32(c, address.m_access, 4), highValue, order);
    c.write(0, k32(c, 0));
    c.branch(endLabel);
    c.placeLabel(failLabel);
    c.write(0, k32(c, 1));
    c.placeLabel(endLabel);
    c.move(c.getRegister(ARM_REG_EXCLUSIVE_SIZE), k32(c, 0));
}

// clrex
void liftClearExclusive(ArmLiftContext& c)
{
    c.move(c.getRegister(ARM_REG_EXCLUSIVE_SIZE), k32(c, 0));
}

// pld, pldw, pli: a hint to the memory system
void liftPrefetch(ArmLiftContext& c)
{
    if (c.getOperand(0).m_kind != ArmDecodedOperand::KIND_MEMORY)
    {
        c.emit(SemanticInstruction(Op::OP_NOP, SemanticType(), SemanticOperand()));
        return;
    }
    const MemoryAddress address = resolveMemory(c, 0);
    c.emit(SemanticInstruction(Op::OP_PREFETCH, SemanticType(), SemanticOperand(),
                               address.m_access));
}

// tbb, tbh: pc = pc + 2 * the byte / halfword at [Rn, Rm{, lsl #1}]
void liftTableBranch(ArmLiftContext& c)
{
    const ArmDecodedOperand& memory = c.getOperand(0);
    SemanticOperand base = getRegisterValue(c, memory.m_base);
    SemanticOperand offset = getShiftedIndex(c, memory, memory.m_index);
    SemanticOperand address = b32(c, Op::OP_ADD, base, offset);
    const bool isHalf = isMnemonic(c, "tbh");
    SemanticOperand entry = loadOrdered(c, address, isHalf ? 16 : 8,
                                        Op::ORDER_RELAXED);
    entry = c.emit(Op::OP_ZERO_EXTEND, word(), entry);
    SemanticOperand target = b32(c, Op::OP_ADD, c.getPcValue(),
                                 b32(c, Op::OP_SHIFT_LEFT, entry, k32(c, 1)));
    c.branchWritePc(target, false);
}

} // namespace a32
} // namespace armdis
