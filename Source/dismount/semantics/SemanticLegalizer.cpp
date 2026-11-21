/*
 * SemanticLegalizer.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include <stdio.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/sarray.h"
#include "xStl/data/list.h"
#include "xStl/exceptions.h"
#include "dismount/semantics/SemanticLegalizer.h"

namespace {

// Throw the error of an illegal block: 'text' names the operation and the
// guest address which can't be legalized
void legalizeFail(const char* text)
{
    XSTL_THROW(cException, cString(text).getBuffer(), EXCEPTION_FAILED);
}

// The guest address of the instruction 'index' of the block: the address of
// the guest instruction which holds it (0 if the block has no guest
// instructions)
uint64 getGuestAddress(const SemanticBlock& block, uint index)
{
    uint64 address = 0;
    const cList<SemanticBlock::GuestInstruction>& guests =
                                            block.getGuestInstructions();
    for (cList<SemanticBlock::GuestInstruction>::iterator i = guests.begin();
         i != guests.end(); i++)
    {
        if ((*i).m_firstInstruction <= index)
            address = (*i).m_address.getAddress();
    }
    return address;
}

// Return true if the instruction only computes its destination: removing it
// changes nothing else in the guest state or in the memory
bool isPure(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    return !SemanticOperation::hasSideEffects(operation) &&
           !SemanticOperation::isMemoryAccess(operation) &&
           !SemanticOperation::isControlFlow(operation) &&
           (instruction.getFlagMask() == 0) &&
           ((instruction.getAttributes() &
             (SemanticInstruction::ATTRIBUTE_MAY_FAULT |
              SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS)) == 0);
}

} // namespace

SemanticLegalizer::SemanticLegalizer(const ArchitectureDescriptionPtr& guest,
                                     const ArchitectureDescriptionPtr& target) :
    m_guest(guest),
    m_target(target)
{
    CHECK((m_guest.getPointer() != NULL) && (m_target.getPointer() != NULL));
}

void SemanticLegalizer::legalize(SemanticBlock& block)
{
    if (block.getArchitecture() != m_guest->getType())
        legalizeFail("legalizer: the block was not lifted from the guest");

    expandSubRegisters(block);
    expandFlags(block);
    splitWideValues(block);
    widenNarrowValues(block);
    expandOperations(block);
    removeDeadValues(block);
    enforceMemoryModel(block);
}

void SemanticLegalizer::removeDeadValues(SemanticBlock& block)
{
    // The temporaries are written once, so a temporary is dead when no
    // instruction reads it. A dead instruction is replaced by OP_NOP rather
    // than removed: the guest instructions of the block index their first IR
    // instruction, which must stay where it is
    const uint temporaries = block.getTemporariesCount();
    cBuffer isRead(temporaries);

    bool isRemoved = true;
    while (isRemoved)
    {
        isRemoved = false;
        for (uint t = 0; t < temporaries; t++)
            isRead[t] = 0;

        cList<SemanticInstruction>& instructions = block.getInstructions();
        for (cList<SemanticInstruction>::iterator i = instructions.begin();
             i != instructions.end(); i++)
        {
            for (uint s = 0; s < SemanticInstruction::MAX_SOURCES; s++)
            {
                const SemanticOperand& source = (*i).getSource(s);
                if (source.getKind() == SemanticOperand::KIND_TEMPORARY)
                    isRead[source.getIndex()] = 1;
            }
        }

        // An instruction removed in this pass still counts as a reader of
        // its sources: they are found dead by the next pass
        for (cList<SemanticInstruction>::iterator i = instructions.begin();
             i != instructions.end(); i++)
        {
            const SemanticOperand& destination = (*i).getDestination();
            if (!SemanticOperation::hasDestination((*i).getOperation()) ||
                (destination.getKind() != SemanticOperand::KIND_TEMPORARY) ||
                isRead[destination.getIndex()] || !isPure(*i))
                continue;
            *i = SemanticInstruction();
            isRemoved = true;
        }
    }
}

void SemanticLegalizer::splitWideValues(SemanticBlock& block)
{
    // No split rule is implemented yet. The guest values are at most 64 bits
    // (RV64) and the AMD64 target holds 64 bit integers natively, so nothing
    // needs splitting. An operation which would need a split is refused by
    // expandOperations(), which asks the target whether it can do the
    // operation on the type
    (void)block;
}

void SemanticLegalizer::widenNarrowValues(SemanticBlock& block)
{
    // The AMD64 target computes the 8, 16, 32 and 64 bit integers in place
    // (see ArchitectureDescription::isSupported()), so no narrow value is
    // widened: the instructions keep their sizes
    (void)block;
}

void SemanticLegalizer::expandOperations(SemanticBlock& block)
{
    // No expansion rule is implemented yet: every operation must be one the
    // target does natively. OP_UNDEFINED is a guest instruction which has no
    // semantics, so it can't be translated at all
    const cList<SemanticInstruction>& instructions = block.getInstructions();
    uint index = 0;
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++, index++)
    {
        const SemanticOperation::Operation operation = (*i).getOperation();
        if (operation == SemanticOperation::OP_UNDEFINED)
        {
            char text[256];
            snprintf(text, sizeof(text),
                     "legalizer: undefined guest instruction at 0x%llx",
                     (unsigned long long)getGuestAddress(block, index));
            legalizeFail(text);
        }
        if (!m_target->isSupported(operation, (*i).getType()))
        {
            char text[256];
            snprintf(text, sizeof(text),
                     "legalizer: operation %s<%llu bits> at guest 0x%llx is not "
                     "supported by the target",
                     SemanticOperation::getName(operation),
                     (unsigned long long)(*i).getType().getTotalBits(),
                     (unsigned long long)getGuestAddress(block, index));
            legalizeFail(text);
        }
    }
}

void SemanticLegalizer::enforceMemoryModel(SemanticBlock& block)
{
    // A guest whose memory model is weaker than the target's (RVWMO on AMD64,
    // total store order) needs no fence: the target orders the accesses more
    // than the guest requires. The other way round needs fences, which are not
    // implemented yet
    (void)block;
    const ArchitectureDescription::MemoryModel guestModel =
                                            m_guest->getMemoryModel();
    const ArchitectureDescription::MemoryModel targetModel =
                                            m_target->getMemoryModel();
    if ((guestModel == targetModel) ||
        ((guestModel == ArchitectureDescription::MEMORY_WEAK) &&
         (targetModel == ArchitectureDescription::MEMORY_TOTAL_STORE_ORDER)))
    {
        return;
    }
    legalizeFail("legalizer: the guest memory model on this target is not "
                 "implemented");
}
