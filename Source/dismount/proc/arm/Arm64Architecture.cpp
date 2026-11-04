/*
 * Arm64Architecture.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/array.h"
#include "xStl/except/assert.h"
#include "xStl/except/trace.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/Arm64Architecture.h"

using namespace armdis;

namespace {

// Short names for the table below
const uint FILE_GENERAL         = ArchitectureRegister::FILE_GENERAL;
const uint FILE_PROGRAM_COUNTER = ArchitectureRegister::FILE_PROGRAM_COUNTER;
const uint FILE_FLAG            = ArchitectureRegister::FILE_FLAG;
const uint FILE_VECTOR          = ArchitectureRegister::FILE_VECTOR;
const uint FILE_MASK            = ArchitectureRegister::FILE_MASK;
const uint FILE_MATRIX          = ArchitectureRegister::FILE_MATRIX;
const uint FILE_CONTROL         = ArchitectureRegister::FILE_CONTROL;
const uint NO_PARENT            = ArchitectureRegister::NO_PARENT;

} // namespace

// Expand 'M(n)' for n = 0..15, 0..31 and 0..63, for the register groups
#define ARM_REPEAT16(M) \
    M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) \
    M(8) M(9) M(10) M(11) M(12) M(13) M(14) M(15)
#define ARM_REPEAT32(M) \
    ARM_REPEAT16(M) \
    M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23) \
    M(24) M(25) M(26) M(27) M(28) M(29) M(30) M(31)
#define ARM_REPEAT64(M) \
    ARM_REPEAT32(M) \
    M(32) M(33) M(34) M(35) M(36) M(37) M(38) M(39) \
    M(40) M(41) M(42) M(43) M(44) M(45) M(46) M(47) \
    M(48) M(49) M(50) M(51) M(52) M(53) M(54) M(55) \
    M(56) M(57) M(58) M(59) M(60) M(61) M(62) M(63)

// The names of the register groups. The names are static strings, the
// register table points to them
#define ARM_NAMES(NAME, PREFIX, REPEAT) \
    static const char* const NAME[] = { REPEAT(PREFIX) };
#define ARM_NAME_X(n) "x" #n,
#define ARM_NAME_W(n) "w" #n,
#define ARM_NAME_Z(n) "z" #n,
#define ARM_NAME_V(n) "v" #n,
#define ARM_NAME_Q(n) "q" #n,
#define ARM_NAME_D(n) "d" #n,
#define ARM_NAME_S(n) "s" #n,
#define ARM_NAME_H(n) "h" #n,
#define ARM_NAME_B(n) "b" #n,
#define ARM_NAME_P(n) "p" #n,
#define ARM_NAME_ZA(n) "za_row" #n,

namespace {

ARM_NAMES(gNamesX, ARM_NAME_X, ARM_REPEAT32)
ARM_NAMES(gNamesW, ARM_NAME_W, ARM_REPEAT32)
ARM_NAMES(gNamesZ, ARM_NAME_Z, ARM_REPEAT32)
ARM_NAMES(gNamesV, ARM_NAME_V, ARM_REPEAT32)
ARM_NAMES(gNamesQ, ARM_NAME_Q, ARM_REPEAT32)
ARM_NAMES(gNamesD, ARM_NAME_D, ARM_REPEAT32)
ARM_NAMES(gNamesS, ARM_NAME_S, ARM_REPEAT32)
ARM_NAMES(gNamesH, ARM_NAME_H, ARM_REPEAT32)
ARM_NAMES(gNamesB, ARM_NAME_B, ARM_REPEAT32)
ARM_NAMES(gNamesP, ARM_NAME_P, ARM_REPEAT16)
ARM_NAMES(gNamesZa, ARM_NAME_ZA, ARM_REPEAT64)

// The other spellings of the registers
struct Arm64Alias {
    const char* m_name;
    uint m_registerId;
};

const Arm64Alias gAliases[] =
{
    { "fp", ARM64_REG_X0 + 29 },
    { "lr", ARM64_REG_X0 + 30 },
    { "ip0", ARM64_REG_X0 + 16 },
    { "ip1", ARM64_REG_X0 + 17 },
    { "nzcv_n", ARM64_REG_N },
    { "nzcv_z", ARM64_REG_Z },
    { "nzcv_c", ARM64_REG_C },
    { "nzcv_v", ARM64_REG_V },
    { "pn0", ARM64_REG_P0 + 0 },
    { "pn1", ARM64_REG_P0 + 1 },
    { "pn2", ARM64_REG_P0 + 2 },
    { "pn3", ARM64_REG_P0 + 3 },
    { "pn4", ARM64_REG_P0 + 4 },
    { "pn5", ARM64_REG_P0 + 5 },
    { "pn6", ARM64_REG_P0 + 6 },
    { "pn7", ARM64_REG_P0 + 7 },
    { "pn8", ARM64_REG_P0 + 8 },
    { "pn9", ARM64_REG_P0 + 9 },
    { "pn10", ARM64_REG_P0 + 10 },
    { "pn11", ARM64_REG_P0 + 11 },
    { "pn12", ARM64_REG_P0 + 12 },
    { "pn13", ARM64_REG_P0 + 13 },
    { "pn14", ARM64_REG_P0 + 14 },
    { "pn15", ARM64_REG_P0 + 15 }
};

// Return the larger of two sizes
uint maximum(uint a, uint b)
{
    return (a > b) ? a : b;
}

// Append a register to the table
void addRegister(cArray<ArchitectureRegister>& registers,
                 const char* name, uint file, uint bits,
                 uint parent = NO_PARENT, uint bitOffset = 0,
                 bool zeroExtendsParent = false, uint32 flag = 0,
                 bool isZero = false)
{
    ArchitectureRegister reg;
    reg.m_name = name;
    reg.m_file = file;
    reg.m_bits = bits;
    reg.m_parent = parent;
    reg.m_bitOffset = bitOffset;
    reg.m_zeroExtendsParent = zeroExtendsParent;
    reg.m_flag = flag;
    reg.m_isZero = isZero;
    registers.append(reg);
}

} // namespace

namespace armdis {

uint getArm64GeneralRegister(uint number, bool is64, bool isSpRegister)
{
    CHECK(number < 32);
    if (number < 31)
        return (is64 ? ARM64_REG_X0 : ARM64_REG_W0) + number;
    if (isSpRegister)
        return is64 ? ARM64_REG_SP : ARM64_REG_WSP;
    return is64 ? ARM64_REG_XZR : ARM64_REG_WZR;
}

uint getArm64SimdRegister(uint number, uint bits)
{
    CHECK(number < 32);
    switch (bits)
    {
    case 0:   return ARM64_REG_V0 + number;
    case 8:   return ARM64_REG_B0 + number;
    case 16:  return ARM64_REG_H0 + number;
    case 32:  return ARM64_REG_S0 + number;
    case 64:  return ARM64_REG_D0 + number;
    case 128: return ARM64_REG_Q0 + number;
    default:
        CHECK_FAIL();
    }
}

} // namespace armdis

Arm64Architecture::Arm64Architecture(uint vectorBits,
                                     uint streamingVectorBits) :
    m_vectorBits(vectorBits),
    m_streamingVectorBits(streamingVectorBits)
{
    // The vector lengths are powers of 2, between 128 and the widest value
    // of the interpreter
    CHECK((vectorBits >= 128) && (vectorBits <= 512) &&
          ((vectorBits & (vectorBits - 1)) == 0));
    CHECK((streamingVectorBits >= 128) && (streamingVectorBits <= 512) &&
          ((streamingVectorBits & (streamingVectorBits - 1)) == 0));

    // A z register holds the vector of the current mode: VL, or SVL in the
    // streaming mode. The description keeps the one register file for both,
    // so it is as wide as the wider of the two (p and ffr, the eighth)
    const uint zBits = maximum(vectorBits, streamingVectorBits);
    const uint pBits = zBits / 8;
    uint i;

    // The registers, in the order of the armdis::ARM64_REG_* ids. Writing a
    // w register zeroes the rest of the x register
    for (i = 0; i < 31; i++)
        addRegister(m_registers, gNamesX[i], FILE_GENERAL, 64);
    addRegister(m_registers, "sp", FILE_GENERAL, 64);
    addRegister(m_registers, "xzr", FILE_GENERAL, 64, NO_PARENT, 0, false,
                0, true);
    for (i = 0; i < 31; i++)
        addRegister(m_registers, gNamesW[i], FILE_GENERAL, 32,
                    ARM64_REG_X0 + i, 0, true);
    addRegister(m_registers, "wsp", FILE_GENERAL, 32, ARM64_REG_SP, 0, true);
    addRegister(m_registers, "wzr", FILE_GENERAL, 32, ARM64_REG_XZR, 0, true,
                0, true);

    addRegister(m_registers, "pc", FILE_PROGRAM_COUNTER, 64);

    addRegister(m_registers, "n", FILE_FLAG, 1, NO_PARENT, 0, false,
                SemanticFlags::FLAG_SIGN);
    addRegister(m_registers, "z", FILE_FLAG, 1, NO_PARENT, 0, false,
                SemanticFlags::FLAG_ZERO);
    addRegister(m_registers, "c", FILE_FLAG, 1, NO_PARENT, 0, false,
                SemanticFlags::FLAG_CARRY);
    addRegister(m_registers, "v", FILE_FLAG, 1, NO_PARENT, 0, false,
                SemanticFlags::FLAG_OVERFLOW);

    // z is the full register. v, q, d, s, h and b are its low bits and any
    // write of them zeroes the rest of z
    for (i = 0; i < 32; i++)
        addRegister(m_registers, gNamesZ[i], FILE_VECTOR, zBits);
    for (i = 0; i < 32; i++)
        addRegister(m_registers, gNamesV[i], FILE_VECTOR, 128,
                    ARM64_REG_Z0 + i, 0, true);
    for (i = 0; i < 32; i++)
        addRegister(m_registers, gNamesQ[i], FILE_VECTOR, 128,
                    ARM64_REG_Z0 + i, 0, true);
    for (i = 0; i < 32; i++)
        addRegister(m_registers, gNamesD[i], FILE_VECTOR, 64,
                    ARM64_REG_Z0 + i, 0, true);
    for (i = 0; i < 32; i++)
        addRegister(m_registers, gNamesS[i], FILE_VECTOR, 32,
                    ARM64_REG_Z0 + i, 0, true);
    for (i = 0; i < 32; i++)
        addRegister(m_registers, gNamesH[i], FILE_VECTOR, 16,
                    ARM64_REG_Z0 + i, 0, true);
    for (i = 0; i < 32; i++)
        addRegister(m_registers, gNamesB[i], FILE_VECTOR, 8,
                    ARM64_REG_Z0 + i, 0, true);

    // The predicates and the first fault register
    for (i = 0; i < 16; i++)
        addRegister(m_registers, gNamesP[i], FILE_MASK, pBits);
    addRegister(m_registers, "ffr", FILE_MASK, pBits);

    // The rows of za
    for (i = 0; i < ARM64_MAXIMUM_ZA_ROWS; i++)
        addRegister(m_registers, gNamesZa[i], FILE_MATRIX,
                    streamingVectorBits);
    addRegister(m_registers, "zt0", FILE_MATRIX, 512);

    // The system registers
    addRegister(m_registers, "fpcr", FILE_CONTROL, 32);
    addRegister(m_registers, "fpsr", FILE_CONTROL, 32);
    addRegister(m_registers, "tpidr_el0", FILE_CONTROL, 64);
    addRegister(m_registers, "tpidrro_el0", FILE_CONTROL, 64);
    addRegister(m_registers, "svcr", FILE_CONTROL, 64);
    addRegister(m_registers, "exclusive_address", FILE_CONTROL, 64);
    addRegister(m_registers, "exclusive_size", FILE_CONTROL, 64);
    addRegister(m_registers, "btype", FILE_CONTROL, 2);

    ASSERT(m_registers.getSize() == ARM64_REGISTERS_COUNT);
}

uint Arm64Architecture::getVectorBits() const
{
    return m_vectorBits;
}

uint Arm64Architecture::getStreamingVectorBits() const
{
    return m_streamingVectorBits;
}

OpcodeSubsystems::DisassemblerType Arm64Architecture::getType() const
{
    return OpcodeSubsystems::DISASSEMBLER_ARM_64_LE;
}

uint Arm64Architecture::getAddressBits() const
{
    return 64;
}

uint Arm64Architecture::getNativeIntegerBits() const
{
    return 64;
}

uint Arm64Architecture::getMaximumVectorBits() const
{
    return m_vectorBits;
}

bool Arm64Architecture::isVectorLengthScalable() const
{
    return true;
}

bool Arm64Architecture::isLittleEndian() const
{
    return true;
}

ArchitectureDescription::MemoryModel Arm64Architecture::getMemoryModel() const
{
    return MEMORY_WEAK;
}

uint Arm64Architecture::getRegistersCount() const
{
    return ARM64_REGISTERS_COUNT;
}

const ArchitectureRegister& Arm64Architecture::getRegister(
                                                    uint registerId) const
{
    CHECK(registerId < ARM64_REGISTERS_COUNT);
    return m_registers[registerId];
}

bool Arm64Architecture::findRegister(const cString& name,
                                     uint& registerId) const
{
    for (uint i = 0; i < ARM64_REGISTERS_COUNT; i++)
    {
        if (name == cString(m_registers[i].m_name))
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

uint Arm64Architecture::getStackPointer() const
{
    return ARM64_REG_SP;
}

uint Arm64Architecture::getProgramCounter() const
{
    return ARM64_REG_PC;
}

bool Arm64Architecture::getModeRegister(uint&) const
{
    // A single instruction set
    return false;
}

bool Arm64Architecture::getFlagRegister(uint32 flag, uint& registerId) const
{
    switch (flag)
    {
    case SemanticFlags::FLAG_CARRY:     registerId = ARM64_REG_C; return true;
    case SemanticFlags::FLAG_ZERO:      registerId = ARM64_REG_Z; return true;
    case SemanticFlags::FLAG_SIGN:      registerId = ARM64_REG_N; return true;
    case SemanticFlags::FLAG_OVERFLOW:  registerId = ARM64_REG_V; return true;
    default:
        // ARM has no parity, adjust and direction flags
        return false;
    }
}

bool Arm64Architecture::isCarryInvertedOnSubtract() const
{
    return true;
}

bool Arm64Architecture::getFloatEnvironment(
                                    uint32,
                                    FloatEnvironment& environment) const
{
    // fpcr: the rounding field (bits 22-23) is RN, RP, RM, RZ
    environment.m_controlRegister = ARM64_REG_FPCR;
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
    // fpsr: the cumulative flags IOC DZC OFC UFC IXC (bits 0-4) and IDC (7)
    environment.m_statusRegister = ARM64_REG_FPSR;
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
    // The NaN of the default fpcr (DN = 0): a signaling NaN operand first,
    // then a quiet one. With DN = 1 the result is the default NaN (the lifter
    // chooses NAN_DEFAULT). The default NaN is positive
    environment.m_nanRule = FloatEnvironment::NAN_SIGNALING_FIRST;
    environment.m_isDefaultNanNegative = false;
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

// Operations of the general purpose registers, up to 64 bits
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
    case SemanticOperation::OP_ABSOLUTE:
    case SemanticOperation::OP_ABSOLUTE_DIFFERENCE_SIGNED:
    case SemanticOperation::OP_ABSOLUTE_DIFFERENCE_UNSIGNED:
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
    case SemanticOperation::OP_COUNT_LEADING_SIGN_BITS:
    case SemanticOperation::OP_POPULATION_COUNT:
    case SemanticOperation::OP_BYTE_SWAP:
    case SemanticOperation::OP_BIT_REVERSE:
    case SemanticOperation::OP_MINIMUM_SIGNED:
    case SemanticOperation::OP_MAXIMUM_SIGNED:
    case SemanticOperation::OP_MINIMUM_UNSIGNED:
    case SemanticOperation::OP_MAXIMUM_UNSIGNED:
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_TEST_FLAGS:
    case SemanticOperation::OP_SET_FLAGS:
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
    case SemanticOperation::OP_ADD_SATURATE_SIGNED:
    case SemanticOperation::OP_ADD_SATURATE_UNSIGNED:
    case SemanticOperation::OP_SUBTRACT_SATURATE_SIGNED:
    case SemanticOperation::OP_SUBTRACT_SATURATE_UNSIGNED:
    case SemanticOperation::OP_REDUCE:
        return true;
    case SemanticOperation::OP_MULTIPLY:
    case SemanticOperation::OP_MINIMUM_SIGNED:
    case SemanticOperation::OP_MAXIMUM_SIGNED:
    case SemanticOperation::OP_MINIMUM_UNSIGNED:
    case SemanticOperation::OP_MAXIMUM_UNSIGNED:
    case SemanticOperation::OP_AVERAGE_UNSIGNED:
    case SemanticOperation::OP_ABSOLUTE_DIFFERENCE_SIGNED:
    case SemanticOperation::OP_ABSOLUTE_DIFFERENCE_UNSIGNED:
    case SemanticOperation::OP_COUNT_LEADING_ZEROS:
    case SemanticOperation::OP_COUNT_LEADING_SIGN_BITS:
    case SemanticOperation::OP_MULTIPLY_HIGH_ROUNDING:
        // No 64 bit lanes (mul, smax, uabd...)
        return elementBits <= 32;
    case SemanticOperation::OP_ABSOLUTE:
    case SemanticOperation::OP_PAIRWISE_ADD:
        return true;
    case SemanticOperation::OP_POPULATION_COUNT:
        // cnt
        return elementBits == 8;
    case SemanticOperation::OP_BIT_REVERSE:
        // rbit, a byte at a time
        return elementBits == 8;
    case SemanticOperation::OP_BYTE_SWAP:
        // rev16 / rev32 / rev64
        return elementBits >= 16;
    case SemanticOperation::OP_NARROW:
        return elementBits <= 32;
    case SemanticOperation::OP_CARRYLESS_MULTIPLY:
        // pmull (8 and 64 bits)
        return (elementBits == 8) || (elementBits == 64);
    default:
        // No vector division, remainder, multiply high or rotate
        return false;
    }
}

// The operations of the scalable vectors (SVE and the streaming mode),
// beyond the Advanced SIMD ones
bool isScalableIntegerOperation(SemanticOperation::Operation operation,
                                uint elementBits)
{
    switch (operation)
    {
    case SemanticOperation::OP_DIVIDE_SIGNED:
    case SemanticOperation::OP_DIVIDE_UNSIGNED:
        // sdiv / udiv: 32 and 64 bits
        return (elementBits == 32) || (elementBits == 64);
    case SemanticOperation::OP_MULTIPLY:
    case SemanticOperation::OP_MULTIPLY_HIGH_SIGNED:
    case SemanticOperation::OP_MULTIPLY_HIGH_UNSIGNED:
    case SemanticOperation::OP_MINIMUM_SIGNED:
    case SemanticOperation::OP_MAXIMUM_SIGNED:
    case SemanticOperation::OP_MINIMUM_UNSIGNED:
    case SemanticOperation::OP_MAXIMUM_UNSIGNED:
    case SemanticOperation::OP_ABSOLUTE:
    case SemanticOperation::OP_COUNT_LEADING_ZEROS:
    case SemanticOperation::OP_COUNT_LEADING_SIGN_BITS:
    case SemanticOperation::OP_POPULATION_COUNT:
    case SemanticOperation::OP_BIT_REVERSE:
    case SemanticOperation::OP_BYTE_SWAP:
    case SemanticOperation::OP_ROTATE_RIGHT:
    case SemanticOperation::OP_COMPRESS:
    case SemanticOperation::OP_EXPAND:
    case SemanticOperation::OP_SLIDE:
    case SemanticOperation::OP_LANE_INDEX:
    case SemanticOperation::OP_ACTIVE_LANE_MASK:
    case SemanticOperation::OP_MASK_COUNT:
    case SemanticOperation::OP_MASK_FIRST:
    case SemanticOperation::OP_LOAD_MASKED:
    case SemanticOperation::OP_STORE_MASKED:
    case SemanticOperation::OP_GATHER:
    case SemanticOperation::OP_SCATTER:
    case SemanticOperation::OP_LOAD_INTERLEAVED:
    case SemanticOperation::OP_STORE_INTERLEAVED:
        return true;
    default:
        return isVectorIntegerOperation(operation, elementBits);
    }
}

// The scalar and Advanced SIMD operations of floating point values
bool isFloatOperation(SemanticOperation::Operation operation)
{
    switch (operation)
    {
    case SemanticOperation::OP_FLOAT_ADD:
    case SemanticOperation::OP_FLOAT_SUBTRACT:
    case SemanticOperation::OP_FLOAT_MULTIPLY:
    case SemanticOperation::OP_FLOAT_DIVIDE:
    case SemanticOperation::OP_FLOAT_MULTIPLY_ADD:
    case SemanticOperation::OP_FLOAT_SQUARE_ROOT:
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
    case SemanticOperation::OP_FLOAT_SCALE:
    case SemanticOperation::OP_FLOAT_RECIPROCAL_ESTIMATE:
    case SemanticOperation::OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE:
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_SHUFFLE:
    case SemanticOperation::OP_INTERLEAVE:
    case SemanticOperation::OP_BROADCAST:
    case SemanticOperation::OP_SIGN_MASK:
    case SemanticOperation::OP_REDUCE:
    case SemanticOperation::OP_PAIRWISE_ADD:
        return true;
    default:
        return false;
    }
}

} // namespace

bool Arm64Architecture::isSupported(SemanticOperation::Operation operation,
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
    case SemanticOperation::OP_READ_REGISTER_INDEXED:
    case SemanticOperation::OP_WRITE_REGISTER_INDEXED:
    case SemanticOperation::OP_HELPER:
    case SemanticOperation::OP_UNDEFINED:
        return true;
    default:
        break;
    }

    // Moving bits works for every size which the registers hold (a 128 bit
    // integer is a pair of x registers or a q register), and for the
    // scalable vectors
    if (isMovement(operation) &&
        (type.isScalable() || (type.getTotalBits() <= 128)))
        return true;

    switch (type.getKind())
    {
    case SemanticType::KIND_INTEGER:
        if (type.isScalable())
            return isScalableIntegerOperation(operation,
                                              type.getElementBits());
        if (!type.isVector())
            return (type.getTotalBits() <= 64) &&
                   isScalarIntegerOperation(operation);
        return (type.getTotalBits() <= 128) &&
               isVectorIntegerOperation(operation, type.getElementBits());
    case SemanticType::KIND_FLOAT:
        switch (type.getFloatFormat())
        {
        case SemanticType::FLOAT_HALF:
        case SemanticType::FLOAT_SINGLE:
        case SemanticType::FLOAT_DOUBLE:
            return (type.isScalable() || (type.getTotalBits() <= 128)) &&
                   isFloatOperation(operation);
        case SemanticType::FLOAT_BFLOAT16:
            // bfcvt and the bf16 dot product
            return (operation == SemanticOperation::OP_FLOAT_CONVERT) ||
                   (operation == SemanticOperation::OP_DOT_PRODUCT);
        default:
            return false;
        }
    case SemanticType::KIND_MASK:
        // The SVE predicates
        return (operation == SemanticOperation::OP_AND) ||
               (operation == SemanticOperation::OP_OR) ||
               (operation == SemanticOperation::OP_XOR) ||
               (operation == SemanticOperation::OP_AND_NOT) ||
               (operation == SemanticOperation::OP_NOT) ||
               (operation == SemanticOperation::OP_MASK_COUNT) ||
               (operation == SemanticOperation::OP_MASK_FIRST) ||
               (operation == SemanticOperation::OP_ACTIVE_LANE_MASK) ||
               (operation == SemanticOperation::OP_COMPARE);
    default:
        return false;
    }
}
