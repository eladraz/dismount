/*
 * dismount_cli.cpp
 *
 * Line-oriented driver over libdismount for the IA-32 compatibility tests
 * (see TEST_PLAN.md). Reads stdin, writes one record per input line to stdout.
 *
 *   dismount_cli asm [--bits 16|32]   each line: one instruction (nasm syntax,
 *                                     ';' comments allowed)
 *                                     -> "OK <hex bytes>" | "ERR <message> [file]"
 *   dismount_cli dis [--bits 16|32]   each line: hex bytes of one instruction
 *                                     -> "OK <len> <text>" |
 *                                        "INVALID <len> <text>" |
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
    fprintf(stderr, "usage: dismount_cli asm|dis [--bits 16|32] < input\n"
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
    if (strcmp(argv[1], "cpuinfo") == 0)
        return runCpuinfo();

    usage();
    return 2;
}
