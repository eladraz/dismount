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
 *   Part 2  The AMD64 lowering (SemanticLoweringFactory / AMD64Lowering):
 *           2a  the RV64I rows lower and assemble, with the label
 *               dependencies the translator resolves (B_<guest address>,
 *               __reforge_exit);
 *           2b  the exact bytes of two small blocks;
 *           2c  an operation which isn't lowered yet is refused naming the
 *               operation and the guest address;
 *           2d  (x86-64 Linux hosts) the lowered code of the RV64I rows and
 *               of synthetic IR is run natively and compared with the
 *               SemanticInterpreter on the same random guest state.
 *
 *   Part 3  The differential test (WP9). For each of the 230 RV64I rows:
 *           lift, legalize, AMD64Lowering::lowerBlock, link the label
 *           dependencies to stubs, decode the bytes back with the AMD64
 *           disassembler, lift them with the AMD64 lifter, run them in the
 *           SemanticInterpreter (state at r15, the guest memory at the guest
 *           address + r14, state and memory preset from the row) and compare
 *           the guest registers, the memory and the next address / exit with
 *           the expectation of the row.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include <string.h>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#define LOWERING_TEST_NATIVE 1
#endif
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "xStl/exceptions.h"
#include "xStl/stream/ioStream.h"
#include "xStl/data/datastream.h"
#include "xStl/data/hash.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/semantics/SemanticLowering.h"
#include "dismount/semantics/SemanticInterpreter.h"
#include "dismount/semantics/SemanticLifter.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
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


// ----------------------------------------------------------------------
// Part 2 helpers
// ----------------------------------------------------------------------

// The guest state of the tests: the layout of reforge/GuestState.h. The
// registers x0-x31 at 0, the program counter slot at 0x100, the exit reason
// at 0x110, the temporaries at 0x200 and the other registers after 0x1000
class TestRegisterMap : public SemanticRegisterMap {
public:
    virtual bool getTargetRegister(uint, uint&) const { return false; }
    virtual uint getStateOffset(uint guestRegister) const
    {
        if ((guestRegister >= RISCV_REG_X0) && (guestRegister <= RISCV_REG_X31))
            return (guestRegister - RISCV_REG_X0) * 8;
        return 0x1000 + guestRegister * 8;
    }
    virtual uint getStateRegister() const { return ia32dis::AMD64_REG_R15; }
    virtual uint getTemporaryOffset(uint temporary) const { return 0x200 + temporary * 8; }
    virtual uint getMemoryBaseRegister() const { return ia32dis::AMD64_REG_R14; }
    virtual uint getProgramCounterOffset() const { return 0x100; }
    virtual uint getExitReasonOffset() const { return 0x110; }
};

// The stack of the first pass binary, which the lowering doesn't use
class TestLoweringStack : public StackInterface {
public:
    TestLoweringStack(FirstPassBinaryPtr& binary) :
        StackInterface(*binary, true, 4) {}
    RegisterAllocationTable gNull;
    virtual uint getStackType() { return 4; }
    virtual uint getRegisterSize() { return 4; }
    virtual RegisterAllocationTable& getRegistersTable() { return gNull; }
    virtual const RegisterAllocationTable& getRegistersTable() const { return gNull; }
    virtual StackLocation replaceRegisterToStackVariable(StackLocation source) { return source; }
};

// Lower a block into a new first pass binary
static FirstPassBinaryPtr lowerBlock(const SemanticBlock& block)
{
    FirstPassBinaryPtr binary(new FirstPassBinary(
                            OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
    binary->createNewBlockWithoutChange(0,
            StackInterfacePtr(new TestLoweringStack(binary)));
    binary->changeBasicBlock(0);
    AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                            binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);
    SemanticLoweringPtr lowering = SemanticLoweringFactory::getLowering(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);
    TestRegisterMap registerMap;
    lowering->lower(block, registerMap, *assembler);
    return binary;
}

// Return the number of dependencies of a lowered block
static uint getDependenciesCount(const FirstPassBinaryPtr& binary)
{
    return (uint)binary->getCurrentDependecies().getList().length();
}

// Return the name of the dependency 'index' of a lowered block
static cString getDependencyName(const FirstPassBinaryPtr& binary, uint index)
{
    const BinaryDependencies::DependencyObjectList& list =
                                    binary->getCurrentDependecies().getList();
    BinaryDependencies::DependencyObjectList::iterator i = list.begin();
    for (; index > 0; index--)
        i++;
    return (*i).m_name;
}

// The first instruction of the block which is a branch of any kind, or NULL
static const SemanticInstruction* findBranch(const SemanticBlock& block)
{
    const cList<SemanticInstruction>& instructions = block.getInstructions();
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        const SemanticOperation::Operation operation = (*i).getOperation();
        if (SemanticOperation::isControlFlow(operation) &&
            (operation != SemanticOperation::OP_LABEL))
        {
            return &(*i);
        }
    }
    return NULL;
}

// The guest address of the rows of Part 2
static const uint64 ROW_ADDRESS = 0x80001000ULL;


#ifdef LOWERING_TEST_NATIVE
// ----------------------------------------------------------------------
// Part 2d: run the lowered code on the host
// ----------------------------------------------------------------------

// The guest memory of the native tests: 64 KiB at the guest address
// NATIVE_GUEST_BASE, for the interpreter and (through the bias in r14) for
// the lowered code
static const uint64 NATIVE_GUEST_BASE = 0x10000000ULL;
enum { NATIVE_MEMORY_SIZE = 0x10000 };

class NativeTestMemory : public SemanticMemory {
public:
    NativeTestMemory() { memset(m_bytes, 0, sizeof(m_bytes)); }
    virtual bool read(uint64 address, uint8* buffer, uint size)
    {
        if ((address < NATIVE_GUEST_BASE) ||
            (address + size > NATIVE_GUEST_BASE + NATIVE_MEMORY_SIZE))
            return false;
        memcpy(buffer, &m_bytes[address - NATIVE_GUEST_BASE], size);
        return true;
    }
    virtual bool write(uint64 address, const uint8* buffer, uint size)
    {
        if ((address < NATIVE_GUEST_BASE) ||
            (address + size > NATIVE_GUEST_BASE + NATIVE_MEMORY_SIZE))
            return false;
        memcpy(&m_bytes[address - NATIVE_GUEST_BASE], buffer, size);
        return true;
    }
    uint8 m_bytes[NATIVE_MEMORY_SIZE];
};

// The pseudo random numbers of the native tests (xorshift64, fixed seed)
static uint64 gNativeRandom = 88172645463325252ULL;
static uint64 nativeRandom()
{
    gNativeRandom ^= gNativeRandom << 13;
    gNativeRandom ^= gNativeRandom >> 7;
    gNativeRandom ^= gNativeRandom << 17;
    return gNativeRandom;
}

// A register value: the edge values most of the time. A pointer into the
// guest memory when the block accesses it
static uint64 nativeValue(bool isPointer)
{
    static const uint64 edges[] = {
        0, 1, 2, 3, 31, 32, 63, 64, 0xFF, 0x7F, 0x80, 0xFFFF, 0x8000,
        0x7FFFFFFFULL, 0x80000000ULL, 0xFFFFFFFFULL, 0x100000000ULL,
        0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, ~(uint64)0,
        ~(uint64)1, 0xFFFFFFFFFFFFF800ULL, 0x12345678ABCDEF01ULL };
    const uint64 r = nativeRandom();
    if (isPointer)
        return NATIVE_GUEST_BASE + 0x4000 + ((r % 0x2000) & ~(uint64)7);
    switch (r % 4)
    {
    case 0:  return edges[(r >> 8) % (sizeof(edges) / sizeof(edges[0]))];
    case 1:  return edges[(r >> 8) % (sizeof(edges) / sizeof(edges[0]))] +
                    (r >> 40) % 5 - 2;
    default: return nativeRandom();
    }
}

// An immediate operand of a width
static int64 nativeImmediate()
{
    static const int64 edges[] = {
        0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 100, 127, 128, 255,
        256, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFFLL, 0x80000000LL, 0xFFFFFFFFLL,
        0x100000000LL, -1, -2, -128, -129, -32768, -32769,
        (int64)0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFLL,
        0x123456789ABCDEFLL, -0x7FFFFFFFLL, -0x80000000LL, -0x80000001LL };
    return edges[nativeRandom() % (sizeof(edges) / sizeof(edges[0]))];
}

typedef void (*NativeFunction)(void* state, void* memoryBias);
#endif

class TestObjectTestSemanticLowering : public cTestObject {
public:
    TestObjectTestSemanticLowering() : m_failures(0), m_differentialChecks(0)
#ifdef LOWERING_TEST_NATIVE
        , m_nativeChecks(0), m_nativeSkipped(0)
#endif
    {}

    virtual void test()
    {
        // Part 1: the legalizer
        testRv64iRows();
        testDeadValues();
        testRejections();

        // Part 2: the AMD64 lowering
        testLoweringFactory();
        testRv64iLowering();
        testLoweredBytes();
        testNotLowered();
        testDifferentialRows();
        testLowerBlockApi();
        testEmptyBlock();
        testTrailingGuestLabels();
#ifdef LOWERING_TEST_NATIVE
        testNativeRows();
        testNativeEdgeEncodings();
        testNativeSynthetic();
#endif

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


    // ------------------------------------------------------------------
    // Part 2: the lowering
    // ------------------------------------------------------------------
    void testLoweringFactory()
    {
        SemanticLoweringPtr lowering = SemanticLoweringFactory::getLowering(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);
        TESTS_ASSERT(lowering.getPointer() != NULL);
        TESTS_ASSERT(lowering->getTarget()->getType() ==
                     OpcodeSubsystems::DISASSEMBLER_AMD_64);
        // The other processors have no lowering
        TESTS_EXCEPTION(SemanticLoweringFactory::getLowering(
                                    OpcodeSubsystems::DISASSEMBLER_ARM_64_LE));
        TESTS_ASSERT(AMD64Lowering::getBlockLabel(0x140001000ULL) ==
                     cString("B_140001000"));
    }

    // 2a: every RV64I row lowers and assembles; the dependencies are the
    // labels of the guest targets and of the exit
    void testRv64iLowering()
    {
        ArchitectureDescriptionPtr guest = getGuest();
        RiscvSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_RISCV_64, guest);
        SemanticLegalizer legalizer(guest, getTarget());
        const cString fallThrough = AMD64Lowering::getBlockLabel(ROW_ADDRESS + 4);
        const cString exitLabel = AMD64Lowering::getExitLabel();

        uint rows = 0;
        uint failures = 0;
        for (const SemanticLoweringRow* row = gRv64SemanticRows; row->m_text != NULL; row++)
        {
            if (!isRv64iRow(*row))
                continue;
            rows++;
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            if (!liftRow(*row, lifter, block))
            {
                LOWERING_ROW_FAIL(row->m_text, "the instruction isn't lifted");
                failures++;
                continue;
            }
            legalizer.legalize(block);

            FirstPassBinaryPtr binary;
            try
            {
                binary = lowerBlock(block);
            } catch (cException&)
            {
                LOWERING_ROW_FAIL(row->m_text, "the lowering throws");
                failures++;
                continue;
            }

            // The expected dependencies, from the branch of the IR
            const SemanticInstruction* branch = findBranch(block);
            cList<cString> expected;
            if (branch == NULL)
            {
                expected.append(fallThrough);
            } else if (branch->getOperation() == SemanticOperation::OP_TRAP)
            {
                expected.append(exitLabel);
            } else if (branch->getOperation() ==
                       SemanticOperation::OP_BRANCH_CONDITIONAL)
            {
                // jne target; jmp fall through
                expected.append(AMD64Lowering::getBlockLabel(
                                    (uint64)branch->getSource(1).getValue()));
                expected.append(fallThrough);
            } else if (branch->getSource(0).getKind() ==
                       SemanticOperand::KIND_GUEST_ADDRESS)
            {
                expected.append(AMD64Lowering::getBlockLabel(
                                    (uint64)branch->getSource(0).getValue()));
            } else
            {
                expected.append(exitLabel);
            }

            bool isSame = (getDependenciesCount(binary) == expected.length());
            uint index = 0;
            for (cList<cString>::iterator e = expected.begin();
                 isSame && (e != expected.end()); e++, index++)
            {
                isSame = (getDependencyName(binary, index) == *e);
            }
            if (!isSame)
            {
                LOWERING_ROW_FAIL(row->m_text, "the label dependencies are wrong");
                failures++;
            }
        }
        cout << rows << " RV64I rows lowered, " << failures << " failures" << endl;
        TESTS_ASSERT(rows == RV64I_ROWS_COUNT);
        TESTS_ASSERT(failures == 0);
    }

    // 2b: the exact bytes of small blocks
    void testLoweredBytes()
    {
        const SemanticType type = SemanticType::integer(64);
        const SemanticOperand x1 = SemanticOperand::registerOperand(RISCV_REG_X0 + 1, type);
        const SemanticOperand x2 = SemanticOperand::registerOperand(RISCV_REG_X0 + 2, type);
        const SemanticOperand x5 = SemanticOperand::registerOperand(RISCV_REG_X0 + 5, type);
        const SemanticOperand x6 = SemanticOperand::registerOperand(RISCV_REG_X0 + 6, type);

        // x5 = 1; falls through to 0x80001004:
        //   mov qword [r15+0x28], 1 ; jmp B_80001004
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            block.beginGuestInstruction(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, ROW_ADDRESS), 4);
            block.append(SemanticInstruction(SemanticOperation::OP_MOVE, type, x5,
                         SemanticOperand::immediate(1, type)));
            block.setFallThroughAddress(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, ROW_ADDRESS + 4));
            FirstPassBinaryPtr binary = lowerBlock(block);
            static const uint8 expected[] = {
                0x49, 0xC7, 0x47, 0x28, 0x01, 0x00, 0x00, 0x00,
                0xE9, 0x00, 0x00, 0x00, 0x00 };
            checkBytes(binary, expected, sizeof(expected));
            TESTS_ASSERT_EQUAL(getDependenciesCount(binary), 1);
            TESTS_ASSERT(getDependencyName(binary, 0) == cString("B_80001004"));
            const BinaryDependencies::DependencyObject& dependency =
                        *binary->getCurrentDependecies().getList().begin();
            TESTS_ASSERT_EQUAL(dependency.m_position, 9);
            TESTS_ASSERT_EQUAL(dependency.m_length, (uint)BinaryDependencies::DEP_32BIT);
            TESTS_ASSERT_EQUAL((int)dependency.m_type, (int)BinaryDependencies::DEP_RELATIVE);
        }
        // x6 = x1 + x2; then a jump to 0x80002000:
        //   mov rax, [r15+8] ; mov [r15+0x200], rax ... (through t0)
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            block.beginGuestInstruction(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, ROW_ADDRESS), 4);
            block.append(SemanticInstruction(SemanticOperation::OP_ADD, type, x6, x1, x2));
            block.append(SemanticInstruction(SemanticOperation::OP_BRANCH,
                         SemanticType(), SemanticOperand(),
                         SemanticOperand::guestAddress(0x80002000ULL, type)));
            FirstPassBinaryPtr binary = lowerBlock(block);
            //   mov rax, [r15+8]; add rax, [r15+0x10]; mov [r15+0x30], rax
            //   jmp B_80002000 (the branch ends the block: no fall through)
            static const uint8 expected[] = {
                0x49, 0x8B, 0x47, 0x08,
                0x49, 0x03, 0x47, 0x10,
                0x49, 0x89, 0x47, 0x30,
                0xE9, 0x00, 0x00, 0x00, 0x00 };
            checkBytes(binary, expected, sizeof(expected));
            TESTS_ASSERT_EQUAL(getDependenciesCount(binary), 1);
            TESTS_ASSERT(getDependencyName(binary, 0) == cString("B_80002000"));
        }
    }

    // Compare the bytes of a lowered block
    void checkBytes(const FirstPassBinaryPtr& binary, const uint8* expected, uint size)
    {
        const cBuffer& code = binary->getCurrentBlockData();
        bool isSame = (code.getSize() == size);
        for (uint i = 0; isSame && (i < size); i++)
            isSame = (code[i] == expected[i]);
        if (!isSame)
        {
            cout << "lowered bytes:";
            for (uint i = 0; i < code.getSize(); i++)
                cout << ' ' << HEXBYTE(code[i]);
            cout << endl;
        }
        TESTS_ASSERT(isSame);
    }

    // 2c: an operation which isn't lowered is refused with its name and the
    // guest address
    void testNotLowered()
    {
        const SemanticType type = SemanticType::integer(64);
        const SemanticOperand x1 = SemanticOperand::registerOperand(RISCV_REG_X0 + 1, type);
        const SemanticOperand x2 = SemanticOperand::registerOperand(RISCV_REG_X0 + 2, type);
        const SemanticOperand x3 = SemanticOperand::registerOperand(RISCV_REG_X0 + 3, type);

        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        block.beginGuestInstruction(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, 0x1234), 4);
        block.append(SemanticInstruction(SemanticOperation::OP_MOVE, type, x3, x1));
        block.beginGuestInstruction(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, 0x1238), 4);
        block.append(SemanticInstruction(SemanticOperation::OP_CARRYLESS_MULTIPLY, type,
                                         x3, x1, x2));
        bool isRefused = false;
        try
        {
            lowerBlock(block);
        } catch (cException& e)
        {
            isRefused = true;
            cSArray<char> message = cString(e.getMessage()).getASCIIstring();
            cout << "refused: " << message.getBuffer() << endl;
            TESTS_ASSERT(strstr(message.getBuffer(), "carryless_multiply") != NULL);
            TESTS_ASSERT(strstr(message.getBuffer(), "0x1238") != NULL);
        }
        TESTS_ASSERT(isRefused);

        // A label of the block is not lowered yet: refused, not miscompiled
        SemanticBlock labels(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        const SemanticOperand label = labels.newLabel();
        labels.append(SemanticInstruction(SemanticOperation::OP_BRANCH,
                      SemanticType(), SemanticOperand(), label));
        labels.append(SemanticInstruction(SemanticOperation::OP_LABEL,
                      SemanticType(), SemanticOperand(), label));
        TESTS_EXCEPTION(lowerBlock(labels));
    }


    // 3c: a block without IR still jumps to its fall-through address (R-L1)
    void testEmptyBlock()
    {
        static const uint8 jump[] = { 0xE9, 0x00, 0x00, 0x00, 0x00 };
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, 0x1004));
        FirstPassBinaryPtr binary = lowerBlock(block);
        checkBytes(binary, jump, sizeof(jump));
        TESTS_ASSERT_EQUAL(getDependenciesCount(binary), 1);
        TESTS_ASSERT(getDependencyName(binary, 0) == cString("B_1004"));
    }

    // 3d: every guest instruction of a block gets its label, also those
    // which produced no IR at the end of the block (R-L2)
    void testTrailingGuestLabels()
    {
        const SemanticType type = SemanticType::integer(64);
        const SemanticOperand x5 = SemanticOperand::registerOperand(RISCV_REG_X0 + 5, type);
        static const uint8 jump[] = { 0xE9, 0x00, 0x00, 0x00, 0x00 };

        AMD64Lowering lowering;
        TestRegisterMap registerMap;

        // One guest instruction without IR: its label is at the jump
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            block.beginGuestInstruction(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, 0x8), 4);
            AMD64Lowering::LoweredBlock lowered;
            lowering.lowerBlock(block, registerMap, lowered);
            TESTS_ASSERT_EQUAL(lowered.m_code.getSize(), sizeof(jump));
            TESTS_ASSERT_EQUAL(lowered.m_labels.length(), 1);
            TESTS_ASSERT((*lowered.m_labels.begin()).m_name == cString("B_8"));
            TESTS_ASSERT_EQUAL((*lowered.m_labels.begin()).m_offset, 0);
            TESTS_ASSERT_EQUAL(lowered.m_dependencies.length(), 1);
            TESTS_ASSERT((*lowered.m_dependencies.begin()).m_name == cString("B_c"));
        }

        // Two trailing guest instructions without IR: their labels are
        // before the fall-through jump
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            block.beginGuestInstruction(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, 0x1000), 4);
            block.append(SemanticInstruction(SemanticOperation::OP_MOVE, type, x5,
                         SemanticOperand::immediate(1, type)));
            block.beginGuestInstruction(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, 0x1004), 4);
            block.beginGuestInstruction(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, 0x1008), 4);
            AMD64Lowering::LoweredBlock lowered;
            lowering.lowerBlock(block, registerMap, lowered);
            const uint jumpOffset = lowered.m_code.getSize() - sizeof(jump);
            TESTS_ASSERT(jumpOffset > 0);
            TESTS_ASSERT_EQUAL(lowered.m_labels.length(), 3);
            cList<AMD64Lowering::LabelDefinition>::iterator label = lowered.m_labels.begin();
            TESTS_ASSERT((*label).m_name == cString("B_1000"));
            TESTS_ASSERT_EQUAL((*label).m_offset, 0);
            label++;
            TESTS_ASSERT((*label).m_name == cString("B_1004"));
            TESTS_ASSERT_EQUAL((*label).m_offset, jumpOffset);
            label++;
            TESTS_ASSERT((*label).m_name == cString("B_1008"));
            TESTS_ASSERT_EQUAL((*label).m_offset, jumpOffset);
            TESTS_ASSERT_EQUAL(lowered.m_dependencies.length(), 1);
            TESTS_ASSERT((*lowered.m_dependencies.begin()).m_name == cString("B_100c"));
            TESTS_ASSERT_EQUAL((*lowered.m_dependencies.begin()).m_offset,
                               lowered.m_code.getSize() - 4);
        }
    }

    // ------------------------------------------------------------------
    // Part 3: the differential test of the rows (WP9)
    // ------------------------------------------------------------------

    // The tokens "name=value" of a setup or expected string
    struct Token {
        char m_name[16];
        const char* m_value;
        const char* m_end;
    };
    enum { MAX_TOKENS = 96 };

    static uint split(const char* text, Token* tokens, uint maximum)
    {
        uint count = 0;
        const char* p = text;
        while ((*p != 0) && (count < maximum))
        {
            while (*p == ' ')
                p++;
            if (*p == 0)
                break;
            const char* end = p;
            while ((*end != 0) && (*end != ' '))
                end++;
            const char* equals = (const char*)memchr(p, '=', (size_t)(end - p));
            TESTS_ASSERT(equals != NULL);
            const uint length = (uint)(equals - p);
            TESTS_ASSERT(length < sizeof(tokens[count].m_name));
            memcpy(tokens[count].m_name, p, length);
            tokens[count].m_name[length] = 0;
            tokens[count].m_value = equals + 1;
            tokens[count].m_end = end;
            count++;
            p = end;
        }
        return count;
    }

    static uint64 parseHex(const char* text, const char* end)
    {
        uint64 value = 0;
        for (const char* p = text; p < end; p++)
            value = (value << 4) | digit(*p);
        return value;
    }

    // The number after 'prefix' in a name ("x11" -> 11), or -1
    static int numbered(const char* name, char prefix)
    {
        if ((name[0] != prefix) || (name[1] == 0))
            return -1;
        int value = 0;
        for (const char* p = name + 1; *p != 0; p++)
        {
            if ((*p < '0') || (*p > '9'))
                return -1;
            value = value * 10 + (*p - '0');
        }
        return (value < 32) ? value : -1;
    }

    // The guest memory of the rows (the same model as TestRiscvSemantics):
    // the data window [0x80100000, + 0x4000) holds the pattern
    // ((uint32)(address * 2654435761) >> 24) unless the row sets bytes, the
    // code region is the 3 pages around the page of the pc, zeros but the
    // code, with the shadow stack page 0x80200000 when cfi is on. Other
    // addresses fault
    class RowMemory {
    public:
        RowMemory(const cBuffer& code, uint64 pc, bool isShadowStack) :
            m_code(code), m_pc(pc), m_isShadowStack(isShadowStack) {}

        bool read(uint64 address, uint8* buffer, uint size)
        {
            for (uint i = 0; i < size; i++)
                if (!isMapped(address + i))
                    return false;
            for (uint i = 0; i < size; i++)
                buffer[i] = getByte(address + i);
            return true;
        }

        bool write(uint64 address, const uint8* buffer, uint size)
        {
            for (uint i = 0; i < size; i++)
                if (!isMapped(address + i))
                    return false;
            for (uint i = 0; i < size; i++)
            {
                if (m_written.hasKey(address + i))
                    m_written[address + i] = buffer[i];
                else
                    m_written.append(address + i, buffer[i]);
            }
            return true;
        }

        void setInitial(uint64 address, uint8 value)
        {
            if (m_initial.hasKey(address))
                m_initial[address] = value;
            else
                m_initial.append(address, value);
        }

        uint8 getByte(uint64 address) const
        {
            if (m_written.hasKey(address))
                return m_written[address];
            return getInitial(address);
        }

        uint8 getInitial(uint64 address) const
        {
            if ((address - m_pc) < m_code.getSize())
                return m_code[(uint)(address - m_pc)];
            if (m_initial.hasKey(address))
                return m_initial[address];
            if (isWindow(address))
                return (uint8)((uint32)(address * 2654435761u) >> 24);
            return 0;
        }

        cList<uint64> getWritten() const { return m_written.keys(); }

        bool isWindow(uint64 address) const
        {
            return ((address - 0x80100000ULL) < 0x4000) ||
                   (m_isShadowStack && ((address - 0x80200000ULL) < 0x1000));
        }

    private:
        bool isMapped(uint64 address) const
        {
            const uint64 regionBase = (m_pc & ~(uint64)0xFFF) - 0x1000;
            return isWindow(address) || ((address - regionBase) < 0x3000);
        }

        cBuffer m_code;
        uint64 m_pc;
        bool m_isShadowStack;
        cHash<uint64, uint8> m_initial;
        cHash<uint64, uint8> m_written;
    };

    // The memory of the lowered code: the guest state at STATE_BASE (r15) and
    // the guest memory at the guest address + the bias (r14)
    enum { STATE_SIZE = 0x10000 };
    static const uint64 STATE_BASE = 0x70000000ULL;
    static const uint64 MEMORY_BIAS = 0x300000000000ULL;
    static const uint64 CODE_BASE = 0x60000000ULL;

    class LoweredMemory : public SemanticMemory {
    public:
        LoweredMemory(RowMemory& guest) : m_guest(guest), m_state(STATE_SIZE)
        {
            memset(m_state.getBuffer(), 0, STATE_SIZE);
        }

        virtual bool read(uint64 address, uint8* buffer, uint size)
        {
            if ((address - STATE_BASE) < STATE_SIZE - size + 1)
            {
                memcpy(buffer, &m_state[(uint)(address - STATE_BASE)], size);
                return true;
            }
            return m_guest.read(address - MEMORY_BIAS, buffer, size);
        }

        virtual bool write(uint64 address, const uint8* buffer, uint size)
        {
            if ((address - STATE_BASE) < STATE_SIZE - size + 1)
            {
                memcpy(&m_state[(uint)(address - STATE_BASE)], buffer, size);
                return true;
            }
            return m_guest.write(address - MEMORY_BIAS, buffer, size);
        }

        uint64 getState(uint offset) const
        {
            uint64 value = 0;
            memcpy(&value, &m_state[offset], 8);
            return value;
        }

        void setState(uint offset, uint64 value)
        {
            memcpy(&m_state[offset], &value, 8);
        }

    private:
        RowMemory& m_guest;
        cBuffer m_state;
    };

    // The stubs the labels of a lowered block are linked to
    struct Stub {
        cString m_name;
        uint64 m_address;
    };

    // The result of running lowered code
    struct LoweredRun {
        LoweredRun() : m_stub(-1), m_fault(false), m_error(NULL) {}
        // The stub the code jumped to, as an index of the stubs, or -1
        int m_stub;
        // The code accessed memory that is not mapped
        bool m_fault;
        // A failure of the harness (the bytes don't decode, lift or run)
        const char* m_error;
    };

    // Link the lowered block with stubs: after the code, a 16 byte slot of
    // int3 for each label the code refers to and one for __reforge_exit. The
    // stub label B_<va> stands for "the guest block at <va>" and the harness
    // stops when the code jumps to a slot
    static bool linkWithStubs(AMD64Lowering::LoweredBlock& lowered,
                              cBuffer& image, cList<Stub>& stubs)
    {
        cList<cString> names;
        for (cList<AMD64Lowering::LabelDependency>::iterator d =
                lowered.m_dependencies.begin();
             d != lowered.m_dependencies.end(); d++)
        {
            bool isKnown = false;
            for (cList<cString>::iterator n = names.begin(); n != names.end(); n++)
                isKnown = isKnown || (*n == (*d).m_name);
            if (!isKnown)
                names.append((*d).m_name);
        }
        const uint codeSize = lowered.m_code.getSize();
        const uint slots = (uint)names.length();
        const uint stubBase = (codeSize + 15) & ~15u;
        image = cBuffer(stubBase + 16 * (slots + 1));
        for (uint i = 0; i < image.getSize(); i++)
            image[i] = (i < codeSize) ? lowered.m_code[i] : 0xCC;
        uint slot = 0;
        for (cList<cString>::iterator n = names.begin(); n != names.end(); n++, slot++)
        {
            Stub stub;
            stub.m_name = *n;
            stub.m_address = CODE_BASE + stubBase + 16 * slot;
            stubs.append(stub);
        }
        for (cList<AMD64Lowering::LabelDependency>::iterator d =
                lowered.m_dependencies.begin();
             d != lowered.m_dependencies.end(); d++)
        {
            if (((*d).m_size != 4) ||
                ((*d).m_kind != BinaryDependencies::DEP_RELATIVE))
                return false;
            for (cList<Stub>::iterator st = stubs.begin(); st != stubs.end(); st++)
            {
                if ((*st).m_name == (*d).m_name)
                {
                    AMD64Lowering::patchRelative32(image, (*d).m_offset,
                        (int64)((*st).m_address - CODE_BASE) -
                        (int64)((*d).m_offset + 4));
                }
            }
        }
        return true;
    }

    // Decode, lift and run the bytes of the image from CODE_BASE until the
    // code reaches a stub (or faults, or the harness fails)
    static LoweredRun runLowered(const cBuffer& image, uint codeSize,
                                 const cList<Stub>& stubs,
                                 SemanticInterpreter& interpreter)
    {
        LoweredRun run;
        SemanticLifterPtr lifter = SemanticLifterFactory::getLifter(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);
        uint64 pc = CODE_BASE;
        for (uint step = 0; step < 4096; step++)
        {
            int index = 0;
            for (cList<Stub>::iterator st = stubs.begin(); st != stubs.end();
                 st++, index++)
            {
                if ((*st).m_address == pc)
                {
                    run.m_stub = index;
                    return run;
                }
            }
            if ((pc < CODE_BASE) || (pc >= CODE_BASE + codeSize))
            {
                run.m_error = "the code runs out of its bytes";
                return run;
            }
            const uint offset = (uint)(pc - CODE_BASE);
            const uint available = (codeSize - offset < 16) ? codeSize - offset : 16;
            cBuffer bytes(available);
            for (uint i = 0; i < available; i++)
                bytes[i] = image[offset + i];
            cMemoryStream stream(bytes);
            StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
                OpcodeSubsystems::DISASSEMBLER_AMD_64,
                BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
                ProcessorAddress(ProcessorAddress::PROCESSOR_64, pc));
            OpcodePtr opcode;
            XSTL_TRY
            {
                opcode = disassembler->next();
            }
            XSTL_CATCH(cException&)
            {
                run.m_error = "the bytes don't decode";
                return run;
            }
            if (opcode.getPointer() == NULL)
            {
                run.m_error = "the bytes don't decode";
                return run;
            }
            const uint size = opcode->getOpcodeSize();
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_AMD_64);
            if (!lifter->lift(opcode, block))
            {
                run.m_error = "the instruction isn't lifted";
                return run;
            }
            block.setFallThroughAddress(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, pc + size));
            const SemanticInterpreter::ExitReason reason = interpreter.run(block);
            if (reason == SemanticInterpreter::EXIT_FALL_THROUGH)
            {
                pc = pc + size;
            } else if (reason == SemanticInterpreter::EXIT_BRANCH)
            {
                pc = interpreter.getNextAddress();
            } else if (reason == SemanticInterpreter::EXIT_MEMORY_FAULT)
            {
                run.m_fault = true;
                return run;
            } else
            {
                run.m_error = "the interpreter stops on the instruction";
                return run;
            }
        }
        run.m_error = "the code doesn't end";
        return run;
    }

    // The stub 'index'
    static Stub getStub(const cList<Stub>& stubs, int index)
    {
        cList<Stub>::iterator i = stubs.begin();
        for (; index > 0; index--)
            i++;
        return *i;
    }

    // The guest address of a stub named B_<hex>
    static uint64 getStubGuestAddress(const cString& name)
    {
        cSArray<char> ascii = name.getASCIIstring();
        const char* text = ascii.getBuffer();
        return parseHex(text + 2, text + strlen(text));
    }

    // Run a row through the whole chain and compare it with its expectation
    void runDifferentialRow(const SemanticLoweringRow& row)
    {
        const uint64 pc = ROW_ADDRESS;
        const char* name = row.m_text;
        cBuffer code(4);
        for (uint i = 0; i < 4; i++)
            code[i] = (uint8)((digit(row.m_code[2 * i]) << 4) |
                              digit(row.m_code[2 * i + 1]));

        // The start state
        Token tokens[MAX_TOKENS];
        uint64 startX[32];
        memset(startX, 0, sizeof(startX));
        uint64 startFcsr = 0, startSsp = 0, startElp = 0;
        bool isCfi = false;
        const uint setupCount = split(row.m_setup, tokens, MAX_TOKENS);
        for (uint i = 0; i < setupCount; i++)
        {
            const Token& t = tokens[i];
            if (strcmp(t.m_name, "fcsr") == 0)
                startFcsr = parseHex(t.m_value, t.m_end);
            else if (strcmp(t.m_name, "ssp") == 0)
                startSsp = parseHex(t.m_value, t.m_end);
            else if (strcmp(t.m_name, "elp") == 0)
                startElp = parseHex(t.m_value, t.m_end) & 1;
            else if (strcmp(t.m_name, "cfi") == 0)
                isCfi = true;
            else if (strcmp(t.m_name, "mem") == 0)
                continue;
            else if (numbered(t.m_name, 'x') >= 0)
                startX[numbered(t.m_name, 'x')] = parseHex(t.m_value, t.m_end);
            else
                TESTS_ASSERT(false);
        }

        // Lift, legalize, lower
        ArchitectureDescriptionPtr guest(new RiscvArchitecture(
                RiscvInstructionSet::RISCV_RV64, 128, isCfi));
        RiscvSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_RISCV_64, guest);
        SemanticLegalizer legalizer(guest, getTarget());
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        {
            uint32 encoding = 0;
            for (uint i = 0; i < 4; i++)
                encoding |= (uint32)code[i] << (8 * i);
            RiscvOpcodePtr opcode = RiscvStreamDisassembler::decode(
                RiscvInstructionSet::RISCV_RV64, encoding, 4, pc,
                RiscvFeatureSet::all());
            if ((opcode.getPointer() == NULL) ||
                !lifter.lift(asOpcode(opcode), block))
            {
                LOWERING_ROW_FAIL(name, "the instruction isn't lifted");
                return;
            }
        }
        AMD64Lowering::LoweredBlock lowered;
        try
        {
            legalizer.legalize(block);
            AMD64Lowering lowering;
            TestRegisterMap registerMap;
            lowering.lowerBlock(block, registerMap, lowered);
        } catch (cException& e)
        {
            cSArray<char> message = cString(e.getMessage()).getASCIIstring();
            LOWERING_ROW_FAIL(name, message.getBuffer());
            return;
        }
        // The block defines the label of its instruction, at its start
        if ((lowered.m_labels.length() != 1) ||
            !((*lowered.m_labels.begin()).m_name == AMD64Lowering::getBlockLabel(pc)) ||
            ((*lowered.m_labels.begin()).m_offset != 0))
        {
            LOWERING_ROW_FAIL(name, "the label of the guest instruction is wrong");
            return;
        }
        cBuffer image;
        cList<Stub> stubs;
        if (!linkWithStubs(lowered, image, stubs))
        {
            LOWERING_ROW_FAIL(name, "a dependency is not a rel32");
            return;
        }

        // The memory and the registers of the lowered code
        RowMemory guestMemory(code, pc, isCfi);
        for (uint i = 0; i < setupCount; i++)
            if (strcmp(tokens[i].m_name, "mem") == 0)
                parseMemory(tokens[i].m_value, tokens[i].m_end, &guestMemory, NULL);
        LoweredMemory memory(guestMemory);
        TestRegisterMap registerMap;
        for (uint i = 1; i < 32; i++)
            memory.setState(registerMap.getStateOffset(RISCV_REG_X0 + i), startX[i]);
        memory.setState(registerMap.getStateOffset(RISCV_REG_FCSR), startFcsr);
        memory.setState(registerMap.getStateOffset(RISCV_REG_SSP), startSsp);
        memory.setState(registerMap.getStateOffset(RISCV_REG_ELP), startElp);
        // Poison the exit slots: an exit has to write them
        memory.setState(registerMap.getProgramCounterOffset(), 0xDEADBEEFDEADBEEFULL);
        memory.setState(registerMap.getExitReasonOffset(), 0);

        ArchitectureDescriptionPtr target = getTarget();
        SemanticInterpreter interpreter(target, memory);
        interpreter.setRegister(registerMap.getStateRegister(), SemanticValue(STATE_BASE));
        interpreter.setRegister(registerMap.getMemoryBaseRegister(), SemanticValue(MEMORY_BIAS));
        interpreter.setRegister(ia32dis::AMD64_REG_RIP, SemanticValue(CODE_BASE));
        const LoweredRun run = runLowered(image, lowered.m_code.getSize(), stubs,
                                          interpreter);
        if (run.m_error != NULL)
        {
            LOWERING_ROW_FAIL(name, run.m_error);
            return;
        }

        // What the row expects
        uint64 wantedX[32];
        memcpy(wantedX, startX, sizeof(wantedX));
        uint64 wantedElp = startElp;
        uint64 wantedSsp = startSsp;
        bool hasPc = false;
        uint64 expectedPc = 0;
        cString exitName = "fall";
        cHash<uint64, uint8> expectedMemory;
        const uint expectedCount = split(row.m_expected, tokens, MAX_TOKENS);
        for (uint i = 0; i < expectedCount; i++)
        {
            const Token& t = tokens[i];
            if (strcmp(t.m_name, "exit") == 0)
                exitName = cString(cString(t.m_value).left((uint)(t.m_end - t.m_value)));
            else if (strcmp(t.m_name, "mem") == 0)
                parseMemory(t.m_value, t.m_end, NULL, &expectedMemory);
            else if (strcmp(t.m_name, "pc") == 0)
            {
                hasPc = true;
                expectedPc = parseHex(t.m_value, t.m_end);
            } else if (strcmp(t.m_name, "elp") == 0)
                wantedElp = parseHex(t.m_value, t.m_end) & 1;
            else if (strcmp(t.m_name, "ssp") == 0)
                wantedSsp = parseHex(t.m_value, t.m_end);
            else if (numbered(t.m_name, 'x') >= 0)
                wantedX[numbered(t.m_name, 'x')] = parseHex(t.m_value, t.m_end);
            else
                TESTS_ASSERT(false);
        }

        // The exit of the lowered code. Leaving to a B_ stub is a jump to a
        // guest address; __reforge_exit has the address in the program
        // counter slot and the reason in the exit reason slot
        cString actualExit;
        uint64 actualPc = pc;
        if (run.m_fault)
        {
            actualExit = "trap:fault";
        } else
        {
            const cString stubName = getStub(stubs, run.m_stub).m_name;
            if (stubName == AMD64Lowering::getExitLabel())
            {
                const uint64 reason = memory.getState(registerMap.getExitReasonOffset());
                actualPc = memory.getState(registerMap.getProgramCounterOffset());
                const uint kind = (uint)(reason & 0xFF);
                if (kind == AMD64Lowering::EXIT_TRAP)
                {
                    const uint64 number = reason >>
                                    AMD64Lowering::EXIT_TRAP_NUMBER_SHIFT;
                    if (number == RISCV_TRAP_SOFTWARE_CHECK)
                        actualExit = "trap:swcheck";
                    else
                        actualExit = "trap:other";
                } else if ((kind == AMD64Lowering::EXIT_BRANCH) ||
                           (kind == AMD64Lowering::EXIT_CALL) ||
                           (kind == AMD64Lowering::EXIT_RETURN))
                {
                    actualExit = "branch";
                } else
                {
                    actualExit = "other";
                }
            } else
            {
                actualPc = getStubGuestAddress(stubName);
                actualExit = "branch";
            }
            // (a branch which lands on the next instruction is a fall through)
            if (actualExit == cString("branch") && (actualPc == pc + 4) &&
                (exitName == cString("fall")))
                actualExit = "fall";
        }
        if (!(actualExit == exitName))
        {
            char message[256];
            snprintf(message, sizeof(message), "exit %s, expected %s",
                     actualExit.getASCIIstring().getBuffer(),
                     exitName.getASCIIstring().getBuffer());
            LOWERING_ROW_FAIL(name, message);
            return;
        }
        if (hasPc && (actualPc != expectedPc))
        {
            char message[256];
            snprintf(message, sizeof(message), "next pc %llx, expected %llx",
                     (unsigned long long)actualPc, (unsigned long long)expectedPc);
            LOWERING_ROW_FAIL(name, message);
        }
        m_differentialChecks++;

        // The registers (x0 is zero)
        for (uint i = 0; i < 32; i++)
        {
            const uint64 actual = (i == 0) ? memory.getState(0) :
                memory.getState(registerMap.getStateOffset(RISCV_REG_X0 + i));
            if (actual != wantedX[i])
            {
                char message[256];
                snprintf(message, sizeof(message), "x%u = %llx, expected %llx",
                         i, (unsigned long long)actual,
                         (unsigned long long)wantedX[i]);
                LOWERING_ROW_FAIL(name, message);
            }
            m_differentialChecks++;
        }
        if (memory.getState(registerMap.getStateOffset(RISCV_REG_FCSR)) != startFcsr)
            LOWERING_ROW_FAIL(name, "fcsr changed");
        if (memory.getState(registerMap.getStateOffset(RISCV_REG_ELP)) != wantedElp)
            LOWERING_ROW_FAIL(name, "elp");
        if (isCfi && (memory.getState(registerMap.getStateOffset(RISCV_REG_SSP)) != wantedSsp))
            LOWERING_ROW_FAIL(name, "ssp");

        // The memory: the expected bytes, and nothing else changed
        cList<uint64> keys = expectedMemory.keys();
        for (cList<uint64>::iterator i = keys.begin(); i != keys.end(); i++)
        {
            if (guestMemory.getByte(*i) != expectedMemory[*i])
            {
                char message[256];
                snprintf(message, sizeof(message), "mem[%llx] = %02x, expected %02x",
                         (unsigned long long)*i, guestMemory.getByte(*i),
                         expectedMemory[*i]);
                LOWERING_ROW_FAIL(name, message);
            }
            m_differentialChecks++;
        }
        cList<uint64> written = guestMemory.getWritten();
        for (cList<uint64>::iterator i = written.begin(); i != written.end(); i++)
        {
            if (!expectedMemory.hasKey(*i) && guestMemory.isWindow(*i) &&
                (guestMemory.getByte(*i) != guestMemory.getInitial(*i)))
            {
                char message[256];
                snprintf(message, sizeof(message), "mem[%llx] was written",
                         (unsigned long long)*i);
                LOWERING_ROW_FAIL(name, message);
            }
        }
    }

    // Parse "addr:hex,addr:hex" into the initial bytes or the expected bytes
    static void parseMemory(const char* text, const char* end,
                            RowMemory* memory, cHash<uint64, uint8>* expected)
    {
        const char* p = text;
        while (p < end)
        {
            const char* colon = (const char*)memchr(p, ':', (size_t)(end - p));
            TESTS_ASSERT(colon != NULL);
            const uint64 address = parseHex(p, colon);
            const char* q = colon + 1;
            uint count = 0;
            while ((q < end) && (*q != ','))
            {
                const uint8 byte = (uint8)parseHex(q, q + 2);
                if (expected != NULL)
                {
                    if (expected->hasKey(address + count))
                        (*expected)[address + count] = byte;
                    else
                        expected->append(address + count, byte);
                } else
                {
                    memory->setInitial(address + count, byte);
                }
                q += 2;
                count++;
            }
            p = (q < end) ? q + 1 : q;
        }
    }

    // 3: the 230 RV64I rows
    void testDifferentialRows()
    {
        uint rows = 0;
        uint passed = 0;
        for (const SemanticLoweringRow* row = gRv64SemanticRows; row->m_text != NULL; row++)
        {
            if (!isRv64iRow(*row))
                continue;
            rows++;
            const uint before = m_failures;
            runDifferentialRow(*row);
            if (m_failures == before)
                passed++;
        }
        cout << "differential: " << passed << "/" << rows << " RV64I rows pass ("
             << m_differentialChecks << " comparisons)" << endl;
        TESTS_ASSERT(rows == RV64I_ROWS_COUNT);
        TESTS_ASSERT(passed == rows);
    }

    // 3b: lowerBlock() with a label of the block, and the dependencies
    void testLowerBlockApi()
    {
        const SemanticType type = SemanticType::integer(64);
        const SemanticType boolean = SemanticType::integer(1);
        const SemanticOperand x1 = SemanticOperand::registerOperand(RISCV_REG_X0 + 1, type);
        const SemanticOperand x2 = SemanticOperand::registerOperand(RISCV_REG_X0 + 2, type);

        // x2 = 0; loop: x2 = x2 + x1 ... : a counted loop with a label
        //   L: x2 = x2 + 3 ; flag = x2 <u 10 ; if flag goto L ;
        //   then branch to the guest 0x2000 (taken when the flag is clear)
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        block.beginGuestInstruction(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, 0x1000), 4);
        block.append(SemanticInstruction(SemanticOperation::OP_MOVE, type, x2,
                     SemanticOperand::immediate(0, type)));
        const SemanticOperand label = block.newLabel();
        block.append(SemanticInstruction(SemanticOperation::OP_LABEL,
                     SemanticType(), SemanticOperand(), label));
        block.append(SemanticInstruction(SemanticOperation::OP_ADD, type, x2, x2,
                     SemanticOperand::immediate(3, type)));
        block.beginGuestInstruction(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, 0x1004), 4);
        SemanticOperand flag = block.newTemporary(boolean);
        SemanticInstruction compare(SemanticOperation::OP_COMPARE, type, flag,
                                    x2, SemanticOperand::immediate(10, type));
        compare.setCondition(SemanticOperation::COND_UNSIGNED_LESS);
        block.append(compare);
        block.append(SemanticInstruction(SemanticOperation::OP_BRANCH_CONDITIONAL,
                     SemanticType(), SemanticOperand(), flag, label));
        block.append(SemanticInstruction(SemanticOperation::OP_BRANCH,
                     SemanticType(), SemanticOperand(),
                     SemanticOperand::guestAddress(0x2000, type)));

        AMD64Lowering lowering;
        TestRegisterMap registerMap;
        AMD64Lowering::LoweredBlock lowered;
        lowering.lowerBlock(block, registerMap, lowered);
        // The labels of the two guest instructions, the dependency of the
        // branch only (the local label is resolved)
        TESTS_ASSERT_EQUAL(lowered.m_labels.length(), 2);
        TESTS_ASSERT((*lowered.m_labels.begin()).m_name == cString("B_1000"));
        TESTS_ASSERT_EQUAL((*lowered.m_labels.begin()).m_offset, 0);
        {
            cList<AMD64Lowering::LabelDefinition>::iterator second = lowered.m_labels.begin();
            second++;
            TESTS_ASSERT((*second).m_name == cString("B_1004"));
            TESTS_ASSERT((*second).m_offset > 0);
        }
        TESTS_ASSERT_EQUAL(lowered.m_dependencies.length(), 1);
        TESTS_ASSERT((*lowered.m_dependencies.begin()).m_name == cString("B_2000"));
        // lower() on the same block refuses: it can't resolve the label
        TESTS_EXCEPTION(lowerBlock(block));

        // Run it
        cBuffer image;
        cList<Stub> stubs;
        TESTS_ASSERT(linkWithStubs(lowered, image, stubs));
        cBuffer none(1);
        RowMemory guestMemory(none, 0x1000, false);
        LoweredMemory memory(guestMemory);
        SemanticInterpreter interpreter(getTarget(), memory);
        interpreter.setRegister(registerMap.getStateRegister(), SemanticValue(STATE_BASE));
        interpreter.setRegister(registerMap.getMemoryBaseRegister(), SemanticValue(MEMORY_BIAS));
        interpreter.setRegister(ia32dis::AMD64_REG_RIP, SemanticValue(CODE_BASE));
        const LoweredRun run = runLowered(image, lowered.m_code.getSize(), stubs,
                                          interpreter);
        TESTS_ASSERT(run.m_error == NULL);
        TESTS_ASSERT(!run.m_fault);
        TESTS_ASSERT(getStub(stubs, run.m_stub).m_name == cString("B_2000"));
        // 3, 6, 9, 12: stops when x2 >= 10
        TESTS_ASSERT_EQUAL(memory.getState(registerMap.getStateOffset(RISCV_REG_X0 + 2)), 12);

        // patchRelative32
        cBuffer field(8);
        for (uint i = 0; i < 8; i++)
            field[i] = 0;
        AMD64Lowering::patchRelative32(field, 2, -2);
        TESTS_ASSERT(field[2] == 0xFE && field[3] == 0xFF && field[4] == 0xFF &&
                     field[5] == 0xFF && field[6] == 0);
        TESTS_EXCEPTION(AMD64Lowering::patchRelative32(field, 6, 0));
        TESTS_EXCEPTION(AMD64Lowering::patchRelative32(field, 0, (int64)1 << 40));
    }


#ifdef LOWERING_TEST_NATIVE
    // Lower a block with the entry and exit stubs of the runtime around it,
    // resolve the labels (a B_ label stores its address as the next program
    // counter and exits), and return the code
    bool buildNative(const SemanticBlock& block, cBuffer& code)
    {
        FirstPassBinaryPtr binary(new FirstPassBinary(
                            OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
        binary->createNewBlockWithoutChange(0,
                StackInterfacePtr(new TestLoweringStack(binary)));
        binary->changeBasicBlock(0);
        AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                            binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);
        assembleText(assembler,
            "push rbx\npush rbp\npush r12\npush r13\npush r14\npush r15\n"
            "mov r15, rdi\nmov r14, rsi\n");

        SemanticLoweringPtr lowering = SemanticLoweringFactory::getLowering(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);
        TestRegisterMap registerMap;
        lowering->lower(block, registerMap, *assembler);

        // A stub for each label the block refers to, then the exit
        cList<cString> names;
        cList<uint> positions;
        const BinaryDependencies::DependencyObjectList& list =
                                    binary->getCurrentDependecies().getList();
        cList<cString> wanted;
        for (BinaryDependencies::DependencyObjectList::iterator i = list.begin();
             i != list.end(); i++)
        {
            bool isKnown = false;
            for (cList<cString>::iterator w = wanted.begin(); w != wanted.end(); w++)
                isKnown = isKnown || (*w == (*i).m_name);
            if (!isKnown && !((*i).m_name == AMD64Lowering::getExitLabel()))
                wanted.append((*i).m_name);
        }
        for (cList<cString>::iterator w = wanted.begin(); w != wanted.end(); w++)
        {
            names.append(*w);
            positions.append(binary->getCurrentBlockData().getSize());
            cSArray<char> label = (*w).getASCIIstring();
            // (the name is B_<hex>)
            char text[300];
            snprintf(text, sizeof(text),
                     "mov rax, 0x%s\nmov qword [r15+0x100], rax\n"
                     "mov qword [r15+0x110], 0x1000\njmp __reforge_exit\n",
                     label.getBuffer() + 2);
            assembleText(assembler, text);
        }
        names.append(AMD64Lowering::getExitLabel());
        positions.append(binary->getCurrentBlockData().getSize());
        assembleText(assembler,
            "pop r15\npop r14\npop r13\npop r12\npop rbp\npop rbx\nret\n");

        // Resolve the relative 32 bit dependencies
        const cBuffer& data = binary->getCurrentBlockData();
        code = cBuffer(data.getSize());
        for (uint i = 0; i < data.getSize(); i++)
            code[i] = data[i];
        for (BinaryDependencies::DependencyObjectList::iterator i = list.begin();
             i != list.end(); i++)
        {
            bool isFound = false;
            cList<uint>::iterator p = positions.begin();
            for (cList<cString>::iterator n = names.begin(); n != names.end(); n++, p++)
            {
                if (!(*n == (*i).m_name))
                    continue;
                const int32 relative = (int32)((*p) - ((*i).m_position + 4));
                for (uint b = 0; b < 4; b++)
                    code[(*i).m_position + b] = (uint8)(relative >> (8 * b));
                isFound = true;
            }
            if (!isFound)
                return false;
        }
        return true;
    }

    // Assemble a text into the binary of the assembler
    void assembleText(AssemblerInterfacePtr& assembler, const char* text)
    {
        cStringerStreamPtr stream = assembler->getAssembler();
        *stream << cString(text);
        IA32AssemblerStream* assembly =
                    dynamic_cast<IA32AssemblerStream*>(stream.getPointer());
        TESTS_ASSERT(assembly != NULL);
        assembly->assemble();
    }

    // Run the block natively and in the interpreter on 'trials' random
    // states, and compare the guest registers, the memory and the exit.
    // Return false on a difference
    bool runNative(const char* name, SemanticBlock& block, uint trials,
                   bool isPointers)
    {
        // (the blocks built from IR have no guest instruction: the address
        // after the block is the one the interpreter and the code report)
        block.setFallThroughAddress(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, ROW_ADDRESS + 4));
        cBuffer code;
        if (!buildNative(block, code))
        {
            LOWERING_ROW_FAIL(name, "an unresolved label");
            return false;
        }
        void* memory = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED)
        {
            // (a host which doesn't allow executable memory: nothing to run)
            m_nativeSkipped++;
            return true;
        }
        for (uint i = 0; i < code.getSize(); i++)
            ((uint8*)memory)[i] = code[i];
        NativeFunction function = reinterpret_cast<NativeFunction>(memory);

        static NativeTestMemory interpreted;
        static NativeTestMemory native;
        bool isOk = true;
        for (uint trial = 0; isOk && (trial < trials); trial++)
        {
            uint64 state[0x800];
            memset(state, 0, sizeof(state));
            for (uint i = 0; i < NATIVE_MEMORY_SIZE; i++)
                interpreted.m_bytes[i] = native.m_bytes[i] = (uint8)nativeRandom();
            SemanticInterpreter interpreter(getGuest(), interpreted);
            for (uint i = 1; i < 32; i++)
            {
                state[i] = nativeValue(isPointers);
                interpreter.setRegister(RISCV_REG_X0 + i, SemanticValue(state[i]));
            }
            const SemanticInterpreter::ExitReason reason = interpreter.run(block);
            if ((reason == SemanticInterpreter::EXIT_MEMORY_FAULT) ||
                (reason == SemanticInterpreter::EXIT_UNDEFINED))
            {
                // (the random state faults in the guest: nothing to compare)
                continue;
            }
            function(state, native.m_bytes - NATIVE_GUEST_BASE);
            m_nativeChecks++;

            for (uint i = 0; i < 32; i++)
            {
                if (state[i] != interpreter.getRegister(RISCV_REG_X0 + i).getWord(0))
                {
                    LOWERING_ROW_FAIL(name, "a register differs from the interpreter");
                    cout << "  x" << i << " native "
                         << HEXQWORD(state[i]) << " interpreter "
                         << HEXQWORD(interpreter.getRegister(RISCV_REG_X0 + i).getWord(0))
                         << endl;
                    isOk = false;
                }
            }
            if (memcmp(interpreted.m_bytes, native.m_bytes, NATIVE_MEMORY_SIZE) != 0)
            {
                LOWERING_ROW_FAIL(name, "the memory differs from the interpreter");
                isOk = false;
            }
            // The exit: the next address (the target of a B_ label or the
            // program counter slot) and a trap
            if (state[0x100 / 8] != interpreter.getNextAddress())
            {
                LOWERING_ROW_FAIL(name, "the next address differs from the interpreter");
                isOk = false;
            }
            const bool isTrap = (state[0x110 / 8] & 0xFF) == AMD64Lowering::EXIT_TRAP;
            if (isTrap != (reason == SemanticInterpreter::EXIT_TRAP))
            {
                LOWERING_ROW_FAIL(name, "the trap differs from the interpreter");
                isOk = false;
            }
        }
        munmap(memory, 0x10000);
        return isOk;
    }

    // 2d: the RV64I rows run natively
    void testNativeRows()
    {
        ArchitectureDescriptionPtr guest = getGuest();
        RiscvSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_RISCV_64, guest);
        SemanticLegalizer legalizer(guest, getTarget());
        uint rows = 0;
        uint failures = 0;
        for (const SemanticLoweringRow* row = gRv64SemanticRows; row->m_text != NULL; row++)
        {
            if (!isRv64iRow(*row))
                continue;
            rows++;
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            TESTS_ASSERT(liftRow(*row, lifter, block));
            legalizer.legalize(block);
            // (the interpreter reports the fall-through address of the
            // block: the one which the lowering derives from the guest)
            block.setFallThroughAddress(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, ROW_ADDRESS + 4));
            // A block which accesses memory gets pointers in its registers
            bool isMemory = false;
            const cList<SemanticInstruction>& instructions = block.getInstructions();
            for (cList<SemanticInstruction>::iterator i = instructions.begin();
                 i != instructions.end(); i++)
            {
                isMemory = isMemory ||
                           ((*i).getOperation() == SemanticOperation::OP_LOAD) ||
                           ((*i).getOperation() == SemanticOperation::OP_STORE);
            }
            if (!runNative(row->m_text, block, 12, isMemory))
                failures++;
        }
        cout << rows << " RV64I rows run natively, " << m_nativeChecks
             << " comparisons, " << failures << " failures";
        if (m_nativeSkipped != 0)
            cout << ", " << m_nativeSkipped << " skipped (no executable memory)";
        cout << endl;
        TESTS_ASSERT(failures == 0);
    }

    // 2d: edge encodings the rows don't have (register aliasing rd == rs1,
    // shifts by 31/63 and by their own register, W forms, odd jalr offsets,
    // negative offsets, lui/auipc with the sign bit, comparisons against
    // -1/x0), run natively against the interpreter on edge register values
    void testNativeEdgeEncodings()
    {
        static const SemanticLoweringRow edgeRows[] = {
        { "e7801000", "jalr 1(ra)", "", "" },
        { "67053500", "jalr a0, 3(a0)", "", "" },
        { "6785f5ff", "jalr a0, -1(a1)", "", "" },
        { "67000500", "jr a0", "", "" },
        { "3b95c500", "sllw a0, a1, a2", "", "" },
        { "3bd5c500", "srlw a0, a1, a2", "", "" },
        { "3bd5c540", "sraw a0, a1, a2", "", "" },
        { "3315a500", "sll a0, a0, a0", "", "" },
        { "33d5b540", "sra a0, a1, a1", "", "" },
        { "13d5f503", "srli a0, a1, 63", "", "" },
        { "13d5f543", "srai a0, a1, 63", "", "" },
        { "1395f503", "slli a0, a1, 63", "", "" },
        { "1bd5f501", "srliw a0, a1, 31", "", "" },
        { "1bd5f541", "sraiw a0, a1, 31", "", "" },
        { "1b95f501", "slliw a0, a1, 31", "", "" },
        { "1b950500", "slliw a0, a1, 0", "", "" },
        { "1b050580", "addiw a0, a0, -2048", "", "" },
        { "3b05a500", "addw a0, a0, a0", "", "" },
        { "3b85b540", "subw a0, a1, a1", "", "" },
        { "3b05b040", "negw a0, a1", "", "" },
        { "3305a040", "neg a0, a0", "", "" },
        { "13b5f5ff", "sltiu a0, a1, -1", "", "" },
        { "13b51500", "seqz a0, a1", "", "" },
        { "13a50580", "slti a0, a1, -2048", "", "" },
        { "3335b000", "snez a0, a1", "", "" },
        { "33a5b500", "slt a0, a1, a1", "", "" },
        { "33a50500", "sltz a0, a1", "", "" },
        { "37050080", "lui a0, 524288", "", "" },
        { "37f5ffff", "lui a0, 1048575", "", "" },
        { "17050080", "auipc a0, 524288", "", "" },
        { "17f5ffff", "auipc a0, 1048575", "", "" },
        { "13805500", "addi zero, a1, 5", "", "" },
        { "33050000", "add a0, zero, zero", "", "" },
        { "03a5c5ff", "lw a0, -4(a1)", "", "" },
        { "03e5c5ff", "lwu a0, -4(a1)", "", "" },
        { "0395e5ff", "lh a0, -2(a1)", "", "" },
        { "03d5e5ff", "lhu a0, -2(a1)", "", "" },
        { "0385f5ff", "lb a0, -1(a1)", "", "" },
        { "03c5f57f", "lbu a0, 2047(a1)", "", "" },
        { "03b50580", "ld a0, -2048(a1)", "", "" },
        { "83b50500", "ld a1, 0(a1)", "", "" },
        { "03250500", "lw a0, 0(a0)", "", "" },
        { "233ca5fe", "sd a0, -8(a0)", "", "" },
        { "23bcb5fe", "sd a1, -8(a1)", "", "" },
        { "2322a500", "sw a0, 4(a0)", "", "" },
        { "239fb5fe", "sh a1, -2(a1)", "", "" },
        { "a38fb5fe", "sb a1, -1(a1)", "", "" },
        { "2300a500", "sb a0, 0(a0)", "", "" },
        { "6304a500", "beq a0, a0, 8", "", "" },
        { "6314a500", "bne a0, a0, 8", "", "" },
        { "6344b500", "blt a0, a1, 8", "", "" },
        { "6354b500", "bge a0, a1, 8", "", "" },
        { "6364b500", "bltu a0, a1, 8", "", "" },
        { "6374b500", "bgeu a0, a1, 8", "", "" },
        { "e34ca5fe", "blt a0, a0, -8", "", "" },
        { "63740500", "bgeu a0, zero, 8", "", "" },
        { "6364a000", "bltu zero, a0, 8", "", "" },
        { "6354a000", "blez a0, 8", "", "" },
        { "6ff09fff", "j -8", "", "" },
        { "ef008000", "jal 8", "", "" },
        { "eff0ff7f", "jal 1048574", "", "" },
        { "13f5f5ff", "andi a0, a1, -1", "", "" },
        { "13c5f5ff", "not a0, a1", "", "" },
        { "13e50580", "ori a0, a1, -2048", "", "" },
        { "3345a500", "xor a0, a0, a0", "", "" },
        { "13000000", "nop", "", "" },
        { "0f00f00f", "fence", "", "" },
        { NULL, NULL, NULL, NULL }
        };
        ArchitectureDescriptionPtr guest = getGuest();
        RiscvSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_RISCV_64, guest);
        SemanticLegalizer legalizer(guest, getTarget());
        uint failures = 0;
        uint count = 0;
        for (const SemanticLoweringRow* row = edgeRows; row->m_text != NULL; row++)
        {
            count++;
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            TESTS_ASSERT(liftRow(*row, lifter, block));
            legalizer.legalize(block);
            block.setFallThroughAddress(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, ROW_ADDRESS + 4));
            bool isMemory = false;
            const cList<SemanticInstruction>& instructions = block.getInstructions();
            for (cList<SemanticInstruction>::iterator i = instructions.begin();
                 i != instructions.end(); i++)
            {
                isMemory = isMemory ||
                           ((*i).getOperation() == SemanticOperation::OP_LOAD) ||
                           ((*i).getOperation() == SemanticOperation::OP_STORE);
            }
            if (!runNative(row->m_text, block, 60, isMemory))
                failures++;
        }
        cout << count << " edge encodings run natively, " << failures
             << " failures" << endl;
        TESTS_ASSERT(failures == 0);
    }

    // The integer operand of a width
    static SemanticType integerType(uint bits) { return SemanticType::integer(bits); }
    static SemanticOperand guestX(uint index, uint bits)
    {
        return SemanticOperand::registerOperand(RISCV_REG_X0 + index, integerType(bits));
    }

    // Append "x10 = zero extend 'value'; x11 = sign extend 'value'"
    static void observe(SemanticBlock& block, const SemanticOperand& value, uint bits)
    {
        const SemanticType wide = integerType(64);
        if (bits == 64)
        {
            block.append(SemanticInstruction(SemanticOperation::OP_MOVE, wide, guestX(10, 64), value));
            block.append(SemanticInstruction(SemanticOperation::OP_MOVE, wide, guestX(11, 64), value));
            return;
        }
        SemanticOperand zero = block.newTemporary(wide);
        block.append(SemanticInstruction(SemanticOperation::OP_ZERO_EXTEND, wide, zero, value));
        block.append(SemanticInstruction(SemanticOperation::OP_MOVE, wide, guestX(10, 64), zero));
        SemanticOperand sign = block.newTemporary(wide);
        block.append(SemanticInstruction(SemanticOperation::OP_SIGN_EXTEND, wide, sign, value));
        block.append(SemanticInstruction(SemanticOperation::OP_MOVE, wide, guestX(11, 64), sign));
    }

    // 2d: synthetic IR of every width and operand form, run natively. The
    // RV64I rows only use some of the widths
    void testNativeSynthetic()
    {
        typedef SemanticOperation Op;
        static const uint widths[] = { 8, 16, 32, 64 };
        static const Op::Operation binary[] = {
            Op::OP_ADD, Op::OP_SUBTRACT, Op::OP_AND, Op::OP_OR, Op::OP_XOR,
            Op::OP_AND_NOT, Op::OP_MULTIPLY, Op::OP_MULTIPLY_HIGH_UNSIGNED,
            Op::OP_MULTIPLY_HIGH_SIGNED, Op::OP_SHIFT_LEFT,
            Op::OP_SHIFT_RIGHT_LOGICAL, Op::OP_SHIFT_RIGHT_ARITHMETIC,
            Op::OP_ROTATE_LEFT, Op::OP_ROTATE_RIGHT };
        static const Op::Operation unary[] = { Op::OP_NOT, Op::OP_NEGATE };
        static const Op::Condition conditions[] = {
            Op::COND_EQUAL, Op::COND_NOT_EQUAL, Op::COND_SIGNED_LESS,
            Op::COND_SIGNED_LESS_EQUAL, Op::COND_SIGNED_GREATER,
            Op::COND_SIGNED_GREATER_EQUAL, Op::COND_UNSIGNED_LESS,
            Op::COND_UNSIGNED_LESS_EQUAL, Op::COND_UNSIGNED_GREATER,
            Op::COND_UNSIGNED_GREATER_EQUAL, Op::COND_ALWAYS, Op::COND_NEVER };
        const SemanticType wide = integerType(64);
        const uint failuresBefore = m_failures;
        uint blocks = 0;

        for (uint w = 0; w < 4; w++)
        {
            const uint bits = widths[w];
            const SemanticType type = integerType(bits);

            // d = s0 <op> s1: both temporaries, an immediate second operand
            // and an immediate first operand
            for (uint o = 0; o < sizeof(binary) / sizeof(binary[0]); o++)
            {
                if (((binary[o] == Op::OP_MULTIPLY_HIGH_UNSIGNED) ||
                     (binary[o] == Op::OP_MULTIPLY_HIGH_SIGNED)) && (bits == 8))
                    continue;
                for (uint form = 0; form < 3 * 4; form++)
                {
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    SemanticOperand a = block.newTemporary(type);
                    SemanticOperand c = block.newTemporary(type);
                    block.append(SemanticInstruction(Op::OP_TRUNCATE, type, a, guestX(1, 64)));
                    block.append(SemanticInstruction(Op::OP_TRUNCATE, type, c, guestX(2, 64)));
                    SemanticOperand s0 = a;
                    SemanticOperand s1 = c;
                    if (form % 3 == 1)
                        s1 = SemanticOperand::immediate(nativeImmediate(), type);
                    if (form % 3 == 2)
                        s0 = SemanticOperand::immediate(nativeImmediate(), type);
                    SemanticOperand d = block.newTemporary(type);
                    block.append(SemanticInstruction(binary[o], type, d, s0, s1));
                    observe(block, d, bits);
                    blocks++;
                    runNative(Op::getName(binary[o]), block, 6, false);
                }
            }
            for (uint o = 0; o < sizeof(unary) / sizeof(unary[0]); o++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                SemanticOperand a = block.newTemporary(type);
                block.append(SemanticInstruction(Op::OP_TRUNCATE, type, a, guestX(1, 64)));
                SemanticOperand d = block.newTemporary(type);
                block.append(SemanticInstruction(unary[o], type, d, a));
                observe(block, d, bits);
                blocks++;
                runNative(Op::getName(unary[o]), block, 6, false);
            }

            // compare, and what is done with a boolean
            for (uint k = 0; k < sizeof(conditions) / sizeof(conditions[0]); k++)
            {
                for (uint form = 0; form < 3 * 3; form++)
                {
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    SemanticOperand a = block.newTemporary(type);
                    SemanticOperand c = block.newTemporary(type);
                    block.append(SemanticInstruction(Op::OP_TRUNCATE, type, a, guestX(1, 64)));
                    block.append(SemanticInstruction(Op::OP_TRUNCATE, type, c, guestX(2, 64)));
                    SemanticOperand s0 = a;
                    SemanticOperand s1 = c;
                    if (form % 3 == 1)
                        s1 = SemanticOperand::immediate(nativeImmediate(), type);
                    if (form % 3 == 2)
                        s0 = SemanticOperand::immediate(nativeImmediate(), type);
                    const SemanticType boolean = integerType(1);
                    SemanticOperand flag = block.newTemporary(boolean);
                    SemanticInstruction compare(Op::OP_COMPARE, type, flag, s0, s1);
                    compare.setCondition(conditions[k]);
                    block.append(compare);
                    // (x10, x11) the boolean zero and sign extended
                    SemanticOperand zero = block.newTemporary(wide);
                    block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, zero, flag));
                    block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(10, 64), zero));
                    SemanticOperand sign = block.newTemporary(type);
                    block.append(SemanticInstruction(Op::OP_SIGN_EXTEND, type, sign, flag));
                    observe(block, sign, bits);
                    // x12 = flag ? a : c
                    SemanticOperand chosen = block.newTemporary(type);
                    block.append(SemanticInstruction(Op::OP_SELECT, type, chosen, flag, a, c));
                    SemanticOperand chosenWide = block.newTemporary(wide);
                    block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, chosenWide, chosen));
                    block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(12, 64), chosenWide));
                    // x13 = (!flag | flag) & !flag, x14 = flag ^ !flag
                    SemanticOperand not1 = block.newTemporary(boolean);
                    block.append(SemanticInstruction(Op::OP_NOT, boolean, not1, flag));
                    SemanticOperand or1 = block.newTemporary(boolean);
                    block.append(SemanticInstruction(Op::OP_OR, boolean, or1, not1, flag));
                    SemanticOperand and1 = block.newTemporary(boolean);
                    block.append(SemanticInstruction(Op::OP_AND, boolean, and1, or1, not1));
                    SemanticOperand xor1 = block.newTemporary(boolean);
                    block.append(SemanticInstruction(Op::OP_XOR, boolean, xor1, flag, not1));
                    SemanticOperand z3 = block.newTemporary(wide);
                    block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, z3, and1));
                    block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(13, 64), z3));
                    SemanticOperand z4 = block.newTemporary(wide);
                    block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, z4, xor1));
                    block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(14, 64), z4));
                    blocks++;
                    runNative("compare", block, 6, false);
                }
            }

            // zero and sign extension, to every wider width, of a value and
            // of an immediate
            for (uint w2 = w + 1; w2 < 4; w2++)
            {
                for (uint sign = 0; sign < 2; sign++)
                {
                    const SemanticType to = integerType(widths[w2]);
                    const Op::Operation extension = sign ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND;
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    SemanticOperand a = block.newTemporary(type);
                    block.append(SemanticInstruction(Op::OP_TRUNCATE, type, a, guestX(1, 64)));
                    SemanticOperand d = block.newTemporary(to);
                    block.append(SemanticInstruction(extension, to, d, a));
                    SemanticOperand e = block.newTemporary(to);
                    block.append(SemanticInstruction(extension, to, e,
                                 SemanticOperand::immediate(nativeImmediate(), type)));
                    observe(block, d, widths[w2]);
                    SemanticOperand z = block.newTemporary(wide);
                    if (widths[w2] == 64)
                        block.append(SemanticInstruction(Op::OP_MOVE, wide, z, e));
                    else
                        block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, z, e));
                    block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(12, 64), z));
                    blocks++;
                    runNative("extend", block, 6, false);
                }
            }

            // moves of immediates and temporaries, a truncation to a
            // boolean, and the loads and stores of the width
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                SemanticOperand a = block.newTemporary(type);
                block.append(SemanticInstruction(Op::OP_TRUNCATE, type, a, guestX(1, 64)));
                SemanticOperand m = block.newTemporary(type);
                block.append(SemanticInstruction(Op::OP_MOVE, type, m, a));
                SemanticOperand n = block.newTemporary(type);
                block.append(SemanticInstruction(Op::OP_MOVE, type, n,
                             SemanticOperand::immediate(nativeImmediate(), type)));
                SemanticOperand bit = block.newTemporary(integerType(1));
                block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(1), bit, guestX(2, 64)));
                SemanticOperand bitWide = block.newTemporary(wide);
                block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, bitWide, bit));
                block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(12, 64), bitWide));
                observe(block, m, bits);
                SemanticOperand nWide = block.newTemporary(wide);
                if (bits == 64)
                    block.append(SemanticInstruction(Op::OP_MOVE, wide, nWide, n));
                else
                    block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, nWide, n));
                block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(13, 64), nWide));
                block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(15, 64),
                             SemanticOperand::immediate(nativeImmediate(), wide)));
                block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(16, 64),
                             SemanticOperand::guestAddress(0x140001234ULL, wide)));
                block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(17, 64),
                             SemanticOperand::immediate((int64)0xFEDCBA9876543210ULL, wide)));
                blocks++;
                runNative("move", block, 6, false);
            }
            for (uint form = 0; form < 3; form++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                SemanticOperand loaded = block.newTemporary(type);
                block.append(SemanticInstruction(Op::OP_LOAD, type, loaded, guestX(1, 64)));
                observe(block, loaded, bits);
                SemanticOperand value = loaded;
                if (form == 1)
                    value = SemanticOperand::immediate(nativeImmediate(), type);
                if (form == 2)
                    value = guestX(2, bits);
                SemanticOperand address = block.newTemporary(wide);
                block.append(SemanticInstruction(Op::OP_ADD, wide, address, guestX(1, 64),
                             SemanticOperand::immediate(8, wide)));
                block.append(SemanticInstruction(Op::OP_STORE, type, SemanticOperand(), address, value));
                SemanticOperand again = block.newTemporary(type);
                block.append(SemanticInstruction(Op::OP_LOAD, type, again,
                             SemanticOperand::guestAddress(NATIVE_GUEST_BASE + 0x100, wide)));
                SemanticOperand againWide = block.newTemporary(wide);
                if (bits == 64)
                    block.append(SemanticInstruction(Op::OP_MOVE, wide, againWide, again));
                else
                    block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, againWide, again));
                block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(12, 64), againWide));
                block.append(SemanticInstruction(Op::OP_STORE, type, SemanticOperand(),
                             SemanticOperand::guestAddress(NATIVE_GUEST_BASE + 0x200, wide),
                             SemanticOperand::immediate(nativeImmediate(), type)));
                blocks++;
                runNative("memory", block, 6, true);
            }
        }
        cout << blocks << " synthetic blocks run natively, " << m_nativeChecks
             << " comparisons in total" << endl;
        TESTS_ASSERT(m_failures == failuresBefore);
    }
#endif

    // The failures of the rows
    uint m_failures;
    // The comparisons of the differential test
    uint m_differentialChecks;
#ifdef LOWERING_TEST_NATIVE
    // The native comparisons made, and the blocks which couldn't run
    uint m_nativeChecks;
    uint m_nativeSkipped;
#endif
};

// Instance test object
TestObjectTestSemanticLowering g_globalTestSemanticLowering;
