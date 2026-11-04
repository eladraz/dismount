#include "dismount/dismount.h"
/*
 * RiscvInstructionSet.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"

RiscvInstructionSet::DisassemblerTypes RiscvInstructionSet::fromSubsystem(
                                    OpcodeSubsystems::DisassemblerType type)
{
    switch (type)
    {
    case OpcodeSubsystems::DISASSEMBLER_RISCV_32:
        return RISCV_RV32;
    case OpcodeSubsystems::DISASSEMBLER_RISCV_64:
        return RISCV_RV64;
    default:
        // Not a RISC-V subsystem
        CHECK_FAIL();
    }
}

OpcodeSubsystems::DisassemblerType RiscvInstructionSet::toSubsystem(
                                                        DisassemblerTypes type)
{
    switch (type)
    {
    case RISCV_RV32:
        return OpcodeSubsystems::DISASSEMBLER_RISCV_32;
    case RISCV_RV64:
        return OpcodeSubsystems::DISASSEMBLER_RISCV_64;
    }
    // Should never reach here
    CHECK_FAIL();
}

uint RiscvInstructionSet::getXlen(DisassemblerTypes type)
{
    return (type == RISCV_RV32) ? 32 : 64;
}

uint RiscvInstructionSet::getInstructionSize(uint16 firstParcel)
{
    if ((firstParcel & 0x3) != 0x3)
        return 2;
    if ((firstParcel & 0x1c) != 0x1c)
        return 4;
    // 48 bits and longer
    return 0;
}
