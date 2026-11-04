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
 * The floating point exceptions of the semantic IR (IEEE-754, plus the
 * denormal operand of IA-32 and ARM). Each architecture keeps them in its own
 * status register, see FloatEnvironment.
 */
class SemanticFloatFlags {
public:
    enum Flag {
        // An invalid operation (a NaN result from non NaN operands, a signaling
        // NaN operand, an out of range integer conversion)
        FLOAT_INVALID           = 0x01,
        // A denormal operand (IA-32 DE, ARM IDC)
        FLOAT_DENORMAL          = 0x02,
        // A division of a finite non zero number by zero
        FLOAT_DIVIDE_BY_ZERO    = 0x04,
        // The rounded result is too large
        FLOAT_OVERFLOW          = 0x08,
        // The result is tiny (and inexact, when the underflow is masked)
        FLOAT_UNDERFLOW         = 0x10,
        // The result was rounded
        FLOAT_INEXACT           = 0x20,

        // The number of flags
        FLOAT_FLAGS_COUNT       = 6
    };
};

/*
 * Where an architecture keeps its floating point environment: the rounding
 * mode, the exception flags and masks, and the rules for NaNs. Each field is
 * a bit offset in a register; NO_FIELD when the architecture has no such
 * field. The interpreter uses it for the operations with ROUND_DYNAMIC and
 * ATTRIBUTE_FLOAT_EXCEPTIONS.
 *
 *            control   rounding      status  flags            traps
 *   x87      fcw       bits 10-11    fsw     bits 0-5         deferred (#MF)
 *   SSE      mxcsr     bits 13-14    mxcsr   bits 0-5         #XM (19)
 *   ARM64    fpcr      bits 22-23    fpsr    bits 0-4, 7      (enable bits)
 *   RISC-V   fcsr      bits 5-7      fcsr    bits 0-4         none
 *
 * A field which an architecture doesn't have is NO_FIELD: the mask of an
 * exception (never trapping: the exception is masked) and the flag of an
 * exception (the exception is not recorded: RISC-V has no denormal operand
 * flag).
 */
struct FloatEnvironment {
    // The value of a field offset which doesn't exist
    enum { NO_FIELD = 0xFF };

    // How a NaN result is chosen when an operand is a NaN
    enum NanRule {
        // The first NaN operand, quieted (SSE). An invalid operation gives
        // m_defaultNan
        NAN_FIRST_OPERAND,
        // The NaN operand with the larger significand, quieted (x87)
        NAN_LARGER_SIGNIFICAND,
        // A signaling NaN operand first, then a quiet one, quieted (ARM
        // with FPCR.DN = 0)
        NAN_SIGNALING_FIRST,
        // Always m_defaultNan (ARM with FPCR.DN = 1, RISC-V)
        NAN_DEFAULT
    };

    // How an unmasked exception traps
    enum TrapMode {
        // Exceptions never trap
        TRAP_NONE,
        // The operation faults with m_trapNumber before writing its result
        // (SSE #XM)
        TRAP_IMMEDIATE,
        // The flags and m_summaryOffset are set, the result is not written,
        // and the next waiting instruction traps (x87 #MF, see the lifter)
        TRAP_DEFERRED
    };

    // The register of the control fields
    uint m_controlRegister;
    // The rounding field (m_roundingBits wide, 2 by default), and the
    // SemanticOperation::RoundingMode of each of its values. A value of the
    // field which is not a mode (the reserved ones of RISC-V frm, 5 to 7)
    // has an entry too (round to nearest even): the lifter of the processor
    // checks them before the operation
    uint8 m_roundingOffset;
    uint8 m_roundingBits;
    uint8 m_roundingModes[8];
    // The 2 bit precision control field of x87 (0: 24 bits, 2: 53 bits, 3:
    // 64 bits significand)
    uint8 m_precisionOffset;
    // Flush tiny results to zero (SSE FTZ, ARM FZ)
    uint8 m_flushToZeroOffset;
    // Treat denormal operands as zero (SSE DAZ)
    uint8 m_denormalsAreZeroOffset;
    // The mask (or enable) bit of each SemanticFloatFlags flag, by the bit
    // number of the flag
    uint8 m_maskOffsets[SemanticFloatFlags::FLOAT_FLAGS_COUNT];
    // True if a set mask bit disables the trap (IA-32), false if a set bit
    // enables it (ARM)
    bool m_isMaskSetDisabled;

    // The register of the sticky exception flags
    uint m_statusRegister;
    // The bit of each SemanticFloatFlags flag, by the bit number of the flag
    uint8 m_flagOffsets[SemanticFloatFlags::FLOAT_FLAGS_COUNT];
    // The summary bits set with a deferred trap (x87 ES and B)
    uint8 m_summaryOffset;
    uint8 m_busyOffset;

    // See TrapMode
    uint8 m_trapMode;
    // The trap number of TRAP_IMMEDIATE
    uint m_trapNumber;

    // See NanRule
    uint8 m_nanRule;
    // The sign of the default NaN (IA-32 "indefinite" is negative, ARM and
    // RISC-V are positive). The default NaN is the quiet NaN with only the
    // top significand bit set
    bool m_isDefaultNanNegative;

    // The bit of the control register which selects the default NaN for every
    // NaN result (ARM DN), NO_FIELD for none
    uint8 m_defaultNanOffset;
    // Tininess is detected before rounding (ARM), not after (IA-32)
    bool m_isTininessBeforeRounding;
    // A denormal operand flushed to zero raises the denormal flag (ARM IDC)
    bool m_isInputFlushSignaled;
    // A tiny result flushed to zero raises the inexact flag with the
    // underflow one (IA-32); ARM raises the underflow flag only
    bool m_isFlushInexact;

    // BEHAVIOR_MINMAX_NUMBER returns a NaN for a signaling NaN operand (ARM
    // FMAXNM, with any NaN rule)
    bool m_isMinMaxNanSignaling;
    // The bits of the control register of the half precision flush to zero
    // (ARM FZ16) and of the alternative half precision format (ARM AHP),
    // NO_FIELD for none. The half precision has its own flush control when it
    // is present
    uint8 m_flushToZeroHalfOffset;
    uint8 m_alternativeHalfOffset;
    // The NaN operand of a multiply-add is chosen in the order addend, first
    // and second multiplicand (ARM FPMulAdd)
    bool m_isAddendNanFirst;

    // The defaults of the fields above are the ones of IA-32
    FloatEnvironment() :
        m_roundingBits(2),
        m_defaultNanOffset(NO_FIELD),
        m_isTininessBeforeRounding(false),
        m_isInputFlushSignaled(false),
        m_isFlushInexact(true),
        m_isMinMaxNanSignaling(false),
        m_flushToZeroHalfOffset(NO_FIELD),
        m_alternativeHalfOffset(NO_FIELD),
        m_isAddendNanFirst(false)
    {
    }
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
     * Return the floating point environment of the operations
     *
     * attributes  - The SemanticInstruction attributes of the operation
     *               (IA-32 has one environment for x87, ATTRIBUTE_X87, and
     *               one for SSE)
     * environment - Will be filled with the environment
     *
     * Return false if the processor has no floating point
     */
    virtual bool getFloatEnvironment(uint32 attributes,
                                     FloatEnvironment& environment) const = 0;

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
