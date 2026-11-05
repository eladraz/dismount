#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVSEMANTICS_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVSEMANTICS_H

/*
 * RiscvSemantics.h
 *
 * The semantics of the RISC-V instructions (RV32 and RV64): the template
 * tables and the lifter which instantiates them for decoded instructions.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/Opcode.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/SemanticLifter.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"

namespace riscvdis {

/*
 * The runtime helpers of RISC-V (OP_HELPER s0). A helper works on the guest
 * state of the translator (or the interpreter's registers) directly.
 */
enum {
    // csrr* of a CSR which isn't a register of the description (the
    // counters cycle / time / instret / hpmcounter*, seed, the supervisor,
    // hypervisor and machine CSRs; s1 is the CSR number): d = the value /
    // the value s2 is written. The CSRs of the description (fflags, frm,
    // fcsr, vstart, vxsat, vxrm, vcsr, vl, vtype, vlenb, ssp) are lifted
    // inline
    RISCV_HELPER_READ_CSR,
    RISCV_HELPER_WRITE_CSR,
    // The privileged instructions (mret, sret, wfi, sfence.vma, the
    // Svinval and H instructions): the whole instruction on the guest
    // state, s1 is the encoding
    RISCV_HELPER_PRIVILEGED,
    // The instructions which the IR has no operation for (the SM4 S-box of
    // sm4ed and sm4ks, and for the vector cryptography the SHA-2, SM3 and
    // SM4 rounds and the GHASH multiply, if the lifter doesn't lift them
    // inline): the whole instruction on the guest state, s1 is the
    // encoding. RiscvHelpers (RiscvHelpers.h) implements the ones of the
    // scalar lifter
    RISCV_HELPER_INSTRUCTION,
    // The SM4 S-box of the vector cryptography (vsm4k, vsm4r): d = the S-box
    // of each of the 4 bytes of the word s1 (the low 32 bits of the native
    // integer)
    RISCV_HELPER_SM4_SBOX,
    // The number of helpers. Must be last
    RISCV_HELPERS_COUNT
};

/*
 * The trap numbers of OP_TRAP (SemanticOperation::TrapKind gives the kind)
 */
enum {
    // ecall
    RISCV_TRAP_ECALL,
    // ebreak / c.ebreak
    RISCV_TRAP_BREAKPOINT,
    // An illegal instruction: the reserved encodings the decoder accepts,
    // a dynamic rounding mode of 5-7, a vector instruction with vtype.vill
    // set or a reserved vtype / register group, a CSR write to a read only
    // CSR
    RISCV_TRAP_ILLEGAL_INSTRUCTION,
    // Zicfilp / Zicfiss: a missing landing pad, a shadow stack mismatch
    // (the software check exception)
    RISCV_TRAP_SOFTWARE_CHECK,
    // An access fault raised by the instruction itself rather than by the
    // memory: an AMO which crosses a naturally aligned 16 byte region
    // (Zama16b allows the misaligned AMOs inside one only)
    RISCV_TRAP_ACCESS_FAULT
};

struct RiscvSemanticEntry;

} // namespace riscvdis

/*
 * The state of lifting one RISC-V instruction, and the operations the
 * templates and the hand written lifters use to emit its IR.
 *
 * The IR is added to the block given to the constructor.
 *
 * NOTE: This class is not thread-safe
 */
class RiscvLiftContext {
public:
    /*
     * Constructor
     *
     * instruction  - The decoded instruction (a compressed instruction is
     *                lifted by its expansion, with the size and the address
     *                of the compressed one; an alias by the general encoding
     *                it is an alias of, RiscvOpcode::getNoAliasForm()).
     *                The context resolves both: getInstruction(),
     *                getOperand() and getOperandsCount() are those of the
     *                resolved 32 bit instruction (the one whose entry has a
     *                semantic entry), getSize() is the size of the
     *                instruction given here
     * block        - The block to add the IR into
     * architecture - The RiscvArchitecture (XLEN, VLEN)
     */
    RiscvLiftContext(const RiscvOpcode& instruction,
                     SemanticBlock& block,
                     const ArchitectureDescriptionPtr& architecture);

    // Return the decoded instruction (resolved, see the constructor) / the
    // block / the description
    const RiscvOpcode& getInstruction() const;
    // Return the size of the instruction in bytes (2 for a compressed one,
    // whose expansion is getInstruction())
    uint getSize() const;
    SemanticBlock& getBlock();
    const ArchitectureDescriptionPtr& getArchitecture() const;
    // Return XLEN and VLEN
    uint getXlen() const;
    uint getVectorBits() const;
    // Return true if the description models Zicfilp and Zicfiss as enabled
    // (RiscvArchitecture::isControlFlowIntegrity())
    bool isControlFlowIntegrity() const;

    /*
     * The semantic entry the instruction is lifted by (set by the lifter
     * before it calls the hand written function) and its parameter: one
     * function serves a family of mnemonics (RiscvSemanticEntry::m_parameter)
     */
    void setEntry(const riscvdis::RiscvSemanticEntry* entry);
    uint64 getParameter() const;

    // Return the number of operands / the decoded operand 'index'
    uint getOperandsCount() const;
    const RiscvDecodedOperand& getOperand(uint index) const;
    // Return the integer type of XLEN
    SemanticType getNativeType() const;

    /*
     * Return the value of operand 'index': an integer register (XLEN bits),
     * the immediate (as 'type', sign extended), the branch target address.
     * x0 reads 0
     */
    SemanticOperand read(uint index, const SemanticType& type);
    SemanticOperand read(uint index);
    /*
     * Write 'value' into the integer register of operand 'index' (writes to
     * x0 are dropped). A 32 bit value written in RV64 by the w instructions
     * is sign extended by the caller (writeSignExtended())
     */
    void write(uint index, const SemanticOperand& value);
    void writeSignExtended(uint index, const SemanticOperand& value);

    /*
     * The floating point registers: read operand 'index' as a value of
     * 'format' (SemanticType::FloatFormat of 16 / 32 / 64 bits), unboxing it
     * (a narrower value whose upper bits aren't all ones reads as the
     * canonical NaN of the format); write a value, boxing it (the upper bits
     * set to ones)
     */
    SemanticOperand readFloat(uint index, const SemanticType& type);
    void writeFloat(uint index, const SemanticOperand& value);

    /*
     * Return the address of memory operand 'index': the base + the offset
     */
    SemanticOperand getAddress(uint index);

    // Return a register operand (its type is the register's size)
    SemanticOperand getRegister(uint registerId) const;
    // Return an immediate
    SemanticOperand getImmediate(int64 value, const SemanticType& type) const;
    // Return the address of the instruction / of the next instruction
    // (+ 2 for a compressed one)
    SemanticOperand getCurrentAddress() const;
    SemanticOperand getNextAddress() const;

    /*
     * The rounding mode of the instruction: the rm field of its encoding (the
     * decoder leaves the operand out when it holds the default of the text,
     * 'index' is ignored), or dyn when the encoding has none. A static
     * SemanticOperation::RoundingMode, or ROUND_DYNAMIC for dyn (frm, with
     * the check of the reserved values of frm: an illegal instruction). The
     * instructions whose result doesn't depend on the mode (fcvt.d.w, fcvt.d.s)
     * check it too, as the reference processors do
     */
    uint getRoundingMode(uint index) const;

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
    // Add an instruction as is (for the attributes, behaviors and kinds)
    void emit(const SemanticInstruction& instruction);
    // Add 'destination = source' (a register or a temporary)
    void move(const SemanticOperand& destination, const SemanticOperand& source);
    // Add a load / a store
    SemanticOperand load(const SemanticOperand& address, const SemanticType& type);
    void store(const SemanticOperand& address, const SemanticOperand& value);

    // Add a label / a branch / a conditional branch (to a label or a guest
    // address)
    void placeLabel(const SemanticOperand& label);
    void branch(const SemanticOperand& target);
    void branchIf(const SemanticOperand& condition, const SemanticOperand& target);
    // Add an OP_TRAP
    void trap(SemanticOperation::TrapKind kind, uint number);
    /*
     * Continue at the address in the register or the temporary 'target'
     * (OP_BRANCH_INDIRECT), as a call (OP_CALL: jal / jalr which write ra or
     * t0) or as a return (OP_RETURN: jalr x0, 0(ra) / jalr x0, 0(t0)), the
     * hints of the specification for the return address stack
     */
    void branchIndirect(const SemanticOperand& target);
    void call(const SemanticOperand& target);
    void returnTo(const SemanticOperand& target);

private:
    // The instruction given to the constructor, and the one it is lifted as
    // (the expansion, the general encoding of an alias, or the instruction
    // itself) kept by m_resolvedStorage when it isn't the given one
    const RiscvOpcode& m_instruction;
    RiscvOpcodePtr m_resolvedStorage;
    const RiscvOpcode* m_resolved;
    const riscvdis::RiscvSemanticEntry* m_entry;
    SemanticBlock& m_block;
    ArchitectureDescriptionPtr m_architecture;
    // The addresses of the memory operands, computed once
    SemanticOperand m_addresses[RiscvOpcode::MAX_OPERANDS];
    bool m_isAddressComputed[RiscvOpcode::MAX_OPERANDS];
};

/*
 * A hand written lifter of a mnemonic, for the instructions which a list of
 * steps doesn't describe well (the atomics, the conversions with their
 * saturation, every vector instruction)
 */
typedef void (*RiscvLiftFunction)(RiscvLiftContext& context);

namespace riscvdis {

/*
 * The semantic templates of the RISC-V mnemonics, terminated by an entry
 * whose name is NULL.
 *
 * The table is keyed by the mnemonic of the encoding entry
 * (RiscvEncodingEntry::m_mnemonic, without the mnemonic parts) and the
 * number of operands, as the ARM ones (ArmSemanticEntry). The compressed
 * instructions have no entries: they are lifted by their expansion. The
 * aliases have no entries either: the lifter finds the entry of the
 * general encoding (it decodes the instruction again without the aliases).
 *
 * An alias which has an entry of its own (lpad, sspush, sspopchk, ssrdp,
 * pause, fence.tso: the aliases whose semantics are not the ones of the
 * general encoding) is lifted by that entry. A compressed hint (the
 * encodings whose destination is x0, which have no expansion) has no entry:
 * the lifter lifts it as a nop.
 *
 * The encoding tables are not changed: a mnemonic without an entry is lifted
 * to OP_UNDEFINED and classified as CATEGORY_UNKNOWN, and its feature is
 * marked without SUPPORT_LIFT in RiscvFeatures::getSupport().
 */
struct RiscvSemanticEntry {
    // The number of operands of the decoded instruction this entry is for,
    // or ANY_OPERANDS_COUNT
    uint m_operandsCount;
    // The semantics. When m_lift is set, the steps are not used (NULL) and
    // the template gives the name and the classification only
    SemanticTemplate m_template;
    // The hand written lifter, or NULL
    RiscvLiftFunction m_lift;
    // The data of the vector and the floating point instructions, for
    // OpcodeClassification::getDataType(), as in ArmSemanticEntry: the
    // SemanticType::Kind, the SemanticType::FloatFormat, the bits of an
    // element and the number of elements (0: from vtype, at runtime)
    uint8 m_dataKind;
    uint8 m_dataFormat;
    uint16 m_dataBits;
    uint16 m_dataLanes;
    // A number the hand written lifter uses to tell the members of the
    // family of mnemonics it serves apart (RiscvLiftContext::getParameter()).
    // Zero when the entry doesn't need one (the tables may leave it out)
    uint64 m_parameter;

    enum { ANY_OPERANDS_COUNT = 0xFF };
};

/*
 * The tables, one per encoding table (the base, the vector and the
 * privileged ones; the compressed instructions use the base table through
 * their expansion), each terminated by an entry whose name is NULL
 */
// The scalar unprivileged instructions (RiscvSemanticTable.cpp)
extern const RiscvSemanticEntry gRiscvSemanticTable[];
// The vector instructions (RiscvSemanticTableVector.cpp): the table of the
// vector, the vector memory and the vector extension encoding tables
extern const RiscvSemanticEntry gRiscvVectorSemanticTable[];
// The privileged instructions (RiscvSemanticTablePrivileged.cpp)
extern const RiscvSemanticEntry gRiscvPrivilegedSemanticTable[];

} // namespace riscvdis

/*
 * The lifter of the RISC-V processors.
 *
 * Semantics which the templates rely on:
 *   - x0 reads 0 and its writes are dropped; the w instructions of RV64 sign
 *     extend their 32 bit result.
 *   - The integer divide by zero gives -1 (the quotient, all ones unsigned)
 *     and the dividend (the remainder); the signed overflow gives the
 *     dividend and 0. There are no integer traps.
 *   - The floating point: the rounding mode of the instruction or frm (dyn;
 *     a reserved frm is an illegal instruction), the exceptions accumulate
 *     in fflags, every NaN result is the canonical NaN, the narrower values
 *     are NaN boxed in f (RiscvLiftContext::readFloat / writeFloat). The
 *     conversions to integers saturate (NaN gives the largest positive
 *     value) and raise invalid; fmin / fmax return the number of a NaN and a
 *     number (minimumNumber, -0 < +0), fminm / fmaxm (Zfa) propagate the NaN.
 *     The environment comes from RiscvArchitecture::getFloatEnvironment();
 *     the behaviors which differ from the IA-32 and ARM ones are flags of
 *     FloatEnvironment (ArchitectureDescription.h), as the ARM lifter added
 *     its own.
 *   - lr / sc use the reservation registers of RiscvArchitecture (an sc
 *     writes 0 on success and 1 on a failure); the amos are OP_ATOMIC_RMW /
 *     OP_ATOMIC_COMPARE_EXCHANGE with the ordering of aq / rl.
 *   - jal / jalr: a call when rd is ra or t0, a return for jalr x0 of ra /
 *     t0 (the hints of the specification). The target of jalr has bit 0
 *     cleared. A misaligned target (bit 1 without a compressed feature) is
 *     not checked: the lifter assumes the compressed instructions.
 *   - Zicfilp: an indirect jump (but jalr through x7 / t2 and the returns)
 *     sets elp, and the next instruction must be an lpad whose label
 *     matches x7[31:12] (0 matches any), else a software check exception.
 *     Zicfiss: sspush / sspopchk / ssrdp / ssamoswap use ssp; a mismatch
 *     of sspopchk is a software check exception. Without the enables of
 *     the CFI (the senvcfg bits, a supervisor's choice) they are Zimop /
 *     Zcmop instructions which write 0 and lpad is a nop. The enabled
 *     behavior is lifted when RiscvArchitecture::isControlFlowIntegrity()
 *     is true: every instruction but lpad then begins with the check of
 *     elp (the fetch of the instruction after an indirect jump traps when
 *     it isn't a landing pad); see tests/riscv/semantics/TEST_PLAN.md for
 *     how the oracles are configured.
 *   - The memory accesses are byte accurate and may be misaligned (Zicclsm);
 *     a misaligned AMO, lr or sc completes when the access stays inside an
 *     aligned 16 byte region (Zama16b) and raises an access fault
 *     (RISCV_TRAP_ACCESS_FAULT) when it crosses one.
 *   - The vector instructions: vtype and vl are registers read at runtime,
 *     not values the lifter knows (the same instruction runs with any
 *     vtype). Each vector instruction checks vtype.vill and the reserved
 *     combinations of its SEW, LMUL and register group alignment (an
 *     illegal instruction), then works on the elements vstart to vl - 1:
 *     the active ones (by v0 when masked) get the result, the inactive and
 *     the tail ones are left undisturbed (the agnostic policies allow it,
 *     and the reference processors are configured so; see
 *     tests/riscv/semantics/TEST_PLAN.md), and vstart is set to 0 at the end. The
 *     loads and stores are element by element in order, so a fault on an
 *     element sets vstart to its index (the fault only first loads set vl
 *     instead). The SEW / LMUL dispatch and the loop over the elements or
 *     the registers of a group are the lifter's choice (see
 *     RiscvSemanticVector.h); an optimizer can specialize them when vtype
 *     is known. vsetvl{i} sets vl = min(AVL, VLMAX); a reserved vtype
 *     (vlmul 4, vsew above 3, SEW > ELEN * min(LMUL, 1), any bit above bit 7,
 *     altfmt included) is vill with vl = 0, and so is the keep-vl form
 *     (rd = rs1 = x0) whose new VLMAX differs from the current one (as
 *     Spike; QEMU keeps vl when it fits). The reductions, vcpop.m, vfirst.m,
 *     vmsbf.m, vmsif.m, vmsof.m, viota.m and vcompress.vm raise illegal
 *     instruction when vstart != 0, the other instructions work from vstart.
 *     A fault only first segment load is atomic (every field is loaded
 *     before one is written).
 *   - The privileged instructions are helpers (RISCV_HELPER_PRIVILEGED),
 *     the CSRs outside of the description too (RISCV_HELPER_READ_CSR).
 */
class RiscvSemanticLifter : public SemanticLifter {
public:
    /*
     * Constructor
     *
     * type         - DISASSEMBLER_RISCV_32 or DISASSEMBLER_RISCV_64
     * architecture - The description to lift for (RiscvArchitecture with
     *                VLEN), or a NULL pointer for the default one of the
     *                type
     */
    RiscvSemanticLifter(OpcodeSubsystems::DisassemblerType type,
                        const ArchitectureDescriptionPtr& architecture =
                            ArchitectureDescriptionPtr());

    // See SemanticLifter
    virtual ArchitectureDescriptionPtr getArchitecture() const;
    virtual OpcodeClassification classify(const OpcodePtr& instruction) const;
    virtual bool lift(const OpcodePtr& instruction, SemanticBlock& block) const;

    /*
     * Return the semantic entry of a decoded instruction (of its expansion
     * for a compressed one, of the general encoding for an alias), or NULL
     * if the mnemonic has none. The table searched is the one of the
     * encoding table of the entry
     */
    static const riscvdis::RiscvSemanticEntry* findEntry(const RiscvOpcode& instruction);

    /*
     * Return the instruction which is lifted for 'instruction': the
     * expansion of a compressed one, its getNoAliasForm() for an alias (unless
     * the alias has an entry of its own, see RiscvSemanticEntry), or
     * 'instruction' itself (also a compressed hint, which has no expansion). A pointer to the
     * result is kept in 'storage' when it is not 'instruction'. Return a
     * NULL pointer if the instruction can't be resolved
     */
    static const RiscvOpcode* resolve(const RiscvOpcode& instruction,
                                      RiscvOpcodePtr& storage);

private:
    /*
     * Instantiate the steps of a template into the context
     */
    void instantiate(const SemanticTemplate& semanticTemplate,
                     RiscvLiftContext& context) const;

    // The processor
    OpcodeSubsystems::DisassemblerType m_type;
    // The description
    ArchitectureDescriptionPtr m_architecture;
};

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVSEMANTICS_H
