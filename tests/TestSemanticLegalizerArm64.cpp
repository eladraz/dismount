/*
 * TestSemanticLegalizerArm64.cpp
 *
 * Tests of the SemanticLegalizer on the AArch64 guest (A64 lifted from
 * ArmSemantics, the target is not involved in these rules). The file is in
 * parts, one per rule of the legalizer:
 *
 *   Part 1  Sub-registers. Every row of tests/arm/Arm64SemanticRows.inc
 *           and of the four tests/arm/Arm64SimdSemanticRows*.inc files, plus
 *           the directed rows below (sp, wsp, wzr), is lifted, the block is
 *           copied, the copy is rewritten by expandSubRegisters(), and both
 *           blocks run in the SemanticInterpreter from the setup of the row.
 *           The exit, the next address, the trap, every register (the full
 *           ones and the sub-registers) and the memory must be equal. The
 *           upper bits of the z registers are seeded non-zero, so a missing
 *           zeroing above 128 bits shows. The whole set runs for each vector
 *           configuration: Arm64Architecture(128,512) and (128,128) (the Lua
 *           configuration). The rewritten block has no sub-register and no
 *           zero register operand left. A refused rewrite leaves its input
 *           block unchanged.
 *   Part 2  Flags: OP_SET_FLAGS and OP_TEST_FLAGS are materialized
 *           into plain IR, and checked the same way against the rows.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include <stdio.h>
#include <string.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/hash.h"
#include "xStl/data/list.h"
#include "xStl/data/smartptr.h"
#include "xStl/exceptions.h"
#include "xStl/stream/ioStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/Semantics.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticInterpreter.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"

using namespace armdis;

// The rows: {words}, count, text, setup, expected (the expected state is the
// one of TestArm64Semantics, the rules here compare the two runs instead)
struct LegalizerArm64Row {
    uint32 m_words[4];
    uint m_count;
    const char* m_text;
    const char* m_setup;
    const char* m_expected;
};

#include "arm/Arm64SemanticRows.inc"
#include "arm/Arm64SimdSemanticRowsFloat.inc"
#include "arm/Arm64SimdSemanticRowsInteger.inc"
#include "arm/Arm64SimdSemanticRowsMemory.inc"
#include "arm/Arm64SimdSemanticRowsCrypto.inc"

static const LegalizerArm64Row gBaseRows[] = {
    ARM64_SEMANTIC_ROWS
    { {0}, 0, NULL, NULL, NULL }
};

static const LegalizerArm64Row gSimdRows[] = {
    ARM64_SIMD_ROWS_FLOAT
    ARM64_SIMD_ROWS_INTEGER
    ARM64_SIMD_ROWS_MEMORY
    ARM64_SIMD_ROWS_CRYPTO
    { {0}, 0, NULL, NULL, NULL }
};

// The directed rows: the stack pointer and wsp (a write of wsp zeroes the
// upper half of sp), the zero registers, and the 32 bit forms of the rest
static const LegalizerArm64Row gDirectedRows[] = {
    { {0x910043ff}, 1, "add sp, sp, #16", "sp=100000008000", "" },
    { {0x110043ff}, 1, "add wsp, wsp, #16", "sp=100000008000", "" },
    { {0x510007e0}, 1, "sub w0, wsp, #1", "sp=100000008000", "" },
    { {0x1100003f}, 1, "add wsp, w1, #0", "x1=ffffffff80000000 sp=100000008000", "" },
    { {0x710003ff}, 1, "subs wzr, wsp, #0", "sp=100000008000", "" },
    { {0x2a1f03e0}, 1, "orr w0, wzr, wzr", "x0=ffffffffffffffff", "" },
    { {0x2a0103e0}, 1, "mov w0, w1", "x1=ffffffff00000005 x0=ffffffffffffffff", "" },
    { {0x8b0103e0}, 1, "add x0, xzr, x1", "x1=1234", "" },
    { {0x2b0103e0}, 1, "adds w0, wzr, w1", "x1=ffffffff00000005", "" },
    { {0xf900003f}, 1, "str xzr, [x1]", "x1=4000 mem=4000:0102030405060708", "" },
    { {0xb9400020}, 1, "ldr w0, [x1]", "x1=4000 mem=4000:01020304", "" },
    { {0x9e670020}, 1, "fmov d0, x1", "x1=0123456789abcdef", "" },
    { {0x1e270020}, 1, "fmov s0, w1", "x1=ffffffff12345678", "" },
    { {0x0b020020}, 1, "add w0, w1, w2", "x1=7fffffff x2=1", "" },
    { {0}, 0, NULL, NULL, NULL }
};

// The vector configurations of the run: the Arm64Architecture(vl, svl) of
// the rows (128,512) and the one of the Lua build (128,128)
struct LegalizerArm64Config {
    uint m_vectorBits;
    uint m_streamingBits;
};

static const LegalizerArm64Config gConfigs[] = {
    { 128, 512 },
    { 128, 128 },
};

// The memory of a run: [0, 0x20000), zero unless set
class LegalizerArm64Memory : public SemanticMemory {
public:
    virtual bool read(uint64 address, uint8* buffer, uint size)
    {
        if (!isValid(address, size))
            return false;
        for (uint i = 0; i < size; i++)
            buffer[i] = getByte(address + i);
        return true;
    }

    virtual bool write(uint64 address, const uint8* buffer, uint size)
    {
        if (!isValid(address, size))
            return false;
        for (uint i = 0; i < size; i++)
            set(address + i, buffer[i]);
        return true;
    }

    uint8 getByte(uint64 address) const
    {
        return m_bytes.hasKey(address) ? m_bytes[address] : 0;
    }

    void set(uint64 address, uint8 value)
    {
        if (m_bytes.hasKey(address))
            m_bytes[address] = value;
        else
            m_bytes.append(address, value);
    }

    cList<uint64> getAddresses() const
    {
        return m_bytes.keys();
    }

private:
    static bool isValid(uint64 address, uint size)
    {
        return (address < 0x20000) && (address + size <= 0x20000);
    }

    cHash<uint64, uint8> m_bytes;
};

// The start state of a row, as the setup text gives it
struct LegalizerArm64State {
    uint64 m_x[31];
    SemanticValue m_v[32];
    uint64 m_sp;
    uint64 m_pc;
    uint m_nzcv;
    uint64 m_fpcr;
    uint64 m_fpsr;

    LegalizerArm64State() : m_sp(0x8000), m_pc(0x1000), m_nzcv(0),
                            m_fpcr(0), m_fpsr(0)
    {
        memset(m_x, 0, sizeof(m_x));
    }
};

// Parse the hexadecimal digits of [text, end), the last digit is the bit 0
static SemanticValue parseHex(const char* text, const char* end)
{
    SemanticValue value;
    const uint digits = (uint)(end - text);
    for (uint i = 0; i < digits; i++)
    {
        const char c = text[digits - 1 - i];
        const uint nibble = (c >= 'a') ? (uint)(c - 'a' + 10) : (uint)(c - '0');
        value.setBits(i * 4, 4, nibble);
    }
    return value;
}

// Set the bytes of "addr:hex,addr:hex" in the memory
static void parseMemory(const char* text, LegalizerArm64Memory& memory)
{
    while (*text != 0)
    {
        const char* colon = strchr(text, ':');
        if (colon == NULL)
            return;
        const uint64 address = parseHex(text, colon).getWord(0);
        const char* p = colon + 1;
        uint64 count = 0;
        while ((*p != 0) && (*p != ','))
        {
            const uint8 byte = (uint8)parseHex(p, p + 2).getWord(0);
            memory.set(address + count, byte);
            p += 2;
            count++;
        }
        text = (*p == ',') ? p + 1 : p;
    }
}

// Apply the "name=value" tokens of a setup to the state and the memory. Return
// false for a token the test doesn't know
static bool parseSetup(const char* setup, LegalizerArm64State& state,
                       LegalizerArm64Memory& memory)
{
    char buffer[1024];
    strncpy(buffer, setup, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = 0;
    bool isKnown = true;
    for (char* token = strtok(buffer, " "); token != NULL;
         token = strtok(NULL, " "))
    {
        char* equals = strchr(token, '=');
        if (equals == NULL)
            return false;
        *equals = 0;
        const char* name = token;
        const char* value = equals + 1;
        const char* end = value + strlen(value);
        unsigned int index = 0;
        if (strcmp(name, "mem") == 0)
            parseMemory(value, memory);
        else if ((sscanf(name, "x%u", &index) == 1) && (index < 31))
            state.m_x[index] = parseHex(value, end).getWord(0);
        else if ((sscanf(name, "v%u", &index) == 1) && (index < 32))
            state.m_v[index] = parseHex(value, end);
        else if (strcmp(name, "sp") == 0)
            state.m_sp = parseHex(value, end).getWord(0);
        else if (strcmp(name, "pc") == 0)
            state.m_pc = parseHex(value, end).getWord(0);
        else if (strcmp(name, "nzcv") == 0)
            state.m_nzcv = (uint)parseHex(value, end).getWord(0);
        else if (strcmp(name, "fpcr") == 0)
            state.m_fpcr = parseHex(value, end).getWord(0);
        else if (strcmp(name, "fpsr") == 0)
            state.m_fpsr = parseHex(value, end).getWord(0);
        else
            isKnown = false;
    }
    return isKnown;
}

// Put the start state into an interpreter
static void applyState(SemanticInterpreter& interpreter,
                       const LegalizerArm64State& state)
{
    for (uint i = 0; i < 31; i++)
        interpreter.setRegister(ARM64_REG_X0 + i, SemanticValue(state.m_x[i]));
    interpreter.setRegister(ARM64_REG_SP, SemanticValue(state.m_sp));
    interpreter.setRegister(ARM64_REG_PC, SemanticValue(state.m_pc));
    interpreter.setRegister(ARM64_REG_N, SemanticValue((state.m_nzcv >> 3) & 1));
    interpreter.setRegister(ARM64_REG_Z, SemanticValue((state.m_nzcv >> 2) & 1));
    interpreter.setRegister(ARM64_REG_C, SemanticValue((state.m_nzcv >> 1) & 1));
    interpreter.setRegister(ARM64_REG_V, SemanticValue(state.m_nzcv & 1));
    interpreter.setRegister(ARM64_REG_FPCR, SemanticValue(state.m_fpcr));
    interpreter.setRegister(ARM64_REG_FPSR, SemanticValue(state.m_fpsr));
    // The bits of z above 128 are seeded non-zero: a write of a vector
    // register must zero them (the interpreter does it, the rewrite too)
    for (uint i = 0; i < 32; i++)
    {
        SemanticValue z = state.m_v[i];
        for (uint word = 2; word < SemanticValue::WORDS_COUNT; word++)
            z.setWord(word, 0xa5a5a5a5a5a5a5a5ull ^ (uint64)(i * 0x101 + word));
        interpreter.setRegister(ARM64_REG_Z0 + i, z);
    }
}

// Return a pointer to the instruction which doesn't own it
static OpcodePtr asOpcode(const ArmOpcodePtr& opcode)
{
    return OpcodePtr((Opcode*)opcode.getPointer(), SMARTPTR_DESTRUCT_NONE);
}

// Return true if an operand of the instruction is a sub-register or a zero
// register of the architecture
static bool isRuleRegister(const ArchitectureDescription& architecture,
                           const SemanticOperand& operand)
{
    if (operand.getKind() != SemanticOperand::KIND_REGISTER)
        return false;
    const ArchitectureRegister& reg = architecture.getRegister(operand.getIndex());
    return reg.m_isZero || (reg.m_parent != ArchitectureRegister::NO_PARENT);
}

// Return true if the block has an operand the rule rewrites
static bool hasRuleRegister(const ArchitectureDescription& architecture,
                            const SemanticBlock& block)
{
    const cList<SemanticInstruction>& instructions = block.getInstructions();
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        if (isRuleRegister(architecture, (*i).getDestination()))
            return true;
        for (uint s = 0; s < SemanticInstruction::MAX_SOURCES; s++)
        {
            if (isRuleRegister(architecture, (*i).getSource(s)))
                return true;
        }
    }
    return false;
}

// The rows of a configuration by category, counted over the original blocks:
// a row counts in every category one of its operands belongs to
struct LegalizerArm64Categories {
    uint m_rows;
    uint m_w;      // a general register written or read as wN
    uint m_wsp;    // wsp
    uint m_vector; // a vector register read or written as vN, qN, dN, sN...
    uint m_zero;   // xzr or wzr

    LegalizerArm64Categories() : m_rows(0), m_w(0), m_wsp(0), m_vector(0),
                                 m_zero(0)
    {
    }
};

class TestObjectTestSemanticLegalizerArm64 : public cTestObject {
public:
    TestObjectTestSemanticLegalizerArm64() : m_failures(0)
    {
    }

    // Report a mismatch of a row and count it
    void fail(const char* text, const char* message)
    {
        fprintf(stderr, "TestSemanticLegalizerArm64: %s [%s]: %s\n", text,
                __FILE__, message);
        m_failures++;
    }

    // The zeroed variant of a block: every 32 and 64 bit general register operand
// (except the stack pointer) becomes wzr or xzr. The lifter folds the zero
// registers it reads, so the rule's zero path is only reached this way. Both
// runs of a variant see the same block, so only the equality is checked
static void zeroRegisters(const ArchitectureDescription& architecture,
                          SemanticBlock& block)
{
    cList<SemanticInstruction>& instructions = block.getInstructions();
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        for (uint n = 0; n < 1 + SemanticInstruction::MAX_SOURCES; n++)
        {
            const SemanticOperand operand = (n == 0) ?
                    (*i).getDestination() : (*i).getSource(n - 1);
            if (operand.getKind() != SemanticOperand::KIND_REGISTER)
                continue;
            const ArchitectureRegister& reg =
                                architecture.getRegister(operand.getIndex());
            if ((reg.m_file != ArchitectureRegister::FILE_GENERAL) ||
                (reg.m_isZero) || (strcmp(reg.m_name, "sp") == 0) ||
                (strcmp(reg.m_name, "wsp") == 0))
                continue;
            uint zero = 0;
            if (reg.m_bits == 64)
                zero = ARM64_REG_XZR;
            else if (reg.m_bits == 32)
                zero = ARM64_REG_WZR;
            else
                continue;
            const SemanticOperand replacement = SemanticOperand::registerOperand(
                    zero, operand.getType());
            if (n == 0)
                (*i).setDestination(replacement);
            else
                (*i).setSource(n - 1, replacement);
        }
    }
}

// Count the categories of a block (before the rewrite)
    static void countCategories(const ArchitectureDescription& architecture,
                                const SemanticBlock& block,
                                LegalizerArm64Categories& categories)
    {
        bool isW = false, isWsp = false, isVector = false, isZero = false;
        const cList<SemanticInstruction>& instructions = block.getInstructions();
        for (cList<SemanticInstruction>::iterator i = instructions.begin();
             i != instructions.end(); i++)
        {
            for (uint n = 0; n < 1 + SemanticInstruction::MAX_SOURCES; n++)
            {
                const SemanticOperand& operand = (n == 0) ?
                        (*i).getDestination() : (*i).getSource(n - 1);
                if (operand.getKind() != SemanticOperand::KIND_REGISTER)
                    continue;
                const ArchitectureRegister& reg =
                                    architecture.getRegister(operand.getIndex());
                if (reg.m_isZero)
                    isZero = true;
                else if (reg.m_parent == ArchitectureRegister::NO_PARENT)
                    continue;
                else if (reg.m_file == ArchitectureRegister::FILE_VECTOR)
                    isVector = true;
                else if (strcmp(reg.m_name, "wsp") == 0)
                    isWsp = true;
                else
                    isW = true;
            }
        }
        categories.m_rows++;
        categories.m_w += isW ? 1 : 0;
        categories.m_wsp += isWsp ? 1 : 0;
        categories.m_vector += isVector ? 1 : 0;
        categories.m_zero += isZero ? 1 : 0;
    }

    // Lift a row, rewrite a copy with the sub-register rule, and compare the
    // two runs of the interpreter from the same start state
    void checkSubRegisterRow(const LegalizerArm64Row& row,
                             const LegalizerArm64Config& config,
                             bool isZeroed,
                             LegalizerArm64Categories& categories)
    {
        ArchitectureDescriptionPtr architecture(
                new Arm64Architecture(config.m_vectorBits,
                                      config.m_streamingBits));
        ArmSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE,
                                 architecture);

        // The start state, and the same one for each run
        LegalizerArm64State state;
        LegalizerArm64Memory memory;
        LegalizerArm64Memory copyMemory;
        if (!parseSetup(row.m_setup, state, memory) ||
            !parseSetup(row.m_setup, state, copyMemory))
        {
            fail(row.m_text, "bad setup token");
            return;
        }

        // Decode and lift the original block
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        for (uint i = 0; i < row.m_count; i++)
        {
            ArmOpcodePtr opcode = ArmStreamDisassembler::decode(
                    ArmInstructionSet::ARM_A64, row.m_words[i], 4,
                    state.m_pc + 4 * i, ArmFeatureSet::all());
            if (opcode.getPointer() == NULL)
            {
                fail(row.m_text, "the word doesn't decode");
                return;
            }
            lifter.lift(asOpcode(opcode), block);
        }
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64,
                state.m_pc + 4 * row.m_count));
        if (isZeroed)
            zeroRegisters(*architecture, block);
        countCategories(*architecture, block, categories);

        // The copy, rewritten. A refusal of the rule is a failure of the row
        SemanticBlock rewritten(block);
        SemanticLegalizer legalizer(architecture, architecture);
        try
        {
            legalizer.expandSubRegisters(rewritten);
        } catch (cException&)
        {
            fail(row.m_text, "expandSubRegisters() refused the row");
            return;
        }
        if (hasRuleRegister(*architecture, rewritten))
        {
            fail(row.m_text, "a sub-register or a zero register is left");
            return;
        }

        // Both blocks run from the same state
        SemanticInterpreter interpreter(architecture, memory);
        SemanticInterpreter copyInterpreter(architecture, copyMemory);
        applyState(interpreter, state);
        applyState(copyInterpreter, state);
        const SemanticInterpreter::ExitReason reason = interpreter.run(block);
        const SemanticInterpreter::ExitReason copyReason =
                                            copyInterpreter.run(rewritten);
        if ((reason != copyReason) ||
            (interpreter.getNextAddress() != copyInterpreter.getNextAddress()) ||
            (interpreter.getTrapKind() != copyInterpreter.getTrapKind()) ||
            (interpreter.getTrapNumber() != copyInterpreter.getTrapNumber()) ||
            (interpreter.getFaultAddress() != copyInterpreter.getFaultAddress()))
        {
            fail(row.m_text, "the exit of the rewritten block differs");
            return;
        }

        // Every register, the sub-registers too: a difference in a parent is
        // seen through the sub-registers and the other way round
        const uint registers = architecture->getRegistersCount();
        for (uint id = 0; id < registers; id++)
        {
            if (interpreter.getRegister(id) != copyInterpreter.getRegister(id))
            {
                char message[256];
                snprintf(message, sizeof(message), "register %s differs",
                         architecture->getRegister(id).m_name);
                fail(row.m_text, message);
                return;
            }
        }

        // The memory: the addresses of both runs, the values of both
        cList<uint64> addresses = memory.getAddresses();
        cList<uint64> copyAddresses = copyMemory.getAddresses();
        for (cList<uint64>::iterator a = copyAddresses.begin();
             a != copyAddresses.end(); a++)
            addresses.append(*a);
        for (cList<uint64>::iterator a = addresses.begin();
             a != addresses.end(); a++)
        {
            if (memory.getByte(*a) != copyMemory.getByte(*a))
            {
                char message[256];
                snprintf(message, sizeof(message), "mem[%llx] differs",
                         (unsigned long long)*a);
                fail(row.m_text, message);
                return;
            }
        }
    }

    // Run one table of rows under one configuration
    void runTable(const LegalizerArm64Row* table,
                  const LegalizerArm64Config& config,
                  LegalizerArm64Categories& categories)
    {
        // Each row runs as it is, and with its general registers zeroed
        for (const LegalizerArm64Row* row = table; row->m_text != NULL; row++)
        {
            checkSubRegisterRow(*row, config, false, categories);
            checkSubRegisterRow(*row, config, true, categories);
        }
    }

    // Part 1: the sub-register rule on the rows of every table, for each
    // vector configuration. The categories must not be empty: the rows
    // exercise the rule
    void testSubRegisters()
    {
        const LegalizerArm64Row* tables[] = { gBaseRows, gSimdRows, gDirectedRows };
        for (uint c = 0; c < sizeof(gConfigs) / sizeof(gConfigs[0]); c++)
        {
            const LegalizerArm64Config& config = gConfigs[c];
            const uint failuresBefore = m_failures;
            LegalizerArm64Categories categories;
            for (uint t = 0; t < sizeof(tables) / sizeof(tables[0]); t++)
                runTable(tables[t], config, categories);

            char summary[256];
            snprintf(summary, sizeof(summary),
                     "subregs: %u rows, %u mismatches [Arm64Architecture(%u,%u)]",
                     (unsigned)categories.m_rows,
                     (unsigned)(m_failures - failuresBefore),
                     (unsigned)config.m_vectorBits,
                     (unsigned)config.m_streamingBits);
            cout << summary << endl;
            snprintf(summary, sizeof(summary),
                     "subregs: categories w=%u wsp=%u vector=%u zero=%u",
                     (unsigned)categories.m_w, (unsigned)categories.m_wsp,
                     (unsigned)categories.m_vector, (unsigned)categories.m_zero);
            cout << summary << endl;

            // Per category floors, so the run can't pass without exercising
            // the rule in every category
            TESTS_ASSERT(categories.m_rows > 0);
            TESTS_ASSERT(categories.m_w >= kFloorW);
            TESTS_ASSERT(categories.m_wsp >= kFloorWsp);
            TESTS_ASSERT(categories.m_vector >= kFloorVector);
            TESTS_ASSERT(categories.m_zero >= kFloorZero);
            TESTS_ASSERT(m_failures == failuresBefore);
        }
    }

    // A refused rewrite (here: a sub-register written with the width of
    // another register) throws, and the block it was given is unchanged
    void testRefusalLeavesBlock()
    {
        ArchitectureDescriptionPtr architecture(new Arm64Architecture(128, 512));
        ArmSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE,
                                 architecture);
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        ArmOpcodePtr opcode = ArmStreamDisassembler::decode(
                ArmInstructionSet::ARM_A64, 0x0b020020, 4, 0x1000,
                ArmFeatureSet::all());
        TESTS_ASSERT(opcode.getPointer() != NULL);
        lifter.lift(asOpcode(opcode), block);

        // The destination w0 is written as a 64 bit value: the rule refuses
        cList<SemanticInstruction>& instructions = block.getInstructions();
        for (cList<SemanticInstruction>::iterator i = instructions.begin();
             i != instructions.end(); i++)
        {
            const SemanticOperand destination = (*i).getDestination();
            if (destination.getKind() == SemanticOperand::KIND_REGISTER)
            {
                (*i).setDestination(SemanticOperand::registerOperand(
                        destination.getIndex(), SemanticType::integer(64)));
            }
        }

        const SemanticBlock before(block);
        SemanticLegalizer legalizer(architecture, architecture);
        bool isRefused = false;
        try
        {
            legalizer.expandSubRegisters(block);
        } catch (cException&)
        {
            isRefused = true;
        }
        TESTS_ASSERT(isRefused);

        const cList<SemanticInstruction>& after = block.getInstructions();
        const cList<SemanticInstruction>& expected = before.getInstructions();
        TESTS_ASSERT(after.length() == expected.length());
        for (cList<SemanticInstruction>::iterator a = after.begin(),
             e = expected.begin(); a != after.end(); a++, e++)
        {
            TESTS_ASSERT((*a).getOperation() == (*e).getOperation());
            TESTS_ASSERT((*a).getDestination() == (*e).getDestination());
        }
        TESTS_ASSERT(block.getGuestInstructions().length() ==
                     before.getGuestInstructions().length());
        TESTS_ASSERT(block.getTemporariesCount() == before.getTemporariesCount());
    }

    // Part 2: the flags rule. Not implemented yet

    virtual void test()
    {
        const uint failuresBefore = m_failures;
        testSubRegisters();
        testRefusalLeavesBlock();
        TESTS_ASSERT(m_failures == failuresBefore);
        cout << "SemanticLegalizer Arm64 tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    // The category floors (see testSubRegisters)
    // (Measured with both variants of every row: w 1771, wsp 8 (the four
    // directed wsp rows), vector 9860, zero 3752; the floors are well below)
    static const uint kFloorW = 1000;
    static const uint kFloorWsp = 8;
    static const uint kFloorVector = 1000;
    static const uint kFloorZero = 1000;

    uint m_failures;
};

// Instance test object
TestObjectTestSemanticLegalizerArm64 g_globalTestSemanticLegalizerArm64;
