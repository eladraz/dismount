/*
 * TestRiscvVector.cpp
 *
 * Tests of the RISC-V vector extensions of the disassembler and the assembler
 * (V, the Zve* subsets, Zvfbfmin, Zvfbfwma, Zvbb, Zvkb, Zvbc and the vector
 * cryptography; RV32 and RV64): the text of the decoded instructions in the
 * three formats and the assembled bytes for a few shapes of every mnemonic
 * (tests/riscv/RiscvVectorTextRows.inc, the LLVM text), the decoding of
 * encodings under feature sets (the gating of Zve32f, Zve64x and the
 * extensions, the reserved vtype values, the misaligned register groups), the
 * assembling of lines (the constraints of the destination which llvm-mc
 * checks, the aliases, the pseudo instructions of more than one instruction,
 * the spellings of vtype: RiscvVectorAsmRows.inc) and the vtype of vsetvli and
 * vsetivli for every value.
 *
 * The big comparison with llvm-mc and llvm-objdump is tests/riscv/run_compat.py
 * (the v_*.s corpora) and tests/riscv/tools/fuzz_decode.py --class vector.
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
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/DisassemblerInvalidOpcodeException.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvNotation.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"
#include "dismount/assembler/proc/riscv/RiscvAssembler.h"

// Like TESTS_ASSERT, but tell which check failed
static uint gRiscvVectorFailures = 0;
#define RISCV_VECTOR_ASSERT(condition) \
    if (!(condition)) \
    { \
        fprintf(stderr, "TestRiscvVector.cpp:%d: %s\n", __LINE__, #condition); \
        gRiscvVectorFailures++; \
    }

class TestObjectTestRiscvVector : public cTestObject {
public:
    // A row: the text of an encoding in the three formats
    struct TextRow {
        uint m_bits;
        uint32 m_encoding;
        const char* m_text;
        const char* m_noAliasesText;
        const char* m_numericText;
    };

    // A decode row: the text of an encoding under a feature set ("" if invalid)
    struct DecodeRow {
        uint m_bits;
        const char* m_mattr;
        uint32 m_encoding;
        const char* m_text;
    };

    // An assembly row: the bytes of a statement as hex ("" if it is rejected)
    struct AsmRow {
        uint m_bits;
        const char* m_mattr;
        const char* m_text;
        const char* m_hex;
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

    static RiscvInstructionSet::DisassemblerTypes setOf(uint bits)
    {
        return (bits == 32) ? RiscvInstructionSet::RISCV_RV32 : RiscvInstructionSet::RISCV_RV64;
    }

    static ProcessorAddress addressOf(uint bits, uint64 address)
    {
        return ProcessorAddress((bits == 32) ? ProcessorAddress::PROCESSOR_32 :
                                               ProcessorAddress::PROCESSOR_64, address);
    }

    RiscvFeatureSet featuresOf(const char* list)
    {
        RiscvFeatureSet set;
        RISCV_VECTOR_ASSERT(set.parse(list));
        return set;
    }

    // Disassemble a 32 bit encoding with the stream disassembler, return the
    // text. 'isValid' is set to false if the encoding is an invalid instruction
    cString disassemble(uint bits, uint32 encoding, const RiscvFeatureSet& features,
                        bool& isValid, int format = 0)
    {
        uint8 bytes[4] = { (uint8)encoding, (uint8)(encoding >> 8), (uint8)(encoding >> 16),
                           (uint8)(encoding >> 24) };
        cBuffer data(bytes, 4);
        cMemoryStream stream(data);
        RiscvStreamDisassembler disassembler(setOf(bits),
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            addressOf(bits, 0x1000), false, features);
        isValid = true;
        XSTL_TRY
        {
            OpcodePtr opcode = disassembler.next();
            RISCV_VECTOR_ASSERT(opcode->getOpcodeSize() == 4);
            DefaultOpcodeDataFormatter formatter(1);
            OpcodeFormatterPtr fr = disassembler.getOpcodeFormat(opcode, formatter, format);
            return fr->string();
        }
        XSTL_CATCH(DisassemblerInvalidOpcodeException&)
        {
            isValid = false;
        }
        return cString();
    }

    // Assemble a statement (it may be more than one instruction) with the stream
    // on a FirstPassBinary and return the bytes as hex, "" if it is rejected
    cString assembleStatement(uint bits, const char* mattr, const char* text)
    {
        FirstPassBinaryPtr binary(new FirstPassBinary((bits == 32) ? OpcodeSubsystems::DISASSEMBLER_RISCV_32 :
                                                      OpcodeSubsystems::DISASSEMBLER_RISCV_64, false));
        binary->createNewBlockWithoutChange(0, StackInterfacePtr(new TempClassInterface(binary)));
        binary->changeBasicBlock(0);
        bool isValid = true;
        XSTL_TRY
        {
            RiscvAssemblerStream compiler(*binary, setOf(bits), featuresOf(mattr));
            compiler.setCompression(false);
            compiler << text << endl;
            compiler.assemble();
        }
        XSTL_CATCH(cException&)
        {
            isValid = false;
        }
        if (!isValid)
            return cString();
        const cBuffer& data = binary->getCurrentBlockData();
        static const char digits[] = "0123456789abcdef";
        cString hex;
        for (uint i = 0; i < data.getSize(); i++)
        {
            hex += cString(digits[data[i] >> 4]);
            hex += cString(digits[data[i] & 0xf]);
        }
        return hex;
    }

    static cString hexOf(uint32 encoding)
    {
        static const char digits[] = "0123456789abcdef";
        cString hex;
        for (uint i = 0; i < 4; i++)
        {
            uint8 byte = (uint8)(encoding >> (8 * i));
            hex += cString(digits[byte >> 4]);
            hex += cString(digits[byte & 0xf]);
        }
        return hex;
    }

    // The rows: the encoding gives the text in the three formats, the texts
    // give the encoding back (or an encoding with the same text)
    void testRows()
    {
        static const TextRow rows[] = {
#include "riscv/RiscvVectorTextRows.inc"
            { 0, 0, NULL, NULL, NULL }
        };
        uint count = 0;
        for (const TextRow* row = rows; row->m_text != NULL; row++)
        {
            RiscvFeatureSet all = RiscvFeatureSet::all();
            bool isValid;
            cString text = disassemble(row->m_bits, row->m_encoding, all, isValid);
            if (!isValid || !(text == row->m_text))
            {
                cout << "row " << HEXDWORD(row->m_encoding) << ": expected '" << row->m_text
                     << "' got '" << (isValid ? text : cString("(invalid)")) << "'" << endl;
                RISCV_VECTOR_ASSERT(false);
            }
            cString noAliases = disassemble(row->m_bits, row->m_encoding, all, isValid,
                                            RiscvNotation::RISCV_FORMAT_NO_ALIASES);
            if (!isValid || !(noAliases == row->m_noAliasesText))
            {
                cout << "row " << HEXDWORD(row->m_encoding) << ": expected '" << row->m_noAliasesText
                     << "' got '" << noAliases << "' (no aliases)" << endl;
                RISCV_VECTOR_ASSERT(false);
            }
            cString numeric = disassemble(row->m_bits, row->m_encoding, all, isValid,
                                          RiscvNotation::RISCV_FORMAT_NUMERIC);
            if (!isValid || !(numeric == row->m_numericText))
            {
                cout << "row " << HEXDWORD(row->m_encoding) << ": expected '" << row->m_numericText
                     << "' got '" << numeric << "' (numeric)" << endl;
                RISCV_VECTOR_ASSERT(false);
            }
            // The texts assemble to the encoding
            const char* texts[] = { row->m_text, row->m_noAliasesText, row->m_numericText };
            for (uint i = 0; i < 3; i++)
            {
                cString hex = assembleStatement(row->m_bits, "all", texts[i]);
                if (!(hex == hexOf(row->m_encoding)))
                {
                    // Another encoding is fine if it is the same instruction
                    uint32 other = 0;
                    for (uint k = 0; k < 4 && hex.length() >= 8; k++)
                    {
                        uint32 byte = 0;
                        for (uint d = 0; d < 2; d++)
                        {
                            character c = hex[2 * k + d];
                            byte = byte * 16 + ((c >= XSTL_CHAR('a')) ? (c - XSTL_CHAR('a') + 10) :
                                                                        (c - XSTL_CHAR('0')));
                        }
                        other |= byte << (8 * k);
                    }
                    bool isOtherValid = (hex.length() == 8);
                    cString otherText = isOtherValid ? disassemble(row->m_bits, other, all, isOtherValid,
                                            (i == 0) ? 0 : ((i == 1) ? RiscvNotation::RISCV_FORMAT_NO_ALIASES :
                                                                       RiscvNotation::RISCV_FORMAT_NUMERIC))
                                        : cString();
                    if (!isOtherValid || !(otherText == texts[i]))
                    {
                        cout << "row " << HEXDWORD(row->m_encoding) << ": '" << texts[i]
                             << "' assembled to " << hex << endl;
                        RISCV_VECTOR_ASSERT(false);
                    }
                }
            }
            count++;
        }
        RISCV_VECTOR_ASSERT(count > 600);
    }

    // The decode rows: the gating by the features, the reserved vtype values,
    // the misaligned register groups, the reserved fields
    void testDecode()
    {
        static const DecodeRow rows[] = {
#include "riscv/RiscvVectorDecodeRows.inc"
            { 0, NULL, 0, NULL }
        };
        uint count = 0;
        for (const DecodeRow* row = rows; row->m_mattr != NULL; row++)
        {
            bool isValid;
            cString text = disassemble(row->m_bits, row->m_encoding, featuresOf(row->m_mattr), isValid);
            cString expected(row->m_text);
            if (isValid != (expected.length() != 0) || (isValid && !(text == expected)))
            {
                cout << "decode " << HEXDWORD(row->m_encoding) << " (" << row->m_mattr << "): expected '"
                     << expected << "' got '" << (isValid ? text : cString("(invalid)")) << "'" << endl;
                RISCV_VECTOR_ASSERT(false);
            }
            count++;
        }
        RISCV_VECTOR_ASSERT(count > 200);
    }

    // The assembly rows: the bytes of llvm-mc, or the rejection of the lines
    // which it rejects
    void testAssembler()
    {
        static const AsmRow rows[] = {
#include "riscv/RiscvVectorAsmRows.inc"
            { 0, NULL, NULL, NULL }
        };
        uint count = 0;
        for (const AsmRow* row = rows; row->m_text != NULL; row++)
        {
            cString hex = assembleStatement(row->m_bits, row->m_mattr, row->m_text);
            if (!(hex == row->m_hex))
            {
                cout << "assemble '" << row->m_text << "' (rv" << row->m_bits << ", " << row->m_mattr
                     << "): expected '" << row->m_hex << "' got '" << hex << "'" << endl;
                RISCV_VECTOR_ASSERT(false);
            }
            count++;
        }
        RISCV_VECTOR_ASSERT(count > 300);
    }

    // vsetvli / vsetivli: every value of the vtype is printed as LLVM does (the
    // reserved values are numbers) and the text is assembled back
    void testVtype()
    {
        static const char* const sews[] = { "e8", "e16", "e32", "e64" };
        static const char* const lmuls[] = { "m1", "m2", "m4", "m8", NULL, "mf8", "mf4", "mf2" };
        RiscvFeatureSet all = RiscvFeatureSet::all();
        for (uint bits = 32; bits <= 64; bits += 32)
        {
            for (uint wide = 0; wide < 2; wide++)
            {
                uint count = wide ? 2048 : 1024;
                for (uint vtype = 0; vtype < count; vtype++)
                {
                    uint32 encoding = wide ?
                        (0x57 | (10 << 7) | (7 << 12) | (11 << 15) | (vtype << 20)) :
                        (0x57 | (10 << 7) | (7 << 12) | (5 << 15) | (vtype << 20) | (3u << 30));
                    bool isValid;
                    cString text = disassemble(bits, encoding, all, isValid);
                    RISCV_VECTOR_ASSERT(isValid);
                    uint vlmul = vtype & 7;
                    uint vsew = (vtype >> 3) & 7;
                    bool isAlt = (vtype & 0x100) != 0;
                    bool isReserved = (vtype >> 9) != 0 || vlmul == 4 || vsew > 3 || (isAlt && vsew > 1);
                    cString expected = wide ? cString("vsetvli a0, a1, ") : cString("vsetivli a0, 5, ");
                    if (isReserved)
                        expected += cString((uint32)vtype);
                    else
                        expected += cString(sews[vsew]) + (isAlt ? "alt" : "") + ", " + lmuls[vlmul] +
                                    (((vtype & 0x40) != 0) ? ", ta" : ", tu") +
                                    (((vtype & 0x80) != 0) ? ", ma" : ", mu");
                    if (!(text == expected))
                    {
                        cout << "vtype " << vtype << ": expected '" << expected << "' got '" << text << "'" << endl;
                        RISCV_VECTOR_ASSERT(false);
                    }
                    cString hex = assembleStatement(bits, "all", expected.getASCIIstring().getBuffer());
                    if (!(hex == hexOf(encoding)))
                    {
                        cout << "vtype '" << expected << "' assembled to " << hex << endl;
                        RISCV_VECTOR_ASSERT(false);
                    }
                }
            }
        }
    }

    // Every operand class of the table decodes and encodes: the register of every
    // number of a whole register instruction, the alignment of the groups
    void testRegisterGroups()
    {
        RiscvFeatureSet all = RiscvFeatureSet::all();
        // vmv2r.v / vmv4r.v / vmv8r.v: the registers of a group of n registers
        static const struct { uint32 m_base; uint m_align; const char* m_name; } moves[] = {
            { 0x9e003057, 1, "vmv2r.v" }, { 0x9e00b057, 1, "vmv2r.v" },
            { 0x9e01b057, 2, "vmv4r.v" }, { 0x9e03b057, 3, "vmv8r.v" }
        };
        for (uint i = 1; i < 4; i++)
        {
            uint align = moves[i].m_align;
            for (uint vd = 0; vd < 32; vd++)
            {
                for (uint vs2 = 0; vs2 < 32; vs2 += 3)
                {
                    uint32 encoding = moves[i].m_base | (vd << 7) | (vs2 << 20);
                    bool isValid;
                    cString text = disassemble(64, encoding, all, isValid);
                    bool isAligned = ((vd & ((1u << align) - 1)) == 0) && ((vs2 & ((1u << align) - 1)) == 0);
                    RISCV_VECTOR_ASSERT(isValid == isAligned);
                    if (isValid)
                    {
                        cString expected = cString(moves[i].m_name) + " v" + cString((uint32)vd) + ", v" +
                                           cString((uint32)vs2);
                        RISCV_VECTOR_ASSERT(text == expected);
                        RISCV_VECTOR_ASSERT(assembleStatement(64, "all", expected.getASCIIstring().getBuffer()) ==
                                            hexOf(encoding));
                    } else
                    {
                        cString line = cString(moves[i].m_name) + " v" + cString((uint32)vd) + ", v" +
                                       cString((uint32)vs2);
                        RISCV_VECTOR_ASSERT(assembleStatement(64, "all", line.getASCIIstring().getBuffer()).length() == 0);
                    }
                }
            }
        }
    }

    // The features: the support of the vector extensions and the shorthands
    void testFeatures()
    {
        RiscvFeatureSet set = featuresOf("rv64gcv");
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_V));
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVE64D));
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVE32X));
        RISCV_VECTOR_ASSERT(!set.has(RiscvFeatures::FEAT_ZVBB));
        set = featuresOf("rv64i_zvkn");
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVKB));
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVKNED));
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVKNHB));
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVKNHA));
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVKT));
        RISCV_VECTOR_ASSERT(!set.has(RiscvFeatures::FEAT_ZVKG));
        set = featuresOf("rv64i_zvknc");
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVBC));
        set = featuresOf("rv64i_zvkng");
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVKG));
        set = featuresOf("rv64i_zvks");
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVKSED));
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVKSH));
        set = featuresOf("rv64i_zvksc");
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVBC));
        set = featuresOf("rv64i_zvksg");
        RISCV_VECTOR_ASSERT(set.has(RiscvFeatures::FEAT_ZVKG));
        // The marks of the vector features
        const uint implemented = RiscvFeatures::SUPPORT_DISASSEMBLE | RiscvFeatures::SUPPORT_ASSEMBLE |
                                 RiscvFeatures::SUPPORT_ENCODING_VERIFIED;
        static const RiscvFeatures::Feature withInstructions[] = {
            RiscvFeatures::FEAT_ZVE32X, RiscvFeatures::FEAT_ZVE32F, RiscvFeatures::FEAT_ZVE64X,
            RiscvFeatures::FEAT_ZVFBFMIN, RiscvFeatures::FEAT_ZVFBFWMA, RiscvFeatures::FEAT_ZVKB,
            RiscvFeatures::FEAT_ZVBB, RiscvFeatures::FEAT_ZVBC, RiscvFeatures::FEAT_ZVKG,
            RiscvFeatures::FEAT_ZVKNED, RiscvFeatures::FEAT_ZVKNHA, RiscvFeatures::FEAT_ZVKSED,
            RiscvFeatures::FEAT_ZVKSH
        };
        for (uint i = 0; i < sizeof(withInstructions) / sizeof(withInstructions[0]); i++)
            RISCV_VECTOR_ASSERT((RiscvFeatures::getSupport(withInstructions[i]) & implemented) == implemented);
    }

    // A statement per line, whatever the line end is ("\r\n" of endl on Windows)
    void testLineEnds()
    {
        RISCV_VECTOR_ASSERT(assembleStatement(64, "all", "vadd.vv v1, v2, v3\r\nvadd.vv v1, v2, v3\r\n") ==
                            assembleStatement(64, "all", "vadd.vv v1, v2, v3") +
                            assembleStatement(64, "all", "vadd.vv v1, v2, v3"));
    }

    virtual void test()
    {
        testLineEnds();
        testRows();
        testDecode();
        testAssembler();
        testVtype();
        testRegisterGroups();
        testFeatures();
        if (gRiscvVectorFailures != 0)
            XSTL_THROW(cException, XSTL_STRING("Condition asserted"));
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestRiscvVector g_globalTestRiscvVector;
