#include "dismount/dismount.h"
/*
 * RiscvOperandCodec.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
// stdio.h (sscanf), not stdlib.h: the glibc stdlib.h declares a type named
// uint, which xStl defines as a macro
#include <stdio.h>
#include <string.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/sarray.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvOperandCodec.h"
#include "dismount/proc/riscv/RiscvOperandCodecInternal.h"

using namespace riscvdis;

namespace riscvdis {

// The values of fli, 32 entries (Zfa): the text is LLVM's
const RiscvFliValue gRiscvFliValues[32] = {
    { "-1.0", 0xbc00, 0xbf800000u, 0xbff0000000000000ull },
    { "min", 0x0400, 0x00800000u, 0x0010000000000000ull },
    { "1.52587890625e-05", 0x0100, 0x37800000u, 0x3ef0000000000000ull },
    { "3.0517578125e-05", 0x0200, 0x38000000u, 0x3f00000000000000ull },
    { "0.00390625", 0x1c00, 0x3b800000u, 0x3f70000000000000ull },
    { "0.0078125", 0x2000, 0x3c000000u, 0x3f80000000000000ull },
    { "0.0625", 0x2c00, 0x3d800000u, 0x3fb0000000000000ull },
    { "0.125", 0x3000, 0x3e000000u, 0x3fc0000000000000ull },
    { "0.25", 0x3400, 0x3e800000u, 0x3fd0000000000000ull },
    { "0.3125", 0x3500, 0x3ea00000u, 0x3fd4000000000000ull },
    { "0.375", 0x3600, 0x3ec00000u, 0x3fd8000000000000ull },
    { "0.4375", 0x3700, 0x3ee00000u, 0x3fdc000000000000ull },
    { "0.5", 0x3800, 0x3f000000u, 0x3fe0000000000000ull },
    { "0.625", 0x3900, 0x3f200000u, 0x3fe4000000000000ull },
    { "0.75", 0x3a00, 0x3f400000u, 0x3fe8000000000000ull },
    { "0.875", 0x3b00, 0x3f600000u, 0x3fec000000000000ull },
    { "1.0", 0x3c00, 0x3f800000u, 0x3ff0000000000000ull },
    { "1.25", 0x3d00, 0x3fa00000u, 0x3ff4000000000000ull },
    { "1.5", 0x3e00, 0x3fc00000u, 0x3ff8000000000000ull },
    { "1.75", 0x3f00, 0x3fe00000u, 0x3ffc000000000000ull },
    { "2.0", 0x4000, 0x40000000u, 0x4000000000000000ull },
    { "2.5", 0x4100, 0x40200000u, 0x4004000000000000ull },
    { "3.0", 0x4200, 0x40400000u, 0x4008000000000000ull },
    { "4.0", 0x4400, 0x40800000u, 0x4010000000000000ull },
    { "8.0", 0x4800, 0x41000000u, 0x4020000000000000ull },
    { "16.0", 0x4c00, 0x41800000u, 0x4030000000000000ull },
    { "128.0", 0x5800, 0x43000000u, 0x4060000000000000ull },
    { "256.0", 0x5c00, 0x43800000u, 0x4070000000000000ull },
    { "32768.0", 0x7800, 0x47000000u, 0x40e0000000000000ull },
    { "65536.0", 0x7c00, 0x47800000u, 0x40f0000000000000ull },
    { "inf", 0x7c00, 0x7f800000u, 0x7ff0000000000000ull },
    { "nan", 0x7e00, 0x7fc00000u, 0x7ff8000000000000ull }
};

int64 signExtend(uint64 value, uint bits)
{
    uint64 sign = (uint64)1 << (bits - 1);
    value &= (sign << 1) - 1;
    return (int64)((value ^ sign) - sign);
}

// The bits of a scheme which the immediate occupies in the 32 bit instructions
static uint32 getBits(uint32 encoding, uint hi, uint lo)
{
    return (encoding >> lo) & ((1u << (hi - lo + 1)) - 1);
}

bool isUnsignedScheme(uint scheme)
{
    switch (scheme)
    {
    case RISCV_IMM_U:
    case RISCV_IMM_CSR_UIMM5:
    case RISCV_IMM_AES_BS:
    case RISCV_IMM_AES_RNUM:
    case RISCV_IMM_LPAD:
    case RISCV_IMM_V_UIMM5:
    case RISCV_IMM_V_UIMM6:
    case RISCV_IMM_V_AVL:
    case RISCV_IMM_CI_LUI:
        return true;
    default:
        return false;
    }
}

bool decodeImmediate(uint scheme, uint32 e, int64& value)
{
    switch (scheme)
    {
    case RISCV_IMM_I:
        value = signExtend(getBits(e, 31, 20), 12);
        return true;
    case RISCV_IMM_S:
        value = signExtend((getBits(e, 31, 25) << 5) | getBits(e, 11, 7), 12);
        return true;
    case RISCV_IMM_B:
        value = signExtend((getBits(e, 31, 31) << 12) | (getBits(e, 7, 7) << 11) |
                           (getBits(e, 30, 25) << 5) | (getBits(e, 11, 8) << 1), 13);
        return true;
    case RISCV_IMM_U:
        value = getBits(e, 31, 12);
        return true;
    case RISCV_IMM_J:
        value = signExtend((getBits(e, 31, 31) << 20) | (getBits(e, 19, 12) << 12) |
                           (getBits(e, 20, 20) << 11) | (getBits(e, 30, 21) << 1), 21);
        return true;
    case RISCV_IMM_CSR_UIMM5:
        value = getBits(e, 19, 15);
        return true;
    case RISCV_IMM_PREFETCH:
        value = signExtend(getBits(e, 31, 25) << 5, 12);
        return true;
    case RISCV_IMM_AES_BS:
        value = getBits(e, 31, 30);
        return true;
    case RISCV_IMM_AES_RNUM:
        // (LLVM's decoder accepts 11-15, only the assembler checks the range)
        value = getBits(e, 23, 20);
        return true;
    case RISCV_IMM_LPAD:
        value = getBits(e, 31, 12);
        return true;
    case RISCV_IMM_V_SIMM5:
        value = signExtend(getBits(e, 19, 15), 5);
        return true;
    case RISCV_IMM_V_UIMM5:
    case RISCV_IMM_V_AVL:
        value = getBits(e, 19, 15);
        return true;
    case RISCV_IMM_V_UIMM6:
        value = (getBits(e, 26, 26) << 5) | getBits(e, 19, 15);
        return true;
    case RISCV_IMM_CI:
        value = signExtend((getBits(e, 12, 12) << 5) | getBits(e, 6, 2), 6);
        return true;
    case RISCV_IMM_CI_LUI:
        // Printed as the 20 bit value of lui
        value = signExtend((getBits(e, 12, 12) << 5) | getBits(e, 6, 2), 6) & 0xfffff;
        return true;
    case RISCV_IMM_CI_ADDI16SP:
        value = signExtend((getBits(e, 12, 12) << 9) | (getBits(e, 4, 3) << 7) |
                           (getBits(e, 5, 5) << 6) | (getBits(e, 2, 2) << 5) |
                           (getBits(e, 6, 6) << 4), 10);
        return true;
    case RISCV_IMM_CIW:
        value = (getBits(e, 10, 7) << 6) | (getBits(e, 12, 11) << 4) |
                (getBits(e, 5, 5) << 3) | (getBits(e, 6, 6) << 2);
        return true;
    case RISCV_IMM_CI_LWSP:
        value = (getBits(e, 3, 2) << 6) | (getBits(e, 12, 12) << 5) | (getBits(e, 6, 4) << 2);
        return true;
    case RISCV_IMM_CI_LDSP:
        value = (getBits(e, 4, 2) << 6) | (getBits(e, 12, 12) << 5) | (getBits(e, 6, 5) << 3);
        return true;
    case RISCV_IMM_CSS_SWSP:
        value = (getBits(e, 8, 7) << 6) | (getBits(e, 12, 9) << 2);
        return true;
    case RISCV_IMM_CSS_SDSP:
        value = (getBits(e, 9, 7) << 6) | (getBits(e, 12, 10) << 3);
        return true;
    case RISCV_IMM_CL_W:
        value = (getBits(e, 5, 5) << 6) | (getBits(e, 12, 10) << 3) | (getBits(e, 6, 6) << 2);
        return true;
    case RISCV_IMM_CL_D:
        value = (getBits(e, 6, 5) << 6) | (getBits(e, 12, 10) << 3);
        return true;
    case RISCV_IMM_CLB:
        value = (getBits(e, 5, 5) << 1) | getBits(e, 6, 6);
        return true;
    case RISCV_IMM_CLH:
        value = getBits(e, 5, 5) << 1;
        return true;
    case RISCV_IMM_CB:
        value = signExtend((getBits(e, 12, 12) << 8) | (getBits(e, 6, 5) << 6) |
                           (getBits(e, 2, 2) << 5) | (getBits(e, 11, 10) << 3) |
                           (getBits(e, 4, 3) << 1), 9);
        return true;
    case RISCV_IMM_CJ:
        value = signExtend((getBits(e, 12, 12) << 11) | (getBits(e, 8, 8) << 10) |
                           (getBits(e, 10, 9) << 8) | (getBits(e, 6, 6) << 7) |
                           (getBits(e, 7, 7) << 6) | (getBits(e, 2, 2) << 5) |
                           (getBits(e, 11, 11) << 4) | (getBits(e, 5, 3) << 1), 12);
        return true;
    }
    CHECK_FAIL();
}

// Put the bit 'from' of 'value' at the bit 'to' of the encoding
#define PUT(from, to) (((uint32)(((uint64)value >> (from)) & 1)) << (to))
// Put the bits hi..lo of 'value' at lo2 of the encoding
#define PUTS(hi, lo, to) (((uint32)(((uint64)value >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))) << (to))

// Return true if the value is a signed number of 'bits' bits and a multiple of
// 'align'
static bool fitsSigned(int64 value, uint bits, uint align)
{
    int64 limit = (int64)1 << (bits - 1);
    return (value >= -limit) && (value < limit) && ((value % (int64)align) == 0);
}

static bool fitsUnsigned(int64 value, uint bits, uint align)
{
    return (value >= 0) && (value < ((int64)1 << bits)) && ((value % (int64)align) == 0);
}

bool encodeImmediate(uint scheme, int64 value, uint32& e)
{
    uint32 bits = 0;
    uint32 mask = 0;
    switch (scheme)
    {
    case RISCV_IMM_I:
        if (!fitsSigned(value, 12, 1)) return false;
        mask = 0xfff00000; bits = PUTS(11, 0, 20);
        break;
    case RISCV_IMM_S:
        if (!fitsSigned(value, 12, 1)) return false;
        mask = 0xfe000f80; bits = PUTS(11, 5, 25) | PUTS(4, 0, 7);
        break;
    case RISCV_IMM_B:
        if (!fitsSigned(value, 13, 2)) return false;
        mask = 0xfe000f80;
        bits = PUT(12, 31) | PUTS(10, 5, 25) | PUTS(4, 1, 8) | PUT(11, 7);
        break;
    case RISCV_IMM_U:
    case RISCV_IMM_LPAD:
        if (!fitsUnsigned(value, 20, 1)) return false;
        mask = 0xfffff000; bits = PUTS(19, 0, 12);
        break;
    case RISCV_IMM_J:
        if (!fitsSigned(value, 21, 2)) return false;
        mask = 0xfffff000;
        bits = PUT(20, 31) | PUTS(10, 1, 21) | PUT(11, 20) | PUTS(19, 12, 12);
        break;
    case RISCV_IMM_CSR_UIMM5:
        if (!fitsUnsigned(value, 5, 1)) return false;
        mask = 0xf8000; bits = PUTS(4, 0, 15);
        break;
    case RISCV_IMM_PREFETCH:
        if (!fitsSigned(value, 12, 32)) return false;
        mask = 0xfe000000; bits = PUTS(11, 5, 25);
        break;
    case RISCV_IMM_AES_BS:
        if (!fitsUnsigned(value, 2, 1)) return false;
        mask = 0xc0000000; bits = PUTS(1, 0, 30);
        break;
    case RISCV_IMM_AES_RNUM:
        if (!fitsUnsigned(value, 4, 1) || value > 10) return false;
        mask = 0xf00000; bits = PUTS(3, 0, 20);
        break;
    case RISCV_IMM_V_SIMM5:
        if (!fitsSigned(value, 5, 1)) return false;
        mask = 0xf8000; bits = PUTS(4, 0, 15);
        break;
    case RISCV_IMM_V_UIMM5:
    case RISCV_IMM_V_AVL:
        if (!fitsUnsigned(value, 5, 1)) return false;
        mask = 0xf8000; bits = PUTS(4, 0, 15);
        break;
    case RISCV_IMM_V_UIMM6:
        if (!fitsUnsigned(value, 6, 1)) return false;
        mask = 0x40f8000; bits = PUT(5, 26) | PUTS(4, 0, 15);
        break;
    case RISCV_IMM_CI:
        if (!fitsSigned(value, 6, 1)) return false;
        mask = 0x107c; bits = PUT(5, 12) | PUTS(4, 0, 2);
        break;
    case RISCV_IMM_CI_LUI:
        // The 20 bit value of lui whose upper 14 bits are the sign
        if (!fitsUnsigned(value, 20, 1)) return false;
        if (signExtend((uint64)value, 6) != signExtend((uint64)value, 20)) return false;
        mask = 0x107c; bits = PUT(5, 12) | PUTS(4, 0, 2);
        break;
    case RISCV_IMM_CI_ADDI16SP:
        if (!fitsSigned(value, 10, 16)) return false;
        mask = 0x107c;
        bits = PUT(9, 12) | PUT(4, 6) | PUT(6, 5) | PUTS(8, 7, 3) | PUT(5, 2);
        break;
    case RISCV_IMM_CIW:
        if (!fitsUnsigned(value, 10, 4)) return false;
        mask = 0x1fe0; bits = PUTS(5, 4, 11) | PUTS(9, 6, 7) | PUT(2, 6) | PUT(3, 5);
        break;
    case RISCV_IMM_CI_LWSP:
        if (!fitsUnsigned(value, 8, 4)) return false;
        mask = 0x107c; bits = PUT(5, 12) | PUTS(4, 2, 4) | PUTS(7, 6, 2);
        break;
    case RISCV_IMM_CI_LDSP:
        if (!fitsUnsigned(value, 9, 8)) return false;
        mask = 0x107c; bits = PUT(5, 12) | PUTS(4, 3, 5) | PUTS(8, 6, 2);
        break;
    case RISCV_IMM_CSS_SWSP:
        if (!fitsUnsigned(value, 8, 4)) return false;
        mask = 0x1f80; bits = PUTS(5, 2, 9) | PUTS(7, 6, 7);
        break;
    case RISCV_IMM_CSS_SDSP:
        if (!fitsUnsigned(value, 9, 8)) return false;
        mask = 0x1f80; bits = PUTS(5, 3, 10) | PUTS(8, 6, 7);
        break;
    case RISCV_IMM_CL_W:
        if (!fitsUnsigned(value, 7, 4)) return false;
        mask = 0x1c60; bits = PUTS(5, 3, 10) | PUT(2, 6) | PUT(6, 5);
        break;
    case RISCV_IMM_CL_D:
        if (!fitsUnsigned(value, 8, 8)) return false;
        mask = 0x1c60; bits = PUTS(5, 3, 10) | PUTS(7, 6, 5);
        break;
    case RISCV_IMM_CLB:
        if (!fitsUnsigned(value, 2, 1)) return false;
        mask = 0x60; bits = PUT(1, 5) | PUT(0, 6);
        break;
    case RISCV_IMM_CLH:
        if (!fitsUnsigned(value, 2, 2)) return false;
        mask = 0x20; bits = PUT(1, 5);
        break;
    case RISCV_IMM_CB:
        if (!fitsSigned(value, 9, 2)) return false;
        mask = 0x1c7c;
        bits = PUT(8, 12) | PUTS(4, 3, 10) | PUTS(7, 6, 5) | PUTS(2, 1, 3) | PUT(5, 2);
        break;
    case RISCV_IMM_CJ:
        if (!fitsSigned(value, 12, 2)) return false;
        mask = 0x1ffc;
        bits = PUT(11, 12) | PUT(4, 11) | PUTS(9, 8, 9) | PUT(10, 8) | PUT(6, 7) |
               PUT(7, 6) | PUTS(3, 1, 3) | PUT(5, 2);
        break;
    default:
        CHECK_FAIL();
    }
    e = (e & ~mask) | bits;
    return true;
}

} // namespace riscvdis

//////////////////////////////////////////////////////////////////////////
// RiscvOperandCodec

uint32 RiscvOperandCodec::getField(uint32 encoding, uint position, uint width)
{
    if (width >= 32)
        return encoding >> position;
    return (encoding >> position) & ((1u << width) - 1);
}

void RiscvOperandCodec::setField(uint32& encoding, uint position, uint width, uint32 value)
{
    uint32 mask = (width >= 32) ? 0xffffffffu : ((1u << width) - 1);
    encoding = (encoding & ~(mask << position)) | ((value & mask) << position);
}

bool RiscvOperandCodec::isMnemonicPart(const RiscvOperandSpec& spec)
{
    return spec.m_kind == RISCV_KIND_AQRL;
}

bool RiscvOperandCodec::isHidden(const RiscvOperandSpec& spec)
{
    return spec.m_kind == RISCV_KIND_GPR_TIED;
}

bool RiscvOperandCodec::isOptional(const RiscvOperandSpec& spec)
{
    return (spec.m_kind == RISCV_KIND_VMASK) || (spec.m_kind == RISCV_KIND_ROUNDING_MODE);
}

bool RiscvOperandCodec::isPrinted(const RiscvOperandSpec& spec, uint32 encoding)
{
    if (spec.m_kind == RISCV_KIND_VMASK)
        return getField(encoding, spec.m_position, 1) == 0;
    if (spec.m_kind == RISCV_KIND_ROUNDING_MODE)
    {
        uint defaultMode = ((spec.m_flags & RISCV_ROUNDING_DEFAULT_RNE) != 0) ? 0 : 7;
        return getField(encoding, spec.m_position, 3) != defaultMode;
    }
    return true;
}

cString RiscvOperandCodec::decodeMnemonicPart(const RiscvOperandSpec& spec, uint32 encoding)
{
    CHECK(spec.m_kind == RISCV_KIND_AQRL);
    bool aq = getField(encoding, 26, 1) != 0;
    bool rl = getField(encoding, 25, 1) != 0;
    if (aq && rl)
        return cString(".aqrl");
    if (aq)
        return cString(".aq");
    if (rl)
        return cString(".rl");
    return cString();
}

bool RiscvOperandCodec::encodeMnemonicPart(const RiscvOperandSpec& spec,
                                           cString& suffix,
                                           uint32& encoding)
{
    CHECK(spec.m_kind == RISCV_KIND_AQRL);
    uint32 value = 0;
    if (suffix.length() >= 5 && suffix.left(5) == ".aqrl")
    {
        value = 3;
        suffix = suffix.part(5, suffix.length());
    } else if (suffix.length() >= 3 && suffix.left(3) == ".aq")
    {
        value = 2;
        suffix = suffix.part(3, suffix.length());
    } else if (suffix.length() >= 3 && suffix.left(3) == ".rl")
    {
        value = 1;
        suffix = suffix.part(3, suffix.length());
    }
    // (aq is bit 26, rl bit 25)
    setField(encoding, 25, 2, value);
    return true;
}

bool RiscvOperandCodec::findCsr(const char* name,
                                const RiscvCodecContext& context,
                                uint& number)
{
    for (const RiscvCsrName* csr = gRiscvCsrNames; csr->m_name != NULL; csr++)
    {
        if ((csr->m_flags & RISCV_CSR_RV32_ONLY) != 0 &&
            context.m_type != RiscvInstructionSet::RISCV_RV32)
            continue;
        if (!context.m_features.has((RiscvFeatures::Feature)csr->m_feature))
            continue;
        if (strcmp(csr->m_name, name) == 0)
        {
            number = csr->m_number;
            return true;
        }
    }
    return false;
}

const char* RiscvOperandCodec::getCsrName(uint number, const RiscvCodecContext& context)
{
    for (const RiscvCsrName* csr = gRiscvCsrNames; csr->m_name != NULL; csr++)
    {
        if (csr->m_number != number)
            continue;
        if ((csr->m_flags & (RISCV_CSR_ALTERNATIVE)) != 0)
            continue;
        if ((csr->m_flags & RISCV_CSR_RV32_ONLY) != 0 &&
            context.m_type != RiscvInstructionSet::RISCV_RV32)
            continue;
        if (!context.m_features.has((RiscvFeatures::Feature)csr->m_feature))
            continue;
        return csr->m_name;
    }
    return NULL;
}

static uint registerOf(uint kind, uint number)
{
    switch (kind)
    {
    case RISCV_KIND_GPR:
    case RISCV_KIND_GPR_C:
    case RISCV_KIND_GPR_FIXED:
        return RISCV_REG_X0 + number;
    case RISCV_KIND_FPR:
    case RISCV_KIND_FPR_C:
        return RISCV_REG_F0 + number;
    default:
        return RISCV_REG_V0 + number;
    }
}

bool RiscvOperandCodec::decode(const RiscvOperandSpec& spec,
                               uint32 encoding,
                               const RiscvCodecContext& context,
                               RiscvDecodedOperand& operand)
{
    operand = RiscvDecodedOperand();
    switch (spec.m_kind)
    {
    case RISCV_KIND_GPR:
    {
        uint32 number = getField(encoding, spec.m_position, 5);
        if (((spec.m_flags & RISCV_GPR_NOT_ZERO) != 0) && (number == 0))
            return false;
        if (((spec.m_flags & RISCV_GPR_NOT_SP) != 0) && (number == 2))
            return false;
        if (((spec.m_flags & RISCV_GPR_EVEN) != 0) && ((number & 1) != 0))
            return false;
        operand.m_kind = RiscvDecodedOperand::KIND_REGISTER;
        operand.m_register = registerOf(spec.m_kind, number);
        return true;
    }
    case RISCV_KIND_GPR_C:
    case RISCV_KIND_FPR_C:
        operand.m_kind = RiscvDecodedOperand::KIND_REGISTER;
        operand.m_register = registerOf(spec.m_kind, 8 + getField(encoding, spec.m_position, 3));
        return true;
    case RISCV_KIND_GPR_FIXED:
        operand.m_kind = RiscvDecodedOperand::KIND_REGISTER;
        operand.m_register = registerOf(spec.m_kind, spec.m_extra);
        return true;
    case RISCV_KIND_GPR_TIED:
        // No operand: the two fields must be equal
        return getField(encoding, spec.m_position, 5) == getField(encoding, spec.m_extra, 5);
    case RISCV_KIND_FPR:
        operand.m_kind = RiscvDecodedOperand::KIND_REGISTER;
        operand.m_register = registerOf(spec.m_kind, getField(encoding, spec.m_position, 5));
        return true;
    case RISCV_KIND_VR:
    case RISCV_KIND_VR_V0:
    case RISCV_KIND_VMASK:
    case RISCV_KIND_VTYPE:
        return decodeVectorKind(spec, encoding, operand);
    case RISCV_KIND_IMMEDIATE:
    {
        int64 value;
        if (!decodeImmediate(spec.m_extra, encoding, value))
            return false;
        if (((spec.m_flags & RISCV_IMMEDIATE_NOT_ZERO) != 0) && (value == 0))
            return false;
        operand.m_kind = RiscvDecodedOperand::KIND_IMMEDIATE;
        operand.m_immediate = value;
        operand.m_isHex = (spec.m_flags & RISCV_IMMEDIATE_HEX) != 0;
        operand.m_isUnsigned = isUnsignedScheme(spec.m_extra);
        return true;
    }
    case RISCV_KIND_SHAMT:
    {
        uint32 value;
        if (spec.m_position == 2)
            value = (getField(encoding, 12, 1) << 5) | getField(encoding, 2, 5);
        else
            value = getField(encoding, spec.m_position, spec.m_extra);
        // RV32: shamt[5] must be 0
        if ((spec.m_extra == 6) && (value >= 32) &&
            (context.m_type == RiscvInstructionSet::RISCV_RV32))
            return false;
        if (((spec.m_flags & RISCV_IMMEDIATE_NOT_ZERO) != 0) && (value == 0))
            return false;
        operand.m_kind = RiscvDecodedOperand::KIND_IMMEDIATE;
        operand.m_immediate = value;
        operand.m_isUnsigned = true;
        return true;
    }
    case RISCV_KIND_FLOAT_IMMEDIATE:
        operand.m_kind = RiscvDecodedOperand::KIND_FLOAT_IMMEDIATE;
        operand.m_fliIndex = getField(encoding, spec.m_position, 5);
        return true;
    case RISCV_KIND_MEMORY:
    {
        operand.m_kind = RiscvDecodedOperand::KIND_MEMORY;
        if ((spec.m_flags & RISCV_MEMORY_COMPRESSED) != 0)
            operand.m_base = RISCV_REG_X0 + 8 + getField(encoding, spec.m_position, 3);
        else if ((spec.m_flags & RISCV_MEMORY_SP) != 0)
            operand.m_base = RISCV_REG_X0 + 2;
        else
            operand.m_base = RISCV_REG_X0 + getField(encoding, spec.m_position, 5);
        if ((spec.m_flags & RISCV_MEMORY_NO_OFFSET) != 0)
        {
            operand.m_hasOffset = false;
            return true;
        }
        int64 offset;
        if (!decodeImmediate(spec.m_extra, encoding, offset))
            return false;
        operand.m_offset = offset;
        operand.m_hasOffset = true;
        return true;
    }
    case RISCV_KIND_BRANCH_TARGET:
    {
        int64 offset;
        if (!decodeImmediate(spec.m_extra, encoding, offset))
            return false;
        uint64 target = context.m_address + (uint64)offset;
        if (context.m_type == RiscvInstructionSet::RISCV_RV32)
            target &= 0xffffffffull;
        operand.m_kind = RiscvDecodedOperand::KIND_BRANCH_TARGET;
        operand.m_immediate = (int64)target;
        return true;
    }
    case RISCV_KIND_CSR:
        operand.m_kind = RiscvDecodedOperand::KIND_CSR;
        operand.m_csr = getField(encoding, spec.m_position, 12);
        return true;
    case RISCV_KIND_ROUNDING_MODE:
    {
        uint32 mode = getField(encoding, spec.m_position, 3);
        // 5 and 6 are reserved
        if ((mode == 5) || (mode == 6))
            return false;
        operand.m_kind = RiscvDecodedOperand::KIND_ROUNDING_MODE;
        operand.m_roundingMode = mode;
        return true;
    }
    case RISCV_KIND_FENCE_SET:
        operand.m_kind = RiscvDecodedOperand::KIND_FENCE_SET;
        operand.m_fenceSet = getField(encoding, spec.m_position, 4);
        return true;
    default:
        CHECK_FAIL();
    }
}

// Parse the text of a float for fli: "0.5", "1.0", "min"... Return the index
// of the value, false if the text is not one of the 32 values
static bool findFliValue(const cString& text, uint& index)
{
    for (uint i = 0; i < 32; i++)
        if (text == gRiscvFliValues[i].m_text)
        {
            index = i;
            return true;
        }
    // Another spelling of a number ("1", "0.50", "1e0", "0x1p0"; not "+1.0")
    if (text.length() == 0 || !((text[0] >= XSTL_CHAR('0') && text[0] <= XSTL_CHAR('9')) ||
                                text[0] == XSTL_CHAR('-')))
        return false;
    cSArray<char> ascii = text.getASCIIstring();
    double value = 0;
    int consumed = 0;
    if ((sscanf(ascii.getBuffer(), "%lf%n", &value, &consumed) != 1) ||
        (ascii.getBuffer()[consumed] != 0))
        return false;
    for (uint i = 0; i < 32; i++)
    {
        if (i == 1 || i == 30 || i == 31)
            continue;
        double known = 0;
        sscanf(gRiscvFliValues[i].m_text, "%lf", &known);
        if (known == value)
        {
            index = i;
            return true;
        }
    }
    return false;
}

static bool isRegister(const RiscvDecodedOperand& operand, uint first, uint last)
{
    return (operand.m_kind == RiscvDecodedOperand::KIND_REGISTER) &&
           (operand.m_register >= first) && (operand.m_register <= last);
}

bool RiscvOperandCodec::encode(const RiscvOperandSpec& spec,
                               const RiscvDecodedOperand& operand,
                               const RiscvCodecContext& context,
                               uint32& encoding)
{
    switch (spec.m_kind)
    {
    case RISCV_KIND_GPR:
    {
        if (!isRegister(operand, RISCV_REG_X0, RISCV_REG_X31))
            return false;
        uint32 number = operand.m_register - RISCV_REG_X0;
        if (((spec.m_flags & RISCV_GPR_NOT_ZERO) != 0) && (number == 0))
            return false;
        if (((spec.m_flags & RISCV_GPR_NOT_SP) != 0) && (number == 2))
            return false;
        if (((spec.m_flags & RISCV_GPR_EVEN) != 0) && ((number & 1) != 0))
            return false;
        setField(encoding, spec.m_position, 5, number);
        return true;
    }
    case RISCV_KIND_GPR_C:
        if (!isRegister(operand, RISCV_REG_X0 + 8, RISCV_REG_X0 + 15))
            return false;
        setField(encoding, spec.m_position, 3, operand.m_register - RISCV_REG_X0 - 8);
        return true;
    case RISCV_KIND_FPR_C:
        if (!isRegister(operand, RISCV_REG_F0 + 8, RISCV_REG_F0 + 15))
            return false;
        setField(encoding, spec.m_position, 3, operand.m_register - RISCV_REG_F0 - 8);
        return true;
    case RISCV_KIND_GPR_FIXED:
        return isRegister(operand, RISCV_REG_X0 + spec.m_extra, RISCV_REG_X0 + spec.m_extra);
    case RISCV_KIND_GPR_TIED:
        setField(encoding, spec.m_position, 5, getField(encoding, spec.m_extra, 5));
        return true;
    case RISCV_KIND_FPR:
        if (!isRegister(operand, RISCV_REG_F0, RISCV_REG_F31))
            return false;
        setField(encoding, spec.m_position, 5, operand.m_register - RISCV_REG_F0);
        return true;
    case RISCV_KIND_VR:
    case RISCV_KIND_VR_V0:
    case RISCV_KIND_VMASK:
    case RISCV_KIND_VTYPE:
        return encodeVectorKind(spec, operand, encoding);
    case RISCV_KIND_IMMEDIATE:
        if (operand.m_kind != RiscvDecodedOperand::KIND_IMMEDIATE)
            return false;
        if (((spec.m_flags & RISCV_IMMEDIATE_NOT_ZERO) != 0) && (operand.m_immediate == 0))
            return false;
        return encodeImmediate(spec.m_extra, operand.m_immediate, encoding);
    case RISCV_KIND_SHAMT:
    {
        if (operand.m_kind != RiscvDecodedOperand::KIND_IMMEDIATE)
            return false;
        int64 value = operand.m_immediate;
        uint width = spec.m_extra;
        if ((width == 6) && (context.m_type == RiscvInstructionSet::RISCV_RV32))
            width = 5;
        if ((value < 0) || (value >= ((int64)1 << width)))
            return false;
        if (((spec.m_flags & RISCV_IMMEDIATE_NOT_ZERO) != 0) && (value == 0))
            return false;
        if (spec.m_position == 2)
        {
            setField(encoding, 12, 1, (uint32)(value >> 5));
            setField(encoding, 2, 5, (uint32)value);
        } else
        {
            setField(encoding, spec.m_position, spec.m_extra, (uint32)value);
        }
        return true;
    }
    case RISCV_KIND_FLOAT_IMMEDIATE:
    {
        uint index;
        if (operand.m_kind == RiscvDecodedOperand::KIND_FLOAT_IMMEDIATE)
            index = operand.m_fliIndex;
        else if (operand.m_kind != RiscvDecodedOperand::KIND_NAME || !findFliValue(operand.m_name, index))
            return false;
        setField(encoding, spec.m_position, 5, index);
        return true;
    }
    case RISCV_KIND_MEMORY:
    {
        if (operand.m_kind != RiscvDecodedOperand::KIND_MEMORY)
            return false;
        if ((spec.m_flags & RISCV_MEMORY_COMPRESSED) != 0)
        {
            if ((operand.m_base < RISCV_REG_X0 + 8) || (operand.m_base > RISCV_REG_X0 + 15))
                return false;
            setField(encoding, spec.m_position, 3, operand.m_base - RISCV_REG_X0 - 8);
        } else if ((spec.m_flags & RISCV_MEMORY_SP) != 0)
        {
            if (operand.m_base != RISCV_REG_X0 + 2)
                return false;
        } else
        {
            if (operand.m_base > RISCV_REG_X31)
                return false;
            setField(encoding, spec.m_position, 5, operand.m_base - RISCV_REG_X0);
        }
        int64 offset = operand.m_hasOffset ? operand.m_offset : 0;
        if ((spec.m_flags & RISCV_MEMORY_NO_OFFSET) != 0)
            return offset == 0;
        return encodeImmediate(spec.m_extra, offset, encoding);
    }
    case RISCV_KIND_BRANCH_TARGET:
        // (the offset from the instruction)
        if ((operand.m_kind != RiscvDecodedOperand::KIND_IMMEDIATE) &&
            (operand.m_kind != RiscvDecodedOperand::KIND_BRANCH_TARGET))
            return false;
        return encodeImmediate(spec.m_extra, operand.m_immediate, encoding);
    case RISCV_KIND_CSR:
    {
        uint number;
        if (operand.m_kind == RiscvDecodedOperand::KIND_CSR)
            number = operand.m_csr;
        else if (operand.m_kind == RiscvDecodedOperand::KIND_IMMEDIATE)
        {
            if ((operand.m_immediate < 0) || (operand.m_immediate > 0xfff))
                return false;
            number = (uint)operand.m_immediate;
        } else if (operand.m_kind == RiscvDecodedOperand::KIND_NAME)
        {
            if (!findCsr(operand.m_name.getASCIIstring().getBuffer(), context, number))
                return false;
        } else
            return false;
        setField(encoding, spec.m_position, 12, number);
        return true;
    }
    case RISCV_KIND_ROUNDING_MODE:
    {
        uint mode;
        if (operand.m_kind == RiscvDecodedOperand::KIND_NONE)
            // (left out: the default mode)
            mode = ((spec.m_flags & RISCV_ROUNDING_DEFAULT_RNE) != 0) ? 0 : 7;
        else if (operand.m_kind == RiscvDecodedOperand::KIND_ROUNDING_MODE)
            mode = operand.m_roundingMode;
        else if (operand.m_kind == RiscvDecodedOperand::KIND_NAME)
        {
            // (LLVM reads the names of the rounding modes in lowercase only)
            if (operand.m_immediate != 0)
                return false;
            static const char* const names[] = { "rne", "rtz", "rdn", "rup", "rmm", "", "", "dyn" };
            mode = 8;
            for (uint i = 0; i < 8; i++)
                if (names[i][0] != 0 && operand.m_name == names[i])
                    mode = i;
            if (mode == 8)
                return false;
        } else
            return false;
        setField(encoding, spec.m_position, 3, mode);
        return true;
    }
    case RISCV_KIND_FENCE_SET:
    {
        uint set = 0;
        if (operand.m_kind == RiscvDecodedOperand::KIND_FENCE_SET)
            set = operand.m_fenceSet;
        else if (operand.m_kind == RiscvDecodedOperand::KIND_IMMEDIATE)
        {
            // Only 0 is a number
            if (operand.m_immediate != 0)
                return false;
        } else if (operand.m_kind == RiscvDecodedOperand::KIND_NAME)
        {
            // (lowercase letters only)
            if (operand.m_immediate != 0)
                return false;
            // Letters i o r w in this order, each once
            static const char order[] = "iorw";
            uint last = 0;
            for (uint i = 0; i < operand.m_name.length(); i++)
            {
                uint found = 0;
                for (uint j = 0; j < 4; j++)
                    if (operand.m_name[i] == (character)order[j])
                        found = j + 1;
                if (found <= last)
                    return false;
                last = found;
                set |= 1u << (4 - found);
            }
            if (set == 0)
                return false;
        } else
            return false;
        setField(encoding, spec.m_position, 4, set);
        return true;
    }
    default:
        CHECK_FAIL();
    }
}
