/*
 * SectionMemoryInterface.cpp
 *
 * Implementation file
 *
 * Author: Tal Harel
 */
#include "dismount/dismount.h"
#include "dismount/SectionMemoryInterface.h"

void SectionMemoryInterface::initMaps()
{
    // Create the arrays, initialized to zero
    uint mapLength = 8 * ((m_memorySize / 8) + 1);
    //m_readMap  = cBuffer(mapLength);
    m_writeMap = cBuffer(mapLength);
    m_execMap  = cBuffer(mapLength);
    for (uint i = 0; i < mapLength; i++)
    {
        //m_readMap[i]  = 0;
        m_writeMap[i] = 0;
        m_execMap[i]  = 0;
    }

    // Enumerate the sections and fill the maps
    for (cList<SectionMemoryInterface::GeneralSection>::iterator i = m_sectionList.begin();
         i != m_sectionList.end();
         ++i)
    {
        // Skip this section if it isn't of any of the types we want
        //if (!((*i).m_flags & (SECTION_FLAG_READ | SECTION_FLAG_WRITE | SECTION_FLAG_EXECUTABLE)))
        if (!((*i).m_flags & (SECTION_FLAG_WRITE | SECTION_FLAG_EXECUTABLE)))
            continue;

        for (addressNumericValue addr = (*i).m_start; addr < (*i).m_end; addr++)
        {
            uint bitField = (1 << ((uint)addr % 8));
            //if ((*i).m_flags & SECTION_FLAG_READ)       m_readMap[addr / 8]  |= bitField;
            if ((*i).m_flags & SECTION_FLAG_WRITE)      m_writeMap[addr / 8] |= bitField;
            if ((*i).m_flags & SECTION_FLAG_EXECUTABLE) m_execMap[addr / 8]  |= bitField;
        }
    }
}

bool SectionMemoryInterface::checkAddress(addressNumericValue address, uint flags)
{
    // If the address is out of bounds
    if (address > m_memorySize)
        return false;

    // If the flags aren't of any of the types we manage
    if (!(flags & (SECTION_FLAG_READ | SECTION_FLAG_WRITE | SECTION_FLAG_EXECUTABLE)))
        return false;

    uint bitField = (1 << ((uint)address % 8));
    /*if ((flags & SECTION_FLAG_READ) && (!(m_readMap[(uint)address / 8] & bitField)))
        return false;*/
    if ((flags & SECTION_FLAG_WRITE) && (!(m_writeMap[(uint)address / 8] & bitField)))
        return false;
    if ((flags & SECTION_FLAG_EXECUTABLE) && (!(m_execMap[(uint)address / 8] & bitField)))
        return false;

    return true;
}

uint SectionMemoryInterface::virtualToRawAddress(addressNumericValue virtualAddress)
{
    // Enumerate the sections to find the section the address belongs to
    cList<SectionMemoryInterface::GeneralSection>::iterator i = m_sectionList.begin();
    for (; i != m_sectionList.end(); ++i)
    {
        if ((virtualAddress >= (*i).m_start) && (virtualAddress < (*i).m_end))
            return (uint)(virtualAddress - (*i).m_start + (*i).m_rawDataAddress);
    }

    return 0;
}

SectionMemoryInterface::SectionMemoryInterface(addressNumericValue moduleBaseAddress,
                                               addressNumericValue imageBase,
                                               uint memorySize,
                                               cList<GeneralSection> sectionList) :
    m_moduleBaseAddress(moduleBaseAddress),
    m_imageBase(imageBase),
    m_memorySize(memorySize),
    m_sectionList(sectionList)
{
    initMaps();
}
