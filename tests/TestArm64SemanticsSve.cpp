/*
 * TestArm64SemanticsSve.cpp
 *
 * Tests of the A64 SVE, SVE2 and SVE2.1 lifter (ArmSemantics.h,
 * Arm64SemanticLiftSve*.cpp): instructions run by the interpreter from a state
 * against the state the Arm ARM defines, at more than one vector length. The
 * expected values (tests/arm/Arm64SveSemanticRows.inc) are computed by
 * tests/arm/tools/gen_sve_semantic_rows.py from the independent model of the
 * pseudocode in tests/arm/tools/sve_model*.py, and not by running dismount.
 *
 * The differential tests against Unicorn and the Apple M4 (tests/arm/
 * semantics, a64_sve*.s) cover the instructions those processors run; these
 * rows pin the classification-sensitive behavior at the vector lengths the
 * processors cannot run (256 bits), and cover the instructions neither
 * processor implements (the SVE2.1 and later extensions, bf16 and fp8
 * forms...).
 *
 * Row: { {word}, text, vl, setup, expected }. The setup and expected strings
 * are blank separated tokens: z0..z31, p0..p15, ffr (hexadecimal, VL / 8
 * bits for p and ffr), x0..x30, sp, nzcv, fpcr, fpsr, mem=<addr>:<hex bytes>,
 * ... (the initial memory; the bytes which differ at the end), exit=<kind>.
 * The registers which the expectation does not name must be unchanged.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include <string.h>
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/string.h"
#include "xStl/data/hash.h"
#include "xStl/data/list.h"
#include "xStl/data/datastream.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/exceptions.h"
#include "xStl/stream/ioStream.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/Semantics.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"

using namespace armdis;

// The rows: {word}, text, vl, setup, expected
struct Arm64SveSemanticRow {
    uint32 m_words[2];
    uint m_count;
    const char* m_text;
    uint m_vectorBits;
    const char* m_setup;
    const char* m_expected;
};

static const Arm64SveSemanticRow gArm64SveSemanticRows[] = {
#include "arm/Arm64SveSemanticRows.inc"
    { {0, 0}, 0, NULL, 0, NULL, NULL }
};

// The memory of the tests: [0, 0x20000), zero unless set; the writes are kept
class TestSveRunMemory : public SemanticMemory {
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
        {
            set(address + i, buffer[i]);
            if (!m_written.hasKey(address + i))
                m_written.append(address + i, true);
        }
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

    cList<uint64> getWritten() const
    {
        return m_written.keys();
    }

private:
    static bool isValid(uint64 address, uint size)
    {
        return (address < 0x20000) && (address + size <= 0x20000);
    }

    cHash<uint64, uint8> m_bytes;
    cHash<uint64, bool> m_written;
};

class TestObjectTestArm64SemanticsSve : public cTestObject {
public:
    TestObjectTestArm64SemanticsSve() : m_failures(0) {}

    // Parse a hexadecimal number of up to 512 bits
    static SemanticValue parseHex(const char* text, const char* end)
    {
        SemanticValue value;
        uint digits = (uint)(end - text);
        for (uint i = 0; i < digits; i++)
        {
            const char c = text[digits - 1 - i];
            uint nibble = (c >= 'a') ? (uint)(c - 'a' + 10) : (uint)(c - '0');
            value.setBits(i * 4, 4, nibble);
        }
        return value;
    }

    struct Token {
        cString m_name;
        const char* m_value;
        const char* m_end;
    };

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
            tokens[count].m_name = cString(cString(p).left((uint)(equals - p)));
            tokens[count].m_value = equals + 1;
            tokens[count].m_end = end;
            count++;
            p = end;
        }
        return count;
    }

    // Parse "addr:hex,addr:hex" into the memory, or into the expected bytes
    static void parseMemory(const char* text, const char* end, TestSveRunMemory& memory,
                            cHash<uint64, uint8>* expected)
    {
        const char* p = text;
        while (p < end)
        {
            const char* colon = (const char*)memchr(p, ':', (size_t)(end - p));
            TESTS_ASSERT(colon != NULL);
            uint64 address = parseHex(p, colon).getWord(0);
            const char* q = colon + 1;
            uint count = 0;
            while ((q < end) && (*q != ','))
            {
                uint8 byte = (uint8)parseHex(q, q + 2).getWord(0);
                if (expected != NULL)
                {
                    if (expected->hasKey(address + count))
                        (*expected)[address + count] = byte;
                    else
                        expected->append(address + count, byte);
                } else
                {
                    memory.set(address + count, byte);
                }
                q += 2;
                count++;
            }
            p = (q < end) ? q + 1 : q;
        }
    }

    void fail(const Arm64SveSemanticRow& row, const char* message)
    {
        fprintf(stderr, "TestArm64SemanticsSve: %s (vl %u): %s\n", row.m_text,
                (unsigned)row.m_vectorBits, message);
        m_failures++;
    }

    void failValue(const Arm64SveSemanticRow& row, const char* name,
                   const SemanticValue& actual, const SemanticValue& wanted, uint bits)
    {
        char message[1024];
        char a[160], w[160];
        a[0] = w[0] = 0;
        for (int i = (int)(bits / 64) - 1; i >= 0; i--)
        {
            char part[24];
            snprintf(part, sizeof(part), "%016llx", (unsigned long long)actual.getWord(i));
            strcat(a, part);
            snprintf(part, sizeof(part), "%016llx", (unsigned long long)wanted.getWord(i));
            strcat(w, part);
        }
        snprintf(message, sizeof(message), "%s = %s, expected %s", name, a, w);
        fail(row, message);
    }

    // Run a row
    void runRow(const Arm64SveSemanticRow& row)
    {
        const uint vl = row.m_vectorBits;
        ArchitectureDescriptionPtr architecture(new Arm64Architecture(vl, 512));
        ArmSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE, architecture);
        TestSveRunMemory memory;
        SemanticInterpreter interpreter(architecture, memory);

        // The start state
        Token tokens[96];
        uint64 initialX[31];
        SemanticValue initialZ[32];
        SemanticValue initialP[16];
        SemanticValue initialFfr;
        uint64 initialSp = 0x8000;
        const uint64 pc = 0x1000;
        uint initialNzcv = 0;
        uint64 initialFpcr = 0, initialFpsr = 0;
        memset(initialX, 0, sizeof(initialX));
        const uint setupCount = split(row.m_setup, tokens, 96);
        for (uint i = 0; i < setupCount; i++)
        {
            const Token& t = tokens[i];
            const cSArray<char> name = t.m_name.getASCIIstring();
            unsigned int index = 0;
            if (t.m_name == "mem")
            {
                parseMemory(t.m_value, t.m_end, memory, NULL);
                continue;
            }
            const SemanticValue value = parseHex(t.m_value, t.m_end);
            if ((sscanf(name.getBuffer(), "x%u", &index) == 1) && (index < 31))
                initialX[index] = value.getWord(0);
            else if ((sscanf(name.getBuffer(), "z%u", &index) == 1) && (index < 32))
                initialZ[index] = value;
            else if ((sscanf(name.getBuffer(), "p%u", &index) == 1) && (index < 16))
                initialP[index] = value;
            else if (t.m_name == "ffr")
                initialFfr = value;
            else if (t.m_name == "sp")
                initialSp = value.getWord(0);
            else if (t.m_name == "nzcv")
                initialNzcv = (uint)value.getWord(0);
            else if (t.m_name == "fpcr")
                initialFpcr = value.getWord(0);
            else if (t.m_name == "fpsr")
                initialFpsr = value.getWord(0);
            else
                TESTS_ASSERT(false);
        }

        // Decode and lift
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        for (uint i = 0; i < row.m_count; i++)
        {
            ArmOpcodePtr opcode = ArmStreamDisassembler::decode(
                    ArmInstructionSet::ARM_A64, row.m_words[i], 4, pc + 4 * i,
                    ArmFeatureSet::all());
            if (opcode.getPointer() == NULL)
            {
                fail(row, "the word doesn't decode");
                return;
            }
            lifter.lift(OpcodePtr((Opcode*)opcode.getPointer(), SMARTPTR_DESTRUCT_NONE), block);
        }
        block.setFallThroughAddress(ProcessorAddress(ProcessorAddress::PROCESSOR_64,
                                                     pc + 4 * row.m_count));

        for (uint i = 0; i < 31; i++)
            interpreter.setRegister(ARM64_REG_X0 + i, SemanticValue(initialX[i]));
        interpreter.setRegister(ARM64_REG_SP, SemanticValue(initialSp));
        interpreter.setRegister(ARM64_REG_PC, SemanticValue(pc));
        interpreter.setRegister(ARM64_REG_N, SemanticValue((initialNzcv >> 3) & 1));
        interpreter.setRegister(ARM64_REG_Z, SemanticValue((initialNzcv >> 2) & 1));
        interpreter.setRegister(ARM64_REG_C, SemanticValue((initialNzcv >> 1) & 1));
        interpreter.setRegister(ARM64_REG_V, SemanticValue(initialNzcv & 1));
        interpreter.setRegister(ARM64_REG_FPCR, SemanticValue(initialFpcr));
        interpreter.setRegister(ARM64_REG_FPSR, SemanticValue(initialFpsr));
        for (uint i = 0; i < 32; i++)
            interpreter.setRegister(ARM64_REG_Z0 + i, initialZ[i]);
        for (uint i = 0; i < 16; i++)
            interpreter.setRegister(ARM64_REG_P0 + i, initialP[i]);
        interpreter.setRegister(ARM64_REG_FFR, initialFfr);
        const SemanticInterpreter::ExitReason reason = interpreter.run(block);

        // The expected state
        cString exitName = "fall";
        bool hasX[31], hasZ[32], hasP[16];
        uint64 expectedX[31];
        SemanticValue expectedZ[32], expectedP[16], expectedFfr;
        bool hasFfr = false, hasSp = false, hasNzcv = false, hasFpsr = false;
        uint64 expectedSp = 0, expectedFpsr = 0;
        uint expectedNzcv = 0;
        cHash<uint64, uint8> expectedMemory;
        for (uint i = 0; i < 31; i++)
            hasX[i] = false;
        for (uint i = 0; i < 32; i++)
            hasZ[i] = false;
        for (uint i = 0; i < 16; i++)
            hasP[i] = false;
        const uint expectedCount = split(row.m_expected, tokens, 96);
        for (uint i = 0; i < expectedCount; i++)
        {
            const Token& t = tokens[i];
            unsigned int index = 0;
            const cSArray<char> ascii = cString(t.m_name).getASCIIstring();
            if (t.m_name == "exit")
            {
                exitName = cString(cString(t.m_value).left((uint)(t.m_end - t.m_value)));
                continue;
            }
            if (t.m_name == "mem")
            {
                parseMemory(t.m_value, t.m_end, memory, &expectedMemory);
                continue;
            }
            const SemanticValue value = parseHex(t.m_value, t.m_end);
            if (sscanf(ascii.getBuffer(), "x%u", &index) == 1)
            {
                hasX[index] = true;
                expectedX[index] = value.getWord(0);
            } else if (sscanf(ascii.getBuffer(), "z%u", &index) == 1)
            {
                hasZ[index] = true;
                expectedZ[index] = value;
            } else if (sscanf(ascii.getBuffer(), "p%u", &index) == 1)
            {
                hasP[index] = true;
                expectedP[index] = value;
            } else if (t.m_name == "ffr")
            {
                hasFfr = true;
                expectedFfr = value;
            } else if (t.m_name == "sp")
            {
                hasSp = true;
                expectedSp = value.getWord(0);
            } else if (t.m_name == "nzcv")
            {
                hasNzcv = true;
                expectedNzcv = (uint)value.getWord(0);
            } else if (t.m_name == "fpsr")
            {
                hasFpsr = true;
                expectedFpsr = value.getWord(0);
            } else
            {
                TESTS_ASSERT(false);
            }
        }

        // The exit
        cString actualExit;
        switch (reason)
        {
        case SemanticInterpreter::EXIT_FALL_THROUGH: actualExit = "fall"; break;
        case SemanticInterpreter::EXIT_BRANCH: actualExit = "branch"; break;
        case SemanticInterpreter::EXIT_MEMORY_FAULT: actualExit = "fault"; break;
        case SemanticInterpreter::EXIT_UNDEFINED: actualExit = "undefined"; break;
        case SemanticInterpreter::EXIT_TRAP:
            actualExit = (interpreter.getTrapKind() == SemanticOperation::TRAP_UNDEFINED) ?
                         "trap:undef" : "trap:other";
            break;
        default: actualExit = "other"; break;
        }
        if (!(actualExit == exitName))
        {
            char message[256];
            snprintf(message, sizeof(message), "exit %s, expected %s",
                     actualExit.getASCIIstring().getBuffer(),
                     exitName.getASCIIstring().getBuffer());
            fail(row, message);
            return;
        }
        if (exitName != "fall")
            return;

        // The registers
        for (uint i = 0; i < 31; i++)
        {
            const uint64 wanted = hasX[i] ? expectedX[i] : initialX[i];
            const uint64 actual = interpreter.getRegister(ARM64_REG_X0 + i).getWord(0);
            if (actual != wanted)
            {
                char message[256];
                snprintf(message, sizeof(message), "x%u = %llx, expected %llx", (unsigned)i,
                         (unsigned long long)actual, (unsigned long long)wanted);
                fail(row, message);
            }
        }
        if (interpreter.getRegister(ARM64_REG_SP).getWord(0) != (hasSp ? expectedSp : initialSp))
            fail(row, "sp");
        uint nzcv = 0;
        if (interpreter.getRegister(ARM64_REG_N).getBits(0, 1) != 0) nzcv |= 8;
        if (interpreter.getRegister(ARM64_REG_Z).getBits(0, 1) != 0) nzcv |= 4;
        if (interpreter.getRegister(ARM64_REG_C).getBits(0, 1) != 0) nzcv |= 2;
        if (interpreter.getRegister(ARM64_REG_V).getBits(0, 1) != 0) nzcv |= 1;
        if (nzcv != (hasNzcv ? expectedNzcv : initialNzcv))
        {
            char message[256];
            snprintf(message, sizeof(message), "nzcv = %x, expected %x", (unsigned)nzcv,
                     (unsigned)(hasNzcv ? expectedNzcv : initialNzcv));
            fail(row, message);
        }
        if (interpreter.getRegister(ARM64_REG_FPCR).getWord(0) != initialFpcr)
            fail(row, "fpcr");
        if (interpreter.getRegister(ARM64_REG_FPSR).getWord(0) != (hasFpsr ? expectedFpsr : initialFpsr))
        {
            char message[256];
            snprintf(message, sizeof(message), "fpsr = %llx, expected %llx",
                     (unsigned long long)interpreter.getRegister(ARM64_REG_FPSR).getWord(0),
                     (unsigned long long)(hasFpsr ? expectedFpsr : initialFpsr));
            fail(row, message);
        }
        for (uint i = 0; i < 32; i++)
        {
            SemanticValue actual = interpreter.getRegister(ARM64_REG_Z0 + i);
            SemanticValue wanted = hasZ[i] ? expectedZ[i] : initialZ[i];
            actual.truncate(vl);
            if (actual != wanted)
            {
                char name[16];
                snprintf(name, sizeof(name), "z%u", (unsigned)i);
                failValue(row, name, actual, wanted, vl);
            }
        }
        for (uint i = 0; i < 16; i++)
        {
            SemanticValue actual = interpreter.getRegister(ARM64_REG_P0 + i);
            SemanticValue wanted = hasP[i] ? expectedP[i] : initialP[i];
            actual.truncate(vl / 8);
            if (actual != wanted)
            {
                char name[16];
                snprintf(name, sizeof(name), "p%u", (unsigned)i);
                failValue(row, name, actual, wanted, vl / 8 < 64 ? 64 : vl / 8);
            }
        }
        {
            SemanticValue actual = interpreter.getRegister(ARM64_REG_FFR);
            SemanticValue wanted = hasFfr ? expectedFfr : initialFfr;
            actual.truncate(vl / 8);
            if (actual != wanted)
                failValue(row, "ffr", actual, wanted, vl / 8 < 64 ? 64 : vl / 8);
        }

        // The memory: the expected bytes, and nothing else changed
        cList<uint64> keys = expectedMemory.keys();
        for (cList<uint64>::iterator i = keys.begin(); i != keys.end(); i++)
        {
            if (memory.getByte(*i) != expectedMemory[*i])
            {
                char message[256];
                snprintf(message, sizeof(message), "mem[%llx] = %02x, expected %02x",
                         (unsigned long long)*i, memory.getByte(*i), expectedMemory[*i]);
                fail(row, message);
            }
        }
        cList<uint64> written = memory.getWritten();
        for (cList<uint64>::iterator i = written.begin(); i != written.end(); i++)
        {
            if (!expectedMemory.hasKey(*i))
            {
                char message[256];
                snprintf(message, sizeof(message), "stray write to %llx", (unsigned long long)*i);
                fail(row, message);
                break;
            }
        }
    }

    virtual void test()
    {
        uint rows = 0;
        for (const Arm64SveSemanticRow* row = gArm64SveSemanticRows; row->m_text != NULL; row++)
        {
            runRow(*row);
            rows++;
        }
        cout << rows << " A64 SVE semantic rows run, " << m_failures << " failures" << endl;
        TESTS_ASSERT(m_failures == 0);
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    uint m_failures;
};

// Instance test object
TestObjectTestArm64SemanticsSve g_globalTestArm64SemanticsSve;
