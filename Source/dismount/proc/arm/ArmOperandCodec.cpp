#include "dismount/dismount.h"
/*
 * ArmOperandCodec.cpp
 *
 * Implementation file. The dispatch by the kind of the spec, and the kinds
 * which are the same for every instruction set. The kinds of an area are
 * in ArmOperandCodec<Area>.cpp (see ArmOperandCodecInternal.h).
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

//////////////////////////////////////////////////////////////////////////
// The tables of ArmOperandCodec.h

const ArmSplitImmediate gArmSplitImmediates[ARM_SPLIT_COUNT] = {
    // ARM_SPLIT_TBZ_BIT: b5 (31) : b40 (19, 5 bits)
    { 2, { 31, 19, 0, 0 }, { 1, 5, 0, 0 } }
};

const ArmMnemonicSuffix gArmMnemonicSuffixes[ARM_SUFFIX_COUNT] = {
    // ARM_SUFFIX_WIDE_NARROW
    { "", ".w" }
};

//////////////////////////////////////////////////////////////////////////
// Helpers

int64 signExtend(uint64 value, uint width)
{
    if (width >= 64)
        return (int64)value;
    uint64 sign = (uint64)1 << (width - 1);
    value &= (((uint64)1 << width) - 1);
    return (int64)((value ^ sign) - sign);
}

cString hexText(uint64 value)
{
    cString text(value, 16);
    text.makeLower();
    return text;
}

bool parseNumberedName(const cString& name, const char* prefix, uint maximum,
                       uint& number)
{
    cString start(prefix);
    uint length = start.length();
    if ((name.length() <= length) || (name.length() > length + 3))
        return false;
    if (!(name.left(length) == start))
        return false;
    // The digits, without a leading zero
    uint value = 0;
    for (uint i = length; i < name.length(); i++)
    {
        character c = name[i];
        if ((c < XSTL_CHAR('0')) || (c > XSTL_CHAR('9')))
            return false;
        if ((i == length) && (c == XSTL_CHAR('0')) && (name.length() > length + 1))
            return false;
        value = value * 10 + (c - XSTL_CHAR('0'));
    }
    if (value > maximum)
        return false;
    number = value;
    return true;
}

bool getBranchTarget(const ArmDecodedOperand& operand,
                     const ArmCodecContext& context,
                     uint64& target)
{
    if (operand.m_kind == ArmDecodedOperand::KIND_BRANCH_TARGET)
    {
        target = (uint64)operand.m_immediate;
        return true;
    }
    if (operand.m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
    {
        if (operand.m_shift != ArmDecodedOperand::SHIFT_NONE)
            return false;
        target = operand.m_hasHash ?
                    (context.m_address + (uint64)operand.m_immediate) :
                    (uint64)operand.m_immediate;
        return true;
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////
// ArmOperandCodec

uint32 ArmOperandCodec::getField(uint32 encoding, uint position, uint width)
{
    ASSERT(width >= 1 && width <= 32 && position + width <= 32);
    if (width == 32)
        return encoding;
    return (encoding >> position) & (((uint32)1 << width) - 1);
}

void ArmOperandCodec::setField(uint32& encoding, uint position, uint width, uint32 value)
{
    ASSERT(width >= 1 && width <= 32 && position + width <= 32);
    uint32 mask = (width == 32) ? 0xFFFFFFFFu : (((uint32)1 << width) - 1);
    encoding = (encoding & ~(mask << position)) | ((value & mask) << position);
}

bool ArmOperandCodec::isMnemonicPart(const ArmOperandSpec& spec)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_CONDITION_MNEMONIC:
    case ARM_KIND_SETS_FLAGS:
    case ARM_KIND_A32_NEON_DATA_TYPE:
    case ARM_KIND_MNEMONIC_SUFFIX:
    case ARM_KIND_T16_IT_MASK:
        return true;
    default:
        return false;
    }
}

bool ArmOperandCodec::isHidden(const ArmOperandSpec& spec)
{
    return spec.m_kind == ARM_KIND_A64_TIED_REGISTER;
}

//////////////////////////////////////////////////////////////////////////
// The mnemonic parts

cString ArmOperandCodec::decodeMnemonicPart(const ArmOperandSpec& spec,
                                            uint32 encoding,
                                            const ArmCodecContext& context)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_CONDITION_MNEMONIC:
    {
        uint condition = getField(encoding, spec.m_position, 4);
        if (context.m_type == ArmInstructionSet::ARM_A64)
            return cString(".") + getConditionName(condition);
        // AArch32: al is not printed
        if (condition == 14)
            return cString();
        return cString(getConditionName(condition));
    }
    case ARM_KIND_SETS_FLAGS:
        return (getField(encoding, spec.m_position, 1) != 0) ? cString("s") : cString();
    case ARM_KIND_MNEMONIC_SUFFIX:
    {
        ASSERT(spec.m_extra < ARM_SUFFIX_COUNT);
        const ArmMnemonicSuffix& suffix = gArmMnemonicSuffixes[spec.m_extra];
        return cString((getField(encoding, spec.m_position, 1) != 0) ?
                                    suffix.m_set : suffix.m_clear);
    }
    default:
        // TODO: the Advanced SIMD data types and the it mask of AArch32
        return cString();
    }
}

bool ArmOperandCodec::encodeMnemonicPart(const ArmOperandSpec& spec,
                                         cString& suffix,
                                         const ArmCodecContext& context,
                                         uint32& encoding)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_CONDITION_MNEMONIC:
    {
        bool isA64 = (context.m_type == ArmInstructionSet::ARM_A64);
        uint start = 0;
        if (isA64)
        {
            // b.eq: the condition is mandatory
            if ((suffix.length() < 3) || (suffix[0] != XSTL_CHAR('.')))
                return false;
            start = 1;
        } else if (suffix.length() < 2)
        {
            // The AArch32 condition is optional: al
            setField(encoding, spec.m_position, 4, 14);
            return true;
        }
        uint condition;
        if (!findCondition(suffix.part(start, start + 2), condition))
        {
            if (isA64)
                return false;
            setField(encoding, spec.m_position, 4, 14);
            return true;
        }
        setField(encoding, spec.m_position, 4, condition);
        suffix = suffix.part(start + 2, suffix.length());
        return true;
    }
    case ARM_KIND_SETS_FLAGS:
        if ((suffix.length() > 0) && (suffix[0] == XSTL_CHAR('s')))
        {
            setField(encoding, spec.m_position, 1, 1);
            suffix = suffix.part(1, suffix.length());
        } else
        {
            setField(encoding, spec.m_position, 1, 0);
        }
        return true;
    case ARM_KIND_MNEMONIC_SUFFIX:
    {
        ASSERT(spec.m_extra < ARM_SUFFIX_COUNT);
        const ArmMnemonicSuffix& pair = gArmMnemonicSuffixes[spec.m_extra];
        cString setText(pair.m_set);
        cString clearText(pair.m_clear);
        if ((setText.length() > 0) && (suffix.length() >= setText.length()) &&
            (suffix.left(setText.length()) == setText))
        {
            setField(encoding, spec.m_position, 1, 1);
            suffix = suffix.part(setText.length(), suffix.length());
            return true;
        }
        if ((clearText.length() == 0) ||
            ((suffix.length() >= clearText.length()) &&
             (suffix.left(clearText.length()) == clearText)))
        {
            setField(encoding, spec.m_position, 1, 0);
            suffix = suffix.part(clearText.length(), suffix.length());
            return true;
        }
        return false;
    }
    default:
        // TODO: the Advanced SIMD data types and the it mask of AArch32
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// decode / encode

bool ArmOperandCodec::decode(const ArmOperandSpec& spec,
                             uint32 encoding,
                             const ArmCodecContext& context,
                             ArmDecodedOperand& operand)
{
    operand = ArmDecodedOperand();
    switch (spec.m_kind)
    {
    case ARM_KIND_IMMEDIATE:
    case ARM_KIND_SIGNED_IMMEDIATE:
    {
        uint scale = spec.m_flags & ARM_SPEC_SCALE_MASK;
        uint64 value = getField(encoding, spec.m_position, spec.m_extra);
        if (spec.m_kind == ARM_KIND_SIGNED_IMMEDIATE)
            operand.m_immediate = signExtend(value, spec.m_extra) * ((int64)1 << scale);
        else
            operand.m_immediate = (int64)(value << scale);
        if ((spec.m_flags & ARM_SPEC_PLUS_ONE) != 0)
            operand.m_immediate++;
        operand.m_kind = ArmDecodedOperand::KIND_IMMEDIATE;
        operand.m_isHex = (spec.m_flags & ARM_SPEC_HEX) != 0;
        if (((spec.m_flags & ARM_SPEC_DECIMAL_ZERO) != 0) && (operand.m_immediate == 0))
            operand.m_isHex = false;
        return true;
    }
    case ARM_KIND_SPLIT_IMMEDIATE:
    {
        ASSERT(spec.m_extra < ARM_SPLIT_COUNT);
        const ArmSplitImmediate& split = gArmSplitImmediates[spec.m_extra];
        uint64 value = 0;
        for (uint i = 0; i < split.m_count; i++)
            value = (value << split.m_width[i]) |
                    getField(encoding, split.m_position[i], split.m_width[i]);
        uint scale = spec.m_flags & ARM_SPEC_SCALE_MASK;
        operand.m_immediate = (int64)(value << scale);
        if ((spec.m_flags & ARM_SPEC_PLUS_ONE) != 0)
            operand.m_immediate++;
        operand.m_kind = ArmDecodedOperand::KIND_IMMEDIATE;
        operand.m_isHex = (spec.m_flags & ARM_SPEC_HEX) != 0;
        return true;
    }
    case ARM_KIND_BRANCH_TARGET:
    {
        // A64 only for now. TODO: A32 and T32 (the pc offset, the split fields)
        if (context.m_type != ArmInstructionSet::ARM_A64)
            return false;
        int64 offset = signExtend(getField(encoding, spec.m_position, spec.m_extra),
                                  spec.m_extra) * 4;
        operand.m_kind = ArmDecodedOperand::KIND_BRANCH_TARGET;
        operand.m_immediate = (int64)(context.m_address + (uint64)offset);
        return true;
    }
    case ARM_KIND_CONDITION:
    {
        uint condition = getField(encoding, spec.m_position, 4);
        if ((spec.m_flags & ARM_SPEC_INVERT) != 0)
            condition ^= 1;
        operand.m_kind = ArmDecodedOperand::KIND_CONDITION;
        operand.m_condition = condition;
        return true;
    }
    case ARM_KIND_GPR:
        if (context.m_type == ArmInstructionSet::ARM_A64)
            return decodeA64(spec, encoding, context, operand);
        return decodeA32(spec, encoding, context, operand);
    default:
        break;
    }

    // The kinds of the areas
    if ((context.m_type == ArmInstructionSet::ARM_A64) && isSimdSpec(spec))
        return decodeSimd(spec, encoding, context, operand);
    if (context.m_type == ArmInstructionSet::ARM_A64)
        return decodeA64(spec, encoding, context, operand);
    // TODO: the AArch32, SVE and SME kinds
    return decodeA32(spec, encoding, context, operand);
}

bool ArmOperandCodec::encode(const ArmOperandSpec& spec,
                             const ArmDecodedOperand& operand,
                             const ArmCodecContext& context,
                             uint32& encoding)
{
    // A scalar register has no arrangement and no element (h1.h[4], w10.b[1])
    if ((context.m_type == ArmInstructionSet::ARM_A64) &&
        ((spec.m_kind == ARM_KIND_GPR) || (spec.m_kind == ARM_KIND_A64_FPR)) &&
        ((operand.m_arrangement != ArmDecodedOperand::ARRANGEMENT_NONE) ||
         (operand.m_elementIndex >= 0)))
        return false;
    switch (spec.m_kind)
    {
    case ARM_KIND_IMMEDIATE:
    case ARM_KIND_SIGNED_IMMEDIATE:
    {
        if ((operand.m_kind != ArmDecodedOperand::KIND_IMMEDIATE) ||
            (operand.m_shift != ArmDecodedOperand::SHIFT_NONE))
            return false;
        uint scale = spec.m_flags & ARM_SPEC_SCALE_MASK;
        int64 value = operand.m_immediate;
        if ((spec.m_flags & ARM_SPEC_PLUS_ONE) != 0)
            value--;
        if ((value & (((int64)1 << scale) - 1)) != 0)
            return false;
        value >>= scale;
        if (spec.m_kind == ARM_KIND_SIGNED_IMMEDIATE)
        {
            int64 limit = (int64)1 << (spec.m_extra - 1);
            if ((value < -limit) || (value >= limit))
                return false;
        } else
        {
            if ((value < 0) || (spec.m_extra < 63 && value >= ((int64)1 << spec.m_extra)))
                return false;
        }
        setField(encoding, spec.m_position, spec.m_extra, (uint32)value);
        return true;
    }
    case ARM_KIND_SPLIT_IMMEDIATE:
    {
        if ((operand.m_kind != ArmDecodedOperand::KIND_IMMEDIATE) ||
            (operand.m_shift != ArmDecodedOperand::SHIFT_NONE))
            return false;
        ASSERT(spec.m_extra < ARM_SPLIT_COUNT);
        const ArmSplitImmediate& split = gArmSplitImmediates[spec.m_extra];
        uint scale = spec.m_flags & ARM_SPEC_SCALE_MASK;
        int64 value = operand.m_immediate;
        if ((spec.m_flags & ARM_SPEC_PLUS_ONE) != 0)
            value--;
        if ((value < 0) || ((value & (((int64)1 << scale) - 1)) != 0))
            return false;
        value >>= scale;
        uint totalWidth = 0;
        for (uint i = 0; i < split.m_count; i++)
            totalWidth += split.m_width[i];
        if (value >= ((int64)1 << totalWidth))
            return false;
        // The fields, from the least significant
        for (int i = split.m_count - 1; i >= 0; i--)
        {
            setField(encoding, split.m_position[i], split.m_width[i], (uint32)value);
            value >>= split.m_width[i];
        }
        return true;
    }
    case ARM_KIND_BRANCH_TARGET:
    {
        if (context.m_type != ArmInstructionSet::ARM_A64)
            return false;
        uint64 target;
        if (!getBranchTarget(operand, context, target))
            return false;
        int64 offset = (int64)(target - context.m_address);
        if ((offset & 3) != 0)
            return false;
        offset /= 4;
        int64 limit = (int64)1 << (spec.m_extra - 1);
        if ((offset < -limit) || (offset >= limit))
            return false;
        setField(encoding, spec.m_position, spec.m_extra, (uint32)offset);
        return true;
    }
    case ARM_KIND_CONDITION:
    {
        uint condition;
        if (operand.m_kind == ArmDecodedOperand::KIND_CONDITION)
            condition = operand.m_condition;
        else if (operand.m_kind == ArmDecodedOperand::KIND_NAME)
        {
            if (!findCondition(operand.m_name, condition))
                return false;
        } else
            return false;
        if ((spec.m_flags & ARM_SPEC_INVERT) != 0)
            condition ^= 1;
        setField(encoding, spec.m_position, 4, condition);
        return true;
    }
    case ARM_KIND_GPR:
        if (context.m_type == ArmInstructionSet::ARM_A64)
            return encodeA64(spec, operand, context, encoding);
        return encodeA32(spec, operand, context, encoding);
    default:
        break;
    }

    if ((context.m_type == ArmInstructionSet::ARM_A64) && isSimdSpec(spec))
        return encodeSimd(spec, operand, context, encoding);
    if (context.m_type == ArmInstructionSet::ARM_A64)
        return encodeA64(spec, operand, context, encoding);
    return encodeA32(spec, operand, context, encoding);
}

} // namespace armdis
