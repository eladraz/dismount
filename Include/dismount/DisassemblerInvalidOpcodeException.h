#ifndef __TBA_DISMOUNT_DISASSEMBLERINVALIDOPCODEEXCEPTION_H
#define __TBA_DISMOUNT_DISASSEMBLERINVALIDOPCODEEXCEPTION_H

/*
 * DisassemblerInvalidOpcodeException.h
 *
 * Run-time information class which indicates that the disassembler had a
 * problem decoding the next instruction since the opcode is invalid (One
 * of the argument or the instruction don't exist at the target platform)
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/DisassemblerException.h"
class DisassemblerInvalidOpcodeException : public DisassemblerException {
public:
    // Default constructor.
    DisassemblerInvalidOpcodeException(char * file, uint32 line) : DisassemblerException(file, line)  {};
};

#endif // __TBA_DISMOUNT_DISASSEMBLERINVALIDOPCODEEXCEPTION_H
