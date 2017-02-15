#include "dismount/dismount.h"
/*
 * StackInterface.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/utils/algorithm.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/FirstPassBinary.h"


StackInterface::StackInterface(FirstPassBinary& firstPass,
                               bool shouldAlignStack,
                               uint stackAlignmentSize) :
    m_totalTempStack(0),
    m_stackAlignmentSize(stackAlignmentSize),
    m_shouldAlignStack(shouldAlignStack),
    m_firstPass(firstPass),
    m_baseRegister(StackInterface::EMPTY),
    m_allocatedTemporaryStack((uint)0, 0x200)
    // Don't forget to add new entries to the copy-constructor as well!
{
}

StackInterface::StackInterface(const StackInterface& other) :
    m_totalTempStack(other.m_totalTempStack),
    m_stackAlignmentSize(other.m_stackAlignmentSize),
    m_shouldAlignStack(other.m_shouldAlignStack),
    m_firstPass(other.m_firstPass),
    m_baseRegister(other.m_baseRegister),
    m_allocatedTemporaryStack(other.m_allocatedTemporaryStack)
{
}

StackInterface::~StackInterface()
{
}

uint StackInterface::getTotalTemporaryStackSize()
{
    return m_totalTempStack;
}

StackLocation StackInterface::buildStackLocation(int _reg, int _flag)
{
    StackLocation ret;
    ret.u.reg = _reg;
    ret.u.flags = _flag;
    return ret;
}

StackLocation StackInterface::NO_MEMORY =
    StackInterface::buildStackLocation(0, STACK_LOCATION_NO_MEMORY);

StackLocation StackInterface::EMPTY =
    StackInterface::buildStackLocation(0, STACK_LOCATION_EMPTY);

StackLocation StackInterface::allocateTemporaryRegister(bool bOnlyNonVolatile)
{
    // Find the first free register
    cList<int> keys(getRegistersTable().keys());
    cList<int>::iterator i(keys.begin());
    for (; i != keys.end(); ++i)
    {
        if (bOnlyNonVolatile && (getRegistersTable()[(*i)].m_eType != NonVolatile))
            continue;

        m_firstPass.touch(*i);
        if (!getRegistersTable()[(*i)].m_bAllocated)
        {
            // Check the pool for negative value
            ASSERT((*i) < 0);
            getRegistersTable()[(*i)].m_bAllocated = true;
            StackLocation ret;
            ret.raw = 0;
            ret.u.reg = (*i);
            return ret;
        }
    }

    // Cannot allocate any register
    return NO_MEMORY;
}

StackLocation StackInterface::allocateTemporaryStackBuffer(uint size)
{
    StackLocation ret;
    ret.raw = 0;
    ret.u.flags = STACK_LOCATION_FLAGS_LOCAL;

    // TODO! Improve the algorithm
    if (m_shouldAlignStack)
    {
        size = (size + m_stackAlignmentSize - 1) / m_stackAlignmentSize;
        size = size * m_stackAlignmentSize;
    }

    // Try to find an empty space at the stack
    uint jump = m_shouldAlignStack ? m_stackAlignmentSize : 1;
    for (int i = 0;
         i < ((int)m_allocatedTemporaryStack.getSize()) - (int)size;
         i+= jump)
    {
        bool found = true;
        for (uint j = i; j < i + size; j++)
        {
            if (m_allocatedTemporaryStack[j] != 0)
            {
                found = false;
                break;
            }
        }

        if (found)
        {
            // Allocate this stack position
            for (uint j = i; j < i + size; j++)
            {
                ASSERT(m_allocatedTemporaryStack[j] == 0);
                m_allocatedTemporaryStack[j] = size;
            }
            ret.u.reg = i + LOCAL_STACK_START_VALUE;
            return ret;
        }
    }

    // Need to increase the size of the stack
    uint pos = m_allocatedTemporaryStack.getSize();

    // TODO! There something missing here (SVN?)
    m_allocatedTemporaryStack.changeSize(
                        m_allocatedTemporaryStack.getSize() + size);

    m_totalTempStack = t_max(m_totalTempStack, m_allocatedTemporaryStack.getSize());

    for (uint j = pos; j < pos + size; j++)
        m_allocatedTemporaryStack[j] = size;

    ret.u.reg = pos + LOCAL_STACK_START_VALUE;
    return ret;
}

bool StackInterface::isFreeTemporaryRegister(
                                          StackLocation registerNumber) const
{
    ASSERT(registerNumber.u.reg < 0);

    return !getRegistersTable()[registerNumber.u.reg].m_bAllocated;
}

void StackInterface::freeTemporaryRegister(StackLocation registerNumber)
{
    ASSERT(registerNumber.u.reg < 0);
    // Free register
    CHECK(getRegistersTable().hasKey(registerNumber.u.reg));

    // Trying to double-free?
    ASSERT(getRegistersTable()[registerNumber.u.reg].m_bAllocated);

    getRegistersTable()[registerNumber.u.reg].m_bAllocated = false;
}

void StackInterface::freeTemporaryStackBuffer(StackLocation stackPosition)
{
    ASSERT((stackPosition.u.flags & STACK_LOCATION_FLAGS_LOCAL) != 0);

    uint position = stackPosition.u.reg - LOCAL_STACK_START_VALUE;
    // CHECK(position < m_allocatedTemporaryStack.getSize());
    // This check is committed by the [] operator
    uint size = m_allocatedTemporaryStack[position];
    CHECK((position + size) <= m_allocatedTemporaryStack.getSize());

    for (uint j = position; j < position + size; j++)
    {
        CHECK(m_allocatedTemporaryStack[j] == size);
        m_allocatedTemporaryStack[j] = 0;
    }
}

void StackInterface::UpdateTotalTempStack(unsigned int nTotalTempStack)
{
    if (m_totalTempStack < nTotalTempStack)
        m_totalTempStack = nTotalTempStack;
}

StackLocation StackInterface::setBaseStackRegister(StackLocation baseRegister)
{
    StackLocation old = m_baseRegister;
    m_baseRegister = baseRegister;
    return old;
}

StackLocation StackInterface::getBaseStackRegister() const
{
    return m_baseRegister;
}

RegisterEntry::RegisterEntry(RegisterType eType, bool bAllocated, int iFastCallOrder) :
    m_bAllocated(bAllocated),
    m_eType(eType),
    m_iFastCallOrder(iFastCallOrder)
{
}

