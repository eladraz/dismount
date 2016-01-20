#ifndef __TBA_DISMOUNT_FLOWMAPPEREXCEPTION_H
#define __TBA_DISMOUNT_FLOWMAPPEREXCEPTION_H

/*
* FlowMapperException.h
*
* Base root exception class. Used for run-time filter over all the Flow Mapper
* exceptions.
*
* Author: Tal Harel
*/

#include "xStl/exceptions.h"

class FlowMapperException : public cException {
public:
    // Default constructor.
    FlowMapperException(char * file, uint32 line);
};

#endif // __TBA_DISMOUNT_FLOWMAPPEREXCEPTION_H
