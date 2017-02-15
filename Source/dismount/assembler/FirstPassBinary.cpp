#include "dismount/dismount.h"
/*
 * FirstPassBinary.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/setArray.h"
#include "dismount/assembler/FirstPassBinary.h"

FirstPassBinary::FirstPassBinary(OpcodeSubsystems::DisassemblerType type,
                                 bool defaultCallingConvention) :
    m_argSize(0),
    m_stdCall(defaultCallingConvention),
    m_defaultCallingConvention(defaultCallingConvention),
    m_type(type),
    m_stackSize(0),
    m_nonVolSize(0),
    m_localStackSize(0),
    m_isSeal(false),
    // Reset current block to null
    m_currentBlock(m_data.begin())
{
    // Make sure that the current block points out to null
    m_currentBlock = m_data.end();
}

OpcodeSubsystems::DisassemblerType FirstPassBinary::getAssemblerType() const
{
    return m_type;
}

bool FirstPassBinary::isStdCall() const
{
    return m_stdCall;
}

bool FirstPassBinary::isDefaultStdCall() const
{
    return m_defaultCallingConvention;
}

void FirstPassBinary::setStdCall(bool isStdCall)
{
    m_stdCall = isStdCall;
}

uint FirstPassBinary::getArgumentsSize() const
{
    return m_argSize;
}

const RegisterAllocationTable& FirstPassBinary::getTouchedRegisters() const
{
    return m_touched;
}

RegisterAllocationTable& FirstPassBinary::getTouchedRegisters()
{
    return m_touched;
}

int FirstPassBinary::getCurrentBlockID() const
{
    ASSERT(m_currentBlock != m_data.end());
    return (*m_currentBlock).m_blockNumber;
}

const cBuffer& FirstPassBinary::getCurrentBlockData() const
{
    ASSERT(m_currentBlock != m_data.end());
    return (*m_currentBlock).m_data;
}

const BinaryDependencies& FirstPassBinary::getCurrentDependecies() const
{
    ASSERT(m_currentBlock != m_data.end());
    return (*m_currentBlock).m_dependecies;
}

StackInterfacePtr& FirstPassBinary::getCurrentStack()
{
    ASSERT(m_currentBlock != m_data.end());
    return (*m_currentBlock).m_stack;
}

const StackInterfacePtr& FirstPassBinary::getCurrentStack() const
{
    ASSERT(m_currentBlock != m_data.end());
    return (*m_currentBlock).m_stack;
}

const FirstPassBinary::BasicBlockList& FirstPassBinary::getBlocksList() const
{
    return m_data;
}

void FirstPassBinary::createNewBlockWithoutChange(int blockID,
                                                const StackInterfacePtr& stack)
{
    // Start by finding the nearest block
    BasicBlockList::iterator i = getNearestBlock(m_data, blockID);
    // Check for not exist value
    if ((i != m_data.end()) &&
        ((*i).m_blockNumber == blockID))
    {
        // Error, found occupied cell
        CHECK_FAIL();
    }

    BasicBlock newBlock;
    newBlock.m_blockNumber = blockID;
    newBlock.m_stack = stack;
    m_data.insert(i, newBlock);
}

void FirstPassBinary::changeBasicBlock(int blockID)
{
    // Get block
    BasicBlockList::iterator i = getNearestBlock(m_data, blockID, true);
    // And change
    m_currentBlock = i;
}

FirstPassBinary::BasicBlockList::iterator
               FirstPassBinary::getNearestBlock(const BasicBlockList& list,
                                                int blockID,
                                                bool shouldAssert)
{
    BasicBlockList::iterator i = list.begin();
    // Scan the list
    for (; i != list.end(); ++i)
    {
        // Check stop condition
        if ((*i).m_blockNumber >= blockID)
            break;
    }

    // Check found element
    if (shouldAssert)
    {
        CHECK(i != list.end());
        CHECK((*i).m_blockNumber == blockID);
    }
    return i;
}

void FirstPassBinary::deleteBlock(
                      FirstPassBinary::BasicBlockList::iterator& blockPos)
{
    if (m_currentBlock == blockPos)
    {
        // Put a dummy end
        m_currentBlock = m_data.end();
    }
    m_data.remove(blockPos);
}

void FirstPassBinary::appendBuffer(const cBuffer& data)
{
    CHECK(!m_isSeal);
    (*m_currentBlock).m_data+= data;
}

void FirstPassBinary::appendBuffer(const uint8* data, uint size)
{
    CHECK(!m_isSeal);
    uint oldSize = (*m_currentBlock).m_data.getSize();
    (*m_currentBlock).m_data.changeSize(oldSize + size);
    cOS::memcpy((*m_currentBlock).m_data.getBuffer() + oldSize, data, size);
}

void FirstPassBinary::appendUint8(uint8 data)
{
    CHECK(!m_isSeal);
    (*m_currentBlock).m_data+= data;
}

BinaryDependencies& FirstPassBinary::getCurrentDependecies()
{
    return (*m_currentBlock).m_dependecies;
}

void FirstPassBinary::seal()
{
    CHECK(!m_isSeal);
    m_isSeal = true;
}

uint FirstPassBinary::getStackSize() const
{
    uint maxStackSize = 0;
    // Scan all blocks and take the maximum temporary stack-positions.
    BasicBlockList::iterator i = m_data.begin();
    // Scan the list
    for (; i != m_data.end(); ++i)
    {
        // Check stop condition
        StackInterfacePtr& stack = (*i).m_stack;
        if (!stack.isEmpty())
            maxStackSize = t_max(maxStackSize,
                                 stack->getTotalTemporaryStackSize());
    }

    return m_localStackSize + maxStackSize;
}

uint FirstPassBinary::getNonVolatilesSize()
{
    return m_nonVolSize;
}

uint FirstPassBinary::getStackBaseSize()
{
    return m_localStackSize;
}

void FirstPassBinary::setNonVolatilesSize(int nonVolSize)
{
    m_nonVolSize = nonVolSize;
}

void FirstPassBinary::setStackBaseSize(uint stackSize)
{
    m_localStackSize = stackSize;
}

void FirstPassBinary::setArgumentsSize(uint argSize)
{
    m_argSize = argSize;
}

void FirstPassBinary::touch(int _register)
{
    if (!m_touched.hasKey(_register))
        m_touched.append(_register, RegisterEntry(Volatile, true));
}

void FirstPassBinary::untouch(int _register)
{
    if (m_touched.hasKey(_register))
        m_touched.remove( _register);
}

void FirstPassBinary::moveBasicBlocks(const cSetArray& setArray, FirstPassBinary& other)
{
    BasicBlockList::iterator iMine = m_data.begin();
    while (iMine != m_data.end())
    {
        BasicBlock& block = (*iMine);
        if ((block.m_blockNumber >= 0) && (block.m_blockNumber < (int)setArray.getLength()) && setArray.isSet(block.m_blockNumber))
        {
            // find the nearest block
            BasicBlockList::iterator iTheirs = getNearestBlock(other.m_data, block.m_blockNumber);
            // Make sure this block ID does not exist already
            if ((iTheirs != other.m_data.end()) &&
                ((*iTheirs).m_blockNumber == block.m_blockNumber))
            {
                // Error, found occupied cell
                CHECK_FAIL();
            }

            other.m_data.insert(iTheirs, block);
            iMine = m_data.remove(iMine);
        }
        else
            iMine++;
    }
}

FirstPassBinary::BasicBlock::BasicBlock() :
	m_blockNumber(0),
	m_data((uint)0, 256)
{
}

FirstPassBinary::BasicBlock::BasicBlock(const BasicBlock& other) : m_blockNumber(other.m_blockNumber), m_data(other.m_data), m_dependecies(other.m_dependecies), m_stack(other.m_stack)
{
}
