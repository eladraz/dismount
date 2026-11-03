#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICTEMPLATE_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICTEMPLATE_H

/*
 * SemanticTemplate.h
 *
 * The table format which describes the semantics of a processor: for each
 * mnemonic a short list of IR steps over the instruction's operands, and its
 * classification. A lifter instantiates the template of a decoded instruction
 * into SemanticInstructions.
 *
 * The same format is used in the other direction by the lowering of a target:
 * a template is then the IR pattern which a target instruction implements,
 * see SemanticLowering.h.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/OpcodeClassification.h"

/*
 * Where a template step reads a value from or writes it to.
 *
 * KIND_OPERAND is an operand of the decoded instruction (0 is the first
 * operand). Its kind is decided when the template is instantiated:
 *   - A register operand reads or writes the register.
 *   - A memory operand computes its address, and reading it is an OP_LOAD,
 *     writing it is an OP_STORE.
 *   - An immediate operand is an immediate.
 * So a single template covers every addressing form of the mnemonic
 * ('add eax, ebx', 'add [esi+ecx*4], 5', ...).
 */
struct SemanticTemplateReference {
    enum Kind {
        // Not in use
        KIND_NONE,
        // Operand m_index of the decoded instruction
        KIND_OPERAND,
        // The address of the memory operand m_index (lea, and for the steps
        // which read and write the same memory once)
        KIND_OPERAND_ADDRESS,
        // Temporary m_index of the template (0, 1, 2...)
        KIND_TEMPORARY,
        // The architecture register m_value (implicit operands: edx:eax of
        // mul, esp of push)
        KIND_REGISTER,
        // The constant m_value
        KIND_IMMEDIATE,
        // The size of the operation in bytes (push/pop of 2 or 4 bytes)
        KIND_OPERATION_BYTES,
        // The address of the next guest instruction (call, IA-32 relative
        // branches)
        KIND_NEXT_ADDRESS,
        // The address of the guest instruction itself (ARM adr and literal
        // loads, RISC-V auipc). m_value is added to it: 8 for the AArch32 pc,
        // 4 for the Thumb pc
        KIND_CURRENT_ADDRESS,
        // Label m_index of the template (a branch inside the instruction)
        KIND_LABEL
    };

    // See Kind
    uint8 m_kind;
    // The operand or temporary index
    uint8 m_index;
    // The register id or the constant
    int32 m_value;
};

/*
 * The type of a template step. Usually the type follows the decoded
 * instruction, so the same template serves all its operand sizes.
 */
struct SemanticTemplateType {
    enum Source {
        // The type is fixed: m_kind, m_bits and m_lanes
        SOURCE_FIXED,
        // An integer of the operand size of the instruction (16 or 32 bit
        // on IA-32, by the 66 prefix). Note: a byte instruction ('add al,
        // bl') has its operand size too, so SOURCE_OPERAND is usually the
        // right choice
        SOURCE_OPERAND_SIZE,
        // An integer of the address size of the instruction (67 prefix)
        SOURCE_ADDRESS_SIZE,
        // The type of operand m_bits of the decoded instruction
        SOURCE_OPERAND,
        // Twice the size of operand m_bits (the edx:eax product of mul)
        SOURCE_DOUBLE_OPERAND,
        // A 1 bit integer (conditions)
        SOURCE_BOOLEAN,
        // A vector of the size of operand m_lanes, whose elements are
        // m_kind of m_bits (paddw xmm: 8 lanes of i16). The vector size
        // comes from the encoding (VEX.L, EVEX.L'L, the NEON Q bit)
        SOURCE_VECTOR_OF_OPERAND,
        // A vector of operand m_lanes whose element size comes from the
        // encoding (the ARM size field, RVV SEW)
        SOURCE_VECTOR_OF_ELEMENT_SIZE
    };

    // See Source
    uint8 m_source;
    // The SemanticType::Kind of a SOURCE_FIXED type
    uint8 m_kind;
    // The element bits of a SOURCE_FIXED / SOURCE_VECTOR_OF_OPERAND type,
    // the operand index of a SOURCE_OPERAND / SOURCE_DOUBLE_OPERAND type
    uint16 m_bits;
    // The lanes of a SOURCE_FIXED type (1 for a scalar), the operand index
    // of the SOURCE_VECTOR_* types
    uint16 m_lanes;
};

/*
 * A single step of a template: one IR instruction.
 */
struct SemanticTemplateStep {
    // The SemanticOperation::Operation
    uint16 m_operation;
    // The type of the operation
    SemanticTemplateType m_type;
    // The destination
    SemanticTemplateReference m_destination;
    // The sources
    SemanticTemplateReference m_sources[3];
    // The SemanticOperation::Condition of OP_COMPARE, OP_TEST_FLAGS, or
    // SemanticTemplate::CONDITION_FROM_OPCODE
    uint8 m_condition;
    // The SemanticOperation::FlagRecipe of OP_SET_FLAGS
    uint8 m_flagRecipe;
    // The SemanticFlags::Flag bits of OP_SET_FLAGS
    uint32 m_flagMask;
};

/*
 * The semantics of a mnemonic.
 *
 * Example, the IA-32 'add' (all its forms):
 *
 *   static const SemanticTemplateStep gAddSteps[] = {
 *       // t0 = op0 + op1, in the size of operand 0 (al, ax or eax)
 *       { OP_ADD,  TYPE_OPERAND(0), TEMPORARY(0), { OPERAND(0), OPERAND(1), NONE } },
 *       // the flags of the addition
 *       { OP_SET_FLAGS, TYPE_OPERAND(0), NONE, { OPERAND(0), OPERAND(1), TEMPORARY(0) },
 *         COND_NONE, FLAGS_ADD, SemanticFlags::FLAGS_ARITHMETIC },
 *       // op0 = t0
 *       { OP_MOVE, TYPE_OPERAND(0), OPERAND(0), { TEMPORARY(0), NONE, NONE } }
 *   };
 *
 *   { "add", gAddSteps, 3, CATEGORY_ARITHMETIC, UNIT_INTEGER_ALU, 0,
 *     0, SemanticFlags::FLAGS_ARITHMETIC, 0 }
 *
 * When operand 0 is memory the lifter computes its address once, loads it for
 * the first step and stores it in the last one.
 *
 * The conditional mnemonics share one template (jcc, setcc, cmovcc): their
 * steps use CONDITION_FROM_OPCODE, and the lifter of the processor decodes
 * the condition from the instruction (on IA-32 the low 4 bits of 0x7x, 0F 8x,
 * 0F 9x, 0F 4x; on ARM the cond field).
 *
 * The mnemonic is the canonical one of the instruction, not an alias: ARM
 * 'cmp' is 'subs' to the zero register, RISC-V 'mv' is 'addi'.
 */
struct SemanticTemplate {
    // The m_condition of a step which takes its condition from the opcode
    enum { CONDITION_FROM_OPCODE = 0xFF };

    // The mnemonic, as in the opcode table of the processor (the name
    // conventions of the table, '#d', '##' and '/', are allowed)
    const char* m_name;
    // The steps
    const SemanticTemplateStep* m_steps;
    // The number of steps
    uint m_stepsCount;

    // The classification, see OpcodeClassification
    uint8 m_category;
    uint32 m_units;
    uint32 m_properties;
    // The SemanticFlags::Flag bits the mnemonic reads, writes and leaves
    // undefined
    uint32 m_flagsRead;
    uint32 m_flagsWritten;
    uint32 m_flagsUndefined;
    // The x87 status word bits (C0-C3, in fsw bit positions) that the
    // mnemonic leaves undefined. Trailing: 0 for the other architectures
    uint32 m_floatStatusUndefined;
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICTEMPLATE_H
