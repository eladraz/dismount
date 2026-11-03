#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICBLOCK_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICBLOCK_H

/*
 * SemanticBlock.h
 *
 * A list of semantic IR instructions: the lifted code of one or more guest
 * instructions, usually a guest basic block.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/list.h"
#include "xStl/data/array.h"
#include "xStl/data/smartptr.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"

/*
 * A list of semantic IR instructions.
 *
 * The registers of the instructions belong to the architecture of the block
 * (getArchitecture()): after lifting it's the guest, and after the lowering
 * maps the registers it's the target.
 *
 * Each guest instruction starts with a marker (see GuestInstruction), so the
 * translator knows which guest address an IR instruction belongs to, for
 * precise guest exceptions and for debugging.
 *
 * NOTE: This class is not thread-safe
 */
class SemanticBlock {
public:
    // The location of a guest instruction inside the block
    struct GuestInstruction {
        // The address of the guest instruction
        ProcessorAddress m_address;
        // The size of the guest instruction in bytes
        uint m_size;
        // The index of its first IR instruction inside the block
        uint m_firstInstruction;
    };

    /*
     * Constructor. Construct an empty block
     *
     * architecture - The processor whose registers the block uses
     */
    SemanticBlock(OpcodeSubsystems::DisassemblerType architecture);

    // Return the processor whose registers the block uses
    OpcodeSubsystems::DisassemblerType getArchitecture() const;
    // Change the processor (after the lowering maps the registers)
    void setArchitecture(OpcodeSubsystems::DisassemblerType architecture);

    /*
     * Start the IR of a new guest instruction. The following append() calls
     * belong to it.
     *
     * address - The address of the guest instruction
     * size    - The size of the guest instruction in bytes
     */
    void beginGuestInstruction(const ProcessorAddress& address, uint size);

    /*
     * Add an instruction at the end of the block
     */
    void append(const SemanticInstruction& instruction);

    /*
     * Allocate a new temporary value
     *
     * type - The type of the temporary
     *
     * Return the operand of the temporary
     */
    SemanticOperand newTemporary(const SemanticType& type);

    // Return the number of temporaries allocated so far
    uint getTemporariesCount() const;

    /*
     * Allocate a new label. The label is placed in the block by an OP_LABEL
     * instruction, and branched to with the returned operand
     */
    SemanticOperand newLabel();

    // Return the number of labels allocated so far
    uint getLabelsCount() const;

    // Return the instructions of the block
    const cList<SemanticInstruction>& getInstructions() const;
    cList<SemanticInstruction>& getInstructions();

    // Return the guest instructions of the block, by address order
    const cList<GuestInstruction>& getGuestInstructions() const;

    /*
     * The address after the last guest instruction, where execution continues
     * when the block doesn't end with a branch
     */
    const ProcessorAddress& getFallThroughAddress() const;
    void setFallThroughAddress(const ProcessorAddress& address);

    // Remove all the instructions, temporaries and labels
    void clear();

private:
    // The processor whose registers the block uses
    OpcodeSubsystems::DisassemblerType m_architecture;
    // The instructions
    cList<SemanticInstruction> m_instructions;
    // The number of instructions in m_instructions
    uint m_instructionsCount;
    // The guest instructions
    cList<GuestInstruction> m_guestInstructions;
    // The number of temporaries allocated
    uint m_temporariesCount;
    // The number of labels allocated
    uint m_labelsCount;
    // Where the execution continues after the block
    ProcessorAddress m_fallThroughAddress;
};

// The reference countable object
typedef cSmartPtr<SemanticBlock> SemanticBlockPtr;

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICBLOCK_H
