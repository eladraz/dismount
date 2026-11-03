/*
 * AMD64Architecture.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/except/assert.h"
#include "xStl/except/trace.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/AMD64Architecture.h"

using namespace ia32dis;

namespace {

// Short names for the table below
const uint FILE_GENERAL         = ArchitectureRegister::FILE_GENERAL;
const uint FILE_PROGRAM_COUNTER = ArchitectureRegister::FILE_PROGRAM_COUNTER;
const uint FILE_FLAG            = ArchitectureRegister::FILE_FLAG;
const uint FILE_SEGMENT         = ArchitectureRegister::FILE_SEGMENT;
const uint FILE_X87             = ArchitectureRegister::FILE_X87;
const uint FILE_VECTOR          = ArchitectureRegister::FILE_VECTOR;
const uint FILE_CONTROL         = ArchitectureRegister::FILE_CONTROL;
const uint FILE_DEBUG           = ArchitectureRegister::FILE_DEBUG;
const uint NO_PARENT            = ArchitectureRegister::NO_PARENT;

} // namespace

// The registers, in the order of the ia32dis::IA32_REG_* and AMD64_REG_* ids.
// The IA-32 ids come first and alias the 64 bit registers: eax..edi are bits
// 0..31 of rax..rdi and zero extend them, eip is bits 0..31 of rip, the
// segment bases, the descriptor table bases and the control and debug
// registers are 64 bits. A sub-register always points to its full register
// (there are no chains). The fields are m_name, m_file, m_bits, m_parent,
// m_bitOffset, m_zeroExtendsParent, m_flag, m_isZero
const ArchitectureRegister AMD64Architecture::gRegisters[AMD64_REGISTERS_COUNT] =
{
    { "eax", FILE_GENERAL, 32, AMD64_REG_RAX, 0, true, 0, false },
    { "ecx", FILE_GENERAL, 32, AMD64_REG_RCX, 0, true, 0, false },
    { "edx", FILE_GENERAL, 32, AMD64_REG_RDX, 0, true, 0, false },
    { "ebx", FILE_GENERAL, 32, AMD64_REG_RBX, 0, true, 0, false },
    { "esp", FILE_GENERAL, 32, AMD64_REG_RSP, 0, true, 0, false },
    { "ebp", FILE_GENERAL, 32, AMD64_REG_RBP, 0, true, 0, false },
    { "esi", FILE_GENERAL, 32, AMD64_REG_RSI, 0, true, 0, false },
    { "edi", FILE_GENERAL, 32, AMD64_REG_RDI, 0, true, 0, false },
    { "ax", FILE_GENERAL, 16, AMD64_REG_RAX, 0, false, 0, false },
    { "cx", FILE_GENERAL, 16, AMD64_REG_RCX, 0, false, 0, false },
    { "dx", FILE_GENERAL, 16, AMD64_REG_RDX, 0, false, 0, false },
    { "bx", FILE_GENERAL, 16, AMD64_REG_RBX, 0, false, 0, false },
    { "sp", FILE_GENERAL, 16, AMD64_REG_RSP, 0, false, 0, false },
    { "bp", FILE_GENERAL, 16, AMD64_REG_RBP, 0, false, 0, false },
    { "si", FILE_GENERAL, 16, AMD64_REG_RSI, 0, false, 0, false },
    { "di", FILE_GENERAL, 16, AMD64_REG_RDI, 0, false, 0, false },
    { "al", FILE_GENERAL, 8, AMD64_REG_RAX, 0, false, 0, false },
    { "cl", FILE_GENERAL, 8, AMD64_REG_RCX, 0, false, 0, false },
    { "dl", FILE_GENERAL, 8, AMD64_REG_RDX, 0, false, 0, false },
    { "bl", FILE_GENERAL, 8, AMD64_REG_RBX, 0, false, 0, false },
    { "ah", FILE_GENERAL, 8, AMD64_REG_RAX, 8, false, 0, false },
    { "ch", FILE_GENERAL, 8, AMD64_REG_RCX, 8, false, 0, false },
    { "dh", FILE_GENERAL, 8, AMD64_REG_RDX, 8, false, 0, false },
    { "bh", FILE_GENERAL, 8, AMD64_REG_RBX, 8, false, 0, false },
    { "eip", FILE_PROGRAM_COUNTER, 32, AMD64_REG_RIP, 0, true, 0, false },
    { "cf", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_CARRY, false },
    { "pf", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_PARITY, false },
    { "af", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_ADJUST, false },
    { "zf", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_ZERO, false },
    { "sf", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_SIGN, false },
    { "df", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_DIRECTION, false },
    { "of", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_OVERFLOW, false },
    { "if", FILE_CONTROL, 1, NO_PARENT, 0, false, 0, false },
    { "ac", FILE_CONTROL, 1, NO_PARENT, 0, false, 0, false },
    { "id", FILE_CONTROL, 1, NO_PARENT, 0, false, 0, false },
    { "es", FILE_SEGMENT, 16, NO_PARENT, 0, false, 0, false },
    { "cs", FILE_SEGMENT, 16, NO_PARENT, 0, false, 0, false },
    { "ss", FILE_SEGMENT, 16, NO_PARENT, 0, false, 0, false },
    { "ds", FILE_SEGMENT, 16, NO_PARENT, 0, false, 0, false },
    { "fs", FILE_SEGMENT, 16, NO_PARENT, 0, false, 0, false },
    { "gs", FILE_SEGMENT, 16, NO_PARENT, 0, false, 0, false },
    { "es_base", FILE_SEGMENT, 64, NO_PARENT, 0, false, 0, false },
    { "cs_base", FILE_SEGMENT, 64, NO_PARENT, 0, false, 0, false },
    { "ss_base", FILE_SEGMENT, 64, NO_PARENT, 0, false, 0, false },
    { "ds_base", FILE_SEGMENT, 64, NO_PARENT, 0, false, 0, false },
    { "fs_base", FILE_SEGMENT, 64, NO_PARENT, 0, false, 0, false },
    { "gs_base", FILE_SEGMENT, 64, NO_PARENT, 0, false, 0, false },
    { "fpr0", FILE_X87, 80, NO_PARENT, 0, false, 0, false },
    { "fpr1", FILE_X87, 80, NO_PARENT, 0, false, 0, false },
    { "fpr2", FILE_X87, 80, NO_PARENT, 0, false, 0, false },
    { "fpr3", FILE_X87, 80, NO_PARENT, 0, false, 0, false },
    { "fpr4", FILE_X87, 80, NO_PARENT, 0, false, 0, false },
    { "fpr5", FILE_X87, 80, NO_PARENT, 0, false, 0, false },
    { "fpr6", FILE_X87, 80, NO_PARENT, 0, false, 0, false },
    { "fpr7", FILE_X87, 80, NO_PARENT, 0, false, 0, false },
    { "fpu_control", FILE_CONTROL, 16, NO_PARENT, 0, false, 0, false },
    { "fpu_status", FILE_CONTROL, 16, NO_PARENT, 0, false, 0, false },
    { "fpu_top", FILE_CONTROL, 3, IA32_REG_FPU_STATUS, 11, false, 0, false },
    { "fpu_c0", FILE_CONTROL, 1, IA32_REG_FPU_STATUS, 8, false, 0, false },
    { "fpu_c1", FILE_CONTROL, 1, IA32_REG_FPU_STATUS, 9, false, 0, false },
    { "fpu_c2", FILE_CONTROL, 1, IA32_REG_FPU_STATUS, 10, false, 0, false },
    { "fpu_c3", FILE_CONTROL, 1, IA32_REG_FPU_STATUS, 14, false, 0, false },
    { "fpu_tag", FILE_CONTROL, 8, NO_PARENT, 0, false, 0, false },
    { "mm0", FILE_VECTOR, 64, IA32_REG_FPR0, 0, false, 0, false },
    { "mm1", FILE_VECTOR, 64, IA32_REG_FPR1, 0, false, 0, false },
    { "mm2", FILE_VECTOR, 64, IA32_REG_FPR2, 0, false, 0, false },
    { "mm3", FILE_VECTOR, 64, IA32_REG_FPR3, 0, false, 0, false },
    { "mm4", FILE_VECTOR, 64, IA32_REG_FPR4, 0, false, 0, false },
    { "mm5", FILE_VECTOR, 64, IA32_REG_FPR5, 0, false, 0, false },
    { "mm6", FILE_VECTOR, 64, IA32_REG_FPR6, 0, false, 0, false },
    { "mm7", FILE_VECTOR, 64, IA32_REG_FPR7, 0, false, 0, false },
    { "xmm0", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm1", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm2", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm3", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm4", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm5", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm6", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm7", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "mxcsr", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "gdtr_base", FILE_CONTROL, 64, NO_PARENT, 0, false, 0, false },
    { "gdtr_limit", FILE_CONTROL, 16, NO_PARENT, 0, false, 0, false },
    { "ldtr", FILE_SEGMENT, 16, NO_PARENT, 0, false, 0, false },
    { "ldtr_base", FILE_CONTROL, 64, NO_PARENT, 0, false, 0, false },
    { "ldtr_limit", FILE_CONTROL, 16, NO_PARENT, 0, false, 0, false },
    { "cr0", FILE_CONTROL, 64, NO_PARENT, 0, false, 0, false },
    { "cr2", FILE_CONTROL, 64, NO_PARENT, 0, false, 0, false },
    { "cr3", FILE_CONTROL, 64, NO_PARENT, 0, false, 0, false },
    { "cr4", FILE_CONTROL, 64, NO_PARENT, 0, false, 0, false },
    { "dr0", FILE_DEBUG, 64, NO_PARENT, 0, false, 0, false },
    { "dr1", FILE_DEBUG, 64, NO_PARENT, 0, false, 0, false },
    { "dr2", FILE_DEBUG, 64, NO_PARENT, 0, false, 0, false },
    { "dr3", FILE_DEBUG, 64, NO_PARENT, 0, false, 0, false },
    { "dr6", FILE_DEBUG, 64, NO_PARENT, 0, false, 0, false },
    { "dr7", FILE_DEBUG, 64, NO_PARENT, 0, false, 0, false },
    { "rax", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "rcx", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "rdx", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "rbx", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "rsp", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "rbp", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "rsi", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "rdi", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "r8", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "r9", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "r10", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "r11", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "r12", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "r13", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "r14", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "r15", FILE_GENERAL, 64, NO_PARENT, 0, false, 0, false },
    { "r8d", FILE_GENERAL, 32, AMD64_REG_R8, 0, true, 0, false },
    { "r9d", FILE_GENERAL, 32, AMD64_REG_R9, 0, true, 0, false },
    { "r10d", FILE_GENERAL, 32, AMD64_REG_R10, 0, true, 0, false },
    { "r11d", FILE_GENERAL, 32, AMD64_REG_R11, 0, true, 0, false },
    { "r12d", FILE_GENERAL, 32, AMD64_REG_R12, 0, true, 0, false },
    { "r13d", FILE_GENERAL, 32, AMD64_REG_R13, 0, true, 0, false },
    { "r14d", FILE_GENERAL, 32, AMD64_REG_R14, 0, true, 0, false },
    { "r15d", FILE_GENERAL, 32, AMD64_REG_R15, 0, true, 0, false },
    { "r8w", FILE_GENERAL, 16, AMD64_REG_R8, 0, false, 0, false },
    { "r9w", FILE_GENERAL, 16, AMD64_REG_R9, 0, false, 0, false },
    { "r10w", FILE_GENERAL, 16, AMD64_REG_R10, 0, false, 0, false },
    { "r11w", FILE_GENERAL, 16, AMD64_REG_R11, 0, false, 0, false },
    { "r12w", FILE_GENERAL, 16, AMD64_REG_R12, 0, false, 0, false },
    { "r13w", FILE_GENERAL, 16, AMD64_REG_R13, 0, false, 0, false },
    { "r14w", FILE_GENERAL, 16, AMD64_REG_R14, 0, false, 0, false },
    { "r15w", FILE_GENERAL, 16, AMD64_REG_R15, 0, false, 0, false },
    { "spl", FILE_GENERAL, 8, AMD64_REG_RSP, 0, false, 0, false },
    { "bpl", FILE_GENERAL, 8, AMD64_REG_RBP, 0, false, 0, false },
    { "sil", FILE_GENERAL, 8, AMD64_REG_RSI, 0, false, 0, false },
    { "dil", FILE_GENERAL, 8, AMD64_REG_RDI, 0, false, 0, false },
    { "r8b", FILE_GENERAL, 8, AMD64_REG_R8, 0, false, 0, false },
    { "r9b", FILE_GENERAL, 8, AMD64_REG_R9, 0, false, 0, false },
    { "r10b", FILE_GENERAL, 8, AMD64_REG_R10, 0, false, 0, false },
    { "r11b", FILE_GENERAL, 8, AMD64_REG_R11, 0, false, 0, false },
    { "r12b", FILE_GENERAL, 8, AMD64_REG_R12, 0, false, 0, false },
    { "r13b", FILE_GENERAL, 8, AMD64_REG_R13, 0, false, 0, false },
    { "r14b", FILE_GENERAL, 8, AMD64_REG_R14, 0, false, 0, false },
    { "r15b", FILE_GENERAL, 8, AMD64_REG_R15, 0, false, 0, false },
    { "rip", FILE_PROGRAM_COUNTER, 64, NO_PARENT, 0, false, 0, false },
    { "xmm8", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm9", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm10", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm11", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm12", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm13", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm14", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "xmm15", FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
    { "cr8", FILE_CONTROL, 64, NO_PARENT, 0, false, 0, false },
    { "kernel_gs_base", FILE_SEGMENT, 64, NO_PARENT, 0, false, 0, false },
};

AMD64Architecture::AMD64Architecture() :
    m_ia32(OpcodeSubsystems::DISASSEMBLER_INTEL_32)
{
    // The array has exactly AMD64_REGISTERS_COUNT entries, so a too long
    // initializer doesn't compile. A short one leaves the last entries empty
    ASSERT(gRegisters[AMD64_REGISTERS_COUNT - 1].m_name != NULL);
}

OpcodeSubsystems::DisassemblerType AMD64Architecture::getType() const
{
    return OpcodeSubsystems::DISASSEMBLER_AMD_64;
}

uint AMD64Architecture::getAddressBits() const
{
    return 64;
}

uint AMD64Architecture::getNativeIntegerBits() const
{
    return 64;
}

uint AMD64Architecture::getMaximumVectorBits() const
{
    return 128;
}

bool AMD64Architecture::isVectorLengthScalable() const
{
    return false;
}

bool AMD64Architecture::isLittleEndian() const
{
    return true;
}

ArchitectureDescription::MemoryModel AMD64Architecture::getMemoryModel() const
{
    return MEMORY_TOTAL_STORE_ORDER;
}

uint AMD64Architecture::getRegistersCount() const
{
    return AMD64_REGISTERS_COUNT;
}

const ArchitectureRegister& AMD64Architecture::getRegister(
                                                    uint registerId) const
{
    CHECK(registerId < AMD64_REGISTERS_COUNT);
    return gRegisters[registerId];
}

bool AMD64Architecture::findRegister(const cString& name,
                                     uint& registerId) const
{
    for (uint i = 0; i < AMD64_REGISTERS_COUNT; i++)
    {
        if (name == cString(gRegisters[i].m_name))
        {
            registerId = i;
            return true;
        }
    }
    return false;
}

uint AMD64Architecture::getStackPointer() const
{
    return AMD64_REG_RSP;
}

uint AMD64Architecture::getProgramCounter() const
{
    return AMD64_REG_RIP;
}

bool AMD64Architecture::getModeRegister(uint& registerId) const
{
    return m_ia32.getModeRegister(registerId);
}

bool AMD64Architecture::getFlagRegister(uint32 flag, uint& registerId) const
{
    return m_ia32.getFlagRegister(flag, registerId);
}

bool AMD64Architecture::isCarryInvertedOnSubtract() const
{
    return m_ia32.isCarryInvertedOnSubtract();
}

bool AMD64Architecture::getFloatEnvironment(
                                    uint32 attributes,
                                    FloatEnvironment& environment) const
{
    return m_ia32.getFloatEnvironment(attributes, environment);
}

bool AMD64Architecture::isSupported(SemanticOperation::Operation operation,
                                    const SemanticType& type) const
{
    // The general purpose registers are 64 bits: the scalar integer
    // operations of 64 bits are the ones which IA-32 has for 32 bits
    if ((type.getKind() == SemanticType::KIND_INTEGER) && !type.isVector() &&
        (type.getTotalBits() == 64))
    {
        return m_ia32.isSupported(operation, SemanticType::integer(32));
    }
    return m_ia32.isSupported(operation, type);
}
