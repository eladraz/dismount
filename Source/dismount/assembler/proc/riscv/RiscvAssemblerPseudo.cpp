#include "dismount/dismount.h"
/*
 * RiscvAssemblerPseudo.cpp
 *
 * Implementation file
 *
 * The pseudo instructions of more than one instruction, as llvm-mc expands
 * them: li (the sequences of LLVM's RISCVMatInt), la / lla, call / tail, the
 * loads and stores of a symbol and the vector compares vmsge.vx / vmsgeu.vx
 * (one to four instructions) and vmsge.vi / vmsgeu.vi / vmslt.vi / vmsltu.vi.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/exceptions.h"
#include "dismount/assembler/proc/riscv/RiscvAssemblerInternal.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"

using namespace riscvasm;
using namespace riscvdis;

#define ASM_CHECK(cond, msg) if (!(cond)) { XSTL_THROW(cException, XSTL_STRING(msg), EXCEPTION_FAILED); }

namespace {

typedef cArray<MaterializeStep> Sequence;

bool isInt(int64 value, uint bits)
{
    int64 limit = (int64)1 << (bits - 1);
    return (value >= -limit) && (value < limit);
}

bool isUInt(uint64 value, uint bits)
{
    return (bits >= 64) || (value < ((uint64)1 << bits));
}

int64 signExtend12(int64 value)
{
    return (int64)(((uint64)value & 0xfff) ^ 0x800) - 0x800;
}

uint countTrailingZeros(uint64 value)
{
    uint count = 0;
    while (((value >> count) & 1) == 0)
        count++;
    return count;
}

uint countLeadingZeros(uint64 value)
{
    uint count = 0;
    while (count < 64 && ((value >> (63 - count)) & 1) == 0)
        count++;
    return count;
}

uint popCount(uint64 value)
{
    uint count = 0;
    for (; value != 0; value &= value - 1)
        count++;
    return count;
}

bool isPowerOf2(uint64 value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

void emit(Sequence& sequence, const char* mnemonic, int64 immediate)
{
    MaterializeStep step;
    step.m_mnemonic = mnemonic;
    step.m_immediate = immediate;
    sequence.append(step);
}

// LLVM's generateInstSeqImpl
void generateImpl(int64 value, bool is64, const RiscvFeatureSet& features, Sequence& result)
{
    // BSETI for a single bit which a LUI or an ADDI can't give
    if (features.has(RiscvFeatures::FEAT_ZBS) && isPowerOf2((uint64)value) &&
        (!isInt(value, 32) || value == 0x800))
    {
        emit(result, "bseti", countTrailingZeros((uint64)value));
        return;
    }

    if (isInt(value, 32))
    {
        int64 hi20 = ((value + 0x800) >> 12) & 0xfffff;
        int64 lo12 = signExtend12(value);
        if (hi20 != 0)
            emit(result, "lui", hi20);
        if (lo12 != 0 || hi20 == 0)
        {
            // ADDIW only when the sum of the LUI (sign extended from 32 bits)
            // and the ADDI isn't the value
            int64 luiValue = (int64)(int32)(uint32)((uint64)hi20 << 12);
            emit(result, (is64 && hi20 != 0 && (luiValue + lo12) != value) ? "addiw" : "addi", lo12);
        }
        return;
    }

    int64 lo12 = signExtend12(value);
    value = (int64)((uint64)value - (uint64)lo12);

    int shiftAmount = 0;
    bool isUnsigned = false;
    // value might now be valid for LUI without needing a shift
    if (!isInt(value, 32))
    {
        shiftAmount = (int)countTrailingZeros((uint64)value);
        value >>= shiftAmount;

        // If the remaining bits don't fit in 12 bits, the shift can be reduced
        // to use LUI, which zeroes the lower 12 bits
        if (shiftAmount > 12 && !isInt(value, 12))
        {
            if (isInt((int64)((uint64)value << 12), 32))
            {
                shiftAmount -= 12;
                value = (int64)((uint64)value << 12);
            } else if (isUInt((uint64)value << 12, 32) && features.has(RiscvFeatures::FEAT_ZBA))
            {
                shiftAmount -= 12;
                value = (int64)(((uint64)value << 12) | (0xffffffffull << 32));
                isUnsigned = true;
            }
        }

        // SLLI.UW for a value which is uint32 but not int32
        if (isUInt((uint64)value, 32) && !isInt(value, 32) &&
            features.has(RiscvFeatures::FEAT_ZBA))
        {
            value = (int64)((uint64)value | (0xffffffffull << 32));
            isUnsigned = true;
        }
    }

    generateImpl(value, is64, features, result);

    if (shiftAmount != 0)
        emit(result, isUnsigned ? "slli.uw" : "slli", shiftAmount);
    if (lo12 != 0)
        emit(result, "addi", lo12);
}

} // anonymous namespace

namespace riscvasm {

void generateMaterialization(int64 value, uint xlen, const RiscvFeatureSet& features,
                             cArray<MaterializeStep>& steps)
{
    const bool is64 = (xlen == 64);
    Sequence result;
    generateImpl(value, is64, features, result);

    // If the low 12 bits are non-zero, the first expansion may end with an ADDI.
    // With trailing zeros try a sign extended constant without them and a
    // final SLLI
    if (((value & 0xfff) != 0) && ((value & 1) == 0) && result.getSize() >= 2)
    {
        uint trailingZeros = countTrailingZeros((uint64)value);
        int64 shiftedValue = value >> trailingZeros;
        // C.LI + C.SLLI is more compressible than LUI + ADDI(W)
        bool isShiftedCompressible = isInt(shiftedValue, 6);
        Sequence temporary;
        generateImpl(shiftedValue, is64, features, temporary);
        if (((temporary.getSize() + 1) < result.getSize()) ||
            ((result.getSize() >= 2) && isShiftedCompressible))
        {
            emit(temporary, "slli", trailingZeros);
            result = temporary;
        }
    }

    // A positive constant: a shifted constant without leading zeros and a
    // final SRLI
    if (value > 0 && result.getSize() > 2)
    {
        uint leadingZeros = countLeadingZeros((uint64)value);
        uint64 shiftedValue = (uint64)value << leadingZeros;
        // The bits which are shifted out are 1s (a trailing ones mask)
        shiftedValue |= (leadingZeros == 0) ? 0 : (0xffffffffffffffffull >> (64 - leadingZeros));

        Sequence temporary;
        generateImpl((int64)shiftedValue, is64, features, temporary);
        if ((temporary.getSize() + 1) < result.getSize())
        {
            emit(temporary, "srli", leadingZeros);
            result = temporary;
        }

        // Some cases benefit from zeros in the low bits
        if (leadingZeros != 0)
            shiftedValue &= ~(0xffffffffffffffffull >> (64 - leadingZeros));
        temporary = Sequence();
        generateImpl((int64)shiftedValue, is64, features, temporary);
        if ((temporary.getSize() + 1) < result.getSize())
        {
            emit(temporary, "srli", leadingZeros);
            result = temporary;
        }

        // 32 leading zeros and Zba: zext.w (add.uw) at the end
        if (leadingZeros == 32 && features.has(RiscvFeatures::FEAT_ZBA))
        {
            uint64 leadingOnes = (uint64)value | 0xffffffff00000000ull;
            temporary = Sequence();
            generateImpl((int64)leadingOnes, is64, features, temporary);
            if ((temporary.getSize() + 1) < result.getSize())
            {
                emit(temporary, "add.uw", 0);
                result = temporary;
            }
        }
    }

    // PACK of a value whose halves are the same (Zbkb)
    if (result.getSize() > 2 && features.has(RiscvFeatures::FEAT_ZBKB))
    {
        int64 loValue = (int64)(int32)(uint32)((uint64)value & 0xffffffff);
        int64 hiValue = (int64)(int32)(uint32)((uint64)value >> 32);
        if (loValue == hiValue)
        {
            Sequence temporary;
            generateImpl(loValue, is64, features, temporary);
            if ((temporary.getSize() + 1) < result.getSize())
            {
                emit(temporary, "pack", 0);
                result = temporary;
            }
        }
    }

    // BSETI / BCLRI of the high bits (Zbs)
    if (result.getSize() > 2 && features.has(RiscvFeatures::FEAT_ZBS))
    {
        const char* opcode = NULL;
        uint64 lo = (uint64)value & 0x7fffffff;
        uint64 hi = (uint64)value ^ lo;
        Sequence temporary;
        if (lo != 0)
            generateImpl((int64)lo, is64, features, temporary);
        if (temporary.getSize() + popCount(hi) < result.getSize())
        {
            opcode = "bseti";
        } else
        {
            // BCLRI of the bits of an all ones upper part
            hi = ~(uint64)value & 0xffffffff80000000ull;
            lo = (uint64)value | hi;
            temporary = Sequence();
            generateImpl((int64)lo, is64, features, temporary);
            if (temporary.getSize() + popCount(hi) < result.getSize())
                opcode = "bclri";
        }
        if (opcode != NULL)
        {
            while (hi != 0)
            {
                emit(temporary, opcode, countTrailingZeros(hi));
                hi &= hi - 1;
            }
            result = temporary;
        }
    }

    // BSETI of the bit which is left when the low 12 bits are taken off, then
    // the ADDI of them (Zbs)
    if (result.getSize() > 2 && features.has(RiscvFeatures::FEAT_ZBS))
    {
        int64 lo12 = signExtend12(value);
        uint64 rest = (uint64)value - (uint64)lo12;
        if (isPowerOf2(rest) && (rest > 0x7ffffffeull) && result.getSize() > 2)
        {
            Sequence temporary;
            emit(temporary, "bseti", countTrailingZeros(rest));
            if (lo12 != 0)
                emit(temporary, "addi", lo12);
            if (temporary.getSize() < result.getSize())
                result = temporary;
        }
    }

    // SH1ADD / SH2ADD / SH3ADD of a smaller constant (Zba)
    if (result.getSize() > 2 && features.has(RiscvFeatures::FEAT_ZBA))
    {
        int64 divisor = 0;
        const char* opcode = NULL;
        Sequence temporary;
        if ((value % 3) == 0 && isInt(value / 3, 32))
        {
            divisor = 3;
            opcode = "sh1add";
        } else if ((value % 5) == 0 && isInt(value / 5, 32))
        {
            divisor = 5;
            opcode = "sh2add";
        } else if ((value % 9) == 0 && isInt(value / 9, 32))
        {
            divisor = 9;
            opcode = "sh3add";
        }
        if (divisor > 0)
        {
            generateImpl(value / divisor, is64, features, temporary);
            if ((temporary.getSize() + 1) < result.getSize())
            {
                emit(temporary, opcode, 0);
                result = temporary;
            }
        } else
        {
            // LUI + SH*ADD + ADDI
            int64 hi52 = (int64)(((uint64)value + 0x800ull) & ~0xfffull);
            int64 lo12 = signExtend12(value);
            if (isInt(hi52 / 3, 32) && (hi52 % 3) == 0)
            {
                divisor = 3;
                opcode = "sh1add";
            } else if (isInt(hi52 / 5, 32) && (hi52 % 5) == 0)
            {
                divisor = 5;
                opcode = "sh2add";
            } else if (isInt(hi52 / 9, 32) && (hi52 % 9) == 0)
            {
                divisor = 9;
                opcode = "sh3add";
            }
            if (divisor > 0 && lo12 != 0)
            {
                generateImpl(hi52 / divisor, is64, features, temporary);
                if ((temporary.getSize() + 2) < result.getSize())
                {
                    emit(temporary, opcode, 0);
                    emit(temporary, "addi", lo12);
                    result = temporary;
                }
            }
        }
    }


    // RORI of a rotated value which is a 12 bit constant (Zbb)
    if (result.getSize() > 1 && features.has(RiscvFeatures::FEAT_ZBB))
    {
        for (uint rotate = 1; rotate < 64; rotate++)
        {
            // value = rotate right (rotated, rotate)
            uint64 rotated = ((uint64)value << rotate) | ((uint64)value >> (64 - rotate));
            if (isInt((int64)rotated, 12) && (int64)rotated < 0 && result.getSize() > 2)
            {
                Sequence temporary;
                emit(temporary, "addi", (int64)rotated);
                emit(temporary, "rori", rotate);
                result = temporary;
                break;
            }
        }
    }

    // Two ADDIs for the low 13 bits, if the rest has more trailing zeros and
    // saves an instruction
    if (result.getSize() > 7 && !isInt(value, 32))
    {
        int64 lo12 = signExtend12(value);
        int64 lo13 = lo12 - 4096;
        if (lo12 > 0)
        {
            Sequence temporary;
            generateMaterialization((int64)((uint64)value - (uint64)lo13), xlen, features, temporary);
            if ((temporary.getSize() + 2) < result.getSize())
            {
                int64 first = -2048;
                emit(temporary, "addi", first);
                emit(temporary, "addi", lo13 - first);
                result = temporary;
            }
        }
    }

    // The complement of a negative constant, finished by XORI -1
    if (value < 0 && result.getSize() > 3)
    {
        cArray<MaterializeStep> complement;
        generateMaterialization(~value, xlen, RiscvFeatureSet(), complement);
        if ((complement.getSize() + 1) < result.getSize())
        {
            MaterializeStep step;
            step.m_mnemonic = "xori";
            step.m_immediate = -1;
            complement.append(step);
            result = complement;
        }
    }

    steps = result;
}

} // namespace riscvasm

//////////////////////////////////////////////////////////////////////////

namespace {

Operand makeRegister(uint number)
{
    Operand operand;
    operand.m_kind = Operand::KIND_REGISTER;
    operand.m_register = RISCV_REG_X0 + number;
    return operand;
}

Operand makeImmediate(int64 value)
{
    Operand operand;
    operand.m_kind = Operand::KIND_IMMEDIATE;
    operand.m_immediate = value;
    return operand;
}

Operand makeMemory(uint base)
{
    Operand operand;
    operand.m_kind = Operand::KIND_MEMORY;
    operand.m_base = RISCV_REG_X0 + base;
    operand.m_hasOffset = true;
    operand.m_offset = 0;
    return operand;
}

void assembleOne(const Target& target, const cString& mnemonic, const cArray<Operand>& operands,
                 bool allowCompression, cArray<Unit>& units)
{
    Target current = target;
    // The units which follow are at the next addresses
    for (uint i = 0; i < units.getSize(); i++)
        current.m_address += units[i].m_size;
    Unit unit;
    ASM_CHECK(assembleOperands(current, mnemonic, operands, allowCompression, unit),
              "invalid pseudo instruction");
    units.append(unit);
}

bool isRegister(const Operand& operand)
{
    return (operand.m_kind == Operand::KIND_REGISTER) && (operand.m_register <= RISCV_REG_X31);
}

bool isLabel(const Operand& operand)
{
    return operand.m_kind == Operand::KIND_NAME;
}

bool isVectorRegister(const Operand& operand)
{
    return (operand.m_kind == Operand::KIND_REGISTER) &&
           (operand.m_register >= RISCV_REG_V0) && (operand.m_register <= RISCV_REG_V31);
}

Operand makeVectorMask()
{
    Operand operand;
    operand.m_kind = Operand::KIND_VECTOR_MASK;
    return operand;
}

// Assemble "mnemonic a, b, c[, v0.t]" of vector registers and one more
// operand into 'units'
void assembleVector(const Target& target, const char* mnemonic, const Operand& first,
                    const Operand& second, const Operand& third, bool isMasked,
                    cArray<Unit>& units)
{
    cArray<Operand> ops;
    ops.append(first);
    ops.append(second);
    ops.append(third);
    if (isMasked)
        ops.append(makeVectorMask());
    assembleOne(target, cString(mnemonic), ops, false, units);
}

// vmsge.vx / vmsgeu.vx, as llvm-mc expands them (RISCVAsmParser::emitVMSGE)
bool expandVectorGreaterEqual(const Target& target, const cString& mnemonic,
                              const cArray<Operand>& operands, cArray<Unit>& units)
{
    const bool isUnsigned = (mnemonic == "vmsgeu.vx");
    const char* compare = isUnsigned ? "vmsltu.vx" : "vmslt.vx";
    const uint count = operands.getSize();
    if (count < 3 || count > 5)
        return false;
    if (!isVectorRegister(operands[0]) || !isVectorRegister(operands[1]) || !isRegister(operands[2]))
        return false;
    Operand v0;
    v0.m_kind = Operand::KIND_REGISTER;
    v0.m_register = RISCV_REG_V0;
    const Operand& destination = operands[0];
    const Operand& source = operands[1];
    const bool isDestinationV0 = (destination.m_register == RISCV_REG_V0);
    if (count == 3)
    {
        // vmslt vd, va, x; vmnot.m vd (vmnand.mm vd, vd, vd)
        cArray<Operand> ops;
        ops.append(destination);
        ops.append(source);
        ops.append(operands[2]);
        assembleOne(target, cString(compare), ops, false, units);
        assembleVector(target, "vmnand.mm", destination, destination, destination, false, units);
        return true;
    }
    ASM_CHECK(operands[3].m_kind == Operand::KIND_VECTOR_MASK, "expected v0.t");
    if (count == 4)
    {
        // vmslt vd, va, x, v0.t; vmxor.mm vd, vd, v0
        ASM_CHECK(!isDestinationV0, "the destination cannot overlap the mask");
        cArray<Operand> ops;
        ops.append(destination);
        ops.append(source);
        ops.append(operands[2]);
        ops.append(makeVectorMask());
        assembleOne(target, cString(compare), ops, false, units);
        assembleVector(target, "vmxor.mm", destination, destination, v0, false, units);
        return true;
    }
    // With a temporary register (not v0, not the destination)
    const Operand& temporary = operands[4];
    ASM_CHECK(isVectorRegister(temporary) && (temporary.m_register != RISCV_REG_V0),
              "invalid temporary register");
    ASM_CHECK(temporary.m_register != destination.m_register,
              "the temporary cannot be the destination");
    cArray<Operand> ops;
    ops.append(temporary);
    ops.append(source);
    ops.append(operands[2]);
    assembleOne(target, cString(compare), ops, false, units);
    if (isDestinationV0)
    {
        // vmandn.mm vd, vd, vt
        assembleVector(target, "vmandn.mm", destination, destination, temporary, false, units);
    } else
    {
        // vmandn.mm vt, v0, vt; vmandn.mm vd, vd, v0; vmor.mm vd, vt, vd
        assembleVector(target, "vmandn.mm", temporary, v0, temporary, false, units);
        assembleVector(target, "vmandn.mm", destination, destination, v0, false, units);
        assembleVector(target, "vmor.mm", destination, temporary, destination, false, units);
    }
    return true;
}

// vmsge.vi / vmsgeu.vi / vmslt.vi / vmsltu.vi: the compare with the immediate
// - 1 (vmsgt.vi, vmsgtu.vi, vmsle.vi, vmsleu.vi), for the immediates of -15 to
// 16. The unsigned compare with 0 is a compare of the source with itself
// (vmseq.vv for vmsgeu.vi: always true; vmsne.vv for vmsltu.vi: always false)
bool expandVectorCompareImmediate(const Target& target, const cString& mnemonic,
                                  const cArray<Operand>& operands, cArray<Unit>& units)
{
    static const struct {
        const char* m_pseudo;
        const char* m_real;
        const char* m_zero;
    } pseudos[] = {
        { "vmsge.vi", "vmsgt.vi", NULL },
        { "vmsgeu.vi", "vmsgtu.vi", "vmseq.vv" },
        { "vmslt.vi", "vmsle.vi", NULL },
        { "vmsltu.vi", "vmsleu.vi", "vmsne.vv" }
    };
    const char* real = NULL;
    const char* zero = NULL;
    for (uint i = 0; i < sizeof(pseudos) / sizeof(pseudos[0]); i++)
    {
        if (mnemonic == pseudos[i].m_pseudo)
        {
            real = pseudos[i].m_real;
            zero = pseudos[i].m_zero;
        }
    }
    if (real == NULL)
        return false;
    const uint count = operands.getSize();
    if (count < 3 || count > 4)
        return false;
    if (!isVectorRegister(operands[0]) || !isVectorRegister(operands[1]) ||
        operands[2].m_kind != Operand::KIND_IMMEDIATE)
        return false;
    if (count == 4 && operands[3].m_kind != Operand::KIND_VECTOR_MASK)
        return false;
    int64 value = operands[2].m_immediate;
    ASM_CHECK(value >= -15 && value <= 16, "immediate out of range");
    cArray<Operand> ops;
    ops.append(operands[0]);
    ops.append(operands[1]);
    const char* name;
    if (zero != NULL && value == 0)
    {
        name = zero;
        ops.append(operands[1]);
    } else
    {
        name = real;
        ops.append(makeImmediate(value - 1));
    }
    if (count == 4)
        ops.append(operands[3]);
    assembleOne(target, cString(name), ops, false, units);
    return true;
}

} // anonymous namespace

namespace riscvasm {

bool expandPseudo(const Target& target, const cString& mnemonic,
                  const cArray<Operand>& operands, cArray<Unit>& units)
{
    const uint xlen = RiscvInstructionSet::getXlen(target.m_type);

    // A branch to '.' + offset which is out of the range of the branch: llvm-mc
    // relaxes it to the opposite branch over a jal
    {
        static const char* const pairs[][2] = {
            { "beq", "bne" }, { "blt", "bge" }, { "bltu", "bgeu" }, { "beqz", "bnez" },
            { "blez", "bgtz" }, { "bgez", "bltz" }, { "bgt", "ble" }, { "bgtu", "bleu" }
        };
        const uint count = operands.getSize();
        if (count >= 2 && operands[count - 1].m_kind == Operand::KIND_IMMEDIATE &&
            operands[count - 1].m_name == ".")
        {
            for (uint i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
            {
                for (uint k = 0; k < 2; k++)
                {
                    if (!(mnemonic == pairs[i][k]))
                        continue;
                    int64 offset = operands[count - 1].m_immediate;
                    if (offset >= -4096 && offset < 4096)
                        return false;
                    ASM_CHECK(offset >= -(1 << 20) && offset < (1 << 20) && (offset & 1) == 0,
                              "branch out of range");
                    cArray<Operand> inverted;
                    for (uint j = 0; j + 1 < count; j++)
                        inverted.append(operands[j]);
                    inverted.append(makeImmediate(8));
                    assembleOne(target, cString(pairs[i][1 - k]), inverted, false, units);
                    cArray<Operand> jump;
                    jump.append(makeRegister(0));
                    jump.append(makeImmediate(offset - 4));
                    assembleOne(target, cString("jal"), jump, false, units);
                    return true;
                }
            }
        }
    }

    if (mnemonic == "vmsge.vx" || mnemonic == "vmsgeu.vx")
        return expandVectorGreaterEqual(target, mnemonic, operands, units);
    if (mnemonic == "vmsge.vi" || mnemonic == "vmsgeu.vi" || mnemonic == "vmslt.vi" ||
        mnemonic == "vmsltu.vi")
        return expandVectorCompareImmediate(target, mnemonic, operands, units);

    if (mnemonic == "li")
    {
        if (operands.getSize() != 2 || !isRegister(operands[0]) ||
            operands[1].m_kind != Operand::KIND_IMMEDIATE)
            return false;
        int64 value = operands[1].m_immediate;
        if (xlen == 32)
        {
            // [-2^31, 2^32)
            ASM_CHECK(value >= -((int64)1 << 31) && value < ((int64)1 << 32), "immediate out of range");
            value = (int64)(int32)(uint32)value;
        }
        uint rd = operands[0].m_register - RISCV_REG_X0;
        if (isInt(value, 12))
        {
            // One instruction (the table's li), with the RV32 value wrapped
            cArray<Operand> ops;
            ops.append(operands[0]);
            ops.append(makeImmediate(value));
            assembleOne(target, cString("li"), ops, true, units);
            return true;
        }
        cArray<MaterializeStep> steps;
        generateMaterialization(value, xlen, target.m_features, steps);
        bool isFirst = true;
        for (uint i = 0; i < steps.getSize(); i++)
        {
            cArray<Operand> ops;
            ops.append(makeRegister(rd));
            const char* name = steps[i].m_mnemonic;
            if (cString(name) == "lui")
            {
                ops.append(makeImmediate(steps[i].m_immediate));
            } else if (cString(name) == "pack")
            {
                ops.append(makeRegister(rd));
                ops.append(makeRegister(rd));
            } else if (cString(name) == "add.uw")
            {
                ops.append(makeRegister(rd));
                ops.append(makeRegister(0));
            } else if (cString(name).length() >= 5 && cString(name).left(2) == "sh" &&
                       cString(name).part(3, 6) == "add")
            {
                ops.append(makeRegister(rd));
                ops.append(makeRegister(rd));
            } else
            {
                // addi / addiw / slli ...: from zero for the first, from rd
                // after
                ops.append(makeRegister(isFirst ? 0 : rd));
                ops.append(makeImmediate(steps[i].m_immediate));
            }
            assembleOne(target, cString(name), ops, true, units);
            isFirst = false;
        }
        return true;
    }

    // zext.h, zext.w, sext.b and sext.h without their extension are a pair of
    // shifts
    if ((mnemonic == "zext.h" || mnemonic == "zext.w" || mnemonic == "sext.b" ||
         mnemonic == "sext.h") && operands.getSize() == 2 && isRegister(operands[0]) &&
        isRegister(operands[1]))
    {
        bool isAvailable;
        uint width;
        if (mnemonic == "zext.w")
        {
            isAvailable = target.m_features.has(RiscvFeatures::FEAT_ZBA);
            width = 32;
        } else if (mnemonic == "zext.h")
        {
            isAvailable = target.m_features.has(RiscvFeatures::FEAT_ZBB) ||
                          target.m_features.has(RiscvFeatures::FEAT_ZBKB);
            width = 16;
        } else
        {
            isAvailable = target.m_features.has(RiscvFeatures::FEAT_ZBB);
            width = (mnemonic == "sext.b") ? 8 : 16;
        }
        if (isAvailable)
            return false;
        // (zext.w is RV64 only)
        ASM_CHECK(!(mnemonic == "zext.w" && xlen == 32), "zext.w is RV64 only");
        uint rd = operands[0].m_register - RISCV_REG_X0;
        cArray<Operand> first, second;
        first.append(operands[0]);
        first.append(operands[1]);
        first.append(makeImmediate(xlen - width));
        assembleOne(target, cString("slli"), first, true, units);
        second.append(makeRegister(rd));
        second.append(makeRegister(rd));
        second.append(makeImmediate(xlen - width));
        assembleOne(target, cString((mnemonic.left(4) == "zext") ? "srli" : "srai"), second, true, units);
        return true;
    }

    if (mnemonic == "la" || mnemonic == "lla")
    {
        if (operands.getSize() != 2 || !isRegister(operands[0]) || !isLabel(operands[1]))
            return false;
        uint rd = operands[0].m_register - RISCV_REG_X0;
        cArray<Operand> first, second;
        first.append(makeRegister(rd));
        first.append(makeImmediate(0));
        assembleOne(target, cString("auipc"), first, false, units);
        second.append(makeRegister(rd));
        second.append(makeRegister(rd));
        second.append(makeImmediate(0));
        assembleOne(target, cString("addi"), second, false, units);
        units[units.getSize() - 2].m_label = operands[1].m_name;
        units[units.getSize() - 2].m_labelSpan = 8;
        return true;
    }

    if (mnemonic == "call" || mnemonic == "tail")
    {
        bool isCall = (mnemonic == "call");
        // call sym / call rd, sym
        if (operands.getSize() < 1 || operands.getSize() > 2 || !isLabel(operands[operands.getSize() - 1]))
            return false;
        uint link = isCall ? 1 : 0;
        // (tail jumps through t2 with the landing pads of Zicfilp, else t1)
        uint temporary = isCall ? 1 : (target.m_features.has(RiscvFeatures::FEAT_ZICFILP) ? 7 : 6);
        if (operands.getSize() == 2)
        {
            if (!isRegister(operands[0]) || !isCall)
                return false;
            link = temporary = operands[0].m_register - RISCV_REG_X0;
        }
        cArray<Operand> first, second;
        first.append(makeRegister(temporary));
        first.append(makeImmediate(0));
        assembleOne(target, cString("auipc"), first, false, units);
        second.append(makeRegister(link));
        second.append(makeMemory(temporary));
        assembleOne(target, cString("jalr"), second, false, units);
        units[units.getSize() - 2].m_label = operands[operands.getSize() - 1].m_name;
        units[units.getSize() - 2].m_labelSpan = 8;
        return true;
    }

    // The loads and stores of a symbol: "lw a0, sym", "sw a0, sym, t0"
    static const char* const loads[] = { "lb", "lh", "lw", "lbu", "lhu", "lwu", "ld", "flh", "flw", "fld", NULL };
    static const char* const stores[] = { "sb", "sh", "sw", "sd", "fsh", "fsw", "fsd", NULL };
    bool isLoad = false, isStore = false;
    for (uint i = 0; loads[i] != NULL; i++)
        if (mnemonic == loads[i])
            isLoad = true;
    for (uint i = 0; stores[i] != NULL; i++)
        if (mnemonic == stores[i])
            isStore = true;
    if ((isLoad || isStore) && operands.getSize() >= 2 && operands.getSize() <= 3 &&
        isLabel(operands[1]) && operands[0].m_kind == Operand::KIND_REGISTER)
    {
        bool isFloatRegister = operands[0].m_register >= RISCV_REG_F0;
        uint temporary;
        if (operands.getSize() == 3)
        {
            if (!isRegister(operands[2]))
                return false;
            temporary = operands[2].m_register - RISCV_REG_X0;
        } else
        {
            // A load into an x register uses it as the temporary; the stores and
            // the floating point loads need one
            if (isStore || isFloatRegister)
                return false;
            temporary = operands[0].m_register - RISCV_REG_X0;
        }
        cArray<Operand> first, second;
        first.append(makeRegister(temporary));
        first.append(makeImmediate(0));
        assembleOne(target, cString("auipc"), first, false, units);
        second.append(operands[0]);
        second.append(makeMemory(temporary));
        assembleOne(target, mnemonic, second, false, units);
        units[units.getSize() - 2].m_label = operands[1].m_name;
        units[units.getSize() - 2].m_labelSpan = 8;
        return true;
    }
    return false;
}

} // namespace riscvasm
