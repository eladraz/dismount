#include "dismount/dismount.h"
/*
 * opcodeTable.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/Opcode.h"

namespace ia32dis {

RegisterDescription gIa8Registers[NUMBER_OF_REGISTERS] = {
    {"al"},
    {"cl"},
    {"dl"},
    {"bl"},
    {"ah"},
    {"ch"},
    {"dh"},
    {"bh"}
};

RegisterDescription gIa16Registers[NUMBER_OF_REGISTERS] = {
    {"ax"},
    {"cx"},
    {"dx"},
    {"bx"},
    {"sp"},
    {"bp"},
    {"si"},
    {"di"}
};

RegisterDescription gIa32Registers[NUMBER_OF_REGISTERS] = {
    {"eax"},
    {"ecx"},
    {"edx"},
    {"ebx"},
    {"esp"},
    {"ebp"},
    {"esi"},
    {"edi"}
};

RegisterDescription gIa32MMXRegisters[NUMBER_OF_REGISTERS] = {
    {"mm0"},
    {"mm1"},
    {"mm2"},
    {"mm3"},
    {"mm4"},
    {"mm5"},
    {"mm6"},
    {"mm7"},
};

RegisterDescription gIa32SIMDRegisters[NUMBER_OF_REGISTERS] = {
    {"xmm0"},
    {"xmm1"},
    {"xmm2"},
    {"xmm3"},
    {"xmm4"},
    {"xmm5"},
    {"xmm6"},
    {"xmm7"},
};

RegisterDescription gIa32SegmentsRegisters[NUMBER_OF_SEGMENTS_REGISTERS] = {
    {"es"},
    {"cs"},
    {"ss"},
    {"ds"},
    {"fs"},
    {"gs"},
};

RegisterDescription gIa32ControlRegisters[NUMBER_OF_CONTROL_REGISTERS] = {
    {"cr0"},
    {"cr1"},
    {"cr2"},
    {"cr3"},
    {"cr4"},
};

RegisterDescription gIa32DebugRegisters[NUMBER_OF_REGISTERS] = {
    {"dr0"},
    {"dr1"},
    {"dr2"},
    {"dr3"},
    {"dr4"},
    {"dr5"},
    {"dr6"},
    {"dr7"},
};

const ModRMTranslation gIa32ModRM16[MODRM_MODE_COUNT][MODRM_RM_COUNT] = {
    // Mode 00
    {{ true, IA32_GP16_BX, IA32_GP16_SI, 0, false, false },
     { true, IA32_GP16_BX, IA32_GP16_DI, 0, false, false },
     { true, IA32_GP16_BP, IA32_GP16_SI, 0, false, false },
     { true, IA32_GP16_BP, IA32_GP16_DI, 0, false, false },
     { true, IA32_GP16_SI, NO_REGISTER,  0, false, false },
     { true, IA32_GP16_DI, NO_REGISTER,  0, false, false },
     { true, NO_REGISTER,  NO_REGISTER,  2, true,  false },
     { true, IA32_GP16_BX, NO_REGISTER,  0, false, false }},
     // Mode 01
    {{ true, IA32_GP16_BX, IA32_GP16_SI, 1, true,  false },
     { true, IA32_GP16_BX, IA32_GP16_DI, 1, true,  false },
     { true, IA32_GP16_BP, IA32_GP16_SI, 1, true,  false },
     { true, IA32_GP16_BP, IA32_GP16_DI, 1, true,  false },
     { true, IA32_GP16_SI, NO_REGISTER,  1, true,  false },
     { true, IA32_GP16_DI, NO_REGISTER,  1, true,  false },
     { true, IA32_GP16_BP, NO_REGISTER,  1, true,  false },
     { true, IA32_GP16_BX, NO_REGISTER,  1, true,  false }},
     // Mode 10
    {{ true, IA32_GP16_BX, IA32_GP16_SI, 2, true,  false },
     { true, IA32_GP16_BX, IA32_GP16_DI, 2, true,  false },
     { true, IA32_GP16_BP, IA32_GP16_SI, 2, true,  false },
     { true, IA32_GP16_BP, IA32_GP16_DI, 2, true,  false },
     { true, IA32_GP16_SI, NO_REGISTER,  2, true,  false },
     { true, IA32_GP16_DI, NO_REGISTER,  2, true,  false },
     { true, IA32_GP16_BP, NO_REGISTER,  2, true,  false },
     { true, IA32_GP16_BX, NO_REGISTER,  2, true,  false }},
     // Mode 11 TODO! Unknown!!
    {{ false, IA32_GP16_AX, NO_REGISTER,  0, true,  false },
     { false, IA32_GP16_CX, NO_REGISTER,  0, true,  false },
     { false, IA32_GP16_DX, NO_REGISTER,  0, true,  false },
     { false, IA32_GP16_BX, NO_REGISTER,  0, true,  false },
     { false, IA32_GP16_SP, NO_REGISTER,  0, true,  false },
     { false, IA32_GP16_BP, NO_REGISTER,  0, true,  false },
     { false, IA32_GP16_SI, NO_REGISTER,  0, true,  false },
     { false, IA32_GP16_DI, NO_REGISTER,  0, true,  false }}
};

const ModRMTranslation gIa32ModRM32[MODRM_MODE_COUNT][MODRM_RM_COUNT] = {
    // Mode 00
    {{ true, IA32_GP32_EAX, NO_REGISTER, 0, false, false },
     { true, IA32_GP32_ECX, NO_REGISTER, 0, false, false },
     { true, IA32_GP32_EDX, NO_REGISTER, 0, false, false },
     { true, IA32_GP32_EBX, NO_REGISTER, 0, false, false },
     { true, NO_REGISTER,   NO_REGISTER, 0, false, true  },
     { true, NO_REGISTER,   NO_REGISTER, 4, false, false },
     { true, IA32_GP32_ESI, NO_REGISTER, 0, false, false },
     { true, IA32_GP32_EDI, NO_REGISTER, 0, false, false }},
    // Mode 01
    {{ true, IA32_GP32_EAX, NO_REGISTER, 1, true,  false },
     { true, IA32_GP32_ECX, NO_REGISTER, 1, true,  false },
     { true, IA32_GP32_EDX, NO_REGISTER, 1, true,  false },
     { true, IA32_GP32_EBX, NO_REGISTER, 1, true,  false },
     { true, NO_REGISTER,   NO_REGISTER, 1, true,  true  },
     { true, IA32_GP32_EBP, NO_REGISTER, 1, true,  false },
     { true, IA32_GP32_ESI, NO_REGISTER, 1, true,  false },
     { true, IA32_GP32_EDI, NO_REGISTER, 1, true,  false }},
    // Mode 10
    {{ true, IA32_GP32_EAX, NO_REGISTER, 4, true,  false },
     { true, IA32_GP32_ECX, NO_REGISTER, 4, true,  false },
     { true, IA32_GP32_EDX, NO_REGISTER, 4, true,  false },
     { true, IA32_GP32_EBX, NO_REGISTER, 4, true,  false },
     { true, NO_REGISTER,   NO_REGISTER, 4, false, true  },
     { true, IA32_GP32_EBP, NO_REGISTER, 4, true,  false },
     { true, IA32_GP32_ESI, NO_REGISTER, 4, true,  false },
     { true, IA32_GP32_EDI, NO_REGISTER, 4, true,  false }},
    // Mode 11 TODO! Unknown!!
    {{ false, IA32_GP32_EAX, NO_REGISTER,  0, false, false },
     { false, IA32_GP32_ECX, NO_REGISTER,  0, false, false },
     { false, IA32_GP32_EDX, NO_REGISTER,  0, false, false },
     { false, IA32_GP32_EBX, NO_REGISTER,  0, false, false },
     { false, IA32_GP32_ESP, NO_REGISTER,  0, false, false },
     { false, IA32_GP32_EBP, NO_REGISTER,  0, false, false },
     { false, IA32_GP32_ESI, NO_REGISTER,  0, false, false },
     { false, IA32_GP32_EDI, NO_REGISTER,  0, false, false }}
};

OpcodePrefixEntry gIa32PrefixTable[IA32_NUMBER_OF_PREFIXS] = {
    {"lock",         0xF0, false,  true},
    {"repne",        0xF2, false,  true},
    {"rep",          0xF3, false,  true},
    {"cs:",          0x2E, true,   true},
    {"ss:",          0x36, true,   true},
    {"ds:",          0x3E, true,   true},
    {"es:",          0x26, true,   true},
    {"fs:",          0x64, true,   true},
    {"gs:",          0x65, true,   true},
    {"operandSize",  0x66, false,  false},
    {"addressSize",  0x67, false,  false}
};

extern uint8 gIa32PrefixGroup3OperandSize = 0x66;

extern uint8 gIa32PrefixGroup4AddressSize = 0x67;

const uint8 gIa32TwoByteEscapeCharacter = 0x0F;

const uint8 gIa32FPUStartEscapeCharacter = 0xD8;
const uint8 gIa32FPUEndEscapeCharacter   = 0xDF;

// Two-bytes opcode table 0x0F
const OpcodeEntry gIa32TwoBytesOpcodeTable[] = {
    //////////////////////////////////////////////////////////////////////////
    // 286, 386 instructions (0x00-0x35)
    // Name        PREFIX  MASK   FIRST-OPERAND        SECONDS-OPERAND       THIRD-OPERAND        MODRM-FILTER                      UNSIGNED  ALTERING        EXAMPLE
    {"sldt",       0x00,   0xFF,  OPND_MODRM_WORDPTR,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000,  true,     Opcode::FLOW_NO_ALTER}, // sldt    word ptr [edx+306h]
    {"str",        0x00,   0xFF,  OPND_MODRM_WORDPTR,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_001,  true,     Opcode::FLOW_NO_ALTER}, // str     word ptr [edx+304h]
    {"lldt",       0x00,   0xFF,  OPND_GP_16_32BIT,    OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_010,  true,     Opcode::FLOW_NO_ALTER}, // lldt    ax
    {"ltr",        0x00,   0xFF,  OPND_GP_16_32BIT,    OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_011,  true,     Opcode::FLOW_NO_ALTER}, // ltr     word ptr [edx+304h]
    {"sgdt",       0x01,   0xFF,  OPND_MODRM_dWORDPTR, OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000,  true,     Opcode::FLOW_NO_ALTER}, // sgdt    fword ptr [edx+2F6h]
    {"sidt",       0x01,   0xFF,  OPND_MODRM_dWORDPTR, OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_001,  true,     Opcode::FLOW_NO_ALTER}, // sidt    fword ptr [edx+2FEh]
    {"lgdt",       0x01,   0xFF,  OPND_MODRM_dWORDPTR, OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_010,  true,     Opcode::FLOW_NO_ALTER}, // lgdt    fword ptr [edx+2F6h]
    {"lidt",       0x01,   0xFF,  OPND_MODRM_dWORDPTR, OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_011,  true,     Opcode::FLOW_NO_ALTER}, // lidt    fword ptr [ebp+var_E]
    {"invlpg",     0x01,   0xFF,  OPND_MODRM_BYTEPTR,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_111,  true,     Opcode::FLOW_NO_ALTER}, // invlpg  byte ptr [eax]
    {"lar",        0x02,   0xFF,  OPND_MODRM_dWORDPTR, OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // lar     eax, eax
    {"clts",       0x06,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // clts
    {"wbinvd",     0x09,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // wbinvd
    {"prefetchnta",0x18,   0xFF,  OPND_MODRM_BYTEPTR,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000,  true,     Opcode::FLOW_NO_ALTER}, // prefetchnta byte ptr [ecx+0]
    {"mov",        0x20,   0xFF,  OPND_MODRM_dWORDPTR, OPND_CTRL_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // mov     eax, cr0
    {"mov",        0x21,   0xFF,  OPND_MODRM_dWORDPTR, OPND_DBG_MODRM,       OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // mov     eax, dr0
    {"mov",        0x22,   0xFF,  OPND_CTRL_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // mov     cr0, ecx
    {"mov",        0x23,   0xFF,  OPND_DBG_MODRM,      OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // mov     dr7, ecx
    {"movaps",     0x28,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000,  true,     Opcode::FLOW_NO_ALTER}, // movaps  xmm0, [esp+14h+var_14]
    {"movaps",     0x29,   0xFF,  OPND_MODRM_dWORDPTR, OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000,  true,     Opcode::FLOW_NO_ALTER}, // movaps  [esp+14h+var_14], xmm0
    {"movntps",    0x2B,   0xFF,  OPND_MODRM_dWORDPTR, OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000,  true,     Opcode::FLOW_NO_ALTER}, // movntps xmmword ptr [ecx+0], xmm0
    {"wrmsr",      0x30,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // wrmsr
    // This one is a bit of a mystery
    {"rdtsc",      0x31,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // rdtsc
    {"rdmsr",      0x32,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // rdmsr
    {"rdpmc",      0x33,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // rdpmc
    {"sysexit",    0x35,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // sysexit
    {"xorps",      0x57,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // xorps   xmm0, xmm0

    {"punpcklbw",  0x60,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpcklbw mm2, mm6
    {"punpcklwd",  0x61,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpcklwd mm0, mm0
    {"punpckldq",  0x62,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpckldq mm0, mm0
    {"packuswb",   0x67,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // packuswb mm4, mm5
    {"punpckhbw",  0x68,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpckhbw mm3, mm6
    {"punpckhwd",  0x69,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpckhwd mm1, mm1
    {INVALID,      0x6D,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_INVALID},  // INVALID
    {"movd",       0x6E,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // movd    mm0, dword ptr [esi]
    {"movq",       0x6F,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // movq    mm7, qword_321FD8

    {"psrlw",      0x71,   0xFF,  OPND_SIMD_MODRM,     OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_010,  true,     Opcode::FLOW_NO_ALTER}, // psrlw   mm2, 8
    {"psrld",      0x72,   0xFF,  OPND_SIMD_MODRM,     OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_010,  true,     Opcode::FLOW_NO_ALTER}, // psrld   mm0, 18h
    {"emms",       0x77,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // emms
    {"movd",       0x7E,   0xFF,  OPND_MODRM_dWORDPTR, OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // movd    dword ptr [edi], mm0
    {"movq",       0x7F,   0xFF,  OPND_MODRM_dWORDPTR, OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // movq    mm1, mm0

    // First table two-byte opcode, 0x80-0xF7

    // Name        PREFIX  MASK   FIRST-OPERAND                   SECONDS-OPERAND       THIRD-OPERAND        MODRM-FILTER      UNSIGNED  ALTERING            EXAMPLE
    {"jo",         0x80,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_OVERFLOW},
    {"jno",        0x81,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_OVERFLOW | Opcode::FLOW_COND_NOT},
    {"jb",         0x82,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER},
    {"jc",         0x82,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_CARRY},
    {"jnae",       0x82,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER},
    {"jae",        0x83,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},
    {"jnb",        0x83,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},
    {"jnc",        0x83,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_CARRY | Opcode::FLOW_COND_NOT},
    {"jz",         0x84,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO},
    {"je",         0x84,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO},
    {"jnz",        0x85,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO | Opcode::FLOW_COND_NOT},
    {"jne",        0x85,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO | Opcode::FLOW_COND_NOT},
    {"jna",        0x86,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},
    {"jbe",        0x86,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},
    {"ja",         0x87,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER},
    {"jnbe",       0x87,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER},

    {"seto",       0x90,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setno",      0x91,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setb",       0x92,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setc",       0x92,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setnae",     0x92,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setae",      0x93,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setnb",      0x93,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setnc",      0x93,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setz",       0x94,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"sete",       0x94,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setnz",      0x95,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setne",      0x95,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setna",      0x96,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setbe",      0x96,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"seta",       0x97,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setnbe",     0x97,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},

    {"push",       0xA0,   0xFF,  OPND_FS,                        OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},
    {"pop",        0xA1,   0xFF,  OPND_FS,                        OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // pop     fs
    {"cpuid",      0xA2,   0xFF,  OPND_NO_OPERAND,                OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},
    {"bt",         0xA3,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    // TODO! The following two instruction are not fully understood for me
    {"shld",       0xA4,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"shld",       0xA5,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_CL,             MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"cmpxchg",    0xA6,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // cmpxchg dl, cl
    {"push",       0xA8,   0xFF,  OPND_GS,                        OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // push    gs
    {"pop",        0xA9,   0xFF,  OPND_GS,                        OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // pop     gs
    {"bts",        0xAB,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // bts     [esp+24h+var_24], eax
    {"shrd",       0xAC,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // shrd    eax, ecx, 0Ch
    {"shrd",       0xAD,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_CL,             MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // shrd    eax, edx, cl
    {"fxsave",     0xAE,   0xFF,  OPND_IMMEDIATE_8BIT,            OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // fxsave  dword ptr [ecx]
    {"imul",       0xAF,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // imul    esi, ecx
    // Slots 6,7 are empty

    {"cmpxchg",    0xB0,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"cmpxchg",    0xB1,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"lss",        0xB2,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // lss     sp, [esp-60h+arg_5C]
    // TODO! Complete the table here
    {"btr",        0xB3,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"movzx",      0xB6,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"movzx",      0xB7,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_WORDPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"bt",         0xBA,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER}, // bt      word ptr [esp+arg_68], 0
    {"btr",        0xBA,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER}, // btr     ecx, 1
    {"bts",        0xBA,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER}, // bts ds:dword_473E58, 0
    {"bsf",        0xBC,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // bsf     ecx, eax
    {"bsr",        0xBD,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // bsr     edx, ebx
    // TODO! Complete the table here
    {"xadd",       0xC0,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"xadd",       0xC1,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"movnti",     0xC3,   0xFF,  OPND_IMMEDIATE_8BIT,            OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // movnti  dword ptr [edi], eax
    {"cmpxchg8b",  0xC7,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // cmpxchg8b qword ptr [ebp+0]
    {"bswap",      0xC8,   0xFF,  OPND_eAX,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // bswap   eax
    {"bswap",      0xC9,   0xFF,  OPND_CL,                        OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // bswap   ecx
    {"bswap",      0xCA,   0xFF,  OPND_eAX,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // bswap   edx
    {"bswap",      0xCE,   0xFF,  OPND_eSI,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // bswap   esi

    {"pmullw",     0xD5,   0xFF,  OPND_SIMD_MODRM,                OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // pmullw  mm2, mm0
    {"paddusb",    0xDC,   0xFF,  OPND_SIMD_MODRM,                OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // paddusb mm4, qword ptr [esi-8]
    {"paddusw",    0xDD,   0xFF,  OPND_SIMD_MODRM,                OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // paddusw mm2, mm7

    {"pxor",       0xEF,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // pxor    mm6, mm6

    //////////////////////////////////////////////////////////////////////////
    // Second table two-byte opcode, 0x88-0xFF

    {"js",         0x88,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_NEGATIVE},
    {"jns",        0x89,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_NEGATIVE | Opcode::FLOW_COND_NOT},
    {"jp",         0x8A,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_PARITY},
    {"jpe",        0x8A,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_PARITY},
    {"jnp",        0x8B,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_PARITY | Opcode::FLOW_COND_NOT},
    {"jpo",        0x8B,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_PARITY | Opcode::FLOW_COND_NOT},
    {"jl",         0x8C,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER},
    {"jnge",       0x8C,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER},
    {"jnl",        0x8D,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},
    {"jge",        0x8D,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},
    {"jng",        0x8E,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},
    {"jle",        0x8E,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},
    {"jg",         0x8F,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER},
    {"jnle",       0x8F,   0xFF,  OPND_IMMEDIATE_OFFSET_LONG_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER},

    {"sets",       0x98,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setns",      0x99,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setp",       0x9A,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setpe",      0x9A,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setnp",      0x9B,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setpo",      0x9B,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setl",       0x9C,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setnge",     0x9C,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setnl",      0x9D,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setge",      0x9D,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setng",      0x9E,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setle",      0x9E,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setg",       0x9F,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"setnle",     0x9F,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},

    // TODO! Complete the table here
    {"movsx",      0xBE,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"movsx",      0xBF,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_WORDPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    // TODO! Complete the table here

    // End marker
    {OPCODEEOT, 0,0, OPND_NO_OPERAND,OPND_NO_OPERAND,OPND_NO_OPERAND, 0, false, Opcode::FLOW_NO_ALTER}
};

const OpcodeEntry gIa32OneByteOpcodeTable[] = {
     // First table one-byte opcode, 0x00-0xF7

    //Name          PREFIX  MASK   FIRST-OPERAND                  SECONDS-OPERAND       THIRD-OPERAND        MODRM-FILTER      UNSIGNED  ALTERING                    EXAMPLE
    {"add",         0x00,   0xFF,  OPND_MODRM_BYTEPTR,            OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // add   [bx+si],  bl
    {"add",         0x01,   0xFF,  OPND_MODRM_dWORDPTR,           OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // add   [bx+si],  bx
    {"add",         0x02,   0xFF,  OPND_GP_8BIT_MODRM,            OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // add   bl,       [bx+si]
    {"add",         0x03,   0xFF,  OPND_GP_16_32BIT,              OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // add   bx,       [bx+si]
    {"add",         0x04,   0xFF,  OPND_AL,                       OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     //
    {"add",         0x05,   0xFF,  OPND_eAX,                      OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     //
    {"push",        0x06,   0xFF,  OPND_ES,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     // push  es
    {"pop",         0x07,   0xFF,  OPND_ES,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     // pop   es

    {"adc",         0x10,   0xFF,  OPND_MODRM_BYTEPTR,            OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // adc   [bx+si],  bl
    {"adc",         0x11,   0xFF,  OPND_MODRM_dWORDPTR,           OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // adc   [bx+si],  bx
    {"adc",         0x12,   0xFF,  OPND_GP_8BIT_MODRM,            OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // adc   bl,       [bx+si]
    {"adc",         0x13,   0xFF,  OPND_GP_16_32BIT,              OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // adc   bx,       [bx+si]
    {"adc",         0x14,   0xFF,  OPND_AL,                       OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     //
    {"adc",         0x15,   0xFF,  OPND_eAX,                      OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     //
    {"push",        0x16,   0xFF,  OPND_SS,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     // push  ss
    {"pop",         0x17,   0xFF,  OPND_SS,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     // pop   ss

    {"and",         0x20,   0xFF,  OPND_MODRM_BYTEPTR,            OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // and   [bx+si],  bl
    {"and",         0x21,   0xFF,  OPND_MODRM_dWORDPTR,           OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // and   [bx+si],  bx
    {"and",         0x22,   0xFF,  OPND_GP_8BIT_MODRM,            OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // and   bl,       [bx+si]
    {"and",         0x23,   0xFF,  OPND_GP_16_32BIT,              OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // and   bx,       [bx+si]
    {"and",         0x24,   0xFF,  OPND_AL,                       OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     //
    {"and",         0x25,   0xFF,  OPND_eAX,                      OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     //
    {INVALID,       0x26,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_INVALID},          // INVALID! ES:
    {"daa",         0x27,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     // daa

    {"xor",         0x30,   0xFF,  OPND_MODRM_BYTEPTR,            OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // xor   [bx+si],  bl
    {"xor",         0x31,   0xFF,  OPND_MODRM_dWORDPTR,           OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // xor   [bx+si],  bx
    {"xor",         0x32,   0xFF,  OPND_GP_8BIT_MODRM,            OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // xor   bl,       [bx+si]
    {"xor",         0x33,   0xFF,  OPND_GP_16_32BIT,              OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},     // xor   bx,       [bx+si]
    {"xor",         0x34,   0xFF,  OPND_AL,                       OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     //
    {"xor",         0x35,   0xFF,  OPND_eAX,                      OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     //
    {INVALID,       0x36,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_INVALID},          // INVALID! SS:
    {"aaa",         0x37,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     // aaa

    {"inc",         0x40,   0xF8,  OPND_ONEBYTES_OPCODE_GP_16_32, OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_NO_ALTER},     // inc  eAX
    {"push",        0x50,   0xF8,  OPND_ONEBYTES_OPCODE_GP_16_32, OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_NO_ALTER},     // push eAX
    {"push",        0x53,   0xFF,  OPND_eBX,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     // push    ebx
    {"push",        0x55,   0xFF,  OPND_eBP,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     // push    ebp
    {"push",        0x57,   0xFF,  OPND_eDI,                       OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER},     // push    edi

    {"pusha#d",     0x60,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_NO_ALTER},     // pusha/d
    {"popa#d",      0x61,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_NO_ALTER},     // popa/d
    {"bound",       0x62,   0xFF,  OPND_GP_16_32BIT,              OPND_MODRM_MEM,       OPND_NO_OPERAND,     MODRM_ALL_FILTER, false,    Opcode::FLOW_NO_ALTER},     // TODO!
    {"arpl",        0x63,   0xFF,  OPND_MODRM_WORDPTR,            OPND_GP_16BIT_MODRM,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, false,    Opcode::FLOW_NO_ALTER},     // TODO!

    {INVALID,       0x64,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     0,                true,     Opcode::FLOW_INVALID},          // INVALID! FS:
    {INVALID,       0x65,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     0,                true,     Opcode::FLOW_INVALID},          // INVALID! GS:
    {INVALID,       0x66,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     0,                true,     Opcode::FLOW_INVALID},          // INVALID! DB 66
    {INVALID,       0x67,   0xFF,  OPND_NO_OPERAND,               OPND_NO_OPERAND,      OPND_NO_OPERAND,     0,                true,     Opcode::FLOW_INVALID},          // INVALID! DB 67

    {"jo",          0x70,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_OVERFLOW},             // jo     $+5
    {"jno",         0x71,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_OVERFLOW | Opcode::FLOW_COND_NOT},             // jo     $+5
    {"jb",          0x72,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER},             // jo     $+5
    {"jnb",         0x73,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},             // jo     $+5
    {"jz",          0x74,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO},             // jo     $+5
    {"jnz",         0x75,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO | Opcode::FLOW_COND_NOT},             // jo     $+5
    {"jbe",         0x76,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},             // jo     $+5
    {"ja",          0x77,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER},             // jo     $+5

    {"add",         0x80,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"add",         0x81,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"add",         0x82,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"add",         0x83,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"or",          0x80,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"or",          0x81,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"or",          0x82,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"or",          0x83,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"adc",         0x80,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"adc",         0x81,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"adc",         0x82,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"adc",         0x83,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"sbb",         0x80,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"sbb",         0x81,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"sbb",         0x82,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"sbb",         0x83,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"and",         0x80,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"and",         0x81,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"and",         0x82,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"and",         0x83,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"sub",         0x80,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"sub",         0x81,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"sub",         0x82,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"sub",         0x83,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"xor",         0x80,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"xor",         0x81,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"xor",         0x82,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"xor",         0x83,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"cmp",         0x80,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},
    {"cmp",         0x81,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},
    {"cmp",         0x82,   0xFF, OPND_MODRM_BYTEPTR,             OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},
    {"cmp",         0x83,   0xFF, OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},

    {"test",        0x84,   0xFF, OPND_MODRM_BYTEPTR,             OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},           // test al, [bx+si]    TODO! Wrong doc
    {"test",        0x85,   0xFF, OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},           // test ax, [bx+si]    TODO! Wrong doc
    {"xchg",        0x86,   0xFF, OPND_MODRM_BYTEPTR,             OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},           // test al, [bx+si]    TODO! Wrong doc
    {"xchg",        0x87,   0xFF, OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},           // test ax, [bx+si]    TODO! Wrong doc

    // NOTE: How the scanning algorithm exclude opcode 0x90 to be nop instead of XCHG EAX,EAX
    {"nop",         0x90,   0xFF, OPND_NO_OPERAND,                OPND_NO_OPERAND,  OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"xchg",        0x91,   0xFF, OPND_eAX,  OPND_ONEBYTES_OPCODE_GP_16_32,         OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER}, // xchg    eax, ecx
    {"xchg",        0x92,   0xFF, OPND_eAX,  OPND_ONEBYTES_OPCODE_GP_16_32,         OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER}, // xchg    eax, edx
    {"xchg",        0x93,   0xFF, OPND_eAX,  OPND_ONEBYTES_OPCODE_GP_16_32,         OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER}, // xchg    eax, ebx
    {"xchg",        0x94,   0xFF, OPND_eAX,  OPND_ONEBYTES_OPCODE_GP_16_32,         OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER}, // xchg    eax, esp
    {"xchg",        0x95,   0xFF, OPND_eAX,  OPND_ONEBYTES_OPCODE_GP_16_32,         OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER}, // xchg    eax, ebp
    {"xchg",        0x96,   0xFF, OPND_eAX,  OPND_ONEBYTES_OPCODE_GP_16_32,         OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER}, // xchg    eax, esi
    {"xchg",        0x97,   0xFF, OPND_eAX,  OPND_ONEBYTES_OPCODE_GP_16_32,         OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER}, // xchg    eax, edi

    {"mov",         0xA0,   0xFF, OPND_AL,               OPND_MEMREF_OFFSET_DS, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"mov",         0xA1,   0xFF, OPND_eAX,              OPND_MEMREF_OFFSET_DS, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"mov",         0xA2,   0xFF, OPND_MEMREF_OFFSET_DS, OPND_AL,               OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"mov",         0xA3,   0xFF, OPND_MEMREF_OFFSET_DS, OPND_eAX,              OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"movsb",       0xA4,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"movs##",      0xA5,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"cmpsb",       0xA6,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"cmps##",      0xA7,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},

    {"mov",         0xB0,   0xF8, OPND_ONEBYTES_OPCODE_GP_8, OPND_IMMEDIATE_8BIT, OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},

    {"rol",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"rol",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"ror",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"ror",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"rcl",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"rcl",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"rcr",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"rcr",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shr",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"shr",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {INVALID,       0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_INVALID},  // INVALID
    {INVALID,       0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_INVALID},  // INVALID
    {"sar",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},
    {"sar",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},

    {"retn",        0xC2,   0xFF, OPND_IMMEDIATE_16BIT,  OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_RET | Opcode::FLOW_ACTION},
    {"ret",         0xC3,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_RET | Opcode::FLOW_ACTION},
    {"les",         0xC4,   0xFF, OPND_GP_16_32BIT,      OPND_MODRM_MEM,        OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"lds",         0xC5,   0xFF, OPND_GP_16_32BIT,      OPND_MODRM_MEM,        OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    // All other modes 0f opcodes C6 C7 are invalid (Left empty)
    {"mov",         0xC6,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {INVALID,       0xC6,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_INVALID}, // INVALID
    {INVALID,       0xC6,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_INVALID}, // INVALID
    {"mov",         0xC7,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_DS,     OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {INVALID,       0xC7,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_DS,     OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_INVALID}, // INVALID

    {"rol",         0xD0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"rol",         0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"rol",         0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"rol",         0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"ror",         0xD0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"ror",         0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"ror",         0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"ror",         0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"rcl",         0xD0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"rcl",         0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"rcl",         0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"rcl",         0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"rcr",         0xD0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"rcr",         0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"rcr",         0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"rcr",         0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xD0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shr",         0xD0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"shr",         0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"shr",         0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"shr",         0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {INVALID,       0xD0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_INVALID},
    {INVALID,       0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_INVALID},
    {INVALID,       0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_INVALID},
    {INVALID,       0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_INVALID},
    {"sar",         0xD0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},
    {"sar",         0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},
    {"sar",         0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},
    {"sar",         0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},


    {"aam",         0xD4,   0xFF, OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"aad",         0xD5,   0xFF, OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {INVALID,       0xD6,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, 0,              false, Opcode::FLOW_INVALID},                   // INVALID! EMPTY
    {"xlatb",       0xD7,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},

    {"loopnz",      0xE0,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"loopz",       0xE1,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"loop",        0xE2,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"j#ecxz",      0xE3,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"in",          0xE4,   0xFF, OPND_AL,                       OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"in",          0xE5,   0xFF, OPND_eAX,                      OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"out",         0xE6,   0xFF, OPND_IMMEDIATE_8BIT,           OPND_AL,               OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"out",         0xE7,   0xFF, OPND_IMMEDIATE_8BIT,           OPND_eAX,              OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},

    {INVALID,       0xF0,   0xFF, OPND_NO_OPERAND,      OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_INVALID}, // INVALID! LOCK prefix
    {INVALID,       0xF1,   0xFF, OPND_NO_OPERAND,      OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_INVALID}, // INVALID! EMPTY
    {INVALID,       0xF2,   0xFF, OPND_NO_OPERAND,      OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_INVALID}, // INVALID! REPNE
    {INVALID,       0xF3,   0xFF, OPND_NO_OPERAND,      OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_INVALID}, // INVALID! REP
    {"hlt",         0xF4,   0xFF, OPND_NO_OPERAND,      OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"cmc",         0xF5,   0xFF, OPND_NO_OPERAND,      OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},

    {"test",        0xF6,   0xFF, OPND_MODRM_BYTEPTR,   OPND_IMMEDIATE_8BIT,    OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"test",        0xF7,   0xFF, OPND_MODRM_dWORDPTR,  OPND_IMMEDIATE_DS,      OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
                                                                                                    // MODRM_MOD_001 not exist
    {"not",         0xF6,   0xFF, OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"not",         0xF7,   0xFF, OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"neg",         0xF6,   0xFF, OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"neg",         0xF7,   0xFF, OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"mul",         0xF6,   0xFF, OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},  //eax:edx * reg/m8
    {"mul",         0xF7,   0xFF, OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},  //eax:edx * reg/m16/32
    {"imul",        0xF6,   0xFF, OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"imul",        0xF7,   0xFF, OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"div",         0xF6,   0xFF, OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"div",         0xF7,   0xFF, OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"idiv",        0xF6,   0xFF, OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},
    {"idiv",        0xF7,   0xFF, OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,        OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},

    // Second table one-byte opcode, 0x08-0xFF

     // Name        PREFIX  MASK   FIRST-OPERAND         SECONDS-OPERAND        THIRD-OPERAND    MODRM-FILTER                EXAMPLE
    {"or",          0x08,   0xFF,  OPND_MODRM_BYTEPTR,   OPND_GP_8BIT_MODRM,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"or",          0x09,   0xFF,  OPND_MODRM_dWORDPTR,  OPND_GP_16_32BIT,      OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"or",          0x0A,   0xFF,  OPND_GP_8BIT_MODRM,   OPND_MODRM_BYTEPTR,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"or",          0x0B,   0xFF,  OPND_GP_16_32BIT,     OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"or",          0x0C,   0xFF,  OPND_AL,              OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"or",          0x0D,   0xFF,  OPND_eAX,             OPND_IMMEDIATE_DS,     OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"push",        0x0E,   0xFF,  OPND_CS,              OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},         // push  cs
    {INVALID,       0x0F,   0xFF,  OPND_NO_OPERAND,      OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_INVALID},              // INVALID! 2Bytes opcode marker

    {"sbb",         0x18,   0xFF,  OPND_MODRM_BYTEPTR,   OPND_GP_8BIT_MODRM,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"sbb",         0x19,   0xFF,  OPND_MODRM_dWORDPTR,  OPND_GP_16_32BIT,      OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"sbb",         0x1A,   0xFF,  OPND_GP_8BIT_MODRM,   OPND_MODRM_BYTEPTR,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"sbb",         0x1B,   0xFF,  OPND_GP_16_32BIT,     OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"sbb",         0x1C,   0xFF,  OPND_AL,              OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"sbb",         0x1D,   0xFF,  OPND_eAX,             OPND_IMMEDIATE_DS,     OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"push",        0x1E,   0xFF,  OPND_DS,              OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},         // push  ds
    {"pop",         0x1F,   0xFF,  OPND_DS,              OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},         // pop   ds

    {"sub",         0x28,   0xFF,  OPND_MODRM_BYTEPTR,   OPND_GP_8BIT_MODRM,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"sub",         0x29,   0xFF,  OPND_MODRM_dWORDPTR,  OPND_GP_16_32BIT,      OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"sub",         0x2A,   0xFF,  OPND_GP_8BIT_MODRM,   OPND_MODRM_BYTEPTR,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"sub",         0x2B,   0xFF,  OPND_GP_16_32BIT,     OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"sub",         0x2C,   0xFF,  OPND_AL,              OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"sub",         0x2D,   0xFF,  OPND_eAX,             OPND_IMMEDIATE_DS,     OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {INVALID,       0x2E,   0xFF,  OPND_NO_OPERAND,      OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_INVALID},              // INVALID! CS:
    {"das",         0x2F,   0xFF,  OPND_NO_OPERAND,      OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},

    {"cmp",         0x38,   0xFF,  OPND_MODRM_BYTEPTR,   OPND_GP_8BIT_MODRM,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"cmp",         0x39,   0xFF,  OPND_MODRM_dWORDPTR,  OPND_GP_16_32BIT,      OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"cmp",         0x3A,   0xFF,  OPND_GP_8BIT_MODRM,   OPND_MODRM_BYTEPTR,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"cmp",         0x3B,   0xFF,  OPND_GP_16_32BIT,     OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"cmp",         0x3C,   0xFF,  OPND_AL,              OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"cmp",         0x3D,   0xFF,  OPND_eAX,             OPND_IMMEDIATE_DS,     OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {INVALID,       0x3E,   0xFF,  OPND_NO_OPERAND,      OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_INVALID},              // INVALID! DS:
    {"aas",         0x3F,   0xFF,  OPND_NO_OPERAND,      OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},

    {"dec",         0x48,   0xF8,  OPND_ONEBYTES_OPCODE_GP_16_32, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},

    {"pop",         0x58,   0xF8,  OPND_ONEBYTES_OPCODE_GP_16_32, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},

    {"push",        0x68,   0xFF,  OPND_IMMEDIATE_DS,    OPND_NO_OPERAND,       OPND_NO_OPERAND,     MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"imul",        0x69,   0xFF,  OPND_GP_16_32BIT,     OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_DS,   MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"push",        0x6A,   0xFF,  OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,       OPND_NO_OPERAND,     MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"imul",        0x6B,   0xFF,  OPND_GP_16_32BIT,     OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"insb",        0x6C,   0xFF,  OPND_NO_OPERAND,      OPND_NO_OPERAND,       OPND_NO_OPERAND,     MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"ins##",       0x6D,   0xFF,  OPND_NO_OPERAND,      OPND_NO_OPERAND,       OPND_NO_OPERAND,     MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"outsb",       0x6E,   0xFF,  OPND_NO_OPERAND,      OPND_NO_OPERAND,       OPND_NO_OPERAND,     MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},
    {"outs##",      0x6F,   0xFF,  OPND_NO_OPERAND,      OPND_NO_OPERAND,       OPND_NO_OPERAND,     MODRM_NO_MODRM,   true, Opcode::FLOW_NO_ALTER},

    {"js",          0x78,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_NEGATIVE},
    {"jns",         0x79,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_NEGATIVE | Opcode::FLOW_COND_NOT},
    {"jp",          0x7A,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_PARITY},
    {"jnp",         0x7B,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_PARITY | Opcode::FLOW_COND_NOT},
    {"jl",          0x7C,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_LOWER},
    {"jnl",         0x7D,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},
    {"jng",         0x7E,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},
    {"jg",          0x7F,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_BIGGER},

    {"mov",         0x88,   0xFF,  OPND_MODRM_BYTEPTR,    OPND_GP_8BIT_MODRM,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"mov",         0x89,   0xFF,  OPND_MODRM_dWORDPTR,   OPND_GP_16_32BIT,      OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"mov",         0x8A,   0xFF,  OPND_GP_8BIT_MODRM,    OPND_MODRM_BYTEPTR,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"mov",         0x8B,   0xFF,  OPND_GP_16_32BIT,      OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"mov",         0x8C,   0xFF,  OPND_MODRM_WORDPTR,    OPND_GP_SEGMENT_MODRM, OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},

    {"lea",         0x8D,   0xFF,  OPND_GP_16_32BIT,      OPND_MODRM_MEM,        OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER}, // lea     eax, ds:40h[edi*8]

    {"mov",         0x8E,   0xFF,  OPND_GP_SEGMENT_MODRM, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"pop",         0x8F,   0xFF,  OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},

    {"cwde",        0x98,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},   // cbw for prefix
    {"cdq",         0x99,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},   // cwd for prefix
    {"callf",       0x9A,   0xFF,  OPND_IMMEDIATE_OFFSET_FAR,  OPND_NO_OPERAND,  OPND_NO_OPERAND, MODRM_NO_MODRM, false,Opcode::FLOW_NO_ALTER},
    {"fwait",       0x9B,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"pushf#d",     0x9C,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"popf#d",      0x9D,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"sahf",        0x9E,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"lahf",        0x9F,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},

    {"test",        0xA8,   0xFF,  OPND_AL,               OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"test",        0xA9,   0xFF,  OPND_eAX,              OPND_IMMEDIATE_DS,     OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"stosb",       0xAA,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"stos##",      0xAB,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"lodsb",       0xAC,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"lods##",      0xAD,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"scasb",       0xAE,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"scas##",      0xAF,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},

    {"mov",         0xB8,   0xF8,  OPND_ONEBYTES_OPCODE_GP_16_32, OPND_IMMEDIATE_DS, OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},

    {"enter",       0xC8,   0xFF,  OPND_IMMEDIATE_16BIT,  OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"leave",       0xC9,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"retf",        0xCA,   0xFF,  OPND_IMMEDIATE_16BIT,  OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_RETF},
    {"retf",        0xCB,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_RETF},
    {"int",         0xCC,   0xFF,  OPND_THREE,            OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_INVALID},
    {"int",         0xCD,   0xFF,  OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_INVALID},
    {"into",        0xCE,   0xFF,  OPND_IMMEDIATE_16BIT,  OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"iret",        0xCF,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_RET},

    //{INVALID,       0xD8,   0xF8,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_INVALID},              // INVALID! ESC to coprocessor

    // TODO: Classify relative jmps
    {"call",        0xE8,   0xFF,  OPND_IMMEDIATE_OFFSET_DS,      OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_STACK_CHANGE | Opcode::FLOW_COND_ALWAYS},
    {"jmp",         0xE9,   0xFF,  OPND_IMMEDIATE_OFFSET_DS,      OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_ALWAYS},
    {"jmp",         0xEA,   0xFF,  OPND_IMMEDIATE_OFFSET_FAR,     OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_ALWAYS},
    {"jmp",         0xEB,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_ALWAYS},
    {"in",          0xEC,   0xFF,  OPND_AL,                       OPND_DX,         OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"in",          0xED,   0xFF,  OPND_eAX,                      OPND_DX,         OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"out",         0xEE,   0xFF,  OPND_DX,                       OPND_AL,         OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},
    {"out",         0xEF,   0xFF,  OPND_DX,                       OPND_eAX,        OPND_NO_OPERAND, MODRM_NO_MODRM, true,  Opcode::FLOW_NO_ALTER},

    {"clc",         0xF8,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"stc",         0xF9,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"cli",         0xFA,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"sti",         0xFB,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"cld",         0xFC,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"std",         0xFD,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},

    {"inc",         0xFE,   0xFF,  OPND_MODRM_BYTEPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,  Opcode::FLOW_NO_ALTER},
    {"inc",         0xFF,   0xFF,  OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,  Opcode::FLOW_NO_ALTER},
    {"dec",         0xFE,   0xFF,  OPND_MODRM_BYTEPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,  Opcode::FLOW_NO_ALTER},
    {"dec",         0xFF,   0xFF,  OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,  Opcode::FLOW_NO_ALTER},
    {INVALID,       0xFE,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true,  Opcode::FLOW_INVALID},
    {"call",        0xFF,   0xFF,  OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, false, Opcode::FLOW_STACK_CHANGE | Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_ACTION},
    {INVALID,       0xFE,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, false, Opcode::FLOW_INVALID},
    {"call",        0xFF,   0xFF,  OPND_MODRM_FAR_OFFSET, OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, false, Opcode::FLOW_STACK_CHANGE | Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_ACTION},
    {INVALID,       0xFE,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, false, Opcode::FLOW_INVALID},
    {"jmp",         0xFF,   0xFF,  OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, false, Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_ACTION},
    {INVALID,       0xFE,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, false, Opcode::FLOW_INVALID},
    {"jmp",         0xFF,   0xFF,  OPND_MODRM_FAR_OFFSET, OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, false, Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_ACTION},
    {INVALID,       0xFE,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, false, Opcode::FLOW_INVALID},
    {"push",        0xFF,   0xFF,  OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, false, Opcode::FLOW_NO_ALTER},
    {INVALID,       0xFE,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, false, Opcode::FLOW_INVALID},
    {INVALID,       0xFF,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, false, Opcode::FLOW_INVALID},

    // End marker
    {OPCODEEOT, 0,0, OPND_NO_OPERAND,OPND_NO_OPERAND,OPND_NO_OPERAND, 0, false, Opcode::FLOW_NO_ALTER}
};

// TODO: Add support for operands with +i (mnemonics st(i))
const OpcodeEntry gIa32FPUOpcodeTable[] = {
    //Name          PREFIX  MASK  FIRST-OPERAND          SECOND-OPERAND         THIRD-OPERAND    MODRM-FILTER                     UNSIGNED  ALTERING                    EXAMPLE
    {"fadd",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,     Opcode::FLOW_NO_ALTER}, // fadd    dword ptr [esi+0]
    {"fmul",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,     Opcode::FLOW_NO_ALTER}, // fmul    dword ptr [eax+4]
    {"fcom",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER}, // fcom    st(3)
    {"fcomp",       0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true,     Opcode::FLOW_NO_ALTER}, // fcomp   dword ptr [edi+90044h]
    {"fsub",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER}, // fsub    st, st(5)
    {"fdiv",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER}, // fdiv    st, st(1)
    {"fdivr",       0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true,     Opcode::FLOW_NO_ALTER}, // fdivr   dword ptr [edi+59h]

    {"fld",         0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,     Opcode::FLOW_NO_ALTER}, // /0
    {"fxch",        0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,     Opcode::FLOW_NO_ALTER}, // /1
    {"fst",         0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER}, // /2
    {"fstp",        0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true,     Opcode::FLOW_NO_ALTER}, // /3
    {"fldenv",      0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER}, // /4
    {"fldcw",       0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true,     Opcode::FLOW_NO_ALTER}, // /5
    {"fnstenv",     0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER}, // /6
    {"fnstcw",      0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true,     Opcode::FLOW_NO_ALTER}, // /7
    {"fld",         0xD9,   0xFF, OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,                  true,     Opcode::FLOW_NO_ALTER}, // D9 XX

    {"fiadd",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,     Opcode::FLOW_NO_ALTER}, // /0
    {"fimul",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,     Opcode::FLOW_NO_ALTER}, // /1
    {"ficom",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER}, // /2
    {"ficomp",      0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true,     Opcode::FLOW_NO_ALTER}, // /3
    {"fisub",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER}, // /4
    {"fisubr",      0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true,     Opcode::FLOW_NO_ALTER}, // /5
    {"fidiv",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER}, // /6
    {"fidivr",      0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true,     Opcode::FLOW_NO_ALTER}, // /7
    {"fucompp",     0xDA,   0xFF, OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,                  true,     Opcode::FLOW_NO_ALTER}, // DA XX

    {"fild",        0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,     Opcode::FLOW_NO_ALTER}, // fild    dword ptr [ecx]
    {"fisttp",      0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,     Opcode::FLOW_NO_ALTER}, // fisttp  dword ptr [eax+eax-54545455h]
    {"fistp",       0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true,     Opcode::FLOW_NO_ALTER}, // fistp   [ebp+arg_0]
    {"fnclex",      0xDB,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER}, // fnclex
    {"fld",         0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true,     Opcode::FLOW_NO_ALTER}, // fld     tbyte_473F50
    {INVALID,       0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true,     Opcode::FLOW_INVALID}, // INVALID
    {"fstp",        0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true,     Opcode::FLOW_NO_ALTER}, // fstp    [ebp+var_1E]

    {"fadd",        0xDC,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,     Opcode::FLOW_NO_ALTER}, // fadd    ds:dbl_405C10
    {"fmul",        0xDC,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,     Opcode::FLOW_NO_ALTER}, // fmul    ds:dbl_4061A8
    {"fcom",        0xDC,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER}, // fcom    [ebp+arg_0]
    {"fcomp",       0xDC,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true,     Opcode::FLOW_NO_ALTER}, // fcomp   ds:dbl_4061A8
    {"fsubr",       0xDC,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true,     Opcode::FLOW_NO_ALTER}, // fsubr   qword ptr [ebx+46h]
    {"fdiv",        0xDC,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER}, // fdiv    qword ptr [ebp-1Ch]

    {"fld",         0xDD,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,     Opcode::FLOW_NO_ALTER}, // fld     qword ptr [ecx]
    {"fldMOD1",     0xDD,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,     Opcode::FLOW_NO_ALTER}, // /1
    {"fst",         0xDD,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER}, // /2
    {"fstp",        0xDD,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true,     Opcode::FLOW_NO_ALTER}, // /3
    {"frstor",      0xDD,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER}, // /4
    {"fldMOD5",     0xDD,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true,     Opcode::FLOW_NO_ALTER}, // /5
    {"fnsave",      0xDD,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER}, // /6
    {"fnstsw",      0xDD,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true,     Opcode::FLOW_NO_ALTER}, // /7
    {"fld",         0xDD,   0xFF, OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,                  true,     Opcode::FLOW_NO_ALTER}, // DD XX

    {INVALID,       0xDE,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,     Opcode::FLOW_INVALID},  // faddp   st, st
    {"fmulp",       0xDE,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,     Opcode::FLOW_INVALID},  // fmulp   st(1), st
    {"fcompp",      0xDE,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true,     Opcode::FLOW_NO_ALTER}, // fcompp
    {"fsubrp",      0xDE,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER}, // fsubrp  st(3), st
    {"fsubp",       0xDE,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true,     Opcode::FLOW_NO_ALTER}, // fsubp   st(1), st
    {"fdivrp",      0xDE,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER}, // fdivrp  st(1), st
    {"fdivp",       0xDE,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true,     Opcode::FLOW_NO_ALTER}, // fdivp   st(1), st

    {"fild",        0xDF,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_000, true,     Opcode::FLOW_NO_ALTER}, // /0
    {"fnstswMOD1",  0xDF,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_001, true,     Opcode::FLOW_NO_ALTER}, // /1
    {"fist",        0xDF,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER}, // /2
    {"fistp1",      0xDF,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_011, true,     Opcode::FLOW_NO_ALTER}, // /3
    {"fbld",        0xDF,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER}, // /4
    {"fild2",       0xDF,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true,     Opcode::FLOW_NO_ALTER}, // /5
    {"fbstp",       0xDF,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER}, // /6
    {"fistp",       0xDF,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true,     Opcode::FLOW_NO_ALTER}, // /7
    {"fnstsw",      0xDF,   0xFF, OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,                  true,     Opcode::FLOW_NO_ALTER}, // DF XX

    // End marker
    {OPCODEEOT, 0,0, OPND_NO_OPERAND,OPND_NO_OPERAND,OPND_NO_OPERAND, 0, false, Opcode::FLOW_NO_ALTER}
};

const char* INVALID = "***";
const char* OPCODEEOT = "---";


}; // end of namespace ia32dis
