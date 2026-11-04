/*
 * TestArm64Sme.cpp
 *
 * Tests of the A64 SME, SME2 and SME2.1 disassembler and assembler (the SME
 * table, the ARM_KIND_SME_* operands): the text of the decoded words and the
 * assembled bytes for every mnemonic of the table (the rows of
 * tests/arm/Arm64SmeTestRows.inc, the LLVM text), the families (the loads
 * and stores of the tiles, mova, zero, the outer products, the multi vector
 * instructions with the vector groups of za, the lookup tables of zt0), the
 * aliases, the invalid words, the feature filtering, the decoded operands and
 * the assembler's checks of the tiles, the slices and the vector groups.
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
#define SME_ASSERT(condition) \
    if (!(condition)) \
    { \
        fprintf(stderr, "TestArm64Sme.cpp:%d: %s\n", __LINE__, #condition); \
        XSTL_THROW(cException, XSTL_STRING("Condition asserted")); \
    }

#include "arm/Arm64SmeTestRows.inc"

class TestObjectTestArm64Sme : public cTestObject {
public:
    // A row: the text of a word at an address, and the LLVM features it needs
    // ("sme2,sme-i16i64", empty for none)
    struct TextRow {
        uint64 m_address;
        uint32 m_word;
        const char* m_text;
        const char* m_features;
    };

    // The features of a target: the base and the features of an LLVM list
    ArmFeatureSet only(const char* list)
    {
        ArmFeatureSet set;
        cString text("none,");
        text += list;
        SME_ASSERT(set.parse(text.getASCIIstring().getBuffer()));
        return set;
    }

    // The features of a row: "sme2,sme-i16i64" is "+sme2,+sme-i16i64"
    ArmFeatureSet rowFeatures(const char* features)
    {
        cString list;
        cString names(features);
        uint start = 0;
        for (uint i = 0; i <= names.length(); i++)
            if ((i == names.length()) || (names[i] == XSTL_CHAR(',')))
            {
                if (list.length() != 0)
                    list += ",";
                list += cString("+") + names.part(start, i);
                start = i + 1;
            }
        return only(list.getASCIIstring().getBuffer());
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
            SME_ASSERT(opcode->getOpcodeSize() == 4);
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
            SME_ASSERT(size == 4);
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

    // True if the line is rejected
    bool isRejected(const char* text)
    {
        bool isValid;
        assemble(text, ArmFeatureSet::all(), isValid);
        return !isValid;
    }

    // Check a word and its text, both ways
    void check(uint32 word, const char* text)
    {
        cString got = textOf(word);
        if (!(got == text))
        {
            cout << "word " << HEXDWORD(word) << ": expected '" << text << "' got '" << got << "'" << endl;
            SME_ASSERT(false);
        }
        bool isValid;
        uint32 back = assemble(text, ArmFeatureSet::all(), isValid);
        if (!isValid || (back != word))
        {
            cout << "text '" << text << "' assembled to "
                 << (isValid ? HEXDWORD(back) : cString("(rejected)")) << " not " << HEXDWORD(word) << endl;
            SME_ASSERT(false);
        }
    }

    // The rows: the word gives the text, the text gives the word back or a word
    // with the same text (the fields that the decoder ignores); the features
    // of the row decode it and the base alone does not
    void testRows()
    {
        static const TextRow rows[] = { ARM64_SME_TEXT_ROWS { 0, 0, NULL, NULL } };
        uint count = 0;
        for (const TextRow* row = rows; row->m_text != NULL; row++)
        {
            bool isValid;
            cString text = disassemble(row->m_word, row->m_address, ArmFeatureSet::all(), isValid);
            if (!isValid || !(text == row->m_text))
            {
                cout << "row " << HEXDWORD(row->m_word) << ": expected '" << row->m_text
                     << "' got '" << (isValid ? text : cString("(invalid)")) << "'" << endl;
                SME_ASSERT(false);
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
                    SME_ASSERT(false);
                }
            }
            // The features of the row
            if (row->m_features[0] != 0)
            {
                ArmFeatureSet with = rowFeatures(row->m_features);
                cString withText = disassemble(row->m_word, row->m_address, with, isValid);
                if (!isValid || !(withText == row->m_text))
                {
                    cout << "row " << HEXDWORD(row->m_word) << " is not decoded with " << row->m_features << endl;
                    SME_ASSERT(false);
                }
                disassemble(row->m_word, row->m_address, ArmFeatureSet(), isValid);
                SME_ASSERT(!isValid);
                assemble(row->m_text, ArmFeatureSet(), isValid);
                SME_ASSERT(!isValid);
            }
            count++;
        }
        SME_ASSERT(count > 300);
    }

    // The words which are not instructions
    void testInvalid()
    {
        static const uint32 words[] = { ARM64_SME_INVALID_WORDS 0xffffffff };
        for (uint i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        {
            bool isValid;
            disassemble(words[i], 0x1000, ArmFeatureSet::all(), isValid);
            if (isValid)
                cout << "invalid word " << HEXDWORD(words[i]) << " decoded" << endl;
            SME_ASSERT(!isValid);
        }
        // Reserved fields of valid instructions: the tile of the byte slices,
        // the unused offset bits of the multi slice moves, the quadword with
        // a size, fmops of FP8
        SME_ASSERT(textOf(0xe0000010) == "(invalid)");
        SME_ASSERT(textOf(0xc0040404) == "(invalid)");
        SME_ASSERT(textOf(0xc0430000) == "(invalid)");
        SME_ASSERT(textOf(0xc0080100) == "(invalid)");
    }

    // The loads, stores and moves of the tiles (SME)
    void testTiles()
    {
        check(0xe0092ce5, "ld1b {za0h.b[w13, 5]}, p3/z, [x7, x9]");
        check(0xe084d469, "ld1w {za2v.s[w14, 1]}, p5/z, [x3, x4, lsl #2]");
        check(0xe01f0000, "ld1b {za0h.b[w12, 0]}, p0/z, [x0]");
        check(0xe1df0000, "ld1q {za0h.q[w12, 0]}, p0/z, [x0]");
        check(0xe0400000, "ld1h {za0h.h[w12, 0]}, p0/z, [x0, x0, lsl #1]");
        check(0xe1002065, "ldr za[w13, 5], [x3, #0x5, mul vl]");
        check(0xe1000000, "ldr za[w12, 0], [x0]");
        check(0xe1200000, "str za[w12, 0], [x0]");
        check(0xe11f8000, "ldr zt0, [x0]");
        check(0xe13f8000, "str zt0, [x0]");
        check(0xc080ad25, "mov za1v.s[w13, 1], p3/m, z9.s");
        check(0xc082aca9, "mov z9.s, p3/m, za1v.s[w13, 1]");
        check(0xc0c30000, "mov z0.q, p0/m, za0h.q[w12, 0]");
        check(0xc0c10003, "mov za3h.q[w12, 0], p0/m, z0.q");
        check(0xc00203ff, "movaz z31.b, za0h.b[w12, 15]");
        check(0xc0900000, "addha za0.s, p0/m, p0/m, z0.s");
        check(0xc0d10000, "addva za0.d, p0/m, p0/m, z0.d");
        check(0xc0040800, "mov za.d[w8, 0, vgx2], { z0.d, z1.d }");
        check(0xc0440402, "mov za1h.h[w12, 0x0:0x3], { z0.h - z3.h }");
        check(0xc0462664, "movaz { z4.h - z7.h }, za1h.h[w13, 0x4:0x7]");
    }

    // zero: the mask is written with the biggest tiles
    void testZero()
    {
        check(0xc0080000, "zero {}");
        check(0xc00800ff, "zero {za}");
        check(0xc0080001, "zero {za0.d}");
        check(0xc0080011, "zero {za0.s}");
        check(0xc0080033, "zero {za0.s,za1.s}");
        check(0xc0080055, "zero {za0.h}");
        check(0xc00800aa, "zero {za1.h}");
        check(0xc0080003, "zero {za0.d, za1.d}");
        check(0xc00800c3, "zero {za0.d, za1.d, za6.d, za7.d}");
        check(0xc0480001, "zero { zt0 }");
        check(0xc00c2003, "zero za.d[w9, 3, vgx2]");
        check(0xc00ea003, "zero za.d[w9, 0xc:0xf]");
        check(0xc00fa001, "zero za.d[w9, 0x4:0x7, vgx4]");
        SME_ASSERT(wordOf("zero {za0.s}") == wordOf("zero {za0.d, za4.d}"));
        SME_ASSERT(wordOf("zero {za0.b}") == wordOf("zero {za}"));
        SME_ASSERT(wordOf("zero {za1.h}") == 0xc00800aa);
    }

    // The outer products
    void testOuterProducts()
    {
        check(0x80800000, "fmopa za0.s, p0/m, p0/m, z0.s, z0.s");
        check(0x80c00000, "fmopa za0.d, p0/m, p0/m, z0.d, z0.d");
        check(0x80c00010, "fmops za0.d, p0/m, p0/m, z0.d, z0.d");
        check(0x81a00000, "fmopa za0.s, p0/m, p0/m, z0.h, z0.h");
        check(0x81800000, "bfmopa za0.s, p0/m, p0/m, z0.h, z0.h");
        check(0xa0800000, "smopa za0.s, p0/m, p0/m, z0.b, z0.b");
        check(0xa0c00000, "smopa za0.d, p0/m, p0/m, z0.h, z0.h");
        check(0xa1a00000, "umopa za0.s, p0/m, p0/m, z0.b, z0.b");
        check(0xa0a00000, "sumopa za0.s, p0/m, p0/m, z0.b, z0.b");
        check(0xa1800000, "usmopa za0.s, p0/m, p0/m, z0.b, z0.b");
        check(0x80800008, "bmopa za0.s, p0/m, p0/m, z0.s, z0.s");
        check(0x81000000, "bfmop4a za0.s, z0.h, z16.h");
        check(0x81008000, "usmop4a za0.s, z0.b, z16.b");
        check(0x80000000, "fmop4a za0.s, z0.s, z16.s");
        check(0x80400000, "ftmopa za0.s, { z0.s, z1.s }, z0.s, z20[0]");
        check(0x80408000, "stmopa za0.s, { z0.b, z1.b }, z0.b, z20[0]");
        // The features of the outer products
        bool isValid;
        disassemble(0x80c00000, 0, only("+sme"), isValid);
        SME_ASSERT(!isValid);
        disassemble(0x80c00000, 0, only("+sme-f64f64"), isValid);
        SME_ASSERT(isValid);
        disassemble(0x80800008, 0, only("+sme"), isValid);
        SME_ASSERT(!isValid);
        disassemble(0x80800008, 0, only("+sme2"), isValid);
        SME_ASSERT(isValid);
    }

    // The multi vector instructions with the vector groups of za
    void testMultiVector()
    {
        check(0xc1a03cd3, "add za.s[w9, 3, vgx2], { z6.s, z7.s }");
        check(0xc12738d3, "add za.s[w9, 3, vgx2], { z6.s, z7.s }, z7.s");
        check(0xc1a838d3, "add za.s[w9, 3, vgx2], { z6.s, z7.s }, { z8.s, z9.s }");
        check(0xc1a93893, "add za.s[w9, 3, vgx4], { z4.s - z7.s }, { z8.s - z11.s }");
        check(0xc1373893, "add za.s[w9, 3, vgx4], { z4.s - z7.s }, z7.s");
        check(0xc1301bff, "sub za.s[w8, 7, vgx4], { z31.s, z0.s, z1.s, z2.s }, z0.s");
        check(0xc1153cc3, "fmla za.h[w9, 3, vgx2], { z6.h, z7.h }, z5.h[6]");
        check(0xc15528c3, "fmla za.s[w9, 3, vgx2], { z6.s, z7.s }, z5.s[2]");
        check(0xc1d524c3, "fmla za.d[w9, 3, vgx2], { z6.d, z7.d }, z5.d[1]");
        check(0xc15538eb, "usdot za.s[w9, 3, vgx2], { z6.b, z7.b }, z5.b[2]");
        check(0xc185b8c1, "fmlal za.s[w9, 0x2:0x3], z6.h, z5.h[6]");
        check(0xc1952ce5, "fmlall za.s[w9, 0x4:0x7, vgx2], { z6.b, z7.b }, z5.b[14]");
        check(0xc120a300, "add { z0.b, z1.b }, { z0.b, z1.b }, z0.b");
        check(0xc1208000, "sel { z0.b, z1.b }, pn8, { z0.b, z1.b }, { z0.b, z1.b }");
        check(0xc120c000, "bfclamp { z0.h, z1.h }, z0.h, z0.h");
        check(0xc120d000, "zip { z0.b, z1.b }, z0.b, z0.b");
        check(0xc160d800, "sqrshr z0.b, { z0.s - z3.s }, #0x20");
        check(0xc1a0d800, "sqrshr z0.h, { z0.d - z3.d }, #0x40");
        check(0xc123e000, "sqcvt z0.h, { z0.s, z1.s }");
        check(0xc165e000, "sunpk { z0.h, z1.h }, z0.b");
        // The two operands which are one register
        SME_ASSERT(isRejected("add { z0.b, z1.b }, { z2.b, z3.b }, z0.b"));
        // The features of the instructions with two
        bool isValid;
        disassemble(0xc1e03cd3, 0, only("+sme2"), isValid);
        SME_ASSERT(!isValid);
        disassemble(0xc1e03cd3, 0, only("+sme-i16i64"), isValid);
        SME_ASSERT(!isValid);
        disassemble(0xc1e03cd3, 0, only("+sme2,+sme-i16i64"), isValid);
        SME_ASSERT(isValid);
        // "sme-f16f16 or sme-f8f16"
        disassemble(0xc1a43cc3, 0, only("+sme2"), isValid);
        SME_ASSERT(!isValid);
        disassemble(0xc1a43cc3, 0, only("+sme-f16f16"), isValid);
        SME_ASSERT(isValid);
        disassemble(0xc1a43cc3, 0, only("+sme-f8f16"), isValid);
        SME_ASSERT(isValid);
        // ... and the assembler takes either
        assemble("fadd za.h[w9, 3, vgx2], { z6.h, z7.h }", only("+sme2"), isValid);
        SME_ASSERT(!isValid);
        SME_ASSERT(wordOf("fadd za.h[w9, 3, vgx2], { z6.h, z7.h }") == 0xc1a43cc3);
        SME_ASSERT(assemble("fadd za.h[w9, 3, vgx2], { z6.h, z7.h }", only("+sme-f16f16"), isValid) == 0xc1a43cc3);
        SME_ASSERT(isValid);
        SME_ASSERT(assemble("fadd za.h[w9, 3, vgx2], { z6.h, z7.h }", only("+sme-f8f16"), isValid) == 0xc1a43cc3);
        SME_ASSERT(isValid);
    }

    // The lookup tables
    void testTables()
    {
        check(0xc08c4000, "luti2 { z0.b, z1.b }, zt0, z0[0]");
        check(0xc0ca0000, "luti4 z0.b, zt0, z0[0]");
        check(0xc09c8000, "luti2 { z0.b, z4.b, z8.b, z12.b }, zt0, z0[0]");
        check(0xc04c03ff, "movt xzr, zt0[0]");
        check(0xc04e43ff, "movt zt0[32], xzr");
        check(0xc04f13ff, "movt zt0[1, mul vl], z31");
        check(0xc04f03ff, "movt zt0, z31");
        check(0xc0c84000, "luti6 z0.b, zt0, z0");
    }

    // The assembler's checks: the tiles, the slices, the vector groups, the aliases
    void testAssembler()
    {
        bool isValid;
        // mova is the name of mov
        SME_ASSERT(wordOf("mova za0h.b[w12, 0], p0/m, z0.b") == wordOf("mov za0h.b[w12, 0], p0/m, z0.b"));
        SME_ASSERT(wordOf("mova z0.s, p0/m, za0v.s[w12, 0]") == 0xc0828000);
        SME_ASSERT(wordOf("mova {z0.d, z1.d}, za.d[w8, 0, vgx2]") == 0xc0060800);
        // The tile, offset and slice register ranges
        SME_ASSERT(isRejected("mov za0h.b[w12, 16], p0/m, z0.b"));
        SME_ASSERT(isRejected("mov za1h.b[w12, 0], p0/m, z0.b"));
        SME_ASSERT(isRejected("mov za2h.h[w12, 0], p0/m, z0.h"));
        SME_ASSERT(isRejected("mov za0h.h[w12, 8], p0/m, z0.h"));
        SME_ASSERT(isRejected("mov za0h.b[w11, 0], p0/m, z0.b"));
        SME_ASSERT(isRejected("mov za0h.b[w16, 0], p0/m, z0.b"));
        SME_ASSERT(isRejected("mov za0h.b[x12, 0], p0/m, z0.b"));
        SME_ASSERT(!isRejected("mov za15v.q[w15, 0], p0/m, z0.q"));
        SME_ASSERT(isRejected("mov za15v.q[w15, 1], p0/m, z0.q"));
        // The element sizes of the operands agree
        SME_ASSERT(isRejected("mov za0h.b[w12, 0], p0/m, z0.h"));
        SME_ASSERT(isRejected("fmopa za0.s, p0/m, p0/m, z0.d, z0.d"));
        SME_ASSERT(isRejected("fmopa za4.s, p0/m, p0/m, z0.s, z0.s"));
        SME_ASSERT(isRejected("fmopa za0.s, p0/z, p0/m, z0.s, z0.s"));
        // The vector select register and the offsets of the vector groups
        SME_ASSERT(isRejected("add za.s[w12, 0, vgx2], { z0.s, z1.s }"));
        SME_ASSERT(isRejected("add za.s[w8, 8, vgx2], { z0.s, z1.s }"));
        SME_ASSERT(isRejected("add za.s[w8, 0, vgx2], { z0.s, z1.s, z2.s, z3.s }"));
        SME_ASSERT(isRejected("add za.s[w8, 0, vgx4], { z0.s, z1.s }"));
        SME_ASSERT(isRejected("add za.s[w8, 0, vgx2], { z1.s, z2.s }"));
        SME_ASSERT(!isRejected("add za.s[w8, 0, vgx2], { z1.s, z2.s }, z0.s"));
        SME_ASSERT(isRejected("fmlal za.s[w8, 1:2], z0.h, z0.h"));
        SME_ASSERT(isRejected("fmlal za.s[w8, 0:2], z0.h, z0.h"));
        SME_ASSERT(!isRejected("fmlal za.s[w8, 14:15], z0.h, z0.h"));
        // The index of a by element operand
        SME_ASSERT(isRejected("fmla za.s[w8, 0, vgx2], { z0.s, z1.s }, z0.s[4]"));
        SME_ASSERT(!isRejected("fmla za.s[w8, 0, vgx2], { z0.s, z1.s }, z0.s[3]"));
        SME_ASSERT(isRejected("fmla za.s[w8, 0, vgx2], { z0.s, z1.s }, z16.s[0]"));
        // zt0 and its memory form
        SME_ASSERT(isRejected("movt zt0[0, mul vl], z0"));
        SME_ASSERT(isRejected("movt x0, zt0[9]"));
        SME_ASSERT(isRejected("ldr zt0, [x0, #1, mul vl]"));
        SME_ASSERT(isRejected("ldr za[w12, 16], [x0, #16, mul vl]"));
        SME_ASSERT(isRejected("ldr za[w12, 1], [x0, #2, mul vl]"));
        // The features
        assemble("fmopa za0.d, p0/m, p0/m, z0.d, z0.d", only("+sme"), isValid);
        SME_ASSERT(!isValid);
        assemble("fmopa za0.d, p0/m, p0/m, z0.d, z0.d", only("+sme-f64f64"), isValid);
        SME_ASSERT(isValid);
        assemble("add za.d[w8, 0, vgx2], { z0.d, z1.d }", only("+sme2,+sme-f64f64"), isValid);
        SME_ASSERT(!isValid);
        assemble("add za.d[w8, 0, vgx2], { z0.d, z1.d }", only("+sme2,+sme-i16i64"), isValid);
        SME_ASSERT(isValid);
        assemble("zero { zt0 }", only("+sme"), isValid);
        SME_ASSERT(!isValid);
        assemble("zero { zt0 }", only("+sme2"), isValid);
        SME_ASSERT(isValid);
        assemble("movaz z0.b, za0h.b[w12, 0]", only("+sme2"), isValid);
        SME_ASSERT(!isValid);
        assemble("movaz z0.b, za0h.b[w12, 0]", only("+sme2p1"), isValid);
        SME_ASSERT(isValid);
    }

    // The decoded operands: the tiles, the slices, the arrays, the list, zt0
    void testOperands()
    {
        typedef ArmDecodedOperand Operand;
        // ld1w {za2v.s[w14, 1]}, p5/z, [x3, x4, lsl #2]
        ArmOpcodePtr ld = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0xe084d469, 4, 0x1000, ArmFeatureSet::all());
        SME_ASSERT(!ld.isEmpty());
        const Operand& slice = ld->getOperand(0);
        SME_ASSERT(slice.m_kind == Operand::KIND_MATRIX);
        SME_ASSERT(slice.m_tile == 2);
        SME_ASSERT(slice.m_register == armdis::ARM64_REG_ZA_ROW0 + 2);
        SME_ASSERT(slice.m_arrangement == Operand::ARRANGEMENT_S);
        SME_ASSERT(slice.m_sliceDirection == Operand::SLICE_VERTICAL);
        SME_ASSERT(slice.m_sliceRegister == armdis::ARM64_REG_W0 + 14);
        SME_ASSERT(slice.m_sliceOffset == 1);
        SME_ASSERT(slice.m_sliceOffsetLast == 1);
        SME_ASSERT(slice.m_isExplicit);

        // addha za3.s, p3/m, p5/m, z9.s
        ArmOpcodePtr addha = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0xc0900000 | 3 | (3 << 10) | (5 << 13) | (9 << 5), 4, 0x1000,
            ArmFeatureSet::all());
        SME_ASSERT(!addha.isEmpty());
        SME_ASSERT(addha->getOperand(0).m_kind == Operand::KIND_MATRIX);
        SME_ASSERT(addha->getOperand(0).m_tile == 3);
        SME_ASSERT(addha->getOperand(0).m_sliceDirection == Operand::SLICE_NONE);
        SME_ASSERT(addha->getOperand(1).m_predicateQualifier == Operand::PREDICATE_MERGING);
        SME_ASSERT(addha->getOperand(1).m_register == armdis::ARM64_REG_P0 + 3);
        SME_ASSERT(addha->getOperand(2).m_register == armdis::ARM64_REG_P0 + 5);

        // add za.s[w9, 3, vgx2], { z6.s, z7.s }, { z8.s, z9.s }
        ArmOpcodePtr add = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0xc1a838d3, 4, 0x1000, ArmFeatureSet::all());
        SME_ASSERT(!add.isEmpty());
        const Operand& array = add->getOperand(0);
        SME_ASSERT(array.m_kind == Operand::KIND_MATRIX);
        SME_ASSERT(array.m_register == armdis::ARM64_REG_ZA_ROW0);
        SME_ASSERT(array.m_arrangement == Operand::ARRANGEMENT_S);
        SME_ASSERT(array.m_sliceDirection == Operand::SLICE_NONE);
        SME_ASSERT(array.m_sliceRegister == armdis::ARM64_REG_W0 + 9);
        SME_ASSERT(array.m_sliceOffset == 3);
        SME_ASSERT(array.m_vectorGroup == 2);
        SME_ASSERT(add->getOperand(1).m_kind == Operand::KIND_REGISTER_LIST);
        SME_ASSERT(add->getOperand(1).m_count == 2);
        SME_ASSERT(add->getOperand(1).m_register == armdis::ARM64_REG_Z0 + 6);
        SME_ASSERT(add->getOperand(2).m_register == armdis::ARM64_REG_Z0 + 8);

        // fmlal za.s[w9, 0x2:0x3], z6.h, z5.h[6]
        ArmOpcodePtr fmlal = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0xc185b8c1, 4, 0x1000, ArmFeatureSet::all());
        SME_ASSERT(!fmlal.isEmpty());
        SME_ASSERT(fmlal->getOperand(0).m_sliceOffset == 2);
        SME_ASSERT(fmlal->getOperand(0).m_sliceOffsetLast == 3);
        SME_ASSERT(fmlal->getOperand(0).m_vectorGroup == 0);
        SME_ASSERT(fmlal->getOperand(2).m_register == armdis::ARM64_REG_Z0 + 5);
        SME_ASSERT(fmlal->getOperand(2).m_elementIndex == 6);

        // zero {za0.d, za1.d}: the mask of the tiles
        ArmOpcodePtr zero = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0xc0080003, 4, 0x1000, ArmFeatureSet::all());
        SME_ASSERT(!zero.isEmpty());
        SME_ASSERT(zero->getOperand(0).m_kind == Operand::KIND_MATRIX);
        SME_ASSERT(zero->getOperand(0).m_registerMask == 3);

        // luti2 { z0.b, z1.b }, zt0, z0[0] and movt x0, zt0[8]
        ArmOpcodePtr luti = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0xc08c4000, 4, 0x1000, ArmFeatureSet::all());
        SME_ASSERT(!luti.isEmpty());
        SME_ASSERT(luti->getOperand(1).m_kind == Operand::KIND_REGISTER);
        SME_ASSERT(luti->getOperand(1).m_register == armdis::ARM64_REG_ZT0);
        ArmOpcodePtr movt = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0xc04c13e0, 4, 0x1000, ArmFeatureSet::all());
        SME_ASSERT(!movt.isEmpty());
        SME_ASSERT(movt->getOperand(1).m_register == armdis::ARM64_REG_ZT0);
        SME_ASSERT(movt->getOperand(1).m_elementIndex == 8);
    }

    // The features: the extensions of SME are separate
    void testFeatures()
    {
        bool isValid;
        // SME: zero {za}
        disassemble(0xc00800ff, 0, ArmFeatureSet(), isValid);
        SME_ASSERT(!isValid);
        disassemble(0xc00800ff, 0, only("+sme"), isValid);
        SME_ASSERT(isValid);
        // SME2: sclamp { z0.b, z1.b }, z0.b, z0.b
        disassemble(0xc120c400, 0, only("+sme"), isValid);
        SME_ASSERT(!isValid);
        disassemble(0xc120c400, 0, only("+sme2"), isValid);
        SME_ASSERT(isValid);
        // SME2.2: the zeroing forms, and SME2.3: luti6
        disassemble(0xc0c84000, 0, only("+sme2p2"), isValid);
        SME_ASSERT(!isValid);
        disassemble(0xc0c84000, 0, only("+sme2p3"), isValid);
        SME_ASSERT(isValid);
        // The FP8 instructions of SVE need "ssve-fp8fma or (sve2 and fp8fma)": fmlalb z0.h, z1.b, z2.b
        // and the dot products have an entry for the feature of the streaming mode
        uint32 fmlalb = wordOf("fmlalb z0.h, z1.b, z2.b");
        SME_ASSERT(fmlalb != 0);
        disassemble(fmlalb, 0, only("+ssve-fp8fma"), isValid);
        SME_ASSERT(isValid);
        disassemble(fmlalb, 0, only("+fp8fma"), isValid);
        SME_ASSERT(isValid);
        disassemble(fmlalb, 0, only("+fp8"), isValid);
        SME_ASSERT(!isValid);
        assemble("fdot z0.s, z1.b, z2.b", only("+ssve-fp8dot4"), isValid);
        SME_ASSERT(isValid);
        assemble("fdot z0.s, z1.b, z2.b", only("+ssve-fp8dot2"), isValid);
        SME_ASSERT(!isValid);
        assemble("fdot z0.h, z1.b, z2.b", only("+ssve-fp8dot2"), isValid);
        SME_ASSERT(isValid);
        ArmFeatures::Feature feature;
        SME_ASSERT(ArmFeatures::findFeature("sme-mop4", feature) && (feature == ArmFeatures::FEAT_SME_MOP4));
        SME_ASSERT(ArmFeatures::findFeature("sme-tmop", feature) && (feature == ArmFeatures::FEAT_SME_TMOP));
        SME_ASSERT(ArmFeatures::findFeature("sme-f8f16", feature) && (feature == ArmFeatures::FEAT_SME_F8F16));
        SME_ASSERT(ArmFeatures::findFeature("sme2p2", feature) && (feature == ArmFeatures::FEAT_SME2P2));
        SME_ASSERT(ArmFeatures::findFeature("ssve-fp8dot2", feature) && (feature == ArmFeatures::FEAT_SSVE_FP8DOT2));
        SME_ASSERT(ArmFeatures::findFeature("sme-fa64", feature) && (feature == ArmFeatures::FEAT_SME_FA64));
        SME_ASSERT(ArmFeatures::getSupport(ArmFeatures::FEAT_SME2) != 0);
        // The features bring the ones they need
        ArmFeatureSet set;
        SME_ASSERT(set.parse("+sme-b16b16"));
        SME_ASSERT(set.has(ArmFeatures::FEAT_SME) && set.has(ArmFeatures::FEAT_SME2));
        SME_ASSERT(set.has(ArmFeatures::FEAT_SVE_B16B16));
        ArmFeatureSet second;
        SME_ASSERT(second.parse("+sme2p3"));
        SME_ASSERT(second.has(ArmFeatures::FEAT_SME2P1) && second.has(ArmFeatures::FEAT_SME2P2));
        ArmFeatureSet third;
        SME_ASSERT(third.parse("+sme-f8f32"));
        SME_ASSERT(third.has(ArmFeatures::FEAT_FP8) && !third.has(ArmFeatures::FEAT_SME_F8F16));
    }

    virtual void test()
    {
        testRows();
        testInvalid();
        testTiles();
        testZero();
        testOuterProducts();
        testMultiVector();
        testTables();
        testAssembler();
        testOperands();
        testFeatures();
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestArm64Sme g_globalTestArm64Sme;
