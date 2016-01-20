/*
 * FlowMapper.cpp
 *
 * Implementation file
 *
 * Author: Tal Harel
 */
#include "dismount/dismount.h"
#include "xStl/stream/ioStream.h"
#include "xStl/stream/fileStream.h"
#include "xStl/data/datastream.h"
#include "xStl/../../tests/tests.h"
#include "dismount/FlowMapper.h"
#include "dismount/FlowMapperException.h"

FlowMapper::MapAction FlowMapper::handleSingleOpcode(OpcodePtr& opcode,
                                                     WalkParametersStackObjectPtr& saveStack,
                                                     const ProcessorAddress& startAddress,
                                                     const ProcessorAddress& forcedEndAddress,
                                                     const bool isFaultTolerant /* = false */)
{
    // Save the opcode address we're trying to parse
    ProcessorAddress nextOpcodeLocation(gNullPointerProcessorAddress);
    m_disassembler->getNextOpcodeLocation(nextOpcodeLocation);
    m_lastOpcode = nextOpcodeLocation;

    // Read the next opcode
    XSTL_TRY
    {
        opcode = m_disassembler->next();
        m_lastParsedOpcode = opcode;
    }
    XSTL_CATCH(DisassemblerInvalidOpcodeException& e)
    {
        if (isFaultTolerant)
            return FlowMapper::MAP_QUIT_INVALID;
        throw(e);
    }

    // Get the current opcode address
    ProcessorAddress currAddress(gNullPointerProcessorAddress);
    if (!opcode->getOpcodeAddress(currAddress))
        XSTL_THROW(FlowMapperException);

    // Check if we forced an end to the subset and if we are on it
    if((0 != forcedEndAddress.getAddress()) &&
       (forcedEndAddress.getAddress() <= currAddress.getAddress()))
    {
        if (Opcode::FLOW_RET & opcode->getAlterProperty())
            return FlowMapper::MAP_BREAK;
        return FlowMapper::MAP_POTENTIAL;
    }

    // Check if we have visited this address
    XSTL_TRY
    {
        if (isVisited(currAddress))
        {
            // If we are at the start of the block - quit entirely
            if (currAddress == startAddress)
                return FlowMapper::MAP_QUIT_VALID;
            else
                return FlowMapper::MAP_BREAK;
        }
    }
    XSTL_CATCH(cException& e)
    {
        if (isFaultTolerant)
            return FlowMapper::MAP_QUIT_INVALID;
        throw(e);
    }

    // Set address as visited
    markVisited(currAddress);

    // Check if the address is defined as executable
    if(!isExecutable(currAddress))
        return FlowMapper::MAP_BREAK;

    // Check if this is a flow altering opcode
    if (opcode->isBranch())
    {
        // Will tell us if this is an unconditional JMP instruction
        bool jmpAlways = false;

        // Check for a RET opcode
        if (Opcode::FLOW_RETF == opcode->getAlterProperty())
        {
            if (isFaultTolerant)
                return FlowMapper::MAP_QUIT_INVALID;
            return FlowMapper::MAP_BREAK;
        }
        if (Opcode::FLOW_RET & opcode->getAlterProperty())
        {
            // If we are at the start of the block - quit entirely
            if (isFaultTolerant && (currAddress == startAddress))
                return FlowMapper::MAP_QUIT_VALID;
            else
                return FlowMapper::MAP_BREAK;
        }
        // Invalid opcodes
        if (Opcode::FLOW_INVALID == opcode->getAlterProperty())
            return FlowMapper::MAP_POTENTIAL;
        // Check for unconditional JMPs
        if ((Opcode::FLOW_COND_ALWAYS == opcode->getAlterProperty()) ||
            ((Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_ACTION) == opcode->getAlterProperty()))
            jmpAlways = true;

        // For JMPs belonging to a switch statement: Make sure we don't analyze the jump table
        if (opcode->isSwitch())
            markVisited(ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                         opcode->getSwitchTableOffset() - m_memoryInterface->getImageBase()));

        // Get the address to jump to from the opcode operand
        OpcodeFormatterPtr opcodeFormatter = m_disassembler->getOpcodeFormat(opcode, *m_formatter);
        ProcessorAddress jmpAddress(gNullPointerProcessorAddress);
        uint operandType = opcodeFormatter->parseOperandAddress(jmpAddress);

        // Check if the 'relocated' address is in a writable section
        if (ia32dis::OPND_MODRM_dWORDPTR == operandType)
        {
            // If it is, omit the destination address (We only use the caller address)
            if (isWritable(ProcessorAddress(ProcessorAddress::PROCESSOR_32,
                                            jmpAddress.getAddress() - m_memoryInterface->getImageBase())))
                jmpAddress.setAddress(0);
            else
            {
                if (jmpAlways)
                    return FlowMapper::MAP_BREAK;
                else
                    return FlowMapper::MAP_CONTINUE;
            }
        }

        /* In case of an operand that's not an address
           or an executable address, or if we have
           visited the address before - we treat it
           like a normal opcode and continue on.
           (Break if this is an unconditional JMP) */
        XSTL_TRY
        {
            if ((0 != jmpAddress.getAddress()) &&
                !isExecutable(jmpAddress))
            {
                if (jmpAlways)
                    return FlowMapper::MAP_BREAK;
                else
                    return FlowMapper::MAP_CONTINUE;
            }
        }
        XSTL_CATCH(cException& e)
        {
            if (isFaultTolerant)
                return FlowMapper::MAP_QUIT_INVALID;
            throw(e);
        }

        // Check if we visited the address before
        XSTL_TRY
        {
            if ((0 != jmpAddress.getAddress()) && isVisited(jmpAddress))
            {
                // If we have potential subsets
                if (0 != m_potentialSubsets.length())
                {
                    // Search the jump address in the potential list
                    cList<CodeSubset>::iterator subsetIter = m_potentialSubsets.begin();
                    cList<WalkParametersStackObjectPtr>::iterator stackIter = m_potentialStacks.begin();
                    for (; subsetIter != m_potentialSubsets.end(); subsetIter++, stackIter++)
                    {
                        if (jmpAddress == (*subsetIter).m_startAddress)
                        {
                            // We found the address, append it to the subset list
                            m_tempListMap.append(*subsetIter);
                            m_potentialSubsets.remove(subsetIter);

                            while(!(*stackIter)->isEmpty())
                            {
                                // Get the current walk parameters from the top of the stack
                                ProcessorAddress dummyAddress(gNullPointerProcessorAddress);
                                JumpInstruction currWalkParameters(dummyAddress,
                                                                   dummyAddress,
                                                                   OpcodePtr(),
                                                                   dummyAddress);
                                (*stackIter)->pop(currWalkParameters);

                                // Push the walk parameters to the actual global stack
                                m_walkStack.push(currWalkParameters);
                            }
                            m_potentialStacks.remove(stackIter);
                            break;
                        }
                    }
                }

                // Finally, continue on (Break if this is an unconditional JMP)
                if (jmpAlways)
                    return FlowMapper::MAP_BREAK;
                else
                    return FlowMapper::MAP_CONTINUE;
            }
        }
        XSTL_CATCH(cException& e)
        {
            if (isFaultTolerant)
                return FlowMapper::MAP_QUIT_INVALID;
            throw(e);
        }

        // Get the next opcode address
        ProcessorAddress nextOpcode(gNullPointerProcessorAddress);
        if (!m_disassembler->getNextOpcodeLocation(nextOpcode))
            XSTL_THROW(FlowMapperException);

        // Mark a jump to the operand address
        saveStack->push(JumpInstruction(jmpAddress,
                                        nextOpcode,
                                        opcode,
                                        ProcessorAddress(gNullPointerProcessorAddress)));

        // Finally, if this is an uncoditional JMP, end this block
        if (jmpAlways)
            return FlowMapper::MAP_BREAK;
    }

    return FlowMapper::MAP_NORMAL;
}

bool FlowMapper::walk(const ProcessorAddress& startAddress,
                      const ProcessorAddress& returnAddress,
                      const OpcodePtr& callingOpcode,
                      const ProcessorAddress& forcedEndAddress,
                      const bool isFaultTolerant /* = false */)
{
    // Get the caller address
    ProcessorAddress callerAddress(gNullPointerProcessorAddress);
    if (!callingOpcode->getOpcodeAddress(callerAddress))
        XSTL_THROW(FlowMapperException);

    // Create a subset for branches for whom we couldn't parse the destination address
    if (0 == startAddress.getAddress())
    {
        m_tempListMap.append(CodeSubset(ProcessorAddress(gNullPointerProcessorAddress),
                                        ProcessorAddress(gNullPointerProcessorAddress),
                                        callerAddress,
                                        Opcode::FLOW_NO_ALTER,
                                        callingOpcode->getAlterProperty()));
        return true;
    }

    OpcodePtr endOpcode;
    bool isPotential = false;
    WalkParametersStackObjectPtr saveStack(new WalkParametersStackObject());

    XSTL_TRY
    {
        // Loop until a break or an End-Of-Stream exception
        while (true)
        {
            // Read and parse the next opcode
            OpcodePtr opcode;
            FlowMapper::MapAction action = handleSingleOpcode(opcode,
                                                              saveStack,
                                                              startAddress,
                                                              forcedEndAddress,
                                                              isFaultTolerant);

            // Decide what to do based on the returned action
            if (FlowMapper::MAP_BREAK == action)
            {
                // Set the end opcode as the current opcode
                endOpcode = opcode;
                break;
            }
            else if (FlowMapper::MAP_POTENTIAL == action)
            {
                // Set a potential subset and the end opcode as the current opcode
                endOpcode = opcode;
                isPotential = true;
                break;
            }
            else if (FlowMapper::MAP_CONTINUE == action)
                continue;
            else if (FlowMapper::MAP_QUIT_VALID == action)
                return true;
            else if (FlowMapper::MAP_QUIT_INVALID == action)
                return false;
        }
    }
    XSTL_CATCH (DisassemblerEndOfStreamException&)
    {
        // End of stream. Nothing to do.
    }

    // Get the subset end address
    ProcessorAddress endAddress(gNullPointerProcessorAddress);
    if (!endOpcode->getOpcodeAddress(endAddress))
        XSTL_THROW(FlowMapperException);

    // Create new subset with the given bounds
    if (isPotential)
    {
        m_potentialSubsets.append(CodeSubset(startAddress,
                                             endAddress,
                                             callerAddress,
                                             endOpcode->getAlterProperty(),
                                             callingOpcode->getAlterProperty()));
        m_potentialStacks.append(saveStack);
    }
    else
    {
        m_tempListMap.append(CodeSubset(startAddress,
                                        endAddress,
                                        callerAddress,
                                        endOpcode->getAlterProperty(),
                                        callingOpcode->getAlterProperty()));
        while(!saveStack->isEmpty())
        {
            // Get the current walk parameters from the top of the stack
            ProcessorAddress dummyAddress(gNullPointerProcessorAddress);
            JumpInstruction currWalkParameters(dummyAddress,
                                               dummyAddress,
                                               OpcodePtr(),
                                               dummyAddress);
            saveStack->pop(currWalkParameters);

            // Push the walk parameters to the actual global stack
            m_walkStack.push(currWalkParameters);
        }
    }

    return true;
}

uint FlowMapper::map(const addressNumericValue start,
                     const addressNumericValue forcedEnd /* = 0 */,
                     const bool isFaultTolerant /* = false */)
{
    // Create object for the start and the forced end addresses
    ProcessorAddress startProcessorAddress(ProcessorAddress::PROCESSOR_32, start);
    ProcessorAddress forcedEndAddress(ProcessorAddress::PROCESSOR_32, forcedEnd);

    // Clear data structures
    m_tempListMap.removeAll();
    m_walkStack.clear();

    // Push the initial walk parameters to the stack
    m_walkStack.push(JumpInstruction(startProcessorAddress,
                                     ProcessorAddress(gNullPointerProcessorAddress),
                                     OpcodePtr(new InvalidOpcodeByte(
                                                   0,
                                                   true,
                                                   ProcessorAddress(gNullPointerProcessorAddress))),
                                     forcedEndAddress));

    // Loop until the stack is empty
    while(!m_walkStack.isEmpty())
    {
        // Get the current walk parameters from the top of the stack
        ProcessorAddress dummyAddress(gNullPointerProcessorAddress);
        JumpInstruction currWalkParameters(dummyAddress,
                                           dummyAddress,
                                           OpcodePtr(),
                                           dummyAddress);
        m_walkStack.pop(currWalkParameters);

        // Jump to the start address for the walk
        if (0 != currWalkParameters.m_jmpAddress.getAddress())
        {
            uint rawAddress =
                m_memoryInterface->virtualToRawAddress(currWalkParameters.m_jmpAddress.getAddress());
            if (0 == rawAddress) continue;
            m_disassembler->jumpToAddress(currWalkParameters.m_jmpAddress, rawAddress);
        }

        // Perform the walk
        if (!walk(currWalkParameters.m_jmpAddress,
                 currWalkParameters.m_returnAddress,
                 currWalkParameters.m_callingOpcode,
                 currWalkParameters.m_forcedEndAddress,
                 isFaultTolerant))
            return 0;
    }

    // Add all the subsets created under this address to the actual list map
    if (0 < m_tempListMap.length())
        for (cList<CodeSubset>::iterator iter = m_tempListMap.begin();
             iter != m_tempListMap.end();
             iter++)
            m_listMap.append(*iter);

    return m_listMap.length();
}

bool FlowMapper::dumpMapList(BasicOutputPtr& outputObject)
{
    if(0 == m_listMap.length())
        return false;

    XSTL_TRY
    {
        // Write the length of the list
        uint8 sizeBuffer[sizeof(DWORD)];
        outputObject->writeUint32(sizeBuffer, m_listMap.length());
        outputObject->write(sizeBuffer, sizeof(DWORD));

        // Enumerate the subsets and write them to the file
        for (cList<CodeSubset>::iterator iter = m_listMap.begin(); iter != m_listMap.end(); iter++)
        {
            // Write the address DWORDs to buffers
            uint8 startAddressBuffer[sizeof(DWORD)],
                  endAddressBuffer[sizeof(DWORD)],
                  callerAddressBuffer[sizeof(DWORD)],
                  endAlterPropertyBuffer[sizeof(DWORD)],
                  callerAlterPropertyBuffer[sizeof(DWORD)];
            outputObject->writeUint32(startAddressBuffer, (uint32)(*iter).m_startAddress.getAddress());
            outputObject->writeUint32(endAddressBuffer, (uint32)(*iter).m_endAddress.getAddress());
            outputObject->writeUint32(callerAddressBuffer, (uint32)(*iter).m_callerAddress.getAddress());
            outputObject->writeUint32(endAlterPropertyBuffer, (uint32)(*iter).m_endAlterProperty);
            outputObject->writeUint32(callerAlterPropertyBuffer, (uint32)(*iter).m_callerAlterProperty);

            // Write the addresses to the file
            outputObject->write(startAddressBuffer, sizeof(DWORD));
            outputObject->write(endAddressBuffer, sizeof(DWORD));
            outputObject->write(callerAddressBuffer, sizeof(DWORD));
            outputObject->write(endAlterPropertyBuffer, sizeof(DWORD));
            outputObject->write(callerAlterPropertyBuffer, sizeof(DWORD));
        }
    }
    XSTL_CATCH_ALL
    {
        return false;
    }

    return true;
}

bool FlowMapper::loadMapList(BasicInputPtr& inputObject)
{
    // Start with a clean list
    m_listMap.removeAll();

    XSTL_TRY
    {
        // Read the length of the list
        uint8 sizeBuffer[sizeof(DWORD)];
        inputObject->read(sizeBuffer, sizeof(DWORD));
        uint32 listLength = inputObject->readUint32(sizeBuffer);

        for (uint i = 0; i < listLength; i++)
        {
            // Read the address buffers from the file
            uint8 startAddressBuffer[sizeof(DWORD)],
                  endAddressBuffer[sizeof(DWORD)],
                  callerAddressBuffer[sizeof(DWORD)],
                  endAlterPropertyBuffer[sizeof(DWORD)],
                  callerAlterPropertyBuffer[sizeof(DWORD)];
            inputObject->read(startAddressBuffer, sizeof(DWORD));
            inputObject->read(endAddressBuffer, sizeof(DWORD));
            inputObject->read(callerAddressBuffer, sizeof(DWORD));
            inputObject->read(endAlterPropertyBuffer, sizeof(DWORD));
            inputObject->read(callerAlterPropertyBuffer, sizeof(DWORD));

            // Parse the address buffers to DWORDs
            uint32 startAddress = inputObject->readUint32(startAddressBuffer);
            uint32 endAddress = inputObject->readUint32(endAddressBuffer);
            uint32 callerAddress = inputObject->readUint32(callerAddressBuffer);
            int endAlterProperty = (int)inputObject->readUint32(endAlterPropertyBuffer);
            int callerAlterProperty = (int)inputObject->readUint32(callerAlterPropertyBuffer);

            // Create a new node
            m_listMap.append(CodeSubset(ProcessorAddress(ProcessorAddress::PROCESSOR_32, startAddress),
                                        ProcessorAddress(ProcessorAddress::PROCESSOR_32, endAddress),
                                        ProcessorAddress(ProcessorAddress::PROCESSOR_32, callerAddress),
                                        endAlterProperty,
                                        callerAlterProperty));
        }
    }
    XSTL_CATCH_ALL
    {
        return false;
    }

    return true;
}

void FlowMapper::initHasVisited()
{
    // Create the array, initialized to zero
    uint visitedLength = 8 * ((m_inputStream->length() / 8) + 1);
    m_hasVisited = cBuffer(visitedLength);
    for (uint i = 0; i < visitedLength; i++)
        m_hasVisited[i] = 0;
}

void FlowMapper::markVisited(const ProcessorAddress& address)
{
    m_hasVisited[(uint)address.getAddress() / 8] |=
        (1 << ((uint)address.getAddress() % 8));
}

bool FlowMapper::isVisited(const ProcessorAddress& address)
{
    return (0 != (m_hasVisited[(uint)address.getAddress() / 8] &
                  (1 << ((uint)address.getAddress() % 8))));
}

bool FlowMapper::isExecutable(const ProcessorAddress& address)
{
    return m_memoryInterface->checkAddress(address.getAddress(),
                                           SectionMemoryInterface::SECTION_FLAG_EXECUTABLE);
}

bool FlowMapper::isWritable(const ProcessorAddress& address)
{
    return m_memoryInterface->checkAddress(address.getAddress(),
                                           SectionMemoryInterface::SECTION_FLAG_WRITE);
}

FlowMapper::FlowMapper(const BasicInputPtr& inputStream,
                       const SectionMemoryInterfacePtr& memoryInterface) :
    m_inputStream(inputStream),
    m_memoryInterface(memoryInterface),
    m_lastOpcode(gNullPointerProcessorAddress)
{
    // Initialize the isVisited map
    initHasVisited();

    /* Generate the disassembler. Will throw exception in case of
       invalid opcode */
    m_disassembler = StreamDisassemblerFactory::disassemble(
            OpcodeSubsystems::DISASSEMBLER_INTEL_32,
            inputStream,
            true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0), false);

    // Prepare the default data-formatter
    m_formatter = DefaultOpcodeDataFormatterPtr(new DefaultOpcodeDataFormatter(OPCODE_MARGIN));
}

void FlowMapper::getMapList(cList<CodeSubset>& listMap)
{
    listMap = m_listMap;
}
OpcodeFormatterPtr FlowMapper::getFormatter(OpcodePtr& opcode)
{
  return m_disassembler->getOpcodeFormat(opcode, *m_formatter);
}
