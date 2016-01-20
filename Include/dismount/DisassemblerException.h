#ifndef __TBA_DISMOUNT_DISASSEMBLEREXCEPTION_H
#define __TBA_DISMOUNT_DISASSEMBLEREXCEPTION_H

/*
 * DisassemblerException.h
 *
 * Base root exception class. Used for run-time filter over all the Dismount
 * exceptions.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/exceptions.h"

class DisassemblerException : public cException {
public:
    // Default constructor.
    DisassemblerException(char * file, uint32 line) : cException(file, line, NULL, 0)  {};
};

#endif // __TBA_DISMOUNT_DISASSEMBLEREXCEPTION_H
