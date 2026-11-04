/*
 * ArchitectureDescription.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"

ArchitectureDescriptionPtr ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DisassemblerType type)
{
    switch (type)
    {
    case OpcodeSubsystems::DISASSEMBLER_INTEL_16:
    case OpcodeSubsystems::DISASSEMBLER_INTEL_32:
        return ArchitectureDescriptionPtr(new IA32Architecture(type));
    case OpcodeSubsystems::DISASSEMBLER_AMD_64:
        return ArchitectureDescriptionPtr(new AMD64Architecture());
    case OpcodeSubsystems::DISASSEMBLER_ARM_32_LE:
    case OpcodeSubsystems::DISASSEMBLER_THUMB_LE:
        // A32 and T32 share the description, the T bit selects between them
        return ArchitectureDescriptionPtr(new ArmArchitecture());
    case OpcodeSubsystems::DISASSEMBLER_ARM_64_LE:
        return ArchitectureDescriptionPtr(new Arm64Architecture());
    case OpcodeSubsystems::DISASSEMBLER_RISCV_32:
        return ArchitectureDescriptionPtr(new RiscvArchitecture(
                                RiscvInstructionSet::RISCV_RV32));
    case OpcodeSubsystems::DISASSEMBLER_RISCV_64:
        return ArchitectureDescriptionPtr(new RiscvArchitecture(
                                RiscvInstructionSet::RISCV_RV64));
    default:
        // No description for this processor
        CHECK_FAIL();
    }
}
