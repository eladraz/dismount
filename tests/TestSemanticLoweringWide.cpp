/*
 * TestSemanticLoweringWide.cpp
 *
 * Tests of the lowering of the wide values (AMD64LoweringWide.cpp): the 128
 * bit values (16 byte slots), the bit moves of 1 to 64 bits and the bit
 * casts. Each synthetic block is:
 *   - lowered for AMD64 on every host (it must lower);
 *   - on x86-64 Linux, run natively and compared with the SemanticInterpreter
 *     on random guest states: the guest registers x0-x31, the 128 bit vector
 *     registers v0-v31, the guest memory and the next address. The state
 *     outside the compared registers and the block's temporaries is filled
 *     with a canary, which must still be there afterwards (a stray write
 *     beyond a slot is caught). Every case must compare every trial (no
 *     vacuous pass).
 * The lowering must refuse the operations it doesn't implement (the negative
 * cases of Part 3), each for its reason: a 128 bit constant, a vector zero
 * extension, an EXTRACT of 128 bits past bit 0, an INSERT which is not a whole
 * byte into 128 bits, the logic of a constant, a broadcast of 128 bit lanes.
 * The positive cases of Part 1 (F10 to F17) cover EXTRACT, INSERT, BROADCAST,
 * the 128 bit logic and the float moves at the edges: the first, the middle
 * and the last lanes, the bit positions which straddle a qword, the positions
 * past the value.
 *
 * The harness is copied from TestSemanticLowering.cpp (Part 2d), whose
 * helpers are private to that file.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include <string.h>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#define WIDE_TEST_NATIVE 1
#endif
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "xStl/exceptions.h"
#include "xStl/stream/ioStream.h"
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
#include "dismount/semantics/SemanticInterpreter.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"

using namespace riscvdis;

// The helpers of this file are local to it (no clash with the other test
// files, e.g. NativeTestMemory of TestSemanticLowering.cpp)
namespace {

typedef SemanticOperation Op;

// The guest of the tests: RV64 with 128 bit vector registers (VLEN = 128)
static ArchitectureDescriptionPtr getGuest()
{
    return ArchitectureDescriptionPtr(new RiscvArchitecture(
                RiscvInstructionSet::RISCV_RV64, 128, false));
}

// The guest state of the tests (see WideRegisterMap)
static const uint STATE_WORDS = 0x400;           // 8 KiB of state
static const uint64 CANARY = 0xC0A1C0A1C0A1C0A1ULL;
static const uint TEMP_BASE = 0x200;             // temporaries, 16 bytes each
static const uint VECTOR_BASE = 0x1800;          // v0-v31, 16 bytes each
static const uint TEMP_MAX = 0x1000;             // (the temporaries fit below)

// The guest state of the tests: x0-x31 at 0 (8 bytes each), the program
// counter at 0x100, the exit reason at 0x110, the temporaries from 0x200 (16
// bytes each), the other registers at 0x1000 + 8 * id, the vector registers
// v0-v31 at 0x1800 (16 bytes each)
class WideRegisterMap : public SemanticRegisterMap {
public:
    virtual bool getTargetRegister(uint, uint&) const { return false; }
    virtual uint getStateOffset(uint guestRegister) const
    {
        if ((guestRegister >= RISCV_REG_X0) && (guestRegister <= RISCV_REG_X31))
            return (guestRegister - RISCV_REG_X0) * 8;
        if ((guestRegister >= RISCV_REG_V0) && (guestRegister <= RISCV_REG_V31))
            return VECTOR_BASE + (guestRegister - RISCV_REG_V0) * 16;
        return 0x1000 + guestRegister * 8;
    }
    virtual uint getStateRegister() const { return ia32dis::AMD64_REG_R15; }
    virtual uint getTemporaryOffset(uint temporary) const
    {
        return TEMP_BASE + temporary * 16;
    }
    virtual uint getMemoryBaseRegister() const { return ia32dis::AMD64_REG_R14; }
    virtual uint getProgramCounterOffset() const { return 0x100; }
    virtual uint getExitReasonOffset() const { return 0x110; }
};

// The stack of the first pass binary, which the lowering doesn't use
class WideLoweringStack : public StackInterface {
public:
    WideLoweringStack(FirstPassBinaryPtr& binary) :
        StackInterface(*binary, true, 4) {}
    RegisterAllocationTable m_registers;
    virtual uint getStackType() { return 4; }
    virtual uint getRegisterSize() { return 4; }
    virtual RegisterAllocationTable& getRegistersTable() { return m_registers; }
    virtual const RegisterAllocationTable& getRegistersTable() const { return m_registers; }
    virtual StackLocation replaceRegisterToStackVariable(StackLocation source) { return source; }
};

// The width of each temporary of the block being built, by its index: the
// canary check needs the width of a slot (see newTemp())
static uint gTempWidth[TEMP_MAX / 16];

// A new temporary of the block, its width recorded
static SemanticOperand newTemp(SemanticBlock& block, const SemanticType& type)
{
    const SemanticOperand temp = block.newTemporary(type);
    if (temp.getIndex() < sizeof(gTempWidth) / sizeof(gTempWidth[0]))
        gTempWidth[temp.getIndex()] = type.getTotalBits();
    return temp;
}

// The guest memory of the cases: 64 KiB at NATIVE_GUEST_BASE, for the
// interpreter and (through the bias in r14) for the lowered code. The 128 bit
// values are observed at NATIVE_STORE_ADDRESS (see observeWide())
static const uint64 NATIVE_GUEST_BASE = 0x10000000ULL;
enum { NATIVE_MEMORY_SIZE = 0x10000 };
static const uint64 NATIVE_STORE_ADDRESS = NATIVE_GUEST_BASE + 0x300;

// An integer operand of a width
static SemanticType integerType(uint bits) { return SemanticType::integer(bits); }
static SemanticOperand guestX(uint index, uint bits)
{
    return SemanticOperand::registerOperand(RISCV_REG_X0 + index, integerType(bits));
}
// The vector register v<index>, 128 bits
static SemanticOperand vectorV(uint index)
{
    return SemanticOperand::registerOperand(RISCV_REG_V0 + index, integerType(128));
}
static SemanticOperand guestAddress(uint64 address)
{
    return SemanticOperand::guestAddress(address, integerType(64));
}

// Lower a block for AMD64 into a binary (no stubs): the lowering must accept
// the block, or throw
static void lowerOnly(const SemanticBlock& block)
{
    FirstPassBinaryPtr binary(new FirstPassBinary(
                        OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
    binary->createNewBlockWithoutChange(0,
            StackInterfacePtr(new WideLoweringStack(binary)));
    binary->changeBasicBlock(0);
    AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                        binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);
    SemanticLoweringPtr lowering = SemanticLoweringFactory::getLowering(
                                OpcodeSubsystems::DISASSEMBLER_AMD_64);
    WideRegisterMap registerMap;
    lowering->lower(block, registerMap, *assembler);
}

// The chain of an A64 guest instruction: decoded, lifted, legalized for AMD64
// (SemanticLegalizer) and lowered (lowering only, on every host). Return false,
// naming the stage, when a stage refuses the instruction
static bool lowerA64Chain(const char* name, uint32 encoding)
{
    using namespace armdis;
    ArchitectureDescriptionPtr guest(new Arm64Architecture(128, 128));
    ArchitectureDescriptionPtr target(ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_AMD_64));
    ArmOpcodePtr decoded = ArmStreamDisassembler::decode(ArmInstructionSet::ARM_A64,
                    encoding, 4, 0x1000, ArmFeatureSet::all());
    if (decoded.getPointer() == NULL)
    {
        fprintf(stderr, "TestSemanticLoweringWide: %s: not decoded\n", name);
        return false;
    }
    OpcodePtr opcode((Opcode*)decoded.getPointer(), SMARTPTR_DESTRUCT_NONE);
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
    const char* stage = "lift";
    try
    {
        ArmSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE, guest);
        lifter.lift(opcode, block);
        block.setFallThroughAddress(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, 0x1004));
        stage = "legalize";
        SemanticLegalizer legalizer(guest, target);
        legalizer.legalize(block);
        stage = "lower";
        lowerOnly(block);
    } catch (cException& exception)
    {
        cSArray<char> text = cString(exception.getMessage()).getASCIIstring();
        fprintf(stderr, "TestSemanticLoweringWide: %s: %s: %s\n", name, stage,
                text.getBuffer());
        return false;
    }
    return true;
}

// Append "x10 = zero extend 'value'; x11 = sign extend 'value'" for a value of
// 1, 8, 16, 32 or 64 bits. Another width is stored whole (observed as a
// 128 bit zero extension), which the narrow operations don't lower
static void observe(SemanticBlock& block, const SemanticOperand& value, uint bits)
{
    const SemanticType wide = integerType(64);
    if ((bits == 1) || (bits == 8) || (bits == 16) || (bits == 32) || (bits == 64))
    {
        SemanticOperand zero = newTemp(block, wide);
        block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, zero, value));
        block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(10, 64), zero));
        SemanticOperand sign = newTemp(block, wide);
        block.append(SemanticInstruction(Op::OP_SIGN_EXTEND, wide, sign, value));
        block.append(SemanticInstruction(Op::OP_MOVE, wide, guestX(11, 64), sign));
        return;
    }
    const SemanticType wide128 = integerType(128);
    SemanticOperand extended = newTemp(block, wide128);
    block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide128, extended, value));
    block.append(SemanticInstruction(Op::OP_STORE, wide128, SemanticOperand(),
                 guestAddress(NATIVE_STORE_ADDRESS), extended));
}

// Append the store of a 128 bit value to the observed address
static void observeWide(SemanticBlock& block, const SemanticOperand& value)
{
    block.append(SemanticInstruction(Op::OP_STORE, integerType(128), SemanticOperand(),
                 guestAddress(NATIVE_STORE_ADDRESS), value));
}

// Append a 128 bit value built from two guest registers (x_a low, x_b high)
static SemanticOperand concatX(SemanticBlock& block, uint a, uint b)
{
    const SemanticOperand wide = newTemp(block, integerType(128));
    block.append(SemanticInstruction(Op::OP_CONCAT, integerType(128), wide,
                 guestX(a, 64), guestX(b, 64)));
    return wide;
}

// Append a 128 bit value loaded from the address of guest register 'a'
static SemanticOperand loadWide(SemanticBlock& block, uint a)
{
    const SemanticOperand wide = newTemp(block, integerType(128));
    block.append(SemanticInstruction(Op::OP_LOAD, integerType(128), wide, guestX(a, 64)));
    return wide;
}

// Append the EXTRACT of a scalar of 'bits' bits at the constant 'position' (a
// lane index for a vector source)
static SemanticOperand extractX(SemanticBlock& block, const SemanticOperand& source,
                                uint bits, uint64 position)
{
    const SemanticOperand part = newTemp(block, integerType(bits));
    block.append(SemanticInstruction(Op::OP_EXTRACT, integerType(bits), part, source,
                 SemanticOperand::immediate((int64)position, integerType(64))));
    return part;
}

// Append the INSERT of 'inserted' into 'base' at the constant 'position' (a
// lane index for a vector 'type')
static SemanticOperand insertX(SemanticBlock& block, const SemanticOperand& base,
                               const SemanticOperand& inserted,
                               const SemanticType& type, uint64 position)
{
    const SemanticOperand out = newTemp(block, type);
    block.append(SemanticInstruction(Op::OP_INSERT, type, out, base, inserted,
                 SemanticOperand::immediate((int64)position, integerType(64))));
    return out;
}

// The floating point type of 'bits' bits (16, 32 or 64)
static SemanticType floatOf(uint bits)
{
    return SemanticType::floating((bits == 16) ? SemanticType::FLOAT_HALF :
                                  ((bits == 32) ? SemanticType::FLOAT_SINGLE :
                                                  SemanticType::FLOAT_DOUBLE));
}

// Append the bit cast of 'value' to an integer of its size
static SemanticOperand bitsOf(SemanticBlock& block, const SemanticOperand& value,
                              uint bits)
{
    const SemanticOperand out = newTemp(block, integerType(bits));
    block.append(SemanticInstruction(Op::OP_BITCAST, integerType(bits), out, value));
    return out;
}

#ifdef WIDE_TEST_NATIVE
// ----------------------------------------------------------------------
// The native comparisons
// ----------------------------------------------------------------------

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
static uint64 gWideRandom = 0x9E3779B97F4A7C15ULL;
static uint64 wideRandom()
{
    gWideRandom ^= gWideRandom << 13;
    gWideRandom ^= gWideRandom >> 7;
    gWideRandom ^= gWideRandom << 17;
    return gWideRandom;
}

// A register value: an edge value most of the time, or a pointer into the
// guest memory (the blocks which access it)
static uint64 wideValue(bool isPointer)
{
    static const uint64 edges[] = {
        0, 1, 2, 7, 8, 15, 16, 31, 32, 63, 64, 0x7F, 0x80, 0xFF, 0x100,
        0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFFULL, 0x80000000ULL, 0xFFFFFFFFULL,
        0x100000000ULL, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
        ~(uint64)0, 0x12345678ABCDEF01ULL };
    const uint64 r = wideRandom();
    if (isPointer)
        return NATIVE_GUEST_BASE + 0x4000 + ((r % 0x2000) & ~(uint64)15);
    if (r % 3 == 0)
        return wideRandom();
    return edges[(r >> 8) % (sizeof(edges) / sizeof(edges[0]))];
}

typedef void (*NativeFunction)(void* state, void* memoryBias);

// Lower a block with the entry and exit stubs of the runtime around it,
// resolve the labels, and return the code
static bool buildNative(const SemanticBlock& block, cBuffer& code)
{
    FirstPassBinaryPtr binary(new FirstPassBinary(
                        OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
    binary->createNewBlockWithoutChange(0,
            StackInterfacePtr(new WideLoweringStack(binary)));
    binary->changeBasicBlock(0);
    AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                        binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);
    const char* const entry =
        "push rbx\npush rbp\npush r12\npush r13\npush r14\npush r15\n"
        "mov r15, rdi\nmov r14, rsi\n";
    {
        cStringerStreamPtr stream = assembler->getAssembler();
        *stream << cString(entry);
        IA32AssemblerStream* assembly =
                    dynamic_cast<IA32AssemblerStream*>(stream.getPointer());
        if (assembly == NULL)
            return false;
        assembly->assemble();
    }

    SemanticLoweringPtr lowering = SemanticLoweringFactory::getLowering(
                                OpcodeSubsystems::DISASSEMBLER_AMD_64);
    WideRegisterMap registerMap;
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
        cStringerStreamPtr stream = assembler->getAssembler();
        *stream << cString(text);
        IA32AssemblerStream* assembly =
                    dynamic_cast<IA32AssemblerStream*>(stream.getPointer());
        if (assembly == NULL)
            return false;
        assembly->assemble();
    }
    names.append(AMD64Lowering::getExitLabel());
    positions.append(binary->getCurrentBlockData().getSize());
    {
        cStringerStreamPtr stream = assembler->getAssembler();
        *stream << cString("pop r15\npop r14\npop r13\npop r12\npop rbp\npop rbx\nret\n");
        IA32AssemblerStream* assembly =
                    dynamic_cast<IA32AssemblerStream*>(stream.getPointer());
        if (assembly == NULL)
            return false;
        assembly->assemble();
    }

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
#endif

// The trials of each native case: the states of the random inputs
static const uint TRIALS = 8;

class TestObjectTestSemanticLoweringWide : public cTestObject {
public:
    TestObjectTestSemanticLoweringWide() :
        m_failures(0), m_cases(0), m_comparisons(0), m_skipped(0) {}

    virtual void test()
    {
        const uint failuresBefore = m_failures;
        runPositiveCases();
        runNegativeCases();
        runLegalizedCases();
        cout << "wide: " << m_cases << " synthetic cases native==interpreter, "
             << m_comparisons << " comparisons";
        if (m_skipped != 0)
            cout << ", " << m_skipped << " skipped (no executable memory)";
        cout << endl;
        TESTS_ASSERT(m_failures == failuresBefore);
#ifdef WIDE_TEST_NATIVE
        // No vacuous pass: every case compared every trial
        TESTS_ASSERT(m_skipped != 0 || m_comparisons == m_cases * TRIALS);
        TESTS_ASSERT(m_cases >= 200 || m_skipped != 0);
#endif
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    // Part 1: the synthetic blocks. Each one must lower, and runs natively
    // on x86-64 Linux
    void runPositiveCases()
    {
        // The guest register pairs the blocks read: (low, high) for the
        // concatenations, and the address registers of the memory accesses
        static const uint pairs[][2] = {
            { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 5 },
            { 5, 6 }, { 6, 7 }, { 7, 8 }, { 8, 9 } };
        const uint pairsCount = sizeof(pairs) / sizeof(pairs[0]);
        static const uint widths[] = { 8, 16, 32, 64 };
        const uint widthsCount = sizeof(widths) / sizeof(widths[0]);
        const SemanticType wide = integerType(128);

        // F1: zero extension of a narrow value to 128 bits: a guest register
        // (or a constant), and a boolean
        static const uint extendWidths[] = { 1, 8, 16, 32, 64 };
        for (uint w = 0; w < sizeof(extendWidths) / sizeof(extendWidths[0]); w++)
        {
            const uint bits = extendWidths[w];
            for (uint p = 0; p < pairsCount; p++)
            {
                for (uint form = 0; form < 2; form++)
                {
                    if ((bits == 1) && (form == 1))
                        continue;
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    SemanticOperand source = guestX(pairs[p][0], bits);
                    if (bits == 1)
                    {
                        source = newTemp(block, integerType(1));
                        block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(1),
                                     source, guestX(pairs[p][0], 64)));
                    } else if (form == 1)
                    {
                        source = SemanticOperand::immediate(
                                (int64)wideValue(false), integerType(bits));
                    }
                    SemanticOperand z = newTemp(block, wide);
                    block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, z, source));
                    observeWide(block, z);
                    SemanticOperand low = newTemp(block, integerType(64));
                    block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(64), low, z));
                    observe(block, low, 64);
                    runCase("zero extend", block, false);
                }
            }
        }

        // F2: truncation of a 128 bit value (of a concatenation, a load) to
        // narrow widths, including the non-standard 2 and 7 bits
        static const uint truncations[] = { 1, 2, 7, 8, 16, 32, 64 };
        for (uint t = 0; t < sizeof(truncations) / sizeof(truncations[0]); t++)
        {
            const uint bits = truncations[t];
            for (uint p = 0; p < pairsCount; p++)
            {
                for (uint form = 0; form < 2; form++)
                {
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    const SemanticOperand value = (form == 0) ?
                        concatX(block, pairs[p][0], pairs[p][1]) :
                        loadWide(block, pairs[p][0]);
                    SemanticOperand narrow = newTemp(block, integerType(bits));
                    block.append(SemanticInstruction(Op::OP_TRUNCATE,
                                 integerType(bits), narrow, value));
                    observe(block, narrow, bits);
                    observeWide(block, value);
                    runCase("truncate", block, form == 1);
                }
            }
        }

        // F3: moves of 128 bit values, between temporaries
        for (uint p = 0; p < pairsCount; p++)
        {
            for (uint form = 0; form < 2; form++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand value = (form == 0) ?
                    concatX(block, pairs[p][0], pairs[p][1]) :
                    loadWide(block, pairs[p][0]);
                SemanticOperand copy = newTemp(block, wide);
                block.append(SemanticInstruction(Op::OP_MOVE, wide, copy, value));
                SemanticOperand again = newTemp(block, wide);
                block.append(SemanticInstruction(Op::OP_MOVE, wide, again, copy));
                observeWide(block, again);
                runCase("move", block, form == 1);
            }
        }

        // F4: loads of 128 bits from a register, from a register plus an
        // offset and from a constant address
        for (uint p = 0; p < pairsCount; p++)
        {
            for (uint form = 0; form < 3; form++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                SemanticOperand address = guestX(pairs[p][0], 64);
                if (form == 1)
                {
                    address = newTemp(block, integerType(64));
                    block.append(SemanticInstruction(Op::OP_ADD, integerType(64), address,
                                 guestX(pairs[p][0], 64),
                                 SemanticOperand::immediate(16, integerType(64))));
                }
                if (form == 2)
                    address = guestAddress(NATIVE_GUEST_BASE + 0x100 + 16 * p);
                const SemanticOperand loaded = newTemp(block, wide);
                block.append(SemanticInstruction(Op::OP_LOAD, wide, loaded, address));
                observeWide(block, loaded);
                SemanticOperand low = newTemp(block, integerType(64));
                block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(64), low, loaded));
                observe(block, low, 64);
                runCase("load", block, true);
            }
        }

        // F5: stores of 128 bits, of a concatenation and of a load, to a
        // register address and to a constant address
        for (uint p = 0; p < pairsCount; p++)
        {
            for (uint form = 0; form < 4; form++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand value = (form & 1) ?
                    loadWide(block, pairs[p][1]) :
                    concatX(block, pairs[p][0], pairs[p][1]);
                const SemanticOperand address = (form & 2) ?
                    guestAddress(NATIVE_GUEST_BASE + 0x200 + 16 * p) :
                    guestX(pairs[p][0], 64);
                block.append(SemanticInstruction(Op::OP_STORE, wide, SemanticOperand(),
                             address, value));
                runCase("store", block, true);
            }
        }

        // F6: concatenations: the parts of the same width, of widths which
        // differ, of 1 bit (the boolean parts) and with a constant low part
        static const uint joins[][2] = {
            { 1, 1 }, { 1, 7 }, { 7, 1 }, { 8, 8 }, { 8, 16 }, { 16, 8 },
            { 16, 16 }, { 32, 8 }, { 32, 32 }, { 64, 64 } };
        for (uint j = 0; j < sizeof(joins) / sizeof(joins[0]); j++)
        {
            const uint lowBits = joins[j][0];
            const uint highBits = joins[j][1];
            for (uint p = 0; p < pairsCount; p++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                SemanticOperand low = guestX(pairs[p][0], lowBits);
                SemanticOperand high = guestX(pairs[p][1], highBits);
                if (lowBits == 1)
                {
                    low = newTemp(block, integerType(1));
                    block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(1),
                                 low, guestX(pairs[p][0], 64)));
                }
                if (highBits == 1)
                {
                    high = newTemp(block, integerType(1));
                    block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(1),
                                 high, guestX(pairs[p][1], 64)));
                }
                const uint total = lowBits + highBits;
                const SemanticType totalType = integerType(total);
                SemanticOperand joined = newTemp(block, totalType);
                block.append(SemanticInstruction(Op::OP_CONCAT, totalType, joined, low, high));
                observe(block, joined, total);
                runCase("concat", block, false);
            }
        }
        // (a constant low part: 32 and 64 bits)
        for (uint p = 0; p < pairsCount; p++)
        {
            for (uint w = 0; w < 2; w++)
            {
                const uint lowBits = (w == 0) ? 32 : 64;
                const uint total = lowBits + ((w == 0) ? 32 : 64);
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                SemanticOperand joined = newTemp(block, integerType(total));
                block.append(SemanticInstruction(Op::OP_CONCAT, integerType(total), joined,
                             SemanticOperand::immediate((int64)wideValue(false),
                                                        integerType(lowBits)),
                             guestX(pairs[p][1], lowBits)));
                observe(block, joined, total);
                runCase("concat constant", block, false);
            }
        }

        // F7: bit casts of 128 bits to vectors and to floating point, and back;
        // of 64, 32, 16, 8 and 1 bits; of constants
        for (uint p = 0; p < pairsCount; p++)
        {
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand value = concatX(block, pairs[p][0], pairs[p][1]);
                const SemanticOperand lanes = newTemp(block,
                        SemanticType::vector(integerType(32), 4));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::vector(integerType(32), 4), lanes, value));
                const SemanticOperand back = newTemp(block, wide);
                block.append(SemanticInstruction(Op::OP_BITCAST, wide, back, lanes));
                observeWide(block, back);
                runCase("bitcast 128", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand value = concatX(block, pairs[p][0], pairs[p][1]);
                const SemanticOperand quad = newTemp(block,
                        SemanticType::floating(SemanticType::FLOAT_QUAD));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::floating(SemanticType::FLOAT_QUAD), quad, value));
                const SemanticOperand back = newTemp(block, wide);
                block.append(SemanticInstruction(Op::OP_BITCAST, wide, back, quad));
                observeWide(block, back);
                runCase("bitcast quad", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand loaded = loadWide(block, pairs[p][0]);
                const SemanticOperand bytes = newTemp(block,
                        SemanticType::vector(integerType(8), 16));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::vector(integerType(8), 16), bytes, loaded));
                const SemanticOperand halves = newTemp(block,
                        SemanticType::vector(integerType(64), 2));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::vector(integerType(64), 2), halves, bytes));
                observeWide(block, halves);
                runCase("bitcast 128 vectors", block, true);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand bits64 = newTemp(block, integerType(64));
                block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(64),
                             bits64, concatX(block, pairs[p][0], pairs[p][1])));
                const SemanticOperand asDouble = newTemp(block,
                        SemanticType::floating(SemanticType::FLOAT_DOUBLE));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::floating(SemanticType::FLOAT_DOUBLE),
                             asDouble, bits64));
                const SemanticOperand again = newTemp(block, integerType(64));
                block.append(SemanticInstruction(Op::OP_BITCAST, integerType(64),
                             again, asDouble));
                observe(block, again, 64);
                runCase("bitcast 64", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand bits32 = newTemp(block, integerType(32));
                block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(32),
                             bits32, guestX(pairs[p][0], 32)));
                const SemanticOperand asSingle = newTemp(block,
                        SemanticType::floating(SemanticType::FLOAT_SINGLE));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::floating(SemanticType::FLOAT_SINGLE),
                             asSingle, bits32));
                const SemanticOperand again = newTemp(block, integerType(32));
                block.append(SemanticInstruction(Op::OP_BITCAST, integerType(32),
                             again, asSingle));
                observe(block, again, 32);
                runCase("bitcast 32", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand bits16 = newTemp(block, integerType(16));
                block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(16),
                             bits16, guestX(pairs[p][0], 16)));
                const SemanticOperand asHalf = newTemp(block,
                        SemanticType::floating(SemanticType::FLOAT_HALF));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::floating(SemanticType::FLOAT_HALF),
                             asHalf, bits16));
                const SemanticOperand again = newTemp(block, integerType(16));
                block.append(SemanticInstruction(Op::OP_BITCAST, integerType(16),
                             again, asHalf));
                observe(block, again, 16);
                runCase("bitcast 16", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand bits8 = newTemp(block, integerType(8));
                block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(8),
                             bits8, guestX(pairs[p][0], 8)));
                const SemanticOperand asFloat8 = newTemp(block,
                        SemanticType::floating(SemanticType::FLOAT_E5M2));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::floating(SemanticType::FLOAT_E5M2),
                             asFloat8, bits8));
                const SemanticOperand again = newTemp(block, integerType(8));
                block.append(SemanticInstruction(Op::OP_BITCAST, integerType(8),
                             again, asFloat8));
                observe(block, again, 8);
                runCase("bitcast 8", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand flag = newTemp(block, integerType(1));
                block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(1),
                             flag, guestX(pairs[p][0], 64)));
                const SemanticOperand again = newTemp(block, integerType(1));
                block.append(SemanticInstruction(Op::OP_BITCAST, integerType(1),
                             again, flag));
                observe(block, again, 1);
                runCase("bitcast 1", block, false);
            }
            {
                // A constant: 64, 16 and 8 bits, to floating point and back
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand asDouble = newTemp(block,
                        SemanticType::floating(SemanticType::FLOAT_DOUBLE));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::floating(SemanticType::FLOAT_DOUBLE), asDouble,
                             SemanticOperand::immediate((int64)wideValue(false),
                                                        integerType(64))));
                const SemanticOperand again = newTemp(block, integerType(64));
                block.append(SemanticInstruction(Op::OP_BITCAST, integerType(64),
                             again, asDouble));
                observe(block, again, 64);
                const SemanticOperand asHalf = newTemp(block,
                        SemanticType::floating(SemanticType::FLOAT_HALF));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::floating(SemanticType::FLOAT_HALF), asHalf,
                             SemanticOperand::immediate((int64)wideValue(false),
                                                        integerType(16))));
                const SemanticOperand asByte = newTemp(block, integerType(8));
                block.append(SemanticInstruction(Op::OP_BITCAST, integerType(8), asByte,
                             SemanticOperand::immediate((int64)wideValue(false),
                                                        integerType(8))));
                observe(block, asByte, 8);
                runCase("bitcast constant", block, false);
            }
        }

        // F8: a chain: concatenate, truncate to the width, extend to 128 bits
        for (uint w = 0; w < widthsCount; w++)
        {
            const uint bits = widths[w];
            for (uint p = 0; p < pairsCount; p++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticType total = integerType(2 * bits);
                SemanticOperand joined = newTemp(block, total);
                block.append(SemanticInstruction(Op::OP_CONCAT, total, joined,
                             guestX(pairs[p][0], bits), guestX(pairs[p][1], bits)));
                SemanticOperand narrow = newTemp(block, integerType(bits));
                block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(bits),
                             narrow, joined));
                SemanticOperand extended = newTemp(block, wide);
                block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, extended, narrow));
                observeWide(block, extended);
                observe(block, narrow, bits);
                runCase("chain", block, false);
            }
        }

        // F9: the 128 bit vector registers: moves, extensions, truncations,
        // loads and stores, the registers are 16 bytes apart in the state
        for (uint p = 0; p < 8; p++)
        {
            const uint dst = (p * 5 + 3) % 32;
            const uint src = (p * 7 + 1) % 32;
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                block.append(SemanticInstruction(Op::OP_MOVE, wide, vectorV(dst), vectorV(src)));
                observeWide(block, vectorV(dst));
                runCase("vector move", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand temp = concatX(block, pairs[p % pairsCount][0],
                                                     pairs[p % pairsCount][1]);
                block.append(SemanticInstruction(Op::OP_MOVE, wide, vectorV(dst), temp));
                const SemanticOperand copy = newTemp(block, wide);
                block.append(SemanticInstruction(Op::OP_MOVE, wide, copy, vectorV(dst)));
                observeWide(block, copy);
                runCase("temporary to vector", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, wide, vectorV(dst),
                             guestX(pairs[p % pairsCount][0], 64)));
                observeWide(block, vectorV(dst));
                runCase("vector zero extend", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(64),
                             guestX(10, 64), vectorV(src)));
                observe(block, guestX(10, 64), 64);
                runCase("vector truncate", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                block.append(SemanticInstruction(Op::OP_LOAD, wide, vectorV(dst),
                             guestX(pairs[p % pairsCount][0], 64)));
                observeWide(block, vectorV(dst));
                runCase("vector load", block, true);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                block.append(SemanticInstruction(Op::OP_STORE, wide, SemanticOperand(),
                             guestAddress(NATIVE_GUEST_BASE + 0x280 + 16 * p),
                             vectorV(src)));
                runCase("vector store", block, false);
            }
        }

        // F10: EXTRACT of a scalar from a 128 bit value (a concatenation, a
        // load) at constant positions: the bytes, the qword edges, the last
        // bits, and past the value (the bits above it are zero)
        static const uint extractWidths[] = { 1, 8, 16, 32, 64 };
        static const uint extractPositions[] = {
            0, 1, 3, 7, 8, 12, 15, 16, 31, 32, 33, 47, 56, 63, 64, 65, 70,
            96, 100, 120, 127, 128, 130 };
        for (uint w = 0; w < sizeof(extractWidths) / sizeof(extractWidths[0]); w++)
        {
            for (uint q = 0; q < sizeof(extractPositions) / sizeof(extractPositions[0]); q++)
            {
                for (uint form = 0; form < 2; form++)
                {
                    const uint pair = (form == 0) ? 0 : 3;
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    const SemanticOperand value = (form == 0) ?
                        concatX(block, pairs[pair][0], pairs[pair][1]) :
                        loadWide(block, pairs[pair][0]);
                    observe(block, extractX(block, value, extractWidths[w],
                                            extractPositions[q]), extractWidths[w]);
                    runCase("extract 128", block, form == 1);
                }
            }
        }

        // F11: EXTRACT of a scalar from a narrow value (of 8 to 64 bits),
        // positions past its width give zero
        static const uint narrowSources[] = { 8, 16, 32, 64 };
        static const uint narrowResults[] = { 1, 8, 16, 32 };
        static const uint narrowPositions[] = { 0, 1, 3, 7, 8, 15, 16, 31, 32, 33, 63 };
        for (uint s = 0; s < sizeof(narrowSources) / sizeof(narrowSources[0]); s++)
        {
            for (uint r = 0; r < sizeof(narrowResults) / sizeof(narrowResults[0]); r++)
            {
                for (uint q = 0; q < sizeof(narrowPositions) / sizeof(narrowPositions[0]); q++)
                {
                    for (uint p = 0; p < 2; p++)
                    {
                        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                        const SemanticOperand source = guestX(pairs[(p == 0) ? 0 : 4][0],
                                                              narrowSources[s]);
                        observe(block, extractX(block, source, narrowResults[r],
                                                narrowPositions[q]), narrowResults[r]);
                        runCase("extract narrow", block, false);
                    }
                }
            }
        }

        // F12: EXTRACT of a lane: a vector source, a scalar result. The first,
        // the middle and the last lanes, of the 128 and the 64 bit vectors
        static const uint laneShapes[][2] = {
            { 8, 16 }, { 16, 8 }, { 32, 4 }, { 64, 2 } };
        static const uint halfShapes[][2] = { { 8, 8 }, { 16, 4 }, { 32, 2 } };
        for (uint s = 0; s < sizeof(laneShapes) / sizeof(laneShapes[0]); s++)
        {
            const uint elementBits = laneShapes[s][0];
            const uint lanes = laneShapes[s][1];
            for (uint lane = 0; lane < 3; lane++)
            {
                const uint index = (lane == 0) ? 0 : ((lane == 1) ? lanes / 2 : lanes - 1);
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand vector = newTemp(block,
                        SemanticType::vector(integerType(elementBits), lanes));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::vector(integerType(elementBits), lanes), vector,
                             concatX(block, pairs[1][0], pairs[1][1])));
                observe(block, extractX(block, vector, elementBits, index), elementBits);
                runCase("extract lane", block, false);
            }
        }
        for (uint s = 0; s < sizeof(halfShapes) / sizeof(halfShapes[0]); s++)
        {
            const uint elementBits = halfShapes[s][0];
            const uint lanes = halfShapes[s][1];
            for (uint lane = 0; lane < lanes; lane++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand vector = newTemp(block,
                        SemanticType::vector(integerType(elementBits), lanes));
                block.append(SemanticInstruction(Op::OP_BITCAST,
                             SemanticType::vector(integerType(elementBits), lanes), vector,
                             guestX(pairs[2][0], 64)));
                observe(block, extractX(block, vector, elementBits, lane), elementBits);
                runCase("extract half lane", block, false);
            }
        }

        // F13: INSERT into a narrow value (of 8 to 64 bits) at constant bit
        // positions: the field inside the value, the top bit, and a position
        // past the value (the value is kept)
        static const uint insertDestinations[] = { 8, 16, 32, 64 };
        static const uint insertWidths[] = { 1, 8, 16, 32 };
        static const uint insertPositions[] = { 0, 1, 4, 7, 8, 15, 16, 31, 32, 33, 63 };
        for (uint d = 0; d < sizeof(insertDestinations) / sizeof(insertDestinations[0]); d++)
        {
            const uint destBits = insertDestinations[d];
            for (uint i = 0; i < sizeof(insertWidths) / sizeof(insertWidths[0]); i++)
            {
                const uint insertedBits = insertWidths[i];
                if (insertedBits > destBits)
                    continue;
                for (uint q = 0; q < sizeof(insertPositions) / sizeof(insertPositions[0]); q++)
                {
                    for (uint p = 0; p < 2; p++)
                    {
                        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                        const uint pair = (p == 0) ? 0 : 5;
                        const SemanticOperand base = guestX(pairs[pair][0], destBits);
                        SemanticOperand inserted = guestX(pairs[pair][1], insertedBits);
                        if (insertedBits == 1)
                        {
                            inserted = newTemp(block, integerType(1));
                            block.append(SemanticInstruction(Op::OP_TRUNCATE, integerType(1),
                                         inserted, guestX(pairs[pair][1], 64)));
                        }
                        const SemanticOperand out = insertX(block, base, inserted,
                                                            integerType(destBits),
                                                            insertPositions[q]);
                        observe(block, out, destBits);
                        runCase("insert narrow", block, false);
                    }
                }
            }
        }

        // F14: INSERT into 128 bits at byte positions (a scalar of 8 to 64
        // bits, or a 128 bit value), and into the lanes of a vector
        static const uint wholeWidths[] = { 8, 16, 32, 64 };
        static const uint widePositions[][9] = {
            { 0, 8, 16, 32, 56, 64, 96, 112, 120 },
            { 0, 8, 16, 32, 56, 64, 96, 112, 0 },
            { 0, 8, 32, 56, 64, 96, 0, 0, 0 },
            { 0, 8, 56, 64, 0, 0, 0, 0, 0 } };
        static const uint widePositionsCount[] = { 9, 8, 6, 4 };
        for (uint w = 0; w < 4; w++)
        {
            for (uint q = 0; q < widePositionsCount[w]; q++)
            {
                for (uint p = 0; p < 2; p++)
                {
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    const uint pair = (p == 0) ? 0 : 2;
                    const SemanticOperand base = concatX(block, pairs[pair][0], pairs[pair][1]);
                    const SemanticOperand out = insertX(block, base,
                            guestX(pairs[pair + 4][0], wholeWidths[w]), wide,
                            widePositions[w][q]);
                    observeWide(block, out);
                    runCase("insert 128", block, false);
                }
            }
        }
        for (uint p = 0; p < 2; p++)
        {
            const uint pair = (p == 0) ? 0 : 2;
            {
                // A byte past the value: the value is kept
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand base = concatX(block, pairs[pair][0], pairs[pair][1]);
                observeWide(block, insertX(block, base, guestX(pairs[pair + 4][0], 8),
                                           wide, 128));
                runCase("insert 128 past the end", block, false);
            }
            {
                // A 128 bit value at bit 0: it replaces the value
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand base = concatX(block, pairs[pair][0], pairs[pair][1]);
                const SemanticOperand whole = concatX(block, pairs[pair + 4][0],
                                                      pairs[pair + 4][1]);
                observeWide(block, insertX(block, base, whole, wide, 0));
                runCase("insert 128 whole", block, false);
            }
        }
        // The lanes of the 128 bit vectors: the first, the middle and the last
        for (uint s = 0; s < sizeof(laneShapes) / sizeof(laneShapes[0]); s++)
        {
            const uint elementBits = laneShapes[s][0];
            const uint lanes = laneShapes[s][1];
            for (uint lane = 0; lane < 3; lane++)
            {
                const uint index = (lane == 0) ? 0 : ((lane == 1) ? lanes / 2 : lanes - 1);
                for (uint p = 0; p < 2; p++)
                {
                    const uint pair = (p == 0) ? 0 : 2;
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    const SemanticType vectorType = SemanticType::vector(
                                    integerType(elementBits), lanes);
                    const SemanticOperand base = newTemp(block, vectorType);
                    block.append(SemanticInstruction(Op::OP_BITCAST, vectorType, base,
                                 concatX(block, pairs[pair][0], pairs[pair][1])));
                    const SemanticOperand out = insertX(block, base,
                            guestX(pairs[pair + 4][0], elementBits), vectorType, index);
                    observeWide(block, bitsOf(block, out, 128));
                    runCase("insert lane", block, false);
                }
            }
        }

        // F15: BROADCAST of a scalar (a register, a constant, a float) into
        // the lanes of vectors of 128 and 64 bits
        static const uint broadcastShapes[][2] = {
            { 8, 16 }, { 16, 8 }, { 32, 4 }, { 64, 2 } };
        for (uint s = 0; s < sizeof(broadcastShapes) / sizeof(broadcastShapes[0]); s++)
        {
            const uint elementBits = broadcastShapes[s][0];
            const uint lanes = broadcastShapes[s][1];
            const SemanticType vectorType = SemanticType::vector(integerType(elementBits), lanes);
            for (uint w = 0; w < 4; w++)
            {
                for (uint p = 0; p < 2; p++)
                {
                    // (a scalar narrower than the lanes is zero extended)
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    const SemanticOperand scalar = guestX(pairs[(p == 0) ? 0 : 6][0],
                                                          wholeWidths[w]);
                    const SemanticOperand vector = newTemp(block, vectorType);
                    block.append(SemanticInstruction(Op::OP_BROADCAST, vectorType, vector,
                                 scalar));
                    observeWide(block, bitsOf(block, vector, 128));
                    runCase("broadcast", block, false);
                }
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand vector = newTemp(block, vectorType);
                block.append(SemanticInstruction(Op::OP_BROADCAST, vectorType, vector,
                             SemanticOperand::immediate((int64)wideValue(false),
                                                        integerType(elementBits))));
                observeWide(block, bitsOf(block, vector, 128));
                runCase("broadcast constant", block, false);
            }
        }
        static const uint halfBroadcastShapes[][2] = {
            { 8, 8 }, { 16, 4 }, { 32, 2 } };
        for (uint s = 0; s < sizeof(halfBroadcastShapes) / sizeof(halfBroadcastShapes[0]); s++)
        {
            const uint elementBits = halfBroadcastShapes[s][0];
            const uint lanes = halfBroadcastShapes[s][1];
            const SemanticType vectorType = SemanticType::vector(integerType(elementBits), lanes);
            for (uint w = 0; w < 4; w++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand vector = newTemp(block, vectorType);
                block.append(SemanticInstruction(Op::OP_BROADCAST, vectorType, vector,
                             guestX(pairs[w][0], wholeWidths[w])));
                observe(block, bitsOf(block, vector, 64), 64);
                runCase("broadcast 64", block, false);
            }
        }
        {
            // The lanes of a float: single, double and half
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticType singles = SemanticType::vector(floatOf(32), 4);
            const SemanticOperand scalar = newTemp(block, floatOf(32));
            block.append(SemanticInstruction(Op::OP_BITCAST, floatOf(32), scalar,
                         guestX(3, 32)));
            const SemanticOperand vector = newTemp(block, singles);
            block.append(SemanticInstruction(Op::OP_BROADCAST, singles, vector, scalar));
            observeWide(block, bitsOf(block, vector, 128));
            runCase("broadcast single", block, false);
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticType doubles = SemanticType::vector(floatOf(64), 2);
            const SemanticOperand scalar = newTemp(block, floatOf(64));
            block.append(SemanticInstruction(Op::OP_BITCAST, floatOf(64), scalar,
                         guestX(4, 64)));
            const SemanticOperand vector = newTemp(block, doubles);
            block.append(SemanticInstruction(Op::OP_BROADCAST, doubles, vector, scalar));
            observeWide(block, bitsOf(block, vector, 128));
            runCase("broadcast double", block, false);
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticType halves = SemanticType::vector(floatOf(16), 8);
            const SemanticOperand scalar = newTemp(block, floatOf(16));
            block.append(SemanticInstruction(Op::OP_BITCAST, floatOf(16), scalar,
                         guestX(5, 16)));
            const SemanticOperand vector = newTemp(block, halves);
            block.append(SemanticInstruction(Op::OP_BROADCAST, halves, vector, scalar));
            observeWide(block, bitsOf(block, vector, 128));
            runCase("broadcast half", block, false);
        }

        // F16: AND, OR, XOR, AND_NOT and NOT of 128 bits: temporaries, and
        // the vector registers (the destination is one of the sources)
        static const SemanticOperation::Operation logicOperations[] = {
            Op::OP_AND, Op::OP_OR, Op::OP_XOR, Op::OP_AND_NOT };
        for (uint o = 0; o < 4; o++)
        {
            for (uint p = 0; p < pairsCount; p++)
            {
                for (uint form = 0; form < 2; form++)
                {
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    const SemanticOperand a = (form == 0) ?
                        concatX(block, pairs[p][0], pairs[p][1]) : loadWide(block, pairs[p][0]);
                    const SemanticOperand b = concatX(block, pairs[(p + 3) % pairsCount][0],
                                                      pairs[(p + 3) % pairsCount][1]);
                    const SemanticOperand out = newTemp(block, wide);
                    block.append(SemanticInstruction(logicOperations[o], wide, out, a, b));
                    observeWide(block, out);
                    runCase("logic", block, form == 1);
                }
            }
            for (uint p = 0; p < 8; p++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const uint dst = (p * 5 + 3) % 32;
                const uint src = (p * 7 + 1) % 32;
                block.append(SemanticInstruction(logicOperations[o], wide, vectorV(dst),
                             vectorV(dst), vectorV(src)));
                observeWide(block, vectorV(dst));
                runCase("logic vector", block, false);
            }
        }
        for (uint p = 0; p < pairsCount; p++)
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticOperand a = concatX(block, pairs[p][0], pairs[p][1]);
            const SemanticOperand out = newTemp(block, wide);
            block.append(SemanticInstruction(Op::OP_NOT, wide, out, a));
            observeWide(block, out);
            runCase("not", block, false);
        }
        for (uint p = 0; p < 8; p++)
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const uint dst = (p * 5 + 3) % 32;
            const uint src = (p * 7 + 1) % 32;
            block.append(SemanticInstruction(Op::OP_NOT, wide, vectorV(dst), vectorV(src)));
            observeWide(block, vectorV(dst));
            runCase("not vector", block, false);
        }

        // F17: floats of 16, 32 and 64 bits: moved, loaded and stored as
        // their bits (the integer of the same size)
        static const uint floatWidths[] = { 16, 32, 64 };
        for (uint f = 0; f < 3; f++)
        {
            const uint bits = floatWidths[f];
            for (uint p = 0; p < 2; p++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand value = newTemp(block, floatOf(bits));
                block.append(SemanticInstruction(Op::OP_BITCAST, floatOf(bits), value,
                             guestX(pairs[(p == 0) ? 0 : 3][0], bits)));
                const SemanticOperand copy = newTemp(block, floatOf(bits));
                block.append(SemanticInstruction(Op::OP_MOVE, floatOf(bits), copy, value));
                observe(block, bitsOf(block, copy, bits), bits);
                runCase("float move", block, false);
            }
            for (uint p = 0; p < pairsCount; p++)
            {
                for (uint form = 0; form < 2; form++)
                {
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    const SemanticOperand address = (form == 0) ?
                        guestX(pairs[p][0], 64) :
                        guestAddress(NATIVE_GUEST_BASE + 0x100 + 16 * p);
                    const SemanticOperand loaded = newTemp(block, floatOf(bits));
                    block.append(SemanticInstruction(Op::OP_LOAD, floatOf(bits), loaded,
                                 address));
                    observe(block, bitsOf(block, loaded, bits), bits);
                    runCase("float load", block, true);
                }
            }
            for (uint p = 0; p < pairsCount; p++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand value = newTemp(block, floatOf(bits));
                block.append(SemanticInstruction(Op::OP_BITCAST, floatOf(bits), value,
                             guestX(pairs[p][1], bits)));
                block.append(SemanticInstruction(Op::OP_STORE, floatOf(bits),
                             SemanticOperand(), guestX(pairs[p][0], 64), value));
                runCase("float store", block, true);
            }
        }

        // F18: 128 bit constants (an immediate is sign extended to 128 bits:
        // its high qword is all ones when it is negative): moved, stored, the
        // operands of the logic, the base of an INSERT (the lanes 0 and 1) and
        // a vector constant; the logic of 64 bit vectors and of a constant
        static const int64 constants[] = { 5, -5, 0x7FFFFFFFFFFFFFFFLL, -1 };
        for (uint c = 0; c < sizeof(constants) / sizeof(constants[0]); c++)
        {
            const int64 value = constants[c];
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand out = newTemp(block, wide);
                block.append(SemanticInstruction(Op::OP_MOVE, wide, out,
                             SemanticOperand::immediate(value, wide)));
                observeWide(block, out);
                runCase("constant move", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                block.append(SemanticInstruction(Op::OP_STORE, wide, SemanticOperand(),
                             guestX(pairs[c][0], 64), SemanticOperand::immediate(value, wide)));
                runCase("constant store", block, true);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                block.append(SemanticInstruction(Op::OP_STORE, wide, SemanticOperand(),
                             guestAddress(NATIVE_GUEST_BASE + 0x2C0 + 16 * c),
                             SemanticOperand::immediate(value, wide)));
                runCase("constant store to a constant address", block, false);
            }
            for (uint o = 0; o < 5; o++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticOperand a = concatX(block, pairs[c][0], pairs[c][1]);
                const SemanticOperand out = newTemp(block, wide);
                const SemanticOperand constant = SemanticOperand::immediate(value, wide);
                if (o == 4)
                {
                    block.append(SemanticInstruction(Op::OP_NOT, wide, out, constant));
                } else
                {
                    const SemanticOperation::Operation operations[] = {
                        Op::OP_AND, Op::OP_OR, Op::OP_XOR, Op::OP_AND_NOT };
                    // (the constant is the first operand of the AND_NOT)
                    block.append(SemanticInstruction(operations[o], wide, out,
                                 (o == 3) ? constant : a, (o == 3) ? a : constant));
                }
                observeWide(block, out);
                runCase("constant logic", block, false);
            }
            {
                // An INSERT into the constant: the lanes 0 and 1 of 64 bits
                for (uint lane = 0; lane < 2; lane++)
                {
                    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                    const SemanticOperand out = insertX(block,
                            SemanticOperand::immediate(value, wide),
                            guestX(pairs[c][1], 64), wide, lane * 64);
                    observeWide(block, out);
                    runCase("insert into a constant", block, false);
                }
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticType lanes = SemanticType::vector(integerType(64), 2);
                const SemanticOperand out = insertX(block,
                        SemanticOperand::immediate(value, lanes),
                        guestX(pairs[c][1], 64), lanes, 1);
                observeWide(block, bitsOf(block, out, 128));
                runCase("insert lane 1 into a constant", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                observe(block, extractX(block, SemanticOperand::immediate(value, wide),
                                        64, 64), 64);
                observe(block, extractX(block, SemanticOperand::immediate(value, wide),
                                        8, 120), 8);
                runCase("extract from a constant", block, false);
            }
        }
        // Lane 0 and the lane 1 of 64 bits from a register, the 128 bit vector
        // of the register, the 64 bit vectors of the logic
        for (uint p = 0; p < pairsCount; p++)
        {
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticType lanes = SemanticType::vector(integerType(64), 2);
                const SemanticOperand out = insertX(block,
                        SemanticOperand::immediate(0, lanes), guestX(pairs[p][0], 64),
                        lanes, 0);
                observeWide(block, bitsOf(block, out, 128));
                runCase("insert lane 0 into a zero vector", block, false);
            }
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
                const SemanticType halves = SemanticType::vector(integerType(8), 8);
                const SemanticOperation::Operation operations[] = {
                    Op::OP_AND, Op::OP_OR, Op::OP_XOR, Op::OP_AND_NOT };
                const SemanticOperand a = newTemp(block, halves);
                block.append(SemanticInstruction(Op::OP_BITCAST, halves, a,
                             guestX(pairs[p][0], 64)));
                const SemanticOperand b = newTemp(block, halves);
                block.append(SemanticInstruction(Op::OP_BITCAST, halves, b,
                             guestX(pairs[(p + 2) % pairsCount][0], 64)));
                const SemanticOperand out = newTemp(block, halves);
                block.append(SemanticInstruction(operations[p % 4], halves, out, a, b));
                observe(block, bitsOf(block, out, 64), 64);
                runCase("logic of 64 bit vectors", block, false);
            }
        }
    }

    // Part 3: the operations the lowering refuses (lowering only, on every host)
    void runNegativeCases()
    {
        const SemanticType wide = integerType(128);
        // The vector zero extensions: the interpreter extends the lanes
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticOperand v = newTemp(block, SemanticType::vector(integerType(32), 2));
            block.append(SemanticInstruction(Op::OP_BITCAST,
                         SemanticType::vector(integerType(32), 2), v, guestX(2, 64)));
            const SemanticOperand w = newTemp(block, SemanticType::vector(integerType(64), 2));
            block.append(SemanticInstruction(Op::OP_ZERO_EXTEND,
                         SemanticType::vector(integerType(64), 2), w, v));
            refused("ZERO_EXTEND v2i32 to v2i64", block, "the zero extension of vectors");
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticOperand v = newTemp(block, SemanticType::vector(integerType(16), 4));
            block.append(SemanticInstruction(Op::OP_BITCAST,
                         SemanticType::vector(integerType(16), 4), v, guestX(2, 64)));
            const SemanticOperand w = newTemp(block, SemanticType::vector(integerType(32), 4));
            block.append(SemanticInstruction(Op::OP_ZERO_EXTEND,
                         SemanticType::vector(integerType(32), 4), w, v));
            refused("ZERO_EXTEND v4i16 to v4i32", block, "the zero extension of vectors");
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticOperand v = newTemp(block, SemanticType::vector(integerType(8), 8));
            block.append(SemanticInstruction(Op::OP_BITCAST,
                         SemanticType::vector(integerType(8), 8), v, guestX(2, 64)));
            const SemanticOperand w = newTemp(block, SemanticType::vector(integerType(16), 8));
            block.append(SemanticInstruction(Op::OP_ZERO_EXTEND,
                         SemanticType::vector(integerType(16), 8), w, v));
            refused("ZERO_EXTEND v8i8 to v8i16", block, "the zero extension of vectors");
        }
        // EXTRACT past bit 0 of a 128 bit value: not implemented
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticOperand value = concatX(block, 1, 2);
            const SemanticOperand part = newTemp(block, wide);
            block.append(SemanticInstruction(Op::OP_EXTRACT, wide, part, value,
                         SemanticOperand::immediate(8, integerType(64))));
            refused("EXTRACT of 128 bits at bit 8", block,
                    "an extraction of 128 bits at a position other than 0");
        }
        // INSERT of 8 bits at bit 4 of 128 bits: not a whole byte
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticOperand value = concatX(block, 1, 2);
            const SemanticOperand out = newTemp(block, wide);
            block.append(SemanticInstruction(Op::OP_INSERT, wide, out, value,
                         guestX(3, 8), SemanticOperand::immediate(4, integerType(64))));
            refused("INSERT of 8 bits at bit 4 of 128 bits", block,
                    "whole bytes only");
        }
        // A broadcast of 128 bit lanes: not implemented
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticOperand a = concatX(block, 1, 2);
            const SemanticOperand out = newTemp(block,
                    SemanticType::vector(integerType(128), 1));
            block.append(SemanticInstruction(Op::OP_BROADCAST,
                         SemanticType::vector(integerType(128), 1), out, a));
            refused("BROADCAST of 128 bit lanes", block,
                    "the broadcast of these lanes");
        }
    }

    // The guest instructions which the legalizer and the lowering must take:
    // the logic of the 16 byte and the 8 byte vectors, the element insert
    // and the element move (the A64 lifter emits INSERT of a constant zero for
    // them). The lane-wise arithmetic is not in this list (it is not lowered here)
    void runLegalizedCases()
    {
        struct Encoding {
            const char* name;
            uint32 encoding;
        };
        static const Encoding encodings[] = {
            { "and v0.16b, v1.16b, v2.16b", 0x4E221C20 },
            { "orr v0.16b, v1.16b, v2.16b", 0x4EA21C20 },
            { "eor v0.16b, v1.16b, v2.16b", 0x6E221C20 },
            { "bic v0.16b, v1.16b, v2.16b", 0x4E621C20 },
            { "orn v0.16b, v1.16b, v2.16b", 0x4EE21C20 },
            { "and v0.8b, v1.8b, v2.8b", 0x0E221C20 },
            { "dup v0.2d, x1", 0x4E080C20 },
            { "mov d0, v1.d[1]", 0x5E180420 } };
        for (uint i = 0; i < sizeof(encodings) / sizeof(encodings[0]); i++)
        {
            // (lowered only: not counted as the native cases)
            if (!lowerA64Chain(encodings[i].name, encodings[i].encoding))
                m_failures++;
        }
    }

    // The lowering of a block must refuse it (throw) for the 'reason' (a
    // text of the message). A refusal for another reason is a failure: the
    // block must reach the operation, not stop at another one
    void refused(const char* name, SemanticBlock& block, const char* reason)
    {
        // (the blocks have no guest instruction: the fall-through address is
        // set as in runCase())
        block.setFallThroughAddress(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, 0x80001004ULL));
        bool isRefused = false;
        bool isForReason = false;
        cSArray<char> text;
        try
        {
            lowerOnly(block);
        } catch (cException& exception)
        {
            isRefused = true;
            text = cString(exception.getMessage()).getASCIIstring();
            isForReason = (strstr(text.getBuffer(), reason) != NULL);
        }
        if (!isRefused)
        {
            fprintf(stderr, "TestSemanticLoweringWide: %s is lowered, it must be refused\n", name);
            m_failures++;
        } else if (!isForReason)
        {
            fprintf(stderr, "TestSemanticLoweringWide: %s is refused for another reason: %s\n",
                    name, text.getBuffer());
            m_failures++;
        }
    }

    // Lower a case, then run it natively against the interpreter. The
    // 'isPointers' registers point into the guest memory (the blocks access it)
    void runCase(const char* name, SemanticBlock& block, bool isPointers)
    {
        // (the blocks have no guest instruction: the address after the block
        // is the one the interpreter and the code report)
        block.setFallThroughAddress(ProcessorAddress(
                    ProcessorAddress::PROCESSOR_64, 0x80001004ULL));
        lowerOnly(block);
        m_cases++;
#ifdef WIDE_TEST_NATIVE
        compareNative(name, block, isPointers);
#else
        (void)isPointers;
#endif
    }

#ifdef WIDE_TEST_NATIVE
    void compareNative(const char* name, SemanticBlock& block, bool isPointers)
    {
        cBuffer code;
        if (!buildNative(block, code))
        {
            fprintf(stderr, "TestSemanticLoweringWide: %s: the lowering is not assembled\n", name);
            m_failures++;
            return;
        }
        void* memory = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED)
        {
            // (a host which doesn't allow executable memory: nothing to run)
            m_skipped++;
            m_cases--;
            return;
        }
        for (uint i = 0; i < code.getSize(); i++)
            ((uint8*)memory)[i] = code[i];
        NativeFunction function = reinterpret_cast<NativeFunction>(memory);
        const uint temps = block.getTemporariesCount();

        static NativeTestMemory interpreted;
        static NativeTestMemory native;
        bool isOk = true;
        uint compared = 0;
        for (uint trial = 0; isOk && (trial < TRIALS); trial++)
        {
            // The state: the canary, then the inputs of the registers
            static uint64 state[STATE_WORDS];
            for (uint i = 0; i < STATE_WORDS; i++)
                state[i] = CANARY;
            state[0] = 0;
            for (uint i = 1; i < 32; i++)
                state[i] = wideValue(isPointers);
            for (uint v = 0; v < 32; v++)
            {
                state[(VECTOR_BASE + v * 16) / 8] = wideValue(false);
                state[(VECTOR_BASE + v * 16) / 8 + 1] = wideValue(false);
            }
            for (uint i = 0; i < NATIVE_MEMORY_SIZE; i++)
                interpreted.m_bytes[i] = native.m_bytes[i] = (uint8)wideRandom();

            SemanticInterpreter interpreter(getGuest(), interpreted);
            for (uint i = 1; i < 32; i++)
                interpreter.setRegister(RISCV_REG_X0 + i, SemanticValue(state[i]));
            for (uint v = 0; v < 32; v++)
            {
                SemanticValue value;
                value.setWord(0, state[(VECTOR_BASE + v * 16) / 8]);
                value.setWord(1, state[(VECTOR_BASE + v * 16) / 8 + 1]);
                interpreter.setRegister(RISCV_REG_V0 + v, value);
            }
            const SemanticInterpreter::ExitReason reason = interpreter.run(block);
            if ((reason == SemanticInterpreter::EXIT_MEMORY_FAULT) ||
                (reason == SemanticInterpreter::EXIT_UNDEFINED))
            {
                // (the random state faults in the guest: nothing to compare)
                continue;
            }
            function(state, native.m_bytes - NATIVE_GUEST_BASE);
            compared++;

            for (uint i = 0; i < 32; i++)
            {
                if (state[i] != interpreter.getRegister(RISCV_REG_X0 + i).getWord(0))
                {
                    fprintf(stderr, "TestSemanticLoweringWide: %s: x%u differs: "
                            "native %llx interpreter %llx\n", name, (unsigned)i,
                            (unsigned long long)state[i],
                            (unsigned long long)interpreter.getRegister(
                                RISCV_REG_X0 + i).getWord(0));
                    isOk = false;
                }
            }
            for (uint v = 0; v < 32; v++)
            {
                const uint64 lo = state[(VECTOR_BASE + v * 16) / 8];
                const uint64 hi = state[(VECTOR_BASE + v * 16) / 8 + 1];
                const SemanticValue expected = interpreter.getRegister(RISCV_REG_V0 + v);
                if ((lo != expected.getWord(0)) || (hi != expected.getWord(1)))
                {
                    fprintf(stderr, "TestSemanticLoweringWide: %s: v%u differs\n",
                            name, (unsigned)v);
                    isOk = false;
                }
            }
            if (memcmp(interpreted.m_bytes, native.m_bytes, NATIVE_MEMORY_SIZE) != 0)
            {
                fprintf(stderr, "TestSemanticLoweringWide: %s: the memory differs\n", name);
                isOk = false;
            }
            if (state[0x100 / 8] != interpreter.getNextAddress())
            {
                fprintf(stderr, "TestSemanticLoweringWide: %s: the next address differs\n", name);
                isOk = false;
            }
            // The canary: a stray write outside the slots of the block is
            // a failure. The writable words are the registers (compared
            // above), the program counter and exit slots, and the temporaries:
            // a temporary of 16 bytes or less has its high qword kept
            for (uint w = 0; w < STATE_WORDS; w++)
            {
                const uint offset = w * 8;
                if ((offset < 0x100) || (offset == 0x100) || (offset == 0x110))
                    continue;
                if ((offset >= VECTOR_BASE) && (offset < VECTOR_BASE + 32 * 16))
                    continue;
                bool mustBeCanary = true;
                if ((offset >= TEMP_BASE) && (offset < TEMP_BASE + 16 * temps))
                {
                    const uint index = (offset - TEMP_BASE) / 16;
                    const bool isHigh = ((offset - TEMP_BASE) % 16) == 8;
                    mustBeCanary = isHigh && (gTempWidth[index] != 0) &&
                                   (gTempWidth[index] <= 64);
                }
                if (mustBeCanary && (state[w] != CANARY))
                {
                    fprintf(stderr, "TestSemanticLoweringWide: %s: a stray write at "
                            "state offset 0x%x\n", name, (unsigned)offset);
                    isOk = false;
                }
            }
        }
        munmap(memory, 0x10000);
        if (compared != TRIALS)
        {
            // (no fault is expected from the operands of the cases)
            fprintf(stderr, "TestSemanticLoweringWide: %s: %u of %u trials compared\n",
                    name, (unsigned)compared, (unsigned)TRIALS);
            isOk = false;
        }
        m_comparisons += compared;
        if (!isOk)
            m_failures++;
    }
#endif

    // The failures of the cases
    uint m_failures;
    // The cases lowered (and run natively when the host allows it)
    uint m_cases;
    // The trials compared with the interpreter
    uint m_comparisons;
    // The cases which couldn't run (no executable memory)
    uint m_skipped;
};

} // namespace

// Instance test object
TestObjectTestSemanticLoweringWide g_globalTestSemanticLoweringWide;
