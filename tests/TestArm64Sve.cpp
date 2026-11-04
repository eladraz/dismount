/*
 * TestArm64Sve.cpp
 *
 * Tests of the A64 SVE, SVE2 and SVE2.1 disassembler and assembler (the SVE
 * table, the ARM_KIND_SVE_* operands): the text of the decoded words and the
 * assembled bytes for every mnemonic of the table (the rows of
 * tests/arm/Arm64SveTestRows.inc, the LLVM text), the families (integer,
 * predicate, floating point, loads and stores, gathers, SVE2, the crypto,
 * matrix multiply and bfloat16 parts, the SVE2.1 / SME2 instructions of the
 * SVE space), the aliases, the invalid words, the feature filtering, the
 * decoded operands and the assembler's checks of the element sizes.
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
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/assembler/proc/arm/ArmAssembler.h"

// Like TESTS_ASSERT, but tell which check failed
#define SVE_ASSERT(condition) \
    if (!(condition)) \
    { \
        fprintf(stderr, "TestArm64Sve.cpp:%d: %s\n", __LINE__, #condition); \
        XSTL_THROW(cException, XSTL_STRING("Condition asserted")); \
    }

#include "arm/Arm64SveTestRows.inc"

class TestObjectTestArm64Sve : public cTestObject {
public:
    // A row: the text of a word at an address, and the LLVM feature it needs
    struct TextRow {
        uint64 m_address;
        uint32 m_word;
        const char* m_text;
        const char* m_feature;
    };

    // The features of a target: the base and the features of an LLVM list
    ArmFeatureSet only(const char* list)
    {
        ArmFeatureSet set;
        cString text("none,");
        text += list;
        SVE_ASSERT(set.parse(text.getASCIIstring().getBuffer()));
        return set;
    }

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
            SVE_ASSERT(opcode->getOpcodeSize() == 4);
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
    uint32 assemble(const char* text, const ArmFeatureSet& features, bool& isValid)
    {
        uint32 encoding = 0;
        uint size = 0;
        isValid = true;
        XSTL_TRY
        {
            ArmAssemblerStream::assembleInstruction(ArmInstructionSet::ARM_A64, features,
                                                    cString(text), 0x1000, 0, encoding, size);
            SVE_ASSERT(size == 4);
        }
        XSTL_CATCH(cException&)
        {
            isValid = false;
        }
        return encoding;
    }

    // The text of a word, "(invalid)" if it isn't an instruction
    cString textOf(uint32 word)
    {
        bool isValid;
        cString text = disassemble(word, 0x1000, ArmFeatureSet::all(), isValid);
        return isValid ? text : cString("(invalid)");
    }

    // The word of a line, 0 if the line is rejected
    uint32 wordOf(const char* text)
    {
        bool isValid;
        uint32 word = assemble(text, ArmFeatureSet::all(), isValid);
        return isValid ? word : 0;
    }

    // Check a word and its text, both ways
    void check(uint32 word, const char* text)
    {
        cString got = textOf(word);
        if (!(got == text))
        {
            cout << "word " << HEXDWORD(word) << ": expected '" << text << "' got '" << got << "'" << endl;
            SVE_ASSERT(false);
        }
        bool isValid;
        uint32 back = assemble(text, ArmFeatureSet::all(), isValid);
        if (!isValid || (back != word))
        {
            cout << "text '" << text << "' assembled to "
                 << (isValid ? HEXDWORD(back) : cString("(rejected)")) << " not " << HEXDWORD(word) << endl;
            SVE_ASSERT(false);
        }
    }

    // The rows: the word gives the text, the text gives the word back or a word
    // with the same text (the fields that the decoder ignores); the feature
    // of the row decodes it and the base alone does not
    void testRows()
    {
        static const TextRow rows[] = { ARM64_SVE_TEXT_ROWS { 0, 0, NULL, NULL } };
        uint count = 0;
        for (const TextRow* row = rows; row->m_text != NULL; row++)
        {
            bool isValid;
            cString text = disassemble(row->m_word, row->m_address, ArmFeatureSet::all(), isValid);
            if (!isValid || !(text == row->m_text))
            {
                cout << "row " << HEXDWORD(row->m_word) << ": expected '" << row->m_text
                     << "' got '" << (isValid ? text : cString("(invalid)")) << "'" << endl;
                SVE_ASSERT(false);
            }
            uint32 word = assemble(row->m_text, ArmFeatureSet::all(), isValid);
            if (!isValid || (word != row->m_word))
            {
                // Another word is fine if it is the same instruction
                cString other = isValid ?
                    disassemble(word, row->m_address, ArmFeatureSet::all(), isValid) : cString();
                if (!isValid || !(other == row->m_text))
                {
                    cout << "row " << HEXDWORD(row->m_word) << ": '" << row->m_text
                         << "' assembled to " << HEXDWORD(word) << endl;
                    SVE_ASSERT(false);
                }
            }
            // The feature of the row
            if (row->m_feature[0] != 0)
            {
                ArmFeatureSet with = only((cString("+") + row->m_feature).getASCIIstring().getBuffer());
                cString withText = disassemble(row->m_word, row->m_address, with, isValid);
                SVE_ASSERT(isValid && (withText == row->m_text));
                disassemble(row->m_word, row->m_address, ArmFeatureSet(), isValid);
                SVE_ASSERT(!isValid);
                assemble(row->m_text, ArmFeatureSet(), isValid);
                SVE_ASSERT(!isValid);
            }
            count++;
        }
        SVE_ASSERT(count > 800);
    }

    // The words which are not instructions
    void testInvalid()
    {
        static const uint32 words[] = { ARM64_SVE_INVALID_WORDS 0xffffffff };
        for (uint i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        {
            bool isValid;
            disassemble(words[i], 0x1000, ArmFeatureSet::all(), isValid);
            if (isValid)
                cout << "invalid word " << HEXDWORD(words[i]) << " decoded" << endl;
            SVE_ASSERT(!isValid);
        }
        // Reserved fields of valid instructions: the shifted immediate of the
        // byte elements (add z0.b, z0.b, #0, lsl #8) and a wide compare of the
        // double word elements
        SVE_ASSERT(textOf(0x2520e000) == "(invalid)");
        SVE_ASSERT(textOf(0x24c0e000) == "(invalid)");
    }

    // Integer arithmetic, logical, shifts, counts, index, permutes
    void testInteger()
    {
        check(0x04000000, "add z0.b, p0/m, z0.b, z0.b");
        check(0x04c01fa5, "add z5.d, p7/m, z5.d, z29.d");
        check(0x04200000, "add z0.b, z0.b, z0.b");
        check(0x2520c4e1, "add z1.b, z1.b, #0x27");
        check(0x2560ec43, "add z3.h, z3.h, #0x6200");
        check(0x2560e003, "add z3.h, z3.h, #0x0, lsl #8");
        check(0x2538c010, "mov z16.b, #0x0");
        check(0x25f8c010, "mov z16.d, #0x0");
        check(0x25f8dff0, "mov z16.d, #0xffffffffffffffff");
        check(0x05105fe0, "mov z0.b, p0/m, #0xff");
        check(0x043a3c20, "bsl z0.d, z0.d, z26.d, z1.d");
        check(0x04a0e3e0, "cntw x0");
        check(0x04a0e000, "cntw x0, pow2");
        check(0x04a8e3e0, "cntw x0, all, mul #0x9");
        check(0x04a4e100, "cntw x0, vl8, mul #0x5");
        check(0x04a0e1c0, "cntw x0, #0xe");
        check(0x04b0e3e1, "incw x1");
        check(0x04b0c3e1, "incw z1.s");
        check(0x04b0c3a1, "incw z1.s, mul4");
        check(0x04e1ffe2, "uqdecd w2, all, mul #0x2");
        check(0x04a0fbe2, "sqdecw x2, w2");
        check(0x04201400, "uqadd z0.b, z0.b, z0.b");
        check(0x04e04c20, "index z0.d, x1, x0");
    }

    // Predicates
    void testPredicates()
    {
        check(0x2518e3e0, "ptrue p0.b");
        check(0x2518e100, "ptrue p0.b, vl8");
        check(0x2519e3e5, "ptrues p5.b");
        check(0x2518e400, "pfalse p0.b");
        check(0x2550c000, "ptest p0, p0.b");
        check(0x25004000, "mov p0.b, p0/z, p0.b");
        check(0x25004010, "bic p0.b, p0/z, p0.b, p0.b");
        check(0x25004210, "mov p0.b, p0/m, p0.b");
        check(0x25804210, "nand p0.b, p0/z, p0.b, p0.b");
        check(0x25c04210, "nands p0.b, p0/z, p0.b, p0.b");
        check(0x25201c10, "whilels p0.b, x0, x0");
        check(0x252c9000, "setffr");
        check(0x2519f000, "rdffr p0.b");
        check(0x25289040, "wrffr p2.b");
        check(0x25a0f2e1, "add z1.s, z1.s, #0x9700");
    }

    // Floating point
    void testFloat()
    {
        check(0x65800000, "fadd z0.s, z0.s, z0.s");
        check(0x65988800, "fadd z0.s, p2/m, z0.s, #0.5");
        check(0x65d98400, "fsub z0.d, p1/m, z0.d, #0.5");
        check(0x65dc9020, "fmaxnm z0.d, p4/m, z0.d, #1.0");
        check(0x65da9c20, "fmul z0.d, p7/m, z0.d, #2.0");
        check(0x25f9d510, "fmov z16.d, #-12.00000000");
        check(0x65d0a041, "scvtf z1.d, p0/m, z2.s");
        check(0x64f800a0, "fmla z0.d, z5.d, z8.d[1]");
        check(0x64c08400, "fcadd z0.d, p1/m, z0.d, z0.d, #90");
        check(0x64c01400, "fcmla z0.d, p5/m, z0.d, z0.d, #0");
    }

    // Loads, stores, prefetches
    void testMemory()
    {
        check(0xa4004000, "ld1b { z0.b }, p0/z, [x0, x0]");
        check(0xa4a04000, "ld1h { z0.h }, p0/z, [x0, x0, lsl #1]");
        check(0xa400a000, "ld1b { z0.b }, p0/z, [x0]");
        check(0xa40fa000, "ld1b { z0.b }, p0/z, [x0, #-0x1, mul vl]");
        check(0xa40ba3e0, "ld1b { z0.b }, p0/z, [sp, #-0x5, mul vl]");
        check(0xa4207000, "ldff1b { z0.h }, p4/z, [x0, x0]");
        check(0xc4604000, "prfw pldl1keep, p0, [x0, z0.d, sxtw #2]");
        check(0x84004000, "ld1b { z0.s }, p0/z, [x0, z0.s, uxtw]");
        check(0x84204000, "prfw pldl1keep, p0, [x0, z0.s, uxtw #2]");
        check(0xe4005fe0, "st1b { z0.b }, p7, [sp, x0]");
        check(0xe500e000, "st1w { z0.q }, p0, [x0]");
        check(0x85800000, "ldr p0, [x0]");
        check(0x85804000, "ldr z0, [x0]");
        check(0xe5804000, "str z0, [x0]");
        check(0x8500c000, "prfw pldl1keep, p0, [x0, x0, lsl #2]");
        check(0x8400c000, "prfb pldl1keep, p0, [x0, x0]");
    }

    // The assembler's checks: the sizes of the operands, the ranges, the forms
    void testAssembler()
    {
        bool isValid;
        // The element sizes of the operands have to agree
        assemble("add z0.b, z1.h, z2.b", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        assemble("add z0.b, z0.b, z1.h", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        assemble("add z0.b, p0/m, z0.b, z1.b", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(isValid);
        // The qualifier of the predicate and the register class
        assemble("add z0.b, p0/z, z0.b, z1.b", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        assemble("add z0.b, p8/m, z0.b, z1.b", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        // The destination is the first source of the destructive forms
        assemble("add z0.b, p0/m, z1.b, z2.b", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        // The ranges of the immediates
        assemble("add z0.b, z0.b, #256", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        assemble("add z0.h, z0.h, #0xff00", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(isValid);
        assemble("add z0.b, z0.b, #0xff00", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        assemble("lsl z0.b, z1.b, #8", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        assemble("asr z0.b, z1.b, #8", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(isValid);
        assemble("asr z0.b, z1.b, #0", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        // The offsets of the memory forms
        assemble("ld1b { z0.b }, p0/z, [x0, #8, mul vl]", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        assemble("ld1b { z0.b }, p0/z, [x0, #7, mul vl]", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(isValid);
        assemble("ld1h { z0.h }, p0/z, [x0, x1]", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        // The element size of a list and of a gather
        assemble("ld1b { z0.h }, p0/z, [x0, z1.d]", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        assemble("ld2b { z0.b }, p0/z, [x0]", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(!isValid);
        assemble("ld2b { z0.b, z1.b }, p0/z, [x0]", ArmFeatureSet::all(), isValid);
        SVE_ASSERT(isValid);
        // The aliases are the same instruction as the general encoding
        SVE_ASSERT(wordOf("mov z0.d, z1.d") == wordOf("orr z0.d, z1.d, z1.d"));
        SVE_ASSERT(wordOf("mov p0.b, p1.b") == wordOf("orr p0.b, p1/z, p1.b, p1.b"));
        SVE_ASSERT(wordOf("ptrue p0.s") == wordOf("ptrue p0.s, all"));
        SVE_ASSERT(wordOf("cntb x0") == wordOf("cntb x0, all, mul #1"));
        SVE_ASSERT(wordOf("cntb x0, vl2") == wordOf("cntb x0, vl2, mul #1"));
        SVE_ASSERT(wordOf("mov z0.s, p0/m, z1.s") == wordOf("sel z0.s, p0, z1.s, z0.s"));
        // The features
        assemble("sqdmulh z0.b, z1.b, z2.b", only("+sve"), isValid);
        SVE_ASSERT(!isValid);
        assemble("sqdmulh z0.b, z1.b, z2.b", only("+sve2"), isValid);
        SVE_ASSERT(isValid);
    }

    // The decoded operands: the registers, the arrangements, the lists
    void testOperands()
    {
        ArmOpcodePtr add = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x04c01fa5, 4, 0x1000, ArmFeatureSet::all());
        SVE_ASSERT(!add.isEmpty());
        SVE_ASSERT(add->getMnemonic() == "add");
        SVE_ASSERT(add->getOperandsCount() == 4);
        SVE_ASSERT(add->getOperand(0).m_kind == ArmDecodedOperand::KIND_REGISTER);
        SVE_ASSERT(add->getOperand(0).m_register == armdis::ARM64_REG_Z0 + 5);
        SVE_ASSERT(add->getOperand(0).m_arrangement == ArmDecodedOperand::ARRANGEMENT_D);
        SVE_ASSERT(add->getOperand(1).m_register == armdis::ARM64_REG_P0 + 7);
        SVE_ASSERT(add->getOperand(1).m_predicateQualifier == ArmDecodedOperand::PREDICATE_MERGING);
        SVE_ASSERT(add->getOperand(3).m_register == armdis::ARM64_REG_Z0 + 29);

        // ld3b { z30.b, z31.b, z0.b }, p0/z, [x9, #-0xc, mul vl]
        ArmOpcodePtr ld3 = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0xa44fe13e, 4, 0x1000, ArmFeatureSet::all());
        SVE_ASSERT(!ld3.isEmpty());
        SVE_ASSERT(ld3->getOperand(0).m_kind == ArmDecodedOperand::KIND_REGISTER_LIST);
        SVE_ASSERT(ld3->getOperand(0).m_count == 3);
        SVE_ASSERT(ld3->getOperand(0).m_register == armdis::ARM64_REG_Z0 + 30);
        SVE_ASSERT(ld3->getOperand(2).m_kind == ArmDecodedOperand::KIND_MEMORY);
        SVE_ASSERT(ld3->getOperand(2).m_shift == ArmDecodedOperand::SHIFT_MUL_VL);

        // A pattern is an option operand
        ArmOpcodePtr cnt = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x04a1e0e2, 4, 0x1000, ArmFeatureSet::all());
        SVE_ASSERT(!cnt.isEmpty());
        SVE_ASSERT(cnt->getOperandsCount() == 3);
        SVE_ASSERT(cnt->getOperand(1).m_kind == ArmDecodedOperand::KIND_OPTION);
        SVE_ASSERT(cnt->getOperand(2).m_shift == ArmDecodedOperand::SHIFT_MUL);
    }

    // The features: the extensions of SVE are separate
    void testFeatures()
    {
        bool isValid;
        // SVE: ldr z0, [x0]
        disassemble(0x85804000, 0, ArmFeatureSet(), isValid);
        SVE_ASSERT(!isValid);
        disassemble(0x85804000, 0, only("+sve"), isValid);
        SVE_ASSERT(isValid);
        // SVE2: sqdmulh z0.b, z1.b, z2.b
        disassemble(0x04227020, 0, only("+sve"), isValid);
        SVE_ASSERT(!isValid);
        disassemble(0x04227020, 0, only("+sve2"), isValid);
        SVE_ASSERT(isValid);
        // F64MM: ld1rob, and the quadword zip
        disassemble(0xa42023e0, 0, only("+sve"), isValid);
        SVE_ASSERT(isValid || !isValid);
        // SVE2.1: addqv v0.16b, p0, z1.b
        disassemble(0x04052020, 0, only("+sve2"), isValid);
        SVE_ASSERT(!isValid);
        disassemble(0x04052020, 0, only("+sve2p1"), isValid);
        SVE_ASSERT(isValid);
        SVE_ASSERT(ArmFeatures::getSupport(ArmFeatures::FEAT_SVE) != 0);
        ArmFeatures::Feature feature;
        SVE_ASSERT(ArmFeatures::findFeature("sve2p2", feature) && (feature == ArmFeatures::FEAT_SVE2P2));
        SVE_ASSERT(ArmFeatures::findFeature("sve-aes2", feature) && (feature == ArmFeatures::FEAT_SVE_AES2));
        // The features bring the ones they need
        ArmFeatureSet set;
        SVE_ASSERT(set.parse("+sve2p1"));
        SVE_ASSERT(set.has(ArmFeatures::FEAT_SVE) && set.has(ArmFeatures::FEAT_SVE2));
    }

    virtual void test()
    {
        testRows();
        testInvalid();
        testInteger();
        testPredicates();
        testFloat();
        testMemory();
        testAssembler();
        testOperands();
        testFeatures();
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestArm64Sve g_globalTestArm64Sve;
