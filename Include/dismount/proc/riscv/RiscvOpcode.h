#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVOPCODE_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVOPCODE_H

/*
 * RiscvOpcode.h
 *
 * A decoded RISC-V instruction, with its operands decoded into structured
 * values (registers, immediates, addresses...).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvEncodingTable.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"

/*
 * A decoded operand of a RISC-V instruction. The register ids are the ones of
 * RiscvArchitecture.h. The fields which a kind doesn't use are 0.
 *
 * The formatter prints it (RiscvNotation) and the assembler parses the text
 * into it (RiscvAssembler), so a kind and its fields are enough to give the
 * text, together with the printing hints (m_isHex).
 */
struct RiscvDecodedOperand {
    enum Kind {
        // The operand doesn't exist
        KIND_NONE,
        // A register: m_register (an x, f or v register id)
        KIND_REGISTER,
        // An integer immediate: m_immediate
        KIND_IMMEDIATE,
        // The floating point immediate of fli: m_fliIndex, the index of the
        // value in gRiscvFliValues (RiscvOperandCodec.h)
        KIND_FLOAT_IMMEDIATE,
        // A memory operand "offset(base)": m_base, m_offset; m_hasOffset is
        // false for "(base)"
        KIND_MEMORY,
        // A pc relative target: the absolute address m_immediate (wrapped to
        // 32 bits in RV32; the address of the instruction is 0 when the
        // disassembler has no address). The assembler's operand is the offset
        // from the instruction (KIND_IMMEDIATE; m_name "." if it was written
        // with '.')
        KIND_BRANCH_TARGET,
        // A CSR: m_csr, its 12 bit number
        KIND_CSR,
        // A rounding mode: m_roundingMode (0 rne, 1 rtz, 2 rdn, 3 rup, 4
        // rmm, 7 dyn)
        KIND_ROUNDING_MODE,
        // A fence set: m_fenceSet, bit 3 i, bit 2 o, bit 1 r, bit 0 w
        KIND_FENCE_SET,
        // The vtype of vsetvli / vsetivli: m_vtype, the immediate bits
        KIND_VTYPE,
        // The mask operand "v0.t" (RISCV_KIND_VMASK with vm = 0)
        KIND_VECTOR_MASK,
        // An identifier the assembler could not classify by its syntax: a
        // CSR name (fflags), a rounding mode (rtz), a fence set (rw), a
        // vtype word (e32), a fli name (min, inf, 0.5) or a label. m_name holds
        // the lowercase text, and the codec resolves it for the spec it
        // encodes; m_immediate is 1 when the text had uppercase letters (LLVM
        // reads the rounding modes and the fence sets in lowercase only). The
        // disassembler never produces it
        KIND_NAME
    };

    // See Kind
    uint m_kind;
    // KIND_REGISTER: the register id
    uint m_register;
    // KIND_IMMEDIATE, KIND_BRANCH_TARGET: the value
    int64 m_immediate;
    // KIND_FLOAT_IMMEDIATE: see Kind
    uint m_fliIndex;
    // KIND_MEMORY: see Kind
    uint m_base;
    int64 m_offset;
    bool m_hasOffset;
    // KIND_CSR: see Kind
    uint m_csr;
    // KIND_ROUNDING_MODE: see Kind
    uint m_roundingMode;
    // KIND_FENCE_SET: see Kind
    uint m_fenceSet;
    // KIND_VTYPE: see Kind
    uint m_vtype;
    // KIND_NAME: the identifier, lowercase. KIND_MEMORY of the assembler: the
    // text of the offset ("(base)" accepts one integer token only)
    cString m_name;
    // Printing hints: print the immediate in hexadecimal (no instruction
    // sets it: the tests compare with llvm-objdump --no-print-imm-hex)
    bool m_isHex;
    // KIND_IMMEDIATE: m_immediate is an unsigned value, not a signed one
    bool m_isUnsigned;

    // Constructor: KIND_NONE, every field cleared
    RiscvDecodedOperand();
};

/*
 * A decoded instruction of a RISC-V instruction set.
 *
 * A compressed instruction is decoded as itself (its "c." entry of
 * gRiscvCompressedEncodingTable, with its own operands) and carries the 32
 * bit instruction it expands to (getExpansion()): the default formatter
 * prints the expansion as objdump does, and the lifter lifts it with the
 * size of the compressed instruction.
 */
class RiscvOpcode;
typedef cSmartPtr<RiscvOpcode> RiscvOpcodePtr;

class RiscvOpcode : public Opcode {
public:
    // The maximum number of operands (the mnemonic parts are not operands)
    enum { MAX_OPERANDS = riscvdis::RISCV_MAX_OPERAND_SPECS };

    /*
     * Constructor, by the disassembler
     *
     * type             - The instruction set
     * shouldUseAddress - Set to true if 'opcodeAddress' is valid
     * opcodeAddress    - The address of the instruction
     * encoding         - The bits (the 16 bit instruction in the low bits)
     * size             - 2 or 4
     * entry            - The encoding of the instruction
     */
    RiscvOpcode(RiscvInstructionSet::DisassemblerTypes type,
                bool shouldUseAddress,
                const ProcessorAddress& opcodeAddress,
                uint32 encoding,
                uint size,
                const riscvdis::RiscvEncodingEntry* entry);

    // See Opcode
    virtual OpcodeSubsystems::DisassemblerType getType() const;
    virtual cBuffer getOpcode() const;
    virtual uint getOpcodeSize() const;
    virtual bool getOpcodeAddress(ProcessorAddress& address) const;
    virtual bool isBranch() const;
    virtual bool isSwitch() const;
    virtual uint32 getSwitchTableOffset() const;
    virtual int getAlterProperty() const;

    // Return the instruction set
    RiscvInstructionSet::DisassemblerTypes getInstructionSet() const;
    // Return the bits of the instruction
    uint32 getEncoding() const;
    // Return the entry of the encoding table
    const riscvdis::RiscvEncodingEntry* getEntry() const;
    // Return the address of the instruction (0 if the disassembler has no
    // address)
    uint64 getAddress() const;
    // Return true for a 16 bit instruction
    bool isCompressed() const;

    /*
     * A compressed instruction: the decoded 32 bit instruction it expands to
     * (with the same address, its size is 4), or a NULL pointer for a 32
     * bit instruction
     */
    const RiscvOpcodePtr& getExpansion() const;
    void setExpansion(const RiscvOpcodePtr& expansion);

    /*
     * An instruction decoded from an alias entry (RISCV_ENTRY_ALIAS: li, mv,
     * ret...): the same bits decoded without the aliases ("addi a0, zero, 5"
     * for li), which the formatter of RISCV_FORMAT_NO_ALIASES prints. A NULL
     * pointer for the other instructions (they print the same either way)
     */
    const RiscvOpcodePtr& getNoAliasForm() const;
    void setNoAliasForm(const RiscvOpcodePtr& form);

    // Return the number of operands (not the mnemonic parts)
    uint getOperandsCount() const;
    // Return an operand, 0 is the first. Throw exception if it doesn't exist
    const RiscvDecodedOperand& getOperand(uint index) const;
    // Add an operand (the disassembler builds the opcode)
    void addOperand(const RiscvDecodedOperand& operand);

    /*
     * Return the full mnemonic, with the parts which come from the fields:
     * "amoadd.w.aqrl" (built by the disassembler)
     */
    const cString& getMnemonic() const;
    void setMnemonic(const cString& mnemonic);

private:
    // The instruction set
    RiscvInstructionSet::DisassemblerTypes m_type;
    // Set to true if m_opcodeAddress is valid
    bool m_shouldUseAddress;
    ProcessorAddress m_opcodeAddress;
    // The bits and the size in bytes
    uint32 m_encoding;
    uint m_size;
    // The encoding
    const riscvdis::RiscvEncodingEntry* m_entry;
    // See getExpansion() and getNoAliasForm()
    RiscvOpcodePtr m_expansion;
    RiscvOpcodePtr m_noAliasForm;
    // The operands
    RiscvDecodedOperand m_operands[MAX_OPERANDS];
    uint m_operandsCount;
    // See getMnemonic()
    cString m_mnemonic;
};

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVOPCODE_H
