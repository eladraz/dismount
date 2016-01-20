#ifndef __TBA_DISMOUNT_INTEGERENCODING_H
#define __TBA_DISMOUNT_INTEGERENCODING_H

/*
 * IntegerEncoding.h
 *
 * The integer-encoding holds the different encoding format a number can
 * sustained.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"

/*
 * Store the encoding format of a number.
 * The 7 bit holds the method of the encoding. If it equal to 1 then the number
 * is stored in big-endian, otherwise it's stored in little endian.
 */
class IntegerEncoding {
public:
    // The different encoding-type
    typedef enum {
        // Used to mark NULL number (Which is different than zero, the number is
        // not exist)
        INTEGER_NOT_EXIST = 0,
        // The number of bits of the number
        INTEGER_8BIT = 1,
        INTEGER_16BIT = 2,
        INTEGER_32BIT = 4,
        INTEGER_64BIT = 8,

        // The size of the integer
        INTEGER_SIZE_MASK = 0xF,

        // Intel notation, 0x4433 ->  0:33 1:44
        INTEGER_LITTLE_ENDIAN = 0,
        // Intel notation, 0x4433 ->  0:44 1:33
        INTEGER_BIG_ENDIAN    = 128
    } IntegerEncodingType;

    // The integer-encoding can be pack into a single 8 bit number
    typedef uint8 IntegerEncodingPack;
};

#endif // __TBA_DISMOUNT_INTEGERENCODING_H
