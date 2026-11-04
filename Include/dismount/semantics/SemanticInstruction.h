#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICINSTRUCTION_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICINSTRUCTION_H

/*
 * SemanticInstruction.h
 *
 * A single operation of the semantic IR (a "micro-operation"). A guest
 * instruction is lifted into one or more of them.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"

/*
 * A single operation of the semantic IR:
 *     destination = operation<type>(source0, source1, source2)
 *
 * The type is the type of the operation. For most operations it is also the
 * type of the destination and of the sources. The exceptions are documented
 * in SemanticOperation (OP_ZERO_EXTEND, OP_COMPARE, OP_LOAD...).
 *
 * Example, IA-32 'add eax, [ebx+4]':
 *     t0:i32 = add ebx, 4
 *     t1:i32 = load t0
 *     t2:i32 = add eax, t1
 *     set_flags<add> s0=eax s1=t1 s2=t2 mask=CZSOPA
 *     eax    = move t2
 *
 * NOTE: This class is a value, it can be copied freely.
 */
class SemanticInstruction {
public:
    // The maximum number of sources
    enum { MAX_SOURCES = 3 };

    // Additional properties of an operation
    enum Attribute {
        // No attributes
        ATTRIBUTE_NONE              = 0,
        // OP_NARROW saturates as signed
        ATTRIBUTE_SATURATE_SIGNED   = 0x0001,
        // OP_NARROW saturates as unsigned
        ATTRIBUTE_SATURATE_UNSIGNED = 0x0002,
        // The memory access can't be removed or merged (I/O memory)
        ATTRIBUTE_VOLATILE          = 0x0004,
        // The memory access may be unaligned
        ATTRIBUTE_UNALIGNED         = 0x0008,
        // Writing a sub-register zeroes the rest of its parent register,
        // like a register whose ArchitectureRegister::m_zeroExtendsParent is
        // set, for this write only (AVX VEX-encoded writes of xmm zero the
        // upper part of ymm, legacy SSE writes keep it)
        ATTRIBUTE_ZERO_PARENT       = 0x0010,
        // The operation follows the x87 rules (80 bit internal precision)
        ATTRIBUTE_X87               = 0x0020,
        // The operation may raise a guest exception (a division by zero, a
        // memory fault). Such operations are not reordered or removed
        ATTRIBUTE_MAY_FAULT         = 0x0040,
        // The floating point operation updates the cumulative exception
        // flags of the guest (MXCSR, FPSR, fflags)
        ATTRIBUTE_FLOAT_EXCEPTIONS  = 0x0080,
        // The floating point operation doesn't raise the denormal operand
        // exception (the x87 stores to a narrower format: fst m32, m64)
        ATTRIBUTE_IGNORE_DENORMAL   = 0x0100,
        // The floating point operation uses the "standard" environment
        // instead of the control register of the guest: round to nearest,
        // flush to zero, default NaN (the standard FPSCR value of the
        // AArch32 Advanced SIMD). The exception flags still accumulate in
        // the status register
        ATTRIBUTE_FLOAT_STANDARD    = 0x0200,
        // The half precision conversions of the operation are the IEEE ones
        // whatever the alternative half precision control of the guest says
        // (ARM FEAT_FHM: the widening multiply adds)
        ATTRIBUTE_FLOAT_IEEE_HALF   = 0x0400
    };

    /*
     * Default constructor. Construct an OP_NOP
     */
    SemanticInstruction();

    /*
     * Construct an operation
     *
     * operation   - The operation
     * type        - The type of the operation
     * destination - The destination (KIND_NONE if there isn't one)
     * source0..2  - The sources (KIND_NONE when not used)
     */
    SemanticInstruction(SemanticOperation::Operation operation,
                        const SemanticType& type,
                        const SemanticOperand& destination,
                        const SemanticOperand& source0 = SemanticOperand(),
                        const SemanticOperand& source1 = SemanticOperand(),
                        const SemanticOperand& source2 = SemanticOperand());

    // Return the operation
    SemanticOperation::Operation getOperation() const;
    // Return the type of the operation
    const SemanticType& getType() const;
    // Return the destination
    const SemanticOperand& getDestination() const;
    // Return the source 'index' (0 to MAX_SOURCES - 1)
    const SemanticOperand& getSource(uint index) const;

    // Change the destination
    void setDestination(const SemanticOperand& destination);
    // Change the source 'index'
    void setSource(uint index, const SemanticOperand& source);

    /*
     * The condition of OP_COMPARE, OP_TEST_FLAGS and of a conditional
     * OP_SET_FLAGS
     */
    SemanticOperation::Condition getCondition() const;
    void setCondition(SemanticOperation::Condition condition);

    /*
     * The recipe of OP_SET_FLAGS and the flags it defines (SemanticFlags::Flag
     * bits)
     */
    SemanticOperation::FlagRecipe getFlagRecipe() const;
    uint32 getFlagMask() const;
    void setFlags(SemanticOperation::FlagRecipe recipe, uint32 flagMask);

    /*
     * The flags (SemanticFlags::Flag bits) which a conditional OP_SET_FLAGS
     * sets when its condition is false; the flags of the mask which are not
     * in it are cleared (the #nzcv of ARM ccmp)
     */
    uint32 getFalseFlags() const;
    void setFalseFlags(uint32 flags);

    /*
     * The operation which OP_ATOMIC_RMW and OP_REDUCE apply, see
     * SemanticOperation
     */
    SemanticOperation::Operation getCombineOperation() const;
    void setCombineOperation(SemanticOperation::Operation operation);

    /*
     * The memory ordering of OP_FENCE and of the atomic operations
     */
    SemanticOperation::MemoryOrder getMemoryOrder() const;
    void setMemoryOrder(SemanticOperation::MemoryOrder order);

    /*
     * The accesses OP_FENCE orders (SemanticOperation::FenceAccess bits)
     */
    uint8 getFenceAccess() const;
    void setFenceAccess(uint8 access);

    /*
     * The behavior of the guest for the edge cases of the operation, see
     * SemanticOperation::Behavior
     */
    SemanticOperation::Behavior getBehavior() const;
    void setBehavior(SemanticOperation::Behavior behavior);

    /*
     * The kind of OP_TRAP, OP_HINT, OP_CACHE_MAINTENANCE, OP_FLOAT_ROUND and
     * the FLAGS_FLOAT_COMPARE OP_SET_FLAGS: a SemanticOperation::TrapKind,
     * HintKind, CacheOperation, FloatRoundKind or FloatCompareKind
     */
    uint getKind() const;
    void setKind(uint kind);

    /*
     * The rounding mode of the floating point operations
     */
    SemanticOperation::RoundingMode getRoundingMode() const;
    void setRoundingMode(SemanticOperation::RoundingMode mode);

    /*
     * The attributes, see Attribute
     */
    uint32 getAttributes() const;
    void setAttributes(uint32 attributes);

private:
    // See SemanticOperation::Operation
    uint16 m_operation;
    // See SemanticOperation::Condition
    uint8 m_condition;
    // See SemanticOperation::FlagRecipe
    uint8 m_flagRecipe;
    // See SemanticOperation::MemoryOrder
    uint8 m_memoryOrder;
    // See SemanticOperation::RoundingMode
    uint8 m_roundingMode;
    // See SemanticOperation::FenceAccess
    uint8 m_fenceAccess;
    // See SemanticOperation::Behavior
    uint8 m_behavior;
    // See getKind()
    uint8 m_kind;
    // The operation of OP_ATOMIC_RMW and OP_REDUCE
    uint16 m_combineOperation;
    // The flags OP_SET_FLAGS defines (SemanticFlags::Flag bits)
    uint32 m_flagMask;
    // See getFalseFlags()
    uint32 m_falseFlags;
    // See Attribute
    uint32 m_attributes;
    // The type of the operation
    SemanticType m_type;
    // The destination
    SemanticOperand m_destination;
    // The sources
    SemanticOperand m_sources[MAX_SOURCES];
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICINSTRUCTION_H
