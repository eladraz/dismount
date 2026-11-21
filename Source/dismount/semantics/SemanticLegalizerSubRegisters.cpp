/*
 * SemanticLegalizerSubRegisters.cpp
 *
 * Implementation file: the sub-register rule of the SemanticLegalizer
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include <stdio.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/list.h"
#include "xStl/data/hash.h"
#include "xStl/exceptions.h"
#include "dismount/semantics/SemanticLegalizer.h"

namespace {

// Throw the error of an illegal block: 'text' names the operation and the
// guest address which can't be legalized
void legalizeFail(const char* text)
{
    XSTL_THROW(cException, cString(text).getBuffer(), EXCEPTION_FAILED);
}

// The operand 'n' of an instruction: the destination for n = 0, the source
// n - 1 otherwise
const SemanticOperand& getOperand(const SemanticInstruction& instruction,
                                  uint n)
{
    if (n == 0)
        return instruction.getDestination();
    return instruction.getSource(n - 1);
}

// The number of operands of an instruction which are looked at: the
// destination and the sources
const uint OPERANDS_COUNT = 1 + SemanticInstruction::MAX_SOURCES;

// Return true if the operand is a register which the rule rewrites: a zero
// register (read as 0, written to nothing) or a sub-register (a part of its
// parent register)
bool isRewritten(const ArchitectureDescription* guest,
                 const SemanticOperand& operand)
{
    if (operand.getKind() != SemanticOperand::KIND_REGISTER)
        return false;
    const ArchitectureRegister& reg = guest->getRegister(operand.getIndex());
    return reg.m_isZero || (reg.m_parent != ArchitectureRegister::NO_PARENT);
}

// Return true if the block has an operand the rule rewrites
bool needsRewrite(const ArchitectureDescription* guest,
                  const SemanticBlock& block)
{
    const cList<SemanticInstruction>& instructions = block.getInstructions();
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        for (uint n = 0; n < OPERANDS_COUNT; n++)
        {
            if (isRewritten(guest, getOperand(*i, n)))
                return true;
        }
    }
    return false;
}

/*
 * The rebuild of a block for the sub-register rule. Each instruction of the
 * old block is appended in turn, between the reads and the write backs it
 * needs (into a new block, which replaces the old one only when complete):
 *
 *   read sub-register r (bits b, offset o, parent p)
 *                                             t = TRUNCATE<b>(SHIFT_RIGHT<o>(p))
 *                                             (MOVE if p is b bits and o is 0)
 *   read zero register                        the immediate 0
 *   write sub-register r = v (zero extends)   t = op(...)  then  p = ZERO_EXTEND<p>(t)
 *   write zero register                       t = op(...)  (t is never read)
 *
 * The temporaries and the labels are numbered again in order of first use,
 * and the guest instructions are begun again at their new indices
 * (SemanticBlock has no way to move them). The numbering drops the
 * temporaries which no instruction names any more.
 */
class SubRegisterRewrite {
public:
    SubRegisterRewrite(const ArchitectureDescription* guest,
                       SemanticBlock& block) :
        m_guest(guest),
        m_block(block),
        m_address(0)
    {
    }

    // Set the guest address of the instructions which follow, for the errors
    void setAddress(uint64 address)
    {
        m_address = address;
    }

    // Append the rewrite of an instruction of the old block
    void rewrite(const SemanticInstruction& instruction);

private:
    // Return the operand of a source (reads of the sub-registers are emitted)
    SemanticOperand readSource(const SemanticOperand& operand);
    // Return the operand of a temporary or a label renumbered in the block
    SemanticOperand mapOperand(const SemanticOperand& operand);
    // Write back a sub-register into its parent after the instruction
    void writeBack(const ArchitectureRegister& reg,
                   const SemanticOperand& value, bool isParentZeroed);
    // Refuse a sub-register whose parent is not a full register
    void checkParent(const ArchitectureRegister& reg);
    // Refuse a written sub-register which is not a plain part of a full register
    void checkSubRegister(const ArchitectureRegister& reg);
    // Refuse an operand whose width is not the width of its register
    void checkWidth(const ArchitectureRegister& reg,
                    const SemanticOperand& value);
    // Throw "not implemented", naming the register and the guest address
    void failNotImplemented(const ArchitectureRegister& reg,
                            const char* what);

    // The architecture the block was lifted from
    const ArchitectureDescription* m_guest;
    // The block being rebuilt
    SemanticBlock& m_block;
    // The old temporaries and labels, by their old number
    cHash<uint64, uint32> m_temporaries;
    cHash<uint64, uint32> m_labels;
    // The guest address of the instructions being rewritten
    uint64 m_address;
};

void SubRegisterRewrite::failNotImplemented(const ArchitectureRegister& reg,
                                            const char* what)
{
    char text[256];
    snprintf(text, sizeof(text),
             "legalizer: %s %s at guest 0x%llx is not implemented", what,
             reg.m_name, (unsigned long long)m_address);
    legalizeFail(text);
}

void SubRegisterRewrite::checkParent(const ArchitectureRegister& reg)
{
    // The parent is a full register
    if (m_guest->getRegister(reg.m_parent).m_parent !=
        ArchitectureRegister::NO_PARENT)
    {
        failNotImplemented(reg, "sub-register");
    }
}

void SubRegisterRewrite::checkSubRegister(const ArchitectureRegister& reg)
{
    // A write starts at the bottom of the parent (no insert into the parent)
    if (reg.m_bitOffset != 0)
        failNotImplemented(reg, "sub-register");
    checkParent(reg);
}

void SubRegisterRewrite::checkWidth(const ArchitectureRegister& reg,
                                    const SemanticOperand& value)
{
    if (value.getType().getTotalBits() != reg.m_bits)
        failNotImplemented(reg, "sub-register of another width");
}

SemanticOperand SubRegisterRewrite::mapOperand(const SemanticOperand& operand)
{
    const uint64 index = operand.getIndex();
    switch (operand.getKind())
    {
    case SemanticOperand::KIND_TEMPORARY:
        if (!m_temporaries.hasKey(index))
        {
            m_temporaries.append(index,
                    m_block.newTemporary(operand.getType()).getIndex());
        }
        return SemanticOperand::temporary(m_temporaries[index],
                                          operand.getType());
    case SemanticOperand::KIND_LABEL:
        if (!m_labels.hasKey(index))
            m_labels.append(index, m_block.newLabel().getIndex());
        return SemanticOperand::label(m_labels[index]);
    default:
        return operand;
    }
}

SemanticOperand SubRegisterRewrite::readSource(const SemanticOperand& operand)
{
    if (operand.getKind() != SemanticOperand::KIND_REGISTER)
        return mapOperand(operand);

    const ArchitectureRegister& reg = m_guest->getRegister(operand.getIndex());
    if (reg.m_isZero)
        return SemanticOperand::immediate(0, operand.getType());
    if (reg.m_parent == ArchitectureRegister::NO_PARENT)
        return operand;

    checkParent(reg);
    checkWidth(reg, operand);
    const ArchitectureRegister& parent = m_guest->getRegister(reg.m_parent);
    const SemanticType parentType = SemanticType::integer(parent.m_bits);
    SemanticOperand parentOperand = SemanticOperand::registerOperand(
                    reg.m_parent, parentType);
    // The bits from the offset up come down to bit 0 (frm is bits 5-7 of fcsr)
    if (reg.m_bitOffset != 0)
    {
        // The narrow values are computed in place at 8, 16, 32 and 64 bits
        // (widenNarrowValues() widens nothing): a field of another width (frm,
        // 3 bits) has no lowering, so such a read is refused as before
        const uint bits = operand.getType().getTotalBits();
        if ((bits < 8) || (bits > 64) || ((bits & (bits - 1)) != 0))
            failNotImplemented(reg, "sub-register");
        const SemanticOperand shifted = m_block.newTemporary(parentType);
        m_block.append(SemanticInstruction(
                SemanticOperation::OP_SHIFT_RIGHT_LOGICAL, parentType, shifted,
                parentOperand,
                SemanticOperand::immediate(reg.m_bitOffset, parentType)));
        parentOperand = shifted;
    }
    const SemanticOperand value = m_block.newTemporary(operand.getType());
    const bool isWhole = (reg.m_bitOffset == 0) && (parent.m_bits == reg.m_bits);
    const SemanticOperation::Operation operation =
            isWhole ? SemanticOperation::OP_MOVE : SemanticOperation::OP_TRUNCATE;
    m_block.append(SemanticInstruction(operation, operand.getType(), value,
                                       parentOperand));
    return value;
}

void SubRegisterRewrite::writeBack(const ArchitectureRegister& reg,
                                   const SemanticOperand& value,
                                   bool isParentZeroed)
{
    // A write which keeps the rest of the parent (IA-32 al) would need an
    // insert; A64 has none, so it is refused
    if (!reg.m_zeroExtendsParent && !isParentZeroed)
        failNotImplemented(reg, "sub-register write which keeps its parent");

    const ArchitectureRegister& parent = m_guest->getRegister(reg.m_parent);
    const SemanticOperand parentOperand = SemanticOperand::registerOperand(
                    reg.m_parent, SemanticType::integer(parent.m_bits));
    const SemanticOperation::Operation operation =
            (parent.m_bits == reg.m_bits) ? SemanticOperation::OP_MOVE :
                                            SemanticOperation::OP_ZERO_EXTEND;
    m_block.append(SemanticInstruction(operation,
                                       SemanticType::integer(parent.m_bits),
                                       parentOperand, value));
}

void SubRegisterRewrite::rewrite(const SemanticInstruction& instruction)
{
    // The sources are read first, the sub-registers through a temporary
    SemanticInstruction rewritten(instruction);
    for (uint s = 0; s < SemanticInstruction::MAX_SOURCES; s++)
        rewritten.setSource(s, readSource(instruction.getSource(s)));

    // The destination: a zero register is written to a temporary nobody
    // reads, a sub-register to a temporary which is written back after
    const SemanticOperand& destination = instruction.getDestination();
    bool isWriteBack = false;
    const ArchitectureRegister* writtenRegister = NULL;
    SemanticOperand value;
    if (destination.getKind() == SemanticOperand::KIND_REGISTER)
    {
        const ArchitectureRegister& reg =
                                    m_guest->getRegister(destination.getIndex());
        if (reg.m_isZero)
        {
            rewritten.setDestination(m_block.newTemporary(destination.getType()));
        } else if (reg.m_parent != ArchitectureRegister::NO_PARENT)
        {
            checkSubRegister(reg);
            checkWidth(reg, destination);
            value = m_block.newTemporary(destination.getType());
            rewritten.setDestination(value);
            isWriteBack = true;
            writtenRegister = &reg;
        } else
        {
            rewritten.setDestination(destination);
        }
    } else
    {
        rewritten.setDestination(mapOperand(destination));
    }

    m_block.append(rewritten);

    if (isWriteBack)
    {
        const bool isParentZeroed =
            (instruction.getAttributes() &
             SemanticInstruction::ATTRIBUTE_ZERO_PARENT) != 0;
        writeBack(*writtenRegister, value, isParentZeroed);
    }
}

} // namespace

void SemanticLegalizer::expandSubRegisters(SemanticBlock& block)
{
    // A block without zero registers and sub-registers keeps its numbering
    if (!needsRewrite(m_guest.getPointer(), block))
        return;

    // The result is built in a local block and assigned at the end: a refusal
    // (a throw) in the middle leaves the caller's block as it was
    cList<SemanticInstruction> old = block.getInstructions();
    cList<SemanticBlock::GuestInstruction> guests = block.getGuestInstructions();
    SemanticBlock result(block.getArchitecture());
    result.setFallThroughAddress(block.getFallThroughAddress());

    SubRegisterRewrite rewrite(m_guest.getPointer(), result);
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
        i++;
        index++;
    }

    block = result;
}
