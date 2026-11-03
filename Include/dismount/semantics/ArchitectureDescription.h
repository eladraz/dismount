#ifndef __TBA_DISMOUNT_SEMANTICS_ARCHITECTUREDESCRIPTION_H
#define __TBA_DISMOUNT_SEMANTICS_ARCHITECTUREDESCRIPTION_H

/*
 * ArchitectureDescription.h
 *
 * Describes a processor to the semantic layer: its registers (and how the
 * sub-registers alias them), its flags, its sizes and the operations it can do
 * natively.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"

/*
 * The flags of the semantic IR. They are architecture independent: each
 * architecture maps them to its own flag registers (IA-32 EFLAGS, ARM NZCV).
 * An architecture which doesn't have a flag computes it when it's read.
 */
class SemanticFlags {
public:
    enum Flag {
        // Unsigned overflow (IA-32 CF, ARM C with the inverted borrow of a
        // subtraction, see ArchitectureDescription::isCarryInvertedOnSubtract)
        FLAG_CARRY      = 0x01,
        // The result is zero (IA-32 ZF, ARM Z)
        FLAG_ZERO       = 0x02,
        // The result is negative (IA-32 SF, ARM N)
        FLAG_SIGN       = 0x04,
        // Signed overflow (IA-32 OF, ARM V)
        FLAG_OVERFLOW   = 0x08,
        // The low byte of the result has an even number of bits (IA-32 PF)
        FLAG_PARITY     = 0x10,
        // Carry out of bit 3 (IA-32 AF)
        FLAG_ADJUST     = 0x20,
        // String instructions go backward (IA-32 DF)
        FLAG_DIRECTION  = 0x40,

        // The flags which the arithmetic operations define
        FLAGS_ARITHMETIC = FLAG_CARRY | FLAG_ZERO | FLAG_SIGN |
                           FLAG_OVERFLOW | FLAG_PARITY | FLAG_ADJUST
    };
};

/*
 * A register of an architecture.
 *
 * A sub-register is an alias of bits of its parent register:
 *     IA-32  al  = eax[0..7]    ah = eax[8..15]    ax = eax[0..15]
 *     ARM64  w0  = x0[0..31]    (writing w0 zeroes x0[32..63])
 *     SSE    xmm0 = ymm0[0..127]
 * Reading a sub-register reads its bits of the parent. Writing it changes only
 * its bits, unless m_zeroExtendsParent is set.
 */
struct ArchitectureRegister {
    // The register file of a register
    enum RegisterFile {
        // General purpose integer registers
        FILE_GENERAL,
        // The program counter (eip, pc)
        FILE_PROGRAM_COUNTER,
        // Single flag bits (CF, ZF, NZCV...), see m_flag
        FILE_FLAG,
        // Segment registers
        FILE_SEGMENT,
        // x87 stack registers
        FILE_X87,
        // Vector registers (mm, xmm, ymm, zmm, ARM v)
        FILE_VECTOR,
        // Vector predicates (AVX-512 k, SVE p)
        FILE_MASK,
        // Matrix registers (AMX tmm, SME za)
        FILE_MATRIX,
        // Control and status registers (cr0, MXCSR, FPCR...)
        FILE_CONTROL,
        // Debug registers
        FILE_DEBUG
    };

    // The name of the register, in lower case ("eax", "ah", "x0")
    const char* m_name;
    // The register file, see RegisterFile
    uint m_file;
    // The size of the register in bits
    uint m_bits;
    // The id of the parent register, or NO_PARENT for a full register
    uint m_parent;
    // The offset of the register inside its parent, in bits
    uint m_bitOffset;
    // Writing the register zeroes the rest of its parent (ARM64 w0, AVX
    // VEX-encoded writes of xmm). Otherwise the rest is kept
    bool m_zeroExtendsParent;
    // The SemanticFlags::Flag of a FILE_FLAG register, 0 otherwise
    uint32 m_flag;
    // The register is hard-wired to zero: it reads 0 and writes to it are
    // dropped (RISC-V x0, ARM64 xzr)
    bool m_isZero;

    // The m_parent value of a full register
    enum { NO_PARENT = 0xFFFFFFFF };
};

/*
 * Describes a processor to the semantic layer.
 *
 * The registers are referenced by their id: the index of the register in
 * the description (0 to getRegistersCount() - 1). The ids are stable, each
 * architecture defines them as an enum (see IA32Architecture.h).
 */
class ArchitectureDescription {
public:
    // The memory ordering the processor guarantees
    enum MemoryModel {
        // Total store order: only a store followed by a load to another
        // address may be reordered (IA-32)
        MEMORY_TOTAL_STORE_ORDER,
        // Weak ordering: accesses are ordered only by fences and
        // dependencies (ARM)
        MEMORY_WEAK
    };

    // Virtual destructor. You can inherit from me.
    virtual ~ArchitectureDescription() {};

    // Return the processor
    virtual OpcodeSubsystems::DisassemblerType getType() const = 0;

    // Return the size of an address in bits
    virtual uint getAddressBits() const = 0;
    // Return the size of the widest integer the processor computes natively
    virtual uint getNativeIntegerBits() const = 0;
    // Return the size of the widest vector register in bits, 0 for none.
    // For a scalable vector length, the minimum size
    virtual uint getMaximumVectorBits() const = 0;
    // Return true if the vector length is known only at runtime (SVE, RVV),
    // see SemanticType::isScalable()
    virtual bool isVectorLengthScalable() const = 0;
    // Return true for a little endian processor
    virtual bool isLittleEndian() const = 0;
    // Return the memory ordering the processor guarantees
    virtual MemoryModel getMemoryModel() const = 0;

    // Return the number of registers
    virtual uint getRegistersCount() const = 0;
    // Return the register 'registerId'
    virtual const ArchitectureRegister& getRegister(uint registerId) const = 0;
    /*
     * Find a register by its name
     *
     * name       - The name of the register
     * registerId - Will be filled with the id of the register
     *
     * Return true if the register exists
     */
    virtual bool findRegister(const cString& name, uint& registerId) const = 0;

    // Return the id of the stack pointer register
    virtual uint getStackPointer() const = 0;
    // Return the id of the program counter register
    virtual uint getProgramCounter() const = 0;

    /*
     * Return the register which selects the instruction set, for the
     * processors which have more than one (the ARM / Thumb T bit). A branch
     * which writes it switches the decoder of the next block
     *
     * registerId - Will be filled with the id of the mode register
     *
     * Return false if the processor has a single instruction set
     */
    virtual bool getModeRegister(uint& registerId) const = 0;

    /*
     * Return the id of the register which holds a flag
     *
     * flag       - A single SemanticFlags::Flag
     * registerId - Will be filled with the id of the flag register
     *
     * Return false if the processor doesn't have the flag (it's computed by
     * the translator when it's read)
     */
    virtual bool getFlagRegister(uint32 flag, uint& registerId) const = 0;

    /*
     * Return true if the carry flag of a subtraction is the inverted borrow
     * (ARM: C = 1 when there was no borrow). IA-32 returns false
     */
    virtual bool isCarryInvertedOnSubtract() const = 0;

    /*
     * Return true if the processor can do 'operation' on 'type' with a short
     * sequence of its own instructions. Used by the SemanticLegalizer to
     * decide which operations to split or expand.
     *
     * For example a 32 bit ARM returns false for OP_ADD on a 64 bit integer,
     * and NEON returns false for OP_DIVIDE_SIGNED on vectors.
     */
    virtual bool isSupported(SemanticOperation::Operation operation,
                             const SemanticType& type) const = 0;
};

// The reference countable object
typedef cSmartPtr<ArchitectureDescription> ArchitectureDescriptionPtr;

/*
 * Return the architecture description of a processor
 */
class ArchitectureDescriptionFactory {
public:
    /*
     * Return the description of a processor
     *
     * type - The processor
     *
     * Throw exception if the processor has no description
     */
    static ArchitectureDescriptionPtr getArchitecture(
                                OpcodeSubsystems::DisassemblerType type);
};

#endif // __TBA_DISMOUNT_SEMANTICS_ARCHITECTUREDESCRIPTION_H
