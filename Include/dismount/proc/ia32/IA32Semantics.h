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
#include "dismount/proc/ia32/AMD64Architecture.h"
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
    // The x87 functions which are not exact (the results depend on the
    // implementation of the processor). A helper does the whole instruction
    // on the guest state: it checks the stack, computes the result, updates
    // the registers, the tags and the status word
    IA32_HELPER_X87_F2XM1,
    IA32_HELPER_X87_FYL2X,
    IA32_HELPER_X87_FYL2XP1,
    IA32_HELPER_X87_FPATAN,
    IA32_HELPER_X87_FPTAN,
    IA32_HELPER_X87_FSIN,
    IA32_HELPER_X87_FCOS,
    IA32_HELPER_X87_FSINCOS,
    // rcpps / rcpss: d = the estimate of 1 / s1 (the table of the processor
    // isn't public, so the interpreter doesn't run it)
    IA32_HELPER_RECIPROCAL_ESTIMATE,
    // rsqrtps / rsqrtss: d = the estimate of 1 / sqrt(s1)
    IA32_HELPER_RECIPROCAL_SQRT_ESTIMATE,
    // rdtscp: -> edx:eax and ecx (IA32_TSC_AUX)
    IA32_HELPER_RDTSCP,
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
     *               mode, where every address adds its segment base. 64 bit
     *               mode is always flat
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
    // Return the integer type of the operand size / address size (16, 32 or
    // 64)
    SemanticType getOperandSizeType() const;
    SemanticType getAddressSizeType() const;
    // Return the integer type of the stack accesses (32 in protected mode,
    // 16 in real mode, 64 in 64 bit mode)
    SemanticType getStackAddressType() const;
    // Return the integer type of the native registers and addresses (32, 64
    // in 64 bit mode)
    SemanticType getNativeType() const;
    // Return true in 64 bit mode
    bool is64BitMode() const;

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
    // Return the general purpose register 'number' (0 = eax..., 8..15 =
    // r8..r15 in 64 bit mode) of 8, 16, 32 or 64 bits. The 8 bit numbers 4..7
    // are spl..dil if the instruction has a REX prefix, see
    // ia32dis::getAmd64GeneralRegister
    SemanticOperand getGeneralRegister(uint number, uint bits) const;
    // Return the stack pointer / the instruction pointer of the processor
    // mode (esp / sp / rsp, eip / rip) as a register operand of the stack
    // address size / the native size
    SemanticOperand getStackPointer() const;
    SemanticOperand getInstructionPointer() const;
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

    // Push / pop a value of the type's size, on ss:esp (ss:sp in real mode,
    // rsp in 64 bit mode)
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

    // The data of the vector and the floating point instructions, for
    // OpcodeClassification::getDataType(). The entries which leave these
    // fields out work on the integer of the size of their first operand.
    // m_dataKind is a SemanticType::Kind (KIND_VOID: not given)
    uint8 m_dataKind;
    // The SemanticType::FloatFormat of a KIND_FLOAT element
    uint8 m_dataFormat;
    // The bits of one element
    uint16 m_dataBits;
    // The number of elements: 1 for a scalar, 0 for all the elements which
    // fill the vector register of the instruction (64 bits for mm, 128 bits
    // for xmm)
    uint16 m_dataLanes;

    enum { ANY_OPERANDS_COUNT = 0xFF };
};

/*
 * The tables, one per area, each terminated by an entry whose name is NULL.
 * IA32SemanticLifter::findEntry() searches them in this order.
 */
// The integer instructions (IA32SemanticTable.cpp)
extern const IA32SemanticEntry gIa32SemanticTable[];
// The segment registers, far branches and descriptor tables
// (IA32SemanticTableSegments.cpp)
extern const IA32SemanticEntry gIa32SegmentsSemanticTable[];
// The x87 instructions (IA32SemanticTableX87.cpp)
extern const IA32SemanticEntry gIa32X87SemanticTable[];
// The MMX, SSE and SSE2 instructions (IA32SemanticTableSimd.cpp)
extern const IA32SemanticEntry gIa32SimdSemanticTable[];
// The instructions of 64 bit mode which the IA-32 tables don't have, or
// which differ there (movsxd, cdqe, cqo, cmpxchg16b, swapgs, syscall,
// sysret, iretq, rdtscp...), see IA32SemanticTableAMD64.cpp. In 64 bit
// mode IA32SemanticLifter::findEntry() searches it first; in 16 and 32
// bit mode it is not used
extern const IA32SemanticEntry gAmd64SemanticTable[];

} // namespace ia32dis

/*
 * The IA-32 lifter, see SemanticLifter.
 *
 * How the decoded IA-32 details are lifted:
 *   - Memory operands: [base + index * scale + displacement] becomes OP_ADD
 *     and OP_SHIFT_LEFT on the address size, plus the segment base: fs and
 *     gs only with SEGMENTS_FLAT, every segment with SEGMENTS_FULL. The
 *     segment limits and access rights of the memory accesses are not
 *     checked (the segment loads check the descriptors).
 *   - Segment loads: base = selector * 16 in real mode; in protected mode
 *     the descriptor is read from the GDT / LDT (IA32_REG_GDTR_BASE...) and
 *     checked, raising #GP (13), #SS (12) or #NP (11). Far branches to a
 *     code segment of the same privilege are lifted; call gates, task
 *     switches and privilege changes are not (OP_UNDEFINED).
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
 *   - x87: ST(i) is the physical register (IA32_REG_FPU_TOP + i) mod 8,
 *     accessed with OP_READ/WRITE_REGISTER_INDEXED. The pushes and pops
 *     update IA32_REG_FPU_TOP and IA32_REG_FPU_TAG; a stack overflow or
 *     underflow is an invalid operation with SF and C1 set (or cleared); the
 *     masked response is the real indefinite. The arithmetic is
 *     FLOAT_EXTENDED with ATTRIBUTE_X87 (the precision and rounding control
 *     come from the control word); the unmasked faults leave the registers
 *     alone and set ES and B, a waiting instruction after them raises #MF
 *     (trap 16). See IA32SemanticTableX87.cpp. fxsave and fxrstor are
 *     there too (the SSE registers are read and written). The
 *     transcendental functions are OP_HELPER (IA32_HELPER_X87_*).
 *   - MMX: writing mm(i) writes the physical x87 register i with the
 *     exponent bits set; every instruction with an mm operand (even if it
 *     only reads it) sets TOP to 0 and all the tags valid; emms empties the
 *     tags.
 *   - SSE: the legacy encodings keep the upper lanes of the destination
 *     register in the scalar instructions. A 128 bit memory operand must be
 *     16 byte aligned (#GP) except for the unaligned moves. The floating
 *     point operations use MXCSR (rounding, FTZ, DAZ, the exception masks).
 *
 * 64 bit mode (DISASSEMBLER_AMD_64, with AMD64Architecture) reuses all the
 * above with the 64 bit details:
 *   - The registers: REX selects r8-r15, spl..dil, xmm8-xmm15. A write of a
 *     32 bit register zero extends it into the 64 bit register (the
 *     architecture does it, see AMD64Architecture); 8 and 16 bit writes
 *     don't.
 *   - The operand size is 64 with REX.W, and the default 64 bit operand size
 *     of the stack, the near branches and the others (ia32dis::Mode64). The
 *     shifts and rotates of a 64 bit operand mask their count to 6 bits.
 *   - The addresses are 64 bits (32 with the 67 prefix, zero extended).
 *     [rip + disp32] is the address of the next instruction + disp32. Only
 *     the fs and gs bases are added (the segment model is always flat).
 *   - The stack is rsp; push / pop / call / ret move 8 bytes (2 with 66).
 *   - The string instructions and loop use rcx, rsi, rdi (ecx, esi, edi with
 *     67); jrcxz tests rcx.
 *   - The segment loads (mov, pop fs / gs, lfs, lgs, lss) read the descriptor
 *     from the GDT / LDT with the checks of protected mode. The bases of
 *     es, cs, ss and ds are ignored; loading fs or gs sets its base register
 *     to the 32 bit base of the descriptor (0 for a null selector); the limit
 *     is not checked. A null ds, es, fs, gs is allowed, a null ss only at
 *     CPL 0 to 2. lfs, lgs, lss read m16:16, m16:32 and m16:64 (REX.W).
 *   - jmp / call far (m16:16/32/64), retf (retfq) and iretq / iretd go to a
 *     code segment of the same privilege: a 64 bit one (L = 1, no limit
 *     check, L and D together are #GP) or a 32 bit compatibility mode one
 *     (L = 0, D = 1, the offset is checked against the limit); the block ends
 *     there. iret pops rip, cs, rflags, rsp and ss in any case. Call gates,
 *     task switches, 16 bit code segments, a return to an outer privilege
 *     level and the 16 bit forms of retf / iret are OP_UNDEFINED.
 *   - syscall and sysret are traps (TRAP_SYSTEM_CALL); swapgs exchanges
 *     the gs base with AMD64_REG_KERNEL_GS_BASE.
 *   - The instructions which are invalid in 64 bit mode are not decoded
 *     (ia32dis::MODE64_INVALID), so they never reach the lifter.
 */
class IA32SemanticLifter : public SemanticLifter {
public:
    // Which segment bases the memory operands add
    enum SegmentModel {
        // SEGMENTS_FLAT for INTEL_32 and AMD_64, SEGMENTS_FULL for INTEL_16
        SEGMENTS_DEFAULT,
        // Only fs and gs have a base (the flat model of the 32 bit operating
        // systems, where fs / gs point to the thread data)
        SEGMENTS_FLAT,
        // Every segment has a base
        SEGMENTS_FULL
    };

    /*
     * Constructor
     *
     * type         - DISASSEMBLER_INTEL_16 (real mode),
     *                DISASSEMBLER_INTEL_32 (protected mode) or
     *                DISASSEMBLER_AMD_64 (64 bit mode)
     * segmentModel - See SegmentModel. Ignored in 64 bit mode (always
     *                SEGMENTS_FLAT)
     */
    IA32SemanticLifter(OpcodeSubsystems::DisassemblerType type,
                       SegmentModel segmentModel = SEGMENTS_DEFAULT);

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
    // True for SEGMENTS_FLAT
    bool m_isFlat;
    // The IA-32 description (AMD64Architecture in 64 bit mode)
    ArchitectureDescriptionPtr m_architecture;
};

#endif // __TBA_DISMOUNT_PROC_IA32_IA32SEMANTICS_H
