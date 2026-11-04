#include "dismount/dismount.h"
/*
 * ArmOperandCodecSimd.cpp
 *
 * Implementation file. The kinds of the A64 scalar floating point, Advanced
 * SIMD and cryptographic instructions: the vector registers with their
 * arrangement, the vector elements and lists, the floating point, modified
 * and shift immediates, the fixed point and rotation operands, the new
 * scalar register sizes and the addressing of the structure loads and
 * stores.
 *
 * The fields which choose the arrangement (Q, size, sz, immh, imm5) are
 * shared by the operands of an entry: each operand writes its view of them
 * when it is encoded, and the assembler decodes the bits again and compares
 * the result with the text (isSameSimdOperand), so a text whose operands
 * disagree (add v0.8b, v1.16b, v2.8b) is rejected.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"

namespace armdis {

namespace {

typedef ArmDecodedOperand Operand;

// The bits of the instruction which are named in the Arm ARM
enum {
    // The vector length bit
    BIT_Q = 30,
    // The H, L and M bits of the by element forms
    BIT_H = 11,
    BIT_L = 21,
    BIT_M = 20
};

uint getField(uint32 encoding, uint position, uint width)
{
    return ArmOperandCodec::getField(encoding, position, width);
}

void setField(uint32& encoding, uint position, uint width, uint value)
{
    ArmOperandCodec::setField(encoding, position, width, value);
}

// The index of the lowest set bit of a non zero value
uint lowestBit(uint value)
{
    uint index = 0;
    while (((value >> index) & 1) == 0)
        index++;
    return index;
}

// The index of the highest set bit of a non zero value
uint highestBit(uint value)
{
    uint index = 0;
    while ((value >> (index + 1)) != 0)
        index++;
    return index;
}

// True if the size is in the mask of the valid sizes (0 allows all)
bool isSizeAllowed(uint flags, uint size)
{
    return ((flags & 0xF) == 0) || (((flags >> size) & 1) != 0);
}

//////////////////////////////////////////////////////////////////////////
// The arrangements

// The arrangement of (element size, Q): 8b 16b 4h 8h 2s 4s 1d 2d
uint makeArrangement(uint sizeLog2, uint q)
{
    return Operand::ARRANGEMENT_8B + sizeLog2 * 2 + q;
}

// The inverse of makeArrangement(). False for the other arrangements
bool splitArrangement(uint arrangement, uint& sizeLog2, uint& q)
{
    if ((arrangement < Operand::ARRANGEMENT_8B) || (arrangement > Operand::ARRANGEMENT_2D))
        return false;
    sizeLog2 = (arrangement - Operand::ARRANGEMENT_8B) / 2;
    q = (arrangement - Operand::ARRANGEMENT_8B) % 2;
    return true;
}

// The arrangement the bits give for a scheme. False if the bits are reserved
bool getArrangement(const ArmOperandSpec& spec, uint32 encoding, uint& arrangement)
{
    uint q = getField(encoding, BIT_Q, 1);
    uint flags = spec.m_flags;
    switch (spec.m_extra)
    {
    case ARM_ARR_Q_SIZE:
    case ARM_ARR_Q_SIZE_NO_B:
    case ARM_ARR_Q_SIZE_1D:
    {
        uint size = getField(encoding, ((flags & ARM_ARR_FLAG_SIZE10) != 0) ? 10 : 22, 2);
        if (!isSizeAllowed(flags, size))
            return false;
        if ((spec.m_extra == ARM_ARR_Q_SIZE_NO_B) && (size == 0))
            return false;
        if ((size == 3) && (q == 0) && (spec.m_extra != ARM_ARR_Q_SIZE_1D))
            return false;
        if (((flags & ARM_ARR_FLAG_NO_2S) != 0) && (size == 2) && (q == 0))
            return false;
        arrangement = makeArrangement(size, q);
        return true;
    }
    case ARM_ARR_Q_SZ:
    {
        uint sz = getField(encoding, 22, 1);
        if ((sz == 1) && (q == 0))
            return false;
        arrangement = makeArrangement(sz + 2, q);
        return true;
    }
    case ARM_ARR_Q_HALF:
        arrangement = q ? Operand::ARRANGEMENT_8H : Operand::ARRANGEMENT_4H;
        return true;
    case ARM_ARR_Q_BYTES:
        arrangement = q ? Operand::ARRANGEMENT_16B : Operand::ARRANGEMENT_8B;
        return true;
    case ARM_ARR_Q_SINGLE:
        arrangement = q ? Operand::ARRANGEMENT_4S : Operand::ARRANGEMENT_2S;
        return true;
    case ARM_ARR_Q_2H4H:
        arrangement = q ? Operand::ARRANGEMENT_4H : Operand::ARRANGEMENT_2H;
        return true;
    case ARM_ARR_LONG_SIZE:
    {
        uint size = getField(encoding, 22, 2);
        if (size == 3)
            return false;
        if (!isSizeAllowed(flags, size))
            return false;
        arrangement = Operand::ARRANGEMENT_8H + size * 2;
        return true;
    }
    case ARM_ARR_Q_LONG_PAIR:
    {
        uint size = getField(encoding, 22, 2);
        if ((size == 3) || !isSizeAllowed(flags, size))
            return false;
        arrangement = makeArrangement(size + 1, q);
        return true;
    }
    case ARM_ARR_Q_IMMH:
    {
        uint immh = getField(encoding, 19, 4);
        if (immh == 0)
            return false;
        uint k = highestBit(immh);
        if ((k == 3) && ((q == 0) || ((flags & 1) != 0)))
            return false;
        arrangement = makeArrangement(k, q);
        return true;
    }
    case ARM_ARR_IMMH_LONG:
    {
        uint immh = getField(encoding, 19, 4);
        if ((immh == 0) || (immh >= 8))
            return false;
        arrangement = Operand::ARRANGEMENT_8H + highestBit(immh) * 2;
        return true;
    }
    case ARM_ARR_Q_IMM5:
    {
        uint imm5 = getField(encoding, 16, 5);
        if ((imm5 & 15) == 0)
            return false;
        uint k = lowestBit(imm5);
        if ((k == 3) && (q == 0))
            return false;
        arrangement = makeArrangement(k, q);
        return true;
    }
    case ARM_ARR_Q_SZ_NARROW:
    {
        uint sz = getField(encoding, 22, 1);
        if (sz == 0)
            arrangement = q ? Operand::ARRANGEMENT_8H : Operand::ARRANGEMENT_4H;
        else
            arrangement = q ? Operand::ARRANGEMENT_4S : Operand::ARRANGEMENT_2S;
        return true;
    }
    case ARM_ARR_SZ_LONG:
        arrangement = getField(encoding, 22, 1) ? Operand::ARRANGEMENT_2D :
                                                  Operand::ARRANGEMENT_4S;
        return true;
    case ARM_ARR_FIXED_16B:
        arrangement = Operand::ARRANGEMENT_16B;
        return true;
    case ARM_ARR_FIXED_4S:
        arrangement = Operand::ARRANGEMENT_4S;
        return true;
    case ARM_ARR_FIXED_2D:
        arrangement = Operand::ARRANGEMENT_2D;
        return true;
    case ARM_ARR_FIXED_1Q:
        arrangement = Operand::ARRANGEMENT_1Q;
        return true;
    case ARM_ARR_FIXED:
        arrangement = flags;
        return true;
    case ARM_ARR_FIXED_8H:
        arrangement = Operand::ARRANGEMENT_8H;
        return true;
    case ARM_ARR_ELEM_B:
        arrangement = Operand::ARRANGEMENT_B;
        return true;
    case ARM_ARR_ELEM_H:
        arrangement = Operand::ARRANGEMENT_H;
        return true;
    case ARM_ARR_ELEM_S:
        arrangement = Operand::ARRANGEMENT_S;
        return true;
    case ARM_ARR_ELEM_D:
        arrangement = Operand::ARRANGEMENT_D;
        return true;
    default:
        return false;
    }
}

// Set the fields of a scheme for an arrangement: the inverse of
// getArrangement(). False if the arrangement doesn't fit the scheme
bool setArrangement(const ArmOperandSpec& spec, uint arrangement, uint32& encoding)
{
    uint flags = spec.m_flags;
    uint sizeLog2, q;
    switch (spec.m_extra)
    {
    case ARM_ARR_Q_SIZE:
    case ARM_ARR_Q_SIZE_NO_B:
    case ARM_ARR_Q_SIZE_1D:
        if (!splitArrangement(arrangement, sizeLog2, q))
            return false;
        if (!isSizeAllowed(flags, sizeLog2))
            return false;
        if ((spec.m_extra == ARM_ARR_Q_SIZE_NO_B) && (sizeLog2 == 0))
            return false;
        if ((sizeLog2 == 3) && (q == 0) && (spec.m_extra != ARM_ARR_Q_SIZE_1D))
            return false;
        if (((flags & ARM_ARR_FLAG_NO_2S) != 0) && (sizeLog2 == 2) && (q == 0))
            return false;
        setField(encoding, BIT_Q, 1, q);
        setField(encoding, ((flags & ARM_ARR_FLAG_SIZE10) != 0) ? 10 : 22, 2, sizeLog2);
        return true;
    case ARM_ARR_Q_SZ:
        if (!splitArrangement(arrangement, sizeLog2, q) || (sizeLog2 < 2))
            return false;
        if ((sizeLog2 == 3) && (q == 0))
            return false;
        setField(encoding, BIT_Q, 1, q);
        setField(encoding, 22, 1, sizeLog2 - 2);
        return true;
    case ARM_ARR_Q_HALF:
        if ((arrangement != Operand::ARRANGEMENT_4H) && (arrangement != Operand::ARRANGEMENT_8H))
            return false;
        setField(encoding, BIT_Q, 1, (arrangement == Operand::ARRANGEMENT_8H) ? 1 : 0);
        return true;
    case ARM_ARR_Q_BYTES:
        if ((arrangement != Operand::ARRANGEMENT_8B) && (arrangement != Operand::ARRANGEMENT_16B))
            return false;
        setField(encoding, BIT_Q, 1, (arrangement == Operand::ARRANGEMENT_16B) ? 1 : 0);
        return true;
    case ARM_ARR_Q_SINGLE:
        if ((arrangement != Operand::ARRANGEMENT_2S) && (arrangement != Operand::ARRANGEMENT_4S))
            return false;
        setField(encoding, BIT_Q, 1, (arrangement == Operand::ARRANGEMENT_4S) ? 1 : 0);
        return true;
    case ARM_ARR_Q_2H4H:
        if ((arrangement != Operand::ARRANGEMENT_2H) && (arrangement != Operand::ARRANGEMENT_4H))
            return false;
        setField(encoding, BIT_Q, 1, (arrangement == Operand::ARRANGEMENT_4H) ? 1 : 0);
        return true;
    case ARM_ARR_LONG_SIZE:
        if ((arrangement != Operand::ARRANGEMENT_8H) && (arrangement != Operand::ARRANGEMENT_4S) &&
            (arrangement != Operand::ARRANGEMENT_2D))
            return false;
        sizeLog2 = (arrangement - Operand::ARRANGEMENT_8H) / 2;
        if (!isSizeAllowed(flags, sizeLog2))
            return false;
        setField(encoding, 22, 2, sizeLog2);
        return true;
    case ARM_ARR_Q_LONG_PAIR:
        if (!splitArrangement(arrangement, sizeLog2, q) || (sizeLog2 == 0))
            return false;
        if (!isSizeAllowed(flags, sizeLog2 - 1))
            return false;
        setField(encoding, BIT_Q, 1, q);
        setField(encoding, 22, 2, sizeLog2 - 1);
        return true;
    case ARM_ARR_Q_IMMH:
        if (!splitArrangement(arrangement, sizeLog2, q))
            return false;
        if ((sizeLog2 == 3) && ((q == 0) || ((flags & 1) != 0)))
            return false;
        setField(encoding, BIT_Q, 1, q);
        setField(encoding, 19, 4, 1u << sizeLog2);
        return true;
    case ARM_ARR_IMMH_LONG:
        if ((arrangement != Operand::ARRANGEMENT_8H) && (arrangement != Operand::ARRANGEMENT_4S) &&
            (arrangement != Operand::ARRANGEMENT_2D))
            return false;
        sizeLog2 = (arrangement - Operand::ARRANGEMENT_8H) / 2;
        if (sizeLog2 > 2)
            return false;
        setField(encoding, 19, 4, 1u << sizeLog2);
        return true;
    case ARM_ARR_Q_IMM5:
        if (!splitArrangement(arrangement, sizeLog2, q))
            return false;
        if ((sizeLog2 == 3) && (q == 0))
            return false;
        setField(encoding, BIT_Q, 1, q);
        setField(encoding, 16, 5, 1u << sizeLog2);
        return true;
    case ARM_ARR_Q_SZ_NARROW:
        if (arrangement == Operand::ARRANGEMENT_4H || arrangement == Operand::ARRANGEMENT_8H)
        {
            setField(encoding, 22, 1, 0);
            setField(encoding, BIT_Q, 1, (arrangement == Operand::ARRANGEMENT_8H) ? 1 : 0);
            return true;
        }
        if (arrangement == Operand::ARRANGEMENT_2S || arrangement == Operand::ARRANGEMENT_4S)
        {
            setField(encoding, 22, 1, 1);
            setField(encoding, BIT_Q, 1, (arrangement == Operand::ARRANGEMENT_4S) ? 1 : 0);
            return true;
        }
        return false;
    case ARM_ARR_SZ_LONG:
        if ((arrangement != Operand::ARRANGEMENT_4S) && (arrangement != Operand::ARRANGEMENT_2D))
            return false;
        setField(encoding, 22, 1, (arrangement == Operand::ARRANGEMENT_2D) ? 1 : 0);
        return true;
    case ARM_ARR_FIXED_16B:
        return arrangement == Operand::ARRANGEMENT_16B;
    case ARM_ARR_FIXED_4S:
        return arrangement == Operand::ARRANGEMENT_4S;
    case ARM_ARR_FIXED_2D:
        return arrangement == Operand::ARRANGEMENT_2D;
    case ARM_ARR_FIXED_1Q:
        return arrangement == Operand::ARRANGEMENT_1Q;
    case ARM_ARR_FIXED:
        return arrangement == flags;
    case ARM_ARR_FIXED_8H:
        return arrangement == Operand::ARRANGEMENT_8H;
    case ARM_ARR_ELEM_B:
        return arrangement == Operand::ARRANGEMENT_B;
    case ARM_ARR_ELEM_H:
        return arrangement == Operand::ARRANGEMENT_H;
    case ARM_ARR_ELEM_S:
        return arrangement == Operand::ARRANGEMENT_S;
    case ARM_ARR_ELEM_D:
        return arrangement == Operand::ARRANGEMENT_D;
    default:
        return false;
    }
}

// True if the register id is one of v0-v31
bool isVectorRegister(uint registerId)
{
    return (registerId >= ARM64_REG_V0) && (registerId <= ARM64_REG_V31);
}

// The element size log2 of the arrangements B H S D, false for the others
bool elementSizeLog2(uint arrangement, uint& sizeLog2)
{
    if ((arrangement < Operand::ARRANGEMENT_B) || (arrangement > Operand::ARRANGEMENT_D))
        return false;
    sizeLog2 = arrangement - Operand::ARRANGEMENT_B;
    return true;
}

//////////////////////////////////////////////////////////////////////////
// The scalar floating point registers of the new schemes

// The register id of a b / h / s / d scalar register
uint scalarRegisterId(uint number, uint bits)
{
    switch (bits)
    {
    case 8:   return ARM64_REG_B0 + number;
    case 16:  return ARM64_REG_H0 + number;
    case 32:  return ARM64_REG_S0 + number;
    case 64:  return ARM64_REG_D0 + number;
    default:  return ARM64_REG_Q0 + number;
    }
}

// Classify a scalar register id: its size in bits and its number
bool classifyScalar(uint id, uint& bits, uint& number)
{
    static const uint bases[] = { ARM64_REG_B0, ARM64_REG_H0, ARM64_REG_S0,
                                  ARM64_REG_D0, ARM64_REG_Q0 };
    for (uint i = 0; i < 5; i++)
        if ((id >= bases[i]) && (id < bases[i] + 32))
        {
            bits = 8u << i;
            number = id - bases[i];
            return true;
        }
    return false;
}

bool decodeScalar(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    uint flags = spec.m_flags;
    uint bits;
    switch (spec.m_extra)
    {
    case ARM_FPR_SIZE_MASK:
    {
        uint size = getField(encoding, 22, 2);
        if (!isSizeAllowed(flags, size))
            return false;
        bits = 8u << size;
        break;
    }
    case ARM_FPR_LONG_MASK:
    {
        uint size = getField(encoding, 22, 2);
        if (size > 2)
            return false;
        if (((flags & 0xF) != 0) && !isSizeAllowed(flags, size))
            return false;
        bits = 16u << size;
        break;
    }
    case ARM_FPR_IMMH:
    case ARM_FPR_IMMH_NARROW:
    case ARM_FPR_IMMH_WIDE:
    {
        uint immh = getField(encoding, 19, 4);
        if (immh == 0)
            return false;
        uint k = highestBit(immh);
        if ((spec.m_extra != ARM_FPR_IMMH) && (k > 2))
            return false;
        bits = ((spec.m_extra == ARM_FPR_IMMH_WIDE) ? 16u : 8u) << k;
        break;
    }
    case ARM_FPR_IMM5:
    {
        uint imm5 = getField(encoding, 16, 5);
        if ((imm5 & 15) == 0)
            return false;
        if (!isSizeAllowed(flags, lowestBit(imm5)))
            return false;
        bits = 8u << lowestBit(imm5);
        break;
    }
    default:
        return false;
    }
    operand.m_kind = Operand::KIND_REGISTER;
    operand.m_register = scalarRegisterId(getField(encoding, spec.m_position, 5), bits);
    return true;
}

bool encodeScalar(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if (operand.m_kind != Operand::KIND_REGISTER)
        return false;
    uint bits, number;
    if (!classifyScalar(operand.m_register, bits, number))
        return false;
    uint flags = spec.m_flags;
    uint sizeLog2 = lowestBit(bits / 8);
    switch (spec.m_extra)
    {
    case ARM_FPR_SIZE_MASK:
        if ((bits == 128) || !isSizeAllowed(flags, sizeLog2))
            return false;
        setField(encoding, 22, 2, sizeLog2);
        break;
    case ARM_FPR_LONG_MASK:
    {
        if ((bits < 16) || (bits > 64))
            return false;
        uint size = sizeLog2 - 1;
        if (((flags & 0xF) != 0) && !isSizeAllowed(flags, size))
            return false;
        setField(encoding, 22, 2, size);
        break;
    }
    case ARM_FPR_IMMH:
    case ARM_FPR_IMMH_NARROW:
    case ARM_FPR_IMMH_WIDE:
    {
        uint k = sizeLog2;
        if (spec.m_extra == ARM_FPR_IMMH_WIDE)
        {
            if (bits < 16)
                return false;
            k--;
        }
        if ((bits == 128) || ((spec.m_extra != ARM_FPR_IMMH) && (k > 2)))
            return false;
        setField(encoding, 19, 4, 1u << k);
        break;
    }
    case ARM_FPR_IMM5:
        if ((bits == 128) || !isSizeAllowed(flags, sizeLog2))
            return false;
        setField(encoding, 16, 5, 1u << sizeLog2);
        break;
    default:
        return false;
    }
    setField(encoding, spec.m_position, 5, number);
    return true;
}

//////////////////////////////////////////////////////////////////////////
// The vector elements

// The element of an ArmElementScheme: the register number, the arrangement
// and the index. False for the reserved bits
bool decodeElement(const ArmOperandSpec& spec, uint32 encoding,
                   uint& number, uint& arrangement, int& index)
{
    uint h = getField(encoding, BIT_H, 1);
    uint l = getField(encoding, BIT_L, 1);
    uint m = getField(encoding, BIT_M, 1);
    switch (spec.m_extra)
    {
    case ARM_ELEM_IMM5:
    {
        uint imm5 = getField(encoding, 16, 5);
        if ((imm5 & 15) == 0)
            return false;
        uint k = lowestBit(imm5);
        if (!isSizeAllowed(spec.m_flags, k))
            return false;
        number = getField(encoding, spec.m_position, 5);
        arrangement = Operand::ARRANGEMENT_B + k;
        index = (int)(imm5 >> (k + 1));
        return true;
    }
    case ARM_ELEM_IMM4:
    {
        uint imm5 = getField(encoding, 16, 5);
        if ((imm5 & 15) == 0)
            return false;
        uint k = lowestBit(imm5);
        number = getField(encoding, spec.m_position, 5);
        arrangement = Operand::ARRANGEMENT_B + k;
        index = (int)(getField(encoding, 11, 4) >> k);
        return true;
    }
    case ARM_ELEM_BY_SIZE:
    {
        uint size = getField(encoding, 22, 2);
        if (size == 1)
        {
            number = getField(encoding, spec.m_position, 4);
            arrangement = Operand::ARRANGEMENT_H;
            index = (int)((h << 2) | (l << 1) | m);
            return true;
        }
        if (size == 2)
        {
            number = getField(encoding, spec.m_position, 5);
            arrangement = Operand::ARRANGEMENT_S;
            index = (int)((h << 1) | l);
            return true;
        }
        return false;
    }
    case ARM_ELEM_BY_SZ:
    {
        number = getField(encoding, spec.m_position, 5);
        if (getField(encoding, 22, 1) == 0)
        {
            arrangement = Operand::ARRANGEMENT_S;
            index = (int)((h << 1) | l);
            return true;
        }
        if (l != 0)
            return false;
        arrangement = Operand::ARRANGEMENT_D;
        index = (int)h;
        return true;
    }
    case ARM_ELEM_HALF:
        number = getField(encoding, spec.m_position, 4);
        arrangement = Operand::ARRANGEMENT_H;
        index = (int)((h << 2) | (l << 1) | m);
        return true;
    case ARM_ELEM_DOT4B:
        number = getField(encoding, spec.m_position, 5);
        arrangement = Operand::ARRANGEMENT_4B;
        index = (int)((h << 1) | l);
        return true;
    case ARM_ELEM_PAIR2H:
        number = getField(encoding, spec.m_position, 5);
        arrangement = Operand::ARRANGEMENT_2H;
        index = (int)((h << 1) | l);
        return true;
    case ARM_ELEM_FCMLA:
    {
        uint size = getField(encoding, 22, 2);
        number = getField(encoding, spec.m_position, 5);
        if (size == 1)
        {
            arrangement = Operand::ARRANGEMENT_H;
            index = (int)((h << 1) | l);
            return true;
        }
        if ((size == 2) && (l == 0))
        {
            arrangement = Operand::ARRANGEMENT_S;
            index = (int)h;
            return true;
        }
        return false;
    }
    case ARM_ELEM_PAIR2B:
        number = getField(encoding, spec.m_position, 4);
        arrangement = Operand::ARRANGEMENT_2B;
        index = (int)((h << 2) | (l << 1) | m);
        return true;
    case ARM_ELEM_BYTE:
        number = getField(encoding, spec.m_position, 3);
        arrangement = Operand::ARRANGEMENT_B;
        index = (int)((h << 3) | (l << 2) | (m << 1) | getField(encoding, 19, 1));
        return true;
    case ARM_ELEM_D1:
        number = getField(encoding, spec.m_position, 5);
        arrangement = Operand::ARRANGEMENT_D;
        index = 1;
        return true;
    case ARM_ELEM_IMM2_S:
        number = getField(encoding, spec.m_position, 5);
        arrangement = Operand::ARRANGEMENT_S;
        index = (int)getField(encoding, 12, 2);
        return true;
    case ARM_ELEM_LUT:
    {
        // m_flags bits 0-1: the number of index bits, bits 2-4: the position
        // of the index minus 10
        uint width = spec.m_flags & 3;
        uint position = 10 + ((spec.m_flags >> 2) & 7);
        number = getField(encoding, spec.m_position, 5);
        arrangement = Operand::ARRANGEMENT_NONE;
        index = (int)getField(encoding, position, width);
        return true;
    }
    default:
        return false;
    }
}

bool encodeElement(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_REGISTER) || !isVectorRegister(operand.m_register) ||
        (operand.m_elementIndex < 0))
        return false;
    uint number = operand.m_register - ARM64_REG_V0;
    uint index = (uint)operand.m_elementIndex;
    uint arrangement = operand.m_arrangement;
    uint k = 0;
    switch (spec.m_extra)
    {
    case ARM_ELEM_IMM5:
        if (!elementSizeLog2(arrangement, k) || !isSizeAllowed(spec.m_flags, k))
            return false;
        if (index >= (16u >> k))
            return false;
        setField(encoding, 16, 5, (index << (k + 1)) | (1u << k));
        setField(encoding, spec.m_position, 5, number);
        return true;
    case ARM_ELEM_IMM4:
    {
        if (!elementSizeLog2(arrangement, k))
            return false;
        uint imm5 = getField(encoding, 16, 5);
        if (((imm5 & 15) == 0) || (lowestBit(imm5) != k))
            return false;
        if (index >= (16u >> k))
            return false;
        setField(encoding, 11, 4, index << k);
        setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case ARM_ELEM_BY_SIZE:
        if (arrangement == Operand::ARRANGEMENT_H)
        {
            if ((index >= 8) || (number >= 16))
                return false;
            setField(encoding, 22, 2, 1);
            setField(encoding, BIT_H, 1, index >> 2);
            setField(encoding, BIT_L, 1, (index >> 1) & 1);
            setField(encoding, BIT_M, 1, index & 1);
            setField(encoding, spec.m_position, 4, number);
            return true;
        }
        if (arrangement == Operand::ARRANGEMENT_S)
        {
            if (index >= 4)
                return false;
            setField(encoding, 22, 2, 2);
            setField(encoding, BIT_H, 1, index >> 1);
            setField(encoding, BIT_L, 1, index & 1);
            setField(encoding, spec.m_position, 5, number);
            return true;
        }
        return false;
    case ARM_ELEM_BY_SZ:
        if (arrangement == Operand::ARRANGEMENT_S)
        {
            if (index >= 4)
                return false;
            setField(encoding, 22, 1, 0);
            setField(encoding, BIT_H, 1, index >> 1);
            setField(encoding, BIT_L, 1, index & 1);
        } else if (arrangement == Operand::ARRANGEMENT_D)
        {
            if (index >= 2)
                return false;
            setField(encoding, 22, 1, 1);
            setField(encoding, BIT_H, 1, index);
            setField(encoding, BIT_L, 1, 0);
        } else
            return false;
        setField(encoding, spec.m_position, 5, number);
        return true;
    case ARM_ELEM_HALF:
        if ((arrangement != Operand::ARRANGEMENT_H) || (index >= 8) || (number >= 16))
            return false;
        setField(encoding, BIT_H, 1, index >> 2);
        setField(encoding, BIT_L, 1, (index >> 1) & 1);
        setField(encoding, BIT_M, 1, index & 1);
        setField(encoding, spec.m_position, 4, number);
        return true;
    case ARM_ELEM_DOT4B:
    case ARM_ELEM_PAIR2H:
        if ((arrangement != ((spec.m_extra == ARM_ELEM_DOT4B) ? Operand::ARRANGEMENT_4B :
                                                                Operand::ARRANGEMENT_2H)) ||
            (index >= 4))
            return false;
        setField(encoding, BIT_H, 1, index >> 1);
        setField(encoding, BIT_L, 1, index & 1);
        setField(encoding, spec.m_position, 5, number);
        return true;
    case ARM_ELEM_FCMLA:
        if (arrangement == Operand::ARRANGEMENT_H)
        {
            if (index >= 4)
                return false;
            setField(encoding, 22, 2, 1);
            setField(encoding, BIT_H, 1, index >> 1);
            setField(encoding, BIT_L, 1, index & 1);
        } else if (arrangement == Operand::ARRANGEMENT_S)
        {
            if (index >= 2)
                return false;
            setField(encoding, 22, 2, 2);
            setField(encoding, BIT_H, 1, index);
            setField(encoding, BIT_L, 1, 0);
        } else
            return false;
        setField(encoding, spec.m_position, 5, number);
        return true;
    case ARM_ELEM_PAIR2B:
        if ((arrangement != Operand::ARRANGEMENT_2B) || (index >= 8) || (number >= 16))
            return false;
        setField(encoding, BIT_H, 1, index >> 2);
        setField(encoding, BIT_L, 1, (index >> 1) & 1);
        setField(encoding, BIT_M, 1, index & 1);
        setField(encoding, spec.m_position, 4, number);
        return true;
    case ARM_ELEM_BYTE:
        if ((arrangement != Operand::ARRANGEMENT_B) || (index >= 16) || (number >= 8))
            return false;
        setField(encoding, BIT_H, 1, index >> 3);
        setField(encoding, BIT_L, 1, (index >> 2) & 1);
        setField(encoding, BIT_M, 1, (index >> 1) & 1);
        setField(encoding, 19, 1, index & 1);
        setField(encoding, spec.m_position, 3, number);
        return true;
    case ARM_ELEM_D1:
        if ((arrangement != Operand::ARRANGEMENT_D) || (index != 1))
            return false;
        setField(encoding, spec.m_position, 5, number);
        return true;
    case ARM_ELEM_IMM2_S:
        if ((arrangement != Operand::ARRANGEMENT_S) || (index >= 4))
            return false;
        setField(encoding, 12, 2, index);
        setField(encoding, spec.m_position, 5, number);
        return true;
    case ARM_ELEM_LUT:
    {
        uint width = spec.m_flags & 3;
        uint position = 10 + ((spec.m_flags >> 2) & 7);
        if ((arrangement != Operand::ARRANGEMENT_NONE) || (index >= (1u << width)))
            return false;
        setField(encoding, position, width, index);
        setField(encoding, spec.m_position, 5, number);
        return true;
    }
    default:
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// The lists

// The spec of the arrangement of a list: the m_flags of a list are its
// register count, the arrangement flags are the ones of ARM_ARR_FLAG_SIZE10
ArmOperandSpec getListArrangementSpec(const ArmOperandSpec& spec)
{
    ArmOperandSpec result = spec;
    result.m_flags = spec.m_flags & ARM_ARR_FLAG_SIZE10;
    return result;
}

// The element index of the single structure forms: Q:S:size
int getListElementIndex(uint arrangement, uint32 encoding)
{
    uint q = getField(encoding, BIT_Q, 1);
    uint s = getField(encoding, 12, 1);
    uint size = getField(encoding, 10, 2);
    switch (arrangement)
    {
    case Operand::ARRANGEMENT_B: return (int)((q << 3) | (s << 2) | size);
    case Operand::ARRANGEMENT_H: return (int)((q << 2) | (s << 1) | (size >> 1));
    case Operand::ARRANGEMENT_S: return (int)((q << 1) | s);
    default:                     return (int)q;
    }
}

bool setListElementIndex(uint arrangement, uint index, uint32& encoding)
{
    switch (arrangement)
    {
    case Operand::ARRANGEMENT_B:
        if (index >= 16)
            return false;
        setField(encoding, BIT_Q, 1, index >> 3);
        setField(encoding, 12, 1, (index >> 2) & 1);
        setField(encoding, 10, 2, index & 3);
        return true;
    case Operand::ARRANGEMENT_H:
        if (index >= 8)
            return false;
        setField(encoding, BIT_Q, 1, index >> 2);
        setField(encoding, 12, 1, (index >> 1) & 1);
        setField(encoding, 10, 2, (index & 1) << 1);
        return true;
    case Operand::ARRANGEMENT_S:
        if (index >= 4)
            return false;
        setField(encoding, BIT_Q, 1, index >> 1);
        setField(encoding, 12, 1, index & 1);
        setField(encoding, 10, 2, 0);
        return true;
    case Operand::ARRANGEMENT_D:
        if (index >= 2)
            return false;
        setField(encoding, BIT_Q, 1, index);
        setField(encoding, 12, 1, 0);
        setField(encoding, 10, 2, 1);
        return true;
    default:
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// The immediates

// The real value of an 8 bit floating point immediate (VFPExpandImm): the
// sign a, the exponent NOT(b):c:d - 3 and the fraction efgh
double expandFloatImmediate(uint imm8)
{
    uint n = (((imm8 >> 6) & 1) ^ 1) << 2 | ((imm8 >> 4) & 3);
    int exponent = (int)n - 3;
    double value = (16 + (imm8 & 15)) / 16.0;
    while (exponent > 0)
    {
        value *= 2;
        exponent--;
    }
    while (exponent < 0)
    {
        value /= 2;
        exponent++;
    }
    return ((imm8 >> 7) & 1) ? -value : value;
}

// The imm8 of the modified immediate forms: a:b:c at 16-18, d:e:f:g:h at 5-9
uint getModifiedImm8(uint32 encoding)
{
    return (getField(encoding, 16, 3) << 5) | getField(encoding, 5, 5);
}

void setModifiedImm8(uint32& encoding, uint imm8)
{
    setField(encoding, 16, 3, imm8 >> 5);
    setField(encoding, 5, 5, imm8 & 31);
}

bool decodeModifiedImmediate(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    uint imm8 = getModifiedImm8(encoding);
    operand.m_kind = Operand::KIND_IMMEDIATE;
    operand.m_isHex = true;
    operand.m_immediate = (int64)imm8;
    switch (spec.m_extra)
    {
    case ARM_MODIMM_32_LSL:
    case ARM_MODIMM_16_LSL:
    {
        uint amount = 8 * ((spec.m_extra == ARM_MODIMM_32_LSL) ? getField(encoding, 13, 2) :
                                                                  getField(encoding, 13, 1));
        if (amount != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = amount;
        }
        return true;
    }
    case ARM_MODIMM_32_MSL:
        operand.m_shift = Operand::SHIFT_MSL;
        operand.m_shiftAmount = 8 + 8 * getField(encoding, 12, 1);
        return true;
    case ARM_MODIMM_8:
        return true;
    case ARM_MODIMM_64:
    {
        uint64 value = 0;
        for (uint i = 0; i < 8; i++)
            if (((imm8 >> i) & 1) != 0)
                value |= (uint64)0xFF << (i * 8);
        operand.m_immediate = (int64)value;
        // LLVM prints it with the C format %#016llx: 16 digits for zero
        // (without the prefix), at least 14 and the prefix for the others
        operand.m_isUnsigned = true;
        operand.m_isExplicit = true;
        return true;
    }
    default:
        return false;
    }
}

bool encodeModifiedImmediate(const ArmOperandSpec& spec, const Operand& operand,
                             uint32& encoding)
{
    if (operand.m_kind != Operand::KIND_IMMEDIATE)
        return false;
    uint64 value = (uint64)operand.m_immediate;
    uint amount = operand.m_shiftAmount;
    if (operand.m_shift == Operand::SHIFT_NONE)
        amount = 0;
    switch (spec.m_extra)
    {
    case ARM_MODIMM_32_LSL:
    case ARM_MODIMM_16_LSL:
    {
        if (((operand.m_shift != Operand::SHIFT_NONE) && (operand.m_shift != Operand::SHIFT_LSL)) ||
            (value > 255) || ((amount % 8) != 0))
            return false;
        uint step = amount / 8;
        if (step > ((spec.m_extra == ARM_MODIMM_32_LSL) ? 3u : 1u))
            return false;
        if (spec.m_extra == ARM_MODIMM_32_LSL)
            setField(encoding, 13, 2, step);
        else
            setField(encoding, 13, 1, step);
        setModifiedImm8(encoding, (uint)value);
        return true;
    }
    case ARM_MODIMM_32_MSL:
        if ((operand.m_shift != Operand::SHIFT_MSL) || (value > 255) ||
            ((amount != 8) && (amount != 16)))
            return false;
        setField(encoding, 12, 1, (amount == 16) ? 1 : 0);
        setModifiedImm8(encoding, (uint)value);
        return true;
    case ARM_MODIMM_8:
        if ((operand.m_shift != Operand::SHIFT_NONE) || (value > 255))
            return false;
        setModifiedImm8(encoding, (uint)value);
        return true;
    case ARM_MODIMM_64:
    {
        if (operand.m_shift != Operand::SHIFT_NONE)
            return false;
        uint imm8 = 0;
        for (uint i = 0; i < 8; i++)
        {
            uint byte = (uint)((value >> (i * 8)) & 0xFF);
            if (byte == 0xFF)
                imm8 |= 1u << i;
            else if (byte != 0)
                return false;
        }
        setModifiedImm8(encoding, imm8);
        return true;
    }
    default:
        return false;
    }
}

// The element size log2 of the highest set bit of immh, 0xFF if immh is 0
uint getImmhSize(uint32 encoding)
{
    uint immh = getField(encoding, 19, 4);
    return (immh == 0) ? 0xFF : highestBit(immh);
}

bool decodeShiftImmediate(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    uint flags = spec.m_flags;
    int64 amount;
    if ((flags & ARM_SIMD_SHIFT_XAR) != 0)
    {
        amount = getField(encoding, spec.m_position, 6);
    } else if ((flags & ARM_SIMD_SHIFT_ESIZE) != 0)
    {
        uint size = getField(encoding, 22, 2);
        if (size > 2)
            return false;
        amount = 8 << size;
    } else
    {
        uint k = getImmhSize(encoding);
        if (k == 0xFF)
            return false;
        int64 esize = 8 << k;
        int64 field = getField(encoding, 16, 7);
        amount = ((flags & ARM_SIMD_SHIFT_RIGHT) != 0) ? (2 * esize - field) : (field - esize);
    }
    operand.m_kind = Operand::KIND_IMMEDIATE;
    operand.m_immediate = amount;
    operand.m_isHex = (flags & ARM_SIMD_SHIFT_HEX) != 0;
    return true;
}

bool encodeShiftImmediate(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_IMMEDIATE) || (operand.m_shift != Operand::SHIFT_NONE))
        return false;
    uint flags = spec.m_flags;
    int64 amount = operand.m_immediate;
    if ((flags & ARM_SIMD_SHIFT_XAR) != 0)
    {
        if ((amount < 0) || (amount > 63))
            return false;
        setField(encoding, spec.m_position, 6, (uint)amount);
        return true;
    }
    if ((flags & ARM_SIMD_SHIFT_ESIZE) != 0)
        return amount == (int64)(8 << getField(encoding, 22, 2));
    uint k = getImmhSize(encoding);
    if (k == 0xFF)
        return false;
    int64 esize = 8 << k;
    int64 field;
    if ((flags & ARM_SIMD_SHIFT_RIGHT) != 0)
    {
        if ((amount < 1) || (amount > esize))
            return false;
        field = 2 * esize - amount;
    } else
    {
        if ((amount < 0) || (amount >= esize))
            return false;
        field = esize + amount;
    }
    setField(encoding, 16, 7, (uint)field);
    return true;
}

bool decodeFixedPoint(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    int64 fbits;
    if ((spec.m_flags & ARM_FBITS_VECTOR) != 0)
    {
        uint k = getImmhSize(encoding);
        if (k == 0xFF)
            return false;
        fbits = 2 * (int64)(8 << k) - (int64)getField(encoding, 16, 7);
    } else
    {
        uint scale = getField(encoding, spec.m_position, 6);
        // A 32 bit register has up to 32 fraction bits
        if ((getField(encoding, 31, 1) == 0) && (scale < 32))
            return false;
        fbits = 64 - (int64)scale;
    }
    operand.m_kind = Operand::KIND_IMMEDIATE;
    operand.m_immediate = fbits;
    operand.m_isHex = true;
    return true;
}

bool encodeFixedPoint(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_IMMEDIATE) || (operand.m_shift != Operand::SHIFT_NONE))
        return false;
    int64 fbits = operand.m_immediate;
    if ((spec.m_flags & ARM_FBITS_VECTOR) != 0)
    {
        uint k = getImmhSize(encoding);
        if (k == 0xFF)
            return false;
        int64 esize = 8 << k;
        if ((fbits < 1) || (fbits > esize))
            return false;
        setField(encoding, 16, 7, (uint)(2 * esize - fbits));
        return true;
    }
    int64 limit = (getField(encoding, 31, 1) != 0) ? 64 : 32;
    if ((fbits < 1) || (fbits > limit))
        return false;
    setField(encoding, spec.m_position, 6, (uint)(64 - fbits));
    return true;
}

//////////////////////////////////////////////////////////////////////////
// The memory operand of the structure loads and stores

bool decodeSimdMemory(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    uint base = getField(encoding, spec.m_position, 5);
    operand.m_kind = Operand::KIND_MEMORY;
    operand.m_base = (base == 31) ? ARM64_REG_SP : (ARM64_REG_X0 + base);
    operand.m_isHex = false;
    uint rm = getField(encoding, 16, 5);
    switch (spec.m_extra)
    {
    case ARM_ADDR_SIMD_POST_REG:
        if (rm == 31)
            return false;
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_postIndexRegister = ARM64_REG_X0 + rm;
        return true;
    case ARM_ADDR_SIMD_POST_IMM_MULTI:
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_offset = (int64)((getField(encoding, BIT_Q, 1) != 0) ? 16 : 8) * spec.m_flags;
        return true;
    case ARM_ADDR_SIMD_POST_IMM_FIXED:
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_offset = (int64)spec.m_flags;
        return true;
    default:
        return false;
    }
}

bool encodeSimdMemory(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_MEMORY) || (operand.m_index != ARM_NO_REGISTER) ||
        (operand.m_addressing != Operand::ADDRESSING_POST_INDEX))
        return false;
    uint base;
    if (operand.m_base == ARM64_REG_SP)
        base = 31;
    else if (operand.m_base <= ARM64_REG_X30)
        base = operand.m_base;
    else
        return false;
    switch (spec.m_extra)
    {
    case ARM_ADDR_SIMD_POST_REG:
        if ((operand.m_postIndexRegister < ARM64_REG_X0) ||
            (operand.m_postIndexRegister > ARM64_REG_X30))
            return false;
        setField(encoding, 16, 5, operand.m_postIndexRegister - ARM64_REG_X0);
        break;
    case ARM_ADDR_SIMD_POST_IMM_MULTI:
    {
        if (operand.m_postIndexRegister != ARM_NO_REGISTER)
            return false;
        int64 bytes = (int64)((getField(encoding, BIT_Q, 1) != 0) ? 16 : 8) * spec.m_flags;
        if (operand.m_offset != bytes)
            return false;
        setField(encoding, 16, 5, 31);
        break;
    }
    case ARM_ADDR_SIMD_POST_IMM_FIXED:
        if ((operand.m_postIndexRegister != ARM_NO_REGISTER) ||
            (operand.m_offset != (int64)spec.m_flags))
            return false;
        setField(encoding, 16, 5, 31);
        break;
    default:
        return false;
    }
    setField(encoding, spec.m_position, 5, base);
    return true;
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////
// The interface of ArmOperandCodecInternal.h

bool isSimdSpec(const ArmOperandSpec& spec)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_A64_VECTOR:
    case ARM_KIND_A64_VECTOR_ELEMENT:
    case ARM_KIND_A64_VECTOR_LIST:
    case ARM_KIND_A64_FLOAT_IMMEDIATE:
    case ARM_KIND_A64_SIMD_MODIFIED_IMMEDIATE:
    case ARM_KIND_A64_SIMD_SHIFT_IMMEDIATE:
    case ARM_KIND_A64_FIXED_POINT_FBITS:
    case ARM_KIND_A64_SIMD_ROTATION:
        return true;
    case ARM_KIND_A64_FPR:
        return spec.m_extra >= ARM_FPR_SIZE_MASK;
    case ARM_KIND_A64_MEMORY:
        return spec.m_extra >= ARM_ADDR_SIMD_POST_REG;
    default:
        return false;
    }
}

bool decodeSimd(const ArmOperandSpec& spec,
                uint32 encoding,
                const ArmCodecContext& context,
                ArmDecodedOperand& operand)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_A64_FPR:
        return decodeScalar(spec, encoding, operand);
    case ARM_KIND_A64_VECTOR:
    {
        uint arrangement;
        if (!getArrangement(spec, encoding, arrangement))
            return false;
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = ARM64_REG_V0 + getField(encoding, spec.m_position, 5);
        operand.m_arrangement = arrangement;
        return true;
    }
    case ARM_KIND_A64_VECTOR_ELEMENT:
    {
        uint number, arrangement;
        int index;
        if (!decodeElement(spec, encoding, number, arrangement, index))
            return false;
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = ARM64_REG_V0 + number;
        operand.m_arrangement = arrangement;
        operand.m_elementIndex = index;
        return true;
    }
    case ARM_KIND_A64_VECTOR_LIST:
    {
        uint arrangement;
        if (!getArrangement(getListArrangementSpec(spec), encoding, arrangement))
            return false;
        operand.m_kind = Operand::KIND_REGISTER_LIST;
        operand.m_register = ARM64_REG_V0 + getField(encoding, spec.m_position, 5);
        operand.m_count = spec.m_flags & 7;
        operand.m_stride = 1;
        operand.m_arrangement = arrangement;
        if ((spec.m_flags & ARM_SPEC_LIST_ELEMENT) != 0)
            operand.m_elementIndex = getListElementIndex(arrangement, encoding);
        return true;
    }
    case ARM_KIND_A64_FLOAT_IMMEDIATE:
    {
        operand.m_kind = Operand::KIND_FLOAT_IMMEDIATE;
        if ((spec.m_flags & ARM_FIMM_ZERO) != 0)
        {
            operand.m_floatImmediate = 0;
            operand.m_isExplicit = true;
            return true;
        }
        uint imm8 = ((spec.m_flags & ARM_FIMM_SPLIT) != 0) ?
                        getModifiedImm8(encoding) :
                        getField(encoding, spec.m_position, 8);
        operand.m_floatImmediate = expandFloatImmediate(imm8);
        return true;
    }
    case ARM_KIND_A64_SIMD_MODIFIED_IMMEDIATE:
        return decodeModifiedImmediate(spec, encoding, operand);
    case ARM_KIND_A64_SIMD_SHIFT_IMMEDIATE:
        return decodeShiftImmediate(spec, encoding, operand);
    case ARM_KIND_A64_FIXED_POINT_FBITS:
        return decodeFixedPoint(spec, encoding, operand);
    case ARM_KIND_A64_SIMD_ROTATION:
    {
        uint value = getField(encoding, spec.m_position, (spec.m_extra == ARM_ROT_ADD) ? 1 : 2);
        operand.m_kind = Operand::KIND_IMMEDIATE;
        operand.m_immediate = (spec.m_extra == ARM_ROT_ADD) ? (90 + 180 * (int64)value) :
                                                              (90 * (int64)value);
        return true;
    }
    case ARM_KIND_A64_MEMORY:
        return decodeSimdMemory(spec, encoding, operand);
    default:
        return false;
    }
}

bool encodeSimd(const ArmOperandSpec& spec,
                const ArmDecodedOperand& operand,
                const ArmCodecContext& context,
                uint32& encoding)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_A64_FPR:
        return encodeScalar(spec, operand, encoding);
    case ARM_KIND_A64_VECTOR:
        if ((operand.m_kind != Operand::KIND_REGISTER) || !isVectorRegister(operand.m_register) ||
            (operand.m_elementIndex >= 0))
            return false;
        if (!setArrangement(spec, operand.m_arrangement, encoding))
            return false;
        setField(encoding, spec.m_position, 5, operand.m_register - ARM64_REG_V0);
        return true;
    case ARM_KIND_A64_VECTOR_ELEMENT:
        return encodeElement(spec, operand, encoding);
    case ARM_KIND_A64_VECTOR_LIST:
    {
        if ((operand.m_kind != Operand::KIND_REGISTER_LIST) || !isVectorRegister(operand.m_register) ||
            (operand.m_count != (spec.m_flags & 7u)))
            return false;
        bool isElement = (spec.m_flags & ARM_SPEC_LIST_ELEMENT) != 0;
        if (isElement != (operand.m_elementIndex >= 0))
            return false;
        if (!setArrangement(getListArrangementSpec(spec), operand.m_arrangement, encoding))
            return false;
        if (isElement && !setListElementIndex(operand.m_arrangement,
                                              (uint)operand.m_elementIndex, encoding))
            return false;
        setField(encoding, spec.m_position, 5, operand.m_register - ARM64_REG_V0);
        return true;
    }
    case ARM_KIND_A64_FLOAT_IMMEDIATE:
    {
        if ((spec.m_flags & ARM_FIMM_ZERO) != 0)
        {
            // #0.0, and #0 which LLVM reads as the same
            if ((operand.m_kind == Operand::KIND_IMMEDIATE) &&
                (operand.m_shift == Operand::SHIFT_NONE))
                return ((spec.m_flags & ARM_FIMM_ZERO_INTEGER) != 0) &&
                       (operand.m_immediate == 0);
            return (operand.m_kind == Operand::KIND_FLOAT_IMMEDIATE) &&
                   (operand.m_floatImmediate == 0) &&
                   (1 / operand.m_floatImmediate > 0);
        }
        if (operand.m_kind != Operand::KIND_FLOAT_IMMEDIATE)
            return false;
        for (uint imm8 = 0; imm8 < 256; imm8++)
            if (expandFloatImmediate(imm8) == operand.m_floatImmediate)
            {
                if ((spec.m_flags & ARM_FIMM_SPLIT) != 0)
                    setModifiedImm8(encoding, imm8);
                else
                    setField(encoding, spec.m_position, 8, imm8);
                return true;
            }
        return false;
    }
    case ARM_KIND_A64_SIMD_MODIFIED_IMMEDIATE:
        return encodeModifiedImmediate(spec, operand, encoding);
    case ARM_KIND_A64_SIMD_SHIFT_IMMEDIATE:
        return encodeShiftImmediate(spec, operand, encoding);
    case ARM_KIND_A64_FIXED_POINT_FBITS:
        return encodeFixedPoint(spec, operand, encoding);
    case ARM_KIND_A64_SIMD_ROTATION:
    {
        if ((operand.m_kind != Operand::KIND_IMMEDIATE) || (operand.m_shift != Operand::SHIFT_NONE))
            return false;
        int64 value = operand.m_immediate;
        if (spec.m_extra == ARM_ROT_ADD)
        {
            if ((value != 90) && (value != 270))
                return false;
            setField(encoding, spec.m_position, 1, (value == 270) ? 1 : 0);
            return true;
        }
        if ((value < 0) || (value > 270) || ((value % 90) != 0))
            return false;
        setField(encoding, spec.m_position, 2, (uint)(value / 90));
        return true;
    }
    case ARM_KIND_A64_MEMORY:
        return encodeSimdMemory(spec, operand, encoding);
    default:
        return false;
    }
}

bool isSameSimdOperand(const ArmDecodedOperand& parsed, const ArmDecodedOperand& decoded)
{
    switch (decoded.m_kind)
    {
    case Operand::KIND_REGISTER:
    case Operand::KIND_REGISTER_LIST:
        if (!isVectorRegister(decoded.m_register))
            return true;
        // Same kind, count, arrangement and element: a list is one operand
        return (parsed.m_kind == decoded.m_kind) &&
               (parsed.m_arrangement == decoded.m_arrangement) &&
               (parsed.m_elementIndex == decoded.m_elementIndex) &&
               ((decoded.m_kind != Operand::KIND_REGISTER_LIST) ||
                (parsed.m_count == decoded.m_count));
    case Operand::KIND_MEMORY:
        return (parsed.m_postIndexRegister == decoded.m_postIndexRegister) &&
               (parsed.m_addressing == decoded.m_addressing);
    default:
        return true;
    }
}

bool isConsecutiveRegister(uint previous, uint next)
{
    // The registers of a group wrap around after the last one
    static const struct { uint m_first; uint m_count; } groups[] = {
        { ARM64_REG_Z0, 32 }, { ARM64_REG_V0, 32 }, { ARM64_REG_Q0, 32 },
        { ARM64_REG_D0, 32 }, { ARM64_REG_S0, 32 }, { ARM64_REG_H0, 32 },
        { ARM64_REG_B0, 32 }, { ARM64_REG_P0, 16 }
    };
    for (uint i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
        if ((previous >= groups[i].m_first) && (previous < groups[i].m_first + groups[i].m_count))
            return next == groups[i].m_first +
                   ((previous - groups[i].m_first + 1) % groups[i].m_count);
    return next == previous + 1;
}

} // namespace armdis
