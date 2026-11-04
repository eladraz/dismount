/*
 * dismount_cli.cpp
 *
 * Line-oriented driver over libdismount for the IA-32 compatibility tests
 * (see TEST_PLAN.md) and the semantics tests (tests/semantics/TEST_PLAN.md).
 * Reads stdin, writes one record per input line to stdout.
 *
 *   dismount_cli asm [--bits 16|32|64] each line: one instruction (nasm syntax,
 *                                     ';' comments allowed)
 *                                     -> "OK <hex bytes>" | "ERR <message> [file]"
 *   dismount_cli dis [--bits 16|32|64] each line: hex bytes of one instruction
 *                                     -> "OK <len> <text>" |
 *                                        "INVALID <len> <text>" |
 *                                        "ERR <message>"
 *   dismount_cli run [--bits 16|32|64] each line: "<hex bytes of one instruction>
 *                                     ; eax=.. ecx=.. edx=.. ebx=.. esp=..
 *                                       ebp=.. esi=.. edi=.. eip=.. eflags=.."
 *                                     (hex numbers; more tokens select the
 *                                     segments, the x87 and SSE state and
 *                                     the memory; in 64 bit mode rax..r15, rip,
 *                                     fs_base, gs_base, xmm0..xmm15 and 64 bit
 *                                     addresses). Executes the instruction
 *                                     as the semantic IR of the lifter, see
 *                                     "State v2" in tests/semantics/TEST_PLAN.md
 *                                     -> "OK exit=<exit> undef=<eflags bits>
 *                                        fundef=<fsw bits> eax=.. ... eip=..
 *                                        eflags=.. cs=.. ... mem=.." |
 *                                        "ERR <message>"
 *   dismount_cli cpuinfo              host x86 feature flags (Linux names)
 *
 * ARM: --arch a32|t32|a64 (instead of --bits) selects the instruction set and
 * --mattr <llvm feature list> the features of the target (default: all of
 * them; "none" resets to the base, "+lse,-sve" adds and removes, see
 * ArmFeatureSet::parse). See tests/arm/TEST_PLAN.md:
 *   dismount_cli dis --arch a64       each line: [@<hex address> ]<hex word>
 *                                     (T32: the hex of the halfwords, "eb01 0002"
 *                                     or "eb010002", "1888" for 16 bits)
 *                                     -> "OK <size> <text>" | "INVALID <size>" |
 *                                        "ERR <message>"
 *   dismount_cli asm --arch a64       each line: [@<hex address> ]<instruction>
 *                                     -> "OK <hex bytes>" | "ERR <message>"
 *   dismount_cli run --arch a64       each line: "<hex bytes of the code> ; x0=.. sp=..
 *                                     pc=.. nzcv=.. v0=.. mem=.." (a32 / t32:
 *                                     r0..r12 sp lr pc apsr fpscr q0..q15).
 *                                     Executes the instructions as the IR of
 *                                     the lifter, see "The run protocol" in
 *                                     tests/arm/semantics/TEST_PLAN.md
 *                                     -> "OK exit=<exit> x0=.. ... mem=.." |
 *                                        "ERR <message>"
 *   dismount_cli features --arch a64  the support matrix, one line per feature:
 *                                     "<index> <name> <set> <support hex> <entries>"
 *   dismount_cli entries --arch a64   the encodings of the tables, one line per
 *                                     entry: "<mask hex> <value hex> <feature
 *                                     index> <flags hex> <size> <mnemonic>"
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
#include "dismount/DisassemblerInvalidOpcodeException.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/ia32/IA32Semantics.h"
#include "dismount/semantics/Semantics.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmEncodingTable.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/assembler/proc/arm/ArmAssembler.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvEncodingTable.h"
#include "dismount/proc/riscv/RiscvNotation.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"
#include "dismount/assembler/proc/riscv/RiscvAssembler.h"

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
// the memory pattern (see tests/semantics/TEST_PLAN.md), the bytes the line
// gives with mem= replace the pattern, and the bytes of the instruction
// replace both at its address. The written bytes are kept. The addresses are
// 32 bits in 16 and 32 bit mode (an access wraps around 4GB) and 64 bits in 64
// bit mode.
class PatternMemory : public SemanticMemory {
public:
    PatternMemory(const cBuffer& code, uint64 codeAddress, bool is64) :
        m_code(code),
        m_mask(is64 ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFULL),
        m_codeAddress(codeAddress & m_mask),
        m_digits(is64 ? 16 : 8)
    {
    }

    // Set a byte of the memory before the run (mem=, the GDT)
    void setInitial(uint64 address, uint8 value)
    {
        address &= m_mask;
        if (m_initial.hasKey(address))
            m_initial[address] = value;
        else
            m_initial.append(address, value);
    }

    virtual bool read(uint64 address, uint8* buffer, uint size)
    {
        for (uint i = 0; i < size; i++)
            buffer[i] = getByte((address + i) & m_mask);
        return true;
    }

    virtual bool write(uint64 address, const uint8* buffer, uint size)
    {
        for (uint i = 0; i < size; i++)
        {
            uint64 byteAddress = (address + i) & m_mask;
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
        cList<uint64> keys = m_written.keys();
        cArray<uint64> addresses(keys.length());
        uint count = 0;
        for (cList<uint64>::iterator i = keys.begin(); i != keys.end(); i++)
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
                result += hexString(addresses[i], m_digits) + cString(":");
            }
            result += hexString(m_written[addresses[i]], 2);
        }
        return result;
    }

    // Return 'digits' hexadecimal digits of 'value'
    static cString hexString(uint64 value, int digits)
    {
        char text[32];
        snprintf(text, sizeof(text), "%0*llx", digits,
                 (unsigned long long)value);
        return cString(text);
    }

private:
    // byte(A) = ((A * 2654435761) mod 2^32) >> 24
    uint8 getByte(uint64 address) const
    {
        if (m_written.hasKey(address))
            return m_written[address];
        if ((uint64)((address - m_codeAddress) & m_mask) < m_code.getSize())
            return m_code[(uint)((address - m_codeAddress) & m_mask)];
        if (m_initial.hasKey(address))
            return m_initial[address];
        return (uint8)((uint32)(address * 2654435761u) >> 24);
    }

    // The bytes of the instruction, at m_codeAddress
    cBuffer m_code;
    // The mask of the addresses
    uint64 m_mask;
    uint64 m_codeAddress;
    // The number of hexadecimal digits of an address
    int m_digits;
    // The initial bytes
    cHash<uint64, uint8> m_initial;
    // The written bytes
    cHash<uint64, uint8> m_written;
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

// The same for the 64 bit mode
static const struct {
    const char* m_name;
    uint m_register;
} gStateRegisters64[] = {
    { "rax", ia32dis::AMD64_REG_RAX }, { "rcx", ia32dis::AMD64_REG_RCX },
    { "rdx", ia32dis::AMD64_REG_RDX }, { "rbx", ia32dis::AMD64_REG_RBX },
    { "rsp", ia32dis::AMD64_REG_RSP }, { "rbp", ia32dis::AMD64_REG_RBP },
    { "rsi", ia32dis::AMD64_REG_RSI }, { "rdi", ia32dis::AMD64_REG_RDI },
    { "r8", ia32dis::AMD64_REG_R8 }, { "r9", ia32dis::AMD64_REG_R9 },
    { "r10", ia32dis::AMD64_REG_R10 }, { "r11", ia32dis::AMD64_REG_R11 },
    { "r12", ia32dis::AMD64_REG_R12 }, { "r13", ia32dis::AMD64_REG_R13 },
    { "r14", ia32dis::AMD64_REG_R14 }, { "r15", ia32dis::AMD64_REG_R15 }
};
static const uint gStateRegisters64Count = sizeof(gStateRegisters64) /
                                           sizeof(gStateRegisters64[0]);

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

// The segment registers of the state, in the order of the protocol and of the
// ia32dis::IA32_REG_ES.. ids
static const char* const gStateSegments[] = {
    "es", "cs", "ss", "ds", "fs", "gs"
};
static const uint gStateSegmentsCount = sizeof(gStateSegments) /
                                        sizeof(gStateSegments[0]);

// The fixed GDT of the 32 bit mode ("State v2" in tests/semantics/TEST_PLAN.md),
// at the linear address GDT_ADDRESS. The selector is the index * 8
enum {
    GDT_ADDRESS = 0x000f0000,
    GDT_LIMIT = 0x4f
};
static const uint8 gGdt[] = {
    // 0x00 null
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x08 code 32 bit, DPL 0, base 0, limit 4G, execute/read
    0xff, 0xff, 0x00, 0x00, 0x00, 0x9a, 0xcf, 0x00,
    // 0x10 data, DPL 0, base 0, limit 4G, read/write
    0xff, 0xff, 0x00, 0x00, 0x00, 0x92, 0xcf, 0x00,
    // 0x18 data, DPL 0, base 0x10000000, limit 4G, read/write, accessed
    0xff, 0xff, 0x00, 0x00, 0x00, 0x93, 0xcf, 0x10,
    // 0x20 code 32 bit, DPL 0, base 0x2000, limit 4G, execute/read
    0xff, 0xff, 0x00, 0x20, 0x00, 0x9a, 0xcf, 0x00,
    // 0x28 code 16 bit, DPL 0, base 0, limit 0xffff, execute/read
    0xff, 0xff, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00,
    // 0x30 data, DPL 0, base 0, limit 4G, read only
    0xff, 0xff, 0x00, 0x00, 0x00, 0x90, 0xcf, 0x00,
    // 0x38 data, DPL 0, base 0, limit 4G, read/write, not present
    0xff, 0xff, 0x00, 0x00, 0x00, 0x12, 0xcf, 0x00,
    // 0x40 data, DPL 3, base 0x20000000, limit 4G, read/write
    0xff, 0xff, 0x00, 0x00, 0x00, 0xf2, 0xcf, 0x20,
    // 0x48 32 bit call gate, DPL 3, to 0x08:0x00003000
    0x00, 0x30, 0x08, 0x00, 0x00, 0xec, 0x00, 0x00
};
static const uint gGdtCount = sizeof(gGdt) / 8;

// The fixed GDT of the 64 bit mode, at the same address, see
// tests/semantics/TEST_PLAN.md. The descriptors of the long mode: L (bit 53)
// is the 0xa nibble of the flags, a 32 bit code segment has D (0xc)
enum {
    GDT64_LIMIT = 0x97
};
static const uint8 gGdt64[] = {
    // 0x00 null
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x08 code 64 bit, DPL 0, execute/read
    0xff, 0xff, 0x00, 0x00, 0x00, 0x9a, 0xaf, 0x00,
    // 0x10 data, DPL 0, base 0, read/write
    0xff, 0xff, 0x00, 0x00, 0x00, 0x92, 0xcf, 0x00,
    // 0x18 data, DPL 0, base 0x10000000, read/write, accessed
    0xff, 0xff, 0x00, 0x00, 0x00, 0x93, 0xcf, 0x10,
    // 0x20 code 32 bit (compatibility mode), DPL 0, execute/read
    0xff, 0xff, 0x00, 0x00, 0x00, 0x9a, 0xcf, 0x00,
    // 0x28 code 16 bit, DPL 0 (not lifted)
    0xff, 0xff, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00,
    // 0x30 data, DPL 0, read only
    0xff, 0xff, 0x00, 0x00, 0x00, 0x90, 0xcf, 0x00,
    // 0x38 data, DPL 0, read/write, not present
    0xff, 0xff, 0x00, 0x00, 0x00, 0x12, 0xcf, 0x00,
    // 0x40 data, DPL 3, base 0x20000000, read/write
    0xff, 0xff, 0x00, 0x00, 0x00, 0xf2, 0xcf, 0x20,
    // 0x48 64 bit call gate, DPL 3 (16 bytes, not lifted)
    0x00, 0x30, 0x08, 0x00, 0x00, 0xec, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x58 code 64 bit, DPL 3
    0xff, 0xff, 0x00, 0x00, 0x00, 0xfa, 0xaf, 0x00,
    // 0x60 code 64 bit, conforming, DPL 0, execute/read
    0xff, 0xff, 0x00, 0x00, 0x00, 0x9e, 0xaf, 0x00,
    // 0x68 code with L and D (invalid)
    0xff, 0xff, 0x00, 0x00, 0x00, 0x9a, 0xef, 0x00,
    // 0x70 code 64 bit, execute only
    0xff, 0xff, 0x00, 0x00, 0x00, 0x98, 0xaf, 0x00,
    // 0x78 data, DPL 1, read/write
    0xff, 0xff, 0x00, 0x00, 0x00, 0xb2, 0xcf, 0x00,
    // 0x80 data, DPL 0, base 0x87654321, read/write
    0xff, 0xff, 0x21, 0x43, 0x65, 0x92, 0xcf, 0x87,
    // 0x88 code 64 bit, conforming, DPL 3, execute/read
    0xff, 0xff, 0x00, 0x00, 0x00, 0xfe, 0xaf, 0x00,
    // 0x90 code 32 bit, DPL 0, base 0x2000
    0xff, 0xff, 0x00, 0x20, 0x00, 0x9a, 0xcf, 0x00
};

// Return the base of the segment 'selector' of the initial state: selector *
// 16 in real mode; in protected mode 0 for a null selector, otherwise from the
// fixed GDT
static uint32 getSegmentBase(bool is16, uint32 selector)
{
    if (is16)
        return (selector & 0xFFFF) << 4;
    if ((selector & 0xFFFC) == 0)
        return 0;

    const uint index = (selector & 0xFFFF) >> 3;
    if (((selector & 4) != 0) || (index >= gGdtCount) ||
        ((gGdt[index * 8 + 5] & 0x10) == 0))
        XSTL_THROW(cException, XSTL_STRING("bad selector in the state"), 0);
    const uint8* descriptor = &gGdt[index * 8];
    return (uint32)descriptor[2] | ((uint32)descriptor[3] << 8) |
           ((uint32)descriptor[4] << 16) | ((uint32)descriptor[7] << 24);
}

// The bits of the x87 status word (C0-C3) that the instruction leaves
// undefined, in their fsw bit positions
static uint32 getFpuStatusUndefined(const OpcodeClassification& classification)
{
    return classification.getFloatStatusUndefined();
}

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
    RunState(bool is64) :
        m_is64(is64),
        m_ip(0),
        m_eflags(2),
        m_fsBase(0),
        m_gsBase(0),
        m_kernelGsBase(0),
        m_fcw(0x37f),
        m_fsw(0),
        m_ftw(0),
        m_mxcsr(0x1f80)
    {
        memset(m_registers, 0, sizeof(m_registers));
        memset(m_segments, 0, sizeof(m_segments));
    }

    // 64 bit mode: the registers are rax..r15 and rip, otherwise eax..edi and
    // eip
    bool m_is64;
    uint64 m_registers[16];
    uint64 m_ip;
    uint32 m_eflags;
    // The selectors: es cs ss ds fs gs
    uint32 m_segments[6];
    // 64 bit mode: the bases of fs and gs, and the other gs base (swapgs)
    uint64 m_fsBase;
    uint64 m_gsBase;
    uint64 m_kernelGsBase;
    // The x87 control word, status word and abridged tag word
    uint32 m_fcw;
    uint32 m_fsw;
    uint32 m_ftw;
    // The physical x87 registers (80 bits) and the SSE state
    SemanticValue m_fpr[8];
    uint32 m_mxcsr;
    SemanticValue m_xmm[16];
    // mem=: the bytes written over the memory pattern before the run
    cHash<uint64, uint8> m_memory;
};

// Parses the hexadecimal number text[0..end) of up to 'bits' bits
static bool parseHexWide(const char* text, const char* end, uint bits,
                         SemanticValue& value)
{
    const uint digits = (uint)(end - text);
    if ((digits == 0) || (digits > bits / 4))
        return false;
    value = SemanticValue();
    for (uint i = 0; i < digits; i++)
    {
        const int nibble = hexValue(text[digits - 1 - i]);
        if (nibble < 0)
            return false;
        value.setBits(i * 4, 4, (uint64)nibble);
    }
    return true;
}

// Parses a number of up to 'bits' (at most 64) bits
static bool parseNumber(const char* text, const char* end, uint bits,
                        uint64& value)
{
    SemanticValue wide;
    if (!parseHexWide(text, end, bits, wide))
        return false;
    value = wide.getWord(0);
    return true;
}

// Parses a number of up to 32 bits
static bool parseWord(const char* text, const char* end, uint32& value)
{
    uint64 wide;
    if (!parseNumber(text, end, 32, wide))
        return false;
    value = (uint32)wide;
    return true;
}

// Parses mem=: "ADDR:HEX,ADDR:HEX" (or "-"), the bytes at the addresses
static bool parseMemory(const char* text, const char* end, RunState& state)
{
    if ((end - text == 1) && (*text == '-'))
        return true;
    const uint maxDigits = state.m_is64 ? 16 : 8;
    const uint64 mask = state.m_is64 ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFULL;
    const char* p = text;
    while (p < end)
    {
        uint64 address = 0;
        uint digits = 0;
        while ((p < end) && (hexValue(*p) >= 0))
        {
            address = address * 16 + (uint64)hexValue(*p);
            p++;
            digits++;
        }
        if ((digits == 0) || (digits > maxDigits) || (p >= end) || (*p != ':'))
            return false;
        p++;

        uint count = 0;
        while ((p + 1 < end) && (hexValue(p[0]) >= 0) && (hexValue(p[1]) >= 0))
        {
            const uint64 byteAddress = (address + count) & mask;
            const uint8 byte = (uint8)((hexValue(p[0]) << 4) | hexValue(p[1]));
            if (state.m_memory.hasKey(byteAddress))
                state.m_memory[byteAddress] = byte;
            else
                state.m_memory.append(byteAddress, byte);
            p += 2;
            count++;
        }
        if (count == 0)
            return false;
        if (p < end)
        {
            if ((*p != ',') || (p + 1 >= end))
                return false;
            p++;
        }
    }
    return true;
}

// Parses "eax=1 ecx=2 ... eip=1000 eflags=202 ds=18 fcw=37f r0=3fff8000...
// xmm1=... mem=1000:aabb" (hexadecimal numbers, see "State v2" in
// tests/semantics/TEST_PLAN.md). In 64 bit mode "rax=.. r15=.. rip=.. fs_base=
// gs_base= kernel_gs_base= xmm0..xmm15=" and 64 bit mem= addresses. A token
// which isn't there has its default (registers 0). Returns false on a
// malformed token.
static bool parseState(const char* text, RunState& state)
{
    const uint registersCount = state.m_is64 ? gStateRegisters64Count :
                                               gStateRegistersCount;
    const uint registerBits = state.m_is64 ? 64 : 32;
    const uint xmmCount = state.m_is64 ? 16 : 8;
    const char* p = text;
    while (*p != 0)
    {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        {
            p++;
            continue;
        }
        const char* tokenEnd = p;
        while ((*tokenEnd != 0) && (*tokenEnd != ' ') && (*tokenEnd != '\t') &&
               (*tokenEnd != '\n') && (*tokenEnd != '\r'))
            tokenEnd++;
        const char* equals = (const char*)memchr(p, '=', (size_t)(tokenEnd - p));
        if (equals == NULL)
            return false;
        cString name(cString(p).left((uint)(equals - p)));
        const char* value = equals + 1;

        bool found = false;
        bool ok = true;
        for (uint i = 0; i < registersCount; i++)
            if (name == (state.m_is64 ? gStateRegisters64[i].m_name :
                                        gStateRegisters[i].m_name))
            {
                ok = parseNumber(value, tokenEnd, registerBits,
                                 state.m_registers[i]);
                found = true;
            }
        for (uint i = 0; i < gStateSegmentsCount; i++)
            if (name == gStateSegments[i])
            {
                ok = parseWord(value, tokenEnd, state.m_segments[i]) &&
                     (state.m_segments[i] <= 0xFFFF);
                found = true;
            }
        for (uint i = 0; i < 8; i++)
        {
            if (name == (cString("r") + cString((int32)i)))
            {
                ok = parseHexWide(value, tokenEnd, 80, state.m_fpr[i]);
                found = true;
            }
        }
        for (uint i = 0; i < xmmCount; i++)
        {
            if (name == (cString("xmm") + cString((int32)i)))
            {
                ok = parseHexWide(value, tokenEnd, 128, state.m_xmm[i]);
                found = true;
            }
        }
        if (name == (state.m_is64 ? "rip" : "eip"))
        {
            ok = parseNumber(value, tokenEnd, registerBits, state.m_ip);
            found = true;
        } else if (name == "eflags")
        {
            ok = parseWord(value, tokenEnd, state.m_eflags);
            found = true;
        } else if (state.m_is64 && (name == "fs_base"))
        {
            ok = parseNumber(value, tokenEnd, 64, state.m_fsBase);
            found = true;
        } else if (state.m_is64 && (name == "gs_base"))
        {
            ok = parseNumber(value, tokenEnd, 64, state.m_gsBase);
            found = true;
        } else if (state.m_is64 && (name == "kernel_gs_base"))
        {
            ok = parseNumber(value, tokenEnd, 64, state.m_kernelGsBase);
            found = true;
        } else if (name == "fcw")
        {
            ok = parseWord(value, tokenEnd, state.m_fcw);
            found = true;
        } else if (name == "fsw")
        {
            ok = parseWord(value, tokenEnd, state.m_fsw);
            found = true;
        } else if (name == "ftw")
        {
            ok = parseWord(value, tokenEnd, state.m_ftw);
            found = true;
        } else if (name == "mxcsr")
        {
            ok = parseWord(value, tokenEnd, state.m_mxcsr);
            found = true;
        } else if (name == "mem")
        {
            ok = parseMemory(value, tokenEnd, state);
            found = true;
        }
        if (!found || !ok)
            return false;
        p = tokenEnd;
    }
    return true;
}

// Return the hexadecimal digits of the low 'bits' bits of 'value'
static cString wideHexString(const SemanticValue& value, uint bits)
{
    static const char digits[] = "0123456789abcdef";
    cString result;
    for (uint i = bits / 4; i > 0; i--)
        result += cString(digits[value.getBits((i - 1) * 4, 4)]);
    return result;
}

// Return the 'digits' hexadecimal digits of the register 'registerId'
static cString registerHex(const SemanticInterpreter& interpreter,
                           uint registerId, int digits)
{
    return PatternMemory::hexString(interpreter.getRegister(registerId).
                                    getWord(0), digits);
}

// Executes one instruction as the IR of the lifter from the state of the line
// ("<hex bytes> ; <state>"), and returns the result record
static cString runLine(char* line, OpcodeSubsystems::DisassemblerType type,
                       const SemanticLifter& lifter)
{
    char* separator = strchr(line, ';');
    if (separator == NULL)
        XSTL_THROW(cException, XSTL_STRING("missing ';' before the state"), 0);
    *separator = 0;
    cBuffer bytes;
    if (!parseHex(line, bytes))
        XSTL_THROW(cException, XSTL_STRING("bad hex input"), 0);
    const bool is16 = (type == OpcodeSubsystems::DISASSEMBLER_INTEL_16);
    const bool is64 = (type == OpcodeSubsystems::DISASSEMBLER_AMD_64);
    // The digits of the addresses and of the registers
    const int addressDigits = is64 ? 16 : 8;
    RunState state(is64);
    if (!parseState(separator + 1, state))
        XSTL_THROW(cException, XSTL_STRING("bad state"), 0);

    // The bases of the segments. In 64 bit mode only fs and gs have a base
    uint64 bases[6];
    for (uint i = 0; i < gStateSegmentsCount; i++)
        bases[i] = is64 ? 0 : getSegmentBase(is16, state.m_segments[i]);
    if (is64)
    {
        bases[ia32dis::IA32_REG_FS - ia32dis::IA32_REG_ES] = state.m_fsBase;
        bases[ia32dis::IA32_REG_GS - ia32dis::IA32_REG_ES] = state.m_gsBase;
    }

    // Decode the instruction at cs:ip
    const ProcessorAddress address(is64 ? ProcessorAddress::PROCESSOR_64 :
                                   is16 ? ProcessorAddress::PROCESSOR_20 :
                                          ProcessorAddress::PROCESSOR_32,
                                   state.m_ip);
    cMemoryStream stream(bytes);
    StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
            type, BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true, address);
    OpcodePtr opcode = disassembler->next();

    // Lift it
    SemanticBlock block(type);
    lifter.lift(opcode, block);
    block.setFallThroughAddress(address + opcode->getOpcodeSize());

    // The memory: the code is at cs_base + ip
    PatternMemory memory(bytes, bases[ia32dis::IA32_REG_CS - ia32dis::IA32_REG_ES] +
                                state.m_ip, is64);
    if (is64)
        for (uint i = 0; i < sizeof(gGdt64); i++)
            memory.setInitial(GDT_ADDRESS + i, gGdt64[i]);
    else if (!is16)
        for (uint i = 0; i < sizeof(gGdt); i++)
            memory.setInitial(GDT_ADDRESS + i, gGdt[i]);
    {
        cList<uint64> keys = state.m_memory.keys();
        for (cList<uint64>::iterator i = keys.begin(); i != keys.end(); i++)
            memory.setInitial(*i, state.m_memory[*i]);
    }

    // Execute it from the state
    SemanticInterpreter interpreter(lifter.getArchitecture(), memory);
    if (is64)
    {
        for (uint i = 0; i < gStateRegisters64Count; i++)
            interpreter.setRegister(gStateRegisters64[i].m_register,
                                    SemanticValue(state.m_registers[i]));
        interpreter.setRegister(ia32dis::AMD64_REG_RIP, SemanticValue(state.m_ip));
        interpreter.setRegister(ia32dis::AMD64_REG_KERNEL_GS_BASE,
                                SemanticValue(state.m_kernelGsBase));
    }
    else
    {
        for (uint i = 0; i < gStateRegistersCount; i++)
            interpreter.setRegister(gStateRegisters[i].m_register,
                                    SemanticValue(state.m_registers[i]));
        interpreter.setRegister(ia32dis::IA32_REG_EIP, SemanticValue(state.m_ip));
    }
    for (uint i = 0; i < gStateFlagsCount; i++)
        interpreter.setRegister(gStateFlags[i].m_register,
                SemanticValue((state.m_eflags >> gStateFlags[i].m_bit) & 1));
    for (uint i = 0; i < gStateSegmentsCount; i++)
    {
        interpreter.setRegister(ia32dis::IA32_REG_ES + i,
                                SemanticValue(state.m_segments[i]));
        interpreter.setRegister(ia32dis::IA32_REG_ES_BASE + i,
                                SemanticValue(bases[i]));
    }
    interpreter.setRegister(ia32dis::IA32_REG_GDTR_BASE, SemanticValue(GDT_ADDRESS));
    interpreter.setRegister(ia32dis::IA32_REG_GDTR_LIMIT,
                            SemanticValue(is64 ? GDT64_LIMIT : GDT_LIMIT));
    interpreter.setRegister(ia32dis::IA32_REG_FPU_CONTROL, SemanticValue(state.m_fcw));
    interpreter.setRegister(ia32dis::IA32_REG_FPU_STATUS, SemanticValue(state.m_fsw));
    interpreter.setRegister(ia32dis::IA32_REG_FPU_TAG, SemanticValue(state.m_ftw));
    interpreter.setRegister(ia32dis::IA32_REG_MXCSR, SemanticValue(state.m_mxcsr));
    for (uint i = 0; i < 8; i++)
    {
        interpreter.setRegister(ia32dis::IA32_REG_FPR0 + i, state.m_fpr[i]);
        interpreter.setRegister(ia32dis::IA32_REG_XMM0 + i, state.m_xmm[i]);
    }
    if (is64)
        for (uint i = 8; i < 16; i++)
            interpreter.setRegister(ia32dis::AMD64_REG_XMM8 + (i - 8),
                                    state.m_xmm[i]);
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
        // A system call has no vector: the protocol numbers it 256
        exitText = cString("trap:") + cString((interpreter.getTrapKind() ==
                        SemanticOperation::TRAP_SYSTEM_CALL) ? 256 :
                        (int32)interpreter.getTrapNumber());
        break;
    case SemanticInterpreter::EXIT_MEMORY_FAULT:
        exitText = cString("fault:") +
                   PatternMemory::hexString(interpreter.getFaultAddress(),
                                            addressDigits);
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
    const OpcodeClassification classification = lifter.classify(opcode);
    const uint32 undefined = getEflagsBits(classification.getFlagsUndefined());

    // EFLAGS from the flag registers, bit 1 is always set
    uint32 eflags = 2;
    for (uint i = 0; i < gStateFlagsCount; i++)
        if (interpreter.getRegister(gStateFlags[i].m_register).getBits(0, 1) != 0)
            eflags |= (uint32)1 << gStateFlags[i].m_bit;

    cString result = cString("OK exit=") + exitText + cString(" undef=") +
                     PatternMemory::hexString(undefined, 8) +
                     cString(" fundef=") +
                     PatternMemory::hexString(getFpuStatusUndefined(classification), 4);
    if (is64)
    {
        for (uint i = 0; i < gStateRegisters64Count; i++)
            result += cString(" ") + cString(gStateRegisters64[i].m_name) +
                      cString("=") + registerHex(interpreter,
                            gStateRegisters64[i].m_register, 16);
        result += cString(" rip=") + registerHex(interpreter,
                                            ia32dis::AMD64_REG_RIP, 16);
    }
    else
    {
        for (uint i = 0; i < gStateRegistersCount; i++)
            result += cString(" ") + cString(gStateRegisters[i].m_name) +
                      cString("=") + registerHex(interpreter,
                            gStateRegisters[i].m_register, 8);
        result += cString(" eip=") + registerHex(interpreter,
                                            ia32dis::IA32_REG_EIP, 8);
    }
    result += cString(" eflags=") + PatternMemory::hexString(eflags, 8);
    for (uint i = 0; i < gStateSegmentsCount; i++)
        result += cString(" ") + cString(gStateSegments[i]) + cString("=") +
                  registerHex(interpreter, ia32dis::IA32_REG_ES + i, 4);
    // The bases of the segments (Unicorn doesn't expose them: the driver
    // checks them against the selectors)
    for (uint i = 0; i < gStateSegmentsCount; i++)
        result += cString(" ") + cString(gStateSegments[i]) + cString("_base=") +
                  registerHex(interpreter, ia32dis::IA32_REG_ES_BASE + i,
                              addressDigits);
    if (is64)
        result += cString(" kernel_gs_base=") + registerHex(interpreter,
                                    ia32dis::AMD64_REG_KERNEL_GS_BASE, 16);
    result += cString(" fcw=") + registerHex(interpreter,
                                    ia32dis::IA32_REG_FPU_CONTROL, 4);
    result += cString(" fsw=") + registerHex(interpreter,
                                    ia32dis::IA32_REG_FPU_STATUS, 4);
    result += cString(" ftw=") + registerHex(interpreter,
                                    ia32dis::IA32_REG_FPU_TAG, 2);
    for (uint i = 0; i < 8; i++)
        result += cString(" r") + cString((int32)i) + cString("=") +
                  wideHexString(interpreter.getRegister(
                        ia32dis::IA32_REG_FPR0 + i), 80);
    result += cString(" mxcsr=") + registerHex(interpreter,
                                    ia32dis::IA32_REG_MXCSR, 8);
    for (uint i = 0; i < 8; i++)
        result += cString(" xmm") + cString((int32)i) + cString("=") +
                  wideHexString(interpreter.getRegister(
                        ia32dis::IA32_REG_XMM0 + i), 128);
    if (is64)
        for (uint i = 8; i < 16; i++)
            result += cString(" xmm") + cString((int32)i) + cString("=") +
                      wideHexString(interpreter.getRegister(
                            ia32dis::AMD64_REG_XMM8 + (i - 8)), 128);
    result += cString(" mem=") + memory.getWrittenRuns();
    return result;
}

static int runRun(OpcodeSubsystems::DisassemblerType type)
{
    // Every segment has a base: the states select them
    IA32SemanticLifter lifter(type, IA32SemanticLifter::SEGMENTS_FULL);
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


/////////////////////////////////////////////////////////////////////////////
// ARM: --arch a32|t32|a64

// Parses the optional "@<hex address> " prefix of a line. Moves 'text' after
// it. Returns the address, 0 if there is no prefix
static uint64 parseAddressPrefix(const char*& text)
{
    while (*text == ' ' || *text == '\t')
        text++;
    if (*text != '@')
        return 0;
    text++;
    uint64 address = 0;
    while (hexValue(*text) >= 0)
    {
        address = (address << 4) | (uint64)hexValue(*text);
        text++;
    }
    while (*text == ' ' || *text == '\t')
        text++;
    return address;
}

// Parses the hex of an instruction: the value of one A32 / A64 word, or the
// values of the T32 halfwords. Appends the little endian bytes to 'out'
static bool parseArmHex(const char* text, ArmInstructionSet::DisassemblerTypes type,
                        cBuffer& out)
{
    // The groups of hex digits, separated by blanks
    uint8 bytes[64];
    uint count = 0;
    while (*text != 0)
    {
        // (a ';' separates the instructions of a sequence, see runArmDis)
        while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r' || *text == ';')
            text++;
        if (*text == 0)
            break;
        uint digits = 0;
        uint32 value = 0;
        while (hexValue(*text) >= 0)
        {
            value = (value << 4) | (uint32)hexValue(*text);
            digits++;
            text++;
        }
        if (((digits != 4) && (digits != 8)) || ((*text != 0) && (*text != ' ') &&
            (*text != '\t') && (*text != '\n') && (*text != '\r') && (*text != ';')))
            return false;
        if ((type != ArmInstructionSet::ARM_T32) && (digits != 8))
            return false;
        // A 32 bit group is a halfword pair in T32 (hw1 first), one word
        // otherwise
        if (digits == 8)
        {
            if (count + 4 > sizeof(bytes))
                return false;
            if (type == ArmInstructionSet::ARM_T32)
            {
                bytes[count++] = (uint8)(value >> 16);
                bytes[count++] = (uint8)(value >> 24);
                bytes[count++] = (uint8)(value);
                bytes[count++] = (uint8)(value >> 8);
            } else
            {
                bytes[count++] = (uint8)(value);
                bytes[count++] = (uint8)(value >> 8);
                bytes[count++] = (uint8)(value >> 16);
                bytes[count++] = (uint8)(value >> 24);
            }
        } else
        {
            if (count + 2 > sizeof(bytes))
                return false;
            bytes[count++] = (uint8)(value);
            bytes[count++] = (uint8)(value >> 8);
        }
    }
    if (count == 0)
        return false;
    out.changeSize(count);
    for (uint i = 0; i < count; i++)
        out[i] = bytes[i];
    return true;
}

static int runArmDis(ArmInstructionSet::DisassemblerTypes type, const ArmFeatureSet& features)
{
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), stdin))
    {
        XSTL_TRY
        {
            const char* text = line;
            uint64 address = parseAddressPrefix(text);
            cBuffer bytes;
            if (!parseArmHex(text, type, bytes))
                XSTL_THROW(cException, XSTL_STRING("bad hex input"), 0);

            cMemoryStream stream(bytes);
            ArmStreamDisassembler disassembler(type,
                BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
                ProcessorAddress((type == ArmInstructionSet::ARM_A64) ?
                                    ProcessorAddress::PROCESSOR_64 :
                                    ProcessorAddress::PROCESSOR_32, address),
                false, features);
            XSTL_TRY
            {
                // A line is one instruction, or a sequence of them (T32: the
                // it state is kept over the sequence): the texts are joined
                // with " ; "
                cString texts;
                uint total = 0;
                bool isFirst = true;
                while (!disassembler.isEndOfStream())
                {
                    OpcodePtr opcode = disassembler.next();
                    DefaultOpcodeDataFormatter formatter(1);
                    OpcodeFormatterPtr fr = disassembler.getOpcodeFormat(opcode, formatter);
                    if (!isFirst)
                        texts += " ; ";
                    texts += fr->string();
                    total += opcode->getOpcodeSize();
                    isFirst = false;
                }
                writeLine(cString("OK ") + cString((int32)total) + " " + texts);
            }
            XSTL_CATCH(DisassemblerInvalidOpcodeException&)
            {
                writeLine(cString("INVALID ") + cString((int32)bytes.getSize()));
            }
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

static int runArmAsm(ArmInstructionSet::DisassemblerTypes type, const ArmFeatureSet& features)
{
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), stdin))
    {
        const char* text = line;
        uint64 address = parseAddressPrefix(text);
        char copy[MAX_LINE];
        strncpy(copy, text, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = 0;
        normalizeAsmLine(copy);
        if (copy[0] == 0)
        {
            writeLine(cString("ERR empty instruction"));
            continue;
        }
        XSTL_TRY
        {
            // A line is one instruction or a sequence of them separated by ';'
            // (T32: an it block, the it state is kept over the sequence). The
            // bytes of all of them are written
            cString hex;
            static const char digits[] = "0123456789abcdef";
            uint itState = 0;
            char* part = copy;
            for (;;)
            {
                char* end = (type == ArmInstructionSet::ARM_T32) ? strchr(part, ';') : NULL;
                if (end != NULL)
                    *end = 0;
                normalizeAsmLine(part);
                uint32 encoding = 0;
                uint size = 0;
                ArmAssemblerStream::assembleInstruction(type, features, cString(part),
                                                        address, itState, encoding, size);
                // The bytes in memory order
                for (uint i = 0; i < size; i++)
                {
                    uint8 b;
                    if (size == 2)
                        b = (uint8)(encoding >> (8 * i));
                    else if (type == ArmInstructionSet::ARM_T32)
                        b = (uint8)(encoding >> ((i < 2) ? (16 + 8 * i) : (8 * (i - 2))));
                    else
                        b = (uint8)(encoding >> (8 * i));
                    hex += cString(digits[b >> 4]);
                    hex += cString(digits[b & 0xf]);
                }
                address += size;
                // The it state of the next instruction: an it sets it (the
                // condition 1111 is 1110, as LLVM does), the others advance it
                if (type == ArmInstructionSet::ARM_T32)
                {
                    if ((size == 2) && ((encoding & 0xFF00) == 0xBF00) && ((encoding & 0xF) != 0))
                    {
                        itState = encoding & 0xFF;
                        if ((itState >> 4) == 15)
                            itState = (itState & 0x0F) | 0xE0;
                    } else if (itState != 0)
                    {
                        itState = ((itState & 7) == 0) ? 0 :
                                  ((itState & 0xE0) | ((itState << 1) & 0x1F));
                    }
                }
                if (end == NULL)
                    break;
                part = end + 1;
            }
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

// Calls 'callback' for each entry of the tables of an instruction set
static void forEachArmEntry(ArmInstructionSet::DisassemblerTypes type,
                            void (*callback)(const armdis::ArmEncodingEntry&, uint, void*),
                            void* context)
{
    for (uint size = 2; size <= 4; size += 2)
    {
        if ((size == 2) && (type != ArmInstructionSet::ARM_T32))
            continue;
        const armdis::ArmEncodingEntry* const* tables =
            ArmStreamDisassembler::getTables(type, size);
        for (; *tables != NULL; tables++)
            for (const armdis::ArmEncodingEntry* entry = *tables; entry->m_mnemonic != NULL; entry++)
                callback(*entry, size, context);
    }
}

static void countEntry(const armdis::ArmEncodingEntry& entry, uint, void* context)
{
    ((uint*)context)[entry.m_feature]++;
}

static void printEntry(const armdis::ArmEncodingEntry& entry, uint size, void*)
{
    char text[256];
    snprintf(text, sizeof(text), "%08x %08x %u %x %u %s", entry.m_mask, entry.m_value,
             (unsigned)entry.m_feature, (unsigned)entry.m_flags, (unsigned)size, entry.m_mnemonic);
    writeLine(cString(text));
}

static int runArmFeatures(ArmInstructionSet::DisassemblerTypes type)
{
    uint counts[ArmFeatures::FEATURES_COUNT];
    for (uint i = 0; i < ArmFeatures::FEATURES_COUNT; i++)
        counts[i] = 0;
    forEachArmEntry(type, countEntry, counts);
    for (uint i = 0; i < ArmFeatures::FEATURES_COUNT; i++)
    {
        ArmFeatures::Feature feature = (ArmFeatures::Feature)i;
        ArmInstructionSet::DisassemblerTypes set = ArmFeatures::getInstructionSet(feature);
        // Each set lists its own features (the T32 list has the A32 ones too)
        bool listed = (set == type) ||
                      ((type == ArmInstructionSet::ARM_T32) && (set == ArmInstructionSet::ARM_A32));
        if (!listed)
            continue;
        char text[256];
        snprintf(text, sizeof(text), "%u %s %s %x %u", (unsigned)i,
                 (ArmFeatures::getName(feature)[0] == 0) ? "-" : ArmFeatures::getName(feature),
                 (set == ArmInstructionSet::ARM_A64) ? "a64" :
                 (set == ArmInstructionSet::ARM_T32) ? "t32" : "a32",
                 (unsigned)ArmFeatures::getSupport(feature), (unsigned)counts[i]);
        writeLine(cString(text));
    }
    return 0;
}

static int runArmEntries(ArmInstructionSet::DisassemblerTypes type)
{
    forEachArmEntry(type, printEntry, NULL);
    return 0;
}

/////////////////////////////////////////////////////////////////////////////
// ARM: run --arch a32|t32|a64 (tests/arm/semantics/TEST_PLAN.md, "The run
// protocol")

// The memory of the ARM run command. The window is [base, base + 0x10000), its
// bytes are the pattern of tests/semantics unless the state sets them. The
// code region (A64: 3 pages of 16 KB around the page of the pc, A32 / T32: 3
// pages of 4 KB) reads as brk #0x1234 words (A64) or zeros, the bytes of the
// code at the pc, and brk #0x1235 right after them (A64); an access outside
// the window and the code region is a fault (the access returns false). The
// written bytes are kept, the ones of the code region are not reported.
class ArmRunMemory : public SemanticMemory {
public:
    ArmRunMemory(const cBuffer& code, uint64 pc, bool is64) :
        m_code(code),
        m_pc(pc),
        m_is64(is64),
        m_windowBase(is64 ? 0x10100000000ULL : 0x20000000ULL),
        m_regionBase(is64 ? ((pc & ~(uint64)0x3FFF) - 0x4000) :
                            ((pc & ~(uint64)0xFFF) - 0x1000)),
        m_regionSize(is64 ? 0xC000 : 0x3000)
    {
    }

    // Set a byte of the memory before the run (mem=)
    void setInitial(uint64 address, uint8 value)
    {
        if (m_initial.hasKey(address))
            m_initial[address] = value;
        else
            m_initial.append(address, value);
    }

    virtual bool read(uint64 address, uint8* buffer, uint size)
    {
        for (uint i = 0; i < size; i++)
            if (!getByte(address + i, buffer[i]))
                return false;
        return true;
    }

    virtual bool write(uint64 address, const uint8* buffer, uint size)
    {
        for (uint i = 0; i < size; i++)
            if (!isValid(address + i))
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

    // Return the written bytes of the window as "address:bytes" runs sorted
    // by address, or "-" if nothing was written
    cString getWrittenRuns() const
    {
        cList<uint64> keys = m_written.keys();
        cArray<uint64> addresses(keys.length());
        uint count = 0;
        for (cList<uint64>::iterator i = keys.begin(); i != keys.end(); i++)
        {
            if (!isWindow(*i))
                continue;
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
            if ((i == 0) || (addresses[i] != addresses[i - 1] + 1))
            {
                if (i != 0)
                    result += cString(",");
                result += PatternMemory::hexString(addresses[i], 1) +
                          cString(":");
            }
            result += PatternMemory::hexString(m_written[addresses[i]], 2);
        }
        return result;
    }

private:
    bool isWindow(uint64 address) const
    {
        return (address - m_windowBase) < 0x10000;
    }

    bool isRegion(uint64 address) const
    {
        return (address - m_regionBase) < m_regionSize;
    }

    bool isValid(uint64 address) const
    {
        return isWindow(address) || isRegion(address);
    }

    // Read a byte: false if it isn't memory
    bool getByte(uint64 address, uint8& value) const
    {
        if (m_written.hasKey(address))
        {
            value = m_written[address];
            return true;
        }
        if ((address - m_pc) < m_code.getSize())
        {
            value = m_code[(uint)(address - m_pc)];
            return true;
        }
        if (isWindow(address))
        {
            value = m_initial.hasKey(address) ? m_initial[address] :
                    (uint8)((uint32)(address * 2654435761u) >> 24);
            return true;
        }
        if (!isRegion(address))
            return false;
        if (!m_is64)
        {
            value = 0;
            return true;
        }
        // brk #0x1235 right after the code, brk #0x1234 elsewhere
        const uint64 end = m_pc + m_code.getSize();
        const uint32 word = ((address >= end) && (address < end + 4)) ?
                            0xd42246a0u : 0xd4224680u;
        value = (uint8)(word >> (8 * (address & 3)));
        return true;
    }

    cBuffer m_code;
    uint64 m_pc;
    bool m_is64;
    uint64 m_windowBase;
    uint64 m_regionBase;
    uint64 m_regionSize;
    cHash<uint64, uint8> m_initial;
    cHash<uint64, uint8> m_written;
};

// The state of an ARM input line (the tokens of the protocol)
struct ArmRunState {
    ArmRunState(bool is64) :
        m_is64(is64),
        m_sp(is64 ? 0x10100008000ULL : 0x20008000ULL),
        m_pc(is64 ? 0x10000001000ULL : 0x10000ULL),
        m_lr(0),
        m_nzcv(0),
        m_fpcr(0),
        m_fpsr(0),
        m_apsr(0),
        m_fpscr(0),
        m_isSve(false),
        m_isStreaming(false),
        m_isZa(false),
        m_vl(128),
        m_svl(512)
    {
        memset(m_x, 0, sizeof(m_x));
        for (uint i = 0; i < 32; i++)
            m_hasV[i] = false;
    }

    bool m_is64;
    // x0..x30, or r0..r12
    uint64 m_x[31];
    uint64 m_sp;
    uint64 m_pc;
    uint64 m_lr;
    uint32 m_nzcv;
    uint32 m_fpcr;
    uint32 m_fpsr;
    uint32 m_apsr;
    uint32 m_fpscr;
    // v0..v31 (A64) and q0..q15 (A32), and the SVE state
    SemanticValue m_v[32];
    bool m_hasV[32];
    SemanticValue m_z[32];
    SemanticValue m_p[16];
    SemanticValue m_ffr;
    SemanticValue m_za[64];
    SemanticValue m_zt0;
    bool m_isSve;
    bool m_isStreaming;
    bool m_isZa;
    uint m_vl;
    uint m_svl;
    cHash<uint64, uint8> m_memory;
};

// Return true if the token name text[0..end) is 'prefix' followed by a decimal
// number below 'limit'
static bool parseIndexedName(const char* text, const char* end,
                             const char* prefix, uint limit, uint& index)
{
    const size_t length = strlen(prefix);
    if (((size_t)(end - text) <= length) || (strncmp(text, prefix, length) != 0))
        return false;
    uint value = 0;
    for (const char* p = text + length; p < end; p++)
    {
        if ((*p < '0') || (*p > '9'))
            return false;
        value = value * 10 + (uint)(*p - '0');
        if (value >= limit)
            return false;
    }
    index = value;
    return true;
}

// Parses mem=: "ADDR:HEX,ADDR:HEX" (or "-")
static bool parseArmMemory(const char* text, const char* end, ArmRunState& state)
{
    if ((end - text == 1) && (*text == '-'))
        return true;
    const char* p = text;
    while (p < end)
    {
        uint64 address = 0;
        uint digits = 0;
        while ((p < end) && (hexValue(*p) >= 0))
        {
            address = address * 16 + (uint64)hexValue(*p);
            p++;
            digits++;
        }
        if ((digits == 0) || (digits > 16) || (p >= end) || (*p != ':'))
            return false;
        p++;
        uint count = 0;
        while ((p + 1 < end) && (hexValue(p[0]) >= 0) && (hexValue(p[1]) >= 0))
        {
            const uint8 byte = (uint8)((hexValue(p[0]) << 4) | hexValue(p[1]));
            if (state.m_memory.hasKey(address + count))
                state.m_memory[address + count] = byte;
            else
                state.m_memory.append(address + count, byte);
            p += 2;
            count++;
        }
        if (count == 0)
            return false;
        if (p < end)
        {
            if ((*p != ',') || (p + 1 >= end))
                return false;
            p++;
        }
    }
    return true;
}

// Parses "x0=1 sp=.. pc=.. nzcv=4 v0=.. mem=.." (A64) or "r0=1 sp=.. lr=..
// pc=.. apsr=.. q0=.. mem=.." (A32, T32). Returns false on a malformed token
static bool parseArmState(const char* text, ArmRunState& state)
{
    const char* p = text;
    while (*p != 0)
    {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        {
            p++;
            continue;
        }
        const char* tokenEnd = p;
        while ((*tokenEnd != 0) && (*tokenEnd != ' ') && (*tokenEnd != '\t') &&
               (*tokenEnd != '\n') && (*tokenEnd != '\r'))
            tokenEnd++;
        const char* equals = (const char*)memchr(p, '=', (size_t)(tokenEnd - p));
        if (equals == NULL)
            return false;
        const char* value = equals + 1;
        uint index = 0;
        bool ok = true;
        SemanticValue wide;
        if (state.m_is64 && parseIndexedName(p, equals, "x", 31, index))
            ok = parseNumber(value, tokenEnd, 64, state.m_x[index]);
        else if (!state.m_is64 && parseIndexedName(p, equals, "r", 13, index))
            ok = parseNumber(value, tokenEnd, 32, state.m_x[index]);
        else if (state.m_is64 && parseIndexedName(p, equals, "v", 32, index))
        {
            ok = parseHexWide(value, tokenEnd, 128, state.m_v[index]);
            state.m_hasV[index] = true;
        }
        else if (!state.m_is64 && parseIndexedName(p, equals, "q", 16, index))
            ok = parseHexWide(value, tokenEnd, 128, state.m_v[index]);
        else if (state.m_is64 && parseIndexedName(p, equals, "z", 32, index))
        {
            ok = parseHexWide(value, tokenEnd, 512, state.m_z[index]);
            state.m_isSve = true;
        }
        else if (state.m_is64 && parseIndexedName(p, equals, "p", 16, index))
            ok = parseHexWide(value, tokenEnd, 64, state.m_p[index]);
        else if (state.m_is64 && parseIndexedName(p, equals, "za", 64, index))
            ok = parseHexWide(value, tokenEnd, 512, state.m_za[index]);
        else
        {
            const cString name(cString(p).left((uint)(equals - p)));
            uint64 number = 0;
            if (name == "sp")
                ok = parseNumber(value, tokenEnd, 64, state.m_sp);
            else if (name == "pc")
                ok = parseNumber(value, tokenEnd, 64, state.m_pc);
            else if (!state.m_is64 && (name == "lr"))
                ok = parseNumber(value, tokenEnd, 32, state.m_lr);
            else if (state.m_is64 && (name == "nzcv"))
            {
                ok = parseNumber(value, tokenEnd, 4, number);
                state.m_nzcv = (uint32)number;
            } else if (state.m_is64 && (name == "fpcr"))
                ok = parseWord(value, tokenEnd, state.m_fpcr);
            else if (state.m_is64 && (name == "fpsr"))
                ok = parseWord(value, tokenEnd, state.m_fpsr);
            else if (!state.m_is64 && (name == "apsr"))
                ok = parseWord(value, tokenEnd, state.m_apsr);
            else if (!state.m_is64 && (name == "fpscr"))
                ok = parseWord(value, tokenEnd, state.m_fpscr);
            else if (state.m_is64 && (name == "ffr"))
            {
                ok = parseHexWide(value, tokenEnd, 64, state.m_ffr);
            } else if (state.m_is64 && (name == "zt0"))
                ok = parseHexWide(value, tokenEnd, 512, state.m_zt0);
            else if (state.m_is64 && (name == "sve"))
            {
                ok = parseNumber(value, tokenEnd, 4, number);
                state.m_isSve = state.m_isSve || (number != 0);
            } else if (state.m_is64 && (name == "sm"))
            {
                ok = parseNumber(value, tokenEnd, 4, number);
                state.m_isStreaming = number != 0;
                state.m_isSve = state.m_isSve || state.m_isStreaming;
            } else if (state.m_is64 && (name == "za"))
            {
                ok = parseNumber(value, tokenEnd, 4, number);
                state.m_isZa = number != 0;
            } else if (state.m_is64 && (name == "vl"))
            {
                ok = parseNumber(value, tokenEnd, 16, number);
                state.m_vl = (uint)number;
            } else if (state.m_is64 && (name == "svl"))
            {
                ok = parseNumber(value, tokenEnd, 16, number);
                state.m_svl = (uint)number;
            } else if (name == "mem")
                ok = parseArmMemory(value, tokenEnd, state);
            else
                ok = false;
        }
        if (!ok)
            return false;
        p = tokenEnd;
    }
    return true;
}

// The flag registers of the AArch32 apsr: bit, register
static const struct {
    uint m_register;
    uint m_bit;
} gArmApsrFlags[] = {
    { armdis::ARM_REG_N, 31 }, { armdis::ARM_REG_Z, 30 },
    { armdis::ARM_REG_C, 29 }, { armdis::ARM_REG_V, 28 },
    { armdis::ARM_REG_Q, 27 },
    { armdis::ARM_REG_GE0, 16 }, { armdis::ARM_REG_GE1, 17 },
    { armdis::ARM_REG_GE2, 18 }, { armdis::ARM_REG_GE3, 19 }
};

// Executes the instructions of a line of the ARM run protocol ("<hex bytes>
// ; <state>") as the IR of the lifter, and returns the result record
static cString runArmLine(char* line, ArmInstructionSet::DisassemblerTypes type)
{
    char* separator = strchr(line, ';');
    if (separator == NULL)
        XSTL_THROW(cException, XSTL_STRING("missing ';' before the state"), 0);
    *separator = 0;
    cBuffer bytes;
    if (!parseHex(line, bytes))
        XSTL_THROW(cException, XSTL_STRING("bad hex input"), 0);
    const bool is64 = type == ArmInstructionSet::ARM_A64;
    ArmRunState state(is64);
    if (!parseArmState(separator + 1, state))
        XSTL_THROW(cException, XSTL_STRING("bad state"), 0);
    if (state.m_isSve && ((state.m_vl < 128) || (state.m_vl > 512) ||
                          (state.m_svl < 128) || (state.m_svl > 512)))
        XSTL_THROW(cException, XSTL_STRING("bad vector length"), 0);

    const OpcodeSubsystems::DisassemblerType subsystem =
        is64 ? OpcodeSubsystems::DISASSEMBLER_ARM_64_LE :
        (type == ArmInstructionSet::ARM_T32) ?
            OpcodeSubsystems::DISASSEMBLER_THUMB_LE :
            OpcodeSubsystems::DISASSEMBLER_ARM_32_LE;
    // The description is the same for most of the lines: kept (building the
    // registers of a description costs more than running a block)
    static ArchitectureDescriptionPtr cachedArchitecture;
    static bool cachedIs64 = false;
    static uint cachedVl = 0;
    static uint cachedSvl = 0;
    if ((cachedArchitecture.getPointer() == NULL) || (cachedIs64 != is64) ||
        (is64 && ((cachedVl != state.m_vl) || (cachedSvl != state.m_svl))))
    {
        if (is64)
            cachedArchitecture = ArchitectureDescriptionPtr(
                            new Arm64Architecture(state.m_vl, state.m_svl));
        else
            cachedArchitecture = ArchitectureDescriptionPtr(new ArmArchitecture());
        cachedIs64 = is64;
        cachedVl = state.m_vl;
        cachedSvl = state.m_svl;
    }
    ArchitectureDescriptionPtr architecture = cachedArchitecture;
    ArmSemanticLifter lifter(subsystem, architecture);

    // Decode and lift the sequence at the pc
    const uint64 codeAddress = state.m_pc & ~(uint64)(is64 ? 3 : 1);
    cMemoryStream stream(bytes);
    ArmStreamDisassembler disassembler(type,
        BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
        ProcessorAddress(is64 ? ProcessorAddress::PROCESSOR_64 :
                                ProcessorAddress::PROCESSOR_32, codeAddress),
        false);
    SemanticBlock block(subsystem);
    while (!disassembler.isEndOfStream())
    {
        OpcodePtr opcode = disassembler.next();
        lifter.lift(opcode, block);
    }
    block.setFallThroughAddress(ProcessorAddress(is64 ?
        ProcessorAddress::PROCESSOR_64 : ProcessorAddress::PROCESSOR_32,
        codeAddress + bytes.getSize()));

    ArmRunMemory memory(bytes, codeAddress, is64);
    {
        cList<uint64> keys = state.m_memory.keys();
        for (cList<uint64>::iterator i = keys.begin(); i != keys.end(); i++)
            memory.setInitial(*i, state.m_memory[*i]);
    }

    // Execute it from the state
    SemanticInterpreter interpreter(architecture, memory);
    if (is64)
    {
        using namespace armdis;
        for (uint i = 0; i < 31; i++)
            interpreter.setRegister(ARM64_REG_X0 + i, SemanticValue(state.m_x[i]));
        interpreter.setRegister(ARM64_REG_SP, SemanticValue(state.m_sp));
        interpreter.setRegister(ARM64_REG_PC, SemanticValue(codeAddress));
        interpreter.setRegister(ARM64_REG_N, SemanticValue((state.m_nzcv >> 3) & 1));
        interpreter.setRegister(ARM64_REG_Z, SemanticValue((state.m_nzcv >> 2) & 1));
        interpreter.setRegister(ARM64_REG_C, SemanticValue((state.m_nzcv >> 1) & 1));
        interpreter.setRegister(ARM64_REG_V, SemanticValue(state.m_nzcv & 1));
        interpreter.setRegister(ARM64_REG_FPCR, SemanticValue(state.m_fpcr));
        interpreter.setRegister(ARM64_REG_FPSR, SemanticValue(state.m_fpsr));
        interpreter.setRegister(ARM64_REG_SVCR, SemanticValue(
            (state.m_isStreaming ? 1 : 0) | (state.m_isZa ? 2 : 0)));
        for (uint i = 0; i < 32; i++)
        {
            // z is applied first, then v over its low 128 bits
            SemanticValue vector = state.m_z[i];
            if (state.m_hasV[i])
            {
                vector.setWord(0, state.m_v[i].getWord(0));
                vector.setWord(1, state.m_v[i].getWord(1));
            }
            interpreter.setRegister(ARM64_REG_Z0 + i, vector);
        }
        for (uint i = 0; i < 16; i++)
            interpreter.setRegister(ARM64_REG_P0 + i, state.m_p[i]);
        interpreter.setRegister(ARM64_REG_FFR, state.m_ffr);
        for (uint i = 0; i < 64; i++)
            interpreter.setRegister(ARM64_REG_ZA_ROW0 + i, state.m_za[i]);
        interpreter.setRegister(ARM64_REG_ZT0, state.m_zt0);
    }
    else
    {
        using namespace armdis;
        for (uint i = 0; i < 13; i++)
            interpreter.setRegister(ARM_REG_R0 + i, SemanticValue(state.m_x[i]));
        interpreter.setRegister(ARM_REG_SP, SemanticValue(state.m_sp));
        interpreter.setRegister(ARM_REG_LR, SemanticValue(state.m_lr));
        interpreter.setRegister(ARM_REG_PC, SemanticValue(codeAddress));
        for (uint i = 0; i < sizeof(gArmApsrFlags) / sizeof(gArmApsrFlags[0]); i++)
            interpreter.setRegister(gArmApsrFlags[i].m_register,
                SemanticValue((state.m_apsr >> gArmApsrFlags[i].m_bit) & 1));
        interpreter.setRegister(ARM_REG_T, SemanticValue(
                (type == ArmInstructionSet::ARM_T32) ? 1 : 0));
        interpreter.setRegister(ARM_REG_FPSCR, SemanticValue(state.m_fpscr));
        for (uint i = 0; i < 16; i++)
            interpreter.setRegister(ARM_REG_Q0 + i, state.m_v[i]);
    }
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
        switch (interpreter.getTrapKind())
        {
        case SemanticOperation::TRAP_SYSTEM_CALL:
            exitText = "trap:svc";
            break;
        case SemanticOperation::TRAP_BREAKPOINT:
            exitText = "trap:brk";
            break;
        case SemanticOperation::TRAP_UNDEFINED:
            exitText = "trap:undef";
            break;
        default:
            exitText = "trap:fault";
            break;
        }
        break;
    case SemanticInterpreter::EXIT_MEMORY_FAULT:
        exitText = "trap:fault";
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

    cString result = cString("OK exit=") + exitText;
    const bool isFall = reason == SemanticInterpreter::EXIT_FALL_THROUGH;
    using namespace armdis;
    if (is64)
    {
        if (state.m_isSve)
            result += state.m_isStreaming ? cString(" sve=1 sm=1") : cString(" sve=1");
        if (state.m_isZa)
            result += " za=1";
        if (state.m_isSve)
            result += cString(" vl=") + PatternMemory::hexString(state.m_vl, 1) +
                      cString(" svl=") + PatternMemory::hexString(state.m_svl, 1);
        for (uint i = 0; i < 31; i++)
            result += cString(" x") + cString((int32)i) + cString("=") +
                      registerHex(interpreter, ARM64_REG_X0 + i, 16);
        result += cString(" sp=") + registerHex(interpreter, ARM64_REG_SP, 16);
        result += cString(" pc=") + registerHex(interpreter, ARM64_REG_PC, 16);
        uint nzcv = 0;
        if (interpreter.getRegister(ARM64_REG_N).getBits(0, 1) != 0) nzcv |= 8;
        if (interpreter.getRegister(ARM64_REG_Z).getBits(0, 1) != 0) nzcv |= 4;
        if (interpreter.getRegister(ARM64_REG_C).getBits(0, 1) != 0) nzcv |= 2;
        if (interpreter.getRegister(ARM64_REG_V).getBits(0, 1) != 0) nzcv |= 1;
        result += cString(" nzcv=") + PatternMemory::hexString(nzcv, 1);
        result += cString(" fpcr=") + registerHex(interpreter, ARM64_REG_FPCR, 8);
        result += cString(" fpsr=") + registerHex(interpreter, ARM64_REG_FPSR, 8);
        for (uint i = 0; i < 32; i++)
            result += cString(" v") + cString((int32)i) + cString("=") +
                      wideHexString(interpreter.getRegister(ARM64_REG_V0 + i), 128);
        if (state.m_isSve && isFall)
        {
            const uint vectorBits = state.m_isStreaming ? state.m_svl : state.m_vl;
            for (uint i = 0; i < 32; i++)
                result += cString(" z") + cString((int32)i) + cString("=") +
                          wideHexString(interpreter.getRegister(ARM64_REG_Z0 + i),
                                        vectorBits);
            for (uint i = 0; i < 16; i++)
                result += cString(" p") + cString((int32)i) + cString("=") +
                          wideHexString(interpreter.getRegister(ARM64_REG_P0 + i),
                                        (vectorBits / 32 == 0 ? 1 : vectorBits / 32) * 4);
            if (!state.m_isStreaming)
                result += cString(" ffr=") + wideHexString(
                    interpreter.getRegister(ARM64_REG_FFR),
                    (vectorBits / 32 == 0 ? 1 : vectorBits / 32) * 4);
            if (state.m_isZa)
            {
                for (uint i = 0; i < state.m_svl / 8; i++)
                    result += cString(" za") + cString((int32)i) + cString("=") +
                              wideHexString(interpreter.getRegister(
                                    ARM64_REG_ZA_ROW0 + i), state.m_svl);
                result += cString(" zt0=") + wideHexString(
                    interpreter.getRegister(ARM64_REG_ZT0), 512);
            }
        }
    } else
    {
        for (uint i = 0; i < 13; i++)
            result += cString(" r") + cString((int32)i) + cString("=") +
                      registerHex(interpreter, ARM_REG_R0 + i, 8);
        result += cString(" sp=") + registerHex(interpreter, ARM_REG_SP, 8);
        result += cString(" lr=") + registerHex(interpreter, ARM_REG_LR, 8);
        result += cString(" pc=") + registerHex(interpreter, ARM_REG_PC, 8);
        uint32 apsr = 0;
        for (uint i = 0; i < sizeof(gArmApsrFlags) / sizeof(gArmApsrFlags[0]); i++)
            if (interpreter.getRegister(gArmApsrFlags[i].m_register).getBits(0, 1) != 0)
                apsr |= (uint32)1 << gArmApsrFlags[i].m_bit;
        result += cString(" apsr=") + PatternMemory::hexString(apsr, 8);
        result += cString(" t=") + PatternMemory::hexString(
                    interpreter.getRegister(ARM_REG_T).getBits(0, 1), 1);
        result += cString(" fpscr=") + registerHex(interpreter, ARM_REG_FPSCR, 8);
        for (uint i = 0; i < 16; i++)
            result += cString(" q") + cString((int32)i) + cString("=") +
                      wideHexString(interpreter.getRegister(ARM_REG_Q0 + i), 128);
    }
    result += cString(" mem=") + memory.getWrittenRuns();
    return result;
}

static int runArmRun(ArmInstructionSet::DisassemblerTypes type)
{
    // (the state of SME has za: 64 rows of 128 digits, more than MAX_LINE)
    static char line[1 << 17];
    while (fgets(line, sizeof(line), stdin))
    {
        XSTL_TRY
        {
            writeLine(runArmLine(line, type));
        }
        XSTL_CATCH(DisassemblerEndOfStreamException&)
        {
            writeLine(cString("ERR truncated instruction (end of stream)"));
        }
        XSTL_CATCH(DisassemblerInvalidOpcodeException&)
        {
            // The bits are not an instruction of the tables: not lifted
            writeLine(cString("OK exit=undefined"));
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
// RISC-V: --arch rv32|rv64 [--mattr <list>] [--format no-aliases,numeric] [--no-rvc]
//
//   dis    each line: [@<hex address> ]<groups of 4 / 8 hex digits, the values
//          of the instructions as objdump prints them>
//          -> "OK <size> <text> [; <text>...]" | "INVALID <size>" | "ERR <message>"
//   sweep  each line: [@<hex address> ]<raw bytes in memory order>, decoded as a
//          linear sweep with the fault tolerance on
//          -> "OK <size>:<text> ; ..." ("1:<unknown>" for an invalid byte)
//   asm    each line: [@<hex address> ]<statement> -> "OK <hex bytes>" | "ERR <message>"
//   features / entries / set: the support matrix, the encodings, the features
//          of --mattr

// Parses the hex of an instruction: groups of 4 (a compressed instruction) or
// 8 (a 32 bit one) hex digits, each the value of the instruction as objdump
// prints it. Appends the little endian bytes to 'out'
static bool parseRiscvHex(const char* text, cBuffer& out)
{
    uint8 bytes[64];
    uint count = 0;
    while (*text != 0)
    {
        while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
            text++;
        if (*text == 0)
            break;
        uint digits = 0;
        uint32 value = 0;
        while (hexValue(*text) >= 0)
        {
            value = (value << 4) | (uint32)hexValue(*text);
            digits++;
            text++;
        }
        if (((digits != 4) && (digits != 8)) ||
            ((*text != 0) && (*text != ' ') && (*text != '\t') && (*text != '\n') && (*text != '\r')))
            return false;
        if (count + digits / 2 > sizeof(bytes))
            return false;
        for (uint i = 0; i < digits / 2; i++)
            bytes[count++] = (uint8)(value >> (8 * i));
    }
    if (count == 0)
        return false;
    out.changeSize(count);
    for (uint i = 0; i < count; i++)
        out[i] = bytes[i];
    return true;
}

// Parses contiguous hex bytes in memory order ("154537250900")
static bool parseRiscvBytes(const char* text, cBuffer& out)
{
    static uint8 bytes[1 << 16];
    uint count = 0;
    while (*text == ' ' || *text == '\t')
        text++;
    while (hexValue(text[0]) >= 0 && hexValue(text[1]) >= 0)
    {
        if (count >= sizeof(bytes))
            return false;
        bytes[count++] = (uint8)((hexValue(text[0]) << 4) | hexValue(text[1]));
        text += 2;
    }
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
        text++;
    if (*text != 0 || count == 0)
        return false;
    out.changeSize(count);
    for (uint i = 0; i < count; i++)
        out[i] = bytes[i];
    return true;
}

static ProcessorAddress riscvAddress(RiscvInstructionSet::DisassemblerTypes type, uint64 address)
{
    return ProcessorAddress((type == RiscvInstructionSet::RISCV_RV64) ?
                                ProcessorAddress::PROCESSOR_64 : ProcessorAddress::PROCESSOR_32,
                            address);
}

static int runRiscvDis(RiscvInstructionSet::DisassemblerTypes type, const RiscvFeatureSet& features,
                       int formatType)
{
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), stdin))
    {
        XSTL_TRY
        {
            const char* text = line;
            uint64 address = parseAddressPrefix(text);
            cBuffer bytes;
            if (!parseRiscvHex(text, bytes))
                XSTL_THROW(cException, XSTL_STRING("bad hex input"), 0);

            cMemoryStream stream(bytes);
            RiscvStreamDisassembler disassembler(type,
                BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
                riscvAddress(type, address), false, features);
            XSTL_TRY
            {
                // The texts of a sequence are joined with " ; "
                cString texts;
                uint total = 0;
                bool isFirst = true;
                while (!disassembler.isEndOfStream())
                {
                    OpcodePtr opcode = disassembler.next();
                    DefaultOpcodeDataFormatter formatter(1);
                    OpcodeFormatterPtr fr = disassembler.getOpcodeFormat(opcode, formatter, formatType);
                    if (!isFirst)
                        texts += " ; ";
                    texts += fr->string();
                    total += opcode->getOpcodeSize();
                    isFirst = false;
                }
                writeLine(cString("OK ") + cString((int32)total) + " " + texts);
            }
            XSTL_CATCH(DisassemblerInvalidOpcodeException&)
            {
                writeLine(cString("INVALID ") + cString((int32)bytes.getSize()));
            }
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

static int runRiscvSweep(RiscvInstructionSet::DisassemblerTypes type, const RiscvFeatureSet& features,
                         int formatType)
{
    static char line[(1 << 17) + 64];
    while (fgets(line, sizeof(line), stdin))
    {
        XSTL_TRY
        {
            const char* text = line;
            uint64 address = parseAddressPrefix(text);
            cBuffer bytes;
            if (!parseRiscvBytes(text, bytes))
                XSTL_THROW(cException, XSTL_STRING("bad hex input"), 0);

            cMemoryStream stream(bytes);
            RiscvStreamDisassembler disassembler(type,
                BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
                riscvAddress(type, address), true, features);
            cString texts;
            bool isFirst = true;
            XSTL_TRY
            {
                while (!disassembler.isEndOfStream())
                {
                    OpcodePtr opcode = disassembler.next();
                    DefaultOpcodeDataFormatter formatter(1);
                    OpcodeFormatterPtr fr = disassembler.getOpcodeFormat(opcode, formatter, formatType);
                    if (!isFirst)
                        texts += " ; ";
                    if (opcode->getType() == OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE)
                        texts += cString("1:<unknown>");
                    else
                        texts += cString((int32)opcode->getOpcodeSize()) + ":" + fr->string();
                    isFirst = false;
                }
            }
            XSTL_CATCH(DisassemblerEndOfStreamException&)
            {
                // (a truncated last instruction)
                if (!isFirst)
                    texts += " ; ";
                texts += cString("?:<truncated>");
            }
            writeLine(cString("OK ") + texts);
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

static int runRiscvAsm(RiscvInstructionSet::DisassemblerTypes type, const RiscvFeatureSet& features,
                       bool isCompressing)
{
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), stdin))
    {
        const char* text = line;
        parseAddressPrefix(text);
        char copy[MAX_LINE];
        strncpy(copy, text, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = 0;
        normalizeAsmLine(copy);
        if (copy[0] == 0)
        {
            writeLine(cString("ERR empty instruction"));
            continue;
        }
        XSTL_TRY
        {
            FirstPassBinaryPtr binary(new FirstPassBinary(RiscvInstructionSet::toSubsystem(type), false));
            binary->createNewBlockWithoutChange(0, StackInterfacePtr(new CliStackInterface(binary)));
            binary->changeBasicBlock(0);
            {
                // (the stream is used directly so that errors are raised here)
                RiscvAssemblerStream stream(*binary, type, features);
                stream.setCompression(isCompressing);
                stream << copy << endl;
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

// Calls 'callback' for each entry of the tables
static void forEachRiscvEntry(void (*callback)(const riscvdis::RiscvEncodingEntry&, uint, void*),
                              void* context)
{
    for (uint size = 2; size <= 4; size += 2)
    {
        const riscvdis::RiscvEncodingEntry* const* tables = RiscvStreamDisassembler::getTables(size);
        for (; *tables != NULL; tables++)
            for (const riscvdis::RiscvEncodingEntry* entry = *tables; entry->m_mnemonic != NULL; entry++)
                callback(*entry, size, context);
    }
}

static void countRiscvEntry(const riscvdis::RiscvEncodingEntry& entry, uint, void* context)
{
    ((uint*)context)[entry.m_feature]++;
}

static void printRiscvEntry(const riscvdis::RiscvEncodingEntry& entry, uint size, void*)
{
    char text[256];
    snprintf(text, sizeof(text), "%x %x %u %x %u %s", (unsigned int)entry.m_mask,
             (unsigned int)entry.m_value, (unsigned int)entry.m_feature,
             (unsigned int)entry.m_flags, (unsigned int)size, entry.m_mnemonic);
    writeLine(cString(text));
}

// "<index> <name> <profile status> <support hex> <entries>"
static int runRiscvFeatures()
{
    uint counts[RiscvFeatures::FEATURES_COUNT];
    for (uint i = 0; i < RiscvFeatures::FEATURES_COUNT; i++)
        counts[i] = 0;
    forEachRiscvEntry(countRiscvEntry, counts);
    for (uint i = 0; i < RiscvFeatures::FEATURES_COUNT; i++)
    {
        RiscvFeatures::Feature feature = (RiscvFeatures::Feature)i;
        char text[256];
        snprintf(text, sizeof(text), "%u %s %u %x %u", (unsigned int)i,
                 (RiscvFeatures::getName(feature)[0] == 0) ? "-" : RiscvFeatures::getName(feature),
                 (unsigned int)RiscvFeatures::getProfileStatus(feature),
                 (unsigned int)RiscvFeatures::getSupport(feature), (unsigned int)counts[i]);
        writeLine(cString(text));
    }
    return 0;
}

static int runRiscvEntries()
{
    forEachRiscvEntry(printRiscvEntry, NULL);
    return 0;
}

// The names of the features of the --mattr list
static int runRiscvSet(const RiscvFeatureSet& features)
{
    cString text;
    for (uint i = 0; i < RiscvFeatures::FEATURES_COUNT; i++)
        if (features.has((RiscvFeatures::Feature)i))
        {
            const char* name = RiscvFeatures::getName((RiscvFeatures::Feature)i);
            if (name[0] != 0)
                text += cString(name) + " ";
        }
    writeLine(text);
    return 0;
}

static void usage()
{
    fprintf(stderr, "usage: dismount_cli asm|dis|run [--bits 16|32|64] < input\n"
                    "       dismount_cli asm|dis|run|features|entries --arch a32|t32|a64"
                    " [--mattr +feature,-feature] < input\n"
                    "       dismount_cli asm|dis|sweep|features|entries|set --arch rv32|rv64"
                    " [--mattr list] [--format no-aliases,numeric] [--no-rvc] < input\n"
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
    bool isArm = false;
    ArmInstructionSet::DisassemblerTypes armType = ArmInstructionSet::ARM_A64;
    ArmFeatureSet armFeatures = ArmFeatureSet::all();
    bool isRiscv = false;
    RiscvInstructionSet::DisassemblerTypes riscvType = RiscvInstructionSet::RISCV_RV64;
    RiscvFeatureSet riscvFeatures = RiscvFeatureSet::all();
    int riscvFormat = 0;
    bool riscvCompress = true;
    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--arch") == 0 && i + 1 < argc &&
            (strcmp(argv[i + 1], "rv32") == 0 || strcmp(argv[i + 1], "rv64") == 0))
        {
            i++;
            isRiscv = true;
            riscvType = (strcmp(argv[i], "rv32") == 0) ? RiscvInstructionSet::RISCV_RV32 :
                                                         RiscvInstructionSet::RISCV_RV64;
        }
        else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc)
        {
            i++;
            if (strstr(argv[i], "no-aliases") != NULL)
                riscvFormat |= RiscvNotation::RISCV_FORMAT_NO_ALIASES;
            if (strstr(argv[i], "numeric") != NULL)
                riscvFormat |= RiscvNotation::RISCV_FORMAT_NUMERIC;
        }
        else if (strcmp(argv[i], "--no-rvc") == 0)
        {
            riscvCompress = false;
        }
        else if (strcmp(argv[i], "--arch") == 0 && i + 1 < argc)
        {
            i++;
            isArm = true;
            if (strcmp(argv[i], "a32") == 0)
                armType = ArmInstructionSet::ARM_A32;
            else if (strcmp(argv[i], "t32") == 0)
                armType = ArmInstructionSet::ARM_T32;
            else if (strcmp(argv[i], "a64") == 0)
                armType = ArmInstructionSet::ARM_A64;
            else
            {
                usage();
                return 2;
            }
        }
        else if (strcmp(argv[i], "--mattr") == 0 && i + 1 < argc)
        {
            i++;
            if (isRiscv)
            {
                if (!riscvFeatures.parse(argv[i]))
                    fprintf(stderr, "warning: unknown feature in --mattr %s\n", argv[i]);
            }
            else if (!armFeatures.parse(argv[i]))
                fprintf(stderr, "warning: unknown feature in --mattr %s\n", argv[i]);
        }
        else if (strcmp(argv[i], "--bits") == 0 && i + 1 < argc)
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
            else if (strcmp(argv[i], "64") == 0)
            {
                asmMode = IA32eInstructionSet::AMD_64;
                disType = OpcodeSubsystems::DISASSEMBLER_AMD_64;
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

    if (isRiscv)
    {
        if (strcmp(argv[1], "asm") == 0)
            return runRiscvAsm(riscvType, riscvFeatures, riscvCompress);
        if (strcmp(argv[1], "dis") == 0)
            return runRiscvDis(riscvType, riscvFeatures, riscvFormat);
        if (strcmp(argv[1], "sweep") == 0)
            return runRiscvSweep(riscvType, riscvFeatures, riscvFormat);
        if (strcmp(argv[1], "features") == 0)
            return runRiscvFeatures();
        if (strcmp(argv[1], "entries") == 0)
            return runRiscvEntries();
        if (strcmp(argv[1], "set") == 0)
            return runRiscvSet(riscvFeatures);
        usage();
        return 2;
    }

    if (isArm)
    {
        if (strcmp(argv[1], "asm") == 0)
            return runArmAsm(armType, armFeatures);
        if (strcmp(argv[1], "dis") == 0)
            return runArmDis(armType, armFeatures);
        if (strcmp(argv[1], "features") == 0)
            return runArmFeatures(armType);
        if (strcmp(argv[1], "entries") == 0)
            return runArmEntries(armType);
        if (strcmp(argv[1], "run") == 0)
            return runArmRun(armType);
        usage();
        return 2;
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
