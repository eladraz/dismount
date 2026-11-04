#include "dismount/dismount.h"
/*
 * RiscvOperandCodecVector.cpp
 *
 * Implementation file
 *
 * The minimal codec of the vector kinds: the registers, v0, the mask and the
 * vtype of vsetvli / vsetivli. The constraints of RiscvVectorRegisterFlags
 * (the masked destination, the overlap of the register groups) which LLVM
 * checks when it assembles are left to the vector tables' owner: the
 * decoder never checks them.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/list.h"
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

// Return the index of a name in a table, 0xff if it is not there
uint findName(const char* const* table, uint count, const cString& name)
{
    for (uint i = 0; i < count; i++)
        if ((table[i] != NULL) && (name == table[i]))
            return i;
    return 0xff;
}

} // anonymous namespace

cString formatVtype(uint vtype)
{
    uint vlmul = vtype & 7;
    uint vsew = (vtype >> 3) & 7;
    // The reserved values are printed as the number
    if ((vtype >> 8) != 0 || vlmul == 4 || vsew > 3)
        return cString((uint32)vtype);
    return cString(gSew[vsew]) + ", " + gLmul[vlmul] + ", " +
           (((vtype & 0x40) != 0) ? "ta" : "tu") + ", " +
           (((vtype & 0x80) != 0) ? "ma" : "mu");
}

bool parseVtype(const cString& text, uint& vtype)
{
    cList<cString> items = text.split(XSTL_STRING(","));
    uint index = 0;
    uint value = 0;
    uint sew = 0xff, lmul = 0xff;
    for (cList<cString>::iterator item = items.begin(); item != items.end(); item++, index++)
    {
        cString word(*item);
        word.trim();
        if (index == 0)
        {
            sew = findName(gSew, 4, word);
            if (sew == 0xff)
                return false;
        } else if (index == 1)
        {
            lmul = findName(gLmul, 8, word);
            if (lmul == 0xff)
                return false;
        } else if (index == 2)
        {
            if (word == "ta")
                value |= 0x40;
            else if (word != "tu")
                return false;
        } else if (index == 3)
        {
            if (word == "ma")
                value |= 0x80;
            else if (word != "mu")
                return false;
        } else
            return false;
    }
    if ((sew == 0xff) || (lmul == 0xff))
        return false;
    vtype = value | (sew << 3) | lmul;
    return true;
}

bool decodeVectorKind(const RiscvOperandSpec& spec, uint32 encoding,
                      RiscvDecodedOperand& operand)
{
    switch (spec.m_kind)
    {
    case RISCV_KIND_VR:
        operand.m_kind = RiscvDecodedOperand::KIND_REGISTER;
        operand.m_register = RISCV_REG_V0 + RiscvOperandCodec::getField(encoding, spec.m_position, 5);
        return true;
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
        if ((operand.m_kind != RiscvDecodedOperand::KIND_REGISTER) ||
            (operand.m_register < RISCV_REG_V0) || (operand.m_register > RISCV_REG_V31))
            return false;
        RiscvOperandCodec::setField(encoding, spec.m_position, 5, operand.m_register - RISCV_REG_V0);
        return true;
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
            if (!parseVtype(operand.m_name, vtype))
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
