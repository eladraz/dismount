#ifndef __TBA_DISMOUNT_ASSEMBLER_PROCESSORS_IA32_IA32ASSEMBLER_H
#define __TBA_DISMOUNT_ASSEMBLER_PROCESSORS_IA32_IA32ASSEMBLER_H
/*
 * IA32Assembler.h
 *
 * Provide an easy and simple way to generate ia32 binary opcodes by assembling
 * simplier english instructions
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/parser/parser.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/IntegerEncoding.h"
#include "dismount/assembler/AssemblerInterface.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/proc/ia32/IA32OpcodeDatastruct.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"

/*
 * Implement the AssemblerInterface for IA32 processor family. Including the
 * old 16 bit processor up to the 32 bit processor with MMX/SMID and the 64bit
 * REX extention processors.
 */
class IA32Assembler : public AssemblerInterface {
public:
    /*
     * Constructor
     *
     * binary        - The binary to fill the information to
     * processorMode - The mode of the processor can be either 16 bit, 32 bit
     *                 and more
     */
    IA32Assembler(FirstPassBinaryPtr binary,
                  IA32eInstructionSet::DisassemblerTypes processorMode);

    // See AssemblerInterface::getAssembler()
    virtual cStringerStreamPtr getAssembler();

    // See AssemblerInterface::getFirstPass()
    virtual FirstPassBinaryPtr getFirstPass();

private:
    // The binary object
    FirstPassBinaryPtr m_binary;

    // The processor mode
    IA32eInstructionSet::DisassemblerTypes m_processorMode;
};

/*
 * Usage:
 *     IA32Assembler compiler(firstBinaryPass, INTEL_32);
 *     compiler << "push ebp" << endl;
 *     compiler << "mov  ebp, esp" << endl;
 *     compiler << "sub  esp, 4" << endl;
 *      ...
 *     compiler << "mov  esp, ebp" << endl;
 *     compiler << "pop  ebp" << endl;
 *     compiler << "ret" << endl;
 *
 * The syntax is the nasm subset used by the IA-32 compatibility tests
 * (tests/ia32/TEST_PLAN.md): lowercase mnemonics, hex as 0x.., optional 'ptr'
 * after size keywords, memory as [seg:base+index*scale+disp], ';' comments and
 * the prefixes lock/rep/repe/repz/repne/repnz, the segment names and the nasm
 * prefixes o16/o32/o64 (operand size) and a16/a32/a64 (address size). In 64 bit
 * mode (IA32eInstructionSet::AMD_64) the registers r8-r15, xmm8-xmm15, cr8, the
 * rip relative memory [rip+disp] / [rel $+N] and the 64 bit offsets
 * [qword 0x1122334455667788] are supported too.
 *
 * TODO! Add to this assumption dependency mode.
 *   When the destructor is called all the source code will be assemble and
 *   append to the binary code.
 *
 * When the compiler seeing a label he don't even trying to resolve it. The name
 * of the label is added into the dependency tree and will be resolved later;
 * The binary will contains an empty reference.
 *
 * Opcodes such as 'call eax' will not be appended to the relocation assuming
 * that the 'mov' opcode causes the relocation.
 */
class IA32AssemblerStream : public cStringStream {
public:
    /*
     * Constructor
     *
     * binary - The binary to fill the information to
     * processorMode - The mode of the processor can be either 16 bit, 32 bit
     *                 and more
     */
    IA32AssemblerStream(FirstPassBinary& binary,
                        IA32eInstructionSet::DisassemblerTypes processorMode);

    /*
     * Destructor. Compile the stream (if any data is pending) and never throws.
     */
    virtual ~IA32AssemblerStream();

    /*
     * Parse and encode all the buffered text into the binary, then clear the
     * buffer. Throws on invalid input. Called by the destructor.
     */
    void assemble();

private:
    /*
     * Represent an operand
     *
     * Author: Elad Raz
     */
    class Operand {
    public:
        // The kind of the operand
        enum Kind {
            // This operand is empty
            NO_OPERAND,
            // A register (see RegisterClass and getReference())
            REGISTER,
            // A constant value (see getValue())
            IMMEDIATE,
            // A relative target of a branch: '$+N', see getValue()
            OFFSET,
            // A far pointer 'segment:offset', see getValue() and getSegmentValue()
            FAR_POINTER,
            // A memory reference, see getBase(), getIndex(), ...
            MEMORY
        };

        // The class of a register operand
        enum RegisterClass {
            // al..bh (0..7); in 64 bit mode also r8b..r15b (8..15)
            REG_8BIT,
            // ax..di, r8w..r15w
            REG_16BIT,
            // eax..edi, r8d..r15d
            REG_32BIT,
            // rax..rdi, r8..r15 (64 bit mode only)
            REG_64BIT,
            // spl, bpl, sil, dil (64 bit mode only, the indexes 4..7): an 8
            // bit register which needs a REX prefix (and can't be used with
            // ah..bh in the same instruction)
            REG_8BIT_REX,
            REG_SEGMENT,
            // MMX registers (mm0-mm7)
            REG_MMX,
            // SSE registers (xmm0-xmm7, xmm8-xmm15 in 64 bit mode)
            REG_XMM,
            // x87 stack registers (st0-st7, 'st' is st0)
            REG_FPU,
            REG_CONTROL,
            REG_DEBUG
        };

        // The branch hint (short/near keyword in front of an offset)
        enum BranchHint {
            HINT_NONE,
            HINT_SHORT,
            HINT_NEAR
        };

        // Default constructor. Define that this operand is not in use
        Operand();

        /*
         * Read an operand from the parser. The operand ends at the end of the
         * line, at a ';' comment or at a ',' (the separator of the next
         * operand, which is not read). Throws if the operand is invalid.
         *
         * The text is lowercase (see IA32AssemblerStream::assemble()).
         *
         * Supported forms:
         *   eax, al, mm0, xmm1, es, cr0, dr0           registers
         *   rax, r8, r9d, r10w, r11b, spl, xmm12, cr8  registers of 64 bit mode
         *   [rip+0x10], [rel $+0x10]                    rip relative memory
         *                                              (64 bit mode, see
         *                                              isRipRelative())
         *   [qword 0x1122334455667788], [a32 0x1234]    the offset forms of
         *                                              mov al, [offset] (64 bit
         *                                              or 32 bit offset)
         *   push word fs                                a size keyword of a
         *                                              segment register
         *   0x12, -1, 255, 7bh                         immediates
         *   [eax+ecx*4+0x10], dword [..], byte ptr [..] memory, see below
         *   fs:[eax], [fs:eax], word [gs:bx]           memory with segment
         *   $+0x10, $-2, short $+0x10, near $+0x10     branch offsets
         *   0x1234:0x12345678                          far pointers
         *
         * Memory: the address is base+index*scale+disp in any subset, with
         * scale 1/2/4/8 and a leading '-' allowed on the displacement. A scale
         * of 1 with no base uses the index as the base, and a scale of 2 with
         * no base is encoded as base=index,scale=1 (nasm does the same). The
         * 16 bit registers bx, bp, si and di select 16 bit addressing, the
         * 32 bit registers select 32 bit addressing in 64 bit mode (the 67
         * prefix), the 64 bit registers the default 64 bit addressing.
         *
         * Offsets (branches) use nasm semantics: '$+N' is N bytes from the
         * start of the instruction (the old meaning was a raw displacement from
         * the end of the instruction). The encoding (rel8 or rel32) is chosen
         * during matching, see IA32AssemblerStream::writeOpcode().
         */
        void readOperand(Parser& parser);

        /*
         * Return the operand which is written in 'text' (a fixed operand such
         * as "st1" or "0x0a"). Throws if the operand is invalid.
         */
        static Operand fromText(const cString& text);

        // Return true if the operand is the register 'index' of the class
        bool isRegister(RegisterClass regClass, uint index) const;
        // Return true if the operand is an x87 stack register (st0-st7)
        bool isFpuRegister() const;
        // Return the kind of the operand
        Kind getKind() const;

        // Return the explicit size keyword in bits (8/16/32/...), 0 if none
        uint getSize() const;

        // Return the register class (REGISTER only)
        RegisterClass getRegisterClass() const;

        // Return the register index (REGISTER only)
        uint getReference() const;

        // Return the immediate value, the offset target or the far offset
        int64 getValue() const;

        // Return the segment of a far pointer (FAR_POINTER only)
        uint32 getSegmentValue() const;

        // Return the branch hint (OFFSET only)
        BranchHint getBranchHint() const;

        // Return true if the memory reference has the 'far' keyword
        bool isFar() const;

        // Return the segment register index of a memory reference, -1 if none
        int getSegment() const;

        // Return the base register of a memory reference, -1 if none
        int getBase() const;

        // Return the index register of a memory reference, -1 if none
        int getIndex() const;

        // Return the scale of the index (1, 2, 4 or 8)
        uint getScale() const;

        // Return the address size of a memory reference (16/32/64), 0 for
        // default
        uint getAddressWidth() const;
        // Return true for a rip relative memory reference: [rip + disp] is
        // the displacement from the end of the instruction, [rel $+N] the
        // nasm form of a target N bytes from the start of the instruction
        // (encoded as [rip + N - length])
        bool isRipRelative() const;
        // Return true for [rel $+N] (see isRipRelative())
        bool isRelativeToInstruction() const;

        // Return the displacement of a memory reference
        int64 getDisplacement() const;

    private:
        // Read a memory reference, after the opening '['
        void readAddress(Parser& parser);

        // Add a register term into the address
        void addAddressRegister(RegisterClass regClass, uint index, uint scale, int sign);

        // Read the '$' relative target
        void readOffset(Parser& parser);

        // Check a register name and set the register fields of the operand
        static bool checkRegister(const cString& regname,
                                  RegisterClass& regClass,
                                  uint& regIndex);

        // Operand kind and explicit size (the size keyword)
        Kind m_kind;
        uint m_size;

        // REGISTER: the register class and index
        RegisterClass m_regClass;
        uint m_reference;

        // IMMEDIATE, OFFSET, FAR_POINTER: the value. MEMORY: the displacement
        int64 m_value;
        bool m_hasDisplacement;

        // FAR_POINTER: the segment value
        uint32 m_segmentValue;

        // OFFSET: branch hint
        BranchHint m_hint;

        // MEMORY: the 'far' keyword (far pointer, jmp far dword [mem])
        bool m_far;

        // MEMORY: address components (-1 is not in use)
        int m_segment;
        int m_base;
        int m_index;
        uint m_scale;
        uint m_addressWidth;
        // MEMORY: rip relative, see isRipRelative() and
        // isRelativeToInstruction()
        bool m_ripRelative;
        bool m_relativeToInstruction;
    };

    /*
     * The prefixes written in front of the instruction (before the opcode).
     * The segment override may also come from the memory operand.
     */
    struct Prefixes {
        // The segment register index (IA32_SEG_*), -1 if none
        int m_segment;
        // True if the lock prefix is used
        bool m_lock;
        // 0, or the repeat prefix opcode (0xf2 or 0xf3)
        uint8 m_repeat;
        // 0, or the operand size which the nasm prefixes o16 / o32 / o64 force
        // (16, 32 or 64)
        uint m_operandWidth;
        // 0, or the address size which the nasm prefixes a16 / a32 / a64 force
        // (16, 32 or 64)
        uint m_addressWidth;
    };

    /*
     * The encoding of one matched opcode table row. Filled by matchOpcode().
     */
    struct Encoding {
        // The table index: 0 for one-byte opcodes, 1 for 0x0f opcodes, 2 for
        // the x87 (FPU) opcodes whose prefix byte is the escape itself
        uint m_table;
        // The opcode entry
        const ia32dis::OpcodeEntry* m_opcode;
        // The operand width (8, 16, 32 or 64 bits), 0 if not fixed by the
        // operands
        uint m_operandWidth;
        // True if the address size of the memory reference is not the default
        bool m_addressSizePrefix;
        // True if a modrm byte exists
        bool m_hasModrm;
        // True if an operand fills the reg field (the /digit filter is not used)
        bool m_regFieldSet;
        // The modrm byte (the reg field is filled from the registers)
        IA32OpcodeDatastruct::MODRM m_modrm;
        // True if a SIB byte exists (encoded in m_modrm.m_bits.m_rm == 4)
        bool m_hasSib;
        IA32OpcodeDatastruct::SIB m_sib;
        // The size of the memory displacement in bytes, 0 if none
        uint m_displacementSize;
        // The segment prefix (IA32_SEG_*), -1 if none
        int m_segment;
        // The prefixes of the instruction
        Prefixes m_prefixes;
        // 64 bit mode: the REX prefix, written when m_needsRex. Its W bit is
        // set for a 64 bit operand size (unless the entry is MODE64_DEFAULT_64
        // or MODE64_FORCE_64), R / X / B extend the reg field, the SIB index
        // and the r/m, SIB base or opcode register field
        IA32OpcodeDatastruct::REX m_rex;
        bool m_needsRex;
        // 64 bit mode: true if an operand is ah, ch, dh or bh, which can't be
        // encoded with a REX prefix
        bool m_hasHighByteRegister;
        // 64 bit mode: true for a rip relative displacement, which is
        // computed once the instruction length is known
        bool m_isRipRelative;
        // The total length of the instruction in bytes
        uint m_length;
    };

    /*
     * Read one instruction from the parser (up to the end of the line or a ';'
     * comment), split into prefixes, name and operands, and write it.
     */
    void assembleInstruction(Parser& parser);

    /*
     * Try to find opcode according to it's name and it's parameters and
     * write it into the first-binary pass
     *
     * name                 - The name of the opcode (e.g. 'mov')
     * prefixes             - The prefixes of the instruction
     * first, second, third - The operand description (e.g. OPERAND_32BIT_REG)
     *
     * Throw exception if the opcode is invalid
     */
    void writeOpcode(const cString& name,
                     const Prefixes& prefixes,
                     const Operand& first,
                     const Operand& second,
                     const Operand& third);

    /*
     * Search the opcode tables for the shortest encoding of the instruction
     * (name, prefixes and operands). Return true and fill 'best' if found.
     * segment is the segment override of the instruction (-1 if none).
     */
    bool findEncoding(const cString& name,
                      const Prefixes& prefixes,
                      int segment,
                      uint addressWidth,
                      const Operand& first,
                      const Operand& second,
                      const Operand& third,
                      Encoding& best,
                      bool preferDcForSt0) const;

    /*
     * 64 bit mode: return true if the entry of the IA-32 table (source 0 is
     * the one-byte table, 1 the two-bytes table) has the same encoding as an
     * entry of the AMD64 table, which then replaces it (movsxd and arpl,
     * mov r64, imm64 and mov r32, imm32, ...).
     */
    bool isReplacedByAmd64Entry(const ia32dis::OpcodeEntry* opcode,
                                uint source) const;

    /*
     * Return true if the 66 operand size prefix is written for the encoding
     * (the operand size differs from the default, and no mandatory 66 prefix
     * already carries it).
     */
    bool needsOperandSizePrefix(const Encoding& enc) const;

    /*
     * Check whether the mnemonic 'name' is the name of the table entry
     * 'tableName' in the current processor mode. The table names can contain
     * wildcards:
     *   '/'  - 16 bit name / 32 bit name        (cbw/cwde)
     *          An optional third name is the 64 bit operand size name
     *                                           (cbw/cwde/cdqe)
     *   '##' - 'w' in 16 bit, 'd' in 32 bit     (movsw/movsd)
     *   '#d' - 'd' in 32 bit, nothing in 16 bit (pusha/pushad)
     *   '#e' - 'e' in 32 bit, nothing in 16 bit (jcxz/jecxz)
     * In 64 bit mode all the names of the entry are valid, 'q' is the 64 bit
     * operand size of '##' and '#d' (movsq, pushfq) and 'r' is the 64 bit
     * address size of '#e' (jrcxz). The names of the 16 bit address size are
     * not valid.
     *
     * operandWidth    - The operand size (16, 32 or 64) which the spelling of
     *                   'name' selects, 0 for the default. In 16 and 32 bit
     *                   mode it is set only if 'name' is the other size of the
     *                   table entry (the operand size prefix must be changed)
     * addressSizeFlip - Set if 'name' is the other size of the address size
     *                   (jcxz in 32 bit mode, jecxz in 64 bit mode)
     */
    bool matchMnemonic(const char* tableName, const cString& name,
                       uint& operandWidth, bool& addressSizeFlip) const;

    /*
     * Match an opcode into entry of opcode table
     *
     * opcode               - The opcode entry
     * first, second, third - The operand description
     * relaxed              - 0: the immediates are the sign-extended forms
     *                        1: 8 bit immediates may be 0x80..0xff, and the
     *                           immediate of a 64 bit operand may be bigger
     *                           than 32 bits (its low dword is used)
     *                        2: 8 bit immediates are the low byte of any value
     *                           (nasm truncates them)
     *                        The levels are tried one after the other
     * enc                  - Will be filled with the encoding if matched
     *
     * Return true if the opcode is match
     * Return false otherwise
     */
    bool matchOpcode(const ia32dis::OpcodeEntry* opcode,
                     const Operand& first,
                     const Operand& second,
                     const Operand& third,
                     uint relaxed,
                     Encoding& enc) const;

    /*
     * Tries to match between compiler operand information and assembler table
     * operand type.
     *
     * ia32disType     - The operand type of the table entry
     * compilerOperand - The operand description
     * relaxed         - See matchOpcode()
     * enc             - The encoding which is updated
     *
     * Return true for any possible match
     * False otherwise
     */
    bool matchOperand(ia32dis::OperandType ia32disType,
                      const Operand& compilerOperand,
                      uint relaxed,
                      Encoding& enc) const;

    /*
     * Match a memory reference against the table type. Fills the modrm, sib
     * and displacement of the encoding.
     */
    bool matchMemory(ia32dis::OperandType ia32disType,
                     const Operand& compilerOperand,
                     Encoding& enc) const;

    /*
     * Fix the operand width (16/32/64 bits) of the instruction. Return false
     * if the operands disagree on the width, or if the table entry has no such
     * operand size in the processor mode (a 32 bit 'push' in 64 bit mode, a
     * 64 bit 'in').
     */
    bool setOperandWidth(uint width, Encoding& enc) const;

    /*
     * Return true if the table entry can have the operand size 'width' (see
     * ia32dis::Mode64)
     */
    bool isWidthAllowed(uint width, const Encoding& enc) const;

    /*
     * 64 bit mode: record the use of a register. The bit 3 of the index is
     * the REX extension bit (B for the r/m, the SIB base and the register of
     * the opcode, R for the reg field), spl..dil need a REX prefix and
     * ah..bh can't be encoded with one.
     *
     * isReg - True if the register is in the reg field, false for the
     *         r/m field or the register of the opcode
     */
    static void useRegister(Operand::RegisterClass regClass, uint index,
                            bool isReg, Encoding& enc);

    /*
     * Put the register into the reg field / the r/m field (mod 11) of the modrm
     * byte, see useRegister()
     */
    static void setRegField(const Operand& reg, Encoding& enc);
    static void setRmRegister(const Operand& reg, Encoding& enc);

    /*
     * 64 bit mode: complete the REX prefix once the operand size is known (the
     * W bit). Return false if the instruction can't be encoded (ah..bh with a
     * REX prefix).
     */
    bool finishRex(Encoding& enc) const;

    /*
     * Return true if the immediate value fits the table operand type.
     */
    bool checkImmediate(ia32dis::OperandType ia32disType,
                        const Operand& compilerOperand,
                        uint relaxed,
                        const Encoding& enc) const;

    /*
     * Compute the address encodings for a memory reference, 32 or 16 bit
     * address size. Throws if the addressing form is not encodable.
     */
    static void encodeMemory32(const Operand& compilerOperand, Encoding& enc);
    void encodeMemory16(const Operand& compilerOperand, Encoding& enc) const;

    /*
     * Compute the address encodings for a memory reference of 64 bit mode
     * (64 or 32 bit address size): the REX.X / REX.B bits of r8-r15, the SIB
     * which rsp / r12 need, the displacement which rbp / r13 need, and rip
     * relative addressing. Throws if the addressing form is not encodable.
     */
    void encodeMemory64(const Operand& compilerOperand, Encoding& enc) const;

    /*
     * Return true if the processor mode is 64 bit mode
     */
    bool is64BitMode() const;

    /*
     * Gives the length of the encoded instruction in bytes.
     *
     * enc                  - The matched encoding
     * first, second, third - The operands
     */
    uint calculateLength(const Encoding& enc) const;

    /*
     * Check that the relative offsets of the opcode fit into their encoding,
     * once the instruction length is known.
     */
    bool checkOffsets(const Encoding& enc,
                      const Operand& first,
                      const Operand& second,
                      const Operand& third) const;

    /*
     * Return the displacement of the rip relative memory reference, once the
     * length of the instruction is known (see Operand::isRipRelative())
     */
    int64 getRipDisplacement(const Operand& memory, const Encoding& enc) const;

    /*
     * Return the size in bytes of an immediate, offset or far pointer in the
     * encoded instruction.
     */
    uint getImmediateSize(ia32dis::OperandType ia32disType,
                          const Encoding& enc) const;

    /*
     * Encode an opcode inside the first-binary pass
     *
     * enc                  - The matched encoding
     * first, second, third - The operands
     *
     * Throw exception if some error occured.
     */
    void encodeOpcode(const Encoding& enc,
                      const Operand& first,
                      const Operand& second,
                      const Operand& third);

    /*
     * Write an immediate, offset or far pointer of a single operand.
     */
    void writeImmediate(ia32dis::OperandType ia32disType,
                        const Operand& compilerOperand,
                        const Encoding& enc);

    // Write 'bytes' bytes (1, 2, 4 or 8) of value in little endian
    void writeValue(int64 value, uint bytes);

    // Return the default operand size of the processor mode (16 or 32, 32
    // in 64 bit mode)
    uint getDefaultOperandWidth() const;

    // Return the operand width used by the encoding (16, 32 or 64): the
    // width of the operands, or the default of the table entry (32, 64 for the
    // 64 bit mode entries with a default operand size of 64 bits)
    uint getEncodingWidth(const Encoding& enc) const;

    // Return the default address size of the processor mode (16, 32 or 64)
    uint getDefaultAddressWidth() const;

    // Return the byte opcode of the segment prefix (from the table)
    static uint8 getSegmentPrefix(uint segment);

    // Return the opcode byte of a prefix from the table, by its name
    static uint8 getPrefixOpcode(const cString& name);

    // The binary
    FirstPassBinary& m_binary;

    // The mode of the processor
    IA32eInstructionSet::DisassemblerTypes m_processorMode;
};

#endif // __TBA_DISMOUNT_ASSEMBLER_PROCESSORS_IA32_IA32ASSEMBLER_H
