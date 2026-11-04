/*
 * TestArmAArch32Disassembler.cpp
 *
 * Tests of the AArch32 disassembler and assembler (A32 and T32, the integer,
 * floating point and Advanced SIMD instructions): the text of the decoded
 * words and the assembled bytes for every mnemonic and alias (the rows of
 * tests/arm/AArch32TestRows_*.inc, the LLVM text), the invalid words, the
 * features (an encoding of a feature which is off is invalid, the encodings
 * which exist before ARMv8 only), the it blocks of Thumb, the stream (fault
 * tolerance, addresses, truncated data) and the assembler on a
 * FirstPassBinary.
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
#include "dismount/proc/arm/ArmNotation.h"
#include "dismount/assembler/proc/arm/ArmAssembler.h"

// Like TESTS_ASSERT, but tell which check failed
#define AARCH32_ASSERT(condition) \
    if (!(condition)) \
    { \
        fprintf(stderr, "TestArmAArch32Disassembler.cpp:%d: %s\n", __LINE__, #condition); \
        XSTL_THROW(cException, XSTL_STRING("Condition asserted")); \
    }

// The rows of each area: A32 {address, word, text}, T32 {address, size,
// encoding, it state, text}. The files of the areas which are not there are
// empty
#include "arm/AArch32TestRows_A32.inc"
#if defined(__has_include)
#  if __has_include("arm/AArch32TestRows_T32.inc")
#    include "arm/AArch32TestRows_T32.inc"
#  endif
#  if __has_include("arm/AArch32TestRows_Simd.inc")
#    include "arm/AArch32TestRows_Simd.inc"
#  endif
#endif
#ifndef AARCH32_T32_ROWS
#  define AARCH32_T32_ROWS
#  define AARCH32_T32_INVALID
#endif
#ifndef AARCH32_SIMD_A32_ROWS
#  define AARCH32_SIMD_A32_ROWS
#  define AARCH32_SIMD_T32_ROWS
#  define AARCH32_SIMD_INVALID
#endif

class TestObjectTestArmAArch32Disassembler : public cTestObject {
public:
    // A row of A32: the text of a word at an address
    struct TextRow {
        uint64 m_address;
        uint32 m_word;
        const char* m_text;
    };

    // A row of T32: the text of an encoding at an address, with the it state
    struct ThumbRow {
        uint64 m_address;
        uint m_size;
        uint32 m_encoding;
        uint m_itState;
        const char* m_text;
    };

    // An encoding which must be invalid
    struct InvalidRow {
        uint32 m_encoding;
        uint m_size;
        bool m_isThumb;
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

    // The text of an instruction (the bits of 'size' bytes) decoded in the
    // given state. 'isValid' is set to false if the bits are invalid
    cString decodeText(ArmInstructionSet::DisassemblerTypes type, uint32 encoding, uint size,
                       uint64 address, const ArmFeatureSet& features, uint itState,
                       bool& isValid)
    {
        ArmOpcodePtr opcode = ArmStreamDisassembler::decode(type, encoding, size, address,
                                                            features, itState);
        isValid = !opcode.isEmpty();
        if (!isValid)
            return cString();
        AARCH32_ASSERT(opcode->getOpcodeSize() == size);
        AARCH32_ASSERT(opcode->getEncoding() == encoding);
        DefaultOpcodeDataFormatter formatter(1);
        return formatText(opcode, formatter);
    }

    // The text of a decoded instruction
    cString formatText(const ArmOpcodePtr& opcode, DefaultOpcodeDataFormatter& formatter)
    {
        OpcodeFormatterPtr formatted(new ArmNotation(OpcodePtr(static_cast<Opcode*>(const_cast<ArmOpcode*>(opcode.getPointer())),
                                                               SMARTPTR_DESTRUCT_NONE),
                                                     formatter));
        return formatted->string();
    }

    // Assemble a line, return the bits. 'isValid' is false if the line is
    // rejected
    uint32 assemble(ArmInstructionSet::DisassemblerTypes type, const char* text, uint64 address,
                    const ArmFeatureSet& features, uint itState, uint& size, bool& isValid)
    {
        uint32 encoding = 0;
        size = 0;
        isValid = true;
        XSTL_TRY
        {
            ArmAssemblerStream::assembleInstruction(type, features, cString(text), address,
                                                    itState, encoding, size);
        }
        XSTL_CATCH(cException&)
        {
            isValid = false;
        }
        return encoding;
    }

    uint32 assembleA32(const char* text, uint64 address, bool& isValid)
    {
        uint size;
        return assemble(ArmInstructionSet::ARM_A32, text, address, ArmFeatureSet::all(), 0, size,
                        isValid);
    }

    // Check a row: the bits give the text, the text gives the bits back or
    // bits with the same text (the fields that the decoder ignores)
    void checkRow(ArmInstructionSet::DisassemblerTypes type, uint64 address, uint size,
                  uint32 encoding, uint itState, const char* expected)
    {
        bool isValid;
        cString text = decodeText(type, encoding, size, address, ArmFeatureSet::all(), itState,
                                  isValid);
        if (!isValid || !(text == expected))
        {
            cout << "row " << HEXDWORD(encoding) << ": expected '" << expected
                 << "' got '" << (isValid ? text : cString("(invalid)")) << "'" << endl;
            AARCH32_ASSERT(false);
        }
        uint assembledSize;
        uint32 assembled = assemble(type, expected, address, ArmFeatureSet::all(), itState,
                                    assembledSize, isValid);
        if (!isValid || (assembled != encoding) || (assembledSize != size))
        {
            // Other bits are fine if they are the same instruction
            cString other = isValid ?
                decodeText(type, assembled, assembledSize, address, ArmFeatureSet::all(),
                           itState, isValid) : cString();
            if (!isValid || !(other == expected))
            {
                cout << "row " << HEXDWORD(encoding) << ": '" << expected
                     << "' assembled to " << HEXDWORD(assembled) << endl;
                AARCH32_ASSERT(false);
            }
        }
    }

    // The rows of A32
    void testRows()
    {
        static const TextRow rows[] = { AARCH32_A32_ROWS AARCH32_SIMD_A32_ROWS { 0, 0, NULL } };
        uint count = 0;
        for (const TextRow* row = rows; row->m_text != NULL; row++)
        {
            checkRow(ArmInstructionSet::ARM_A32, row->m_address, 4, row->m_word, 0, row->m_text);
            count++;
        }
        AARCH32_ASSERT(count > 500);
    }

    // The rows of T32 (and the it blocks)
    void testThumbRows()
    {
        static const ThumbRow rows[] = { AARCH32_T32_ROWS AARCH32_SIMD_T32_ROWS { 0, 0, 0, 0, NULL } };
        for (const ThumbRow* row = rows; row->m_text != NULL; row++)
            checkRow(ArmInstructionSet::ARM_T32, row->m_address, row->m_size, row->m_encoding,
                     row->m_itState, row->m_text);
    }

    // The words which are not instructions
    void testInvalid()
    {
        static const uint32 words[] = { AARCH32_A32_INVALID 0xffffffff };
        for (uint i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        {
            bool isValid;
            decodeText(ArmInstructionSet::ARM_A32, words[i], 4, 0x1000, ArmFeatureSet::all(), 0,
                       isValid);
            if (isValid)
                cout << "invalid word " << HEXDWORD(words[i]) << " decoded" << endl;
            AARCH32_ASSERT(!isValid);
        }
        static const InvalidRow more[] = { AARCH32_T32_INVALID AARCH32_SIMD_INVALID { 0, 0, false } };
        for (const InvalidRow* row = more; row->m_size != 0; row++)
        {
            bool isValid;
            decodeText(row->m_isThumb ? ArmInstructionSet::ARM_T32 : ArmInstructionSet::ARM_A32,
                       row->m_encoding, row->m_size, 0x1000, ArmFeatureSet::all(), 0, isValid);
            AARCH32_ASSERT(!isValid);
        }
        // The condition 1111 is not a condition: add r0, r1, r2 with it
        bool isValid;
        decodeText(ArmInstructionSet::ARM_A32, 0xf0810002, 4, 0, ArmFeatureSet::all(), 0, isValid);
        AARCH32_ASSERT(!isValid);
    }

    // The features and the architecture versions
    void testFeatures()
    {
        bool isValid;
        // clz r0, r1 is ARMv5T
        const uint32 clz = 0xe16f0f11;
        AARCH32_ASSERT(decodeText(ArmInstructionSet::ARM_A32, clz, 4, 0, ArmFeatureSet::all(), 0,
                                  isValid) == "clz r0, r1");
        AARCH32_ASSERT(isValid);
        ArmFeatureSet base;
        decodeText(ArmInstructionSet::ARM_A32, clz, 4, 0, base, 0, isValid);
        AARCH32_ASSERT(!isValid);
        uint size;
        assemble(ArmInstructionSet::ARM_A32, "clz r0, r1", 0, base, 0, size, isValid);
        AARCH32_ASSERT(!isValid);
        AARCH32_ASSERT(assembleA32("clz r0, r1", 0, isValid) == clz);
        AARCH32_ASSERT(isValid);
        ArmFeatureSet v5;
        AARCH32_ASSERT(v5.parse("+v5t"));
        decodeText(ArmInstructionSet::ARM_A32, clz, 4, 0, v5, 0, isValid);
        AARCH32_ASSERT(isValid);

        // swp exists before ARMv8 only
        const uint32 swp = 0xe1002091;
        AARCH32_ASSERT(decodeText(ArmInstructionSet::ARM_A32, swp, 4, 0,
                                  ArmFeatureSet::fromArchitecture("armv7-a"), 0, isValid) ==
                       "swp r2, r1, [r0]");
        AARCH32_ASSERT(isValid);
        decodeText(ArmInstructionSet::ARM_A32, swp, 4, 0, ArmFeatureSet::fromArchitecture("armv8-a"),
                   0, isValid);
        AARCH32_ASSERT(!isValid);
        // sdiv: hwdiv-arm
        const uint32 sdiv = 0xe710f211;
        decodeText(ArmInstructionSet::ARM_A32, sdiv, 4, 0, ArmFeatureSet::fromArchitecture("armv7-a"),
                   0, isValid);
        AARCH32_ASSERT(!isValid);
        decodeText(ArmInstructionSet::ARM_A32, sdiv, 4, 0, ArmFeatureSet::fromArchitecture("armv8-a"),
                   0, isValid);
        AARCH32_ASSERT(isValid);
        // The architecture versions bring their earlier ones
        ArmFeatureSet set;
        AARCH32_ASSERT(set.parse("+v8"));
        AARCH32_ASSERT(set.has(ArmFeatures::FEAT_V7) && set.has(ArmFeatures::FEAT_V6T2) &&
                       set.has(ArmFeatures::FEAT_V5TE));
        AARCH32_ASSERT(set.parse("-v6"));
        AARCH32_ASSERT(!set.has(ArmFeatures::FEAT_V7) && set.has(ArmFeatures::FEAT_V5TE));
        ArmFeatures::Feature feature;
        AARCH32_ASSERT(ArmFeatures::findFeature("v6t2", feature) && (feature == ArmFeatures::FEAT_V6T2));
        AARCH32_ASSERT(ArmFeatures::getInstructionSet(ArmFeatures::FEAT_V5TE) ==
                       ArmInstructionSet::ARM_A32);
        AARCH32_ASSERT(ArmFeatures::getInstructionSet(ArmFeatures::FEAT_V6T2) ==
                       ArmInstructionSet::ARM_T32);
        AARCH32_ASSERT((ArmFeatures::getSupport(ArmFeatures::FEAT_A32_BASE) &
                        ArmFeatures::SUPPORT_ENCODING_VERIFIED) != 0);
        AARCH32_ASSERT(cString(ArmFeatures::getName(ArmFeatures::FEAT_V6)) == "v6");
    }

    // Disassemble the bytes with the stream disassembler. Return the texts
    void disassembleStream(ArmInstructionSet::DisassemblerTypes type, const cBuffer& data,
                           cArray<cString>& texts, bool isFaultTolerant)
    {
        cMemoryStream stream(data);
        ArmStreamDisassembler disassembler(type, BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE),
            true, ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0x400000), isFaultTolerant);
        DefaultOpcodeDataFormatter formatter(1);
        while (!disassembler.isEndOfStream())
        {
            OpcodePtr opcode = disassembler.next();
            texts.append(disassembler.getOpcodeFormat(opcode, formatter)->string());
        }
    }

    // The stream: addresses, fault tolerance, truncated data
    void testStream()
    {
        // add r0, r1, r2; invalid; bx lr
        const uint8 code[] = { 0x02, 0x00, 0x81, 0xe0,
                               0xff, 0xff, 0xff, 0xff,
                               0x1e, 0xff, 0x2f, 0xe1 };
        cBuffer data(code, sizeof(code));
        cMemoryStream stream(data);
        ArmStreamDisassembler disassembler(ArmInstructionSet::ARM_A32,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0x400000), true);
        DefaultOpcodeDataFormatter formatter(1);

        OpcodePtr opcode = disassembler.next();
        ProcessorAddress address(gNullPointerProcessorAddress);
        AARCH32_ASSERT(opcode->getOpcodeAddress(address) && (address.getAddress() == 0x400000));
        AARCH32_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "add r0, r1, r2");
        AARCH32_ASSERT(!opcode->isBranch());
        AARCH32_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_ARM_32_LE);
        AARCH32_ASSERT(opcode->getOpcode().getSize() == 4);
        AARCH32_ASSERT(opcode->getOpcode()[3] == 0xe0);

        // The invalid word: one InvalidOpcodeByte for each byte
        for (uint i = 0; i < 4; i++)
        {
            opcode = disassembler.next();
            AARCH32_ASSERT(opcode->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE);
            AARCH32_ASSERT(opcode->getOpcodeAddress(address) && (address.getAddress() == 0x400004 + i));
        }
        opcode = disassembler.next();
        AARCH32_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "bx lr");
        AARCH32_ASSERT(opcode->isBranch());
        AARCH32_ASSERT(disassembler.isEndOfStream());

        // Without the fault tolerance the invalid word is an exception
        cMemoryStream stream2(data);
        ArmStreamDisassembler strict(ArmInstructionSet::ARM_A32,
            BasicInputPtr(&stream2, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0), false);
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
        AARCH32_ASSERT(isThrown);

        // A truncated instruction
        const uint8 half[] = { 0x20, 0x00 };
        cBuffer halfData(half, sizeof(half));
        cMemoryStream stream3(halfData);
        ArmStreamDisassembler truncated(ArmInstructionSet::ARM_A32,
            BasicInputPtr(&stream3, SMARTPTR_DESTRUCT_NONE), false,
            ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0), false);
        isThrown = false;
        XSTL_TRY
        {
            truncated.next();
        }
        XSTL_CATCH(DisassemblerEndOfStreamException&)
        {
            isThrown = true;
        }
        AARCH32_ASSERT(isThrown);

        // The factory
        cMemoryStream stream4(cBuffer(code, 4));
        StreamDisassemblerPtr factory = StreamDisassemblerFactory::disassemble(
            OpcodeSubsystems::DISASSEMBLER_ARM_32_LE,
            BasicInputPtr(&stream4, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0x1000));
        opcode = factory->next();
        AARCH32_ASSERT(factory->getOpcodeFormat(opcode, formatter)->string() == "add r0, r1, r2");
        AARCH32_ASSERT(factory->getType() == OpcodeSubsystems::DISASSEMBLER_ARM_32_LE);
    }

    // The branch targets: absolute addresses (pc + 8), the relative forms
    void testBranches()
    {
        bool isValid;
        // b .+8 at 0x1000 is 0xea000000 (the offset is from pc + 8)
        AARCH32_ASSERT(decodeText(ArmInstructionSet::ARM_A32, 0xea000000, 4, 0x1000,
                                  ArmFeatureSet::all(), 0, isValid) == "b 0x1008");
        AARCH32_ASSERT(assembleA32("b .+8", 0x1000, isValid) == 0xea000000);
        AARCH32_ASSERT(assembleA32("b #8", 0x1000, isValid) == 0xea000000);
        AARCH32_ASSERT(assembleA32("b 0x1008", 0x1000, isValid) == 0xea000000);
        AARCH32_ASSERT(assembleA32("bl .-4", 0x1000, isValid) == 0xebfffffd);
        AARCH32_ASSERT(assembleA32("bne .+8", 0x2000, isValid) == 0x1a000000);
        AARCH32_ASSERT(decodeText(ArmInstructionSet::ARM_A32, 0x1a000000, 4, 0x2000,
                                  ArmFeatureSet::all(), 0, isValid) == "bne 0x2008");
        // blx to a Thumb function: the half word bit
        AARCH32_ASSERT(decodeText(ArmInstructionSet::ARM_A32, 0xfb000000, 4, 0x1000,
                                  ArmFeatureSet::all(), 0, isValid) == "blx 0x100a");
        AARCH32_ASSERT(assembleA32("blx 0x100a", 0x1000, isValid) == 0xfb000000);
        // Misaligned and out of range offsets are rejected
        assembleA32("b .+2", 0x1000, isValid);
        AARCH32_ASSERT(!isValid);
        assembleA32("b .+134217728", 0x1000, isValid);
        AARCH32_ASSERT(!isValid);
    }

    // The syntax of the assembler
    void testAssembler()
    {
        bool isValid;
        AARCH32_ASSERT(assembleA32("add r0, r1, r2", 0, isValid) == 0xe0810002);
        AARCH32_ASSERT(assembleA32("ADD R0, R1, R2", 0, isValid) == 0xe0810002);
        AARCH32_ASSERT(assembleA32("add r0, r1, r2 @ comment", 0, isValid) == 0xe0810002);
        AARCH32_ASSERT(assembleA32("add r0, r1, r2 // comment", 0, isValid) == 0xe0810002);
        AARCH32_ASSERT(assembleA32("addeq r0, r1, r2", 0, isValid) == 0x00810002);
        AARCH32_ASSERT(assembleA32("addseq r0, r1, r2", 0, isValid) == 0x00910002);
        AARCH32_ASSERT(assembleA32("add r0, r1, r2, lsl #3", 0, isValid) == 0xe0810182);
        AARCH32_ASSERT(assembleA32("add r0, r1, r2, lsl r3", 0, isValid) == 0xe0810312);
        AARCH32_ASSERT(assembleA32("add r0, r1, #255", 0, isValid) == 0xe28100ff);
        AARCH32_ASSERT(assembleA32("add r0, r1, #0x100", 0, isValid) == 0xe2810c01);
        AARCH32_ASSERT(assembleA32("add r0, r1, #257", 0, isValid) == 0 && !isValid);
        AARCH32_ASSERT(assembleA32("mov r0, #1, #30", 0, isValid) == 0xe3a00f01);
        AARCH32_ASSERT(assembleA32("mov r0, r1", 0, isValid) == 0xe1a00001);
        AARCH32_ASSERT(assembleA32("lsl r0, r1, #2", 0, isValid) == 0xe1a00101);
        AARCH32_ASSERT(assembleA32("movw r0, #0x1234", 0, isValid) == 0xe3010234);
        AARCH32_ASSERT(assembleA32("ldr r0, [r1, #8]", 0, isValid) == 0xe5910008);
        AARCH32_ASSERT(assembleA32("ldr r0, [r1, #-8]", 0, isValid) == 0xe5110008);
        AARCH32_ASSERT(assembleA32("ldr r0, [r1, #8]!", 0, isValid) == 0xe5b10008);
        AARCH32_ASSERT(assembleA32("ldr r0, [r1], #8", 0, isValid) == 0xe4910008);
        AARCH32_ASSERT(assembleA32("ldr r0, [r1, r2, lsl #2]", 0, isValid) == 0xe7910102);
        AARCH32_ASSERT(assembleA32("ldr r0, [r1, -r2]", 0, isValid) == 0xe7110002);
        AARCH32_ASSERT(assembleA32("ldr r0, [r1], r2", 0, isValid) == 0xe6910002);
        AARCH32_ASSERT(assembleA32("ldrh r0, [r1, #2]", 0, isValid) == 0xe1d100b2);
        AARCH32_ASSERT(assembleA32("ldrd r0, r1, [r2, #8]", 0, isValid) == 0xe1c200d8);
        AARCH32_ASSERT(assembleA32("ldr r0, [r1, #4096]", 0, isValid) == 0 && !isValid);
        AARCH32_ASSERT(assembleA32("push {r4, lr}", 0, isValid) == 0xe92d4010);
        AARCH32_ASSERT(assembleA32("stmdb sp!, {r4, lr}", 0, isValid) == 0xe92d4010);
        AARCH32_ASSERT(assembleA32("pop {r4, pc}", 0, isValid) == 0xe8bd8010);
        AARCH32_ASSERT(assembleA32("ldm r0, {r1, r2-r4}", 0, isValid) == 0xe890001e);
        AARCH32_ASSERT(assembleA32("bx lr", 0, isValid) == 0xe12fff1e);
        AARCH32_ASSERT(assembleA32("nop", 0, isValid) == 0xe320f000);
        AARCH32_ASSERT(assembleA32("dmb ish", 0, isValid) == 0xf57ff05b);
        AARCH32_ASSERT(assembleA32("msr cpsr_fc, r0", 0, isValid) == 0xe129f000);
        AARCH32_ASSERT(assembleA32("mrs r0, apsr", 0, isValid) == 0xe10f0000);
        AARCH32_ASSERT(assembleA32("mcr p15, #0, r0, c7, c10, #5", 0, isValid) == 0xee070fba);
        AARCH32_ASSERT(assembleA32("svc #0x123456", 0, isValid) == 0xef123456);
        AARCH32_ASSERT(assembleA32("frobnicate r0", 0, isValid) == 0 && !isValid);
        AARCH32_ASSERT(assembleA32("", 0, isValid) == 0 && !isValid);
    }

    // The assembler on a FirstPassBinary: text lines and a branch to a label
    void testFirstPassBinary()
    {
        FirstPassBinaryPtr binary(new FirstPassBinary(OpcodeSubsystems::DISASSEMBLER_ARM_32_LE, false));
        binary->createNewBlockWithoutChange(0, StackInterfacePtr(new TempClassInterface(binary)));
        binary->changeBasicBlock(0);
        AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                        binary, OpcodeSubsystems::DISASSEMBLER_ARM_32_LE);
        {
            ArmAssemblerStream compiler(*binary, ArmInstructionSet::ARM_A32);
            compiler << "push {r4, lr}" << endl;
            compiler << "mov r4, sp" << endl;
            compiler << "bl callee" << endl;
            compiler << "pop {r4, pc}" << endl;
            compiler.assemble();
        }
        const cBuffer& code = binary->getCurrentBlockData();
        AARCH32_ASSERT(code.getSize() == 16);
        // push {r4, lr} = e92d4010 (little endian)
        AARCH32_ASSERT(code[0] == 0x10 && code[1] == 0x40 && code[2] == 0x2d && code[3] == 0xe9);
        // bl callee: the offset is left 0 and callee is a dependency
        AARCH32_ASSERT(code[8] == 0 && code[9] == 0 && code[10] == 0 && code[11] == 0xeb);
        AARCH32_ASSERT(binary->getCurrentDependecies().getList().length() == 1);

        // Through the interface: the stream of the factory
        cStringerStreamPtr stream = assembler->getAssembler();
        (*stream) << "nop" << endl;
        stream = cStringerStreamPtr();
        AARCH32_ASSERT(binary->getCurrentBlockData().getSize() == 20);
    }

    // The flow properties and the operands
    void testOpcodes()
    {
        const uint32 words[] = { 0xea000000 /* b */, 0xeb000000 /* bl */, 0xe12fff10 /* bx r0 */,
                                 0x0a000000 /* beq */, 0xe12fff1e /* bx lr */, 0xe0810002 /* add */ };
        const bool isBranch[] = { true, true, true, true, true, false };
        for (uint i = 0; i < 6; i++)
        {
            ArmOpcodePtr opcode = ArmStreamDisassembler::decode(
                ArmInstructionSet::ARM_A32, words[i], 4, 0x1000, ArmFeatureSet::all());
            AARCH32_ASSERT(!opcode.isEmpty());
            AARCH32_ASSERT(opcode->isBranch() == isBranch[i]);
            AARCH32_ASSERT(opcode->getEncoding() == words[i]);
        }
        ArmOpcodePtr add = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A32, 0xe0810002, 4, 0x1000, ArmFeatureSet::all());
        AARCH32_ASSERT(add->getOperandsCount() == 3);
        AARCH32_ASSERT(add->getOperand(0).m_kind == ArmDecodedOperand::KIND_REGISTER);
        AARCH32_ASSERT(add->getOperand(0).m_register == armdis::ARM_REG_R0);
        AARCH32_ASSERT(add->getOperand(1).m_register == armdis::ARM_REG_R0 + 1);
        AARCH32_ASSERT(add->getOperand(2).m_register == armdis::ARM_REG_R0 + 2);
        AARCH32_ASSERT(add->getMnemonic() == "add");
        AARCH32_ASSERT(add->getCondition() == ArmOpcode::CONDITION_ALWAYS);
        AARCH32_ASSERT(!add->isSettingFlags());

        ArmOpcodePtr adds = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A32, 0x00910002, 4, 0x1000, ArmFeatureSet::all());
        AARCH32_ASSERT(adds->getMnemonic() == "addseq");
        AARCH32_ASSERT(adds->getCondition() == 0);
        AARCH32_ASSERT(adds->isSettingFlags());

        ArmOpcodePtr beq = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A32, 0x0a000000, 4, 0x1000, ArmFeatureSet::all());
        AARCH32_ASSERT(beq->getMnemonic() == "beq");
        AARCH32_ASSERT(beq->getOperand(0).m_kind == ArmDecodedOperand::KIND_BRANCH_TARGET);
        AARCH32_ASSERT(beq->getOperand(0).m_immediate == 0x1008);

        ArmOpcodePtr ldr = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A32, 0xe5b10008, 4, 0, ArmFeatureSet::all());
        AARCH32_ASSERT(ldr->getOperand(1).m_kind == ArmDecodedOperand::KIND_MEMORY);
        AARCH32_ASSERT(ldr->getOperand(1).m_base == armdis::ARM_REG_R0 + 1);
        AARCH32_ASSERT(ldr->getOperand(1).m_offset == 8);
        AARCH32_ASSERT(ldr->getOperand(1).m_addressing == ArmDecodedOperand::ADDRESSING_PRE_INDEX);
        AARCH32_ASSERT(!ldr->getOperand(1).m_isSubtract);

        ArmOpcodePtr pop = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A32, 0xe8bd8010, 4, 0, ArmFeatureSet::all());
        AARCH32_ASSERT(pop->getMnemonic() == "pop");
        AARCH32_ASSERT(pop->getOperand(0).m_kind == ArmDecodedOperand::KIND_REGISTER_LIST);
        AARCH32_ASSERT(pop->getOperand(0).m_registerMask == 0x8010);
    }

    // The it blocks, the sizes and the stream of Thumb
    void testThumb()
    {
        // Thumb is there only when the T32 tables are
        if (ArmStreamDisassembler::decode(ArmInstructionSet::ARM_T32, 0x1888, 2, 0,
                                          ArmFeatureSet::all()).isEmpty())
            return;
        bool isValid;
        uint size;
        // The narrow form is preferred (like clang), the wide one with .w
        AARCH32_ASSERT(assemble(ArmInstructionSet::ARM_T32, "adds r0, r1, r2", 0,
                                ArmFeatureSet::all(), 0, size, isValid) == 0x1888);
        AARCH32_ASSERT(isValid && (size == 2));
        assemble(ArmInstructionSet::ARM_T32, "add.w r0, r1, r2", 0, ArmFeatureSet::all(), 0, size,
                 isValid);
        AARCH32_ASSERT(isValid && (size == 4));
        // add r0, r1, r2 outside of an it block doesn't set the flags: wide
        assemble(ArmInstructionSet::ARM_T32, "add r0, r1, r2", 0, ArmFeatureSet::all(), 0, size,
                 isValid);
        AARCH32_ASSERT(isValid && (size == 4));
        // inside an it block it is the narrow one (eq: firstcond 0, mask 1000)
        AARCH32_ASSERT(assemble(ArmInstructionSet::ARM_T32, "addeq r0, r1, r2", 0,
                                ArmFeatureSet::all(), 0x08, size, isValid) == 0x1888);
        AARCH32_ASSERT(isValid && (size == 2));
        // the condition has to be the one of the block
        assemble(ArmInstructionSet::ARM_T32, "addne r0, r1, r2", 0, ArmFeatureSet::all(), 0x08,
                 size, isValid);
        AARCH32_ASSERT(!isValid);
        assemble(ArmInstructionSet::ARM_T32, "addeq r0, r1, r2", 0, ArmFeatureSet::all(), 0, size,
                 isValid);
        AARCH32_ASSERT(!isValid);

        // A block assembled from lines and disassembled again
        FirstPassBinaryPtr binary(new FirstPassBinary(OpcodeSubsystems::DISASSEMBLER_THUMB_LE, false));
        binary->createNewBlockWithoutChange(0, StackInterfacePtr(new TempClassInterface(binary)));
        binary->changeBasicBlock(0);
        {
            ArmAssemblerStream compiler(*binary, ArmInstructionSet::ARM_T32);
            compiler << "ite ne" << endl;
            compiler << "addne r0, r1, r2" << endl;
            compiler << "subeq r0, r1, r2" << endl;
            compiler << "adds r0, r1, r2" << endl;
            compiler << "bx lr" << endl;
            compiler.assemble();
        }
        cArray<cString> texts;
        disassembleStream(ArmInstructionSet::ARM_T32, binary->getCurrentBlockData(), texts, false);
        AARCH32_ASSERT(texts.getSize() == 5);
        AARCH32_ASSERT(texts[0] == "ite ne");
        AARCH32_ASSERT(texts[1] == "addne r0, r1, r2");
        AARCH32_ASSERT(texts[2] == "subeq r0, r1, r2");
        AARCH32_ASSERT(texts[3] == "adds r0, r1, r2");
        AARCH32_ASSERT(texts[4] == "bx lr");

        // jumpToAddress resets the it block
        const uint8 code[] = { 0x08, 0xbf, 0x88, 0x18, 0x88, 0x18 };
        cBuffer data(code, sizeof(code));
        cMemoryStream stream(data);
        ArmStreamDisassembler disassembler(ArmInstructionSet::ARM_T32,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0x1000), false);
        DefaultOpcodeDataFormatter formatter(1);
        OpcodePtr opcode = disassembler.next();
        AARCH32_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "it eq");
        opcode = disassembler.next();
        AARCH32_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "addeq r0, r1, r2");
        AARCH32_ASSERT(static_cast<ArmOpcode*>(opcode.getPointer())->isInItBlock());
        AARCH32_ASSERT(static_cast<ArmOpcode*>(opcode.getPointer())->isLastInItBlock());
        AARCH32_ASSERT(static_cast<ArmOpcode*>(opcode.getPointer())->getCondition() == 0);
        opcode = disassembler.next();
        AARCH32_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "adds r0, r1, r2");
        AARCH32_ASSERT(!static_cast<ArmOpcode*>(opcode.getPointer())->isInItBlock());
        disassembler.jumpToAddress(ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0x1000), 0);
        disassembler.next();
        disassembler.jumpToAddress(ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0x1002), 0);
        opcode = disassembler.next();
        AARCH32_ASSERT(disassembler.getOpcodeFormat(opcode, formatter)->string() == "adds r0, r1, r2");
        AARCH32_ASSERT(opcode->getOpcodeSize() == 2);
    }

    // The features of Thumb: the 16 bit base, Thumb-2, the divide and the
    // architecture versions
    void testThumbFeatures()
    {
        if (ArmStreamDisassembler::decode(ArmInstructionSet::ARM_T32, 0x1888, 2, 0,
                                          ArmFeatureSet::all()).isEmpty())
            return;
        bool isValid;
        ArmFeatureSet base;
        // bx lr: the 16 bit Thumb of ARMv4T
        AARCH32_ASSERT(decodeText(ArmInstructionSet::ARM_T32, 0x4770, 2, 0, base, 0, isValid) ==
                       "bx lr");
        AARCH32_ASSERT(isValid);
        // it eq, cbz and the 32 bit encodings are Thumb-2
        decodeText(ArmInstructionSet::ARM_T32, 0xbf08, 2, 0, base, 0, isValid);
        AARCH32_ASSERT(!isValid);
        decodeText(ArmInstructionSet::ARM_T32, 0xbf08, 2, 0, ArmFeatureSet::fromArchitecture("armv7-a"),
                   0, isValid);
        AARCH32_ASSERT(isValid);
        uint size;
        uint32 movw = assemble(ArmInstructionSet::ARM_T32, "movw r0, #0x1234", 0,
                               ArmFeatureSet::all(), 0, size, isValid);
        AARCH32_ASSERT(isValid && (size == 4));
        decodeText(ArmInstructionSet::ARM_T32, movw, 4, 0, base, 0, isValid);
        AARCH32_ASSERT(!isValid);
        ArmFeatureSet v6t2 = ArmFeatureSet::fromArchitecture("armv6t2");
        AARCH32_ASSERT(decodeText(ArmInstructionSet::ARM_T32, movw, 4, 0, v6t2, 0, isValid) ==
                       "movw r0, #0x1234");
        AARCH32_ASSERT(isValid);
        assemble(ArmInstructionSet::ARM_T32, "movw r0, #0x1234", 0, base, 0, size, isValid);
        AARCH32_ASSERT(!isValid);
        // udiv: hwdiv in Thumb (ARMv7-A has no divide, ARMv8-A has)
        uint32 udiv = assemble(ArmInstructionSet::ARM_T32, "udiv r0, r1, r2", 0,
                               ArmFeatureSet::all(), 0, size, isValid);
        AARCH32_ASSERT(isValid && (size == 4));
        decodeText(ArmInstructionSet::ARM_T32, udiv, 4, 0, ArmFeatureSet::fromArchitecture("armv7-a"),
                   0, isValid);
        AARCH32_ASSERT(!isValid);
        decodeText(ArmInstructionSet::ARM_T32, udiv, 4, 0, ArmFeatureSet::fromArchitecture("armv8-a"),
                   0, isValid);
        AARCH32_ASSERT(isValid);
        // The floating point of Thumb needs the VFP features
        uint32 vadd = assemble(ArmInstructionSet::ARM_T32, "vadd.f32 s0, s1, s2", 0,
                               ArmFeatureSet::all(), 0, size, isValid);
        AARCH32_ASSERT(isValid && (size == 4));
        decodeText(ArmInstructionSet::ARM_T32, vadd, 4, 0, v6t2, 0, isValid);
        AARCH32_ASSERT(!isValid);
        ArmFeatureSet vfp = v6t2;
        AARCH32_ASSERT(vfp.parse("+vfp2"));
        AARCH32_ASSERT(decodeText(ArmInstructionSet::ARM_T32, vadd, 4, 0, vfp, 0, isValid) ==
                       "vadd.f32 s0, s1, s2");
        AARCH32_ASSERT(isValid);
        // inside an it block the condition is added
        AARCH32_ASSERT(decodeText(ArmInstructionSet::ARM_T32, vadd, 4, 0, vfp, 0x08, isValid) ==
                       "vaddeq.f32 s0, s1, s2");
    }

    virtual void test()
    {
        testRows();
        testThumbRows();
        testInvalid();
        testFeatures();
        testStream();
        testBranches();
        testAssembler();
        testFirstPassBinary();
        testOpcodes();
        testThumb();
        testThumbFeatures();
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestArmAArch32Disassembler g_globalTestArmAArch32Disassembler;
