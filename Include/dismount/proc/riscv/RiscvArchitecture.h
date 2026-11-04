#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVARCHITECTURE_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVARCHITECTURE_H

/*
 * RiscvArchitecture.h
 *
 * The RISC-V (RV32 and RV64) description for the semantic layer: the
 * register ids, the floating point and vector control state and the
 * floating point environment.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"

namespace riscvdis {

/*
 * The register ids of RISC-V. The decoded operands of the instructions
 * (RiscvDecodedOperand::m_register) are these ids.
 *
 * The order of each group follows the encoding, so an id is the group base +
 * the register number (RISCV_REG_X0 + 10 is a0).
 *
 *   - x0-x31 are XLEN bits. x0 is the zero register (m_isZero: it reads 0
 *     and its writes are discarded).
 *   - f0-f31 are 64 bits (FLEN = 64, the D extension of every profile).
 *     The narrower values are NaN boxed: a single or half precision write
 *     sets the bits above it to ones, and a read of a narrower value which
 *     isn't properly boxed gives the canonical NaN. The boxing is done by
 *     the lifter (the registers have no sub-registers), see RiscvSemantics.h.
 *   - v0-v31 are VLEN bits (RiscvArchitecture::RiscvArchitecture()). A
 *     register group of LMUL = 2 / 4 / 8 is the consecutive registers; the
 *     lifter reads and writes them one by one (or by
 *     OP_READ_REGISTER_INDEXED), so VLEN is limited to SemanticValue::
 *     MAX_BITS, not VLEN * 8.
 *   - fcsr holds frm (bits 5-7) and fflags (bits 0-4), which are its
 *     sub-registers; vcsr holds vxrm (bits 1-2) and vxsat (bit 0) the same
 *     way.
 *   - vl, vtype and vstart are XLEN bits (vtype.vill is bit XLEN - 1); vlenb
 *     is a constant (VLEN / 8), not a register.
 *   - The reservation of lr / sc: the address and the size (0 when there is
 *     none), as the exclusive monitor of ARM.
 *   - The control flow integrity state: ssp (Zicfiss, XLEN bits) and elp
 *     (Zicfilp, 1 bit: a landing pad is expected at the target of the
 *     indirect jump which set it).
 */
enum {
    // The integer registers
    RISCV_REG_X0,
    RISCV_REG_X31 = RISCV_REG_X0 + 31,
    // The program counter (the address of the current instruction)
    RISCV_REG_PC,
    // The floating point registers
    RISCV_REG_F0,
    RISCV_REG_F31 = RISCV_REG_F0 + 31,
    // The vector registers
    RISCV_REG_V0,
    RISCV_REG_V31 = RISCV_REG_V0 + 31,
    // The floating point control and status register and its fields
    RISCV_REG_FCSR,
    RISCV_REG_FFLAGS,
    RISCV_REG_FRM,
    // The vector state
    RISCV_REG_VSTART,
    RISCV_REG_VCSR,
    RISCV_REG_VXSAT,
    RISCV_REG_VXRM,
    RISCV_REG_VL,
    RISCV_REG_VTYPE,
    // The reservation of lr / sc
    RISCV_REG_RESERVATION_ADDRESS,
    RISCV_REG_RESERVATION_SIZE,
    // The control flow integrity state
    RISCV_REG_SSP,
    RISCV_REG_ELP,
    // The number of registers. Must be last
    RISCV_REGISTERS_COUNT
};

// The ABI names of the registers
enum {
    RISCV_REG_ZERO = RISCV_REG_X0,
    RISCV_REG_RA = RISCV_REG_X0 + 1,
    RISCV_REG_SP = RISCV_REG_X0 + 2,
    RISCV_REG_GP = RISCV_REG_X0 + 3,
    RISCV_REG_TP = RISCV_REG_X0 + 4
};

} // namespace riscvdis

/*
 * The RISC-V description for the semantic layer.
 *
 *   - The address and the native integer are XLEN bits.
 *   - The vectors are VLEN bits (getMaximumVectorBits() is VLEN,
 *     isVectorLengthScalable() is true: the scalable types of the IR are
 *     instantiated with it). The element width and the grouping (vtype) and
 *     the length (vl) are registers read at runtime, see RiscvSemantics.h.
 *   - The memory model is weak (RVWMO).
 *   - There are no flags: the comparisons write integer registers.
 *     getFlagRegister() returns false for every flag.
 *   - The floating point environment is fcsr: frm bits 5-7 (0 rne, 1 rtz, 2
 *     rdn, 3 rup, 4 rmm; 5 and 6 are reserved and 7 is not a mode: an
 *     instruction which uses the dynamic mode then is an illegal
 *     instruction), fflags bits 0-4 (nx, uf, of, dz, nv). There are no
 *     traps (TRAP_NONE), no flush to zero, the tininess is detected after
 *     rounding, and every NaN result is the canonical NaN (NAN_DEFAULT,
 *     positive, the quiet bit only). The 3 bit rounding field with 5 modes
 *     is more than the 4 entries of FloatEnvironment::m_roundingModes, see
 *     FloatEnvironment.
 */
class RiscvArchitecture : public ArchitectureDescription {
public:
    /*
     * Constructor
     *
     * type       - The instruction set (XLEN)
     * vectorBits - VLEN: 128 to 512 (SemanticValue::MAX_BITS), a power of
     *              2. The default is the one of the test processors (QEMU's
     *              rva23u64 has VLEN = 128)
     * isControlFlowIntegrity - The environment runs with Zicfilp and Zicfiss
     *              enabled (the menvcfg / senvcfg bits, a choice of the
     *              supervisor): the lifter then models elp (every
     *              instruction but lpad traps while a landing pad is
     *              expected) and the shadow stack instructions. Without it
     *              lpad is a nop and the encodings of Zicfiss are the Zimop /
     *              Zcmop instructions which write 0 to rd, as on a processor
     *              whose supervisor didn't enable them
     */
    RiscvArchitecture(RiscvInstructionSet::DisassemblerTypes type =
                          RiscvInstructionSet::RISCV_RV64,
                      uint vectorBits = 128,
                      bool isControlFlowIntegrity = false);

    // Return the instruction set, XLEN and VLEN
    RiscvInstructionSet::DisassemblerTypes getInstructionSet() const;
    uint getXlen() const;
    uint getVectorBits() const;
    // Return true if Zicfilp and Zicfiss are enabled, see the constructor
    bool isControlFlowIntegrity() const;

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
    // Both the ABI names ("a0", "fa0", "fp") and the numeric ones ("x10",
    // "f10")
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
    // The instruction set and VLEN
    RiscvInstructionSet::DisassemblerTypes m_type;
    uint m_vectorBits;
    // See the constructor
    bool m_isControlFlowIntegrity;
    // The registers, indexed by the riscvdis::RISCV_REG_* ids. Built by the
    // constructor, since the sizes of x and v depend on XLEN and VLEN. The
    // names are static strings
    cArray<ArchitectureRegister> m_registers;
};

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVARCHITECTURE_H
