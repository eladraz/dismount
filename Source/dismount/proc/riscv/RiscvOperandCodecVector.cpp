#include "dismount/dismount.h"
/*
 * RiscvOperandCodecVector.cpp
 *
 * Implementation file
 *
 * The codec of the vector kinds: the registers (with the alignment of the
 * register groups of vmv2r.v, vl4re8.v and vs8r.v), v0, the mask and the
 * vtype of vsetvli / vsetivli, as LLVM prints and reads it. The constraints of
 * RiscvVectorRegisterFlags (the masked destination, the overlap of the
 * register groups) which LLVM checks when it assembles are
 * RiscvOperandCodec::checkAssemblerConstraints: the decoder never checks them.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/array.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvOperandCodec.h"
#include "dismount/proc/riscv/RiscvOperandCodecInternal.h"

namespace riscvdis {

namespace {

const char* const gSew[] = { "e8", "e16", "e32", "e64" };
// By vlmul: 0 m1, 1 m2, 2 m4, 3 m8, 4 reserved, 5 mf8, 6 mf4, 7 mf2
const char* const gLmul[] = { "m1", "m2", "m4", "m8", NULL, "mf8", "mf4", "mf2" };

// The bits of the vtype
enum {
    VTYPE_ALTFMT = 0x100,
    VTYPE_MA = 0x80,
    VTYPE_TA = 0x40
};

// The log2 of the alignment of a register of the class of the spec (m_extra)
uint alignmentOf(const RiscvOperandSpec& spec)
{
    return spec.m_extra & 7;
}

// Parse a decimal number of digits only (leading zeros are fine, as LLVM's
// getAsInteger). Return false if there are no digits or another character
bool parseDigits(const cString& text, uint start, uint& value)
{
    if (start >= text.length())
        return false;
    value = 0;
    for (uint i = start; i < text.length(); i++)
    {
        if ((text[i] < XSTL_CHAR('0')) || (text[i] > XSTL_CHAR('9')))
            return false;
        value = value * 10 + (uint)(text[i] - XSTL_CHAR('0'));
        // (bigger than any valid value)
        if (value > 1000000)
            return false;
    }
    return true;
}

// Parse "e8", "e16alt"... Set the vsew and the altfmt bits of the vtype
bool parseSew(const cString& word, uint& vtype)
{
    if ((word.length() < 2) || (word[0] != XSTL_CHAR('e')))
        return false;
    bool isAlt = false;
    cString digits = word.part(1, word.length());
    if ((digits.length() > 3) && (digits.part(digits.length() - 3, digits.length()) == "alt"))
    {
        isAlt = true;
        digits = digits.left(digits.length() - 3);
    }
    uint sew;
    if (!parseDigits(digits, 0, sew))
        return false;
    uint vsew;
    switch (sew)
    {
    case 8:  vsew = 0; break;
    case 16: vsew = 1; break;
    case 32: vsew = 2; break;
    case 64: vsew = 3; break;
    default: return false;
    }
    // (the alternate formats exist for 8 and 16 bit elements)
    if (isAlt && (vsew > 1))
        return false;
    vtype |= vsew << 3;
    if (isAlt)
        vtype |= VTYPE_ALTFMT;
    return true;
}

// Parse "m1", "mf8"... Set the vlmul bits of the vtype
bool parseLmul(const cString& word, uint& vtype)
{
    if ((word.length() < 2) || (word[0] != XSTL_CHAR('m')))
        return false;
    bool isFraction = (word[1] == XSTL_CHAR('f'));
    uint value;
    if (!parseDigits(word, isFraction ? 2 : 1, value))
        return false;
    uint vlmul;
    if (isFraction)
    {
        switch (value)
        {
        case 2: vlmul = 7; break;
        case 4: vlmul = 6; break;
        case 8: vlmul = 5; break;
        default: return false;
        }
    } else
    {
        switch (value)
        {
        case 1: vlmul = 0; break;
        case 2: vlmul = 1; break;
        case 4: vlmul = 2; break;
        case 8: vlmul = 3; break;
        default: return false;
        }
    }
    vtype |= vlmul;
    return true;
}

// True if the register 'source' (a vector register number) is one of the
// registers of the destination: its first register and the (tuple - 1) which
// follow
bool overlaps(uint destination, uint tuple, uint source)
{
    return (source >= destination) && (source < destination + ((tuple == 0) ? 1 : tuple));
}

} // anonymous namespace

cString formatVtype(uint vtype)
{
    uint vlmul = vtype & 7;
    uint vsew = (vtype >> 3) & 7;
    bool isAlt = (vtype & VTYPE_ALTFMT) != 0;
    // The reserved values are printed as the number (the alternate formats
    // are 8 and 16 bit elements)
    if ((vtype >> 9) != 0 || vlmul == 4 || vsew > 3 || (isAlt && vsew > 1))
        return cString((uint32)vtype);
    return cString(gSew[vsew]) + (isAlt ? "alt" : "") + ", " + gLmul[vlmul] + ", " +
           (((vtype & VTYPE_TA) != 0) ? "ta" : "tu") + ", " +
           (((vtype & VTYPE_MA) != 0) ? "ma" : "mu");
}

bool parseVtype(const cString& text, uint& vtype)
{
    // LLVM reads, in this order, the optional words "e<sew>[alt]",
    // "m<lmul>", "ta" | "tu" and "ma" | "mu", each at most once, comma
    // separated (e8, m1, ta, ma; the defaults are e8, m1, tu, mu: "ta" is
    // e8, m1, ta, mu). One comma after the last word is fine
    cArray<cString> words;
    uint start = 0;
    for (uint i = 0; i <= text.length(); i++)
    {
        if ((i == text.length()) || (text[i] == XSTL_CHAR(',')))
        {
            cString word = text.part(start, i);
            word.trim();
            words.append(word);
            start = i + 1;
        }
    }
    uint value = 0;
    // The last class which was read: 0 none, 1 sew, 2 lmul, 3 tail, 4 mask
    uint last = 0;
    for (uint index = 0; index < words.getSize(); index++)
    {
        const cString& word = words[index];
        if ((index + 1 == words.getSize()) && (index != 0) && (word.length() == 0))
            // (the comma after the last word)
            break;
        if (last < 1 && parseSew(word, value))
            last = 1;
        else if (last < 2 && parseLmul(word, value))
            last = 2;
        else if (last < 3 && (word == "ta" || word == "tu"))
        {
            if (word == "ta")
                value |= VTYPE_TA;
            last = 3;
        } else if (last < 4 && (word == "ma" || word == "mu"))
        {
            if (word == "ma")
                value |= VTYPE_MA;
            last = 4;
        } else
            return false;
    }
    vtype = value;
    return true;
}

bool decodeVectorKind(const RiscvOperandSpec& spec, uint32 encoding,
                      RiscvDecodedOperand& operand)
{
    switch (spec.m_kind)
    {
    case RISCV_KIND_VR:
    {
        uint number = RiscvOperandCodec::getField(encoding, spec.m_position, 5);
        // The first register of a group of 2, 4 or 8 registers
        if ((number & ((1u << alignmentOf(spec)) - 1)) != 0)
            return false;
        operand.m_kind = RiscvDecodedOperand::KIND_REGISTER;
        operand.m_register = RISCV_REG_V0 + number;
        return true;
    }
    case RISCV_KIND_VR_V0:
        operand.m_kind = RiscvDecodedOperand::KIND_REGISTER;
        operand.m_register = RISCV_REG_V0;
        return true;
    case RISCV_KIND_VMASK:
        operand.m_kind = RiscvDecodedOperand::KIND_VECTOR_MASK;
        return true;
    case RISCV_KIND_VTYPE:
        operand.m_kind = RiscvDecodedOperand::KIND_VTYPE;
        operand.m_vtype = RiscvOperandCodec::getField(encoding, 20, spec.m_extra);
        return true;
    default:
        CHECK_FAIL();
    }
}

bool encodeVectorKind(const RiscvOperandSpec& spec,
                      const RiscvDecodedOperand& operand,
                      uint32& encoding)
{
    switch (spec.m_kind)
    {
    case RISCV_KIND_VR:
    {
        if ((operand.m_kind != RiscvDecodedOperand::KIND_REGISTER) ||
            (operand.m_register < RISCV_REG_V0) || (operand.m_register > RISCV_REG_V31))
            return false;
        uint number = operand.m_register - RISCV_REG_V0;
        if ((number & ((1u << alignmentOf(spec)) - 1)) != 0)
            return false;
        RiscvOperandCodec::setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case RISCV_KIND_VR_V0:
        return (operand.m_kind == RiscvDecodedOperand::KIND_REGISTER) &&
               (operand.m_register == RISCV_REG_V0);
    case RISCV_KIND_VMASK:
        if (operand.m_kind == RiscvDecodedOperand::KIND_NONE)
            RiscvOperandCodec::setField(encoding, spec.m_position, 1, 1);
        else if (operand.m_kind == RiscvDecodedOperand::KIND_VECTOR_MASK)
            RiscvOperandCodec::setField(encoding, spec.m_position, 1, 0);
        else
            return false;
        return true;
    case RISCV_KIND_VTYPE:
    {
        uint vtype;
        if (operand.m_kind == RiscvDecodedOperand::KIND_VTYPE)
            vtype = operand.m_vtype;
        else if (operand.m_kind == RiscvDecodedOperand::KIND_IMMEDIATE)
        {
            if ((operand.m_immediate < 0) || (operand.m_immediate >= (1 << spec.m_extra)))
                return false;
            vtype = (uint)operand.m_immediate;
        } else if (operand.m_kind == RiscvDecodedOperand::KIND_NAME)
        {
            // (LLVM reads the words in lowercase only)
            if (operand.m_immediate != 0)
                return false;
            vtype = 0;
            if (!parseVtype(operand.m_name, vtype))
                return false;
            // (the words never set the high bits of the 10 bit vsetivli
            // immediate: the altfmt bit is bit 8)
            if (vtype >= (1u << spec.m_extra))
                return false;
        } else
            return false;
        RiscvOperandCodec::setField(encoding, 20, spec.m_extra, vtype);
        return true;
    }
    default:
        CHECK_FAIL();
    }
}

} // namespace riscvdis

bool RiscvOperandCodec::checkAssemblerConstraints(const riscvdis::RiscvEncodingEntry& entry,
                                                  uint32 encoding)
{
    using namespace riscvdis;
    const RiscvOperandSpec* destination = NULL;
    bool hasMask = false;
    for (uint i = 0; i < RISCV_MAX_OPERAND_SPECS; i++)
    {
        const RiscvOperandSpec& spec = entry.m_operands[i];
        if (spec.m_kind == RISCV_KIND_NONE)
            break;
        // (the destination is the one vector register which has the flags)
        if ((spec.m_kind == RISCV_KIND_VR) && (spec.m_flags != 0) && (destination == NULL))
            destination = &spec;
        if (spec.m_kind == RISCV_KIND_VMASK)
            hasMask = true;
    }
    if (destination == NULL)
        return true;
    uint number = getField(encoding, destination->m_position, 5);
    uint tuple = (destination->m_extra >> 3) & 15;
    uint flags = destination->m_flags;
    if (((flags & RISCV_VR_NOT_V0) != 0) && overlaps(number, tuple, 0))
        return false;
    // (the mask operand is v0 when vm = 0)
    if (hasMask && ((flags & RISCV_VR_NOT_MASK_WHEN_MASKED) != 0) &&
        (getField(encoding, 25, 1) == 0) && overlaps(number, tuple, 0))
        return false;
    if (((flags & RISCV_VR_NO_VS2_OVERLAP) != 0) &&
        overlaps(number, tuple, getField(encoding, 20, 5)))
        return false;
    if (((flags & RISCV_VR_NO_VS1_OVERLAP) != 0) &&
        overlaps(number, tuple, getField(encoding, 15, 5)))
        return false;
    return true;
}
