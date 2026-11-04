#include "dismount/dismount.h"
/*
 * ArmOperandCodecA64.cpp
 *
 * Implementation file. The A64 kinds of ArmOperandCodec: the general purpose
 * and the scalar floating point registers, the immediates, the shifted and
 * extended registers, the memory addressing, the pc relative operands, the
 * system registers and the named options.
 *
 * TODO: The Advanced SIMD, SVE and SME kinds (ARM_KIND_A64_VECTOR...) are not
 * implemented, they belong to ArmOperandCodecA64Simd.cpp, ...Sve.cpp and
 * ...Sme.cpp. A spec of such a kind is a reserved field here.
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

//////////////////////////////////////////////////////////////////////////
// The registers

// Return true if the register is the 64 bit one, by the ArmGprFlags and the
// instruction bits
bool isWideRegister(uint flags, uint32 encoding)
{
    if ((flags & ARM_GPR_X) != 0)
        return true;
    if ((flags & ARM_GPR_W) != 0)
        return false;
    if ((flags & ARM_GPR_SF) != 0)
        return ArmOperandCodec::getField(encoding, 31, 1) != 0;
    if ((flags & ARM_GPR_OPC22) != 0)
        return ArmOperandCodec::getField(encoding, 22, 1) == 0;
    return false;
}

// Return the id of a general purpose register. Number 31 is sp / wsp or the
// zero register
uint getGprId(uint number, bool is64, bool isStackPointer)
{
    if (number == 31)
    {
        if (isStackPointer)
            return is64 ? ARM64_REG_SP : ARM64_REG_WSP;
        return is64 ? ARM64_REG_XZR : ARM64_REG_WZR;
    }
    return (is64 ? ARM64_REG_X0 : ARM64_REG_W0) + number;
}

// Classify a general purpose register id. False if it isn't one
bool classifyGpr(uint id, bool& is64, uint& number, bool& isStackPointer)
{
    isStackPointer = false;
    if (id <= ARM64_REG_X30)
    {
        is64 = true;
        number = id - ARM64_REG_X0;
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
    case ARM64_REG_SP:
        is64 = true;
        number = 31;
        isStackPointer = true;
        return true;
    case ARM64_REG_XZR:
        is64 = true;
        number = 31;
        return true;
    case ARM64_REG_WSP:
        is64 = false;
        number = 31;
        isStackPointer = true;
        return true;
    case ARM64_REG_WZR:
        is64 = false;
        number = 31;
        return true;
    }
    return false;
}

// The scalar floating point register sizes in bits
const uint gFprBits[] = { 8, 16, 32, 64, 128 };

// Return the id of a scalar floating point register of 8..128 bits
uint getFprId(uint number, uint bits)
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

// Classify a scalar floating point register id
bool classifyFpr(uint id, uint& bits, uint& number)
{
    static const uint bases[] = { ARM64_REG_B0, ARM64_REG_H0, ARM64_REG_S0,
                                  ARM64_REG_D0, ARM64_REG_Q0 };
    for (uint i = 0; i < 5; i++)
        if ((id >= bases[i]) && (id < bases[i] + 32))
        {
            bits = gFprBits[i];
            number = id - bases[i];
            return true;
        }
    return false;
}

//////////////////////////////////////////////////////////////////////////
// The logical immediate

// Return the value of a bitmask immediate. False for the reserved ones
bool decodeLogicalImmediate(uint n, uint immr, uint imms, uint size, uint64& value)
{
    uint pattern = (n << 6) | ((~imms) & 0x3f);
    if (pattern == 0)
        return false;
    uint length = 0;
    while ((pattern >> (length + 1)) != 0)
        length++;
    // length 0 is an element of one bit
    if (length < 1)
        return false;
    uint elementSize = 1u << length;
    if (elementSize > size)
        return false;
    uint levels = elementSize - 1;
    uint ones = imms & levels;
    uint rotation = immr & levels;
    if (ones == levels)
        return false;
    uint64 element = (ones + 1 == 64) ? ~(uint64)0 : (((uint64)1 << (ones + 1)) - 1);
    // Rotate right inside the element
    if (rotation != 0)
    {
        uint64 mask = (elementSize == 64) ? ~(uint64)0 : (((uint64)1 << elementSize) - 1);
        element = ((element >> rotation) | (element << (elementSize - rotation))) & mask;
    }
    // Replicate the element
    uint64 result = element;
    for (uint filled = elementSize; filled < size; filled *= 2)
        result |= result << filled;
    value = result;
    return true;
}

// Find the n, immr and imms of a bitmask immediate. False if the value is not
// one. The immediate of 32 bits sizes must have its high half equal to its
// low half in the 64 bit pattern
bool encodeLogicalImmediate(uint64 value, uint size, uint& n, uint& immr, uint& imms)
{
    if (size == 32)
        value = (value & 0xFFFFFFFFu) | (value << 32);
    if ((value == 0) || (value == ~(uint64)0))
        return false;
    // The smallest period of the pattern
    uint elementSize = 64;
    while (elementSize > 2)
    {
        uint half = elementSize / 2;
        uint64 mask = ((uint64)1 << half) - 1;
        if ((value & mask) != ((value >> half) & mask))
            break;
        elementSize = half;
    }
    uint64 elementMask = (elementSize == 64) ? ~(uint64)0 : (((uint64)1 << elementSize) - 1);
    uint64 element = value & elementMask;
    // The number of ones, and the rotation of the run of ones
    uint ones = 0;
    for (uint i = 0; i < elementSize; i++)
        if (((element >> i) & 1) != 0)
            ones++;
    uint64 run = (ones == 64) ? ~(uint64)0 : (((uint64)1 << ones) - 1);
    for (uint rotation = 0; rotation < elementSize; rotation++)
    {
        uint64 rotated = (rotation == 0) ? run :
            (((run >> rotation) | (run << (elementSize - rotation))) & elementMask);
        if (rotated == element)
        {
            n = (elementSize == 64) ? 1 : 0;
            immr = rotation;
            imms = ((~(2 * elementSize - 1)) & 0x3f) | (ones - 1);
            return true;
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////
// The options

// Return the field of an option, by its scheme
uint getOptionValue(uint scheme, uint32 encoding)
{
    switch (scheme)
    {
    case ARM_OPT_BARRIER:
        return ArmOperandCodec::getField(encoding, 8, 4);
    case ARM_OPT_BARRIER_NXS:
        return ArmOperandCodec::getField(encoding, 10, 2);
    case ARM_OPT_ISB:
        return ArmOperandCodec::getField(encoding, 8, 4);
    case ARM_OPT_PREFETCH:
        return ArmOperandCodec::getField(encoding, 0, 5);
    case ARM_OPT_PSTATE:
    {
        uint value = (ArmOperandCodec::getField(encoding, 16, 3) << 3) |
                     ArmOperandCodec::getField(encoding, 5, 3);
        // ALLINT (CRm 000x) and PM (CRm 001x) share op1 and op2
        if ((value == 8) && (ArmOperandCodec::getField(encoding, 9, 3) == 0))
            value = 0x48;
        return value;
    }
    case ARM_OPT_SYS_DC:
    case ARM_OPT_SYS_IC:
    case ARM_OPT_SYS_AT:
    case ARM_OPT_SYS_TLBI:
    case ARM_OPT_SYS_PLBI:
    case ARM_OPT_SYS_BRB:
    case ARM_OPT_SYS_TLBIP:
        return ArmOperandCodec::getField(encoding, 5, 14);
    case ARM_OPT_RPRFM:
        return (ArmOperandCodec::getField(encoding, 15, 1) << 5) |
               (ArmOperandCodec::getField(encoding, 13, 1) << 4) |
               (ArmOperandCodec::getField(encoding, 12, 1) << 3) |
               ArmOperandCodec::getField(encoding, 0, 3);
    case ARM_OPT_STSHH:
        return ArmOperandCodec::getField(encoding, 5, 3);
    case ARM_OPT_BTI:
        return ArmOperandCodec::getField(encoding, 6, 2);
    case ARM_OPT_SVCR:
        return ArmOperandCodec::getField(encoding, 9, 2);
    default:
        return 0;
    }
}

// Set the fields of an option. False if the value doesn't fit
bool setOptionValue(uint scheme, uint value, uint32& encoding)
{
    switch (scheme)
    {
    case ARM_OPT_BARRIER:
    case ARM_OPT_ISB:
        if (value > 15)
            return false;
        ArmOperandCodec::setField(encoding, 8, 4, value);
        return true;
    case ARM_OPT_BARRIER_NXS:
        if (value > 3)
            return false;
        ArmOperandCodec::setField(encoding, 10, 2, value);
        return true;
    case ARM_OPT_PREFETCH:
        if (value > 31)
            return false;
        ArmOperandCodec::setField(encoding, 0, 5, value);
        return true;
    case ARM_OPT_PSTATE:
        // ALLINT shares op1 and op2 with PM
        if (value == 0x48)
            value = 8;
        if (value > 63)
            return false;
        ArmOperandCodec::setField(encoding, 16, 3, value >> 3);
        ArmOperandCodec::setField(encoding, 5, 3, value & 7);
        return true;
    case ARM_OPT_SYS_DC:
    case ARM_OPT_SYS_IC:
    case ARM_OPT_SYS_AT:
    case ARM_OPT_SYS_TLBI:
    case ARM_OPT_SYS_PLBI:
    case ARM_OPT_SYS_BRB:
    case ARM_OPT_SYS_TLBIP:
        if (value > 0x3fff)
            return false;
        ArmOperandCodec::setField(encoding, 5, 14, value);
        return true;
    case ARM_OPT_RPRFM:
        if (value > 63)
            return false;
        ArmOperandCodec::setField(encoding, 15, 1, value >> 5);
        ArmOperandCodec::setField(encoding, 13, 1, value >> 4);
        ArmOperandCodec::setField(encoding, 12, 1, value >> 3);
        ArmOperandCodec::setField(encoding, 0, 3, value & 7);
        return true;
    case ARM_OPT_STSHH:
        if (value > 7)
            return false;
        ArmOperandCodec::setField(encoding, 5, 3, value);
        return true;
    case ARM_OPT_BTI:
        if (value > 3)
            return false;
        ArmOperandCodec::setField(encoding, 6, 2, value);
        return true;
    case ARM_OPT_SVCR:
        if (value > 3)
            return false;
        ArmOperandCodec::setField(encoding, 9, 2, value);
        return true;
    case ARM_OPT_CSYNC:
    case ARM_OPT_DSYNC:
    case ARM_OPT_NB:
    case ARM_OPT_SYS_PREDRES:
        // The operand is implied by the mnemonic (cfp rctx)
        return value == 0;
    default:
        return false;
    }
}

// True if a scheme accepts the numeric form "#imm" in the text
bool isNumericOption(uint scheme)
{
    return (scheme == ARM_OPT_BARRIER) || (scheme == ARM_OPT_PREFETCH) ||
           (scheme == ARM_OPT_RPRFM) || (scheme == ARM_OPT_STSHH);
    // (ARM_OPT_CREG takes the names c0-c15 only)
}

//////////////////////////////////////////////////////////////////////////
// The memory operand

// The extend type of the register offset by the option field (13-15), or
// SHIFT_NONE for the reserved ones
uint getMemoryExtend(uint option)
{
    switch (option)
    {
    case 2: return Operand::EXTEND_UXTW;
    case 3: return Operand::SHIFT_LSL;
    case 6: return Operand::EXTEND_SXTW;
    case 7: return Operand::EXTEND_SXTX;
    default: return Operand::SHIFT_NONE;
    }
}

// The decoded form of a memory operand: the base register, with the stack
// pointer for number 31
uint getBaseId(uint number)
{
    return (number == 31) ? ARM64_REG_SP : (ARM64_REG_X0 + number);
}

bool decodeMemory(const ArmOperandSpec& spec, uint32 encoding, Operand& operand)
{
    uint scale = spec.m_flags & ARM_SPEC_SCALE_MASK;
    operand.m_kind = Operand::KIND_MEMORY;
    operand.m_base = getBaseId(ArmOperandCodec::getField(encoding, spec.m_position, 5));
    // The offsets are written in hexadecimal, a pre index one even if zero
    // (but the [x0]! of the memory copy and set instructions)
    // (the fixed offsets of ldapr / stilp are decimal)
    operand.m_isHex = (spec.m_extra != ARM_ADDR_BASE) && (spec.m_extra != ARM_ADDR_BASE_WRITEBACK) &&
                      (spec.m_extra != ARM_ADDR_FIXED_PRE_INDEX) &&
                      (spec.m_extra != ARM_ADDR_FIXED_POST_INDEX);
    switch (spec.m_extra)
    {
    case ARM_ADDR_UNSIGNED_OFFSET:
        operand.m_offset = (int64)ArmOperandCodec::getField(encoding, 10, 12) << scale;
        return true;
    case ARM_ADDR_UNSCALED:
    case ARM_ADDR_PRE_INDEX:
    case ARM_ADDR_POST_INDEX:
        operand.m_offset = signExtend(ArmOperandCodec::getField(encoding, 12, 9), 9) *
                           ((int64)1 << scale);
        operand.m_addressing = (spec.m_extra == ARM_ADDR_PRE_INDEX) ?
                                    Operand::ADDRESSING_PRE_INDEX :
                               (spec.m_extra == ARM_ADDR_POST_INDEX) ?
                                    Operand::ADDRESSING_POST_INDEX :
                                    Operand::ADDRESSING_OFFSET;
        return true;
    case ARM_ADDR_PAIR_OFFSET:
    case ARM_ADDR_PAIR_PRE_INDEX:
    case ARM_ADDR_PAIR_POST_INDEX:
        operand.m_offset = signExtend(ArmOperandCodec::getField(encoding, 15, 7), 7) *
                           ((int64)1 << scale);
        operand.m_addressing = (spec.m_extra == ARM_ADDR_PAIR_PRE_INDEX) ?
                                    Operand::ADDRESSING_PRE_INDEX :
                               (spec.m_extra == ARM_ADDR_PAIR_POST_INDEX) ?
                                    Operand::ADDRESSING_POST_INDEX :
                                    Operand::ADDRESSING_OFFSET;
        return true;
    case ARM_ADDR_REGISTER_OFFSET:
    {
        uint extend = getMemoryExtend(ArmOperandCodec::getField(encoding, 13, 3));
        if (extend == Operand::SHIFT_NONE)
            return false;
        uint indexNumber = ArmOperandCodec::getField(encoding, 16, 5);
        bool isWide = (extend == Operand::SHIFT_LSL) || (extend == Operand::EXTEND_SXTX);
        operand.m_index = getGprId(indexNumber, isWide, false);
        operand.m_shift = extend;
        operand.m_isExplicit = ArmOperandCodec::getField(encoding, 12, 1) != 0;
        operand.m_shiftAmount = operand.m_isExplicit ? scale : 0;
        return true;
    }
    case ARM_ADDR_BASE:
        return true;
    case ARM_ADDR_BASE_WRITEBACK:
        operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
        return true;
    case ARM_ADDR_PAUTH_OFFSET:
    case ARM_ADDR_PAUTH_PRE_INDEX:
    {
        uint32 field = (ArmOperandCodec::getField(encoding, 22, 1) << 9) |
                       ArmOperandCodec::getField(encoding, 12, 9);
        operand.m_offset = signExtend(field, 10) * 8;
        if (spec.m_extra == ARM_ADDR_PAUTH_PRE_INDEX)
            operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
        return true;
    }
    case ARM_ADDR_FIXED_PRE_INDEX:
        operand.m_offset = -((int64)1 << scale);
        operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
        return true;
    case ARM_ADDR_FIXED_POST_INDEX:
        operand.m_offset = (int64)1 << scale;
        operand.m_addressing = Operand::ADDRESSING_POST_INDEX;
        return true;
    default:
        return false;
    }
}

// Check and set a signed scaled offset
bool encodeScaledSigned(int64 offset, uint scale, uint width, uint position,
                        uint32& encoding)
{
    if ((offset & (((int64)1 << scale) - 1)) != 0)
        return false;
    offset >>= scale;
    int64 limit = (int64)1 << (width - 1);
    if ((offset < -limit) || (offset >= limit))
        return false;
    ArmOperandCodec::setField(encoding, position, width, (uint32)offset);
    return true;
}

bool encodeMemory(const ArmOperandSpec& spec, const Operand& operand, uint32& encoding)
{
    if (operand.m_kind != Operand::KIND_MEMORY)
        return false;
    uint scale = spec.m_flags & ARM_SPEC_SCALE_MASK;

    // The base: x0-x30 or sp
    bool is64, isStackPointer;
    uint number;
    if (!classifyGpr(operand.m_base, is64, number, isStackPointer) || !is64)
        return false;
    if ((number == 31) && !isStackPointer)
        return false;

    // Only the register offset has an index
    bool hasIndex = (operand.m_index != ARM_NO_REGISTER);
    if (hasIndex != (spec.m_extra == ARM_ADDR_REGISTER_OFFSET))
        return false;
    // The writeback forms and the offset forms have to agree
    uint addressing = operand.m_addressing;
    uint expected = Operand::ADDRESSING_OFFSET;
    switch (spec.m_extra)
    {
    case ARM_ADDR_PRE_INDEX:
    case ARM_ADDR_PAIR_PRE_INDEX:
    case ARM_ADDR_BASE_WRITEBACK:
    case ARM_ADDR_PAUTH_PRE_INDEX:
    case ARM_ADDR_FIXED_PRE_INDEX:
        expected = Operand::ADDRESSING_PRE_INDEX;
        break;
    case ARM_ADDR_POST_INDEX:
    case ARM_ADDR_PAIR_POST_INDEX:
    case ARM_ADDR_FIXED_POST_INDEX:
        expected = Operand::ADDRESSING_POST_INDEX;
        break;
    }
    if (addressing != expected)
        return false;
    ArmOperandCodec::setField(encoding, spec.m_position, 5, number);

    int64 offset = operand.m_offset;
    switch (spec.m_extra)
    {
    case ARM_ADDR_UNSIGNED_OFFSET:
        if ((offset < 0) || ((offset & (((int64)1 << scale) - 1)) != 0) ||
            ((offset >> scale) >= 4096))
            return false;
        ArmOperandCodec::setField(encoding, 10, 12, (uint32)(offset >> scale));
        return true;
    case ARM_ADDR_UNSCALED:
    case ARM_ADDR_PRE_INDEX:
    case ARM_ADDR_POST_INDEX:
        return encodeScaledSigned(offset, scale, 9, 12, encoding);
    case ARM_ADDR_PAIR_OFFSET:
    case ARM_ADDR_PAIR_PRE_INDEX:
    case ARM_ADDR_PAIR_POST_INDEX:
        return encodeScaledSigned(offset, scale, 7, 15, encoding);
    case ARM_ADDR_REGISTER_OFFSET:
    {
        // Plain offset, no immediate
        if (offset != 0)
            return false;
        bool isIndex64, isIndexSp;
        uint indexNumber;
        if (!classifyGpr(operand.m_index, isIndex64, indexNumber, isIndexSp) || isIndexSp)
            return false;
        uint option;
        switch (operand.m_shift)
        {
        case Operand::SHIFT_NONE:
        case Operand::SHIFT_LSL:
            if (!isIndex64)
                return false;
            option = 3;
            break;
        case Operand::EXTEND_UXTW:
            if (isIndex64)
                return false;
            option = 2;
            break;
        case Operand::EXTEND_SXTW:
            if (isIndex64)
                return false;
            option = 6;
            break;
        case Operand::EXTEND_SXTX:
            if (!isIndex64)
                return false;
            option = 7;
            break;
        default:
            return false;
        }
        // An amount is the S bit: it has to be the access size
        bool isScaled = operand.m_isExplicit;
        if (isScaled && (operand.m_shiftAmount != scale))
            return false;
        if (!isScaled && (operand.m_shiftAmount != 0))
            return false;
        ArmOperandCodec::setField(encoding, 16, 5, indexNumber);
        ArmOperandCodec::setField(encoding, 13, 3, option);
        ArmOperandCodec::setField(encoding, 12, 1, isScaled ? 1 : 0);
        return true;
    }
    case ARM_ADDR_BASE:
    case ARM_ADDR_BASE_WRITEBACK:
        return offset == 0;
    case ARM_ADDR_PAUTH_OFFSET:
    case ARM_ADDR_PAUTH_PRE_INDEX:
    {
        if (((offset & 7) != 0) || (offset < -4096) || (offset >= 4096))
            return false;
        uint32 field = (uint32)(offset >> 3) & 0x3ff;
        ArmOperandCodec::setField(encoding, 22, 1, field >> 9);
        ArmOperandCodec::setField(encoding, 12, 9, field & 0x1ff);
        return true;
    }
    case ARM_ADDR_FIXED_PRE_INDEX:
        return offset == -((int64)1 << scale);
    case ARM_ADDR_FIXED_POST_INDEX:
        return offset == ((int64)1 << scale);
    default:
        return false;
    }
}

} // anonymous namespace

bool decodeLogicalImmediateValue(uint n, uint immr, uint imms, uint size, uint64& value)
{
    return decodeLogicalImmediate(n, immr, imms, size, value);
}

//////////////////////////////////////////////////////////////////////////
// decode

bool decodeA64(const ArmOperandSpec& spec,
               uint32 encoding,
               const ArmCodecContext& context,
               ArmDecodedOperand& operand)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_GPR:
    {
        uint number = ArmOperandCodec::getField(encoding, spec.m_position, spec.m_extra);
        // The pair register is even, or the zero register
        if (((spec.m_flags & ARM_GPR_PAIR) != 0) && ((number & 1) != 0) &&
            ((number != 31) || ((spec.m_flags & ARM_GPR_PAIR_ZERO) == 0)))
            return false;
        bool is64 = isWideRegister(spec.m_flags, encoding);
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = getGprId(number, is64, (spec.m_flags & ARM_GPR_SP) != 0);
        if ((spec.m_flags & ARM_GPR_PAIR) != 0)
        {
            // casp x0, x1: the register and the next one
            operand.m_kind = Operand::KIND_REGISTER_LIST;
            operand.m_count = 2;
            operand.m_stride = 1;
        }
        if ((spec.m_flags & ARM_GPR_WRITEBACK) != 0)
            operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
        return true;
    }
    case ARM_KIND_A64_TIED_REGISTER:
        // Hidden
        return false;
    case ARM_KIND_A64_FIXED_REGISTER:
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = getGprId(spec.m_extra, true, false);
        return true;
    case ARM_KIND_A64_FPR:
    {
        uint number = ArmOperandCodec::getField(encoding, spec.m_position, 5);
        uint bits;
        switch (spec.m_extra)
        {
        case ARM_FPR_B: bits = 8; break;
        case ARM_FPR_H: bits = 16; break;
        case ARM_FPR_S: bits = 32; break;
        case ARM_FPR_D: bits = 64; break;
        case ARM_FPR_Q: bits = 128; break;
        case ARM_FPR_FTYPE:
            switch (ArmOperandCodec::getField(encoding, 22, 2))
            {
            case 0: bits = 32; break;
            case 1: bits = 64; break;
            case 3: bits = 16; break;
            default: return false;
            }
            break;
        case ARM_FPR_SIZE:
            bits = 8u << ArmOperandCodec::getField(encoding, 22, 2);
            break;
        case ARM_FPR_SZ:
            bits = 32u << ArmOperandCodec::getField(encoding, 22, 1);
            break;
        case ARM_FPR_LOAD_STORE:
        {
            uint size = ArmOperandCodec::getField(encoding, 30, 2);
            if (ArmOperandCodec::getField(encoding, 23, 1) != 0)
            {
                // The 128 bit access is size 0, opc 1x
                if (size != 0)
                    return false;
                bits = 128;
            } else
            {
                bits = 8u << size;
            }
            break;
        }
        case ARM_FPR_LONG_SIZE:
        {
            uint size = ArmOperandCodec::getField(encoding, 22, 2);
            if ((size < 1) || (size > 2))
                return false;
            bits = 16u << size;
            break;
        }
        default:
            return false;
        }
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = getFprId(number, bits);
        return true;
    }
    case ARM_KIND_A64_ARITHMETIC_IMMEDIATE:
        operand.m_kind = Operand::KIND_IMMEDIATE;
        operand.m_immediate = ArmOperandCodec::getField(encoding, 10, 12);
        operand.m_isHex = true;
        if (ArmOperandCodec::getField(encoding, 22, 1) != 0)
        {
            operand.m_shift = Operand::SHIFT_LSL;
            operand.m_shiftAmount = 12;
        }
        return true;
    case ARM_KIND_A64_LOGICAL_IMMEDIATE:
    {
        bool is64 = isWideRegister(spec.m_flags, encoding);
        uint64 value;
        if (!decodeLogicalImmediate(ArmOperandCodec::getField(encoding, 22, 1),
                                    ArmOperandCodec::getField(encoding, 16, 6),
                                    ArmOperandCodec::getField(encoding, 10, 6),
                                    is64 ? 64 : 32, value))
            return false;
        operand.m_kind = Operand::KIND_IMMEDIATE;
        operand.m_isHex = true;
        if (spec.m_extra == 1)
        {
            // The mov alias: the signed value of the register
            operand.m_immediate = is64 ? (int64)value : (int64)(int32)(uint32)value;
        } else
        {
            operand.m_immediate = (int64)value;
            operand.m_isUnsigned = true;
        }
        return true;
    }
    case ARM_KIND_A64_MOVE_WIDE_IMMEDIATE:
    {
        bool is64 = isWideRegister(spec.m_flags, encoding);
        uint hw = ArmOperandCodec::getField(encoding, 21, 2);
        if (!is64 && (hw > 1))
            return false;
        uint64 value = ArmOperandCodec::getField(encoding, 5, 16);
        operand.m_kind = Operand::KIND_IMMEDIATE;
        operand.m_isHex = true;
        switch (spec.m_extra)
        {
        case ARM_MOVE_WIDE_PLAIN:
            operand.m_immediate = (int64)value;
            if (hw != 0)
            {
                operand.m_shift = Operand::SHIFT_LSL;
                operand.m_shiftAmount = hw * 16;
            }
            return true;
        case ARM_MOVE_WIDE_MOVZ:
            // The signed value of the register
            value <<= (hw * 16);
            operand.m_immediate = is64 ? (int64)value : (int64)(int32)(uint32)value;
            return true;
        case ARM_MOVE_WIDE_MOVN:
            value = ~(value << (hw * 16));
            // The signed value of the register
            operand.m_immediate = is64 ? (int64)value : (int64)(int32)(uint32)value;
            return true;
        default:
            return false;
        }
    }
    case ARM_KIND_SHIFTED_REGISTER:
    {
        bool is64 = isWideRegister(spec.m_flags, encoding);
        uint type = ArmOperandCodec::getField(encoding, 22, 2);
        uint amount = ArmOperandCodec::getField(encoding, 10, 6);
        if (((spec.m_flags & ARM_SPEC_NO_ROR) != 0) && (type == 3))
            return false;
        if (!is64 && (amount >= 32))
            return false;
        operand.m_kind = Operand::KIND_SHIFTED_REGISTER;
        operand.m_register = getGprId(ArmOperandCodec::getField(encoding, spec.m_position, 5),
                                      is64, false);
        operand.m_shift = Operand::SHIFT_LSL + type;
        operand.m_shiftAmount = amount;
        return true;
    }
    case ARM_KIND_A64_EXTENDED_REGISTER:
    {
        uint option = ArmOperandCodec::getField(encoding, 13, 3);
        uint amount = ArmOperandCodec::getField(encoding, 10, 3);
        if (amount > 4)
            return false;
        // The index is a 64 bit register for uxtx and sxtx of the 64 bit
        // instructions
        bool isWideIndex = ((option == 3) || (option == 7)) &&
                           (ArmOperandCodec::getField(encoding, 31, 1) != 0);
        operand.m_kind = Operand::KIND_SHIFTED_REGISTER;
        operand.m_register = getGprId(ArmOperandCodec::getField(encoding, spec.m_position, 5),
                                      isWideIndex, false);
        operand.m_shift = Operand::EXTEND_UXTB + option;
        operand.m_shiftAmount = amount;
        // LLVM prints uxtw / uxtx as lsl when the destination or the first
        // source is the stack pointer. Rd is the stack pointer for the
        // instructions which don't set the flags
        bool isSf = ArmOperandCodec::getField(encoding, 31, 1) != 0;
        if (((option == 2) && !isSf) || ((option == 3) && isSf))
        {
            bool isFirstSp = ArmOperandCodec::getField(encoding, 5, 5) == 31;
            bool isDestinationSp = (ArmOperandCodec::getField(encoding, 0, 5) == 31) &&
                                   (ArmOperandCodec::getField(encoding, 29, 1) == 0);
            if (isFirstSp || isDestinationSp)
                operand.m_shift = Operand::SHIFT_LSL;
        }
        return true;
    }
    case ARM_KIND_A64_LSL_REGISTER:
        operand.m_kind = Operand::KIND_SHIFTED_REGISTER;
        operand.m_register = getGprId(ArmOperandCodec::getField(encoding, spec.m_position, 5),
                                      true, false);
        operand.m_shift = Operand::SHIFT_LSL;
        operand.m_shiftAmount = ArmOperandCodec::getField(encoding, 10, spec.m_extra);
        return true;
    case ARM_KIND_A64_BITFIELD_AMOUNT:
    {
        bool is64 = isWideRegister(spec.m_flags, encoding);
        uint size = is64 ? 64 : 32;
        uint immr = ArmOperandCodec::getField(encoding, 16, 6);
        uint imms = ArmOperandCodec::getField(encoding, 10, 6);
        operand.m_kind = Operand::KIND_IMMEDIATE;
        switch (spec.m_extra)
        {
        case ARM_BF_LSL_AMOUNT:
            operand.m_immediate = size - 1 - imms;
            return true;
        case ARM_BF_WIDTH:
            operand.m_immediate = (int64)imms - (int64)immr + 1;
            return true;
        case ARM_BF_LSB_NEGATED:
            operand.m_immediate = (size - immr) % size;
            return true;
        default:
            return false;
        }
    }
    case ARM_KIND_A64_MEMORY:
        return decodeMemory(spec, encoding, operand);
    case ARM_KIND_A64_PC_RELATIVE:
    {
        operand.m_kind = Operand::KIND_BRANCH_TARGET;
        switch (spec.m_extra)
        {
        case ARM_PCREL_ADR:
        case ARM_PCREL_ADRP:
        {
            uint32 field = (ArmOperandCodec::getField(encoding, 5, 19) << 2) |
                           ArmOperandCodec::getField(encoding, 29, 2);
            int64 offset = signExtend(field, 21);
            if (spec.m_extra == ARM_PCREL_ADRP)
                operand.m_immediate = (int64)((context.m_address & ~(uint64)0xfff) +
                                              (uint64)(offset * 4096));
            else
                operand.m_immediate = (int64)(context.m_address + (uint64)offset);
            return true;
        }
        case ARM_PCREL_LITERAL:
            operand.m_immediate = (int64)(context.m_address +
                (uint64)(signExtend(ArmOperandCodec::getField(encoding, 5, 19), 19) * 4));
            return true;
        case ARM_PCREL_NEGATIVE16:
            operand.m_immediate = (int64)(context.m_address -
                (uint64)ArmOperandCodec::getField(encoding, 5, 16) * 4);
            return true;
        default:
            return false;
        }
    }
    case ARM_KIND_A64_NZCV:
        operand.m_kind = Operand::KIND_IMMEDIATE;
        operand.m_immediate = ArmOperandCodec::getField(encoding, 0, 4);
        operand.m_isHex = true;
        return true;
    case ARM_KIND_A64_SYSTEM_REGISTER:
    {
        operand.m_kind = Operand::KIND_SYSTEM_REGISTER;
        operand.m_systemRegister = ArmOperandCodec::getField(encoding, 5, 16);
        // The L bit tells mrs (read) from msr (write)
        const char* name = getSystemRegisterName(operand.m_systemRegister,
                                    ArmOperandCodec::getField(encoding, 21, 1) == 0);
        if (name != NULL)
            operand.m_name = cString(name);
        return true;
    }
    case ARM_KIND_OPTION:
        operand.m_kind = Operand::KIND_OPTION;
        operand.m_optionScheme = spec.m_extra;
        operand.m_option = (spec.m_extra == ARM_OPT_CREG) ?
            ArmOperandCodec::getField(encoding, spec.m_position, 4) :
            getOptionValue(spec.m_extra, encoding);
        return true;
    default:
        // TODO: the Advanced SIMD, SVE and SME kinds
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// encode

bool encodeA64(const ArmOperandSpec& spec,
               const ArmDecodedOperand& operand,
               const ArmCodecContext& context,
               uint32& encoding)
{
    switch (spec.m_kind)
    {
    case ARM_KIND_GPR:
    {
        // The register, or the first register of a pair
        uint id = operand.m_register;
        if ((spec.m_flags & ARM_GPR_PAIR) != 0)
        {
            if ((operand.m_kind != Operand::KIND_REGISTER_LIST) || (operand.m_count != 2))
                return false;
        } else if (operand.m_kind != Operand::KIND_REGISTER)
        {
            return false;
        }
        bool isWriteback = (operand.m_addressing == Operand::ADDRESSING_PRE_INDEX);
        if (isWriteback != ((spec.m_flags & ARM_GPR_WRITEBACK) != 0))
            return false;

        bool is64, isStackPointer;
        uint number;
        if (!classifyGpr(id, is64, number, isStackPointer))
            return false;
        if (isStackPointer && ((spec.m_flags & ARM_GPR_SP) == 0))
            return false;
        if ((number == 31) && !isStackPointer && ((spec.m_flags & ARM_GPR_SP) != 0))
            return false;
        if (((spec.m_flags & ARM_GPR_PAIR) != 0) && ((number & 1) != 0) &&
            ((number != 31) || ((spec.m_flags & ARM_GPR_PAIR_ZERO) == 0)))
            return false;
        // The size
        if ((spec.m_flags & ARM_GPR_X) != 0)
        {
            if (!is64)
                return false;
        } else if ((spec.m_flags & ARM_GPR_W) != 0)
        {
            if (is64)
                return false;
        } else if ((spec.m_flags & ARM_GPR_SF) != 0)
        {
            ArmOperandCodec::setField(encoding, 31, 1, is64 ? 1 : 0);
        } else if ((spec.m_flags & ARM_GPR_OPC22) != 0)
        {
            ArmOperandCodec::setField(encoding, 22, 1, is64 ? 0 : 1);
        } else if (is64)
        {
            return false;
        }
        ArmOperandCodec::setField(encoding, spec.m_position, spec.m_extra, number);
        return true;
    }
    case ARM_KIND_A64_FIXED_REGISTER:
        // Nothing to set: the operand has to be the register
        return (operand.m_kind == Operand::KIND_REGISTER) &&
               (operand.m_register == getGprId(spec.m_extra, true, false));
    case ARM_KIND_A64_TIED_REGISTER:
        // The copy of the register field at m_extra
        ArmOperandCodec::setField(encoding, spec.m_position, 5,
                                  ArmOperandCodec::getField(encoding, spec.m_extra, 5));
        return true;
    case ARM_KIND_A64_FPR:
    {
        if (operand.m_kind != Operand::KIND_REGISTER)
            return false;
        uint bits, number;
        if (!classifyFpr(operand.m_register, bits, number))
            return false;
        switch (spec.m_extra)
        {
        case ARM_FPR_B: if (bits != 8) return false; break;
        case ARM_FPR_H: if (bits != 16) return false; break;
        case ARM_FPR_S: if (bits != 32) return false; break;
        case ARM_FPR_D: if (bits != 64) return false; break;
        case ARM_FPR_Q: if (bits != 128) return false; break;
        case ARM_FPR_FTYPE:
            if (bits == 32)
                ArmOperandCodec::setField(encoding, 22, 2, 0);
            else if (bits == 64)
                ArmOperandCodec::setField(encoding, 22, 2, 1);
            else if (bits == 16)
                ArmOperandCodec::setField(encoding, 22, 2, 3);
            else
                return false;
            break;
        case ARM_FPR_SIZE:
            if (bits == 128)
                return false;
            ArmOperandCodec::setField(encoding, 22, 2, (bits == 8) ? 0 : (bits == 16) ? 1 :
                                                       (bits == 32) ? 2 : 3);
            break;
        case ARM_FPR_SZ:
            if ((bits != 32) && (bits != 64))
                return false;
            ArmOperandCodec::setField(encoding, 22, 1, (bits == 64) ? 1 : 0);
            break;
        case ARM_FPR_LOAD_STORE:
            if (bits == 128)
            {
                ArmOperandCodec::setField(encoding, 30, 2, 0);
                ArmOperandCodec::setField(encoding, 23, 1, 1);
            } else
            {
                ArmOperandCodec::setField(encoding, 30, 2, (bits == 8) ? 0 : (bits == 16) ? 1 :
                                                           (bits == 32) ? 2 : 3);
                ArmOperandCodec::setField(encoding, 23, 1, 0);
            }
            break;
        case ARM_FPR_LONG_SIZE:
            if ((bits != 32) && (bits != 64))
                return false;
            ArmOperandCodec::setField(encoding, 22, 2, (bits == 32) ? 1 : 2);
            break;
        default:
            return false;
        }
        ArmOperandCodec::setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case ARM_KIND_A64_ARITHMETIC_IMMEDIATE:
    {
        if (operand.m_kind != Operand::KIND_IMMEDIATE)
            return false;
        int64 value = operand.m_immediate;
        bool isShifted = false;
        if (operand.m_shift == Operand::SHIFT_LSL)
        {
            if (operand.m_shiftAmount == 12)
                isShifted = true;
            else if (operand.m_shiftAmount != 0)
                return false;
        } else if (operand.m_shift != Operand::SHIFT_NONE)
        {
            return false;
        }
        if ((value < 0) || (value >= 4096))
        {
            // #0x1000 is #1, lsl #12
            if (isShifted || (value < 0) || ((value & 0xfff) != 0) || (value >= 0x1000000))
                return false;
            value >>= 12;
            isShifted = true;
        }
        ArmOperandCodec::setField(encoding, 10, 12, (uint32)value);
        ArmOperandCodec::setField(encoding, 22, 1, isShifted ? 1 : 0);
        return true;
    }
    case ARM_KIND_A64_LOGICAL_IMMEDIATE:
    {
        if ((operand.m_kind != Operand::KIND_IMMEDIATE) ||
            (operand.m_shift != Operand::SHIFT_NONE))
            return false;
        // The size is a flag of the spec, or the sf bit set by the register
        uint size;
        if ((spec.m_flags & ARM_GPR_X) != 0)
            size = 64;
        else if ((spec.m_flags & ARM_GPR_W) != 0)
            size = 32;
        else
            size = (ArmOperandCodec::getField(encoding, 31, 1) != 0) ? 64 : 32;
        int64 value = operand.m_immediate;
        if (size == 32)
        {
            // Accept the 32 bit values and the sign extended ones
            if ((value < -((int64)1 << 31)) || (value >= ((int64)1 << 32)))
                return false;
        }
        uint n, immr, imms;
        if (!encodeLogicalImmediate((uint64)value, size, n, immr, imms))
            return false;
        ArmOperandCodec::setField(encoding, 22, 1, n);
        ArmOperandCodec::setField(encoding, 16, 6, immr);
        ArmOperandCodec::setField(encoding, 10, 6, imms);
        return true;
    }
    case ARM_KIND_A64_MOVE_WIDE_IMMEDIATE:
    {
        if (operand.m_kind != Operand::KIND_IMMEDIATE)
            return false;
        uint size;
        if ((spec.m_flags & ARM_GPR_X) != 0)
            size = 64;
        else if ((spec.m_flags & ARM_GPR_W) != 0)
            size = 32;
        else
            size = (ArmOperandCodec::getField(encoding, 31, 1) != 0) ? 64 : 32;
        uint64 value = (uint64)operand.m_immediate;
        if (spec.m_extra == ARM_MOVE_WIDE_PLAIN)
        {
            // #imm16{, lsl #shift}
            uint shift = 0;
            if (operand.m_shift == Operand::SHIFT_LSL)
                shift = operand.m_shiftAmount;
            else if (operand.m_shift != Operand::SHIFT_NONE)
                return false;
            if (((shift % 16) != 0) || (shift >= size) || (operand.m_immediate < 0) ||
                (operand.m_immediate > 0xffff))
                return false;
            ArmOperandCodec::setField(encoding, 5, 16, (uint32)value);
            ArmOperandCodec::setField(encoding, 21, 2, shift / 16);
            return true;
        }
        if (operand.m_shift != Operand::SHIFT_NONE)
            return false;
        // The alias: the whole value of the register
        if (size == 32)
        {
            if ((operand.m_immediate < -((int64)1 << 31)) || (operand.m_immediate >= ((int64)1 << 32)))
                return false;
            value &= 0xFFFFFFFFu;
        }
        if (spec.m_extra == ARM_MOVE_WIDE_MOVN)
        {
            value = ~value;
            if (size == 32)
                value &= 0xFFFFFFFFu;
        }
        // Find the chunk. Zero is movz #0 (lsl #0 takes precedence)
        for (uint shift = 0; shift < size; shift += 16)
            if ((value & ~((uint64)0xffff << shift)) == 0)
            {
                ArmOperandCodec::setField(encoding, 5, 16, (uint32)(value >> shift));
                ArmOperandCodec::setField(encoding, 21, 2, shift / 16);
                return true;
            }
        return false;
    }
    case ARM_KIND_SHIFTED_REGISTER:
    {
        if ((operand.m_kind != Operand::KIND_SHIFTED_REGISTER) &&
            (operand.m_kind != Operand::KIND_REGISTER))
            return false;
        bool is64, isStackPointer;
        uint number;
        if (!classifyGpr(operand.m_register, is64, number, isStackPointer) || isStackPointer)
            return false;
        // The size of the register has to be the one of the instruction
        if ((spec.m_flags & ARM_GPR_X) != 0)
        {
            if (!is64)
                return false;
        } else if ((spec.m_flags & ARM_GPR_W) != 0)
        {
            if (is64)
                return false;
        } else if ((spec.m_flags & ARM_GPR_SF) != 0)
        {
            ArmOperandCodec::setField(encoding, 31, 1, is64 ? 1 : 0);
        }
        uint type = 0;
        uint amount = 0;
        if (operand.m_kind == Operand::KIND_SHIFTED_REGISTER)
        {
            if ((operand.m_shift < Operand::SHIFT_LSL) || (operand.m_shift > Operand::SHIFT_ROR))
                return false;
            type = operand.m_shift - Operand::SHIFT_LSL;
            amount = operand.m_shiftAmount;
        }
        if (((spec.m_flags & ARM_SPEC_NO_ROR) != 0) && (type == 3))
            return false;
        if (amount >= (is64 ? 64u : 32u))
            return false;
        ArmOperandCodec::setField(encoding, spec.m_position, 5, number);
        ArmOperandCodec::setField(encoding, 22, 2, type);
        ArmOperandCodec::setField(encoding, 10, 6, amount);
        return true;
    }
    case ARM_KIND_A64_EXTENDED_REGISTER:
    {
        if ((operand.m_kind != Operand::KIND_SHIFTED_REGISTER) &&
            (operand.m_kind != Operand::KIND_REGISTER))
            return false;
        bool is64, isStackPointer;
        uint number;
        if (!classifyGpr(operand.m_register, is64, number, isStackPointer) || isStackPointer)
            return false;
        uint option;
        uint amount = 0;
        if (operand.m_kind == Operand::KIND_REGISTER)
        {
            // The register of the instruction size: the lsl form (uxtx for x
            // registers, uxtw for w registers)
            option = is64 ? 3 : 2;
        } else
        {
            amount = operand.m_shiftAmount;
            if (operand.m_shift == Operand::SHIFT_LSL)
            {
                // lsl: uxtx for x registers, uxtw for w registers
                option = is64 ? 3 : 2;
            } else if ((operand.m_shift >= Operand::EXTEND_UXTB) &&
                       (operand.m_shift <= Operand::EXTEND_SXTX))
            {
                option = operand.m_shift - Operand::EXTEND_UXTB;
            } else
            {
                return false;
            }
            // x registers go with uxtx and sxtx of the 64 bit instructions only
            bool needsWide = ((option == 3) || (option == 7)) &&
                             (ArmOperandCodec::getField(encoding, 31, 1) != 0);
            if (needsWide != is64)
                return false;
        }
        if (amount > 4)
            return false;
        ArmOperandCodec::setField(encoding, spec.m_position, 5, number);
        ArmOperandCodec::setField(encoding, 13, 3, option);
        ArmOperandCodec::setField(encoding, 10, 3, amount);
        return true;
    }
    case ARM_KIND_A64_LSL_REGISTER:
    {
        if ((operand.m_kind != Operand::KIND_SHIFTED_REGISTER) &&
            (operand.m_kind != Operand::KIND_REGISTER))
            return false;
        bool is64, isStackPointer;
        uint number;
        if (!classifyGpr(operand.m_register, is64, number, isStackPointer) || !is64 ||
            isStackPointer)
            return false;
        uint amount = 0;
        if (operand.m_kind == Operand::KIND_SHIFTED_REGISTER)
        {
            if (operand.m_shift != Operand::SHIFT_LSL)
                return false;
            amount = operand.m_shiftAmount;
        }
        if (amount >= (1u << spec.m_extra))
            return false;
        ArmOperandCodec::setField(encoding, spec.m_position, 5, number);
        ArmOperandCodec::setField(encoding, 10, spec.m_extra, amount);
        return true;
    }
    case ARM_KIND_A64_BITFIELD_AMOUNT:
    {
        if ((operand.m_kind != Operand::KIND_IMMEDIATE) ||
            (operand.m_shift != Operand::SHIFT_NONE))
            return false;
        uint size;
        if ((spec.m_flags & ARM_GPR_X) != 0)
            size = 64;
        else if ((spec.m_flags & ARM_GPR_W) != 0)
            size = 32;
        else
            size = (ArmOperandCodec::getField(encoding, 31, 1) != 0) ? 64 : 32;
        int64 value = operand.m_immediate;
        switch (spec.m_extra)
        {
        case ARM_BF_LSL_AMOUNT:
            if ((value < 0) || (value >= (int64)size))
                return false;
            ArmOperandCodec::setField(encoding, 16, 6, (size - (uint)value) % size);
            ArmOperandCodec::setField(encoding, 10, 6, size - 1 - (uint)value);
            return true;
        case ARM_BF_LSB_NEGATED:
            if ((value < 0) || (value >= (int64)size))
                return false;
            ArmOperandCodec::setField(encoding, 16, 6, (size - (uint)value) % size);
            return true;
        case ARM_BF_WIDTH:
        {
            // The lsb is in immr already
            int64 lsb = ArmOperandCodec::getField(encoding, 16, 6);
            if ((value < 1) || (lsb + value > (int64)size))
                return false;
            ArmOperandCodec::setField(encoding, 10, 6, (uint32)(lsb + value - 1));
            return true;
        }
        default:
            return false;
        }
    }
    case ARM_KIND_A64_MEMORY:
        return encodeMemory(spec, operand, encoding);
    case ARM_KIND_A64_PC_RELATIVE:
    {
        uint64 target;
        if (!getBranchTarget(operand, context, target))
            return false;
        switch (spec.m_extra)
        {
        case ARM_PCREL_ADR:
        case ARM_PCREL_ADRP:
        {
            int64 offset;
            if (spec.m_extra == ARM_PCREL_ADRP)
                offset = (int64)((target & ~(uint64)0xfff) - (context.m_address & ~(uint64)0xfff)) / 4096;
            else
                offset = (int64)(target - context.m_address);
            int64 limit = (int64)1 << 20;
            if ((offset < -limit) || (offset >= limit))
                return false;
            uint32 field = (uint32)offset & 0x1fffff;
            ArmOperandCodec::setField(encoding, 29, 2, field & 3);
            ArmOperandCodec::setField(encoding, 5, 19, field >> 2);
            return true;
        }
        case ARM_PCREL_NEGATIVE16:
        {
            int64 offset = (int64)(context.m_address - target);
            if (((offset & 3) != 0) || (offset < 0) || (offset >= ((int64)1 << 18)))
                return false;
            ArmOperandCodec::setField(encoding, 5, 16, (uint32)(offset / 4));
            return true;
        }
        case ARM_PCREL_LITERAL:
        {
            int64 offset = (int64)(target - context.m_address);
            if ((offset & 3) != 0)
                return false;
            offset /= 4;
            if ((offset < -((int64)1 << 18)) || (offset >= ((int64)1 << 18)))
                return false;
            ArmOperandCodec::setField(encoding, 5, 19, (uint32)offset);
            return true;
        }
        default:
            return false;
        }
    }
    case ARM_KIND_A64_NZCV:
        if ((operand.m_kind != Operand::KIND_IMMEDIATE) || (operand.m_shift != Operand::SHIFT_NONE) ||
            (operand.m_immediate < 0) || (operand.m_immediate > 15))
            return false;
        ArmOperandCodec::setField(encoding, 0, 4, (uint32)operand.m_immediate);
        return true;
    case ARM_KIND_A64_SYSTEM_REGISTER:
    {
        uint32 packed;
        if (operand.m_kind == Operand::KIND_SYSTEM_REGISTER)
            packed = operand.m_systemRegister;
        else if (operand.m_kind == Operand::KIND_NAME)
        {
            // The L bit tells mrs from msr
            bool isWrite = ArmOperandCodec::getField(encoding, 21, 1) == 0;
            if (!findSystemRegister(operand.m_name, isWrite, packed))
                return false;
        } else
            return false;
        ArmOperandCodec::setField(encoding, 5, 16, packed);
        return true;
    }
    case ARM_KIND_OPTION:
    {
        uint value;
        // A word which is a register name too (c, za) is a name here
        ArmDecodedOperand named;
        const ArmDecodedOperand* source = &operand;
        if (operand.m_kind == Operand::KIND_REGISTER)
        {
            const char* registerName = getRegisterName(ArmInstructionSet::ARM_A64,
                                                       operand.m_register);
            if (registerName == NULL)
                return false;
            named.m_kind = Operand::KIND_NAME;
            named.m_name = cString(registerName);
            source = &named;
        }
        if (source->m_kind == Operand::KIND_NAME)
        {
            if (spec.m_extra == ARM_OPT_CREG)
            {
                // c0-c15
                uint number;
                if (!parseNumberedName(source->m_name, "c", 15, number))
                    return false;
                value = number;
            } else if (!findOption(spec.m_extra, source->m_name, value))
                return false;
        } else if (operand.m_kind == Operand::KIND_OPTION)
        {
            if (operand.m_optionScheme != spec.m_extra)
                return false;
            value = operand.m_option;
        } else if ((operand.m_kind == Operand::KIND_IMMEDIATE) &&
                   (operand.m_shift == Operand::SHIFT_NONE) &&
                   isNumericOption(spec.m_extra) &&
                   (operand.m_immediate >= 0))
        {
            value = (uint)operand.m_immediate;
        } else
        {
            return false;
        }
        if (spec.m_extra == ARM_OPT_CREG)
        {
            if (value > 15)
                return false;
            ArmOperandCodec::setField(encoding, spec.m_position, 4, value);
            return true;
        }
        if (!setOptionValue(spec.m_extra, value, encoding))
            return false;
        // The fixed bits of the entry (ALLINT / PM) must give back the value
        if ((spec.m_extra == ARM_OPT_PSTATE) &&
            (getOptionValue(ARM_OPT_PSTATE, encoding) != value))
            return false;
        return true;
    }
    default:
        // TODO: the Advanced SIMD, SVE and SME kinds
        return false;
    }
}

} // namespace armdis
