#ifndef __TBA_DISMOUNT_SECTIONMEMORYINTERFACE_H
#define __TBA_DISMOUNT_SECTIONMEMORYINTERFACE_H

/*
 * SectionMemoryInterface.h
 *
 * Provides an interface between a PE format and objects that need access
 * to its memory locations and attributes.
 * The application is responsible for parsing the PE and creating the interface.
 * The object can then access the PE attributes without any knowledge of its format.
 *
 * Author: Tal Harel
 */

#include "xStl/types.h"

class SectionMemoryInterface {
public:
    // Section flags
    enum {
        SECTION_FLAG_READ       = 1,
        SECTION_FLAG_WRITE      = 2,
        SECTION_FLAG_EXECUTABLE = 4,
    };

    /*
     * A struct to represent the most basic form of an executable format's section.
     * Contains the sections bounds (start and end), the address where the section data
     * starts (for translating between virtual addresses and raw addresses in an input file)
     * and the section attribute flags.
     */
    struct GeneralSection {
        addressNumericValue m_start;
        addressNumericValue m_end;
        addressNumericValue m_rawDataAddress;
        uint m_flags;

        GeneralSection(addressNumericValue start,
                       addressNumericValue end,
                       addressNumericValue rawDataAddress,
                       uint flags) : m_start(start),
                                     m_end(end),
                                     m_rawDataAddress(rawDataAddress),
                                     m_flags(flags)
        {}
    };

    /*
     * Constructor.
     *
     * moduleBaseAddress - The address the PE was loaded to in memory
     * imageBase - The "preffered" address in memory the PE wants to load to
     * memorySize - The size of the PE in the memory space
     * sectionList - A list containing information about each of the PE's
     *               sections. See "GeneralSection".
     *
     * Initializes member variables
     */
    SectionMemoryInterface(addressNumericValue moduleBaseAddress,
                           addressNumericValue imageBase,
                           uint memorySize,
                           cList<GeneralSection> sectionList);

    /*
     * Virtual Destructor.
     */
    virtual ~SectionMemoryInterface() {};

    /*
     * Checks a given address inside the PE memory against a flag mask.
     *
     * address - The input address
     * flags - The flag bit mask to check
     *
     * Returns true if the address attributes contains the wanted flags.
     * false otherwise.
     */
    bool checkAddress(addressNumericValue address, uint flags);

    /*
     * Translates virtual addresses (pointed to from inside the PE code)
     * to raw addresses in the PE input file.
     *
     * virtualAddress - The input address
     *
     * Returns the translated address, if found inside the PE sections.
     * 0 otherwise.
     *
     */
    uint virtualToRawAddress(addressNumericValue virtualAddress);

    /*
     * Returns the module base address.
     */
    inline addressNumericValue getModuleBaseAddress() { return m_moduleBaseAddress;};

    /*
     * Returns the image base.
     */
    inline addressNumericValue getImageBase() { return m_imageBase;};

    /*
     * Returns the memory size
     */
    inline uint getMemorySize() { return m_memorySize;};

    /*
     * Returns the section list.
     */
    inline cList<GeneralSection>& getSectionList() { return m_sectionList;};

private:
    /*
     * Prevent copy constructor
     */
    SectionMemoryInterface(const SectionMemoryInterface& other);

    /*
     * Initializes the memory attribute maps, used to check addresses
     * against certain flags (see "checkAddress").
     */
    void initMaps();

    // The module base address
    addressNumericValue m_moduleBaseAddress;
    // The image base
    addressNumericValue m_imageBase;
    // The memory size
    uint m_memorySize;
    // The section list
    cList<GeneralSection> m_sectionList;

    /* Arrays containing information about which addresses are
       defined as read, write and executable */
    // TODO: Create a dedicated data structure for this
    //cBuffer m_readMap;
    cBuffer m_writeMap;
    cBuffer m_execMap;
};

typedef cSmartPtr<SectionMemoryInterface> SectionMemoryInterfacePtr;

#endif // __TBA_DISMOUNT_SECTIONMEMORYINTERFACE_H