/*
 * RiscvArchitecture.cpp
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
#include "dismount/proc/riscv/RiscvArchitecture.h"

using namespace riscvdis;

namespace {

// Short names for the table below
const uint FILE_GENERAL         = ArchitectureRegister::FILE_GENERAL;
const uint FILE_PROGRAM_COUNTER = ArchitectureRegister::FILE_PROGRAM_COUNTER;
const uint FILE_VECTOR          = ArchitectureRegister::FILE_VECTOR;
const uint FILE_CONTROL         = ArchitectureRegister::FILE_CONTROL;
const uint NO_PARENT            = ArchitectureRegister::NO_PARENT;

} // namespace

// The numeric names of the register groups. The names are static strings, the
// register table points to them
#define RISCV_REPEAT32(M) \
    M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) \
    M(8) M(9) M(10) M(11) M(12) M(13) M(14) M(15) \
    M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23) \
    M(24) M(25) M(26) M(27) M(28) M(29) M(30) M(31)
#define RISCV_NAME_X(n) "x" #n,
#define RISCV_NAME_F(n) "f" #n,
#define RISCV_NAME_V(n) "v" #n,

namespace {

const char* const gNamesX[] = { RISCV_REPEAT32(RISCV_NAME_X) };
const char* const gNamesF[] = { RISCV_REPEAT32(RISCV_NAME_F) };
const char* const gNamesV[] = { RISCV_REPEAT32(RISCV_NAME_V) };

// The ABI names of x0-x31 and f0-f31, by the register number
const char* const gAbiX[] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};
const char* const gAbiF[] = {
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
    "fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
    "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
    "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"
};

// Append a register to the table
void addRegister(cArray<ArchitectureRegister>& registers,
                 const char* name, uint file, uint bits,
                 uint parent = NO_PARENT, uint bitOffset = 0,
                 bool isZero = false)
{
    ArchitectureRegister reg;
    reg.m_name = name;
    reg.m_file = file;
    reg.m_bits = bits;
    reg.m_parent = parent;
    reg.m_bitOffset = bitOffset;
    reg.m_zeroExtendsParent = false;
    reg.m_flag = 0;
    reg.m_isZero = isZero;
    registers.append(reg);
}

// Return true if 'name' is "<prefix><decimal number up to 31>" and fill the
// number
bool parseNumbered(const cString& name, char prefix, uint& number)
{
    if (name.length() < 2)
        return false;
    if (name[0] != prefix)
        return false;
    // No leading zeroes
    if ((name.length() > 2) && (name[1] == '0'))
        return false;
    uint value = 0;
    for (uint i = 1; i < name.length(); i++)
    {
        const char c = name[i];
        if ((c < '0') || (c > '9'))
            return false;
        value = value * 10 + (uint)(c - '0');
        if (value > 31)
            return false;
    }
    number = value;
    return true;
}

} // namespace

RiscvArchitecture::RiscvArchitecture(
                    RiscvInstructionSet::DisassemblerTypes type,
                    uint vectorBits) :
    m_type(type),
    m_vectorBits(vectorBits)
{
    // VLEN is a power of 2, between 128 and the widest value of the
    // interpreter
    CHECK((vectorBits >= 128) && (vectorBits <= 512) &&
          ((vectorBits & (vectorBits - 1)) == 0));
    const uint xlen = (type == RiscvInstructionSet::RISCV_RV64) ? 64 : 32;
    uint i;

    // The registers, in the order of the riscvdis::RISCV_REG_* ids
    addRegister(m_registers, gNamesX[0], FILE_GENERAL, xlen,
                NO_PARENT, 0, true);
    for (i = 1; i < 32; i++)
        addRegister(m_registers, gNamesX[i], FILE_GENERAL, xlen);
    addRegister(m_registers, "pc", FILE_PROGRAM_COUNTER, xlen);
    // FLEN = 64: a narrower value is NaN boxed by the lifter
    for (i = 0; i < 32; i++)
        addRegister(m_registers, gNamesF[i], FILE_VECTOR, 64);
    for (i = 0; i < 32; i++)
        addRegister(m_registers, gNamesV[i], FILE_VECTOR, vectorBits);

    // fcsr: fflags is bits 0-4 and frm bits 5-7
    addRegister(m_registers, "fcsr", FILE_CONTROL, 32);
    addRegister(m_registers, "fflags", FILE_CONTROL, 5, RISCV_REG_FCSR, 0);
    addRegister(m_registers, "frm", FILE_CONTROL, 3, RISCV_REG_FCSR, 5);

    // The vector state. vcsr: vxsat is bit 0 and vxrm bits 1-2
    addRegister(m_registers, "vstart", FILE_CONTROL, xlen);
    addRegister(m_registers, "vcsr", FILE_CONTROL, 32);
    addRegister(m_registers, "vxsat", FILE_CONTROL, 1, RISCV_REG_VCSR, 0);
    addRegister(m_registers, "vxrm", FILE_CONTROL, 2, RISCV_REG_VCSR, 1);
    addRegister(m_registers, "vl", FILE_CONTROL, xlen);
    addRegister(m_registers, "vtype", FILE_CONTROL, xlen);

    // The reservation of lr / sc
    addRegister(m_registers, "reservation_address", FILE_CONTROL, xlen);
    addRegister(m_registers, "reservation_size", FILE_CONTROL, xlen);

    // The control flow integrity state
    addRegister(m_registers, "ssp", FILE_CONTROL, xlen);
    addRegister(m_registers, "elp", FILE_CONTROL, 1);

    ASSERT(m_registers.getSize() == RISCV_REGISTERS_COUNT);
}

RiscvInstructionSet::DisassemblerTypes RiscvArchitecture::getInstructionSet() const
{
    return m_type;
}

uint RiscvArchitecture::getXlen() const
{
    return (m_type == RiscvInstructionSet::RISCV_RV64) ? 64 : 32;
}

uint RiscvArchitecture::getVectorBits() const
{
    return m_vectorBits;
}

OpcodeSubsystems::DisassemblerType RiscvArchitecture::getType() const
{
    return (m_type == RiscvInstructionSet::RISCV_RV64) ?
                OpcodeSubsystems::DISASSEMBLER_RISCV_64 :
                OpcodeSubsystems::DISASSEMBLER_RISCV_32;
}

uint RiscvArchitecture::getAddressBits() const
{
    return getXlen();
}

uint RiscvArchitecture::getNativeIntegerBits() const
{
    return getXlen();
}

uint RiscvArchitecture::getMaximumVectorBits() const
{
    return m_vectorBits;
}

bool RiscvArchitecture::isVectorLengthScalable() const
{
    return true;
}

bool RiscvArchitecture::isLittleEndian() const
{
    return true;
}

ArchitectureDescription::MemoryModel RiscvArchitecture::getMemoryModel() const
{
    // RVWMO
    return MEMORY_WEAK;
}

uint RiscvArchitecture::getRegistersCount() const
{
    return RISCV_REGISTERS_COUNT;
}

const ArchitectureRegister& RiscvArchitecture::getRegister(
                                                    uint registerId) const
{
    CHECK(registerId < RISCV_REGISTERS_COUNT);
    return m_registers[registerId];
}

bool RiscvArchitecture::findRegister(const cString& name,
                                     uint& registerId) const
{
    uint i = 0;
    // The numeric names of the x, f and v registers
    if (parseNumbered(name, 'x', i))
    {
        registerId = RISCV_REG_X0 + i;
        return true;
    }
    if (parseNumbered(name, 'f', i))
    {
        registerId = RISCV_REG_F0 + i;
        return true;
    }
    if (parseNumbered(name, 'v', i))
    {
        registerId = RISCV_REG_V0 + i;
        return true;
    }
    // The ABI names
    for (i = 0; i < 32; i++)
    {
        if (name == cString(gAbiX[i]))
        {
            registerId = RISCV_REG_X0 + i;
            return true;
        }
        if (name == cString(gAbiF[i]))
        {
            registerId = RISCV_REG_F0 + i;
            return true;
        }
    }
    if (name == cString("fp"))
    {
        registerId = RISCV_REG_X0 + 8;
        return true;
    }
    // The other registers
    for (i = RISCV_REG_FCSR; i < RISCV_REGISTERS_COUNT; i++)
    {
        if (name == cString(m_registers[i].m_name))
        {
            registerId = i;
            return true;
        }
    }
    if (name == cString("pc"))
    {
        registerId = RISCV_REG_PC;
        return true;
    }
    return false;
}

uint RiscvArchitecture::getStackPointer() const
{
    return RISCV_REG_SP;
}

uint RiscvArchitecture::getProgramCounter() const
{
    return RISCV_REG_PC;
}

bool RiscvArchitecture::getModeRegister(uint&) const
{
    // A single instruction set
    return false;
}

bool RiscvArchitecture::getFlagRegister(uint32, uint&) const
{
    // The comparisons write integer registers
    return false;
}

bool RiscvArchitecture::isCarryInvertedOnSubtract() const
{
    return false;
}

bool RiscvArchitecture::getFloatEnvironment(
                                    uint32,
                                    FloatEnvironment& environment) const
{
    // fcsr: the rounding field (frm, bits 5-7, 3 bits) is rne, rtz, rdn,
    // rup, rmm. 5 and 6 are reserved and 7 is the dynamic mode, which is not
    // a value of frm: the lifter checks them (an illegal instruction), and
    // the interpreter rounds them to nearest even
    environment.m_controlRegister = RISCV_REG_FCSR;
    environment.m_roundingOffset = 5;
    environment.m_roundingBits = 3;
    environment.m_roundingModes[0] = SemanticOperation::ROUND_NEAREST_EVEN;
    environment.m_roundingModes[1] = SemanticOperation::ROUND_TOWARD_ZERO;
    environment.m_roundingModes[2] = SemanticOperation::ROUND_TOWARD_NEGATIVE;
    environment.m_roundingModes[3] = SemanticOperation::ROUND_TOWARD_POSITIVE;
    environment.m_roundingModes[4] = SemanticOperation::ROUND_NEAREST_AWAY;
    environment.m_roundingModes[5] = SemanticOperation::ROUND_NEAREST_EVEN;
    environment.m_roundingModes[6] = SemanticOperation::ROUND_NEAREST_EVEN;
    environment.m_roundingModes[7] = SemanticOperation::ROUND_NEAREST_EVEN;
    environment.m_precisionOffset = FloatEnvironment::NO_FIELD;
    // No flush to zero and no denormals are zero
    environment.m_flushToZeroOffset = FloatEnvironment::NO_FIELD;
    environment.m_denormalsAreZeroOffset = FloatEnvironment::NO_FIELD;
    // No traps: the exceptions have no mask bits
    for (uint i = 0; i < SemanticFloatFlags::FLOAT_FLAGS_COUNT; i++)
        environment.m_maskOffsets[i] = FloatEnvironment::NO_FIELD;
    environment.m_isMaskSetDisabled = false;
    // fflags (bits 0-4) are nx uf of dz nv, by the SemanticFloatFlags bit:
    // invalid, denormal (not recorded), divide by zero, overflow, underflow,
    // inexact
    environment.m_statusRegister = RISCV_REG_FCSR;
    environment.m_flagOffsets[0] = 4;
    environment.m_flagOffsets[1] = FloatEnvironment::NO_FIELD;
    environment.m_flagOffsets[2] = 3;
    environment.m_flagOffsets[3] = 2;
    environment.m_flagOffsets[4] = 1;
    environment.m_flagOffsets[5] = 0;
    environment.m_summaryOffset = FloatEnvironment::NO_FIELD;
    environment.m_busyOffset = FloatEnvironment::NO_FIELD;
    environment.m_trapMode = FloatEnvironment::TRAP_NONE;
    environment.m_trapNumber = 0;
    // Every NaN result is the canonical NaN: positive, the quiet bit only.
    // The tininess is detected after rounding (the defaults of the
    // environment), there is no flush to zero and no denormal operand flag
    environment.m_nanRule = FloatEnvironment::NAN_DEFAULT;
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

// Operations of the integer registers, up to XLEN bits
bool isScalarIntegerOperation(SemanticOperation::Operation operation)
{
    switch (operation)
    {
    case SemanticOperation::OP_ADD:
    case SemanticOperation::OP_SUBTRACT:
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
    case SemanticOperation::OP_BIT_REVERSE:
    case SemanticOperation::OP_MINIMUM_SIGNED:
    case SemanticOperation::OP_MAXIMUM_SIGNED:
    case SemanticOperation::OP_MINIMUM_UNSIGNED:
    case SemanticOperation::OP_MAXIMUM_UNSIGNED:
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_ATOMIC_EXCHANGE:
    case SemanticOperation::OP_ATOMIC_COMPARE_EXCHANGE:
    case SemanticOperation::OP_ATOMIC_RMW:
        return true;
    default:
        return false;
    }
}

// The operations of the floating point values: the scalar ones and the lanes
// of the vector instructions
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
    case SemanticOperation::OP_FLOAT_COPY_SIGN:
    case SemanticOperation::OP_FLOAT_CONVERT:
    case SemanticOperation::OP_FLOAT_ROUND:
    case SemanticOperation::OP_SIGNED_TO_FLOAT:
    case SemanticOperation::OP_UNSIGNED_TO_FLOAT:
    case SemanticOperation::OP_FLOAT_TO_SIGNED:
    case SemanticOperation::OP_FLOAT_TO_UNSIGNED:
    case SemanticOperation::OP_FLOAT_CLASSIFY:
    case SemanticOperation::OP_FLOAT_RECIPROCAL_ESTIMATE:
    case SemanticOperation::OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE:
    case SemanticOperation::OP_COMPARE:
    case SemanticOperation::OP_REDUCE:
        return true;
    default:
        return false;
    }
}

} // namespace

bool RiscvArchitecture::isSupported(SemanticOperation::Operation operation,
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

    // Moving bits works for the registers (a 2 XLEN integer is a pair), and
    // for the scalable vectors
    if (isMovement(operation) &&
        (type.isScalable() || (type.getTotalBits() <= 2 * getXlen())))
        return true;

    switch (type.getKind())
    {
    case SemanticType::KIND_INTEGER:
        if (type.isScalable() || type.isVector())
            // The vector instructions work on any element
            return type.getElementBits() <= 64;
        return (type.getTotalBits() <= getXlen()) &&
               isScalarIntegerOperation(operation);
    case SemanticType::KIND_FLOAT:
        switch (type.getFloatFormat())
        {
        case SemanticType::FLOAT_HALF:
        case SemanticType::FLOAT_SINGLE:
        case SemanticType::FLOAT_DOUBLE:
            return isFloatOperation(operation);
        case SemanticType::FLOAT_BFLOAT16:
            // Zfbfmin / Zvfbfmin: the conversions only
            return operation == SemanticOperation::OP_FLOAT_CONVERT;
        default:
            return false;
        }
    case SemanticType::KIND_MASK:
        // The mask registers of the vector instructions
        return (operation == SemanticOperation::OP_AND) ||
               (operation == SemanticOperation::OP_OR) ||
               (operation == SemanticOperation::OP_XOR) ||
               (operation == SemanticOperation::OP_AND_NOT) ||
               (operation == SemanticOperation::OP_NOT) ||
               (operation == SemanticOperation::OP_MASK_COUNT) ||
               (operation == SemanticOperation::OP_MASK_FIRST);
    default:
        return false;
    }
}
