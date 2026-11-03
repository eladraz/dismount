/*
 * TestAMD64Disassembler.cpp
 *
 * Directed tests of the 64 bit mode of the IA-32 disassembler: the text of
 * the decoded instructions, the length, the opcodes which are invalid in
 * 64 bit mode and the structured operands (the REX registers, the rip
 * relative memory, the moffs64 and imm64 operands).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/datastream.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/stream/ioStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/InvalidOpcodeByte.h"
#include "dismount/proc/ia32/IA32Opcode.h"
#include "dismount/proc/ia32/IA32OperandDecoder.h"
#include "dismount/proc/ia32/AMD64Architecture.h"

using namespace ia32dis;

// Like TESTS_ASSERT, but tell which check failed
#define AMD64_ASSERT(condition) \
    if (!(condition)) \
    { \
        cout << "TestAMD64Disassembler.cpp:" << __LINE__ << ": " << #condition \
             << " (operand test " << m_current << ")" << endl; \
        XSTL_THROW(cException, XSTL_STRING("Condition asserted")); \
    }
#define AMD64_ASSERT_EQUAL(a, b) AMD64_ASSERT(((a) == (b)))

class TestObjectTestAMD64Disassembler : public cTestObject {
public:
    // Max-opcode size
    #define MAX_AMD64_OPCODE (16)

    // The text of a decoded instruction
    struct TextTest {
        const byte opcode[MAX_AMD64_OPCODE];
        uint  opcodeSize;
        const char* text;
    };

    // The type of the operand which the structured test checks
    enum OperandCheck {
        // A register: m_register, m_bits
        CHECK_REGISTER,
        // A memory reference: base, index, scale, displacement, rip relative
        CHECK_MEMORY,
        // An immediate or a branch target: m_immediate, m_bits
        CHECK_IMMEDIATE
    };

    // The expected structure of a decoded operand
    struct OperandTest {
        const byte opcode[MAX_AMD64_OPCODE];
        uint  opcodeSize;
        // The operand to decode
        uint  operandIndex;
        OperandCheck check;
        // The number of operands of the instruction
        uint  operandsCount;
        // Register / immediate: the value, memory: the base register
        int64 value;
        // The size in bits
        uint  bits;
        // Memory: the index register (or NO_REGISTER), the scale, the
        // displacement, rip relative and the address size
        uint  index;
        uint  scale;
        int64 displacement;
        bool  isRipRelative;
        uint  addressBits;
    };

    // Decode a single instruction placed at the address 0x1000
    OpcodePtr decode(const byte* opcode, uint size)
    {
        cBuffer data(opcode, size);
        cMemoryStream stream(data);
        StreamDisassemblerPtr disassembler =
            StreamDisassemblerFactory::disassemble(
                        OpcodeSubsystems::DISASSEMBLER_AMD_64,
                        BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE),
                        true,
                        ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0x1000));
        return disassembler->next();
    }

    // The text of a decoded instruction
    cString format(const byte* opcode, uint size)
    {
        cBuffer data(opcode, size);
        cMemoryStream stream(data);
        StreamDisassemblerPtr disassembler =
            StreamDisassemblerFactory::disassemble(
                        OpcodeSubsystems::DISASSEMBLER_AMD_64,
                        BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE));
        DefaultOpcodeDataFormatter formatter(1);
        OpcodePtr instruction = disassembler->next();
        AMD64_ASSERT_EQUAL(instruction->getOpcodeSize(), size);
        OpcodeFormatterPtr fr = disassembler->getOpcodeFormat(instruction,
                                                              formatter);
        return fr->string();
    }

    // Test that the bytes are decoded to an invalid opcode (the first byte)
    void testInvalid(const byte* opcode, uint size)
    {
        OpcodePtr instruction = decode(opcode, size);
        // An invalid opcode byte (a single byte), or an entry of the table
        // which is flagged invalid (d6, C6 /1: the instruction length is the
        // one of the entry)
        const bool isByte = (instruction->getType() ==
                             OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE);
        if (!isByte && ((instruction->getAlterProperty() & Opcode::FLOW_INVALID) == 0))
        {
            // Say which bytes are valid
            cout << "AMD64 invalid test:";
            for (uint i = 0; i < size; i++)
                cout << ' ' << HEXBYTE(opcode[i]);
            cout << endl;
        }
        AMD64_ASSERT(isByte ||
                     ((instruction->getAlterProperty() & Opcode::FLOW_INVALID) != 0));
        if (isByte)
            AMD64_ASSERT_EQUAL(instruction->getOpcodeSize(), 1);
    }

    // The text of the instruction, or "(invalid)" when the bytes decode to an
    // invalid opcode, in the 16, 32 or 64 bit mode. The length of the
    // instruction is returned in size
    cString formatInMode(OpcodeSubsystems::DisassemblerType type,
                         const byte* opcode, uint& size)
    {
        cBuffer data(opcode, size);
        cMemoryStream stream(data);
        StreamDisassemblerPtr disassembler =
            StreamDisassemblerFactory::disassemble(
                        type, BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE));
        DefaultOpcodeDataFormatter formatter(1);
        OpcodePtr instruction = disassembler->next();
        size = instruction->getOpcodeSize();
        if (instruction->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE)
            return cString("(invalid)");
        OpcodeFormatterPtr fr = disassembler->getOpcodeFormat(instruction,
                                                              formatter);
        return fr->string();
    }

    // Test the structure of one operand
    void testOperand(const OperandTest& test)
    {
        OpcodePtr instruction = decode(test.opcode, test.opcodeSize);
        AMD64_ASSERT_EQUAL(instruction->getType(),
                           OpcodeSubsystems::DISASSEMBLER_AMD_64);
        AMD64_ASSERT_EQUAL(instruction->getOpcodeSize(), test.opcodeSize);
        const IA32Opcode& ia32 = *(IA32Opcode*)instruction.getPointer();
        AMD64_ASSERT_EQUAL(IA32OperandDecoder::getOperandsCount(ia32),
                           test.operandsCount);
        IA32DecodedOperand operand = IA32OperandDecoder::decode(ia32,
                                                        test.operandIndex);
        switch (test.check)
        {
        case CHECK_REGISTER:
            AMD64_ASSERT_EQUAL(operand.m_kind, IA32DecodedOperand::KIND_REGISTER);
            AMD64_ASSERT_EQUAL(operand.m_register, (uint)test.value);
            AMD64_ASSERT_EQUAL(operand.m_bits, test.bits);
            break;
        case CHECK_MEMORY:
            AMD64_ASSERT_EQUAL(operand.m_kind, IA32DecodedOperand::KIND_MEMORY);
            AMD64_ASSERT_EQUAL(operand.m_base, (uint)test.value);
            AMD64_ASSERT_EQUAL(operand.m_index, test.index);
            AMD64_ASSERT_EQUAL(operand.m_scale, test.scale);
            AMD64_ASSERT_EQUAL(operand.m_displacement, test.displacement);
            AMD64_ASSERT_EQUAL(operand.m_isRipRelative, test.isRipRelative);
            AMD64_ASSERT_EQUAL(operand.m_addressBits, test.addressBits);
            AMD64_ASSERT_EQUAL(operand.m_bits, test.bits);
            break;
        case CHECK_IMMEDIATE:
            AMD64_ASSERT((operand.m_kind == IA32DecodedOperand::KIND_IMMEDIATE) ||
                              (operand.m_kind == IA32DecodedOperand::KIND_BRANCH_TARGET));
            AMD64_ASSERT_EQUAL(operand.m_immediate, test.value);
            AMD64_ASSERT_EQUAL(operand.m_bits, test.bits);
            break;
        }
    }

    // The text of the instructions. The immediates and the displacements
    // are written by DefaultOpcodeDataFormatter (hexadecimal with the h suffix)
    void testText()
    {
        static const TextTest gTexts[] = {
            // REX.W and the extended registers
            {{0x48, 0x89, 0xD8}, 3, "mov rax, rbx"},
            {{0x49, 0x89, 0xC7}, 3, "mov r15, rax"},
            {{0x4D, 0x89, 0xFE}, 3, "mov r14, r15"},
            {{0x41, 0x89, 0xC0}, 3, "mov r8d, eax"},
            {{0x66, 0x41, 0x89, 0xC1}, 4, "mov r9w, ax"},
            {{0x48, 0x01, 0xD8}, 3, "add rax, rbx"},
            // 66 loses to REX.W
            {{0x66, 0x48, 0x89, 0xD8}, 4, "mov rax, rbx"},
            {{0x66, 0x89, 0xD8}, 3, "mov ax, bx"},
            // spl..dil need a REX prefix, otherwise ah..bh
            {{0x40, 0x88, 0xC4}, 3, "mov spl, al"},
            {{0x88, 0xC4}, 2, "mov ah, al"},
            {{0x41, 0xB0, 0x11}, 3, "mov r8b, 11h"},
            {{0x44, 0x88, 0xFE}, 3, "mov sil, r15b"},
            // Memory: rsp / r12 need a SIB, rbp / r13 need a displacement
            {{0x8B, 0x04, 0x24}, 3, "mov eax, dword ptr [rsp]"},
            {{0x41, 0x8B, 0x04, 0x24}, 4, "mov eax, dword ptr [r12]"},
            {{0x8B, 0x45, 0x00}, 3, "mov eax, dword ptr [rbp + 00h]"},
            {{0x41, 0x8B, 0x45, 0x00}, 4, "mov eax, dword ptr [r13 + 00h]"},
            // The index 4 is r12 with REX.X, no index without it
            {{0x42, 0x8B, 0x04, 0xE5, 0x10, 0x00, 0x00, 0x00}, 8,
             "mov eax, dword ptr [r12*8 + 10h]"},
            {{0x8B, 0x04, 0xE5, 0x10, 0x00, 0x00, 0x00}, 7,
             "mov eax, dword ptr [0000000000000010h]"},
            {{0x43, 0x8B, 0x04, 0x6C}, 4, "mov eax, dword ptr [r12+r13*2]"},
            {{0x8B, 0x04, 0x88}, 3, "mov eax, dword ptr [rax+rcx*4]"},
            {{0x41, 0x8B, 0x4C, 0x2D, 0x00}, 5,
             "mov ecx, dword ptr [r13+rbp*1 + 00h]"},
            // Absolute (a sign extended disp32 in a SIB byte)
            {{0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x80}, 8,
             "mov rax, qword ptr [FFFFFFFF80000000h]"},
            // 32 bit addresses
            {{0x67, 0x8B, 0x00}, 3, "mov eax, dword ptr [eax]"},
            {{0x67, 0x41, 0x8B, 0x00}, 4, "mov eax, dword ptr [r8d]"},
            // Rip relative: the displacement is from the start of the
            // instruction in the nasm notation ($ + length + disp32)
            {{0x4C, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00}, 7, "lea r8, [rel $+07h]"},
            {{0x8B, 0x05, 0xFC, 0xFF, 0xFF, 0xFF}, 6, "mov eax, dword ptr [rel $+02h]"},
            {{0x67, 0x8B, 0x05, 0xFC, 0xFF, 0xFF, 0xFF}, 7,
             "mov eax, dword ptr [a32 rel $+03h]"},
            {{0xFF, 0x25, 0xF0, 0xFF, 0xFF, 0xFF}, 6, "jmp qword ptr [rel $-0Ah]"},
            {{0x64, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00}, 7,
             "mov eax, dword ptr fs:[rel $+17h]"},
            // 64 bit immediates and offsets
            {{0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 10,
             "mov rax, 1122334455667788h"},
            {{0xB8, 0x88, 0x77, 0x66, 0x55}, 5, "mov eax, 55667788h"},
            {{0x49, 0xBF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, 10,
             "mov r15, 8000000000000001h"},
            {{0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF}, 7, "mov rax, -00000001h"},
            {{0x48, 0x83, 0xC4, 0xF8}, 4, "add rsp, -08h"},
            {{0x48, 0x05, 0x00, 0x00, 0x00, 0x80}, 6, "add rax, -80000000h"},
            {{0x48, 0xA1, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 10,
             "mov rax, [qword 1122334455667788h]"},
            {{0xA2, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 9,
             "mov [qword 1122334455667788h], al"},
            {{0x67, 0xA1, 0x78, 0x56, 0x34, 0x12}, 6,
             "mov eax, [a32 12345678h]"},
            {{0x68, 0xFF, 0xFF, 0xFF, 0xFF}, 5, "push -00000001h"},
            {{0x66, 0x68, 0x34, 0x12}, 4, "push word 1234h"},
            // Branches: rel32 sign extended, 66 and REX.W are ignored
            {{0xE8, 0xFB, 0xFF, 0xFF, 0xFF}, 5, "call $+00h"},
            {{0x66, 0xE8, 0x10, 0x00, 0x00, 0x00}, 6, "call $+16h"},
            {{0x48, 0xE8, 0xFC, 0xFF, 0xFF, 0xFF}, 6, "call $+02h"},
            {{0x0F, 0x84, 0x00, 0x00, 0x00, 0x00}, 6, "jz near $+06h"},
            {{0xEB, 0x02}, 2, "jmp $+04h"},
            {{0xE3, 0xFE}, 2, "jrcxz $+00h"},
            {{0x67, 0xE3, 0xFE}, 3, "jecxz $+01h"},
            {{0xE2, 0xFE}, 2, "loop $+00h"},
            {{0x67, 0xE2, 0xFE}, 3, "loop $+01h, ecx"},
            // Stack instructions default to 64 bits
            {{0x50}, 1, "push rax"},
            {{0x41, 0x50}, 2, "push r8"},
            {{0x66, 0x50}, 2, "push ax"},
            {{0x66, 0x48, 0x50}, 3, "push rax"},
            {{0x66, 0x48, 0x9C}, 3, "pushfq "},
            {{0x58}, 1, "pop rax"},
            {{0x41, 0x5F}, 2, "pop r15"},
            {{0xFF, 0x30}, 2, "push qword ptr [rax]"},
            {{0x41, 0xFF, 0xD0}, 3, "call r8"},
            {{0xFF, 0xD0}, 2, "call rax"},
            {{0x9C}, 1, "pushfq "},
            {{0x66, 0x9C}, 2, "pushfw "},
            {{0xC9}, 1, "leave "},
            {{0x66, 0xC9}, 2, "leavew "},
            {{0xC3}, 1, "ret "},
            {{0x66, 0xC3}, 2, "retw "},
            {{0x48, 0xCF}, 2, "iretq "},
            {{0xCF}, 1, "iret "},
            {{0x48, 0xCB}, 2, "retfq "},
            {{0x0F, 0xA0}, 2, "push fs"},
            // The operand size names
            {{0x48, 0x98}, 2, "cdqe "},
            {{0x98}, 1, "cwde "},
            {{0x66, 0x98}, 2, "cbw "},
            {{0x48, 0x99}, 2, "cqo "},
            {{0x48, 0xA5}, 2, "movsq "},
            {{0xA5}, 1, "movsd "},
            {{0x66, 0xA5}, 2, "movsw "},
            {{0x48, 0xAB}, 2, "stosq "},
            {{0x67, 0xA4}, 2, "a32 movsb "},
            // xchg / nop
            {{0x90}, 1, "nop "},
            {{0x48, 0x90}, 2, "xchg rax, rax"},
            {{0x41, 0x90}, 2, "xchg eax, r8d"},
            {{0x49, 0x90}, 2, "xchg rax, r8"},
            // pause is F3 90 only: REX.B / REX.W select xchg
            {{0xF3, 0x90}, 2, "pause "},
            {{0xF3, 0x48, 0x90}, 3, "rep xchg rax, rax"},
            {{0xF3, 0x41, 0x90}, 3, "rep xchg eax, r8d"},
            {{0x66, 0xF3, 0x90}, 3, "rep xchg ax, ax"},
            // The baseline instructions. In 64 bit mode REX.W gives the 64 bit
            // operands, a 32 bit destination is a 32 bit register
            {{0x0F, 0x44, 0xC1}, 3, "cmovz eax, ecx"},
            {{0x48, 0x0F, 0x4F, 0xC1}, 4, "cmovg rax, rcx"},
            {{0x4D, 0x0F, 0x42, 0x01}, 4, "cmovb r8, qword ptr [r9]"},
            {{0x66, 0x44, 0x0F, 0x45, 0xCA}, 5, "cmovnz r9w, dx"},
            {{0x0F, 0x0B}, 2, "ud2 "},
            {{0x48, 0x0F, 0xB9, 0xC1}, 4, "ud1 rax, rcx"},
            {{0x44, 0x0F, 0xFF, 0x01}, 4, "ud0 r8d, dword ptr [rcx]"},
            {{0xF1}, 1, "int1 "},
            {{0xF3, 0x0F, 0x1E, 0xFB}, 4, "endbr32 "},
            {{0xF3, 0x0F, 0x1E, 0xFA}, 4, "endbr64 "},
            {{0xF3, 0x48, 0x0F, 0xB8, 0xC1}, 5, "popcnt rax, rcx"},
            {{0xF3, 0x0F, 0xB8, 0xC1}, 4, "popcnt eax, ecx"},
            {{0x66, 0xF3, 0x45, 0x0F, 0xB8, 0xCA}, 6, "popcnt r9w, r10w"},
            {{0xF3, 0x4D, 0x0F, 0xBD, 0xC2}, 5, "lzcnt r8, r10"},
            {{0xF3, 0x48, 0x0F, 0xBC, 0x01}, 5, "tzcnt rax, qword ptr [rcx]"},
            {{0x48, 0x0F, 0xBC, 0xC1}, 4, "bsf rax, rcx"},
            {{0x48, 0x0F, 0x38, 0xF0, 0x06}, 5, "movbe rax, qword ptr [rsi]"},
            {{0x45, 0x0F, 0x38, 0xF1, 0x08}, 5, "movbe dword ptr [r8], r9d"},
            {{0xF2, 0x48, 0x0F, 0x38, 0xF1, 0xC1}, 6, "crc32 rax, rcx"},
            {{0xF2, 0x49, 0x0F, 0x38, 0xF0, 0xC1}, 6, "crc32 rax, r9b"},
            {{0xF2, 0x41, 0x0F, 0x38, 0xF1, 0xC1}, 6, "crc32 eax, r9d"},
            {{0x0F, 0x0D, 0x08}, 3, "prefetchw byte ptr [rax]"},
            // 64 bit only instructions
            {{0x48, 0x63, 0xC1}, 3, "movsxd rax, ecx"},
            {{0x63, 0xC1}, 2, "movsxd eax, ecx"},
            {{0x4D, 0x63, 0xC1}, 3, "movsxd r8, r9d"},
            {{0x0F, 0x05}, 2, "syscall "},
            {{0x0F, 0x07}, 2, "sysret "},
            {{0x0F, 0x01, 0xF8}, 3, "swapgs "},
            {{0x48, 0x0F, 0xC7, 0x0F}, 4, "cmpxchg16b oword ptr [rdi]"},
            {{0x0F, 0xC7, 0x0F}, 3, "cmpxchg8b qword ptr [rdi]"},
            {{0x0F, 0x20, 0xC0}, 3, "mov rax, cr0"},
            {{0x44, 0x0F, 0x20, 0xC0}, 4, "mov rax, cr8"},
            {{0x45, 0x0F, 0x22, 0xC7}, 4, "mov cr8, r15"},
            {{0x0F, 0x23, 0xF8}, 3, "mov dr7, rax"},
            // The mod field of mov cr / dr is ignored (the register form, the
            // SIB and the displacement are not read)
            {{0x0F, 0x20, 0x00}, 3, "mov rax, cr0"},
            {{0x0F, 0x22, 0x58, 0x00}, 3, "mov cr3, rax"},
            {{0x0F, 0x21, 0x8B}, 3, "mov rbx, dr1"},
            {{0x66, 0x0F, 0x20, 0xC0}, 4, "mov rax, cr0"},
            {{0x44, 0x0F, 0x22, 0x07}, 4, "mov cr8, rdi"},
            // pextrw: the SSE4.1 form with the memory destination and the
            // register form of 66 0F 3A 15
            {{0x66, 0x0F, 0x3A, 0x15, 0x00, 0x05}, 6, "pextrw word ptr [rax], xmm0, 05h"},
            {{0x66, 0x0F, 0x3A, 0x15, 0xC0, 0x05}, 6, "pextrw eax, xmm0, 05h"},
            // bswap with 66 REX.W is the 64 bit form
            {{0x66, 0x48, 0x0F, 0xC8}, 4, "bswap rax"},
            // The memory operand forms
            {{0x8D, 0x00}, 2, "lea eax, [rax]"},
            {{0x8F, 0x00}, 2, "pop qword ptr [rax]"},
            {{0x8F, 0xC0}, 2, "pop rax"},
            {{0x0F, 0x01, 0x38}, 3, "invlpg byte ptr [rax]"},
            {{0xFF, 0x18}, 2, "call far dword ptr [rax]"},
            {{0x0F, 0xB2, 0x00}, 3, "lss eax, dword ptr [rax]"},
            {{0x0F, 0x01, 0x10}, 3, "lgdt [rax]"},
            {{0x48, 0x0F, 0xAE, 0x00}, 4, "fxsave64 [rax]"},
            {{0x0F, 0xAE, 0x00}, 3, "fxsave [rax]"},
            // SSE with the general purpose registers
            {{0x66, 0x48, 0x0F, 0x6E, 0xC0}, 5, "movq xmm0, rax"},
            {{0x66, 0x0F, 0x6E, 0xC0}, 4, "movd xmm0, eax"},
            {{0x66, 0x48, 0x0F, 0x7E, 0xC0}, 5, "movq rax, xmm0"},
            {{0x66, 0x4D, 0x0F, 0x6E, 0xC7}, 5, "movq xmm8, r15"},
            {{0xF3, 0x48, 0x0F, 0x2A, 0xC1}, 5, "cvtsi2ss xmm0, rcx"},
            {{0xF2, 0x4C, 0x0F, 0x2D, 0xC1}, 5, "cvtsd2si r8, xmm1"},
            {{0xF3, 0x0F, 0x2C, 0xC1}, 4, "cvttss2si eax, xmm1"},
            {{0x48, 0x0F, 0x50, 0xC1}, 4, "movmskps rax, xmm1"},
            {{0xF3, 0x44, 0x0F, 0x7E, 0xD1}, 5, "movq xmm10, xmm1"},
            {{0x66, 0x44, 0x0F, 0xEF, 0xC1}, 5, "pxor xmm8, xmm1"},
            {{0x66, 0x45, 0x0F, 0xEF, 0xFF}, 5, "pxor xmm15, xmm15"},
            {{0x48, 0x0F, 0xC3, 0x08}, 4, "movnti qword ptr [rax], rcx"},
            // REX prefixes
            {{0x40, 0xFF, 0xC0}, 3, "inc eax"},
            {{0x48, 0x48, 0x89, 0xC0}, 4, "mov rax, rax"},
            {{0x41, 0x48, 0xB9, 0x11, 0x22, 0x33, 0x44}, 7, "mov r9d, 44332211h"},
            {{0x48, 0x41, 0xB9, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 11,
             "mov rcx, 1122334455667788h"},
            // The FPU
            {{0xD9, 0x00}, 2, "fld dword ptr [rax]"},
            {{0x48, 0xD8, 0x45, 0x49}, 4, "fadd dword ptr [rbp + 49h]"},
            {{0x41, 0xDD, 0x00}, 3, "fld qword ptr [r8]"},
            {{0xD9, 0xE8}, 2, "fld1 "},
            {{0xDF, 0xE0}, 2, "fnstsw ax"},
            {{0x0F, 0xB6, 0xC4}, 3, "movzx eax, ah"},
            {{0x40, 0x0F, 0xB6, 0xC4}, 4, "movzx eax, spl"},
            {{0x4C, 0x0F, 0xBE, 0xC1}, 4, "movsx r8, cl"},
            {{0x0F, 0xCF}, 2, "bswap edi"},
            {{0x49, 0x0F, 0xC8}, 3, "bswap r8"},
            {{0xCC}, 1, "int3 "},
            {{0xF4}, 1, "hlt "},
            {{0}, 0, NULL}
        };

        for (const TextTest* test = gTexts; test->text != NULL; test++)
        {
            cString text = format(test->opcode, test->opcodeSize);
            if (text != cString(test->text))
            {
                // Say which instruction failed
                cout << "AMD64 text test:";
                for (uint i = 0; i < test->opcodeSize; i++)
                    cout << ' ' << HEXBYTE(test->opcode[i]);
                cout << endl << "  expected: " << test->text
                     << endl << "  actual:   " << text << endl;
            }
            AMD64_ASSERT_EQUAL(text, cString(test->text));
        }
    }

    // The opcodes which are not valid in 64 bit mode are invalid
    void testInvalidOpcodes()
    {
        static const byte gInvalid[][8] = {
            // push / pop of es, ss, ds, cs
            {0x06}, {0x07}, {0x0E}, {0x16}, {0x17}, {0x1E}, {0x1F},
            // daa, das, aaa, aas
            {0x27}, {0x2F}, {0x37}, {0x3F},
            // pusha, popa, bound, (arpl is movsxd)
            {0x60}, {0x61}, {0x62, 0xC0},
            // 82 /r, call and jmp far ptr16:32
            {0x82, 0xC0, 0x01}, {0x9A, 0x01, 0x02, 0x03}, {0xEA, 0x01, 0x02, 0x03},
            // les, lds (the VEX prefixes), into, aam, aad
            {0xC4, 0xC0}, {0xC5, 0xC0}, {0xCE}, {0xD4, 0x0A}, {0xD5, 0x0A},
            // salc
            {0xD6},
            // The forms with a register of the opcodes which need memory:
            // lea, lss / lfs / lgs, movnti, the far call and jmp, invlpg,
            // sgdt / sidt / lgdt / lidt
            {0x8D, 0xC0}, {0x0F, 0xB2, 0xC0}, {0x0F, 0xB4, 0xC0}, {0x0F, 0xB5, 0xC0},
            {0x0F, 0xC3, 0xC0}, {0xFF, 0xD8}, {0xFF, 0xE8}, {0x0F, 0x01, 0xF9},
            {0x0F, 0x01, 0xC2}, {0x0F, 0x01, 0xCA}, {0x0F, 0x01, 0xD2}, {0x0F, 0x01, 0xDA},
            // cmpxchg8b / cmpxchg16b, clflush, fxsave, ldmxcsr with a register
            {0x0F, 0xC7, 0xC8}, {0x0F, 0xAE, 0xC0}, {0x0F, 0xAE, 0xD0}, {0x0F, 0xAE, 0xF9},
            // 8F and C6 / C7 are valid for /0 only
            {0x8F, 0xF0}, {0x8F, 0x30}, {0x8F, 0xC8}, {0xC6, 0xC8, 0x00}, {0xC7, 0xF8, 0x00, 0x00, 0x00},
            // FE /2.. and FF /7
            {0xFE, 0xD0}, {0xFF, 0xF8}, {0xFF, 0x38},
            // movntps, movntdq and the movlps forms with a register, movhlps
            // with memory, the shift groups with memory, pmovmskb with memory
            {0x0F, 0x2B, 0xC0}, {0x66, 0x0F, 0xE7, 0xC0}, {0x0F, 0xE7, 0xC0},
            {0x0F, 0x13, 0xC0}, {0x0F, 0x50, 0x00}, {0x0F, 0xD7, 0x00},
            {0x66, 0x0F, 0x71, 0x00, 0x00}, {0x0F, 0x72, 0x00, 0x00},
            // The reserved control registers (cr1, cr5), the segment registers
            {0x0F, 0x20, 0xC8}, {0x0F, 0x22, 0xE8}, {0x8C, 0xF0}, {0x8E, 0xF0},
            // bswap with a 16 bit operand
            {0x66, 0x0F, 0xC8},
            // salc, and the endbr / movbe with a register, are not instructions in 64 bit mode
            {0xD6}, {0xF3, 0x0F, 0x1E, 0xFC}, {0x0F, 0x38, 0xF0, 0xC1},
            // The reserved NOPs
            {0x0F, 0x19, 0x00}, {0x0F, 0x18, 0x20}, {0x0F, 0x18, 0xC0},
            // The VEX prefix (vzeroupper)
            {0xC5, 0xF8, 0x77}
        };
        for (uint i = 0; i < sizeof(gInvalid) / sizeof(gInvalid[0]); i++)
            testInvalid(gInvalid[i], sizeof(gInvalid[i]));

        // A REX prefix which is followed by a legacy prefix is an instruction
        // by itself
        static const byte gRexPrefix[] = {0x48, 0x66, 0x89, 0xC0};
        testInvalid(gRexPrefix, sizeof(gRexPrefix));

        // The same opcodes are valid in 32 bit mode
        static const byte gDaa[] = {0x27};
        cBuffer data(gDaa, sizeof(gDaa));
        cMemoryStream stream(data);
        StreamDisassemblerPtr disassembler =
            StreamDisassemblerFactory::disassemble(
                        OpcodeSubsystems::DISASSEMBLER_INTEL_32,
                        BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE));
        OpcodePtr instruction = disassembler->next();
        AMD64_ASSERT_EQUAL(instruction->getType(),
                           OpcodeSubsystems::DISASSEMBLER_INTEL_32);
    }

    // The bytes of a validity test of the 16 and 32 bit modes: the text, or
    // "(invalid)", and the length
    struct ModeTest {
        const byte opcode[MAX_AMD64_OPCODE];
        uint  opcodeSize;
        const char* text16;
        const char* text32;
        uint  length;
    };

    // The memory and register forms, the undefined /digit and the ignored mod
    // field of mov cr / dr, in the 16 and 32 bit modes
    void testValidityInModes()
    {
        static const ModeTest gTests[] = {
            // lea and the far pointers need a memory operand
            {{0x8D, 0xD9}, 2, "(invalid)", "(invalid)", 1},
            {{0x8D, 0x07}, 2, "lea ax, [bx]", "lea eax, [edi]", 2},
            {{0xC5, 0xC0}, 2, "(invalid)", "(invalid)", 1},
            {{0xC4, 0xC0}, 2, "(invalid)", "(invalid)", 1},
            {{0x62, 0xC0}, 2, "(invalid)", "(invalid)", 1},
            {{0x0F, 0xB2, 0xC0}, 3, "(invalid)", "(invalid)", 1},
            {{0xFF, 0xEA}, 2, "(invalid)", "(invalid)", 1},
            {{0xFF, 0xDA}, 2, "(invalid)", "(invalid)", 1},
            // The descriptor tables: memory only; the other mod 11 encodings
            // of 0F 01 are not instructions of the table
            {{0x0F, 0x01, 0xC2}, 3, "(invalid)", "(invalid)", 1},
            {{0x0F, 0x01, 0xD0}, 3, "(invalid)", "(invalid)", 1},
            {{0x0F, 0x01, 0xF8}, 3, "(invalid)", "(invalid)", 1},
            {{0x0F, 0x01, 0x10}, 3, "lgdt word ptr [bx+si]", "lgdt dword ptr [eax]", 3},
            // pop r/m is 8F /0
            {{0x8F, 0xF0}, 2, "(invalid)", "(invalid)", 1},
            {{0x8F, 0x30}, 2, "(invalid)", "(invalid)", 1},
            {{0x8F, 0xC0}, 2, "pop ax", "pop eax", 2},
            // The ModR/M mod field of mov cr / dr is ignored, the register
            // is the 32 bit register
            {{0x0F, 0x20, 0x00}, 3, "mov eax, cr0", "mov eax, cr0", 3},
            {{0x0F, 0x20, 0xC0}, 3, "mov eax, cr0", "mov eax, cr0", 3},
            {{0x0F, 0x22, 0x58, 0x7F}, 3, "mov cr3, eax", "mov cr3, eax", 3},
            {{0x0F, 0x21, 0x8B, 0x01, 0x02, 0x03, 0x04}, 7, "mov ebx, dr1", "mov ebx, dr1", 3},
            {{0x66, 0x0F, 0x20, 0xC0}, 4, "mov eax, cr0", "mov eax, cr0", 4},
            {{0x0F, 0x20, 0xC8}, 3, "(invalid)", "(invalid)", 1},
            {{0x0F, 0x22, 0xE8}, 3, "(invalid)", "(invalid)", 1},
            // bswap has no 16 bit form
            {{0x0F, 0xC8}, 2, "(invalid)", "bswap eax", 2},
            {{0x66, 0x0F, 0xC8}, 3, "bswap eax", "(invalid)", 3},
            // Segment registers
            {{0x8C, 0xF0}, 2, "(invalid)", "(invalid)", 1},
            {{0x8E, 0xE8}, 2, "mov gs, ax", "mov gs, ax", 2},
            // The memory-only SSE and the register-only SSE forms
            {{0x0F, 0x2B, 0xC0}, 3, "(invalid)", "(invalid)", 1},
            {{0x0F, 0x50, 0x00}, 3, "(invalid)", "(invalid)", 1},
            {{0x0F, 0x12, 0xC1}, 3, "movhlps xmm0, xmm1", "movhlps xmm0, xmm1", 3},
            {{0x0F, 0x12, 0x01}, 3, "movlps xmm0, qword ptr [bx+di]", "movlps xmm0, qword ptr [ecx]", 3},
            // The reserved NOPs
            {{0x0F, 0x19, 0x00}, 3, "(invalid)", "(invalid)", 1},
            {{0x0F, 0x1F, 0x00}, 3, "nop word ptr [bx+si]", "nop dword ptr [eax]", 3},
            // The memory operand of the FPU and the general purpose operand of
            // movd / cvtsi2sd / movnti have their own size: neither the 66
            // prefix nor the 16 bit mode change it
            {{0xD8, 0x00}, 2, "fadd dword ptr [bx+si]", "fadd dword ptr [eax]", 2},
            {{0x66, 0xD9, 0x00}, 3, "fld dword ptr [bx+si]", "fld dword ptr [eax]", 3},
            {{0xDA, 0x00}, 2, "fiadd dword ptr [bx+si]", "fiadd dword ptr [eax]", 2},
            {{0x0F, 0xAE, 0x1F}, 3, "stmxcsr dword ptr [bx]", "stmxcsr dword ptr [edi]", 3},
            {{0x0F, 0x6E, 0xC0}, 3, "movd mm0, eax", "movd mm0, eax", 3},
            {{0x66, 0x0F, 0x6E, 0xC0}, 4, "movd xmm0, eax", "movd xmm0, eax", 4},
            {{0x66, 0x0F, 0x7E, 0xC0}, 4, "movd eax, xmm0", "movd eax, xmm0", 4},
            {{0xF2, 0x0F, 0x2A, 0xC0}, 4, "cvtsi2sd xmm0, eax", "cvtsi2sd xmm0, eax", 4},
            {{0xF3, 0x0F, 0x2A, 0x00}, 4, "cvtsi2ss xmm0, dword ptr [bx+si]",
             "cvtsi2ss xmm0, dword ptr [eax]", 4},
            {{0x0F, 0xC3, 0x00}, 3, "movnti dword ptr [bx+si], eax",
             "movnti dword ptr [eax], eax", 3},
            {{0x0F, 0x02, 0x00}, 3, "lar ax, word ptr [bx+si]", "lar eax, word ptr [eax]", 3},
            // pextrw
            {{0x66, 0x0F, 0x3A, 0x15, 0x00, 0x05}, 6, "pextrw word ptr [bx+si], xmm0, 05h",
             "pextrw word ptr [eax], xmm0, 05h", 6},
            // push imm8 with the operand size which isn't the default of the
            // mode
            {{0x66, 0x6A, 0x12}, 3, "push dword 12h", "push word 12h", 3},
            {{0, 0}, 0, NULL, NULL, 0}
        };

        const OpcodeSubsystems::DisassemblerType types[2] = {
            OpcodeSubsystems::DISASSEMBLER_INTEL_16,
            OpcodeSubsystems::DISASSEMBLER_INTEL_32 };
        for (const ModeTest* test = gTests; test->text16 != NULL; test++)
        {
            for (uint mode = 0; mode < 2; mode++)
            {
                uint size = test->opcodeSize;
                cString text = formatInMode(types[mode], test->opcode, size);
                const char* expected = (mode == 0) ? test->text16 : test->text32;
                if (text != cString(expected))
                {
                    cout << "Validity test (" << (mode == 0 ? 16 : 32) << " bit):";
                    for (uint i = 0; i < test->opcodeSize; i++)
                        cout << ' ' << HEXBYTE(test->opcode[i]);
                    cout << endl << "  expected: " << expected << " (" << test->length
                         << ")" << endl << "  actual:   " << text << " (" << size
                         << ")" << endl;
                }
                AMD64_ASSERT_EQUAL(text, cString(expected));
                // The length of an invalid opcode is the one of the entry
                if (cString(expected) != cString("(invalid)"))
                    AMD64_ASSERT_EQUAL(size, test->length);
            }
        }
    }

    // The structured operands
    void testOperands()
    {
        static const OperandTest gOperands[] = {
            // mov r15, rax: the registers with REX.B / REX.R
            {{0x49, 0x89, 0xC7}, 3, 0, CHECK_REGISTER, 2, AMD64_REG_R15, 64, 0, 0, 0, false, 0},
            {{0x49, 0x89, 0xC7}, 3, 1, CHECK_REGISTER, 2, AMD64_REG_RAX, 64, 0, 0, 0, false, 0},
            // mov r8d, eax / r9w / r10b
            {{0x41, 0x89, 0xC0}, 3, 0, CHECK_REGISTER, 2, AMD64_REG_R8D, 32, 0, 0, 0, false, 0},
            {{0x66, 0x41, 0x89, 0xC1}, 4, 0, CHECK_REGISTER, 2, AMD64_REG_R9W, 16, 0, 0, 0, false, 0},
            {{0x41, 0x88, 0xC2}, 3, 0, CHECK_REGISTER, 2, AMD64_REG_R10B, 8, 0, 0, 0, false, 0},
            // spl with a REX prefix, ah without
            {{0x40, 0x88, 0xC4}, 3, 0, CHECK_REGISTER, 2, AMD64_REG_SPL, 8, 0, 0, 0, false, 0},
            {{0x88, 0xC4}, 2, 0, CHECK_REGISTER, 2, IA32_REG_AH, 8, 0, 0, 0, false, 0},
            // xmm9 and cr8
            {{0x66, 0x44, 0x0F, 0xEF, 0xC9}, 5, 0, CHECK_REGISTER, 2, AMD64_REG_XMM8 + 1, 128, 0, 0, 0, false, 0},
            {{0x44, 0x0F, 0x22, 0xC0}, 4, 0, CHECK_REGISTER, 2, AMD64_REG_CR8, 64, 0, 0, 0, false, 0},
            // movq xmm0, rax: the general register is 64 bit
            {{0x66, 0x48, 0x0F, 0x6E, 0xC0}, 5, 1, CHECK_REGISTER, 2, AMD64_REG_RAX, 64, 0, 0, 0, false, 0},
            // mov rax, [r12*8 + 0x10]: no base, the index r12, the scale 8
            {{0x4A, 0x8B, 0x04, 0xE5, 0x10, 0x00, 0x00, 0x00}, 8, 1, CHECK_MEMORY, 2,
             DECODER_NO_REGISTER, 64, AMD64_REG_R12, 8, 0x10, false, 64},
            // mov eax, [r13 + rbp*1 + 0]
            {{0x41, 0x8B, 0x44, 0x2D, 0x00}, 5, 1, CHECK_MEMORY, 2,
             AMD64_REG_R13, 32, AMD64_REG_RBP, 1, 0, false, 64},
            // mov eax, [rsp]: no index
            {{0x8B, 0x04, 0x24}, 3, 1, CHECK_MEMORY, 2,
             AMD64_REG_RSP, 32, DECODER_NO_REGISTER, 1, 0, false, 64},
            // mov eax, [0x10]: SIB without base and index
            {{0x8B, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00}, 7, 1, CHECK_MEMORY, 2,
             DECODER_NO_REGISTER, 32, DECODER_NO_REGISTER, 1, 0x10, false, 64},
            // mov rax, [-0x80000000]: the disp32 is sign extended
            {{0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x80}, 8, 1, CHECK_MEMORY, 2,
             DECODER_NO_REGISTER, 64, DECODER_NO_REGISTER, 1, -0x80000000LL, false, 64},
            // mov eax, [rip - 4]
            {{0x8B, 0x05, 0xFC, 0xFF, 0xFF, 0xFF}, 6, 1, CHECK_MEMORY, 2,
             DECODER_NO_REGISTER, 32, DECODER_NO_REGISTER, 1, -4, true, 64},
            // mov eax, [eip + 0x10] and [r8d + ecx*2 - 8]
            {{0x67, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00}, 7, 1, CHECK_MEMORY, 2,
             DECODER_NO_REGISTER, 32, DECODER_NO_REGISTER, 1, 0x10, true, 32},
            {{0x67, 0x41, 0x8B, 0x44, 0x48, 0xF8}, 6, 1, CHECK_MEMORY, 2,
             AMD64_REG_R8D, 32, IA32_REG_ECX, 2, -8, false, 32},
            // mov rax, [moffs64]
            {{0x48, 0xA1, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 10, 1,
             CHECK_MEMORY, 2, DECODER_NO_REGISTER, 64, DECODER_NO_REGISTER, 1,
             0x1122334455667788LL, false, 64},
            // mov al, [moffs32] (67)
            {{0x67, 0xA0, 0x78, 0x56, 0x34, 0x12}, 6, 1, CHECK_MEMORY, 2,
             DECODER_NO_REGISTER, 8, DECODER_NO_REGISTER, 1, 0x12345678, false, 32},
            // mov rax, imm64
            {{0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 10, 1,
             CHECK_IMMEDIATE, 2, 0x1122334455667788LL, 64, 0, 0, 0, false, 0},
            // mov eax, imm32 (B8 without REX.W)
            {{0xB8, 0x88, 0x77, 0x66, 0x55}, 5, 1,
             CHECK_IMMEDIATE, 2, 0x55667788LL, 32, 0, 0, 0, false, 0},
            // mov rax, imm32 sign extended
            {{0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF}, 7, 1,
             CHECK_IMMEDIATE, 2, -1, 64, 0, 0, 0, false, 0},
            // call rel32 at the address 0x1000: the target is 0x1000 + 5 - 5
            {{0xE8, 0xFB, 0xFF, 0xFF, 0xFF}, 5, 0,
             CHECK_IMMEDIATE, 1, 0x1000, 64, 0, 0, 0, false, 0},
            // jz rel8 at 0x1000
            {{0x74, 0x10}, 2, 0,
             CHECK_IMMEDIATE, 1, 0x1012, 64, 0, 0, 0, false, 0},
            {{0, 0, 0}, 0, 0, CHECK_REGISTER, 0, 0, 0, 0, 0, 0, false, 0}
        };

        m_current = 0;
        for (const OperandTest* test = gOperands; test->opcodeSize != 0; test++)
        {
            testOperand(*test);
            m_current++;
        }
    }

    // The instruction flags
    void testInstructionProperties()
    {
        static const byte gRipRelative[] = {0x8B, 0x05, 0x10, 0x00, 0x00, 0x00};
        OpcodePtr instruction = decode(gRipRelative, sizeof(gRipRelative));
        IA32Opcode& ia32 = *(IA32Opcode*)instruction.getPointer();
        AMD64_ASSERT(ia32.isRipRelative());
        AMD64_ASSERT(!ia32.hasRex());
        AMD64_ASSERT_EQUAL(IA32OperandDecoder::getNextAddress(ia32), 0x1006);
        AMD64_ASSERT_EQUAL(IA32OperandDecoder::getOperandBits(ia32), 32);
        AMD64_ASSERT_EQUAL(IA32OperandDecoder::getAddressBits(ia32), 64);
        AMD64_ASSERT(IA32OperandDecoder::is64BitMode(ia32));

        // Not rip relative: mod 00 rm 100 (SIB) base 101
        static const byte gAbsolute[] = {0x8B, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00};
        instruction = decode(gAbsolute, sizeof(gAbsolute));
        AMD64_ASSERT(!((IA32Opcode*)instruction.getPointer())->isRipRelative());

        // The REX.W prefix: 64 bit operand size, the REX prefix is kept
        static const byte gRexW[] = {0x48, 0x89, 0xD8};
        instruction = decode(gRexW, sizeof(gRexW));
        IA32Opcode& rex = *(IA32Opcode*)instruction.getPointer();
        AMD64_ASSERT(rex.hasRex());
        AMD64_ASSERT_EQUAL(rex.getRex().m_packed, 0x48);
        AMD64_ASSERT_EQUAL(IA32OperandDecoder::getOperandBits(rex), 64);
        AMD64_ASSERT_EQUAL(rex.getType(), OpcodeSubsystems::DISASSEMBLER_AMD_64);

        // A branch
        static const byte gCall[] = {0xE8, 0x00, 0x00, 0x00, 0x00};
        instruction = decode(gCall, sizeof(gCall));
        AMD64_ASSERT(instruction->isBranch());
        AMD64_ASSERT_EQUAL(IA32OperandDecoder::getOperandBits(
                        *(IA32Opcode*)instruction.getPointer()), 64);
    }

    // Test the 64 bit disassembler
    virtual void test()
    {
        m_current = 0;
        testText();
        testInvalidOpcodes();
        testValidityInModes();
        testOperands();
        testInstructionProperties();
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    // The index of the operand test which runs
    uint m_current;

    // The m_base / m_index of a memory operand which has none
    enum { DECODER_NO_REGISTER = IA32DecodedOperand::NO_REGISTER };
};

// Instance test object
TestObjectTestAMD64Disassembler g_globalTestAMD64Disassembler;
