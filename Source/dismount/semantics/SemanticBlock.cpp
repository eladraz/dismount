/*
 * SemanticBlock.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/data/list.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/semantics/SemanticBlock.h"

SemanticBlock::SemanticBlock(OpcodeSubsystems::DisassemblerType architecture) :
    m_architecture(architecture),
    m_instructionsCount(0),
    m_temporariesCount(0),
    m_labelsCount(0),
    m_fallThroughAddress(gNullPointerProcessorAddress)
{
}

OpcodeSubsystems::DisassemblerType SemanticBlock::getArchitecture() const
{
    return m_architecture;
}

void SemanticBlock::setArchitecture(
                        OpcodeSubsystems::DisassemblerType architecture)
{
    m_architecture = architecture;
}

void SemanticBlock::beginGuestInstruction(const ProcessorAddress& address,
                                          uint size)
{
    GuestInstruction guest = { address, size, m_instructionsCount };
    m_guestInstructions.append(guest);
}

void SemanticBlock::append(const SemanticInstruction& instruction)
{
    m_instructions.append(instruction);
    m_instructionsCount++;
}

SemanticOperand SemanticBlock::newTemporary(const SemanticType& type)
{
    return SemanticOperand::temporary(m_temporariesCount++, type);
}

uint SemanticBlock::getTemporariesCount() const
{
    return m_temporariesCount;
}

SemanticOperand SemanticBlock::newLabel()
{
    return SemanticOperand::label(m_labelsCount++);
}

uint SemanticBlock::getLabelsCount() const
{
    return m_labelsCount;
}

const cList<SemanticInstruction>& SemanticBlock::getInstructions() const
{
    return m_instructions;
}

cList<SemanticInstruction>& SemanticBlock::getInstructions()
{
    return m_instructions;
}

const cList<SemanticBlock::GuestInstruction>&
                                SemanticBlock::getGuestInstructions() const
{
    return m_guestInstructions;
}

const ProcessorAddress& SemanticBlock::getFallThroughAddress() const
{
    return m_fallThroughAddress;
}

void SemanticBlock::setFallThroughAddress(const ProcessorAddress& address)
{
    m_fallThroughAddress = address;
}

void SemanticBlock::clear()
{
    m_instructions.removeAll();
    m_instructionsCount = 0;
    m_guestInstructions.removeAll();
    m_temporariesCount = 0;
    m_labelsCount = 0;
}
