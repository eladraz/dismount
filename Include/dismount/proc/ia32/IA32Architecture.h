#ifndef __TBA_DISMOUNT_PROC_IA32_IA32ARCHITECTURE_H
#define __TBA_DISMOUNT_PROC_IA32_IA32ARCHITECTURE_H

/*
 * IA32Architecture.h
 *
 * The IA-32 description for the semantic layer: the register ids, how the
 * sub-registers alias their parents, and the flags.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/ArchitectureDescription.h"

namespace ia32dis {

/*
 * The register ids of IA-32 in the semantic layer, see
 * ArchitectureDescription. The order of each group follows the encoding
 * (IA32_GP32_EAX is 0, ...), so a register id is the group base + the ModRM
 * register number.
 */
enum {
    // 32 bit general purpose registers
    IA32_REG_EAX, IA32_REG_ECX, IA32_REG_EDX, IA32_REG_EBX,
    IA32_REG_ESP, IA32_REG_EBP, IA32_REG_ESI, IA32_REG_EDI,
    // 16 bit aliases, bits 0..15 of the 32 bit registers
    IA32_REG_AX, IA32_REG_CX, IA32_REG_DX, IA32_REG_BX,
    IA32_REG_SP, IA32_REG_BP, IA32_REG_SI, IA32_REG_DI,
    // 8 bit aliases: al, cl, dl, bl are bits 0..7 of eax..ebx, ah, ch, dh, bh
    // bits 8..15
    IA32_REG_AL, IA32_REG_CL, IA32_REG_DL, IA32_REG_BL,
    IA32_REG_AH, IA32_REG_CH, IA32_REG_DH, IA32_REG_BH,

    // The instruction pointer
    IA32_REG_EIP,

    // The flags of EFLAGS, one register each (FILE_FLAG)
    IA32_REG_CF, IA32_REG_PF, IA32_REG_AF, IA32_REG_ZF,
    IA32_REG_SF, IA32_REG_DF, IA32_REG_OF,
    // The system bits of EFLAGS which user code can see (pushf, popf), one
    // bit each (FILE_CONTROL): interrupts enabled, alignment check, and the
    // cpuid detection bit
    IA32_REG_IF, IA32_REG_AC, IA32_REG_ID,

    // Segment registers (the selectors)
    IA32_REG_ES, IA32_REG_CS, IA32_REG_SS, IA32_REG_DS,
    IA32_REG_FS, IA32_REG_GS,
    // The base addresses of the segments (from the descriptor tables in
    // protected mode, selector * 16 in real mode), added to the addresses
    // of the memory operands, see IA32SemanticLifter
    IA32_REG_ES_BASE, IA32_REG_CS_BASE, IA32_REG_SS_BASE, IA32_REG_DS_BASE,
    IA32_REG_FS_BASE, IA32_REG_GS_BASE,

    // x87 stack registers, by their physical number (the top of stack is
    // IA32_REG_FPU_TOP, ST(i) is (top + i) mod 8)
    IA32_REG_FPR0, IA32_REG_FPR1, IA32_REG_FPR2, IA32_REG_FPR3,
    IA32_REG_FPR4, IA32_REG_FPR5, IA32_REG_FPR6, IA32_REG_FPR7,
    // The x87 control word, status word and the top of stack field
    IA32_REG_FPU_CONTROL, IA32_REG_FPU_STATUS, IA32_REG_FPU_TOP,

    // MMX registers, aliases of bits 0..63 of the x87 registers
    IA32_REG_MM0, IA32_REG_MM1, IA32_REG_MM2, IA32_REG_MM3,
    IA32_REG_MM4, IA32_REG_MM5, IA32_REG_MM6, IA32_REG_MM7,

    // SSE registers and their control register
    IA32_REG_XMM0, IA32_REG_XMM1, IA32_REG_XMM2, IA32_REG_XMM3,
    IA32_REG_XMM4, IA32_REG_XMM5, IA32_REG_XMM6, IA32_REG_XMM7,
    IA32_REG_MXCSR,

    // Control and debug registers
    IA32_REG_CR0, IA32_REG_CR2, IA32_REG_CR3, IA32_REG_CR4,
    IA32_REG_DR0, IA32_REG_DR1, IA32_REG_DR2, IA32_REG_DR3,
    IA32_REG_DR6, IA32_REG_DR7,

    // The number of registers. Must be last
    IA32_REGISTERS_COUNT
};

} // namespace ia32dis

/*
 * The IA-32 description for the semantic layer.
 *
 * In 16 bit mode (INTEL_16) the same registers are used: the operand and
 * address sizes of the instructions select the 16 bit aliases.
 */
class IA32Architecture : public ArchitectureDescription {
public:
    /*
     * Constructor
     *
     * type - DISASSEMBLER_INTEL_16 or DISASSEMBLER_INTEL_32
     */
    IA32Architecture(OpcodeSubsystems::DisassemblerType type);

    // See ArchitectureDescription
    virtual OpcodeSubsystems::DisassemblerType getType() const;
    virtual uint getAddressBits() const;
    virtual uint getNativeIntegerBits() const;
    virtual uint getMaximumVectorBits() const;
    virtual bool isVectorLengthScalable() const;
    virtual bool isLittleEndian() const;
    virtual MemoryModel getMemoryModel() const;
    virtual uint getRegistersCount() const;
    virtual const ArchitectureRegister& getRegister(uint registerId) const;
    virtual bool findRegister(const cString& name, uint& registerId) const;
    virtual uint getStackPointer() const;
    virtual uint getProgramCounter() const;
    virtual bool getModeRegister(uint& registerId) const;
    virtual bool getFlagRegister(uint32 flag, uint& registerId) const;
    virtual bool isCarryInvertedOnSubtract() const;
    virtual bool isSupported(SemanticOperation::Operation operation,
                             const SemanticType& type) const;

private:
    // DISASSEMBLER_INTEL_16 or DISASSEMBLER_INTEL_32
    OpcodeSubsystems::DisassemblerType m_type;

    // The registers, indexed by the ia32dis::IA32_REG_* ids
    static const ArchitectureRegister gRegisters[ia32dis::IA32_REGISTERS_COUNT];
};

#endif // __TBA_DISMOUNT_PROC_IA32_IA32ARCHITECTURE_H
