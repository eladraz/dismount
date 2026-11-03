#ifndef __TBA_DISMOUNT_PROC_IA32_IA32OPERANDDECODER_H
#define __TBA_DISMOUNT_PROC_IA32_IA32OPERANDDECODER_H

/*
 * IA32OperandDecoder.h
 *
 * Decode the operands of an IA-32 instruction into structured values
 * (registers, memory references, immediates and branch targets) instead of
 * text. Used by the semantic lifter.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/ia32/IA32Opcode.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/AMD64Architecture.h"

/*
 * A decoded operand of an IA-32 instruction.
 */
struct IA32DecodedOperand {
    enum Kind {
        // The operand doesn't exist
        KIND_NONE,
        // A register, see m_register
        KIND_REGISTER,
        // A memory reference: m_segment:[m_base + m_index * m_scale +
        // m_displacement], computed in m_addressBits. With m_isRipRelative
        // the base is the address of the next instruction
        KIND_MEMORY,
        // An immediate, see m_immediate
        KIND_IMMEDIATE,
        // The target of a relative branch, an absolute guest address in
        // m_immediate (already truncated to 16 bits for a 16 bit operand
        // size)
        KIND_BRANCH_TARGET,
        // A far pointer immediate: m_farSegment:m_immediate
        KIND_FAR_POINTER
    };

    // The m_base / m_index of a memory reference which has none
    enum { NO_REGISTER = ia32dis::AMD64_REGISTERS_COUNT };

    // See Kind
    uint m_kind;
    // The size of the operand's value in bits: the register size, the size
    // of the memory access (the operand size for OPND_MODRM_MEM, the
    // offset and the selector of OPND_MODRM_FAR_OFFSET), the size of the
    // operand an immediate is used as (the sign extended imm8 of
    // 'add eax, imm8' is 32 bits)
    uint m_bits;

    // KIND_REGISTER: the ia32dis::IA32_REG_* or AMD64_REG_* id (al, ax, eax,
    // xmm1, rax, r8d, spl, xmm9...)
    uint m_register;

    // KIND_MEMORY: the segment register (ia32dis::IA32_REG_ES..GS), the
    // override prefix or the default (ss for ebp/esp/bp based addresses)
    uint m_segment;
    // KIND_MEMORY: true if the segment comes from a prefix
    bool m_isSegmentOverride;
    // KIND_MEMORY: the base and index registers (of the address size: 16,
    // 32 or 64 bit registers), or NO_REGISTER
    uint m_base;
    uint m_index;
    // KIND_MEMORY: 1, 2, 4 or 8
    uint m_scale;
    // KIND_MEMORY: the displacement, sign extended (the 64 bit absolute
    // offset of 'mov al, [moffs64]' as is)
    int64 m_displacement;
    // KIND_MEMORY: the address size, 16, 32 or 64
    uint m_addressBits;
    // KIND_MEMORY: true for [rip + disp32] (64 bit mode): m_base is
    // NO_REGISTER, the address is getNextAddress() + m_displacement (in the
    // address size, a 32 bit address size truncates it)
    bool m_isRipRelative;

    // KIND_IMMEDIATE: the value, sign extended from its encoded size (see
    // IA32OperandDecoder::decode for the exceptions). KIND_BRANCH_TARGET,
    // KIND_FAR_POINTER: the address
    int64 m_immediate;
    // KIND_FAR_POINTER: the selector
    uint16 m_farSegment;
};

/*
 * Decode the operands of a decoded IA-32 instruction.
 */
class IA32OperandDecoder {
public:
    /*
     * Return the number of explicit operands of the instruction (0 to 3)
     */
    static uint getOperandsCount(const IA32Opcode& instruction);

    /*
     * Decode an operand
     *
     * instruction - The instruction
     * index       - The operand index (0 is the first operand)
     *
     * The immediates are sign extended from their encoded size, except:
     *   - The imm16 of 'enter', 'ret imm16' and 'retf imm16', and the imm8
     *     of 'enter', 'int', 'in', 'out', 'aam', 'aad': zero extended.
     *   - An immediate whose size is the operand size is the same either way.
     *
     * Throw exception if the operand doesn't exist
     */
    static IA32DecodedOperand decode(const IA32Opcode& instruction, uint index);

    // Return the operand size of the instruction, 16, 32 or 64
    static uint getOperandBits(const IA32Opcode& instruction);
    // Return the address size of the instruction, 16, 32 or 64
    static uint getAddressBits(const IA32Opcode& instruction);

    // Return true if the instruction is decoded in 64 bit mode
    static bool is64BitMode(const IA32Opcode& instruction);

    /*
     * Return true if the instruction has a prefix. A mandatory prefix of the
     * opcode (66 / F2 / F3 of SSE) is not a prefix of the instruction
     *
     * prefix - The prefix byte: 0xF0 (lock), 0xF2 (repne), 0xF3 (rep, repe)
     */
    static bool hasPrefix(const IA32Opcode& instruction, uint8 prefix);

    /*
     * Return the segment register of the last segment override prefix, or
     * 'defaultSegment' if there is none (for the implicit memory operands of
     * movs, lods, xlat...)
     */
    static uint getSegment(const IA32Opcode& instruction, uint defaultSegment);

    /*
     * Return the last byte of the opcode (without the ModR/M): the condition
     * code of jcc, setcc and cmovcc is its low 4 bits
     */
    static uint8 getLastOpcodeByte(const IA32Opcode& instruction);

    /*
     * Return the reg/opcode field of the ModR/M (the /digit of the group
     * opcodes)
     */
    static uint getModrmRegOpcode(const IA32Opcode& instruction);

    /*
     * Return the address of the instruction and of the next one (0 based if
     * the disassembler didn't give the instruction an address)
     */
    static uint64 getAddress(const IA32Opcode& instruction);
    static uint64 getNextAddress(const IA32Opcode& instruction);
};

#endif // __TBA_DISMOUNT_PROC_IA32_IA32OPERANDDECODER_H
