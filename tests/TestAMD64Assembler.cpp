/*
 * TestAMD64Assembler.cpp
 *
 * Tests the 64 bit mode of the IA-32 assembler: every instruction of the table
 * is assembled in 64 bit mode, the bytes are the ones nasm writes (nasm 3.02,
 * 'bits 64'), and the disassembler decodes the bytes back to the text of the
 * table. The text which are invalid in 64 bit mode, or in the 32 bit mode
 * (the 64 bit instructions), must be rejected.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/datastream.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/exceptions.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/stream/ioStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/DefaultOpcodeDataFormatter.h"

#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblerInterface.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"

class TestObjectTestAMD64Assembler : public cTestObject {
public:
    // Max-opcode size
    #define MAX_AMD64_ASM_OPCODE (16)

    // The instruction text, the bytes nasm writes for it, and the text of the
    // disassembler for the bytes (the hex numbers of the default formatter)
    struct SingleOpcodeTest {
        const char* instructionText;
        const byte opcode[MAX_AMD64_ASM_OPCODE];
        uint  opcodeSize;
        const char* instructionTextHexSuffix;
    };

    class TempClassInterface : public StackInterface {
    public:
        TempClassInterface(FirstPassBinaryPtr& binary) :
            StackInterface(*binary, true, 4)
        {
        };
        RegisterAllocationTable gNull;
        virtual uint getStackType() { return 4; };
        virtual uint getRegisterSize() { return 4; };
        virtual RegisterAllocationTable& getRegistersTable() { return gNull; };
        virtual const RegisterAllocationTable& getRegistersTable()const { return gNull; };
        virtual StackLocation replaceRegisterToStackVariable(StackLocation source) { return source; };
    };

    // Assemble the text into a basic block. The IA32AssemblerStream is used
    // directly so that the errors are raised here, and not swallowed by the
    // destructor of the stream
    cBuffer assemble(const char* data, IA32eInstructionSet::DisassemblerTypes mode)
    {
        FirstPassBinaryPtr main(new FirstPassBinary(
                    OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
        main->createNewBlockWithoutChange(0,
            StackInterfacePtr(new TempClassInterface(main)));
        main->changeBasicBlock(0);
        {
            IA32AssemblerStream stream(*main, mode);
            stream << data << endl;
            stream.assemble();
        }
        return main->getCurrentBlockData();
    }

    /*
     * Test a single instruction by assembling it and disassembling the bytes
     */
    void testInstruction(const char* data, const byte* originalOpcode,
                         const uint size, const char* dataHexSuffix)
    {
        cBuffer instruction = assemble(data, IA32eInstructionSet::AMD_64);
        cout << data << " -> ";
        for (uint i = 0; i < instruction.getSize(); i++)
            cout << HEXBYTE(instruction[i]) << ' ';
        cout << endl;
        TESTS_ASSERT_EQUAL(instruction.getSize(), size);
        for (uint i = 0; i < size; i++)
            TESTS_ASSERT_EQUAL(instruction[i], originalOpcode[i]);

        // And test the disassembler
        cMemoryStream disstream(instruction);
        StreamDisassemblerPtr disassembler =
            StreamDisassemblerFactory::disassemble(
                        OpcodeSubsystems::DISASSEMBLER_AMD_64,
                        BasicInputPtr(&disstream, SMARTPTR_DESTRUCT_NONE));
        DefaultOpcodeDataFormatter formatter(1);
        OpcodePtr opcode = disassembler->next();
        TESTS_ASSERT_EQUAL(opcode->getOpcode(), instruction);
        OpcodeFormatterPtr fr = disassembler->getOpcodeFormat(opcode, formatter);
        cString opcodeText = fr->string();
        cout << "    " << opcodeText << endl;
        TESTS_ASSERT_EQUAL(opcodeText, dataHexSuffix);
    }

    // The factory generates the assembler of 64 bit mode, the assembled text is
    // appended to the basic block when the stream is destroyed
    void testFactory()
    {
        FirstPassBinaryPtr main(new FirstPassBinary(
                    OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
        main->createNewBlockWithoutChange(0,
            StackInterfacePtr(new TempClassInterface(main)));
        main->changeBasicBlock(0);
        AssemblerInterfacePtr amd64asm = AssemblingFactory::generateAssembler(
                    main, OpcodeSubsystems::DISASSEMBLER_AMD_64);
        (*amd64asm->getAssembler()) << "push rbp" << endl
                                    << "mov rbp, rsp" << endl
                                    << "mov rax, [rbp+0x10]" << endl
                                    << "pop rbp" << endl
                                    << "ret" << endl;
        static const byte expected[] = { 0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B,
                                         0x45, 0x10, 0x5D, 0xC3 };
        cBuffer data = amd64asm->getFirstPass()->getCurrentBlockData();
        TESTS_ASSERT_EQUAL(data.getSize(), sizeof(expected));
        for (uint i = 0; i < sizeof(expected); i++)
            TESTS_ASSERT_EQUAL(data[i], expected[i]);
    }

    // The assembler must reject the text
    void testRejected(const char* data, IA32eInstructionSet::DisassemblerTypes mode)
    {
        bool rejected = false;
        XSTL_TRY
        {
            assemble(data, mode);
        }
        XSTL_CATCH(cException&)
        {
            rejected = true;
        }
        if (!rejected)
            cout << "Accepted: " << data << endl;
        TESTS_ASSERT_EQUAL(rejected, true);
    }

    /*
     * The table: the text, the bytes of nasm, their length and the text of the
     * disassembler. The instructions of the table cover the REX registers
     * (r8-r15, spl..dil and ah..bh without REX), the immediates (imm64, the
     * sign extended imm32 and imm8), moffs64, the rip relative and the SIB
     * forms (rsp / r12, rbp / r13, no base, no index, the absolute address), the
     * 32 bit addresses (67), the instructions whose default operand size is
     * 64 bits (push, pop, call, ret, ...), cdqe / cqo / movsq / iretq / jrcxz,
     * movsxd, cmpxchg16b, swapgs, and SSE with xmm8-xmm15 and the 64 bit
     * registers.
     */
    virtual void test()
    {
        static const SingleOpcodeTest gOpcodes[] = {
            {"mov rax, rbx", {0x48, 0x89, 0xD8}, 3, "mov rax, rbx"},
            {"mov r8, r9", {0x4D, 0x89, 0xC8}, 3, "mov r8, r9"},
            {"mov r15, rax", {0x49, 0x89, 0xC7}, 3, "mov r15, rax"},
            {"mov eax, r8d", {0x44, 0x89, 0xC0}, 3, "mov eax, r8d"},
            {"mov r8d, eax", {0x41, 0x89, 0xC0}, 3, "mov r8d, eax"},
            {"mov ax, r9w", {0x66, 0x44, 0x89, 0xC8}, 4, "mov ax, r9w"},
            {"mov al, r10b", {0x44, 0x88, 0xD0}, 3, "mov al, r10b"},
            {"mov r11b, bl", {0x41, 0x88, 0xDB}, 3, "mov r11b, bl"},
            {"mov rsp, rbp", {0x48, 0x89, 0xEC}, 3, "mov rsp, rbp"},
            {"mov spl, al", {0x40, 0x88, 0xC4}, 3, "mov spl, al"},
            {"mov bpl, sil", {0x40, 0x88, 0xF5}, 3, "mov bpl, sil"},
            {"mov dil, r8b", {0x44, 0x88, 0xC7}, 3, "mov dil, r8b"},
            {"mov ah, bl", {0x88, 0xDC}, 2, "mov ah, bl"},
            {"add [rax], spl", {0x40, 0x00, 0x20}, 3, "add byte ptr [rax], spl"},
            {"add sil, [r9]", {0x41, 0x02, 0x31}, 3, "add sil, byte ptr [r9]"},
            {"mov rax, 1", {0xB8, 0x01, 0x00, 0x00, 0x00}, 5, "mov eax, 00000001h"},
            {"mov rax, -1", {0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF}, 7, "mov rax, -00000001h"},
            {"mov r8, 1", {0x41, 0xB8, 0x01, 0x00, 0x00, 0x00}, 6, "mov r8d, 00000001h"},
            {"mov eax, 1", {0xB8, 0x01, 0x00, 0x00, 0x00}, 5, "mov eax, 00000001h"},
            {"mov rax, 0x80000000", {0xB8, 0x00, 0x00, 0x00, 0x80}, 5, "mov eax, 80000000h"},
            {"mov rax, 0xffffffff80000000", {0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x80}, 7, "mov rax, -80000000h"},
            {"mov rax, 0x1122334455667788", {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 10, "mov rax, 1122334455667788h"},
            {"mov r12, 0x1122334455667788", {0x49, 0xBC, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 10, "mov r12, 1122334455667788h"},
            {"mov qword [rax], 0x7fffffff", {0x48, 0xC7, 0x00, 0xFF, 0xFF, 0xFF, 0x7F}, 7, "mov qword ptr [rax], 7FFFFFFFh"},
            {"mov qword [rax], -1", {0x48, 0xC7, 0x00, 0xFF, 0xFF, 0xFF, 0xFF}, 7, "mov qword ptr [rax], -00000001h"},
            {"add rax, 0x7f", {0x48, 0x83, 0xC0, 0x7F}, 4, "add rax, 7Fh"},
            {"add rax, 0x80", {0x48, 0x05, 0x80, 0x00, 0x00, 0x00}, 6, "add rax, 00000080h"},
            {"add rax, -0x80", {0x48, 0x83, 0xC0, 0x80}, 4, "add rax, -80h"},
            {"add qword [r8], 0x12345678", {0x49, 0x81, 0x00, 0x78, 0x56, 0x34, 0x12}, 7, "add qword ptr [r8], 12345678h"},
            {"cmp r9, -1", {0x49, 0x83, 0xF9, 0xFF}, 4, "cmp r9, -01h"},
            {"mov al, [qword 0x1122334455667788]", {0xA0, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 9, "mov al, [qword 1122334455667788h]"},
            {"mov rax, [qword 0x1122334455667788]", {0x48, 0xA1, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 10, "mov rax, [qword 1122334455667788h]"},
            {"mov [qword 0x1122334455667788], rax", {0x48, 0xA3, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 10, "mov [qword 1122334455667788h], rax"},
            {"mov [qword 0x1122334455667788], al", {0xA2, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 9, "mov [qword 1122334455667788h], al"},
            {"mov eax, [0x1234]", {0x8B, 0x04, 0x25, 0x34, 0x12, 0x00, 0x00}, 7, "mov eax, dword ptr [0000000000001234h]"},
            {"mov [0x1234], eax", {0x89, 0x04, 0x25, 0x34, 0x12, 0x00, 0x00}, 7, "mov dword ptr [0000000000001234h], eax"},
            {"mov eax, [a32 0x12345678]", {0x67, 0xA1, 0x78, 0x56, 0x34, 0x12}, 6, "mov eax, [a32 12345678h]"},
            {"mov eax, [rax]", {0x8B, 0x00}, 2, "mov eax, dword ptr [rax]"},
            {"mov eax, [rsp]", {0x8B, 0x04, 0x24}, 3, "mov eax, dword ptr [rsp]"},
            {"mov eax, [r12]", {0x41, 0x8B, 0x04, 0x24}, 4, "mov eax, dword ptr [r12]"},
            {"mov eax, [rbp]", {0x8B, 0x45, 0x00}, 3, "mov eax, dword ptr [rbp + 00h]"},
            {"mov eax, [r13]", {0x41, 0x8B, 0x45, 0x00}, 4, "mov eax, dword ptr [r13 + 00h]"},
            {"mov eax, [rbp+0x10]", {0x8B, 0x45, 0x10}, 3, "mov eax, dword ptr [rbp + 10h]"},
            {"mov eax, [r13+0x10]", {0x41, 0x8B, 0x45, 0x10}, 4, "mov eax, dword ptr [r13 + 10h]"},
            {"mov eax, [rsp+0x10]", {0x8B, 0x44, 0x24, 0x10}, 4, "mov eax, dword ptr [rsp + 10h]"},
            {"mov eax, [r12+0x1000]", {0x41, 0x8B, 0x84, 0x24, 0x00, 0x10, 0x00, 0x00}, 8, "mov eax, dword ptr [r12 + 1000h]"},
            {"mov eax, [rax+rcx*4]", {0x8B, 0x04, 0x88}, 3, "mov eax, dword ptr [rax+rcx*4]"},
            {"mov eax, [rax+rcx*4+0x10]", {0x8B, 0x44, 0x88, 0x10}, 4, "mov eax, dword ptr [rax+rcx*4 + 10h]"},
            {"mov eax, [r8+r9*8+0x12345678]", {0x43, 0x8B, 0x84, 0xC8, 0x78, 0x56, 0x34, 0x12}, 8, "mov eax, dword ptr [r8+r9*8 + 12345678h]"},
            {"mov eax, [rsp+r12]", {0x42, 0x8B, 0x04, 0x24}, 4, "mov eax, dword ptr [rsp+r12]"},
            {"mov eax, [rax*4]", {0x8B, 0x04, 0x85, 0x00, 0x00, 0x00, 0x00}, 7, "mov eax, dword ptr [rax*4 + 00h]"},
            {"mov eax, [r10*8+0x10]", {0x42, 0x8B, 0x04, 0xD5, 0x10, 0x00, 0x00, 0x00}, 8, "mov eax, dword ptr [r10*8 + 10h]"},
            {"mov eax, [rax*2]", {0x8B, 0x04, 0x00}, 3, "mov eax, dword ptr [rax+rax]"},
            {"mov eax, [rax+rax*1]", {0x8B, 0x04, 0x00}, 3, "mov eax, dword ptr [rax+rax]"},
            {"mov eax, [rax+rsp]", {0x8B, 0x04, 0x04}, 3, "mov eax, dword ptr [rsp+rax]"},
            {"mov eax, [rel $+0x10]", {0x8B, 0x05, 0x0A, 0x00, 0x00, 0x00}, 6, "mov eax, dword ptr [rel $+10h]"},
            {"mov rax, [rel $-0x10]", {0x48, 0x8B, 0x05, 0xE9, 0xFF, 0xFF, 0xFF}, 7, "mov rax, qword ptr [rel $-10h]"},
            {"mov [rel $+0x100], r9", {0x4C, 0x89, 0x0D, 0xF9, 0x00, 0x00, 0x00}, 7, "mov qword ptr [rel $+0100h], r9"},
            {"lea rax, [rel $+0x20]", {0x48, 0x8D, 0x05, 0x19, 0x00, 0x00, 0x00}, 7, "lea rax, [rel $+20h]"},
            {"mov dword [rel $+0x20], 0x12345678", {0xC7, 0x05, 0x16, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12}, 10, "mov dword ptr [rel $+20h], 12345678h"},
            {"cmp byte [rel $+0x20], 5", {0x80, 0x3D, 0x19, 0x00, 0x00, 0x00, 0x05}, 7, "cmp byte ptr [rel $+20h], 05h"},
            {"mov eax, [eax]", {0x67, 0x8B, 0x00}, 3, "mov eax, dword ptr [eax]"},
            {"mov eax, [eax+ecx*2]", {0x67, 0x8B, 0x04, 0x48}, 4, "mov eax, dword ptr [eax+ecx*2]"},
            {"mov eax, [r8d]", {0x67, 0x41, 0x8B, 0x00}, 4, "mov eax, dword ptr [r8d]"},
            {"mov eax, [esp]", {0x67, 0x8B, 0x04, 0x24}, 4, "mov eax, dword ptr [esp]"},
            {"mov eax, [a32 rel $+0x10]", {0x67, 0x8B, 0x05, 0x09, 0x00, 0x00, 0x00}, 7, "mov eax, dword ptr [a32 rel $+10h]"},
            {"mov rax, fs:[0x10]", {0x64, 0x48, 0x8B, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00}, 9, "mov rax, qword ptr fs:[0000000000000010h]"},
            {"mov eax, [gs:rax]", {0x65, 0x8B, 0x00}, 3, "mov eax, dword ptr gs:[rax]"},
            {"push rax", {0x50}, 1, "push rax"},
            {"push r8", {0x41, 0x50}, 2, "push r8"},
            {"push ax", {0x66, 0x50}, 2, "push ax"},
            {"push r9w", {0x66, 0x41, 0x51}, 3, "push r9w"},
            {"push 1", {0x6A, 0x01}, 2, "push 01h"},
            {"push -1", {0x6A, 0xFF}, 2, "push -01h"},
            {"push 0x80", {0x68, 0x80, 0x00, 0x00, 0x00}, 5, "push 00000080h"},
            {"push 0x12345678", {0x68, 0x78, 0x56, 0x34, 0x12}, 5, "push 12345678h"},
            {"push word 1", {0x66, 0x6A, 0x01}, 3, "push word 01h"},
            {"push qword [rax]", {0xFF, 0x30}, 2, "push qword ptr [rax]"},
            {"push word [rax]", {0x66, 0xFF, 0x30}, 3, "push word ptr [rax]"},
            {"pop rbx", {0x5B}, 1, "pop rbx"},
            {"pop r15", {0x41, 0x5F}, 2, "pop r15"},
            {"pop qword [r12]", {0x41, 0x8F, 0x04, 0x24}, 4, "pop qword ptr [r12]"},
            {"pushfq", {0x9C}, 1, "pushfq "},
            {"popfq", {0x9D}, 1, "popfq "},
            {"pushfw", {0x66, 0x9C}, 2, "pushfw "},
            {"ret", {0xC3}, 1, "ret "},
            {"retw", {0x66, 0xC3}, 2, "retw "},
            {"retq", {0xC3}, 1, "ret "},
            {"ret 8", {0xC2, 0x08, 0x00}, 3, "ret 0008h"},
            {"leave", {0xC9}, 1, "leave "},
            {"leavew", {0x66, 0xC9}, 2, "leavew "},
            {"enter 8, 0", {0xC8, 0x08, 0x00, 0x00}, 4, "enter 0008h, 00h"},
            {"push fs", {0x0F, 0xA0}, 2, "push fs"},
            {"pop gs", {0x0F, 0xA9}, 2, "pop gs"},
            {"push qword fs", {0x0F, 0xA0}, 2, "push fs"},
            {"call rax", {0xFF, 0xD0}, 2, "call rax"},
            {"call r8", {0x41, 0xFF, 0xD0}, 3, "call r8"},
            {"call qword [rax]", {0xFF, 0x10}, 2, "call qword ptr [rax]"},
            {"jmp qword [r9+rcx*8]", {0x41, 0xFF, 0x24, 0xC9}, 4, "jmp qword ptr [r9+rcx*8]"},
            {"jmp rbx", {0xFF, 0xE3}, 2, "jmp rbx"},
            {"call far dword [rax]", {0xFF, 0x18}, 2, "call far dword ptr [rax]"},
            {"jmp far qword [rax]", {0x48, 0xFF, 0x28}, 3, "jmp far qword ptr [rax]"},
            {"call $+5", {0xE8, 0x00, 0x00, 0x00, 0x00}, 5, "call $+05h"},
            {"call $+0x1000", {0xE8, 0xFB, 0x0F, 0x00, 0x00}, 5, "call $+1000h"},
            {"jmp $+2", {0xEB, 0x00}, 2, "jmp $+02h"},
            {"jmp $-0x100", {0xE9, 0xFB, 0xFE, 0xFF, 0xFF}, 5, "jmp $-0100h"},
            {"jmp $+0x1000", {0xE9, 0xFB, 0x0F, 0x00, 0x00}, 5, "jmp $+1000h"},
            {"je $+0x10", {0x74, 0x0E}, 2, "jz $+10h"},
            {"jne $+0x1000", {0x0F, 0x85, 0xFA, 0x0F, 0x00, 0x00}, 6, "jnz $+1000h"},
            {"jrcxz $+5", {0xE3, 0x03}, 2, "jrcxz $+05h"},
            {"jecxz $+5", {0x67, 0xE3, 0x02}, 3, "jecxz $+05h"},
            {"loop $+5", {0xE2, 0x03}, 2, "loop $+05h"},
            {"a32 loop $+5", {0x67, 0xE2, 0x02}, 3, "loop $+05h, ecx"},
            {"cdqe", {0x48, 0x98}, 2, "cdqe "},
            {"cqo", {0x48, 0x99}, 2, "cqo "},
            {"cwde", {0x98}, 1, "cwde "},
            {"cdq", {0x99}, 1, "cdq "},
            {"cbw", {0x66, 0x98}, 2, "cbw "},
            {"cwd", {0x66, 0x99}, 2, "cwd "},
            {"movsq", {0x48, 0xA5}, 2, "movsq "},
            {"movsd", {0xA5}, 1, "movsd "},
            {"movsw", {0x66, 0xA5}, 2, "movsw "},
            {"movsb", {0xA4}, 1, "movsb "},
            {"rep movsq", {0xF3, 0x48, 0xA5}, 3, "rep movsq "},
            {"rep stosb", {0xF3, 0xAA}, 2, "rep stosb "},
            {"a32 movsb", {0x67, 0xA4}, 2, "a32 movsb "},
            {"iretq", {0x48, 0xCF}, 2, "iretq "},
            {"iretd", {0xCF}, 1, "iret "},
            {"iretw", {0x66, 0xCF}, 2, "iretw "},
            {"iret", {0xCF}, 1, "iret "},
            {"movsxd rax, ecx", {0x48, 0x63, 0xC1}, 3, "movsxd rax, ecx"},
            {"movsxd r8, r9d", {0x4D, 0x63, 0xC1}, 3, "movsxd r8, r9d"},
            {"movsxd rax, dword [rax]", {0x48, 0x63, 0x00}, 3, "movsxd rax, dword ptr [rax]"},
            {"movsxd eax, ecx", {0x63, 0xC1}, 2, "movsxd eax, ecx"},
            {"movzx rax, byte [rax]", {0x48, 0x0F, 0xB6, 0x00}, 4, "movzx rax, byte ptr [rax]"},
            {"movzx r8d, bl", {0x44, 0x0F, 0xB6, 0xC3}, 4, "movzx r8d, bl"},
            {"movsx rax, ax", {0x48, 0x0F, 0xBF, 0xC0}, 4, "movsx rax, ax"},
            {"movsx rax, sil", {0x48, 0x0F, 0xBE, 0xC6}, 4, "movsx rax, sil"},
            {"cmpxchg16b [rax]", {0x48, 0x0F, 0xC7, 0x08}, 4, "cmpxchg16b oword ptr [rax]"},
            {"cmpxchg16b [r9+rcx*2]", {0x49, 0x0F, 0xC7, 0x0C, 0x49}, 5, "cmpxchg16b oword ptr [r9+rcx*2]"},
            {"cmpxchg8b [rax]", {0x0F, 0xC7, 0x08}, 3, "cmpxchg8b qword ptr [rax]"},
            {"fxsave64 [rax]", {0x48, 0x0F, 0xAE, 0x00}, 4, "fxsave64 [rax]"},
            {"fxrstor64 [r8]", {0x49, 0x0F, 0xAE, 0x08}, 4, "fxrstor64 [r8]"},
            {"fxsave [rax]", {0x0F, 0xAE, 0x00}, 3, "fxsave [rax]"},
            {"swapgs", {0x0F, 0x01, 0xF8}, 3, "swapgs "},
            {"syscall", {0x0F, 0x05}, 2, "syscall "},
            {"sysret", {0x0F, 0x07}, 2, "sysret "},
            {"mov rax, cr0", {0x0F, 0x20, 0xC0}, 3, "mov rax, cr0"},
            {"mov cr8, rax", {0x44, 0x0F, 0x22, 0xC0}, 4, "mov cr8, rax"},
            {"mov r9, cr3", {0x41, 0x0F, 0x20, 0xD9}, 4, "mov r9, cr3"},
            {"mov dr7, r8", {0x41, 0x0F, 0x23, 0xF8}, 4, "mov dr7, r8"},
            {"movq xmm0, rax", {0x66, 0x48, 0x0F, 0x6E, 0xC0}, 5, "movq xmm0, rax"},
            {"movq rax, xmm0", {0x66, 0x48, 0x0F, 0x7E, 0xC0}, 5, "movq rax, xmm0"},
            {"movq xmm8, r9", {0x66, 0x4D, 0x0F, 0x6E, 0xC1}, 5, "movq xmm8, r9"},
            {"movd xmm8, r9d", {0x66, 0x45, 0x0F, 0x6E, 0xC1}, 5, "movd xmm8, r9d"},
            {"movd r8d, xmm9", {0x66, 0x45, 0x0F, 0x7E, 0xC8}, 5, "movd r8d, xmm9"},
            {"movd eax, xmm0", {0x66, 0x0F, 0x7E, 0xC0}, 4, "movd eax, xmm0"},
            {"movq xmm1, [r9+rcx*2+0x10]", {0xF3, 0x41, 0x0F, 0x7E, 0x4C, 0x49, 0x10}, 7, "movq xmm1, qword ptr [r9+rcx*2 + 10h]"},
            {"movq xmm1, xmm2", {0xF3, 0x0F, 0x7E, 0xCA}, 4, "movq xmm1, xmm2"},
            {"movq mm0, rax", {0x48, 0x0F, 0x6E, 0xC0}, 4, "movq mm0, rax"},
            {"movq rax, mm1", {0x48, 0x0F, 0x7E, 0xC8}, 4, "movq rax, mm1"},
            {"cvttss2si rax, xmm0", {0xF3, 0x48, 0x0F, 0x2C, 0xC0}, 5, "cvttss2si rax, xmm0"},
            {"cvttsd2si r8, xmm9", {0xF2, 0x4D, 0x0F, 0x2C, 0xC1}, 5, "cvttsd2si r8, xmm9"},
            {"cvtsi2sd xmm0, rax", {0xF2, 0x48, 0x0F, 0x2A, 0xC0}, 5, "cvtsi2sd xmm0, rax"},
            {"cvtsi2ss xmm10, r9", {0xF3, 0x4D, 0x0F, 0x2A, 0xD1}, 5, "cvtsi2ss xmm10, r9"},
            {"paddq xmm8, xmm9", {0x66, 0x45, 0x0F, 0xD4, 0xC1}, 5, "paddq xmm8, xmm9"},
            {"paddq xmm1, [r9+r10*2]", {0x66, 0x43, 0x0F, 0xD4, 0x0C, 0x51}, 6, "paddq xmm1, oword ptr [r9+r10*2]"},
            {"pshufd xmm10, xmm11, 0x1b", {0x66, 0x45, 0x0F, 0x70, 0xD3, 0x1B}, 6, "pshufd xmm10, xmm11, 1Bh"},
            {"pslldq xmm10, 0x12", {0x66, 0x41, 0x0F, 0x73, 0xFA, 0x12}, 6, "pslldq xmm10, 12h"},
            {"movmskps r9d, xmm8", {0x45, 0x0F, 0x50, 0xC8}, 4, "movmskps r9d, xmm8"},
            {"movmskps rax, xmm0", {0x48, 0x0F, 0x50, 0xC0}, 4, "movmskps rax, xmm0"},
            {"pextrw eax, xmm8, 1", {0x66, 0x41, 0x0F, 0xC5, 0xC0, 0x01}, 6, "pextrw eax, xmm8, 01h"},
            {"pinsrw xmm9, r8d, 3", {0x66, 0x45, 0x0F, 0xC4, 0xC8, 0x03}, 6, "pinsrw xmm9, r8d, 03h"},
            {"addsd xmm15, qword [rel $+0x40]", {0xF2, 0x44, 0x0F, 0x58, 0x3D, 0x37, 0x00, 0x00, 0x00}, 9, "addsd xmm15, qword ptr [rel $+40h]"},
            {"movaps [r8], xmm15", {0x45, 0x0F, 0x29, 0x38}, 4, "movaps oword ptr [r8], xmm15"},
            {"movups xmm12, [rsp+r13*4]", {0x46, 0x0F, 0x10, 0x24, 0xAC}, 5, "movups xmm12, oword ptr [rsp+r13*4]"},
            {"xchg rax, rax", {0x48, 0x90}, 2, "xchg rax, rax"},
            {"xchg r8, rax", {0x49, 0x90}, 2, "xchg rax, r8"},
            {"xchg eax, r8d", {0x41, 0x90}, 2, "xchg eax, r8d"},
            {"xchg eax, ecx", {0x91}, 1, "xchg eax, ecx"},
            {"xchg rbx, rcx", {0x48, 0x87, 0xD9}, 3, "xchg rbx, rcx"},
            {"xchg ax, ax", {0x66, 0x90}, 2, "xchg ax, ax"},
            {"xchg [r9], bl", {0x41, 0x86, 0x19}, 3, "xchg bl, byte ptr [r9]"},
            {"bswap r9", {0x49, 0x0F, 0xC9}, 3, "bswap r9"},
            {"bswap rax", {0x48, 0x0F, 0xC8}, 3, "bswap rax"},
            {"inc r9", {0x49, 0xFF, 0xC1}, 3, "inc r9"},
            {"dec qword [rax]", {0x48, 0xFF, 0x08}, 3, "dec qword ptr [rax]"},
            {"inc eax", {0xFF, 0xC0}, 2, "inc eax"},
            {"not r10b", {0x41, 0xF6, 0xD2}, 3, "not r10b"},
            {"neg spl", {0x40, 0xF6, 0xDC}, 3, "neg spl"},
            {"imul rax, rbx", {0x48, 0x0F, 0xAF, 0xC3}, 4, "imul rax, rbx"},
            {"imul rax, [r9], 500", {0x49, 0x69, 0x01, 0xF4, 0x01, 0x00, 0x00}, 7, "imul rax, qword ptr [r9], 000001F4h"},
            {"imul r8, r9, 5", {0x4D, 0x6B, 0xC1, 0x05}, 4, "imul r8, r9, 05h"},
            {"test rax, rax", {0x48, 0x85, 0xC0}, 3, "test rax, rax"},
            {"shl r9, 5", {0x49, 0xC1, 0xE1, 0x05}, 4, "shl r9, 05h"},
            {"shl rax, cl", {0x48, 0xD3, 0xE0}, 3, "shl rax, cl"},
            {"shr qword [r8], 1", {0x49, 0xD1, 0x28}, 3, "shr qword ptr [r8], 1"},
            {"bt r9, 5", {0x49, 0x0F, 0xBA, 0xE1, 0x05}, 5, "bt r9, 05h"},
            {"bt qword [rax], rbx", {0x48, 0x0F, 0xA3, 0x18}, 4, "bt qword ptr [rax], rbx"},
            {"cmpxchg [rax], rbx", {0x48, 0x0F, 0xB1, 0x18}, 4, "cmpxchg qword ptr [rax], rbx"},
            {"xadd [r9], r10", {0x4D, 0x0F, 0xC1, 0x11}, 4, "xadd qword ptr [r9], r10"},
            {"setz r8b", {0x41, 0x0F, 0x94, 0xC0}, 4, "setz r8b"},
            {"setz ah", {0x0F, 0x94, 0xC4}, 3, "setz ah"},
            {"out dx, eax", {0xEF}, 1, "out dx, eax"},
            {"in eax, dx", {0xED}, 1, "in eax, dx"},
            {"in al, 0x10", {0xE4, 0x10}, 2, "in al, 10h"},
            {"mov eax, ds", {0x8C, 0xD8}, 2, "mov eax, ds"},
            {"mov ds, eax", {0x8E, 0xD8}, 2, "mov ds, ax"},
            {"mov ax, ds", {0x66, 0x8C, 0xD8}, 3, "mov ax, ds"},
            {"nop", {0x90}, 1, "nop "},
            {"o16 nop", {0x66, 0x90}, 2, "xchg ax, ax"},
            {"lgdt [rax]", {0x0F, 0x01, 0x10}, 3, "lgdt [rax]"},
            {"sidt [r9]", {0x41, 0x0F, 0x01, 0x09}, 4, "sidt [r9]"},
            {"lock cmpxchg [rax], rbx", {0xF0, 0x48, 0x0F, 0xB1, 0x18}, 5, "lock cmpxchg qword ptr [rax], rbx"},
            {"lock add qword [r8], 1", {0xF0, 0x49, 0x83, 0x00, 0x01}, 5, "lock add qword ptr [r8], 01h"},
            {"rep stosq", {0xF3, 0x48, 0xAB}, 3, "rep stosq "},
            {"repne scasb", {0xF2, 0xAE}, 2, "repne scasb "},
            {"xlatb", {0xD7}, 1, "xlatb "},
            {"lodsq", {0x48, 0xAD}, 2, "lodsq "},
            {"stosq", {0x48, 0xAB}, 2, "stosq "},
            {"a32 lodsq", {0x67, 0x48, 0xAD}, 3, "a32 lodsq "},
            {"fld qword [r9]", {0x41, 0xDD, 0x01}, 3, "fld qword ptr [r9]"},
            {"fild qword [rax]", {0xDF, 0x28}, 2, "fild qword ptr [rax]"},
            {"fstp tword [rsp+r12]", {0x42, 0xDB, 0x3C, 0x24}, 4, "fstp tword ptr [rsp+r12]"},
            {"fnstcw [r8]", {0x41, 0xD9, 0x38}, 3, "fnstcw word ptr [r8]"},
            {"fxch st3", {0xD9, 0xCB}, 2, "fxch st3"},
            // The rip relative syntax which nasm doesn't have: [rip + disp] is the displacement from the end of the instruction
            {"mov eax, [rip+0x10]", {0x8B, 0x05, 0x10, 0x00, 0x00, 0x00}, 6, "mov eax, dword ptr [rel $+16h]"},
            {"lea rax, [rip-5]", {0x48, 0x8D, 0x05, 0xFB, 0xFF, 0xFF, 0xFF}, 7, "lea rax, [rel $+02h]"},
            {"mov dword [rip+0x10], 5", {0xC7, 0x05, 0x10, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00}, 10, "mov dword ptr [rel $+1Ah], 00000005h"},
            {NULL, {}, 0, NULL}
        };

        const SingleOpcodeTest* ptr = gOpcodes;
        while (ptr->instructionText != NULL)
        {
            testInstruction(ptr->instructionText, ptr->opcode, ptr->opcodeSize,
                            ptr->instructionTextHexSuffix);
            ptr++;
        }

        testFactory();

        // The instructions which are not valid in 64 bit mode: the table rows
        // that are MODE64_INVALID, the 16 bit addresses, the 32 bit operand
        // size of push and pop, and the registers that REX can't reach
        static const char* gRejected64[] = {
            "push es",
            "pop ds",
            "push cs",
            "pusha",
            "popa",
            "pushad",
            "aaa",
            "aas",
            "daa",
            "das",
            "aam",
            "aad",
            "into",
            "bound eax, [rax]",
            "lds eax, [rax]",
            "les eax, [rax]",
            "arpl ax, bx",
            "call 0x1234:0x56789",
            "jmp 0x1234:0x56789",
            "mov ah, r8b",
            "mov spl, ah",
            "mov ah, [r8]",
            "mov ah, spl",
            "add dh, r9b",
            "push eax",
            "pop ebx",
            "push dword [rax]",
            "pushfd",
            "popfd",
            "call eax",
            "jmp ecx",
            "mov al, [bx]",
            "mov eax, [bx+si]",
            "mov eax, [ax]",
            "jcxz $+5",
            "in rax, dx",
            "out dx, rax",
            "mov cr0, eax",
            "mov dr0, ebx",
            "xchg ah, spl",
            "mov rax, [eax+rbx]",
            "lea rax, rbx",
            "mov eax, [rax*3]",
            "mov eax, [rsp*2]",
            "mov eax, [rax+rsp*2]",
            "mov eax, [rax+rcx+rdx]",
            "mov eax, [rip+rax]",
            "mov eax, [rip*2]",
            "movsxd eax, rcx",
            "movsxd rax, rcx",
            "cmpxchg16b rax",
            "cmpxchg16b eax",
            "cmpxchg8b [rax*3]",
            "mov rax, 0x12345678901234567",
            "movq xmm0, eax",
            "movd xmm0, rax",
            "mov r8, r8b",
            "mov [rax], 5",
            "xmm8",
            "mov xmm8, rax",
            "paddq xmm16, xmm0",
            "mov eax, ebx, ecx",
            "add rax",
            "retd",
            "leaved",
            "pushfd",
            "mov eax, [rax",
            "nop 5",
            NULL
        };
        for (const char** text = gRejected64; *text != NULL; text++)
            testRejected(*text, IA32eInstructionSet::AMD_64);

        // The instructions of 64 bit mode only (MODE64_ONLY rows, the registers
        // of 64 bit mode) are not valid in 32 bit mode
        static const char* gRejected32[] = {
            "mov rax, rbx",
            "mov r8d, eax",
            "mov spl, al",
            "mov eax, [rip+8]",
            "mov eax, [rel $+8]",
            "movsxd eax, ecx",
            "swapgs",
            "cdqe",
            "cqo",
            "movsq",
            "iretq",
            "jrcxz $+5",
            "push rax",
            "mov cr8, eax",
            "movq xmm0, rax",
            "cmpxchg16b [eax]",
            "mov eax, [rax]",
            "mov xmm8, eax",
            "paddq xmm8, xmm9",
            "mov eax, r9d",
            NULL
        };
        for (const char** text = gRejected32; *text != NULL; text++)
            testRejected(*text, IA32eInstructionSet::INTEL_32);
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestAMD64Assembler g_globalTestAMD64Assembler;
