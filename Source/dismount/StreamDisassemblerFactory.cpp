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
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"

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
    case OpcodeSubsystems::DISASSEMBLER_AMD_64:
        // Generate 64bit disassembler
        return StreamDisassemblerPtr(new IA32StreamDisassembler(
                    IA32eInstructionSet::AMD_64,
                    data,
                    shouldUseAddress,
                    streamAddress,
                    shouldOpcodeFaultTolerantEnabled));
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_BE:
    case OpcodeSubsystems::DISASSEMBLER_THUMB_LE:
    case OpcodeSubsystems::DISASSEMBLER_THUMB_BE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_BE:
        // The ARM instructions are always little endian, the BE subsystems
        // are for big endian data. The disassembler decodes all the features
        return StreamDisassemblerPtr(new ArmStreamDisassembler(
                    ArmInstructionSet::fromSubsystem(type),
                    data,
                    shouldUseAddress,
                    streamAddress,
                    shouldOpcodeFaultTolerantEnabled));
    default:
        // I don't recognize the disassembler type.
        CHECK_FAIL();
    }
}

