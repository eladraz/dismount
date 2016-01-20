#ifndef __TBA_CLR_COMPILER_FIRSTPASSBINARY_H
#define __TBA_CLR_COMPILER_FIRSTPASSBINARY_H

/*
 * FirstPassBinary.h
 *
 * Contains the result of the first-pass compiler given by the MethodCompiler
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "xStl/data/hash.h"
#include "xStl/data/list.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/serializedObject.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/assembler/BinaryDependencies.h"
#include "dismount/assembler/StackInterface.h"

/*
 * Contains the information needed for the first pass method compiler.
 *
 * The information this class contains consist from the compiled instructions
 * themselves, separated into basic-block and includes symbol/debug information
 * calling conventions, arguments and private data class. The private data is
 * a facility that can be filled in order to store specific method data, such
 * as exception handling and general stack information.
 *
 * NOTE: The first-binary pass is operative only per method!
 *
 * NOTE: This class is not thread-safe
*/
class FirstPassBinary {
public:
    /*
     * Default constructor. Prepare new method pass binary.
     *
     * NOTE: The first pass is generated without any basic block.
     *       Before using the function methods you must create the first block!
     *
     * type    - The type of the assembler. Used in verification
     * defaultCallingConvention -
     *           Set to true to indicate that this function is a __stdcall,
     *           which means that this function should clean the stack.
     *           Set to false to indicate that this function is __cdecl which
     *           means that the caller should clean the stack (from args).
     *           NOTE: Advance compilers should ignore this convention.
     *           TODO! Will be change to enum
     */
    FirstPassBinary(OpcodeSubsystems::DisassemblerType type,
                    bool defaultCallingConvention);

    //////////////////////////////////////////////////////////////////////////
    // Data retrieving methods

    /*
     * Returns the type of the assembler
     */
    OpcodeSubsystems::DisassemblerType getAssemblerType() const;

    /*
     * Return true if this function is __stdcall function.
     * Return false if this function is __cdecl function.
     *
     * See FirstPassBinary::FirstPassBinary for more information
     */
    bool isStdCall() const;
    bool isDefaultStdCall() const;

    /*
     * Change the calling convention of a function
     */
    void setStdCall(bool isStdCall);

    /*
     * Return the size of the arguments.
     */
    uint getArgumentsSize() const;

    /*
     * Return the list of registers which touched during the method life-time
     */
    const RegisterAllocationTable& getTouchedRegisters() const;

    /*
     * Return a modifyable list of registers which were touched during the method life-time
     */
    RegisterAllocationTable& getTouchedRegisters();

    /*
     * A basic block composed
     */
    class BasicBlock {
    public:
        BasicBlock();
        BasicBlock(const BasicBlock& other);

        // The index for the basic block
        int m_blockNumber;
        // The buffer itself
        cBuffer m_data;
        // The dependencies tree for this current block
        BinaryDependencies m_dependecies;
        // The stack information for the block
        StackInterfacePtr m_stack;
    };
    // A list of all block.
    // NOTE: This list must be shorted according to m_blockNumber.
    typedef cList<BasicBlock> BasicBlockList;

    /*
     * Return the current block ID
     * Throw exception if the first block wasn't generated yet
     */
    int getCurrentBlockID() const;

    /*
     * Return the compiled data buffer for the current stream
     * Throw exception if the first block wasn't generated yet
     */
    const cBuffer& getCurrentBlockData() const;

    /*
     * Return the current block dependencies tree
     * Throw exception if the first block wasn't generated yet
     */
    const BinaryDependencies& getCurrentDependecies() const;

    /*
     * Return the current stack information
     */
    const StackInterfacePtr& getCurrentStack() const;
    StackInterfacePtr& getCurrentStack();

    //////////////////////////////////////////////////////////////////////////

    /*
     * Return a short list of all basic blocks.
     */
    const BasicBlockList& getBlocksList() const;

    /*
     * Create a new basic block to the first binary tree.
     *
     * blockID    - The newly create block ID
     * stack      - The stack of the current block
     *
     * NOTE: This method doesn't change the context of the current block into
     *       the newly created block
     * Throw exception if the block already exist
     */
    void createNewBlockWithoutChange(int blockID,
                                     const StackInterfacePtr& stack);

    /*
     * Change the current block into new block ID
     *
     * blockID - The block ID
     *
     * Throw exception if the block doesn't exist.
     */
    void changeBasicBlock(int blockID);

    //////////////////////////////////////////////////////////////////////////
    // Static basic block management routines

    /*
     * Get closest block from block ID.
     *
     * list         - The basic block list. See FirstPassBinary::getBlocksList
     * blockID      - The block ID
     * shouldAssert - Set to true to throw exception if blockID couldn't be
     *                found
     *
     * If the 'blockID' doesn't exist:
     *      If shouldAssert is true, throw exception
     *      Otherwise: Returns BasicBlockList::end() iterator or nearest
     *                 previous block.
     * Otherwise return the block itself.
     */
    static BasicBlockList::iterator getNearestBlock(const BasicBlockList& list,
                                                    int blockID,
                                                    bool shouldAssert = false);

    /*
     * Delete a block. Used on second recompiling block
     *
     * blockPos - The block position. See "getNearestBlock"
     */
    void deleteBlock(BasicBlockList::iterator& blockPos);

    //////////////////////////////////////////////////////////////////////////
    // Adding information into the binary stream
    // These methods are used by the *CompilerInterface32:: classes

    /*
     * Add information to the current basic block.
     *
     * Throw exception if the first block wasn't generated yet
     * Throw exception if the binary is sealed.
     */
    void appendBuffer(const cBuffer& data);
    void appendBuffer(const uint8* data, uint size);
    void appendUint8(uint8 data);

    /*
     * Return the current dependencies tree
     */
    BinaryDependencies& getCurrentDependecies();

    //////////////////////////////////////////////////////////////////////////
    // Stack repository information

    /*
     * Return the current stack size needed in order to operate this methods.
     * This number is the sum of locals and the sum of all added variables
     */
    uint getStackSize() const;

    /*
     * Return the stack base size (e.g. locals and header).
     */
    uint getStackBaseSize();

    /*
     * Return the size of non-volatiles
     */
    uint getNonVolatilesSize();

    /*
     * Change the size for the base stack size
     *
     * localStackSize - The number of bytes for the initialize stack allocated
     *                  for locals
     */
    void setStackBaseSize(uint stackSize);

    /*
     * Change the size of saved non-volatile registers
     *
     * nonVolSize - The number of bytes used by non-volatiles
     */
    void setNonVolatilesSize(int nonVolSize);

    /*
     * Change the size of the method arguments
     *
     * argSize - The size of the arguments. Can be 0 for __stdcall.
     */
    void setArgumentsSize(uint argSize);

    /*
     * Calling this method will conclude and seal the method binary from any
     * further code generation.
     *
     * NOTE: This function throw an exception if one of the temporary registered
     *       wasn't free properly.
     */
    void seal();

    /*
     * Moves a set of basic blocks to another FPB object.
     *
     * setArray - An array of compiled instructions which denotes which blocks are to be moved
     * other - Another FPB object which will receive the moved blocks
     */
    void moveBasicBlocks(const class cSetArray& setArray, FirstPassBinary& other);

    /*
     * Mark a register as touched.
     */
    void touch(int _register);
    /*
     * Mark a register as untouched.
     */
    void untouch(int _register);

private:
    // Deny copy-constructor and operator =
    // Performance penalties
    FirstPassBinary(const FirstPassBinary& other);
    FirstPassBinary& operator = (const FirstPassBinary& other);

    // Everybody needs a friend.
    friend class StackInterface;
    friend class MethodCompiler;

    // The list of all touched registers.
    RegisterAllocationTable m_touched;

    // The size of the arguments
    uint m_argSize;
    // __stdcall / __cdecl selector
    bool m_stdCall;
    bool m_defaultCallingConvention;

    // The type of the assembler;
    OpcodeSubsystems::DisassemblerType m_type;

    // The size for stacks
    uint m_stackSize;
    // The size of non-volatiles
    uint m_nonVolSize;
    // Initialize stack size
    uint m_localStackSize;
    // Set to true if the method is seal and no future appending should committed
    bool m_isSeal;

    // Basic block list
    BasicBlockList m_data;
    // The current block
    BasicBlockList::iterator m_currentBlock;
};

/*
 * This macro help to add uint8* arraies.
 * Use only for static const predefined value! otherwise sizeof(x) will be
 * 32/64 bit!
 */
#define ADDSCONST(x) (x), sizeof((x))

// Reference countable object for the FirstBinary pass
typedef cSmartPtr<FirstPassBinary> FirstPassBinaryPtr;

#endif // __TBA_CLR_COMPILER_FIRSTPASSBINARY_H
