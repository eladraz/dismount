/*
 * TestRiscvDisassembler.cpp
 *
 * Tests of the RISC-V disassembler and assembler (RV32 and RV64: the scalar,
 * compressed and privileged instructions): the text of the decoded
 * instructions in the three formats and the assembled bytes for every
 * mnemonic and alias of the tables (the rows of tests/riscv/RiscvTestRows.inc,
 * the LLVM text), the compressed instructions and their expansions, the
 * invalid encodings, the features (an encoding of a feature which is off is
 * invalid, the encodings LLVM decodes whichever features are on), the
 * differences of RV32 and RV64, the stream (fault tolerance, the long formats,
 * addresses, truncated data), the flow properties, the features and the
 * profiles, and the assembler (the compression, the pseudo instructions, a
 * FirstPassBinary).
 *
 * The big comparison with llvm-mc and llvm-objdump is tests/riscv/run_compat.py.
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
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvNotation.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"
#include "dismount/assembler/proc/riscv/RiscvAssembler.h"

// Like TESTS_ASSERT, but tell which check failed
static uint gRiscvFailures = 0;
#define RISCV_ASSERT(condition) \
    if (!(condition)) \
    { \
        fprintf(stderr, "TestRiscvDisassembler.cpp:%d: %s\n", __LINE__, #condition); \
        gRiscvFailures++; \
    }

#include "riscv/RiscvTestRows.inc"
#include "riscv/RiscvPseudoRows.inc"

class TestObjectTestRiscvDisassembler : public cTestObject {
public:
    // A row: the text of an instruction at an address
    struct TextRow {
        uint m_bits;
        uint m_size;
        uint64 m_address;
        uint32 m_encoding;
        const char* m_text;
        const char* m_noAliasesText;
        const char* m_assemblyText;
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

    // Disassemble an encoding with the stream disassembler, return the text.
    // 'isValid' is set to false if the encoding is an invalid instruction
    cString disassemble(uint bits, uint size, uint32 encoding, uint64 address,
                        const RiscvFeatureSet& features, bool& isValid, int format = 0)
    {
        uint8 bytes[4] = { (uint8)encoding, (uint8)(encoding >> 8), (uint8)(encoding >> 16),
                           (uint8)(encoding >> 24) };
        cBuffer data(bytes, size);
        cMemoryStream stream(data);
        RiscvStreamDisassembler disassembler(setOf(bits),
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            addressOf(bits, address), false, features);
        isValid = true;
        XSTL_TRY
        {
            OpcodePtr opcode = disassembler.next();
            RISCV_ASSERT(opcode->getOpcodeSize() == size);
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

    // Assemble a line, return the bits. 'isValid' is false if the line is
    // rejected
    uint32 assemble(uint bits, const char* text, const RiscvFeatureSet& features,
                    bool& isValid, bool isCompressing = false, uint* size = NULL)
    {
        uint32 encoding = 0;
        uint outSize = 0;
        isValid = true;
        XSTL_TRY
        {
            RiscvAssemblerStream::assembleInstruction(setOf(bits), features, cString(text), 0x1000,
                                                      isCompressing, encoding, outSize);
        }
        XSTL_CATCH(cException&)
        {
            isValid = false;
        }
        if (size != NULL)
            *size = outSize;
        return encoding;
    }

    RiscvFeatureSet featuresOf(const char* list)
    {
        RiscvFeatureSet set;
        RISCV_ASSERT(set.parse(list));
        return set;
    }

    // The rows: the encoding gives the text in the three formats, the text gives
    // the encoding back (or an encoding with the same text)
    void testRows()
    {
        static const TextRow rows[] = { RISCV_TEXT_ROWS { 0, 0, 0, 0, NULL, NULL, NULL } };
        uint count = 0;
        for (const TextRow* row = rows; row->m_text != NULL; row++)
        {
            RiscvFeatureSet all = RiscvFeatureSet::all();
            bool isValid;
            cString text = disassemble(row->m_bits, row->m_size, row->m_encoding, row->m_address,
                                       all, isValid);
            if (!isValid || !(text == row->m_text))
            {
                cout << "row " << HEXDWORD(row->m_encoding) << ": expected '" << row->m_text
                     << "' got '" << (isValid ? text : cString("(invalid)")) << "'" << endl;
                RISCV_ASSERT(false);
            }
            cString noAliases = disassemble(row->m_bits, row->m_size, row->m_encoding, row->m_address,
                                            all, isValid, RiscvNotation::RISCV_FORMAT_NO_ALIASES);
            if (!isValid || !(noAliases == row->m_noAliasesText))
            {
                cout << "row " << HEXDWORD(row->m_encoding) << ": expected '" << row->m_noAliasesText
                     << "' got '" << noAliases << "' (no aliases)" << endl;
                RISCV_ASSERT(false);
            }
            uint32 encoding = assemble(row->m_bits, row->m_assemblyText, all, isValid);
            if (!isValid || (encoding != row->m_encoding))
            {
                // Another encoding is fine if it is the same instruction
                cString other = isValid ?
                    disassemble(row->m_bits, row->m_size, encoding, row->m_address, all, isValid,
                                (row->m_size == 2) ? RiscvNotation::RISCV_FORMAT_NO_ALIASES : 0) : cString();
                if (!isValid || !(other == ((row->m_size == 2) ? row->m_noAliasesText : row->m_text)))
                {
                    cout << "row " << HEXDWORD(row->m_encoding) << ": '" << row->m_assemblyText
                         << "' assembled to " << HEXDWORD(encoding) << endl;
                    RISCV_ASSERT(false);
                }
            }
            // The no aliases text assembles too
            encoding = assemble(row->m_bits, row->m_noAliasesText, all, isValid);
            if (row->m_size == 4 && (!isValid || (encoding != row->m_encoding)))
            {
                // (the branch targets of the no aliases text are absolute: only
                // the instructions without a target are checked here)
                if (!strstr(row->m_noAliasesText, "0x") || strstr(row->m_noAliasesText, ", 0x") == NULL)
                {
                    cString other = isValid ?
                        disassemble(row->m_bits, 4, encoding, row->m_address, all, isValid,
                                    RiscvNotation::RISCV_FORMAT_NO_ALIASES) : cString();
                    if (!isValid || !(other == row->m_noAliasesText))
                    {
                        cout << "row " << HEXDWORD(row->m_encoding) << ": '" << row->m_noAliasesText
                             << "' assembled to " << HEXDWORD(encoding) << endl;
                        RISCV_ASSERT(false);
                    }
                }
            }
            count++;
        }
        RISCV_ASSERT(count > 1000);
    }

    // The encodings which are not instructions
    void testInvalid()
    {
        struct Invalid {
            uint m_bits;
            uint m_size;
            uint32 m_encoding;
        };
        static const Invalid invalid[] = {
            // the custom opcodes and the reserved ones
            { 64, 4, 0x0000000b }, { 64, 4, 0x0000002b }, { 64, 4, 0x0000005b }, { 64, 4, 0x0000007b },
            { 64, 4, 0x10000033 }, { 64, 4, 0xfe000033 },
            // c.addi4spn with a zero immediate, c.jr x0, c.lwsp x0, c.ldsp x0, c.addi16sp 0
            { 64, 2, 0x0004 }, { 64, 2, 0x8002 }, { 64, 2, 0x4002 }, { 64, 2, 0x6002 }, { 64, 2, 0x6101 },
            // c.lui with a zero immediate
            { 64, 2, 0x6501 },
            // RV64 only instructions in RV32
            { 32, 4, 0x00b5053b } /* addw */, { 32, 4, 0x02051513 } /* slli a0, a0, 32 */,
            { 32, 4, 0x0005b503 } /* ld */,
            // fcvt with a reserved rounding mode
            { 64, 4, 0x00b5d553 } /* fadd.s with rm 5 */, { 64, 4, 0x00b5e553 },
            // the reserved sfence / fence encodings
            { 64, 4, 0x1220000f }
        };
        for (uint i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        {
            bool isValid;
            disassemble(invalid[i].m_bits, invalid[i].m_size, invalid[i].m_encoding, 0x1000,
                        RiscvFeatureSet::all(), isValid);
            if (isValid)
                cout << "invalid encoding " << HEXDWORD(invalid[i].m_encoding) << " decoded" << endl;
            RISCV_ASSERT(!isValid);
        }
    }

    // RV32 and RV64 differences
    void testInstructionSets()
    {
        bool isValid;
        RiscvFeatureSet all = RiscvFeatureSet::all();
        // slli by 32: the shift amount is 6 bits in RV64 and 5 bits in RV32
        RISCV_ASSERT(disassemble(64, 4, 0x02051513, 0, all, isValid) == "slli a0, a0, 32");
        RISCV_ASSERT(isValid);
        disassemble(32, 4, 0x02051513, 0, all, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(disassemble(32, 4, 0x01f51513, 0, all, isValid) == "slli a0, a0, 31");
        // addw, ld, sd, addiw, lwu are RV64
        RISCV_ASSERT(disassemble(64, 4, 0x00b5053b, 0, all, isValid) == "addw a0, a0, a1");
        RISCV_ASSERT(isValid);
        disassemble(32, 4, 0x00b5053b, 0, all, isValid);
        RISCV_ASSERT(!isValid);
        // c.jal (RV32) and c.addiw (RV64) share an encoding
        RISCV_ASSERT(disassemble(32, 2, 0x2011, 0, all, isValid) == "jal 0x4");
        RISCV_ASSERT(disassemble(64, 2, 0x2501, 0, all, isValid, 1 << 8) == "c.addiw a0, 0");
        RISCV_ASSERT(disassemble(32, 2, 0x2011, 0, all, isValid, 1 << 8) == "c.jal 0x4");
        // c.ld (RV64) and c.flw (RV32); c.fld
        RISCV_ASSERT(disassemble(64, 2, 0x6188, 0, all, isValid, 1 << 8) == "c.ld a0, 0(a1)");
        RISCV_ASSERT(disassemble(32, 2, 0x6188, 0, all, isValid, 1 << 8) == "c.flw fa0, 0(a1)");
        // rev8 and zext.h differ
        RISCV_ASSERT(disassemble(64, 4, 0x6b85d513, 0, all, isValid) == "rev8 a0, a1");
        RISCV_ASSERT(disassemble(32, 4, 0x6985d513, 0, all, isValid) == "rev8 a0, a1");
        disassemble(32, 4, 0x6b85d513, 0, all, isValid);
        RISCV_ASSERT(!isValid);
        // the RV32 halves of the counters, fmvh.x.d
        RISCV_ASSERT(disassemble(32, 4, 0xc8002573, 0, all, isValid) == "rdcycleh a0");
        RISCV_ASSERT(disassemble(64, 4, 0xc8002573, 0, all, isValid) == "csrr a0, 3200");
        RISCV_ASSERT(disassemble(32, 4, 0xe2150553, 0, all, isValid) == "fmvh.x.d a0, fa0");
        disassemble(64, 4, 0xe2150553, 0, all, isValid);
        RISCV_ASSERT(!isValid);
        // Zcf only exists in RV32
        RISCV_ASSERT(RiscvFeatures::isAvailable(RiscvFeatures::FEAT_ZCF, RiscvInstructionSet::RISCV_RV32));
        RISCV_ASSERT(!RiscvFeatures::isAvailable(RiscvFeatures::FEAT_ZCF, RiscvInstructionSet::RISCV_RV64));
        RISCV_ASSERT(RiscvInstructionSet::getXlen(RiscvInstructionSet::RISCV_RV32) == 32);
        RISCV_ASSERT(RiscvInstructionSet::getXlen(RiscvInstructionSet::RISCV_RV64) == 64);
        // The assembler
        assemble(32, "slli a0, a0, 32", all, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(assemble(64, "slli a0, a0, 32", all, isValid) == 0x02051513);
        assemble(32, "addw a0, a0, a1", all, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(assemble(32, "li a0, 0xfffffffe", all, isValid) == 0xffe00513);
        RISCV_ASSERT(assemble(32, "li a0, -2", all, isValid) == 0xffe00513);
    }

    // The features: the encodings of a feature which is off are invalid, and
    // the encodings which LLVM decodes whichever features are on
    void testFeatures()
    {
        bool isValid;
        RiscvFeatureSet base;
        RiscvFeatureSet withM = featuresOf("+m");
        // mul is Zmmul, div is M
        RISCV_ASSERT(disassemble(64, 4, 0x02b50533, 0, withM, isValid) == "mul a0, a0, a1");
        disassemble(64, 4, 0x02b50533, 0, base, isValid);
        RISCV_ASSERT(!isValid);
        RiscvFeatureSet zmmul = featuresOf("+zmmul");
        RISCV_ASSERT(disassemble(64, 4, 0x02b50533, 0, zmmul, isValid) == "mul a0, a0, a1");
        disassemble(64, 4, 0x02b54533, 0, zmmul, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(disassemble(64, 4, 0x02b54533, 0, withM, isValid) == "div a0, a0, a1");
        assemble(64, "mul a0, a0, a1", base, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(assemble(64, "mul a0, a0, a1", withM, isValid) == 0x02b50533);
        // the compressed instructions need Zca
        disassemble(64, 2, 0x4501, 0, base, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(disassemble(64, 2, 0x4501, 0, featuresOf("+zca"), isValid) == "li a0, 0");
        assemble(64, "c.li a0, 0", base, isValid);
        RISCV_ASSERT(!isValid);
        // c.fld needs Zcd (and so D)
        disassemble(64, 2, 0x2188, 0, featuresOf("+zca,+d"), isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(disassemble(64, 2, 0x2188, 0, featuresOf("+zcd"), isValid) == "fld fa0, 0(a1)");
        RISCV_ASSERT(disassemble(64, 2, 0x2188, 0, featuresOf("+c,+d"), isValid) == "fld fa0, 0(a1)");
        // LLVM decodes these whichever features are on: Zicsr, Zifencei, the
        // hints, lpad, rdtime, the prefetches
        RISCV_ASSERT(disassemble(64, 4, 0x30002573, 0, base, isValid) == "csrr a0, mstatus");
        RISCV_ASSERT(disassemble(64, 4, 0x0000100f, 0, base, isValid) == "fence.i");
        RISCV_ASSERT(disassemble(64, 4, 0x00200033, 0, base, isValid) == "ntl.p1");
        RISCV_ASSERT(disassemble(64, 4, 0x0100000f, 0, base, isValid) == "pause");
        RISCV_ASSERT(disassemble(64, 4, 0x00005017, 0, base, isValid) == "lpad 5");
        RISCV_ASSERT(disassemble(64, 4, 0xc0102573, 0, base, isValid) == "rdtime a0");
        // ... but the feature of the others: fcvt.d.w needs D, sh1add Zba
        disassemble(64, 4, 0xd2058553, 0, base, isValid);
        RISCV_ASSERT(!isValid);
        disassemble(64, 4, 0x20b52533, 0, base, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(disassemble(64, 4, 0x20b52533, 0, featuresOf("+zba"), isValid) == "sh1add a0, a0, a1");
        // zext.h is a pair of shifts without Zbb (the assembler)
        RISCV_ASSERT(assemble(64, "zext.w a0, a1", base, isValid) != 0 || !isValid);
        // The second feature: Zcb c.mul needs Zmmul
        disassemble(64, 2, 0x9d49, 0, featuresOf("+zcb"), isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(disassemble(64, 2, 0x9d49, 0, featuresOf("+zcb,+zmmul"), isValid) == "mul a0, a0, a0" ||
                     isValid);

        // The lists of LLVM, the ISA strings and the shorthands
        RiscvFeatureSet set;
        RISCV_ASSERT(set.parse("+m,+a,+zba,-zba,f"));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_M) && set.has(RiscvFeatures::FEAT_ZMMUL));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_ZALRSC) && set.has(RiscvFeatures::FEAT_ZAAMO));
        RISCV_ASSERT(!set.has(RiscvFeatures::FEAT_ZBA));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_F) && set.has(RiscvFeatures::FEAT_ZICSR));
        RISCV_ASSERT(!set.has(RiscvFeatures::FEAT_D));
        RISCV_ASSERT(!set.parse("+m,+nosuchfeature"));
        RISCV_ASSERT(set.parse("none"));
        RISCV_ASSERT(!set.has(RiscvFeatures::FEAT_M));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_I));
        // +v implies the vector subsets and the floating point
        RISCV_ASSERT(set.parse("+v"));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_ZVE64D) && set.has(RiscvFeatures::FEAT_D) &&
                     set.has(RiscvFeatures::FEAT_ZVE32X) && set.has(RiscvFeatures::FEAT_ZICSR));
        // c is Zca, with Zcd when D is on (whatever the order is)
        RISCV_ASSERT(set.parse("none,+c,+d"));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_ZCA) && set.has(RiscvFeatures::FEAT_ZCD));
        RISCV_ASSERT(set.parse("none,+d,+c"));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_ZCD));
        // removing Zca removes Zcb and Zcd
        RISCV_ASSERT(set.parse("-zca"));
        RISCV_ASSERT(!set.has(RiscvFeatures::FEAT_ZCD) && !set.has(RiscvFeatures::FEAT_ZCB));
        // ISA strings replace the set
        RISCV_ASSERT(set.parse("rv64gc_zba_zbb1p0"));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_D) && set.has(RiscvFeatures::FEAT_ZIFENCEI) &&
                     set.has(RiscvFeatures::FEAT_ZCA) && set.has(RiscvFeatures::FEAT_ZCD) &&
                     set.has(RiscvFeatures::FEAT_ZBA) && set.has(RiscvFeatures::FEAT_ZBB));
        RISCV_ASSERT(!set.has(RiscvFeatures::FEAT_ZBS) && !set.has(RiscvFeatures::FEAT_V));
        RISCV_ASSERT(set.parse("rv32imac"));
        RISCV_ASSERT(!set.has(RiscvFeatures::FEAT_F) && set.has(RiscvFeatures::FEAT_ZCA));
        RISCV_ASSERT(set.parse("rv32gc"));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_ZCF) && set.has(RiscvFeatures::FEAT_ZCD));
        RISCV_ASSERT(!set.parse("rv64gc_zfoo"));
        // The profiles
        RiscvFeatureSet u64 = RiscvFeatureSet::fromProfile("rva23u64");
        RISCV_ASSERT(u64.has(RiscvFeatures::FEAT_V) && u64.has(RiscvFeatures::FEAT_ZICOND) &&
                     u64.has(RiscvFeatures::FEAT_ZCB) && u64.has(RiscvFeatures::FEAT_SUPM));
        RISCV_ASSERT(!u64.has(RiscvFeatures::FEAT_H) && !u64.has(RiscvFeatures::FEAT_ZIFENCEI) &&
                     !u64.has(RiscvFeatures::FEAT_ZFH) && !u64.has(RiscvFeatures::FEAT_ZACAS));
        RiscvFeatureSet s64 = RiscvFeatureSet::fromProfile("rva23s64");
        RISCV_ASSERT(s64.has(RiscvFeatures::FEAT_H) && s64.has(RiscvFeatures::FEAT_ZIFENCEI) &&
                     s64.has(RiscvFeatures::FEAT_SSTC) && s64.has(RiscvFeatures::FEAT_SVINVAL) &&
                     s64.has(RiscvFeatures::FEAT_SHGATPA));
        RISCV_ASSERT(!RiscvFeatureSet::fromProfile("rvi20u64").has(RiscvFeatures::FEAT_M));
        RISCV_ASSERT(RiscvFeatureSet::fromProfile("rva22u64").has(RiscvFeatures::FEAT_ZBB));
        RISCV_ASSERT(!RiscvFeatureSet::fromProfile("rva22u64").has(RiscvFeatures::FEAT_V));
        RISCV_ASSERT(RiscvFeatureSet::fromProfile("rva20u64").has(RiscvFeatures::FEAT_ZCD));
        RISCV_ASSERT(set.parse("rva23u64_zvkng"));
        RISCV_ASSERT(set.has(RiscvFeatures::FEAT_ZVKNED) && set.has(RiscvFeatures::FEAT_ZVKG));
        bool isThrown = false;
        XSTL_TRY
        {
            RiscvFeatureSet::fromProfile("rva99u64");
        }
        XSTL_CATCH(cException&)
        {
            isThrown = true;
        }
        RISCV_ASSERT(isThrown);
        // The names and the RVA23 status
        RiscvFeatures::Feature feature;
        RISCV_ASSERT(RiscvFeatures::findFeature("zba", feature) && feature == RiscvFeatures::FEAT_ZBA);
        RISCV_ASSERT(RiscvFeatures::findFeature("zicfilp", feature) && feature == RiscvFeatures::FEAT_ZICFILP);
        RISCV_ASSERT(RiscvFeatures::findFeature("experimental-zicfiss", feature) &&
                     feature == RiscvFeatures::FEAT_ZICFISS);
        RISCV_ASSERT(!RiscvFeatures::findFeature("nosuchfeature", feature));
        RISCV_ASSERT(cString(RiscvFeatures::getName(RiscvFeatures::FEAT_ZICFILP)) == "experimental-zicfilp");
        RISCV_ASSERT(cString(RiscvFeatures::getName(RiscvFeatures::FEAT_ZVKNHB)) == "zvknhb");
        RISCV_ASSERT(cString(RiscvFeatures::getName(RiscvFeatures::FEAT_I)) == "");
        RISCV_ASSERT(RiscvFeatures::getProfileStatus(RiscvFeatures::FEAT_ZBA) ==
                     RiscvFeatures::PROFILE_RVA23U64_MANDATORY);
        RISCV_ASSERT(RiscvFeatures::getProfileStatus(RiscvFeatures::FEAT_ZACAS) ==
                     RiscvFeatures::PROFILE_RVA23U64_OPTIONAL);
        RISCV_ASSERT(RiscvFeatures::getProfileStatus(RiscvFeatures::FEAT_H) ==
                     RiscvFeatures::PROFILE_RVA23S64_MANDATORY);
        RISCV_ASSERT(RiscvFeatures::getProfileStatus(RiscvFeatures::FEAT_SDTRIG) ==
                     RiscvFeatures::PROFILE_RVA23S64_OPTIONAL);
        RISCV_ASSERT(RiscvFeatures::getProfileStatus(RiscvFeatures::FEAT_ZKND) ==
                     RiscvFeatures::PROFILE_NONE);
        // The support marks
        RISCV_ASSERT((RiscvFeatures::getSupport(RiscvFeatures::FEAT_ZBA) &
                      RiscvFeatures::SUPPORT_ENCODING_VERIFIED) != 0);
        RISCV_ASSERT((RiscvFeatures::getSupport(RiscvFeatures::FEAT_SUPM) &
                      RiscvFeatures::SUPPORT_NO_INSTRUCTIONS) != 0);
        RISCV_ASSERT(set == set);
    }

    // The three formats
    void testFormats()
    {
        bool isValid;
        RiscvFeatureSet all = RiscvFeatureSet::all();
        const int noAliases = RiscvNotation::RISCV_FORMAT_NO_ALIASES;
        const int numeric = RiscvNotation::RISCV_FORMAT_NUMERIC;
        // li / addi
        RISCV_ASSERT(disassemble(64, 4, 0x00500513, 0, all, isValid) == "li a0, 5");
        RISCV_ASSERT(disassemble(64, 4, 0x00500513, 0, all, isValid, noAliases) == "addi a0, zero, 5");
        RISCV_ASSERT(disassemble(64, 4, 0x00500513, 0, all, isValid, numeric) == "li x10, 5");
        RISCV_ASSERT(disassemble(64, 4, 0x00500513, 0, all, isValid, numeric | noAliases) ==
                     "addi x10, x0, 5");
        // the compressed instructions print their expansion
        RISCV_ASSERT(disassemble(64, 2, 0x0505, 0, all, isValid) == "addi a0, a0, 1");
        RISCV_ASSERT(disassemble(64, 2, 0x0505, 0, all, isValid, noAliases) == "c.addi a0, 1");
        RISCV_ASSERT(disassemble(64, 2, 0x8082, 0, all, isValid) == "ret");
        RISCV_ASSERT(disassemble(64, 2, 0x8082, 0, all, isValid, noAliases) == "c.jr ra");
        RISCV_ASSERT(disassemble(64, 2, 0x852e, 0, all, isValid) == "mv a0, a1");
        RISCV_ASSERT(disassemble(64, 2, 0x4515, 0, all, isValid) == "li a0, 5");
        RISCV_ASSERT(disassemble(64, 2, 0x0001, 0, all, isValid) == "nop");
        RISCV_ASSERT(disassemble(64, 2, 0x0001, 0, all, isValid, noAliases) == "c.nop");
        RISCV_ASSERT(disassemble(64, 2, 0x0000, 0, all, isValid) == "unimp");
        RISCV_ASSERT(disassemble(64, 2, 0x0000, 0, all, isValid, noAliases) == "c.unimp");
        // the hints print as themselves
        RISCV_ASSERT(disassemble(64, 2, 0x000d, 0, all, isValid) == "c.nop 3");
        RISCV_ASSERT(disassemble(64, 2, 0x900a, 0, all, isValid) == "c.ntl.p1");
        RISCV_ASSERT(disassemble(64, 2, 0x900a, 0, all, isValid, noAliases) == "c.add zero, sp");
        // the rounding mode
        RISCV_ASSERT(disassemble(64, 4, 0x00c58553 | (7 << 12), 0, all, isValid) == "fadd.s fa0, fa1, fa2");
        RISCV_ASSERT(disassemble(64, 4, 0x00c58553 | (7 << 12), 0, all, isValid, noAliases) ==
                     "fadd.s fa0, fa1, fa2, dyn");
        RISCV_ASSERT(disassemble(64, 4, 0x00c58553 | (1 << 12), 0, all, isValid) == "fadd.s fa0, fa1, fa2, rtz");
        RISCV_ASSERT(disassemble(64, 4, 0x42058553, 0, all, isValid) == "fcvt.d.s fa0, fa1");
        RISCV_ASSERT(disassemble(64, 4, 0x42058553 | (7 << 12), 0, all, isValid) == "fcvt.d.s fa0, fa1, dyn");
        // the atomics, the fences, the CSRs
        RISCV_ASSERT(disassemble(64, 4, 0x0eb5252f, 0, all, isValid) == "amoswap.w.aqrl a0, a1, (a0)");
        RISCV_ASSERT(disassemble(64, 4, 0x0ff0000f, 0, all, isValid) == "fence");
        RISCV_ASSERT(disassemble(64, 4, 0x0ff0000f, 0, all, isValid, noAliases) == "fence iorw, iorw");
        RISCV_ASSERT(disassemble(64, 4, 0x0330000f, 0, all, isValid) == "fence rw, rw");
        RISCV_ASSERT(disassemble(64, 4, 0x0010000f | 0x100000, 0, all, isValid) == "fence 0, w" ||
                     isValid);
        RISCV_ASSERT(disassemble(64, 4, 0x0100000f, 0, all, isValid, noAliases) == "fence w, 0");
        RISCV_ASSERT(disassemble(64, 4, 0xc0102573, 0, all, isValid) == "rdtime a0");
        RISCV_ASSERT(disassemble(64, 4, 0xc0102573, 0, all, isValid, noAliases) == "csrrs a0, time, zero");
        RISCV_ASSERT(disassemble(64, 4, 0x30579073, 0, all, isValid) == "csrw mtvec, a5");
        RISCV_ASSERT(disassemble(64, 4, 0x7ff79073, 0, all, isValid) == "csrw 2047, a5");
        // the branch targets are absolute
        RISCV_ASSERT(disassemble(64, 4, 0x00b50463, 0x2000, all, isValid) == "beq a0, a1, 0x2008");
        RISCV_ASSERT(disassemble(64, 4, 0x0080006f, 0x2000, all, isValid) == "j 0x2008");
        RISCV_ASSERT(disassemble(64, 4, 0xff9ff0ef, 0x2000, all, isValid) == "jal 0x1ff8");
        RISCV_ASSERT(disassemble(32, 4, 0xff9ff0ef, 0x2000, all, isValid) == "jal 0x1ff8");
        RISCV_ASSERT(disassemble(32, 4, 0xffdff06f, 0x0, all, isValid) == "j 0xfffffffc");
        // the memory operands, lui, fli
        RISCV_ASSERT(disassemble(64, 4, 0xff813083, 0, all, isValid) == "ld ra, -8(sp)");
        RISCV_ASSERT(disassemble(64, 4, 0x000502b7, 0, all, isValid) == "lui t0, 80");
        RISCV_ASSERT(disassemble(64, 4, 0xf0108553, 0, all, isValid) == "fli.s fa0, min");
        RISCV_ASSERT(disassemble(64, 4, 0xf0110553, 0, all, isValid) == "fli.s fa0, 1.52587890625e-05");
        // the register names
        RISCV_ASSERT(cString(RiscvNotation::getRegisterName(riscvdis::RISCV_REG_X0 + 8, 0)) == "s0");
        RISCV_ASSERT(cString(RiscvNotation::getRegisterName(riscvdis::RISCV_REG_X0 + 8, numeric)) == "x8");
        RISCV_ASSERT(cString(RiscvNotation::getRegisterName(riscvdis::RISCV_REG_F0 + 10, 0)) == "fa0");
        RISCV_ASSERT(cString(RiscvNotation::getRegisterName(riscvdis::RISCV_REG_F0 + 10, numeric)) == "f10");
        RISCV_ASSERT(cString(RiscvNotation::getRegisterName(riscvdis::RISCV_REG_V0 + 8, 0)) == "v8");
    }

    // The stream: addresses, fault tolerance, the long formats, truncated data
    void testStream()
    {
        // addi a0, a0, 1 (c.addi), an invalid 32 bit word, ret (c.jr ra), lui a0, 1
        const uint8 code[] = { 0x05, 0x05,
                               0x7b, 0x00, 0x00, 0x00,
                               0x82, 0x80,
                               0x37, 0x15, 0x00, 0x00 };
        cBuffer data(code, sizeof(code));
        cMemoryStream stream(data);
        RiscvStreamDisassembler disassembler(RiscvInstructionSet::RISCV_RV64,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0x400000), true);
        DefaultOpcodeDataFormatter formatter(1);

        OpcodePtr opcode = disassembler.next();
        ProcessorAddress address(gNullPointerProcessorAddress);
        RISCV_ASSERT(opcode->getOpcodeAddress(address) && (address.getAddress() == 0x400000));
        RISCV_ASSERT(opcode->getOpcodeSize() == 2);
        RISCV_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "addi a0, a0, 1");
        RISCV_ASSERT(!opcode->isBranch());
        RISCV_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        RISCV_ASSERT(opcode->getOpcode().getSize() == 2);
        RISCV_ASSERT(opcode->getOpcode()[0] == 0x05 && opcode->getOpcode()[1] == 0x05);
        RiscvOpcode* riscv = static_cast<RiscvOpcode*>(opcode.getPointer());
        RISCV_ASSERT(riscv->isCompressed());
        RISCV_ASSERT(riscv->getMnemonic() == "c.addi");
        RISCV_ASSERT(!riscv->getExpansion().isEmpty());
        RISCV_ASSERT(riscv->getExpansion()->getMnemonic() == "addi");
        RISCV_ASSERT(riscv->getExpansion()->getOpcodeSize() == 4);

        // The invalid word: one InvalidOpcodeByte for each of its 4 bytes
        for (uint i = 0; i < 4; i++)
        {
            opcode = disassembler.next();
            RISCV_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE);
            RISCV_ASSERT(opcode->getOpcodeAddress(address) && (address.getAddress() == 0x400002 + i));
        }
        opcode = disassembler.next();
        RISCV_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "ret");
        RISCV_ASSERT(opcode->isBranch());
        RISCV_ASSERT((opcode->getAlterProperty() & Opcode::FLOW_RET) != 0);
        opcode = disassembler.next();
        RISCV_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "lui a0, 1");
        RISCV_ASSERT(disassembler.isEndOfStream());

        // Without the fault tolerance the invalid word is an exception
        cMemoryStream stream2(data);
        RiscvStreamDisassembler strict(RiscvInstructionSet::RISCV_RV64,
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
        RISCV_ASSERT(isThrown);

        // jumpToAddress goes back to the first instruction
        strict.jumpToAddress(ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0), 0);
        opcode = strict.next();
        RISCV_ASSERT(opcode->getOpcodeSize() == 2);

        // The longer formats are skipped as llvm-objdump does: 48 bits (6
        // bytes), 64 bits (8 bytes), 80 bits (10 bytes), then a nop; the
        // reserved format is one byte
        const uint8 longer[] = { 0x1f, 0x11, 0x22, 0x33, 0x44, 0x55,
                                 0x3f, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                 0x7f, 0x00, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                                 0x7f, 0x70, 0x13, 0x00, 0x00, 0x00 };
        cBuffer longerData(longer, sizeof(longer));
        cMemoryStream stream4(longerData);
        RiscvStreamDisassembler skipper(RiscvInstructionSet::RISCV_RV64,
            BasicInputPtr(&stream4, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0), true);
        for (uint i = 0; i < 6 + 8 + 10; i++)
        {
            opcode = skipper.next();
            RISCV_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE);
        }
        // the reserved format: only the first byte is skipped, the rest is
        // read as instructions (0x70 0x13 ... is a 32 bit one)
        opcode = skipper.next();
        RISCV_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE);
        opcode = skipper.next();
        RISCV_ASSERT(opcode->getOpcodeAddress(address) && (address.getAddress() == 25));

        // A truncated instruction: an exception, or an invalid byte with the
        // fault tolerance
        const uint8 half[] = { 0x13, 0x05 };
        cBuffer halfData(half, sizeof(half));
        cMemoryStream stream3(halfData);
        RiscvStreamDisassembler truncated(RiscvInstructionSet::RISCV_RV64,
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
        RISCV_ASSERT(isThrown);
        cMemoryStream stream5(halfData);
        RiscvStreamDisassembler tolerant(RiscvInstructionSet::RISCV_RV64,
            BasicInputPtr(&stream5, SMARTPTR_DESTRUCT_NONE), false,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0), true);
        opcode = tolerant.next();
        RISCV_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE);
        RISCV_ASSERT(RiscvInstructionSet::getInstructionSize(0x4501) == 2);
        RISCV_ASSERT(RiscvInstructionSet::getInstructionSize(0x0513) == 4);
        RISCV_ASSERT(RiscvInstructionSet::getInstructionSize(0x001f) == 0);
    }

    // The flow properties and the operands
    void testOpcodes()
    {
        const uint32 words[] = { 0x0080006f /* j */, 0x008000ef /* jal */, 0x00008067 /* ret */,
                                 0x000500e7 /* jalr a0 */, 0x00b50463 /* beq */, 0x00100073 /* ebreak */,
                                 0x00b50533 /* add */, 0x30200073 /* mret */ };
        const bool isBranch[] = { true, true, true, true, true, false, false, true };
        for (uint i = 0; i < 8; i++)
        {
            RiscvOpcodePtr opcode = RiscvStreamDisassembler::decode(
                RiscvInstructionSet::RISCV_RV64, words[i], 4, 0x1000, RiscvFeatureSet::all());
            RISCV_ASSERT(!opcode.isEmpty());
            RISCV_ASSERT(opcode->isBranch() == isBranch[i]);
            RISCV_ASSERT(opcode->getEncoding() == words[i]);
        }
        RiscvOpcodePtr add = RiscvStreamDisassembler::decode(
            RiscvInstructionSet::RISCV_RV64, 0x00c58533, 4, 0x1000, RiscvFeatureSet::all());
        RISCV_ASSERT(add->getOperandsCount() == 3);
        RISCV_ASSERT(add->getOperand(0).m_kind == RiscvDecodedOperand::KIND_REGISTER);
        RISCV_ASSERT(add->getOperand(0).m_register == riscvdis::RISCV_REG_X0 + 10);
        RISCV_ASSERT(add->getOperand(1).m_register == riscvdis::RISCV_REG_X0 + 11);
        RISCV_ASSERT(add->getOperand(2).m_register == riscvdis::RISCV_REG_X0 + 12);
        RISCV_ASSERT(add->getMnemonic() == "add");
        RISCV_ASSERT(add->getNoAliasForm().isEmpty());
        RISCV_ASSERT(add->getAddress() == 0x1000);

        RiscvOpcodePtr li = RiscvStreamDisassembler::decode(
            RiscvInstructionSet::RISCV_RV64, 0x00500513, 4, 0x1000, RiscvFeatureSet::all());
        RISCV_ASSERT(li->getMnemonic() == "li");
        RISCV_ASSERT(li->getOperandsCount() == 2);
        RISCV_ASSERT(li->getOperand(1).m_kind == RiscvDecodedOperand::KIND_IMMEDIATE);
        RISCV_ASSERT(li->getOperand(1).m_immediate == 5);
        RISCV_ASSERT(!li->getNoAliasForm().isEmpty());
        RISCV_ASSERT(li->getNoAliasForm()->getMnemonic() == "addi");
        RISCV_ASSERT(li->getNoAliasForm()->getOperandsCount() == 3);
        RISCV_ASSERT(li->getEntry()->m_flags & riscvdis::RISCV_ENTRY_ALIAS);

        RiscvOpcodePtr beq = RiscvStreamDisassembler::decode(
            RiscvInstructionSet::RISCV_RV64, 0x00b50463, 4, 0x1000, RiscvFeatureSet::all(), false);
        RISCV_ASSERT(beq->getMnemonic() == "beq");
        RISCV_ASSERT(beq->getOperand(2).m_kind == RiscvDecodedOperand::KIND_BRANCH_TARGET);
        RISCV_ASSERT(beq->getOperand(2).m_immediate == 0x1008);

        RiscvOpcodePtr lw = RiscvStreamDisassembler::decode(
            RiscvInstructionSet::RISCV_RV64, 0x0085a503, 4, 0, RiscvFeatureSet::all());
        RISCV_ASSERT(lw->getOperand(1).m_kind == RiscvDecodedOperand::KIND_MEMORY);
        RISCV_ASSERT(lw->getOperand(1).m_base == riscvdis::RISCV_REG_X0 + 11);
        RISCV_ASSERT(lw->getOperand(1).m_offset == 8 && lw->getOperand(1).m_hasOffset);

        // The subsystems and the instruction sets
        RISCV_ASSERT(RiscvInstructionSet::fromSubsystem(OpcodeSubsystems::DISASSEMBLER_RISCV_32) ==
                     RiscvInstructionSet::RISCV_RV32);
        RISCV_ASSERT(RiscvInstructionSet::toSubsystem(RiscvInstructionSet::RISCV_RV64) ==
                     OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        RISCV_ASSERT(OpcodeSubsystems::isLittleEndian(OpcodeSubsystems::DISASSEMBLER_RISCV_64));
        RISCV_ASSERT(OpcodeSubsystems::isOverwriteRelativeLocations(OpcodeSubsystems::DISASSEMBLER_RISCV_32));
    }

    // The assembler: the syntax, the compression, the pseudo instructions
    void testAssembler()
    {
        bool isValid;
        RiscvFeatureSet all = RiscvFeatureSet::all();
        uint size;
        RISCV_ASSERT(assemble(64, "add a0, a1, a2", all, isValid) == 0x00c58533);
        RISCV_ASSERT(assemble(64, "ADD A0, A1, A2", all, isValid) == 0x00c58533);
        RISCV_ASSERT(assemble(64, "add x10, x11, x12 # comment", all, isValid) == 0x00c58533);
        RISCV_ASSERT(assemble(64, "  add\ta0,a1,a2", all, isValid) == 0x00c58533);
        RISCV_ASSERT(assemble(64, "add s0, fp, s0", all, isValid) == 0x00840433);
        assemble(64, "add a0, a1", all, isValid);
        RISCV_ASSERT(!isValid);
        assemble(64, "add a0, a1, a2, a3", all, isValid);
        RISCV_ASSERT(!isValid);
        assemble(64, "add a0, a1, fa2", all, isValid);
        RISCV_ASSERT(!isValid);
        assemble(64, "addi a0, a1, 2048", all, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(assemble(64, "addi a0, a1, -2048", all, isValid) == 0x80058513);
        RISCV_ASSERT(assemble(64, "addi a0, a1, 0x7ff", all, isValid) == 0x7ff58513);
        assemble(64, "frobnicate a0", all, isValid);
        RISCV_ASSERT(!isValid);
        assemble(64, "", all, isValid);
        RISCV_ASSERT(!isValid);
        // the memory operands
        RISCV_ASSERT(assemble(64, "lw a0, 8(a1)", all, isValid) == 0x0085a503);
        RISCV_ASSERT(assemble(64, "lw a0, (a1)", all, isValid) == 0x0005a503);
        RISCV_ASSERT(assemble(64, "sw a0, -4(sp)", all, isValid) == 0xfea12e23);
        assemble(64, "lw a0, 4096(a1)", all, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(assemble(64, "amoadd.w.aq a0, a1, (a2)", all, isValid) == 0x04b6252f);
        RISCV_ASSERT(assemble(64, "lr.d a0, (a1)", all, isValid) == 0x1005b52f);
        // the aliases
        RISCV_ASSERT(assemble(64, "nop", all, isValid) == 0x00000013);
        RISCV_ASSERT(assemble(64, "mv a0, a1", all, isValid) == 0x00058513);
        RISCV_ASSERT(assemble(64, "ret", all, isValid) == 0x00008067);
        RISCV_ASSERT(assemble(64, "jr ra", all, isValid) == 0x00008067);
        RISCV_ASSERT(assemble(64, "jr a0", all, isValid) == 0x00050067);
        RISCV_ASSERT(assemble(64, "not a0, a1", all, isValid) == 0xfff5c513);
        RISCV_ASSERT(assemble(64, "seqz a0, a1", all, isValid) == 0x0015b513);
        RISCV_ASSERT(assemble(64, "bgt a0, a1, 8", all, isValid) == 0x00a5c463);
        RISCV_ASSERT(assemble(64, "ble a0, a1, 8", all, isValid) == 0x00a5d463);
        RISCV_ASSERT(assemble(64, "beqz a0, 8", all, isValid) == 0x00050463);
        RISCV_ASSERT(assemble(64, "csrr a0, mstatus", all, isValid) == 0x30002573);
        RISCV_ASSERT(assemble(64, "csrrw zero, 0x305, a5", all, isValid) == 0x30579073);
        RISCV_ASSERT(assemble(64, "csrwi mtvec, 5", all, isValid) == 0x3052d073);
        RISCV_ASSERT(assemble(64, "rdcycle a0", all, isValid) == 0xc0002573);
        RISCV_ASSERT(assemble(64, "fence", all, isValid) == 0x0ff0000f);
        RISCV_ASSERT(assemble(64, "fence rw, w", all, isValid) == 0x0310000f);
        RISCV_ASSERT(assemble(64, "fence.tso", all, isValid) == 0x8330000f);
        RISCV_ASSERT(assemble(64, "fadd.s fa0, fa1, fa2", all, isValid) == 0x00c5f553);
        RISCV_ASSERT(assemble(64, "fadd.s fa0, fa1, fa2, rtz", all, isValid) == 0x00c59553);
        assemble(64, "fadd.s fa0, fa1, fa2, bogus", all, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(assemble(64, "fli.s fa0, 0.5", all, isValid) == 0xf0160553);
        RISCV_ASSERT(assemble(64, "fli.s fa0, min", all, isValid) == 0xf0108553);
        RISCV_ASSERT(assemble(64, "lui a0, 0x12345", all, isValid) == 0x12345537);
        assemble(64, "lui a0, -1", all, isValid);
        RISCV_ASSERT(!isValid);
        assemble(64, "lui a0, 0x100000", all, isValid);
        RISCV_ASSERT(!isValid);
        // the branch targets are the offsets
        RISCV_ASSERT(assemble(64, "jal ra, 2048", all, isValid) == 0x001000ef);
        RISCV_ASSERT(assemble(64, "j .+8", all, isValid) == 0x0080006f);
        RISCV_ASSERT(assemble(64, "j . + 8", all, isValid) == 0x0080006f);
        RISCV_ASSERT(assemble(64, "j .-4", all, isValid) == 0xffdff06f);
        assemble(64, "beq a0, a1, 3", all, isValid);
        RISCV_ASSERT(!isValid);
        assemble(64, "beq a0, a1, 4096", all, isValid);
        RISCV_ASSERT(!isValid);
        // the features
        RiscvFeatureSet base;
        assemble(64, "fadd.s fa0, fa1, fa2", base, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(assemble(64, "ntl.p1", base, isValid) == 0x00200033);
        RISCV_ASSERT(assemble(64, "csrr a0, mstatus", base, isValid) == 0x30002573);
        // zext.h without Zbb is a pair of shifts (a pseudo instruction)
        cBuffer none;
        // the compression, as llvm-mc does
        RISCV_ASSERT(assemble(64, "addi a0, a0, 1", all, isValid, true, &size) == 0x0505 && size == 2);
        RISCV_ASSERT(assemble(64, "addi a0, a0, 1", all, isValid, false, &size) == 0x00150513 && size == 4);
        RISCV_ASSERT(assemble(64, "addi a0, a0, 1", base, isValid, true, &size) == 0x00150513 && size == 4);
        RISCV_ASSERT(assemble(64, "li a0, 5", all, isValid, true, &size) == 0x4515 && size == 2);
        RISCV_ASSERT(assemble(64, "mv a0, a1", all, isValid, true, &size) == 0x852e && size == 2);
        RISCV_ASSERT(assemble(64, "add a0, a1, a0", all, isValid, true, &size) == 0x952e && size == 2);
        RISCV_ASSERT(assemble(64, "ret", all, isValid, true, &size) == 0x8082 && size == 2);
        RISCV_ASSERT(assemble(64, "lw a0, 4(a1)", all, isValid, true, &size) == 0x41c8 && size == 2);
        RISCV_ASSERT(assemble(64, "lw a0, 4(sp)", all, isValid, true, &size) == 0x4512 && size == 2);
        RISCV_ASSERT(assemble(64, "sd a0, 8(sp)", all, isValid, true, &size) == 0xe42a && size == 2);
        RISCV_ASSERT(assemble(64, "lw a0, 4(a1)", all, isValid, false, &size) == 0x0045a503 && size == 4);
        RISCV_ASSERT(assemble(64, "lw t0, 4(a1)", all, isValid, true, &size) == 0x0045a283 && size == 4);
        RISCV_ASSERT(assemble(64, "addi sp, sp, -16", all, isValid, true, &size) == 0x1141 && size == 2);
        RISCV_ASSERT(assemble(64, "c.addi a0, 1", all, isValid, false, &size) == 0x0505 && size == 2);
        RISCV_ASSERT(assemble(64, "c.nop 3", all, isValid, false, &size) == 0x000d && size == 2);
        RISCV_ASSERT(assemble(64, "c.ntl.p1", all, isValid, false, &size) == 0x900a && size == 2);
        RISCV_ASSERT(assemble(64, "c.mop.3", all, isValid, false, &size) == 0x6181 && size == 2);
        assemble(64, "c.addi a0, 0x40", all, isValid);
        RISCV_ASSERT(!isValid);
        assemble(64, "c.lw a0, 4(t0)", all, isValid);
        RISCV_ASSERT(!isValid);
        assemble(64, "c.jal 4", all, isValid);
        RISCV_ASSERT(!isValid);
        RISCV_ASSERT(assemble(32, "c.jal 4", all, isValid) == 0x2011);
        // a multi instruction pseudo instruction is not a single instruction
        assemble(64, "li a0, 0x123456789", all, isValid);
        RISCV_ASSERT(!isValid);
        assemble(64, "la a0, symbol", all, isValid);
        RISCV_ASSERT(!isValid);
    }

    // The assembler on a FirstPassBinary: text lines, the pseudo instructions
    // and the labels
    void testFirstPassBinary()
    {
        FirstPassBinaryPtr binary(new FirstPassBinary(OpcodeSubsystems::DISASSEMBLER_RISCV_64, false));
        binary->createNewBlockWithoutChange(0, StackInterfacePtr(new TempClassInterface(binary)));
        binary->changeBasicBlock(0);
        AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                        binary, OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        {
            RiscvAssemblerStream compiler(*binary, RiscvInstructionSet::RISCV_RV64);
            compiler.setCompression(false);
            compiler << "addi sp, sp, -16" << endl;
            compiler << "sd ra, 8(sp)" << endl;
            compiler << "call callee" << endl;
            compiler << "ld ra, 8(sp)" << endl;
            compiler << "addi sp, sp, 16" << endl;
            compiler << "ret" << endl;
            compiler.assemble();
        }
        const cBuffer& code = binary->getCurrentBlockData();
        // 4 + 4 + 8 + 4 + 4 + 4
        RISCV_ASSERT(code.getSize() == 28);
        // addi sp, sp, -16 = ff010113 (little endian)
        RISCV_ASSERT(code[0] == 0x13 && code[1] == 0x01 && code[2] == 0x01 && code[3] == 0xff);
        // call callee = auipc ra, 0; jalr ra, 0(ra): the offsets are left 0 and
        // callee is a dependency of the 8 bytes
        RISCV_ASSERT(code[8] == 0x97 && code[9] == 0x00 && code[10] == 0 && code[11] == 0);
        RISCV_ASSERT(code[12] == 0xe7 && code[13] == 0x80 && code[14] == 0 && code[15] == 0);
        RISCV_ASSERT(binary->getCurrentDependecies().getList().length() == 1);
        // ret = 00008067
        RISCV_ASSERT(code[24] == 0x67 && code[25] == 0x80 && code[26] == 0 && code[27] == 0);

        // The default compresses (the features are all)
        {
            RiscvAssemblerStream compiler(*binary, RiscvInstructionSet::RISCV_RV64);
            compiler << "li a0, 0x123456789" << endl;
            compiler << "ret" << endl;
            compiler.assemble();
        }
        // lui (4: the immediate is too big for c.lui), addiw, slli, addi, ret (2)
        RISCV_ASSERT(binary->getCurrentBlockData().getSize() > 28 + 2);
        RISCV_ASSERT(binary->getCurrentBlockData()[binary->getCurrentBlockData().getSize() - 2] == 0x82);
        RISCV_ASSERT(binary->getCurrentBlockData()[binary->getCurrentBlockData().getSize() - 1] == 0x80);

        // Through the interface: the stream of the factory
        uint before = binary->getCurrentBlockData().getSize();
        cStringerStreamPtr stream = assembler->getAssembler();
        (*stream) << "nop" << endl;
        stream = cStringerStreamPtr();
        RISCV_ASSERT(binary->getCurrentBlockData().getSize() == before + 2);
    }

    // Assemble a statement with the stream on a FirstPassBinary and return the
    // bytes as hex
    cString assembleStatement(uint bits, const char* mattr, const char* text, bool isCompressing)
    {
        FirstPassBinaryPtr binary(new FirstPassBinary((bits == 32) ? OpcodeSubsystems::DISASSEMBLER_RISCV_32 :
                                                      OpcodeSubsystems::DISASSEMBLER_RISCV_64, false));
        binary->createNewBlockWithoutChange(0, StackInterfacePtr(new TempClassInterface(binary)));
        binary->changeBasicBlock(0);
        {
            RiscvAssemblerStream compiler(*binary, setOf(bits), featuresOf(mattr));
            compiler.setCompression(isCompressing);
            compiler << text << endl;
            compiler.assemble();
        }
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

    // The pseudo instructions: li (the sequences of LLVM's RISCVMatInt with the
    // bit manipulation extensions), la, call, tail, the symbol loads and
    // stores, as llvm-mc expands them (RiscvPseudoRows.inc)
    void testPseudo()
    {
        struct PseudoRow {
            uint m_bits;
            const char* m_mattr;
            const char* m_text;
            const char* m_hex;
            const char* m_compressedHex;
        };
        static const PseudoRow rows[] = { RISCV_PSEUDO_ROWS { 0, NULL, NULL, NULL, NULL } };
        uint count = 0;
        for (const PseudoRow* row = rows; row->m_text != NULL; row++)
        {
            cString hex = assembleStatement(row->m_bits, row->m_mattr, row->m_text, false);
            if (!(hex == row->m_hex))
            {
                cout << "pseudo '" << row->m_text << "' (" << row->m_mattr << "): expected "
                     << row->m_hex << " got " << hex << endl;
                RISCV_ASSERT(false);
            }
            hex = assembleStatement(row->m_bits, row->m_mattr, row->m_text, true);
            if (!(hex == row->m_compressedHex))
            {
                cout << "pseudo '" << row->m_text << "' (" << row->m_mattr << "), compressed: expected "
                     << row->m_compressedHex << " got " << hex << endl;
                RISCV_ASSERT(false);
            }
            count++;
        }
        RISCV_ASSERT(count > 100);
    }

    // The factory of the stream disassembler
    void testFactory()
    {
        const uint8 code[] = { 0x13, 0x00, 0x00, 0x00 };
        cBuffer data(code, sizeof(code));
        cMemoryStream stream(data);
        StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
            OpcodeSubsystems::DISASSEMBLER_RISCV_32,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0x1000));
        OpcodePtr opcode = disassembler->next();
        DefaultOpcodeDataFormatter formatter(1);
        RISCV_ASSERT(disassembler->getOpcodeFormat(opcode, formatter)->string() == "nop");
        RISCV_ASSERT(disassembler->getType() == OpcodeSubsystems::DISASSEMBLER_RISCV_32);
        RISCV_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_RISCV_32);
    }

    virtual void test()
    {
        testRows();
        testInvalid();
        testInstructionSets();
        testFeatures();
        testFormats();
        testStream();
        testOpcodes();
        testAssembler();
        testFirstPassBinary();
        testPseudo();
        testFactory();
        if (gRiscvFailures != 0)
            XSTL_THROW(cException, XSTL_STRING("Condition asserted"));
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestRiscvDisassembler g_globalTestRiscvDisassembler;
