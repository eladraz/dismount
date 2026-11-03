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

uint8 gIa32PrefixGroup3OperandSize = 0x66;
uint8 gIa32PrefixGroup4AddressSize = 0x67;

const uint8 gIa32TwoByteEscapeCharacter = 0x0F;
const uint8 gIa32ThreeBytes38EscapeCharacter = 0x38;
const uint8 gIa32ThreeBytes3AEscapeCharacter = 0x3A;

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
    {"lar",        0x02,   0xFF,  OPND_GP_16_32BIT,    OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // lar     eax, eax
    {"clts",       0x06,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // clts
    {"wbinvd",     0x09,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // wbinvd
    {"movss",      0x10,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"movsd",      0x10,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"movupd",     0x10,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movups",     0x10,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"movss",      0x11,   0xFF,  OPND_SIMD_RM_DWORD,  OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"movsd",      0x11,   0xFF,  OPND_SIMD_RM_QWORD,  OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"movupd",     0x11,   0xFF,  OPND_SIMD_RM,        OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movups",     0x11,   0xFF,  OPND_SIMD_RM,        OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"movlpd",     0x12,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movlps",     0x12,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER},
    {"movhlps",    0x12,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,             true,     Opcode::FLOW_NO_ALTER},
    {"movlpd",     0x13,   0xFF,  OPND_SIMD_RM_QWORD,  OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movlps",     0x13,   0xFF,  OPND_SIMD_RM_QWORD,  OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER},
    {"unpcklpd",   0x14,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"unpcklps",   0x14,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"unpckhpd",   0x15,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"unpckhps",   0x15,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"movhpd",     0x16,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movhps",     0x16,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER},
    {"movlhps",    0x16,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,             true,     Opcode::FLOW_NO_ALTER},
    {"movhpd",     0x17,   0xFF,  OPND_SIMD_RM_QWORD,  OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movhps",     0x17,   0xFF,  OPND_SIMD_RM_QWORD,  OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER},
    {"prefetchnta",0x18,   0xFF,  OPND_MODRM_BYTEPTR,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_000, true,     Opcode::FLOW_NO_ALTER},
    {"prefetcht0", 0x18,   0xFF,  OPND_MODRM_BYTEPTR,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_001, true,     Opcode::FLOW_NO_ALTER},
    {"prefetcht1", 0x18,   0xFF,  OPND_MODRM_BYTEPTR,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER},
    {"prefetcht2", 0x18,   0xFF,  OPND_MODRM_BYTEPTR,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_011, true,     Opcode::FLOW_NO_ALTER},
    {"nop",        0x1F,   0xFF,  OPND_MODRM_dWORDPTR, OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // nop     dword ptr [eax]
    {"mov",        0x20,   0xFF,  OPND_MODRM_dWORDPTR, OPND_CTRL_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // mov     eax, cr0
    {"mov",        0x21,   0xFF,  OPND_MODRM_dWORDPTR, OPND_DBG_MODRM,       OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // mov     eax, dr0
    {"mov",        0x22,   0xFF,  OPND_CTRL_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // mov     cr0, ecx
    {"mov",        0x23,   0xFF,  OPND_DBG_MODRM,      OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // mov     dr7, ecx
    {"movapd",     0x28,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movaps",     0x28,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"movapd",     0x29,   0xFF,  OPND_SIMD_RM,        OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movaps",     0x29,   0xFF,  OPND_SIMD_RM,        OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"cvtpi2pd",   0x2A,   0xFF,  OPND_SIMD_MODRM,     OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"cvtsi2ss",   0x2A,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"cvtsi2sd",   0x2A,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"cvtpi2ps",   0x2A,   0xFF,  OPND_SIMD_MODRM,     OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"movntpd",    0x2B,   0xFF,  OPND_SIMD_RM,        OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movntps",    0x2B,   0xFF,  OPND_SIMD_RM,        OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,                true,     Opcode::FLOW_NO_ALTER},
    {"cvttpd2pi",  0x2C,   0xFF,  OPND_MMX_MODRM,      OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"cvttss2si",  0x2C,   0xFF,  OPND_GP_32BIT_MODRM, OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"cvttsd2si",  0x2C,   0xFF,  OPND_GP_32BIT_MODRM, OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"cvttps2pi",  0x2C,   0xFF,  OPND_MMX_MODRM,      OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"cvtpd2pi",   0x2D,   0xFF,  OPND_MMX_MODRM,      OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"cvtss2si",   0x2D,   0xFF,  OPND_GP_32BIT_MODRM, OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"cvtsd2si",   0x2D,   0xFF,  OPND_GP_32BIT_MODRM, OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"cvtps2pi",   0x2D,   0xFF,  OPND_MMX_MODRM,      OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"ucomisd",    0x2E,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"ucomiss",    0x2E,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"comisd",     0x2F,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"comiss",     0x2F,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"wrmsr",      0x30,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // wrmsr
    // This one is a bit of a mystery
    {"rdtsc",      0x31,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // rdtsc
    {"rdmsr",      0x32,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // rdmsr
    {"rdpmc",      0x33,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // rdpmc
    {"sysexit",    0x35,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // sysexit
    {"getsec",     0x37,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER},
    {"movmskpd",   0x50,   0xFF,  OPND_GP_32BIT_MODRM, OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,             true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movmskps",   0x50,   0xFF,  OPND_GP_32BIT_MODRM, OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,             true,     Opcode::FLOW_NO_ALTER},
    {"sqrtss",     0x51,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"sqrtsd",     0x51,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"sqrtpd",     0x51,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"sqrtps",     0x51,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"rsqrtss",    0x52,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"rsqrtps",    0x52,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"rcpss",      0x53,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"rcpps",      0x53,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"andpd",      0x54,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"andps",      0x54,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"andnpd",     0x55,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"andnps",     0x55,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"orpd",       0x56,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"orps",       0x56,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"xorpd",      0x57,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"xorps",      0x57,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"addss",      0x58,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"addsd",      0x58,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"addpd",      0x58,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"addps",      0x58,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"mulss",      0x59,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"mulsd",      0x59,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"mulpd",      0x59,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"mulps",      0x59,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"cvtss2sd",   0x5A,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"cvtsd2ss",   0x5A,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"cvtpd2ps",   0x5A,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"cvtps2pd",   0x5A,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"cvttps2dq",  0x5B,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"cvtps2dq",   0x5B,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"cvtdq2ps",   0x5B,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"subss",      0x5C,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"subsd",      0x5C,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"subpd",      0x5C,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"subps",      0x5C,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"minss",      0x5D,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"minsd",      0x5D,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"minpd",      0x5D,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"minps",      0x5D,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"divss",      0x5E,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"divsd",      0x5E,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"divpd",      0x5E,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"divps",      0x5E,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"maxss",      0x5F,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_DWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"maxsd",      0x5F,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"maxpd",      0x5F,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"maxps",      0x5F,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},

    {"punpcklbw",  0x60,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"punpcklbw",  0x60,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpcklbw mm2, mm6
    {"punpcklwd",  0x61,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"punpcklwd",  0x61,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpcklwd mm0, mm0
    {"punpckldq",  0x62,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"punpckldq",  0x62,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpckldq mm0, mm0
    {"packsswb",   0x63,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"packsswb",   0x63,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"pcmpgtb",    0x64,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pcmpgtb",    0x64,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"pcmpgtw",    0x65,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pcmpgtw",    0x65,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"pcmpgtd",    0x66,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pcmpgtd",    0x66,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"packuswb",   0x67,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"packuswb",   0x67,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // packuswb mm4, mm5
    {"punpckhbw",  0x68,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"punpckhbw",  0x68,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpckhbw mm3, mm6
    {"punpckhwd",  0x69,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"punpckhwd",  0x69,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // punpckhwd mm1, mm1
    {"punpckhdq",  0x6A,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"punpckhdq",  0x6A,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"packssdw",   0x6B,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"packssdw",   0x6B,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"punpcklqdq", 0x6C,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"punpckhqdq", 0x6D,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {INVALID,      0x6D,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_INVALID},  // INVALID
    {"movd",       0x6E,   0xFF,  OPND_SIMD_MODRM,     OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movd",       0x6E,   0xFF,  OPND_MMX_MODRM,      OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // movd    mm0, dword ptr [esi]
    {"movdqa",     0x6F,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movdqu",     0x6F,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"movq",       0x6F,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // movq    mm7, qword_321FD8
    {"pshufd",     0x70,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pshufhw",    0x70,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"pshuflw",    0x70,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"pshufw",     0x70,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},

    {"psrlw",      0x71,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psrlw",      0x71,   0xFF,  OPND_MMX_RM,         OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER}, // psrlw   mm2, 8
    {"psraw",      0x71,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psraw",      0x71,   0xFF,  OPND_MMX_RM,         OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER},
    {"psllw",      0x71,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psllw",      0x71,   0xFF,  OPND_MMX_RM,         OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER},
    {"psrld",      0x72,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psrld",      0x72,   0xFF,  OPND_MMX_RM,         OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER}, // psrld   mm0, 18h
    {"psrad",      0x72,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psrad",      0x72,   0xFF,  OPND_MMX_RM,         OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_100, true,     Opcode::FLOW_NO_ALTER},
    {"pslld",      0x72,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pslld",      0x72,   0xFF,  OPND_MMX_RM,         OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER},
    {"psrlq",      0x73,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psrlq",      0x73,   0xFF,  OPND_MMX_RM,         OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_010, true,     Opcode::FLOW_NO_ALTER},
    {"psllq",      0x73,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psllq",      0x73,   0xFF,  OPND_MMX_RM,         OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_110, true,     Opcode::FLOW_NO_ALTER},
    {"psrldq",     0x73,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_011,true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pslldq",     0x73,   0xFF,  OPND_SIMD_RM,        OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_REGISTERS | MODRM_MOD_111,true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pcmpeqb",    0x74,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pcmpeqb",    0x74,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"pcmpeqw",    0x75,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pcmpeqw",    0x75,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"pcmpeqd",    0x76,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pcmpeqd",    0x76,   0xFF,  OPND_MMX_MODRM,      OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER},
    {"emms",       0x77,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,                   true,     Opcode::FLOW_NO_ALTER}, // emms
    {"movd",       0x7E,   0xFF,  OPND_MODRM_dWORDPTR, OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movq",       0x7E,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"movd",       0x7E,   0xFF,  OPND_MODRM_dWORDPTR, OPND_MMX_MODRM,       OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // movd    dword ptr [edi], mm0
    {"movdqa",     0x7F,   0xFF,  OPND_SIMD_RM,        OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movdqu",     0x7F,   0xFF,  OPND_SIMD_RM,        OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"movq",       0x7F,   0xFF,  OPND_MMX_RM,         OPND_MMX_MODRM,       OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER}, // movq    mm1, mm0

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
    {"push",       0xA8,   0xFF,  OPND_GS,                        OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // push    gs
    {"pop",        0xA9,   0xFF,  OPND_GS,                        OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // pop     gs
    {"bts",        0xAB,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // bts     [esp+24h+var_24], eax
    {"shrd",       0xAC,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // shrd    eax, ecx, 0Ch
    {"shrd",       0xAD,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_CL,             MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // shrd    eax, edx, cl
    {"fxsave",     0xAE,   0xFF,  OPND_MODRM_MEM,                 OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER}, // fxsave  dword ptr [ecx]
    {"fxrstor",    0xAE,   0xFF,  OPND_MODRM_MEM,                 OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER}, // fxrstor [eax]
    {"ldmxcsr",    0xAE,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER}, // ldmxcsr dword ptr [eax]
    {"stmxcsr",    0xAE,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER}, // stmxcsr dword ptr [eax]
    {"clflush",    0xAE,   0xFF,  OPND_MODRM_BYTEPTR,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_111, true,     Opcode::FLOW_NO_ALTER},
    {"lfence",     0xAE,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_EXACT(0xE8),                true,     Opcode::FLOW_NO_ALTER},
    {"mfence",     0xAE,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_EXACT(0xF0),                true,     Opcode::FLOW_NO_ALTER},
    {"sfence",     0xAE,   0xFF,  OPND_NO_OPERAND,     OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_EXACT(0xF8),                true,     Opcode::FLOW_NO_ALTER},
    {"imul",       0xAF,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // imul    esi, ecx
    // Slots 6,7 are empty

    {"cmpxchg",    0xB0,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"cmpxchg",    0xB1,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"lss",        0xB2,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // lss     sp, [esp-60h+arg_5C]
    // TODO! Complete the table here
    {"btr",        0xB3,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"lfs",        0xB4,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"lgs",        0xB5,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"movzx",      0xB6,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"movzx",      0xB7,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_WORDPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"bt",         0xBA,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER}, // bt      word ptr [esp+arg_68], 0
    {"btr",        0xBA,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER}, // btr     ecx, 1
    {"btc",        0xBA,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER}, // btc     ecx, 1
    {"bts",        0xBA,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_IMMEDIATE_8BIT,  OPND_NO_OPERAND,     MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER}, // bts ds:dword_473E58, 0
    {"btc",        0xBB,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // btc     ecx, eax
    {"bsf",        0xBC,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // bsf     ecx, eax
    {"bsr",        0xBD,   0xFF,  OPND_GP_16_32BIT,               OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // bsr     edx, ebx
    // TODO! Complete the table here
    {"xadd",       0xC0,   0xFF,  OPND_MODRM_BYTEPTR,             OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"xadd",       0xC1,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"cmpsd",      0xC2,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM_QWORD,   OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"cmpss",      0xC2,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM_DWORD,   OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"cmppd",      0xC2,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"cmpps",      0xC2,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"movnti",     0xC3,   0xFF,  OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // movnti  dword ptr [edi], eax
    {"pinsrw",     0xC4,   0xFF,  OPND_SIMD_MODRM,                OPND_MODRM_R32_WORDPTR,OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pinsrw",     0xC4,   0xFF,  OPND_MMX_MODRM,                 OPND_MODRM_R32_WORDPTR,OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pextrw",     0xC5,   0xFF,  OPND_GP_32BIT_MODRM,            OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_REGISTERS_ONLY,true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pextrw",     0xC5,   0xFF,  OPND_GP_32BIT_MODRM,            OPND_MMX_RM,          OPND_IMMEDIATE_8BIT, MODRM_REGISTERS_ONLY,true,     Opcode::FLOW_NO_ALTER},
    {"shufpd",     0xC6,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"shufps",     0xC6,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"cmpxchg8b",  0xC7,   0xFF,  OPND_MODRM_QWORDPTR,            OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_MOD_MEMORY | MODRM_MOD_001, true,     Opcode::FLOW_NO_ALTER}, // cmpxchg8b qword ptr [ebp+0]
    {"bswap",      0xC8,   0xF8,  OPND_ONEBYTES_OPCODE_GP_16_32,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER}, // bswap   eax

    {"psrlw",      0xD1,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psrlw",      0xD1,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psrld",      0xD2,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psrld",      0xD2,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psrlq",      0xD3,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psrlq",      0xD3,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"paddq",      0xD4,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"paddq",      0xD4,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pmullw",     0xD5,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pmullw",     0xD5,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // pmullw  mm2, mm0
    {"movq",       0xD6,   0xFF,  OPND_SIMD_RM_QWORD,             OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movq2dq",    0xD6,   0xFF,  OPND_SIMD_MODRM,                OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"movdq2q",    0xD6,   0xFF,  OPND_MMX_MODRM,                 OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"pmovmskb",   0xD7,   0xFF,  OPND_GP_32BIT_MODRM,            OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pmovmskb",   0xD7,   0xFF,  OPND_GP_32BIT_MODRM,            OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,true,     Opcode::FLOW_NO_ALTER},
    {"psubusb",    0xD8,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psubusb",    0xD8,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psubusw",    0xD9,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psubusw",    0xD9,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pminub",     0xDA,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pminub",     0xDA,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pand",       0xDB,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pand",       0xDB,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"paddusb",    0xDC,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"paddusb",    0xDC,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // paddusb mm4, qword ptr [esi-8]
    {"paddusw",    0xDD,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"paddusw",    0xDD,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // paddusw mm2, mm7
    {"pmaxub",     0xDE,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pmaxub",     0xDE,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pandn",      0xDF,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pandn",      0xDF,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},

    {"pavgb",      0xE0,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pavgb",      0xE0,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psraw",      0xE1,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psraw",      0xE1,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psrad",      0xE2,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psrad",      0xE2,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pavgw",      0xE3,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pavgw",      0xE3,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pmulhuw",    0xE4,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pmulhuw",    0xE4,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pmulhw",     0xE5,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pmulhw",     0xE5,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"cvttpd2dq",  0xE6,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"cvtdq2pd",   0xE6,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM_QWORD,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_F3},
    {"cvtpd2dq",   0xE6,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_F2},
    {"movntdq",    0xE7,   0xFF,  OPND_SIMD_RM,                   OPND_SIMD_MODRM,      OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"movntq",     0xE7,   0xFF,  OPND_MMX_RM,                    OPND_MMX_MODRM,       OPND_NO_OPERAND,     MODRM_MEMORY_ONLY,true,     Opcode::FLOW_NO_ALTER},
    {"psubsb",     0xE8,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psubsb",     0xE8,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psubsw",     0xE9,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psubsw",     0xE9,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pminsw",     0xEA,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pminsw",     0xEA,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"por",        0xEB,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"por",        0xEB,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"paddsb",     0xEC,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"paddsb",     0xEC,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"paddsw",     0xED,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"paddsw",     0xED,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pmaxsw",     0xEE,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pmaxsw",     0xEE,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pxor",       0xEF,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pxor",       0xEF,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER}, // pxor    mm6, mm6
    {"psllw",      0xF1,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psllw",      0xF1,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pslld",      0xF2,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pslld",      0xF2,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psllq",      0xF3,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psllq",      0xF3,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pmuludq",    0xF4,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pmuludq",    0xF4,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"pmaddwd",    0xF5,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"pmaddwd",    0xF5,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psadbw",     0xF6,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psadbw",     0xF6,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"maskmovdqu", 0xF7,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"maskmovq",   0xF7,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_REGISTERS_ONLY,true,     Opcode::FLOW_NO_ALTER},
    {"psubb",      0xF8,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psubb",      0xF8,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psubw",      0xF9,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psubw",      0xF9,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psubd",      0xFA,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psubd",      0xFA,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"psubq",      0xFB,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"psubq",      0xFB,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"paddb",      0xFC,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"paddb",      0xFC,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"paddw",      0xFD,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"paddw",      0xFD,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},
    {"paddd",      0xFE,   0xFF,  OPND_SIMD_MODRM,                OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"paddd",      0xFE,   0xFF,  OPND_MMX_MODRM,                 OPND_MMX_RM,          OPND_NO_OPERAND,     MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER},

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

// Three-bytes opcode table 0F 38
const OpcodeEntry gIa32ThreeBytes38OpcodeTable[] = {
    // Name        PREFIX  MASK   FIRST-OPERAND        SECONDS-OPERAND       THIRD-OPERAND        MODRM-FILTER                      UNSIGNED  ALTERING
    {"aesimc",     0xDB,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"aesenc",     0xDC,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"aesenclast", 0xDD,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"aesdec",     0xDE,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"aesdeclast", 0xDF,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_NO_OPERAND,     MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},

    // End marker
    {OPCODEEOT, 0,0, OPND_NO_OPERAND,OPND_NO_OPERAND,OPND_NO_OPERAND, 0, false, Opcode::FLOW_NO_ALTER}
};

// Three-bytes opcode table 0F 3A
const OpcodeEntry gIa32ThreeBytes3AOpcodeTable[] = {
    // Name        PREFIX  MASK   FIRST-OPERAND        SECONDS-OPERAND       THIRD-OPERAND        MODRM-FILTER                      UNSIGNED  ALTERING
    {"pclmulqdq",  0x44,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},
    {"aeskeygenassist",0xDF,   0xFF,  OPND_SIMD_MODRM,     OPND_SIMD_RM,         OPND_IMMEDIATE_8BIT, MODRM_ALL_FILTER,                 true,     Opcode::FLOW_NO_ALTER, MANDATORY_66},

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
    {"jc",          0x72,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER},
    {"jnae",        0x72,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER},
    {"jnb",         0x73,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},             // jo     $+5
    {"jnc",         0x73,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},
    {"jae",         0x73,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},
    {"jz",          0x74,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO},             // jo     $+5
    {"je",          0x74,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO},
    {"jnz",         0x75,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO | Opcode::FLOW_COND_NOT},             // jo     $+5
    {"jne",         0x75,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_ZERO | Opcode::FLOW_COND_NOT},
    {"jbe",         0x76,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},             // jo     $+5
    {"jna",         0x76,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},
    {"ja",          0x77,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER},             // jo     $+5
    {"jnbe",        0x77,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8,  OPND_NO_OPERAND,      OPND_NO_OPERAND,     MODRM_NO_MODRM,   false,    Opcode::FLOW_COND_BIGGER},

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
    {"xchg",        0x86,   0xFF, OPND_GP_8BIT_MODRM,             OPND_MODRM_BYTEPTR,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},           // xchg cl, al
    {"xchg",        0x86,   0xFF, OPND_MODRM_BYTEPTR,             OPND_GP_8BIT_MODRM,   OPND_NO_OPERAND,     MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},           // test al, [bx+si]    TODO! Wrong doc
    {"xchg",        0x87,   0xFF, OPND_GP_16_32BIT,               OPND_MODRM_dWORDPTR,  OPND_NO_OPERAND,     MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},           // xchg ecx, eax
    {"xchg",        0x87,   0xFF, OPND_MODRM_dWORDPTR,            OPND_GP_16_32BIT,     OPND_NO_OPERAND,     MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},           // test ax, [bx+si]    TODO! Wrong doc

    // NOTE: How the scanning algorithm exclude opcode 0x90 to be nop instead of XCHG EAX,EAX
    {"nop",         0x90,   0xFF, OPND_NO_OPERAND,                OPND_NO_OPERAND,  OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"xchg",        0x90,   0xF8, OPND_eAX,  OPND_ONEBYTES_OPCODE_GP_16_32,         OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER}, // xchg    eax, ecx
    {"xchg",        0x90,   0xF8, OPND_ONEBYTES_OPCODE_GP_16_32,  OPND_eAX,              OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER}, // xchg    ecx, eax

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
    {"sal",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"sal",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shr",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"shr",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {INVALID,       0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_INVALID},  // INVALID
    {INVALID,       0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_110, true, Opcode::FLOW_INVALID},  // INVALID
    {"sar",         0xC0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},
    {"sar",         0xC1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},

    {"ret",         0xC2,   0xFF, OPND_IMMEDIATE_16BIT,  OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM,   true, Opcode::FLOW_RET | Opcode::FLOW_ACTION},
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
    {"sal",         0xD0,   0xFF, OPND_MODRM_BYTEPTR,    OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"sal",         0xD1,   0xFF, OPND_MODRM_dWORDPTR,   OPND_ONE,              OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"sal",         0xD2,   0xFF, OPND_MODRM_BYTEPTR,    OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"shl",         0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"sal",         0xD3,   0xFF, OPND_MODRM_dWORDPTR,   OPND_CL,               OPND_NO_OPERAND, MODRM_MOD_MEM11 | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
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
    {"loopne",      0xE0,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"loopz",       0xE1,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
    {"loope",       0xE1,   0xFF, OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_NO_ALTER},
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
    {"jpe",         0x7A,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_PARITY},
    {"jnp",         0x7B,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_PARITY | Opcode::FLOW_COND_NOT},
    {"jpo",         0x7B,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_PARITY | Opcode::FLOW_COND_NOT},
    {"jl",          0x7C,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_LOWER},
    {"jnge",        0x7C,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_LOWER},
    {"jnl",         0x7D,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},
    {"jge",         0x7D,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_LOWER | Opcode::FLOW_COND_NOT},
    {"jng",         0x7E,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},
    {"jle",         0x7E,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_BIGGER | Opcode::FLOW_COND_NOT},
    {"jg",          0x7F,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_BIGGER},
    {"jnle",        0x7F,   0xFF,  OPND_IMMEDIATE_OFFSET_SHORT_8, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_NO_MODRM, false, Opcode::FLOW_COND_BIGGER},

    {"mov",         0x88,   0xFF,  OPND_MODRM_BYTEPTR,    OPND_GP_8BIT_MODRM,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"mov",         0x89,   0xFF,  OPND_MODRM_dWORDPTR,   OPND_GP_16_32BIT,      OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"mov",         0x8A,   0xFF,  OPND_GP_8BIT_MODRM,    OPND_MODRM_BYTEPTR,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"mov",         0x8B,   0xFF,  OPND_GP_16_32BIT,      OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"mov",         0x8C,   0xFF,  OPND_MODRM_WORDPTR,    OPND_GP_SEGMENT_MODRM, OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},

    {"lea",         0x8D,   0xFF,  OPND_GP_16_32BIT,      OPND_MODRM_MEM,        OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER}, // lea     eax, ds:40h[edi*8]

    {"mov",         0x8E,   0xFF,  OPND_GP_SEGMENT_MODRM, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},
    {"pop",         0x8F,   0xFF,  OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_ALL_FILTER, true, Opcode::FLOW_NO_ALTER},

    {"cbw/cwde",    0x98,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},   // cbw for prefix
    {"cwd/cdq",     0x99,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},   // cwd for prefix
    {"call",        0x9A,   0xFF,  OPND_IMMEDIATE_OFFSET_FAR,  OPND_NO_OPERAND,  OPND_NO_OPERAND, MODRM_NO_MODRM, false,Opcode::FLOW_NO_ALTER},
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
    {"int3",        0xCC,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"int",         0xCD,   0xFF,  OPND_IMMEDIATE_8BIT,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
    {"into",        0xCE,   0xFF,  OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_NO_MODRM, true, Opcode::FLOW_NO_ALTER},
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
    {"fadd",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000, true,   Opcode::FLOW_NO_ALTER}, // fadd    dword ptr [esi+0]
    {"fmul",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001, true,   Opcode::FLOW_NO_ALTER}, // fmul    dword ptr [eax+4]
    {"fcom",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010, true,   Opcode::FLOW_NO_ALTER}, // fcom    st(3)
    {"fcomp",       0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011, true,   Opcode::FLOW_NO_ALTER}, // fcomp   dword ptr [edi+90044h]
    {"fsub",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_100, true,   Opcode::FLOW_NO_ALTER}, // fsub    st, st(5)
    {"fsubr",       0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_101, true,   Opcode::FLOW_NO_ALTER},
    {"fdiv",        0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_110, true,   Opcode::FLOW_NO_ALTER}, // fdiv    st, st(1)
    {"fdivr",       0xD8,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_111, true,   Opcode::FLOW_NO_ALTER}, // fdivr   dword ptr [edi+59h]
    {"fadd",        0xD8,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"fmul",        0xD8,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"fcom",        0xD8,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"fcomp",       0xD8,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"fsub",        0xD8,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"fsubr",       0xD8,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"fdiv",        0xD8,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"fdivr",       0xD8,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},

    {"fld",         0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000, true,   Opcode::FLOW_NO_ALTER}, // /0
    {"fld",         0xD9,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"fxch",        0xD9,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER}, // /1
    {"fst",         0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010, true,   Opcode::FLOW_NO_ALTER}, // /2
    {"fstp",        0xD9,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011, true,   Opcode::FLOW_NO_ALTER}, // /3
    {"fldenv",      0xD9,   0xFF, OPND_MODRM_MEM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_100, true,   Opcode::FLOW_NO_ALTER}, // /4
    {"fldcw",       0xD9,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_101, true,   Opcode::FLOW_NO_ALTER}, // /5
    {"fnstenv",     0xD9,   0xFF, OPND_MODRM_MEM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_110, true,   Opcode::FLOW_NO_ALTER}, // /6
    {"fnstcw",      0xD9,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_111, true,   Opcode::FLOW_NO_ALTER}, // /7
    {"fnop",        0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xD0),               true,     Opcode::FLOW_NO_ALTER},
    {"fchs",        0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE0),               true,     Opcode::FLOW_NO_ALTER},
    {"fabs",        0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE1),               true,     Opcode::FLOW_NO_ALTER},
    {"ftst",        0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE4),               true,     Opcode::FLOW_NO_ALTER},
    {"fxam",        0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE5),               true,     Opcode::FLOW_NO_ALTER},
    {"fld1",        0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE8),               true,     Opcode::FLOW_NO_ALTER},
    {"fldl2t",      0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE9),               true,     Opcode::FLOW_NO_ALTER},
    {"fldl2e",      0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xEA),               true,     Opcode::FLOW_NO_ALTER},
    {"fldpi",       0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xEB),               true,     Opcode::FLOW_NO_ALTER},
    {"fldlg2",      0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xEC),               true,     Opcode::FLOW_NO_ALTER},
    {"fldln2",      0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xED),               true,     Opcode::FLOW_NO_ALTER},
    {"fldz",        0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xEE),               true,     Opcode::FLOW_NO_ALTER},
    {"f2xm1",       0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF0),               true,     Opcode::FLOW_NO_ALTER},
    {"fyl2x",       0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF1),               true,     Opcode::FLOW_NO_ALTER},
    {"fptan",       0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF2),               true,     Opcode::FLOW_NO_ALTER},
    {"fpatan",      0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF3),               true,     Opcode::FLOW_NO_ALTER},
    {"fxtract",     0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF4),               true,     Opcode::FLOW_NO_ALTER},
    {"fprem1",      0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF5),               true,     Opcode::FLOW_NO_ALTER},
    {"fdecstp",     0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF6),               true,     Opcode::FLOW_NO_ALTER},
    {"fincstp",     0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF7),               true,     Opcode::FLOW_NO_ALTER},
    {"fprem",       0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF8),               true,     Opcode::FLOW_NO_ALTER},
    {"fyl2xp1",     0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xF9),               true,     Opcode::FLOW_NO_ALTER},
    {"fsqrt",       0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xFA),               true,     Opcode::FLOW_NO_ALTER},
    {"fsincos",     0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xFB),               true,     Opcode::FLOW_NO_ALTER},
    {"frndint",     0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xFC),               true,     Opcode::FLOW_NO_ALTER},
    {"fscale",      0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xFD),               true,     Opcode::FLOW_NO_ALTER},
    {"fsin",        0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xFE),               true,     Opcode::FLOW_NO_ALTER},
    {"fcos",        0xD9,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xFF),               true,     Opcode::FLOW_NO_ALTER},

    {"fiadd",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000, true,   Opcode::FLOW_NO_ALTER}, // /0
    {"fimul",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001, true,   Opcode::FLOW_NO_ALTER}, // /1
    {"ficom",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010, true,   Opcode::FLOW_NO_ALTER}, // /2
    {"ficomp",      0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011, true,   Opcode::FLOW_NO_ALTER}, // /3
    {"fisub",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_100, true,   Opcode::FLOW_NO_ALTER}, // /4
    {"fisubr",      0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_101, true,   Opcode::FLOW_NO_ALTER}, // /5
    {"fidiv",       0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_110, true,   Opcode::FLOW_NO_ALTER}, // /6
    {"fidivr",      0xDA,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_111, true,   Opcode::FLOW_NO_ALTER}, // /7
    {"fcmovb",      0xDA,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"fcmove",      0xDA,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"fcmovbe",     0xDA,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"fcmovu",      0xDA,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"fucompp",     0xDA,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE9),               true,     Opcode::FLOW_NO_ALTER},

    {"fild",        0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000, true,   Opcode::FLOW_NO_ALTER}, // fild    dword ptr [ecx]
    {"fisttp",      0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001, true,   Opcode::FLOW_NO_ALTER}, // fisttp  dword ptr [eax+eax-54545455h]
    {"fist",        0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010, true,   Opcode::FLOW_NO_ALTER},
    {"fistp",       0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011, true,   Opcode::FLOW_NO_ALTER}, // fistp   [ebp+arg_0]
    {"fld",         0xDB,   0xFF, OPND_MODRM_TWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_101, true,   Opcode::FLOW_NO_ALTER}, // fld     tbyte_473F50
    {INVALID,       0xDB,   0xFF, OPND_MODRM_dWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_110, true,   Opcode::FLOW_INVALID}, // INVALID
    {"fstp",        0xDB,   0xFF, OPND_MODRM_TWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_111, true,   Opcode::FLOW_NO_ALTER}, // fstp    [ebp+var_1E]
    {"fcmovnb",     0xDB,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"fcmovne",     0xDB,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"fcmovnbe",    0xDB,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"fcmovnu",     0xDB,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"fucomi",      0xDB,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"fcomi",       0xDB,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"fneni" ,      0xDB,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE0),               true,     Opcode::FLOW_NO_ALTER},
    {"fndisi",      0xDB,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE1),               true,     Opcode::FLOW_NO_ALTER},
    {"fsetpm",      0xDB,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE4),               true,     Opcode::FLOW_NO_ALTER},
    {"fnclex",      0xDB,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE2),               true,     Opcode::FLOW_NO_ALTER},
    {"fninit",      0xDB,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE3),               true,     Opcode::FLOW_NO_ALTER},

    {"fadd",        0xDC,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000, true,   Opcode::FLOW_NO_ALTER}, // fadd    ds:dbl_405C10
    {"fmul",        0xDC,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001, true,   Opcode::FLOW_NO_ALTER}, // fmul    ds:dbl_4061A8
    {"fcom",        0xDC,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010, true,   Opcode::FLOW_NO_ALTER}, // fcom    [ebp+arg_0]
    {"fcomp",       0xDC,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011, true,   Opcode::FLOW_NO_ALTER}, // fcomp   ds:dbl_4061A8
    {"fsub",        0xDC,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_100, true,   Opcode::FLOW_NO_ALTER},
    {"fsubr",       0xDC,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_101, true,   Opcode::FLOW_NO_ALTER}, // fsubr   qword ptr [ebx+46h]
    {"fdiv",        0xDC,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_110, true,   Opcode::FLOW_NO_ALTER}, // fdiv    qword ptr [ebp-1Ch]
    {"fdivr",       0xDC,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_111, true,   Opcode::FLOW_NO_ALTER},
    {"fadd",        0xDC,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"fmul",        0xDC,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER},
    {"fsubr",       0xDC,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"fsub",        0xDC,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"fdivr",       0xDC,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"fdiv",        0xDC,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER},

    {"fld",         0xDD,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000, true,   Opcode::FLOW_NO_ALTER}, // fld     qword ptr [ecx]
    {"fisttp",      0xDD,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001, true,   Opcode::FLOW_NO_ALTER}, // /1
    {"fst",         0xDD,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010, true,   Opcode::FLOW_NO_ALTER}, // /2
    {"fstp",        0xDD,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011, true,   Opcode::FLOW_NO_ALTER}, // /3
    {"frstor",      0xDD,   0xFF, OPND_MODRM_MEM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_100, true,   Opcode::FLOW_NO_ALTER}, // /4
    {"fnsave",      0xDD,   0xFF, OPND_MODRM_MEM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_110, true,   Opcode::FLOW_NO_ALTER}, // /6
    {"fnstsw",      0xDD,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_111, true,   Opcode::FLOW_NO_ALTER}, // /7
    {"ffree",       0xDD,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"fst",         0xDD,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_010, true, Opcode::FLOW_NO_ALTER},
    {"fstp",        0xDD,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_011, true, Opcode::FLOW_NO_ALTER},
    {"fucom",       0xDD,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER},
    {"fucomp",      0xDD,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},

    {"fiadd",       0xDE,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000, true,   Opcode::FLOW_NO_ALTER}, // /0
    {"fimul",       0xDE,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001, true,   Opcode::FLOW_NO_ALTER}, // /1
    {"ficom",       0xDE,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010, true,   Opcode::FLOW_NO_ALTER}, // /2
    {"ficomp",      0xDE,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011, true,   Opcode::FLOW_NO_ALTER}, // /3
    {"fisub",       0xDE,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_100, true,   Opcode::FLOW_NO_ALTER}, // /4
    {"fisubr",      0xDE,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_101, true,   Opcode::FLOW_NO_ALTER}, // /5
    {"fidiv",       0xDE,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_110, true,   Opcode::FLOW_NO_ALTER}, // /6
    {"fidivr",      0xDE,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_111, true,   Opcode::FLOW_NO_ALTER}, // /7
    {"faddp",       0xDE,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER}, // faddp   st, st
    {"fmulp",       0xDE,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_001, true, Opcode::FLOW_NO_ALTER}, // fmulp   st(1), st
    {"fcompp",      0xDE,   0xFF, OPND_NO_OPERAND,       OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xD9),               true,     Opcode::FLOW_NO_ALTER}, // fcompp
    {"fsubrp",      0xDE,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_100, true, Opcode::FLOW_NO_ALTER}, // fsubrp  st(3), st
    {"fsubp",       0xDE,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER}, // fsubp   st(1), st
    {"fdivrp",      0xDE,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER}, // fdivrp  st(1), st
    {"fdivp",       0xDE,   0xFF, OPND_FPU_ST_RM,        OPND_FPU_ST0,          OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_111, true, Opcode::FLOW_NO_ALTER}, // fdivp   st(1), st

    {"fild",        0xDF,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000, true,   Opcode::FLOW_NO_ALTER}, // /0
    {"fisttp",      0xDF,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001, true,   Opcode::FLOW_NO_ALTER}, // /1
    {"fist",        0xDF,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010, true,   Opcode::FLOW_NO_ALTER}, // /2
    {"fistp",       0xDF,   0xFF, OPND_MODRM_WORDPTR,    OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011, true,   Opcode::FLOW_NO_ALTER}, // /3
    {"fbld",        0xDF,   0xFF, OPND_MODRM_TWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_100, true,   Opcode::FLOW_NO_ALTER}, // /4
    {"fild",        0xDF,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_101, true,   Opcode::FLOW_NO_ALTER}, // /5
    {"fbstp",       0xDF,   0xFF, OPND_MODRM_TWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_110, true,   Opcode::FLOW_NO_ALTER}, // /6
    {"fistp",       0xDF,   0xFF, OPND_MODRM_QWORDPTR,   OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_111, true,   Opcode::FLOW_NO_ALTER}, // /7
    {"ffreep",      0xDF,   0xFF, OPND_FPU_ST_RM,        OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_000, true, Opcode::FLOW_NO_ALTER},
    {"fucomip",     0xDF,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_101, true, Opcode::FLOW_NO_ALTER},
    {"fcomip",      0xDF,   0xFF, OPND_FPU_ST0,          OPND_FPU_ST_RM,        OPND_NO_OPERAND, MODRM_MOD_REGISTERS | MODRM_MOD_110, true, Opcode::FLOW_NO_ALTER},
    {"fnstsw",      0xDF,   0xFF, OPND_AX,               OPND_NO_OPERAND,       OPND_NO_OPERAND, MODRM_EXACT(0xE0),               true,     Opcode::FLOW_NO_ALTER},

    // End marker
    {OPCODEEOT, 0,0, OPND_NO_OPERAND,OPND_NO_OPERAND,OPND_NO_OPERAND, 0, false, Opcode::FLOW_NO_ALTER}
};

const char* INVALID = "***";
const char* OPCODEEOT = "---";


}; // end of namespace ia32dis
