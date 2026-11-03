#ifndef __TBA_DISMOUNT_PROC_IA32_OPCODETABLE_H
#define __TBA_DISMOUNT_PROC_IA32_OPCODETABLE_H

/*
 * opcodeTable.h
 *
 * Defines the format for Intel opcodes
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"

namespace ia32dis {


enum {
    // The number of registers for each table
    NUMBER_OF_REGISTERS = 8,
    // The number of segment registers.
    NUMBER_OF_SEGMENTS_REGISTERS = 6,
    // The number of control registers.
    NUMBER_OF_CONTROL_REGISTERS = 5,
    // A register marker for no register...
    NO_REGISTER = NUMBER_OF_REGISTERS,

    // The number of general purpose, SSE, control and debug registers which
    // the REX prefix can select in 64 bit mode (r8-r15, xmm8-xmm15, cr8)
    AMD64_NUMBER_OF_REGISTERS = 16,
    AMD64_NUMBER_OF_CONTROL_REGISTERS = 9
};

/*
 * Define a register name and attribute.
 * So far this struct only contains the name of the register
 */
struct RegisterDescription {
    // The name of the register
    const char* m_name;
};

extern RegisterDescription gIa8Registers[NUMBER_OF_REGISTERS];
extern RegisterDescription gIa16Registers[NUMBER_OF_REGISTERS];
extern RegisterDescription gIa32Registers[NUMBER_OF_REGISTERS];
extern RegisterDescription gIa32MMXRegisters[NUMBER_OF_REGISTERS];
extern RegisterDescription gIa32SIMDRegisters[NUMBER_OF_REGISTERS];
extern RegisterDescription gIa32SegmentsRegisters[NUMBER_OF_SEGMENTS_REGISTERS];
extern RegisterDescription gIa32ControlRegisters[NUMBER_OF_CONTROL_REGISTERS];
extern RegisterDescription gIa32DebugRegisters[NUMBER_OF_REGISTERS];

/*
 * The register names of 64 bit mode, indexed by the register number with the
 * REX extension bit (REX.R, REX.X or REX.B) as bit 3. See
 * opcodeTableAMD64.cpp.
 */
// rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8 .. r15
extern RegisterDescription gAmd64Registers[AMD64_NUMBER_OF_REGISTERS];
// eax .. edi, r8d .. r15d
extern RegisterDescription gAmd64Registers32[AMD64_NUMBER_OF_REGISTERS];
// ax .. di, r8w .. r15w
extern RegisterDescription gAmd64Registers16[AMD64_NUMBER_OF_REGISTERS];
// The 8 bit registers of an instruction with a REX prefix: al, cl, dl, bl,
// spl, bpl, sil, dil, r8b .. r15b. Without a REX prefix the numbers 4..7 are
// ah, ch, dh, bh (gIa8Registers)
extern RegisterDescription gAmd64Registers8[AMD64_NUMBER_OF_REGISTERS];
// xmm0 .. xmm15
extern RegisterDescription gAmd64SIMDRegisters[AMD64_NUMBER_OF_REGISTERS];
// cr0 .. cr8 (cr8 is encoded with REX.R and the number 0)
extern RegisterDescription gAmd64ControlRegisters[AMD64_NUMBER_OF_CONTROL_REGISTERS];

/*
 * The different GP registers values
 */
enum {
    // 64 bit registers table (the low 3 bits are the ModRM / SIB / opcode
    // field, bit 3 is the REX extension bit)
    AMD64_GP64_RAX =  0,
    AMD64_GP64_RSP =  4,
    AMD64_GP64_RBP =  5,
    AMD64_GP64_R8  =  8,
    AMD64_GP64_R12 = 12,
    AMD64_GP64_R13 = 13,

    // 32 bit registers table
    IA32_GP32_EAX =  0,
    IA32_GP32_ECX =  1,
    IA32_GP32_EDX =  2,
    IA32_GP32_EBX =  3,
    IA32_GP32_ESP =  4,
    IA32_GP32_EBP =  5,
    IA32_GP32_ESI =  6,
    IA32_GP32_EDI =  7,

    // 16 bit registers table
    IA32_GP16_AX =  0,
    IA32_GP16_CX =  1,
    IA32_GP16_DX =  2,
    IA32_GP16_BX =  3,
    IA32_GP16_SP =  4,
    IA32_GP16_BP =  5,
    IA32_GP16_SI =  6,
    IA32_GP16_DI =  7,

    IA32_GP8_AL =  0,
    IA32_GP8_CL =  1,
    IA32_GP8_DL =  2,
    IA32_GP8_BL =  3,
    IA32_GP8_AH =  4,
    IA32_GP8_CH =  5,
    IA32_GP8_DH =  6,
    IA32_GP8_BH =  7,

    // Segments registers
    IA32_SEG_ES = 0,
    IA32_SEG_CS = 1,
    IA32_SEG_SS = 2,
    IA32_SEG_DS = 3,
    IA32_SEG_FS = 4,
    IA32_SEG_GS = 5
};

/*
 * The table entry description which parse the modRM status.
 * See gIa32ModRM16, gIa32ModRM32
 */
struct ModRMTranslation {
    /*
     * True if the object is a memory reference
     */
    bool m_isReference;

    /*
     * Can be a number from range 0..7 (Which points to one of the registers
     * inside the table) or NO_REGISTER to indicate that the register is not
     * used
     */
    uint m_firstRegisterPointer;
    uint m_secondRegisterPointer;

    /*
     * The number of bytes belongs to the displacement.
     */
    uint m_displacementLength;

    /*
     * Set to true if the displacement is relative or not.
     */
    bool m_displacementRelative;

    /*
     * Set to true to indicates that SIB contains the registers and it's scale,
     * modrm contains the count of the displacement.
     * NOTE: Exist only at 32bit modrm table...
     */
    bool m_forceSib;
};

enum {
    MODRM_MODE_COUNT = 4,
    MODRM_RM_COUNT = 8
};


// Array pointers is a complex busniess.
typedef const ia32dis::ModRMTranslation ModRMTranslationType[MODRM_MODE_COUNT][MODRM_RM_COUNT];


// 16 bit modrm table, point to registers inside the gIa16Registers...
extern ModRMTranslationType gIa32ModRM16;
// 32 bit modrm table, point to registers inside the gIa32Registers...
extern ModRMTranslationType gIa32ModRM32;

/*
 * The value for the displacements and immediates length
 */
enum {
    // The 2 bytes displacement length.
    DISPLACEMENT_16BIT_LENGTH = 2,
    // The 4 bytes displacement length.
    DISPLACEMENT_32BIT_LENGTH = 4,
    // Default choise between 2 bytes displacement and 4 bytes displacement
    DEFUALT_DISPLACEMENT_LENGTH = 0xFF
};

/*
 *
 */
typedef enum {
    // There is not operand in this opcode
    OPND_NO_OPERAND,

    // The operand is 8bit immediate
    OPND_IMMEDIATE_8BIT,
    // The operand is 16bit immediate
    OPND_IMMEDIATE_16BIT,
    // The operand is 16bit or 32bit immediate according to the state and the
    // current 66 size prefix. With a 64 bit operand size (REX.W) the
    // immediate is 32 bits, sign extended to 64 bits.
    OPND_IMMEDIATE_DS,
    // The operand is an immediate of the operand size: 16, 32 or 64 bits.
    // Only 'mov r64, imm64' (REX.W B8+r) has a 64 bit immediate. In 16 and
    // 32 bit mode it is the same as OPND_IMMEDIATE_DS
    OPND_IMMEDIATE_DQ,

    // The operand is memory reference offest encoded as direct immediate.
    // The length of the immediate is the default address-size (16bit/32bit
    // according to the 67 prefix... In 64 bit mode the offset is 64 bits
    // (32 bits with the 67 prefix): mov al, [moffs64]
    OPND_MEMREF_OFFSET_DS,

    // The operand is offset (used in loops, jmp etc) encoded as immediate
    OPND_IMMEDIATE_OFFSET_SHORT_8,
    // The operand is long offset (used in loops, jmp etc) encoded as immediate
    OPND_IMMEDIATE_OFFSET_LONG_32,
    // The operans is offset (used in jmp near, call near). In 64 bit mode
    // the offset is always 32 bits, sign extended to 64 bits (rip + rel32)
    OPND_IMMEDIATE_OFFSET_DS,
    // The operand is a far offset (offset16/32:segment)
    OPND_IMMEDIATE_OFFSET_FAR,

    // The operand is GP encoded within the 3 bits of the first-bytes opcode
    // (of the operand size: 16, 32 or 64 bits; REX.B selects r8-r15)
    OPND_ONEBYTES_OPCODE_GP_16_32,
    // The operand is 8bit GP encoded within the 3 bits of the first-bytes opcode
    OPND_ONEBYTES_OPCODE_GP_8,

    // The operand is AL register
    OPND_AL,
    // The operand is AX, EAX or RAX registers (the operand size)
    OPND_eAX,
    // The operand is BX or EBX registers
    OPND_eBX,
    // The operand is BP or EBP registers
    OPND_eBP,
    // The operand is SI or ESI registers
    OPND_eSI,
    // The operand is DI or EDI registers
    OPND_eDI,
    // The operand is CL register
    OPND_CL,
    // The operand is DX register
    OPND_DX,
    // The operand is the digit 1
    OPND_ONE,
    // The operand is the digit 3
    OPND_THREE,
    // The operand is CS register
    OPND_CS,
    // The operand is DS register
    OPND_DS,
    // The operand is ES register
    OPND_ES,
    // The operand is SS register
    OPND_SS,
    // The operand is FS register
    OPND_FS,
    // The operand is GS register
    OPND_GS,

    // The operand is 8 bit register stored at modrm reg/opcode
    OPND_GP_8BIT_MODRM,
    // The operand is 16 bit register stored at modrm reg/opcode
    OPND_GP_16BIT_MODRM,
    // The operand is a segment register stored at the modrm reg/opcode
    OPND_GP_SEGMENT_MODRM,
    // The operand is 16 or 32 bit register depending by the default behaviour,
    // stored at the modrm (64 bit with a 64 bit operand size, REX.R selects
    // r8-r15)
    OPND_GP_16_32BIT,
    // The operand is an SSE register
    OPND_SIMD_MODRM,
    // The operand is a control register stored at modrm reg/opcode
    OPND_CTRL_MODRM,
    // The operand is a debug register stored at modrm reg/opcode
    OPND_DBG_MODRM,

    // The operand is encode at the modrm bytes and treated as byte operation
    OPND_MODRM_BYTEPTR,
    // The operand is encode at the modrm bytes and treated as word operation
    OPND_MODRM_WORDPTR,
    // The operand is encode at the modrm bytes and treated as d/word operation
    // (q/d/word: the operand size, 64 bits with REX.W or a default 64 bit
    // operand size)
    OPND_MODRM_dWORDPTR,
    // Used in jmpf, callf. TODO! Not implement yet!
    OPND_MODRM_FAR_OFFSET,
    // Used at opcode 'lea, les, bound' indicate that the operand is only valid
    // for memory reference
    OPND_MODRM_MEM,
    // The operand is encode at the modrm bytes and treated as qword operation
    // (m64fp, m64int)
    OPND_MODRM_QWORDPTR,
    // The operand is encode at the modrm bytes and treated as tword operation
    // (m80fp, m80bcd)
    OPND_MODRM_TWORDPTR,

    // The operand is an MMX register stored at modrm reg/opcode
    OPND_MMX_MODRM,
    // The operand is encode at the modrm bytes, an MMX register or a qword
    // memory reference
    OPND_MMX_RM,
    // The operand is encode at the modrm bytes, an SSE register or an oword
    // (128 bit) memory reference
    OPND_SIMD_RM,
    // The operand is encode at the modrm bytes, an SSE register or a qword
    // memory reference (movq, movsd)
    OPND_SIMD_RM_QWORD,
    // The operand is encode at the modrm bytes, an SSE register or a dword
    // memory reference (movss, addss)
    OPND_SIMD_RM_DWORD,
    // The operand is a 32 bit register stored at modrm reg/opcode (cvttss2si,
    // pextrw, movmskps), regardless of the operand-size prefix. In 64 bit mode
    // REX.W selects the 64 bit register (cvttss2si rax, xmm0)
    OPND_GP_32BIT_MODRM,
    // The operand is encode at the modrm bytes, a 32 bit register or a word
    // memory reference (pinsrw)
    OPND_MODRM_R32_WORDPTR,

    // The operand is ST(0), the top of the FPU stack
    OPND_FPU_ST0,
    // The operand is ST(i), stored at the modrm r/m
    OPND_FPU_ST_RM,
    // The operand is AX register (fnstsw ax)
    OPND_AX,

    //////////////////////////////////////////////////////////////////////
    // 64 bit mode operands. In 16 and 32 bit mode they are not used (the
    // entries which have them are in the AMD64 tables, see
    // gAmd64OneByteOpcodeTable)

    // The operand is encode at the modrm bytes and treated as a dword
    // operation, regardless of the operand size (the source of
    // 'movsxd r64, r/m32')
    OPND_MODRM_FIXED_DWORDPTR,
    // The operand is a general purpose register of the native size (32 bit,
    // 64 bit in 64 bit mode) stored at the modrm r/m, regardless of the
    // operand size (the general register of 'mov cr0, eax' / 'mov cr0, rax'
    // and 'mov dr0, rax')
    OPND_GP_NATIVE_RM,
    // The operand is encode at the modrm bytes: a memory reference of the
    // operand size doubled (m64 for cmpxchg8b, m128 for 'cmpxchg16b' with
    // REX.W)
    OPND_MODRM_DOUBLE_MEM
} OperandType;

/*
 *
 */
typedef enum {
    // Indicate that modrm byte is not present. Used for optimization.
    MODRM_NO_MODRM      = 0,

    // Indicate that modrm 'mod' referring to value (00, 01, 10) which means
    // memory access [eax], [bx+si], etc...
    MODRM_MOD_MEMORY    = 1,
    // Indicate that modrm 'r/m' referring to value (11) which means registers
    // pointer
    MODRM_MOD_REGISTERS = 2,
    // Both memory and registers (11)
    MODRM_MOD_MEM11     = MODRM_MOD_REGISTERS | MODRM_MOD_MEMORY,

    // The filter over the modrm mode
    MODRM_MOD_000       = 4,
    MODRM_MOD_001       = 8,
    MODRM_MOD_010       = 16,
    MODRM_MOD_011       = 32,
    MODRM_MOD_100       = 64,
    MODRM_MOD_101       = 128,
    MODRM_MOD_110       = 256,
    MODRM_MOD_111       = 512,

    // Indicate that the opcode is valid for all possible combinations..
    MODRM_ALL_FILTER    = 0xFFFF,
    // Indicate that the opcode is valid only for a memory reference (movntps)
    // or only for registers (movmskps)
    MODRM_MEMORY_ONLY   = MODRM_ALL_FILTER & ~MODRM_MOD_REGISTERS,
    MODRM_REGISTERS_ONLY = MODRM_ALL_FILTER & ~MODRM_MOD_MEMORY,

    // Indicate that only the modrm byte at bits 16..23 is valid (FPU opcodes
    // such as fchs, D9 E0). See MODRM_EXACT
    MODRM_EXACT_FLAG    = 0x1000000
} ModrmFilter;

// The modrm filter of an opcode which is valid for a single modrm byte
#define MODRM_EXACT(modrm) (MODRM_EXACT_FLAG | ((modrm) << 16))

/*
 * The mandatory prefix of an opcode: the prefix byte which selects the opcode
 * (SSE/SSE2 instructions). For example 0F 6E is 'movd mm, r/m32' and 66 0F 6E
 * is 'movd xmm, r/m32'.
 */
enum MandatoryPrefix {
    MANDATORY_NONE = 0x00,
    MANDATORY_66   = 0x66,
    MANDATORY_F2   = 0xF2,
    MANDATORY_F3   = 0xF3
};

/*
 * Here is a list of all information needed from an opcode:
 *   - A way to calculate it's length
 *   - The prefix of an opcode (How to separate opcodes)
 *   - The number of operands
 *   - Each operand and it's information
 */
struct OpcodeEntry {
    /*
     * The name of the opcode. The name must be in lower-cases, such as:
     * 'push', 'add', 'sub', etc...
     *
     * Symbols in names:
     *   *** - INVALID
     *   --- - OPCODEEOT
     *   #d  - 32bit substitute 'd', 16bit nothing        (pusha/pushad)
     *   #e  - 32bit substitute 'e', 16bit nothing        (jcxz/jecxz)
     *   ##  - 32bit substitute 'd', 16bit substitute 'w' (movsw/movsd)
     *   /   - First part of the word os the 16 bit opcode, Seconds part is
     *         the 32 bit opcode                          (cbw/cwde)
     *         An optional third part is the 64 bit operand size name
     *                                                    (cbw/cwde/cdqe)
     *
     * With a 64 bit operand size: '##' and '#d' substitute 'q' (movsq,
     * iretq, pushfq), and '#e' substitutes 'r' with a 64 bit address size
     * (jrcxz).
     */
    const char* m_opcodeName;

    /*
     * The opcode prefix and the opcode mask.
     * For example: Opcode
     */
    uint8 m_prefix;
    uint8 m_prefixMask;

    /*
     * The number of bytes the operands needed, can be 0 1 or 2.
     * For exmpale: 0 operands:  stosb
     *              1 operands:  push   eax
     *              2 operands:  add    cx,  al
     *              3 operands:  imul   eax, [eax], 0
     */
    OperandType m_firstOperand;
    OperandType m_secondOperand;
    OperandType m_thridOperand;

    // The state of the modrm, see ModrmFilter
    uint m_modrm;
    // Set to true if the immediate can be unsigned integer as well
    bool m_isUnsigned;

    // The flow altering properties of the opcode
    // Generally - jmps, calls and rets
    int m_alterProperty;

    // The mandatory prefix of the opcode, see MandatoryPrefix. The opcode is
    // valid only when the prefix exists, and the prefix is then a part of the
    // opcode (not an operand-size or a repeat prefix). The entries with a
    // mandatory prefix come before the entries of the same opcode without it.
    uint8 m_mandatoryPrefix;

    // The behaviour of the opcode in 64 bit mode, see Mode64. 0
    // (MODE64_LEGACY) for the entries which leave it out
    uint8 m_mode64;
};

/*
 * The behaviour of an opcode entry in 64 bit mode (OpcodeEntry::m_mode64).
 * In 16 and 32 bit mode the field is ignored, except MODE64_ONLY,
 * MODE64_NO_16BIT and MODE64_OPERAND_32_64.
 *
 * The operand size of 64 bit mode is 32 bits, 64 bits with REX.W (which wins
 * over 66), 16 bits with 66. The address size is 64 bits, 32 bits with 67.
 */
enum Mode64 {
    // The operand size follows REX.W and 66 as above
    MODE64_LEGACY = 0,
    // The opcode is invalid in 64 bit mode (push es, pusha, aaa, bound, lds,
    // les, into, jmp far ptr16:32, arpl, salc, 82 /r, the one byte inc / dec
    // 40-4F which are the REX prefixes...). The decoder skips the entry
    MODE64_INVALID = 1,
    // The default operand size is 64 bits: 66 selects 16 bits and there is
    // no 32 bit form (push, pop, pushf, popf, leave, enter, call / jmp
    // r/m, push fs, ret...). REX.W is 64 bits as the default, so it wins over
    // 66 (66 REX.W push rax is push rax)
    MODE64_DEFAULT_64 = 2,
    // The operand size is always 64 bits, 66 and REX.W are ignored: the near
    // relative branches (call rel32, jmp rel8 / rel32, jcc, loop, jrcxz).
    // 'ret' is MODE64_DEFAULT_64: nasm and ndisasm take 66 C3 as 'retw'
    MODE64_FORCE_64 = 3,
    // The entry exists in 64 bit mode only (movsxd, swapgs, cmpxchg16b...).
    // The decoder skips it in 16 and 32 bit mode
    MODE64_ONLY = 4,
    // The operand size is 32 bits, or 16 bits with 66, and REX.W is ignored:
    // the instructions which have no 64 bit operand (in, out, ins, outs).
    // Their registers still follow the other REX bits
    MODE64_NO_REXW = 5,
    // The opcode is invalid with a 16 bit operand size, in all modes: the
    // 16 bit form is not an instruction (bswap: the SDM leaves the result of
    // 'bswap r16' undefined, with 66 and in 16 bit mode without it). The operand
    // size is as MODE64_LEGACY
    MODE64_NO_16BIT = 6,
    // The general purpose operand is 32 bits, or 64 bits with REX.W, whatever
    // the 66 prefix or the 16 bit default operand size say, in all modes:
    // the r/m32 of movd, cvtsi2ss / cvtsi2sd and movnti (nasm has no
    // 'movd xmm0, ax')
    MODE64_OPERAND_32_64 = 7
};

extern const char* INVALID;
extern const char* OPCODEEOT;

// One byte opcode table
extern const OpcodeEntry gIa32OneByteOpcodeTable[];
// Two-bytes opcode table
extern const OpcodeEntry gIa32TwoBytesOpcodeTable[];
// FPU opcode table
extern const OpcodeEntry gIa32FPUOpcodeTable[];
// Three-bytes opcode tables, 0F 38 xx and 0F 3A xx
extern const OpcodeEntry gIa32ThreeBytes38OpcodeTable[];
extern const OpcodeEntry gIa32ThreeBytes3AOpcodeTable[];
// The entries of 64 bit mode which replace or add to the IA-32 tables
// (opcodeTableAMD64.cpp). In 64 bit mode the decoder searches them before the
// IA-32 table of the same escape; in 16 and 32 bit mode they are not used.
// For example 63 /r is 'movsxd r64, r/m32' (arpl in the IA-32 table, which is
// MODE64_INVALID), and REX.W 0F C7 /1 is cmpxchg16b
extern const OpcodeEntry gAmd64OneByteOpcodeTable[];
extern const OpcodeEntry gAmd64TwoBytesOpcodeTable[];
// Two-bytes opcode escape
extern const uint8 gIa32TwoByteEscapeCharacter;
// Three-bytes opcode escapes, the byte which follows the two-bytes escape
extern const uint8 gIa32ThreeBytes38EscapeCharacter;
extern const uint8 gIa32ThreeBytes3AEscapeCharacter;
// FPU opcode escape range
extern const uint8 gIa32FPUStartEscapeCharacter;
extern const uint8 gIa32FPUEndEscapeCharacter;

/*
 * A legacy prefix descriptor. Provide the name of the prefix (If there is one),
 * and the opcode number.
 */
struct OpcodePrefixEntry {
    // The name of the prefix
    const char* m_prefixName;
    // The code for the prefix
    uint8 m_opcode;
    // Set to true if the prefix is segment selector
    bool m_isSegmentSelector;
    // Set to true if the prefix has user-name meaning
    bool m_isOpcodeNameValid;
};

// The number of all possible legacy prefixs
enum { IA32_NUMBER_OF_PREFIXS = 11 };
// The table of all prefixs information
extern OpcodePrefixEntry gIa32PrefixTable[IA32_NUMBER_OF_PREFIXS];
// The operand-size override prefix opcode value
extern uint8 gIa32PrefixGroup3OperandSize;
// The address-size override prefix opcode value
extern uint8 gIa32PrefixGroup4AddressSize;

/*
 * The REX prefix of 64 bit mode: 40..4F (see IA32OpcodeDatastruct::REX). It
 * must be the last prefix before the opcode (a legacy prefix after it makes
 * it ignored, as on the processor); the mandatory prefix of SSE comes before
 * it (66 REX.W 0F 7E is 'movq r/m64, xmm'). In 16 and 32 bit mode 40..4F are
 * inc / dec
 */
enum {
    AMD64_REX_PREFIX      = 0x40,
    AMD64_REX_PREFIX_MASK = 0xF0
};

// Each instruction may handle up to 4 prefixs, 3 bytes of opcode, 8 bytes
// of immediate information
enum { MAX_PREFIX = 16 };


}; // end of namespace ia32dis

#endif // __TBA_DISMOUNT_PROC_IA32_OPCODETABLE_H
