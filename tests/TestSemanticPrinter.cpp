/*
 * TestSemanticPrinter.cpp
 *
 * Tests the text dump of the semantic IR (SemanticPrinter): RISC-V RV64I
 * instructions are decoded, lifted with the RISC-V lifter and printed, and the
 * text is compared with the expected dump, line by line. The expected dumps
 * were written by hand from the lifter and the format of SemanticPrinter.h.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/string.h"
#include "xStl/data/datastream.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/stream/ioStream.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/stream/stringerStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/Semantics.h"
#include "dismount/semantics/SemanticPrinter.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"

using namespace riscvdis;

// A string output which collects the text
class TestPrinterCapture : public cStringerStream {
public:
    TestPrinterCapture() : cStringerStream(false) {}

    virtual void outputString(const cString& string)
    {
        m_text += string;
    }

    cString m_text;
};

// One RV64I instruction of a test: its encoding and size, and the dump which
// SemanticPrinter must print for its IR
struct SemanticPrinterRow {
    uint32 m_encoding;
    uint m_size;
    const char* m_expected;
};

// The dumps of the rows, the guest address of each one is 0x80001000
static const SemanticPrinterRow gRows[] = {
    // add a0, a1, a2
    { 0x00c58533, 4,
      "; guest 0x80001000 size 4\n"
      "0x80001000: t0:i64 = add x11, x12\n"
      "0x80001000: x10 = move t0\n"
      "; fall-through 0x80001004\n" },
    // addi a0, a0, 5
    { 0x00550513, 4,
      "; guest 0x80001000 size 4\n"
      "0x80001000: t0:i64 = add x10, 5\n"
      "0x80001000: x10 = move t0\n"
      "; fall-through 0x80001004\n" },
    // beq a0, a1, +8
    { 0x00b50463, 4,
      "; guest 0x80001000 size 4\n"
      "0x80001000: t0:i1 = compare[COND_EQUAL] x10, x11\n"
      "0x80001000: branch_conditional t0, 0x80001008\n"
      "; fall-through 0x80001004\n" },
};

// Return the instruction which does not own the opcode
static OpcodePtr asOpcode(const RiscvOpcodePtr& opcode)
{
    return OpcodePtr((Opcode*)opcode.getPointer(), SMARTPTR_DESTRUCT_NONE);
}

class TestObjectTestSemanticPrinter : public cTestObject {
public:
    TestObjectTestSemanticPrinter() : m_failures(0) {}

    // Lift one row, print it and compare the dump with the expected text
    void runRow(const SemanticPrinterRow& row)
    {
        RiscvFeatureSet features;
        TESTS_ASSERT(features.parse("rv64i"));
        RiscvOpcodePtr opcode = RiscvStreamDisassembler::decode(
                RiscvInstructionSet::RISCV_RV64, row.m_encoding, row.m_size,
                0x80001000ULL, features);
        TESTS_ASSERT(opcode.getPointer() != NULL);

        SemanticLifterPtr lifter = SemanticLifterFactory::getLifter(
                                    OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, 0x80001004ULL));
        TESTS_ASSERT(lifter->lift(asOpcode(opcode), block));

        ArchitectureDescriptionPtr architecture =
                ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        TestPrinterCapture output;
        SemanticPrinter::print(block, *architecture, output);
        output.flush();

        const cString expected(row.m_expected);
        if (!(output.m_text == expected))
        {
            m_failures++;
            cout << "FAILED row encoding " << HEXDWORD(row.m_encoding) << endl;
            cout << "expected:" << endl << expected << endl;
            cout << "actual:" << endl << output.m_text << endl;
        }
    }

    virtual void test()
    {
        for (uint i = 0; i < sizeof(gRows) / sizeof(gRows[0]); i++)
            runRow(gRows[i]);
        TESTS_ASSERT_EQUAL(m_failures, 0);
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    uint m_failures;
};

// Instance test object
TestObjectTestSemanticPrinter g_globalTestSemanticPrinter;
