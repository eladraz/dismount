#ifndef __TBA_DISMOUNT_DISASSEMBLERENDOFSTREAMEXCEPTION_H
#define __TBA_DISMOUNT_DISASSEMBLERENDOFSTREAMEXCEPTION_H

/*
 * DisassemblerEndOfStreamException.h
 *
 * Run-time information class which indicates that the disassembler had a
 * problem decoding the next instruction since the stream reaches it's EOS.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/DisassemblerException.h"
class DisassemblerEndOfStreamException : public DisassemblerException {
public:
    // Default constructor.
    DisassemblerEndOfStreamException(char * file, uint32 line) : DisassemblerException(file, line)  {};
};

#endif // __TBA_DISMOUNT_DISASSEMBLERENDOFSTREAMEXCEPTION_H
