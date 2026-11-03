#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICLOWERING_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICLOWERING_H

/*
 * SemanticLowering.h
 *
 * Translate a legal semantic block into the instructions of a target
 * processor, through the target's AssemblerInterface.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/assembler/AssemblerInterface.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/ArchitectureDescription.h"

/*
 * A target instruction pattern: the target instruction which implements an
 * IR pattern.
 *
 * The pattern is a template (SemanticTemplateStep) matched against the IR.
 * A pattern of a few steps covers a sequence of IR instructions, so a target
 * instruction which does more than one operation is used when it fits:
 *
 *   IR                                   ARM pattern
 *   t0 = shl r1, 2; t1 = add r0, t0      "add $d, $s0, $s1, lsl #$s2"
 *   t0 = sub r0, r1; set_flags<sub>      "subs $d, $s0, $s1"
 *   t0 = mul r1, r2; t1 = add t0, r3     "madd $d, $s0, $s1, $s2"
 *   t0 = add r0, 8;  t1 = load t0        "ldr $d, [$s0, #$s1]"
 *
 * The text is given to the target's assembler. $d is the destination of the
 * last step, $s0..$s2 are the operands of the pattern by order of their first
 * use.
 */
struct SemanticLoweringPattern {
    // The IR steps the pattern covers
    const SemanticTemplateStep* m_steps;
    // The number of steps
    uint m_stepsCount;
    // The target assembly, one or more lines separated by '\n'
    const char* m_assembly;
    // The cost of the pattern (target instructions, or cycles). When more
    // than one pattern covers the same IR, the cheapest cover is used
    uint m_cost;
};

/*
 * Map the registers of the guest to the target.
 *
 * The guest registers which the target has room for are kept in target
 * registers (on ARM64, all the IA-32 general purpose registers). The others
 * live in the guest state, a memory block which a target register points to,
 * and are loaded and stored around their uses.
 */
class SemanticRegisterMap {
public:
    // Virtual destructor
    virtual ~SemanticRegisterMap() {};

    /*
     * Return the target register of a guest register
     *
     * guestRegister  - The id of a guest register
     * targetRegister - Will be filled with the id of the target register
     *
     * Return false if the guest register lives in the guest state
     */
    virtual bool getTargetRegister(uint guestRegister,
                                   uint& targetRegister) const = 0;

    /*
     * Return the offset of a guest register in the guest state, in bytes
     */
    virtual uint getStateOffset(uint guestRegister) const = 0;

    /*
     * Return the id of the target register which points to the guest state
     */
    virtual uint getStateRegister() const = 0;
};

// The reference countable object
typedef cSmartPtr<SemanticRegisterMap> SemanticRegisterMapPtr;

/*
 * Translate a legal semantic block into instructions of the target.
 *
 * The block must be legalized for the target first (see SemanticLegalizer):
 * every operation is then covered by at least one pattern.
 *
 * NOTE: This class is not thread-safe. Only one thread can work on a single
 *       instance.
 */
class SemanticLowering {
public:
    // Virtual destructor
    virtual ~SemanticLowering() {};

    /*
     * Return the description of the target processor
     */
    virtual ArchitectureDescriptionPtr getTarget() const = 0;

    /*
     * Write the target instructions of a block
     *
     * block       - A block legalized for the target
     * registerMap - Where the guest registers live on the target
     * assembler   - The assembler of the target. The guest branches are
     *               written as dependencies (BinaryDependencies) on the
     *               translated guest addresses, which the translator
     *               resolves when it links the blocks
     *
     * Throw exception if an IR instruction isn't covered by a pattern
     */
    virtual void lower(const SemanticBlock& block,
                       const SemanticRegisterMap& registerMap,
                       AssemblerInterface& assembler) = 0;
};

// The reference countable object
typedef cSmartPtr<SemanticLowering> SemanticLoweringPtr;

/*
 * Return the lowering of a target processor
 */
class SemanticLoweringFactory {
public:
    /*
     * Return a new lowering
     *
     * target - The processor to generate instructions for
     *
     * Throw exception if the processor has no lowering
     */
    static SemanticLoweringPtr getLowering(
                            OpcodeSubsystems::DisassemblerType target);
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICLOWERING_H
