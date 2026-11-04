#include "dismount/dismount.h"
/*
 * ArmOperandCodecA32Simd.cpp
 *
 * Implementation file. The floating point and Advanced SIMD kinds of the
 * AArch32 ArmOperandCodec: the s, d and q registers, the scalars, the lists
 * of registers, the memory operand of the structure loads and stores, the
 * immediates, and the data type of the mnemonics.
 *
 * The decoder follows the decoder of LLVM (ARMDisassembler.cpp): a value
 * which LLVM rejects (Fail) makes the decode return false, a value which LLVM
 * only soft fails is decoded.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"
#include "dismount/proc/arm/ArmOperandCodecA32.h"
#include "dismount/proc/arm/ArmOperandCodecA32Simd.h"

namespace armdis {

namespace {

typedef ArmDecodedOperand Operand;

uint field(uint32 encoding, uint position, uint width)
{
    return ArmOperandCodec::getField(encoding, position, width);
}

void setBits(uint32& encoding, uint position, uint width, uint value)
{
    ArmOperandCodec::setField(encoding, position, width, value);
}

// True if the d16-d31 registers exist (VFPv3 and up)
bool hasD32(const ArmCodecContext& context)
{
    return context.m_features.has(ArmFeatures::FEAT_VFP3);
}

//////////////////////////////////////////////////////////////////////////
// The registers

// Return the number of a register from its fields: the field of 'width' bits at
// 'position' and the top / low bit at m_extra
uint registerField(const ArmOperandSpec& spec, uint32 encoding, bool isSingle)
{
    uint width = ((spec.m_flags & ARM_A32_REG_BITS3) != 0) ? 3 : 4;
    uint value = field(encoding, spec.m_position, width);
    bool hasExtra = ((spec.m_flags & ARM_A32_REG_NO_TOP) == 0);
    uint extra = hasExtra ? field(encoding, spec.m_extra, 1) : 0;
    return isSingle ? ((value << 1) | extra) : ((extra << 4) | value);
}

bool encodeRegisterField(const ArmOperandSpec& spec, uint number, bool isSingle, uint32& encoding)
{
    uint width = ((spec.m_flags & ARM_A32_REG_BITS3) != 0) ? 3 : 4;
    bool hasExtra = ((spec.m_flags & ARM_A32_REG_NO_TOP) == 0);
    uint extra;
    uint value;
    if (isSingle)
    {
        value = number >> 1;
        extra = number & 1;
    } else
    {
        value = number & 15;
        extra = number >> 4;
    }
    if (value >= (1u << width))
        return false;
    if (!hasExtra && extra != 0)
        return false;
    setBits(encoding, spec.m_position, width, value);
    if (hasExtra)
        setBits(encoding, spec.m_extra, 1, extra);
    return true;
}

bool decodeRegister(const ArmOperandSpec& spec, uint32 encoding,
                    const ArmCodecContext& context, Operand& operand)
{
    operand.m_kind = Operand::KIND_REGISTER;
    switch (spec.m_kind)
    {
    case ARM_KIND_A32_SREG:
    {
        uint number = registerField(spec, encoding, true);
        if ((spec.m_flags & ARM_A32_REG_NEXT) != 0)
        {
            number++;
            if (number > 31)
                return false;
        }
        operand.m_register = ARM_REG_S0 + number;
        return true;
    }
    case ARM_KIND_A32_DREG:
    {
        uint number = registerField(spec, encoding, false);
        if ((number > 15) && !hasD32(context))
            return false;
        operand.m_register = ARM_REG_D0 + number;
        return true;
    }
    default:
    {
        // The q registers: the d number has to be even
        uint number = registerField(spec, encoding, false);
        if ((number & 1) != 0)
            return false;
        operand.m_register = ARM_REG_Q0 + number / 2;
        return true;
    }
    }
}

bool encodeRegister(const ArmOperandSpec& spec, const Operand& operand,
                    const ArmCodecContext& context, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_REGISTER) || (operand.m_elementIndex >= 0))
        return false;
    uint id = operand.m_register;
    switch (spec.m_kind)
    {
    case ARM_KIND_A32_SREG:
        if ((id < ARM_REG_S0) || (id > ARM_REG_S31))
            return false;
        if ((spec.m_flags & ARM_A32_REG_NEXT) != 0)
        {
            // (checked against the first register: the encoding has it already)
            return registerField(spec, encoding, true) + 1 == id - ARM_REG_S0;
        }
        return encodeRegisterField(spec, id - ARM_REG_S0, true, encoding);
    case ARM_KIND_A32_DREG:
        if ((id < ARM_REG_D0) || (id > ARM_REG_D31))
            return false;
        if ((id - ARM_REG_D0 > 15) && !hasD32(context))
            return false;
        return encodeRegisterField(spec, id - ARM_REG_D0, false, encoding);
    default:
        if ((id < ARM_REG_Q0) || (id > ARM_REG_Q15))
            return false;
        return encodeRegisterField(spec, (id - ARM_REG_Q0) * 2, false, encoding);
    }
}

//////////////////////////////////////////////////////////////////////////
// The scalars: d0[1]

bool getIndex(uint scheme, uint32 encoding, uint& index)
{
    switch (scheme)
    {
    case ARM_A32_INDEX_M16:  index = (field(encoding, 5, 1) << 1) | field(encoding, 3, 1); return true;
    case ARM_A32_INDEX_M32:  index = field(encoding, 5, 1); return true;
    case ARM_A32_INDEX_S32:  index = field(encoding, 3, 1); return true;
    case ARM_A32_INDEX_ZERO: index = 0; return true;
    case ARM_A32_INDEX_DUP8: index = field(encoding, 17, 3); return true;
    case ARM_A32_INDEX_DUP16: index = field(encoding, 18, 2); return true;
    case ARM_A32_INDEX_DUP32: index = field(encoding, 19, 1); return true;
    case ARM_A32_INDEX_MOV8:
        index = (field(encoding, 21, 1) << 2) | field(encoding, 5, 2);
        return true;
    case ARM_A32_INDEX_MOV16:
        index = (field(encoding, 21, 1) << 1) | field(encoding, 6, 1);
        return true;
    case ARM_A32_INDEX_MOV32: index = field(encoding, 21, 1); return true;
    default: return false;
    }
}

bool setIndex(uint scheme, uint index, uint32& encoding)
{
    switch (scheme)
    {
    case ARM_A32_INDEX_M16:
        if (index > 3) return false;
        setBits(encoding, 5, 1, index >> 1);
        setBits(encoding, 3, 1, index & 1);
        return true;
    case ARM_A32_INDEX_M32:
    case ARM_A32_INDEX_S32:
        if (index > 1) return false;
        setBits(encoding, (scheme == ARM_A32_INDEX_M32) ? 5 : 3, 1, index);
        return true;
    case ARM_A32_INDEX_ZERO:
        return index == 0;
    case ARM_A32_INDEX_DUP8:
        if (index > 7) return false;
        setBits(encoding, 17, 3, index);
        return true;
    case ARM_A32_INDEX_DUP16:
        if (index > 3) return false;
        setBits(encoding, 18, 2, index);
        return true;
    case ARM_A32_INDEX_DUP32:
        if (index > 1) return false;
        setBits(encoding, 19, 1, index);
        return true;
    case ARM_A32_INDEX_MOV8:
        if (index > 7) return false;
        setBits(encoding, 21, 1, index >> 2);
        setBits(encoding, 5, 2, index & 3);
        return true;
    case ARM_A32_INDEX_MOV16:
        if (index > 3) return false;
        setBits(encoding, 21, 1, index >> 1);
        setBits(encoding, 6, 1, index & 1);
        return true;
    case ARM_A32_INDEX_MOV32:
        if (index > 1) return false;
        setBits(encoding, 21, 1, index);
        return true;
    default:
        return false;
    }
}

bool decodeScalar(const ArmOperandSpec& spec, uint32 encoding,
                  const ArmCodecContext& context, Operand& operand)
{
    ArmOperandSpec reg = spec;
    reg.m_kind = ((spec.m_flags & ARM_A32_REG_SINGLE) != 0) ? ARM_KIND_A32_SREG : ARM_KIND_A32_DREG;
    // (the scalar index scheme is m_extra, the register top bit is a field of the
    // encoding at 5 / 7 / 22: m_flags carries it in its high bits)
    reg.m_extra = (spec.m_flags >> 3) & 0x1F;
    reg.m_flags &= 7;
    if (reg.m_extra == 0x1F)
        reg.m_flags |= ARM_A32_REG_NO_TOP;
    if (!decodeRegister(reg, encoding, context, operand))
        return false;
    uint index;
    if (!getIndex(spec.m_extra, encoding, index))
        return false;
    operand.m_elementIndex = (int)index;
    return true;
}

bool encodeScalar(const ArmOperandSpec& spec, const Operand& operand,
                  const ArmCodecContext& context, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_REGISTER) || (operand.m_elementIndex < 0))
        return false;
    ArmOperandSpec reg = spec;
    reg.m_kind = ((spec.m_flags & ARM_A32_REG_SINGLE) != 0) ? ARM_KIND_A32_SREG : ARM_KIND_A32_DREG;
    reg.m_extra = (spec.m_flags >> 3) & 0x1F;
    reg.m_flags &= 7;
    if (reg.m_extra == 0x1F)
        reg.m_flags |= ARM_A32_REG_NO_TOP;
    Operand copy(operand);
    copy.m_elementIndex = -1;
    if (!encodeRegister(reg, copy, context, encoding))
        return false;
    return setIndex(spec.m_extra, (uint)operand.m_elementIndex, encoding);
}

//////////////////////////////////////////////////////////////////////////
// The structure loads and stores

// The registers count and spacing of the type field of the multiple structures
bool structureShape(uint n, uint type, uint& count, uint& stride)
{
    stride = 1;
    switch (n)
    {
    case 1:
        switch (type)
        {
        case 7: count = 1; return true;
        case 10: count = 2; return true;
        case 6: count = 3; return true;
        case 2: count = 4; return true;
        }
        return false;
    case 2:
        switch (type)
        {
        case 8: count = 2; return true;
        case 9: count = 2; stride = 2; return true;
        case 3: count = 4; return true;
        }
        return false;
    case 3:
        if (type == 4) { count = 3; return true; }
        if (type == 5) { count = 3; stride = 2; return true; }
        return false;
    case 4:
        if (type == 0) { count = 4; return true; }
        if (type == 1) { count = 4; stride = 2; return true; }
        return false;
    }
    return false;
}

// The single lane forms: the index, the alignment in bytes and the spacing.
// False if the encoding is invalid
bool laneInfo(uint n, uint32 encoding, uint& index, uint& alignment, uint& stride)
{
    uint size = field(encoding, 10, 2);
    alignment = 0;
    stride = 1;
    if (size == 3)
        return false;
    switch (n)
    {
    case 1:
        switch (size)
        {
        case 0:
            if (field(encoding, 4, 1) != 0) return false;
            index = field(encoding, 5, 3);
            break;
        case 1:
            if (field(encoding, 5, 1) != 0) return false;
            index = field(encoding, 6, 2);
            if (field(encoding, 4, 1) != 0) alignment = 2;
            break;
        default:
            if (field(encoding, 6, 1) != 0) return false;
            index = field(encoding, 7, 1);
            switch (field(encoding, 4, 2))
            {
            case 0: alignment = 0; break;
            case 3: alignment = 4; break;
            default: return false;
            }
            break;
        }
        return true;
    case 2:
        switch (size)
        {
        case 0:
            index = field(encoding, 5, 3);
            if (field(encoding, 4, 1) != 0) alignment = 2;
            break;
        case 1:
            index = field(encoding, 6, 2);
            if (field(encoding, 4, 1) != 0) alignment = 4;
            if (field(encoding, 5, 1) != 0) stride = 2;
            break;
        default:
            if (field(encoding, 5, 1) != 0) return false;
            index = field(encoding, 7, 1);
            if (field(encoding, 4, 1) != 0) alignment = 8;
            if (field(encoding, 6, 1) != 0) stride = 2;
            break;
        }
        return true;
    case 3:
        switch (size)
        {
        case 0:
            if (field(encoding, 4, 1) != 0) return false;
            index = field(encoding, 5, 3);
            break;
        case 1:
            if (field(encoding, 4, 1) != 0) return false;
            index = field(encoding, 6, 2);
            if (field(encoding, 5, 1) != 0) stride = 2;
            break;
        default:
            if (field(encoding, 4, 2) != 0) return false;
            index = field(encoding, 7, 1);
            if (field(encoding, 6, 1) != 0) stride = 2;
            break;
        }
        return true;
    default:
        switch (size)
        {
        case 0:
            if (field(encoding, 4, 1) != 0) alignment = 4;
            index = field(encoding, 5, 3);
            break;
        case 1:
            if (field(encoding, 4, 1) != 0) alignment = 8;
            index = field(encoding, 6, 2);
            if (field(encoding, 5, 1) != 0) stride = 2;
            break;
        default:
            switch (field(encoding, 4, 2))
            {
            case 0: alignment = 0; break;
            case 3: return false;
            default: alignment = 4u << field(encoding, 4, 2); break;
            }
            index = field(encoding, 7, 1);
            if (field(encoding, 6, 1) != 0) stride = 2;
            break;
        }
        return true;
    }
}

// The all lanes forms: the alignment in bytes, the spacing, the count
bool dupInfo(uint n, uint32 encoding, uint& alignment, uint& stride, uint& count)
{
    uint size = field(encoding, 6, 2);
    uint a = field(encoding, 4, 1);
    uint t = field(encoding, 5, 1);
    alignment = 0;
    stride = 1;
    switch (n)
    {
    case 1:
        if (size == 0 && a == 1)
            return false;
        alignment = a * (1u << size);
        count = t ? 2 : 1;
        return true;
    case 2:
        alignment = a * 2 * (1u << size);
        count = 2;
        stride = t ? 2 : 1;
        return true;
    case 3:
        count = 3;
        stride = t + 1;
        return true;
    default:
        if (size == 3)
        {
            if (a == 0)
                return false;
            alignment = 16;
        } else if (size == 2)
        {
            alignment = a * 8;
        } else
        {
            alignment = a * 4 * (1u << size);
        }
        count = 4;
        stride = t + 1;
        return true;
    }
}

bool decodeList(const ArmOperandSpec& spec, uint32 encoding,
                const ArmCodecContext& context, Operand& operand)
{
    operand.m_kind = Operand::KIND_REGISTER_LIST;
    operand.m_optionScheme = ARM_A32_OPT_VECTOR_LIST;
    operand.m_stride = 1;
    uint count = 1;
    switch (spec.m_extra)
    {
    case ARM_A32_LIST_VFP_S:
    {
        uint first = (field(encoding, spec.m_position, 4) << 1) | field(encoding, spec.m_flags, 1);
        uint regs = field(encoding, 0, 8);
        // LLVM tweaks the unpredictable counts
        if ((regs == 0) || (first + regs > 32))
        {
            regs = (first + regs > 32) ? (32 - first) : regs;
            if (regs < 1)
                regs = 1;
        }
        operand.m_register = ARM_REG_S0 + first;
        operand.m_count = regs;
        return true;
    }
    case ARM_A32_LIST_VFP_D:
    case ARM_A32_LIST_VFP_X:
    {
        uint first = field(encoding, spec.m_position, 4);
        if (spec.m_flags != 0xFF)
            first |= field(encoding, spec.m_flags, 1) << 4;
        uint regs = field(encoding, 1, 7);
        uint maximum = hasD32(context) ? 32 : 16;
        if ((regs == 0) || (first + regs > maximum))
        {
            regs = (first + regs > maximum) ? (maximum - first) : regs;
            if (regs < 1)
                regs = 1;
            if (regs > maximum)
                regs = maximum;
        }
        if ((first > 15) && !hasD32(context))
            return false;
        operand.m_register = ARM_REG_D0 + first;
        operand.m_count = regs;
        return true;
    }
    default:
        break;
    }

    uint first = field(encoding, 12, 4) | (field(encoding, 22, 1) << 4);
    uint n = spec.m_flags & 7;
    switch (spec.m_extra)
    {
    case ARM_A32_LIST_STRUCT:
    {
        uint type = field(encoding, 8, 4);
        uint size = field(encoding, 6, 2);
        uint align = field(encoding, 4, 2);
        uint stride;
        if (!structureShape(n, type, count, stride))
            return false;
        switch (n)
        {
        case 1:
            if (((type == 6) || (type == 7)) && ((align & 2) != 0)) return false;
            if ((type == 10) && (align == 3)) return false;
            break;
        case 2:
            if (size == 3) return false;
            if (((type == 8) || (type == 9)) && (align == 3)) return false;
            break;
        case 3:
            if ((size == 3) || ((align & 2) != 0)) return false;
            break;
        default:
            if (size == 3) return false;
            break;
        }
        // The registers which LLVM decodes: the pairs are register classes,
        // the lists of three and four d registers are checked one by one
        bool isPair = (count == 2);
        if (isPair)
        {
            if (stride == 1 && first > 30) return false;
            if (stride == 2 && first > 29) return false;
        } else if (n >= 3)
        {
            for (uint i = 0; i < count; i++)
                if ((((first + i * stride) % 32) > 15) && !hasD32(context))
                    return false;
        } else if ((first > 15) && !hasD32(context))
        {
            return false;
        }
        operand.m_register = ARM_REG_D0 + first;
        operand.m_count = count;
        operand.m_stride = stride;
        if (n >= 3)
            operand.m_index = 32;
        return true;
    }
    case ARM_A32_LIST_LANE:
    {
        uint index, alignment, stride;
        if (!laneInfo(n, encoding, index, alignment, stride))
            return false;
        for (uint i = 0; i < n; i++)
            if (((first + i * stride) > 31) || (((first + i * stride) > 15) && !hasD32(context)))
                return false;
        operand.m_register = ARM_REG_D0 + first;
        operand.m_count = n;
        operand.m_stride = stride;
        operand.m_elementIndex = (int)index;
        return true;
    }
    case ARM_A32_LIST_DUP:
    {
        uint alignment, stride;
        if (!dupInfo(n, encoding, alignment, stride, count))
            return false;
        if (n == 1 && count == 2)
        {
            if (first > 30)
                return false;
        } else if (n == 2)
        {
            if (stride == 1 && first > 30) return false;
            if (stride == 2 && first > 29) return false;
        } else if (n == 1)
        {
            if ((first > 15) && !hasD32(context))
                return false;
        } else
        {
            for (uint i = 0; i < count; i++)
                if ((((first + i * stride) % 32) > 15) && !hasD32(context))
                    return false;
        }
        operand.m_register = ARM_REG_D0 + first;
        operand.m_count = count;
        operand.m_stride = stride;
        operand.m_isExplicit = true;
        if (n >= 3)
            operand.m_index = 32;
        return true;
    }
    case ARM_A32_LIST_TBL:
    {
        uint reg = field(encoding, 16, 4) | (field(encoding, 7, 1) << 4);
        if (n == 2)
        {
            if (reg > 30)
                return false;
        } else if ((reg > 15) && !hasD32(context))
        {
            return false;
        }
        operand.m_register = ARM_REG_D0 + reg;
        operand.m_count = n;
        return true;
    }
    default:
        return false;
    }
}

bool encodeList(const ArmOperandSpec& spec, const Operand& operand,
                const ArmCodecContext& context, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_REGISTER_LIST) || (operand.m_registerMask != 0))
        return false;
    uint stride = (operand.m_stride == 0) ? 1 : operand.m_stride;
    uint count = operand.m_count;
    uint firstId = operand.m_register;
    switch (spec.m_extra)
    {
    case ARM_A32_LIST_VFP_S:
        if ((firstId < ARM_REG_S0) || (firstId > ARM_REG_S31) || (stride != 1) ||
            (count < 1) || (count > 255) || (firstId - ARM_REG_S0 + count > 32))
            return false;
        setBits(encoding, spec.m_position, 4, (firstId - ARM_REG_S0) >> 1);
        setBits(encoding, spec.m_flags, 1, (firstId - ARM_REG_S0) & 1);
        setBits(encoding, 0, 8, count);
        return true;
    case ARM_A32_LIST_VFP_D:
    case ARM_A32_LIST_VFP_X:
    {
        if ((firstId < ARM_REG_D0) || (firstId > ARM_REG_D31) || (stride != 1) ||
            (count < 1) || (count > 16 + (hasD32(context) ? 16u : 0u)))
            return false;
        uint first = firstId - ARM_REG_D0;
        if (first + count > 32)
            return false;
        setBits(encoding, spec.m_position, 4, first & 15);
        if (spec.m_flags != 0xFF)
            setBits(encoding, spec.m_flags, 1, first >> 4);
        else if (first > 15)
            return false;
        setBits(encoding, 1, 7, count);
        // (the low bit of imm8 is the fldmx / vldm form bit: kept)
        setBits(encoding, 0, 1, (spec.m_extra == ARM_A32_LIST_VFP_X) ? 1 : 0);
        return true;
    }
    default:
        break;
    }
    if ((firstId < ARM_REG_D0) || (firstId > ARM_REG_D31))
        return false;
    uint first = firstId - ARM_REG_D0;
    uint n = spec.m_flags & 7;
    switch (spec.m_extra)
    {
    case ARM_A32_LIST_STRUCT:
    {
        if ((operand.m_elementIndex >= 0) || operand.m_isExplicit)
            return false;
        uint type = field(encoding, 8, 4);
        uint expectedCount, expectedStride;
        if (!structureShape(n, type, expectedCount, expectedStride))
            return false;
        if ((count != expectedCount) || ((count > 1) && (stride != expectedStride)))
            return false;
        break;
    }
    case ARM_A32_LIST_LANE:
    {
        // The spacing is made by the size and the bit 5 / 6 of the encoding
        if (count != n)
            return false;
        if ((operand.m_elementIndex < 0) || (operand.m_elementIndex == 255))
            return false;
        uint size = field(encoding, 10, 2);
        uint index = (uint)operand.m_elementIndex;
        if (size == 0)
        {
            if (index > 7) return false;
            setBits(encoding, 5, 3, index);
        } else if (size == 1)
        {
            if (index > 3) return false;
            setBits(encoding, 6, 2, index);
            if (n != 1)
                setBits(encoding, 5, 1, (stride == 2) ? 1 : 0);
        } else
        {
            if (index > 1) return false;
            setBits(encoding, 7, 1, index);
            if (n != 1)
                setBits(encoding, 6, 1, (stride == 2) ? 1 : 0);
        }
        if ((size == 0) && (stride != 1))
            return false;
        break;
    }
    case ARM_A32_LIST_DUP:
    {
        if (!operand.m_isExplicit && (operand.m_elementIndex != 255))
            return false;
        if (n == 1)
        {
            if ((count != 1) && (count != 2))
                return false;
            setBits(encoding, 5, 1, count == 2 ? 1 : 0);
        } else
        {
            if (count != ((n == 2) ? 2u : n))
                return false;
            if ((stride != 1) && (stride != 2))
                return false;
            setBits(encoding, 5, 1, stride == 2 ? 1 : 0);
        }
        break;
    }
    case ARM_A32_LIST_TBL:
    {
        if ((count != n) || (stride != 1))
            return false;
        setBits(encoding, 16, 4, first & 15);
        setBits(encoding, 7, 1, first >> 4);
        return true;
    }
    default:
        return false;
    }
    setBits(encoding, 12, 4, first & 15);
    setBits(encoding, 22, 1, first >> 4);
    return true;
}

// The alignment in bits of a NEON memory operand, false if invalid
bool memoryAlignment(const ArmOperandSpec& spec, uint32 encoding, uint& bits)
{
    uint n = spec.m_flags & 7;
    uint alignment = 0;
    switch (spec.m_extra)
    {
    case ARM_A32_NMEM_STRUCT:
    {
        uint align = field(encoding, 4, 2);
        alignment = (align == 0) ? 0 : (4u << align);
        break;
    }
    case ARM_A32_NMEM_LANE:
    {
        uint index, stride;
        if (!laneInfo(n, encoding, index, alignment, stride))
            return false;
        break;
    }
    default:
    {
        uint stride, count;
        if (!dupInfo(n, encoding, alignment, stride, count))
            return false;
        break;
    }
    }
    bits = alignment * 8;
    return true;
}

bool decodeMemory(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    uint bits;
    if (!memoryAlignment(spec, encoding, bits))
        return false;
    operand.m_kind = Operand::KIND_MEMORY;
    operand.m_base = ARM_REG_R0 + field(encoding, spec.m_position, 4);
    operand.m_alignment = bits;
    uint rm = field(encoding, 0, 4);
    if (rm == 13)
        operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
    else if (rm != 15)
    {
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        operand.m_postIndexRegister = ARM_REG_R0 + rm;
    }
    return true;
}

bool encodeMemory(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if ((operand.m_kind != Operand::KIND_MEMORY) || (operand.m_base < ARM_REG_R0) ||
        (operand.m_base > ARM_REG_PC) || (operand.m_index != ARM_NO_REGISTER) ||
        (operand.m_offset != 0) || operand.m_isSubtract)
        return false;
    setBits(encoding, spec.m_position, 4, operand.m_base - ARM_REG_R0);
    uint rm;
    switch (operand.m_addressing)
    {
    case Operand::ADDRESSING_OFFSET:
        rm = 15;
        break;
    case Operand::ADDRESSING_PRE_INDEX:
        rm = 13;
        break;
    default:
        if ((operand.m_postIndexRegister < ARM_REG_R0) || (operand.m_postIndexRegister > ARM_REG_PC))
            return false;
        rm = operand.m_postIndexRegister - ARM_REG_R0;
        if ((rm == 13) || (rm == 15))
            return false;
        break;
    }
    setBits(encoding, 0, 4, rm);
    // The alignment field: the value whose decoding gives the alignment. Bit 5
    // is the spacing or the index of the lanes forms: it is kept when it can
    uint wanted = operand.m_alignment;
    uint current5 = field(encoding, 5, 1);
    for (uint pass = 0; pass < 2; pass++)
    {
        for (uint bit4 = 0; bit4 < 2; bit4++)
        {
            uint bit5 = (pass == 0) ? current5 : (current5 ^ 1);
            if ((pass == 1) && (spec.m_extra != ARM_A32_NMEM_STRUCT) &&
                !((spec.m_extra == ARM_A32_NMEM_LANE) && (field(encoding, 10, 2) == 2)))
                continue;
            // (vld4 all lanes of 32 bit elements: the bit 6 tells the 128 bit alignment)
            bool isFourDup = (spec.m_extra == ARM_A32_NMEM_DUP) && ((spec.m_flags & 7) == 4) &&
                             (field(encoding, 7, 1) == 1);
            for (uint bit6 = 0; bit6 < (isFourDup ? 2u : 1u); bit6++)
            {
                uint32 trial = encoding;
                setBits(trial, 4, 2, (bit5 << 1) | bit4);
                if (isFourDup)
                    setBits(trial, 6, 1, bit6);
                uint bits;
                if (memoryAlignment(spec, trial, bits) && (bits == wanted))
                {
                    encoding = trial;
                    return true;
                }
            }
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////
// The immediates

// The floating point immediate of the VFP / NEON: 8 bits to a float
double expandFloatImmediate(uint imm8)
{
    uint32 sign = (imm8 >> 7) & 1;
    uint32 exponent = (imm8 >> 4) & 7;
    uint32 mantissa = imm8 & 15;
    uint32 bits = (sign << 31) | (((exponent & 4) != 0 ? 0 : 1) << 30) |
                  (((exponent & 4) != 0 ? 0x1f : 0) << 25) | ((exponent & 3) << 23) |
                  (mantissa << 19);
    union { uint32 m_bits; float m_value; } convert;
    convert.m_bits = bits;
    return (double)convert.m_value;
}

// The text of a float like LLVM prints it: 1.000000e+00
cString floatText(double value)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%e", value);
    return cString(buffer);
}

bool decodeFloatImmediate(uint imm8, Operand& operand)
{
    operand.m_kind = Operand::KIND_FLOAT_IMMEDIATE;
    operand.m_floatImmediate = expandFloatImmediate(imm8);
    operand.m_immediate = imm8;
    operand.m_name = floatText(operand.m_floatImmediate);
    return true;
}

bool encodeFloatImmediate(const Operand& operand, uint& imm8)
{
    if (operand.m_kind != Operand::KIND_FLOAT_IMMEDIATE)
        return false;
    // (the text of the number is converted with an error: compare with a tolerance)
    for (uint i = 0; i < 256; i++)
    {
        double expected = expandFloatImmediate(i);
        double difference = expected - operand.m_floatImmediate;
        if (difference < 0)
            difference = -difference;
        double magnitude = expected < 0 ? -expected : expected;
        if (difference <= magnitude * 1e-6)
        {
            imm8 = i;
            return true;
        }
    }
    return false;
}

// The element value of a NEON modified immediate (op:cmode, imm8)
bool expandModifiedImmediate(uint opCmode, uint imm8, uint64& value)
{
    value = 0;
    if (opCmode == 0xe)
    {
        value = imm8;
    } else if ((opCmode & 0xc) == 0x8)
    {
        value = (uint64)imm8 << (8 * ((opCmode & 0x6) >> 1));
    } else if ((opCmode & 0x8) == 0)
    {
        value = (uint64)imm8 << (8 * ((opCmode & 0x6) >> 1));
    } else if ((opCmode & 0xe) == 0xc)
    {
        uint byteNumber = 1 + (opCmode & 1);
        value = ((uint64)imm8 << (8 * byteNumber)) | (0xffffu >> (8 * (2 - byteNumber)));
    } else if (opCmode == 0x1e)
    {
        for (uint i = 0; i < 8; i++)
            if (((imm8 >> i) & 1) != 0)
                value |= (uint64)0xff << (8 * i);
    } else
    {
        return false;
    }
    return true;
}

// Find the cmode and imm8 which give a value, for the entry of the context:
// the bits of the entry (op, and the cmode bits which are fixed) are kept
bool findModifiedImmediate(const ArmCodecContext& context, uint32& encoding, uint64 value)
{
    for (uint cmode = 0; cmode < 16; cmode++)
    {
        uint32 trial = encoding;
        setBits(trial, 8, 4, cmode);
        if ((context.m_entry != NULL) &&
            (((trial ^ context.m_entry->m_value) & context.m_entry->m_mask) != 0))
            continue;
        uint opCmode = (field(trial, 5, 1) << 4) | cmode;
        for (uint i = 0; i < 256; i++)
        {
            uint64 candidate;
            if (expandModifiedImmediate(opCmode, i, candidate) && (candidate == value))
            {
                encoding = trial;
                setBits(encoding, 0, 4, i & 15);
                setBits(encoding, 16, 3, (i >> 4) & 7);
                setBits(encoding, 24, 1, i >> 7);
                return true;
            }
        }
    }
    return false;
}

uint modifiedImmediateField(uint32 encoding)
{
    return field(encoding, 0, 4) | (field(encoding, 16, 3) << 4) | (field(encoding, 24, 1) << 7);
}

void setModifiedImmediateField(uint32& encoding, uint imm8)
{
    setBits(encoding, 0, 4, imm8 & 15);
    setBits(encoding, 16, 3, (imm8 >> 4) & 7);
    setBits(encoding, 24, 1, imm8 >> 7);
}

bool decodeSimdField(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    operand.m_kind = Operand::KIND_IMMEDIATE;
    switch (spec.m_extra)
    {
    case ARM_A32_SIMD_FIELD_ROT:
        operand.m_immediate = 90 * field(encoding, spec.m_position, 2);
        return true;
    case ARM_A32_SIMD_FIELD_ROT_ODD:
        operand.m_immediate = 90 + 180 * field(encoding, spec.m_position, 1);
        return true;
    case ARM_A32_SIMD_FIELD_MODIMM:
    {
        uint64 value;
        uint opCmode = (field(encoding, 5, 1) << 4) | field(encoding, 8, 4);
        if (!expandModifiedImmediate(opCmode, modifiedImmediateField(encoding), value))
            return false;
        operand.m_immediate = (int64)value;
        operand.m_isHex = true;
        operand.m_isUnsigned = true;
        return true;
    }
    case ARM_A32_SIMD_FIELD_MODIMM_FLOAT:
        return decodeFloatImmediate(modifiedImmediateField(encoding), operand);
    case ARM_A32_SIMD_FIELD_SHIFT_RIGHT:
        operand.m_isHex = true;
        operand.m_immediate = (int64)spec.m_flags - (int64)field(encoding, spec.m_position, spec.m_flags == 8 ? 3 :
                                                      spec.m_flags == 16 ? 4 : spec.m_flags == 32 ? 5 : 6);
        return true;
    case ARM_A32_SIMD_FIELD_SHIFT_LEFT:
        operand.m_isHex = true;
        operand.m_immediate = field(encoding, spec.m_position, spec.m_flags);
        return true;
    case ARM_A32_SIMD_FIELD_PLAIN:
        operand.m_immediate = field(encoding, spec.m_position, spec.m_flags);
        operand.m_isHex = true;
        return true;
    case ARM_A32_SIMD_FIELD_FBITS:
    {
        // imm4 at 0 : bit 5
        uint value = (field(encoding, 0, 4) << 1) | field(encoding, 5, 1);
        operand.m_immediate = (int64)spec.m_flags - (int64)value;
        return true;
    }
    case ARM_A32_SIMD_FIELD_VCVT_IMM:
        // (the encodings without the top bit are vmov immediates)
        if (field(encoding, 21, 1) == 0)
            return false;
        operand.m_isHex = true;
        operand.m_immediate = 64 - (int64)field(encoding, 16, 6);
        return true;
    case ARM_A32_SIMD_FIELD_VFP_IMM:
        return decodeFloatImmediate((field(encoding, 16, 4) << 4) | field(encoding, 0, 4), operand);
    default:
        return false;
    }
}

bool encodeSimdField(const ArmOperandSpec& spec, const Operand& operand,
                     const ArmCodecContext& context, uint32& encoding)
{
    if (spec.m_extra == ARM_A32_SIMD_FIELD_VFP_IMM || spec.m_extra == ARM_A32_SIMD_FIELD_MODIMM_FLOAT)
    {
        uint imm8;
        if (!encodeFloatImmediate(operand, imm8))
            return false;
        if (spec.m_extra == ARM_A32_SIMD_FIELD_VFP_IMM)
        {
            setBits(encoding, 16, 4, imm8 >> 4);
            setBits(encoding, 0, 4, imm8 & 15);
        } else
        {
            setModifiedImmediateField(encoding, imm8);
        }
        return true;
    }
    if (operand.m_kind != Operand::KIND_IMMEDIATE)
        return false;
    int64 value = operand.m_immediate;
    switch (spec.m_extra)
    {
    case ARM_A32_SIMD_FIELD_ROT:
        if ((value < 0) || (value > 270) || (value % 90 != 0))
            return false;
        setBits(encoding, spec.m_position, 2, (uint)(value / 90));
        return true;
    case ARM_A32_SIMD_FIELD_ROT_ODD:
        if ((value != 90) && (value != 270))
            return false;
        setBits(encoding, spec.m_position, 1, value == 270 ? 1 : 0);
        return true;
    case ARM_A32_SIMD_FIELD_MODIMM:
        return findModifiedImmediate(context, encoding, (uint64)value);
    case ARM_A32_SIMD_FIELD_SHIFT_RIGHT:
    {
        uint width = spec.m_flags == 8 ? 3 : spec.m_flags == 16 ? 4 : spec.m_flags == 32 ? 5 : 6;
        int64 raw = (int64)spec.m_flags - value;
        if ((raw < 0) || (raw >= ((int64)1 << width)))
            return false;
        setBits(encoding, spec.m_position, width, (uint)raw);
        return true;
    }
    case ARM_A32_SIMD_FIELD_SHIFT_LEFT:
    case ARM_A32_SIMD_FIELD_PLAIN:
        if ((value < 0) || (value >= ((int64)1 << spec.m_flags)))
            return false;
        setBits(encoding, spec.m_position, spec.m_flags, (uint)value);
        return true;
    case ARM_A32_SIMD_FIELD_FBITS:
    {
        int64 raw = (int64)spec.m_flags - value;
        if ((raw < 0) || (raw > 31))
            return false;
        setBits(encoding, 0, 4, (uint)raw >> 1);
        setBits(encoding, 5, 1, (uint)raw & 1);
        return true;
    }
    case ARM_A32_SIMD_FIELD_VCVT_IMM:
    {
        int64 raw = 64 - value;
        if ((raw < 0) || (raw > 63))
            return false;
        setBits(encoding, 16, 6, (uint)raw);
        return true;
    }
    default:
        return false;
    }
}

const char* const gSimdFixedTexts[ARM_A32_SIMD_FIXED_COUNT - ARM_A32_SIMD_BASE] = {
    "fpscr", "fpexc", "fpsid", "mvfr0", "mvfr1", "mvfr2", "fpinst", "fpinst2", "APSR_nzcv",
    "#0x8", "#0x10", "#0x20"
};

} // anonymous namespace

const char* getA32SimdFixedText(uint text)
{
    ASSERT((text >= ARM_A32_SIMD_BASE) && (text < ARM_A32_SIMD_FIXED_COUNT));
    return gSimdFixedTexts[text - ARM_A32_SIMD_BASE];
}

bool decodeA32Simd(const ArmOperandSpec& spec, uint32 encoding,
                   const ArmCodecContext& context, ArmDecodedOperand& operand)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_A32_SREG:
    case ARM_KIND_A32_DREG:
    case ARM_KIND_A32_QREG:
        return decodeRegister(spec, encoding, context, operand);
    case ARM_KIND_A32_NEON_SCALAR:
        return decodeScalar(spec, encoding, context, operand);
    case ARM_KIND_A32_FP_LIST:
        return decodeList(spec, encoding, context, operand);
    case ARM_KIND_A32_NEON_MEMORY:
        return decodeMemory(spec, encoding, operand);
    case ARM_KIND_A32_MEMORY:
        // vldr / vstr of half precision
        operand.m_kind = Operand::KIND_MEMORY;
        operand.m_base = ARM_REG_R0 + field(encoding, 16, 4);
        operand.m_offset = field(encoding, 0, 8) * 2;
        operand.m_isSubtract = (field(encoding, 23, 1) == 0);
        return true;
    case ARM_KIND_A32_FIELD:
        return decodeSimdField(spec, encoding, operand);
    case ARM_KIND_A32_FIXED:
        operand.m_kind = Operand::KIND_NAME;
        operand.m_name = cString(getA32SimdFixedText(spec.m_extra));
        return true;
    default:
        return false;
    }
}

bool encodeA32Simd(const ArmOperandSpec& spec, const ArmDecodedOperand& operand,
                   const ArmCodecContext& context, uint32& encoding)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_A32_SREG:
    case ARM_KIND_A32_DREG:
    case ARM_KIND_A32_QREG:
        return encodeRegister(spec, operand, context, encoding);
    case ARM_KIND_A32_NEON_SCALAR:
        return encodeScalar(spec, operand, context, encoding);
    case ARM_KIND_A32_FP_LIST:
        return encodeList(spec, operand, context, encoding);
    case ARM_KIND_A32_NEON_MEMORY:
        return encodeMemory(spec, operand, encoding);
    case ARM_KIND_A32_MEMORY:
    {
        if ((operand.m_kind != Operand::KIND_MEMORY) || (operand.m_base < ARM_REG_R0) ||
            (operand.m_base > ARM_REG_PC) || (operand.m_index != ARM_NO_REGISTER) ||
            (operand.m_addressing != Operand::ADDRESSING_OFFSET) || (operand.m_offset < 0) ||
            ((operand.m_offset & 1) != 0) || (operand.m_offset > 510))
            return false;
        setBits(encoding, 16, 4, operand.m_base - ARM_REG_R0);
        setBits(encoding, 0, 8, (uint)operand.m_offset / 2);
        setBits(encoding, 23, 1, operand.m_isSubtract ? 0 : 1);
        return true;
    }
    case ARM_KIND_A32_FIELD:
        return encodeSimdField(spec, operand, context, encoding);
    case ARM_KIND_A32_FIXED:
    {
        cString text;
        if (operand.m_kind == Operand::KIND_NAME)
            text = operand.m_name;
        else if (operand.m_kind == Operand::KIND_REGISTER)
        {
            const char* registerName = getRegisterName(ArmInstructionSet::ARM_A32, operand.m_register);
            if (registerName == NULL)
                return false;
            text = cString(registerName);
        } else if (operand.m_kind == Operand::KIND_IMMEDIATE)
            text = cString("#0x") + hexText((uint64)operand.m_immediate);
        else
            return false;
        text.makeLower();
        cString expected(getA32SimdFixedText(spec.m_extra));
        expected.makeLower();
        return text == expected;
    }
    default:
        return false;
    }
}

cString decodeNeonDataType(const ArmOperandSpec& spec, uint32 encoding)
{
    static const char* const sizes[] = { ".8", ".16", ".32", ".64" };
    return cString(sizes[field(encoding, spec.m_position, 2)]);
}

bool encodeNeonDataType(const ArmOperandSpec& spec, cString& suffix, uint32& encoding)
{
    static const char* const sizes[] = { ".16", ".32", ".64", ".8" };
    static const uint values[] = { 1, 2, 3, 0 };
    for (uint i = 0; i < 4; i++)
    {
        cString text(sizes[i]);
        if ((suffix.length() >= text.length()) && (suffix.left(text.length()) == text))
        {
            setBits(encoding, spec.m_position, 2, values[i]);
            suffix = suffix.part(text.length(), suffix.length());
            return true;
        }
    }
    return false;
}

} // namespace armdis
