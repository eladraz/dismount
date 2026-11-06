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
#ifdef LOWERING_TEST_NATIVE
        testNativeRows();
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
        block.append(SemanticInstruction(SemanticOperation::OP_DIVIDE_SIGNED, type,
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
            TESTS_ASSERT(strstr(message.getBuffer(), "divide_signed") != NULL);
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
