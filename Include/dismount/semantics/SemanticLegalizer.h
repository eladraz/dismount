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
 *   Flags    OP_SET_FLAGS is expanded into IR which writes the flag registers
 *            from the recipe (integer recipes, the conditional form of ccmp),
 *            and OP_TEST_FLAGS into the boolean IR of its condition. The
 *            expansion computes what the SemanticInterpreter computes.
 *   Alias    Reading a guest sub-register (w0, s0) is a truncate of its
 *            parent register; writing one that zero extends its parent (w0
 *            zeroes x0[32..63]) is a zero extend of the value into the parent.
 *            The zero registers (xzr, wzr) read as 0 and drop their writes.
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

    /*
     * The sub-register rule alone, exposed for the differential tests of the
     * rule (tests/TestSemanticLegalizerArm64.cpp: the rule runs on the flag
     * rows, which legalize() refuses until the flags rule is in). Rewrite a
     * block in place: a read of a sub-register is a truncate of its parent, a
     * write of one that zero extends its parent (ARM64 w0 and x0) is computed
     * in a temporary and written back, a zero register reads as 0 and its
     * writes go to a temporary nobody reads. The guest instructions keep
     * their addresses. A sub-register write which keeps the rest of its parent
     * is refused (no target has it yet).
     *
     * RISC-V has sub-registers too: fflags (bits 0-4 of fcsr) and vxsat (bit
     * 0 of vcsr) are read as a truncate of their parent, and the writes of
     * them are refused (no zero extension), as are frm (fcsr bits 5-7) and
     * vxrm (vcsr bits 1-2), which have a bit offset. RISC-V x0 is a zero
     * register, so it reads as 0 and its writes are dropped.
     *
     * block - A block lifted from the guest processor
     *
     * Throw exception for a sub-register which is not a plain part of a full
     * register (bit offset or a parent of its own), and for a sub-register
     * write which keeps the rest of its parent. The block is unchanged then.
     */
    void expandSubRegisters(SemanticBlock& block);

    /*
     * The flags rule alone, exposed for the same reason. Rewrite a
     * block in place: every OP_SET_FLAGS becomes IR which writes the flag
     * registers of the architecture from the recipe (see
     * SemanticOperation::FlagRecipe), every OP_TEST_FLAGS becomes the boolean
     * IR of its condition. The flags which the architecture has no register
     * for are constants (ARM has no parity flag): they are not written and
     * they read as 0. The other instructions, and the numbering of the
     * temporaries and labels, are kept. A block without flag operations is
     * unchanged.
     *
     * The emitted IR uses the integer compare, the boolean and integer logic,
     * the shifts, the moves and the truncations and extensions; a float compare
     * recipe emits OP_COMPARE on the float type with the COND_FLOAT conditions
     * (the target must lower them, see SemanticOperation::Condition).
     *
     * The fusion (fuse = true, the default): a test of the flags which
     * follows an unconditional OP_SET_FLAGS of all of N Z C V (the A64 compare
     * and the plain add, or a logic result) computes its condition from the
     * operands with a compare, not from the flag registers, when that flag
     * write is overwritten by a later write of all of N Z C V before any join,
     * branch or effect of the block. The operands must hold the same values
     * at the test (the registers they read are checked). A flag write which a
     * later write overwrites before any read is removed. fuse = false gives
     * the expansion alone, which the tests compare the fused block with.
     *
     * block - A block lifted from the guest processor
     * fuse  - The fusion described above
     *
     * Throw exception for a recipe or a condition the rule doesn't expand
     * yet, and for a flag operation at a width the rule doesn't handle. The
     * block is unchanged then.
     */
    void expandFlags(SemanticBlock& block, bool fuse = true);

private:
    // Remove OP_SET_FLAGS whose flags are not read before they are written
    // again, and the temporaries which are not read
    void removeDeadValues(SemanticBlock& block);
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
