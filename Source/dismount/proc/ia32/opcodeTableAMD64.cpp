#include "dismount/dismount.h"
/*
 * opcodeTableAMD64.cpp
 *
 * The register names and the opcode entries of 64 bit mode.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/Opcode.h"

namespace ia32dis {

RegisterDescription gAmd64Registers[AMD64_NUMBER_OF_REGISTERS] = {
    {"rax"},  {"rcx"},  {"rdx"},  {"rbx"},
    {"rsp"},  {"rbp"},  {"rsi"},  {"rdi"},
    {"r8"},   {"r9"},   {"r10"},  {"r11"},
    {"r12"},  {"r13"},  {"r14"},  {"r15"}
};

RegisterDescription gAmd64Registers32[AMD64_NUMBER_OF_REGISTERS] = {
    {"eax"},  {"ecx"},  {"edx"},  {"ebx"},
    {"esp"},  {"ebp"},  {"esi"},  {"edi"},
    {"r8d"},  {"r9d"},  {"r10d"}, {"r11d"},
    {"r12d"}, {"r13d"}, {"r14d"}, {"r15d"}
};

RegisterDescription gAmd64Registers16[AMD64_NUMBER_OF_REGISTERS] = {
    {"ax"},   {"cx"},   {"dx"},   {"bx"},
    {"sp"},   {"bp"},   {"si"},   {"di"},
    {"r8w"},  {"r9w"},  {"r10w"}, {"r11w"},
    {"r12w"}, {"r13w"}, {"r14w"}, {"r15w"}
};

RegisterDescription gAmd64Registers8[AMD64_NUMBER_OF_REGISTERS] = {
    {"al"},   {"cl"},   {"dl"},   {"bl"},
    {"spl"},  {"bpl"},  {"sil"},  {"dil"},
    {"r8b"},  {"r9b"},  {"r10b"}, {"r11b"},
    {"r12b"}, {"r13b"}, {"r14b"}, {"r15b"}
};

RegisterDescription gAmd64SIMDRegisters[AMD64_NUMBER_OF_REGISTERS] = {
    {"xmm0"},  {"xmm1"},  {"xmm2"},  {"xmm3"},
    {"xmm4"},  {"xmm5"},  {"xmm6"},  {"xmm7"},
    {"xmm8"},  {"xmm9"},  {"xmm10"}, {"xmm11"},
    {"xmm12"}, {"xmm13"}, {"xmm14"}, {"xmm15"}
};

RegisterDescription gAmd64ControlRegisters[AMD64_NUMBER_OF_CONTROL_REGISTERS] = {
    {"cr0"}, {"cr1"}, {"cr2"}, {"cr3"}, {"cr4"}, {"cr5"}, {"cr6"}, {"cr7"},
    {"cr8"}
};

// One byte opcodes of 64 bit mode, searched before gIa32OneByteOpcodeTable
const OpcodeEntry gAmd64OneByteOpcodeTable[] = {
    // Name         PREFIX  MASK   FIRST-OPERAND            SECONDS-OPERAND               THIRD-OPERAND    MODRM-FILTER      UNSIGNED  ALTERING                 MANDATORY  MODE64
    // 63 is arpl in the IA-32 table
    {"movsxd",      0x63,   0xFF,  OPND_GP_16_32BIT,        OPND_MODRM_FIXED_DWORDPTR,    OPND_NO_OPERAND, MODRM_ALL_FILTER, true,     Opcode::FLOW_NO_ALTER,   0,         MODE64_ONLY},

    // cdqe and cqo with REX.W
    {"cbw/cwde/cdqe", 0x98, 0xFF,  OPND_NO_OPERAND,         OPND_NO_OPERAND,              OPND_NO_OPERAND, MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER,   0,         MODE64_ONLY},
    {"cwd/cdq/cqo", 0x99,   0xFF,  OPND_NO_OPERAND,         OPND_NO_OPERAND,              OPND_NO_OPERAND, MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER,   0,         MODE64_ONLY},

    // mov r64, imm64 with REX.W
    {"mov",         0xB8,   0xF8,  OPND_ONEBYTES_OPCODE_GP_16_32, OPND_IMMEDIATE_DQ,      OPND_NO_OPERAND, MODRM_NO_MODRM,   true,     Opcode::FLOW_NO_ALTER,   0,         MODE64_ONLY},

    // End marker
    {OPCODEEOT, 0,0, OPND_NO_OPERAND,OPND_NO_OPERAND,OPND_NO_OPERAND, 0, false, Opcode::FLOW_NO_ALTER}
};

// Two bytes opcodes (0F xx) of 64 bit mode, searched before
// gIa32TwoBytesOpcodeTable. The entries with a mandatory prefix come first
const OpcodeEntry gAmd64TwoBytesOpcodeTable[] = {
    // Name         PREFIX  MASK   FIRST-OPERAND            SECONDS-OPERAND               THIRD-OPERAND    MODRM-FILTER                        UNSIGNED  ALTERING                 MANDATORY       MODE64
    // movd / movq between a general purpose register and an SSE or MMX register
    {"movd/movd/movq", 0x6E, 0xFF, OPND_SIMD_MODRM,         OPND_MODRM_dWORDPTR,          OPND_NO_OPERAND, MODRM_ALL_FILTER,                   true,     Opcode::FLOW_NO_ALTER,   MANDATORY_66,   MODE64_ONLY},
    {"movd/movd/movq", 0x6E, 0xFF, OPND_MMX_MODRM,          OPND_MODRM_dWORDPTR,          OPND_NO_OPERAND, MODRM_ALL_FILTER,                   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},
    {"movd/movd/movq", 0x7E, 0xFF, OPND_MODRM_dWORDPTR,     OPND_SIMD_MODRM,              OPND_NO_OPERAND, MODRM_ALL_FILTER,                   true,     Opcode::FLOW_NO_ALTER,   MANDATORY_66,   MODE64_ONLY},
    // The IA-32 entry of 'movq xmm, xmm / m64' must not lose to the next entry
    {"movq",        0x7E,   0xFF,  OPND_SIMD_MODRM,         OPND_SIMD_RM_QWORD,           OPND_NO_OPERAND, MODRM_ALL_FILTER,                   true,     Opcode::FLOW_NO_ALTER,   MANDATORY_F3,   MODE64_LEGACY},
    {"movd/movd/movq", 0x7E, 0xFF, OPND_MODRM_dWORDPTR,     OPND_MMX_MODRM,               OPND_NO_OPERAND, MODRM_ALL_FILTER,                   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},

    // The moves of the control and the debug registers always use the 64 bit
    // register, the mod field is ignored by the processor (the decoder takes it as 11)
    {"mov",         0x20,   0xFF,  OPND_GP_NATIVE_RM,       OPND_CTRL_MODRM,              OPND_NO_OPERAND, MODRM_REGISTERS_ONLY,               true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},
    {"mov",         0x21,   0xFF,  OPND_GP_NATIVE_RM,       OPND_DBG_MODRM,               OPND_NO_OPERAND, MODRM_REGISTERS_ONLY,               true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},
    {"mov",         0x22,   0xFF,  OPND_CTRL_MODRM,         OPND_GP_NATIVE_RM,            OPND_NO_OPERAND, MODRM_REGISTERS_ONLY,               true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},
    {"mov",         0x23,   0xFF,  OPND_DBG_MODRM,          OPND_GP_NATIVE_RM,            OPND_NO_OPERAND, MODRM_REGISTERS_ONLY,               true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},

    // System call
    {"syscall",     0x05,   0xFF,  OPND_NO_OPERAND,         OPND_NO_OPERAND,              OPND_NO_OPERAND, MODRM_NO_MODRM,                     true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_NO_REXW},
    {"sysret",      0x07,   0xFF,  OPND_NO_OPERAND,         OPND_NO_OPERAND,              OPND_NO_OPERAND, MODRM_NO_MODRM,                     true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_NO_REXW},
    {"swapgs",      0x01,   0xFF,  OPND_NO_OPERAND,         OPND_NO_OPERAND,              OPND_NO_OPERAND, MODRM_EXACT(0xF8),                  true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},

    // The descriptor tables are addressed with m16&64
    {"sgdt",        0x01,   0xFF,  OPND_MODRM_MEM,          OPND_NO_OPERAND,              OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000,   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_NO_REXW},
    {"sidt",        0x01,   0xFF,  OPND_MODRM_MEM,          OPND_NO_OPERAND,              OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001,   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_NO_REXW},
    {"lgdt",        0x01,   0xFF,  OPND_MODRM_MEM,          OPND_NO_OPERAND,              OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010,   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_NO_REXW},
    {"lidt",        0x01,   0xFF,  OPND_MODRM_MEM,          OPND_NO_OPERAND,              OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011,   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_NO_REXW},

    // The 64 bit operand size forms of the memory instructions
    {"fxsave/fxsave/fxsave64",   0xAE, 0xFF, OPND_MODRM_MEM, OPND_NO_OPERAND,             OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_000,   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},
    {"fxrstor/fxrstor/fxrstor64", 0xAE, 0xFF, OPND_MODRM_MEM, OPND_NO_OPERAND,            OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001,   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},
    {"ldmxcsr",     0xAE,   0xFF,  OPND_MODRM_FIXED_DWORDPTR, OPND_NO_OPERAND,            OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_010,   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_NO_REXW},
    {"stmxcsr",     0xAE,   0xFF,  OPND_MODRM_FIXED_DWORDPTR, OPND_NO_OPERAND,            OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_011,   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_NO_REXW},
    {"cmpxchg8b/cmpxchg8b/cmpxchg16b", 0xC7, 0xFF, OPND_MODRM_DOUBLE_MEM, OPND_NO_OPERAND, OPND_NO_OPERAND, MODRM_MOD_MEMORY | MODRM_MOD_001,   true,     Opcode::FLOW_NO_ALTER,   0,              MODE64_ONLY},

    // End marker
    {OPCODEEOT, 0,0, OPND_NO_OPERAND,OPND_NO_OPERAND,OPND_NO_OPERAND, 0, false, Opcode::FLOW_NO_ALTER}
};

}; // end of namespace ia32dis
