#ifndef __TBA_DISMOUNT_PROCESSORADDRESS_H
#define __TBA_DISMOUNT_PROCESSORADDRESS_H

/*
 * ProcessorAddress.h
 *
 * Each processor has his own different number of address bits. IA32 uses 32bit
 * address. Itanium or XEON uses 64bit address, etc.
 * This module abstract this layer of addressing and provide a single shared
 * interface of which programmer can develop different disassembler without the
 * need to be restricted into 32bit numbers...
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"

/*
 * A variant representation of an address. The class should be constructed by
 * the disassembler.
 */
class ProcessorAddress {
public:
    /*
     * In order to use a single common numeric calculation platform over the
     * address range, two typedef are declared: 'uintAddress' and 'intAddress'.
     * Both of them declared as the largest integer types which any processor
     * can implement.
     * The relative addressing mode is used to substract/append numbers from
     * the address.
     */
    typedef uint64 uintAddress;
    typedef int64 intAddress;

    /*
     * The different address types for the different processors
     */
    enum ProcessorAddressType {
        // The processor uses 20bit address bit, used by x86, selector and
        // offset combined together into 32 bit number pack as: 16bit segment
        // and 16bit offset
        PROCESSOR_20,
        // The processor uses 32bit address bit
        PROCESSOR_32,
        // The processor uses 64bit address bit
        PROCESSOR_64,

        // Used for relative (near segment) address in Real-Mode
        PROCESSOR_16
    };

    /*
     * Constructor.
     *
     * type - The type of the processor-address. This value is fixed and cannot
     *        be change.
     * address - The address to be stored in the class
     *
     * Throw exception if 'type' is unknown processor addressing mode
     */
    ProcessorAddress(ProcessorAddressType type,
                     uintAddress address);

    /*
     * Copy-constructor and operator = will be implemented by the compiler
     */

    /*
     * Return the address type for this processors
     */
    ProcessorAddressType getAddressType() const;

    /*
     * Return the address pointed by this class.
     */
    uintAddress getAddress() const;

    /*
     * Change the address into new address value
     */
    void setAddress(uintAddress newAddress);

    /*
     * Cast the address into relative value. Used to expand the sign bit over
     * 'intAddress' type.
     */
    intAddress castRelativeAddress() const;

    /*
     * Implements relative relocation.
     */
    ProcessorAddress operator + (const intAddress& relative) const;

    /*
     * Implements the == operator for comparing against other ProcessorAddresses
     */
    bool operator == (const ProcessorAddress& other) const;

    /*
     * Implements the > operator for being able to sort ProcessorAddresses
     * by address
     */
    bool operator > (const ProcessorAddress& other) const;

private:
    // The address type for this processors
    ProcessorAddress::ProcessorAddressType m_type;
    // The address in union format, cast by 'm_type'
    union {
        uint16 m_16bitProcessorAddress;
        uint32 m_32bitProcessorAddress;
        uint64 m_64bitProcessorAddress;
    } m_value;
};

// The default NULL pointer address
extern const ProcessorAddress gNullPointerProcessorAddress;

#endif // __TBA_DISMOUNT_PROCESSORADDRESS_H
