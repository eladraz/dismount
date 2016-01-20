#include "dismount/dismount.h"
/*
 * ProcessorAddress.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "dismount/ProcessorAddress.h"

// No own should use this value
const ProcessorAddress gNullPointerProcessorAddress(
    ProcessorAddress::PROCESSOR_64,
    0);

ProcessorAddress::ProcessorAddress(ProcessorAddress::ProcessorAddressType type,
                                   uintAddress address) :
    m_type(type)
{
    setAddress(address);
}

ProcessorAddress::ProcessorAddressType ProcessorAddress::getAddressType() const
{
    return m_type;
}

ProcessorAddress::uintAddress ProcessorAddress::getAddress() const
{
    switch (m_type)
    {
    case PROCESSOR_16:
        return m_value.m_16bitProcessorAddress;
    case PROCESSOR_20:
    case PROCESSOR_32:
        return m_value.m_32bitProcessorAddress;
    case PROCESSOR_64:
        return m_value.m_64bitProcessorAddress;
    }
    // Never should get here
    CHECK_FAIL();
}

void ProcessorAddress::setAddress(uintAddress newAddress)
{
    switch (m_type)
    {
    case PROCESSOR_16:
        m_value.m_16bitProcessorAddress = (uint16)(newAddress); break;
    case PROCESSOR_20:
    case PROCESSOR_32:
        m_value.m_32bitProcessorAddress = (uint32)(newAddress); break;
    case PROCESSOR_64:
        m_value.m_64bitProcessorAddress = newAddress; break;
    default:
        CHECK_FAIL();
    }
}

ProcessorAddress::intAddress ProcessorAddress::castRelativeAddress() const
{
    switch (m_type)
    {
    case PROCESSOR_16:
        return (int64)((int16)m_value.m_32bitProcessorAddress);
    case PROCESSOR_20:
    case PROCESSOR_32:
        return (int64)((int32)m_value.m_32bitProcessorAddress);
    case PROCESSOR_64:
        return (int64)(m_value.m_64bitProcessorAddress);
    }
    // Never should get here
    CHECK_FAIL();
}

ProcessorAddress ProcessorAddress::operator + (const intAddress& relative) const
{
    return ProcessorAddress(m_type, getAddress() + relative);
}

bool ProcessorAddress::operator == (const ProcessorAddress& other) const
{
    return getAddress() == other.getAddress();
}

bool ProcessorAddress::operator > (const ProcessorAddress& other) const
{
    return getAddress() > other.getAddress();
}
