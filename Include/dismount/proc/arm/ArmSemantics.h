#ifndef __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICS_H
#define __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICS_H

/*
 * ArmSemantics.h
 *
 * The semantics of the ARM instructions (A32, T32 and A64): the template
 * tables and the lifter which instantiates them for decoded instructions.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/Opcode.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/SemanticLifter.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/Arm64Architecture.h"

namespace armdis {

/*
 * The runtime helpers of ARM (OP_HELPER s0). A helper works on the guest
 * state of the translator (or the interpreter's registers) directly.
 */
enum {
    // A64 mrs / msr of a system register which isn't a register of the
    // description (s1 is the packed o0:op1:CRn:CRm:op2): d = the value /
    // the value s2 is written
    ARM_HELPER_READ_SYSTEM_REGISTER,
    ARM_HELPER_WRITE_SYSTEM_REGISTER,
    // AArch32 mrc / mcr of a coprocessor register (s1: coproc:opc1:CRn:CRm:
    // opc2)
    ARM_HELPER_READ_COPROCESSOR,
    ARM_HELPER_WRITE_COPROCESSOR,
    // The reciprocal and reciprocal square root estimates (frecpe, frsqrte,
    // urecpe, ursqrte, vrecpe...): the tables of the Arm ARM are exact, so
    // the interpreter computes them; a translator may call the helper
    ARM_HELPER_RECIPROCAL_ESTIMATE,
    ARM_HELPER_RECIPROCAL_SQRT_ESTIMATE,
    // rndr / rndrrs: d = a random number, the flags NZCV = 0000 (0100 on a
    // failure)
    ARM_HELPER_RANDOM,
    // The SME2 lookup table and the other instructions which the IR has no
    // operation for (luti2 / luti4, the histogram of histcnt / histseg, the
    // bit permutes bdep / bext / bgrp): the whole instruction on the guest
    // state, s1 is the encoding
    ARM_HELPER_INSTRUCTION,
    // The number of helpers. Must be last
    ARM_HELPERS_COUNT
};

/*
 * The trap numbers of OP_TRAP (SemanticOperation::TrapKind gives the kind)
 */
enum {
    // svc #imm (A64, AArch32): the number is the immediate
    ARM_TRAP_SVC,
    // brk #imm / bkpt #imm
    ARM_TRAP_BREAKPOINT,
    // udf / an undefined instruction / an SME instruction outside of its
    // mode
    ARM_TRAP_UNDEFINED,
    // hlt #imm
    ARM_TRAP_HALT
};

} // namespace armdis

/*
 * The state of lifting one ARM instruction, and the operations the templates
 * and the hand written lifters use to emit its IR.
 *
 * The IR is added to the block given to the constructor. The memory operands
 * are resolved once; the writeback of the base (pre / post index) is added
 * by writeBack() after the access, so a load into the base register keeps
 * the order of the Arm ARM.
 *
 * NOTE: This class is not thread-safe
 */
class ArmLiftContext {
public:
    /*
     * Constructor
     *
     * instruction  - The decoded instruction
     * block        - The block to add the IR into
     * architecture - ArmArchitecture for A32 / T32, Arm64Architecture for
     *                A64 (the vector lengths)
     */
    ArmLiftContext(const ArmOpcode& instruction,
                   SemanticBlock& block,
                   const ArchitectureDescriptionPtr& architecture);

    // Return the decoded instruction / the block / the description
    const ArmOpcode& getInstruction() const;
    SemanticBlock& getBlock();
    const ArchitectureDescriptionPtr& getArchitecture() const;
    // Return true for A64
    bool isA64() const;

    // Return the number of operands
    uint getOperandsCount() const;
    // Return the decoded operand 'index'
    const ArmDecodedOperand& getOperand(uint index) const;
    // Return the type of operand 'index': the integer of a general purpose
    // register, the vector of a vector register (by its arrangement; the
    // scalable vector of a z / p register), the access of a memory operand
    // (from the mnemonic and the destination, see the lifter)
    SemanticType getOperandType(uint index) const;
    // Return the integer type of the native registers and addresses (32 or
    // 64)
    SemanticType getNativeType() const;

    /*
     * Return the value of operand 'index': the register (with its shift or
     * extend applied), the loaded memory, the immediate (as 'type'), the
     * branch target address
     *
     * Reading pc gives the AArch32 pc (the address + 8 in A32, + 4 in T32)
     */
    SemanticOperand read(uint index, const SemanticType& type);
    SemanticOperand read(uint index);

    /*
     * Write 'value' into operand 'index' (a register, or a store to memory).
     * Writing a w register zero extends the x; writing a SIMD scalar or
     * vector register zeroes the rest of the z register; writing pc in
     * AArch32 branches (with the interworking of bx for the loads and the A32
     * data processing, see the lifter)
     */
    void write(uint index, const SemanticOperand& value);

    /*
     * Return the address of memory operand 'index' (the base + the offset
     * for the offset and pre index forms, the base for post index)
     */
    SemanticOperand getAddress(uint index);

    /*
     * Write the base register of memory operand 'index' back, for the pre
     * and post index forms (nothing for the offset form)
     */
    void writeBack(uint index);

    // Return a register operand (its type is the register's size)
    SemanticOperand getRegister(uint registerId) const;
    // Return an immediate
    SemanticOperand getImmediate(int64 value, const SemanticType& type) const;
    // Return the address of the instruction / of the next instruction
    SemanticOperand getCurrentAddress() const;
    SemanticOperand getNextAddress() const;

    /*
     * Return the condition of the instruction as a SemanticOperation::
     * Condition on the flags (eq -> COND_EQUAL...)
     */
    SemanticOperation::Condition getCondition() const;
    // Convert an ARM condition code (0-15) into a SemanticOperation::Condition
    static SemanticOperation::Condition convertCondition(uint condition);

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

    // Add an OP_SET_FLAGS of N Z C V
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

    // Add an OP_TRAP
    void trap(SemanticOperation::TrapKind kind, uint number);

private:
    // The instruction, the block and the description
    const ArmOpcode& m_instruction;
    SemanticBlock& m_block;
    ArchitectureDescriptionPtr m_architecture;
    // The addresses of the memory operands, once computed
    SemanticOperand m_addresses[ArmOpcode::MAX_OPERANDS];
    bool m_isAddressComputed[ArmOpcode::MAX_OPERANDS];
};

/*
 * A hand written lifter of a mnemonic, for the instructions which a list of
 * steps doesn't describe well (ldm / stm, the structure loads, the SVE
 * predicated operations, the SME slices...)
 */
typedef void (*ArmLiftFunction)(ArmLiftContext& context);

namespace armdis {

/*
 * The semantic templates of the ARM mnemonics, terminated by an entry whose
 * name is NULL.
 *
 * The table is keyed by the mnemonic of the encoding entry
 * (ArmEncodingEntry::m_mnemonic, without the mnemonic parts), and the
 * number of operands (m_operandsCount). A32 and T32 share their tables (the
 * same mnemonics in the unified syntax); the condition of an AArch32
 * instruction is applied by the lifter around the template (a branch over
 * the instruction when the condition fails), so the templates don't handle
 * it.
 *
 * The encoding tables are not changed: a mnemonic without an entry is lifted
 * to OP_UNDEFINED and classified as CATEGORY_UNKNOWN, and its feature is
 * marked without SUPPORT_LIFT in ArmFeatures::getSupport().
 */
struct ArmSemanticEntry {
    // The number of operands of the decoded instruction this entry is for,
    // or ANY_OPERANDS_COUNT
    uint m_operandsCount;
    // The semantics. When m_lift is set, the steps are not used (NULL) and
    // the template gives the name and the classification only
    SemanticTemplate m_template;
    // The hand written lifter, or NULL
    ArmLiftFunction m_lift;

    // The data of the vector and the floating point instructions, for
    // OpcodeClassification::getDataType(), as in IA32SemanticEntry: the
    // SemanticType::Kind, the SemanticType::FloatFormat, the bits of an
    // element and the number of elements (0: from the arrangement of the
    // first operand, for the scalable vectors the minimum)
    uint8 m_dataKind;
    uint8 m_dataFormat;
    uint16 m_dataBits;
    uint16 m_dataLanes;

    enum { ANY_OPERANDS_COUNT = 0xFF };
};

/*
 * The tables, one per area, each terminated by an entry whose name is NULL.
 * ArmSemanticLifter::findEntry() searches the ones of the instruction set in
 * this order.
 */
// AArch32 integer, branch, load / store and system (ArmSemanticTable.cpp)
extern const ArmSemanticEntry gArmSemanticTable[];
// AArch32 VFP and Advanced SIMD (ArmSemanticTableSimd.cpp)
extern const ArmSemanticEntry gArmSimdSemanticTable[];
// A64 integer, branch, load / store and system (Arm64SemanticTable.cpp)
extern const ArmSemanticEntry gArm64SemanticTable[];
// A64 floating point and Advanced SIMD (Arm64SemanticTableSimd.cpp)
extern const ArmSemanticEntry gArm64SimdSemanticTable[];
// A64 SVE / SVE2 (Arm64SemanticTableSve.cpp)
extern const ArmSemanticEntry gArm64SveSemanticTable[];
// A64 SME / SME2 (Arm64SemanticTableSme.cpp)
extern const ArmSemanticEntry gArm64SmeSemanticTable[];

} // namespace armdis

/*
 * The lifter of the ARM processors.
 *
 * Semantics which the templates rely on:
 *   - AArch32: the condition wraps the instruction (see ArmSemanticEntry);
 *     reading pc gives the address + 8 (A32) / + 4 (T32); the writes of pc
 *     by a load (ldr pc, pop {pc}, ldm) and bx / blx interwork (bit 0 sets T),
 *     the A32 data processing writes of pc interwork from ARMv7 (the
 *     description is ARMv7 and up); the T bit of the block is the mode of
 *     the next block (ArchitectureDescription::getModeRegister()).
 *   - A64: register 31 is sp or zr as the operand says; w writes zero
 *     extend; the SIMD and floating point writes zero the rest of z; the
 *     floating point uses fpcr (rounding, FZ, DN) and sets fpsr's flags.
 *   - AArch32 Advanced SIMD uses the standard FPSCR value (round to nearest,
 *     flush to zero, default NaN) for its floating point, and sets fpscr's
 *     flags (and QC for the saturating instructions).
 *   - SVE: the scalable types are instantiated with VL of the
 *     Arm64Architecture (VL / SVL in the streaming mode, by SVCR.SM which
 *     the block's state gives; the lifter keeps a block in one mode, so
 *     smstart / smstop end the block).
 *   - SME: za is read and written by rows, see Arm64Architecture.h.
 */
class ArmSemanticLifter : public SemanticLifter {
public:
    /*
     * Constructor
     *
     * type         - DISASSEMBLER_ARM_32_LE, DISASSEMBLER_THUMB_LE or
     *                DISASSEMBLER_ARM_64_LE
     * architecture - The description to lift for (Arm64Architecture with
     *                the vector lengths), or a NULL pointer for the default
     *                one of the type
     */
    ArmSemanticLifter(OpcodeSubsystems::DisassemblerType type,
                      const ArchitectureDescriptionPtr& architecture =
                          ArchitectureDescriptionPtr());

    // See SemanticLifter
    virtual ArchitectureDescriptionPtr getArchitecture() const;
    virtual OpcodeClassification classify(const OpcodePtr& instruction) const;
    virtual bool lift(const OpcodePtr& instruction, SemanticBlock& block) const;

    /*
     * Return the semantic entry of a decoded instruction, or NULL if the
     * mnemonic has none
     */
    static const armdis::ArmSemanticEntry* findEntry(const ArmOpcode& instruction);

private:
    /*
     * Instantiate the steps of a template into the context
     */
    void instantiate(const SemanticTemplate& semanticTemplate,
                     ArmLiftContext& context) const;

    // The processor
    OpcodeSubsystems::DisassemblerType m_type;
    // The description
    ArchitectureDescriptionPtr m_architecture;
};

#endif // __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICS_H
