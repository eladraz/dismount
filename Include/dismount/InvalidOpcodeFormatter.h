#ifndef __TBA_DISMOUNT_INVALIDOPCODEFORMATTER_H
#define __TBA_DISMOUNT_INVALIDOPCODEFORMATTER_H

/*
 * InvalidOpcodeFormatter.h
 *
 * The invalid opcode formatter translate InvalidOpcode instruction into a
 * human readable string.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeFormatter.h"
#include "dismount/OpcodeDataFormatter.h"

/*
 * Translate invalid-opcode into a string, such as "DB 66" etc...
 */
class InvalidOpcodeFormatter : public OpcodeFormatter {
public:
    /*
     * Constructor.
     *
     * instruction   - The InvalidOpcodeByte class type.
     * dataFormatter - The data-formatter to be used when showing the content
     *                 of the data-stream
     *
     * Throw exception if the instruction is not InvalidOpcodeByte disassembler
     * type.
     */
    InvalidOpcodeFormatter(const OpcodePtr& instruction,
                           OpcodeDataFormatter& dataFormatter);

    /*
     * See OpcodeFormatter::string.
     * Return the following:
     *    Opcode:       "DB"
     *    Intermediate: "XX"
     * Where the intermediate is the faulting instruction.
     */
    virtual cString string(OpcodeFormatStruct* formatStruct = NULL) const;

    /*
     * See OpcodeFormatter::parseOperandAddress()
     */
    uint parseOperandAddress(ProcessorAddress& address);

private:
    // The data-formatter to be used.
    OpcodeDataFormatter& m_dataFormatter;
    // The byte to be shown.
    OpcodePtr m_data;
    // The DB string
    static const character* gInvalidOpcodeStringName;
};

#endif // __TBA_DISMOUNT_INVALIDOPCODEFORMATTER_H
