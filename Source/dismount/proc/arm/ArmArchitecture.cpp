/*
 * ArmArchitecture.cpp
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
#include "dismount/proc/arm/ArmArchitecture.h"

using namespace armdis;

namespace {

// Short names for the table below
const uint FILE_GENERAL         = ArchitectureRegister::FILE_GENERAL;
const uint FILE_PROGRAM_COUNTER = ArchitectureRegister::FILE_PROGRAM_COUNTER;
const uint FILE_FLAG            = ArchitectureRegister::FILE_FLAG;
const uint FILE_VECTOR          = ArchitectureRegister::FILE_VECTOR;
const uint FILE_CONTROL         = ArchitectureRegister::FILE_CONTROL;
const uint NO_PARENT            = ArchitectureRegister::NO_PARENT;

} // namespace

// Expand 'M(n)' for n = 0..15 and 0..31, for the register groups
#define ARM_REPEAT16(M) \
    M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) \
    M(8) M(9) M(10) M(11) M(12) M(13) M(14) M(15)
#define ARM_REPEAT32(M) \
    ARM_REPEAT16(M) \
    M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23) \
    M(24) M(25) M(26) M(27) M(28) M(29) M(30) M(31)

// The rows of the table. The d registers are halves of q(n / 2) and the s
// registers quarters of q(n / 4): a sub-register always points to its full
// register (there are no chains) and the writes don't zero the rest, as
// opposed to AArch64
#define ARM_Q(n) { "q" #n, FILE_VECTOR, 128, NO_PARENT, 0, false, 0, false },
#define ARM_D(n) { "d" #n, FILE_VECTOR, 64, ARM_REG_Q0 + (n) / 2, \
                   64 * ((n) % 2), false, 0, false },
#define ARM_S(n) { "s" #n, FILE_VECTOR, 32, ARM_REG_Q0 + (n) / 4, \
                   32 * ((n) % 4), false, 0, false },

// The registers, in the order of the armdis::ARM_REG_* ids. The fields are
// m_name, m_file, m_bits, m_parent, m_bitOffset, m_zeroExtendsParent, m_flag,
// m_isZero. r13 - r15 are named sp, lr and pc like LLVM does, findRegister()
// accepts the other spellings
const ArchitectureRegister ArmArchitecture::gRegisters[ARM_REGISTERS_COUNT] =
{
    { "r0", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r1", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r2", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r3", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r4", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r5", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r6", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r7", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r8", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r9", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r10", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r11", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "r12", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "sp", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "lr", FILE_GENERAL, 32, NO_PARENT, 0, false, 0, false },
    { "pc", FILE_PROGRAM_COUNTER, 32, NO_PARENT, 0, false, 0, false },

    // The APSR flags. Q and GE have no SemanticFlags flag
    { "n", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_SIGN, false },
    { "z", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_ZERO, false },
    { "c", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_CARRY, false },
    { "v", FILE_FLAG, 1, NO_PARENT, 0, false, SemanticFlags::FLAG_OVERFLOW, false },
    { "q", FILE_FLAG, 1, NO_PARENT, 0, false, 0, false },
    { "ge0", FILE_FLAG, 1, NO_PARENT, 0, false, 0, false },
    { "ge1", FILE_FLAG, 1, NO_PARENT, 0, false, 0, false },
    { "ge2", FILE_FLAG, 1, NO_PARENT, 0, false, 0, false },
    { "ge3", FILE_FLAG, 1, NO_PARENT, 0, false, 0, false },
    // The Thumb bit and the data endianness
    { "t", FILE_CONTROL, 1, NO_PARENT, 0, false, 0, false },
    { "e", FILE_CONTROL, 1, NO_PARENT, 0, false, 0, false },

    ARM_REPEAT16(ARM_Q)
    ARM_REPEAT32(ARM_D)
    ARM_REPEAT32(ARM_S)

    { "fpscr", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "fpexc", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "fpsid", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "mvfr0", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "mvfr1", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "mvfr2", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "tpidruro", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "tpidrurw", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "exclusive_address", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false },
    { "exclusive_size", FILE_CONTROL, 32, NO_PARENT, 0, false, 0, false }
};

namespace {

// The other spellings of the registers (the ARM ARM names of r9 - r15)
struct ArmAlias {
    const char* m_name;
    uint m_registerId;
};

const ArmAlias gAliases[] =
{
    { "r13", ARM_REG_SP },
    { "r14", ARM_REG_LR },
    { "r15", ARM_REG_PC },
    { "ip", ARM_REG_R12 },
    { "fp", ARM_REG_R11 },
    { "sl", ARM_REG_R10 },
    { "sb", ARM_REG_R9 },
    { "apsr_n", ARM_REG_N },
    { "apsr_z", ARM_REG_Z },
    { "apsr_c", ARM_REG_C },
    { "apsr_v", ARM_REG_V }
};

} // namespace

ArmArchitecture::ArmArchitecture()
{
    // The array has exactly ARM_REGISTERS_COUNT entries, so a too long
    // initializer doesn't compile. A short one leaves the last entries empty
    ASSERT(gRegisters[ARM_REGISTERS_COUNT - 1].m_name != NULL);
}

OpcodeSubsystems::DisassemblerType ArmArchitecture::getType() const
{
    return OpcodeSubsystems::DISASSEMBLER_ARM_32_LE;
}

uint ArmArchitecture::getAddressBits() const
{
    return 32;
}

uint ArmArchitecture::getNativeIntegerBits() const
{
    return 32;
}

uint ArmArchitecture::getMaximumVectorBits() const
{
    return 128;
}

bool ArmArchitecture::isVectorLengthScalable() const
{
    return false;
}

bool ArmArchitecture::isLittleEndian() const
{
    return true;
}

ArchitectureDescription::MemoryModel ArmArchitecture::getMemoryModel() const
{
    return MEMORY_WEAK;
}

uint ArmArchitecture::getRegistersCount() const
{
    return ARM_REGISTERS_COUNT;
}

const ArchitectureRegister& ArmArchitecture::getRegister(
                                                    uint registerId) const
{
    CHECK(registerId < ARM_REGISTERS_COUNT);
    return gRegisters[registerId];
}

bool ArmArchitecture::findRegister(const cString& name,
                                   uint& registerId) const
{
    for (uint i = 0; i < ARM_REGISTERS_COUNT; i++)
    {
        if (name == cString(gRegisters[i].m_name))
        {
            registerId = i;
            return true;
        }
    }
    for (uint i = 0; i < sizeof(gAliases) / sizeof(gAliases[0]); i++)
    {
        if (name == cString(gAliases[i].m_name))
        {
            registerId = gAliases[i].m_registerId;
            return true;
        }
    }
    return false;
}

uint ArmArchitecture::getStackPointer() const
{
    return ARM_REG_SP;
}

uint ArmArchitecture::getProgramCounter() const
{
    return ARM_REG_PC;
}

bool ArmArchitecture::getModeRegister(uint& registerId) const
{
    // The T bit selects between A32 and T32
    registerId = ARM_REG_T;
    return true;
}

bool ArmArchitecture::getFlagRegister(uint32 flag, uint& registerId) const
{
    switch (flag)
    {
    case SemanticFlags::FLAG_CARRY:     registerId = ARM_REG_C; return true;
    case SemanticFlags::FLAG_ZERO:      registerId = ARM_REG_Z; return true;
    case SemanticFlags::FLAG_SIGN:      registerId = ARM_REG_N; return true;
    case SemanticFlags::FLAG_OVERFLOW:  registerId = ARM_REG_V; return true;
    default:
        // ARM has no parity, adjust and direction flags
        return false;
    }
}

bool ArmArchitecture::isCarryInvertedOnSubtract() const
{
    return true;
}

bool ArmArchitecture::getFloatEnvironment(uint32,
                                          FloatEnvironment& environment) const
{
    // A single environment for VFP and Advanced SIMD: fpscr. The rounding
    // field (bits 22-23) is RN, RP, RM, RZ
    environment.m_controlRegister = ARM_REG_FPSCR;
    environment.m_roundingOffset = 22;
    environment.m_roundingModes[0] = SemanticOperation::ROUND_NEAREST_EVEN;
    environment.m_roundingModes[1] = SemanticOperation::ROUND_TOWARD_POSITIVE;
    environment.m_roundingModes[2] = SemanticOperation::ROUND_TOWARD_NEGATIVE;
    environment.m_roundingModes[3] = SemanticOperation::ROUND_TOWARD_ZERO;
    environment.m_precisionOffset = FloatEnvironment::NO_FIELD;
    // FZ (bit 24) flushes both the denormal inputs and the tiny results
    environment.m_flushToZeroOffset = 24;
    environment.m_denormalsAreZeroOffset = 24;
    // The trap enables IOE DZE OFE UFE IXE (bits 8-12) and IDE (15), by the
    // SemanticFloatFlags bit: invalid, denormal, divide by zero, overflow,
    // underflow, inexact. A set bit enables the trap
    environment.m_maskOffsets[0] = 8;
    environment.m_maskOffsets[1] = 15;
    environment.m_maskOffsets[2] = 9;
    environment.m_maskOffsets[3] = 10;
    environment.m_maskOffsets[4] = 11;
    environment.m_maskOffsets[5] = 12;
    environment.m_isMaskSetDisabled = false;
    // The cumulative flags IOC DZC OFC UFC IXC (bits 0-4) and IDC (7)
    environment.m_statusRegister = ARM_REG_FPSCR;
    environment.m_flagOffsets[0] = 0;
    environment.m_flagOffsets[1] = 7;
    environment.m_flagOffsets[2] = 1;
    environment.m_flagOffsets[3] = 2;
    environment.m_flagOffsets[4] = 3;
    environment.m_flagOffsets[5] = 4;
    environment.m_summaryOffset = FloatEnvironment::NO_FIELD;
    environment.m_busyOffset = FloatEnvironment::NO_FIELD;
    // No processor implements the traps
    environment.m_trapMode = FloatEnvironment::TRAP_NONE;
    environment.m_trapNumber = 0;
    // The NaN of the default fpscr (DN = 0): a signaling NaN operand first,
    // then a quiet one. With DN = 1 the result is the default NaN (the lifter
    // chooses NAN_DEFAULT for the Advanced SIMD "standard FPSCR value"). The
    // default NaN is positive
    environment.m_nanRule = FloatEnvironment::NAN_SIGNALING_FIRST;
    environment.m_isDefaultNanNegative = false;
    // DN (bit 25) selects the default NaN, a flushed denormal operand raises
    // IDC, a flushed result UFC only, the tininess is detected before rounding
    environment.m_defaultNanOffset = 25;
    environment.m_isTininessBeforeRounding = true;
    environment.m_isInputFlushSignaled = true;
    environment.m_isFlushInexact = false;
    environment.m_isMinMaxNanSignaling = true;
    environment.m_flushToZeroHalfOffset = 19;
    environment.m_alternativeHalfOffset = 26;
    environment.m_isAddendNanFirst = true;
    return true;
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
    case SemanticOperation::OP_AND:
    case SemanticOperation::OP_OR:
    case SemanticOperation::OP_XOR:
    case SemanticOperation::OP_NOT:
    case SemanticOperation::OP_AND_NOT:
    case SemanticOperation::OP_SHIFT_LEFT:
    case SemanticOperation::OP_SHIFT_RIGHT_LOGICAL:
    case SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC:
    case SemanticOperation::OP_ROTATE_RIGHT:
    case SemanticOperation::OP_COUNT_LEADING_ZEROS:
    case SemanticOperation::OP_BYTE_SWAP:
    case SemanticOperation::OP_BIT_REVERSE:
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_TEST_FLAGS:
    case SemanticOperation::OP_SET_FLAGS:
    case SemanticOperation::OP_ADD_SATURATE_SIGNED:
    case SemanticOperation::OP_SUBTRACT_SATURATE_SIGNED:
    case SemanticOperation::OP_ABSOLUTE_DIFFERENCE_UNSIGNED:
    case SemanticOperation::OP_ATOMIC_EXCHANGE:
    case SemanticOperation::OP_ATOMIC_COMPARE_EXCHANGE:
    case SemanticOperation::OP_ATOMIC_RMW:
    case SemanticOperation::OP_LOAD_EXCLUSIVE:
    case SemanticOperation::OP_STORE_EXCLUSIVE:
    case SemanticOperation::OP_CRC32:
        return true;
    default:
        return false;
    }
}

// The Advanced SIMD operations of integer vectors (64 and 128 bits)
bool isVectorIntegerOperation(SemanticOperation::Operation operation,
                              uint elementBits)
{
    switch (operation)
    {
    case SemanticOperation::OP_ADD:
    case SemanticOperation::OP_SUBTRACT:
    case SemanticOperation::OP_NEGATE:
    case SemanticOperation::OP_AND:
    case SemanticOperation::OP_OR:
    case SemanticOperation::OP_XOR:
    case SemanticOperation::OP_NOT:
    case SemanticOperation::OP_AND_NOT:
    case SemanticOperation::OP_BITWISE_SELECT:
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_SHIFT_LEFT:
    case SemanticOperation::OP_SHIFT_RIGHT_LOGICAL:
    case SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC:
    case SemanticOperation::OP_SHUFFLE:
    case SemanticOperation::OP_INTERLEAVE:
    case SemanticOperation::OP_BROADCAST:
    case SemanticOperation::OP_SIGN_MASK:
        return true;
    case SemanticOperation::OP_MULTIPLY:
    case SemanticOperation::OP_MINIMUM_SIGNED:
    case SemanticOperation::OP_MAXIMUM_SIGNED:
    case SemanticOperation::OP_MINIMUM_UNSIGNED:
    case SemanticOperation::OP_MAXIMUM_UNSIGNED:
    case SemanticOperation::OP_AVERAGE_UNSIGNED:
    case SemanticOperation::OP_ABSOLUTE:
    case SemanticOperation::OP_ABSOLUTE_DIFFERENCE_SIGNED:
    case SemanticOperation::OP_ABSOLUTE_DIFFERENCE_UNSIGNED:
    case SemanticOperation::OP_COUNT_LEADING_ZEROS:
    case SemanticOperation::OP_COUNT_LEADING_SIGN_BITS:
    case SemanticOperation::OP_PAIRWISE_ADD:
        // No 64 bit lanes (vmul, vmin, vabs...)
        return elementBits <= 32;
    case SemanticOperation::OP_ADD_SATURATE_SIGNED:
    case SemanticOperation::OP_ADD_SATURATE_UNSIGNED:
    case SemanticOperation::OP_SUBTRACT_SATURATE_SIGNED:
    case SemanticOperation::OP_SUBTRACT_SATURATE_UNSIGNED:
        return true;
    case SemanticOperation::OP_POPULATION_COUNT:
    case SemanticOperation::OP_BIT_REVERSE:
        // vcnt, vrbit
        return elementBits == 8;
    case SemanticOperation::OP_BYTE_SWAP:
        // vrev16 / vrev32 / vrev64
        return elementBits >= 16;
    case SemanticOperation::OP_NARROW:
        return elementBits <= 32;
    default:
        // No vector division, remainder, multiply high or rotate
        return false;
    }
}

// The VFP and Advanced SIMD operations of floating point values
bool isFloatOperation(SemanticOperation::Operation operation, bool isVector)
{
    switch (operation)
    {
    // vdiv and vsqrt are VFP instructions: Advanced SIMD has no vector divide
    // and square root
    case SemanticOperation::OP_FLOAT_DIVIDE:
    case SemanticOperation::OP_FLOAT_SQUARE_ROOT:
        return !isVector;
    case SemanticOperation::OP_FLOAT_ADD:
    case SemanticOperation::OP_FLOAT_SUBTRACT:
    case SemanticOperation::OP_FLOAT_MULTIPLY:
    case SemanticOperation::OP_FLOAT_MULTIPLY_ADD:
    case SemanticOperation::OP_FLOAT_MINIMUM:
    case SemanticOperation::OP_FLOAT_MAXIMUM:
    case SemanticOperation::OP_FLOAT_ABSOLUTE:
    case SemanticOperation::OP_FLOAT_NEGATE:
    case SemanticOperation::OP_FLOAT_CONVERT:
    case SemanticOperation::OP_FLOAT_ROUND:
    case SemanticOperation::OP_SIGNED_TO_FLOAT:
    case SemanticOperation::OP_UNSIGNED_TO_FLOAT:
    case SemanticOperation::OP_FLOAT_TO_SIGNED:
    case SemanticOperation::OP_FLOAT_TO_UNSIGNED:
    case SemanticOperation::OP_FLOAT_RECIPROCAL_ESTIMATE:
    case SemanticOperation::OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE:
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

bool ArmArchitecture::isSupported(SemanticOperation::Operation operation,
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
    case SemanticOperation::OP_FENCE_INSTRUCTION:
    case SemanticOperation::OP_PREFETCH:
    case SemanticOperation::OP_CACHE_MAINTENANCE:
    case SemanticOperation::OP_READ_SYSTEM_REGISTER:
    case SemanticOperation::OP_WRITE_SYSTEM_REGISTER:
    case SemanticOperation::OP_HELPER:
    case SemanticOperation::OP_UNDEFINED:
        return true;
    default:
        break;
    }

    // Moving bits works for every size which the registers hold (a 64 bit
    // integer is a pair of registers or a d register)
    if (isMovement(operation) && (type.getTotalBits() <= 128))
        return true;

    switch (type.getKind())
    {
    case SemanticType::KIND_INTEGER:
        // There is no native 64 bit integer arithmetic (adds / adc pairs are
        // done by the legalizer) and no multiply high of 64 bits
        if (!type.isVector())
            return (type.getTotalBits() <= 32) &&
                   isScalarIntegerOperation(operation);
        return (type.getTotalBits() <= 128) &&
               isVectorIntegerOperation(operation, type.getElementBits());
    case SemanticType::KIND_FLOAT:
        switch (type.getFloatFormat())
        {
        case SemanticType::FLOAT_SINGLE:
            return (type.getTotalBits() <= 128) &&
                   isFloatOperation(operation, type.isVector());
        case SemanticType::FLOAT_DOUBLE:
            // VFP has the scalar doubles, Advanced SIMD has no vector double
            return !type.isVector() && isFloatOperation(operation, false);
        case SemanticType::FLOAT_HALF:
            // Only conversions (vcvtb / vcvtt) without FEAT_FP16 arithmetic
            return (operation == SemanticOperation::OP_FLOAT_CONVERT);
        default:
            return false;
        }
    default:
        return false;
    }
}
