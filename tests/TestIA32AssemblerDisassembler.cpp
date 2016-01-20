/*
 * TestIA32AssemblerDisassembler.cpp
 *
 * Tests the IA32 processor by assembling and disassembling instructions...
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

#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblerInterface.h"
#include "dismount/assembler/AssemblingFactory.h"


class TestObjectTestIA32AssemblerDisassembler : public cTestObject {
public:
    // Max-opcode size
    #define MAX_OPCODE (20)
    // Operand table
    struct SingleOpcodeTest {
        const char* instructionText;
        const byte opcode[MAX_OPCODE];
        uint  opcodeSize;
        const char*    instructionTextHexSuffix;
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


    // Output test
    void outputTest(const char* data, const cBuffer opcodeData,
                    const char* dataHexSuffix, const cString opcodeText)
    {

        cout << data << " -> ";

        // Trace out data...
        for (uint i = 0; i < opcodeData.getSize(); i++)
        {
            cout << HEXBYTE(opcodeData[i]) << ' ';
        }

        cout << endl << opcodeText
             << "  |  " << dataHexSuffix << endl << endl;
    }


    /*
     * Test a single instruction by compiling
     */
    void testInstruction(const char* data,
                         const byte* orginialOpcode, const uint size, const char* dataHexSuffix)
    {


        // Generate empty function binary pass
        FirstPassBinaryPtr main(new FirstPassBinary(OpcodeSubsystems::DISASSEMBLER_INTEL_32, false));
        main->createNewBlockWithoutChange(0,
            StackInterfacePtr(new TempClassInterface(main)));
        main->changeBasicBlock(0);
        // Generate assembler
        AssemblerInterfacePtr ia32asm = AssemblingFactory::generateAssembler(
                    main, OpcodeSubsystems::DISASSEMBLER_INTEL_32);

        // Assemble instruction
        (*ia32asm->getAssembler()) << data << endl;
        // Instruction is now compiled. Test binary
        cBuffer instruction = ia32asm->getFirstPass()->getCurrentBlockData();
        TESTS_ASSERT_EQUAL(instruction.getSize(), size);
        for (uint i = 0; i < size; i++)
        TESTS_ASSERT_EQUAL(instruction[i], orginialOpcode[i]);

        // And test disassembler
        cMemoryStream disstream(instruction);
        // Generate the disassembler
        StreamDisassemblerPtr disassembler =
            StreamDisassemblerFactory::disassemble(
                        OpcodeSubsystems::DISASSEMBLER_INTEL_32,
                        BasicInputPtr(&disstream, SMARTPTR_DESTRUCT_NONE));
        // Prepare the default data-formatter
        DefaultOpcodeDataFormatter formatter(1);
        // Disassembler single instruction
        OpcodePtr opcode = disassembler->next();
        cBuffer opcodeData = opcode->getOpcode();
        TESTS_ASSERT_EQUAL(opcodeData, instruction);

        // Get the translated opcode
        OpcodeFormatterPtr fr = disassembler->getOpcodeFormat(opcode,
                                                              formatter);

        cString opcodeText = fr->string();

        // Output test
        outputTest(data, opcodeData, dataHexSuffix, opcodeText);


        // Test string
        TESTS_ASSERT_EQUAL(opcodeText, dataHexSuffix);
    }


    /*
             A single opcode test is made up of the following:

             1. The opcode text to be passed to the assembler.
                When specifying displacements (probably immeditates too.
                Check this), the assembler currently only supports displacement
                either in decimal format or hex with the 0x prefix.
                The assembler does not support hex values with the 'h' suffix
             2. The Instruction value as it should appear once assembled
                and running in memory. Little / big endian must be accounted
                for. This value will be compared to the output of the
                disassembler. The assembler currently supports only the operand
                size override prefix (0x66) which will be used in order to
                override the default operand size (which is in most cases
                32-bit). The operand size override prefix will normally be used
                by the assembler in order to specify 16-bit operands.
             3. The total size in bytes of the given opcode. This includes
                prefixes, the opcode command itself, the MOD/RM byte,
                displacement and immediate (Am I missing anything else?
                double check this...)
             4. The opcode text once again. However, this time using hex values
                with the 'h' suffix. This string will be compared to the output
                of the disassembler. The disassembler formatter currently
                outputs hex values with the 'h' suffix.

             Test format:
                {"", {0x} , , ""},

             Currently, for each opcode, all Mod values will b tested. For each
             Mod value, one random R/M will be chosen. For each R/M, two random
             reg values will b used. SIB form will not be tested as of now as it
             is currently not fully supported
    */

    virtual void test()
    {
        // Displacement definitions (Should go here...):
        // As I'm having trouble with using the preprocessor definition
        // inside a string, the displacements will be specified explicitly.
        //
        //        8-bit displacement:  0x1E       | 1Eh       | 30
        //        16-bit displacement: 0x013C        | 013Ch        | 316
        //        32-bit displacement: 0x000117A5 | 000117A5h | 71589
        #define DISP8PREFIX "0x1E"
        #define DISP8SUFFIX "1Eh"
        #define DISP8BYTE  0x1E
        #define DISP16PREFIX "0x013C"
        #define DISP16SUFFIX "013Ch"
        #define DISP16BYTES  0x01, 0x37
        #define DISP32PREFIX "0x000117A5"
        #define DISP32SUFFIX "000117A5h"
        #define DISP32BYTES 0xA5, 0x17, 0x01, 0x00


        // Immediate definitions:
        //
        //      8-bit immediate:  0x2C     | 2Ch       | 44
        //      16-bit immediate: 0xE266   | E266h     | 57958
        //      32-bit immediate: FFFFF097 | FFFFF097h | 4294963351


        static const SingleOpcodeTest gOpcodes[] = {




            // One-byte Opcode:
            // 00H - 0FH
            // ----------------------------------------------------------------
            // Opcode: 00
            // add r/m8, r8 - Byte, discards operand-size attribute
            // ----------------------------------------------------------------
            // Mod: 00
            {"add byte ptr [eax], al", {0x00, 0x00}, 2,
                "add byte ptr [eax], al"},
            {"add byte ptr [eax], dh", {0x00, 0x30}, 2,
                "add byte ptr [eax], dh"},
            // Mod: 01
            {"add byte ptr [ebx + 0x1E], dl", {0x00, 0x53, 0x1E}, 3,
                "add byte ptr [ebx + 1Eh], dl"},
            {"add byte ptr [edi + 0x1E], bl", {0x00, 0x5F, 0x1E}, 3,
                "add byte ptr [edi + 1Eh], bl"},
            // Mod: 10
            {"add byte ptr [esi + 0x000117A5], cl",
                {0x00, 0x8E, 0xA5, 0x17, 0x01, 0x00}, 6,
                "add byte ptr [esi + 000117A5h], cl"},
            {"add byte ptr [esi + 0x000117A5], bh",
                {0x00, 0xBE, 0xA5, 0x17, 0x01, 0x00}, 6,
                "add byte ptr [esi + 000117A5h], bh"},
            // Mod: 11
            {"add cl, ah", {0x00, 0xE1}, 2, "add cl, ah"},
            {"add cl, ch", {0x00, 0xE9}, 2, "add cl, ch"},
            // ----------------------------------------------------------------
            // Opcode: 01
            // add r/m16, r16 | add r/m32, r32 -    Word or doubleword,
            //                                             depending on
            //                                             operand-size
            //                                             attribute or override
            // ----------------------------------------------------------------
            // Mod: 00
            {"add word ptr [edi], bx", {0x66, 0x01, 0x1F}, 3,
                "add word ptr [edi], bx"},
            {"add dword ptr [edi], ebx", {0x01, 0x1F}, 2,
                "add dword ptr [edi], ebx"},
            {"add word ptr [edi], si", {0x66, 0x01, 0x37}, 3,
                "add word ptr [edi], si"},
            {"add dword ptr [edi], esi", {0x01, 0x37}, 2,
                "add dword ptr [edi], esi"},
            // Mod: 01
            {"add word ptr [eax + 0x1E], bp", {0x66, 0x01, 0x68, 0x1E}, 4,
                "add word ptr [eax + 1Eh], bp"},
            {"add dword ptr [eax + 0x1E], ebp",  {0x01, 0x68, 0x1E}, 3,
                "add dword ptr [eax + 1Eh], ebp"},
            {"add word ptr [eax + 0x1E], di", {0x66, 0x01, 0x78, 0x1E}, 4,
                "add word ptr [eax + 1Eh], di"},
            {"add dword ptr [eax + 0x1E], edi", {0x01, 0x78, 0x1E}, 3,
                "add dword ptr [eax + 1Eh], edi"},
            // Mod: 10
            {"add word ptr [ebx + 0x000117A5], ax", {0x66, 0x01, 0x83, 0xA5, 0x17, 0x01, 0x00},
                7, "add word ptr [ebx + 000117A5h], ax"},
            {"add dword ptr [ebx+0x000117A5], eax",
                {0x01, 0x83, 0xA5, 0x17, 0x01, 0x00}, 6,
                "add dword ptr [ebx + 000117A5h], eax"},
            {"add word ptr [ebx + 0x000117A5], bp", {0x66, 0x01, 0xAB, 0xA5, 0x17, 0x01, 0x00},
                7, "add word ptr [ebx + 000117A5h], bp"},
            {"add dword ptr [ebx + 0x000117A5], ebp",
                {0x01, 0xAB, 0xA5, 0x17, 0x01, 0x00}, 6,
                "add dword ptr [ebx + 000117A5h], ebp"},
            // Mod: 11
            {"add sp, bx", {0x66, 0x01, 0xDC}, 3, "add sp, bx"},
            {"add esp, ebx", {0x01, 0xDC}, 2, "add esp, ebx"},
            {"add sp, sp", {0x66, 0x01, 0xE4}, 3, "add sp, sp"},
            {"add esp, esp", {0x01, 0xE4} , 2, "add esp, esp"},
            // ----------------------------------------------------------------
            // Opcode: 02
            // add r8, r/m8 - Byte, discards operand-size attribute
            // ----------------------------------------------------------------
            // Mod: 00
            {"add al, byte ptr [0x000117A5]", {0x02, 0x05, 0xA5, 0x17, 0x01, 0x00}, 6,
             "add al, byte ptr [000117A5h]"},
            {"add ch, byte ptr [0x000117A5]", {0x02, 0x2D, 0xA5, 0x17, 0x01, 0x00}, 6,
             "add ch, byte ptr [000117A5h]"},
            // Mod: 01
            {"add cl, byte ptr [edx + 0x1E]", {0x02, 0x4A, 0x1E}, 3,
             "add cl, byte ptr [edx + 1Eh]"},
            {"add bh, byte ptr [edx + 0x1E]", {0x02, 0x7A, 0x1E}, 3,
             "add bh, byte ptr [edx + 1Eh]"},
            // Mod: 10
            {"add dl, byte ptr [ebx + 0x000117A5]", {0x02, 0x93, 0xA5, 0x17, 0x01, 0x00}, 6,
             "add dl, byte ptr [ebx + 000117A5h]"},
            {"add dh, byte ptr [ebx + 0x000117A5]", {0x02, 0xB3, 0xA5, 0x17, 0x01, 0x00}, 6,
             "add dh, byte ptr [ebx + 000117A5h]"},
            // Mod: 11 Won't work, as match will be found at 00. Test in special case function
            //{"add bl, bh", {0x02, 0xDF}, 2, "add bl, bh"},
            //{"add ah, bh", {0x02, 0xE7}, 2, "add ah, bh"},
            // ----------------------------------------------------------------
            // Opcode: 03
            // add r16, r/m16 | add r32, r/m32 -    Word or doubleword,
            //                                             depending on
            //                                             operand-size
            //                                             attribute or override
            // ----------------------------------------------------------------
            // Mod: 00
            // R/M: 001
            // ---------
            // REG: 010
            {"add dx, word ptr [ecx]", {0x66, 0x03, 0x11}, 3,
             "add dx, word ptr [ecx]"},
            {"add edx, dword ptr [ecx]", {0x03, 0x11}, 2,
             "add edx, dword ptr [ecx]"},
            // REG: 111
            {"add di, word ptr [ecx]", {0x66, 0x03, 0x39}, 3,
             "add di, word ptr [ecx]"},
            {"add edi, dword ptr [ecx]", {0x03, 0x39}, 2,
             "add edi, dword ptr [ecx]"},
            // Mod: 01
            // R/M: 000
            // ---------
            // REG: 000
            {"add ax, word ptr [eax + 0x1E]", {0x66, 0x03, 0x40, 0x1E}, 4,
             "add ax, word ptr [eax + 1Eh]"},
            {"add eax, dword ptr [eax + 0x1E]", {0x03, 0x40, 0x1E}, 3,
             "add eax, dword ptr [eax + 1Eh]"},
            // REG: 011
            {"add bx, word ptr [eax + 0x1E]", {0x66, 0x03, 0x58, 0x1E}, 4,
             "add bx, word ptr [eax + 1Eh]"},
            {"add ebx, dword ptr [eax + 0x1E]", {0x03, 0x58, 0x1E}, 3,
             "add ebx, dword ptr [eax + 1Eh]"},
            // Mod: 10
            // R/M: 110
            // ---------
            // REG: 101
            {"add bp, word ptr [esi + 0x000117A5]", {0x66, 0x03, 0xAE, 0xA5, 0x17, 0x01, 0x00}, 7,
             "add bp, word ptr [esi + 000117A5h]"},
            {"add ebp, dword ptr [esi + 0x000117A5]", {0x03, 0xAE, 0xA5, 0x17, 0x01, 0x00}, 6,
             "add ebp, dword ptr [esi + 000117A5h]"},
            // REG: 100
            {"add sp, word ptr [esi + 0x000117A5]", {0x66, 0x03, 0xA6, 0xA5, 0x17, 0x01, 0x00}, 7,
             "add sp, word ptr [esi + 000117A5h]"},
            {"add esp, dword ptr [esi + 0x000117A5]", {0x03, 0xA6, 0xA5, 0x17, 0x01, 0x00}, 6,
             "add esp, dword ptr [esi + 000117A5h]"},
            // Mod: 11 Fails, as match is found at 01. Test in special case function
            // R/M: 100
            // ---------
            // REG: 001
            //{"add cx, sp", {0x66, 0x03, 0xCC}, 3, "add cx, sp"},
            //{"add ecx, esp", {0x03, 0xCC}, 2, "add ecx, esp"},
            // REG: 110
            //{"add si, sp", {0x66, 0x03, 0xF4}, 3, "add si, sp"},
            //{"add esi, esp", {0x03, 0xF4}, 2, "add esi, esp"},
            // ----------------------------------------------------------------
            // Opcode: 04
            // add al, imm8
            // ----------------------------------------------------------------
            {"add al, 0x2C", {0x04, 0x2C}, 2, "add al, 2Ch"},
            // ----------------------------------------------------------------
            // Opcode: 05
            // add ax, imm16 | add eax, imm32
            // ----------------------------------------------------------------
            {"add ax, 0xE266", {0x66, 0x05, 0x66, 0xE2}, 4,
             "add ax, E266h"},
            {"add eax, 0xFFFFF097", {0x05, 0x97, 0xF0, 0xFF, 0xFF}, 5,
             "add eax, FFFFF097h"},
            // ----------------------------------------------------------------
            // Opcode: 06
            // push es
            // ----------------------------------------------------------------
            {"push es", {0x06}, 1, "push es"},
            // ----------------------------------------------------------------
            // Opcode: 07
            // pop es
            // ----------------------------------------------------------------
            {"pop es", {0x07}, 1, "pop es"},
            // ----------------------------------------------------------------
            // Opcode: 08
            // or r/m8, r8 - Byte, discards operand-size attribute
            // ----------------------------------------------------------------
            // Mod: 00
            // R/M: 110
            // ---------
            // REG: 001
            {"or byte ptr [esi], cl", {0x08, 0x0E}, 2, "or byte ptr [esi], cl"},
            // REG: 101
            {"or byte ptr [esi], ch", {0x08, 0x2E}, 2, "or byte ptr [esi], ch"},
            // Mod: 01
            // R/M: 011
            // ---------
            // REG: 000
            {"or byte ptr [ebx + " DISP8PREFIX "], al", {0x08, 0x43, 0x1E}, 3,
             "or byte ptr [ebx + " DISP8SUFFIX "], al"},
            // REG: 111
            {"or byte ptr [ebx + " DISP8PREFIX "], bh", {0x08, 0x7B, 0x1E}, 3,
             "or byte ptr [ebx + " DISP8SUFFIX "], bh"},
            // Mod: 10
            // R/M: 000
            // ---------
            // REG: 010
            {"or byte ptr [eax + " DISP32PREFIX "], dl", {0x08, 0x90, DISP32BYTES}, 6,
             "or byte ptr [eax + " DISP32SUFFIX "], dl"},
            // REG: 110
            {"or byte ptr [eax + " DISP32PREFIX "], dh", {0x08, 0xB0, DISP32BYTES}, 6,
             "or byte ptr [eax + " DISP32SUFFIX "], dh"},
            // Mod: 11
            // R/M: 111
            // ---------
            // REG:
            {"or bh, bl", {0x08, 0xDF}, 2, "or bh, bl"},
            // REG:
            {"or bh, ah", {0x08, 0xE7}, 2, "or bh, ah"},
            // ----------------------------------------------------------------
            // Opcode: 09
            // or r/m16, r16 | r/m32, r32
            // ----------------------------------------------------------------
            // Mod: 00
            // R/M: 001
            // ---------
            // REG: 000
            {"or word ptr [ecx], ax", {0x66, 0x09, 0x01}, 3, "or word ptr [ecx], ax"},
            {"or dword ptr [ecx], eax", {0x09, 0x01}, 2, "or dword ptr [ecx], eax"},
            // REG: 110
            {"or word ptr [ecx], si", {0x66, 0x09, 0x31}, 3, "or word ptr [ecx], si"},
            {"or dword ptr [ecx], esi", {0x09, 0x31}, 2, "or dword ptr [ecx], esi"},
            // Mod: 01
            // R/M: 101
            // ---------
            // REG: 001
            {"or word ptr [ebp + " DISP8PREFIX "], cx", {0x66, 0x09, 0x4D, DISP8BYTE}, 4,
             "or word ptr [ebp + " DISP8SUFFIX "], cx"},
            {"or dword ptr [ebp + " DISP8PREFIX "], ecx", {0x09, 0x4D, DISP8BYTE}, 3,
             "or dword ptr [ebp + " DISP8SUFFIX "], ecx"},
            // REG: 101
            {"or word ptr [ebp + " DISP8PREFIX "], bp", {0x66, 0x09, 0x6D, DISP8BYTE}, 4,
             "or word ptr [ebp + " DISP8SUFFIX "], bp"},
           /* {"", {0x}, , ""},
            // Mod: 10
            // R/M:
            // ---------
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // Mod: 11
            // R/M:
            // ---------
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},

            // ----------------------------------------------------------------
            // Opcode: xx
            //
            // ----------------------------------------------------------------
            // Mod: 00
            // R/M:
            // ---------
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // Mod: 01
            // R/M:
            // ---------
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // Mod: 10
            // R/M:
            // ---------
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // Mod: 11
            // R/M:
            // ---------
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},
            // REG:
            {"", {0x}, , ""},
            {"", {0x}, , ""},

            {"mov ax, 123",{0x66, 0xB8, 0x00, 0x7B},4},
            {"lea eax, [eax]", {0x8D, 0x00}, 2},
            {"mov ax, [bx]", {0x89, 0x07}, 2},
            {"add al, cl", {0x00, 0xC8}, 2, "add al, cl"},
            {"add byte ptr [eax], bl", {0x00, 0x18}, 2},
            {"push ebp",               {0x55},       1},
            {"mov eax, ebx",           {0x89, 0xD8}, 2},
            {"add bh, bl",           {0x00, 0xDF}, 2},
            {"mov ax, bx", {0x66, 0x89, 0xD8}, 3}, */
            {NULL, {}, 0, NULL}
        };

        const SingleOpcodeTest* ptr = gOpcodes;
        while (ptr->instructionText != NULL)
        {
            testInstruction(ptr->instructionText, ptr->opcode, ptr->opcodeSize, ptr->instructionTextHexSuffix);
            ptr++;
        }
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestIA32AssemblerDisassembler g_globalTestIA32AssemblerDisassembler;
