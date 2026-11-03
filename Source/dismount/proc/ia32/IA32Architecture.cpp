/*
 * IA32Architecture.cpp
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
#include "dismount/proc/ia32/IA32Architecture.h"

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

// The registers, in the order of the ia32dis::IA32_REG_* ids. A sub-register
// always points to its full register (there are no chains). The fields are
// m_name, m_file, m_bits, m_parent, m_bitOffset, m_zeroExtendsParent, m_flag,
// m_isZero
const ArchitectureRegister IA32Architecture::gRegisters[IA32_REGISTERS_COUNT] =
{
    { "eax", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "ecx", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "edx", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "ebx", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "esp", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "ebp", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "esi", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "edi", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "ax", FILE_GENERAL, 16, IA32_REG_EAX, 0, false, 0, false },
    { "cx", FILE_GENERAL, 16, IA32_REG_ECX, 0, false, 0, false },
    { "dx", FILE_GENERAL, 16, IA32_REG_EDX, 0, false, 0, false },
    { "bx", FILE_GENERAL, 16, IA32_REG_EBX, 0, false, 0, false },
    { "sp", FILE_GENERAL, 16, IA32_REG_ESP, 0, false, 0, false },
    { "bp", FILE_GENERAL, 16, IA32_REG_EBP, 0, false, 0, false },
    { "si", FILE_GENERAL, 16, IA32_REG_ESI, 0, false, 0, false },
    { "di", FILE_GENERAL, 16, IA32_REG_EDI, 0, false, 0, false },
    { "al", FILE_GENERAL, 8, IA32_REG_EAX, 0, false, 0, false },
    { "cl", FILE_GENERAL, 8, IA32_REG_ECX, 0, false, 0, false },
    { "dl", FILE_GENERAL, 8, IA32_REG_EDX, 0, false, 0, false },
    { "bl", FILE_GENERAL, 8, IA32_REG_EBX, 0, false, 0, false },
    { "ah", FILE_GENERAL, 8, IA32_REG_EAX, 8, false, 0, false },
    { "ch", FILE_GENERAL, 8, IA32_REG_ECX, 8, false, 0, false },
    { "dh", FILE_GENERAL, 8, IA32_REG_EDX, 8, false, 0, false },
    { "bh", FILE_GENERAL, 8, IA32_REG_EBX, 8, false, 0, false },
    { "eip", FILE_PROGRAM_COUNTER, 32, NO_PARENT, 0, false, 0, false },
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
    { "es_base", FILE_SEGMENT, 32, NO_PARENT, 0, false, 0, false },
    { "cs_base", FILE_SEGMENT, 32, NO_PARENT, 0, false, 0, false },
    { "ss_base", FILE_SEGMENT, 32, NO_PARENT, 0, false, 0, false },
    { "ds_base", FILE_SEGMENT, 32, NO_PARENT, 0, false, 0, false },
    { "fs_base", FILE_SEGMENT, 32, NO_PARENT, 0, false, 0, false },
    { "gs_base", FILE_SEGMENT, 32, NO_PARENT, 0, false, 0, false },
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
    { "fpu_top", FILE_CONTROL, 3, NO_PARENT, 0, false, 0, false },
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
    { "cr0", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "cr2", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "cr3", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "cr4", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "dr0", FILE_DEBUG, 32, NO_PARENT, 0, false, 0, false },
    { "dr1", FILE_DEBUG, 32, NO_PARENT, 0, false, 0, false },
    { "dr2", FILE_DEBUG, 32, NO_PARENT, 0, false, 0, false },
    { "dr3", FILE_DEBUG, 32, NO_PARENT, 0, false, 0, false },
    { "dr6", FILE_DEBUG, 32, NO_PARENT, 0, false, 0, false },
    { "dr7", FILE_DEBUG, 32, NO_PARENT, 0, false, 0, false },
};

IA32Architecture::IA32Architecture(OpcodeSubsystems::DisassemblerType type) :
    m_type(type)
{
    CHECK((type == OpcodeSubsystems::DISASSEMBLER_INTEL_16) ||
          (type == OpcodeSubsystems::DISASSEMBLER_INTEL_32));
    // The array has exactly IA32_REGISTERS_COUNT entries, so a too long
    // initializer doesn't compile. A short one leaves the last entries empty
    ASSERT(gRegisters[IA32_REGISTERS_COUNT - 1].m_name != NULL);
}

OpcodeSubsystems::DisassemblerType IA32Architecture::getType() const
{
    return m_type;
}

uint IA32Architecture::getAddressBits() const
{
    return (m_type == OpcodeSubsystems::DISASSEMBLER_INTEL_16) ? 16 : 32;
}

uint IA32Architecture::getNativeIntegerBits() const
{
    return 32;
}

uint IA32Architecture::getMaximumVectorBits() const
{
    return 128;
}

bool IA32Architecture::isVectorLengthScalable() const
{
    return false;
}

bool IA32Architecture::isLittleEndian() const
{
    return true;
}

ArchitectureDescription::MemoryModel IA32Architecture::getMemoryModel() const
{
    return MEMORY_TOTAL_STORE_ORDER;
}

uint IA32Architecture::getRegistersCount() const
{
    return IA32_REGISTERS_COUNT;
}

const ArchitectureRegister& IA32Architecture::getRegister(
                                                    uint registerId) const
{
    CHECK(registerId < IA32_REGISTERS_COUNT);
    return gRegisters[registerId];
}

bool IA32Architecture::findRegister(const cString& name,
                                    uint& registerId) const
{
    for (uint i = 0; i < IA32_REGISTERS_COUNT; i++)
    {
        if (name == cString(gRegisters[i].m_name))
        {
            registerId = i;
            return true;
        }
    }
    return false;
}

uint IA32Architecture::getStackPointer() const
{
    return IA32_REG_ESP;
}

uint IA32Architecture::getProgramCounter() const
{
    return IA32_REG_EIP;
}

bool IA32Architecture::getModeRegister(uint&) const
{
    // A single instruction set
    return false;
}

bool IA32Architecture::getFlagRegister(uint32 flag, uint& registerId) const
{
    switch (flag)
    {
    case SemanticFlags::FLAG_CARRY:     registerId = IA32_REG_CF; return true;
    case SemanticFlags::FLAG_ZERO:      registerId = IA32_REG_ZF; return true;
    case SemanticFlags::FLAG_SIGN:      registerId = IA32_REG_SF; return true;
    case SemanticFlags::FLAG_OVERFLOW:  registerId = IA32_REG_OF; return true;
    case SemanticFlags::FLAG_PARITY:    registerId = IA32_REG_PF; return true;
    case SemanticFlags::FLAG_ADJUST:    registerId = IA32_REG_AF; return true;
    case SemanticFlags::FLAG_DIRECTION: registerId = IA32_REG_DF; return true;
    default:
        return false;
    }
}

bool IA32Architecture::isCarryInvertedOnSubtract() const
{
    return false;
}

namespace {

// Operations which move or reinterpret bits, whatever the type is
bool isMovement(SemanticOperation::Operation operation)
{
    switch (operation)
    {
    case SemanticOperation::OP_MOVE:
    case SemanticOperation::OP_ZERO_EXTEND:
    case SemanticOperation::OP_SIGN_EXTEND:
    case SemanticOperation::OP_TRUNCATE:
    case SemanticOperation::OP_EXTRACT:
    case SemanticOperation::OP_INSERT:
    case SemanticOperation::OP_CONCAT:
    case SemanticOperation::OP_BITCAST:
    case SemanticOperation::OP_LOAD:
    case SemanticOperation::OP_STORE:
    case SemanticOperation::OP_SELECT:
        return true;
    default:
        return false;
    }
}

// Operations of the general purpose registers, up to 32 bits
bool isScalarIntegerOperation(SemanticOperation::Operation operation)
{
    switch (operation)
    {
    case SemanticOperation::OP_ADD:
    case SemanticOperation::OP_ADD_CARRY:
    case SemanticOperation::OP_SUBTRACT:
    case SemanticOperation::OP_SUBTRACT_BORROW:
    case SemanticOperation::OP_NEGATE:
    case SemanticOperation::OP_MULTIPLY:
    case SemanticOperation::OP_MULTIPLY_HIGH_SIGNED:
    case SemanticOperation::OP_MULTIPLY_HIGH_UNSIGNED:
    case SemanticOperation::OP_DIVIDE_SIGNED:
    case SemanticOperation::OP_DIVIDE_UNSIGNED:
    case SemanticOperation::OP_REMAINDER_SIGNED:
    case SemanticOperation::OP_REMAINDER_UNSIGNED:
    case SemanticOperation::OP_AND:
    case SemanticOperation::OP_OR:
    case SemanticOperation::OP_XOR:
    case SemanticOperation::OP_NOT:
    case SemanticOperation::OP_AND_NOT:
    case SemanticOperation::OP_SHIFT_LEFT:
    case SemanticOperation::OP_SHIFT_RIGHT_LOGICAL:
    case SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC:
    case SemanticOperation::OP_ROTATE_LEFT:
    case SemanticOperation::OP_ROTATE_RIGHT:
    case SemanticOperation::OP_COUNT_LEADING_ZEROS:
    case SemanticOperation::OP_COUNT_TRAILING_ZEROS:
    case SemanticOperation::OP_POPULATION_COUNT:
    case SemanticOperation::OP_BYTE_SWAP:
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_TEST_FLAGS:
    case SemanticOperation::OP_ATOMIC_EXCHANGE:
    case SemanticOperation::OP_ATOMIC_COMPARE_EXCHANGE:
    case SemanticOperation::OP_ATOMIC_RMW:
    case SemanticOperation::OP_CRC32:
        return true;
    default:
        return false;
    }
}

// The SSE2 operations of integer vectors
bool isVectorIntegerOperation(SemanticOperation::Operation operation,
                              uint elementBits)
{
    switch (operation)
    {
    case SemanticOperation::OP_ADD:
    case SemanticOperation::OP_SUBTRACT:
    case SemanticOperation::OP_AND:
    case SemanticOperation::OP_OR:
    case SemanticOperation::OP_XOR:
    case SemanticOperation::OP_AND_NOT:
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_SHUFFLE:
    case SemanticOperation::OP_INTERLEAVE:
    case SemanticOperation::OP_BROADCAST:
        return true;
    case SemanticOperation::OP_SHIFT_LEFT:
    case SemanticOperation::OP_SHIFT_RIGHT_LOGICAL:
        return elementBits >= 16;
    case SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC:
        return (elementBits == 16) || (elementBits == 32);
    case SemanticOperation::OP_MULTIPLY:
    case SemanticOperation::OP_MULTIPLY_HIGH_SIGNED:
    case SemanticOperation::OP_MULTIPLY_HIGH_UNSIGNED:
        return elementBits == 16;
    case SemanticOperation::OP_ADD_SATURATE_SIGNED:
    case SemanticOperation::OP_ADD_SATURATE_UNSIGNED:
    case SemanticOperation::OP_SUBTRACT_SATURATE_SIGNED:
    case SemanticOperation::OP_SUBTRACT_SATURATE_UNSIGNED:
    case SemanticOperation::OP_AVERAGE_UNSIGNED:
    case SemanticOperation::OP_MINIMUM_SIGNED:
    case SemanticOperation::OP_MAXIMUM_SIGNED:
    case SemanticOperation::OP_MINIMUM_UNSIGNED:
    case SemanticOperation::OP_MAXIMUM_UNSIGNED:
        return (elementBits == 8) || (elementBits == 16);
    case SemanticOperation::OP_NARROW:
        return (elementBits == 8) || (elementBits == 16);
    case SemanticOperation::OP_SIGN_MASK:
        return true;
    default:
        return false;
    }
}

// The SSE / SSE2 / x87 operations of floating point values
bool isFloatOperation(SemanticOperation::Operation operation)
{
    switch (operation)
    {
    case SemanticOperation::OP_FLOAT_ADD:
    case SemanticOperation::OP_FLOAT_SUBTRACT:
    case SemanticOperation::OP_FLOAT_MULTIPLY:
    case SemanticOperation::OP_FLOAT_DIVIDE:
    case SemanticOperation::OP_FLOAT_SQUARE_ROOT:
    case SemanticOperation::OP_FLOAT_MINIMUM:
    case SemanticOperation::OP_FLOAT_MAXIMUM:
    case SemanticOperation::OP_FLOAT_CONVERT:
    case SemanticOperation::OP_FLOAT_ROUND:
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_SHUFFLE:
    case SemanticOperation::OP_INTERLEAVE:
    case SemanticOperation::OP_BROADCAST:
    case SemanticOperation::OP_SIGN_MASK:
        return true;
    default:
        return false;
    }
}

} // namespace

bool IA32Architecture::isSupported(SemanticOperation::Operation operation,
                                   const SemanticType& type) const
{
    switch (operation)
    {
    // Control flow, processor state and the operations without a value
    case SemanticOperation::OP_NOP:
    case SemanticOperation::OP_LABEL:
    case SemanticOperation::OP_BRANCH:
    case SemanticOperation::OP_BRANCH_CONDITIONAL:
    case SemanticOperation::OP_BRANCH_INDIRECT:
    case SemanticOperation::OP_CALL:
    case SemanticOperation::OP_RETURN:
    case SemanticOperation::OP_TRAP:
    case SemanticOperation::OP_HINT:
    case SemanticOperation::OP_FENCE:
    case SemanticOperation::OP_PREFETCH:
    case SemanticOperation::OP_CACHE_MAINTENANCE:
    case SemanticOperation::OP_SET_FLAGS:
    case SemanticOperation::OP_READ_SYSTEM_REGISTER:
    case SemanticOperation::OP_WRITE_SYSTEM_REGISTER:
    case SemanticOperation::OP_HELPER:
    case SemanticOperation::OP_UNDEFINED:
        return true;
    default:
        break;
    }

    // Moving bits works for every size which a register holds (the 64 bit
    // integers are held by a register pair)
    if (isMovement(operation) && (type.getTotalBits() <= 128))
        return true;

    switch (type.getKind())
    {
    case SemanticType::KIND_INTEGER:
        if (!type.isVector())
            return (type.getTotalBits() <= 32) &&
                   isScalarIntegerOperation(operation);
        return (type.getTotalBits() <= 128) &&
               isVectorIntegerOperation(operation, type.getElementBits());
    case SemanticType::KIND_FLOAT:
        switch (type.getFloatFormat())
        {
        case SemanticType::FLOAT_SINGLE:
        case SemanticType::FLOAT_DOUBLE:
            return (type.getTotalBits() <= 128) && isFloatOperation(operation);
        case SemanticType::FLOAT_EXTENDED:
            // x87 has no vectors, min / max or compare to a mask
            return !type.isVector() && isFloatOperation(operation) &&
                   (operation != SemanticOperation::OP_FLOAT_MINIMUM) &&
                   (operation != SemanticOperation::OP_FLOAT_MAXIMUM);
        default:
            return false;
        }
    default:
        return false;
    }
}
