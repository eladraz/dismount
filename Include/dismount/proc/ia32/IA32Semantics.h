#ifndef __TBA_DISMOUNT_PROC_IA32_IA32SEMANTICS_H
#define __TBA_DISMOUNT_PROC_IA32_IA32SEMANTICS_H

/*
 * IA32Semantics.h
 *
 * The semantics of the IA-32 instructions: the template table and the lifter
 * which instantiates it for decoded instructions.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/Opcode.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/SemanticLifter.h"
#include "dismount/proc/ia32/IA32Opcode.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/IA32OperandDecoder.h"

namespace ia32dis {

/*
 * The runtime helpers of IA-32 (OP_HELPER s0). A helper works on the guest
 * state of the translator (or the interpreter's registers) directly.
 */
enum {
    // cpuid: eax, ecx -> eax, ebx, ecx, edx
    IA32_HELPER_CPUID,
    // rdtsc: -> edx:eax
    IA32_HELPER_RDTSC,
    // in: d = port s1, s2 is the size in bytes
    IA32_HELPER_IN,
    // out: port s1 = s2, the size is the type of the instruction
    IA32_HELPER_OUT,
    // The BCD adjusts (daa, das, aaa, aas): on al, ah and the flags
    IA32_HELPER_DECIMAL_ADJUST,
    // The number of helpers. Must be last
    IA32_HELPERS_COUNT
};

} // namespace ia32dis

/*
 * The state of lifting one IA-32 instruction, and the operations the
 * templates and the hand written lifters use to emit its IR.
 *
 * The IR is added to the block given to the constructor. The memory operands
 * are resolved once: reading an operand loads it, writing it stores to the
 * same address (read-modify-write instructions).
 *
 * NOTE: This class is not thread-safe
 */
class IA32LiftContext {
public:
    /*
     * Constructor
     *
     * instruction - The decoded instruction
     * block       - The block to add the IR into
     * isFlat      - True for the protected mode flat model: only the fs and
     *               gs bases are added to the addresses. False for real
     *               mode, where every address adds its segment base
     */
    IA32LiftContext(const IA32Opcode& instruction,
                    SemanticBlock& block,
                    bool isFlat);

    // Return the decoded instruction
    const IA32Opcode& getInstruction() const;
    // Return the block
    SemanticBlock& getBlock();

    // Return the number of explicit operands
    uint getOperandsCount() const;
    // Return the decoded operand 'index'
    const IA32DecodedOperand& getOperand(uint index) const;
    // Return the integer type of operand 'index' (its m_bits)
    SemanticType getOperandType(uint index) const;
    // Return the integer type of the operand size / address size (16 or 32)
    SemanticType getOperandSizeType() const;
    SemanticType getAddressSizeType() const;
    // Return the integer type of the stack accesses (32 in protected mode,
    // 16 in real mode)
    SemanticType getStackAddressType() const;

    /*
     * Return the value of operand 'index': the register, the loaded memory,
     * or the immediate (as 'type')
     */
    SemanticOperand read(uint index, const SemanticType& type);
    SemanticOperand read(uint index);

    /*
     * Write 'value' into operand 'index' (a register, or a store to memory)
     */
    void write(uint index, const SemanticOperand& value);

    /*
     * Return the address of memory operand 'index', with its segment base.
     * Computed once
     */
    SemanticOperand getAddress(uint index);

    /*
     * Return the offset of memory operand 'index' without the segment base
     * (lea)
     */
    SemanticOperand getOffset(uint index);

    /*
     * Return the linear address of an implicit memory operand (movs, push)
     *
     * segment - The segment register (ia32dis::IA32_REG_ES..GS)
     * offset  - The offset, of the address size
     */
    SemanticOperand getLinearAddress(uint segment, const SemanticOperand& offset);

    // Return a register operand (its type is the register's size)
    SemanticOperand getRegister(uint registerId) const;
    // Return the general purpose register 'number' (0 = eax...) of 8, 16 or
    // 32 bits
    SemanticOperand getGeneralRegister(uint number, uint bits) const;
    // Return an immediate
    SemanticOperand getImmediate(int64 value, const SemanticType& type) const;
    // Return the address of the instruction / of the next instruction
    SemanticOperand getCurrentAddress() const;
    SemanticOperand getNextAddress() const;

    // Return the condition of jcc, setcc, cmovcc (the low 4 bits of the
    // opcode)
    SemanticOperation::Condition getOpcodeCondition() const;

    // Return a new temporary / label
    SemanticOperand newTemporary(const SemanticType& type);
    SemanticOperand newLabel();

    /*
     * Add an operation whose destination is a new temporary of 'type', and
     * return the temporary
     */
    SemanticOperand emit(SemanticOperation::Operation operation,
                         const SemanticType& type,
                         const SemanticOperand& source0 = SemanticOperand(),
                         const SemanticOperand& source1 = SemanticOperand(),
                         const SemanticOperand& source2 = SemanticOperand());

    /*
     * Add an instruction as is (for the attributes, behaviors and kinds)
     */
    void emit(const SemanticInstruction& instruction);

    // Add 'destination = source' (a register or a temporary)
    void move(const SemanticOperand& destination, const SemanticOperand& source);
    // Add a load / a store
    SemanticOperand load(const SemanticOperand& address, const SemanticType& type);
    void store(const SemanticOperand& address, const SemanticOperand& value);

    // Add an OP_SET_FLAGS
    void setFlags(SemanticOperation::FlagRecipe recipe,
                  uint32 flagMask,
                  const SemanticType& type,
                  const SemanticOperand& source0,
                  const SemanticOperand& source1,
                  const SemanticOperand& result);
    // Set the flags of 'flagMask' to the 1 bit 'value' (FLAGS_EXPLICIT)
    void setFlags(uint32 flagMask, const SemanticOperand& value);
    // Return a 1 bit temporary of a condition on the flags (OP_TEST_FLAGS)
    SemanticOperand testFlags(SemanticOperation::Condition condition);

    // Add a label / a branch / a conditional branch (to a label or a guest
    // address)
    void placeLabel(const SemanticOperand& label);
    void branch(const SemanticOperand& target);
    void branchIf(const SemanticOperand& condition, const SemanticOperand& target);

    // Push / pop a value of the type's size, on ss:esp (ss:sp in real mode)
    void push(const SemanticOperand& value);
    SemanticOperand pop(const SemanticType& type);

    // Add an OP_TRAP
    void trap(SemanticOperation::TrapKind kind, uint number);

private:
    // The instruction
    const IA32Opcode& m_instruction;
    // The block
    SemanticBlock& m_block;
    // See constructor
    bool m_isFlat;
    // The decoded operands
    IA32DecodedOperand m_operands[3];
    uint m_operandsCount;
    // The addresses of the memory operands, once computed
    SemanticOperand m_addresses[3];
};

/*
 * A hand written lifter of a mnemonic, for the instructions which a list of
 * steps doesn't describe well (rep strings, div, shifts with a zero count,
 * pusha, enter, cmpxchg...)
 */
typedef void (*IA32LiftFunction)(IA32LiftContext& context);

namespace ia32dis {

/*
 * The semantic templates of the IA-32 mnemonics, terminated by an entry whose
 * name is NULL.
 *
 * The table is keyed by the mnemonic of the opcode table (gIa32OneByteOpcodeTable,
 * gIa32TwoBytesOpcodeTable, gIa32FPUOpcodeTable, with its '#d', '##' and '/'
 * conventions), not by the opcode entry: all the forms of a mnemonic share one
 * template, since the template operands follow the decoded operands (see
 * SemanticTemplateReference::KIND_OPERAND). A mnemonic whose forms differ
 * (imul with one, two or three operands) has one entry per operands count,
 * see m_operandsCount.
 *
 * The opcode table is not changed: a mnemonic without an entry is lifted to
 * OP_UNDEFINED and classified as CATEGORY_UNKNOWN.
 */
struct IA32SemanticEntry {
    // The number of operands of the decoded instruction this entry is for,
    // or ANY_OPERANDS_COUNT
    uint m_operandsCount;
    // The semantics. When m_lift is set, the steps are not used (NULL) and
    // the template gives the name and the classification only
    SemanticTemplate m_template;
    // The hand written lifter, or NULL
    IA32LiftFunction m_lift;

    enum { ANY_OPERANDS_COUNT = 0xFF };
};

extern const IA32SemanticEntry gIa32SemanticTable[];

} // namespace ia32dis

/*
 * The IA-32 lifter, see SemanticLifter.
 *
 * How the decoded IA-32 details are lifted:
 *   - Memory operands: [base + index * scale + displacement] becomes OP_ADD
 *     and OP_SHIFT_LEFT on the address size, plus the segment base (fs and
 *     gs only in the flat 32 bit model, every segment in 16 bit real mode).
 *   - The 66 and 67 prefixes select the operand and address sizes.
 *   - lock makes the read-modify-write of a memory operand atomic:
 *     OP_ATOMIC_RMW where the operation has a combine operation, otherwise
 *     an OP_ATOMIC_COMPARE_EXCHANGE loop. xchg with memory is always atomic.
 *   - rep / repe / repne wrap the string instruction in a loop on ecx (cx
 *     with a 16 bit address size) inside the block (OP_LABEL), with the
 *     direction taken from IA32_REG_DF.
 *   - The conditional mnemonics (jcc, setcc, cmovcc) take their condition
 *     from the opcode, see IA32LiftContext::getOpcodeCondition().
 *   - The shifts and rotates mask their count to 5 bits, and don't change
 *     the flags for a zero count (a branch over the OP_SET_FLAGS).
 *   - div / idiv raise the divide error (TRAP_FAULT 0) on a zero divisor
 *     or a quotient which doesn't fit, before any register is written.
 *   - x87: ST(i) is the physical register (IA32_REG_FPU_TOP + i) mod 8. The
 *     pushes and pops update IA32_REG_FPU_TOP.
 */
class IA32SemanticLifter : public SemanticLifter {
public:
    /*
     * Constructor
     *
     * type - DISASSEMBLER_INTEL_16 (real mode) or DISASSEMBLER_INTEL_32
     *        (protected mode, flat model)
     */
    IA32SemanticLifter(OpcodeSubsystems::DisassemblerType type);

    // See SemanticLifter
    virtual ArchitectureDescriptionPtr getArchitecture() const;
    virtual OpcodeClassification classify(const OpcodePtr& instruction) const;
    virtual bool lift(const OpcodePtr& instruction, SemanticBlock& block) const;

    /*
     * Return the semantic entry of a decoded instruction, or NULL if the
     * mnemonic has none
     */
    static const ia32dis::IA32SemanticEntry* findEntry(const IA32Opcode& instruction);

private:
    /*
     * Instantiate the steps of a template into the context
     */
    void instantiate(const SemanticTemplate& semanticTemplate,
                     IA32LiftContext& context) const;

    // The processor
    OpcodeSubsystems::DisassemblerType m_type;
    // The IA-32 description
    ArchitectureDescriptionPtr m_architecture;
};

#endif // __TBA_DISMOUNT_PROC_IA32_IA32SEMANTICS_H
