/*
 * SemanticOperand.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "dismount/semantics/SemanticOperand.h"

SemanticOperand::SemanticOperand() :
    m_kind(KIND_NONE),
    m_type(),
    m_index(0),
    m_value(0)
{
}

SemanticOperand SemanticOperand::registerOperand(uint registerId,
                                                 const SemanticType& type)
{
    SemanticOperand operand;
    operand.m_kind = KIND_REGISTER;
    operand.m_type = type;
    operand.m_index = registerId;
    return operand;
}

SemanticOperand SemanticOperand::temporary(uint temporaryIndex,
                                           const SemanticType& type)
{
    SemanticOperand operand;
    operand.m_kind = KIND_TEMPORARY;
    operand.m_type = type;
    operand.m_index = temporaryIndex;
    return operand;
}

SemanticOperand SemanticOperand::immediate(int64 value,
                                           const SemanticType& type)
{
    SemanticOperand operand;
    operand.m_kind = KIND_IMMEDIATE;
    operand.m_type = type;
    operand.m_value = value;
    return operand;
}

SemanticOperand SemanticOperand::guestAddress(uint64 address,
                                              const SemanticType& type)
{
    SemanticOperand operand;
    operand.m_kind = KIND_GUEST_ADDRESS;
    operand.m_type = type;
    operand.m_value = (int64)address;
    return operand;
}

SemanticOperand SemanticOperand::label(uint labelIndex)
{
    SemanticOperand operand;
    operand.m_kind = KIND_LABEL;
    operand.m_index = labelIndex;
    return operand;
}

SemanticOperand::Kind SemanticOperand::getKind() const
{
    return (Kind)m_kind;
}

const SemanticType& SemanticOperand::getType() const
{
    return m_type;
}

uint SemanticOperand::getIndex() const
{
    return m_index;
}

int64 SemanticOperand::getValue() const
{
    return m_value;
}

bool SemanticOperand::isValid() const
{
    return m_kind != KIND_NONE;
}

bool SemanticOperand::operator == (const SemanticOperand& other) const
{
    return (m_kind == other.m_kind) &&
           (m_type == other.m_type) &&
           (m_index == other.m_index) &&
           (m_value == other.m_value);
}

bool SemanticOperand::operator != (const SemanticOperand& other) const
{
    return !(*this == other);
}
