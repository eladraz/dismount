#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICINTERPRETER_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICINTERPRETER_H

/*
 * SemanticInterpreter.h
 *
 * Execute semantic IR blocks over a register state and a memory. The
 * interpreter is the reference semantics of the IR: the lifters are tested by
 * running their IR against the real processor, and the lowerings by running
 * the target code against the interpreter.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticFloat.h"

/*
 * A value of the interpreter: up to MAX_BITS bits, stored as little endian
 * 64 bit words. The bits above the size of the value's type are zero.
 *
 * NOTE: This class is a value, it can be copied freely.
 */
class SemanticValue {
public:
    // The widest value (a zmm register)
    enum { MAX_BITS = 512, WORDS_COUNT = MAX_BITS / 64 };

    /*
     * Constructor
     *
     * value - The low 64 bits, the rest are 0
     */
    SemanticValue(uint64 value = 0);

    // Return the 64 bit word 'index'
    uint64 getWord(uint index) const;
    // Change the 64 bit word 'index'
    void setWord(uint index, uint64 value);

    /*
     * Return 'bits' bits (1 to 64) starting at bit 'offset'
     */
    uint64 getBits(uint offset, uint bits) const;

    /*
     * Change 'bits' bits (1 to 64) starting at bit 'offset' to the low bits
     * of 'value'
     */
    void setBits(uint offset, uint bits, uint64 value);

    /*
     * Clear the bits from bit 'bits' and up, so the value fits 'bits'
     */
    void truncate(uint bits);

    // Compare two values
    bool operator == (const SemanticValue& other) const;
    bool operator != (const SemanticValue& other) const;

private:
    // The words, the lowest first
    uint64 m_words[WORDS_COUNT];
};

/*
 * The memory of the interpreter.
 */
class SemanticMemory {
public:
    // Virtual destructor
    virtual ~SemanticMemory() {};

    /*
     * Read memory
     *
     * address - The guest address
     * buffer  - Will be filled with the bytes
     * size    - The number of bytes
     *
     * Return false if the address isn't readable (a guest memory fault)
     */
    virtual bool read(uint64 address, uint8* buffer, uint size) = 0;

    /*
     * Write memory
     *
     * Return false if the address isn't writable (a guest memory fault)
     */
    virtual bool write(uint64 address, const uint8* buffer, uint size) = 0;
};

/*
 * Execute semantic IR blocks.
 *
 * The state is the full registers of the architecture: reading and writing a
 * sub-register reads and writes its bits of the parent register (see
 * ArchitectureRegister). Every OP_SET_FLAGS is evaluated right away into the
 * flag registers of the architecture (ArchitectureDescription::
 * getFlagRegister()); flags the architecture doesn't have are dropped.
 *
 * The memory is accessed in the byte order of the architecture.
 *
 * Example:
 *    SemanticInterpreter interpreter(architecture, memory);
 *    interpreter.setRegister(ia32dis::IA32_REG_EAX, SemanticValue(5));
 *    if (interpreter.run(block) == SemanticInterpreter::EXIT_BRANCH)
 *        next = interpreter.getNextAddress();
 *
 * NOTE: This class is not thread-safe
 */
class SemanticInterpreter {
public:
    // Why run() stopped
    enum ExitReason {
        // The block branched to a guest address (OP_BRANCH, OP_CALL,
        // OP_RETURN...), see getNextAddress()
        EXIT_BRANCH,
        // The block ended without a branch, getNextAddress() is the fall
        // through address of the block
        EXIT_FALL_THROUGH,
        // An OP_TRAP, or an operation which raised a guest exception (a
        // division with BEHAVIOR_DIVIDE_FAULT): see getTrapKind() and
        // getTrapNumber(). getNextAddress() is the address of the guest
        // instruction which trapped
        EXIT_TRAP,
        // A memory access failed, see getFaultAddress(). getNextAddress()
        // is the address of the guest instruction which faulted
        EXIT_MEMORY_FAULT,
        // An OP_HELPER, see getHelperNumber(). The caller does the helper's
        // work and may continue with resume()
        EXIT_HELPER,
        // An OP_UNDEFINED, or an operation which the interpreter doesn't
        // implement yet. getNextAddress() is the guest instruction
        EXIT_UNDEFINED,
        // The steps limit was reached (an endless loop in the block)
        EXIT_STEPS_LIMIT
    };

    /*
     * Constructor. All the registers start as 0
     *
     * architecture - The processor of the blocks
     * memory       - The memory the blocks access
     */
    SemanticInterpreter(const ArchitectureDescriptionPtr& architecture,
                        SemanticMemory& memory);

    /*
     * Return the value of a register (a sub-register is read from its
     * parent)
     */
    SemanticValue getRegister(uint registerId) const;

    /*
     * Change a register (a sub-register changes only its bits of the parent,
     * unless it zero extends the parent)
     */
    void setRegister(uint registerId, const SemanticValue& value);

    /*
     * Execute a block, from its first instruction
     *
     * block    - The block, its registers are the architecture's
     * maxSteps - The maximum number of IR instructions to execute
     *
     * Return why the execution stopped. The program counter register is
     * set to getNextAddress()
     */
    ExitReason run(const SemanticBlock& block, uint maxSteps = 0x1000000);

    /*
     * Continue the execution after EXIT_HELPER
     *
     * result - The destination value of the OP_HELPER
     */
    ExitReason resume(const SemanticValue& result);

    // Return the guest address where execution continues, see ExitReason
    uint64 getNextAddress() const;
    // Return the SemanticOperation::TrapKind of EXIT_TRAP
    uint getTrapKind() const;
    // Return the trap number of EXIT_TRAP (the vector)
    uint64 getTrapNumber() const;
    // Return the address of EXIT_MEMORY_FAULT
    uint64 getFaultAddress() const;
    // Return the helper number and the arguments of EXIT_HELPER
    uint getHelperNumber() const;
    const SemanticValue& getHelperArgument(uint index) const;

    /*
     * Evaluate an operation over values, without a state (OP_LOAD, OP_STORE,
     * the branches and OP_SET_FLAGS are not values). Used by run() and by the
     * unit tests and the constant folding of the translator
     *
     * instruction - The operation, its type and attributes (the operands
     *               are not used)
     * sources     - The values of the sources
     * result      - Will be filled with the result
     *
     * Return false if the operation faults (a division by zero with
     * BEHAVIOR_DIVIDE_FAULT) or isn't implemented
     */
    static bool evaluate(const SemanticInstruction& instruction,
                         const SemanticValue* sources,
                         SemanticValue& result);

    /*
     * Evaluate the flags of an OP_SET_FLAGS recipe
     *
     * recipe - See SemanticOperation::FlagRecipe
     * type   - The type of the operation (its size is N)
     * s0..s2 - The operands and the result
     *
     * Return the SemanticFlags::Flag bits which are set
     */
    static uint32 evaluateFlags(SemanticOperation::FlagRecipe recipe,
                                const SemanticType& type,
                                const SemanticValue& s0,
                                const SemanticValue& s1,
                                const SemanticValue& s2);

    /*
     * Return the current flags as SemanticFlags::Flag bits, and evaluate a
     * condition on them (see SemanticOperation::Condition)
     */
    uint32 getFlags() const;
    static bool evaluateCondition(SemanticOperation::Condition condition,
                                  uint32 flags);

private:
    // What commitFloatFlags() decided
    enum FloatCommit {
        // Write the result
        FLOAT_COMMIT_OK,
        // An unmasked exception with a deferred trap (x87): don't write
        FLOAT_COMMIT_SUPPRESS,
        // An unmasked exception which traps now: EXIT_TRAP
        FLOAT_COMMIT_TRAP
    };

    // Build the floating point environment of an instruction: the rounding
    // mode, the precision control, FTZ and DAZ come from the guest control
    // register for ROUND_DYNAMIC
    void buildFloatEnvironment(const SemanticInstruction& instruction,
                               SemanticFloat::Environment& environment) const;
    /*
     * Record the exceptions an operation raised (with
     * ATTRIBUTE_FLOAT_EXCEPTIONS) in the status register, and trap on an
     * unmasked one. For FLOAT_COMMIT_TRAP the trap is set up, with the
     * address of the instruction at 'index'
     */
    FloatCommit commitFloatFlags(const SemanticInstruction& instruction,
                                 uint32 flags, uint index);

    // Execute from instruction 'index' of m_block
    ExitReason execute(uint index);
    // Read an operand
    SemanticValue read(const SemanticOperand& operand) const;
    // Write a register or a temporary
    void write(const SemanticOperand& operand, const SemanticValue& value,
               uint32 attributes);
    // Return the address of the guest instruction of IR instruction 'index'
    uint64 getGuestAddress(uint index) const;

    // The processor
    ArchitectureDescriptionPtr m_architecture;
    // The memory
    SemanticMemory& m_memory;
    // The full registers, by register id (the sub-register entries are not
    // used)
    cArray<SemanticValue> m_registers;
    // The temporaries of the current block
    cArray<SemanticValue> m_temporaries;
    // The instruction index of each label of the current block
    cArray<uint> m_labels;
    // The instructions of the current block, by index
    cArray<const SemanticInstruction*> m_instructions;
    // The current block (between run() and resume())
    const SemanticBlock* m_block;
    // The instruction to resume after a helper
    uint m_resumeIndex;
    // The steps left
    uint m_stepsLeft;
    // The results, see the getters
    uint64 m_nextAddress;
    uint m_trapKind;
    uint64 m_trapNumber;
    uint64 m_faultAddress;
    uint m_helperNumber;
    SemanticValue m_helperArguments[2];
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICINTERPRETER_H
