#include "dismount/dismount.h"
/*
 * IA32Assembler.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/char.h"
#include "xStl/data/endian.h"
#include "xStl/except/trace.h"
#include "xStl/exceptions.h"
#include "xStl/parser/parser.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/IntegerEncoding.h"
#include "dismount/proc/ia32/opcodeTable.h"

// Throw an exception with a message describing the assembler failure
#define ASM_CHECK(cond, msg) if (!(cond)) { XSTL_THROW(cException, XSTL_STRING(msg), EXCEPTION_FAILED); }

IA32Assembler::IA32Assembler(FirstPassBinaryPtr binary,
                             IA32eInstructionSet::DisassemblerTypes processorMode) :
    m_binary(binary),
    m_processorMode(processorMode)
{
}

cStringerStreamPtr IA32Assembler::getAssembler()
{
    return cStringerStreamPtr(new IA32AssemblerStream(*m_binary,
                                                      m_processorMode));
}

FirstPassBinaryPtr IA32Assembler::getFirstPass()
{
    return m_binary;
}

//////////////////////////////////////////////////////////////////////////
// Text helpers. The text is read through the xStl Parser, and the words are
// cStrings. The text is lowercased before it is parsed.

namespace {

// True if the next character is 'c'
bool isNextChar(Parser& parser, char c)
{
    return !parser.isEOS() && (parser.peekChar() == c);
}

// True if a letter, a digit or '_' (a word) starts at the parser
bool isIdentifierChar(char c)
{
    return cChar::isLetter(c) || cChar::isDigit(c) || (c == '_');
}

bool isIdentifierAhead(Parser& parser)
{
    return !parser.isEOS() && isIdentifierChar(parser.peekChar());
}

// True if a letter starts at the parser (a register name or a keyword)
bool isLetterAhead(Parser& parser)
{
    return !parser.isEOS() && cChar::isLetter(parser.peekChar());
}

// True if a decimal digit starts at the parser (a number)
bool isDecimalAhead(Parser& parser)
{
    return !parser.isEOS() && cChar::isDigit(parser.peekChar());
}

// Return the word at the parser without reading it. Empty if there is no word.
cString peekWord(Parser& parser)
{
    if (!isIdentifierAhead(parser))
        return cString();
    return parser.peekCString(false);
}

// Read the word at the parser and the blanks after it
void skipWord(Parser& parser)
{
    parser.readCString(false);
    parser.readBlanksUntilEOL();
}

// True if the instruction ends here: the end of the text, the end of the line
// or the start of a ';' comment
bool isEndOfInstruction(Parser& parser)
{
    if (parser.isEOS())
        return true;
    char c = parser.peekChar();
    return cChar::isNewLine(c) || (c == ';');
}

// True if the operand ends here. A ',' separates the next operand.
bool isEndOfOperand(Parser& parser)
{
    return isEndOfInstruction(parser) || isNextChar(parser, ',');
}

// Skip the rest of the line (including a ';' comment)
void skipLine(Parser& parser)
{
    while (!parser.isEOS() && !cChar::isNewLine(parser.peekChar()))
        parser.readChar();
}

// Read a number which must start at the parser. The number is decimal, 0x hex
// or hex with a h suffix (7bh), and it must fit in 32 bits.
uint64 readNumber(Parser& parser)
{
    ASM_CHECK(isDecimalAhead(parser), "number was expected");
    uint64 value = 0;
    ASM_CHECK(parser.isUnsignedInteger64(&value, true), "invalid number");
    ASM_CHECK(value <= 0xFFFFFFFFULL, "number is too big");
    return value;
}

// Size keywords and their size in bits
struct SizeKeyword {
    const char* m_name;
    uint m_bits;
};

const SizeKeyword gSizeKeywords[] = {
    { "byte",  8   },
    { "word",  16  },
    { "dword", 32  },
    { "qword", 64  },
    { "tword", 80  },
    { "oword", 128 },
    { "yword", 256 },
    { "zword", 512 },
};

// Read a size keyword ('byte', 'dword ptr', ...) and return its size in bits.
// Return 0 (and do not advance) if there is no size keyword at the parser.
uint readSizeKeyword(Parser& parser)
{
    cString word = peekWord(parser);
    for (uint i = 0; i < sizeof(gSizeKeywords) / sizeof(gSizeKeywords[0]); i++)
    {
        if (word == gSizeKeywords[i].m_name)
        {
            skipWord(parser);
            // The 'ptr' is optional
            if (peekWord(parser) == "ptr")
                skipWord(parser);
            return gSizeKeywords[i].m_bits;
        }
    }
    return 0;
}

// Return the segment register index of a segment name, -1 if it's not one
int segmentIndex(const cString& name)
{
    for (uint i = 0; i < ia32dis::NUMBER_OF_SEGMENTS_REGISTERS; i++)
        if (name == ia32dis::gIa32SegmentsRegisters[i].m_name)
            return (int)i;
    return -1;
}

// Return the position of 'ch' inside 'text', -1 if it's not there
int indexOf(const cString& text, character ch)
{
    for (int i = 0; i < (int)text.length(); i++)
        if (text[i] == ch)
            return i;
    return -1;
}

// True for the operand sizes of the size keywords (byte..zword) that the
// table can encode, 0 for no size keyword
bool isKnownOperandSize(uint size)
{
    return (size == 0) || (size == 8) || (size == 16) || (size == 32) ||
           (size == 64) || (size == 80) || (size == 128) || (size == 256) ||
           (size == 512);
}

// Return the value of the bit in the scale field of the SIB byte
uint scaleBits(uint scale)
{
    switch (scale)
    {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    default:
        return 3;
    }
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

IA32AssemblerStream::IA32AssemblerStream(FirstPassBinary& binary,
                           IA32eInstructionSet::DisassemblerTypes processorMode) :
    m_binary(binary),
    m_processorMode(processorMode)
{
}

IA32AssemblerStream::~IA32AssemblerStream()
{
    // Destructors must not throw (the parser reports errors via exceptions)
    XSTL_TRY
    {
        if (getData().length() != 0)
            assemble();
    }
    XSTL_CATCH_ALL
    {
    }
}

void IA32AssemblerStream::assemble()
{
    // Check whether the data should be ignored
    if (getData().length() == 0)
        return;

    // The syntax is case insensitive, so the text is lowercased before parsing
    cString text = getData();
    text.makeLower();
    cSArray<char> ascii = text.getASCIIstring();
    Parser parser(ascii.getBuffer(),
                  ascii.getBuffer(),
                  ascii.getSize() - 1, // Reduce the null terminate character!
                  1);

    // Each line holds a single instruction, and a ';' starts a comment which
    // lasts until the end of the line
    parser.readBlanks();
    while (!parser.isEOS())
    {
        if (!isNextChar(parser, ';'))
            assembleInstruction(parser);
        skipLine(parser);
        parser.readBlanks();
    }

    // The buffered text has been consumed
    clean();
}

void IA32AssemblerStream::assembleInstruction(Parser& parser)
{
    Prefixes prefixes;
    prefixes.m_segment = -1;
    prefixes.m_lock = false;
    prefixes.m_repeat = 0;

    // Read the prefixes and the instruction name
    cString name;
    for (;;)
    {
        parser.readBlanksUntilEOL();
        ASM_CHECK(isIdentifierAhead(parser), "instruction name was expected");
        name = parser.readCString(false);
        parser.readBlanksUntilEOL();

        // A prefix is followed by the instruction (a lone 'fs' is a name)
        if (isEndOfInstruction(parser))
            break;

        if (name == "lock")
        {
            ASM_CHECK(!prefixes.m_lock, "lock prefix used twice");
            prefixes.m_lock = true;
        } else if ((name == "rep") || (name == "repe") || (name == "repz"))
        {
            ASM_CHECK(prefixes.m_repeat == 0, "repeat prefix used twice");
            prefixes.m_repeat = getPrefixOpcode("rep");
        } else if ((name == "repne") || (name == "repnz"))
        {
            ASM_CHECK(prefixes.m_repeat == 0, "repeat prefix used twice");
            prefixes.m_repeat = getPrefixOpcode("repne");
        } else if (segmentIndex(name) >= 0)
        {
            ASM_CHECK(prefixes.m_segment < 0, "segment prefix used twice");
            prefixes.m_segment = segmentIndex(name);
        } else
        {
            break;
        }
    }

    // Read the operands, separated by commas
    Operand operands[3];
    uint count = 0;
    parser.readBlanksUntilEOL();
    while (!isEndOfInstruction(parser))
    {
        ASM_CHECK(count < 3, "too many operands");
        operands[count++].readOperand(parser);
        if (isNextChar(parser, ','))
        {
            // A comma must be followed by another operand
            parser.readChar();
            parser.readBlanksUntilEOL();
            ASM_CHECK(!isEndOfInstruction(parser), "operand was expected");
        }
    }

    writeOpcode(name, prefixes, operands[0], operands[1], operands[2]);
}

//////////////////////////////////////////////////////////////////////////

void IA32AssemblerStream::writeOpcode(const cString& name,
                                      const Prefixes& prefixes,
                                      const Operand& first,
                                      const Operand& second,
                                      const Operand& third)
{
    Operand none;
    Operand a = first;
    Operand b = second;
    Operand c = third;
    cString opName = name;
    uint addressWidth = 0;
    bool hasB = (b.getKind() != Operand::NO_OPERAND);
    bool hasC = (c.getKind() != Operand::NO_OPERAND);

    // nasm spellings: 'movsx eax, ax' is cwde and 'movsx ax, al' is cbw
    if ((name == "movsx") && hasB && !hasC)
    {
        if (a.isRegister(Operand::REG_32BIT, ia32dis::IA32_GP32_EAX) &&
            b.isRegister(Operand::REG_16BIT, ia32dis::IA32_GP16_AX))
        {
            opName = "cwde";
            a = none;
            b = none;
        } else if (a.isRegister(Operand::REG_16BIT, ia32dis::IA32_GP16_AX) &&
                   b.isRegister(Operand::REG_8BIT, ia32dis::IA32_GP8_AL))
        {
            opName = "cbw";
            a = none;
            b = none;
        }
    }

    // Operands that the instruction implies: 'aad' and 'aam' are base 10 (0x0a),
    // a bare 'ffree' is ffree st1
    if (((name == "aad") || (name == "aam")) &&
        (a.getKind() == Operand::NO_OPERAND))
        a = Operand::fromText("0x0a");
    if ((name == "ffree") && (a.getKind() == Operand::NO_OPERAND))
        a = Operand::fromText("st1");

    // 'loop label, cx' and 'loop label, ecx' select the address size
    bool isLoop = (name == "loop") || (name == "loopz") ||
                  (name == "loope") || (name == "loopnz") ||
                  (name == "loopne");
    if (isLoop && hasB && !hasC)
    {
        if (b.isRegister(Operand::REG_16BIT, ia32dis::IA32_GP16_CX))
            addressWidth = 16;
        else if (b.isRegister(Operand::REG_32BIT, ia32dis::IA32_GP32_ECX))
            addressWidth = 32;
        if (addressWidth != 0)
        {
            b = none;
            hasB = false;
        }
    }

    // The segment override can come from the prefix or from the memory operand
    int segment = prefixes.m_segment;
    const Operand* operands[3] = { &a, &b, &c };
    for (uint i = 0; i < 3; i++)
    {
        if ((operands[i]->getKind() == Operand::MEMORY) &&
            (operands[i]->getSegment() >= 0))
        {
            ASM_CHECK(segment < 0, "segment prefix used twice");
            segment = operands[i]->getSegment();
        }
    }

    Encoding best;
    if (findEncoding(opName, prefixes, segment, addressWidth, a, b, c, best, true))
    {
        encodeOpcode(best, a, b, c);
        return;
    }

    // x87 register forms with an implicit ST(0): 'fadd st1' is 'fadd st0, st1'
    // and 'faddp st1' is 'faddp st1, st0'. The other way round, 'fcom st0, st1'
    // is 'fcom st1' (the table has one operand for it).
    if (a.isFpuRegister() && !hasB && !hasC)
    {
        Operand st0 = Operand::fromText("st0");
        if (findEncoding(opName, prefixes, segment, 0, st0, a, none, best, false))
        {
            encodeOpcode(best, st0, a, none);
            return;
        }
        if (findEncoding(opName, prefixes, segment, 0, a, st0, none, best, false))
        {
            encodeOpcode(best, a, st0, none);
            return;
        }
    }
    if (a.isFpuRegister() && b.isFpuRegister() && !hasC &&
        ((a.getRegisterClass() == Operand::REG_FPU) &&
         ((a.getReference() == 0) || (b.getReference() == 0))))
    {
        const Operand& single = (a.getReference() == 0) ? b : a;
        if (findEncoding(opName, prefixes, segment, 0, single, none, none, best, false))
        {
            encodeOpcode(best, single, none, none);
            return;
        }
    }

    bool hasMemory = false;
    bool hasImmediate = false;
    for (uint i = 0; i < 3; i++)
    {
        uint size = operands[i]->getSize();
        ASM_CHECK(isKnownOperandSize(size), "unsupported operand size");
        if (operands[i]->getKind() == Operand::MEMORY)
            hasMemory = (size == 0) || hasMemory;
        if (operands[i]->getKind() == Operand::IMMEDIATE)
            hasImmediate = true;
    }
    ASM_CHECK(!(hasMemory && hasImmediate),
              "operation size not specified (mov [eax], 5 needs byte, word or dword)");
    ASM_CHECK(false, "no instruction matches these operands");
}

bool IA32AssemblerStream::findEncoding(const cString& name,
                                       const Prefixes& prefixes,
                                       int segment,
                                       uint addressWidth,
                                       const Operand& first,
                                       const Operand& second,
                                       const Operand& third,
                                       Encoding& best,
                                       bool preferDcForSt0) const
{
    // Find the best encoding: the shortest instruction. The first pass uses
    // the sign-extended 8 bit immediates only, the second pass the full 8 bit
    // range (for operands that have no other choice, like 'int 0xff').
    bool found = false;
    uint bestImmediateBytes = 0;

    // nasm size suffixes: 'retw' is ret with a 16 bit operand and 'iretd' is
    // iret with a 32 bit operand. The name without the suffix is tried when the
    // name itself is not in the table.
    cString bareName;
    uint suffixWidth = 0;
    uint nameLength = name.length();
    if ((nameLength > 1) &&
        ((name[nameLength - 1] == 'w') || (name[nameLength - 1] == 'd')))
    {
        bareName = name.part(0, nameLength - 1);
        suffixWidth = (name[nameLength - 1] == 'w') ? 16 : 32;
    }

    for (uint pass = 0; (pass < 2) && !found; pass++)
    {
        for (uint tableIndex = 0; tableIndex < 3; tableIndex++)
        {
            const ia32dis::OpcodeEntry* table = (tableIndex == 0) ?
                ia32dis::gIa32OneByteOpcodeTable :
                ((tableIndex == 1) ? ia32dis::gIa32TwoBytesOpcodeTable :
                                     ia32dis::gIa32FPUOpcodeTable);
            for (const ia32dis::OpcodeEntry* opcode = table;
                 opcode->m_opcodeName != ia32dis::OPCODEEOT; opcode++)
            {
                if (opcode->m_opcodeName == ia32dis::INVALID)
                    continue;

                bool operandSizeFlip = false;
                bool addressSizeFlip = false;
                bool suffixMatch = false;
                if (!matchMnemonic(opcode->m_opcodeName, name,
                                   operandSizeFlip, addressSizeFlip))
                {
                    if ((suffixWidth == 0) ||
                        !matchMnemonic(opcode->m_opcodeName, bareName,
                                       operandSizeFlip, addressSizeFlip))
                        continue;
                    suffixMatch = true;
                    operandSizeFlip = false;
                    addressSizeFlip = false;
                }

                Encoding enc;
                enc.m_table = tableIndex;
                enc.m_opcode = opcode;
                enc.m_operandWidth = 0;
                enc.m_addressSizePrefix = addressSizeFlip ||
                    ((addressWidth != 0) && (addressWidth != getDefaultOperandWidth()));
                enc.m_hasModrm = false;
                enc.m_regFieldSet = false;
                enc.m_modrm.m_packed = 0;
                enc.m_hasSib = false;
                enc.m_sib.m_packed = 0;
                enc.m_displacementSize = 0;
                enc.m_segment = segment;
                enc.m_prefixes = prefixes;
                enc.m_length = 0;
                if (operandSizeFlip)
                    enc.m_operandWidth = (getDefaultOperandWidth() == 16) ? 32 : 16;
                if (suffixMatch)
                    enc.m_operandWidth = suffixWidth;

                // A fixed ModRM byte (x87 'fchs' is D9 E0) is part of the opcode
                if (opcode->m_modrm & ia32dis::MODRM_EXACT_FLAG)
                {
                    enc.m_hasModrm = true;
                    enc.m_regFieldSet = true;
                    enc.m_modrm.m_packed =
                        (uint8)((opcode->m_modrm >> 16) & 0xFF);
                }

                if (!matchOpcode(opcode, first, second, third, pass == 1, enc))
                    continue;

                // The modrm filter of the entry may allow memory references
                // only (fnstsw m16 is DD /7, fnstsw ax is DF E0)
                if (enc.m_hasModrm && (enc.m_modrm.m_bits.m_mod == 3) &&
                    ((opcode->m_modrm & ia32dis::MODRM_EXACT_FLAG) == 0) &&
                    ((opcode->m_modrm & ia32dis::MODRM_MOD_REGISTERS) == 0))
                    continue;

                enc.m_length = calculateLength(enc);
                if (!checkOffsets(enc, first, second, third))
                    continue;

                // The shortest instruction wins. On a tie the smaller immediate
                // wins (add ax, 0x7f is 83 /0 ib, not 05 iw), as nasm does.
                uint immediateBytes = 0;
                ia32dis::OperandType types[3] = { opcode->m_firstOperand,
                                                  opcode->m_secondOperand,
                                                  opcode->m_thridOperand };
                for (uint i = 0; i < 3; i++)
                    immediateBytes += getImmediateSize(types[i], enc);

                // nasm encodes 'fadd st0, st0' as DC C0 (ST(i), ST(0)), not D8 C0
                bool preferDc = preferDcForSt0 && (enc.m_table == 2) && found &&
                    (enc.m_length == best.m_length) &&
                    (opcode->m_prefix == 0xDC) && (best.m_opcode->m_prefix == 0xD8) &&
                    first.isFpuRegister() && second.isFpuRegister() &&
                    (first.getReference() == 0) && (second.getReference() == 0);

                if (!found || (enc.m_length < best.m_length) || preferDc ||
                    ((enc.m_length == best.m_length) &&
                     (immediateBytes < bestImmediateBytes)))
                {
                    best = enc;
                    bestImmediateBytes = immediateBytes;
                    found = true;
                }
            }
        }
    }
    return found;
}

bool IA32AssemblerStream::matchMnemonic(const char* tableName, const cString& name,
                                        bool& operandSizeFlip,
                                        bool& addressSizeFlip) const
{
    operandSizeFlip = false;
    addressSizeFlip = false;

    // The 16 bit and 32 bit spelling of the table entry
    const cString table(tableName);
    cString name16;
    cString name32;
    bool isAddressSize = false;
    int slash = indexOf(table, '/');
    int hash = indexOf(table, '#');
    if (slash >= 0)
    {
        name16 = table.part(0, slash);
        name32 = table.part(slash + 1, table.length());
    } else if (hash >= 0)
    {
        cString base = table.part(0, hash);
        cString rest = table.part(hash + 2, table.length());
        cString kind = table.part(hash + 1, hash + 2);
        if (kind == "#")
        {
            // '##' is 'w' in 16 bit and 'd' in 32 bit (movsw/movsd)
            name16 = base + "w" + rest;
            name32 = base + "d" + rest;
        } else if (kind == "d")
        {
            // '#d' is 'd' in 32 bit and nothing in 16 bit (pusha/pushad)
            name16 = base + rest;
            name32 = base + "d" + rest;
        } else
        {
            // '#e' is 'e' in 32 bit and nothing in 16 bit (jcxz/jecxz)
            ASM_CHECK(kind == "e", "invalid opcode name in table");
            isAddressSize = true;
            name16 = base + rest;
            name32 = base + "e" + rest;
        }
    } else
    {
        name16 = table;
        name32 = table;
    }

    bool is32 = (m_processorMode == IA32eInstructionSet::INTEL_32);
    const cString& own = is32 ? name32 : name16;
    const cString& other = is32 ? name16 : name32;
    if (name == own)
        return true;
    if (name == other)
    {
        // The other size of the table entry, the prefix must be changed
        if (isAddressSize)
            addressSizeFlip = true;
        else
            operandSizeFlip = true;
        return true;
    }
    return false;
}

bool IA32AssemblerStream::matchOpcode(const ia32dis::OpcodeEntry* opcode,
                                      const Operand& first,
                                      const Operand& second,
                                      const Operand& third,
                                      bool relaxed,
                                      Encoding& enc) const
{
    const ia32dis::OperandType types[3] = { opcode->m_firstOperand,
                                            opcode->m_secondOperand,
                                            opcode->m_thridOperand };
    const Operand* operands[3] = { &first, &second, &third };

    bool hasImmediate = false;
    bool hasUnsizedMemory = false;
    for (uint i = 0; i < 3; i++)
    {
        if (!matchOperand(types[i], *operands[i], relaxed, enc))
            return false;

        // The opcode encodes the register (push eax is 0x50+r), the table
        // entry for a register must match the register it encodes
        if (((types[i] == ia32dis::OPND_ONEBYTES_OPCODE_GP_8) ||
             (types[i] == ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32)) &&
            (opcode->m_prefixMask == 0xFF) &&
            (operands[i]->getReference() != (opcode->m_prefix & 7)))
            return false;

        if ((operands[i]->getKind() == Operand::MEMORY) &&
            (operands[i]->getSize() == 0) &&
            (types[i] != ia32dis::OPND_MEMREF_OFFSET_DS))
            hasUnsizedMemory = true;
        if ((operands[i]->getKind() == Operand::IMMEDIATE) ||
            (operands[i]->getKind() == Operand::FAR_POINTER))
            hasImmediate = true;
    }

    // An unsized memory operand needs another operand to define its size
    // (nasm: 'operation size not specified'). Without an immediate the size
    // is taken from the table entry.
    if (hasUnsizedMemory && hasImmediate && (enc.m_operandWidth == 0))
        return false;

    return true;
}

bool IA32AssemblerStream::setOperandWidth(uint width, Encoding& enc) const
{
    if ((enc.m_operandWidth != 0) && (enc.m_operandWidth != width))
        return false;
    enc.m_operandWidth = width;
    return true;
}

bool IA32AssemblerStream::matchOperand(ia32dis::OperandType ia32disType,
                                       const Operand& compilerOperand,
                                       bool relaxed,
                                       Encoding& enc) const
{
    switch (compilerOperand.getKind())
    {
    case Operand::NO_OPERAND:
        return (ia32disType == ia32dis::OPND_NO_OPERAND);

    case Operand::REGISTER:
    {
        Operand::RegisterClass regClass = compilerOperand.getRegisterClass();
        uint index = compilerOperand.getReference();
        bool is8 = (regClass == Operand::REG_8BIT);
        bool is16 = (regClass == Operand::REG_16BIT);
        bool is32 = (regClass == Operand::REG_32BIT);
        bool isMmx = (regClass == Operand::REG_MMX);
        bool isXmm = (regClass == Operand::REG_XMM);
        bool isFpu = (regClass == Operand::REG_FPU);

        switch (ia32disType)
        {
        // Fixed registers
        case ia32dis::OPND_AL:
            return is8 && (index == ia32dis::IA32_GP8_AL);
        case ia32dis::OPND_AX:
            // The fixed AX of 'fnstsw ax': no operand size is implied
            return is16 && (index == ia32dis::IA32_GP16_AX);
        case ia32dis::OPND_CL:
            return is8 && (index == ia32dis::IA32_GP8_CL);
        case ia32dis::OPND_DX:
            return is16 && (index == ia32dis::IA32_GP16_DX);
        case ia32dis::OPND_eAX:
        case ia32dis::OPND_eBX:
        case ia32dis::OPND_eBP:
        case ia32dis::OPND_eSI:
        case ia32dis::OPND_eDI:
        {
            // The register index of the fixed register, same for 16 and 32 bit
            uint wanted = ia32dis::IA32_GP16_AX;
            if (ia32disType == ia32dis::OPND_eBX) wanted = ia32dis::IA32_GP16_BX;
            if (ia32disType == ia32dis::OPND_eBP) wanted = ia32dis::IA32_GP16_BP;
            if (ia32disType == ia32dis::OPND_eSI) wanted = ia32dis::IA32_GP16_SI;
            if (ia32disType == ia32dis::OPND_eDI) wanted = ia32dis::IA32_GP16_DI;
            if (!(is16 || is32) || (index != wanted))
                return false;
            return setOperandWidth(is16 ? 16 : 32, enc);
        }
        case ia32dis::OPND_CS:
            return (regClass == Operand::REG_SEGMENT) && (index == ia32dis::IA32_SEG_CS);
        case ia32dis::OPND_DS:
            return (regClass == Operand::REG_SEGMENT) && (index == ia32dis::IA32_SEG_DS);
        case ia32dis::OPND_ES:
            return (regClass == Operand::REG_SEGMENT) && (index == ia32dis::IA32_SEG_ES);
        case ia32dis::OPND_SS:
            return (regClass == Operand::REG_SEGMENT) && (index == ia32dis::IA32_SEG_SS);
        case ia32dis::OPND_FS:
            return (regClass == Operand::REG_SEGMENT) && (index == ia32dis::IA32_SEG_FS);
        case ia32dis::OPND_GS:
            return (regClass == Operand::REG_SEGMENT) && (index == ia32dis::IA32_SEG_GS);

        // The register is encoded in the opcode (push eax is 0x50+r)
        case ia32dis::OPND_ONEBYTES_OPCODE_GP_8:
            return is8;
        case ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32:
            if (!(is16 || is32))
                return false;
            return setOperandWidth(is16 ? 16 : 32, enc);

        // The register is in the reg field of the modrm
        case ia32dis::OPND_GP_8BIT_MODRM:
            if (!is8) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_regOpcode = index;
            enc.m_regFieldSet = true;
            return true;
        case ia32dis::OPND_GP_16BIT_MODRM:
            if (!is16) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_regOpcode = index;
            enc.m_regFieldSet = true;
            return true;
        case ia32dis::OPND_GP_16_32BIT:
            if (!(is16 || is32))
                return false;
            if (!setOperandWidth(is16 ? 16 : 32, enc))
                return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_regOpcode = index;
            enc.m_regFieldSet = true;
            return true;
        case ia32dis::OPND_GP_SEGMENT_MODRM:
            if (regClass != Operand::REG_SEGMENT) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_regOpcode = index;
            enc.m_regFieldSet = true;
            return true;
        case ia32dis::OPND_CTRL_MODRM:
            if (regClass != Operand::REG_CONTROL) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_regOpcode = index;
            enc.m_regFieldSet = true;
            return true;
        case ia32dis::OPND_DBG_MODRM:
            if (regClass != Operand::REG_DEBUG) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_regOpcode = index;
            enc.m_regFieldSet = true;
            return true;
        case ia32dis::OPND_MMX_MODRM:
            if (!isMmx) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_regOpcode = index;
            enc.m_regFieldSet = true;
            return true;
        case ia32dis::OPND_SIMD_MODRM:
            if (!isXmm) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_regOpcode = index;
            enc.m_regFieldSet = true;
            return true;

        // x87 ST(i) in the modrm r/m field (mod 11)
        case ia32dis::OPND_FPU_ST0:
            return isFpu && (index == 0);
        case ia32dis::OPND_FPU_ST_RM:
            if (!isFpu) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_mod = 3;
            enc.m_modrm.m_bits.m_rm = index;
            return true;

        // MMX and SSE registers in the modrm r/m field (mod 11)
        case ia32dis::OPND_MMX_RM:
            if (!isMmx) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_mod = 3;
            enc.m_modrm.m_bits.m_rm = index;
            return true;
        case ia32dis::OPND_SIMD_RM:
        case ia32dis::OPND_SIMD_RM_QWORD:
            if (!isXmm) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_mod = 3;
            enc.m_modrm.m_bits.m_rm = index;
            return true;

        // The register is the modrm r/m (mod 3)
        case ia32dis::OPND_MODRM_BYTEPTR:
            if (!is8) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_mod = 3;
            enc.m_modrm.m_bits.m_rm = index;
            return true;
        case ia32dis::OPND_MODRM_WORDPTR:
            if (!is16) return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_mod = 3;
            enc.m_modrm.m_bits.m_rm = index;
            return true;
        case ia32dis::OPND_MODRM_dWORDPTR:
            if (is16 || is32)
            {
                if (!setOperandWidth(is16 ? 16 : 32, enc))
                    return false;
            } else
                return false;
            enc.m_hasModrm = true;
            enc.m_modrm.m_bits.m_mod = 3;
            enc.m_modrm.m_bits.m_rm = index;
            return true;

        default:
            return false;
        }
    }

    case Operand::IMMEDIATE:
    {
        // A size keyword sets the operand size: 'push word 0x12' is 66 6a 12
        uint size = compilerOperand.getSize();
        if ((size == 16) || (size == 32))
        {
            if (!setOperandWidth(size, enc))
                return false;
        } else if ((size != 0) && (size != 8))
            return false;
        return checkImmediate(ia32disType, compilerOperand, relaxed, enc);
    }

    case Operand::OFFSET:
        switch (ia32disType)
        {
        case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
            return compilerOperand.getBranchHint() != Operand::HINT_NEAR;
        case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
        case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
            return compilerOperand.getBranchHint() != Operand::HINT_SHORT;
        default:
            return false;
        }

    case Operand::FAR_POINTER:
        return (ia32disType == ia32dis::OPND_IMMEDIATE_OFFSET_FAR);

    case Operand::MEMORY:
        return matchMemory(ia32disType, compilerOperand, enc);
    }

    return false;
}

bool IA32AssemblerStream::checkImmediate(ia32dis::OperandType ia32disType,
                                         const Operand& compilerOperand,
                                         bool relaxed,
                                         const Encoding& enc) const
{
    int64 value = compilerOperand.getValue();
    switch (ia32disType)
    {
    case ia32dis::OPND_IMMEDIATE_8BIT:
        // Signed 8 bit, or the unsigned byte value in the relaxed pass
        // In the relaxed pass the value is truncated to the low byte, as nasm
        // does (psrld mm1, 0x12345678 is psrld mm1, 0x78)
        if (relaxed)
            return (value >= -0x80000000LL) && (value <= 0xFFFFFFFFLL);
        return (value >= -0x80) && (value <= 0x7F);

    case ia32dis::OPND_IMMEDIATE_16BIT:
        return (value >= -0x8000) && (value <= 0xFFFF);

    case ia32dis::OPND_IMMEDIATE_DS:
        if (getEncodingWidth(enc) == 16)
            return (value >= -0x8000) && (value <= 0xFFFF);
        return (value >= -0x80000000LL) && (value <= 0xFFFFFFFFLL);

    case ia32dis::OPND_ONE:
        return (value == 1);

    case ia32dis::OPND_THREE:
        // 'int 3' is cd 03 in nasm, the 0xcc form is the 'int3' mnemonic,
        // which the table does not name, so the operand never matches here
        return false;

    default:
        return false;
    }
}

bool IA32AssemblerStream::matchMemory(ia32dis::OperandType ia32disType,
                                      const Operand& compilerOperand,
                                      Encoding& enc) const
{
    uint size = compilerOperand.getSize();
    if (!isKnownOperandSize(size))
        return false;

    // 'far' selects the far pointer rows only, and they need the keyword
    if (compilerOperand.isFar() != (ia32disType == ia32dis::OPND_MODRM_FAR_OFFSET))
        return false;

    uint defaultWidth = getDefaultOperandWidth();
    bool isDirect = (compilerOperand.getBase() < 0) &&
                    (compilerOperand.getIndex() < 0);

    // Direct address with the accumulator (mov eax, [0x1234] is A1 id)
    if (ia32disType == ia32dis::OPND_MEMREF_OFFSET_DS)
    {
        if (!isDirect)
            return false;
        if ((compilerOperand.getAddressWidth() != 0) &&
            (compilerOperand.getAddressWidth() != defaultWidth))
            enc.m_addressSizePrefix = true;
        enc.m_displacementSize = (defaultWidth == 16) ? 2 : 4;
        return true;
    }

    switch (ia32disType)
    {
    case ia32dis::OPND_MODRM_MEM:
    case ia32dis::OPND_MODRM_FAR_OFFSET:
        break;
    case ia32dis::OPND_MODRM_BYTEPTR:
        if ((size != 0) && (size != 8))
            return false;
        break;
    case ia32dis::OPND_MODRM_WORDPTR:
        if ((size != 0) && (size != 16))
            return false;
        break;
    case ia32dis::OPND_MODRM_QWORDPTR:
    case ia32dis::OPND_MMX_RM:
    case ia32dis::OPND_SIMD_RM_QWORD:
        if ((size != 0) && (size != 64))
            return false;
        break;
    case ia32dis::OPND_MODRM_TWORDPTR:
        if ((size != 0) && (size != 80))
            return false;
        break;
    case ia32dis::OPND_SIMD_RM:
        if ((size != 0) && (size != 128))
            return false;
        break;
    case ia32dis::OPND_MODRM_dWORDPTR:
        if ((size == 8) || (size == 64) || (size == 80) || (size == 128))
            return false;
        // The x87 memory size selects the opcode (fild word is df /0), it
        // doesn't change the operand size
        if (enc.m_table == 2)
        {
            if (size == 16)
                return false;
            break;
        }
        // The memory size selects the operand size (mov word [eax], ax)
        if (size != 0)
        {
            if (!setOperandWidth(size, enc))
                return false;
        }
        break;
    default:
        return false;
    }

    // The address size comes from the registers of the address
    uint addressWidth = compilerOperand.getAddressWidth();
    if (addressWidth == 0)
        addressWidth = defaultWidth;
    if (addressWidth != defaultWidth)
        enc.m_addressSizePrefix = true;

    if (addressWidth == 16)
        encodeMemory16(compilerOperand, enc);
    else
        encodeMemory32(compilerOperand, enc);
    enc.m_hasModrm = true;
    return true;
}

void IA32AssemblerStream::encodeMemory32(const Operand& compilerOperand,
                                         Encoding& enc)
{
    int base = compilerOperand.getBase();
    int index = compilerOperand.getIndex();
    uint scale = compilerOperand.getScale();
    int64 displacement = compilerOperand.getDisplacement();

    enc.m_hasSib = false;

    // [disp32]: mod 00, rm 101
    if ((base < 0) && (index < 0))
    {
        enc.m_modrm.m_bits.m_mod = 0;
        enc.m_modrm.m_bits.m_rm = 5;
        enc.m_displacementSize = 4;
        return;
    }

    // [index*4+disp32] and [index*8+disp32]: SIB with no base (base 101)
    if (base < 0)
    {
        ASM_CHECK((scale == 4) || (scale == 8), "invalid scale");
        enc.m_modrm.m_bits.m_mod = 0;
        enc.m_modrm.m_bits.m_rm = 4;
        enc.m_hasSib = true;
        enc.m_sib.m_bits.m_scale = scaleBits(scale);
        enc.m_sib.m_bits.m_index = index;
        enc.m_sib.m_bits.m_base = ia32dis::IA32_GP32_EBP;
        enc.m_displacementSize = 4;
        return;
    }

    // Displacement: none for 0 (except EBP as base, which needs disp8 0),
    // disp8 for -128..127, disp32 otherwise
    uint mod;
    uint displacementSize;
    if ((displacement == 0) && (base != ia32dis::IA32_GP32_EBP))
    {
        mod = 0;
        displacementSize = 0;
    } else if ((displacement >= -0x80) && (displacement <= 0x7F))
    {
        mod = 1;
        displacementSize = 1;
    } else
    {
        mod = 2;
        displacementSize = 4;
    }

    // ESP as base and any index need the SIB byte
    bool needsSib = (index >= 0) || (base == ia32dis::IA32_GP32_ESP);
    enc.m_modrm.m_bits.m_mod = mod;
    enc.m_displacementSize = displacementSize;
    if (needsSib)
    {
        enc.m_modrm.m_bits.m_rm = 4;
        enc.m_hasSib = true;
        enc.m_sib.m_bits.m_scale = (index >= 0) ? scaleBits(scale) : 0;
        enc.m_sib.m_bits.m_index = (index >= 0) ? index : ia32dis::IA32_GP32_ESP;
        enc.m_sib.m_bits.m_base = base;
    } else
        enc.m_modrm.m_bits.m_rm = base;
}

void IA32AssemblerStream::encodeMemory16(const Operand& compilerOperand,
                                         Encoding& enc) const
{
    int base = compilerOperand.getBase();
    int index = compilerOperand.getIndex();
    int64 displacement = compilerOperand.getDisplacement();

    enc.m_hasSib = false;

    // [disp16]: mod 00, rm 110
    if ((base < 0) && (index < 0))
    {
        enc.m_modrm.m_bits.m_mod = 0;
        enc.m_modrm.m_bits.m_rm = 6;
        enc.m_displacementSize = 2;
        return;
    }

    // The table lists the register pairs for each modrm value
    uint first = (base >= 0) ? (uint)base : ia32dis::NO_REGISTER;
    uint second = (index >= 0) ? (uint)index : ia32dis::NO_REGISTER;

    uint firstMod = 2;
    if (displacement == 0)
        firstMod = 0;
    else if ((displacement >= -0x80) && (displacement <= 0x7F))
        firstMod = 1;

    for (uint mod = firstMod; mod <= 2; mod++)
    {
        for (uint rm = 0; rm < ia32dis::MODRM_RM_COUNT; rm++)
        {
            const ia32dis::ModRMTranslation& entry =
                ia32dis::gIa32ModRM16[mod][rm];
            if (!entry.m_isReference)
                continue;
            bool isMatch =
                ((entry.m_firstRegisterPointer == first) &&
                 (entry.m_secondRegisterPointer == second)) ||
                ((entry.m_firstRegisterPointer == second) &&
                 (entry.m_secondRegisterPointer == first));
            if (!isMatch)
                continue;

            enc.m_modrm.m_bits.m_mod = mod;
            enc.m_modrm.m_bits.m_rm = rm;
            enc.m_displacementSize = (mod == 0) ? 0 : ((mod == 1) ? 1 : 2);
            return;
        }
    }

    // Not a 16 bit addressing form (e.g. [ax])
    ASM_CHECK(false, "invalid 16 bit address");
}

uint IA32AssemblerStream::calculateLength(const Encoding& enc) const
{
    // The table entry gives the sizes of the immediates
    ia32dis::OperandType types[3] = { enc.m_opcode->m_firstOperand,
                                      enc.m_opcode->m_secondOperand,
                                      enc.m_opcode->m_thridOperand };

    uint length = 0;
    // Prefixes: segment, address size, lock, operand size, repeat, mandatory
    if (enc.m_segment >= 0)
        length++;
    if (enc.m_addressSizePrefix)
        length++;
    if (enc.m_prefixes.m_lock)
        length++;
    if (needsOperandSizePrefix(enc))
        length++;
    if (enc.m_prefixes.m_repeat != 0)
        length++;
    if (enc.m_opcode->m_mandatoryPrefix != ia32dis::MANDATORY_NONE)
        length++;

    // Opcode, modrm, sib and displacement
    if (enc.m_table == 1)
        length++;
    length++;
    if (enc.m_hasModrm)
        length++;
    if (enc.m_hasSib)
        length++;
    length += enc.m_displacementSize;

    // Immediates and offsets
    for (uint i = 0; i < 3; i++)
        length += getImmediateSize(types[i], enc);
    return length;
}

bool IA32AssemblerStream::checkOffsets(const Encoding& enc,
                                       const Operand& first,
                                       const Operand& second,
                                       const Operand& third) const
{
    const Operand* operands[3] = { &first, &second, &third };
    ia32dis::OperandType types[3] = { enc.m_opcode->m_firstOperand,
                                      enc.m_opcode->m_secondOperand,
                                      enc.m_opcode->m_thridOperand };
    for (uint i = 0; i < 3; i++)
    {
        if (operands[i]->getKind() != Operand::OFFSET)
            continue;

        // The offset is measured from the start of the instruction
        int64 relative = operands[i]->getValue() - (int64)enc.m_length;
        if (types[i] == ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8)
        {
            if ((relative < -0x80) || (relative > 0x7F))
                return false;
        } else
        {
            // Near offsets: 16 or 32 bit according to the operand size
            if (getEncodingWidth(enc) == 16)
            {
                if ((relative < -0x8000) || (relative > 0x7FFF))
                    return false;
            } else if ((relative < -0x80000000LL) || (relative > 0x7FFFFFFFLL))
                return false;
        }
    }
    return true;
}

uint IA32AssemblerStream::getImmediateSize(ia32dis::OperandType ia32disType,
                                           const Encoding& enc) const
{
    switch (ia32disType)
    {
    case ia32dis::OPND_IMMEDIATE_8BIT:
    case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
        return 1;
    case ia32dis::OPND_IMMEDIATE_16BIT:
        return 2;
    case ia32dis::OPND_IMMEDIATE_DS:
    case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
        return getEncodingWidth(enc) / 8;
    case ia32dis::OPND_IMMEDIATE_OFFSET_FAR:
        // offset16/32 followed by the segment
        return getEncodingWidth(enc) / 8 + 2;
    default:
        return 0;
    }
}

void IA32AssemblerStream::encodeOpcode(const Encoding& enc,
                                       const Operand& first,
                                       const Operand& second,
                                       const Operand& third)
{
    const Operand* operands[3] = { &first, &second, &third };
    ia32dis::OperandType types[3] = { enc.m_opcode->m_firstOperand,
                                      enc.m_opcode->m_secondOperand,
                                      enc.m_opcode->m_thridOperand };

    // Prefixes in nasm order: segment, address size, lock, operand size,
    // repeat, and the mandatory prefix (66, F2, F3) which is right before 0F
    if (enc.m_segment >= 0)
        m_binary.appendUint8(getSegmentPrefix(enc.m_segment));
    if (enc.m_addressSizePrefix)
        m_binary.appendUint8(ia32dis::gIa32PrefixGroup4AddressSize);
    if (enc.m_prefixes.m_lock)
        m_binary.appendUint8(getPrefixOpcode("lock"));
    if (needsOperandSizePrefix(enc))
        m_binary.appendUint8(ia32dis::gIa32PrefixGroup3OperandSize);
    if (enc.m_prefixes.m_repeat != 0)
        m_binary.appendUint8(enc.m_prefixes.m_repeat);
    if (enc.m_opcode->m_mandatoryPrefix != ia32dis::MANDATORY_NONE)
        m_binary.appendUint8(enc.m_opcode->m_mandatoryPrefix);

    // The table index prefix. Used only for two bytes table (the x87 table
    // has its escape as the opcode prefix)
    if (enc.m_table == 1)
        m_binary.appendUint8(ia32dis::gIa32TwoByteEscapeCharacter);

    // The opcode. The register of a '+r' opcode is added into the opcode
    uint8 opcodeData = enc.m_opcode->m_prefix;
    for (uint i = 0; i < 3; i++)
    {
        if ((types[i] == ia32dis::OPND_ONEBYTES_OPCODE_GP_8) ||
            (types[i] == ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32))
            opcodeData |= (uint8)(operands[i]->getReference() &
                                  (~enc.m_opcode->m_prefixMask));
    }
    m_binary.appendUint8(opcodeData);

    // The modrm, sib and displacement
    if (enc.m_hasModrm)
    {
        IA32OpcodeDatastruct::MODRM modrm = enc.m_modrm;

        // Fix modrm according to filter opcode name (the /digit of the opcode).
        // An operand register in the reg field wins over the filter.
        if ((enc.m_opcode->m_modrm != ia32dis::MODRM_ALL_FILTER) && !enc.m_regFieldSet)
        {
            if ((enc.m_opcode->m_modrm & ia32dis::MODRM_MOD_000) != 0)
                modrm.m_bits.m_regOpcode = 0;
            if ((enc.m_opcode->m_modrm & ia32dis::MODRM_MOD_001) != 0)
                modrm.m_bits.m_regOpcode = 1;
            if ((enc.m_opcode->m_modrm & ia32dis::MODRM_MOD_010) != 0)
                modrm.m_bits.m_regOpcode = 2;
            if ((enc.m_opcode->m_modrm & ia32dis::MODRM_MOD_011) != 0)
                modrm.m_bits.m_regOpcode = 3;
            if ((enc.m_opcode->m_modrm & ia32dis::MODRM_MOD_100) != 0)
                modrm.m_bits.m_regOpcode = 4;
            if ((enc.m_opcode->m_modrm & ia32dis::MODRM_MOD_101) != 0)
                modrm.m_bits.m_regOpcode = 5;
            if ((enc.m_opcode->m_modrm & ia32dis::MODRM_MOD_110) != 0)
                modrm.m_bits.m_regOpcode = 6;
            if ((enc.m_opcode->m_modrm & ia32dis::MODRM_MOD_111) != 0)
                modrm.m_bits.m_regOpcode = 7;
        }
        m_binary.appendUint8(modrm.m_packed);
    }

    if (enc.m_hasSib)
        m_binary.appendUint8(enc.m_sib.m_packed);

    // The memory displacement (the memory operand is the only one)
    if (enc.m_displacementSize != 0)
    {
        for (uint i = 0; i < 3; i++)
        {
            if (operands[i]->getKind() == Operand::MEMORY)
            {
                writeValue(operands[i]->getDisplacement(), enc.m_displacementSize);
                break;
            }
        }
    }

    // Immediates and offsets, in operand order
    for (uint i = 0; i < 3; i++)
        writeImmediate(types[i], *operands[i], enc);
}

void IA32AssemblerStream::writeImmediate(ia32dis::OperandType ia32disType,
                                         const Operand& compilerOperand,
                                         const Encoding& enc)
{
    uint width = getEncodingWidth(enc) / 8;
    int64 value = compilerOperand.getValue();
    switch (ia32disType)
    {
    case ia32dis::OPND_IMMEDIATE_8BIT:
        writeValue(value, 1);
        break;
    case ia32dis::OPND_IMMEDIATE_16BIT:
        writeValue(value, 2);
        break;
    case ia32dis::OPND_IMMEDIATE_DS:
        writeValue(value, width);
        break;
    case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
        writeValue(value - (int64)enc.m_length, 1);
        break;
    case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
        writeValue(value - (int64)enc.m_length, width);
        break;
    case ia32dis::OPND_IMMEDIATE_OFFSET_FAR:
        writeValue(value, width);
        writeValue(compilerOperand.getSegmentValue(), 2);
        break;
    default:
        // Registers, memory displacement and the implicit operands
        break;
    }
}

void IA32AssemblerStream::writeValue(int64 value, uint bytes)
{
    uint8 buffer[sizeof(uint32)];
    for (uint i = 0; i < bytes; i++)
        buffer[i] = (uint8)(value >> (8 * i));
    m_binary.appendBuffer(buffer, bytes);
}

bool IA32AssemblerStream::needsOperandSizePrefix(const Encoding& enc) const
{
    // A mandatory 66 prefix is the operand size prefix too (movd xmm, eax)
    if (enc.m_opcode->m_mandatoryPrefix == ia32dis::MANDATORY_66)
        return false;
    return getEncodingWidth(enc) != getDefaultOperandWidth();
}

uint IA32AssemblerStream::getDefaultOperandWidth() const
{
    return (m_processorMode == IA32eInstructionSet::INTEL_16) ? 16 : 32;
}

uint IA32AssemblerStream::getEncodingWidth(const Encoding& enc) const
{
    return (enc.m_operandWidth != 0) ? enc.m_operandWidth : getDefaultOperandWidth();
}

uint8 IA32AssemblerStream::getSegmentPrefix(uint segment)
{
    // The table name of the segment prefix is the register name and ':'
    cString name(ia32dis::gIa32SegmentsRegisters[segment].m_name);
    name += XSTL_CHAR(':');
    return getPrefixOpcode(name);
}

uint8 IA32AssemblerStream::getPrefixOpcode(const cString& name)
{
    for (uint i = 0; i < ia32dis::IA32_NUMBER_OF_PREFIXS; i++)
    {
        if (name == ia32dis::gIa32PrefixTable[i].m_prefixName)
            return ia32dis::gIa32PrefixTable[i].m_opcode;
    }
    ASM_CHECK(false, "prefix is not in the table");
    return 0;
}

//////////////////////////////////////////////////////////////////////////
//////////
// IA32AssemblerStream::Operand

IA32AssemblerStream::Operand::Operand() :
    m_kind(NO_OPERAND),
    m_size(0),
    m_regClass(REG_8BIT),
    m_reference(0),
    m_value(0),
    m_hasDisplacement(false),
    m_segmentValue(0),
    m_hint(HINT_NONE),
    m_far(false),
    m_segment(-1),
    m_base(-1),
    m_index(-1),
    m_scale(1),
    m_addressWidth(0)
{
}

IA32AssemblerStream::Operand IA32AssemblerStream::Operand::fromText(const cString& text)
{
    cSArray<char> ascii = text.getASCIIstring();
    Parser parser(ascii.getBuffer(),
                  ascii.getBuffer(),
                  ascii.getSize() - 1, // Reduce the null terminate character!
                  1);
    Operand operand;
    operand.readOperand(parser);
    return operand;
}

void IA32AssemblerStream::Operand::readOperand(Parser& parser)
{
    parser.readBlanksUntilEOL();

    // Branch hint: 'short $+N' or 'near $+N'
    cString word = peekWord(parser);
    if ((word == "short") || (word == "near"))
    {
        m_hint = (word == "short") ? HINT_SHORT : HINT_NEAR;
        skipWord(parser);
    }

    // Far keyword: 'jmp far dword [eax]'
    if (peekWord(parser) == "far")
    {
        m_far = true;
        skipWord(parser);
    }

    // Size keyword (the 'ptr' is optional)
    m_size = readSizeKeyword(parser);

    // A word which starts with a letter is a register name, or a segment
    // override when it is followed by ':' ('fs:[eax]')
    int segment = -1;
    cString name;
    if (isLetterAhead(parser))
    {
        cString token = parser.readCString(false);
        if ((segmentIndex(token) >= 0) && isNextChar(parser, ':'))
        {
            segment = segmentIndex(token);
            parser.readChar();
            parser.readBlanksUntilEOL();
        } else
        {
            name = token;
        }
    }

    if (name.length() != 0)
    {
        ASM_CHECK(checkRegister(name, m_regClass, m_reference),
                  "unknown register or operand");
        ASM_CHECK(m_size == 0, "size keyword can't be used with a register");
        m_kind = REGISTER;
    } else if (isNextChar(parser, '['))
    {
        parser.readChar();
        m_kind = MEMORY;
        m_segment = segment;
        readAddress(parser);
        ASM_CHECK(isNextChar(parser, ']'), "']' was expected");
        parser.readChar();
    } else if (segment >= 0)
    {
        ASM_CHECK(false, "segment override must be followed by a memory reference");
    } else if (isNextChar(parser, '$'))
    {
        readOffset(parser);
    } else if (isNextChar(parser, '-') || isNextChar(parser, '+') ||
               isDecimalAhead(parser))
    {
        bool negative = parser.readSignedValue();
        uint64 number = readNumber(parser);

        if (isNextChar(parser, ':') && !negative)
        {
            // Far pointer: segment:offset
            parser.readChar();
            ASM_CHECK(isDecimalAhead(parser), "offset was expected");
            uint64 offset = readNumber(parser);
            m_kind = FAR_POINTER;
            m_segmentValue = (uint32)number;
            m_value = (int64)offset;
        } else
        {
            m_kind = IMMEDIATE;
            m_value = negative ? -(int64)number : (int64)number;
        }
    } else
    {
        ASM_CHECK(false, "operand was expected");
    }

    parser.readBlanksUntilEOL();
    ASM_CHECK(isEndOfOperand(parser), "unexpected text after the operand");
}

void IA32AssemblerStream::Operand::readOffset(Parser& parser)
{
    // '$' (the start of the instruction), '$+N' or '$-N'
    parser.readChar();
    parser.readBlanksUntilEOL();
    bool negative = false;
    if (isNextChar(parser, '-') || isNextChar(parser, '+'))
    {
        negative = parser.readSignedValue();
        parser.readBlanksUntilEOL();
    }
    uint64 number = 0;
    if (isDecimalAhead(parser))
        number = readNumber(parser);
    m_kind = OFFSET;
    m_value = negative ? -(int64)number : (int64)number;
}

void IA32AssemblerStream::Operand::readAddress(Parser& parser)
{
    parser.readBlanksUntilEOL();

    // Segment override inside the brackets: '[fs:eax]'. A segment register
    // which is not followed by ':' is rejected as an address register below.
    int segment = segmentIndex(peekWord(parser));
    if (segment >= 0)
    {
        parser.readCString(false);
        ASM_CHECK(isNextChar(parser, ':'), "register was expected in address");
        parser.readChar();
        ASM_CHECK(m_segment < 0, "segment prefix used twice");
        m_segment = segment;
        parser.readBlanksUntilEOL();
    }

    // Terms of the form [+-]number, [+-]reg, [+-]number*reg, [+-]reg*number
    int sign = 1;
    if (isNextChar(parser, '-'))
    {
        sign = -1;
        parser.readChar();
    } else if (isNextChar(parser, '+'))
        parser.readChar();

    for (;;)
    {
        parser.readBlanksUntilEOL();
        if (isDecimalAhead(parser))
        {
            uint64 number = readNumber(parser);
            parser.readBlanksUntilEOL();
            if (isNextChar(parser, '*'))
            {
                // number*reg
                parser.readChar();
                parser.readBlanksUntilEOL();
                ASM_CHECK(isLetterAhead(parser), "register was expected after '*'");
                cString name = parser.readCString(false);
                RegisterClass regClass;
                uint index;
                ASM_CHECK(checkRegister(name, regClass, index) &&
                          ((regClass == REG_32BIT) || (regClass == REG_16BIT)),
                          "register was expected in address");
                addAddressRegister(regClass, index, (uint)number, sign);
            } else
            {
                m_value += sign * (int64)number;
                m_hasDisplacement = true;
            }
        } else if (isLetterAhead(parser))
        {
            cString name = parser.readCString(false);
            RegisterClass regClass;
            uint index;
            ASM_CHECK(checkRegister(name, regClass, index) &&
                      ((regClass == REG_32BIT) || (regClass == REG_16BIT)),
                      "register was expected in address");
            parser.readBlanksUntilEOL();
            uint scale = 0;
            if (isNextChar(parser, '*'))
            {
                // reg*number
                parser.readChar();
                parser.readBlanksUntilEOL();
                ASM_CHECK(isDecimalAhead(parser), "scale was expected after '*'");
                scale = (uint)readNumber(parser);
                parser.readBlanksUntilEOL();
            }
            addAddressRegister(regClass, index, scale, sign);
        } else
        {
            ASM_CHECK(false, "address was expected");
        }

        parser.readBlanksUntilEOL();
        if (isNextChar(parser, ']'))
            break;
        ASM_CHECK(isNextChar(parser, '+') || isNextChar(parser, '-'),
                  "'+' or '-' was expected in address");
        sign = isNextChar(parser, '-') ? -1 : 1;
        parser.readChar();
    }

    // Normalize the address: nasm encodes [eax*1] as [eax], and [eax*2] as
    // [eax+eax*1]. Without a base the index must be encodable as a base.
    if (m_addressWidth == 32)
    {
        if ((m_index >= 0) && (m_base < 0))
        {
            if (m_scale == 1)
            {
                m_base = m_index;
                m_index = -1;
            } else if (m_scale == 2)
            {
                m_base = m_index;
                m_scale = 1;
            }
        }
        ASM_CHECK(m_index != (int)ia32dis::IA32_GP32_ESP,
                  "esp can't be used as an index register");
    } else if (m_addressWidth == 16)
    {
        // A single 16 bit register is the base
        if ((m_index >= 0) && (m_base < 0))
        {
            m_base = m_index;
            m_index = -1;
        }
    }
    ASM_CHECK((m_base >= 0) || (m_index >= 0) || m_hasDisplacement,
              "empty address");
}

void IA32AssemblerStream::Operand::addAddressRegister(RegisterClass regClass,
                                                      uint index,
                                                      uint scale,
                                                      int sign)
{
    ASM_CHECK(sign > 0, "a register can't be subtracted");

    // The address size is defined by the first register
    uint addressWidth = (regClass == REG_16BIT) ? 16 : 32;
    if (m_addressWidth == 0)
        m_addressWidth = addressWidth;
    ASM_CHECK(m_addressWidth == addressWidth,
              "16 and 32 bit registers can't be mixed in an address");

    if (addressWidth == 16)
    {
        // Only bx, bp, si and di can be used in 16 bit addresses
        ASM_CHECK((index == ia32dis::IA32_GP16_BX) || (index == ia32dis::IA32_GP16_BP) ||
                  (index == ia32dis::IA32_GP16_SI) || (index == ia32dis::IA32_GP16_DI),
                  "only bx, bp, si and di can be used in 16 bit addresses");
        ASM_CHECK(scale == 0, "scale is not allowed in 16 bit addresses");
    }

    if (scale != 0)
    {
        // Scaled index: reg*scale
        ASM_CHECK((scale == 1) || (scale == 2) || (scale == 4) || (scale == 8),
                  "scale must be 1, 2, 4 or 8");
        ASM_CHECK(m_index < 0, "only one index register is allowed");
        m_index = index;
        m_scale = scale;
    } else if (m_base < 0)
    {
        m_base = index;
    } else
    {
        ASM_CHECK(m_index < 0, "only one index register is allowed");
        m_index = index;
        m_scale = 1;
    }
}

bool IA32AssemblerStream::Operand::isRegister(RegisterClass regClass, uint index) const
{
    return (m_kind == REGISTER) && (m_regClass == regClass) && (m_reference == index);
}

bool IA32AssemblerStream::Operand::isFpuRegister() const
{
    return (m_kind == REGISTER) && (m_regClass == REG_FPU);
}

IA32AssemblerStream::Operand::Kind IA32AssemblerStream::Operand::getKind() const
{
    return m_kind;
}

uint IA32AssemblerStream::Operand::getSize() const
{
    return m_size;
}

IA32AssemblerStream::Operand::RegisterClass
    IA32AssemblerStream::Operand::getRegisterClass() const
{
    return m_regClass;
}

uint IA32AssemblerStream::Operand::getReference() const
{
    return m_reference;
}

int64 IA32AssemblerStream::Operand::getValue() const
{
    return m_value;
}

uint32 IA32AssemblerStream::Operand::getSegmentValue() const
{
    return m_segmentValue;
}

IA32AssemblerStream::Operand::BranchHint
    IA32AssemblerStream::Operand::getBranchHint() const
{
    return m_hint;
}

bool IA32AssemblerStream::Operand::isFar() const
{
    return m_far;
}

int IA32AssemblerStream::Operand::getSegment() const
{
    return m_segment;
}

int IA32AssemblerStream::Operand::getBase() const
{
    return m_base;
}

int IA32AssemblerStream::Operand::getIndex() const
{
    return m_index;
}

uint IA32AssemblerStream::Operand::getScale() const
{
    return m_scale;
}

uint IA32AssemblerStream::Operand::getAddressWidth() const
{
    return m_addressWidth;
}

int64 IA32AssemblerStream::Operand::getDisplacement() const
{
    return m_value;
}

bool IA32AssemblerStream::Operand::checkRegister(const cString& regname,
                                                 RegisterClass& regClass,
                                                 uint& regIndex)
{
    for (uint i = 0; i < ia32dis::NUMBER_OF_REGISTERS; i++)
    {
        if (regname == ia32dis::gIa8Registers[i].m_name)
        {
            regClass = REG_8BIT;
            regIndex = i;
            return true;
        }
        if (regname == ia32dis::gIa16Registers[i].m_name)
        {
            regClass = REG_16BIT;
            regIndex = i;
            return true;
        }
        if (regname == ia32dis::gIa32Registers[i].m_name)
        {
            regClass = REG_32BIT;
            regIndex = i;
            return true;
        }
        if (regname == ia32dis::gIa32SIMDRegisters[i].m_name)
        {
            regClass = REG_XMM;
            regIndex = i;
            return true;
        }
        if (regname == ia32dis::gIa32MMXRegisters[i].m_name)
        {
            regClass = REG_MMX;
            regIndex = i;
            return true;
        }
        if (regname == ia32dis::gIa32DebugRegisters[i].m_name)
        {
            regClass = REG_DEBUG;
            regIndex = i;
            return true;
        }
    }

    for (uint i = 0; i < ia32dis::NUMBER_OF_CONTROL_REGISTERS; i++)
    {
        if (regname == ia32dis::gIa32ControlRegisters[i].m_name)
        {
            regClass = REG_CONTROL;
            regIndex = i;
            return true;
        }
    }

    for (uint i = 0; i < ia32dis::NUMBER_OF_SEGMENTS_REGISTERS; i++)
    {
        if (regname == ia32dis::gIa32SegmentsRegisters[i].m_name)
        {
            regClass = REG_SEGMENT;
            regIndex = i;
            return true;
        }
    }
    // The x87 stack: 'st' is 'st0', 'st0' to 'st7' are the stack entries
    if (regname == "st")
    {
        regClass = REG_FPU;
        regIndex = 0;
        return true;
    }
    if ((regname.length() == 3) && (regname[0] == 's') && (regname[1] == 't') &&
        (regname[2] >= '0') && (regname[2] <= '7'))
    {
        regClass = REG_FPU;
        regIndex = (uint)(regname[2] - '0');
        return true;
    }
    return false;
}
