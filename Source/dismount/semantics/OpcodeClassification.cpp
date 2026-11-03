/*
 * OpcodeClassification.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "dismount/semantics/OpcodeClassification.h"

OpcodeClassification::OpcodeClassification() :
    m_category(CATEGORY_UNKNOWN),
    m_units(UNIT_NONE),
    m_properties(PROPERTY_NONE),
    m_flagsRead(0),
    m_flagsWritten(0),
    m_flagsUndefined(0),
    m_stackDelta(0)
{
}

OpcodeClassification::OpcodeClassification(Category category,
                                           uint32 units,
                                           uint32 properties) :
    m_category((uint8)category),
    m_units(units),
    m_properties(properties),
    m_flagsRead(0),
    m_flagsWritten(0),
    m_flagsUndefined(0),
    m_stackDelta(0)
{
}

OpcodeClassification::Category OpcodeClassification::getCategory() const
{
    return (Category)m_category;
}

uint32 OpcodeClassification::getUnits() const
{
    return m_units;
}

uint32 OpcodeClassification::getProperties() const
{
    return m_properties;
}

bool OpcodeClassification::hasProperty(uint32 property) const
{
    return (m_properties & property) == property;
}

uint32 OpcodeClassification::getFlagsRead() const
{
    return m_flagsRead;
}

uint32 OpcodeClassification::getFlagsWritten() const
{
    return m_flagsWritten;
}

uint32 OpcodeClassification::getFlagsUndefined() const
{
    return m_flagsUndefined;
}

void OpcodeClassification::setFlags(uint32 read, uint32 written,
                                    uint32 undefined)
{
    m_flagsRead = read;
    m_flagsWritten = written;
    m_flagsUndefined = undefined;
}

const SemanticType& OpcodeClassification::getDataType() const
{
    return m_dataType;
}

void OpcodeClassification::setDataType(const SemanticType& type)
{
    m_dataType = type;
}

int OpcodeClassification::getStackDelta() const
{
    return m_stackDelta;
}

void OpcodeClassification::setStackDelta(int delta)
{
    m_stackDelta = delta;
}
