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
 *   Part 2  Flags: the sub-register rule, then the flags rule (the
 *           order of legalize()), are checked the same way: no OP_SET_FLAGS
 *           and no OP_TEST_FLAGS are left, and the result runs like the
 *           lifted block. legalize() runs on the same rows too: it must
 *           accept every row whose operations the target supports (the
 *           target refuses the others: SIMD and bit moves),
 *           and then leave no flag operation.
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
    uint m_setFlags; // an OP_SET_FLAGS
    uint m_testFlags; // an OP_TEST_FLAGS
    uint m_legalized; // legalize() accepted the row
    uint m_refused;   // legalize() refused it: the target lacks an operation

    LegalizerArm64Categories() : m_rows(0), m_w(0), m_wsp(0), m_vector(0),
                                 m_zero(0), m_setFlags(0), m_testFlags(0),
                                 m_legalized(0), m_refused(0)
    {
    }
};

// The rewrite of a row which is checked: the sub-register rule alone (part 1),
// or the sub-register rule then the flags rule (part 2), which legalize()
// runs before the target's expansions
enum LegalizerArm64Rule {
    RULE_SUBREGISTERS,
    RULE_FLAGS
};

// Return true if the target has every operation of the block (legalize()
// refuses the others, which the target does not support yet)
static bool isTargetSupported(const ArchitectureDescription& target,
                              const SemanticBlock& block)
{
    const cList<SemanticInstruction>& instructions = block.getInstructions();
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        if ((*i).getOperation() == SemanticOperation::OP_UNDEFINED)
            return false;
        if (!target.isSupported((*i).getOperation(), (*i).getType()))
            return false;
    }
    return true;
}

// Return true if the block has an OP_SET_FLAGS or an OP_TEST_FLAGS
static bool hasFlagOperation(const SemanticBlock& block)
{
    const cList<SemanticInstruction>& instructions = block.getInstructions();
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        const SemanticOperation::Operation operation = (*i).getOperation();
        if ((operation == SemanticOperation::OP_SET_FLAGS) ||
            (operation == SemanticOperation::OP_TEST_FLAGS))
            return true;
    }
    return false;
}

// Part 3 (flag fusion): the flag-heavy blocks, the edge values of the operands, and
// the rows of the scalar floating point conditional compares and selects.
// A block is {words, count, text, setup}: the words are the A64 encodings
// (llvm-mc), the setup overrides the start state of the edge values
struct LegalizerArm64Block {
    uint32 m_words[6];
    uint m_count;
    const char* m_text;
    const char* m_setup;
};

static const LegalizerArm64Block gFlagBlocks[] = {
    { { 0xeb01001f, 0x9a84b062 }, 2, "cmp x0, x1; csel x2, x3, x4, lt", "" },
    { { 0x6b01001f, 0x9a84b062 }, 2, "cmp w0, w1; csel x2, x3, x4, lt", "" },
    { { 0xf1001420, 0x9a848062 }, 2, "subs x0, x1, #5; csel x2, x3, x4, hi", "" },
    { { 0xeb01001f, 0xfa431044, 0x1a9fa7e5 }, 3, "cmp x0, x1; ccmp x2, x3, #4, ne; cset w5, lt", "" },
    { { 0xab020020, 0x1a9f17e3 }, 2, "adds x0, x1, x2; cset w3, eq", "" },
    { { 0x2b020020, 0x9a852083 }, 2, "adds w0, w1, w2; csel x3, x4, x5, hs", "" },
    { { 0xab01001f, 0x1a9f27e2 }, 2, "cmn x0, x1; cset w2, lo", "" },
    { { 0xea01001f, 0x9a840062 }, 2, "tst x0, x1; csel x2, x3, x4, eq", "" },
    { { 0x72001c20, 0x1a9fd7e2 }, 2, "ands w0, w1, #0xff; cset w2, gt", "" },
    { { 0xeb01001f, 0x9a84b062, 0xeb0600bf, 0x9a89c107 }, 4, "cmp x0, x1; csel x2, x3, x4, lt; cmp x5, x6; csel x7, x8, x9, gt", "" },
    { { 0xeb01001f, 0x1a9f07e2, 0xeb04007f, 0x1a9fb7e5, 0xeb0700df, 0x1a9fc7e8 }, 6, "cmp x0, x1; cset w2, ne; cmp x3, x4; cset w5, ge; cmp x6, x7; cset w8, le", "" },
    { { 0xab020020, 0x9a854083, 0xab0800e6, 0x9a8b6149 }, 4, "adds x0, x1, x2; csel x3, x4, x5, mi; adds x6, x7, x8; csel x9, x10, x11, vs", "" },
    { { 0xf100001f, 0xfa420020, 0xfa44b068, 0x1a9fd7e5 }, 4, "cmp x0, #0; ccmp x1, x2, #0, eq; ccmp x3, x4, #8, lt; cset w5, gt", "" },
    { { 0xba418002, 0x9a849062 }, 2, "ccmn x0, x1, #2, hi; csel x2, x3, x4, ls", "" },
    { { 0x6b020020, 0x7a442065, 0x9a87a0c5 }, 3, "subs w0, w1, w2; ccmp w3, w4, #5, hs; csel x5, x6, x7, ge", "" },
    { { 0xeb01001f, 0x5400004b }, 2, "cmp x0, x1; b.lt +8", "" },
    { { 0xeb1f001f, 0x9a83c041 }, 2, "cmp x0, xzr; csel x1, x2, x3, gt", "" },
    { { 0xeb0103ff, 0x9a84b062 }, 2, "cmp xzr, x1; csel x2, x3, x4, lt", "" },
    { { 0xfa020020, 0x9a853083 }, 2, "sbcs x0, x1, x2; csel x3, x4, x5, lo", "" },
    { { 0xba020020, 0x1a9f37e3 }, 2, "adcs x0, x1, x2; cset w3, hs", "" },
    { { 0x1e612000, 0x1e630444, 0x1e66cca4 }, 3, "fcmp d0, d1; fccmp d2, d3, #4, eq; fcsel d4, d5, d6, gt", "v0=3ff0000000000000 v1=4000000000000000 v2=7ff8000000000000 v3=0 v4=4008000000000000 v5=bff0000000000000 v6=4010000000000000" },
    { { 0xeb01001f, 0x9a84b062, 0x9a87c0c5 }, 3, "cmp x0, x1; csel x2, x3, x4, lt; csel x5, x6, x7, gt", "" },
    { { 0x71001c1f, 0x1a9f97e1, 0x7100245f, 0x1a9f87e3, 0xf27c0ca4, 0x9a8810e6 }, 6, "cmp w0, #7; cset w1, hi; cmp w2, #9; cset w3, ls; ands x4, x5, #0xf0; csel x6, x7, x8, ne", "" },
    { { 0xb1000420, 0x9a840062, 0xba4640a3, 0x1a9f67e7 }, 4, "adds x0, x1, #1; csel x2, x3, x4, eq; ccmn x5, x6, #3, mi; cset w7, vc", "" },
    { { 0xeb0a013f, 0xfa4c3160, 0xfa4e81a1, 0x9a91d20f }, 4, "cmp x9, x10; ccmp x11, x12, #0, lo; ccmp x13, x14, #1, hi; csel x15, x16, x17, le", "" },
    { { 0xeb01001f, 0xfa43b842, 0x9a85a083 }, 3, "cmp x0, x1; ccmp x2, #3, #2, lt; csel x3, x4, x5, ge", "" },
    { { 0x7219001f, 0x9a831041, 0xeb05009f, 0x9a8830e6 }, 4, "tst w0, #0x80; csel x1, x2, x3, ne; cmp x4, x5; csel x6, x7, x8, lo", "" },
    { { 0xeb010000, 0x9a84d062 }, 2, "subs x0, x0, x1; csel x2, x3, x4, le", "" },
    { { 0xeb01001f, 0xd28000a0, 0x9a84b062 }, 3, "cmp x0, x1; mov x0, #5; csel x2, x3, x4, lt", "" },
    { { 0xeb01001f, 0xd28000e1, 0x9a840062 }, 3, "cmp x0, x1; mov x1, #7; csel x2, x3, x4, eq", "" },
    { { 0xab020020, 0xd2800022, 0x1a9f27e3 }, 3, "adds x0, x1, x2; mov x2, #1; cset w3, lo", "" },
    { { 0xeb01001f, 0xf9400062, 0x9a86b0a4 }, 3, "cmp x0, x1; ldr x2, [x3]; csel x4, x5, x6, lt", "x3=4000 mem=4000:0102030405060708" },
    { { 0xeb01001f, 0x9a840062, 0xeb0600bf, 0x9a891107 }, 4, "cmp x0, x1; csel x2, x3, x4, eq; cmp x5, x6; csel x7, x8, x9, ne", "" },
    { { 0xeb020020, 0x1a9f97e3, 0xeb0600a4, 0x1a9f87e7 }, 4, "subs x0, x1, x2; cset w3, hi; subs x4, x5, x6; cset w7, ls", "" },
    { { 0xab020020, 0x1a9f37e3, 0xab0600a4, 0x1a9f27e7 }, 4, "adds x0, x1, x2; cset w3, hs; adds x4, x5, x6; cset w7, lo", "" },
    { { 0xab01001f, 0x9a846062, 0xab0600bf, 0x9a897107 }, 4, "cmn x0, x1; csel x2, x3, x4, vs; cmn x5, x6; csel x7, x8, x9, vc", "" },
    { { 0xea01001f, 0x9a841062, 0xea0600bf, 0x9a890107 }, 4, "tst x0, x1; csel x2, x3, x4, ne; tst x5, x6; csel x7, x8, x9, eq", "" },
    { { 0xea020020, 0x1a9f57e3, 0xea0600a4, 0x1a9f47e7 }, 4, "ands x0, x1, x2; cset w3, mi; ands x4, x5, x6; cset w7, pl", "" },
    { { 0x6b01001f, 0x1a9fd7e2, 0x6b04007f, 0x1a9fc7e5 }, 4, "cmp w0, w1; cset w2, gt; cmp w3, w4; cset w5, le", "" },
    { { 0xf1000c1f, 0xfa470821, 0x1a9fa7e2, 0xeb04007f, 0x9a8780c5 }, 5, "cmp x0, #3; ccmp x1, #7, #1, eq; cset w2, lt; cmp x3, x4; csel x5, x6, x7, hi", "" },
    { { 0xeb01001f, 0xba431040, 0xfa452082, 0x9a88a0e6 }, 4, "cmp x0, x1; ccmn x2, x3, #0, ne; ccmp x4, x5, #2, cs; csel x6, x7, x8, ge", "" },
    { { 0x71000420, 0x9a840062, 0x710008c5, 0x1a9f07e7 }, 4, "subs w0, w1, #1; csel x2, x3, x4, eq; subs w5, w6, #2; cset w7, ne", "" },
    { { 0xeb01001f, 0xeb03005f, 0x5400004b }, 3, "cmp x0, x1; cmp x2, x3; b.lt #8", "" },
    { { 0xeb01001f, 0x9a84d062, 0xab0700c5, 0x1a9f97e8 }, 4, "cmp x0, x1; csel x2, x3, x4, le; adds x5, x6, x7; cset w8, hi", "" },
    { { 0xeb01001f, 0xfa43b040, 0x1a9fb7e4, 0xeb0600bf, 0xfa48c0e4, 0x9a8b8149 }, 6, "cmp x0, x1; ccmp x2, x3, #0, lt; cset w4, ge; cmp x5, x6; ccmp x7, x8, #4, gt; csel x9, x10, x11, hi", "" },
    { { 0x2b020020, 0x7a440060, 0x9a8720c5 }, 3, "adds w0, w1, w2; ccmp w3, w4, #0, eq; csel x5, x6, x7, hs", "" },
    { { 0xeb1f001f, 0x1a9f17e1, 0xeb0203ff, 0x1a9fb7e3 }, 4, "cmp x0, xzr; cset w1, eq; cmp xzr, x2; cset w3, ge", "" },
    { { 0xfa020020, 0x1a9f37e3, 0xeb05009f, 0x1a9f27e6 }, 4, "sbcs x0, x1, x2; cset w3, hs; cmp x4, x5; cset w6, lo", "" },
    { { 0x1e612000, 0x9a84c062, 0x1e6620a0, 0x9a89b107 }, 4, "fcmp d0, d1; csel x2, x3, x4, gt; fcmp d5, d6; csel x7, x8, x9, lt", "v0=3ff0000000000000 v1=4000000000000000 v5=7ff8000000000000 v6=3ff0000000000000" },
    { { 0xeb01001f, 0xd2800062, 0xeb03005f, 0x9a86b0a4 }, 4, "cmp x0, x1; mov x2, #3; cmp x2, x3; csel x4, x5, x6, lt", "" },
    { { 0x6a01001f, 0x1a9f07e2, 0x6a04007f, 0x1a9f17e5 }, 4, "tst w0, w1; cset w2, ne; tst w3, w4; cset w5, eq", "" },
    { { 0xf1001c20, 0xba41b844, 0x9a85a083 }, 3, "subs x0, x1, #7; ccmn x2, #1, #4, lt; csel x3, x4, x5, ge", "" },
    { {0}, 0, NULL, NULL }
};

// The scalar fccmp, fccmpe and fcsel rows of Arm64SimdTestRows.inc (the
// double and single forms), with the start states of the compares
static const LegalizerArm64Row gFloatFlagRows[] = {
    { { 0x1e600400 }, 1, "fccmp d0, d0, #0x0, eq", "nzcv=0" },
    { { 0x1e6aa6aa }, 1, "fccmp d21, d10, #0xa, ge", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e6ac508 }, 1, "fccmp d8, d10, #0x8, gt", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e62d52e }, 1, "fccmp d9, d2, #0xe, le", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e71378e }, 1, "fccmp d28, d17, #0xe, lo", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e6cb542 }, 1, "fccmp d10, d12, #0x2, lt", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e68144f }, 1, "fccmp d2, d8, #0xf, ne", "nzcv=0" },
    { { 0x1e61f645 }, 1, "fccmp d18, d1, #0x5, nv", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e755545 }, 1, "fccmp d10, d21, #0x5, pl", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e617729 }, 1, "fccmp d25, d1, #0x9, vc", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e65644f }, 1, "fccmp d2, d5, #0xf, vs", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e34e7c1 }, 1, "fccmp s30, s20, #0x1, al", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e200400 }, 1, "fccmp s0, s0, #0x0, eq", "nzcv=0" },
    { { 0x1e2aa6aa }, 1, "fccmp s21, s10, #0xa, ge", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e36c7ee }, 1, "fccmp s31, s22, #0xe, gt", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e37d624 }, 1, "fccmp s17, s23, #0x4, le", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e20372c }, 1, "fccmp s25, s0, #0xc, lo", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e239429 }, 1, "fccmp s1, s3, #0x9, ls", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e221542 }, 1, "fccmp s10, s2, #0x2, ne", "nzcv=0" },
    { { 0x1e3df5e4 }, 1, "fccmp s15, s29, #0x4, nv", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e2c5781 }, 1, "fccmp s28, s12, #0x1, pl", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e2074c9 }, 1, "fccmp s6, s0, #0x9, vc", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e3966e9 }, 1, "fccmp s23, s25, #0x9, vs", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e78e458 }, 1, "fccmpe d2, d24, #0x8, al", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e600410 }, 1, "fccmpe d0, d0, #0x0, eq", "nzcv=0" },
    { { 0x1e6aa6ba }, 1, "fccmpe d21, d10, #0xa, ge", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e6ec45d }, 1, "fccmpe d2, d14, #0xd, gt", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e6d86f2 }, 1, "fccmpe d23, d13, #0x2, hi", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e702679 }, 1, "fccmpe d19, d16, #0x9, hs", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e63d677 }, 1, "fccmpe d19, d3, #0x7, le", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e764619 }, 1, "fccmpe d16, d22, #0x9, mi", "nzcv=0" },
    { { 0x1e7ff7ff }, 1, "fccmpe d31, d31, #0xf, nv", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e755555 }, 1, "fccmpe d10, d21, #0x5, pl", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e23e636 }, 1, "fccmpe s17, s3, #0x6, al", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e200410 }, 1, "fccmpe s0, s0, #0x0, eq", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e2aa6ba }, 1, "fccmpe s21, s10, #0xa, ge", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e24c7dc }, 1, "fccmpe s30, s4, #0xc, gt", "nzcv=0" },
    { { 0x1e288432 }, 1, "fccmpe s1, s8, #0x2, hi", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e29d652 }, 1, "fccmpe s18, s9, #0x2, le", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e26357d }, 1, "fccmpe s11, s6, #0xd, lo", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e3eb654 }, 1, "fccmpe s18, s30, #0x4, lt", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e2847b8 }, 1, "fccmpe s29, s8, #0x8, mi", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e22f55e }, 1, "fccmpe s10, s2, #0xe, nv", "nzcv=0" },
    { { 0x1e205519 }, 1, "fccmpe s8, s0, #0x9, pl", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e2674d5 }, 1, "fccmpe s6, s6, #0x5, vc", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e2267b1 }, 1, "fccmpe s29, s2, #0x1, vs", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e7fef62 }, 1, "fcsel d2, d27, d31, al", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e600c00 }, 1, "fcsel d0, d0, d0, eq", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e6aaeaa }, 1, "fcsel d10, d21, d10, ge", "nzcv=0" },
    { { 0x1e68cf1b }, 1, "fcsel d27, d24, d8, gt", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e783dcb }, 1, "fcsel d11, d14, d24, lo", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e72be4a }, 1, "fcsel d10, d18, d18, lt", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e6d1f9a }, 1, "fcsel d26, d28, d13, ne", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e62fe15 }, 1, "fcsel d21, d16, d2, nv", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e665dd6 }, 1, "fcsel d22, d14, d6, pl", "nzcv=0" },
    { { 0x1e676d6c }, 1, "fcsel d12, d11, d7, vs", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e24ec08 }, 1, "fcsel s8, s0, s4, al", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e200c00 }, 1, "fcsel s0, s0, s0, eq", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e22ad45 }, 1, "fcsel s5, s10, s2, ge", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e20cf8e }, 1, "fcsel s14, s28, s0, gt", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e208e38 }, 1, "fcsel s24, s17, s0, hi", "nzcv=0" },
    { { 0x1e282f58 }, 1, "fcsel s24, s26, s8, hs", "nzcv=6 v0=3ff0000000000000 v1=4000000000000000" },
    { { 0x1e239c64 }, 1, "fcsel s4, s3, s3, ls", "nzcv=9 v0=7ff8000000000000 v1=3ff0000000000000" },
    { { 0x1e241f23 }, 1, "fcsel s3, s25, s4, ne", "nzcv=5 v0=7ff0000000000001 v1=7ff0000000000001" },
    { { 0x1e26fe34 }, 1, "fcsel s20, s17, s6, nv", "nzcv=f v0=0 v1=8000000000000000" },
    { { 0x1e355d55 }, 1, "fcsel s21, s10, s21, pl", "nzcv=a v0=bff0000000000000 v1=0 v2=4008000000000000 v3=c000000000000000" },
    { { 0x1e2c6fa3 }, 1, "fcsel s3, s29, s12, vs", "nzcv=0" },
    { {0}, 0, NULL, NULL, NULL }
};

// The operands of the sweeps: the edges of 64 bit integers (zero, the small
// values, the sign bits, the word boundaries, all ones)
static const uint64 gEdges[16] = {
    0, 1, 2, 3, 5, 0x7f, 0x80, 0xff,
    0x7fffffff, 0x80000000, 0xffffffff, 0x100000000ull,
    0x7fffffffffffffffull, 0x8000000000000000ull, 0xfffffffffffffffeull,
    0xffffffffffffffffull
};

// The A64 words of the compares which set the flags on the registers x1, x2
// (the destinations are the zero register): cmp x, cmn x, tst x, cmp w
static const uint32 gCompareWords[4] = {
    0xeb02003f, 0xab02003f, 0xea02003f, 0x6b02003f
};

// The integer conditions of OP_COMPARE and OP_TEST_FLAGS (the enum order)
static bool isIntegerCondition(uint c)
{
    return (c >= SemanticOperation::COND_NONE) &&
           (c <= SemanticOperation::COND_PARITY_ODD);
}

// Return true if the operation is pure (see SemanticLegalizer's removal of
// dead temporaries): the count of the blocks below uses the same removal
static bool isPureOperation(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    return !SemanticOperation::hasSideEffects(operation) &&
           !SemanticOperation::isMemoryAccess(operation) &&
           !SemanticOperation::isControlFlow(operation) &&
           (instruction.getFlagMask() == 0) &&
           ((instruction.getAttributes() &
             (SemanticInstruction::ATTRIBUTE_MAY_FAULT |
              SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS)) == 0);
}

// The operations of a block left after its dead temporaries are removed (the
// same removal for both sides of a count, so the counts are comparable)
static uint countOperations(const SemanticBlock& block)
{
    cList<SemanticInstruction> instructions = block.getInstructions();
    const uint temporaries = block.getTemporariesCount();
    cBuffer isRead(temporaries);
    bool isRemoved = true;
    while (isRemoved)
    {
        isRemoved = false;
        for (uint t = 0; t < temporaries; t++)
            isRead[t] = 0;
        for (cList<SemanticInstruction>::iterator i = instructions.begin();
             i != instructions.end(); i++)
        {
            for (uint s = 0; s < SemanticInstruction::MAX_SOURCES; s++)
            {
                const SemanticOperand& source = (*i).getSource(s);
                if (source.getKind() == SemanticOperand::KIND_TEMPORARY)
                    isRead[source.getIndex()] = 1;
            }
        }
        for (cList<SemanticInstruction>::iterator i = instructions.begin();
             i != instructions.end(); i++)
        {
            const SemanticOperand& destination = (*i).getDestination();
            if (!SemanticOperation::hasDestination((*i).getOperation()) ||
                (destination.getKind() != SemanticOperand::KIND_TEMPORARY) ||
                isRead[destination.getIndex()] || !isPureOperation(*i))
                continue;
            *i = SemanticInstruction();
            isRemoved = true;
        }
    }
    uint count = 0;
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++)
    {
        if ((*i).getOperation() != SemanticOperation::OP_NOP)
            count++;
    }
    return count;
}

// Append OP_TEST_FLAGS(condition) into a temporary, and the 64 bit value of
// the temporary into x0 (so the interpreter's registers show the condition)
static void appendTest(SemanticBlock& block,
                       SemanticOperation::Condition condition)
{
    const SemanticType booleanType = SemanticType::integer(1);
    SemanticOperand value = block.newTemporary(booleanType);
    SemanticInstruction test(SemanticOperation::OP_TEST_FLAGS, booleanType,
                             value);
    test.setCondition(condition);
    block.append(test);
    const SemanticType wide = SemanticType::integer(64);
    block.append(SemanticInstruction(SemanticOperation::OP_ZERO_EXTEND, wide,
                 SemanticOperand::registerOperand(ARM64_REG_X0, wide), value));
}

// Part 3 (fusion guards): a flag register written by a plain move, and
// the operands of a fused test which change between the flag write and the
// test. Each block is checked from 256 start states (the edge values of
// x0..x5 and the NZCV), the expansion alone and the fused one
static const LegalizerArm64Block gGuardBlocks[] = {
    { { 0xeb01001f, 0xd51b4205, 0x9a84b062, 0xeb04007f }, 4, "cmp x0, x1; msr nzcv, x5; csel x2, x3, x4, lt; cmp x3, x4", "" },
    { { 0xeb010000, 0x9a84b062, 0xeb04007f }, 3, "subs x0, x0, x1; csel x2, x3, x4, lt; cmp x3, x4", "" },
    { { 0xeb01001f, 0x528000e0, 0x9a84b062, 0xeb04007f }, 4, "cmp x0, x1; mov w0, #7; csel x2, x3, x4, lt; cmp x3, x4", "" },
    { { 0xeb01001f, 0xd2800100, 0x9a84b062, 0xeb04007f }, 4, "cmp x0, x1; mov x0, #8; csel x2, x3, x4, lt; cmp x3, x4", "" },
    { { 0xeb01001f, 0xf8400080, 0x9a84b062, 0xeb04007f }, 4, "cmp x0, x1; ldur x0, [x4]; csel x2, x3, x4, lt; cmp x3, x4", "x4=4000 mem=4000:0102030405060708" },
    { { 0xeb01001f, 0xd500401f, 0x9a84b062, 0xeb04007f }, 4, "cmp x0, x1; cfinv; csel x2, x3, x4, lt; cmp x3, x4", "" },
    { { 0xeb01001f, 0x3a0008ad, 0x9a84b062, 0xeb04007f }, 4, "cmp x0, x1; setf8 w5; csel x2, x3, x4, lt; cmp x3, x4", "" },
    { { 0xeb01001f, 0xba0004a8, 0x9a84b062, 0xeb04007f }, 4, "cmp x0, x1; rmif x5, #0, #8; csel x2, x3, x4, lt; cmp x3, x4", "" },
    { { 0xeb01001f, 0xd51b4205, 0x9a840062, 0xeb04007f }, 4, "cmp x0, x1; msr nzcv, x5; csel x2, x3, x4, eq; cmp x3, x4", "" },
    { {0}, 0, NULL, NULL }
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
        bool isSet = false, isTest = false;
        const cList<SemanticInstruction>& instructions = block.getInstructions();
        for (cList<SemanticInstruction>::iterator i = instructions.begin();
             i != instructions.end(); i++)
        {
            if ((*i).getOperation() == SemanticOperation::OP_SET_FLAGS)
                isSet = true;
            if ((*i).getOperation() == SemanticOperation::OP_TEST_FLAGS)
                isTest = true;
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
        categories.m_setFlags += isSet ? 1 : 0;
        categories.m_testFlags += isTest ? 1 : 0;
    }

    // Lift a row, rewrite a copy with the rule, and compare the two runs of
    // the interpreter from the same start state
    void checkRewrittenRow(const LegalizerArm64Row& row,
                           const LegalizerArm64Config& config,
                           bool isZeroed,
                           LegalizerArm64Rule rule,
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
            if (rule == RULE_FLAGS)
                legalizer.expandFlags(rewritten);
        } catch (cException&)
        {
            fail(row.m_text, (rule == RULE_FLAGS) ?
                    "expandFlags() refused the row" :
                    "expandSubRegisters() refused the row");
            return;
        }
        if (hasRuleRegister(*architecture, rewritten))
        {
            fail(row.m_text, "a sub-register or a zero register is left");
            return;
        }
        if ((rule == RULE_FLAGS) && hasFlagOperation(rewritten))
        {
            fail(row.m_text, "an OP_SET_FLAGS or OP_TEST_FLAGS is left");
            return;
        }

        // legalize() on the row: it runs the same two rules, then the
        // target's expansions. The target must accept the row when it has
        // every operation of the rewritten block
        if (rule == RULE_FLAGS)
        {
            SemanticBlock legalized(block);
            bool isLegalized = true;
            try
            {
                legalizer.legalize(legalized);
            } catch (cException&)
            {
                isLegalized = false;
            }
            if (isLegalized)
            {
                if (hasFlagOperation(legalized) ||
                    hasRuleRegister(*architecture, legalized))
                {
                    fail(row.m_text, "legalize() leaves a flag operation or a "
                                     "sub-register");
                    return;
                }
                categories.m_legalized++;
            } else if (isTargetSupported(*architecture, rewritten))
            {
                fail(row.m_text, "legalize() refused a row the target supports");
                return;
            } else
            {
                categories.m_refused++;
            }
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
                  LegalizerArm64Rule rule,
                  LegalizerArm64Categories& categories)
    {
        // Each row runs as it is, and with its general registers zeroed
        for (const LegalizerArm64Row* row = table; row->m_text != NULL; row++)
        {
            checkRewrittenRow(*row, config, false, rule, categories);
            checkRewrittenRow(*row, config, true, rule, categories);
        }
    }

    // Part 1: the sub-register rule on the rows of every table, for each
    // vector configuration. The categories must not be empty: the rows
    // exercise the rule
    void testSubRegisters()
    {
        const LegalizerArm64Row* tables[] = { gBaseRows, gSimdRows, gDirectedRows, gFloatFlagRows };
        for (uint c = 0; c < sizeof(gConfigs) / sizeof(gConfigs[0]); c++)
        {
            const LegalizerArm64Config& config = gConfigs[c];
            const uint failuresBefore = m_failures;
            LegalizerArm64Categories categories;
            for (uint t = 0; t < sizeof(tables) / sizeof(tables[0]); t++)
                runTable(tables[t], config, RULE_SUBREGISTERS, categories);

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

    // Part 2: the flags rule on every row, for each vector
    // configuration. The rows which set and test the flags must exercise it
    void testFlags()
    {
        const LegalizerArm64Row* tables[] = { gBaseRows, gSimdRows, gDirectedRows, gFloatFlagRows };
        for (uint c = 0; c < sizeof(gConfigs) / sizeof(gConfigs[0]); c++)
        {
            const LegalizerArm64Config& config = gConfigs[c];
            const uint failuresBefore = m_failures;
            LegalizerArm64Categories categories;
            for (uint t = 0; t < sizeof(tables) / sizeof(tables[0]); t++)
                runTable(tables[t], config, RULE_FLAGS, categories);

            char summary[256];
            snprintf(summary, sizeof(summary),
                     "flags: %u rows, %u mismatches [Arm64Architecture(%u,%u)]",
                     (unsigned)categories.m_rows,
                     (unsigned)(m_failures - failuresBefore),
                     (unsigned)config.m_vectorBits,
                     (unsigned)config.m_streamingBits);
            cout << summary << endl;
            snprintf(summary, sizeof(summary),
                     "flags: set_flags=%u test_flags=%u legalize: %u accepted, "
                     "%u refused by the target",
                     (unsigned)categories.m_setFlags,
                     (unsigned)categories.m_testFlags,
                     (unsigned)categories.m_legalized,
                     (unsigned)categories.m_refused);
            cout << summary << endl;

            TESTS_ASSERT(categories.m_rows > 0);
            TESTS_ASSERT(categories.m_setFlags >= kFloorSetFlags);
            TESTS_ASSERT(categories.m_testFlags >= kFloorTestFlags);
            TESTS_ASSERT(m_failures == failuresBefore);
        }
    }

    // Part 2, the floating point compare recipe. No A64 row sets the
    // flags with FLAGS_FLOAT_COMPARE (the A64 lifter compares into the flags
    // with OP_COMPARE), so the recipe is checked on its own: a double compare
    // of v0 and v1 with the flags of the Arm64 mapping, for each kind of
    // compare. The expanded block must run like the original, and the flags
    // must be the relation (unordered: Z and C set; less: C; equal: Z). A
    // compare which raises the exception flags is refused, and the block is
    // unchanged then
    void testFloatCompareRecipe()
    {
        struct Pair {
            uint64 m_a;
            uint64 m_b;
            uint m_z;
            uint m_c;
        };
        static const Pair pairs[] = {
            { 0x3ff0000000000000ull, 0x4000000000000000ull, 0, 1 }, // 1 < 2
            { 0x4000000000000000ull, 0x4000000000000000ull, 1, 0 }, // 2 = 2
            { 0x4000000000000000ull, 0x3ff0000000000000ull, 0, 0 }, // 2 > 1
            { 0x0000000000000000ull, 0x8000000000000000ull, 1, 0 }, // 0 = -0
            { 0x7ff8000000000000ull, 0x3ff0000000000000ull, 1, 1 }, // qNaN
            { 0x3ff0000000000000ull, 0x7ff0000000000001ull, 1, 1 }, // sNaN
            { 0xbff0000000000000ull, 0x0000000000000000ull, 0, 1 }, // -1 < 0
        };
        const SemanticType doubleType = SemanticType::floating(
                                                SemanticType::FLOAT_DOUBLE);
        ArchitectureDescriptionPtr architecture(new Arm64Architecture(128, 128));
        const SemanticOperation::FlagRecipe recipe =
                                        SemanticOperation::FLAGS_FLOAT_COMPARE;
        uint cases = 0;
        const uint failuresBefore = m_failures;
        for (uint kind = 0; kind < 2; kind++)
        {
            for (uint p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
                SemanticInstruction compare(SemanticOperation::OP_SET_FLAGS,
                        doubleType, SemanticOperand(),
                        SemanticOperand::registerOperand(ARM64_REG_Z0, doubleType),
                        SemanticOperand::registerOperand((ARM64_REG_Z0 + 1), doubleType));
                compare.setFlags(recipe, SemanticFlags::FLAGS_ARITHMETIC);
                compare.setKind(kind == 0 ? SemanticOperation::FLOAT_COMPARE_QUIET :
                                            SemanticOperation::FLOAT_COMPARE_SIGNALING);
                block.append(compare);

                LegalizerArm64State state;
                LegalizerArm64Memory memory;
                state.m_v[0] = SemanticValue(pairs[p].m_a);
                state.m_v[1] = SemanticValue(pairs[p].m_b);
                SemanticBlock rewritten(block);
                SemanticLegalizer legalizer(architecture, architecture);
                legalizer.expandFlags(rewritten);
                if (hasFlagOperation(rewritten))
                    fail("float compare", "an OP_SET_FLAGS is left");

                SemanticInterpreter interpreter(architecture, memory);
                LegalizerArm64Memory copyMemory;
                SemanticInterpreter copyInterpreter(architecture, copyMemory);
                applyState(interpreter, state);
                applyState(copyInterpreter, state);
                const SemanticInterpreter::ExitReason reason = interpreter.run(block);
                const SemanticInterpreter::ExitReason copyReason =
                                                copyInterpreter.run(rewritten);
                cases++;
                if (reason != copyReason)
                {
                    fail("float compare", "the exit of the rewritten block differs");
                    continue;
                }
                const uint registers = architecture->getRegistersCount();
                for (uint id = 0; id < registers; id++)
                {
                    if (interpreter.getRegister(id) != copyInterpreter.getRegister(id))
                    {
                        fail("float compare", "a register differs");
                        break;
                    }
                }
                const uint z = (uint)copyInterpreter.getRegister(ARM64_REG_Z).getBits(0, 1);
                const uint c = (uint)copyInterpreter.getRegister(ARM64_REG_C).getBits(0, 1);
                if ((z != pairs[p].m_z) || (c != pairs[p].m_c))
                    fail("float compare", "the flags are not the relation");
            }
        }

        // A compare which raises the exception flags is refused, unchanged
        SemanticBlock raising(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        SemanticInstruction compare(SemanticOperation::OP_SET_FLAGS, doubleType,
                SemanticOperand(),
                SemanticOperand::registerOperand(ARM64_REG_Z0, doubleType),
                SemanticOperand::registerOperand((ARM64_REG_Z0 + 1), doubleType));
        compare.setFlags(recipe, SemanticFlags::FLAGS_ARITHMETIC);
        compare.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
        raising.append(compare);
        SemanticBlock before(raising);
        SemanticLegalizer legalizer(architecture, architecture);
        bool isRefused = false;
        try
        {
            legalizer.expandFlags(raising);
        } catch (cException&)
        {
            isRefused = true;
        }
        TESTS_ASSERT(isRefused);
        const cList<SemanticInstruction>& after = raising.getInstructions();
        TESTS_ASSERT(after.length() == 1);
        TESTS_ASSERT((*after.begin()).getOperation() ==
                     SemanticOperation::OP_SET_FLAGS);

        char summary[256];
        snprintf(summary, sizeof(summary),
                 "flags float: %u cases, %u mismatches",
                 (unsigned)cases, (unsigned)(m_failures - failuresBefore));
        cout << summary << endl;
        TESTS_ASSERT(m_failures == failuresBefore);
    }

    // Lift the A64 words of a block, the instructions at 0x1000 on (the same
    // as checkRewrittenRow). Return false if a word doesn't decode
    static bool liftWords(const ArchitectureDescriptionPtr& architecture,
                          const uint32* words, uint count,
                          SemanticBlock& block)
    {
        ArmSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE,
                                 architecture);
        for (uint i = 0; i < count; i++)
        {
            ArmOpcodePtr opcode = ArmStreamDisassembler::decode(
                    ArmInstructionSet::ARM_A64, words[i], 4, 0x1000 + 4 * i,
                    ArmFeatureSet::all());
            if (opcode.getPointer() == NULL)
                return false;
            lifter.lift(asOpcode(opcode), block);
        }
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, 0x1000 + 4 * count));
        return true;
    }

    // Rewrite a block as legalize() does before the target's expansions: the
    // sub-register rule, then the flags rule (fused, or the expansion alone)
    static void rewriteBlock(SemanticBlock& block,
                             const ArchitectureDescriptionPtr& architecture,
                             bool fuse)
    {
        SemanticLegalizer legalizer(architecture, architecture);
        legalizer.expandSubRegisters(block);
        legalizer.expandFlags(block, fuse);
    }

    // Run a block and its rewrite from the same start state (the setup of
    // the block applied over it), and compare the exits and every register.
    // A mismatch is a failure of 'text'. Return true when they agree
    bool compareRuns(const char* text,
                     const ArchitectureDescriptionPtr& architecture,
                     SemanticBlock& block, SemanticBlock& rewritten,
                     const LegalizerArm64State& start, const char* setup)
    {
        LegalizerArm64State state = start;
        LegalizerArm64Memory memory;
        LegalizerArm64Memory copyMemory;
        if (!parseSetup(setup, state, memory) ||
            !parseSetup(setup, state, copyMemory))
        {
            fail(text, "bad setup token");
            return false;
        }
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
            fail(text, "the exit of the rewritten block differs");
            return false;
        }
        const uint registers = architecture->getRegistersCount();
        for (uint id = 0; id < registers; id++)
        {
            if (interpreter.getRegister(id) != copyInterpreter.getRegister(id))
            {
                char message[256];
                snprintf(message, sizeof(message), "register %s differs",
                         architecture->getRegister(id).m_name);
                fail(text, message);
                return false;
            }
        }
        return true;
    }

    // Part 3 (a): every integer condition through OP_TEST_FLAGS. The
    // expansion alone reads the flags of each of the 16 NZCV states. A test
    // after a compare, a cmn, a tst or a 32 bit compare reads the operands
    // (fused) over the edge pairs. The fused expansion must be shorter than
    // the expansion alone for the fusion to be exercised
    void testIntegerConditions()
    {
        ArchitectureDescriptionPtr architecture(new Arm64Architecture(128, 128));
        const uint failuresBefore = m_failures;
        uint runs = 0;
        uint fusedRuns = 0;
        uint fusedShorter = 0;
        for (uint c = SemanticOperation::COND_NONE; c <= SemanticOperation::COND_PARITY_ODD; c++)
        {
            if (!isIntegerCondition(c))
                continue;
            const SemanticOperation::Condition condition =
                                    (SemanticOperation::Condition)c;

            // The expansion alone: the flags are the start state's
            for (uint nzcv = 0; nzcv < 16; nzcv++)
            {
                SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
                appendTest(block, condition);
                LegalizerArm64State start;
                start.m_nzcv = nzcv;
                start.m_x[1] = gEdges[nzcv];
                SemanticBlock rewritten(block);
                rewriteBlock(rewritten, architecture, true);
                runs++;
                compareRuns("integer condition", architecture, block,
                            rewritten, start, "");
            }

            // The fused tests: a compare on x1 and x2, then the test
            for (uint kind = 0; kind < 4; kind++)
            {
                for (uint i = 0; i < 16; i++)
                {
                    for (uint j = 0; j < 16; j++)
                    {
                        // The compare, the test, then a compare of x3 and x4
                        // which overwrites the flags (the fusion of the test
                        // pays only when they are overwritten)
                        static const uint32 overwrite = 0xeb04007fu; // cmp x3, x4
                        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
                        if (!liftWords(architecture, &gCompareWords[kind], 1,
                                       block))
                        {
                            fail("integer condition", "the compare doesn't decode");
                            return;
                        }
                        appendTest(block, condition);
                        if (!liftWords(architecture, &overwrite, 1, block))
                        {
                            fail("integer condition", "the compare doesn't decode");
                            return;
                        }
                        LegalizerArm64State start;
                        start.m_nzcv = (i + j) & 15;
                        start.m_x[1] = gEdges[i];
                        start.m_x[2] = gEdges[j];
                        start.m_x[3] = gEdges[(i + 3) & 15];
                        start.m_x[4] = gEdges[(j + 7) & 15];
                        SemanticBlock reference(block);
                        rewriteBlock(reference, architecture, false);
                        SemanticBlock rewritten(block);
                        rewriteBlock(rewritten, architecture, true);
                        runs++;
                        fusedRuns++;
                        if (countOperations(rewritten) < countOperations(reference))
                            fusedShorter++;
                        compareRuns("integer condition (fused)", architecture,
                                    block, rewritten, start, "");
                        SemanticBlock expanded(block);
                        rewriteBlock(expanded, architecture, false);
                        compareRuns("integer condition (expansion)", architecture,
                                    block, expanded, start, "");
                    }
                }
            }
        }

        char summary[256];
        snprintf(summary, sizeof(summary),
                 "flags conditions: %u runs (%u fused), %u fused blocks shorter "
                 "than the expansion, %u mismatches",
                 (unsigned)runs, (unsigned)fusedRuns, (unsigned)fusedShorter,
                 (unsigned)(m_failures - failuresBefore));
        cout << summary << endl;
        TESTS_ASSERT(fusedShorter > 0);
        TESTS_ASSERT(m_failures == failuresBefore);
    }

    // Part 3 (b): the conditional compares (ccmp and ccmn, the register forms)
    // for every condition and every false flag value, over the 16 start NZCV
    // states; and after a compare over the edge pairs, where the condition of
    // the ccmp reads the operands of the compare (fused)
    void testConditionalCompares()
    {
        ArchitectureDescriptionPtr architecture(new Arm64Architecture(128, 128));
        const uint failuresBefore = m_failures;
        uint runs = 0;
        // ccmp x0, x1 is 0xfa400000, ccmn x0, x1 is 0xba400000 (Rm = x1)
        const uint32 bases[2] = { 0xfa400000u | (1u << 16), 0xba400000u | (1u << 16) };
        const uint32 compareWord = 0xeb03005fu; // cmp x2, x3
        for (uint variant = 0; variant < 2; variant++)
        {
            for (uint cond = 0; cond < 16; cond++)
            {
                for (uint falseFlags = 0; falseFlags < 16; falseFlags++)
                {
                    const uint32 word = bases[variant] | (cond << 12) | falseFlags;

                    // The expansion alone: the flags are the start state's
                    for (uint start = 0; start < 16; start++)
                    {
                        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
                        if (!liftWords(architecture, &word, 1, block))
                        {
                            fail("conditional compare", "the word doesn't decode");
                            return;
                        }
                        LegalizerArm64State state;
                        state.m_nzcv = start;
                        state.m_x[0] = gEdges[(cond + start) & 15];
                        state.m_x[1] = gEdges[(falseFlags + 3) & 15];
                        SemanticBlock reference(block);
                        rewriteBlock(reference, architecture, false);
                        SemanticBlock rewritten(block);
                        rewriteBlock(rewritten, architecture, true);
                        runs++;
                        compareRuns("conditional compare", architecture, block,
                                    reference, state, "");
                        compareRuns("conditional compare (fused)", architecture,
                                    block, rewritten, state, "");
                    }

                    // After a compare of x2 and x3 (fused) over edge pairs
                    for (uint p = 0; p < 8; p++)
                    {
                        const uint32 words[2] = { compareWord, word };
                        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
                        if (!liftWords(architecture, words, 2, block))
                        {
                            fail("conditional compare", "the words don't decode");
                            return;
                        }
                        LegalizerArm64State state;
                        state.m_nzcv = (p + falseFlags) & 15;
                        state.m_x[0] = gEdges[(2 * p) & 15];
                        state.m_x[1] = gEdges[(2 * p + 1) & 15];
                        state.m_x[2] = gEdges[(p + 5) & 15];
                        state.m_x[3] = gEdges[(p * 3 + 1) & 15];
                        SemanticBlock reference(block);
                        rewriteBlock(reference, architecture, false);
                        SemanticBlock rewritten(block);
                        rewriteBlock(rewritten, architecture, true);
                        runs++;
                        compareRuns("conditional compare after cmp", architecture,
                                    block, reference, state, "");
                        compareRuns("conditional compare after cmp (fused)",
                                    architecture, block, rewritten, state, "");
                    }
                }
            }
        }

        char summary[256];
        snprintf(summary, sizeof(summary),
                 "flags ccmp: %u runs, %u mismatches",
                 (unsigned)runs, (unsigned)(m_failures - failuresBefore));
        cout << summary << endl;
        TESTS_ASSERT(m_failures == failuresBefore);
    }

    // Part 3 (c): the flag-heavy blocks, each from six start states (the edge
    // values in x0..x17, the NZCV of the state, the block's own setup). The
    // expansion alone and the fused one must both match the interpreter. The
    // operations of each block are counted for both (the fusion must not grow a block).
    // The sample has every block; the flag-heavy blocks are the ones with two
    // or more flag writes, the only ones whose flag writes can be overwritten
    // (a block's last writer keeps its flags: its successors may read them)
    void testFlagBlocks()
    {
        ArchitectureDescriptionPtr architecture(new Arm64Architecture(128, 128));
        const uint failuresBefore = m_failures;
        uint blocks = 0;
        uint64 alone = 0;
        uint64 fused = 0;
        uint heavyBlocks = 0;
        uint64 heavyAlone = 0;
        uint64 heavyFused = 0;
        for (const LegalizerArm64Block* b = gFlagBlocks; b->m_text != NULL; b++)
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
            if (!liftWords(architecture, b->m_words, b->m_count, block))
            {
                fail(b->m_text, "a word doesn't decode");
                continue;
            }
            SemanticBlock reference(block);
            rewriteBlock(reference, architecture, false);
            SemanticBlock rewritten(block);
            rewriteBlock(rewritten, architecture, true);
            const uint aloneCount = countOperations(reference);
            const uint fusedCount = countOperations(rewritten);
            uint writes = 0;
            const cList<SemanticInstruction>& instructions = block.getInstructions();
            for (cList<SemanticInstruction>::iterator i = instructions.begin();
                 i != instructions.end(); i++)
            {
                if ((*i).getOperation() == SemanticOperation::OP_SET_FLAGS)
                    writes++;
            }
            blocks++;
            alone += aloneCount;
            fused += fusedCount;
            if (writes >= 2)
            {
                heavyBlocks++;
                heavyAlone += aloneCount;
                heavyFused += fusedCount;
            }
            if (fusedCount > aloneCount)
                fail(b->m_text, "the fused block is longer than the expansion");
            cout << "flags block: " << (unsigned)aloneCount << " ops (D2) -> "
                 << (unsigned)fusedCount << " ops (D3), " << writes
                 << " flag writes: " << b->m_text << endl;

            for (uint k = 0; k < 6; k++)
            {
                LegalizerArm64State state;
                state.m_nzcv = (k * 5 + blocks) & 15;
                for (uint i = 0; i < 18; i++)
                    state.m_x[i] = gEdges[(i * 5 + k * 3 + blocks) & 15];
                compareRuns(b->m_text, architecture, block, reference, state,
                            b->m_setup);
                compareRuns(b->m_text, architecture, block, rewritten, state,
                            b->m_setup);
            }
        }

        char summary[256];
        snprintf(summary, sizeof(summary),
                 "flags blocks: %u blocks, %u ops (D2 alone) -> %u ops (D2+D3), "
                 "%u mismatches",
                 (unsigned)blocks, (unsigned)alone, (unsigned)fused,
                 (unsigned)(m_failures - failuresBefore));
        cout << summary << endl;
        snprintf(summary, sizeof(summary),
                 "flags blocks, two or more flag writes: %u blocks, %u ops (D2 "
                 "alone) -> %u ops (D2+D3)",
                 (unsigned)heavyBlocks, (unsigned)heavyAlone,
                 (unsigned)heavyFused);
        cout << summary << endl;
        TESTS_ASSERT(blocks >= 20);
        TESTS_ASSERT(heavyBlocks >= 20);
        TESTS_ASSERT(heavyFused * 100 <= heavyAlone * 75);
        TESTS_ASSERT(m_failures == failuresBefore);
    }

    // A subtraction whose operand x0 is overwritten by a move before the flag
    // write which reads it: the fused test must not read x0 (the flags are the
    // subtraction's and the operand's old value). Then cmp x3, x4 overwrites
    static void buildMovedOperandBlock(SemanticBlock& block,
                                       const ArchitectureDescriptionPtr& architecture)
    {
        const SemanticType wide = SemanticType::integer(64);
        const SemanticOperand x0 = SemanticOperand::registerOperand(ARM64_REG_X0, wide);
        const SemanticOperand x1 = SemanticOperand::registerOperand(ARM64_REG_X0 + 1, wide);
        const SemanticOperand notB = block.newTemporary(wide);
        block.append(SemanticInstruction(SemanticOperation::OP_NOT, wide, notB, x1));
        const SemanticOperand r = block.newTemporary(wide);
        block.append(SemanticInstruction(SemanticOperation::OP_SUBTRACT, wide, r,
                                         x0, x1));
        block.append(SemanticInstruction(SemanticOperation::OP_MOVE, wide, x0,
                                         SemanticOperand::immediate(5, wide)));
        SemanticInstruction set(SemanticOperation::OP_SET_FLAGS, wide,
                                SemanticOperand(), x0, notB, r);
        set.setFlags(SemanticOperation::FLAGS_ADD,
                     SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO |
                     SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW);
        block.append(set);
        appendTest(block, SemanticOperation::COND_SIGNED_LESS);
    static const uint32 overwrite = 0xeb04007fu; // cmp x3, x4
        liftWords(architecture, &overwrite, 1, block);
    }

    // Part 3 (guards): the blocks of gGuardBlocks and the moved operand block,
    // each over 256 start states (x0..x5 from the edges, the NZCV). The
    // fused and the expanded blocks must match the interpreter
    void testFlagGuards()
    {
        ArchitectureDescriptionPtr architecture(new Arm64Architecture(128, 128));
        const uint failuresBefore = m_failures;
        uint runs = 0;
        for (uint b = 0; b <= 9; b++)
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
            const char* text = "moved operand";
            const char* setup = "";
            if (b < 9)
            {
                const LegalizerArm64Block& row = gGuardBlocks[b];
                if (row.m_text == NULL)
                    break;
                text = row.m_text;
                setup = row.m_setup;
                if (!liftWords(architecture, row.m_words, row.m_count, block))
                {
                    fail(text, "a word doesn't decode");
                    continue;
                }
            } else
            {
                buildMovedOperandBlock(block, architecture);
            }
            for (uint i = 0; i < 16; i++)
            {
                for (uint j = 0; j < 16; j++)
                {
                    LegalizerArm64State start;
                    start.m_nzcv = (i + j) & 15;
                    start.m_x[0] = gEdges[i];
                    start.m_x[1] = gEdges[j];
                    start.m_x[2] = gEdges[(i * 3 + j) & 15];
                    start.m_x[3] = gEdges[(i + 5) & 15];
                    start.m_x[4] = gEdges[(j + 9) & 15];
                    start.m_x[5] = gEdges[(i * 5 + j * 7) & 15];
                    SemanticBlock reference(block);
                    rewriteBlock(reference, architecture, false);
                    SemanticBlock rewritten(block);
                    rewriteBlock(rewritten, architecture, true);
                    runs++;
                    compareRuns(text, architecture, block, reference, start, setup);
                    compareRuns(text, architecture, block, rewritten, start, setup);
                }
            }
        }
        char summary[256];
        snprintf(summary, sizeof(summary),
                 "flags guards: %u runs, %u mismatches",
                 (unsigned)runs, (unsigned)(m_failures - failuresBefore));
        cout << summary << endl;
        TESTS_ASSERT(runs == 256 * 10);
        TESTS_ASSERT(m_failures == failuresBefore);
    }

    virtual void test()
    {
        const uint failuresBefore = m_failures;
        testSubRegisters();
        testRefusalLeavesBlock();
        testFlags();
        testFloatCompareRecipe();
        testIntegerConditions();
        testConditionalCompares();
        testFlagBlocks();
        testFlagGuards();
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
    // (Measured with both variants of every row, both configurations:
    // set_flags 1172, test_flags 2504)
    static const uint kFloorSetFlags = 100;
    static const uint kFloorTestFlags = 100;

    uint m_failures;
};

// Instance test object
TestObjectTestSemanticLegalizerArm64 g_globalTestSemanticLegalizerArm64;
