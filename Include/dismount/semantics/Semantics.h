#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICS_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICS_H

/*
 * Semantics.h
 *
 * The semantic layer describes what each instruction does, independent of the
 * processor which runs it. It is the base for binary translation (for example
 * running IA-32 code on ARM), for analysis (which registers, flags and memory
 * an instruction uses) and for optimizations.
 *
 * The flow of a translation:
 *
 *   bytes --> StreamDisassembler --> Opcode
 *         --> SemanticLifter     --> SemanticBlock (the IR, guest registers)
 *         --> SemanticLegalizer  --> SemanticBlock (only what the target has)
 *         --> SemanticLowering   --> AssemblerInterface (target assembly)
 *         --> FirstPassBinary / SecondPassBinary
 *
 * The IR is a list of SemanticInstruction ("micro-operations"). The basic
 * rules are:
 *   - The type (SemanticType) holds the size: the same OP_ADD adds 8 bit, 64
 *     bit or 128 bit integers and vectors of any lanes. Translating between
 *     machines of different sizes is done by the legalizer (a 64 bit OP_ADD
 *     on a 32 bit target becomes OP_ADD and OP_ADD_CARRY).
 *   - Memory is accessed only by OP_LOAD and OP_STORE. The addressing modes
 *     of the guest are lifted into arithmetic, and the lowering of the target
 *     folds them back into its own addressing modes.
 *   - Flags are lazy: OP_SET_FLAGS records how the flags are computed (a
 *     FlagRecipe and its operands), not their values. Only the flags that are
 *     read are materialized, and a target with similar flags (ARM NZCV) uses
 *     its own flag setting instructions.
 *   - Registers are described by an ArchitectureDescription. Sub-registers
 *     (al, ah, ax of eax) are aliases with a bit offset in their parent.
 *   - An operation that a target can't express inline is turned into
 *     OP_HELPER, a call into a runtime function.
 *
 * The semantics of a processor are table driven, like the opcode tables:
 * each mnemonic has a SemanticTemplate, see SemanticTemplate.h.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/SemanticLifter.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/semantics/SemanticLowering.h"
#include "dismount/semantics/SemanticInterpreter.h"

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICS_H
