/*
 * SemanticInstruction.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "dismount/semantics/SemanticInstruction.h"

SemanticInstruction::SemanticInstruction() :
    m_operation(SemanticOperation::OP_NOP),
    m_condition(SemanticOperation::COND_NONE),
    m_flagRecipe(SemanticOperation::FLAGS_NONE),
    m_memoryOrder(SemanticOperation::ORDER_RELAXED),
    m_roundingMode(SemanticOperation::ROUND_DYNAMIC),
    m_fenceAccess(0),
    m_behavior(SemanticOperation::BEHAVIOR_DEFAULT),
    m_kind(0),
    m_combineOperation(SemanticOperation::OP_NOP),
    m_flagMask(0),
    m_falseFlags(0),
    m_attributes(ATTRIBUTE_NONE)
{
}

SemanticInstruction::SemanticInstruction(
                            SemanticOperation::Operation operation,
                            const SemanticType& type,
                            const SemanticOperand& destination,
                            const SemanticOperand& source0,
                            const SemanticOperand& source1,
                            const SemanticOperand& source2) :
    m_operation((uint16)operation),
    m_condition(SemanticOperation::COND_NONE),
    m_flagRecipe(SemanticOperation::FLAGS_NONE),
    m_memoryOrder(SemanticOperation::ORDER_RELAXED),
    m_roundingMode(SemanticOperation::ROUND_DYNAMIC),
    m_fenceAccess(0),
    m_behavior(SemanticOperation::BEHAVIOR_DEFAULT),
    m_kind(0),
    m_combineOperation(SemanticOperation::OP_NOP),
    m_flagMask(0),
    m_falseFlags(0),
    m_attributes(ATTRIBUTE_NONE),
    m_type(type),
    m_destination(destination)
{
    m_sources[0] = source0;
    m_sources[1] = source1;
    m_sources[2] = source2;
}

SemanticOperation::Operation SemanticInstruction::getOperation() const
{
    return (SemanticOperation::Operation)m_operation;
}

const SemanticType& SemanticInstruction::getType() const
{
    return m_type;
}

const SemanticOperand& SemanticInstruction::getDestination() const
{
    return m_destination;
}

const SemanticOperand& SemanticInstruction::getSource(uint index) const
{
    CHECK(index < MAX_SOURCES);
    return m_sources[index];
}

void SemanticInstruction::setDestination(const SemanticOperand& destination)
{
    m_destination = destination;
}

void SemanticInstruction::setSource(uint index, const SemanticOperand& source)
{
    CHECK(index < MAX_SOURCES);
    m_sources[index] = source;
}

SemanticOperation::Condition SemanticInstruction::getCondition() const
{
    return (SemanticOperation::Condition)m_condition;
}

void SemanticInstruction::setCondition(SemanticOperation::Condition condition)
{
    m_condition = (uint8)condition;
}

SemanticOperation::FlagRecipe SemanticInstruction::getFlagRecipe() const
{
    return (SemanticOperation::FlagRecipe)m_flagRecipe;
}

uint32 SemanticInstruction::getFlagMask() const
{
    return m_flagMask;
}

void SemanticInstruction::setFlags(SemanticOperation::FlagRecipe recipe,
                                   uint32 flagMask)
{
    m_flagRecipe = (uint8)recipe;
    m_flagMask = flagMask;
}

uint32 SemanticInstruction::getFalseFlags() const
{
    return m_falseFlags;
}

void SemanticInstruction::setFalseFlags(uint32 flags)
{
    m_falseFlags = flags;
}

SemanticOperation::Operation SemanticInstruction::getCombineOperation() const
{
    return (SemanticOperation::Operation)m_combineOperation;
}

void SemanticInstruction::setCombineOperation(
                                    SemanticOperation::Operation operation)
{
    m_combineOperation = (uint16)operation;
}

SemanticOperation::MemoryOrder SemanticInstruction::getMemoryOrder() const
{
    return (SemanticOperation::MemoryOrder)m_memoryOrder;
}

void SemanticInstruction::setMemoryOrder(SemanticOperation::MemoryOrder order)
{
    m_memoryOrder = (uint8)order;
}

uint8 SemanticInstruction::getFenceAccess() const
{
    return m_fenceAccess;
}

void SemanticInstruction::setFenceAccess(uint8 access)
{
    m_fenceAccess = access;
}

SemanticOperation::Behavior SemanticInstruction::getBehavior() const
{
    return (SemanticOperation::Behavior)m_behavior;
}

void SemanticInstruction::setBehavior(SemanticOperation::Behavior behavior)
{
    m_behavior = (uint8)behavior;
}

uint SemanticInstruction::getKind() const
{
    return m_kind;
}

void SemanticInstruction::setKind(uint kind)
{
    CHECK(kind <= 0xFF);
    m_kind = (uint8)kind;
}

SemanticOperation::RoundingMode SemanticInstruction::getRoundingMode() const
{
    return (SemanticOperation::RoundingMode)m_roundingMode;
}

void SemanticInstruction::setRoundingMode(SemanticOperation::RoundingMode mode)
{
    m_roundingMode = (uint8)mode;
}

uint32 SemanticInstruction::getAttributes() const
{
    return m_attributes;
}

void SemanticInstruction::setAttributes(uint32 attributes)
{
    m_attributes = attributes;
}
