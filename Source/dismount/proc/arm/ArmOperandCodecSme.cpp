#include "dismount/dismount.h"
/*
 * ArmOperandCodecSme.cpp
 *
 * Implementation file. The SME kinds of ArmOperandCodec: the za tiles and
 * tile slices, the za array vectors of SME2 (za.s[w8, 0, vgx2]), the tile
 * list of zero, zt0, the z registers and lists with an element index, and
 * the memory operands of ldr / str za and zt0, see ArmEncodingTable.h and
 * ArmOperandCodec.h.
 *
 * The operands are KIND_MATRIX (the tiles, the slices, the arrays and the
 * list: ArmOpcode.h says which fields each one has), KIND_REGISTER with
 * ARM64_REG_ZT0 (zt0, zt0[1], zt0[1, mul vl]), and the SVE operands.
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

// The element sizes: b h s d q, and none (ARM_SVE_NONE ... ARM_SVE_Q)
const uint gArrangements[] = {
    Operand::ARRANGEMENT_NONE, Operand::ARRANGEMENT_B, Operand::ARRANGEMENT_H,
    Operand::ARRANGEMENT_S, Operand::ARRANGEMENT_D, Operand::ARRANGEMENT_Q
};

// The index in gArrangements of an arrangement. False if it isn't a size
bool findSize(uint arrangement, uint& size)
{
    for (uint i = 0; i < sizeof(gArrangements) / sizeof(gArrangements[0]); i++)
        if (gArrangements[i] == arrangement)
        {
            size = i;
            return true;
        }
    return false;
}

// The number of bits of the tile number of an element size (1-5: b h s d q)
uint getTileBits(uint size)
{
    return (size == 0) ? 0 : (size - 1);
}

uint getXId(uint number, bool isStackPointer)
{
    if (number == 31)
        return isStackPointer ? ARM64_REG_SP : ARM64_REG_XZR;
    return ARM64_REG_X0 + number;
}

// The number of an x register or sp as the base of a memory operand
bool getBaseNumber(uint id, uint& number)
{
    if (id <= ARM64_REG_X30)
    {
        number = id - ARM64_REG_X0;
        return true;
    }
    if (id == ARM64_REG_SP)
    {
        number = 31;
        return true;
    }
    return false;
}

// The element size of a slice from its spec and bits: 1-5 (b h s d q). False
// for a reserved combination
bool getSliceSize(const ArmOperandSpec& spec, uint32 encoding, uint& size)
{
    uint selector = spec.m_flags & ARM_SME_SLICE_SIZE_MASK;
    if (selector < ARM_SME_SLICE_SIZE_FIELD)
    {
        size = selector + 1;
        return true;
    }
    // The size field of mova: 23-22, and bit 16 for the quadword
    uint field = Codec::getField(encoding, 22, 2);
    bool isQuad = Codec::getField(encoding, 16, 1) != 0;
    if (isQuad)
    {
        if (field != 3)
            return false;
        size = 5;
        return true;
    }
    size = field + 1;
    return true;
}

uint getSliceCountLog2(const ArmOperandSpec& spec)
{
    return (spec.m_flags & ARM_SME_SLICE_COUNT_MASK) >> ARM_SME_SLICE_COUNT_SHIFT;
}

// The layout of the tile and offset field of a slice. size is 1-5
struct SliceLayout {
    uint m_tileBits;
    uint m_offsetBits;
    // The bits of the field (4 for one slice, 3 for two or four)
    uint m_regionBits;
    uint m_usedBits;
};

bool getSliceLayout(uint size, uint countLog2, SliceLayout& layout)
{
    layout.m_tileBits = getTileBits(size);
    if ((countLog2 > 0) && (size == 5))
        return false;
    int offsetBits = 4 - (int)layout.m_tileBits - (int)countLog2;
    layout.m_offsetBits = (offsetBits < 0) ? 0 : offsetBits;
    layout.m_regionBits = (countLog2 == 0) ? 4 : 3;
    layout.m_usedBits = layout.m_tileBits + layout.m_offsetBits;
    return true;
}

} // anonymous namespace

bool isSmeSpec(const ArmOperandSpec& spec)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_SME_TILE:
    case ARM_KIND_SME_TILE_SLICE:
    case ARM_KIND_SME_ARRAY:
    case ARM_KIND_SME_TILE_LIST:
    case ARM_KIND_SME_ZT0:
    case ARM_KIND_SME_Z_INDEXED:
    case ARM_KIND_SME_MEMORY:
        return true;
    default:
        return false;
    }
}

bool decodeSme(const ArmOperandSpec& spec,
               uint32 encoding,
               const ArmCodecContext& context,
               ArmDecodedOperand& operand)
{
    (void)context;
    switch (spec.m_kind)
    {
    case ARM_KIND_SME_TILE:
    {
        uint tile = Codec::getField(encoding, spec.m_position, spec.m_flags);
        ASSERT(spec.m_extra < sizeof(gArrangements) / sizeof(gArrangements[0]));
        operand.m_kind = Operand::KIND_MATRIX;
        operand.m_register = ARM64_REG_ZA_ROW0 + tile;
        operand.m_tile = tile;
        operand.m_arrangement = gArrangements[spec.m_extra];
        return true;
    }
    case ARM_KIND_SME_TILE_SLICE:
    {
        uint size;
        if (!getSliceSize(spec, encoding, size))
            return false;
        uint countLog2 = getSliceCountLog2(spec);
        SliceLayout layout;
        if (!getSliceLayout(size, countLog2, layout))
            return false;
        uint field = Codec::getField(encoding, spec.m_position, layout.m_regionBits);
        if ((field >> layout.m_usedBits) != 0)
            return false;
        uint tile = field >> layout.m_offsetBits;
        uint offset = (field & ((1u << layout.m_offsetBits) - 1)) << countLog2;
        operand.m_kind = Operand::KIND_MATRIX;
        operand.m_register = ARM64_REG_ZA_ROW0 + tile;
        operand.m_tile = tile;
        operand.m_arrangement = gArrangements[size];
        operand.m_sliceDirection = (Codec::getField(encoding, 15, 1) != 0) ?
            Operand::SLICE_VERTICAL : Operand::SLICE_HORIZONTAL;
        operand.m_sliceRegister = ARM64_REG_W0 + 12 + Codec::getField(encoding, 13, 2);
        operand.m_sliceOffset = offset;
        operand.m_sliceOffsetLast = offset + (1u << countLog2) - 1;
        operand.m_isExplicit = (spec.m_flags & ARM_SME_SLICE_BRACES) != 0;
        return true;
    }
    case ARM_KIND_SME_ARRAY:
    {
        uint groupLog2 = spec.m_flags & ARM_SME_ARRAY_GROUP_MASK;
        uint rangeLog2 = (spec.m_flags & ARM_SME_ARRAY_RANGE_MASK) >> ARM_SME_ARRAY_RANGE_SHIFT;
        uint width = (spec.m_flags & ARM_SME_ARRAY_WIDTH_MASK) >> ARM_SME_ARRAY_WIDTH_SHIFT;
        if (groupLog2 > 2)
            return false;
        uint offset = Codec::getField(encoding, spec.m_position, width) << rangeLog2;
        operand.m_kind = Operand::KIND_MATRIX;
        operand.m_register = ARM64_REG_ZA_ROW0;
        operand.m_arrangement = gArrangements[spec.m_extra & 0x7f];
        operand.m_sliceRegister = ARM64_REG_W0 + (((spec.m_flags & ARM_SME_ARRAY_W12) != 0) ? 12 : 8) +
                                  Codec::getField(encoding, 13, 2);
        operand.m_sliceOffset = offset;
        operand.m_sliceOffsetLast = offset + (1u << rangeLog2) - 1;
        operand.m_vectorGroup = (groupLog2 == 0) ? 0 : (1u << groupLog2);
        return true;
    }
    case ARM_KIND_SME_TILE_LIST:
        operand.m_kind = Operand::KIND_MATRIX;
        operand.m_register = ARM64_REG_ZA_ROW0;
        operand.m_arrangement = Operand::ARRANGEMENT_D;
        operand.m_registerMask = Codec::getField(encoding, spec.m_position, 8);
        operand.m_isExplicit = true;
        return true;
    case ARM_KIND_SME_ZT0:
    {
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = ARM64_REG_ZT0;
        switch (spec.m_extra)
        {
        case ARM_SME_ZT0_PLAIN:
            return true;
        case ARM_SME_ZT0_BRACED:
            operand.m_isExplicit = true;
            return true;
        case ARM_SME_ZT0_INDEX:
            // (the byte offset of an eight byte register of the table)
            operand.m_elementIndex = (int)(Codec::getField(encoding, spec.m_position, 3) << 3);
            return true;
        case ARM_SME_ZT0_MUL_VL:
        {
            uint index = Codec::getField(encoding, spec.m_position, 2);
            if (index != 0)
            {
                operand.m_elementIndex = (int)index;
                operand.m_shift = Operand::SHIFT_MUL_VL;
            }
            return true;
        }
        default:
            return false;
        }
    }
    case ARM_KIND_SME_Z_INDEXED:
    {
        ASSERT(spec.m_extra < gArmSmeIndexLayoutsCount);
        const ArmSmeIndexLayout& layout = gArmSmeIndexLayouts[spec.m_extra];
        uint regWidth = spec.m_flags & 15;
        uint sizeCode = (spec.m_flags >> 4) & 7;
        uint field = Codec::getField(encoding, spec.m_position, regWidth);
        if (layout.m_splitBits != 0)
            field = (field & ((1u << layout.m_splitBits) - 1)) +
                    (field >> layout.m_splitBits) * layout.m_splitStep;
        uint number = (field << layout.m_log2Multiple) + layout.m_base;
        uint index = 0;
        for (uint i = 0; i < layout.m_count; i++)
            index = (index << layout.m_width[i]) |
                    Codec::getField(encoding, layout.m_position[i], layout.m_width[i]);
        operand.m_register = ARM64_REG_Z0 + number;
        if (layout.m_listCount != 0)
        {
            operand.m_kind = Operand::KIND_REGISTER_LIST;
            operand.m_count = layout.m_listCount;
            operand.m_stride = 1;
        } else
        {
            operand.m_kind = Operand::KIND_REGISTER;
        }
        operand.m_arrangement = (sizeCode == 7) ? Operand::ARRANGEMENT_NONE :
            gArrangements[sizeCode + 1];
        // (a layout without fields is a narrow register or list: z0-z15)
        operand.m_elementIndex = (layout.m_count == 0) ? -1 : (int)index;
        return true;
    }
    case ARM_KIND_SME_MEMORY:
    {
        uint base = Codec::getField(encoding, spec.m_position, 5);
        operand.m_kind = Operand::KIND_MEMORY;
        operand.m_base = getXId(base, true);
        if (spec.m_extra == ARM_SME_MEM_ZA)
        {
            operand.m_offset = Codec::getField(encoding, 0, 4);
            if (operand.m_offset != 0)
                operand.m_shift = Operand::SHIFT_MUL_VL;
        } else if (spec.m_extra == ARM_SME_MEM_SCALAR_OPTIONAL)
        {
            uint index = Codec::getField(encoding, 16, 5);
            if (index != 31)
            {
                operand.m_index = getXId(index, false);
                if (spec.m_flags != 0)
                {
                    operand.m_shift = Operand::SHIFT_LSL;
                    operand.m_shiftAmount = spec.m_flags;
                    operand.m_isExplicit = true;
                }
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool encodeSme(const ArmOperandSpec& spec,
               const ArmDecodedOperand& operand,
               const ArmCodecContext& context,
               uint32& encoding)
{
    (void)context;
    switch (spec.m_kind)
    {
    case ARM_KIND_SME_TILE:
    {
        if ((operand.m_kind != Operand::KIND_MATRIX) ||
            (operand.m_sliceDirection != Operand::SLICE_NONE) ||
            (operand.m_vectorGroup != 0) || operand.m_isExplicit ||
            (operand.m_sliceRegister != (uint)ARM_NO_REGISTER) ||
            (operand.m_arrangement != gArrangements[spec.m_extra]))
            return false;
        if ((operand.m_tile >> spec.m_flags) != 0)
            return false;
        Codec::setField(encoding, spec.m_position, spec.m_flags, operand.m_tile);
        return true;
    }
    case ARM_KIND_SME_TILE_SLICE:
    {
        if ((operand.m_kind != Operand::KIND_MATRIX) ||
            (operand.m_sliceDirection == Operand::SLICE_NONE) ||
            (operand.m_vectorGroup != 0) ||
            (operand.m_isExplicit != ((spec.m_flags & ARM_SME_SLICE_BRACES) != 0)))
            return false;
        uint size;
        if (!findSize(operand.m_arrangement, size) || (size == 0))
            return false;
        uint selector = spec.m_flags & ARM_SME_SLICE_SIZE_MASK;
        if (selector < ARM_SME_SLICE_SIZE_FIELD)
        {
            if (size != selector + 1)
                return false;
        } else
        {
            Codec::setField(encoding, 22, 2, (size == 5) ? 3 : (size - 1));
            Codec::setField(encoding, 16, 1, (size == 5) ? 1 : 0);
        }
        uint countLog2 = getSliceCountLog2(spec);
        SliceLayout layout;
        if (!getSliceLayout(size, countLog2, layout))
            return false;
        uint count = 1u << countLog2;
        if ((operand.m_tile >> layout.m_tileBits) != 0)
            return false;
        if (((operand.m_sliceOffset & (count - 1)) != 0) ||
            (operand.m_sliceOffsetLast != operand.m_sliceOffset + count - 1))
            return false;
        uint offsetField = operand.m_sliceOffset >> countLog2;
        if ((offsetField >> layout.m_offsetBits) != 0)
            return false;
        if ((operand.m_sliceRegister < (uint)ARM64_REG_W0 + 12) ||
            (operand.m_sliceRegister > (uint)ARM64_REG_W0 + 15))
            return false;
        Codec::setField(encoding, spec.m_position, layout.m_regionBits,
                        (operand.m_tile << layout.m_offsetBits) | offsetField);
        Codec::setField(encoding, 15, 1, (operand.m_sliceDirection == Operand::SLICE_VERTICAL) ? 1 : 0);
        Codec::setField(encoding, 13, 2, operand.m_sliceRegister - (ARM64_REG_W0 + 12));
        return true;
    }
    case ARM_KIND_SME_ARRAY:
    {
        uint groupLog2 = spec.m_flags & ARM_SME_ARRAY_GROUP_MASK;
        uint rangeLog2 = (spec.m_flags & ARM_SME_ARRAY_RANGE_MASK) >> ARM_SME_ARRAY_RANGE_SHIFT;
        uint width = (spec.m_flags & ARM_SME_ARRAY_WIDTH_MASK) >> ARM_SME_ARRAY_WIDTH_SHIFT;
        uint group = (groupLog2 == 0) ? 0u : (1u << groupLog2);
        if ((operand.m_kind != Operand::KIND_MATRIX) ||
            (operand.m_sliceDirection != Operand::SLICE_NONE) ||
            (operand.m_arrangement != gArrangements[spec.m_extra & 0x7f]) ||
            (operand.m_isExplicit))
            return false;
        // (the text without the group: the number of registers of the list says it)
        if ((operand.m_vectorGroup != group) &&
            !(((spec.m_extra & ARM_SME_ARRAY_OPTIONAL_GROUP) != 0) && (operand.m_vectorGroup == 0)))
            return false;
        uint count = 1u << rangeLog2;
        if (((operand.m_sliceOffset & (count - 1)) != 0) ||
            (operand.m_sliceOffsetLast != operand.m_sliceOffset + count - 1))
            return false;
        uint field = operand.m_sliceOffset >> rangeLog2;
        if ((field >> width) != 0)
            return false;
        uint first = ((spec.m_flags & ARM_SME_ARRAY_W12) != 0) ? 12 : 8;
        if ((operand.m_sliceRegister < (uint)ARM64_REG_W0 + first) ||
            (operand.m_sliceRegister > (uint)ARM64_REG_W0 + first + 3))
            return false;
        Codec::setField(encoding, spec.m_position, width, field);
        Codec::setField(encoding, 13, 2, operand.m_sliceRegister - (ARM64_REG_W0 + first));
        return true;
    }
    case ARM_KIND_SME_TILE_LIST:
        if ((operand.m_kind != Operand::KIND_MATRIX) || !operand.m_isExplicit ||
            (operand.m_sliceDirection != Operand::SLICE_NONE) ||
            (operand.m_registerMask > 0xff) || (operand.m_vectorGroup != 0))
            return false;
        Codec::setField(encoding, spec.m_position, 8, operand.m_registerMask);
        return true;
    case ARM_KIND_SME_ZT0:
    {
        if ((operand.m_kind != Operand::KIND_REGISTER) || (operand.m_register != ARM64_REG_ZT0) ||
            (operand.m_arrangement != Operand::ARRANGEMENT_NONE) ||
            (operand.m_predicateQualifier != Operand::PREDICATE_NONE))
            return false;
        switch (spec.m_extra)
        {
        case ARM_SME_ZT0_PLAIN:
            return (operand.m_elementIndex < 0) && (operand.m_shift == Operand::SHIFT_NONE) &&
                   !operand.m_isExplicit;
        case ARM_SME_ZT0_BRACED:
            return (operand.m_elementIndex < 0) && (operand.m_shift == Operand::SHIFT_NONE) &&
                   operand.m_isExplicit;
        case ARM_SME_ZT0_INDEX:
            if ((operand.m_elementIndex < 0) || (operand.m_elementIndex > 56) ||
                ((operand.m_elementIndex & 7) != 0) || (operand.m_shift != Operand::SHIFT_NONE))
                return false;
            Codec::setField(encoding, spec.m_position, 3, (uint32)operand.m_elementIndex >> 3);
            return true;
        case ARM_SME_ZT0_MUL_VL:
        {
            if (operand.m_elementIndex < 0)
            {
                if (operand.m_shift != Operand::SHIFT_NONE)
                    return false;
                Codec::setField(encoding, spec.m_position, 2, 0);
                return true;
            }
            // (zt0[0, mul vl] is not how LLVM writes the first one)
            if ((operand.m_elementIndex < 1) || (operand.m_elementIndex > 3) ||
                (operand.m_shift != Operand::SHIFT_MUL_VL))
                return false;
            Codec::setField(encoding, spec.m_position, 2, (uint32)operand.m_elementIndex);
            return true;
        }
        default:
            return false;
        }
    }
    case ARM_KIND_SME_Z_INDEXED:
    {
        ASSERT(spec.m_extra < gArmSmeIndexLayoutsCount);
        const ArmSmeIndexLayout& layout = gArmSmeIndexLayouts[spec.m_extra];
        uint regWidth = spec.m_flags & 15;
        uint sizeCode = (spec.m_flags >> 4) & 7;
        if ((operand.m_elementIndex < 0) != (layout.m_count == 0))
            return false;
        if (layout.m_listCount != 0)
        {
            if ((operand.m_kind != Operand::KIND_REGISTER_LIST) ||
                (operand.m_count != layout.m_listCount) || (operand.m_stride > 1))
                return false;
        } else if (operand.m_kind != Operand::KIND_REGISTER)
            return false;
        uint expected = (sizeCode == 7) ? (uint)Operand::ARRANGEMENT_NONE : gArrangements[sizeCode + 1];
        if ((operand.m_arrangement != expected) || (operand.m_predicateQualifier != Operand::PREDICATE_NONE) ||
            operand.m_isPredicateCounter)
            return false;
        if ((operand.m_register < (uint)ARM64_REG_Z0) || (operand.m_register > (uint)ARM64_REG_Z31))
            return false;
        uint number = operand.m_register - ARM64_REG_Z0;
        if (number < layout.m_base)
            return false;
        number -= layout.m_base;
        if ((number & ((1u << layout.m_log2Multiple) - 1)) != 0)
            return false;
        number >>= layout.m_log2Multiple;
        if (layout.m_splitBits != 0)
        {
            uint low = number % layout.m_splitStep;
            if (low >= (1u << layout.m_splitBits))
                return false;
            number = ((number / layout.m_splitStep) << layout.m_splitBits) | low;
        }
        if ((number >> regWidth) != 0)
            return false;
        uint totalBits = 0;
        for (uint i = 0; i < layout.m_count; i++)
            totalBits += layout.m_width[i];
        uint index = (layout.m_count == 0) ? 0 : (uint)operand.m_elementIndex;
        if ((index >> totalBits) != 0)
            return false;
        Codec::setField(encoding, spec.m_position, regWidth, number);
        for (int i = (int)layout.m_count - 1; i >= 0; i--)
        {
            Codec::setField(encoding, layout.m_position[i], layout.m_width[i],
                            index & ((1u << layout.m_width[i]) - 1));
            index >>= layout.m_width[i];
        }
        return true;
    }
    case ARM_KIND_SME_MEMORY:
    {
        if ((operand.m_kind != Operand::KIND_MEMORY) ||
            (operand.m_addressing != Operand::ADDRESSING_OFFSET) ||
            (operand.m_postIndexRegister != (uint)ARM_NO_REGISTER))
            return false;
        uint base;
        if (!getBaseNumber(operand.m_base, base))
            return false;
        if (spec.m_extra == ARM_SME_MEM_SCALAR_OPTIONAL)
        {
            uint index = 31;
            if (operand.m_offset != 0)
                return false;
            if (operand.m_index != (uint)ARM_NO_REGISTER)
            {
                // xzr is Rm = 31 too
                if (operand.m_index == ARM64_REG_XZR)
                    index = 31;
                else if (operand.m_index <= ARM64_REG_X30)
                    index = operand.m_index - ARM64_REG_X0;
                else
                    return false;
                if (spec.m_flags != 0)
                {
                    if ((operand.m_shift != Operand::SHIFT_LSL) || (operand.m_shiftAmount != spec.m_flags))
                        return false;
                } else if (operand.m_shift != Operand::SHIFT_NONE)
                    return false;
            } else if (operand.m_shift != Operand::SHIFT_NONE)
                return false;
            Codec::setField(encoding, 16, 5, index);
            Codec::setField(encoding, spec.m_position, 5, base);
            return true;
        }
        if (operand.m_index != (uint)ARM_NO_REGISTER)
            return false;
        if (spec.m_extra == ARM_SME_MEM_ZA)
        {
            // The offset of the memory is the vector select offset of the array operand
            // (which was encoded before): [x8] is the offset 0, [x8, #0, mul vl] too, and a
            // non zero offset is always "mul vl"
            if ((operand.m_offset < 0) || (operand.m_offset > 15))
                return false;
            if ((operand.m_offset != 0) && (operand.m_shift != Operand::SHIFT_MUL_VL))
                return false;
            if ((operand.m_shift != Operand::SHIFT_NONE) && (operand.m_shift != Operand::SHIFT_MUL_VL))
                return false;
            if (Codec::getField(encoding, 0, 4) != (uint32)operand.m_offset)
                return false;
        } else if ((operand.m_offset != 0) || (operand.m_shift != Operand::SHIFT_NONE))
            return false;
        Codec::setField(encoding, spec.m_position, 5, base);
        return true;
    }
    default:
        return false;
    }
}

} // namespace armdis
