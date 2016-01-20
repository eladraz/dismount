#ifndef __TBA_DISMOUNT_DISMOUNTTRACE_H
#define __TBA_DISMOUNT_DISMOUNTTRACE_H

/*
 * DismountTrace.h
 *
 * The macro which trace out commands from the Dismount.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/stream/traceStream.h"


// Debug infrastructure
#ifdef TRACED_DISMOUNT
    // Send to trace stream
    #define DismountTrace(...)       traceHigh("Dismount: " << __VA_ARGS__)
#else
    // Ignore from compilation
    #define DismountTrace(x)
#endif


#endif // __TBA_CLR_ENGINE_ENGINETRACE_H
