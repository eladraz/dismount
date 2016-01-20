#ifndef __TBA_DISMOUNT_STREAMDISASSEMBLERFACTORY_H
#define __TBA_DISMOUNT_STREAMDISASSEMBLERFACTORY_H

/*
 * StreamDisassemblerFactory.h
 *
 * The factory to generate an disassembler stream
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/stream/basicIO.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/ProcessorAddress.h"

/*
 * Here is a complete usage for all disassemble path:
 *
 *   // Assume: MyOpcodeDataFormatter, SystemThread
 *
 *   // Build the formatter
 *   MyOpcodeDataFormatter formatter;
 *
 *   // Build the disassembler
 *   StreamDisassmblerPtr dis = StreamDisassmblerFactory::disassmble(
 *       // Return the assembly language type. According to this value the
 *       // factory will build the right disassembler.
 *       SystemThread::getThreadType(),
 *       // Return a stream of data starting
 *       SystemThread::getThreadEntry(),
 *       // Return the start address for the disassemble.
 *       true,
 *       SystemThread::getThreadEntryAddress()));
 *
 *   // Read an instruction and return the content of the instruction, and in
 *   // our case returns the instruction address
 *   OpcodePtr firstInstruction = dis.next();
 *   // Get the viewer object
 *   OpcodeFormatterPtr formatter = dis.getOpcodeFormat(firstInstruction,
 *                                                      formatter,
 *                                                      INTEL_NOTATION);
 *   cout << formatter.string() << endl;
 */
class StreamDisassemblerFactory {
public:
    /*
     * Factory. Return the disassembler engine.
     *
     * type             - Choose the disassembler engine by the type of the
     *                    instructions
     * inputStream      - The instruction pipe-line. See note later.
     * shouldUseAddress - Set to true if the disassembler engine should follow
     *                    memory instructions
     * streamAddress    - If 'shouldUseAddress' is true, the address of the
     *                    starting instruction in the stream
     * shouldOpcodeFaultTolerantEnabled - Set to true inorder to indicate that
     *                    when invalid-opcode accept than the disassembler should
     *                    instance "InvalidOpcodeByte". Set to false in order to
     *                    throw exception back to the disassembler process unit.
     *
     * NOTE:
     *    The input stream should have the following properties:
     *       - Reading bytes
     *       - Detecting end-of-stream
     *       - Seeking XXX character from the beginning
     *       - Getting the stream pointer
     */
    static StreamDisassemblerPtr disassemble(
            OpcodeSubsystems::DisassemblerType type,
            const BasicInputPtr& inputStream,
            bool shouldUseAddress = false,
            const ProcessorAddress& streamAddress = gNullPointerProcessorAddress,
            bool shouldOpcodeFaultTolerantEnabled = true);
};

#endif // __TBA_DISMOUNT_STREAMDISASSEMBLERFACTORY_H
