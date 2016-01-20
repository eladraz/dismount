#ifndef __TBA_DISMOUNT_FLOWMAPPER_H
#define __TBA_DISMOUNT_FLOWMAPPER_H

/*
 * FlowMapper.h
 *
 * Maps the flow of a program (calls, jmps etc...), starting from a given
 * address, by dividing it into code subsets.
 *
 * Known faults:
 * - The algorithm does not reach blocks whose addresses
 *   are pushed to stack or MOV'd to a register:
 *      - Exception handlers and filters (address pushed into stack)
 *      - Switch statements (the call is to an offset plus an index
 *        stored in a register)
 * - Code blocks that IDA gets using FLIRT
 * - Tries to treat non-functions that are exported as functions
 *   (FsRtlLegalAnsiCharacterArray in ntkrnlpa.exe, for instance)
 *   and does not stop until forced to
 *
 * Author: Tal Harel
 */

#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/proc/ia32/IA32Opcode.h"
#include "dismount/assembler/Stack.h"
#include "dismount/SectionMemoryInterface.h"

#define NUMBER_OF_OPCODE (7)
#define OPCODE_MARGIN (10)

class FlowMapper {
public:
    // Defines how to proceed after reading an opcode
    typedef enum {
        MAP_NORMAL = 0,
        MAP_BREAK,
        MAP_CONTINUE,
        MAP_QUIT_VALID,
        MAP_QUIT_INVALID,
        MAP_POTENTIAL,
    } MapAction;

    // Defines a chunk of machine code, its caller address and their alter properties
    struct CodeSubset {
        ProcessorAddress m_startAddress;
        ProcessorAddress m_endAddress;
        ProcessorAddress m_callerAddress;
        int m_endAlterProperty;
        int m_callerAlterProperty;

        CodeSubset(ProcessorAddress startAddress,
                   ProcessorAddress endAddress,
                   ProcessorAddress callerAddress,
                   int endAlterProperty,
                   int callerAlterProperty) : m_startAddress(startAddress),
                                              m_endAddress(endAddress),
                                              m_callerAddress(callerAddress),
                                              m_endAlterProperty(endAlterProperty),
                                              m_callerAlterProperty(callerAlterProperty)
        {}
    };

    /*
     * Constructor.
     *
     * inputStream - A stream object to read the data from
     * memoryInterface - A SectionMemoryInterface object used as an
     *                   interface between the flow mapper and the PE
     *                   memory and section attributes
     *
     * Initializes member variables
     */
    FlowMapper(const BasicInputPtr& inputStream,
               const SectionMemoryInterfacePtr& memoryInterface);

    /*
     * Virtual Destructor.
     */
    virtual ~FlowMapper() {};

    /*
     * Performs the mapping process.
     *
     * start - The stream address for the start of the map
     * forcedEnd - The absolute final address that, if defined,
     *             determines where the map will stop, if it hasn't
     *             yet stopped of its own accord.
     * isFaultTolerant - Decides if we should raise an exception on
     *                   error, or if we should stop the parsing while
     *                   throwing out all subsets parsed under the
     *                   faulting instruction.
     *
     * Initializes the needed disassembly objects and calls the
     * walk() function from the starting memory location.
     * Returns the number of subsets found.
     */
    uint map(const addressNumericValue start,
             const addressNumericValue forcedEnd = 0,
             const bool isFaultTolerant = false);

    /*
     * Writes the list built during the mapping process to an output
     * object in a binary format:
     *  - List size (DWORD)
     *  - For each list member:
     *      - Subset start address (DWORD)
     *      - Subset end address (DWORD)
     *      - Caller address (DWORD)
     *      - End alter property (DWORD)
     *      - Caller alter property (DWORD)
     *
     * outputObject - The object to write to
     *
     * Returns true if the list was properly written.
     * false if the list is empty or if there was a problem writing.
     */
    bool dumpMapList(BasicOutputPtr& outputObject);

    /*
     * Loads from an input object (in the format specified in dumpMapList)
     * to the map list object.
     *
     * inputObject - The object to read from
     *
     * Returns true if the list was properly loaded.
     * false if there was a problem reading from the object.
     */
    bool loadMapList(BasicInputPtr& inputObject);

    /*
     * Returns the code subset list built during the mapping process.
     *
     * listMap - Will be filled with the list
     */
    void getMapList(cList<CodeSubset>& listMap);

    /*
     * Returns the disassembler object for parsing the opcodes in the stream.
     */
    inline StreamDisassemblerPtr& getDisassembler() { return m_disassembler;};

    /*
     * Returns the opcode foramtter object for correctly formatting the disassembly string.
     *
     * opcode - The opcode object that needs formatting
     */
    OpcodeFormatterPtr getFormatter(OpcodePtr& opcode);

    /*
     * Returns the last opcode that was parsed correctly
     */
    inline OpcodePtr getLastParsedOpcode() { return m_lastParsedOpcode;};

    /*
     * Returns the last opcode address that we tried to parse
     */
    inline ProcessorAddress getLastOpcode() { return m_lastOpcode;};

private:
    // Collects the necessary data for a JMP instruction
    struct JumpInstruction {
        ProcessorAddress m_jmpAddress;
        ProcessorAddress m_returnAddress;
        OpcodePtr m_callingOpcode;
        ProcessorAddress m_forcedEndAddress;

        JumpInstruction(ProcessorAddress jmpAddress,
                        ProcessorAddress returnAddress,
                        OpcodePtr callingOpcode,
                        ProcessorAddress forcedEndAddress) : m_jmpAddress(jmpAddress),
                                                             m_returnAddress(returnAddress),
                                                             m_callingOpcode(callingOpcode),
                                                             m_forcedEndAddress(forcedEndAddress)
        {}
    };

    // Typedefs for a stack of walk function parameters
    typedef cList<JumpInstruction>::iterator WalkParamIter;
    typedef StackInrastructor<JumpInstruction, WalkParamIter> WalkParametersStackObject;
    typedef cSmartPtr<WalkParametersStackObject> WalkParametersStackObjectPtr;

    /*
     * Prevent copy constructor
     */
    FlowMapper(const FlowMapper& other);

    /*
     * Initializes the array that will contain information about addresses
     * that were visited during the mapping process.
     */
    void initHasVisited();

    /*
     * The internal, recursive function used for walking and disassembling
     * the code from a given start address. Branches out according to jumps and
     * calls within the code by calling itself on the jump addresses.
     * Creates a subset for the given start address and adds it to the map list.
     *
     * startAddress - The address in the file to start the walk from.
     * returnAddress - The return address of the current block. Used for creating
     *                 the code subset.
     * callingOpcode - The calling opcode that started this current block. Used
     *                 for creating the code subset.
     * forcedEndAddress - The absolute final address that, if defined,
     *                    determines where the map will stop, if it hasn't
     *                    yet stopped of its own accord.
     * isFaultTolerant - See the map function
     *
     */
    bool walk(const ProcessorAddress& startAddress,
              const ProcessorAddress& returnAddress,
              const OpcodePtr& callingOpcode,
              const ProcessorAddress& forcedEndAddress,
              const bool isFaultTolerant = false);

    /*
     * Reads the next opcode, parse it and decide how to continue, based
     * on the following logic:
     *  - Read the next opcode
     *  - Check if we are forced to end the block. Break walk if so.
     *  - Check for previous visitation. Break walk if so.
     *  - Set address as visited
     *  - Check for a flow altering opcode. If it is:
     *      - Check for ret\int\invalid. Break walk if so.
     *      - Get the jump address
     *      - Check for jump address validity. Continue walk if so. Break if JMP.
     *      - Check that we haven't yet visited the address. Continue walk if so. Break if JMP.
     *      - Mark jump information. Push into walk parameter stack.
     *      - Break if JMP
     *
     * opcode - A pointer the will contain the current opcode
     * saveStack - A stack object for pushing the walk paremeters into
     * startAddress - The starting address for the whole walk
     * forcedEndAddress - The forced end address for the whole walk
     * isFaultTolerant - See the map function
     *
     * Returns a MapAction enum that determines how to continue the walk.
     */
    MapAction handleSingleOpcode(OpcodePtr& opcode,
                                 WalkParametersStackObjectPtr& saveStack,
                                 const ProcessorAddress& startAddress,
                                 const ProcessorAddress& forcedEndAddress,
                                 const bool isFaultTolerant = false);

    /*
     * Marks the given address as visited, so that we will know
     * not to re-parse it
     *
     * address - The address to mark
     */
    void markVisited(const ProcessorAddress& address);

    /*
     * Checks if a given address was already visited and parsed
     *
     * address - The address to check
     *
     * Returns true if the address was already visited, false otherwise
     */
    bool isVisited(const ProcessorAddress& address);

    /*
     * Checks if a given address is defined as executable in the PE.
     *
     * address - The input address
     *
     * Returns true if the address is executable. false otherwise.
     */
    bool isExecutable(const ProcessorAddress& address);

    /*
     * Check if a given address is defined as writable in the PE.
     *
     * address - The input address
     *
     * Returns true if the address is writable. false otherwise.
     */
    bool isWritable(const ProcessorAddress& address);

    // The stream used to read the PE
    BasicInputPtr m_inputStream;
    // The disassembler object for parsing the opcodes in the stream
    StreamDisassemblerPtr m_disassembler;
    // The opcode foramtter object for correctly formatting the disassembly string
    DefaultOpcodeDataFormatterPtr m_formatter;
    // The list that contains the code subsets built during the mapping process
    cList<CodeSubset> m_listMap;
    // A temporary list containing subsets that are to be added to the final list
    cList<CodeSubset> m_tempListMap;
    // A temporary list containing subsets that have been marked as potential
    cList<CodeSubset> m_potentialSubsets;
    // A list containing the call stacks corresponding with the potential subsets
    cList<WalkParametersStackObjectPtr> m_potentialStacks;
    // A stack responsible for managing walk function calls and their parameters
    WalkParametersStackObject m_walkStack;
    // An array that contains information about addresses that were visited during the mapping process.
    cBuffer m_hasVisited;
    // Used as an interface between the flow mapper and the PE memory and section attributes
    SectionMemoryInterfacePtr m_memoryInterface;
    // The last opcode that was parsed correctly
    OpcodePtr m_lastParsedOpcode;
    // The last opcode address we tried to parse
    ProcessorAddress m_lastOpcode;
};

typedef cSmartPtr<FlowMapper> FlowMapperPtr;

#endif // __TBA_DISMOUNT_FLOWMAPPER_H
