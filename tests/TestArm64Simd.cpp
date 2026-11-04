/*
 * TestArm64Simd.cpp
 *
 * Tests of the A64 scalar floating point, Advanced SIMD and cryptographic
 * instructions: the text of the decoded words and the assembled bytes for
 * every form of every mnemonic (the rows of tests/arm/Arm64SimdTestRows.inc,
 * the LLVM text), the aliases, the invalid encodings, the features (an
 * encoding of an extension which is off is invalid and not assembled), the
 * decoded operands and the rejected text of the assembler.
 *
 * The big comparison with clang and objdump is tests/arm/run_compat.py.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/datastream.h"
#include "xStl/data/hash.h"
#include "xStl/data/list.h"
#include "xStl/data/sarray.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/exceptions.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/stream/ioStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/DisassemblerInvalidOpcodeException.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/assembler/proc/arm/ArmAssembler.h"

// Like TESTS_ASSERT, but tell which check failed
#define ARM64_SIMD_ASSERT(condition) \
    if (!(condition)) \
    { \
        fprintf(stderr, "TestArm64Simd.cpp:%d: %s\n", __LINE__, #condition); \
        XSTL_THROW(cException, XSTL_STRING("Condition asserted")); \
    }

#include "arm/Arm64SimdTestRows.inc"

class TestObjectTestArm64Simd : public cTestObject {
public:
    // A row: the text of a word at an address, and the LLVM features it needs
    struct TextRow {
        uint64 m_address;
        uint32 m_word;
        const char* m_text;
        const char* m_features;
    };

    // Disassemble a word, return the text. 'isValid' is false for an invalid
    // instruction
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
            ARM64_SIMD_ASSERT(opcode->getOpcodeSize() == 4);
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

    // Assemble a line, return the word. 'isValid' is false if it is rejected
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
            ARM64_SIMD_ASSERT(size == 4);
        }
        XSTL_CATCH(cException&)
        {
            isValid = false;
        }
        return encoding;
    }

    // Return the text of a word with all the features, "(invalid)" if it
    // isn't an instruction
    cString text(uint32 word, uint64 address = 0x1000)
    {
        bool isValid;
        cString result = disassemble(word, address, ArmFeatureSet::all(), isValid);
        return isValid ? result : cString("(invalid)");
    }

    // The base features and the ones of a comma separated list of LLVM names
    ArmFeatureSet onlyFeatures(const char* list)
    {
        ArmFeatureSet set;
        cString text("none");
        cList<cString> names = cString(list).split(XSTL_STRING(","));
        for (cList<cString>::iterator name = names.begin(); name != names.end(); name++)
            text += cString(",+") + *name;
        cSArray<char> ascii = text.getASCIIstring();
        ARM64_SIMD_ASSERT(set.parse(ascii.getBuffer()));
        return set;
    }

    // The set with all the features but the ones of the list
    ArmFeatureSet withoutFeatures(const char* list)
    {
        ArmFeatureSet set = ArmFeatureSet::all();
        cString text;
        cList<cString> names = cString(list).split(XSTL_STRING(","));
        for (cList<cString>::iterator name = names.begin(); name != names.end(); name++)
        {
            if (text.length() != 0)
                text += ",";
            text += cString("-") + *name;
        }
        cSArray<char> ascii = text.getASCIIstring();
        ARM64_SIMD_ASSERT(set.parse(ascii.getBuffer()));
        return set;
    }

    // The rows: the word gives the text, the text gives the word back or a
    // word with the same text (the fields that the decoder ignores), and the
    // instruction is decoded only with its features
    void testRows()
    {
        static const TextRow rows[] = { ARM64_SIMD_TEXT_ROWS { 0, 0, NULL, NULL } };
        uint count = 0;
        cHash<cString, bool> mnemonics;
        uint mnemonicsCount = 0;
        for (const TextRow* row = rows; row->m_text != NULL; row++)
        {
            bool isValid;
            cString decoded = disassemble(row->m_word, row->m_address, ArmFeatureSet::all(), isValid);
            if (!isValid || !(decoded == row->m_text))
            {
                cout << "row " << HEXDWORD(row->m_word) << ": expected '" << row->m_text
                     << "' got '" << (isValid ? decoded : cString("(invalid)")) << "'" << endl;
                ARM64_SIMD_ASSERT(false);
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
                    ARM64_SIMD_ASSERT(false);
                }
            }

            // The features: with only the features of the row the word is
            // the same instruction, without them it is invalid and the text
            // is not assembled
            if (row->m_features[0] != 0)
            {
                ArmFeatureSet only = onlyFeatures(row->m_features);
                cString withOnly = disassemble(row->m_word, row->m_address, only, isValid);
                if (!isValid || !(withOnly == row->m_text))
                {
                    cout << "row " << HEXDWORD(row->m_word) << " needs " << row->m_features
                         << ": '" << row->m_text << "' got '"
                         << (isValid ? withOnly : cString("(invalid)")) << "'" << endl;
                    ARM64_SIMD_ASSERT(false);
                }
                ArmFeatureSet without = withoutFeatures(row->m_features);
                disassemble(row->m_word, row->m_address, without, isValid);
                if (isValid)
                    cout << "row " << HEXDWORD(row->m_word) << " decoded without "
                         << row->m_features << endl;
                ARM64_SIMD_ASSERT(!isValid);
                assemble(row->m_text, row->m_address, without, isValid);
                ARM64_SIMD_ASSERT(!isValid);
            } else
            {
                // ARMv8.0: decoded with the base features only
                disassemble(row->m_word, row->m_address, ArmFeatureSet(), isValid);
                ARM64_SIMD_ASSERT(isValid);
            }

            // The mnemonic of the text
            cString line(row->m_text);
            uint blank = line.find(XSTL_STRING(" "));
            cString mnemonic = line.left(blank);
            if (!mnemonics.hasKey(mnemonic))
            {
                mnemonics.append(mnemonic, true);
                mnemonicsCount++;
            }
            count++;
        }
        // Every form of every mnemonic of the group
        ARM64_SIMD_ASSERT(count > 2000);
        ARM64_SIMD_ASSERT(mnemonicsCount > 330);
    }

    // The encodings which are not instructions (the reserved values of the
    // fields of the instructions)
    void testInvalid()
    {
        static const uint32 words[] = { ARM64_SIMD_INVALID_WORDS 0xffffffff };
        for (uint i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        {
            bool isValid;
            disassemble(words[i], 0x1000, ArmFeatureSet::all(), isValid);
            if (isValid)
                cout << "invalid word " << HEXDWORD(words[i]) << " decoded" << endl;
            ARM64_SIMD_ASSERT(!isValid);
        }
    }

    // The aliases: the text which LLVM prints and the texts it also reads
    void testAliases()
    {
        bool isValid;
        const ArmFeatureSet all = ArmFeatureSet::all();
        // mov of orr, mvn of not
        ARM64_SIMD_ASSERT(text(0x4ea11c20) == "mov v0.16b, v1.16b");
        ARM64_SIMD_ASSERT(assemble("mov v0.16b, v1.16b", 0, all, isValid) == 0x4ea11c20);
        ARM64_SIMD_ASSERT(assemble("orr v0.16b, v1.16b, v1.16b", 0, all, isValid) == 0x4ea11c20);
        ARM64_SIMD_ASSERT(text(0x4ea21c20) == "orr v0.16b, v1.16b, v2.16b");
        ARM64_SIMD_ASSERT(text(0x6e205820) == "mvn v0.16b, v1.16b");
        ARM64_SIMD_ASSERT(assemble("not v0.16b, v1.16b", 0, all, isValid) == 0x6e205820);
        ARM64_SIMD_ASSERT(assemble("mvn v0.16b, v1.16b", 0, all, isValid) == 0x6e205820);
        // mov of ins and umov, and the dup of a scalar
        ARM64_SIMD_ASSERT(text(0x4e0c1c20) == "mov v0.s[1], w1");
        ARM64_SIMD_ASSERT(assemble("ins v0.s[1], w1", 0, all, isValid) == 0x4e0c1c20);
        ARM64_SIMD_ASSERT(assemble("mov v0.s[1], w1", 0, all, isValid) == 0x4e0c1c20);
        ARM64_SIMD_ASSERT(text(0x6e0c4420) == "mov v0.s[1], v1.s[2]");
        ARM64_SIMD_ASSERT(assemble("ins v0.s[1], v1.s[2]", 0, all, isValid) == 0x6e0c4420);
        ARM64_SIMD_ASSERT(text(0x0e0c3c20) == "mov w0, v1.s[1]");
        ARM64_SIMD_ASSERT(assemble("umov w0, v1.s[1]", 0, all, isValid) == 0x0e0c3c20);
        ARM64_SIMD_ASSERT(text(0x4e183c20) == "mov x0, v1.d[1]");
        ARM64_SIMD_ASSERT(assemble("umov x0, v1.d[1]", 0, all, isValid) == 0x4e183c20);
        ARM64_SIMD_ASSERT(text(0x0e033c20) == "umov w0, v1.b[1]");
        ARM64_SIMD_ASSERT(text(0x5e0c0420) == "mov s0, v1.s[1]");
        ARM64_SIMD_ASSERT(assemble("dup s0, v1.s[1]", 0, all, isValid) == 0x5e0c0420);
        // sxtl is sshll #0
        ARM64_SIMD_ASSERT(text(0x0f08a420) == "sshll v0.8h, v1.8b, #0x0");
        ARM64_SIMD_ASSERT(assemble("sxtl v0.8h, v1.8b", 0, all, isValid) == 0x0f08a420);
        ARM64_SIMD_ASSERT(assemble("uxtl2 v0.4s, v1.8h", 0, all, isValid) == 0x6f10a420);
        // The fmov of the top half of a register
        ARM64_SIMD_ASSERT(text(0x9eae0020) == "fmov x0, v1.d[1]");
        ARM64_SIMD_ASSERT(text(0x9eaf0001) == "fmov v1.d[1], x0");
        // The compare with zero keeps its #0.0
        ARM64_SIMD_ASSERT(text(0x1e202008) == "fcmp s0, #0.0");
        ARM64_SIMD_ASSERT(assemble("fcmp s0, #0.0", 0, all, isValid) == 0x1e202008);
        ARM64_SIMD_ASSERT(assemble("fcmp s0, #0", 0, all, isValid) == 0 && !isValid);
        ARM64_SIMD_ASSERT(text(0x4ea0d820) == "fcmeq v0.4s, v1.4s, #0.0");
        ARM64_SIMD_ASSERT(assemble("fcmeq v0.4s, v1.4s, #0", 0, all, isValid) == 0x4ea0d820);
        ARM64_SIMD_ASSERT(text(0x4ea09820) == "cmeq v0.4s, v1.4s, #0");
        // The floating point immediates: eight digits, and the values of the
        // modified immediates
        ARM64_SIMD_ASSERT(text(0x1e2f1000) == "fmov s0, #1.50000000");
        ARM64_SIMD_ASSERT(assemble("fmov s0, #1.5", 0, all, isValid) == 0x1e2f1000);
        ARM64_SIMD_ASSERT(assemble("fmov s0, #1.50000000", 0, all, isValid) == 0x1e2f1000);
        ARM64_SIMD_ASSERT(assemble("fmov d0, #0.5", 0, all, isValid) == 0x1e6c1000);
        ARM64_SIMD_ASSERT(text(0x4f002420) == "movi v0.4s, #0x1, lsl #8");
        ARM64_SIMD_ASSERT(assemble("movi v0.4s, #1, lsl #8", 0, all, isValid) == 0x4f002420);
        ARM64_SIMD_ASSERT(text(0x4f000420) == "movi v0.4s, #0x1");
        ARM64_SIMD_ASSERT(assemble("movi v0.4s, #1, lsl #0", 0, all, isValid) == 0x4f000420);
        ARM64_SIMD_ASSERT(text(0x4f00c420) == "movi v0.4s, #0x1, msl #8");
        ARM64_SIMD_ASSERT(text(0x6f05e540) == "movi v0.2d, #0xff00ff00ff00ff00");
        ARM64_SIMD_ASSERT(text(0x2f00e400) == "movi d0, #0000000000000000");
        ARM64_SIMD_ASSERT(text(0x2f00e420) == "movi d0, #0x000000000000ff");
        ARM64_SIMD_ASSERT(assemble("movi d0, #0xff", 0, all, isValid) == 0x2f00e420);
        // The lists and the elements
        ARM64_SIMD_ASSERT(text(0x4cdfa000) == "ld1 { v0.16b, v1.16b }, [x0], #32");
        ARM64_SIMD_ASSERT(text(0x4dc2c800) == "ld1r { v0.4s }, [x0], x2");
        ARM64_SIMD_ASSERT(text(0x0ddf9000) == "ld1 { v0.s }[1], [x0], #4");
        ARM64_SIMD_ASSERT(assemble("ld1 {v0.s}[1], [x0], #4", 0, all, isValid) == 0x0ddf9000);
        ARM64_SIMD_ASSERT(text(0x4e057020) == "tbx v0.16b, { v1.16b, v2.16b, v3.16b, v4.16b }, v5.16b");
        ARM64_SIMD_ASSERT(assemble("tbx v0.16b, {v1.16b - v4.16b}, v5.16b", 0, all, isValid) == 0x4e057020);
        // The list wraps around v31
        ARM64_SIMD_ASSERT(assemble("ld2 {v31.4s, v0.4s}, [x0]", 0, all, isValid) == 0x4c40881f);
        ARM64_SIMD_ASSERT(text(0x4c40881f) == "ld2 { v31.4s, v0.4s }, [x0]");
    }

    // The extensions: an instruction is decoded and assembled only with its
    // feature
    void testFeatures()
    {
        bool isValid;
        struct Case {
            const char* m_text;
            uint32 m_word;
            ArmFeatures::Feature m_feature;
        };
        static const Case cases[] = {
            { "sdot v0.4s, v1.16b, v2.16b", 0x4e829420, ArmFeatures::FEAT_DOTPROD },
            { "usdot v0.4s, v1.16b, v2.16b", 0x4e829c20, ArmFeatures::FEAT_I8MM },
            { "smmla v0.4s, v1.16b, v2.16b", 0x4e82a420, ArmFeatures::FEAT_I8MM },
            { "bfdot v0.4s, v1.8h, v2.8h", 0x6e42fc20, ArmFeatures::FEAT_BF16 },
            { "bfcvt h0, s1", 0x1e634020, ArmFeatures::FEAT_BF16 },
            { "sqrdmlah v0.4s, v1.4s, v2.4s", 0x6e828420, ArmFeatures::FEAT_RDM },
            { "fadd v0.8h, v1.8h, v2.8h", 0x4e421420, ArmFeatures::FEAT_FP16 },
            { "fadd h0, h1, h2", 0x1ee22820, ArmFeatures::FEAT_FP16 },
            { "fmlal v0.2s, v1.2h, v2.2h", 0x0e22ec20, ArmFeatures::FEAT_FHM },
            { "fcadd v0.4s, v1.4s, v2.4s, #270", 0x6e82f420, ArmFeatures::FEAT_FCMA },
            { "fjcvtzs w0, d1", 0x1e7e0020, ArmFeatures::FEAT_JSCVT },
            { "frint32z v0.4s, v1.4s", 0x4e21e820, ArmFeatures::FEAT_FRINTTS },
            { "aese v0.16b, v1.16b", 0x4e284820, ArmFeatures::FEAT_AES },
            { "pmull v0.1q, v1.1d, v2.1d", 0x0ee2e020, ArmFeatures::FEAT_AES },
            { "sha1c q0, s1, v2.4s", 0x5e020020, ArmFeatures::FEAT_SHA1 },
            { "sha256h q0, q1, v2.4s", 0x5e024020, ArmFeatures::FEAT_SHA256 },
            { "sha512h q0, q1, v2.2d", 0xce628020, ArmFeatures::FEAT_SHA512 },
            { "eor3 v0.16b, v1.16b, v2.16b, v3.16b", 0xce020c20, ArmFeatures::FEAT_SHA3 },
            { "sm3ss1 v0.4s, v1.4s, v2.4s, v3.4s", 0xce420c20, ArmFeatures::FEAT_SM3 },
            { "sm4e v0.4s, v1.4s", 0xcec08420, ArmFeatures::FEAT_SM4 },
            { "famax v0.4s, v1.4s, v2.4s", 0x4ea2dc20, ArmFeatures::FEAT_FAMINMAX },
            { "luti2 v0.16b, { v1.16b }, v2[0]", 0x4e821020, ArmFeatures::FEAT_LUT },
            { "fdot v0.4s, v1.16b, v2.16b", 0x4e02fc20, ArmFeatures::FEAT_FP8DOT4 },
            { "fmlalb v0.8h, v1.16b, v2.16b", 0x0ec2fc20, ArmFeatures::FEAT_FP8FMA },
            { "fmmla v0.4s, v1.16b, v2.16b", 0x6e82ec20, ArmFeatures::FEAT_F8F32MM },
            { "fcvtas s0, d1", 0x1e7a0020, ArmFeatures::FEAT_FPRCVT },
            { "stl1 { v1.d }[0], [x1]", 0x0d018421, ArmFeatures::FEAT_LRCPC3 }
        };
        for (uint i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        {
            const Case& item = cases[i];
            ARM64_SIMD_ASSERT(text(item.m_word) == item.m_text);
            ARM64_SIMD_ASSERT(assemble(item.m_text, 0, ArmFeatureSet::all(), isValid) == item.m_word);
            ARM64_SIMD_ASSERT(isValid);
            ArmFeatureSet without = ArmFeatureSet::all();
            without.remove(item.m_feature);
            disassemble(item.m_word, 0, without, isValid);
            ARM64_SIMD_ASSERT(!isValid);
            // The assembler names the feature in its message
            assemble(item.m_text, 0, without, isValid);
            ARM64_SIMD_ASSERT(!isValid);
            ArmFeatureSet only;
            only.add(item.m_feature);
            ARM64_SIMD_ASSERT(disassemble(item.m_word, 0, only, isValid) == item.m_text);
            ARM64_SIMD_ASSERT(isValid);
        }
        // The v8.0 floating point and Advanced SIMD are always there
        ARM64_SIMD_ASSERT(disassemble(0x4ea21c20, 0, ArmFeatureSet(), isValid) ==
                          "orr v0.16b, v1.16b, v2.16b");
        ARM64_SIMD_ASSERT(isValid);
        // The names of the new features and the architecture versions
        ArmFeatures::Feature feature;
        ARM64_SIMD_ASSERT(ArmFeatures::findFeature("fprcvt", feature) &&
                          (feature == ArmFeatures::FEAT_FPRCVT));
        ARM64_SIMD_ASSERT(ArmFeatures::findFeature("f8f32mm", feature) &&
                          (feature == ArmFeatures::FEAT_F8F32MM));
        ARM64_SIMD_ASSERT(ArmFeatureSet::fromArchitecture("armv8.2-a").has(ArmFeatures::FEAT_RDM));
        // The support matrix: the complete groups are verified
        const ArmFeatures::Feature verified[] = {
            ArmFeatures::FEAT_AES, ArmFeatures::FEAT_SHA1, ArmFeatures::FEAT_SHA256,
            ArmFeatures::FEAT_SHA512, ArmFeatures::FEAT_SHA3, ArmFeatures::FEAT_SM3,
            ArmFeatures::FEAT_SM4, ArmFeatures::FEAT_RDM, ArmFeatures::FEAT_FP16,
            ArmFeatures::FEAT_FHM, ArmFeatures::FEAT_DOTPROD, ArmFeatures::FEAT_I8MM,
            ArmFeatures::FEAT_BF16, ArmFeatures::FEAT_FCMA, ArmFeatures::FEAT_JSCVT,
            ArmFeatures::FEAT_FRINTTS, ArmFeatures::FEAT_FP8, ArmFeatures::FEAT_FP8FMA,
            ArmFeatures::FEAT_FP8DOT4, ArmFeatures::FEAT_FP8DOT2, ArmFeatures::FEAT_FAMINMAX,
            ArmFeatures::FEAT_LUT, ArmFeatures::FEAT_F8F32MM, ArmFeatures::FEAT_F8F16MM,
            ArmFeatures::FEAT_FPRCVT
        };
        for (uint i = 0; i < sizeof(verified) / sizeof(verified[0]); i++)
            ARM64_SIMD_ASSERT((ArmFeatures::getSupport(verified[i]) &
                               ArmFeatures::SUPPORT_ENCODING_VERIFIED) != 0);
    }

    // The text which must not assemble: the operands of an entry which
    // disagree, and the values out of the range of the fields
    void testRejected()
    {
        static const char* const lines[] = {
            // The arrangements disagree
            "add v0.8b, v1.16b, v2.8b", "add v0.4s, v1.4s, v2.2s", "add v0.1d, v1.1d, v2.1d",
            "addv b0, v1.4h", "mul v0.2d, v1.2d, v2.2d", "pmul v0.4s, v1.4s, v2.4s",
            "saddl v0.4s, v1.8b, v2.8b", "saddl2 v0.8h, v1.8b, v2.8b", "xtn v0.8b, v1.4s",
            "fadd v0.2d, v1.2s, v2.2d", "fmla v0.4s, v1.4s, v2.d[1]", "zip1 v0.4s, v1.4s, v2.8h",
            // The registers
            "fadd s0, d1, d2", "fmov d0, w1", "fmov x0, v1.d[0]", "fmov v0.d[1], w1",
            "dup v0.2d, w1", "dup v0.4s, x1", "umov w0, v1.d[0]", "smov w0, v1.s[0]",
            "mov v0.s[4], w1", "mov v0.d[2], x1", "mov v0.b[16], w1",
            // The immediates
            "fmov s0, #1.234", "fmov v0.4s, #32.0", "movi v0.4s, #256",
            "movi v0.4s, #1, lsl #32", "movi v0.4s, #1, lsl #4", "movi v0.4h, #1, lsl #16",
            "movi v0.4s, #1, msl #24", "movi v0.8b, #1, lsl #8", "movi d0, #0x1ff",
            "shl v0.4s, v1.4s, #32", "sshr v0.4s, v1.4s, #0", "sshr v0.4s, v1.4s, #33",
            "shll v0.4s, v1.4h, #8",
            "scvtf v0.4s, v1.4s, #33", "fcmla v0.4s, v1.4s, v2.4s, #45",
            "fcadd v0.4s, v1.4s, v2.4s, #180", "xar v0.2d, v1.2d, v2.2d, #64",
            "fcmp s0, #1.0", "cmeq v0.4s, v1.4s, #1",
            // The lists
            "ld1 {v0.16b, v2.16b}, [x0]", "ld1 {v0.16b, v1.8b}, [x0]",
            "ld1 {v0.16b, v1.16b, v2.16b, v3.16b, v4.16b}, [x0]", "ld2 {v0.1d, v1.1d}, [x0]",
            "ld1 {v0.s}[4], [x0]", "ld1 {v0.d}[2], [x0]", "ld1r {v0.4s}[0], [x0]",
            "ld1 {v0.16b}, [x0], #8", "ld1 {v0.8b}, [x0], #16",
            "ld4 {v0.4s}, [x0]", "tbl v0.16b, {v1.8b}, v2.16b",
            "tbl v0.16b, {v1.16b, v2.16b, v3.16b, v4.16b, v5.16b}, v6.16b",
            // The elements
            "mul v0.8h, v1.8h, v16.h[0]", "mul v0.4s, v1.4s, v2.s[4]",
            "fmlal v0.2s, v1.2h, v16.h[0]", "sdot v0.4s, v1.16b, v2.4b[4]",
            // The mnemonics
            "frobnicate v0.4s, v1.4s", "fadd v0.4s, v1.4s", "add v0.4s, v1.4s, v2.4s, v3.4s"
        };
        for (uint i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        {
            bool isValid;
            assemble(lines[i], 0x1000, ArmFeatureSet::all(), isValid);
            if (isValid)
                cout << "'" << lines[i] << "' assembled" << endl;
            ARM64_SIMD_ASSERT(!isValid);
        }
    }

    // The decoded operands
    void testOperands()
    {
        typedef ArmDecodedOperand Operand;
        ArmOpcodePtr opcode = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x4cdfa000, 4, 0x1000, ArmFeatureSet::all());
        ARM64_SIMD_ASSERT(!opcode.isEmpty());
        ARM64_SIMD_ASSERT(opcode->getMnemonic() == "ld1");
        ARM64_SIMD_ASSERT(opcode->getOperandsCount() == 2);
        const Operand& list = opcode->getOperand(0);
        ARM64_SIMD_ASSERT(list.m_kind == Operand::KIND_REGISTER_LIST);
        ARM64_SIMD_ASSERT(list.m_register == armdis::ARM64_REG_V0);
        ARM64_SIMD_ASSERT(list.m_count == 2);
        ARM64_SIMD_ASSERT(list.m_arrangement == Operand::ARRANGEMENT_16B);
        const Operand& memory = opcode->getOperand(1);
        ARM64_SIMD_ASSERT(memory.m_kind == Operand::KIND_MEMORY);
        ARM64_SIMD_ASSERT(memory.m_base == armdis::ARM64_REG_X0);
        ARM64_SIMD_ASSERT(memory.m_addressing == Operand::ADDRESSING_POST_INDEX);
        ARM64_SIMD_ASSERT(memory.m_offset == 32);
        ARM64_SIMD_ASSERT(memory.m_postIndexRegister == armdis::ARM_NO_REGISTER);

        // ld1 { v0.s }[1], [x0], x2: the element index and the register
        opcode = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x0dc29000, 4, 0x1000, ArmFeatureSet::all());
        ARM64_SIMD_ASSERT(opcode->getMnemonic() == "ld1");
        ARM64_SIMD_ASSERT(opcode->getOperand(0).m_elementIndex == 1);
        ARM64_SIMD_ASSERT(opcode->getOperand(0).m_arrangement == Operand::ARRANGEMENT_S);
        ARM64_SIMD_ASSERT(opcode->getOperand(1).m_postIndexRegister == armdis::ARM64_REG_X0 + 2);

        // fmul v0.4s, v1.4s, v2.s[3]: the element
        opcode = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x4fa29820, 4, 0x1000, ArmFeatureSet::all());
        ARM64_SIMD_ASSERT(!opcode.isEmpty());
        ARM64_SIMD_ASSERT(opcode->getMnemonic() == "fmul");
        ARM64_SIMD_ASSERT(opcode->getOperand(2).m_arrangement == Operand::ARRANGEMENT_S);
        ARM64_SIMD_ASSERT(opcode->getOperand(2).m_elementIndex == 3);
        ARM64_SIMD_ASSERT(opcode->getOperand(2).m_register == armdis::ARM64_REG_V0 + 2);

        // fmov d0, #0.5: a floating point immediate
        opcode = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x1e6c1000, 4, 0x1000, ArmFeatureSet::all());
        ARM64_SIMD_ASSERT(opcode->getOperand(0).m_register == armdis::ARM64_REG_D0);
        ARM64_SIMD_ASSERT(opcode->getOperand(1).m_kind == Operand::KIND_FLOAT_IMMEDIATE);
        ARM64_SIMD_ASSERT(opcode->getOperand(1).m_floatImmediate == 0.5);

        // movi v0.4s, #1, msl #8: the shift
        opcode = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x4f00c420, 4, 0x1000, ArmFeatureSet::all());
        ARM64_SIMD_ASSERT(opcode->getOperand(1).m_kind == Operand::KIND_IMMEDIATE);
        ARM64_SIMD_ASSERT(opcode->getOperand(1).m_immediate == 1);
        ARM64_SIMD_ASSERT(opcode->getOperand(1).m_shift == Operand::SHIFT_MSL);
        ARM64_SIMD_ASSERT(opcode->getOperand(1).m_shiftAmount == 8);

        // sshr v0.4s, v1.4s, #3 and scvtf w, fixed point
        opcode = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x4f3d0420, 4, 0x1000, ArmFeatureSet::all());
        ARM64_SIMD_ASSERT(opcode->getMnemonic() == "sshr");
        ARM64_SIMD_ASSERT(opcode->getOperand(2).m_immediate == 3);
        opcode = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, 0x1e58f420, 4, 0x1000, ArmFeatureSet::all());
        ARM64_SIMD_ASSERT(opcode->getMnemonic() == "fcvtzs");
        ARM64_SIMD_ASSERT(opcode->getOperand(0).m_register == armdis::ARM64_REG_W0);
        ARM64_SIMD_ASSERT(opcode->getOperand(1).m_register == armdis::ARM64_REG_D0 + 1);
        ARM64_SIMD_ASSERT(opcode->getOperand(2).m_immediate == 3);

        // The SIMD instructions don't alter the flow
        ARM64_SIMD_ASSERT(!opcode->isBranch());
    }

    // A block of instructions through the stream: the sizes, the addresses and
    // the invalid word
    void testStream()
    {
        // fadd s0, s1, s2; ld1 { v0.16b }, [x0]; invalid; eor3 (SHA3)
        const uint8 code[] = { 0x20, 0x28, 0x22, 0x1e,
                               0x00, 0x70, 0x40, 0x4c,
                               0xff, 0xff, 0xff, 0xff,
                               0x20, 0x0c, 0x02, 0xce };
        cBuffer data(code, sizeof(code));
        cMemoryStream stream(data);
        ArmStreamDisassembler disassembler(ArmInstructionSet::ARM_A64,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0x400000), true);
        DefaultOpcodeDataFormatter formatter(1);

        OpcodePtr opcode = disassembler.next();
        ARM64_SIMD_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() ==
                          "fadd s0, s1, s2");
        opcode = disassembler.next();
        ARM64_SIMD_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() ==
                          "ld1 { v0.16b }, [x0]");
        ProcessorAddress address(gNullPointerProcessorAddress);
        ARM64_SIMD_ASSERT(opcode->getOpcodeAddress(address) && (address.getAddress() == 0x400004));
        for (uint i = 0; i < 4; i++)
        {
            opcode = disassembler.next();
            ARM64_SIMD_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE);
        }
        opcode = disassembler.next();
        ARM64_SIMD_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() ==
                          "eor3 v0.16b, v1.16b, v2.16b, v3.16b");
        ARM64_SIMD_ASSERT(disassembler.isEndOfStream());
    }

    virtual void test()
    {
        testRows();
        testInvalid();
        testAliases();
        testFeatures();
        testRejected();
        testOperands();
        testStream();
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestArm64Simd g_globalTestArm64Simd;
