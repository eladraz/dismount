/*
 * TestSemanticLowering.cpp
 *
 * Tests of the lowering of the semantic IR of a guest onto the AMD64 target.
 * The file is in parts, one per stage of the lowering:
 *
 *   Part 1  The SemanticLegalizer (RISC-V RV64 guest, AMD64 target):
 *           1a  the RV64I rows of tests/riscv/RiscvSemanticRows64.inc lift
 *               and legalize, with no undefined operation left;
 *           1b  the dead values are removed, the side effects are kept;
 *           1c  the operations the target can't do are refused.
 *   Part 2  The AMD64 lowering of the operations (added by WP8a and later).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include <string.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "xStl/exceptions.h"
#include "xStl/stream/ioStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"

using namespace riscvdis;

// The rows: the code bytes in hex, the text, the setup and the expected state
struct SemanticLoweringRow {
    const char* m_code;
    const char* m_text;
    const char* m_setup;
    const char* m_expected;
};

#include "riscv/RiscvSemanticRows64.inc"

static const SemanticLoweringRow gRv64SemanticRows[] = {
    RISCV64_SEMANTIC_ROWS
};

// The mnemonics of the RV64I base instructions the rows of Part 1a are taken
// from. The system and fence instructions (ecall, ebreak, unimp, fence) are
// left out: they lower to traps and orderings, which Part 2 covers. The
// pseudo-instructions (mv, li, ret...) are not listed: they are the same
// encodings as the base instructions and are checked through them
static const char* const gRv64iMnemonics[] = {
    "add", "sub", "and", "or", "xor", "addi", "andi", "ori", "xori",
    "addw", "subw", "addiw",
    "sll", "srl", "sra", "slli", "srli", "srai",
    "sllw", "srlw", "sraw", "slliw", "srliw", "sraiw",
    "slt", "sltu", "slti", "sltiu",
    "lui", "auipc",
    "lb", "lh", "lw", "ld", "lbu", "lhu", "lwu",
    "sb", "sh", "sw", "sd",
    "beq", "bne", "blt", "bge", "bltu", "bgeu",
    "jal", "jalr"
};

// The number of rows of gRv64iMnemonics in the rows table
static const uint RV64I_ROWS_COUNT = 230;

// Report a failure of a row and count it
#define LOWERING_ROW_FAIL(row, message) \
    do { \
        fprintf(stderr, "TestSemanticLowering: %s [%s]: %s\n", (row), \
                __FILE__, (message)); \
        m_failures++; \
    } while (0)

// Return the instruction which does not own the opcode
static OpcodePtr asOpcode(const RiscvOpcodePtr& opcode)
{
    return OpcodePtr((Opcode*)opcode.getPointer(), SMARTPTR_DESTRUCT_NONE);
}

// The value of a hexadecimal digit
static uint digit(char c)
{
    return (c >= 'a') ? (uint)(c - 'a' + 10) : (uint)(c - '0');
}

// Return the guest of Part 1: RV64 with the vector length of the rows
static ArchitectureDescriptionPtr getGuest()
{
    return ArchitectureDescriptionPtr(new RiscvArchitecture(
                RiscvInstructionSet::RISCV_RV64, 128, false));
}

// Return the target of Part 1: the AMD64 processor
static ArchitectureDescriptionPtr getTarget()
{
    return ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);
}

// Return true if the row is a 4 byte encoding of an RV64I base instruction
static bool isRv64iRow(const SemanticLoweringRow& row)
{
    if (strlen(row.m_code) != 8)
        return false;
    const char* end = row.m_text;
    while ((*end != 0) && (*end != ' '))
        end++;
    const size_t length = (size_t)(end - row.m_text);
    const uint count = sizeof(gRv64iMnemonics) / sizeof(gRv64iMnemonics[0]);
    for (uint i = 0; i < count; i++)
    {
        if ((strlen(gRv64iMnemonics[i]) == length) &&
            (strncmp(gRv64iMnemonics[i], row.m_text, length) == 0))
        {
            return true;
        }
    }
    return false;
}

// Decode and lift the code of a row (at the pc 0x80001000) into 'block'.
// Return false if the code doesn't decode or lift
static bool liftRow(const SemanticLoweringRow& row, RiscvSemanticLifter& lifter,
                    SemanticBlock& block)
{
    // The bytes are in memory order, the instruction is little endian
    uint32 encoding = 0;
    for (uint i = 0; i < 4; i++)
    {
        const uint8 byte = (uint8)((digit(row.m_code[2 * i]) << 4) |
                                   digit(row.m_code[2 * i + 1]));
        encoding |= (uint32)byte << (8 * i);
    }
    RiscvOpcodePtr opcode = RiscvStreamDisassembler::decode(
                RiscvInstructionSet::RISCV_RV64, encoding, 4, 0x80001000ULL,
                RiscvFeatureSet::all());
    if (opcode.getPointer() == NULL)
        return false;
    return lifter.lift(asOpcode(opcode), block);
}

// Return true if the block has an OP_UNDEFINED
static bool hasUndefined(const SemanticBlock& block)
{
    const cList<SemanticInstruction>& instructions = block.getInstructions();
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        if ((*i).getOperation() == SemanticOperation::OP_UNDEFINED)
            return true;
    }
    return false;
}

class TestObjectTestSemanticLowering : public cTestObject {
public:
    TestObjectTestSemanticLowering() : m_failures(0) {}

    virtual void test()
    {
        // Part 1: the legalizer
        testRv64iRows();
        testDeadValues();
        testRejections();

        // Part 2: the AMD64 lowering (WP8a and later add their tests here)

        cout << "Semantic lowering tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    // ------------------------------------------------------------------
    // Part 1a: every RV64I row lifts and legalizes for AMD64
    // ------------------------------------------------------------------
    void testRv64iRows()
    {
        ArchitectureDescriptionPtr guest = getGuest();
        ArchitectureDescriptionPtr target = getTarget();
        RiscvSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_RISCV_64, guest);
        SemanticLegalizer legalizer(guest, target);

        uint rows = 0;
        for (const SemanticLoweringRow* row = gRv64SemanticRows; row->m_text != NULL; row++)
        {
            if (!isRv64iRow(*row))
                continue;
            rows++;

            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            if (!liftRow(*row, lifter, block))
            {
                LOWERING_ROW_FAIL(row->m_text, "the instruction isn't lifted");
                continue;
            }
            try
            {
                legalizer.legalize(block);
            } catch (cException&)
            {
                LOWERING_ROW_FAIL(row->m_text, "the legalizer throws");
                continue;
            }
            if (hasUndefined(block))
                LOWERING_ROW_FAIL(row->m_text, "an undefined operation is left");
        }

        cout << rows << " RV64I rows legalized, " << m_failures << " failures" << endl;
        TESTS_ASSERT(rows == RV64I_ROWS_COUNT);
        TESTS_ASSERT(m_failures == 0);
    }

    // ------------------------------------------------------------------
    // Part 1b: dead temporaries are removed, the side effects are kept
    // ------------------------------------------------------------------
    void testDeadValues()
    {
        ArchitectureDescriptionPtr guest = getGuest();
        SemanticLegalizer legalizer(guest, getTarget());
        const SemanticType type = SemanticType::integer(64);
        const SemanticOperand x1 = SemanticOperand::registerOperand(RISCV_REG_X0 + 1, type);
        const SemanticOperand x2 = SemanticOperand::registerOperand(RISCV_REG_X0 + 2, type);
        const SemanticOperand x3 = SemanticOperand::registerOperand(RISCV_REG_X0 + 3, type);
        const SemanticOperand x4 = SemanticOperand::registerOperand(RISCV_REG_X0 + 4, type);
        const SemanticOperand x5 = SemanticOperand::registerOperand(RISCV_REG_X0 + 5, type);

        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        // A chain of dead values: 'b' is never read, so neither is 'a' once
        // 'b' is gone
        const SemanticOperand a = block.newTemporary(type);
        block.append(SemanticInstruction(SemanticOperation::OP_ADD, type, a, x1, x2));
        const SemanticOperand b = block.newTemporary(type);
        block.append(SemanticInstruction(SemanticOperation::OP_ADD, type, b, a, x2));
        // A dead load: it may fault, so it stays
        const SemanticOperand loaded = block.newTemporary(type);
        block.append(SemanticInstruction(SemanticOperation::OP_LOAD, type, loaded, x1));
        // A live value, written to a register
        const SemanticOperand live = block.newTemporary(type);
        block.append(SemanticInstruction(SemanticOperation::OP_ADD, type, live, x3, x4));
        block.append(SemanticInstruction(SemanticOperation::OP_MOVE, type, x5, live));

        legalizer.legalize(block);

        const cList<SemanticInstruction>& instructions = block.getInstructions();
        cList<SemanticInstruction>::iterator i = instructions.begin();
        TESTS_ASSERT((*i).getOperation() == SemanticOperation::OP_NOP);
        i++;
        TESTS_ASSERT((*i).getOperation() == SemanticOperation::OP_NOP);
        i++;
        TESTS_ASSERT((*i).getOperation() == SemanticOperation::OP_LOAD);
        i++;
        TESTS_ASSERT((*i).getOperation() == SemanticOperation::OP_ADD);
        i++;
        TESTS_ASSERT((*i).getOperation() == SemanticOperation::OP_MOVE);
        i++;
        TESTS_ASSERT(i == instructions.end());
    }

    // ------------------------------------------------------------------
    // Part 1c: the operations the target can't do are refused
    // ------------------------------------------------------------------
    void testRejections()
    {
        ArchitectureDescriptionPtr guest = getGuest();
        SemanticLegalizer legalizer(guest, getTarget());
        const SemanticType type = SemanticType::integer(64);
        const SemanticType wide = SemanticType::integer(128);
        const SemanticOperand x1 = SemanticOperand::registerOperand(RISCV_REG_X0 + 1, type);
        const SemanticOperand x2 = SemanticOperand::registerOperand(RISCV_REG_X0 + 2, type);

        // An undefined guest instruction has no semantics to translate
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            block.append(SemanticInstruction(SemanticOperation::OP_UNDEFINED, type,
                                             SemanticOperand()));
            TESTS_EXCEPTION(legalizer.legalize(block));
        }
        // A 128 bit multiplication is not a native AMD64 operation
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticOperand t = block.newTemporary(wide);
            const SemanticOperand w1 = SemanticOperand::registerOperand(RISCV_REG_X0 + 1, wide);
            const SemanticOperand w2 = SemanticOperand::registerOperand(RISCV_REG_X0 + 2, wide);
            block.append(SemanticInstruction(SemanticOperation::OP_MULTIPLY, wide, t, w1, w2));
            TESTS_EXCEPTION(legalizer.legalize(block));
        }
        // A block of another processor is not the guest's
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_AMD_64);
            block.append(SemanticInstruction(SemanticOperation::OP_MOVE, type, x1, x2));
            TESTS_EXCEPTION(legalizer.legalize(block));
        }
    }

    // The failures of the rows
    uint m_failures;
};

// Instance test object
TestObjectTestSemanticLowering g_globalTestSemanticLowering;
