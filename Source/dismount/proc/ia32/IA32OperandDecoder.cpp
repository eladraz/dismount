/*
 * IA32OperandDecoder.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/IntegerEncoding.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/proc/ia32/IA32Opcode.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/IA32OperandDecoder.h"

using namespace ia32dis;

namespace {

// Return the register id of the general purpose register 'number' (0 = eax,
// ecx, edx, ebx, esp, ebp, esi, edi. For 8 bits: al, cl, dl, bl, ah, ch, dh,
// bh) of 8, 16 or 32 bits
uint getGeneralRegisterId(uint number, uint bits)
{
    CHECK(number < NUMBER_OF_REGISTERS);
    switch (bits)
    {
    case 8:  return IA32_REG_AL + number;
    case 16: return IA32_REG_AX + number;
    case 32: return IA32_REG_EAX + number;
    default:
        CHECK_FAIL();
    }
}

// Return an operand whose fields are cleared
IA32DecodedOperand makeOperand(uint kind, uint bits)
{
    IA32DecodedOperand operand;
    operand.m_kind = kind;
    operand.m_bits = bits;
    operand.m_register = 0;
    operand.m_segment = 0;
    operand.m_isSegmentOverride = false;
    operand.m_base = IA32DecodedOperand::NO_REGISTER;
    operand.m_index = IA32DecodedOperand::NO_REGISTER;
    operand.m_scale = 1;
    operand.m_displacement = 0;
    operand.m_addressBits = 0;
    operand.m_immediate = 0;
    operand.m_farSegment = 0;
    return operand;
}

// Return a register operand
IA32DecodedOperand makeRegister(uint registerId, uint bits)
{
    IA32DecodedOperand operand = makeOperand(IA32DecodedOperand::KIND_REGISTER,
                                             bits);
    operand.m_register = registerId;
    return operand;
}

// Return an immediate operand
IA32DecodedOperand makeImmediate(int64 value, uint bits, uint kind)
{
    IA32DecodedOperand operand = makeOperand(kind, bits);
    operand.m_immediate = value;
    return operand;
}

// Sign extend the low 'bits' of 'value'
int64 signExtend(uint64 value, uint bits)
{
    switch (bits)
    {
    case 8:  return (int64)(int8)value;
    case 16: return (int64)(int16)value;
    case 32: return (int64)(int32)value;
    default:
        return (int64)value;
    }
}

// Return true if the instruction is encoded in the two bytes table (0F xx).
// The prefixes, including the mandatory ones, precede the opcode bytes
bool isTwoBytesOpcode(const IA32Opcode& instruction)
{
    const cBuffer data = instruction.getOpcode();
    for (uint i = 0; i < data.getSize(); i++)
    {
        bool isPrefix = false;
        for (uint j = 0; j < IA32_NUMBER_OF_PREFIXS; j++)
            if (gIa32PrefixTable[j].m_opcode == data[i])
                isPrefix = true;
        // The first byte which isn't a prefix is the opcode
        if (!isPrefix)
            return (data[i] == gIa32TwoByteEscapeCharacter);
    }
    return false;
}

// Decode the memory reference of the ModR/M byte (mod != 3)
IA32DecodedOperand decodeMemory(const IA32Opcode& instruction, uint bits)
{
    const IA32OpcodeDatastruct::MODRM modrm = instruction.getModrm();
    const uint mod = modrm.m_bits.m_mod;
    const uint rm = modrm.m_bits.m_rm;
    const uint addressBits = IA32OperandDecoder::getAddressBits(instruction);
    CHECK(mod != 3);

    IA32DecodedOperand operand = makeOperand(IA32DecodedOperand::KIND_MEMORY,
                                             bits);
    operand.m_addressBits = addressBits;

    // The registers of the addressing mode, 16 or 32 bits
    const ModRMTranslation& entry = (addressBits == 16) ?
                                    gIa32ModRM16[mod][rm] : gIa32ModRM32[mod][rm];
    if (entry.m_firstRegisterPointer != NO_REGISTER)
        operand.m_base = getGeneralRegisterId(entry.m_firstRegisterPointer,
                                              addressBits);
    if (entry.m_secondRegisterPointer != NO_REGISTER)
        operand.m_index = getGeneralRegisterId(entry.m_secondRegisterPointer,
                                               addressBits);

    if (entry.m_forceSib)
    {
        // [base + index * scale]. Mod 0 with the base ebp has no base (a
        // disp32 is used) and the index esp means no index
        const IA32OpcodeDatastruct::SIB sib = instruction.getSIB();
        if ((mod != 0) || (sib.m_bits.m_base != IA32_GP32_EBP))
            operand.m_base = getGeneralRegisterId(sib.m_bits.m_base, 32);
        if (sib.m_bits.m_index != IA32_GP32_ESP)
        {
            operand.m_index = getGeneralRegisterId(sib.m_bits.m_index, 32);
            operand.m_scale = (uint)1 << sib.m_bits.m_scale;
        }
    }

    switch (instruction.getDisplacementLength())
    {
    case 0:
        break;
    case 1:
    case 2:
    case 4:
        operand.m_displacement = (int32)signExtend(
                        instruction.getDisplacement(),
                        instruction.getDisplacementLength() * 8);
        break;
    default:
        CHECK_FAIL();
    }

    // The segment is the override prefix, or ds. The addresses based on esp,
    // ebp and bp are relative to ss
    uint defaultSegment = IA32_REG_DS;
    if ((operand.m_base == IA32_REG_ESP) || (operand.m_base == IA32_REG_EBP) ||
        (operand.m_base == IA32_REG_BP))
        defaultSegment = IA32_REG_SS;
    operand.m_segment = IA32OperandDecoder::getSegment(instruction,
                                                       defaultSegment);
    operand.m_isSegmentOverride =
        (IA32OperandDecoder::getSegment(instruction, 0xFFFFFFFF) != 0xFFFFFFFF);
    return operand;
}

// Decode the r/m operand of the ModR/M byte, a register or a memory reference
//
// registerBase - The register group of a mod 3 operand: 8, 16 or 32 bits for
//                a general purpose register, 64 for mm and 128 for xmm
// memoryBits   - The size of the memory access
IA32DecodedOperand decodeModrm(const IA32Opcode& instruction,
                               uint registerBase,
                               uint memoryBits)
{
    const IA32OpcodeDatastruct::MODRM modrm = instruction.getModrm();
    if (modrm.m_bits.m_mod != 3)
        return decodeMemory(instruction, memoryBits);

    const uint rm = modrm.m_bits.m_rm;
    switch (registerBase)
    {
    case 8:
    case 16:
    case 32:
        return makeRegister(getGeneralRegisterId(rm, registerBase),
                            registerBase);
    case 64:
        return makeRegister(IA32_REG_MM0 + rm, 64);
    case 128:
        return makeRegister(IA32_REG_XMM0 + rm, 128);
    default:
        CHECK_FAIL();
    }
}

// Return the register id of the control register 'number' (cr0, cr2, cr3, cr4)
uint getControlRegisterId(uint number)
{
    switch (number)
    {
    case 0: return IA32_REG_CR0;
    case 2: return IA32_REG_CR2;
    case 3: return IA32_REG_CR3;
    case 4: return IA32_REG_CR4;
    default:
        CHECK_FAIL();
    }
}

// Return the register id of the debug register 'number' (dr0-dr3, dr6, dr7)
uint getDebugRegisterId(uint number)
{
    switch (number)
    {
    case 0: return IA32_REG_DR0;
    case 1: return IA32_REG_DR1;
    case 2: return IA32_REG_DR2;
    case 3: return IA32_REG_DR3;
    case 6: return IA32_REG_DR6;
    case 7: return IA32_REG_DR7;
    default:
        CHECK_FAIL();
    }
}

} // namespace

uint IA32OperandDecoder::getOperandsCount(const IA32Opcode& instruction)
{
    const OpcodeEntry* entry = instruction.getOpcodeEntry();
    if (entry->m_firstOperand == OPND_NO_OPERAND)
        return 0;
    if (entry->m_secondOperand == OPND_NO_OPERAND)
        return 1;
    if (entry->m_thridOperand == OPND_NO_OPERAND)
        return 2;
    return 3;
}

uint IA32OperandDecoder::getOperandBits(const IA32Opcode& instruction)
{
    switch (instruction.m_operandSize)
    {
    case IntegerEncoding::INTEGER_16BIT: return 16;
    case IntegerEncoding::INTEGER_32BIT: return 32;
    default:
        CHECK_FAIL();
    }
}

uint IA32OperandDecoder::getAddressBits(const IA32Opcode& instruction)
{
    switch (instruction.m_addressSize)
    {
    case IntegerEncoding::INTEGER_16BIT: return 16;
    case IntegerEncoding::INTEGER_32BIT: return 32;
    default:
        CHECK_FAIL();
    }
}

bool IA32OperandDecoder::hasPrefix(const IA32Opcode& instruction, uint8 prefix)
{
    for (uint i = 0; i < instruction.m_prefixsCount; i++)
        if (instruction.m_prefixs[i] == prefix)
            return true;
    return false;
}

uint IA32OperandDecoder::getSegment(const IA32Opcode& instruction,
                                    uint defaultSegment)
{
    // The last segment override prefix wins
    uint segment = defaultSegment;
    for (uint i = 0; i < instruction.m_prefixsCount; i++)
    {
        switch (instruction.m_prefixs[i])
        {
        case 0x26: segment = IA32_REG_ES; break;
        case 0x2E: segment = IA32_REG_CS; break;
        case 0x36: segment = IA32_REG_SS; break;
        case 0x3E: segment = IA32_REG_DS; break;
        case 0x64: segment = IA32_REG_FS; break;
        case 0x65: segment = IA32_REG_GS; break;
        default:
            break;
        }
    }
    return segment;
}

uint8 IA32OperandDecoder::getLastOpcodeByte(const IA32Opcode& instruction)
{
    return instruction.m_lastOpcodeByte;
}

uint IA32OperandDecoder::getModrmRegOpcode(const IA32Opcode& instruction)
{
    return instruction.getModrm().m_bits.m_regOpcode;
}

uint64 IA32OperandDecoder::getAddress(const IA32Opcode& instruction)
{
    if (!instruction.m_shouldUseAddress)
        return 0;
    return instruction.m_opcodeAddress.getAddress();
}

uint64 IA32OperandDecoder::getNextAddress(const IA32Opcode& instruction)
{
    return getAddress(instruction) + instruction.getOpcodeSize();
}

IA32DecodedOperand IA32OperandDecoder::decode(const IA32Opcode& instruction,
                                              uint index)
{
    CHECK(index < getOperandsCount(instruction));
    const OpcodeEntry* entry = instruction.getOpcodeEntry();
    const OperandType type = (index == 0) ? entry->m_firstOperand :
                             (index == 1) ? entry->m_secondOperand :
                                            entry->m_thridOperand;
    const IA32OpcodeDatastruct::ImmediateType immediate = instruction.getImmediate();
    const IA32OpcodeDatastruct::MODRM modrm = instruction.getModrm();
    const uint operandBits = getOperandBits(instruction);
    const uint addressBits = getAddressBits(instruction);

    switch (type)
    {
    //////////////////////////////////////////////////////////////////
    // Registers
    //////////////////////////////////////////////////////////////////
    case OPND_GP_16_32BIT:
        return makeRegister(getGeneralRegisterId(modrm.m_bits.m_regOpcode,
                                                 operandBits), operandBits);
    case OPND_GP_8BIT_MODRM:
        return makeRegister(getGeneralRegisterId(modrm.m_bits.m_regOpcode, 8), 8);
    case OPND_GP_16BIT_MODRM:
        return makeRegister(getGeneralRegisterId(modrm.m_bits.m_regOpcode, 16), 16);
    case OPND_GP_SEGMENT_MODRM:
        CHECK(modrm.m_bits.m_regOpcode < NUMBER_OF_SEGMENTS_REGISTERS);
        return makeRegister(IA32_REG_ES + modrm.m_bits.m_regOpcode, 16);
    case OPND_GP_32BIT_MODRM:
        return makeRegister(getGeneralRegisterId(modrm.m_bits.m_regOpcode, 32), 32);
    case OPND_SIMD_MODRM:
        return makeRegister(IA32_REG_XMM0 + modrm.m_bits.m_regOpcode, 128);
    case OPND_MMX_MODRM:
        return makeRegister(IA32_REG_MM0 + modrm.m_bits.m_regOpcode, 64);
    case OPND_CTRL_MODRM:
        return makeRegister(getControlRegisterId(modrm.m_bits.m_regOpcode), 32);
    case OPND_DBG_MODRM:
        return makeRegister(getDebugRegisterId(modrm.m_bits.m_regOpcode), 32);
    case OPND_ONEBYTES_OPCODE_GP_16_32:
        return makeRegister(getGeneralRegisterId(
                        getLastOpcodeByte(instruction) & 7, operandBits),
                        operandBits);
    case OPND_ONEBYTES_OPCODE_GP_8:
        return makeRegister(getGeneralRegisterId(
                        getLastOpcodeByte(instruction) & 7, 8), 8);
    case OPND_AL:
        return makeRegister(IA32_REG_AL, 8);
    case OPND_CL:
        return makeRegister(IA32_REG_CL, 8);
    case OPND_AX:
        return makeRegister(IA32_REG_AX, 16);
    case OPND_DX:
        return makeRegister(IA32_REG_DX, 16);
    case OPND_eAX:
        return makeRegister(getGeneralRegisterId(IA32_GP32_EAX, operandBits),
                            operandBits);
    case OPND_eBX:
        return makeRegister(getGeneralRegisterId(IA32_GP32_EBX, operandBits),
                            operandBits);
    case OPND_eBP:
        return makeRegister(getGeneralRegisterId(IA32_GP32_EBP, operandBits),
                            operandBits);
    case OPND_eSI:
        return makeRegister(getGeneralRegisterId(IA32_GP32_ESI, operandBits),
                            operandBits);
    case OPND_eDI:
        return makeRegister(getGeneralRegisterId(IA32_GP32_EDI, operandBits),
                            operandBits);
    case OPND_ES:
        return makeRegister(IA32_REG_ES, 16);
    case OPND_CS:
        return makeRegister(IA32_REG_CS, 16);
    case OPND_SS:
        return makeRegister(IA32_REG_SS, 16);
    case OPND_DS:
        return makeRegister(IA32_REG_DS, 16);
    case OPND_FS:
        return makeRegister(IA32_REG_FS, 16);
    case OPND_GS:
        return makeRegister(IA32_REG_GS, 16);
    // ST(i) is the physical register IA32_REG_FPR0 + i relative to the top of
    // the stack, which the x87 lifter resolves
    case OPND_FPU_ST0:
        return makeRegister(IA32_REG_FPR0, 80);
    case OPND_FPU_ST_RM:
        return makeRegister(IA32_REG_FPR0 + modrm.m_bits.m_rm, 80);

    //////////////////////////////////////////////////////////////////
    // The r/m operands: a register or a memory reference
    //////////////////////////////////////////////////////////////////
    case OPND_MODRM_dWORDPTR:
        return decodeModrm(instruction, operandBits, operandBits);
    case OPND_MODRM_WORDPTR:
        // mov r32, sreg writes the whole register (the selector zero extended)
        if (entry->m_secondOperand == OPND_GP_SEGMENT_MODRM)
            return decodeModrm(instruction, operandBits, 16);
        return decodeModrm(instruction, 16, 16);
    case OPND_MODRM_BYTEPTR:
        return decodeModrm(instruction, 8, 8);
    case OPND_MODRM_QWORDPTR:
        return decodeModrm(instruction, 64, 64);
    case OPND_MODRM_TWORDPTR:
        return decodeModrm(instruction, 64, 80);
    case OPND_MODRM_MEM:
        // lea, les, bound: only the address is of interest. The size is the
        // size of the register operand
        CHECK(modrm.m_bits.m_mod != 3);
        return decodeMemory(instruction, operandBits);
    case OPND_MODRM_FAR_OFFSET:
        // An offset and a selector
        CHECK(modrm.m_bits.m_mod != 3);
        return decodeMemory(instruction, operandBits + 16);
    case OPND_MMX_RM:
        return decodeModrm(instruction, 64, 64);
    case OPND_SIMD_RM:
        return decodeModrm(instruction, 128, 128);
    case OPND_SIMD_RM_QWORD:
        return decodeModrm(instruction, 128, 64);
    case OPND_SIMD_RM_DWORD:
        return decodeModrm(instruction, 128, 32);
    case OPND_MODRM_R32_WORDPTR:
        return decodeModrm(instruction, 32, 16);

    //////////////////////////////////////////////////////////////////
    // Immediates and branch targets
    //////////////////////////////////////////////////////////////////
    case OPND_ONE:
        return makeImmediate(1, 8, IA32DecodedOperand::KIND_IMMEDIATE);
    case OPND_THREE:
        return makeImmediate(3, 8, IA32DecodedOperand::KIND_IMMEDIATE);
    case OPND_IMMEDIATE_8BIT:
    {
        // The second immediate of 'enter imm16, imm8'
        if (entry->m_firstOperand == OPND_IMMEDIATE_16BIT)
            return makeImmediate((uint8)immediate.secondOffset, 8,
                                 IA32DecodedOperand::KIND_IMMEDIATE);
        // The sign extended immediates of the operand size: add r/m, imm8
        // (83), imul r, r/m, imm8 (6B) and push imm8 (6A)
        const bool isOneByteTable = !isTwoBytesOpcode(instruction);
        if (isOneByteTable && ((entry->m_prefix == 0x83) ||
                               (entry->m_prefix == 0x6B) ||
                               (entry->m_prefix == 0x6A)))
            return makeImmediate(signExtend(immediate.offset, 8), operandBits,
                                 IA32DecodedOperand::KIND_IMMEDIATE);
        // The unsigned immediates: int, in, out, aam, aad
        if (isOneByteTable && ((entry->m_prefix == 0xCD) ||
                               ((entry->m_prefix >= 0xE4) && (entry->m_prefix <= 0xE7)) ||
                               (entry->m_prefix == 0xD4) ||
                               (entry->m_prefix == 0xD5)))
            return makeImmediate((uint8)immediate.offset, 8,
                                 IA32DecodedOperand::KIND_IMMEDIATE);
        return makeImmediate(signExtend(immediate.offset, 8), 8,
                             IA32DecodedOperand::KIND_IMMEDIATE);
    }
    case OPND_IMMEDIATE_16BIT:
        return makeImmediate((uint16)immediate.offset, 16,
                             IA32DecodedOperand::KIND_IMMEDIATE);
    case OPND_IMMEDIATE_DS:
        return makeImmediate(signExtend(immediate.offset, operandBits),
                             operandBits, IA32DecodedOperand::KIND_IMMEDIATE);
    case OPND_MEMREF_OFFSET_DS:
    {
        // mov al/eax, [moffs] and mov [moffs], al/eax: the access size is
        // the size of the register operand (A0 and A2 are the byte forms)
        IA32DecodedOperand operand = makeOperand(
                    IA32DecodedOperand::KIND_MEMORY,
                    ((entry->m_prefix & 1) == 0) ? 8 : operandBits);
        operand.m_segment = getSegment(instruction, IA32_REG_DS);
        operand.m_isSegmentOverride =
                    (getSegment(instruction, 0xFFFFFFFF) != 0xFFFFFFFF);
        operand.m_displacement = (int32)signExtend(immediate.offset, addressBits);
        operand.m_addressBits = addressBits;
        return operand;
    }
    case OPND_IMMEDIATE_OFFSET_SHORT_8:
    case OPND_IMMEDIATE_OFFSET_LONG_32:
    case OPND_IMMEDIATE_OFFSET_DS:
    {
        // A relative branch: rel8, or rel16 / rel32 by the operand size. The
        // target is relative to the next instruction and wraps at 16 bits
        // for a 16 bit operand size
        const uint relativeBits = (type == OPND_IMMEDIATE_OFFSET_SHORT_8) ? 8 :
                                                                         operandBits;
        uint64 target = getNextAddress(instruction) +
                        (uint64)signExtend(immediate.offset, relativeBits);
        if (operandBits == 16)
            target&= 0xFFFF;
        else
            target&= 0xFFFFFFFF;
        return makeImmediate((int64)target, operandBits,
                             IA32DecodedOperand::KIND_BRANCH_TARGET);
    }
    case OPND_IMMEDIATE_OFFSET_FAR:
    {
        IA32DecodedOperand operand = makeImmediate((int64)immediate.offset,
                                    operandBits + 16,
                                    IA32DecodedOperand::KIND_FAR_POINTER);
        operand.m_farSegment = immediate.segment;
        return operand;
    }

    default:
        CHECK_FAIL();
    }
}
