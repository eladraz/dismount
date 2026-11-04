#include "dismount/dismount.h"
/*
 * AssemblingFactory.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/assembler/proc/arm/ArmAssembler.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/assembler/proc/riscv/RiscvAssembler.h"

AssemblerInterfacePtr AssemblingFactory::generateAssembler(
                                        FirstPassBinaryPtr binary,
                                        OpcodeSubsystems::DisassemblerType type)
{
    switch (type)
    {
    case OpcodeSubsystems::DISASSEMBLER_INTEL_32:
        return AssemblerInterfacePtr(new IA32Assembler(
                                                binary,
                                                IA32eInstructionSet::INTEL_32));
    case OpcodeSubsystems::DISASSEMBLER_AMD_64:
        return AssemblerInterfacePtr(new IA32Assembler(
                                                binary,
                                                IA32eInstructionSet::AMD_64));
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_LE:
    case OpcodeSubsystems::DISASSEMBLER_THUMB_LE:
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_LE:
        return AssemblerInterfacePtr(new ArmAssembler(
                                    binary,
                                    ArmInstructionSet::fromSubsystem(type)));
    case OpcodeSubsystems::DISASSEMBLER_RISCV_32:
    case OpcodeSubsystems::DISASSEMBLER_RISCV_64:
        return AssemblerInterfacePtr(new RiscvAssembler(
                                    binary,
                                    RiscvInstructionSet::fromSubsystem(type)));
    default:
        // Not ready yet...
        CHECK_FAIL();
    }
}

