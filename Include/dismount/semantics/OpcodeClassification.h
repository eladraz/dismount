#ifndef __TBA_DISMOUNT_SEMANTICS_OPCODECLASSIFICATION_H
#define __TBA_DISMOUNT_SEMANTICS_OPCODECLASSIFICATION_H

/*
 * OpcodeClassification.h
 *
 * What an instruction does, in short: its category, the execution units it
 * uses, its effects on memory, flags and stack, and the shape of the data it
 * works on. Cheaper than lifting to the IR, and enough for most analyses
 * (FlowMapper, register usage, scheduling, cost of a translation).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"

/*
 * The classification of an instruction.
 *
 * The classification of a processor's instruction comes from its
 * SemanticTemplate (see SemanticTemplate.h), so the table which describes the
 * semantics describes the classification too.
 *
 * NOTE: This class is a value, it can be copied freely.
 */
class OpcodeClassification {
public:
    // The main purpose of the instruction
    enum Category {
        // Not classified
        CATEGORY_UNKNOWN,
        // No operation (nop, pause, hints)
        CATEGORY_NOP,
        // Copy between registers, or a constant to a register
        CATEGORY_MOVE,
        // Read memory into registers
        CATEGORY_LOAD,
        // Write registers into memory
        CATEGORY_STORE,
        // Push, pop, enter, leave
        CATEGORY_STACK,
        // Integer add, subtract, negate, increment
        CATEGORY_ARITHMETIC,
        // And, or, xor, not
        CATEGORY_LOGIC,
        // Shifts and rotates
        CATEGORY_SHIFT,
        // Integer multiply
        CATEGORY_MULTIPLY,
        // Integer divide and remainder
        CATEGORY_DIVIDE,
        // Bit counts, bit tests, byte swap
        CATEGORY_BIT_MANIPULATION,
        // Compare or test, sets flags only
        CATEGORY_COMPARE,
        // Conditional move or set (cmov, setcc)
        CATEGORY_CONDITIONAL_MOVE,
        // Width or format conversions (movzx, cbw, cvtsi2sd)
        CATEGORY_CONVERT,
        // Jump, conditional or not
        CATEGORY_BRANCH,
        // Call
        CATEGORY_CALL,
        // Return
        CATEGORY_RETURN,
        // String instructions (movs, stos, cmps...)
        CATEGORY_STRING,
        // Scalar floating point (x87, SSE scalar, VFP)
        CATEGORY_FLOAT,
        // Vector integer operations
        CATEGORY_VECTOR_INTEGER,
        // Vector floating point operations
        CATEGORY_VECTOR_FLOAT,
        // Shuffles, interleaves, broadcasts, inserts and extracts
        CATEGORY_VECTOR_PERMUTE,
        // Cryptography (aes, sha, pclmulqdq)
        CATEGORY_CRYPTO,
        // Atomic read-modify-write and fences
        CATEGORY_ATOMIC,
        // Software interrupts, traps and system calls
        CATEGORY_INTERRUPT,
        // Port I/O
        CATEGORY_IO,
        // Processor state and privileged instructions (cpuid, rdtsc, lgdt...)
        CATEGORY_SYSTEM,
        // Matrix operations (AMX, SME)
        CATEGORY_MATRIX,
        // Prefetches and cache maintenance (prefetch, clflush, dc, cbo)
        CATEGORY_CACHE,
        // Control flow integrity and pointer protection (endbr, bti, pac,
        // mte, shadow stacks)
        CATEGORY_SECURITY
    };

    // The execution units an instruction uses (bit mask)
    enum ExecutionUnit {
        UNIT_NONE               = 0,
        UNIT_INTEGER_ALU        = 0x00000001,
        UNIT_SHIFTER            = 0x00000002,
        UNIT_INTEGER_MULTIPLY   = 0x00000004,
        UNIT_INTEGER_DIVIDE     = 0x00000008,
        UNIT_BRANCH             = 0x00000010,
        UNIT_LOAD               = 0x00000020,
        UNIT_STORE              = 0x00000040,
        UNIT_FLOAT_ADD          = 0x00000080,
        UNIT_FLOAT_MULTIPLY     = 0x00000100,
        UNIT_FLOAT_DIVIDE       = 0x00000200,
        UNIT_FLOAT_CONVERT      = 0x00000400,
        UNIT_VECTOR_INTEGER     = 0x00000800,
        UNIT_VECTOR_FLOAT       = 0x00001000,
        UNIT_VECTOR_PERMUTE     = 0x00002000,
        UNIT_CRYPTO             = 0x00004000,
        UNIT_SYSTEM             = 0x00008000,
        UNIT_MATRIX             = 0x00010000
    };

    // The effects of an instruction (bit mask)
    enum Property {
        PROPERTY_NONE               = 0,
        // Reads memory
        PROPERTY_READS_MEMORY       = 0x00000001,
        // Writes memory
        PROPERTY_WRITES_MEMORY      = 0x00000002,
        // Reads flags (getFlagsRead() has the flags)
        PROPERTY_READS_FLAGS        = 0x00000004,
        // Writes flags (getFlagsWritten() has the flags)
        PROPERTY_WRITES_FLAGS       = 0x00000008,
        // Changes the stack pointer (getStackDelta())
        PROPERTY_CHANGES_STACK      = 0x00000010,
        // Executed only when a condition is true (cmov, jcc)
        PROPERTY_CONDITIONAL        = 0x00000020,
        // Repeated by a prefix (rep movs)
        PROPERTY_REPEATED           = 0x00000040,
        // Atomic (lock prefix, xchg with memory)
        PROPERTY_ATOMIC             = 0x00000080,
        // Orders memory accesses (mfence, lock)
        PROPERTY_SERIALIZING        = 0x00000100,
        // Allowed in the kernel only
        PROPERTY_PRIVILEGED         = 0x00000200,
        // Uses registers that are not written in its operands (mul uses
        // edx:eax, movs uses esi/edi/ecx)
        PROPERTY_IMPLICIT_OPERANDS  = 0x00000400,
        // May raise a guest exception (div, memory accesses)
        PROPERTY_MAY_FAULT          = 0x00000800,
        // Its semantics need a runtime helper (OP_HELPER)
        PROPERTY_NEEDS_HELPER       = 0x00001000,
        // Leaves some flags undefined (getFlagsUndefined())
        PROPERTY_UNDEFINED_FLAGS    = 0x00002000,
        // Works on the active lanes of a mask only (AVX-512, SVE, RVV)
        PROPERTY_PREDICATED         = 0x00004000,
        // A valid target of an indirect branch (endbr32, bti)
        PROPERTY_LANDING_PAD        = 0x00008000,
        // May switch the instruction set (ARM bx, IA-32 far jumps)
        PROPERTY_CHANGES_MODE       = 0x00010000,
        // An exclusive access (ldxr/stxr, lr/sc)
        PROPERTY_EXCLUSIVE          = 0x00020000,
        // Its work depends on the runtime vector length (SVE, RVV)
        PROPERTY_VECTOR_LENGTH      = 0x00040000
    };

    /*
     * Default constructor. Construct an unclassified instruction
     */
    OpcodeClassification();

    /*
     * Construct a classification
     *
     * category   - The main purpose, see Category
     * units      - The ExecutionUnit bits
     * properties - The Property bits
     */
    OpcodeClassification(Category category, uint32 units, uint32 properties);

    // Return the main purpose of the instruction
    Category getCategory() const;
    // Return the ExecutionUnit bits
    uint32 getUnits() const;
    // Return the Property bits
    uint32 getProperties() const;

    // Return true if the instruction has all the 'property' bits
    bool hasProperty(uint32 property) const;

    /*
     * The flags the instruction reads, writes and leaves undefined
     * (SemanticFlags::Flag bits)
     */
    uint32 getFlagsRead() const;
    uint32 getFlagsWritten() const;
    uint32 getFlagsUndefined() const;
    void setFlags(uint32 read, uint32 written, uint32 undefined);

    /*
     * The type of the data the instruction works on: the element type and
     * the number of lanes of a vector instruction (paddw xmm is 8 lanes of
     * i16), or the operand size of a scalar one
     */
    const SemanticType& getDataType() const;
    void setDataType(const SemanticType& type);

    /*
     * The number of bytes the instruction moves the stack pointer: positive
     * for a push, negative for a pop, 0 if it doesn't change it or the
     * change isn't constant
     */
    int getStackDelta() const;
    void setStackDelta(int delta);

private:
    // See Category
    uint8 m_category;
    // See ExecutionUnit
    uint32 m_units;
    // See Property
    uint32 m_properties;
    // The flags read, written and left undefined
    uint32 m_flagsRead;
    uint32 m_flagsWritten;
    uint32 m_flagsUndefined;
    // The data the instruction works on
    SemanticType m_dataType;
    // The change of the stack pointer in bytes
    int m_stackDelta;
};

#endif // __TBA_DISMOUNT_SEMANTICS_OPCODECLASSIFICATION_H
