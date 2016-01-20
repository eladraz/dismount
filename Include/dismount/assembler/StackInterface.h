#ifndef __TBA_DISMOUNT_STACKINTERFACE_H
#define __TBA_DISMOUNT_STACKINTERFACE_H

/*
 * StackInterface.h
 *
 * Provide interface for stack registers activities.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/hash.h"
#include "xStl/data/smartptr.h"

// The stack position index
// NOTE: Registers are marked as negative numbers.
// NOTE: Stack based variables starts from LOCAL_STACK_START_VALUE
#pragma pack(push)
#pragma pack(1)
typedef union
{
    uint32 raw;
    struct
    {
        int reg    : 24;
        int flags  : 8;
    } u;
} StackLocation;
#pragma pack(pop)

__inline bool operator==(StackLocation a, StackLocation b)
{
    return a.raw == b.raw;
}

__inline bool operator!=(StackLocation a, StackLocation b)
{
    return a.raw != b.raw;
}

// Flags for StackLocation
enum {
    // Mark that reg is a pointer in the local-variable stack
    STACK_LOCATION_FLAGS_LOCAL       = 1,
    // Mark that reg is a pointer to the arguments variable stack
    // Mark that reg is signed
    // TODO! refactor CompilerInterface32::!  STACK_LOCATION_FLAGS_SIGNED      = 8,

    // Mark this Stack location is invalid
    STACK_LOCATION_NO_MEMORY         = 128,
    STACK_LOCATION_EMPTY             = 256,
};

// Non-volatile register is one that must be preserved by functions
// Plaform register is any stack pointer, instruction pointer, etc which is never used in calculations
// Volatile register is anything else
enum RegisterType
{
    Volatile,
    NonVolatile,
    Platform
};

// Information about a register
struct RegisterEntry
{
    /*
     * eType          - See RegisterType
     * bAllocated     - Set if the register starts as allocated register
     * iFastCallOrder - 0 means that the register isn't one of the fast call register set, other number (1..3) state the order of the fast call
     */
    RegisterEntry(RegisterType eType = Volatile, bool bAllocated = false, int iFastCallOrder = 0);

    bool m_bAllocated;
    RegisterType m_eType;
    int m_iFastCallOrder;
};

// A translation table between registers and their stack status.
// NOTE: All register must be marked as negative numbers!
//
// See allocateTemporaryRegister() for more information
typedef cHash<int, RegisterEntry> RegisterAllocationTable;

// Forward deceleration
class StackInterface;
// The reference countable object
typedef cSmartPtr<StackInterface> StackInterfacePtr;

// Forward deceleration
class FirstPassBinary;


/*
 * This interface provides easy facilities for allocating a registers and
 * freeing them.
 *
 * TODO! The stack temporary registers allocation algorithm is flow! And should
 *       be improved.
 */
class StackInterface {
public:
    // Virtual destructor. You can inherit from me
    virtual ~StackInterface();

    // A value which represent that the current pool is empty
    static StackLocation NO_MEMORY;
    // A value which represent an empty stack location
    static StackLocation EMPTY;

    /*
     * Construct a new StackLocation object
     */
    static StackLocation buildStackLocation(int _reg, int _flag);

    // The starting value of the stack based variables
    enum { LOCAL_STACK_START_VALUE = 4 };


    //////////////////////////////////////////////////////////////////////////
    // Should be implemented by the stack implementor.

    /*
     * Returns a modifyable registers pool
     */
    virtual RegisterAllocationTable& getRegistersTable() = 0;

    /*
     * Returns the registers pool
     */
    virtual const RegisterAllocationTable& getRegistersTable() const = 0;

    /*
     * Return the size of the each register in the stack
     */
    virtual uint getRegisterSize() = 0;

    /*
     * Called by the assembler to notify that all stack reference referring to
     * a specific register variable should be replace to another.
     *
     * source          - The register stack location
     *
     * Return the stack location of the allocated register.
     *
     * NOTE: The implementation depends on the stack representation.
     *
     * Throw exception if the 'source' is not a register or if the register is
     * not allocated.
     */
    virtual StackLocation replaceRegisterToStackVariable(StackLocation source) = 0;

    //////////////////////////////////////////////////////////////////////////

    /*
     * Return the total stack size that needed for all the stack based temporary
     * variables.
     */
    uint getTotalTemporaryStackSize();

    /*
     * Return a temporary register from registers pool of the current block
     * NOTE: Registers are marked as negative numbers.
     *
     * bOnlyNonVolatile - true for nonvolatile register. false for any register
     *
     * NOTE: See CompilerInterface32::::allocateTemporaryBuffer.
     *       Don't use this routine directly. The routine used only for
     *       allocation management interface
     *
     * See freeTemporaryRegister
     */
    StackLocation allocateTemporaryRegister(bool bOnlyNonVolatile = false);

    /*
     * Increase stack by allocating 'size' bytes for temporary usage.
     *
     * size - The number of bytes to allocated from the stack.
     *        This value can be automatically aligned to the size given in the
     *        constructor.
     *
     * NOTE: See CompilerInterface32::::allocateTemporaryBuffer.
     *       Don't use this routine directly.
     *
     * Return handle for stack position.
     * See freeTemporaryStackBuffer
     */
    StackLocation allocateTemporaryStackBuffer(uint size);

    /*
     * Free a temporary register allocated by 'allocateTemporaryRegister()'
     * NOTE: Registers are marked as negative numbers.
     *
     * registerNumber - The register to be free
     *
     * Throw exception if the register was free
     */
    void freeTemporaryRegister(StackLocation registerNumber);

    /*
     * Return true if a register is free from the pool of allocated registers.
     * Return false otherwise
     *
     * registerNumber - The register to be free
     */
    bool isFreeTemporaryRegister(StackLocation registerNumber) const;

    /*
     * Free stack temporary buffer allocated by 'allocateTemporaryStackBuffer'
     *
     * stackPosition - The position returned by 'allocateTemporaryStackBuffer'
     *
     * Throw exception if the stack-position is invalid.
     */
    void freeTemporaryStackBuffer(StackLocation stackPosition);

    /*
     * Update the total temp stack to a higher value
     * Note: This method only enlarges the total temp stack, and never reduces it.
     */
    void UpdateTotalTempStack(unsigned int nTotalTempStack);

    /*
     * Change the base stack register
     *
     * baseRegister - A register that holds the correct stack pointer for use with
     *                locals and arguments. If NO_MEMORY, then use the default stack
     *                pointer register (ebp in x86)
     * Returns the old base stack register
     */
    virtual StackLocation setBaseStackRegister(StackLocation baseRegister);

    /*
     * Retrieve the current base stack register
     *
     * Returns the register that holds the correct stack pointer for use with
     * locals and arguments.
     */
    virtual StackLocation getBaseStackRegister() const;


protected:
    /*
     * Constructor. Initialize stack based variable list
     *
     * firstPass          - The FirstPassBinary. Used for registry touch
     * shouldAlignStack   - Set to true if the stack based variables should
     *                      be aligned.
     * stackAlignmentSize - The stack alignment value
     */
    StackInterface(FirstPassBinary& firstPass,
                   bool shouldAlignStack,
                   uint stackAlignmentSize = 0);

    /*
     * Copy-constructor. Used at basic-block duplication process
     */
    StackInterface(const StackInterface& other);

    // The total temporary stack size
    uint m_totalTempStack;
    // The stack alignment value
    uint m_stackAlignmentSize;
    // Set to true if the stack based variables should be aligned.
    bool m_shouldAlignStack;

    // Used for register touch handling
    FirstPassBinary& m_firstPass;

    /*
     * The current base register for stack operations
     * where locals and arguments can be found
     */
    StackLocation m_baseRegister;

private:
    // Deny operator =
    StackInterface& operator = (const StackInterface& other);

    // The stack layout. For each position in the stack
    // (From LOCAL_STACK_START_VALUE and so on), a byte indicates whether the
    // stack is occupied.
    cSArray<uint> m_allocatedTemporaryStack;
};

#endif // __TBA_DISMOUNT_STACKINTERFACE_H
