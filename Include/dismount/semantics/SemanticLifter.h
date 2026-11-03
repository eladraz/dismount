#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICLIFTER_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICLIFTER_H

/*
 * SemanticLifter.h
 *
 * Translate decoded instructions of a processor into the semantic IR, and
 * classify them.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/semantics/ArchitectureDescription.h"

/*
 * Translate decoded instructions (Opcode, from a StreamDisassembler) into the
 * semantic IR of their processor.
 *
 * A lifter is table driven: it finds the SemanticTemplate of the instruction's
 * mnemonic and instantiates it over the decoded operands (registers, memory
 * addresses, immediates, operand and address sizes, prefixes).
 *
 * Example:
 *    StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(...);
 *    SemanticLifterPtr lifter = SemanticLifterFactory::getLifter(
 *                                    OpcodeSubsystems::DISASSEMBLER_INTEL_32);
 *    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
 *    while (!disassembler->isEndOfStream())
 *    {
 *        OpcodePtr opcode = disassembler->next();
 *        lifter->lift(opcode, block);
 *        if (opcode->isBranch())
 *            break;
 *    }
 *
 * NOTE: This class is not thread-safe. Only one thread can work on a single
 *       instance.
 */
class SemanticLifter {
public:
    // Virtual destructor
    virtual ~SemanticLifter() {};

    /*
     * Return the description of the processor whose instructions are lifted
     */
    virtual ArchitectureDescriptionPtr getArchitecture() const = 0;

    /*
     * Classify an instruction without lifting it
     *
     * instruction - A decoded instruction of the processor
     *
     * Return the classification. CATEGORY_UNKNOWN if the instruction has no
     * template
     */
    virtual OpcodeClassification classify(const OpcodePtr& instruction) const = 0;

    /*
     * Lift an instruction and add its IR at the end of a block. The lifter
     * calls block.beginGuestInstruction() first.
     *
     * instruction - A decoded instruction of the processor
     * block       - The block to add the IR into. Its architecture must be
     *               the lifter's processor
     *
     * Return true if the instruction was lifted. Return false if the
     * instruction has no template: an OP_UNDEFINED is added, so the
     * translator can fall back to an interpreter for it.
     */
    virtual bool lift(const OpcodePtr& instruction, SemanticBlock& block) const = 0;
};

// The reference countable object
typedef cSmartPtr<SemanticLifter> SemanticLifterPtr;

/*
 * Return the lifter of a processor
 */
class SemanticLifterFactory {
public:
    /*
     * Return a new lifter
     *
     * type - The processor of the instructions to lift
     *
     * Throw exception if the processor has no semantic description
     */
    static SemanticLifterPtr getLifter(OpcodeSubsystems::DisassemblerType type);
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICLIFTER_H
