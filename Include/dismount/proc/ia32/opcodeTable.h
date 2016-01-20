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
    NO_REGISTER = NUMBER_OF_REGISTERS
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
 * The different GP registers values
 */
enum {
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
    // current 66 size prefix.
    OPND_IMMEDIATE_DS,

    // The operand is memory reference offest encoded as direct immediate.
    // The length of the immediate is the default address-size (16bit/32bit
    // according to the 67 prefix...
    OPND_MEMREF_OFFSET_DS,

    // The operand is offset (used in loops, jmp etc) encoded as immediate
    OPND_IMMEDIATE_OFFSET_SHORT_8,
    // The operand is long offset (used in loops, jmp etc) encoded as immediate
    OPND_IMMEDIATE_OFFSET_LONG_32,
    // The operans is offset (used in jmp near, call near)
    OPND_IMMEDIATE_OFFSET_DS,
    // The operand is a far offset (offset16/32:segment)
    OPND_IMMEDIATE_OFFSET_FAR,

    // The operand is GP encoded within the 3 bits of the first-bytes opcode
    OPND_ONEBYTES_OPCODE_GP_16_32,
    // The operand is 8bit GP encoded within the 3 bits of the first-bytes opcode
    OPND_ONEBYTES_OPCODE_GP_8,

    // The operand is AL register
    OPND_AL,
    // The operand is AX or EAX registers
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
    // stored at the modrm
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
    OPND_MODRM_dWORDPTR,
    // Used in jmpf, callf. TODO! Not implement yet!
    OPND_MODRM_FAR_OFFSET,
    // Used at opcode 'lea, les, bound' indicate that the operand is only valid
    // for memory reference
    OPND_MODRM_MEM
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
} ModrmFilter;

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
};

extern const char* INVALID;
extern const char* OPCODEEOT;

// One byte opcode table
extern const OpcodeEntry gIa32OneByteOpcodeTable[];
// Two-bytes opcode table
extern const OpcodeEntry gIa32TwoBytesOpcodeTable[];
// FPU opcode table
extern const OpcodeEntry gIa32FPUOpcodeTable[];
// Two-bytes opcode escape
extern const uint8 gIa32TwoByteEscapeCharacter;
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

// Each instruction may handle up to 4 prefixs, 3 bytes of opcode, 8 bytes
// of immediate information
enum { MAX_PREFIX = 16 };


}; // end of namespace ia32dis

#endif // __TBA_DISMOUNT_PROC_IA32_OPCODETABLE_H
