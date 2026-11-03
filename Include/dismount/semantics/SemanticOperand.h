#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICOPERAND_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICOPERAND_H

/*
 * SemanticOperand.h
 *
 * An operand of a semantic IR instruction: a register of the architecture, a
 * temporary value of the block, an immediate, a guest address or a label of
 * the block.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"

/*
 * An operand of a SemanticInstruction.
 *
 * There is no memory operand: memory is accessed only by OP_LOAD and OP_STORE,
 * whose address is a register, a temporary or an immediate.
 *
 * NOTE: This class is a value, it can be copied freely.
 */
class SemanticOperand {
public:
    // The kind of the operand
    enum Kind {
        // The operand is not in use
        KIND_NONE,
        // A register of the ArchitectureDescription of the block. The index
        // is the register id. Sub-registers (al, ah) are registers too
        KIND_REGISTER,
        // A temporary value of the block, written once. The index is the
        // temporary number, see SemanticBlock::newTemporary()
        KIND_TEMPORARY,
        // A constant. The value is held in 64 bits and is truncated or
        // extended to the type. A wider constant is built with OP_CONCAT
        KIND_IMMEDIATE,
        // An address of the guest program (branch targets, return
        // addresses, ip-relative data)
        KIND_GUEST_ADDRESS,
        // A label inside the block, the target of a branch which stays in
        // the block (loops of rep, conditional execution). The index is the
        // label number, see SemanticBlock::newLabel() and OP_LABEL
        KIND_LABEL
    };

    /*
     * Default constructor. Construct a KIND_NONE operand
     */
    SemanticOperand();

    // Return a register operand
    static SemanticOperand registerOperand(uint registerId,
                                           const SemanticType& type);
    // Return a temporary operand
    static SemanticOperand temporary(uint temporaryIndex,
                                     const SemanticType& type);
    // Return an immediate operand
    static SemanticOperand immediate(int64 value, const SemanticType& type);
    // Return a guest address operand
    static SemanticOperand guestAddress(uint64 address,
                                        const SemanticType& type);
    // Return a label operand
    static SemanticOperand label(uint labelIndex);

    // Return the kind of the operand
    Kind getKind() const;
    // Return the type of the operand
    const SemanticType& getType() const;
    // Return the register id, the temporary index or the label index
    uint getIndex() const;
    // Return the value of an immediate or a guest address
    int64 getValue() const;

    // Return true if the operand is in use
    bool isValid() const;

    // Compare two operands
    bool operator == (const SemanticOperand& other) const;
    bool operator != (const SemanticOperand& other) const;

private:
    // See Kind
    uint8 m_kind;
    // The type of the value
    SemanticType m_type;
    // The register id, the temporary index or the label index
    uint m_index;
    // The immediate value or the guest address
    int64 m_value;
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICOPERAND_H
