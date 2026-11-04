/*
 * TestArm64Disassembler.cpp
 *
 * Tests of the A64 disassembler and assembler (the integer, branch, load /
 * store and system instructions): the text of the decoded words and the
 * assembled bytes for every mnemonic and alias of the base table (the rows of
 * tests/arm/Arm64TestRows.inc, the LLVM text), the invalid words, the
 * features (an encoding of a feature which is off is invalid), the stream
 * (fault tolerance, addresses, truncated data), the flow properties and the
 * assembler on a FirstPassBinary.
 *
 * The big comparison with clang and objdump is tests/arm/run_compat.py.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
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
#include "dismount/DisassemblerEndOfStreamException.h"
#include "dismount/DisassemblerInvalidOpcodeException.h"
#include "dismount/InvalidOpcodeByte.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/assembler/proc/arm/ArmAssembler.h"

// Like TESTS_ASSERT, but tell which check failed
#define ARM64_ASSERT(condition) \
    if (!(condition)) \
    { \
        fprintf(stderr, "TestArm64Disassembler.cpp:%d: %s\n", __LINE__, #condition); \
        XSTL_THROW(cException, XSTL_STRING("Condition asserted")); \
    }

#include "arm/Arm64TestRows.inc"

class TestObjectTestArm64Disassembler : public cTestObject {
public:
    // A row: the text of a word at an address
    struct TextRow {
        uint64 m_address;
        uint32 m_word;
        const char* m_text;
    };

    class TempClassInterface : public StackInterface {
    public:
        TempClassInterface(FirstPassBinaryPtr& binary) :
            StackInterface(*binary, true, 8)
        {
        };
        RegisterAllocationTable gNull;
        virtual uint getStackType() { return 8; };
        virtual uint getRegisterSize() { return 8; };
        virtual RegisterAllocationTable& getRegistersTable() { return gNull; };
        virtual const RegisterAllocationTable& getRegistersTable()const { return gNull; };
        virtual StackLocation replaceRegisterToStackVariable(StackLocation source) { return source; };
    };

    // Disassemble a word with the stream disassembler, return the text. 'isValid'
    // is set to false if the word is an invalid instruction
    cString disassemble(uint32 word, uint64 address, const ArmFeatureSet& features,
                        bool& isValid)
    {
        uint8 bytes[4] = { (uint8)word, (uint8)(word >> 8), (uint8)(word >> 16),
                           (uint8)(word >> 24) };
        cBuffer data(bytes, 4);
        cMemoryStream stream(data);
        ArmStreamDisassembler disassembler(ArmInstructionSet::ARM_A64,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, address), false, features);
        isValid = true;
        XSTL_TRY
        {
            OpcodePtr opcode = disassembler.next();
            ARM64_ASSERT(opcode->getOpcodeSize() == 4);
            DefaultOpcodeDataFormatter formatter(1);
            OpcodeFormatterPtr fr = disassembler.getOpcodeFormat(opcode, formatter);
            return fr->string();
        }
        XSTL_CATCH(DisassemblerInvalidOpcodeException&)
        {
            isValid = false;
        }
        return cString();
    }

    // Assemble a line, return the word. 'isValid' is false if the line is
    // rejected
    uint32 assemble(const char* text, uint64 address, const ArmFeatureSet& features,
                    bool& isValid)
    {
        uint32 encoding = 0;
        uint size = 0;
        isValid = true;
        XSTL_TRY
        {
            ArmAssemblerStream::assembleInstruction(ArmInstructionSet::ARM_A64, features,
                                                    cString(text), address, 0, encoding, size);
            ARM64_ASSERT(size == 4);
        }
        XSTL_CATCH(cException&)
        {
            isValid = false;
        }
        return encoding;
    }

    // The rows: the word gives the text, the text gives the word back or a word
    // with the same text (the fields that the decoder ignores)
    void testRows()
    {
        static const TextRow rows[] = { ARM64_TEXT_ROWS { 0, 0, NULL } };
        uint count = 0;
        for (const TextRow* row = rows; row->m_text != NULL; row++)
        {
            bool isValid;
            cString text = disassemble(row->m_word, row->m_address, ArmFeatureSet::all(), isValid);
            if (!isValid || !(text == row->m_text))
            {
                cout << "row " << HEXDWORD(row->m_word) << ": expected '" << row->m_text
                     << "' got '" << (isValid ? text : cString("(invalid)")) << "'" << endl;
                ARM64_ASSERT(false);
            }
            uint32 word = assemble(row->m_text, row->m_address, ArmFeatureSet::all(), isValid);
            if (!isValid || (word != row->m_word))
            {
                // Another word is fine if it is the same instruction
                cString other = isValid ?
                    disassemble(word, row->m_address, ArmFeatureSet::all(), isValid) : cString();
                if (!isValid || !(other == row->m_text))
                {
                    cout << "row " << HEXDWORD(row->m_word) << ": '" << row->m_text
                         << "' assembled to " << HEXDWORD(word) << endl;
                    ARM64_ASSERT(false);
                }
            }
            count++;
        }
        ARM64_ASSERT(count > 1000);
    }

    // The words which are not instructions
    void testInvalid()
    {
        static const uint32 words[] = { ARM64_INVALID_WORDS 0xffffffff };
        for (uint i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        {
            bool isValid;
            disassemble(words[i], 0x1000, ArmFeatureSet::all(), isValid);
            if (isValid)
                cout << "invalid word " << HEXDWORD(words[i]) << " decoded" << endl;
            ARM64_ASSERT(!isValid);
        }
    }

    // The features: the encodings of a feature which is off are invalid
    void testFeatures()
    {
        bool isValid;
        // ldadd x0, x1, [x2] is FEAT_LSE
        const uint32 ldadd = 0xf8200041;
        ARM64_ASSERT(disassemble(ldadd, 0, ArmFeatureSet::all(), isValid) == "ldadd x0, x1, [x2]");
        ARM64_ASSERT(isValid);
        ArmFeatureSet noLse = ArmFeatureSet::all();
        noLse.remove(ArmFeatures::FEAT_LSE);
        disassemble(ldadd, 0, noLse, isValid);
        ARM64_ASSERT(!isValid);
        assemble("ldadd x0, x1, [x2]", 0, noLse, isValid);
        ARM64_ASSERT(!isValid);
        ARM64_ASSERT(assemble("ldadd x0, x1, [x2]", 0, ArmFeatureSet::all(), isValid) == ldadd);
        ARM64_ASSERT(isValid);

        // The architecture versions: armv8-a has no LSE, armv8.1-a has
        ArmFeatureSet v80 = ArmFeatureSet::fromArchitecture("armv8-a");
        ArmFeatureSet v81 = ArmFeatureSet::fromArchitecture("armv8.1-a");
        ARM64_ASSERT(!v80.has(ArmFeatures::FEAT_LSE));
        ARM64_ASSERT(v81.has(ArmFeatures::FEAT_LSE));
        ARM64_ASSERT(v81.has(ArmFeatures::FEAT_CRC32));
        ARM64_ASSERT(!v81.has(ArmFeatures::FEAT_PAUTH));
        ARM64_ASSERT(ArmFeatureSet::fromArchitecture("armv8.3-a").has(ArmFeatures::FEAT_PAUTH));
        ARM64_ASSERT(ArmFeatureSet::fromArchitecture("armv9.2-a").has(ArmFeatures::FEAT_WFXT));
        ARM64_ASSERT(ArmFeatureSet::fromArchitecture("armv9-a").has(ArmFeatures::FEAT_SVE2));
        bool isThrown = false;
        XSTL_TRY
        {
            ArmFeatureSet::fromArchitecture("armv6-a");
        }
        XSTL_CATCH(cException&)
        {
            isThrown = true;
        }
        ARM64_ASSERT(isThrown);

        // The LLVM lists
        ArmFeatureSet set;
        ARM64_ASSERT(set.parse("+lse,+sve2,-sve2,crc"));
        ARM64_ASSERT(set.has(ArmFeatures::FEAT_LSE));
        ARM64_ASSERT(set.has(ArmFeatures::FEAT_CRC32));
        ARM64_ASSERT(set.has(ArmFeatures::FEAT_SVE) && !set.has(ArmFeatures::FEAT_SVE2));
        ARM64_ASSERT(!set.parse("+lse,+nosuchfeature"));
        ARM64_ASSERT(set.has(ArmFeatures::FEAT_LSE));
        ARM64_ASSERT(set.parse("none"));
        ARM64_ASSERT(!set.has(ArmFeatures::FEAT_LSE));
        ARM64_ASSERT(set.has(ArmFeatures::FEAT_A64_BASE));
        ArmFeatures::Feature feature;
        ARM64_ASSERT(ArmFeatures::findFeature("lse", feature) && (feature == ArmFeatures::FEAT_LSE));
        ARM64_ASSERT(!ArmFeatures::findFeature("nosuchfeature", feature));
        ARM64_ASSERT(cString(ArmFeatures::getName(ArmFeatures::FEAT_LSE)) == "lse");
        ARM64_ASSERT(ArmFeatures::getInstructionSet(ArmFeatures::FEAT_LSE) == ArmInstructionSet::ARM_A64);
        // The base is supported and verified, the A64 SIMD is not there yet
        ARM64_ASSERT((ArmFeatures::getSupport(ArmFeatures::FEAT_A64_BASE) &
                      ArmFeatures::SUPPORT_ENCODING_VERIFIED) != 0);
        ARM64_ASSERT(ArmFeatures::getSupport(ArmFeatures::FEAT_SVE) == 0);
    }

    // The stream: addresses, fault tolerance, truncated data
    void testStream()
    {
        // add x0, x1, x2; invalid; ret
        const uint8 code[] = { 0x20, 0x00, 0x02, 0x8b,
                               0xff, 0xff, 0xff, 0xff,
                               0xc0, 0x03, 0x5f, 0xd6 };
        cBuffer data(code, sizeof(code));
        cMemoryStream stream(data);
        ArmStreamDisassembler disassembler(ArmInstructionSet::ARM_A64,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0x400000), true);
        DefaultOpcodeDataFormatter formatter(1);

        OpcodePtr opcode = disassembler.next();
        ProcessorAddress address(gNullPointerProcessorAddress);
        ARM64_ASSERT(opcode->getOpcodeAddress(address) && (address.getAddress() == 0x400000));
        ARM64_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "add x0, x1, x2");
        ARM64_ASSERT(!opcode->isBranch());
        ARM64_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        ARM64_ASSERT(opcode->getOpcode().getSize() == 4);
        ARM64_ASSERT(opcode->getOpcode()[3] == 0x8b);

        // The invalid word: one InvalidOpcodeByte for each byte
        for (uint i = 0; i < 4; i++)
        {
            opcode = disassembler.next();
            ARM64_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE);
            ARM64_ASSERT(opcode->getOpcodeAddress(address) && (address.getAddress() == 0x400004 + i));
        }
        opcode = disassembler.next();
        ARM64_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "ret");
        ARM64_ASSERT(opcode->isBranch());
        ARM64_ASSERT((opcode->getAlterProperty() & Opcode::FLOW_RET) != 0);
        ARM64_ASSERT(disassembler.isEndOfStream());

        // Without the fault tolerance the invalid word is an exception
        cMemoryStream stream2(data);
        ArmStreamDisassembler strict(ArmInstructionSet::ARM_A64,
            BasicInputPtr(&stream2, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0), false);
        strict.next();
        bool isThrown = false;
        XSTL_TRY
        {
            strict.next();
        }
        XSTL_CATCH(DisassemblerInvalidOpcodeException&)
        {
            isThrown = true;
        }
        ARM64_ASSERT(isThrown);

        // jumpToAddress goes back to the first instruction
        strict.jumpToAddress(ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0), 0);
        opcode = strict.next();
        ARM64_ASSERT(opcode->getOpcodeSize() == 4);

        // A truncated instruction
        const uint8 half[] = { 0x20, 0x00 };
        cBuffer halfData(half, sizeof(half));
        cMemoryStream stream3(halfData);
        ArmStreamDisassembler truncated(ArmInstructionSet::ARM_A64,
            BasicInputPtr(&stream3, SMARTPTR_DESTRUCT_NONE), false,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0), false);
        isThrown = false;
        XSTL_TRY
        {
            truncated.next();
        }
        XSTL_CATCH(DisassemblerEndOfStreamException&)
        {
            isThrown = true;
        }
        ARM64_ASSERT(isThrown);
    }

    // The branch targets: absolute addresses, the relative forms
    void testBranches()
    {
        bool isValid;
        // b .+8 at 0x1000 is 0x14000002
        ARM64_ASSERT(disassemble(0x14000002, 0x1000, ArmFeatureSet::all(), isValid) == "b 0x1008");
        ARM64_ASSERT(assemble("b .+8", 0x1000, ArmFeatureSet::all(), isValid) == 0x14000002);
        ARM64_ASSERT(assemble("b #8", 0x1000, ArmFeatureSet::all(), isValid) == 0x14000002);
        ARM64_ASSERT(assemble("b 0x1008", 0x1000, ArmFeatureSet::all(), isValid) == 0x14000002);
        ARM64_ASSERT(assemble("bl .-4", 0x1000, ArmFeatureSet::all(), isValid) == 0x97ffffff);
        ARM64_ASSERT(disassemble(0x54000040, 0x2000, ArmFeatureSet::all(), isValid) == "b.eq 0x2008");
        ARM64_ASSERT(assemble("b.eq .+8", 0x2000, ArmFeatureSet::all(), isValid) == 0x54000040);
        ARM64_ASSERT(assemble("b.cs .+8", 0x2000, ArmFeatureSet::all(), isValid) == 0x54000042);
        ARM64_ASSERT(disassemble(0x90000000, 0x1234, ArmFeatureSet::all(), isValid) == "adrp x0, 0x1000");
        ARM64_ASSERT(assemble("adrp x0, #4096", 0x1234, ArmFeatureSet::all(), isValid) == 0xb0000000);
        // Out of range and misaligned offsets are rejected
        assemble("b .+2", 0x1000, ArmFeatureSet::all(), isValid);
        ARM64_ASSERT(!isValid);
        assemble("b .+134217728", 0x1000, ArmFeatureSet::all(), isValid);
        ARM64_ASSERT(!isValid);
    }

    // The sizes of the registers, the immediates and the aliases
    void testAssembler()
    {
        bool isValid;
        ARM64_ASSERT(assemble("add x0, x1, x2", 0, ArmFeatureSet::all(), isValid) == 0x8b020020);
        ARM64_ASSERT(assemble("ADD X0, X1, X2", 0, ArmFeatureSet::all(), isValid) == 0x8b020020);
        ARM64_ASSERT(assemble("add x0, x1, x2 // comment", 0, ArmFeatureSet::all(), isValid) == 0x8b020020);
        ARM64_ASSERT(assemble("add w0, x1, x2", 0, ArmFeatureSet::all(), isValid) == 0 && !isValid);
        ARM64_ASSERT(assemble("add x0, x1, #4096", 0, ArmFeatureSet::all(), isValid) == 0x91400420);
        ARM64_ASSERT(assemble("add x0, x1, #1, lsl #12", 0, ArmFeatureSet::all(), isValid) == 0x91400420);
        ARM64_ASSERT(assemble("add x0, x1, #4097", 0, ArmFeatureSet::all(), isValid) == 0 && !isValid);
        ARM64_ASSERT(assemble("mov x0, x1", 0, ArmFeatureSet::all(), isValid) == 0xaa0103e0);
        ARM64_ASSERT(assemble("orr x0, xzr, x1", 0, ArmFeatureSet::all(), isValid) == 0xaa0103e0);
        ARM64_ASSERT(assemble("mov x0, #1", 0, ArmFeatureSet::all(), isValid) == 0xd2800020);
        ARM64_ASSERT(assemble("mov x0, #-1", 0, ArmFeatureSet::all(), isValid) == 0x92800000);
        ARM64_ASSERT(assemble("mov x0, #0x5555555555555555", 0, ArmFeatureSet::all(), isValid) == 0xb200f3e0);
        ARM64_ASSERT(assemble("cmp x0, #5", 0, ArmFeatureSet::all(), isValid) == 0xf100141f);
        ARM64_ASSERT(assemble("lsl x0, x1, #3", 0, ArmFeatureSet::all(), isValid) == 0xd37df020);
        ARM64_ASSERT(assemble("ubfx x0, x1, #4, #8", 0, ArmFeatureSet::all(), isValid) == 0xd3442c20);
        ARM64_ASSERT(assemble("ldr x0, [x1, #8]", 0, ArmFeatureSet::all(), isValid) == 0xf9400420);
        ARM64_ASSERT(assemble("ldr x0, [x1, #16]!", 0, ArmFeatureSet::all(), isValid) == 0xf8410c20);
        ARM64_ASSERT(assemble("ldr x0, [x1], #16", 0, ArmFeatureSet::all(), isValid) == 0xf8410420);
        ARM64_ASSERT(assemble("ldr x0, [x1, x2, lsl #3]", 0, ArmFeatureSet::all(), isValid) == 0xf8627820);
        ARM64_ASSERT(assemble("ldr x0, [x1, w2, uxtw #3]", 0, ArmFeatureSet::all(), isValid) == 0xf8625820);
        ARM64_ASSERT(assemble("ldr x0, [x1, #4]", 0, ArmFeatureSet::all(), isValid) == 0 && !isValid);
        ARM64_ASSERT(assemble("stp x29, x30, [sp, #-16]!", 0, ArmFeatureSet::all(), isValid) == 0xa9bf7bfd);
        ARM64_ASSERT(assemble("ldp x29, x30, [sp], #16", 0, ArmFeatureSet::all(), isValid) == 0xa8c17bfd);
        ARM64_ASSERT(assemble("csel x0, x1, x2, ne", 0, ArmFeatureSet::all(), isValid) == 0x9a821020);
        ARM64_ASSERT(assemble("cset w0, eq", 0, ArmFeatureSet::all(), isValid) == 0x1a9f17e0);
        ARM64_ASSERT(assemble("dmb ish", 0, ArmFeatureSet::all(), isValid) == 0xd5033bbf);
        ARM64_ASSERT(assemble("msr nzcv, x0", 0, ArmFeatureSet::all(), isValid) == 0xd51b4200);
        ARM64_ASSERT(assemble("mrs x0, tpidr_el0", 0, ArmFeatureSet::all(), isValid) == 0xd53bd040);
        ARM64_ASSERT(assemble("mrs x0, s3_3_c13_c0_2", 0, ArmFeatureSet::all(), isValid) == 0xd53bd040);
        ARM64_ASSERT(assemble("nop", 0, ArmFeatureSet::all(), isValid) == 0xd503201f);
        ARM64_ASSERT(assemble("ret", 0, ArmFeatureSet::all(), isValid) == 0xd65f03c0);
        ARM64_ASSERT(assemble("ret x30", 0, ArmFeatureSet::all(), isValid) == 0xd65f03c0);
        ARM64_ASSERT(assemble("ret x1", 0, ArmFeatureSet::all(), isValid) == 0xd65f0020);
        ARM64_ASSERT(assemble("frobnicate x0", 0, ArmFeatureSet::all(), isValid) == 0 && !isValid);
        ARM64_ASSERT(assemble("", 0, ArmFeatureSet::all(), isValid) == 0 && !isValid);
    }

    // The assembler on a FirstPassBinary: text lines, a prologue / epilogue and
    // a branch to a label
    void testFirstPassBinary()
    {
        FirstPassBinaryPtr binary(new FirstPassBinary(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE, false));
        binary->createNewBlockWithoutChange(0, StackInterfacePtr(new TempClassInterface(binary)));
        binary->changeBasicBlock(0);
        AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                        binary, OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        {
            ArmAssemblerStream compiler(*binary, ArmInstructionSet::ARM_A64);
            compiler << "stp x29, x30, [sp, #-16]!" << endl;
            compiler << "mov x29, sp" << endl;
            compiler << "bl callee" << endl;
            compiler << "ldp x29, x30, [sp], #16" << endl;
            compiler << "ret" << endl;
            compiler.assemble();
        }
        const cBuffer& code = binary->getCurrentBlockData();
        ARM64_ASSERT(code.getSize() == 20);
        // stp x29, x30, [sp, #-16]! = a9bf7bfd (little endian)
        ARM64_ASSERT(code[0] == 0xfd && code[1] == 0x7b && code[2] == 0xbf && code[3] == 0xa9);
        // ret = d65f03c0
        ARM64_ASSERT(code[16] == 0xc0 && code[17] == 0x03 && code[18] == 0x5f && code[19] == 0xd6);
        // bl callee: the offset is left 0 and callee is a dependency
        ARM64_ASSERT(code[8] == 0 && code[9] == 0 && code[10] == 0 && code[11] == 0x94);
        ARM64_ASSERT(binary->getCurrentDependecies().getList().length() == 1);

        // Through the interface: the stream of the factory
        cStringerStreamPtr stream = assembler->getAssembler();
        (*stream) << "nop" << endl;
        stream = cStringerStreamPtr();
        ARM64_ASSERT(binary->getCurrentBlockData().getSize() == 24);
    }

    // The flow properties and the operands
    void testOpcodes()
    {
        const uint32 words[] = { 0x14000002 /* b */, 0x94000002 /* bl */, 0xd61f0020 /* br x1 */,
                                 0x54000040 /* b.eq */, 0xd65f03c0 /* ret */, 0x8b020020 /* add */ };
        const bool isBranch[] = { true, true, true, true, true, false };
        for (uint i = 0; i < 6; i++)
        {
            ArmOpcodePtr opcode = ArmStreamDisassembler::decode(
                ArmInstructionSet::ARM_A64, words[i], 4, 0x1000, ArmFeatureSet::all());
            ARM64_ASSERT(!opcode.isEmpty());
            ARM64_ASSERT(opcode->isBranch() == isBranch[i]);
            ARM64_ASSERT(opcode->getEncoding() == words[i]);
        }
        ArmOpcodePtr add = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x8b020020, 4, 0x1000, ArmFeatureSet::all());
        ARM64_ASSERT(add->getOperandsCount() == 3);
        ARM64_ASSERT(add->getOperand(0).m_kind == ArmDecodedOperand::KIND_REGISTER);
        ARM64_ASSERT(add->getOperand(0).m_register == armdis::ARM64_REG_X0);
        ARM64_ASSERT(add->getOperand(1).m_register == armdis::ARM64_REG_X0 + 1);
        ARM64_ASSERT(add->getOperand(2).m_kind == ArmDecodedOperand::KIND_SHIFTED_REGISTER);
        ARM64_ASSERT(add->getOperand(2).m_register == armdis::ARM64_REG_X0 + 2);
        ARM64_ASSERT(add->getMnemonic() == "add");
        ARM64_ASSERT(add->getCondition() == ArmOpcode::CONDITION_ALWAYS);

        ArmOpcodePtr beq = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x54000040, 4, 0x1000, ArmFeatureSet::all());
        ARM64_ASSERT(beq->getMnemonic() == "b.eq");
        ARM64_ASSERT(beq->getCondition() == 0);
        ARM64_ASSERT(beq->getOperand(0).m_kind == ArmDecodedOperand::KIND_BRANCH_TARGET);
        ARM64_ASSERT(beq->getOperand(0).m_immediate == 0x1008);

        // The big endian subsystems and the instruction sets
        ARM64_ASSERT(ArmInstructionSet::fromSubsystem(OpcodeSubsystems::DISASSEMBLER_ARM_64_BE) ==
                     ArmInstructionSet::ARM_A64);
        ARM64_ASSERT(ArmInstructionSet::fromSubsystem(OpcodeSubsystems::DISASSEMBLER_THUMB_LE) ==
                     ArmInstructionSet::ARM_T32);
        ARM64_ASSERT(ArmInstructionSet::getInstructionAlignment(ArmInstructionSet::ARM_T32) == 2);
        ARM64_ASSERT(OpcodeSubsystems::isLittleEndian(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE));
        ARM64_ASSERT(!OpcodeSubsystems::isLittleEndian(OpcodeSubsystems::DISASSEMBLER_ARM_64_BE));

        // The A32 and T32 tables are empty: nothing is decoded yet
        ARM64_ASSERT(ArmStreamDisassembler::decode(ArmInstructionSet::ARM_A32, 0xe1a00000, 4, 0,
                                                   ArmFeatureSet::all()).isEmpty());
        ARM64_ASSERT(ArmStreamDisassembler::decode(ArmInstructionSet::ARM_T32, 0x4600, 2, 0,
                                                   ArmFeatureSet::all()).isEmpty());
    }

    // The factory of the stream disassembler
    void testFactory()
    {
        const uint8 code[] = { 0x1f, 0x20, 0x03, 0xd5 };
        cBuffer data(code, sizeof(code));
        cMemoryStream stream(data);
        StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
            OpcodeSubsystems::DISASSEMBLER_ARM_64_LE,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0x1000));
        OpcodePtr opcode = disassembler->next();
        DefaultOpcodeDataFormatter formatter(1);
        ARM64_ASSERT(disassembler->getOpcodeFormat(opcode, formatter)->string() == "nop");
        ARM64_ASSERT(disassembler->getType() == OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
    }

    virtual void test()
    {
        testRows();
        testInvalid();
        testFeatures();
        testStream();
        testBranches();
        testAssembler();
        testFirstPassBinary();
        testOpcodes();
        testFactory();
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestArm64Disassembler g_globalTestArm64Disassembler;
