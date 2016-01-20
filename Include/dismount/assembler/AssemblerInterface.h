#ifndef __TBA_DISMOUNT_ASSEMBLER_ASSEMBLERINTERFACE_H
#define __TBA_DISMOUNT_ASSEMBLER_ASSEMBLERINTERFACE_H

/*
 * AssemblerInterface.h
 *
 * The assembler interface provide a way for assembly instruction for a CPU
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/assembler/FirstPassBinary.h"

/*
 * This class provides assembling capabilities. In order to use this class,
 * instance a class using AssemblingFactory and provide textual string
 * instruction. The instruction will compiled into FirstBinaryPass and
 * references will stored as "BinaryDependencies".
 *
 * NOTE: The design of AssemblerInterface can be used to compile entire
 *       application in a single instance, but it's best to separate each method
 *       into a different binary and then link all methods.
 *
 * Example:
 *    AssemblerInterfacePtr ia32assembler = AssemblerFatctory(...);
 *    cStringerStream& asm = ia32assembler->getAssembler();
 *    asm << "push  ebp"            << endl;
 *    asm << "mov   ebp, esp"       << endl;
 *    asm << "sub   esp, 0x20"      << endl;
 *    etc, etc...
 *
 * NOTE: This class is not thread-safe. Only one thread can work on a single
 *       instance.
*/
class AssemblerInterface {
public:
    /*
     * Return the assembler stream
     *
     * NOTE:
     *   Please be careful when modifying or accessing the first-binary pass.
     *   This class is not thread-safe and not auto-protect
     */
    virtual cStringerStreamPtr getAssembler() = 0;

    /*
     * Return a reference to the first binary pass.
     *
     * NOTE:
     *   Please be careful when modifying or accessing the first-binary pass.
     *   This class is not thread-safe and not auto-protect
     */
    virtual FirstPassBinaryPtr getFirstPass() = 0;
};

// The reference countable object
typedef cSmartPtr<AssemblerInterface> AssemblerInterfacePtr;

#endif // __TBA_DISMOUNT_ASSEMBLER_ASSEMBLERINTERFACE_H
