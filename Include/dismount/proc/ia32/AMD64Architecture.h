#ifndef __TBA_DISMOUNT_PROC_IA32_AMD64ARCHITECTURE_H
#define __TBA_DISMOUNT_PROC_IA32_AMD64ARCHITECTURE_H

/*
 * AMD64Architecture.h
 *
 * The AMD64 (x86-64) description for the semantic layer: the 64 bit register
 * ids, how the IA-32 registers alias them, and the flags.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/ia32/IA32Architecture.h"

namespace ia32dis {

/*
 * The register ids of 64 bit mode, after the IA-32 ones: the ids
 * 0..IA32_REGISTERS_COUNT-1 are the IA-32 registers (IA32_REG_EAX...), so the
 * IA-32 lifters work unchanged in 64 bit mode. In AMD64Architecture these
 * registers become the sub-registers of the 64 bit ones:
 *   - eax..edi are bits 0..31 of rax..rdi and zero extend them when written
 *     (ArchitectureRegister::m_zeroExtendsParent), ax..di bits 0..15, al..bl
 *     bits 0..7, ah..bh bits 8..15 (no zero extension).
 *   - eip is bits 0..31 of rip.
 *   - The segment bases are 64 bits. Only the fs and gs bases are used in 64
 *     bit mode (the es, cs, ss and ds bases are treated as 0).
 *   - The base of the descriptor tables and cr0..cr4, dr0..dr7 are 64 bits.
 *   - xmm0..xmm7, the x87 and MMX registers are the same.
 *
 * The order of each group follows the encoding with the REX bit (rax is 0,
 * r8 is 8), so an id is the group base + the register number.
 */
enum {
    // 64 bit general purpose registers
    AMD64_REG_RAX = IA32_REGISTERS_COUNT,
    AMD64_REG_RCX, AMD64_REG_RDX, AMD64_REG_RBX,
    AMD64_REG_RSP, AMD64_REG_RBP, AMD64_REG_RSI, AMD64_REG_RDI,
    AMD64_REG_R8,  AMD64_REG_R9,  AMD64_REG_R10, AMD64_REG_R11,
    AMD64_REG_R12, AMD64_REG_R13, AMD64_REG_R14, AMD64_REG_R15,
    // 32 bit aliases of r8..r15 (zero extend them), the ids of eax..edi are
    // IA32_REG_EAX..IA32_REG_EDI
    AMD64_REG_R8D,  AMD64_REG_R9D,  AMD64_REG_R10D, AMD64_REG_R11D,
    AMD64_REG_R12D, AMD64_REG_R13D, AMD64_REG_R14D, AMD64_REG_R15D,
    // 16 bit aliases of r8..r15
    AMD64_REG_R8W,  AMD64_REG_R9W,  AMD64_REG_R10W, AMD64_REG_R11W,
    AMD64_REG_R12W, AMD64_REG_R13W, AMD64_REG_R14W, AMD64_REG_R15W,
    // The 8 bit registers which need a REX prefix: bits 0..7 of rsp, rbp,
    // rsi, rdi and r8..r15
    AMD64_REG_SPL, AMD64_REG_BPL, AMD64_REG_SIL, AMD64_REG_DIL,
    AMD64_REG_R8B,  AMD64_REG_R9B,  AMD64_REG_R10B, AMD64_REG_R11B,
    AMD64_REG_R12B, AMD64_REG_R13B, AMD64_REG_R14B, AMD64_REG_R15B,

    // The instruction pointer
    AMD64_REG_RIP,

    // The SSE registers which need a REX prefix
    AMD64_REG_XMM8,  AMD64_REG_XMM9,  AMD64_REG_XMM10, AMD64_REG_XMM11,
    AMD64_REG_XMM12, AMD64_REG_XMM13, AMD64_REG_XMM14, AMD64_REG_XMM15,

    // The task priority register (mov cr8)
    AMD64_REG_CR8,
    // The gs base which swapgs exchanges with IA32_REG_GS_BASE (the
    // IA32_KERNEL_GS_BASE MSR)
    AMD64_REG_KERNEL_GS_BASE,

    // The number of registers. Must be last
    AMD64_REGISTERS_COUNT
};

/*
 * Return the register id of a general purpose register
 *
 * number - The register number, 0..15 (with the REX bit as bit 3)
 * bits   - 8, 16, 32 or 64
 * isRex  - True if the instruction has a REX prefix: the 8 bit numbers 4..7
 *          are spl, bpl, sil, dil, otherwise ah, ch, dh, bh
 *
 * In 16 and 32 bit mode 'number' is 0..7, 'bits' is not 64 and 'isRex' is
 * false: the ids are the IA-32 ones
 */
uint getAmd64GeneralRegister(uint number, uint bits, bool isRex);

/*
 * Return the register id of xmm 'number' (0..15)
 */
uint getAmd64SimdRegister(uint number);

} // namespace ia32dis

/*
 * The AMD64 description for the semantic layer (64 bit mode, the long mode
 * of a 64 bit code segment).
 *
 * The registers are the IA-32 ones plus the 64 bit ones, see
 * ia32dis::AMD64_REG_RAX. The memory model, the flags and the floating point
 * environments are the IA-32 ones.
 */
class AMD64Architecture : public ArchitectureDescription {
public:
    // Constructor
    AMD64Architecture();

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
    virtual bool getFloatEnvironment(uint32 attributes,
                                     FloatEnvironment& environment) const;
    virtual bool isSupported(SemanticOperation::Operation operation,
                             const SemanticType& type) const;

private:
    // The IA-32 description, for what is the same (the flags, the floating
    // point environments, the supported operations)
    IA32Architecture m_ia32;

    // The registers, indexed by the ia32dis::IA32_REG_* and AMD64_REG_* ids
    static const ArchitectureRegister gRegisters[ia32dis::AMD64_REGISTERS_COUNT];
};

#endif // __TBA_DISMOUNT_PROC_IA32_AMD64ARCHITECTURE_H
