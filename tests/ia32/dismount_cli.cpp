/*
 * dismount_cli.cpp
 *
 * Line-oriented driver over libdismount for the IA-32 compatibility tests
 * (see TEST_PLAN.md) and the semantics tests (tests/semantics/TEST_PLAN.md).
 * Reads stdin, writes one record per input line to stdout.
 *
 *   dismount_cli asm [--bits 16|32]   each line: one instruction (nasm syntax,
 *                                     ';' comments allowed)
 *                                     -> "OK <hex bytes>" | "ERR <message> [file]"
 *   dismount_cli dis [--bits 16|32]   each line: hex bytes of one instruction
 *                                     -> "OK <len> <text>" |
 *                                        "INVALID <len> <text>" |
 *                                        "ERR <message>"
 *   dismount_cli run [--bits 16|32]   each line: "<hex bytes of one instruction>
 *                                     ; eax=.. ecx=.. edx=.. ebx=.. esp=..
 *                                       ebp=.. esi=.. edi=.. eip=.. eflags=.."
 *                                     (hex numbers). Executes the instruction
 *                                     as the semantic IR of the lifter, see
 *                                     tests/semantics/TEST_PLAN.md
 *                                     -> "OK exit=<exit> undef=<eflags bits>
 *                                        eax=.. ... eip=.. eflags=.. mem=.." |
 *                                        "ERR <message>"
 *   dismount_cli cpuinfo              host x86 feature flags (Linux names)
 *
 * A bad line never stops the process: every line is wrapped in try/catch.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/sarray.h"
#include "xStl/data/array.h"
#include "xStl/data/list.h"
#include "xStl/data/hash.h"
#include "xStl/except/exception.h"
#include "xStl/exceptions.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/stream/stringerStream.h"

#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/InvalidOpcodeByte.h"
#include "dismount/DisassemblerEndOfStreamException.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/semantics/Semantics.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"

#include "cpufeatures.h"

#define MAX_LINE 8192

/////////////////////////////////////////////////////////////////////////////

// Minimal stack interface, same as tests/TestIA32AssemblerDisassembler.cpp
class CliStackInterface : public StackInterface {
public:
    CliStackInterface(FirstPassBinaryPtr& binary) :
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

// Returns the failure of an exception: its message and the source location
// (file name and line) where it was thrown. The location tells which CHECK of
// the library failed, because the CHECK macros of the release build do not
// carry a message.
static cString describeException(cException& e)
{
    cString where;
    e.whereExceptionOccurred(where);
    cSArray<char> ascii = where.getASCIIstring();
    // Keep the file name only: 'IA32Assembler.cpp:1124'
    const char* text = ascii.getBuffer();
    const char* name = text;
    for (const char* c = text; *c != 0; c++)
        if ((*c == '/') || (*c == '\\'))
            name = c + 1;

    const character* msg = e.getMessage();
    cString result(msg == NULL ? XSTL_STRING("CHECK failed") : msg);
    result += XSTL_STRING(" [");
    result += cString(name);
    result += XSTL_STRING("]");
    return result;
}

// Returns a message without line breaks, safe for a single output record
static cString sanitizeText(const cString& text)
{
    cString out(text);
    cSArray<char> ascii = out.getASCIIstring();
    cString result;
    for (uint i = 0; i + 1 < ascii.getSize(); i++)
    {
        char c = ascii[i];
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        result += cString(c);
    }
    return result;
}

// Returns a message without line breaks, safe for a single output record
static cString sanitize(const character* msg)
{
    return sanitizeText(cString(msg == NULL ? XSTL_STRING("unknown error") : msg));
}

static void writeLine(const cString& text)
{
    cSArray<char> ascii = text.getASCIIstring();
    fwrite(ascii.getBuffer(), 1, ascii.getSize() - 1, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// Strip the trailing line break and trim spaces (comments are handled by the
// assembler itself)
static void normalizeAsmLine(char* line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = 0;
    char* start = line;
    while (*start == ' ' || *start == '\t')
        start++;
    if (start != line)
        memmove(line, start, strlen(start) + 1);
}

/////////////////////////////////////////////////////////////////////////////

// Assemble one instruction. Returns the hex string in 'hex' or throws.
static cString assembleLine(const char* text, IA32eInstructionSet::DisassemblerTypes mode)
{
    FirstPassBinaryPtr binary(new FirstPassBinary(OpcodeSubsystems::DISASSEMBLER_INTEL_32, false));
    binary->createNewBlockWithoutChange(0,
        StackInterfacePtr(new CliStackInterface(binary)));
    binary->changeBasicBlock(0);

    {
        // The IA32AssemblerStream is used directly (not via a temporary
        // cStringerStreamPtr) so that errors are raised by assemble() here
        // instead of being swallowed by the destructor.
        IA32AssemblerStream stream(*binary, mode);
        stream << text << endl;
        stream.assemble();
    }

    const cBuffer& data = binary->getCurrentBlockData();
    cString hex;
    static const char digits[] = "0123456789abcdef";
    for (uint i = 0; i < data.getSize(); i++)
    {
        uint8 b = data[i];
        hex += cString(digits[b >> 4]);
        hex += cString(digits[b & 0xf]);
    }
    return hex;
}

static int runAsm(IA32eInstructionSet::DisassemblerTypes mode)
{
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), stdin))
    {
        normalizeAsmLine(line);
        if (line[0] == 0)
        {
            writeLine(cString("ERR empty instruction"));
            continue;
        }
        XSTL_TRY
        {
            cString hex = assembleLine(line, mode);
            writeLine(cString("OK ") + hex);
        }
        XSTL_CATCH(cException& e)
        {
            writeLine(cString("ERR ") + sanitizeText(describeException(e)));
        }
        XSTL_CATCH_ALL
        {
            writeLine(cString("ERR unknown exception"));
        }
    }
    return 0;
}

/////////////////////////////////////////////////////////////////////////////

static int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses "66 0f 1a" (spaces optional). Returns false on malformed input.
static bool parseHex(const char* text, cBuffer& out)
{
    int nibbles[MAX_LINE];
    int count = 0;
    for (const char* p = text; *p; p++)
    {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            continue;
        int v = hexValue(*p);
        if (v < 0 || count >= MAX_LINE)
            return false;
        nibbles[count++] = v;
    }
    if (count == 0 || (count % 2) != 0)
        return false;
    out.changeSize(count / 2);
    for (int i = 0; i < count / 2; i++)
        out[i] = (uint8)((nibbles[2 * i] << 4) | nibbles[2 * i + 1]);
    return true;
}

static cString disassembleLine(const char* text, OpcodeSubsystems::DisassemblerType type,
                               bool& invalid)
{
    cBuffer bytes;
    if (!parseHex(text, bytes))
        XSTL_THROW(cException, XSTL_STRING("bad hex input"), 0);

    cMemoryStream stream(bytes);
    StreamDisassemblerPtr disassembler =
        StreamDisassemblerFactory::disassemble(type,
                                               BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE));
    OpcodePtr opcode = disassembler->next();
    DefaultOpcodeDataFormatter formatter(1);
    OpcodeFormatterPtr fr = disassembler->getOpcodeFormat(opcode, formatter);
    // Invalid-opcode objects: the fault-tolerant InvalidOpcodeByte, or a table
    // entry flagged FLOW_INVALID (its mnemonic is "***")
    invalid = (dynamic_cast<InvalidOpcodeByte*>(&*opcode) != NULL) ||
              ((opcode->getAlterProperty() & Opcode::FLOW_INVALID) != 0);

    cString result(cString(invalid ? "INVALID " : "OK ") +
                   cString((int32)opcode->getOpcodeSize()) + " " + fr->string());
    return result;
}

static int runDis(OpcodeSubsystems::DisassemblerType type)
{
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), stdin))
    {
        XSTL_TRY
        {
            bool invalid = false;
            cString result = disassembleLine(line, type, invalid);
            writeLine(result);
        }
        XSTL_CATCH(DisassemblerEndOfStreamException&)
        {
            writeLine(cString("ERR truncated instruction (end of stream)"));
        }
        XSTL_CATCH(cException& e)
        {
            writeLine(cString("ERR ") + sanitize(e.getMessage()));
        }
        XSTL_CATCH_ALL
        {
            writeLine(cString("ERR unknown exception"));
        }
    }
    return 0;
}

/////////////////////////////////////////////////////////////////////////////

// The memory of the run command: unlimited, every byte that wasn't written is
// the memory pattern (see tests/semantics/TEST_PLAN.md), and the bytes of the
// instruction replace the pattern at its address. The written bytes are kept.
class PatternMemory : public SemanticMemory {
public:
    PatternMemory(const cBuffer& code, uint32 codeAddress) :
        m_code(code),
        m_codeAddress(codeAddress)
    {
    }

    virtual bool read(uint64 address, uint8* buffer, uint size)
    {
        for (uint i = 0; i < size; i++)
            buffer[i] = getByte((uint32)(address + i));
        return true;
    }

    virtual bool write(uint64 address, const uint8* buffer, uint size)
    {
        for (uint i = 0; i < size; i++)
        {
            uint32 byteAddress = (uint32)(address + i);
            if (m_written.hasKey(byteAddress))
                m_written[byteAddress] = buffer[i];
            else
                m_written.append(byteAddress, buffer[i]);
        }
        return true;
    }

    // Return the written bytes as "address:bytes" runs sorted by address, or
    // "-" if nothing was written
    cString getWrittenRuns() const
    {
        cList<uint32> keys = m_written.keys();
        cArray<uint32> addresses(keys.length());
        uint count = 0;
        for (cList<uint32>::iterator i = keys.begin(); i != keys.end(); i++)
        {
            // Insertion sort
            uint position = count++;
            while ((position > 0) && (addresses[position - 1] > *i))
            {
                addresses[position] = addresses[position - 1];
                position--;
            }
            addresses[position] = *i;
        }
        if (count == 0)
            return cString("-");

        cString result;
        for (uint i = 0; i < count; i++)
        {
            // A new run, unless the byte follows the previous one
            if ((i == 0) || (addresses[i] != addresses[i - 1] + 1))
            {
                if (i != 0)
                    result += cString(",");
                result += hexString(addresses[i], 8) + cString(":");
            }
            result += hexString(m_written[addresses[i]], 2);
        }
        return result;
    }

    // Return 'digits' hexadecimal digits of 'value'
    static cString hexString(uint32 value, int digits)
    {
        char text[16];
        snprintf(text, sizeof(text), "%0*x", digits, (unsigned int)value);
        return cString(text);
    }

private:
    // byte(A) = ((A * 2654435761) mod 2^32) >> 24
    uint8 getByte(uint32 address) const
    {
        if (m_written.hasKey(address))
            return m_written[address];
        if ((uint32)(address - m_codeAddress) < m_code.getSize())
            return m_code[address - m_codeAddress];
        return (uint8)((uint32)(address * 2654435761u) >> 24);
    }

    // The bytes of the instruction, at m_codeAddress
    cBuffer m_code;
    uint32 m_codeAddress;
    // The written bytes
    cHash<uint32, uint8> m_written;
};

// The general purpose registers of the state of the run command, in the order
// of the protocol
static const struct {
    const char* m_name;
    uint m_register;
} gStateRegisters[] = {
    { "eax", ia32dis::IA32_REG_EAX }, { "ecx", ia32dis::IA32_REG_ECX },
    { "edx", ia32dis::IA32_REG_EDX }, { "ebx", ia32dis::IA32_REG_EBX },
    { "esp", ia32dis::IA32_REG_ESP }, { "ebp", ia32dis::IA32_REG_EBP },
    { "esi", ia32dis::IA32_REG_ESI }, { "edi", ia32dis::IA32_REG_EDI }
};
static const uint gStateRegistersCount = sizeof(gStateRegisters) /
                                         sizeof(gStateRegisters[0]);

// The flag registers and their bits in EFLAGS
static const struct {
    uint m_register;
    uint m_bit;
} gStateFlags[] = {
    { ia32dis::IA32_REG_CF, 0 }, { ia32dis::IA32_REG_PF, 2 },
    { ia32dis::IA32_REG_AF, 4 }, { ia32dis::IA32_REG_ZF, 6 },
    { ia32dis::IA32_REG_SF, 7 }, { ia32dis::IA32_REG_IF, 9 },
    { ia32dis::IA32_REG_DF, 10 }, { ia32dis::IA32_REG_OF, 11 },
    { ia32dis::IA32_REG_AC, 18 }, { ia32dis::IA32_REG_ID, 21 }
};
static const uint gStateFlagsCount = sizeof(gStateFlags) / sizeof(gStateFlags[0]);

// The bit of EFLAGS of each semantic flag
static uint32 getEflagsBits(uint32 semanticFlags)
{
    static const struct {
        uint32 m_flag;
        uint32 m_bit;
    } flags[] = {
        { SemanticFlags::FLAG_CARRY,     0x001 }, { SemanticFlags::FLAG_PARITY,    0x004 },
        { SemanticFlags::FLAG_ADJUST,    0x010 }, { SemanticFlags::FLAG_ZERO,      0x040 },
        { SemanticFlags::FLAG_SIGN,      0x080 }, { SemanticFlags::FLAG_DIRECTION, 0x400 },
        { SemanticFlags::FLAG_OVERFLOW,  0x800 }
    };
    uint32 result = 0;
    for (uint i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
        if ((semanticFlags & flags[i].m_flag) != 0)
            result |= flags[i].m_bit;
    return result;
}

// The state of an input line, see parseState()
struct RunState {
    uint32 m_registers[8];
    uint32 m_eip;
    uint32 m_eflags;
};

// Parses "eax=1 ecx=2 ... eip=1000 eflags=202" (hexadecimal numbers). A
// register which isn't there is 0. Returns false on a malformed token.
static bool parseState(const char* text, RunState& state)
{
    memset(&state, 0, sizeof(state));
    state.m_eflags = 2;
    const char* p = text;
    while (*p != 0)
    {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        {
            p++;
            continue;
        }
        const char* equals = strchr(p, '=');
        if (equals == NULL)
            return false;
        cString name(cString(p).left((uint)(equals - p)));
        char* end = NULL;
        uint32 value = (uint32)strtoul(equals + 1, &end, 16);
        if (end == equals + 1)
            return false;

        bool found = false;
        for (uint i = 0; i < gStateRegistersCount; i++)
            if (name == gStateRegisters[i].m_name)
            {
                state.m_registers[i] = value;
                found = true;
            }
        if (name == "eip")
        {
            state.m_eip = value;
            found = true;
        } else if (name == "eflags")
        {
            state.m_eflags = value;
            found = true;
        }
        if (!found)
            return false;
        p = end;
    }
    return true;
}

// Executes one instruction as the IR of the lifter from the state of the line
// ("<hex bytes> ; <state>"), and returns the result record
static cString runLine(char* line, OpcodeSubsystems::DisassemblerType type,
                       const SemanticLifterPtr& lifter)
{
    char* separator = strchr(line, ';');
    if (separator == NULL)
        XSTL_THROW(cException, XSTL_STRING("missing ';' before the state"), 0);
    *separator = 0;
    cBuffer bytes;
    if (!parseHex(line, bytes))
        XSTL_THROW(cException, XSTL_STRING("bad hex input"), 0);
    RunState state;
    if (!parseState(separator + 1, state))
        XSTL_THROW(cException, XSTL_STRING("bad state"), 0);

    // Decode the instruction at eip
    const bool is16 = (type == OpcodeSubsystems::DISASSEMBLER_INTEL_16);
    const ProcessorAddress address(is16 ? ProcessorAddress::PROCESSOR_20 :
                                          ProcessorAddress::PROCESSOR_32,
                                   state.m_eip);
    cMemoryStream stream(bytes);
    StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
            type, BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true, address);
    OpcodePtr opcode = disassembler->next();

    // Lift it
    SemanticBlock block(type);
    lifter->lift(opcode, block);
    block.setFallThroughAddress(address + opcode->getOpcodeSize());

    // Execute it from the state
    PatternMemory memory(bytes, state.m_eip);
    SemanticInterpreter interpreter(lifter->getArchitecture(), memory);
    for (uint i = 0; i < gStateRegistersCount; i++)
        interpreter.setRegister(gStateRegisters[i].m_register,
                                SemanticValue(state.m_registers[i]));
    interpreter.setRegister(ia32dis::IA32_REG_EIP, SemanticValue(state.m_eip));
    for (uint i = 0; i < gStateFlagsCount; i++)
        interpreter.setRegister(gStateFlags[i].m_register,
                SemanticValue((state.m_eflags >> gStateFlags[i].m_bit) & 1));
    const SemanticInterpreter::ExitReason reason = interpreter.run(block);

    cString exitText;
    switch (reason)
    {
    case SemanticInterpreter::EXIT_BRANCH:
        exitText = "branch";
        break;
    case SemanticInterpreter::EXIT_FALL_THROUGH:
        exitText = "fall";
        break;
    case SemanticInterpreter::EXIT_TRAP:
        exitText = cString("trap:") + cString((int32)interpreter.getTrapNumber());
        break;
    case SemanticInterpreter::EXIT_MEMORY_FAULT:
        exitText = cString("fault:") +
                   PatternMemory::hexString((uint32)interpreter.getFaultAddress(), 8);
        break;
    case SemanticInterpreter::EXIT_HELPER:
        exitText = cString("helper:") + cString((int32)interpreter.getHelperNumber());
        break;
    case SemanticInterpreter::EXIT_UNDEFINED:
        exitText = "undefined";
        break;
    default:
        XSTL_THROW(cException, XSTL_STRING("the block didn't finish"), 0);
    }

    // The flags the instruction leaves undefined
    const uint32 undefined = getEflagsBits(
                    lifter->classify(opcode).getFlagsUndefined());

    // EFLAGS from the flag registers, bit 1 is always set
    uint32 eflags = 2;
    for (uint i = 0; i < gStateFlagsCount; i++)
        if (interpreter.getRegister(gStateFlags[i].m_register).getBits(0, 1) != 0)
            eflags |= (uint32)1 << gStateFlags[i].m_bit;

    cString result = cString("OK exit=") + exitText + cString(" undef=") +
                     PatternMemory::hexString(undefined, 8);
    for (uint i = 0; i < gStateRegistersCount; i++)
        result += cString(" ") + cString(gStateRegisters[i].m_name) + cString("=") +
                  PatternMemory::hexString((uint32)interpreter.getRegister(
                        gStateRegisters[i].m_register).getWord(0), 8);
    result += cString(" eip=") + PatternMemory::hexString((uint32)interpreter.
                    getRegister(ia32dis::IA32_REG_EIP).getWord(0), 8);
    result += cString(" eflags=") + PatternMemory::hexString(eflags, 8);
    result += cString(" mem=") + memory.getWrittenRuns();
    return result;
}

static int runRun(OpcodeSubsystems::DisassemblerType type)
{
    SemanticLifterPtr lifter = SemanticLifterFactory::getLifter(type);
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), stdin))
    {
        XSTL_TRY
        {
            writeLine(runLine(line, type, lifter));
        }
        XSTL_CATCH(DisassemblerEndOfStreamException&)
        {
            writeLine(cString("ERR truncated instruction (end of stream)"));
        }
        XSTL_CATCH(cException& e)
        {
            writeLine(cString("ERR ") + sanitizeText(describeException(e)));
        }
        XSTL_CATCH_ALL
        {
            writeLine(cString("ERR unknown exception"));
        }
    }
    return 0;
}

/////////////////////////////////////////////////////////////////////////////

static int runCpuinfo()
{
    char buf[4096];
#if DM_CPU_X86
    if (dm_cpu_features(buf, sizeof(buf)) > 0)
        printf("%s\n", buf);
    else
        printf("none (no CPUID)\n");
#else
#  if defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "arm64";
#  elif defined(__arm__) || defined(_M_ARM)
    const char* arch = "arm";
#  else
    const char* arch = "unknown";
#  endif
    dm_cpu_features(buf, sizeof(buf));
    printf("none (non-x86 host: %s)\n", arch);
#endif
    return 0;
}

static void usage()
{
    fprintf(stderr, "usage: dismount_cli asm|dis|run [--bits 16|32] < input\n"
                    "       dismount_cli cpuinfo\n");
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        usage();
        return 2;
    }

    IA32eInstructionSet::DisassemblerTypes asmMode = IA32eInstructionSet::INTEL_32;
    OpcodeSubsystems::DisassemblerType disType = OpcodeSubsystems::DISASSEMBLER_INTEL_32;
    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--bits") == 0 && i + 1 < argc)
        {
            i++;
            if (strcmp(argv[i], "16") == 0)
            {
                asmMode = IA32eInstructionSet::INTEL_16;
                disType = OpcodeSubsystems::DISASSEMBLER_INTEL_16;
            }
            else if (strcmp(argv[i], "32") == 0)
            {
                asmMode = IA32eInstructionSet::INTEL_32;
                disType = OpcodeSubsystems::DISASSEMBLER_INTEL_32;
            }
            else
            {
                usage();
                return 2;
            }
        }
        else
        {
            usage();
            return 2;
        }
    }

    if (strcmp(argv[1], "asm") == 0)
        return runAsm(asmMode);
    if (strcmp(argv[1], "dis") == 0)
        return runDis(disType);
    if (strcmp(argv[1], "run") == 0)
        return runRun(disType);
    if (strcmp(argv[1], "cpuinfo") == 0)
        return runCpuinfo();

    usage();
    return 2;
}
