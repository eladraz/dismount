/*
 * SemanticLegalizerFlags.cpp
 *
 * Implementation file: the flags rule of the SemanticLegalizer
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include <stdio.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/list.h"
#include "xStl/exceptions.h"
#include "dismount/semantics/SemanticFloat.h"
#include "dismount/semantics/SemanticLegalizer.h"

namespace {

// Throw the error of an illegal block: 'text' names the operation and the
// guest address which can't be legalized
void legalizeFail(const char* text)
{
    XSTL_THROW(cException, cString(text).getBuffer(), EXCEPTION_FAILED);
}

// Return true if the instruction is a flag operation of the rule
bool isFlagOperation(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    return (operation == SemanticOperation::OP_SET_FLAGS) ||
           (operation == SemanticOperation::OP_TEST_FLAGS);
}

// Return true if the two operands name the same register or temporary (the
// width is not compared: the callers compare the values of one width)
bool sameLocation(const SemanticOperand& x, const SemanticOperand& y)
{
    if (x.getKind() != y.getKind())
        return false;
    return (x.getKind() == SemanticOperand::KIND_REGISTER) ||
           (x.getKind() == SemanticOperand::KIND_TEMPORARY) ?
           (x.getIndex() == y.getIndex()) : false;
}

// Return true if the two operands hold the same value: the same location, or
// the same immediate, of the same width
bool sameOperand(const SemanticOperand& x, const SemanticOperand& y)
{
    if ((x.getKind() == SemanticOperand::KIND_IMMEDIATE) &&
        (y.getKind() == SemanticOperand::KIND_IMMEDIATE))
    {
        return (x.getValue() == y.getValue()) &&
               (x.getType().getTotalBits() == y.getType().getTotalBits());
    }
    return sameLocation(x, y) &&
           (x.getType().getTotalBits() == y.getType().getTotalBits());
}

// Return true if the instruction reads the register 'id' as one of its sources
bool readsRegister(const SemanticInstruction& instruction, uint id)
{
    for (uint s = 0; s < SemanticInstruction::MAX_SOURCES; s++)
    {
        const SemanticOperand& source = instruction.getSource(s);
        if ((source.getKind() == SemanticOperand::KIND_REGISTER) &&
            (source.getIndex() == id))
            return true;
    }
    return false;
}

// Return true if the instruction writes the register 'id'
bool writesRegister(const SemanticInstruction& instruction, uint id)
{
    const SemanticOperand& destination = instruction.getDestination();
    return SemanticOperation::hasDestination(instruction.getOperation()) &&
           (destination.getKind() == SemanticOperand::KIND_REGISTER) &&
           (destination.getIndex() == id);
}

// Return true if the instruction writes a flag register of the architecture.
// A plain OP_MOVE does it too (msr nzcv, rmif, cfinv, setf8, the fccmp false
// path), without a flag operation
bool writesFlagRegister(const ArchitectureDescription* guest,
                        const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    if (!SemanticOperation::hasDestination(instruction.getOperation()) ||
        (destination.getKind() != SemanticOperand::KIND_REGISTER))
        return false;
    for (uint32 flag = 1; flag <= SemanticFlags::FLAG_DIRECTION; flag <<= 1)
    {
        uint registerId;
        if (guest->getFlagRegister(flag, registerId) &&
            (registerId == destination.getIndex()))
            return true;
    }
    return false;
}

// Return true if the instruction may leave the straight line of the block
// or touch the state beyond its destination: a label, a call, a branch, a
// trap, an effect, a memory access
bool isFusionBarrier(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    return (operation == SemanticOperation::OP_LABEL) ||
           (SemanticOperation::isControlFlow(operation) &&
            (operation != SemanticOperation::OP_BRANCH_CONDITIONAL)) ||
           SemanticOperation::hasSideEffects(operation);
}

// Return true if a flag write can't be removed across the instruction: the
// flags may be read by the guest (a branch leaves the block, a load or a
// store may fault), or by an effect
bool isFlagBarrier(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    return (operation == SemanticOperation::OP_LABEL) ||
           SemanticOperation::isControlFlow(operation) ||
           SemanticOperation::hasSideEffects(operation) ||
           SemanticOperation::isMemoryAccess(operation) ||
           ((instruction.getAttributes() &
             SemanticInstruction::ATTRIBUTE_MAY_FAULT) != 0);
}

// Return true if the instruction ends the reach of a flag write for the
// lookahead of the fusion: a join or a branch (the flags leave the block), a
// call, a trap or an effect. A conditional branch counts: the flags are read
// on the way out
bool isKillBarrier(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    return (operation == SemanticOperation::OP_LABEL) ||
           SemanticOperation::isControlFlow(operation) ||
           SemanticOperation::hasSideEffects(operation);
}

// Return true if the instruction writes all of N Z C V (conditionally or not)
bool writesAllFlags(const SemanticInstruction& instruction)
{
    const uint32 nzcv = SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO |
                        SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW;
    return (instruction.getOperation() == SemanticOperation::OP_SET_FLAGS) &&
           ((instruction.getFlagMask() & nzcv) == nzcv);
}

// The instructions before the one being rewritten (the nearest first). The
// window is bounded: a fusion looks back a few instructions only
class RecentInstructions {
public:
    static const uint WINDOW = 8;

    RecentInstructions() : m_count(0)
    {
    }

    // Remember an instruction of the old block, as the nearest one
    void push(const SemanticInstruction& instruction)
    {
        for (uint k = WINDOW - 1; k > 0; k--)
            m_items[k] = m_items[k - 1];
        m_items[0] = instruction;
        if (m_count < WINDOW)
            m_count++;
    }

    // The k-th previous instruction (k = 1 is the one before), or NULL
    const SemanticInstruction* get(uint k) const
    {
        if ((k < 1) || (k > m_count))
            return NULL;
        return &m_items[k - 1];
    }

private:
    SemanticInstruction m_items[WINDOW];
    uint m_count;
};

// The distance (1 is the nearest) of the instruction of 'recent' which writes
// the temporary, or 0 when none of the window writes it
uint findDefinition(const RecentInstructions& recent,
                    const SemanticOperand& temporary)
{
    if (temporary.getKind() != SemanticOperand::KIND_TEMPORARY)
        return 0;
    for (uint k = 1; k <= RecentInstructions::WINDOW; k++)
    {
        const SemanticInstruction* previous = recent.get(k);
        if (previous == NULL)
            break;
        if (sameLocation(previous->getDestination(), temporary))
            return k;
    }
    return 0;
}

// The register that an operand read 'distance' instructions before the flags
// reads: the register itself, or the register a truncation of the window
// reads (the operand sizes differ the lifter's own copies: the sub-register rule gives each
// reader of a sub-register its own truncation). Return false otherwise
bool registerRead(const RecentInstructions& recent,
                  const SemanticOperand& operand, uint distance,
                  SemanticOperand& reg, uint& readAt)
{
    if (operand.getKind() == SemanticOperand::KIND_REGISTER)
    {
        reg = operand;
        readAt = distance;
        return true;
    }
    const uint definition = findDefinition(recent, operand);
    if (definition == 0)
        return false;
    const SemanticInstruction* truncate = recent.get(definition);
    if ((truncate->getOperation() != SemanticOperation::OP_TRUNCATE) ||
        (truncate->getSource(0).getKind() != SemanticOperand::KIND_REGISTER))
        return false;
    reg = truncate->getSource(0);
    readAt = definition;
    return true;
}

// Return true if an operand 'u' read 'du' instructions before the flags holds
// the value of the operand 's' read 'ds' instructions before (the flags): the
// same temporary or immediate, or the same register with no write between the
// two reads
bool sameValueRead(const RecentInstructions& recent,
                   const SemanticOperand& u, uint du,
                   const SemanticOperand& s, uint ds)
{
    // Constants and temporaries are values: the same one is the same value
    if ((u.getKind() == SemanticOperand::KIND_IMMEDIATE) ||
        (s.getKind() == SemanticOperand::KIND_IMMEDIATE) ||
        (u.getKind() == SemanticOperand::KIND_TEMPORARY))
    {
        if (sameOperand(u, s))
            return true;
        if ((u.getKind() == SemanticOperand::KIND_IMMEDIATE) ||
            (s.getKind() == SemanticOperand::KIND_IMMEDIATE))
            return false;
    }
    SemanticOperand registerU;
    SemanticOperand registerS;
    uint readU = 0;
    uint readS = 0;
    if (!registerRead(recent, u, du, registerU, readU) ||
        !registerRead(recent, s, ds, registerS, readS))
        return false;
    if (!sameLocation(registerU, registerS))
        return false;
    const uint low = (readU < readS) ? readU : readS;
    const uint high = (readU < readS) ? readS : readU;
    for (uint k = low + 1; k < high; k++)
    {
        const SemanticInstruction* between = recent.get(k);
        if ((between == NULL) || writesRegister(*between, registerU.getIndex()))
            return false;
    }
    return true;
}

/*
 * The expansion of the flag operations of one block. The rebuilt block is
 * appended instruction by instruction (the caller keeps the guest
 * instructions in step). Every value is a 1 bit operand: a temporary, a flag
 * or a register, or a constant (an immediate operand). The constants are
 * folded by the boolean helpers, so an expansion never tests a constant.
 *
 * The flags are read before they are written: the new values of a
 * OP_SET_FLAGS are computed first, and the writes to the flag registers
 * come last. The source operands may name a flag register (an EXPLICIT
 * recipe, a conditional compare) and see the flags as they were before.
 *
 * The fusion (fuse = true): a test of the flags which follows an unconditional
 * OP_SET_FLAGS of all of N Z C V, with its operands unchanged, computes the
 * condition from the operands (a compare of a and b, of the result), not from
 * the flag registers. A flag write which is overwritten before any read is
 * removed (see removeDeadFlagWrites).
 */
class FlagRewrite {
public:
    FlagRewrite(const ArchitectureDescription* guest, SemanticBlock& block,
                bool fuse) :
        m_guest(guest),
        m_block(block),
        m_address(0),
        m_fuse(fuse),
        m_fused(FUSED_NONE)
    {
    }

    // Set the guest address of the instructions which follow, for the errors
    void setAddress(uint64 address)
    {
        m_address = address;
    }

    // Append the expansion of an instruction of the old block
    void rewrite(const SemanticInstruction& instruction);

    // Update the fusion after an instruction of the old block was rewritten:
    // 'recent' are the instructions before it, and 'isOverwritten' says the
    // flags it writes (if it is a flag write) are overwritten before a join,
    // a branch or the end of the block
    void observe(const SemanticInstruction& instruction,
                 const RecentInstructions& recent, bool isOverwritten);

private:
    // What the flags are, as a function of the operands of the last
    // unconditional OP_SET_FLAGS (none: they are read from the flag registers)
    enum FusedKind {
        // a - b with b = ~b0 (the A64 compare): m_fusedA = a, m_fusedB = b0
        FUSED_SUBTRACT,
        // r = a + b: m_fusedA = a, m_fusedR = r
        FUSED_ADD,
        // logic result r: m_fusedR = r; C and V are 0
        FUSED_LOGIC,
        FUSED_NONE
    };

    // A constant 1 bit value
    static SemanticOperand bit(bool value);
    // The boolean operations on 1 bit values, with the constants folded
    SemanticOperand not1(const SemanticOperand& x);
    SemanticOperand and1(const SemanticOperand& x, const SemanticOperand& y);
    SemanticOperand or1(const SemanticOperand& x, const SemanticOperand& y);
    SemanticOperand xor1(const SemanticOperand& x, const SemanticOperand& y);
    // d = s0 <op> s1 (or d = <op> s0), the result has the type of the
    // operands; a temporary is returned
    SemanticOperand emit(SemanticOperation::Operation operation,
                         const SemanticType& type,
                         const SemanticOperand& source0,
                         const SemanticOperand& source1 = SemanticOperand());
    // d (1 bit) = s0 <condition> s1 on the integers of the type of s0
    SemanticOperand compare(SemanticOperation::Condition condition,
                            const SemanticOperand& source0,
                            const SemanticOperand& source1);
    // The bit 'k' of an integer of 'bits' bits (0 above its width)
    SemanticOperand bitAt(const SemanticOperand& x, uint k, uint bits);
    // The most significant bit of an integer of 'bits' bits
    SemanticOperand msb(const SemanticOperand& x, uint bits);
    // The low byte of an integer of 'bits' bits has an even number of ones
    SemanticOperand isEvenParity(const SemanticOperand& x, uint bits);
    // The value of a flag register, or 0 when the architecture has no
    // register for the flag
    SemanticOperand readFlag(uint32 flag) const;
    // The condition evaluated on the operands of the fused flags: an empty
    // operand when the fusion has no expression for it (the flags are read)
    SemanticOperand fusedCondition(SemanticOperation::Condition condition);
    // The condition evaluated on the flags (SemanticInterpreter::evaluateCondition)
    SemanticOperand condition(SemanticOperation::Condition condition);
    // The value of a flag written by the recipe of an OP_SET_FLAGS
    SemanticOperand recipeFlag(const SemanticInstruction& instruction,
                               uint32 flag);
    // The value of a flag of FLAGS_FLOAT_COMPARE: an OP_COMPARE of the
    // float operands (SemanticInterpreter::floatCompareFlags)
    SemanticOperand floatCompareFlag(const SemanticInstruction& instruction,
                                     uint32 flag);
    // OP_SET_FLAGS: the flags of its mask are written
    void rewriteSetFlags(const SemanticInstruction& instruction);
    // OP_TEST_FLAGS: the destination is the condition
    void rewriteTestFlags(const SemanticInstruction& instruction);
    // Set the fusion from an OP_SET_FLAGS of the old block (none if it has no
    // fusion: a conditional write, a partial mask, an unknown recipe)
    void detectFusion(const SemanticInstruction& instruction,
                      const RecentInstructions& recent, bool isOverwritten);
    // The fusion of a subtraction: 'b' is the inverted b0 of the 'recent'
    // OP_SUBTRACT(a, b0) -> r which precedes the flags (see detectFusion)
    void detectSubtraction(const SemanticOperand& a, const SemanticOperand& b,
                           const SemanticOperand& r, uint bits,
                           const RecentInstructions& recent);
    // Return true if the operand may be a fusion operand: a value of 'bits'
    // bits which is not a flag register
    bool isFusionValue(const SemanticOperand& operand, uint bits) const;
    // Return true if the register is a flag register of the architecture
    bool isFlagRegister(uint id) const;
    // Throw "not implemented", naming the guest address and the value
    void failNotImplemented(const char* what, uint64 value);
    // Throw "not implemented", naming the guest address and the text
    void failText(const char* text);

    // The architecture the block was lifted from
    const ArchitectureDescription* m_guest;
    // The block being rebuilt
    SemanticBlock& m_block;
    // The guest address of the instructions being rewritten
    uint64 m_address;
    // Fuse the tests with the operands of the flags (see the class comment)
    bool m_fuse;
    // The fusion of the flags, and its operands (see FusedKind)
    FusedKind m_fused;
    SemanticOperand m_fusedA;
    SemanticOperand m_fusedB;
    SemanticOperand m_fusedR;
};

SemanticOperand FlagRewrite::bit(bool value)
{
    return SemanticOperand::immediate(value ? 1 : 0, SemanticType::integer(1));
}

SemanticOperand FlagRewrite::not1(const SemanticOperand& x)
{
    if (x.getKind() == SemanticOperand::KIND_IMMEDIATE)
        return bit(x.getValue() == 0);
    return emit(SemanticOperation::OP_NOT, SemanticType::integer(1), x);
}

SemanticOperand FlagRewrite::and1(const SemanticOperand& x,
                                  const SemanticOperand& y)
{
    if (x.getKind() == SemanticOperand::KIND_IMMEDIATE)
        return (x.getValue() != 0) ? y : bit(false);
    if (y.getKind() == SemanticOperand::KIND_IMMEDIATE)
        return (y.getValue() != 0) ? x : bit(false);
    return emit(SemanticOperation::OP_AND, SemanticType::integer(1), x, y);
}

SemanticOperand FlagRewrite::or1(const SemanticOperand& x,
                                 const SemanticOperand& y)
{
    if (x.getKind() == SemanticOperand::KIND_IMMEDIATE)
        return (x.getValue() != 0) ? bit(true) : y;
    if (y.getKind() == SemanticOperand::KIND_IMMEDIATE)
        return (y.getValue() != 0) ? bit(true) : x;
    return emit(SemanticOperation::OP_OR, SemanticType::integer(1), x, y);
}

SemanticOperand FlagRewrite::xor1(const SemanticOperand& x,
                                  const SemanticOperand& y)
{
    if (x.getKind() == SemanticOperand::KIND_IMMEDIATE)
        return (x.getValue() != 0) ? not1(y) : y;
    if (y.getKind() == SemanticOperand::KIND_IMMEDIATE)
        return (y.getValue() != 0) ? not1(x) : x;
    return emit(SemanticOperation::OP_XOR, SemanticType::integer(1), x, y);
}

SemanticOperand FlagRewrite::emit(SemanticOperation::Operation operation,
                                  const SemanticType& type,
                                  const SemanticOperand& source0,
                                  const SemanticOperand& source1)
{
    SemanticOperand result = m_block.newTemporary(type);
    m_block.append(SemanticInstruction(operation, type, result, source0,
                                       source1));
    return result;
}

SemanticOperand FlagRewrite::compare(SemanticOperation::Condition condition,
                                     const SemanticOperand& source0,
                                     const SemanticOperand& source1)
{
    // The compare is on the type of its operands, the result is a bit
    SemanticOperand result = m_block.newTemporary(SemanticType::integer(1));
    SemanticInstruction instruction(SemanticOperation::OP_COMPARE,
                                    source0.getType(), result, source0,
                                    source1);
    instruction.setCondition(condition);
    m_block.append(instruction);
    return result;
}

SemanticOperand FlagRewrite::bitAt(const SemanticOperand& x, uint k,
                                   uint bits)
{
    if (k >= bits)
        return bit(false);
    // A 1 bit value is its own bit 0
    if (bits == 1)
        return x;
    const SemanticType type = SemanticType::integer(bits);
    SemanticOperand shifted = x;
    if (k != 0)
    {
        shifted = emit(SemanticOperation::OP_SHIFT_RIGHT_LOGICAL, type, x,
                       SemanticOperand::immediate(k, type));
    }
    SemanticOperand result = m_block.newTemporary(SemanticType::integer(1));
    m_block.append(SemanticInstruction(SemanticOperation::OP_TRUNCATE,
                                       SemanticType::integer(1), result,
                                       shifted));
    return result;
}

SemanticOperand FlagRewrite::msb(const SemanticOperand& x, uint bits)
{
    return bitAt(x, bits - 1, bits);
}

SemanticOperand FlagRewrite::isEvenParity(const SemanticOperand& x,
                                          uint bits)
{
    // The low byte, as an 8 bit value: the xor of its halves, then of the
    // quarters and of the pairs leaves the parity in bit 0
    const SemanticType byte = SemanticType::integer(8);
    SemanticOperand low = x;
    if (bits > 8)
    {
        low = emit(SemanticOperation::OP_TRUNCATE, byte, x);
    } else if (bits < 8)
    {
        low = emit(SemanticOperation::OP_ZERO_EXTEND, byte, x);
    }
    SemanticOperand folded = low;
    static const uint shifts[3] = { 4, 2, 1 };
    for (uint i = 0; i < 3; i++)
    {
        SemanticOperand shifted = emit(SemanticOperation::OP_SHIFT_RIGHT_LOGICAL,
                                       byte, folded,
                                       SemanticOperand::immediate(shifts[i],
                                                                  byte));
        folded = emit(SemanticOperation::OP_XOR, byte, folded, shifted);
    }
    SemanticOperand odd = emit(SemanticOperation::OP_AND, byte, folded,
                               SemanticOperand::immediate(1, byte));
    return compare(SemanticOperation::COND_EQUAL, odd,
                   SemanticOperand::immediate(0, byte));
}

SemanticOperand FlagRewrite::readFlag(uint32 flag) const
{
    uint registerId;
    if (!m_guest->getFlagRegister(flag, registerId))
        return bit(false);
    return SemanticOperand::registerOperand(registerId,
                                            SemanticType::integer(1));
}

bool FlagRewrite::isFlagRegister(uint id) const
{
    for (uint32 flag = 1; flag <= SemanticFlags::FLAG_DIRECTION; flag <<= 1)
    {
        uint registerId;
        if (m_guest->getFlagRegister(flag, registerId) && (registerId == id))
            return true;
    }
    return false;
}

bool FlagRewrite::isFusionValue(const SemanticOperand& operand,
                                uint bits) const
{
    if (operand.getType().getTotalBits() != bits)
        return false;
    if (operand.getKind() == SemanticOperand::KIND_IMMEDIATE)
        return true;
    if (operand.getKind() == SemanticOperand::KIND_TEMPORARY)
        return true;
    return (operand.getKind() == SemanticOperand::KIND_REGISTER) &&
           !isFlagRegister(operand.getIndex());
}

SemanticOperand FlagRewrite::fusedCondition(
                                    SemanticOperation::Condition condition)
{
    // The flags of the fusion are the formulas of the SemanticOperation
    // recipes on the operands, the same as detectFusion checked them. Each
    // condition is the one of evaluateCondition on those flags, rewritten as
    // a compare (only the conditions with a compare form are fused)
    switch (m_fused)
    {
    case FUSED_SUBTRACT:
    {
        // a - b: Z is a == b, the carry C (the ARM carry, not a borrow) is
        // a >= b unsigned, N xor V is a < b signed
        const SemanticOperand& a = m_fusedA;
        const SemanticOperand& b = m_fusedB;
        switch (condition)
        {
        case SemanticOperation::COND_EQUAL:
            return compare(SemanticOperation::COND_EQUAL, a, b);
        case SemanticOperation::COND_NOT_EQUAL:
            return not1(compare(SemanticOperation::COND_EQUAL, a, b));
        // C, or C or Z (Z implies C)
        case SemanticOperation::COND_CARRY:
        case SemanticOperation::COND_UNSIGNED_LESS:
        case SemanticOperation::COND_UNSIGNED_LESS_EQUAL:
            return compare(SemanticOperation::COND_UNSIGNED_GREATER_EQUAL, a, b);
        // not C, or not C and not Z (not C implies not Z)
        case SemanticOperation::COND_NOT_CARRY:
        case SemanticOperation::COND_UNSIGNED_GREATER_EQUAL:
        case SemanticOperation::COND_UNSIGNED_GREATER:
            return compare(SemanticOperation::COND_UNSIGNED_LESS, a, b);
        case SemanticOperation::COND_SIGNED_LESS:
            return compare(SemanticOperation::COND_SIGNED_LESS, a, b);
        case SemanticOperation::COND_SIGNED_LESS_EQUAL:
            return compare(SemanticOperation::COND_SIGNED_LESS_EQUAL, a, b);
        case SemanticOperation::COND_SIGNED_GREATER:
            return compare(SemanticOperation::COND_SIGNED_GREATER, a, b);
        case SemanticOperation::COND_SIGNED_GREATER_EQUAL:
            return compare(SemanticOperation::COND_SIGNED_GREATER_EQUAL, a, b);
        default:
            break;
        }
        break;
    }
    case FUSED_ADD:
    {
        // r = a + b: Z is r == 0, the carry is r < a unsigned (a wrap)
        const SemanticOperand& a = m_fusedA;
        const SemanticOperand& r = m_fusedR;
        const SemanticOperand zero = SemanticOperand::immediate(0, m_fusedR.getType());
        switch (condition)
        {
        case SemanticOperation::COND_EQUAL:
            return compare(SemanticOperation::COND_EQUAL, r, zero);
        case SemanticOperation::COND_NOT_EQUAL:
            return not1(compare(SemanticOperation::COND_EQUAL, r, zero));
        case SemanticOperation::COND_CARRY:
        case SemanticOperation::COND_UNSIGNED_LESS:
            return compare(SemanticOperation::COND_UNSIGNED_LESS, r, a);
        case SemanticOperation::COND_NOT_CARRY:
        case SemanticOperation::COND_UNSIGNED_GREATER_EQUAL:
            return compare(SemanticOperation::COND_UNSIGNED_GREATER_EQUAL, r, a);
        case SemanticOperation::COND_UNSIGNED_LESS_EQUAL:
        {
            SemanticOperand carry = compare(SemanticOperation::COND_UNSIGNED_LESS,
                                            r, a);
            return or1(carry, compare(SemanticOperation::COND_EQUAL, r, zero));
        }
        case SemanticOperation::COND_UNSIGNED_GREATER:
        {
            SemanticOperand notCarry = compare(
                    SemanticOperation::COND_UNSIGNED_GREATER_EQUAL, r, a);
            return and1(notCarry, not1(compare(SemanticOperation::COND_EQUAL,
                                               r, zero)));
        }
        default:
            break;
        }
        break;
    }
    case FUSED_LOGIC:
    {
        // r = a op b: Z is r == 0, N is r < 0 (signed), C and V are 0
        const SemanticOperand& r = m_fusedR;
        const SemanticOperand zero = SemanticOperand::immediate(0, m_fusedR.getType());
        switch (condition)
        {
        case SemanticOperation::COND_EQUAL:
        case SemanticOperation::COND_UNSIGNED_LESS_EQUAL:
            return compare(SemanticOperation::COND_EQUAL, r, zero);
        case SemanticOperation::COND_NOT_EQUAL:
        case SemanticOperation::COND_UNSIGNED_GREATER:
            return not1(compare(SemanticOperation::COND_EQUAL, r, zero));
        case SemanticOperation::COND_CARRY:
        case SemanticOperation::COND_UNSIGNED_LESS:
            return bit(false);
        case SemanticOperation::COND_NOT_CARRY:
        case SemanticOperation::COND_UNSIGNED_GREATER_EQUAL:
            return bit(true);
        case SemanticOperation::COND_SIGNED_LESS:
            return compare(SemanticOperation::COND_SIGNED_LESS, r, zero);
        case SemanticOperation::COND_SIGNED_GREATER_EQUAL:
            return not1(compare(SemanticOperation::COND_SIGNED_LESS, r, zero));
        case SemanticOperation::COND_SIGNED_LESS_EQUAL:
        {
            SemanticOperand isZero = compare(SemanticOperation::COND_EQUAL, r,
                                             zero);
            return or1(isZero, compare(SemanticOperation::COND_SIGNED_LESS, r,
                                       zero));
        }
        case SemanticOperation::COND_SIGNED_GREATER:
        {
            SemanticOperand notZero = not1(compare(
                    SemanticOperation::COND_EQUAL, r, zero));
            return and1(notZero, not1(compare(SemanticOperation::COND_SIGNED_LESS,
                                              r, zero)));
        }
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    return SemanticOperand();
}

SemanticOperand FlagRewrite::condition(SemanticOperation::Condition cond)
{
    // The table of SemanticOperation::Condition: the conditions of the flags
    // (SemanticInterpreter::evaluateCondition)
    if (cond >= SemanticOperation::COND_FLOAT_ORDERED)
        failNotImplemented("float condition on a flag operation", cond);
    const SemanticOperand fused = fusedCondition(cond);
    if (fused.getKind() != SemanticOperand::KIND_NONE)
        return fused;
    switch (cond)
    {
    case SemanticOperation::COND_NONE:
    case SemanticOperation::COND_ALWAYS:
        return bit(true);
    case SemanticOperation::COND_NEVER:
        return bit(false);
    case SemanticOperation::COND_EQUAL:
        return readFlag(SemanticFlags::FLAG_ZERO);
    case SemanticOperation::COND_NOT_EQUAL:
        return not1(readFlag(SemanticFlags::FLAG_ZERO));
    case SemanticOperation::COND_UNSIGNED_LESS:
    case SemanticOperation::COND_CARRY:
        return readFlag(SemanticFlags::FLAG_CARRY);
    case SemanticOperation::COND_UNSIGNED_GREATER_EQUAL:
    case SemanticOperation::COND_NOT_CARRY:
        return not1(readFlag(SemanticFlags::FLAG_CARRY));
    case SemanticOperation::COND_UNSIGNED_LESS_EQUAL:
        return or1(readFlag(SemanticFlags::FLAG_CARRY),
                   readFlag(SemanticFlags::FLAG_ZERO));
    case SemanticOperation::COND_UNSIGNED_GREATER:
        return and1(not1(readFlag(SemanticFlags::FLAG_CARRY)),
                    not1(readFlag(SemanticFlags::FLAG_ZERO)));
    case SemanticOperation::COND_SIGNED_LESS:
        return xor1(readFlag(SemanticFlags::FLAG_SIGN),
                    readFlag(SemanticFlags::FLAG_OVERFLOW));
    case SemanticOperation::COND_SIGNED_GREATER_EQUAL:
        return not1(xor1(readFlag(SemanticFlags::FLAG_SIGN),
                         readFlag(SemanticFlags::FLAG_OVERFLOW)));
    case SemanticOperation::COND_SIGNED_LESS_EQUAL:
        return or1(readFlag(SemanticFlags::FLAG_ZERO),
                   xor1(readFlag(SemanticFlags::FLAG_SIGN),
                        readFlag(SemanticFlags::FLAG_OVERFLOW)));
    case SemanticOperation::COND_SIGNED_GREATER:
        return and1(not1(readFlag(SemanticFlags::FLAG_ZERO)),
                    not1(xor1(readFlag(SemanticFlags::FLAG_SIGN),
                              readFlag(SemanticFlags::FLAG_OVERFLOW))));
    case SemanticOperation::COND_NEGATIVE:
        return readFlag(SemanticFlags::FLAG_SIGN);
    case SemanticOperation::COND_NOT_NEGATIVE:
        return not1(readFlag(SemanticFlags::FLAG_SIGN));
    case SemanticOperation::COND_OVERFLOW:
        return readFlag(SemanticFlags::FLAG_OVERFLOW);
    case SemanticOperation::COND_NOT_OVERFLOW:
        return not1(readFlag(SemanticFlags::FLAG_OVERFLOW));
    case SemanticOperation::COND_PARITY_EVEN:
        return readFlag(SemanticFlags::FLAG_PARITY);
    case SemanticOperation::COND_PARITY_ODD:
        return not1(readFlag(SemanticFlags::FLAG_PARITY));
    default:
        failNotImplemented("condition", cond);
    }
    return bit(false);
}

SemanticOperand FlagRewrite::floatCompareFlag(
                                    const SemanticInstruction& instruction,
                                    uint32 flag)
{
    // The relation of the operands gives the flags: unordered ZF PF CF, less
    // CF, equal ZF, greater none. The sign, the overflow and the adjust are 0
    const SemanticType& type = instruction.getType();
    if ((type.getKind() != SemanticType::KIND_FLOAT) || type.isVector() ||
        !SemanticFloat::isSupported(type.getFloatFormat()))
    {
        failNotImplemented("float compare of the type", type.getTotalBits());
    }
    // The exception flags of a compare are recorded in the status register
    // of the architecture, which the expansion doesn't model (comisd and
    // fcomi raise them)
    if ((instruction.getAttributes() &
         SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS) != 0)
    {
        failText("float compare which raises the exception flags");
    }
    SemanticOperation::Condition condition;
    switch (flag)
    {
    case SemanticFlags::FLAG_ZERO:
        condition = SemanticOperation::COND_FLOAT_UNORDERED_OR_EQUAL;
        break;
    case SemanticFlags::FLAG_PARITY:
        condition = SemanticOperation::COND_FLOAT_UNORDERED;
        break;
    case SemanticFlags::FLAG_CARRY:
        condition = SemanticOperation::COND_FLOAT_UNORDERED_OR_LESS;
        break;
    default:
        return bit(false);
    }

    // A signaling compare (comisd) raises the invalid operation on any NaN,
    // a quiet one (ucomisd) only on a signaling NaN
    SemanticOperand result = m_block.newTemporary(SemanticType::integer(1));
    SemanticInstruction compare(SemanticOperation::OP_COMPARE, type, result,
                                instruction.getSource(0),
                                instruction.getSource(1));
    compare.setCondition(condition);
    compare.setBehavior((instruction.getKind() ==
                         SemanticOperation::FLOAT_COMPARE_SIGNALING) ?
                        SemanticOperation::BEHAVIOR_COMPARE_SIGNALING :
                        SemanticOperation::BEHAVIOR_COMPARE_QUIET);
    m_block.append(compare);
    return result;
}

SemanticOperand FlagRewrite::recipeFlag(const SemanticInstruction& instruction,
                                        uint32 flag)
{
    if (instruction.getFlagRecipe() == SemanticOperation::FLAGS_FLOAT_COMPARE)
        return floatCompareFlag(instruction, flag);

    // The formulas of SemanticOperation::FlagRecipe, with the operands of the
    // interpreter: a = s0, b = s1, r = s2, all of 'bits' bits
    const SemanticOperation::FlagRecipe recipe = instruction.getFlagRecipe();
    const uint bits = instruction.getType().getTotalBits();
    if ((bits < 1) || (bits > 64))
        failNotImplemented("flags of bits", bits);
    if (recipe == SemanticOperation::FLAGS_EXPLICIT)
        return bitAt(instruction.getSource(0), 0, bits);

    const SemanticOperand a = instruction.getSource(0);
    const SemanticOperand b = instruction.getSource(1);
    const SemanticOperand r = instruction.getSource(2);
    const SemanticType type = SemanticType::integer(bits);
    if ((a.getType().getTotalBits() != bits) ||
        (b.getType().getTotalBits() != bits) ||
        (r.getType().getTotalBits() != bits))
    {
        failNotImplemented("flags operand of another width", bits);
    }

    // The flags of the result: the same for the arithmetic and the logic
    if (flag == SemanticFlags::FLAG_ZERO)
        return compare(SemanticOperation::COND_EQUAL, r,
                       SemanticOperand::immediate(0, type));
    if (flag == SemanticFlags::FLAG_SIGN)
        return msb(r, bits);
    if (flag == SemanticFlags::FLAG_PARITY)
        return isEvenParity(r, bits);

    if (recipe == SemanticOperation::FLAGS_LOGIC)
    {
        // The carry, the overflow and the adjust are 0
        return bit(false);
    }
    if (flag == SemanticFlags::FLAG_ADJUST)
    {
        // The carry out of bit 3 (bit 4 of the operands and the result)
        SemanticOperand sum = emit(SemanticOperation::OP_XOR, type,
                emit(SemanticOperation::OP_XOR, type, a, b), r);
        return bitAt(sum, 4, bits);
    }
    if (recipe == SemanticOperation::FLAGS_ADD)
    {
        if (flag == SemanticFlags::FLAG_CARRY)
        {
            // MSB((a & b) | ((a | b) & ~r))
            SemanticOperand notR = emit(SemanticOperation::OP_NOT, type, r);
            SemanticOperand term = emit(SemanticOperation::OP_AND, type,
                emit(SemanticOperation::OP_OR, type, a, b), notR);
            return msb(emit(SemanticOperation::OP_OR, type,
                            emit(SemanticOperation::OP_AND, type, a, b),
                            term), bits);
        }
        if (flag == SemanticFlags::FLAG_OVERFLOW)
        {
            // MSB((a ^ r) & (b ^ r))
            return msb(emit(SemanticOperation::OP_AND, type,
                emit(SemanticOperation::OP_XOR, type, a, r),
                emit(SemanticOperation::OP_XOR, type, b, r)), bits);
        }
    } else if (recipe == SemanticOperation::FLAGS_SUBTRACT)
    {
        if (flag == SemanticFlags::FLAG_CARRY)
        {
            // MSB((~a & b) | ((~a | b) & r)): the borrow
            SemanticOperand notA = emit(SemanticOperation::OP_NOT, type, a);
            SemanticOperand term = emit(SemanticOperation::OP_AND, type,
                emit(SemanticOperation::OP_OR, type, notA, b), r);
            return msb(emit(SemanticOperation::OP_OR, type,
                            emit(SemanticOperation::OP_AND, type, notA, b),
                            term), bits);
        }
        if (flag == SemanticFlags::FLAG_OVERFLOW)
        {
            // MSB((a ^ b) & (a ^ r))
            return msb(emit(SemanticOperation::OP_AND, type,
                emit(SemanticOperation::OP_XOR, type, a, b),
                emit(SemanticOperation::OP_XOR, type, a, r)), bits);
        }
    }
    // The flags which the recipe doesn't define are 0 (the direction flag)
    return bit(false);
}

void FlagRewrite::detectSubtraction(const SemanticOperand& a,
                                    const SemanticOperand& b,
                                    const SemanticOperand& r, uint bits,
                                    const RecentInstructions& recent)
{
    // The lifter's compare: r = a - b0 (OP_SUBTRACT, the window's first
    // instruction) and b = ~b0 (OP_NOT, or the complemented constant). The
    // flags of a + ~b0 + 1 are then the flags of the subtraction, on the
    // operands of the subtraction (which the fused tests read)
    // The subtraction is the definition of r, wherever the sub-register rule put its operand
    // truncations: a temporary is written once, and the registers its
    // operands read are checked between the reads (sameValueRead)
    const uint subtractAt = findDefinition(recent, r);
    const SemanticInstruction* subtract = recent.get(subtractAt);
    if ((subtractAt == 0) ||
        (subtract->getOperation() != SemanticOperation::OP_SUBTRACT) ||
        (subtract->getType().getTotalBits() != bits))
        return;
    const SemanticOperand x = subtract->getSource(0);
    const SemanticOperand y = subtract->getSource(1);
    if ((x.getKind() == SemanticOperand::KIND_IMMEDIATE) ||
        !isFusionValue(y, bits) || !sameValueRead(recent, x, subtractAt, a, 0))
        return;

    if (b.getKind() == SemanticOperand::KIND_IMMEDIATE)
    {
        // An inverted constant: b is ~y on 'bits' bits
        if (y.getKind() != SemanticOperand::KIND_IMMEDIATE)
            return;
        const uint64 mask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1);
        if ((((uint64)b.getValue() ^ (uint64)y.getValue()) & mask) != mask)
            return;
    } else
    {
        // The complement of y is the window's definition of b (a NOT), which
        // reads a value of y's value
        const uint complement = findDefinition(recent, b);
        if (complement == 0)
            return;
        const SemanticInstruction* not1Instruction = recent.get(complement);
        if ((not1Instruction->getOperation() != SemanticOperation::OP_NOT) ||
            (not1Instruction->getType().getTotalBits() != bits) ||
            !sameValueRead(recent, not1Instruction->getSource(0), complement,
                           y, subtractAt))
            return;
    }

    m_fused = FUSED_SUBTRACT;
    m_fusedA = x;
    m_fusedB = y;
    m_fusedR = SemanticOperand();
}

void FlagRewrite::detectFusion(const SemanticInstruction& instruction,
                               const RecentInstructions& recent,
                               bool isOverwritten)
{
    // The flags are a function of the operands only when the write is
    // unconditional and writes all of N Z C V. The fusion pays only when the
    // flags it replaces are overwritten (isOverwritten), so that their writes
    // can go: the last writer of a block keeps its tests as they are
    m_fused = FUSED_NONE;
    if (!m_fuse || !isOverwritten ||
        (instruction.getCondition() != SemanticOperation::COND_NONE))
        return;
    const uint32 nzcv = SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO |
                        SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW;
    if ((instruction.getFlagMask() & nzcv) != nzcv)
        return;
    for (uint32 flag = 1; flag <= nzcv; flag <<= 1)
    {
        uint registerId;
        if (!m_guest->getFlagRegister(flag, registerId))
            return;
    }

    const SemanticOperation::FlagRecipe recipe = instruction.getFlagRecipe();
    const uint bits = instruction.getType().getTotalBits();
    const SemanticOperand a = instruction.getSource(0);
    const SemanticOperand b = instruction.getSource(1);
    const SemanticOperand r = instruction.getSource(2);
    if ((bits < 1) || (bits > 64) || !isFusionValue(a, bits) ||
        !isFusionValue(r, bits) || (r.getKind() == SemanticOperand::KIND_IMMEDIATE))
        return;

    if (recipe == SemanticOperation::FLAGS_LOGIC)
    {
        m_fused = FUSED_LOGIC;
        m_fusedA = SemanticOperand();
        m_fusedB = SemanticOperand();
        m_fusedR = r;
        return;
    }
    if (recipe != SemanticOperation::FLAGS_ADD)
        return;
    if (!isFusionValue(b, bits))
        return;

    // r = a + b, the plain add (not the add with carry), right before
    const uint addAt = findDefinition(recent, r);
    const SemanticInstruction* add = recent.get(addAt);
    if ((addAt != 0) && (add->getOperation() == SemanticOperation::OP_ADD) &&
        (add->getType().getTotalBits() == bits) &&
        sameValueRead(recent, add->getSource(0), addAt, a, 0) &&
        sameValueRead(recent, add->getSource(1), addAt, b, 0))
    {
        m_fused = FUSED_ADD;
        m_fusedA = add->getSource(0);
        m_fusedB = SemanticOperand();
        m_fusedR = r;
        return;
    }
    detectSubtraction(a, b, r, bits, recent);
}

void FlagRewrite::observe(const SemanticInstruction& instruction,
                          const RecentInstructions& recent,
                          bool isOverwritten)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    if (operation == SemanticOperation::OP_SET_FLAGS)
    {
        detectFusion(instruction, recent, isOverwritten);
        return;
    }
    if (m_fused == FUSED_NONE)
        return;

    // A write of a flag register by a plain move changes the flags the
    // fusion stands for: the fusion ends
    if (writesFlagRegister(m_guest, instruction))
    {
        m_fused = FUSED_NONE;
        return;
    }

    // The fusion holds across a conditional branch (the fall through keeps
    // the operands), not across a join, a call, a trap or an effect
    if (isFusionBarrier(instruction))
    {
        m_fused = FUSED_NONE;
        return;
    }
    // A write to an operand (a register or a temporary) ends the fusion
    const SemanticOperand& destination = instruction.getDestination();
    if (SemanticOperation::hasDestination(operation) &&
        ((sameLocation(destination, m_fusedA)) ||
         (sameLocation(destination, m_fusedB)) ||
         (sameLocation(destination, m_fusedR))))
    {
        m_fused = FUSED_NONE;
    }
}

void FlagRewrite::rewriteSetFlags(const SemanticInstruction& instruction)
{
    const SemanticOperation::FlagRecipe recipe = instruction.getFlagRecipe();
    if ((recipe != SemanticOperation::FLAGS_ADD) &&
        (recipe != SemanticOperation::FLAGS_SUBTRACT) &&
        (recipe != SemanticOperation::FLAGS_LOGIC) &&
        (recipe != SemanticOperation::FLAGS_EXPLICIT) &&
        (recipe != SemanticOperation::FLAGS_FLOAT_COMPARE))
    {
        failNotImplemented("flags recipe", recipe);
    }

    // The condition of a conditional compare (ccmp) is on the flags before
    // the instruction. The flags it doesn't select are the false flags
    const SemanticOperation::Condition cond = instruction.getCondition();
    const bool isConditional = (cond != SemanticOperation::COND_NONE);
    const SemanticOperand isTrue = condition(cond);
    const uint32 falseFlags = instruction.getFalseFlags();

    // The new values first, the writes to the flag registers last
    struct Write {
        uint m_register;
        SemanticOperand m_value;
    };
    Write writes[7];
    uint count = 0;
    for (uint32 flag = 1; flag <= SemanticFlags::FLAG_DIRECTION; flag <<= 1)
    {
        uint registerId;
        if (((instruction.getFlagMask() & flag) == 0) ||
            !m_guest->getFlagRegister(flag, registerId))
            continue;
        SemanticOperand value = recipeFlag(instruction, flag);
        if (isConditional)
        {
            // value when the condition holds, the false flag otherwise
            const bool falseBit = (falseFlags & flag) != 0;
            if (isTrue.getKind() == SemanticOperand::KIND_IMMEDIATE)
            {
                if (isTrue.getValue() == 0)
                    value = bit(falseBit);
            } else if (falseBit)
            {
                value = or1(not1(isTrue), value);
            } else
            {
                value = and1(isTrue, value);
            }
        }
        writes[count].m_register = registerId;
        writes[count].m_value = value;
        count++;
    }
    for (uint w = 0; w < count; w++)
    {
        const SemanticType booleanType = SemanticType::integer(1);
        m_block.append(SemanticInstruction(SemanticOperation::OP_MOVE,
                booleanType,
                SemanticOperand::registerOperand(writes[w].m_register,
                                                 booleanType),
                writes[w].m_value));
    }
}

void FlagRewrite::rewriteTestFlags(const SemanticInstruction& instruction)
{
    const SemanticOperand destination = instruction.getDestination();
    const SemanticOperand value = condition(instruction.getCondition());
    m_block.append(SemanticInstruction(SemanticOperation::OP_MOVE,
                                       destination.getType(), destination,
                                       value));
}

void FlagRewrite::failNotImplemented(const char* what, uint64 value)
{
    char text[256];
    snprintf(text, sizeof(text), "legalizer: %s %llu at guest 0x%llx is not "
             "implemented", what, (unsigned long long)value,
             (unsigned long long)m_address);
    legalizeFail(text);
}

void FlagRewrite::failText(const char* what)
{
    char text[256];
    snprintf(text, sizeof(text), "legalizer: %s at guest 0x%llx is not "
             "implemented", what, (unsigned long long)m_address);
    legalizeFail(text);
}

void FlagRewrite::rewrite(const SemanticInstruction& instruction)
{
    switch (instruction.getOperation())
    {
    case SemanticOperation::OP_SET_FLAGS:
        rewriteSetFlags(instruction);
        break;
    case SemanticOperation::OP_TEST_FLAGS:
        rewriteTestFlags(instruction);
        break;
    default:
        m_block.append(instruction);
        break;
    }
}

// Remove the flag writes which are overwritten before they are read: a write
// of a flag register (an OP_MOVE) is dead when the next instruction which
// touches the register writes it, with no read and no barrier between. The
// block's last write of each flag is always kept (its successors may read it).
// The dead writes are replaced by OP_NOP, so the numbering is kept
void removeDeadFlagWrites(SemanticBlock& block,
                          const ArchitectureDescription* guest)
{
    uint ids[8];
    uint count = 0;
    for (uint32 flag = 1; flag <= SemanticFlags::FLAG_DIRECTION; flag <<= 1)
    {
        uint registerId;
        if (guest->getFlagRegister(flag, registerId))
            ids[count++] = registerId;
    }

    cList<SemanticInstruction>& instructions = block.getInstructions();
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        if (((*i).getOperation() != SemanticOperation::OP_MOVE) ||
            ((*i).getDestination().getKind() != SemanticOperand::KIND_REGISTER))
            continue;
        const uint target = (*i).getDestination().getIndex();
        bool isFlag = false;
        for (uint f = 0; f < count; f++)
            isFlag = isFlag || (ids[f] == target);
        if (!isFlag)
            continue;

        bool isDead = false;
        cList<SemanticInstruction>::iterator next = i;
        for (next++; next != instructions.end(); next++)
        {
            if (isFlagBarrier(*next) || readsRegister(*next, target))
                break;
            if (writesRegister(*next, target))
            {
                isDead = true;
                break;
            }
        }
        if (isDead)
            *i = SemanticInstruction();
    }
}

} // namespace

void SemanticLegalizer::expandFlags(SemanticBlock& block, bool fuse)
{
    // A block without flag operations keeps its numbering
    const cList<SemanticInstruction>& instructions = block.getInstructions();
    bool hasFlags = false;
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        if (isFlagOperation(*i))
            hasFlags = true;
    }
    if (!hasFlags)
        return;

    // The result is built in a local block and assigned at the end: a refusal
    // (a throw) in the middle leaves the caller's block as it was
    cList<SemanticInstruction> old = block.getInstructions();
    cList<SemanticBlock::GuestInstruction> guests = block.getGuestInstructions();
    SemanticBlock result(block.getArchitecture());
    result.setFallThroughAddress(block.getFallThroughAddress());
    // The temporaries and the labels of the old instructions keep their
    // numbers: the new ones are numbered after them
    for (uint t = 0; t < block.getTemporariesCount(); t++)
        result.newTemporary(SemanticType::integer(1));
    for (uint l = 0; l < block.getLabelsCount(); l++)
        result.newLabel();

    // For each instruction: a flag write is overwritten when the next full
    // write of N Z C V comes before any join, branch or effect (the lookahead
    // of the fusion; the flags of the instruction are then not read after the
    // overwrite)
    const uint count = old.length();
    cBuffer isOverwritten(count > 0 ? count : 1);
    uint position = 0;
    for (cList<SemanticInstruction>::iterator i = old.begin();
         i != old.end(); i++, position++)
    {
        isOverwritten[position] = 0;
        if (!writesAllFlags(*i))
            continue;
        cList<SemanticInstruction>::iterator next = i;
        for (next++; next != old.end(); next++)
        {
            // A flag write overwrites first: OP_SET_FLAGS is an effect too
            if (writesAllFlags(*next))
            {
                isOverwritten[position] = 1;
                break;
            }
            // A plain write of a flag register (msr nzcv) is not a full
            // overwrite of the flags the fusion can see: no fusion
            if (isKillBarrier(*next) ||
                writesFlagRegister(m_guest.getPointer(), *next))
                break;
        }
    }

    FlagRewrite rewrite(m_guest.getPointer(), result, fuse);
    RecentInstructions recent;
    cList<SemanticBlock::GuestInstruction>::iterator guest = guests.begin();
    cList<SemanticInstruction>::iterator i = old.begin();
    uint index = 0;
    while (true)
    {
        // The guest instructions which start here begin here, in order
        while ((guest != guests.end()) &&
               ((*guest).m_firstInstruction <= index))
        {
            result.beginGuestInstruction((*guest).m_address, (*guest).m_size);
            rewrite.setAddress((*guest).m_address.getAddress());
            guest++;
        }
        if (i == old.end())
            break;
        rewrite.rewrite(*i);
        rewrite.observe(*i, recent, isOverwritten[index] != 0);
        recent.push(*i);
        i++;
        index++;
    }

    // The dead flag writes are removed only when the block is fused: the
    // expansion alone (fuse = false) is the reference of the tests
    if (fuse)
        removeDeadFlagWrites(result, m_guest.getPointer());
    block = result;
}
