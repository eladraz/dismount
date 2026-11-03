#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICLEGALIZER_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICLEGALIZER_H

/*
 * SemanticLegalizer.h
 *
 * Rewrite a semantic block so that every operation is one the target
 * processor supports: split the values which are too wide for it, widen the
 * ones which are too narrow, and expand the operations it doesn't have.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/ArchitectureDescription.h"

/*
 * Rewrite a semantic block for a target processor.
 *
 * The rules, applied until every operation passes
 * ArchitectureDescription::isSupported() of the target:
 *
 *   Split    A value wider than the target is split into parts:
 *              64 bit OP_ADD on a 32 bit target -> OP_ADD + OP_ADD_CARRY
 *              512 bit vector on 128 bit NEON   -> 4 operations of 128 bit
 *   Widen    A value narrower than the target's operations is computed in
 *            a wider one and truncated:
 *              8 bit OP_ADD on ARM -> 32 bit OP_ADD + OP_TRUNCATE
 *   Expand   An operation the target doesn't have becomes a sequence:
 *              OP_POPULATION_COUNT  -> shifts, masks and additions
 *              OP_ROTATE_LEFT       -> OP_ROTATE_RIGHT by (size - count)
 *              vector OP_DIVIDE_*   -> one scalar operation per lane
 *              OP_FLOAT_* on f80    -> OP_HELPER (software x87)
 *   Flags    OP_SET_FLAGS whose flags are never read is removed. The other
 *            flags are materialized from the recipe, unless the target has
 *            the same flags and the lowering can set them directly.
 *   Alias    Writing a guest sub-register (al, ah, ax) becomes an OP_INSERT
 *            into its parent register; reading it, an OP_EXTRACT.
 *   Memory   The guest memory model is kept on the target: an IA-32 (total
 *            store order) guest on a weak ARM target gets the fences or the
 *            acquire/release accesses that TSO implies.
 *
 * NOTE: This class is not thread-safe
 */
class SemanticLegalizer {
public:
    /*
     * Constructor
     *
     * guest  - The processor the block was lifted from
     * target - The processor the block will run on
     */
    SemanticLegalizer(const ArchitectureDescriptionPtr& guest,
                      const ArchitectureDescriptionPtr& target);

    /*
     * Rewrite a block in place for the target. The registers of the block
     * are still the guest's: mapping them is the lowering's job.
     *
     * block - A block lifted from the guest processor
     *
     * Throw exception if an operation can't be legalized for the target
     * (that's a bug in the target's isSupported() or in an expansion rule)
     */
    void legalize(SemanticBlock& block);

private:
    // Remove OP_SET_FLAGS whose flags are not read before they are written
    // again, and the temporaries which are not read
    void removeDeadValues(SemanticBlock& block);
    // Change sub-register accesses into OP_EXTRACT / OP_INSERT
    void expandSubRegisters(SemanticBlock& block);
    // Materialize the flags which are read, from their recipes
    void materializeFlags(SemanticBlock& block);
    // Split values which are wider than the target supports
    void splitWideValues(SemanticBlock& block);
    // Widen values which are narrower than the target supports
    void widenNarrowValues(SemanticBlock& block);
    // Expand the operations the target doesn't support
    void expandOperations(SemanticBlock& block);
    // Add the fences that the guest memory model needs on the target
    void enforceMemoryModel(SemanticBlock& block);

    // The processor the block was lifted from
    ArchitectureDescriptionPtr m_guest;
    // The processor the block will run on
    ArchitectureDescriptionPtr m_target;
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICLEGALIZER_H
