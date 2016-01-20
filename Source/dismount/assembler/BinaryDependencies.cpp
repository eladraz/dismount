#include "dismount/dismount.h"
#include "dismount/assembler/BinaryDependencies.h"
/*
 * BinaryDependencies.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"

uint32 BinaryDependencies::m_extraBlocks = 0;

BinaryDependencies::BinaryDependencies()
{
}

uint32 BinaryDependencies::getExtraBlocks()
{
    return m_extraBlocks;
}

void BinaryDependencies::addDependency(const cString& name,
                                       uint position,
                                       uint length,
                                       DependencyType type,
                                       uint shiftRightCount,
                                       bool shouldAddExistValue,
                                       int fix_offset,
                                       bool bUseParentFirstPass)
{
    m_dependency.append(DependencyObject(name, position, length, type, shiftRightCount, shouldAddExistValue, fix_offset, bUseParentFirstPass));
}

void BinaryDependencies::addDependency(const DependencyObject& dependency)
{
    m_dependency.append(dependency);
}

void BinaryDependencies::addExtraBlockDependency(const cString& name,
                                                 uint position,
                                                 uint length,
                                                 DependencyType type,
                                                 uint shiftRightCount,
                                                 bool shouldAddExistValue)
{
    addDependency(name, position, length, type, shiftRightCount, shouldAddExistValue);
    ++m_extraBlocks;
}

const BinaryDependencies::DependencyObjectList&
                                         BinaryDependencies::getList() const
{
    return m_dependency;
}

BinaryDependencies::DependencyObjectList& BinaryDependencies::getList()
{
    return m_dependency;
}

BinaryDependencies::DependencyObject::DependencyObject(const cString& name,
                                                       uint position,
                                                       uint length,
                                                       DependencyType type,
                                                       uint shiftRightCount,
                                                       bool shouldAddExistValue,
                                                       int fix_offset,
                                                       bool bUseParentFirstPass) :
    m_name(name),
    m_position(position),
    m_length(length),
    m_shiftRightCount(shiftRightCount),
    m_shouldAddExistValue(shouldAddExistValue),
    m_type(type),
    m_fixOffset(fix_offset),
    m_bUseParentFirstPass(bUseParentFirstPass)
{
}

bool BinaryDependencies::isValid() const
{
    return true;
}

void BinaryDependencies::deserialize(basicInput& inputStream)
{
    // Remove all old dependancies
    m_dependency = DependencyObjectList();
    uint32 length;
    inputStream.streamReadUint32(m_extraBlocks);
    inputStream.streamReadUint32(length);
    for (uint j = 0; j < length; j++)
    {
        cString name = inputStream.readUnicodeNullString();
        int16 i; uint16 u;
        uint8 u8;
        DependencyObject obj(name, 0, 0, DEP_LABEL, 0, false, 0, false);

        inputStream.streamReadUint16(u); obj.m_position = u;
        inputStream.streamReadUint16(u); obj.m_length = u;
        inputStream.streamReadInt16 (i); obj.m_fixOffset = i;
        inputStream.streamReadUint8(u8);  obj.m_shiftRightCount = u8;
        inputStream.streamReadUint8(u8);  obj.m_shouldAddExistValue = (u8 != 0);
        inputStream.streamReadUint8(u8);  obj.m_type = (DependencyType)u8;
        inputStream.streamReadUint8(u8);  obj.m_bUseParentFirstPass = (u8 != 0);

        m_dependency.append(obj);
    }
}

void BinaryDependencies::serialize(basicOutput& outputStream) const
{
    // Write the length of extra-block
    outputStream.streamWriteUint32(m_extraBlocks);
    // Writing the list length
    outputStream.streamWriteUint32(m_dependency.length());
    // Writing the objects
    DependencyObjectList::iterator i = m_dependency.begin();
    for (; i != m_dependency.end(); ++i)
    {
        const DependencyObject& obj = *i;
        outputStream.writeUnicodeNullString(obj.m_name);
        outputStream.streamWriteUint16(obj.m_position);
        outputStream.streamWriteUint16(obj.m_length);
        outputStream.streamWriteInt16(obj.m_fixOffset);
        outputStream.streamWriteUint8(obj.m_shiftRightCount);
        outputStream.streamWriteUint8(obj.m_shouldAddExistValue ? 1 : 0);
        outputStream.streamWriteUint8(obj.m_type);
        outputStream.streamWriteUint8(obj.m_bUseParentFirstPass ? 1 : 0);
    }
}
