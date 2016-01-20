#ifndef __TBA_DISMOUNT_ASSEMBLER_ASSEMBLINGFACTORY_H
#define __TBA_DISMOUNT_ASSEMBLER_ASSEMBLINGFACTORY_H

/*
 * AssemblingFactory.h
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/assembler/AssemblerInterface.h"

class AssemblingFactory {
public:
    /*
     * Generate new instance of assembler for specific platform
     *
     * binary - The first binary pass. Used to determine function stack and
     *          variables.
     * type   - The assembler engine
     */
    static AssemblerInterfacePtr generateAssembler(
                                    FirstPassBinaryPtr binary,
                                    OpcodeSubsystems::DisassemblerType type);
};

#endif // __TBA_DISMOUNT_ASSEMBLER_ASSEMBLINGFACTORY_H
