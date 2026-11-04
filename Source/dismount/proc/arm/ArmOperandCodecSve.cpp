#include "dismount/dismount.h"
/*
 * ArmOperandCodecSve.cpp
 *
 * Implementation file. The SVE kinds of ArmOperandCodec: the z, p and ffr
 * registers, the z lists, the indexed z registers, the immediates, the
 * addressing, and the options (the predicate pattern, the prefetch
 * operation), see ArmEncodingTable.h and ArmOperandCodec.h.
 *
 * Every operand of an SVE instruction is described by a spec, and the
 * element size of a register by a scheme which names the fields that give
 * it (gSizeSchemes), so the one entry of the table covers all the sizes.
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
typedef ArmOperandCodec Codec;

//////////////////////////////////////////////////////////////////////////
// The element sizes

// The element sizes: the index in the Operand::ARRANGEMENT_B... order
enum {
    SIZE_B,
    SIZE_H,
    SIZE_S,
    SIZE_D,
    SIZE_Q,
    // The register has no arrangement ("z0", "p0")
    SIZE_NONE,
    // A reserved value of the size field
    SIZE_INVALID = 0xff
};

const uint gArrangementOfSize[] = {
    Operand::ARRANGEMENT_B, Operand::ARRANGEMENT_H, Operand::ARRANGEMENT_S,
    Operand::ARRANGEMENT_D, Operand::ARRANGEMENT_Q, Operand::ARRANGEMENT_NONE
};

// Return the size of an arrangement. False if it isn't an element size
bool getSizeOfArrangement(uint arrangement, uint& size)
{
    for (uint i = 0; i <= SIZE_NONE; i++)
        if (gArrangementOfSize[i] == arrangement)
        {
            size = i;
            return true;
        }
    return false;
}

/*
 * A size scheme: the fields (up to two, the first is the most significant)
 * which make the value that is mapped to the size. A scheme with no field
 * has the size in m_map[0] (the fixed ones).
 */
struct SizeScheme {
    uint8 m_position[2];
    uint8 m_width[2];
    uint8 m_map[32];
};

#define X SIZE_INVALID
#define B SIZE_B
#define H SIZE_H
#define S SIZE_S
#define D SIZE_D
#define Q SIZE_Q
#define N SIZE_NONE

// The tsz values: 0001 b, 001x h, 01xx s, 1xxx d (the highest set bit)
#define TSZ4 { X, B, H, H, S, S, S, S, D, D, D, D, D, D, D, D }
// 001 b, 01x h, 1xx s: three bit tsz
#define TSZ3 { X, B, H, H, S, S, S, S }
// The 3 bit tsz, one larger
#define TSZ3_WIDE { X, H, S, S, D, D, D, D }
// The dup tsz (lowest set bit of 5 bits): xxxx1 b, xxx10 h, xx100 s, x1000 d,
// 10000 q
#define DUP5 { X, B, H, B, S, B, H, B, D, B, H, B, S, B, H, B, \
               Q, B, H, B, S, B, H, B, D, B, H, B, S, B, H, B }

// The tsz of sqxtnb: a single bit
#define XTN3 { X, B, H, X, S, X, X, X }
#define XTN3_WIDE { X, H, S, X, D, X, X, X }

// Indexed by ArmSveSizeScheme
const SizeScheme gSizeSchemes[ARM_SVE_SIZE_SCHEMES_COUNT] = {
    /* ARM_SVE_NONE */ { {0, 0}, {0, 0}, { N } },
    /* ARM_SVE_B */ { {0, 0}, {0, 0}, { B } },
    /* ARM_SVE_H */ { {0, 0}, {0, 0}, { H } },
    /* ARM_SVE_S */ { {0, 0}, {0, 0}, { S } },
    /* ARM_SVE_D */ { {0, 0}, {0, 0}, { D } },
    /* ARM_SVE_Q */ { {0, 0}, {0, 0}, { Q } },
    /* ARM_SVE_SIZE_22 */ { {22, 0}, {2, 0}, { B, H, S, D } },
    /* ARM_SVE_SIZE_22_WIDE */ { {22, 0}, {2, 0}, { X, H, S, D } },
    /* ARM_SVE_SIZE_22_NARROW */ { {22, 0}, {2, 0}, { X, B, H, S } },
    /* ARM_SVE_TSZ */ { {22, 8}, {2, 2}, TSZ4 },
    /* ARM_SVE_DUP_TSZ */ { {16, 0}, {5, 0}, DUP5 },
    /* ARM_SVE_SZ_22 */ { {22, 0}, {1, 0}, { S, D } },
    /* ARM_SVE_SIZE_22_FLOAT */ { {22, 0}, {2, 0}, { X, H, S, D } },
    /* ARM_SVE_LOAD_DTYPE */ { {0, 0}, {0, 0}, { X } },
    /* ARM_SVE_SIZE_22_SD */ { {22, 0}, {2, 0}, { X, X, S, D } },
    /* ARM_SVE_SIZE_22_BHS */ { {22, 0}, {2, 0}, { B, H, S, X } },
    /* ARM_SVE_BIT_22_BH */ { {22, 0}, {1, 0}, { B, H } },
    /* ARM_SVE_BIT_22_SD */ { {22, 0}, {1, 0}, { S, D } },
    /* ARM_SVE_BIT_22_HS */ { {22, 0}, {1, 0}, { H, S } },
    /* ARM_SVE_TSZ_UNPRED */ { {22, 19}, {2, 2}, TSZ4 },
    /* ARM_SVE_TSZ_NARROW */ { {22, 19}, {1, 2}, TSZ3 },
    /* ARM_SVE_TSZ_NARROW_SOURCE */ { {22, 19}, {1, 2}, TSZ3_WIDE },
    /* ARM_SVE_TSZ_WIDEN_SOURCE */ { {22, 19}, {1, 2}, TSZ3 },
    /* ARM_SVE_TSZ_WIDEN_DEST */ { {22, 19}, {1, 2}, TSZ3_WIDE },
    /* ARM_SVE_LOGICAL_IMM */ { {0, 0}, {0, 0}, { X } },
    /* ARM_SVE_TSZ_PMOV */ { {22, 17}, {2, 2}, TSZ4 },
    /* ARM_SVE_XTN_DEST */ { {22, 19}, {1, 2}, XTN3 },
    /* ARM_SVE_XTN_SOURCE */ { {22, 19}, {1, 2}, XTN3_WIDE },
    /* ARM_SVE_SIZE_MOVA */ { {22, 16}, {2, 1}, { B, X, H, X, S, X, D, Q } }
};

#undef X
#undef B
#undef H
#undef S
#undef D
#undef Q
#undef N

// The element size of the bitmask immediate of dupm: the smallest of 8, 16,
// 32 bits which repeats in the 64 bit value, else 64
uint getLogicalImmediateSize(uint32 encoding, uint64& value)
{
    if (!decodeLogicalImmediateValue(Codec::getField(encoding, 17, 1),
                                     Codec::getField(encoding, 11, 6),
                                     Codec::getField(encoding, 5, 6), 64, value))
        return SIZE_INVALID;
    for (uint bits = 8; bits < 64; bits *= 2)
    {
        uint64 mask = ((uint64)1 << bits) - 1;
        uint64 element = value & mask;
        uint64 replicated = 0;
        for (uint i = 0; i < 64; i += bits)
            replicated |= element << i;
        if (replicated == value)
            return (bits == 8) ? SIZE_B : (bits == 16) ? SIZE_H : SIZE_S;
    }
    return SIZE_D;
}

// Decode the size of a scheme from the bits. False for a reserved value
bool decodeSize(uint scheme, uint32 encoding, uint& size)
{
    ASSERT(scheme < ARM_SVE_SIZE_SCHEMES_COUNT);
    if (scheme == ARM_SVE_LOGICAL_IMM)
    {
        uint64 value;
        size = getLogicalImmediateSize(encoding, value);
        return size != SIZE_INVALID;
    }
    const SizeScheme& info = gSizeSchemes[scheme];
    uint value = 0;
    for (uint i = 0; i < 2; i++)
        if (info.m_width[i] != 0)
            value = (value << info.m_width[i]) |
                    Codec::getField(encoding, info.m_position[i], info.m_width[i]);
    size = info.m_map[value];
    return size != SIZE_INVALID;
}

// Set the fields of a scheme for a size. False if the scheme has no such size
bool encodeSize(uint scheme, uint size, uint32& encoding)
{
    ASSERT(scheme < ARM_SVE_SIZE_SCHEMES_COUNT);
    if (scheme == ARM_SVE_LOGICAL_IMM)
    {
        // The immediate holds the size: it reads the element size from the
        // bits 17-16 of the encoding, where this puts it (see IMM_LOGICAL)
        if (size > SIZE_D)
            return false;
        Codec::setField(encoding, 16, 2, size);
        return true;
    }
    if (scheme == ARM_SVE_TSZ_PMOV)
    {
        // The leading one: the index of the pmov element (written by the other
        // operand) is in the bits below it
        if (size > SIZE_D)
            return false;
        uint tsz = ((Codec::getField(encoding, 22, 2) << 2) | Codec::getField(encoding, 17, 2)) |
                   (1u << size);
        Codec::setField(encoding, 22, 2, tsz >> 2);
        Codec::setField(encoding, 17, 2, tsz & 3);
        return true;
    }
    const SizeScheme& info = gSizeSchemes[scheme];
    uint bits = info.m_width[0] + info.m_width[1];
    uint values = 1u << bits;
    for (uint value = 0; value < values; value++)
    {
        if (info.m_map[value] != size)
            continue;
        uint rest = value;
        for (int i = 1; i >= 0; i--)
            if (info.m_width[i] != 0)
            {
                Codec::setField(encoding, info.m_position[i], info.m_width[i],
                                rest & ((1u << info.m_width[i]) - 1));
                rest >>= info.m_width[i];
            }
        return true;
    }
    return false;
}

// The arrangement of a register operand for its scheme and bits
bool decodeArrangement(uint scheme, uint32 encoding, uint& arrangement)
{
    uint size;
    if (!decodeSize(scheme, encoding, size))
        return false;
    arrangement = gArrangementOfSize[size];
    return true;
}

// Set the fields of the scheme for the arrangement of a parsed operand
bool encodeArrangement(uint scheme, uint arrangement, uint32& encoding)
{
    uint size;
    if (!getSizeOfArrangement(arrangement, size))
        return false;
    if (scheme == ARM_SVE_NONE)
        return size == SIZE_NONE;
    if (size == SIZE_NONE)
        return false;
    return encodeSize(scheme, size, encoding);
}

//////////////////////////////////////////////////////////////////////////
// The registers

bool isZRegister(uint id)
{
    return (id >= ARM64_REG_Z0) && (id <= ARM64_REG_Z31);
}

bool isPRegister(uint id)
{
    return (id >= ARM64_REG_P0) && (id <= ARM64_REG_P15);
}

// Return the id of a general purpose register. Number 31 is sp or xzr
uint getXId(uint number, bool isStackPointer)
{
    if (number == 31)
        return isStackPointer ? ARM64_REG_SP : ARM64_REG_XZR;
    return ARM64_REG_X0 + number;
}

// The same for the 32 or 64 bit register
uint getGprId(uint number, bool is64, bool isStackPointer)
{
    if (is64)
        return getXId(number, isStackPointer);
    if (number == 31)
        return isStackPointer ? ARM64_REG_WSP : ARM64_REG_WZR;
    return ARM64_REG_W0 + number;
}

// Classify a general purpose register id (x, w, sp, wsp, xzr, wzr)
bool classifyGpr(uint id, bool& is64, uint& number, bool& isStackPointer)
{
    isStackPointer = false;
    if (id <= ARM64_REG_X30)
    {
        is64 = true;
        number = id;
        return true;
    }
    if ((id >= ARM64_REG_W0) && (id <= ARM64_REG_W30))
    {
        is64 = false;
        number = id - ARM64_REG_W0;
        return true;
    }
    switch (id)
    {
    case ARM64_REG_SP: is64 = true; number = 31; isStackPointer = true; return true;
    case ARM64_REG_XZR: is64 = true; number = 31; return true;
    case ARM64_REG_WSP: is64 = false; number = 31; isStackPointer = true; return true;
    case ARM64_REG_WZR: is64 = false; number = 31; return true;
    default: return false;
    }
}

// Classify an x register id (and sp / xzr). False if it isn't one
bool classifyX(uint id, uint& number, bool& isStackPointer)
{
    isStackPointer = false;
    if (id <= ARM64_REG_X30)
    {
        number = id - ARM64_REG_X0;
        return true;
    }
    if (id == ARM64_REG_SP)
    {
        number = 31;
        isStackPointer = true;
        return true;
    }
    if (id == ARM64_REG_XZR)
    {
        number = 31;
        return true;
    }
    return false;
}

// A plain register operand (no index, no qualifier, no list) of id 'id'
bool isPlainRegister(const Operand& operand)
{
    return (operand.m_kind == Operand::KIND_REGISTER) &&
           (operand.m_elementIndex < 0) &&
           (operand.m_predicateQualifier == Operand::PREDICATE_NONE) &&
           !operand.m_isPredicateCounter &&
           (operand.m_addressing == Operand::ADDRESSING_OFFSET) &&
           (operand.m_sliceRegister == (uint)ARM_NO_REGISTER);
}

//////////////////////////////////////////////////////////////////////////
// The predicates

// Return the PredicateQualifier of the qualifier mode of m_flags bits 1-3
bool getQualifier(uint flags, uint32 encoding, uint& qualifier)
{
    uint mode = (flags & ARM_SVE_PRED_QUALIFIER_MASK) >> ARM_SVE_PRED_QUALIFIER_SHIFT;
    uint bit;
    switch (mode)
    {
    case ARM_SVE_PRED_Q_NONE:
        qualifier = Operand::PREDICATE_NONE;
        return true;
    case ARM_SVE_PRED_Q_MERGING:
        qualifier = Operand::PREDICATE_MERGING;
        return true;
    case ARM_SVE_PRED_Q_ZEROING:
        qualifier = Operand::PREDICATE_ZEROING;
        return true;
    case ARM_SVE_PRED_Q_BIT4: bit = 4; break;
    case ARM_SVE_PRED_Q_BIT13: bit = 13; break;
    case ARM_SVE_PRED_Q_BIT16: bit = 16; break;
    case ARM_SVE_PRED_Q_BIT10: bit = 10; break;
    case ARM_SVE_PRED_Q_BIT20: bit = 20; break;
    default:
        return false;
    }
    qualifier = (Codec::getField(encoding, bit, 1) != 0) ? Operand::PREDICATE_MERGING :
                                                           Operand::PREDICATE_ZEROING;
    return true;
}

// Set the bit of a qualifier mode, check a fixed one. False if the operand's
// qualifier doesn't fit
bool encodeQualifier(uint flags, uint qualifier, uint32& encoding)
{
    uint mode = (flags & ARM_SVE_PRED_QUALIFIER_MASK) >> ARM_SVE_PRED_QUALIFIER_SHIFT;
    uint bit;
    switch (mode)
    {
    case ARM_SVE_PRED_Q_NONE:
        return qualifier == Operand::PREDICATE_NONE;
    case ARM_SVE_PRED_Q_MERGING:
        return qualifier == Operand::PREDICATE_MERGING;
    case ARM_SVE_PRED_Q_ZEROING:
        return qualifier == Operand::PREDICATE_ZEROING;
    case ARM_SVE_PRED_Q_BIT4: bit = 4; break;
    case ARM_SVE_PRED_Q_BIT13: bit = 13; break;
    case ARM_SVE_PRED_Q_BIT16: bit = 16; break;
    case ARM_SVE_PRED_Q_BIT10: bit = 10; break;
    case ARM_SVE_PRED_Q_BIT20: bit = 20; break;
    default:
        return false;
    }
    if (qualifier == Operand::PREDICATE_NONE)
        return false;
    Codec::setField(encoding, bit, 1, (qualifier == Operand::PREDICATE_MERGING) ? 1 : 0);
    return true;
}

//////////////////////////////////////////////////////////////////////////
// The immediates

// The 8 bit floating point constant (VFPExpandImm)
double expandFloat8(uint imm8)
{
    uint sign = (imm8 >> 7) & 1;
    uint exponentBits = (((imm8 >> 6) & 1) ^ 1) << 2 | ((imm8 >> 4) & 3);
    int exponent = (int)exponentBits - 3;
    double value = (16.0 + (double)(imm8 & 15)) / 16.0;
    while (exponent > 0)
    {
        value *= 2.0;
        exponent--;
    }
    while (exponent < 0)
    {
        value /= 2.0;
        exponent++;
    }
    return sign ? -value : value;
}

// Find the 8 bit encoding of a floating point constant
bool findFloat8(double value, uint& imm8)
{
    // The text is parsed with a floating point error: compare with a tolerance
    for (uint i = 0; i < 256; i++)
    {
        double difference = expandFloat8(i) - value;
        if (difference < 0)
            difference = -difference;
        double magnitude = (value < 0) ? -value : value;
        if (difference <= magnitude * 1e-9)
        {
            imm8 = i;
            return true;
        }
    }
    return false;
}

// The tsz:imm3 field of the shifts: the layout of a scheme. 'm_tszScheme' is
// the size scheme of the tsz, 'm_immPosition' the imm3, 'm_totalBits' the bits
// of tsz:imm3
struct ShiftLayout {
    uint m_tszScheme;
    uint m_immPosition;
    // The destination element (for the widening shifts the source one is
    // m_tszScheme), left shift or right shift
    bool m_isLeft;
    // The right shift counts down from 2 * esize (tsz:imm3 = 2 * esize -
    // shift); the narrowing ones use the size of the narrow destination
    // ('esize' is the destination one)
    bool m_isNarrow;
};

const ShiftLayout gShiftLayouts[] = {
    /* SHIFT_PRED_RIGHT */ { ARM_SVE_TSZ,          5, false, false },
    /* SHIFT_PRED_LEFT  */ { ARM_SVE_TSZ,          5, true,  false },
    /* SHIFT_RIGHT      */ { ARM_SVE_TSZ_UNPRED,  16, false, false },
    /* SHIFT_LEFT       */ { ARM_SVE_TSZ_UNPRED,  16, true,  false },
    /* SHIFT_NARROW     */ { ARM_SVE_TSZ_NARROW,  16, false, true  },
    /* SHIFT_WIDEN      */ { ARM_SVE_TSZ_WIDEN_SOURCE, 16, true, false }
};

// The immediate value of tsz:imm3 as one number, and the size. The tsz bits
// come first (the high bits), then the imm3
bool getShiftTsz(uint scheme, uint32 encoding, uint& tsz, uint& tszBits)
{
    const SizeScheme& info = gSizeSchemes[scheme];
    tsz = 0;
    tszBits = 0;
    for (uint i = 0; i < 2; i++)
        if (info.m_width[i] != 0)
        {
            tsz = (tsz << info.m_width[i]) |
                  Codec::getField(encoding, info.m_position[i], info.m_width[i]);
            tszBits += info.m_width[i];
        }
    return true;
}

bool decodeShiftImmediate(uint scheme, uint32 encoding, Operand& operand)
{
    const ShiftLayout& layout = gShiftLayouts[scheme - ARM_SVE_IMM_SHIFT_PRED_RIGHT];
    uint size;
    if (!decodeSize(layout.m_tszScheme, encoding, size))
        return false;
    uint tsz, tszBits;
    getShiftTsz(layout.m_tszScheme, encoding, tsz, tszBits);
    uint imm3 = Codec::getField(encoding, layout.m_immPosition, 3);
    uint combined = (tsz << 3) | imm3;
    uint esize = 8u << size;
    int64 shift;
    if (layout.m_isLeft)
        shift = (int64)combined - (int64)esize;
    else
        shift = (int64)(2 * esize) - (int64)combined;
    operand.m_kind = Operand::KIND_IMMEDIATE;
    operand.m_immediate = shift;
    operand.m_isHex = true;
    return true;
}

bool encodeShiftImmediate(uint scheme, const Operand& operand, uint32& encoding)
{
    const ShiftLayout& layout = gShiftLayouts[scheme - ARM_SVE_IMM_SHIFT_PRED_RIGHT];
    if ((operand.m_kind != Operand::KIND_IMMEDIATE) || (operand.m_shift != Operand::SHIFT_NONE))
        return false;
    // The element size was set by the register operand (the tsz holds only
    // the leading one)
    uint size;
    if (!decodeSize(layout.m_tszScheme, encoding, size))
        return false;
    uint esize = 8u << size;
    int64 shift = operand.m_immediate;
    int64 combined;
    if (layout.m_isLeft)
    {
        if ((shift < 0) || (shift >= (int64)esize))
            return false;
        combined = shift + (int64)esize;
    } else
    {
        if ((shift < 1) || (shift > (int64)esize))
            return false;
        combined = 2 * (int64)esize - shift;
    }
    const SizeScheme& info = gSizeSchemes[layout.m_tszScheme];
    uint tszBits = info.m_width[0] + info.m_width[1];
    if ((combined >> 3) >= (1 << tszBits))
        return false;
    uint tsz = (uint)(combined >> 3);
    // Set the tsz fields: the first is the most significant
    uint rest = tsz;
    for (int i = 1; i >= 0; i--)
        if (info.m_width[i] != 0)
        {
            Codec::setField(encoding, info.m_position[i], info.m_width[i],
                            rest & ((1u << info.m_width[i]) - 1));
            rest >>= info.m_width[i];
        }
    Codec::setField(encoding, layout.m_immPosition, 3, (uint32)(combined & 7));
    // The element size must come back
    uint check;
    return decodeSize(layout.m_tszScheme, encoding, check) && (check == size);
}

// Return the text value of the complex rotations
const int gRotations[] = { 0, 90, 180, 270 };

bool decodeImmediate(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    operand.m_kind = Operand::KIND_IMMEDIATE;
    operand.m_isHex = true;
    switch (spec.m_extra)
    {
    case ARM_SVE_IMM_ADDSUB:
    case ARM_SVE_IMM_DUP:
    {
        // imm8 with the optional lsl #8, printed as the shifted value, or
        // "#0x0, lsl #8" for a zero. The dup / cpy immediate is signed and
        // printed as the unsigned number of the element size (#0xffffff9e)
        bool isDup = (spec.m_extra == ARM_SVE_IMM_DUP);
        int64 value = isDup ? signExtend(Codec::getField(encoding, 5, 8), 8) :
                              (int64)Codec::getField(encoding, 5, 8);
        if (Codec::getField(encoding, 13, 1) != 0)
        {
            // lsl #8 doesn't exist for the byte elements
            if (Codec::getField(encoding, 22, 2) == 0)
                return false;
            if (value == 0)
            {
                operand.m_shift = Operand::SHIFT_LSL;
                operand.m_shiftAmount = 8;
            } else
            {
                value *= 256;
            }
        }
        if (isDup)
        {
            uint bits = 8u << Codec::getField(encoding, 22, 2);
            if (bits < 64)
                value &= (((int64)1 << bits) - 1);
            operand.m_isUnsigned = true;
        }
        operand.m_immediate = value;
        return true;
    }
    case ARM_SVE_IMM_SIGNED8:
        operand.m_immediate = signExtend(Codec::getField(encoding, 5, 8), 8);
        return true;
    case ARM_SVE_IMM_UNSIGNED8:
        operand.m_immediate = Codec::getField(encoding, 5, 8);
        return true;
    case ARM_SVE_IMM_SIGNED5_16:
        operand.m_immediate = signExtend(Codec::getField(encoding, 16, 5), 5);
        return true;
    case ARM_SVE_IMM_UNSIGNED7_14:
        operand.m_immediate = Codec::getField(encoding, 14, 7);
        return true;
    case ARM_SVE_IMM_INDEX5:
        operand.m_immediate = signExtend(Codec::getField(encoding, 5, 5), 5);
        return true;
    case ARM_SVE_IMM_INDEX5_16:
        operand.m_immediate = signExtend(Codec::getField(encoding, 16, 5), 5);
        return true;
    case ARM_SVE_IMM_SHIFT_PRED_RIGHT:
    case ARM_SVE_IMM_SHIFT_PRED_LEFT:
    case ARM_SVE_IMM_SHIFT_RIGHT:
    case ARM_SVE_IMM_SHIFT_LEFT:
    case ARM_SVE_IMM_SHIFT_NARROW:
    case ARM_SVE_IMM_SHIFT_WIDEN:
        return decodeShiftImmediate(spec.m_extra, encoding, operand);
    case ARM_SVE_IMM_FP_HALF_ONE:
    case ARM_SVE_IMM_FP_HALF_TWO:
    case ARM_SVE_IMM_FP_ZERO_ONE:
    {
        static const double values[3][2] = { { 0.5, 1.0 }, { 0.5, 2.0 }, { 0.0, 1.0 } };
        operand.m_kind = Operand::KIND_FLOAT_IMMEDIATE;
        operand.m_floatImmediate = values[spec.m_extra - ARM_SVE_IMM_FP_HALF_ONE]
                                         [Codec::getField(encoding, 5, 1)];
        operand.m_isExplicit = true;
        return true;
    }
    case ARM_SVE_IMM_FP_ZERO:
        operand.m_kind = Operand::KIND_FLOAT_IMMEDIATE;
        operand.m_floatImmediate = 0.0;
        operand.m_isExplicit = true;
        return true;
    case ARM_SVE_IMM_FP8:
        operand.m_kind = Operand::KIND_FLOAT_IMMEDIATE;
        operand.m_floatImmediate = expandFloat8(Codec::getField(encoding, 5, 8));
        return true;
    case ARM_SVE_IMM_ROT_90_270_16:
        operand.m_isHex = false;
        operand.m_immediate = Codec::getField(encoding, 16, 1) ? 270 : 90;
        return true;
    case ARM_SVE_IMM_ROT_90_270_10:
        operand.m_isHex = false;
        operand.m_immediate = Codec::getField(encoding, 10, 1) ? 270 : 90;
        return true;
    case ARM_SVE_IMM_ROT_13:
        operand.m_isHex = false;
        operand.m_immediate = gRotations[Codec::getField(encoding, 13, 2)];
        return true;
    case ARM_SVE_IMM_ROT_10:
        operand.m_isHex = false;
        operand.m_immediate = gRotations[Codec::getField(encoding, 10, 2)];
        return true;
    case ARM_SVE_IMM_EXT:
        operand.m_immediate = (Codec::getField(encoding, 16, 5) << 3) |
                              Codec::getField(encoding, 10, 3);
        return true;
    case ARM_SVE_IMM_EXTQ:
        operand.m_immediate = Codec::getField(encoding, 16, 4);
        return true;
    case ARM_SVE_IMM_SHIFT_MULTI:
        operand.m_immediate = 16 - (int64)Codec::getField(encoding, 16, 4);
        return true;
    case ARM_SVE_IMM_SHIFT_MULTI32:
        operand.m_immediate = 32 - (int64)Codec::getField(encoding, 16, 5);
        return true;
    case ARM_SVE_IMM_SHIFT_MULTI64:
    {
        uint field = Codec::getField(encoding, 16, 7);
        if ((field & 0x20) == 0)
            return false;
        operand.m_immediate = ((field & 0x40) != 0) ? (int64)(0x80 - field) : (int64)(0x60 - field);
        return true;
    }
    case ARM_SVE_IMM_MUL:
        operand.m_immediate = Codec::getField(encoding, 16, 4) + 1;
        operand.m_shift = Operand::SHIFT_MUL;
        return true;
    case ARM_SVE_IMM_SIMM6:
        operand.m_immediate = signExtend(Codec::getField(encoding, 5, 6), 6);
        return true;
    case ARM_SVE_IMM_UIMM3_16:
        operand.m_immediate = Codec::getField(encoding, 16, 3);
        return true;
    case ARM_SVE_IMM_LOGICAL:
    {
        uint64 value;
        uint size = getLogicalImmediateSize(encoding, value);
        if (size == SIZE_INVALID)
            return false;
        uint bits = 8u << size;
        uint64 mask = (bits == 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
        uint64 element = value & mask;
        // Printed as the unsigned number of the element size (#0xfffffc7f)
        operand.m_immediate = (int64)element;
        operand.m_isUnsigned = true;
        return true;
    }
    default:
        return false;
    }
}

// True if the operand is an integer immediate without a shift
bool isPlainImmediate(const Operand& operand)
{
    return (operand.m_kind == Operand::KIND_IMMEDIATE) &&
           (operand.m_shift == Operand::SHIFT_NONE);
}

bool encodeImmediate(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    int64 value = operand.m_immediate;
    switch (spec.m_extra)
    {
    case ARM_SVE_IMM_ADDSUB:
    case ARM_SVE_IMM_DUP:
    {
        if (operand.m_kind != Operand::KIND_IMMEDIATE)
            return false;
        bool isDup = (spec.m_extra == ARM_SVE_IMM_DUP);
        int64 low = isDup ? -128 : 0;
        int64 high = isDup ? 127 : 255;
        uint shift = 0;
        if (isDup)
        {
            // The number is the one of the element size (0xffffff9e or -98)
            uint bits = 8u << Codec::getField(encoding, 22, 2);
            if (bits < 64)
            {
                int64 limit = (int64)1 << bits;
                if ((value >= limit) || (value < -(limit >> 1)))
                    return false;
                value = signExtend((uint64)value & (uint64)(limit - 1), bits);
            }
        }
        if (operand.m_shift == Operand::SHIFT_LSL)
        {
            // "#1, lsl #8" (the value is the 8 bit number) or "#0, lsl #8"
            if (operand.m_shiftAmount == 8)
                shift = 1;
            else if (operand.m_shiftAmount != 0)
                return false;
        } else if (operand.m_shift != Operand::SHIFT_NONE)
            return false;
        if ((shift == 0) && ((value < low) || (value > high)) && ((value & 255) == 0))
        {
            // The shifted value: "#0x1100" is #0x11, lsl #8
            value >>= 8;
            shift = 1;
        }
        if ((value < low) || (value > high))
            return false;
        // The byte elements have no shift
        if ((shift != 0) && (Codec::getField(encoding, 22, 2) == 0))
            return false;
        Codec::setField(encoding, 5, 8, (uint32)value);
        Codec::setField(encoding, 13, 1, shift);
        return true;
    }
    case ARM_SVE_IMM_SIGNED8:
        if (!isPlainImmediate(operand) || (value < -128) || (value > 127))
            return false;
        Codec::setField(encoding, 5, 8, (uint32)value);
        return true;
    case ARM_SVE_IMM_UNSIGNED8:
        if (!isPlainImmediate(operand) || (value < 0) || (value > 255))
            return false;
        Codec::setField(encoding, 5, 8, (uint32)value);
        return true;
    case ARM_SVE_IMM_SIGNED5_16:
    case ARM_SVE_IMM_INDEX5_16:
        if (!isPlainImmediate(operand) || (value < -16) || (value > 15))
            return false;
        Codec::setField(encoding, 16, 5, (uint32)value);
        return true;
    case ARM_SVE_IMM_INDEX5:
        if (!isPlainImmediate(operand) || (value < -16) || (value > 15))
            return false;
        Codec::setField(encoding, 5, 5, (uint32)value);
        return true;
    case ARM_SVE_IMM_UNSIGNED7_14:
        if (!isPlainImmediate(operand) || (value < 0) || (value > 127))
            return false;
        Codec::setField(encoding, 14, 7, (uint32)value);
        return true;
    case ARM_SVE_IMM_SHIFT_PRED_RIGHT:
    case ARM_SVE_IMM_SHIFT_PRED_LEFT:
    case ARM_SVE_IMM_SHIFT_RIGHT:
    case ARM_SVE_IMM_SHIFT_LEFT:
    case ARM_SVE_IMM_SHIFT_NARROW:
    case ARM_SVE_IMM_SHIFT_WIDEN:
        return encodeShiftImmediate(spec.m_extra, operand, encoding);
    case ARM_SVE_IMM_FP_HALF_ONE:
    case ARM_SVE_IMM_FP_HALF_TWO:
    case ARM_SVE_IMM_FP_ZERO_ONE:
    {
        static const double values[3][2] = { { 0.5, 1.0 }, { 0.5, 2.0 }, { 0.0, 1.0 } };
        if (operand.m_kind != Operand::KIND_FLOAT_IMMEDIATE)
            return false;
        const double* pair = values[spec.m_extra - ARM_SVE_IMM_FP_HALF_ONE];
        if (operand.m_floatImmediate == pair[0])
            Codec::setField(encoding, 5, 1, 0);
        else if (operand.m_floatImmediate == pair[1])
            Codec::setField(encoding, 5, 1, 1);
        else
            return false;
        return true;
    }
    case ARM_SVE_IMM_FP_ZERO:
        return (operand.m_kind == Operand::KIND_FLOAT_IMMEDIATE) &&
               (operand.m_floatImmediate == 0.0);
    case ARM_SVE_IMM_FP8:
    {
        uint imm8;
        if ((operand.m_kind != Operand::KIND_FLOAT_IMMEDIATE) ||
            !findFloat8(operand.m_floatImmediate, imm8))
            return false;
        Codec::setField(encoding, 5, 8, imm8);
        return true;
    }
    case ARM_SVE_IMM_ROT_90_270_16:
    case ARM_SVE_IMM_ROT_90_270_10:
    {
        if (!isPlainImmediate(operand) || ((value != 90) && (value != 270)))
            return false;
        Codec::setField(encoding, (spec.m_extra == ARM_SVE_IMM_ROT_90_270_16) ? 16 : 10, 1,
                        (value == 270) ? 1 : 0);
        return true;
    }
    case ARM_SVE_IMM_ROT_13:
    case ARM_SVE_IMM_ROT_10:
    {
        if (!isPlainImmediate(operand))
            return false;
        for (uint i = 0; i < 4; i++)
            if (value == gRotations[i])
            {
                Codec::setField(encoding, (spec.m_extra == ARM_SVE_IMM_ROT_13) ? 13 : 10, 2, i);
                return true;
            }
        return false;
    }
    case ARM_SVE_IMM_EXT:
        if (!isPlainImmediate(operand) || (value < 0) || (value > 255))
            return false;
        Codec::setField(encoding, 16, 5, (uint32)(value >> 3));
        Codec::setField(encoding, 10, 3, (uint32)(value & 7));
        return true;
    case ARM_SVE_IMM_EXTQ:
        if (!isPlainImmediate(operand) || (value < 0) || (value > 15))
            return false;
        Codec::setField(encoding, 16, 4, (uint32)value);
        return true;
    case ARM_SVE_IMM_SHIFT_MULTI:
        if (!isPlainImmediate(operand) || (value < 1) || (value > 16))
            return false;
        Codec::setField(encoding, 16, 4, (uint32)(16 - value));
        return true;
    case ARM_SVE_IMM_SHIFT_MULTI32:
        if (!isPlainImmediate(operand) || (value < 1) || (value > 32))
            return false;
        Codec::setField(encoding, 16, 5, (uint32)(32 - value));
        return true;
    case ARM_SVE_IMM_SHIFT_MULTI64:
        if (!isPlainImmediate(operand) || (value < 1) || (value > 64))
            return false;
        Codec::setField(encoding, 16, 7, (value <= 32) ? (uint32)(0x80 - value) : (uint32)(0x60 - value));
        return true;
    case ARM_SVE_IMM_MUL:
        if ((operand.m_kind != Operand::KIND_IMMEDIATE) ||
            (operand.m_shift != Operand::SHIFT_MUL) || (value < 1) || (value > 16))
            return false;
        Codec::setField(encoding, 16, 4, (uint32)(value - 1));
        return true;
    case ARM_SVE_IMM_SIMM6:
        if (!isPlainImmediate(operand) || (value < -32) || (value > 31))
            return false;
        Codec::setField(encoding, 5, 6, (uint32)value);
        return true;
    case ARM_SVE_IMM_UIMM3_16:
        if (!isPlainImmediate(operand) || (value < 0) || (value > 7))
            return false;
        Codec::setField(encoding, 16, 3, (uint32)value);
        return true;
    case ARM_SVE_IMM_LOGICAL:
    {
        if (!isPlainImmediate(operand))
            return false;
        // The element size which the register operand wrote at bits 17-16
        uint size = Codec::getField(encoding, 16, 2);
        uint bits = 8u << size;
        uint64 mask = (bits == 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
        uint64 element = (uint64)value;
        // The value is the signed number or the unsigned element
        if (bits < 64)
        {
            if ((value < 0) ? (value < -(int64)(mask >> 1) - 1) : ((uint64)value > mask))
                return false;
            element &= mask;
        }
        uint64 replicated = element;
        for (uint filled = bits; filled < 64; filled *= 2)
            replicated |= replicated << filled;
        if ((replicated == 0) || (replicated == ~(uint64)0))
            return false;
        // Find N:immr:imms for the 64 bit pattern
        uint elementSize = 64;
        while (elementSize > 2)
        {
            uint half = elementSize / 2;
            uint64 halfMask = ((uint64)1 << half) - 1;
            if ((replicated & halfMask) != ((replicated >> half) & halfMask))
                break;
            elementSize = half;
        }
        uint64 elementMask = (elementSize == 64) ? ~(uint64)0 : (((uint64)1 << elementSize) - 1);
        uint64 pattern = replicated & elementMask;
        uint ones = 0;
        for (uint i = 0; i < elementSize; i++)
            if (((pattern >> i) & 1) != 0)
                ones++;
        uint64 run = (ones == 64) ? ~(uint64)0 : (((uint64)1 << ones) - 1);
        for (uint rotation = 0; rotation < elementSize; rotation++)
        {
            uint64 rotated = (rotation == 0) ? run :
                (((run >> rotation) | (run << (elementSize - rotation))) & elementMask);
            if (rotated == pattern)
            {
                uint n = (elementSize == 64) ? 1 : 0;
                uint imms = ((~(2 * elementSize - 1)) & 0x3f) | (ones - 1);
                Codec::setField(encoding, 17, 1, n);
                Codec::setField(encoding, 11, 6, rotation);
                Codec::setField(encoding, 5, 6, imms);
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// The options

const char* const gPrefetchNames[16] = {
    "pldl1keep", "pldl1strm", "pldl2keep", "pldl2strm", "pldl3keep", "pldl3strm",
    NULL, NULL,
    "pstl1keep", "pstl1strm", "pstl2keep", "pstl2strm", "pstl3keep", "pstl3strm",
    NULL, NULL
};

const char* const gPatternNames[32] = {
    "pow2", "vl1", "vl2", "vl3", "vl4", "vl5", "vl6", "vl7", "vl8", "vl16", "vl32",
    "vl64", "vl128", "vl256", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, "mul4", "mul3", "all"
};

const char* const gVlxNames[2] = { "vlx2", "vlx4" };

// The names and the width of the field of an SVE option scheme
bool getOptionNames(uint scheme, const char* const*& names, uint& count, uint& width)
{
    switch (scheme)
    {
    case ARM_OPT_SVE_PATTERN:
        names = gPatternNames;
        count = 32;
        width = 5;
        return true;
    case ARM_OPT_SVE_PREFETCH:
        names = gPrefetchNames;
        count = 16;
        width = 4;
        return true;
    case ARM_OPT_SVE_VLX:
        names = gVlxNames;
        count = 2;
        width = 1;
        return true;
    default:
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// The addressing

// A vector length multiple of the memory operand
bool isMulVlOperand(const Operand& operand)
{
    return operand.m_shift == Operand::SHIFT_MUL_VL;
}

// The base register of a memory operand: x or sp
bool getBaseNumber(const Operand& operand, uint& number)
{
    bool isStackPointer;
    if (!classifyX(operand.m_base, number, isStackPointer))
        return false;
    if ((number == 31) && !isStackPointer)
        return false;
    return true;
}

bool decodeMemory(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    operand.m_kind = Operand::KIND_MEMORY;
    uint baseNumber = Codec::getField(encoding, spec.m_position, 5);
    uint scale = spec.m_flags;
    switch (spec.m_extra)
    {
    case ARM_SVE_ADDR_MUL_VL4:
    case ARM_SVE_ADDR_MUL_VL9:
    {
        int64 offset;
        if (spec.m_extra == ARM_SVE_ADDR_MUL_VL4)
            offset = signExtend(Codec::getField(encoding, 16, 4), 4) * (int64)scale;
        else
            offset = signExtend((Codec::getField(encoding, 16, 6) << 3) |
                                Codec::getField(encoding, 10, 3), 9);
        operand.m_base = getXId(baseNumber, true);
        operand.m_offset = offset;
        if (offset != 0)
            operand.m_shift = Operand::SHIFT_MUL_VL;
        return true;
    }
    case ARM_SVE_ADDR_MUL_VL6:
        operand.m_base = getXId(baseNumber, true);
        operand.m_offset = signExtend(Codec::getField(encoding, 16, 6), 6);
        if (operand.m_offset != 0)
            operand.m_shift = Operand::SHIFT_MUL_VL;
        return true;
    case ARM_SVE_ADDR_SCALAR_SCALAR:
    {
        uint index = Codec::getField(encoding, 16, 5);
        operand.m_base = getXId(baseNumber, true);
        // Rm = 31 is not a valid offset register of these forms
        if (index == 31)
            return false;
        operand.m_index = getXId(index, false);
        if (scale != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = scale;
            operand.m_isExplicit = true;
        }
        return true;
    }
    case ARM_SVE_ADDR_SCALAR_SCALAR_ZR:
    {
        // The multi vector accesses: Rm = 31 is xzr
        uint index = Codec::getField(encoding, 16, 5);
        operand.m_base = getXId(baseNumber, true);
        operand.m_index = getXId(index, false);
        if (scale != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = scale;
            operand.m_isExplicit = true;
        }
        return true;
    }
    case ARM_SVE_ADDR_SCALAR_SCALAR_OPTIONAL:
    {
        // The first fault loads: Rm = 31 is the form without an index
        uint index = Codec::getField(encoding, 16, 5);
        operand.m_base = getXId(baseNumber, true);
        if (index == 31)
            return true;
        operand.m_index = getXId(index, false);
        if (scale != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = scale;
            operand.m_isExplicit = true;
        }
        return true;
    }
    case ARM_SVE_ADDR_IMM6:
        operand.m_base = getXId(baseNumber, true);
        operand.m_offset = (int64)Codec::getField(encoding, 16, 6) << scale;
        return true;
    case ARM_SVE_ADDR_SIMM4_SCALED:
        operand.m_base = getXId(baseNumber, true);
        operand.m_offset = signExtend(Codec::getField(encoding, 16, 4), 4) * ((int64)1 << scale);
        return true;
    case ARM_SVE_ADDR_ZM_S_EXT:
    case ARM_SVE_ADDR_ZM_D_EXT:
    case ARM_SVE_ADDR_ZM_S_EXT14:
    case ARM_SVE_ADDR_ZM_D_EXT14:
    {
        bool isWord = (spec.m_extra == ARM_SVE_ADDR_ZM_S_EXT) ||
                      (spec.m_extra == ARM_SVE_ADDR_ZM_S_EXT14);
        uint xsPosition = ((spec.m_extra == ARM_SVE_ADDR_ZM_S_EXT14) ||
                           (spec.m_extra == ARM_SVE_ADDR_ZM_D_EXT14)) ? 14 : 22;
        operand.m_base = getXId(baseNumber, true);
        operand.m_index = ARM64_REG_Z0 + Codec::getField(encoding, 16, 5);
        operand.m_indexArrangement = isWord ? Operand::ARRANGEMENT_S : Operand::ARRANGEMENT_D;
        operand.m_shift = (Codec::getField(encoding, xsPosition, 1) != 0) ? Operand::EXTEND_SXTW :
                                                                            Operand::EXTEND_UXTW;
        operand.m_shiftAmount = scale;
        operand.m_isExplicit = (scale != 0);
        return true;
    }
    case ARM_SVE_ADDR_ZM_D_LSL:
        operand.m_base = getXId(baseNumber, true);
        operand.m_index = ARM64_REG_Z0 + Codec::getField(encoding, 16, 5);
        operand.m_indexArrangement = Operand::ARRANGEMENT_D;
        if (scale != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = scale;
            operand.m_isExplicit = true;
        }
        return true;
    case ARM_SVE_ADDR_ZN_S_IMM5:
    case ARM_SVE_ADDR_ZN_D_IMM5:
        operand.m_base = ARM64_REG_Z0 + baseNumber;
        operand.m_indexArrangement = (spec.m_extra == ARM_SVE_ADDR_ZN_S_IMM5) ?
            Operand::ARRANGEMENT_S : Operand::ARRANGEMENT_D;
        operand.m_offset = (int64)Codec::getField(encoding, 16, 5) << scale;
        return true;
    case ARM_SVE_ADDR_ADR_S:
    case ARM_SVE_ADDR_ADR_D:
    case ARM_SVE_ADDR_ADR_SXTW:
    case ARM_SVE_ADDR_ADR_UXTW:
    {
        uint amount = Codec::getField(encoding, 10, 2);
        operand.m_base = ARM64_REG_Z0 + baseNumber;
        operand.m_index = ARM64_REG_Z0 + Codec::getField(encoding, 16, 5);
        operand.m_indexArrangement = (spec.m_extra == ARM_SVE_ADDR_ADR_S) ?
            Operand::ARRANGEMENT_S : Operand::ARRANGEMENT_D;
        if (spec.m_extra == ARM_SVE_ADDR_ADR_SXTW)
            operand.m_shift = Operand::EXTEND_SXTW;
        else if (spec.m_extra == ARM_SVE_ADDR_ADR_UXTW)
            operand.m_shift = Operand::EXTEND_UXTW;
        else if (amount != 0)
            operand.m_shift = Operand::SHIFT_LSL;
        if (operand.m_shift != Operand::SHIFT_NONE)
        {
            operand.m_shiftAmount = amount;
            operand.m_isExplicit = (amount != 0);
        }
        return true;
    }
    case ARM_SVE_ADDR_ZN_D_SCALAR:
    case ARM_SVE_ADDR_ZN_S_SCALAR:
    {
        uint index = Codec::getField(encoding, 16, 5);
        operand.m_base = ARM64_REG_Z0 + baseNumber;
        operand.m_indexArrangement = (spec.m_extra == ARM_SVE_ADDR_ZN_S_SCALAR) ?
            Operand::ARRANGEMENT_S : Operand::ARRANGEMENT_D;
        if (index != 31)
            operand.m_index = getXId(index, false);
        return true;
    }
    default:
        return false;
    }
}

bool encodeMemory(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_MEMORY) ||
        (operand.m_addressing != Operand::ADDRESSING_OFFSET) ||
        (operand.m_postIndexRegister != (uint)ARM_NO_REGISTER))
        return false;
    uint scale = spec.m_flags;
    uint number;
    switch (spec.m_extra)
    {
    case ARM_SVE_ADDR_MUL_VL4:
    case ARM_SVE_ADDR_MUL_VL9:
    case ARM_SVE_ADDR_MUL_VL6:
    {
        if (!getBaseNumber(operand, number) || (operand.m_index != (uint)ARM_NO_REGISTER))
            return false;
        // The offset is written with "mul vl" (or it is 0)
        if ((operand.m_shift != Operand::SHIFT_NONE) && !isMulVlOperand(operand))
            return false;
        if ((operand.m_shift == Operand::SHIFT_NONE) && (operand.m_offset != 0))
            return false;
        int64 offset = operand.m_offset;
        if (spec.m_extra == ARM_SVE_ADDR_MUL_VL4)
        {
            if ((offset % (int64)scale) != 0)
                return false;
            offset /= (int64)scale;
            if ((offset < -8) || (offset > 7))
                return false;
            Codec::setField(encoding, 16, 4, (uint32)offset);
        } else if (spec.m_extra == ARM_SVE_ADDR_MUL_VL6)
        {
            if ((offset < -32) || (offset > 31))
                return false;
            Codec::setField(encoding, 16, 6, (uint32)offset);
        } else
        {
            if ((offset < -256) || (offset > 255))
                return false;
            Codec::setField(encoding, 16, 6, (uint32)(offset >> 3));
            Codec::setField(encoding, 10, 3, (uint32)(offset & 7));
        }
        Codec::setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case ARM_SVE_ADDR_SCALAR_SCALAR:
    case ARM_SVE_ADDR_SCALAR_SCALAR_ZR:
    case ARM_SVE_ADDR_SCALAR_SCALAR_OPTIONAL:
    {
        if (!getBaseNumber(operand, number) || (operand.m_offset != 0))
            return false;
        uint index = 31;
        if (operand.m_index != (uint)ARM_NO_REGISTER)
        {
            bool isStackPointer;
            if (!classifyX(operand.m_index, index, isStackPointer) || isStackPointer)
                return false;
            if ((index == 31) && (spec.m_extra != ARM_SVE_ADDR_SCALAR_SCALAR_ZR))
                return false;
            if (scale != 0)
            {
                if ((operand.m_shift != Operand::SHIFT_LSL) || (operand.m_shiftAmount != scale))
                    return false;
            } else if (operand.m_shift != Operand::SHIFT_NONE)
                return false;
        } else
        {
            if (spec.m_extra != ARM_SVE_ADDR_SCALAR_SCALAR_OPTIONAL)
                return false;
            if (operand.m_shift != Operand::SHIFT_NONE)
                return false;
        }
        Codec::setField(encoding, 16, 5, index);
        Codec::setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case ARM_SVE_ADDR_IMM6:
    case ARM_SVE_ADDR_SIMM4_SCALED:
    {
        if (!getBaseNumber(operand, number) || (operand.m_index != (uint)ARM_NO_REGISTER) ||
            (operand.m_shift != Operand::SHIFT_NONE))
            return false;
        int64 offset = operand.m_offset;
        if ((offset & (((int64)1 << scale) - 1)) != 0)
            return false;
        offset >>= scale;
        if (spec.m_extra == ARM_SVE_ADDR_IMM6)
        {
            if ((offset < 0) || (offset > 63))
                return false;
            Codec::setField(encoding, 16, 6, (uint32)offset);
        } else
        {
            if ((offset < -8) || (offset > 7))
                return false;
            Codec::setField(encoding, 16, 4, (uint32)offset);
        }
        Codec::setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case ARM_SVE_ADDR_ZM_S_EXT:
    case ARM_SVE_ADDR_ZM_D_EXT:
    case ARM_SVE_ADDR_ZM_S_EXT14:
    case ARM_SVE_ADDR_ZM_D_EXT14:
    {
        if (!getBaseNumber(operand, number) || !isZRegister(operand.m_index) ||
            (operand.m_offset != 0))
            return false;
        bool isWord = (spec.m_extra == ARM_SVE_ADDR_ZM_S_EXT) ||
                      (spec.m_extra == ARM_SVE_ADDR_ZM_S_EXT14);
        uint xsPosition = ((spec.m_extra == ARM_SVE_ADDR_ZM_S_EXT14) ||
                           (spec.m_extra == ARM_SVE_ADDR_ZM_D_EXT14)) ? 14 : 22;
        uint wanted = isWord ? Operand::ARRANGEMENT_S : Operand::ARRANGEMENT_D;
        if (operand.m_indexArrangement != wanted)
            return false;
        uint xs;
        if (operand.m_shift == Operand::EXTEND_UXTW)
            xs = 0;
        else if (operand.m_shift == Operand::EXTEND_SXTW)
            xs = 1;
        else
            return false;
        if (operand.m_shiftAmount != scale)
            return false;
        // The amount is written only when it is not 0 (it is mandatory then)
        if ((scale != 0) && !operand.m_isExplicit)
            return false;
        Codec::setField(encoding, xsPosition, 1, xs);
        Codec::setField(encoding, 16, 5, operand.m_index - ARM64_REG_Z0);
        Codec::setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case ARM_SVE_ADDR_ZM_D_LSL:
    {
        if (!getBaseNumber(operand, number) || !isZRegister(operand.m_index) ||
            (operand.m_offset != 0) || (operand.m_indexArrangement != Operand::ARRANGEMENT_D))
            return false;
        if (scale != 0)
        {
            if ((operand.m_shift != Operand::SHIFT_LSL) || (operand.m_shiftAmount != scale))
                return false;
        } else if (operand.m_shift != Operand::SHIFT_NONE)
            return false;
        Codec::setField(encoding, 16, 5, operand.m_index - ARM64_REG_Z0);
        Codec::setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case ARM_SVE_ADDR_ZN_S_IMM5:
    case ARM_SVE_ADDR_ZN_D_IMM5:
    {
        if (!isZRegister(operand.m_base) || (operand.m_index != (uint)ARM_NO_REGISTER) ||
            (operand.m_shift != Operand::SHIFT_NONE))
            return false;
        uint wanted = (spec.m_extra == ARM_SVE_ADDR_ZN_S_IMM5) ? Operand::ARRANGEMENT_S :
                                                                  Operand::ARRANGEMENT_D;
        if (operand.m_indexArrangement != wanted)
            return false;
        int64 offset = operand.m_offset;
        if ((offset & (((int64)1 << scale) - 1)) != 0)
            return false;
        offset >>= scale;
        if ((offset < 0) || (offset > 31))
            return false;
        Codec::setField(encoding, 16, 5, (uint32)offset);
        Codec::setField(encoding, spec.m_position, 5, operand.m_base - ARM64_REG_Z0);
        return true;
    }
    case ARM_SVE_ADDR_ADR_S:
    case ARM_SVE_ADDR_ADR_D:
    case ARM_SVE_ADDR_ADR_SXTW:
    case ARM_SVE_ADDR_ADR_UXTW:
    {
        if (!isZRegister(operand.m_base) || !isZRegister(operand.m_index) ||
            (operand.m_offset != 0))
            return false;
        uint wanted = (spec.m_extra == ARM_SVE_ADDR_ADR_S) ? Operand::ARRANGEMENT_S :
                                                              Operand::ARRANGEMENT_D;
        if ((operand.m_indexArrangement != wanted) ||
            ((operand.m_shift != Operand::SHIFT_NONE) && (operand.m_shiftAmount > 3)))
            return false;
        uint amount = operand.m_shiftAmount;
        if (spec.m_extra == ARM_SVE_ADDR_ADR_SXTW)
        {
            if (operand.m_shift != Operand::EXTEND_SXTW)
                return false;
        } else if (spec.m_extra == ARM_SVE_ADDR_ADR_UXTW)
        {
            if (operand.m_shift != Operand::EXTEND_UXTW)
                return false;
        } else if (operand.m_shift == Operand::SHIFT_LSL)
        {
            // lsl #0 is not written
            if (amount == 0)
                return false;
        } else if (operand.m_shift != Operand::SHIFT_NONE)
            return false;
        else
            amount = 0;
        // The amount is written when it is not 0
        if ((operand.m_shift != Operand::SHIFT_NONE) && ((amount != 0) != operand.m_isExplicit))
            return false;
        Codec::setField(encoding, 10, 2, amount);
        Codec::setField(encoding, 16, 5, operand.m_index - ARM64_REG_Z0);
        Codec::setField(encoding, spec.m_position, 5, operand.m_base - ARM64_REG_Z0);
        return true;
    }
    case ARM_SVE_ADDR_ZN_D_SCALAR:
    case ARM_SVE_ADDR_ZN_S_SCALAR:
    {
        if (!isZRegister(operand.m_base) ||
            (operand.m_indexArrangement != ((spec.m_extra == ARM_SVE_ADDR_ZN_S_SCALAR) ?
                                            Operand::ARRANGEMENT_S : Operand::ARRANGEMENT_D)) ||
            (operand.m_shift != Operand::SHIFT_NONE) || (operand.m_offset != 0))
            return false;
        uint index = 31;
        if (operand.m_index != (uint)ARM_NO_REGISTER)
        {
            bool isStackPointer;
            if (!classifyX(operand.m_index, index, isStackPointer) || isStackPointer ||
                (index == 31))
                return false;
        }
        Codec::setField(encoding, 16, 5, index);
        Codec::setField(encoding, spec.m_position, 5, operand.m_base - ARM64_REG_Z0);
        return true;
    }
    default:
        return false;
    }
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////
// The names of the options

const char* getSveOptionName(uint scheme, uint value)
{
    const char* const* names;
    uint count, width;
    if (!getOptionNames(scheme, names, count, width) || (value >= count))
        return NULL;
    return names[value];
}

bool findSveOption(uint scheme, const cString& name, uint& value)
{
    const char* const* names;
    uint count, width;
    if (!getOptionNames(scheme, names, count, width))
        return false;
    cString text(name);
    text.makeLower();
    for (uint i = 0; i < count; i++)
        if ((names[i] != NULL) && (text == names[i]))
        {
            value = i;
            return true;
        }
    return false;
}

//////////////////////////////////////////////////////////////////////////
// decode / encode

bool isSveSpec(const ArmOperandSpec& spec)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_SVE_Z:
    case ARM_KIND_SVE_Z_INDEXED:
    case ARM_KIND_SVE_Z_LIST:
    case ARM_KIND_SVE_PREDICATE:
    case ARM_KIND_SVE_PREDICATE_COUNTER:
    case ARM_KIND_SVE_FFR:
    case ARM_KIND_SVE_V128:
    case ARM_KIND_SVE_PREDICATE_INDEXED:
    case ARM_KIND_SVE_GPR:
    case ARM_KIND_SVE_TIED:
    case ARM_KIND_SVE_IMMEDIATE:
    case ARM_KIND_SVE_MEMORY:
        return true;
    case ARM_KIND_OPTION:
        return spec.m_extra >= ARM_OPT_SVE_FIRST;
    default:
        return false;
    }
}

bool decodeSve(const ArmOperandSpec& spec,
               uint32 encoding,
               const ArmCodecContext& context,
               ArmDecodedOperand& operand)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_SVE_Z:
    {
        uint arrangement;
        if (!decodeArrangement(spec.m_extra, encoding, arrangement))
            return false;
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = ARM64_REG_Z0 + Codec::getField(encoding, spec.m_position, 5);
        operand.m_arrangement = arrangement;
        return true;
    }
    case ARM_KIND_SVE_Z_INDEXED:
    {
        uint size = spec.m_flags & 7;
        uint index;
        uint number;
        bool hasIndex = true;
        switch (spec.m_extra)
        {
        case ARM_SVE_IDX_DUP:
        {
            uint tsz = Codec::getField(encoding, 16, 5);
            if (tsz == 0)
                return false;
            size = 0;
            while (((tsz >> size) & 1) == 0)
                size++;
            uint all = (Codec::getField(encoding, 22, 2) << 5) | tsz;
            index = all >> (size + 1);
            number = Codec::getField(encoding, spec.m_position, 5);
            break;
        }
        case ARM_SVE_IDX_I3H:
            index = (Codec::getField(encoding, 22, 1) << 2) | Codec::getField(encoding, 19, 2);
            number = Codec::getField(encoding, spec.m_position, 3);
            break;
        case ARM_SVE_IDX_I2:
            index = Codec::getField(encoding, 19, 2);
            number = Codec::getField(encoding, spec.m_position, 3);
            break;
        case ARM_SVE_IDX_I1:
            index = Codec::getField(encoding, 20, 1);
            number = Codec::getField(encoding, spec.m_position, 4);
            break;
        case ARM_SVE_IDX_WIDE_I3:
            index = (Codec::getField(encoding, 19, 2) << 1) | Codec::getField(encoding, 11, 1);
            number = Codec::getField(encoding, spec.m_position, 3);
            break;
        case ARM_SVE_IDX_WIDE_I2:
            index = (Codec::getField(encoding, 20, 1) << 1) | Codec::getField(encoding, 11, 1);
            number = Codec::getField(encoding, spec.m_position, 4);
            break;
        case ARM_SVE_IDX_I4:
            index = (Codec::getField(encoding, 19, 2) << 2) | Codec::getField(encoding, 10, 2);
            number = Codec::getField(encoding, spec.m_position, 3);
            break;
        case ARM_SVE_IDX_DUPQ:
        {
            uint tsz = Codec::getField(encoding, 16, 5);
            if (tsz == 0)
                return false;
            size = 0;
            while (((tsz >> size) & 1) == 0)
                size++;
            // dupq has no quadword element
            if (size > SIZE_D)
                return false;
            index = tsz >> (size + 1);
            number = Codec::getField(encoding, spec.m_position, 5);
            break;
        }
        case ARM_SVE_IDX_AES:
            index = Codec::getField(encoding, 19, 2);
            number = Codec::getField(encoding, spec.m_position, 5);
            break;
        case ARM_SVE_IDX_LUT_2:
            index = Codec::getField(encoding, 22, 2);
            number = Codec::getField(encoding, spec.m_position, 5);
            break;
        case ARM_SVE_IDX_LUT_1:
            index = Codec::getField(encoding, 23, 1);
            number = Codec::getField(encoding, spec.m_position, 5);
            break;
        case ARM_SVE_IDX_LUT_3:
            index = (Codec::getField(encoding, 22, 2) << 1) | Codec::getField(encoding, 12, 1);
            number = Codec::getField(encoding, spec.m_position, 5);
            break;
        case ARM_SVE_IDX_PMOV:
        {
            // tsz = 23-22 : 18-17: the highest set bit is the size, the bits
            // under it the index (the bytes have none)
            uint tsz = (Codec::getField(encoding, 22, 2) << 2) | Codec::getField(encoding, 17, 2);
            if (tsz == 0)
                return false;
            size = 3;
            while (((tsz >> size) & 1) == 0)
                size--;
            index = tsz & ((1u << size) - 1);
            hasIndex = (size != 0);
            number = Codec::getField(encoding, spec.m_position, 5);
            break;
        }
        default:
            return false;
        }
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = ARM64_REG_Z0 + number;
        {
            uint arrangementSize = ((spec.m_extra == ARM_SVE_IDX_DUP) || (spec.m_extra == ARM_SVE_IDX_DUPQ)) ?
                size : (spec.m_flags & 7);
            operand.m_arrangement = (arrangementSize <= SIZE_NONE) ?
                gArrangementOfSize[arrangementSize] : Operand::ARRANGEMENT_NONE;
        }
        operand.m_elementIndex = hasIndex ? (int)index : -1;
        return true;
    }
    case ARM_KIND_SVE_Z_LIST:
    {
        uint count = spec.m_flags & 7;
        uint log2Multiple = (spec.m_flags >> 4) & 3;
        uint arrangement;
        if (!decodeArrangement(spec.m_extra, encoding, arrangement))
            return false;
        uint first;
        uint stride = 1;
        if ((spec.m_flags & ARM_SPEC_LIST_STRIDED) != 0)
        {
            // The strided list of SME2 { z0.s, z8.s }, { z1.s, z5.s, z9.s, z13.s }:
            // the first register is bit m_position + 4 and the low bits at
            // m_position (3 bits for 2 registers, 2 for 4), the stride 16 / count
            uint lowBits = (count == 2) ? 3 : 2;
            first = (Codec::getField(encoding, spec.m_position + 4, 1) << 4) |
                    Codec::getField(encoding, spec.m_position, lowBits);
            stride = 16 / count;
        } else
        {
            first = Codec::getField(encoding, spec.m_position, 5 - log2Multiple) << log2Multiple;
        }
        operand.m_kind = Operand::KIND_REGISTER_LIST;
        operand.m_register = ARM64_REG_Z0 + first;
        operand.m_count = count;
        operand.m_stride = stride;
        operand.m_arrangement = arrangement;
        return true;
    }
    case ARM_KIND_SVE_PREDICATE:
    case ARM_KIND_SVE_PREDICATE_COUNTER:
    {
        uint arrangement;
        if (!decodeArrangement(spec.m_extra, encoding, arrangement))
            return false;
        uint qualifier;
        if (!getQualifier(spec.m_flags, encoding, qualifier))
            return false;
        uint number;
        if ((spec.m_kind == ARM_KIND_SVE_PREDICATE) && ((spec.m_flags & ARM_SVE_PRED_PAIR) != 0))
        {
            // { p2.s, p3.s }: the even register of a pair (3 bits), or two
            // consecutive registers (4 bits)
            bool isEven = (spec.m_flags & ARM_SVE_PRED_3BIT) != 0;
            number = Codec::getField(encoding, spec.m_position, isEven ? 3 : 4) << (isEven ? 1 : 0);
            operand.m_kind = Operand::KIND_REGISTER_LIST;
            operand.m_register = ARM64_REG_P0 + number;
            operand.m_count = 2;
            operand.m_stride = 1;
            operand.m_arrangement = arrangement;
            return true;
        }
        if (spec.m_kind == ARM_KIND_SVE_PREDICATE)
        {
            number = Codec::getField(encoding, spec.m_position,
                                     ((spec.m_flags & ARM_SVE_PRED_3BIT) != 0) ? 3 : 4);
        } else
        {
            bool isHigh = (spec.m_flags & ARM_SVE_PRED_HIGH) != 0;
            number = Codec::getField(encoding, spec.m_position, isHigh ? 3 : 4) + (isHigh ? 8 : 0);
            operand.m_isPredicateCounter = true;
        }
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = ARM64_REG_P0 + number;
        operand.m_arrangement = arrangement;
        operand.m_predicateQualifier = qualifier;
        return true;
    }
    case ARM_KIND_SVE_FFR:
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = ARM64_REG_FFR;
        return true;
    case ARM_KIND_SVE_PREDICATE_INDEXED:
    {
        operand.m_kind = Operand::KIND_REGISTER;
        switch (spec.m_extra)
        {
        case ARM_SVE_PIDX_PSEL:
        {
            // tsz = 23-22 : 20-18, the lowest set bit is the size
            uint tsz = (Codec::getField(encoding, 22, 2) << 3) | Codec::getField(encoding, 18, 3);
            if (tsz == 0)
                return false;
            uint size = 0;
            while (((tsz >> size) & 1) == 0)
                size++;
            if (size > SIZE_D)
                return false;
            operand.m_register = ARM64_REG_P0 + Codec::getField(encoding, spec.m_position, 4);
            operand.m_arrangement = gArrangementOfSize[size];
            operand.m_sliceRegister = ARM64_REG_W0 + 12 + Codec::getField(encoding, 16, 2);
            operand.m_sliceOffset = tsz >> (size + 1);
            return true;
        }
        case ARM_SVE_PIDX_PEXT_1:
        case ARM_SVE_PIDX_PEXT_2:
            operand.m_register = ARM64_REG_P0 + 8 + Codec::getField(encoding, spec.m_position, 3);
            operand.m_isPredicateCounter = true;
            operand.m_elementIndex = (spec.m_extra == ARM_SVE_PIDX_PEXT_1) ?
                (int)Codec::getField(encoding, 8, 2) : (int)Codec::getField(encoding, 8, 1);
            return true;
        default:
            return false;
        }
    }
    case ARM_KIND_SVE_V128:
    {
        uint size;
        if (!decodeSize(spec.m_extra, encoding, size) || (size > SIZE_D))
            return false;
        static const uint arrangements[] = {
            Operand::ARRANGEMENT_16B, Operand::ARRANGEMENT_8H,
            Operand::ARRANGEMENT_4S, Operand::ARRANGEMENT_2D
        };
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = ARM64_REG_V0 + Codec::getField(encoding, spec.m_position, 5);
        operand.m_arrangement = arrangements[size];
        return true;
    }
    case ARM_KIND_SVE_GPR:
    {
        uint size;
        if (!decodeSize(spec.m_extra, encoding, size))
            return false;
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = getGprId(Codec::getField(encoding, spec.m_position, 5),
                                      size == SIZE_D, (spec.m_flags & 1) != 0);
        return true;
    }
    case ARM_KIND_SVE_IMMEDIATE:
        return decodeImmediate(spec, encoding, operand);
    case ARM_KIND_SVE_MEMORY:
        return decodeMemory(spec, encoding, operand);
    case ARM_KIND_OPTION:
    {
        const char* const* names;
        uint count, width;
        if (!getOptionNames(spec.m_extra, names, count, width))
            return false;
        operand.m_kind = Operand::KIND_OPTION;
        operand.m_optionScheme = spec.m_extra;
        operand.m_option = Codec::getField(encoding, spec.m_position, width);
        return true;
    }
    default:
        return false;
    }
}

bool encodeSve(const ArmOperandSpec& spec,
               const ArmDecodedOperand& operand,
               const ArmCodecContext& context,
               uint32& encoding)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_SVE_Z:
    {
        if (!isPlainRegister(operand) || !isZRegister(operand.m_register))
            return false;
        if (!encodeArrangement(spec.m_extra, operand.m_arrangement, encoding))
            return false;
        Codec::setField(encoding, spec.m_position, 5, operand.m_register - ARM64_REG_Z0);
        return true;
    }
    case ARM_KIND_SVE_Z_INDEXED:
    {
        if ((operand.m_kind != Operand::KIND_REGISTER) || !isZRegister(operand.m_register) ||
            (operand.m_predicateQualifier != Operand::PREDICATE_NONE) ||
            operand.m_isPredicateCounter)
            return false;
        uint size;
        if (!getSizeOfArrangement(operand.m_arrangement, size))
            return false;
        // The bytes of pmov have no index
        if (operand.m_elementIndex < 0)
        {
            if ((spec.m_extra != ARM_SVE_IDX_PMOV) || (size != SIZE_NONE))
                return false;
            Codec::setField(encoding, spec.m_position, 5, operand.m_register - ARM64_REG_Z0);
            return true;
        }
        uint index = (uint)operand.m_elementIndex;
        uint number = operand.m_register - ARM64_REG_Z0;
        switch (spec.m_extra)
        {
        case ARM_SVE_IDX_DUP:
        {
            if (size > SIZE_Q)
                return false;
            if (index >= (1u << (6 - size)))
                return false;
            uint all = (index << (size + 1)) | (1u << size);
            Codec::setField(encoding, 22, 2, all >> 5);
            Codec::setField(encoding, 16, 5, all & 31);
            Codec::setField(encoding, spec.m_position, 5, number);
            return true;
        }
        case ARM_SVE_IDX_I3H:
            if ((size != (spec.m_flags & 7)) || (index > 7) || (number > 7))
                return false;
            Codec::setField(encoding, 22, 1, index >> 2);
            Codec::setField(encoding, 19, 2, index & 3);
            Codec::setField(encoding, spec.m_position, 3, number);
            return true;
        case ARM_SVE_IDX_I2:
            if ((size != (spec.m_flags & 7)) || (index > 3) || (number > 7))
                return false;
            Codec::setField(encoding, 19, 2, index);
            Codec::setField(encoding, spec.m_position, 3, number);
            return true;
        case ARM_SVE_IDX_I1:
            if ((size != (spec.m_flags & 7)) || (index > 1) || (number > 15))
                return false;
            Codec::setField(encoding, 20, 1, index);
            Codec::setField(encoding, spec.m_position, 4, number);
            return true;
        case ARM_SVE_IDX_WIDE_I3:
            if ((size != (spec.m_flags & 7)) || (index > 7) || (number > 7))
                return false;
            Codec::setField(encoding, 19, 2, index >> 1);
            Codec::setField(encoding, 11, 1, index & 1);
            Codec::setField(encoding, spec.m_position, 3, number);
            return true;
        case ARM_SVE_IDX_WIDE_I2:
            if ((size != (spec.m_flags & 7)) || (index > 3) || (number > 15))
                return false;
            Codec::setField(encoding, 20, 1, index >> 1);
            Codec::setField(encoding, 11, 1, index & 1);
            Codec::setField(encoding, spec.m_position, 4, number);
            return true;
        case ARM_SVE_IDX_I4:
            if ((size != (spec.m_flags & 7)) || (index > 15) || (number > 7))
                return false;
            Codec::setField(encoding, 19, 2, index >> 2);
            Codec::setField(encoding, 10, 2, index & 3);
            Codec::setField(encoding, spec.m_position, 3, number);
            return true;
        case ARM_SVE_IDX_DUPQ:
            if ((size > SIZE_D) || (index >= (1u << (4 - size))))
                return false;
            Codec::setField(encoding, 16, 5, (index << (size + 1)) | (1u << size));
            Codec::setField(encoding, spec.m_position, 5, number);
            return true;
        case ARM_SVE_IDX_AES:
            if ((size != (spec.m_flags & 7)) || (index > 3))
                return false;
            Codec::setField(encoding, 19, 2, index);
            Codec::setField(encoding, spec.m_position, 5, number);
            return true;
        case ARM_SVE_IDX_LUT_2:
        case ARM_SVE_IDX_LUT_1:
        case ARM_SVE_IDX_LUT_3:
        {
            if (size != (spec.m_flags & 7))
                return false;
            uint limit = (spec.m_extra == ARM_SVE_IDX_LUT_1) ? 2 :
                         (spec.m_extra == ARM_SVE_IDX_LUT_2) ? 4 : 8;
            if (index >= limit)
                return false;
            if (spec.m_extra == ARM_SVE_IDX_LUT_1)
                Codec::setField(encoding, 23, 1, index);
            else if (spec.m_extra == ARM_SVE_IDX_LUT_2)
                Codec::setField(encoding, 22, 2, index);
            else
            {
                Codec::setField(encoding, 22, 2, index >> 1);
                Codec::setField(encoding, 12, 1, index & 1);
            }
            Codec::setField(encoding, spec.m_position, 5, number);
            return true;
        }
        case ARM_SVE_IDX_PMOV:
        {
            // The leading one of tsz (the size) is written by the predicate
            // operand, which may come after this one: the index is or-ed into
            // the bits below it
            if ((size != SIZE_NONE) || (index > 7))
                return false;
            uint tsz = ((Codec::getField(encoding, 22, 2) << 2) | Codec::getField(encoding, 17, 2)) |
                       index;
            Codec::setField(encoding, 22, 2, tsz >> 2);
            Codec::setField(encoding, 17, 2, tsz & 3);
            Codec::setField(encoding, spec.m_position, 5, number);
            return true;
        }
        default:
            return false;
        }
    }
    case ARM_KIND_SVE_Z_LIST:
    {
        uint count = spec.m_flags & 7;
        uint log2Multiple = (spec.m_flags >> 4) & 3;
        bool isStrided = (spec.m_flags & ARM_SPEC_LIST_STRIDED) != 0;
        if ((operand.m_kind != Operand::KIND_REGISTER_LIST) || (operand.m_count != count) ||
            !isZRegister(operand.m_register) || (operand.m_elementIndex >= 0) ||
            ((operand.m_stride > 1) != isStrided))
            return false;
        uint first = operand.m_register - ARM64_REG_Z0;
        if (isStrided)
        {
            uint stride = 16 / count;
            if ((operand.m_stride != stride) || ((first & 15) >= stride) ||
                !encodeArrangement(spec.m_extra, operand.m_arrangement, encoding))
                return false;
            Codec::setField(encoding, spec.m_position + 4, 1, first >> 4);
            Codec::setField(encoding, spec.m_position, (count == 2) ? 3 : 2, first & (stride - 1));
            return true;
        }
        if ((first & ((1u << log2Multiple) - 1)) != 0)
            return false;
        if (!encodeArrangement(spec.m_extra, operand.m_arrangement, encoding))
            return false;
        Codec::setField(encoding, spec.m_position, 5 - log2Multiple, first >> log2Multiple);
        return true;
    }
    case ARM_KIND_SVE_PREDICATE:
    case ARM_KIND_SVE_PREDICATE_COUNTER:
    {
        bool isCounter = (spec.m_kind == ARM_KIND_SVE_PREDICATE_COUNTER);
        if ((spec.m_kind == ARM_KIND_SVE_PREDICATE) && ((spec.m_flags & ARM_SVE_PRED_PAIR) != 0))
        {
            // { p2.s, p3.s }: the first one is even
            if ((operand.m_kind != Operand::KIND_REGISTER_LIST) || (operand.m_count != 2) ||
                !isPRegister(operand.m_register) || (operand.m_stride > 1) ||
                operand.m_isPredicateCounter || (operand.m_elementIndex >= 0))
                return false;
            uint first = operand.m_register - ARM64_REG_P0;
            bool isEven = (spec.m_flags & ARM_SVE_PRED_3BIT) != 0;
            if ((isEven && ((first & 1) != 0)) ||
                !encodeArrangement(spec.m_extra, operand.m_arrangement, encoding))
                return false;
            Codec::setField(encoding, spec.m_position, isEven ? 3 : 4, isEven ? (first >> 1) : first);
            return true;
        }
        if ((operand.m_kind != Operand::KIND_REGISTER) || !isPRegister(operand.m_register) ||
            (operand.m_elementIndex >= 0) || (operand.m_isPredicateCounter != isCounter) ||
            (operand.m_addressing != Operand::ADDRESSING_OFFSET) ||
            (operand.m_sliceRegister != (uint)ARM_NO_REGISTER))
            return false;
        if (!encodeQualifier(spec.m_flags, operand.m_predicateQualifier, encoding))
            return false;
        if (!encodeArrangement(spec.m_extra, operand.m_arrangement, encoding))
            return false;
        uint number = operand.m_register - ARM64_REG_P0;
        if (!isCounter)
        {
            uint width = ((spec.m_flags & ARM_SVE_PRED_3BIT) != 0) ? 3 : 4;
            if (number >= (1u << width))
                return false;
            Codec::setField(encoding, spec.m_position, width, number);
        } else
        {
            bool isHigh = (spec.m_flags & ARM_SVE_PRED_HIGH) != 0;
            if (isHigh)
            {
                if (number < 8)
                    return false;
                number -= 8;
            }
            Codec::setField(encoding, spec.m_position, isHigh ? 3 : 4, number);
        }
        return true;
    }
    case ARM_KIND_SVE_FFR:
        return isPlainRegister(operand) && (operand.m_register == ARM64_REG_FFR);
    case ARM_KIND_SVE_PREDICATE_INDEXED:
    {
        if ((operand.m_kind != Operand::KIND_REGISTER) || !isPRegister(operand.m_register))
            return false;
        uint number = operand.m_register - ARM64_REG_P0;
        switch (spec.m_extra)
        {
        case ARM_SVE_PIDX_PSEL:
        {
            uint size;
            if (!getSizeOfArrangement(operand.m_arrangement, size) || (size > SIZE_D) ||
                (operand.m_sliceRegister < ARM64_REG_W0 + 12) ||
                (operand.m_sliceRegister > ARM64_REG_W0 + 15) ||
                (operand.m_sliceOffset >= (1u << (4 - size))) || operand.m_isPredicateCounter)
                return false;
            uint tsz = (operand.m_sliceOffset << (size + 1)) | (1u << size);
            Codec::setField(encoding, 22, 2, tsz >> 3);
            Codec::setField(encoding, 18, 3, tsz & 7);
            Codec::setField(encoding, 16, 2, operand.m_sliceRegister - (ARM64_REG_W0 + 12));
            Codec::setField(encoding, spec.m_position, 4, number);
            return true;
        }
        case ARM_SVE_PIDX_PEXT_1:
        case ARM_SVE_PIDX_PEXT_2:
        {
            uint limit = (spec.m_extra == ARM_SVE_PIDX_PEXT_1) ? 4 : 2;
            if (!operand.m_isPredicateCounter || (number < 8) || (operand.m_elementIndex < 0) ||
                ((uint)operand.m_elementIndex >= limit) || (operand.m_arrangement != Operand::ARRANGEMENT_NONE))
                return false;
            Codec::setField(encoding, 8, (limit == 4) ? 2 : 1, (uint)operand.m_elementIndex);
            Codec::setField(encoding, spec.m_position, 3, number - 8);
            return true;
        }
        default:
            return false;
        }
    }
    case ARM_KIND_SVE_V128:
    {
        if (!isPlainRegister(operand) ||
            (operand.m_register < ARM64_REG_V0) || (operand.m_register > ARM64_REG_V31))
            return false;
        uint size;
        switch (operand.m_arrangement)
        {
        case Operand::ARRANGEMENT_16B: size = SIZE_B; break;
        case Operand::ARRANGEMENT_8H: size = SIZE_H; break;
        case Operand::ARRANGEMENT_4S: size = SIZE_S; break;
        case Operand::ARRANGEMENT_2D: size = SIZE_D; break;
        default: return false;
        }
        if (!encodeSize(spec.m_extra, size, encoding))
            return false;
        Codec::setField(encoding, spec.m_position, 5, operand.m_register - ARM64_REG_V0);
        return true;
    }
    case ARM_KIND_SVE_TIED:
        // The copy of a register field
        Codec::setField(encoding, spec.m_position, spec.m_flags,
                        Codec::getField(encoding, spec.m_extra, spec.m_flags));
        return true;
    case ARM_KIND_SVE_GPR:
    {
        // The register class is checked by the comparison of the text with the
        // decoded bits: the size field may be set by an operand after this one
        if (!isPlainRegister(operand))
            return false;
        bool is64, isStackPointer;
        uint number;
        if (!classifyGpr(operand.m_register, is64, number, isStackPointer))
            return false;
        if (isStackPointer != ((spec.m_flags & 1) != 0) && (number == 31))
            return false;
        Codec::setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case ARM_KIND_SVE_IMMEDIATE:
        return encodeImmediate(spec, operand, encoding);
    case ARM_KIND_SVE_MEMORY:
        return encodeMemory(spec, operand, encoding);
    case ARM_KIND_OPTION:
    {
        const char* const* names;
        uint count, width;
        if (!getOptionNames(spec.m_extra, names, count, width))
            return false;
        uint value;
        if (operand.m_kind == Operand::KIND_NAME)
        {
            if (!findSveOption(spec.m_extra, operand.m_name, value))
                return false;
        } else if (operand.m_kind == Operand::KIND_OPTION)
        {
            if (operand.m_optionScheme != spec.m_extra)
                return false;
            value = operand.m_option;
        } else if (isPlainImmediate(operand) && operand.m_hasHash &&
                   (operand.m_immediate >= 0) && (operand.m_immediate < (int64)count))
        {
            // "#0x6": a value which has no name
            value = (uint)operand.m_immediate;
            if (names[value] != NULL)
                return false;
        } else
        {
            return false;
        }
        Codec::setField(encoding, spec.m_position, width, value);
        return true;
    }
    default:
        return false;
    }
}

} // namespace armdis
