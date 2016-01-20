#ifndef __TBA_DISMOUNT_PROC_IA32_IA32OPCODEDATASTRUCT_H
#define __TBA_DISMOUNT_PROC_IA32_IA32OPCODEDATASTRUCT_H

/*
 * IA32OpcodeDatastruct.h
 *
 * Contains a set of data structures used by x86 processor in order to encode
 * instructions.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"

/*
 * The x86 instruction has the following format:
 *    <Legacy prefixs>  <REX prefix>    <Opcode>      ModR/M      SIB
 *      16/32bit        64 bit (Opt)   1,2,3 byte    byte (Opt)   byte
 *
 * After the instruction displacement and immediate information will be followed
 * if needed.
 */
class IA32OpcodeDatastruct {
public:
    /*
     * The ModR/M stand for mode register/memory and use to describe the
     * different operands in the instruction.
     * 64bit extension uses also the REX prefix in order choose which 64bit
     * register to be in use.
     */
    typedef union {
        // The modem packed data
        uint8 m_packed;
        // The bits of the modrm
        struct {
            // The r/m field can specify a register as an operand or can be
            // combined with the mod field to encode an addressing mode.
            unsigned m_rm : 3;
            // The reg/opcode field specifies either a register number or three
            // more bits of opcode infor-mation. The purpose of the reg/opcode
            // field is specified in the primary opcode.
            unsigned m_regOpcode : 3;
            // The mod field combines with the r/m field to form 32 possible
            // values: eight registers and 24 addressing modes.
            unsigned m_mod : 2;
        } m_bits;
    } MODRM;

    /*
     * SIB stand for Scale-Index-Base and use for operand in the format:
     *    [ebx*4+2]+disp.
     */
    typedef union {
        // The SIB packed data
        uint8 m_packed;
        // The bits of the SIB
        struct {
            // The base
            unsigned m_base : 3;
            // The index
            unsigned m_index : 3;
            // The scale multpiler
            unsigned m_scale : 2;
        } m_bits;
    } SIB;

    /*
     * REX stand for register extension. Used in order to expand the abilities
     * of 32bit instruction to match the new register set of the 64bit CPU.
     */
    typedef union {
        // The REX packed data
        uint8 m_packed;
        // The bits of the REX prefix
        struct {
            // Extension for the SIB 'base' field, MODRM 'rm' field, or the
            // 'Opcode-register' field.
            unsigned b : 1;
            // Extension for the SIB 'index' field
            unsigned x : 1;
            // Extension for the MODRM 'reg' field
            unsigned r : 1;
            // 0 means default operand size, 1 means 64bit operand size
            unsigned w : 1;
            // The high nibble of the REX is always 0100 (0x4?)
            unsigned m_prefix : 4;
        } m_bits;
    } REX;

    /*
     * The displacement can be either 1,2,4 bytes or non.
     * TODO! Test 64bit processor so there really aren't any displacement above
     *       4 bytes;
     */
    typedef uint32 DisplacementType;

    /*
     * The immediate can be 1,2,4,8 bytes and also support 16 bit segment number
     */
    typedef struct {
        uint64 offset;
        uint16 segment;
    } ImmediateType;
};

#endif // __TBA_DISMOUNT_PROC_IA32_IA32OPCODEDATASTRUCT_H
