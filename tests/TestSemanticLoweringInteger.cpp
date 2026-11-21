/*
 * TestSemanticLoweringInteger.cpp
 *
 * Native tests of the integer operations of the AMD64 lowering which
 * AMD64LoweringInteger.cpp covers: the divisions and remainders (with the
 * behaviours of a division by zero: fault, zero, all ones), the leading and
 * trailing zero counts, the population count, the byte swap, the bit
 * reversal and the signed by unsigned high product. Each IR operation is
 * lowered with lowerBlock(), run natively on the host, and compared with the
 * SemanticInterpreter on the same guest registers: the destination, the next
 * address and the trap (kind and program counter) of the fault.
 *
 * The inputs are the edge values 0, 1, -1, the minimum and the maximum of each
 * width (and their neighbours), in every pair for the binary operations, with
 * random bits above the width (the lowering must read only the operand width).
 * A divide error on the host would kill the test: the lowering guards every
 * division.
 *
 * OP_ADD_CARRY (the adc and sbc of the A64 lifter) is run the same way, with
 * the carry in a register and as a constant, 0 and 1, in every width. The
 * A64 part lifts the adc, adcs, sbc, sbcs, ngc and ngcs words of 32 and 64
 * bits, legalizes them with the AArch64 guest and the AMD64 target, lowers
 * and runs them natively, and compares the 31 X registers and N, Z, C, V
 * with the SemanticInterpreter on the lifted block.
 *
 * The native runs need an executable mapping and an x86-64 Linux host; the
 * other hosts skip them.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#define LOWERING_INTEGER_NATIVE 1
#endif

#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "xStl/exceptions.h"
#include "xStl/stream/ioStream.h"
#include "xStl/data/datastream.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticLowering.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/semantics/SemanticInterpreter.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"

// The helpers and the test object are local to this file
namespace {

// The guest registers of the blocks: the two sources and the destination
static const uint SRC0 = ia32dis::AMD64_REG_RSI;
static const uint SRC1 = ia32dis::AMD64_REG_RDI;
static const uint DEST = ia32dis::AMD64_REG_R8;
// The carry in of OP_ADD_CARRY when it is a register (a boolean)
static const uint CARRY = ia32dis::AMD64_REG_R9;

// The guest address of the block, and the fall through address after it
static const uint64 ROW_ADDRESS = 0x80001000ULL;

// The guest is the AMD64 description (only its registers are used)
static ArchitectureDescriptionPtr getGuest()
{
    return ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);
}

// The target of the legalizer: the AMD64 processor
static ArchitectureDescriptionPtr getTarget()
{
    return ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);
}

// The state of the tests: the guest registers at 0x400 + 8 * id, the program
// counter at 0x100 and the exit reason at 0x110, as the runtime lays them out
class IntegerTestRegisterMap : public SemanticRegisterMap {
public:
    virtual bool getTargetRegister(uint, uint&) const { return false; }
    virtual uint getStateOffset(uint guestRegister) const
    {
        return 0x400 + guestRegister * 8;
    }
    virtual uint getStateRegister() const { return ia32dis::AMD64_REG_R15; }
    virtual uint getTemporaryOffset(uint temporary) const { return 0x200 + temporary * 8; }
    virtual uint getMemoryBaseRegister() const { return ia32dis::AMD64_REG_R14; }
    virtual uint getProgramCounterOffset() const { return 0x100; }
    virtual uint getExitReasonOffset() const { return 0x110; }
};

// The stack of the first pass binary, which the assembler of the tests wants
class IntegerTestStack : public StackInterface {
public:
    IntegerTestStack(FirstPassBinaryPtr& binary) :
        StackInterface(*binary, true, 4) {}
    RegisterAllocationTable m_registers;
    virtual uint getStackType() { return 4; }
    virtual uint getRegisterSize() { return 4; }
    virtual RegisterAllocationTable& getRegistersTable() { return m_registers; }
    virtual const RegisterAllocationTable& getRegistersTable() const { return m_registers; }
    virtual StackLocation replaceRegisterToStackVariable(StackLocation source) { return source; }
};

// The memory of the interpreter: the blocks of the tests don't access it
class IntegerNoMemory : public SemanticMemory {
public:
    virtual bool read(uint64, uint8*, uint) { return false; }
    virtual bool write(uint64, const uint8*, uint) { return false; }
};

#ifdef LOWERING_INTEGER_NATIVE
// The guest registers of the A64 blocks: the state holds the register 'id' at
// 0x1000 + 8 * id (the flags are registers too). The temporaries are 16 bytes
class Arm64TestRegisterMap : public SemanticRegisterMap {
public:
    virtual bool getTargetRegister(uint, uint&) const { return false; }
    virtual uint getStateOffset(uint guestRegister) const
    {
        return 0x1000 + guestRegister * 8;
    }
    virtual uint getStateRegister() const { return ia32dis::AMD64_REG_R15; }
    virtual uint getTemporaryOffset(uint temporary) const { return 0x200 + temporary * 16; }
    virtual uint getMemoryBaseRegister() const { return ia32dis::AMD64_REG_R14; }
    virtual uint getProgramCounterOffset() const { return 0x100; }
    virtual uint getExitReasonOffset() const { return 0x110; }
};

// The native function: the state in rdi, the memory bias in rsi
typedef void (*NativeFunction)(uint64* state, uint8* bias);

// The prologue and the epilogue of the native function (the exit label)
static const char* const gPrologue =
    "push rbx\npush rbp\npush r12\npush r13\npush r14\npush r15\n"
    "mov r15, rdi\nmov r14, rsi\n";
static const char* const gEpilogue =
    "pop r15\npop r14\npop r13\npop r12\npop rbp\npop rbx\nret\n";

// Assemble a text into bytes
static cBuffer assembleBytes(const char* text)
{
    FirstPassBinaryPtr binary(new FirstPassBinary(
                            OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
    binary->createNewBlockWithoutChange(0,
            StackInterfacePtr(new IntegerTestStack(binary)));
    binary->changeBasicBlock(0);
    AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                            binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);
    cStringerStreamPtr stream = assembler->getAssembler();
    *stream << cString(text);
    IA32AssemblerStream* assembly =
                dynamic_cast<IA32AssemblerStream*>(stream.getPointer());
    TESTS_ASSERT(assembly != NULL);
    assembly->assemble();
    const cBuffer& data = binary->getCurrentBlockData();
    cBuffer code(data.getSize());
    for (uint i = 0; i < data.getSize(); i++)
        code[i] = data[i];
    return code;
}

// Write a 32 bit little endian value at 'offset'
static void patch32(cBuffer& code, uint offset, int32 value)
{
    for (uint b = 0; b < 4; b++)
        code[offset + b] = (uint8)((uint32)value >> (8 * b));
}

// Lower a block and link it into executable memory: the code is the
// prologue, the lowered block, a stub for each guest address it leaves to
// (the next program counter and the reason "not a trap") and the epilogue.
// Return the function, or NULL when the host won't map executable memory
static NativeFunction buildNative(const SemanticBlock& block,
                                  const SemanticRegisterMap& registerMap,
                                  void*& memory, uint& memorySize)
{
    AMD64Lowering lowering;
    AMD64Lowering::LoweredBlock lowered;
    lowering.lowerBlock(block, registerMap, lowered);

    const cBuffer prologue = assembleBytes(gPrologue);
    const cBuffer epilogue = assembleBytes(gEpilogue);

    // The stubs, one for each guest label the block refers to
    cList<cString> names;
    for (cList<AMD64Lowering::LabelDependency>::iterator d =
            lowered.m_dependencies.begin();
         d != lowered.m_dependencies.end(); d++)
    {
        bool isKnown = (*d).m_name == AMD64Lowering::getExitLabel();
        for (cList<cString>::iterator n = names.begin(); n != names.end(); n++)
            isKnown = isKnown || (*n == (*d).m_name);
        if (!isKnown)
            names.append((*d).m_name);
    }
    const uint codeOffset = prologue.getSize();
    uint offset = codeOffset + lowered.m_code.getSize();
    cList<uint> stubOffsets;
    cList<cBuffer> stubs;
    for (cList<cString>::iterator n = names.begin(); n != names.end(); n++)
    {
        // The name is B_<hex>: the next program counter, with the reason
        // 0x1000 (not a trap), then the jump to the epilogue
        cSArray<char> label = (*n).getASCIIstring();
        char text[200];
        snprintf(text, sizeof(text),
                 "mov rax, 0x%s\nmov qword [r15+0x100], rax\n"
                 "mov qword [r15+0x110], 0x1000\n", label.getBuffer() + 2);
        cBuffer stub = assembleBytes(text);
        stubOffsets.append(offset);
        stubs.append(stub);
        offset += stub.getSize() + 5;
    }
    const uint epilogueOffset = offset;

    const uint size = epilogueOffset + epilogue.getSize();
    cBuffer code(size);
    for (uint i = 0; i < codeOffset; i++)
        code[i] = prologue[i];
    for (uint i = 0; i < lowered.m_code.getSize(); i++)
        code[codeOffset + i] = lowered.m_code[i];
    cList<uint>::iterator so = stubOffsets.begin();
    for (cList<cBuffer>::iterator s = stubs.begin(); s != stubs.end(); s++, so++)
    {
        for (uint i = 0; i < (*s).getSize(); i++)
            code[*so + i] = (*s)[i];
        // jmp epilogue
        const uint jump = *so + (*s).getSize();
        code[jump] = 0xE9;
        patch32(code, jump + 1, (int32)(epilogueOffset - (jump + 5)));
    }
    for (uint i = 0; i < epilogue.getSize(); i++)
        code[epilogueOffset + i] = epilogue[i];

    // Resolve the references of the lowered code
    for (cList<AMD64Lowering::LabelDependency>::iterator d =
            lowered.m_dependencies.begin();
         d != lowered.m_dependencies.end(); d++)
    {
        uint target = epilogueOffset;
        if (!((*d).m_name == AMD64Lowering::getExitLabel()))
        {
            target = 0;
            cList<uint>::iterator so2 = stubOffsets.begin();
            for (cList<cString>::iterator n = names.begin(); n != names.end();
                 n++, so2++)
            {
                if (*n == (*d).m_name)
                    target = *so2;
            }
        }
        const uint field = codeOffset + (*d).m_offset;
        patch32(code, field, (int32)(target - (field + 4)));
    }

    memory = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
    {
        memory = NULL;
        return NULL;
    }
    memorySize = size;
    for (uint i = 0; i < size; i++)
        ((uint8*)memory)[i] = code[i];
    return reinterpret_cast<NativeFunction>(memory);
}
#endif

// The edge values of a width: the bit patterns of 0, 1, 2, -1, the minimum, the
// maximum and their neighbours (the bits above the width are zero here)
static void getEdges(uint bits, uint64* values, uint& count)
{
    const uint64 mask = (bits == 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
    const uint64 minimum = (uint64)1 << (bits - 1);
    const uint64 maximum = minimum - 1;
    const uint64 edges[] = {
        0, 1, 2, 3, 7, mask, mask - 1, minimum, minimum + 1, maximum,
        maximum - 1, 0x5555555555555555ULL & mask, 0xAAAAAAAAAAAAAAAAULL & mask,
        (minimum >> 1) | 1, 0x0F0F0F0F0F0F0F0FULL & mask
    };
    count = sizeof(edges) / sizeof(edges[0]);
    for (uint i = 0; i < count; i++)
        values[i] = edges[i] & mask;
}

// The pseudo random numbers of the tests (xorshift64, fixed seed)
static uint64 gRandom = 0x9E3779B97F4A7C15ULL;
#ifdef LOWERING_INTEGER_NATIVE
// The state of the native A64 runs (the registers at 0x1000 + 8 * id)
static uint64 gArm64State[0x2000];
#endif
static uint64 nextRandom()
{
    gRandom ^= gRandom << 13;
    gRandom ^= gRandom >> 7;
    gRandom ^= gRandom << 17;
    return gRandom;
}

class TestObjectTestSemanticLoweringInteger : public cTestObject {
public:
    TestObjectTestSemanticLoweringInteger() : m_failures(0), m_cases(0),
                                              m_arm64Cases(0), m_skipped(0) {}

    virtual void test()
    {
        testRejections();
#ifdef LOWERING_INTEGER_NATIVE
        testDivisions();
        testBitOperations();
        testHighSignedUnsigned();
        testAddCarry();
        testArm64AddCarry();
#endif
        cout << "integer: " << m_cases << " cases native==interpreter";
        if (m_skipped != 0)
            cout << ", " << m_skipped << " skipped (no executable memory)";
        cout << endl;
#ifdef LOWERING_INTEGER_NATIVE
        cout << "integer arm64 carry: " << m_arm64Cases
             << " cases native==interpreter" << endl;
#endif
        TESTS_ASSERT(m_failures == 0);
        TESTS_ASSERT(m_cases > 0 || m_skipped > 0);
        cout << "Semantic lowering integer tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    // The lowering refuses what it doesn't cover, naming the operation
    void testRejections()
    {
        const SemanticType wide = SemanticType::integer(128);
        const SemanticType narrow = SemanticType::integer(8);
        const SemanticOperand x1 = SemanticOperand::registerOperand(SRC0, wide);
        const SemanticOperand x2 = SemanticOperand::registerOperand(SRC1, wide);
        const SemanticOperand x3 = SemanticOperand::registerOperand(DEST, wide);
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_AMD_64);
        block.beginGuestInstruction(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ROW_ADDRESS), 4);
        block.append(SemanticInstruction(SemanticOperation::OP_DIVIDE_UNSIGNED,
                                         wide, x3, x1, x2));
        TESTS_EXCEPTION(lowerBlockOnly(block));

        // The high product of bytes isn't a native multiplication
        const SemanticOperand b1 = SemanticOperand::registerOperand(SRC0, narrow);
        const SemanticOperand b2 = SemanticOperand::registerOperand(SRC1, narrow);
        const SemanticOperand b3 = SemanticOperand::registerOperand(DEST, narrow);
        SemanticBlock bytes(OpcodeSubsystems::DISASSEMBLER_AMD_64);
        bytes.beginGuestInstruction(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ROW_ADDRESS), 4);
        bytes.append(SemanticInstruction(
                SemanticOperation::OP_MULTIPLY_HIGH_SIGNED_UNSIGNED,
                narrow, b3, b1, b2));
        TESTS_EXCEPTION(lowerBlockOnly(bytes));
    }

    // Lower a block with the AMD64 lowering (the rejections are thrown)
    static void lowerBlockOnly(const SemanticBlock& block)
    {
        IntegerTestRegisterMap registerMap;
        AMD64Lowering lowering;
        AMD64Lowering::LoweredBlock lowered;
        lowering.lowerBlock(block, registerMap, lowered);
    }

#ifdef LOWERING_INTEGER_NATIVE
    // Build the block of one operation of 'bits' bits with its guest address
    static void buildBlock(SemanticBlock& block, SemanticOperation::Operation operation,
                           uint bits, SemanticOperation::Behavior behavior,
                           bool isBinary)
    {
        const SemanticType type = SemanticType::integer(bits);
        const SemanticOperand x1 = SemanticOperand::registerOperand(SRC0, type);
        const SemanticOperand x2 = SemanticOperand::registerOperand(SRC1, type);
        const SemanticOperand x3 = SemanticOperand::registerOperand(DEST, type);
        block.beginGuestInstruction(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ROW_ADDRESS), 4);
        SemanticInstruction instruction = isBinary ?
            SemanticInstruction(operation, type, x3, x1, x2) :
            SemanticInstruction(operation, type, x3, x1);
        instruction.setBehavior(behavior);
        block.append(instruction);
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ROW_ADDRESS + 4));
    }

    // Run the block natively and in the interpreter on the sources 'values'
    // (with random bits above the width) and compare. 'isBinary' tells whether
    // the second source is used. Return false on a difference
    bool compareCase(const char* name, SemanticBlock& block, NativeFunction function,
                     uint bits, const uint64* values, bool isBinary,
                     uint64 carry = 0)
    {
        const uint64 mask = (bits == 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
        uint64 inputs[2];
        for (uint i = 0; i < 2; i++)
        {
            const uint64 noise = (bits == 64) ? 0 : (nextRandom() & ~mask);
            inputs[i] = values[i] | noise;
        }

        IntegerNoMemory memory;
        SemanticInterpreter interpreter(getGuest(), memory);
        uint64 state[0x400];
        memset(state, 0, sizeof(state));
        interpreter.setRegister(SRC0, SemanticValue(inputs[0]));
        state[(0x400 + SRC0 * 8) / 8] = inputs[0];
        if (isBinary)
        {
            interpreter.setRegister(SRC1, SemanticValue(inputs[1]));
            state[(0x400 + SRC1 * 8) / 8] = inputs[1];
        }
        // The carry of OP_ADD_CARRY (ignored by the other blocks)
        interpreter.setRegister(CARRY, SemanticValue(carry & 1));
        state[(0x400 + CARRY * 8) / 8] = carry & 1;
        const SemanticInterpreter::ExitReason reason = interpreter.run(block);
        function(state, NULL);
        m_cases++;

        const uint64 pc = state[0x100 / 8];
        const uint64 exitReason = state[0x110 / 8];
        const bool isTrap = (exitReason & 0xFF) == AMD64Lowering::EXIT_TRAP;
        const uint64 faultKind = (exitReason >> AMD64Lowering::EXIT_TRAP_KIND_SHIFT) & 0xFF;
        if (reason == SemanticInterpreter::EXIT_TRAP)
        {
            // A divide error: the same trap, the same guest instruction
            if (!isTrap || (faultKind != SemanticOperation::TRAP_FAULT) ||
                (interpreter.getTrapKind() != SemanticOperation::TRAP_FAULT) ||
                (pc != ROW_ADDRESS) || (interpreter.getNextAddress() != ROW_ADDRESS))
            {
                reportCase(name, bits, inputs, "the trap differs from the interpreter");
                return false;
            }
            return true;
        }
        if ((reason != SemanticInterpreter::EXIT_FALL_THROUGH) || isTrap)
        {
            reportCase(name, bits, inputs, "the exit differs from the interpreter");
            return false;
        }
        if (pc != interpreter.getNextAddress())
        {
            reportCase(name, bits, inputs, "the next address differs from the interpreter");
            return false;
        }
        const uint64 expected = interpreter.getRegister(DEST).getWord(0) & mask;
        const uint64 actual = state[(0x400 + DEST * 8) / 8] & mask;
        if (expected != actual)
        {
            cout << "  " << name << " native " << HEXQWORD(actual)
                 << " interpreter " << HEXQWORD(expected) << endl;
            reportCase(name, bits, inputs, "the result differs from the interpreter");
            return false;
        }
        return true;
    }

    void reportCase(const char* name, uint bits, const uint64* inputs,
                    const char* message)
    {
        fprintf(stderr, "TestSemanticLoweringInteger: %s<%u> [%s]: %s "
                "(sources 0x%llx 0x%llx)\n", name, (unsigned)bits, __FILE__, message,
                (unsigned long long)inputs[0], (unsigned long long)inputs[1]);
        m_failures++;
    }

    // Lower and link a block; run every pair (binary) or every value (unary) of
    // the edge values of its width, plus random values
    void runBlock(const char* name, SemanticOperation::Operation operation,
                  uint bits, SemanticOperation::Behavior behavior, bool isBinary)
    {
        // The interpreter runs the block as built; the lowering runs the
        // block after the legalizer, as the translator does
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_AMD_64);
        buildBlock(block, operation, bits, behavior, isBinary);
        SemanticBlock legalized(OpcodeSubsystems::DISASSEMBLER_AMD_64);
        buildBlock(legalized, operation, bits, behavior, isBinary);
        SemanticLegalizer legalizer(getGuest(), getTarget());
        legalizer.legalize(legalized);
        IntegerTestRegisterMap registerMap;
        void* memory = NULL;
        uint memorySize = 0;
        NativeFunction function = buildNative(legalized, registerMap,
                                              memory, memorySize);
        if (function == NULL)
        {
            m_skipped++;
            return;
        }
        uint64 edges[32];
        uint count = 0;
        getEdges(bits, edges, count);
        for (uint i = 0; i < count; i++)
        {
            for (uint j = 0; j < (isBinary ? count : 1); j++)
            {
                const uint64 values[2] = { edges[i], edges[j] };
                compareCase(name, block, function, bits, values, isBinary);
            }
        }
        const uint randoms = 16;
        for (uint i = 0; i < randoms; i++)
        {
            const uint64 values[2] = { nextRandom(), nextRandom() };
            compareCase(name, block, function, bits, values, isBinary);
        }
        munmap(memory, memorySize);
    }

    // The divisions and remainders, every width, every behaviour
    void testDivisions()
    {
        static const struct {
            SemanticOperation::Operation m_operation;
            const char* m_name;
        } operations[] = {
            { SemanticOperation::OP_DIVIDE_UNSIGNED, "divide_unsigned" },
            { SemanticOperation::OP_DIVIDE_SIGNED, "divide_signed" },
            { SemanticOperation::OP_REMAINDER_UNSIGNED, "remainder_unsigned" },
            { SemanticOperation::OP_REMAINDER_SIGNED, "remainder_signed" }
        };
        static const SemanticOperation::Behavior behaviors[] = {
            SemanticOperation::BEHAVIOR_DIVIDE_FAULT,
            SemanticOperation::BEHAVIOR_DIVIDE_ZERO,
            SemanticOperation::BEHAVIOR_DIVIDE_ALL_ONES
        };
        static const uint widths[] = { 8, 16, 32, 64 };
        for (uint o = 0; o < sizeof(operations) / sizeof(operations[0]); o++)
        {
            for (uint b = 0; b < sizeof(behaviors) / sizeof(behaviors[0]); b++)
            {
                for (uint w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
                {
                    runBlock(operations[o].m_name, operations[o].m_operation,
                             widths[w], behaviors[b], true);
                }
            }
        }
    }

    // The unary bit operations, every width they have
    void testBitOperations()
    {
        static const struct {
            SemanticOperation::Operation m_operation;
            const char* m_name;
            uint m_minimum;
        } operations[] = {
            { SemanticOperation::OP_POPULATION_COUNT, "population_count", 8 },
            { SemanticOperation::OP_COUNT_LEADING_ZEROS, "count_leading_zeros", 8 },
            { SemanticOperation::OP_COUNT_TRAILING_ZEROS, "count_trailing_zeros", 8 },
            { SemanticOperation::OP_BYTE_SWAP, "byte_swap", 8 },
            { SemanticOperation::OP_BIT_REVERSE, "bit_reverse", 8 }
        };
        static const uint widths[] = { 8, 16, 32, 64 };
        for (uint o = 0; o < sizeof(operations) / sizeof(operations[0]); o++)
        {
            for (uint w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
            {
                if (widths[w] < operations[o].m_minimum)
                    continue;
                runBlock(operations[o].m_name, operations[o].m_operation,
                         widths[w], SemanticOperation::BEHAVIOR_DEFAULT, false);
            }
        }
    }

    // The high half of the signed by unsigned product, 16 to 64 bits
    void testHighSignedUnsigned()
    {
        static const uint widths[] = { 16, 32, 64 };
        for (uint w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
        {
            runBlock("multiply_high_signed_unsigned",
                     SemanticOperation::OP_MULTIPLY_HIGH_SIGNED_UNSIGNED,
                     widths[w], SemanticOperation::BEHAVIOR_DEFAULT, true);
        }
    }

    // The block of OP_ADD_CARRY: DEST = SRC0 + SRC1 + carry. The carry is the
    // register CARRY (a boolean) when constantCarry is negative, else the
    // constant 0 or 1
    static void buildCarryBlock(SemanticBlock& block, uint bits,
                                int constantCarry)
    {
        const SemanticType type = SemanticType::integer(bits);
        const SemanticType boolean = SemanticType::integer(1);
        const SemanticOperand x1 = SemanticOperand::registerOperand(SRC0, type);
        const SemanticOperand x2 = SemanticOperand::registerOperand(SRC1, type);
        const SemanticOperand x3 = SemanticOperand::registerOperand(DEST, type);
        const SemanticOperand carry = (constantCarry < 0) ?
            SemanticOperand::registerOperand(CARRY, boolean) :
            SemanticOperand::immediate(constantCarry, boolean);
        block.beginGuestInstruction(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ROW_ADDRESS), 4);
        block.append(SemanticInstruction(SemanticOperation::OP_ADD_CARRY,
                                         type, x3, x1, x2, carry));
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ROW_ADDRESS + 4));
    }

    // Lower and link the OP_ADD_CARRY of 'bits' bits, and run it on every pair
    // of the edge values with carry 0 and 1 (or the constant), plus random
    // values. A register carry is swept; a constant one is the constant
    void runAddCarry(uint bits, int constantCarry)
    {
        const char* name = (constantCarry < 0) ? "add_carry" :
                           (constantCarry == 0) ? "add_carry_constant_0" :
                                                  "add_carry_constant_1";
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_AMD_64);
        buildCarryBlock(block, bits, constantCarry);
        SemanticBlock legalized(OpcodeSubsystems::DISASSEMBLER_AMD_64);
        buildCarryBlock(legalized, bits, constantCarry);
        SemanticLegalizer legalizer(getGuest(), getTarget());
        legalizer.legalize(legalized);
        IntegerTestRegisterMap registerMap;
        void* memory = NULL;
        uint memorySize = 0;
        NativeFunction function = buildNative(legalized, registerMap,
                                              memory, memorySize);
        if (function == NULL)
        {
            m_skipped++;
            return;
        }
        uint64 edges[32];
        uint count = 0;
        getEdges(bits, edges, count);
        const uint carries = (constantCarry < 0) ? 2 : 1;
        for (uint i = 0; i < count; i++)
        {
            for (uint j = 0; j < count; j++)
            {
                for (uint c = 0; c < carries; c++)
                {
                    const uint64 values[2] = { edges[i], edges[j] };
                    const uint64 carry = (constantCarry < 0) ? c : constantCarry;
                    compareCase(name, block, function, bits, values, true, carry);
                }
            }
        }
        for (uint i = 0; i < 16; i++)
        {
            const uint64 values[2] = { nextRandom(), nextRandom() };
            compareCase(name, block, function, bits, values, true, nextRandom() & 1);
        }
        munmap(memory, memorySize);
    }

    // OP_ADD_CARRY in every width (8, 16, 32, 64), with the carry in a
    // register and as the constants 0 and 1
    void testAddCarry()
    {
        static const uint widths[] = { 8, 16, 32, 64 };
        for (uint w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
        {
            runAddCarry(widths[w], -1);
            runAddCarry(widths[w], 0);
            runAddCarry(widths[w], 1);
        }
    }

    // The A64 words of adc, adcs, sbc, sbcs, ngc and ngcs, 32 and 64 bit
    // (ngc and ngcs are sbc and sbcs with the zero register as first source)
    struct Arm64CarryRow {
        uint32 m_word;
        const char* m_name;
    };

    static const Arm64CarryRow* getArm64CarryRows(uint& count)
    {
        static const Arm64CarryRow rows[] = {
            { 0x9a020020, "adc x0, x1, x2" },
            { 0x1a020020, "adc w0, w1, w2" },
            { 0xba020020, "adcs x0, x1, x2" },
            { 0x3a020020, "adcs w0, w1, w2" },
            { 0xda020020, "sbc x0, x1, x2" },
            { 0x5a020020, "sbc w0, w1, w2" },
            { 0xfa020020, "sbcs x0, x1, x2" },
            { 0x7a020020, "sbcs w0, w1, w2" },
            { 0xda0203e0, "ngc x0, x2" },
            { 0x5a0203e0, "ngc w0, w2" },
            { 0xfa0203e0, "ngcs x0, x2" },
            { 0x7a0203e0, "ngcs w0, w2" }
        };
        count = sizeof(rows) / sizeof(rows[0]);
        return rows;
    }

    // Lift a row, legalize it for AMD64, lower and link it, then run the
    // edge values of the two sources with the carry (and random flags)
    void testArm64AddCarry()
    {
        uint count = 0;
        const Arm64CarryRow* rows = getArm64CarryRows(count);
        for (uint r = 0; r < count; r++)
            runArm64Row(rows[r]);
    }

    void runArm64Row(const Arm64CarryRow& row)
    {
        ArchitectureDescriptionPtr arm(new Arm64Architecture(128, 128));
        ArmSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE,
                                         arm);
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        ArmOpcodePtr opcode = ArmStreamDisassembler::decode(
                ArmInstructionSet::ARM_A64, row.m_word, 4, ROW_ADDRESS,
                ArmFeatureSet::all());
        if (opcode.getPointer() == NULL)
        {
            reportArm64(row, "the word doesn't decode");
            return;
        }
        lifter.lift(OpcodePtr((Opcode*)opcode.getPointer(),
                              SMARTPTR_DESTRUCT_NONE), block);
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ROW_ADDRESS + 4));
        bool isAddCarry = false;
        const cList<SemanticInstruction>& instructions = block.getInstructions();
        for (cList<SemanticInstruction>::iterator i = instructions.begin();
             i != instructions.end(); i++)
        {
            isAddCarry = isAddCarry ||
                ((*i).getOperation() == SemanticOperation::OP_ADD_CARRY);
        }
        if (!isAddCarry)
        {
            reportArm64(row, "the lifted block has no OP_ADD_CARRY");
            return;
        }

        // The legalized copy, lowered and linked. The interpreter runs the
        // lifted block as it is
        SemanticBlock legalized(block);
        try
        {
            SemanticLegalizer legalizer(arm, getTarget());
            legalizer.legalize(legalized);
        } catch (cException&)
        {
            reportArm64(row, "legalize() refused the row");
            return;
        }
        Arm64TestRegisterMap registerMap;
        void* memory = NULL;
        uint memorySize = 0;
        NativeFunction function = NULL;
        try
        {
            function = buildNative(legalized, registerMap, memory, memorySize);
        } catch (cException&)
        {
            reportArm64(row, "the lowering refused the legalized row");
            return;
        }
        if (function == NULL)
        {
            m_skipped++;
            return;
        }

        const uint64 edges[] = {
            0, 1, 2, ~(uint64)0, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
            0x80000000ULL, 0x7FFFFFFFULL, 0xFFFFFFFFULL, 0x100000000ULL
        };
        const uint edgeCount = sizeof(edges) / sizeof(edges[0]);
        for (uint i = 0; i < edgeCount; i++)
        {
            for (uint j = 0; j < edgeCount; j++)
            {
                for (uint c = 0; c < 2; c++)
                    compareArm64Case(row, arm, block, function, edges[i],
                                     edges[j], c);
            }
        }
        for (uint i = 0; i < 16; i++)
        {
            compareArm64Case(row, arm, block, function, nextRandom(),
                             nextRandom(), nextRandom() & 1);
        }
        munmap(memory, memorySize);
    }

    // One case of an A64 row: X0..X30 random except the sources x1 and x2, the
    // flags N, Z, V random and C the carry. Return false on a difference
    bool compareArm64Case(const Arm64CarryRow& row,
                          const ArchitectureDescriptionPtr& arm,
                          const SemanticBlock& block, NativeFunction function,
                          uint64 x1, uint64 x2, uint carry)
    {
        uint64 xs[31];
        for (uint i = 0; i < 31; i++)
            xs[i] = nextRandom();
        xs[1] = x1;
        xs[2] = x2;
        const uint64 random = nextRandom();
        const uint64 flags[4] = {
            random & 1,          // N
            (random >> 1) & 1,   // Z
            carry & 1,           // C
            (random >> 2) & 1    // V
        };
        const uint kinds[4] = {
            armdis::ARM64_REG_N, armdis::ARM64_REG_Z,
            armdis::ARM64_REG_C, armdis::ARM64_REG_V
        };

        IntegerNoMemory memory;
        SemanticInterpreter interpreter(arm, memory);
        for (uint i = 0; i < 31; i++)
            interpreter.setRegister(armdis::ARM64_REG_X0 + i, SemanticValue(xs[i]));
        for (uint f = 0; f < 4; f++)
            interpreter.setRegister(kinds[f], SemanticValue(flags[f]));
        const SemanticInterpreter::ExitReason reason = interpreter.run(block);

        uint64* state = gArm64State;
        memset(state, 0, sizeof(gArm64State));
        for (uint i = 0; i < 31; i++)
            state[(0x1000 + (armdis::ARM64_REG_X0 + i) * 8) / 8] = xs[i];
        for (uint f = 0; f < 4; f++)
            state[(0x1000 + kinds[f] * 8) / 8] = flags[f];
        function(state, NULL);
        m_arm64Cases++;

        if ((reason != SemanticInterpreter::EXIT_FALL_THROUGH) ||
            (state[0x110 / 8] != 0x1000))
        {
            reportArm64Case(row, x1, x2, carry, "the exit differs from the interpreter");
            return false;
        }
        for (uint i = 0; i < 31; i++)
        {
            const uint64 expected = interpreter.getRegister(
                                armdis::ARM64_REG_X0 + i).getWord(0);
            const uint64 actual = state[(0x1000 + (armdis::ARM64_REG_X0 + i) * 8) / 8];
            if (expected != actual)
            {
                reportArm64Case(row, x1, x2, carry, "a register differs from the interpreter");
                return false;
            }
        }
        for (uint f = 0; f < 4; f++)
        {
            const uint64 expected = interpreter.getRegister(kinds[f]).getWord(0) & 1;
            const uint64 actual = state[(0x1000 + kinds[f] * 8) / 8] & 1;
            if (expected != actual)
            {
                reportArm64Case(row, x1, x2, carry, "a flag differs from the interpreter");
                return false;
            }
        }
        return true;
    }

    void reportArm64(const Arm64CarryRow& row, const char* message)
    {
        fprintf(stderr, "TestSemanticLoweringInteger: %s [%s]: %s\n",
                row.m_name, __FILE__, message);
        m_failures++;
    }

    void reportArm64Case(const Arm64CarryRow& row, uint64 x1, uint64 x2,
                         uint carry, const char* message)
    {
        fprintf(stderr, "TestSemanticLoweringInteger: %s [%s]: %s "
                "(x1 0x%llx x2 0x%llx carry %u)\n", row.m_name, __FILE__,
                message, (unsigned long long)x1, (unsigned long long)x2,
                (unsigned)carry);
        m_failures++;
    }
#endif

    // The failures of the comparisons
    uint m_failures;
    // The comparisons made
    uint m_cases;
    // The comparisons of the A64 rows
    uint m_arm64Cases;
    // The blocks which couldn't run (no executable memory)
    uint m_skipped;
};

} // namespace

// Instance test object
TestObjectTestSemanticLoweringInteger g_globalTestSemanticLoweringInteger;
