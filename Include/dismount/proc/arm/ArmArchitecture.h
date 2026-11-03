#ifndef __TBA_DISMOUNT_PROC_ARM_ARMARCHITECTURE_H
#define __TBA_DISMOUNT_PROC_ARM_ARMARCHITECTURE_H

/*
 * ArmArchitecture.h
 *
 * The AArch32 (A32 and T32) description for the semantic layer: the register
 * ids, the aliases of the floating point and Advanced SIMD registers, the
 * flags and the floating point environment.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/ArchitectureDescription.h"

namespace armdis {

/*
 * The register ids of AArch32. The decoded operands of the A32 and T32
 * instructions (ArmDecodedOperand::m_register) are these ids.
 *
 * The order of each group follows the encoding, so an id is the group base +
 * the register number (ARM_REG_R0 + 13 is sp).
 *
 *   - r13 is sp, r14 lr, r15 pc. Reading pc gives the address of the
 *     instruction + 8 in A32, + 4 in T32 (the lifter computes it, the pc
 *     register holds the address of the current instruction).
 *   - The flags N, Z, C, V, Q and GE[3:0] are single registers of FILE_FLAG
 *     (the APSR bits). The CPSR / APSR value of mrs / msr is built from them.
 *   - T (the Thumb bit) is the mode register (getModeRegister()). ITSTATE is
 *     not a register: the decoder applies the it block to the instructions.
 *   - s0-s31 are the halves of d0-d15, d0-d31 are the halves of q0-q15:
 *     s(2n) = d(n)[0..31], s(2n+1) = d(n)[32..63], d(2n) = q(n)[0..63],
 *     d(2n+1) = q(n)[64..127]. The full registers are q0-q15.
 *   - fpscr holds the rounding mode, the exception flags and the flags of
 *     vcmp (vmrs APSR_nzcv, fpscr copies them to N Z C V).
 */
enum {
    // The general purpose registers
    ARM_REG_R0, ARM_REG_R1, ARM_REG_R2,  ARM_REG_R3,
    ARM_REG_R4, ARM_REG_R5, ARM_REG_R6,  ARM_REG_R7,
    ARM_REG_R8, ARM_REG_R9, ARM_REG_R10, ARM_REG_R11,
    ARM_REG_R12, ARM_REG_SP, ARM_REG_LR, ARM_REG_PC,

    // The APSR flags
    ARM_REG_N, ARM_REG_Z, ARM_REG_C, ARM_REG_V,
    // The sticky saturation flag (qadd, ssat, smlabb...)
    ARM_REG_Q,
    // The greater than or equal flags of the parallel add and subtract
    ARM_REG_GE0, ARM_REG_GE1, ARM_REG_GE2, ARM_REG_GE3,
    // The Thumb bit, 1 in T32
    ARM_REG_T,
    // The endianness of the data (setend), 1 for big endian
    ARM_REG_E,

    // The 128 bit Advanced SIMD registers
    ARM_REG_Q0,
    ARM_REG_Q15 = ARM_REG_Q0 + 15,
    // The 64 bit registers, halves of q0-q15
    ARM_REG_D0,
    ARM_REG_D31 = ARM_REG_D0 + 31,
    // The 32 bit registers, halves of d0-d15
    ARM_REG_S0,
    ARM_REG_S31 = ARM_REG_S0 + 31,

    // The floating point status and control register
    ARM_REG_FPSCR,
    // The floating point exception register and the identity registers read
    // by vmrs (fpexc, fpsid, mvfr0-2): read only constants of the
    // description
    ARM_REG_FPEXC,
    ARM_REG_FPSID,
    ARM_REG_MVFR0, ARM_REG_MVFR1, ARM_REG_MVFR2,
    // The thread id register of user mode (mrc p15, 0, r0, c13, c0, 3)
    ARM_REG_TPIDRURO,
    // The read / write thread id register of user mode (c13, c0, 2)
    ARM_REG_TPIDRURW,

    // The number of registers. Must be last
    ARM_REGISTERS_COUNT
};

/*
 * The no register value of an operand (ArmDecodedOperand::m_index...), the
 * same in ArmArchitecture and Arm64Architecture
 */
enum { ARM_NO_REGISTER = 0xFFFF };

} // namespace armdis

/*
 * The AArch32 description for the semantic layer, for A32 and T32 (the T bit
 * selects between them, see getModeRegister()).
 *
 *   - The address and the native integer are 32 bits, the vectors 128 bits
 *     (q registers).
 *   - The memory model is weak.
 *   - The carry of a subtraction is the inverted borrow.
 *   - The floating point environment is fpscr: the rounding mode is bits
 *     22-23 (RN, RP, RM, RZ), FZ bit 24, DN bit 25 (NAN_DEFAULT), the
 *     cumulative flags IOC, DZC, OFC, UFC, IXC, IDC in bits 0-4 and 7, the
 *     trap enables in bits 8-12 and 15 (no processor implements the traps:
 *     TRAP_NONE). Advanced SIMD uses the "standard FPSCR value" (round to
 *     nearest, FZ and DN set) except for the flags, see the lifter.
 */
class ArmArchitecture : public ArchitectureDescription {
public:
    // Constructor
    ArmArchitecture();

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
    // The registers, indexed by the armdis::ARM_REG_* ids
    static const ArchitectureRegister gRegisters[armdis::ARM_REGISTERS_COUNT];
};

#endif // __TBA_DISMOUNT_PROC_ARM_ARMARCHITECTURE_H
