#ifndef __TBA_DISMOUNT_PROC_ARM_ARM64ARCHITECTURE_H
#define __TBA_DISMOUNT_PROC_ARM_ARM64ARCHITECTURE_H

/*
 * Arm64Architecture.h
 *
 * The AArch64 (A64) description for the semantic layer: the register ids,
 * the aliases of the general purpose, the SIMD and the scalable registers,
 * the flags and the floating point environment.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/arm/ArmArchitecture.h"

namespace armdis {

/*
 * The register ids of AArch64. The decoded operands of the A64 instructions
 * (ArmDecodedOperand::m_register) are these ids.
 *
 * The order of each group follows the encoding, so an id is the group base +
 * the register number (ARM64_REG_X0 + 30 is lr).
 *
 *   - w0-w30 are the low halves of x0-x30 and zero extend them when written.
 *   - Register number 31 is the stack pointer or the zero register,
 *     depending on the operand: sp / wsp (wsp is the low half of sp), xzr /
 *     wzr (m_isZero). The decoder gives the right id.
 *   - The flags N, Z, C, V are single registers (the NZCV system register is
 *     built from them).
 *   - v0-v31 (128 bits) are the low part of z0-z31; q, d, s, h and b are the
 *     low 128 / 64 / 32 / 16 / 8 bits of v. Every write of a SIMD and
 *     floating point register zeroes the bits above it, up to the top of z
 *     (writing d0 zeroes v0[64..127] and z0[128..VL-1]); m_zeroExtendsParent
 *     is set on all of them.
 *   - z0-z31 are VL bits, p0-p15 and ffr VL / 8 bits, see
 *     Arm64Architecture::Arm64Architecture(). In streaming mode (SME) the
 *     vector length is SVL instead of VL.
 *   - za is SVL x SVL bits, kept as SVL / 8 rows of SVL bits, the
 *     horizontal slices of the za.b tile. The tiles of the larger elements
 *     are rows of it (za1.s is the rows 1, 5, 9...) and their vertical
 *     slices are columns (OP_READ_REGISTER_INDEXED), see the lifter.
 *   - zt0 (SME2) is 512 bits.
 */
enum {
    // The 64 bit general purpose registers. x29 is the frame pointer, x30
    // the link register
    ARM64_REG_X0,
    ARM64_REG_X30 = ARM64_REG_X0 + 30,
    // The stack pointer, and the zero register
    ARM64_REG_SP,
    ARM64_REG_XZR,
    // The 32 bit halves, then wsp and wzr
    ARM64_REG_W0,
    ARM64_REG_W30 = ARM64_REG_W0 + 30,
    ARM64_REG_WSP,
    ARM64_REG_WZR,

    // The program counter (the address of the current instruction)
    ARM64_REG_PC,

    // The flags
    ARM64_REG_N, ARM64_REG_Z, ARM64_REG_C, ARM64_REG_V,

    // The scalable vector registers (VL bits), the parents of v
    ARM64_REG_Z0,
    ARM64_REG_Z31 = ARM64_REG_Z0 + 31,
    // The SIMD registers: v / q (128 bits, the same bits: v is the vector
    // name, q the scalar name), d, s, h, b
    ARM64_REG_V0,
    ARM64_REG_V31 = ARM64_REG_V0 + 31,
    ARM64_REG_Q0,
    ARM64_REG_Q31 = ARM64_REG_Q0 + 31,
    ARM64_REG_D0,
    ARM64_REG_D31 = ARM64_REG_D0 + 31,
    ARM64_REG_S0,
    ARM64_REG_S31 = ARM64_REG_S0 + 31,
    ARM64_REG_H0,
    ARM64_REG_H31 = ARM64_REG_H0 + 31,
    ARM64_REG_B0,
    ARM64_REG_B31 = ARM64_REG_B0 + 31,

    // The SVE predicates (VL / 8 bits) and the first fault register. The
    // predicate as counter registers pn0-pn15 of SME2 / SVE2.1 are the same
    // registers (the decoder gives the p id, with ArmDecodedOperand::
    // m_isPredicateCounter)
    ARM64_REG_P0,
    ARM64_REG_P15 = ARM64_REG_P0 + 15,
    ARM64_REG_FFR,

    // The rows of za (SVL bits each, ARM64_MAXIMUM_ZA_ROWS of them; the ones
    // from SVL / 8 and up are not used)
    ARM64_REG_ZA_ROW0,
    ARM64_REG_ZA_ROW_LAST = ARM64_REG_ZA_ROW0 + 63,
    // The SME2 lookup table register
    ARM64_REG_ZT0,

    // The system registers the user mode instructions use
    // The floating point control and status registers
    ARM64_REG_FPCR,
    ARM64_REG_FPSR,
    // The thread pointers (tpidr_el0, tpidrro_el0)
    ARM64_REG_TPIDR_EL0,
    ARM64_REG_TPIDRRO_EL0,
    // The streaming mode and za enable bits (SVCR.SM bit 0, SVCR.ZA bit 1)
    ARM64_REG_SVCR,
    // The exclusive monitor of ldxr / stxr: the address and the size of the
    // reservation (the size is 0 when there is none)
    ARM64_REG_EXCLUSIVE_ADDRESS,
    ARM64_REG_EXCLUSIVE_SIZE,
    // The branch type of bti (PSTATE.BTYPE)
    ARM64_REG_BTYPE,

    // The number of registers. Must be last
    ARM64_REGISTERS_COUNT
};

// The number of rows of za for the largest SVL (512 bits: 64 rows)
enum { ARM64_MAXIMUM_ZA_ROWS = 64 };

/*
 * Return the register id of a general purpose register
 *
 * number    - The register number, 0..31
 * is64      - True for x, false for w
 * isSpRegister - True if number 31 is the stack pointer (sp / wsp), false
 *             if it is the zero register (xzr / wzr)
 */
uint getArm64GeneralRegister(uint number, bool is64, bool isSpRegister);

/*
 * Return the register id of a SIMD and floating point register
 *
 * number - 0..31
 * bits   - 8, 16, 32, 64 for b, h, s, d; 128 for q; 0 for v (the vector
 *          name, 128 bits too)
 */
uint getArm64SimdRegister(uint number, uint bits);

} // namespace armdis

/*
 * The AArch64 description for the semantic layer.
 *
 *   - The address and the native integer are 64 bits.
 *   - The vectors are VL bits (getMaximumVectorBits() is the VL of the
 *     description, isVectorLengthScalable() is true: the scalable types of
 *     the IR are instantiated with it).
 *   - The memory model is weak.
 *   - The carry of a subtraction is the inverted borrow.
 *   - The floating point environment is fpcr / fpsr: the rounding mode is
 *     fpcr bits 22-23, FZ bit 24, DN bit 25, AH bit 1 and FIZ bit 0
 *     (FEAT_AFP, not modeled), the trap enables bits 8-12 and 15 (no
 *     processor dismount targets implements the traps: TRAP_NONE), the
 *     cumulative flags fpsr bits 0-4 and 7.
 */
class Arm64Architecture : public ArchitectureDescription {
public:
    /*
     * Constructor
     *
     * vectorBits          - VL, the SVE vector length outside of the
     *                       streaming mode: 128 to 512 (SemanticValue::
     *                       MAX_BITS), a power of 2
     * streamingVectorBits - SVL, the vector length in the streaming mode
     *                       and the size of za: 128 to 512, a power of 2
     *
     * The default is the one of the test processors: VL = 128 (Unicorn
     * runs SVE with the smallest vector), SVL = 512 (Apple M4)
     */
    Arm64Architecture(uint vectorBits = 128, uint streamingVectorBits = 512);

    // Return VL and SVL
    uint getVectorBits() const;
    uint getStreamingVectorBits() const;

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
    // VL and SVL
    uint m_vectorBits;
    uint m_streamingVectorBits;
    // The registers, indexed by the armdis::ARM64_REG_* ids. Built by the
    // constructor, since the sizes of z, p, ffr and the za rows depend on VL
    // and SVL. The names are static strings
    cArray<ArchitectureRegister> m_registers;
};

#endif // __TBA_DISMOUNT_PROC_ARM_ARM64ARCHITECTURE_H
