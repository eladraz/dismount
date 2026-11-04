#include "dismount/dismount.h"
/*
 * RiscvOperandCodecCompressed.cpp
 *
 * Implementation file
 *
 * The expansion of the compressed instructions into the 32 bit instructions,
 * per the "RVC instruction set listings" of the specification (the Zca, Zcf,
 * Zcd and Zcb listings).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/proc/riscv/RiscvOperandCodec.h"
#include "dismount/proc/riscv/RiscvOperandCodecInternal.h"

using namespace riscvdis;

namespace {

// 32 bit instruction builders
uint32 makeR(uint opcode, uint f3, uint f7, uint rd, uint rs1, uint rs2)
{
    return opcode | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}

uint32 makeI(uint opcode, uint f3, uint rd, uint rs1, int64 imm)
{
    return opcode | (rd << 7) | (f3 << 12) | (rs1 << 15) | (((uint32)imm & 0xfff) << 20);
}

uint32 makeS(uint opcode, uint f3, uint rs1, uint rs2, int64 imm)
{
    uint32 value = (uint32)imm & 0xfff;
    return opcode | ((value & 0x1f) << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) |
           ((value >> 5) << 25);
}

uint32 makeB(uint f3, uint rs1, uint rs2, int64 imm)
{
    uint32 value = (uint32)imm;
    return 0x63 | (((value >> 11) & 1) << 7) | (((value >> 1) & 0xf) << 8) | (f3 << 12) |
           (rs1 << 15) | (rs2 << 20) | (((value >> 5) & 0x3f) << 25) | (((value >> 12) & 1) << 31);
}

uint32 makeJ(uint rd, int64 imm)
{
    uint32 value = (uint32)imm;
    return 0x6f | (rd << 7) | (((value >> 12) & 0xff) << 12) | (((value >> 11) & 1) << 20) |
           (((value >> 1) & 0x3ff) << 21) | (((value >> 20) & 1) << 31);
}

uint32 makeU(uint rd, int64 imm20)
{
    return 0x37 | (rd << 7) | (((uint32)imm20 & 0xfffff) << 12);
}

int64 imm(uint scheme, uint32 c)
{
    int64 value = 0;
    bool ok = decodeImmediate(scheme, c, value);
    ASSERT(ok);
    return value;
}

} // anonymous namespace

bool RiscvOperandCodec::expandCompressed(RiscvInstructionSet::DisassemblerTypes type,
                                         uint16 encoding,
                                         uint32& expanded)
{
    const bool is64 = (type == RiscvInstructionSet::RISCV_RV64);
    const uint32 c = encoding;
    const uint op = c & 3;
    const uint f3 = (c >> 13) & 7;
    const uint rd = (c >> 7) & 31;
    const uint rs2 = (c >> 2) & 31;
    // The compressed registers
    const uint rdp = 8 + ((c >> 2) & 7);
    const uint rs1p = 8 + ((c >> 7) & 7);
    const uint bit12 = (c >> 12) & 1;

    if (c == 0)
    {
        // c.unimp: csrrw x0, cycle, x0
        expanded = 0xc0001073;
        return true;
    }

    switch (op)
    {
    case 0:
        switch (f3)
        {
        case 0:
        {
            int64 nzuimm = imm(RISCV_IMM_CIW, c);
            if (nzuimm == 0)
                return false;
            expanded = makeI(0x13, 0, rdp, 2, nzuimm);
            return true;
        }
        case 1:
            expanded = makeI(0x07, 3, rdp, rs1p, imm(RISCV_IMM_CL_D, c));
            return true;
        case 2:
            expanded = makeI(0x03, 2, rdp, rs1p, imm(RISCV_IMM_CL_W, c));
            return true;
        case 3:
            if (is64)
                expanded = makeI(0x03, 3, rdp, rs1p, imm(RISCV_IMM_CL_D, c));
            else
                expanded = makeI(0x07, 2, rdp, rs1p, imm(RISCV_IMM_CL_W, c));
            return true;
        case 4:
        {
            // Zcb: c.lbu, c.lhu, c.lh, c.sb, c.sh
            uint funct = (c >> 10) & 7;
            switch (funct)
            {
            case 0:
                expanded = makeI(0x03, 4, rdp, rs1p, imm(RISCV_IMM_CLB, c));
                return true;
            case 1:
                if ((c & 0x40) == 0)
                    expanded = makeI(0x03, 5, rdp, rs1p, imm(RISCV_IMM_CLH, c));
                else
                    expanded = makeI(0x03, 1, rdp, rs1p, imm(RISCV_IMM_CLH, c));
                return true;
            case 2:
                expanded = makeS(0x23, 0, rs1p, rdp, imm(RISCV_IMM_CLB, c));
                return true;
            case 3:
                if ((c & 0x40) != 0)
                    return false;
                expanded = makeS(0x23, 1, rs1p, rdp, imm(RISCV_IMM_CLH, c));
                return true;
            default:
                return false;
            }
        }
        case 5:
            expanded = makeS(0x27, 3, rs1p, rdp, imm(RISCV_IMM_CL_D, c));
            return true;
        case 6:
            expanded = makeS(0x23, 2, rs1p, rdp, imm(RISCV_IMM_CL_W, c));
            return true;
        case 7:
            if (is64)
                expanded = makeS(0x23, 3, rs1p, rdp, imm(RISCV_IMM_CL_D, c));
            else
                expanded = makeS(0x27, 2, rs1p, rdp, imm(RISCV_IMM_CL_W, c));
            return true;
        }
        return false;

    case 1:
        switch (f3)
        {
        case 0:
            // c.nop, c.addi (rd = 0 or imm = 0 are hints)
            expanded = makeI(0x13, 0, rd, rd, imm(RISCV_IMM_CI, c));
            return true;
        case 1:
            if (!is64)
            {
                expanded = makeJ(1, imm(RISCV_IMM_CJ, c));
                return true;
            }
            if (rd == 0)
                return false;
            expanded = makeI(0x1b, 0, rd, rd, imm(RISCV_IMM_CI, c));
            return true;
        case 2:
            expanded = makeI(0x13, 0, rd, 0, imm(RISCV_IMM_CI, c));
            return true;
        case 3:
            // c.mop.1 and c.mop.5 (the odd registers with a zero immediate) are
            // c.sspush x1 and c.sspopchk x5 of Zicfiss
            if ((bit12 == 0) && (rs2 == 0) && ((rd & 1) != 0))
            {
                if (rd == 1)
                    expanded = 0xce104073;
                else if (rd == 5)
                    expanded = 0xcdc2c073;
                else
                    return false;
                return true;
            }
            if (rd == 2)
            {
                int64 nzimm = imm(RISCV_IMM_CI_ADDI16SP, c);
                if (nzimm == 0)
                    return false;
                expanded = makeI(0x13, 0, 2, 2, nzimm);
                return true;
            } else
            {
                int64 nzimm = imm(RISCV_IMM_CI, c);
                if (nzimm == 0)
                    return false;
                expanded = makeU(rd, nzimm);
                return true;
            }
        case 4:
        {
            uint funct = (c >> 10) & 3;
            uint shamt = (bit12 << 5) | ((c >> 2) & 31);
            switch (funct)
            {
            case 0:
                expanded = makeI(0x13, 5, rs1p, rs1p, shamt);
                return true;
            case 1:
                expanded = makeI(0x13, 5, rs1p, rs1p, shamt | 0x400);
                return true;
            case 2:
                expanded = makeI(0x13, 7, rs1p, rs1p, imm(RISCV_IMM_CI, c));
                return true;
            default:
            {
                uint low = (c >> 5) & 3;
                uint rs2p = 8 + ((c >> 2) & 7);
                if (bit12 == 0)
                {
                    static const uint f3s[4] = { 0, 4, 6, 7 };
                    expanded = makeR(0x33, f3s[low], (low == 0) ? 0x20 : 0, rs1p, rs1p, rs2p);
                    return true;
                }
                if (low == 0)
                {
                    if (!is64)
                        return false;
                    expanded = makeR(0x3b, 0, 0x20, rs1p, rs1p, rs2p);
                    return true;
                }
                if (low == 1)
                {
                    if (!is64)
                        return false;
                    expanded = makeR(0x3b, 0, 0, rs1p, rs1p, rs2p);
                    return true;
                }
                if (low == 2)
                {
                    // c.mul
                    expanded = makeR(0x33, 0, 1, rs1p, rs1p, rs2p);
                    return true;
                }
                switch ((c >> 2) & 7)
                {
                case 0:
                    expanded = makeI(0x13, 7, rs1p, rs1p, 255);
                    return true;
                case 1:
                    expanded = 0x60401013 | (rs1p << 7) | (rs1p << 15);
                    return true;
                case 2:
                    expanded = (is64 ? 0x0800403b : 0x08004033) | (rs1p << 7) | (rs1p << 15);
                    return true;
                case 3:
                    expanded = 0x60501013 | (rs1p << 7) | (rs1p << 15);
                    return true;
                case 4:
                    if (!is64)
                        return false;
                    expanded = 0x0800003b | (rs1p << 7) | (rs1p << 15);
                    return true;
                case 5:
                    expanded = makeI(0x13, 4, rs1p, rs1p, -1);
                    return true;
                default:
                    return false;
                }
            }
            }
        }
        case 5:
            expanded = makeJ(0, imm(RISCV_IMM_CJ, c));
            return true;
        case 6:
            expanded = makeB(0, rs1p, 0, imm(RISCV_IMM_CB, c));
            return true;
        case 7:
            expanded = makeB(1, rs1p, 0, imm(RISCV_IMM_CB, c));
            return true;
        }
        return false;

    case 2:
        switch (f3)
        {
        case 0:
            // c.slli (rd = 0 or shamt = 0 are hints)
            expanded = makeI(0x13, 1, rd, rd, (bit12 << 5) | rs2);
            return true;
        case 1:
            expanded = makeI(0x07, 3, rd, 2, imm(RISCV_IMM_CI_LDSP, c));
            return true;
        case 2:
            if (rd == 0)
                return false;
            expanded = makeI(0x03, 2, rd, 2, imm(RISCV_IMM_CI_LWSP, c));
            return true;
        case 3:
            if (is64)
            {
                if (rd == 0)
                    return false;
                expanded = makeI(0x03, 3, rd, 2, imm(RISCV_IMM_CI_LDSP, c));
            } else
                expanded = makeI(0x07, 2, rd, 2, imm(RISCV_IMM_CI_LWSP, c));
            return true;
        case 4:
            if (bit12 == 0)
            {
                if (rs2 == 0)
                {
                    // c.jr
                    if (rd == 0)
                        return false;
                    expanded = makeI(0x67, 0, 0, rd, 0);
                    return true;
                }
                // c.mv (rd = 0 is a hint)
                expanded = makeR(0x33, 0, 0, rd, 0, rs2);
                return true;
            }
            if (rs2 == 0)
            {
                if (rd == 0)
                    expanded = 0x00100073;
                else
                    expanded = makeI(0x67, 0, 1, rd, 0);
                return true;
            }
            // c.add (rd = 0 is a hint)
            expanded = makeR(0x33, 0, 0, rd, rd, rs2);
            return true;
        case 5:
            expanded = makeS(0x27, 3, 2, rs2, imm(RISCV_IMM_CSS_SDSP, c));
            return true;
        case 6:
            expanded = makeS(0x23, 2, 2, rs2, imm(RISCV_IMM_CSS_SWSP, c));
            return true;
        case 7:
            if (is64)
                expanded = makeS(0x23, 3, 2, rs2, imm(RISCV_IMM_CSS_SDSP, c));
            else
                expanded = makeS(0x27, 2, 2, rs2, imm(RISCV_IMM_CSS_SWSP, c));
            return true;
        }
        return false;
    }
    // A 32 bit instruction
    return false;
}
