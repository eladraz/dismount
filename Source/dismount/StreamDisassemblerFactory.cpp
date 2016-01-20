#include "dismount/dismount.h"
/*
 * StreamDisassemblerFactory.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"
#include "dismount/proc/ia32/IA32StreamDisassembler.h"

StreamDisassemblerPtr StreamDisassemblerFactory::disassemble(
                OpcodeSubsystems::DisassemblerType type,
                const BasicInputPtr& data,
                bool shouldUseAddress,
                const ProcessorAddress& streamAddress,
                bool shouldOpcodeFaultTolerantEnabled)
{
    switch (type)
    {
    case OpcodeSubsystems::DISASSEMBLER_INTEL_16:
        // Generate 16bit disassembler
        return StreamDisassemblerPtr(new IA32StreamDisassembler(
                    IA32eInstructionSet::INTEL_16,
                    data,
                    shouldUseAddress,
                    streamAddress,
                    shouldOpcodeFaultTolerantEnabled));
    case OpcodeSubsystems::DISASSEMBLER_INTEL_32:
        // Generate 32bit disassembler
        return StreamDisassemblerPtr(new IA32StreamDisassembler(
                    IA32eInstructionSet::INTEL_32,
                    data,
                    shouldUseAddress,
                    streamAddress,
                    shouldOpcodeFaultTolerantEnabled));
    default:
        // I don't recognize the disassembler type.
        CHECK_FAIL();
    }
}

