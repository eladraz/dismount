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
// or hex with a h suffix (7bh), and it must fit in 64 bits.
uint64 readNumber(Parser& parser)
{
    ASM_CHECK(isDecimalAhead(parser), "number was expected");
    uint64 value = 0;
    ASM_CHECK(parser.isUnsignedInteger64(&value, true), "invalid number");
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
    prefixes.m_operandWidth = 0;
    prefixes.m_addressWidth = 0;

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
        } else if ((name == "o16") || (name == "o32") || (name == "o64"))
        {
            // nasm: the operand size of the instruction
            ASM_CHECK(prefixes.m_operandWidth == 0, "operand size prefix used twice");
            prefixes.m_operandWidth = (name == "o16") ? 16 : ((name == "o32") ? 32 : 64);
        } else if ((name == "a16") || (name == "a32") || (name == "a64"))
        {
            // nasm: the address size of the instruction
            ASM_CHECK(prefixes.m_addressWidth == 0, "address size prefix used twice");
            prefixes.m_addressWidth = (name == "a16") ? 16 : ((name == "a32") ? 32 : 64);
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

/*
 * The nasm pseudo-ops which spell the imm8 of an instruction in the mnemonic:
 * cmpltps xmm1, xmm2 is cmpps xmm1, xmm2, 1 and pclmulhqlqdq is pclmulqdq
 * with the imm8 0x01. Return true and the instruction and its imm8.
 */
static bool splitPseudoOpcode(const cString& name, cString& base, uint& immediate)
{
    static const char* predicates[] = {"eq", "lt", "le", "unord",
                                       "neq", "nlt", "nle", "ord"};
    static const char* compares[] = {"ps", "ss", "pd", "sd"};
    for (uint i = 0; i < sizeof(predicates) / sizeof(predicates[0]); i++)
        for (uint j = 0; j < sizeof(compares) / sizeof(compares[0]); j++)
            if (name == cString("cmp") + predicates[i] + compares[j])
            {
                base = cString("cmp") + compares[j];
                immediate = i;
                return true;
            }

    static const char* clmuls[] = {"pclmullqlqdq", "pclmulhqlqdq",
                                   "pclmullqhqdq", "pclmulhqhqdq"};
    static const uint clmulImmediates[] = {0x00, 0x01, 0x10, 0x11};
    for (uint i = 0; i < sizeof(clmuls) / sizeof(clmuls[0]); i++)
        if (name == clmuls[i])
        {
            base = "pclmulqdq";
            immediate = clmulImmediates[i];
            return true;
        }
    return false;
}

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
    uint addressWidth = prefixes.m_addressWidth;
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

    // 64 bit mode: 'movsx rax, eax' and 'movsxd rax, eax' are cdqe (48 98), the
    // other 'movsx r64, r/m32' are movsxd
    if (is64BitMode() && ((name == "movsx") || (name == "movsxd")) &&
        hasB && !hasC && (a.getKind() == Operand::REGISTER) &&
        (a.getRegisterClass() == Operand::REG_64BIT) &&
        (b.isRegister(Operand::REG_32BIT, b.getReference()) ||
         ((b.getKind() == Operand::MEMORY) && (b.getSize() == 32))))
    {
        if (a.isRegister(Operand::REG_64BIT, ia32dis::AMD64_GP64_RAX) &&
            b.isRegister(Operand::REG_32BIT, ia32dis::IA32_GP32_EAX))
        {
            opName = "cdqe";
            a = none;
            b = none;
        } else
            opName = "movsxd";
    }

    // Operands that the instruction implies: 'aad' and 'aam' are base 10 (0x0a),
    // a bare 'ffree' is ffree st1
    if (((name == "aad") || (name == "aam")) &&
        (a.getKind() == Operand::NO_OPERAND))
        a = Operand::fromText("0x0a");
    if ((name == "ffree") && (a.getKind() == Operand::NO_OPERAND))
        a = Operand::fromText("st1");

    // The predicate or the imm8 which is spelled in the mnemonic
    uint pseudoImmediate = 0;
    cString pseudoBase;
    if (splitPseudoOpcode(name, pseudoBase, pseudoImmediate) && hasB && !hasC)
    {
        opName = pseudoBase;
        c = Operand::fromText(cString((int32)pseudoImmediate));
        hasC = true;
    }

    // 'loop label, cx' and 'loop label, ecx' select the address size (and so
    // do 'jcxz label, cx' and the others, as the disassembler writes them)
    bool isLoop = (name == "loop") || (name == "loopz") ||
                  (name == "loope") || (name == "loopnz") ||
                  (name == "loopne") || (name == "jcxz") ||
                  (name == "jecxz") || (name == "jrcxz");
    if (isLoop && hasB && !hasC)
    {
        uint loopWidth = 0;
        if (b.isRegister(Operand::REG_16BIT, ia32dis::IA32_GP16_CX))
            loopWidth = 16;
        else if (b.isRegister(Operand::REG_32BIT, ia32dis::IA32_GP32_ECX))
            loopWidth = 32;
        else if (is64BitMode() &&
                 b.isRegister(Operand::REG_64BIT, ia32dis::AMD64_GP64_RAX + ia32dis::IA32_GP32_ECX))
            loopWidth = 64;
        if (loopWidth != 0)
        {
            addressWidth = loopWidth;
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

/*
 * Return true for the mnemonics which nasm spells with a size suffix when the
 * operand size is not the default one: retw, iretd, leaveq, pushfw, ...
 */
static bool acceptsSizeSuffix(const cString& name)
{
    static const char* names[] = {"ret", "retf", "iret", "leave", "enter",
                                  "pushf", "popf", "pusha", "popa"};
    for (uint i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (name == names[i])
            return true;
    return false;
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
    const bool mode64 = is64BitMode();

    // nasm size suffixes: 'retw' is ret with a 16 bit operand and 'iretd' is
    // iret with a 32 bit operand ('retq' and 'iretq' with a 64 bit operand in
    // 64 bit mode). The name without the suffix is tried when the name itself
    // is not in the table.
    cString bareName;
    uint suffixWidth = 0;
    uint nameLength = name.length();
    if ((nameLength > 1) && acceptsSizeSuffix(name.part(0, nameLength - 1)) &&
        ((name[nameLength - 1] == 'w') || (name[nameLength - 1] == 'd') ||
         (mode64 && (name[nameLength - 1] == 'q'))))
    {
        bareName = name.part(0, nameLength - 1);
        suffixWidth = (name[nameLength - 1] == 'w') ? 16 :
                      ((name[nameLength - 1] == 'd') ? 32 : 64);
    }

    // The tables. The entries of 64 bit mode come last: they replace the
    // entries of the IA-32 tables which have the same encoding (see
    // isReplacedByAmd64Entry())
    enum { NUMBER_OF_SOURCES = 7 };
    static const ia32dis::OpcodeEntry* const tables[NUMBER_OF_SOURCES] = {
        ia32dis::gIa32OneByteOpcodeTable,
        ia32dis::gIa32TwoBytesOpcodeTable,
        ia32dis::gIa32FPUOpcodeTable,
        ia32dis::gIa32ThreeBytes38OpcodeTable,
        ia32dis::gIa32ThreeBytes3AOpcodeTable,
        ia32dis::gAmd64OneByteOpcodeTable,
        ia32dis::gAmd64TwoBytesOpcodeTable };
    // The value of Encoding::m_table for each source
    static const uint tableIndexes[NUMBER_OF_SOURCES] = { 0, 1, 2, 3, 4, 0, 1 };

    for (uint pass = 0; (pass < 3) && !found; pass++)
    {
        for (uint source = 0; source < NUMBER_OF_SOURCES; source++)
        {
            // The entries of 64 bit mode exist only in 64 bit mode
            if ((source >= 5) && !mode64)
                continue;
            const uint tableIndex = tableIndexes[source];
            const ia32dis::OpcodeEntry* table = tables[source];
            for (const ia32dis::OpcodeEntry* opcode = table;
                 opcode->m_opcodeName != ia32dis::OPCODEEOT; opcode++)
            {
                if (opcode->m_opcodeName == ia32dis::INVALID)
                    continue;

                // The entries which exist in one processor mode only
                if (mode64 ? (opcode->m_mode64 == ia32dis::MODE64_INVALID) :
                             (opcode->m_mode64 == ia32dis::MODE64_ONLY))
                    continue;
                if (mode64 && (source < 2) && isReplacedByAmd64Entry(opcode, source))
                    continue;

                uint operandWidth = 0;
                bool addressSizeFlip = false;
                bool suffixMatch = false;
                if (!matchMnemonic(opcode->m_opcodeName, name,
                                   operandWidth, addressSizeFlip))
                {
                    if ((suffixWidth == 0) ||
                        !matchMnemonic(opcode->m_opcodeName, bareName,
                                       operandWidth, addressSizeFlip))
                        continue;
                    suffixMatch = true;
                    operandWidth = 0;
                    addressSizeFlip = false;
                }

                Encoding enc;
                enc.m_table = tableIndex;
                enc.m_opcode = opcode;
                enc.m_operandWidth = 0;
                enc.m_addressSizePrefix = addressSizeFlip ||
                    ((addressWidth != 0) && (addressWidth != getDefaultAddressWidth()));
                enc.m_hasModrm = false;
                enc.m_regFieldSet = false;
                enc.m_modrm.m_packed = 0;
                enc.m_hasSib = false;
                enc.m_sib.m_packed = 0;
                enc.m_displacementSize = 0;
                enc.m_segment = segment;
                enc.m_prefixes = prefixes;
                enc.m_rex.m_packed = ia32dis::AMD64_REX_PREFIX;
                enc.m_needsRex = false;
                enc.m_hasHighByteRegister = false;
                enc.m_isRipRelative = false;
                enc.m_length = 0;

                // The operand size which the spelling of the mnemonic and the
                // prefix o16 / o32 / o64 select
                if ((operandWidth != 0) && !setOperandWidth(operandWidth, enc))
                    continue;
                if (suffixMatch && !setOperandWidth(suffixWidth, enc))
                    continue;
                if ((prefixes.m_operandWidth != 0) &&
                    !setOperandWidth(prefixes.m_operandWidth, enc))
                    continue;

                // A fixed ModRM byte (x87 'fchs' is D9 E0) is part of the opcode
                if (opcode->m_modrm & ia32dis::MODRM_EXACT_FLAG)
                {
                    enc.m_hasModrm = true;
                    enc.m_regFieldSet = true;
                    enc.m_modrm.m_packed =
                        (uint8)((opcode->m_modrm >> 16) & 0xFF);
                }

                if (!matchOpcode(opcode, first, second, third, pass, enc))
                    continue;

                // The modrm filter of the entry may allow memory references
                // only (fnstsw m16 is DD /7, fnstsw ax is DF E0)
                if (enc.m_hasModrm && (enc.m_modrm.m_bits.m_mod == 3) &&
                    ((opcode->m_modrm & ia32dis::MODRM_EXACT_FLAG) == 0) &&
                    ((opcode->m_modrm & ia32dis::MODRM_MOD_REGISTERS) == 0))
                    continue;
                // and the other way round (movhlps has registers only)
                if (enc.m_hasModrm && (enc.m_modrm.m_bits.m_mod != 3) &&
                    ((opcode->m_modrm & ia32dis::MODRM_EXACT_FLAG) == 0) &&
                    ((opcode->m_modrm & ia32dis::MODRM_MOD_MEMORY) == 0))
                    continue;

                if (!finishRex(enc))
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

                // The entries which exist in 64 bit mode only (movd xmm, r/m32/64)
                // lose the ties to the entries of the IA-32 set
                // (movq xmm, m64), as in nasm
                bool isBetterEntry = found &&
                    (enc.m_length == best.m_length) &&
                    (immediateBytes == bestImmediateBytes) &&
                    (opcode->m_mode64 != ia32dis::MODE64_ONLY) &&
                    (best.m_opcode->m_mode64 == ia32dis::MODE64_ONLY);

                if (!found || (enc.m_length < best.m_length) || preferDc ||
                    isBetterEntry ||
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

bool IA32AssemblerStream::isReplacedByAmd64Entry(const ia32dis::OpcodeEntry* opcode,
                                                 uint source) const
{
    // An entry of the AMD64 table replaces the entry of the IA-32 table which
    // has the same opcode, mandatory prefix and a common modrm byte (63 is
    // arpl in the IA-32 table and movsxd here, B8 is mov r32, imm32 and
    // mov r64, imm64, 0F 20 is mov r32, cr0 and mov r64, cr0). The decoder
    // searches the AMD64 table first, so the first of them to match wins
    const ia32dis::OpcodeEntry* amd64 = (source == 0) ?
        ia32dis::gAmd64OneByteOpcodeTable : ia32dis::gAmd64TwoBytesOpcodeTable;
    for (; amd64->m_opcodeName != ia32dis::OPCODEEOT; amd64++)
    {
        // The copies of the IA-32 entries (movq xmm, xmm/m64) keep the order
        // of the IA-32 table
        if (amd64->m_mode64 == ia32dis::MODE64_LEGACY)
            continue;
        if ((amd64->m_prefix != opcode->m_prefix) ||
            (amd64->m_prefixMask != opcode->m_prefixMask) ||
            (amd64->m_mandatoryPrefix != opcode->m_mandatoryPrefix))
            continue;
        if (amd64->m_modrm == opcode->m_modrm)
            return true;
        // A common mod (memory / registers) and a common /digit
        if (((amd64->m_modrm | opcode->m_modrm) & ia32dis::MODRM_EXACT_FLAG) == 0)
        {
            uint common = amd64->m_modrm & opcode->m_modrm & 0xFFFF;
            if (((common & ia32dis::MODRM_MOD_MEM11) != 0) &&
                ((common & 0xFFFC) != 0))
                return true;
        }
    }
    return false;
}

bool IA32AssemblerStream::matchMnemonic(const char* tableName, const cString& name,
                                        uint& operandWidth,
                                        bool& addressSizeFlip) const
{
    operandWidth = 0;
    addressSizeFlip = false;

    // The 16 bit, 32 bit and 64 bit spelling of the table entry
    const cString table(tableName);
    cString name16;
    cString name32;
    cString name64;
    bool isAddressSize = false;
    bool isDefaultSized = false;
    int slash = indexOf(table, '/');
    int hash = indexOf(table, '#');
    if (slash >= 0)
    {
        name16 = table.part(0, slash);
        name32 = table.part(slash + 1, table.length());
        // The optional third name
        int secondSlash = indexOf(name32, '/');
        if (secondSlash >= 0)
        {
            name64 = name32.part(secondSlash + 1, name32.length());
            name32 = name32.part(0, secondSlash);
        }
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
            name64 = base + "q" + rest;
        } else if (kind == "d")
        {
            // '#d' is 'd' in 32 bit and nothing in 16 bit (pusha/pushad)
            name16 = base + rest;
            name32 = base + "d" + rest;
            name64 = base + "q" + rest;
            isDefaultSized = true;
        } else
        {
            // '#e' is 'e' in 32 bit and nothing in 16 bit (jcxz/jecxz)
            ASM_CHECK(kind == "e", "invalid opcode name in table");
            isAddressSize = true;
            name16 = base + rest;
            name32 = base + "e" + rest;
            name64 = base + "r" + rest;
        }
    } else
    {
        name16 = table;
        name32 = table;
        name64 = table;
    }
    if (name64.length() == 0)
        name64 = name32;

    if (is64BitMode())
    {
        if (isAddressSize)
        {
            // jrcxz is the 64 bit address size, jecxz the 32 bit one. There is
            // no 16 bit address size
            if (name == name64)
                return true;
            if (name == name32)
            {
                addressSizeFlip = true;
                return true;
            }
            return false;
        }
        if (isDefaultSized)
        {
            // pushf is the default operand size (64 bit), pushfd and pushfq
            // are the explicit sizes
            if (name == name16)
                return true;
            if (name == name32)
                operandWidth = 32;
            else if (name == name64)
                operandWidth = 64;
            else
                return false;
            return true;
        }
        if ((slash < 0) && (hash < 0))
            return (name == table);

        // Each spelling selects the operand size (cbw, cwde, cdqe). Where the
        // spellings are the same (movd) the 32 bit operand size is the default
        if (name == name32)
            operandWidth = 32;
        else if (name == name64)
            operandWidth = 64;
        else if (name == name16)
            operandWidth = 16;
        else
            return false;
        return true;
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
            operandWidth = (getDefaultOperandWidth() == 16) ? 32 : 16;
        return true;
    }
    return false;
}

bool IA32AssemblerStream::matchOpcode(const ia32dis::OpcodeEntry* opcode,
                                      const Operand& first,
                                      const Operand& second,
                                      const Operand& third,
                                      uint relaxed,
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
            (types[i] != ia32dis::OPND_MEMREF_OFFSET_DS) &&
            // The xmm and mmx operands have their own size
            (types[i] != ia32dis::OPND_MMX_RM) &&
            (types[i] != ia32dis::OPND_SIMD_RM) &&
            (types[i] != ia32dis::OPND_SIMD_RM_QWORD) &&
            (types[i] != ia32dis::OPND_SIMD_RM_DWORD) &&
            (types[i] != ia32dis::OPND_MODRM_R32_WORDPTR))
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

    if (is64BitMode())
    {
        // In 64 bit mode nasm writes 'xchg eax, eax' as 87 C0, because 90 is
        // the nop. 'xchg ax, ax', 'xchg rax, rax' and 'xchg r8d, eax' are 90
        if ((opcode->m_prefix == 0x90) && (opcode->m_prefixMask == 0xF8) &&
            (enc.m_table == 0) && (getEncodingWidth(enc) == 32))
        {
            for (uint i = 0; i < 3; i++)
                if ((types[i] == ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32) &&
                    (operands[i]->getReference() == 0))
                    return false;
        }

        // nasm writes 'mov r64, imm' with an unsigned 32 bit value as
        // 'mov r32, imm32' (the upper half of the register is cleared), and
        // uses 'mov r64, imm64' for the other values which do not fit in the
        // sign extended 'mov r/m64, imm32'
        if ((types[0] == ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32) &&
            (types[1] == ia32dis::OPND_IMMEDIATE_DQ) &&
            (enc.m_operandWidth == 64) &&
            (second.getValue() >= 0) && (second.getValue() <= 0xFFFFFFFFLL))
            enc.m_operandWidth = 32;
    }

    return true;
}

bool IA32AssemblerStream::isWidthAllowed(uint width, const Encoding& enc) const
{
    if ((width != 16) && (width != 32) && (width != 64))
        return false;
    if (!is64BitMode())
        return (width != 64);

    // The x87 memory operands have a fixed size in 64 bit mode, REX.W is
    // ignored
    uint mode = enc.m_opcode->m_mode64;
    if (enc.m_table == 2)
        mode = ia32dis::MODE64_NO_REXW;
    switch (mode)
    {
    case ia32dis::MODE64_FORCE_64:
        // The near branches: the operand size is not selectable
        return false;
    case ia32dis::MODE64_DEFAULT_64:
        // 64 bits, or 16 bits with the 66 prefix
        return (width != 32);
    case ia32dis::MODE64_NO_REXW:
        return (width != 64);
    default:
        return true;
    }
}

bool IA32AssemblerStream::setOperandWidth(uint width, Encoding& enc) const
{
    if (!isWidthAllowed(width, enc))
        return false;
    if ((enc.m_operandWidth != 0) && (enc.m_operandWidth != width))
        return false;
    enc.m_operandWidth = width;
    return true;
}

void IA32AssemblerStream::useRegister(Operand::RegisterClass regClass,
                                      uint index,
                                      bool isReg,
                                      Encoding& enc)
{
    // The REX extension bit of the register
    if (index >= 8)
    {
        if (isReg)
            enc.m_rex.m_bits.r = 1;
        else
            enc.m_rex.m_bits.b = 1;
        enc.m_needsRex = true;
    }
    // spl, bpl, sil and dil exist only with a REX prefix
    if (regClass == Operand::REG_8BIT_REX)
        enc.m_needsRex = true;
    // ah, ch, dh and bh don't exist with a REX prefix
    if ((regClass == Operand::REG_8BIT) && (index >= 4) && (index < 8))
        enc.m_hasHighByteRegister = true;
}

void IA32AssemblerStream::setRegField(const Operand& reg, Encoding& enc)
{
    enc.m_hasModrm = true;
    enc.m_modrm.m_bits.m_regOpcode = reg.getReference() & 7;
    enc.m_regFieldSet = true;
    useRegister(reg.getRegisterClass(), reg.getReference(), true, enc);
}

void IA32AssemblerStream::setRmRegister(const Operand& reg, Encoding& enc)
{
    enc.m_hasModrm = true;
    enc.m_modrm.m_bits.m_mod = 3;
    enc.m_modrm.m_bits.m_rm = reg.getReference() & 7;
    useRegister(reg.getRegisterClass(), reg.getReference(), false, enc);
}

bool IA32AssemblerStream::finishRex(Encoding& enc) const
{
    if (!is64BitMode())
        return true;

    // REX.W is the 64 bit operand size, unless 64 bits is what the entry does
    // anyway (push, call, ...)
    if ((enc.m_operandWidth == 64) &&
        (enc.m_opcode->m_mode64 != ia32dis::MODE64_DEFAULT_64) &&
        (enc.m_opcode->m_mode64 != ia32dis::MODE64_FORCE_64))
    {
        enc.m_rex.m_bits.w = 1;
        enc.m_needsRex = true;
    }

    // ah..bh can't be used with a REX prefix
    return !(enc.m_needsRex && enc.m_hasHighByteRegister);
}

bool IA32AssemblerStream::matchOperand(ia32dis::OperandType ia32disType,
                                       const Operand& compilerOperand,
                                       uint relaxed,
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
        // The size keyword of a segment register ('push word fs') sets the
        // operand size of the instruction
        if (compilerOperand.getSize() != 0)
        {
            if ((regClass != Operand::REG_SEGMENT) ||
                !setOperandWidth(compilerOperand.getSize(), enc))
                return false;
        }
        // The registers of 64 bit mode: r8-r15, rax.., spl.., xmm8-xmm15, cr8
        if (!is64BitMode() && ((regClass == Operand::REG_64BIT) ||
                               (regClass == Operand::REG_8BIT_REX) ||
                               (index >= 8)))
            return false;
        bool is8 = (regClass == Operand::REG_8BIT);
        // A byte register of any kind
        bool isByte = is8 || (regClass == Operand::REG_8BIT_REX);
        bool is16 = (regClass == Operand::REG_16BIT);
        bool is32 = (regClass == Operand::REG_32BIT);
        bool is64 = (regClass == Operand::REG_64BIT);
        bool isMmx = (regClass == Operand::REG_MMX);
        bool isXmm = (regClass == Operand::REG_XMM);
        bool isFpu = (regClass == Operand::REG_FPU);
        // The width of a general purpose register: 16, 32 or 64 bits
        uint registerWidth = is16 ? 16 : (is32 ? 32 : (is64 ? 64 : 0));

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
            // The register index of the fixed register, same for 16, 32 and
            // 64 bit
            uint wanted = ia32dis::IA32_GP16_AX;
            if (ia32disType == ia32dis::OPND_eBX) wanted = ia32dis::IA32_GP16_BX;
            if (ia32disType == ia32dis::OPND_eBP) wanted = ia32dis::IA32_GP16_BP;
            if (ia32disType == ia32dis::OPND_eSI) wanted = ia32dis::IA32_GP16_SI;
            if (ia32disType == ia32dis::OPND_eDI) wanted = ia32dis::IA32_GP16_DI;
            if ((registerWidth == 0) || (index != wanted))
                return false;
            return setOperandWidth(registerWidth, enc);
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
            if (!isByte)
                return false;
            useRegister(regClass, index, false, enc);
            return true;
        case ia32dis::OPND_ONEBYTES_OPCODE_GP_16_32:
            if (registerWidth == 0)
                return false;
            if (!setOperandWidth(registerWidth, enc))
                return false;
            useRegister(regClass, index, false, enc);
            return true;

        // The register is in the reg field of the modrm
        case ia32dis::OPND_GP_8BIT_MODRM:
            if (!isByte) return false;
            setRegField(compilerOperand, enc);
            return true;
        case ia32dis::OPND_GP_16BIT_MODRM:
            if (!is16) return false;
            setRegField(compilerOperand, enc);
            return true;
        case ia32dis::OPND_GP_16_32BIT:
            if (registerWidth == 0)
                return false;
            if (!setOperandWidth(registerWidth, enc))
                return false;
            setRegField(compilerOperand, enc);
            return true;
        case ia32dis::OPND_GP_SEGMENT_MODRM:
            if (regClass != Operand::REG_SEGMENT) return false;
            setRegField(compilerOperand, enc);
            return true;
        case ia32dis::OPND_CTRL_MODRM:
            if (regClass != Operand::REG_CONTROL) return false;
            setRegField(compilerOperand, enc);
            return true;
        case ia32dis::OPND_DBG_MODRM:
            if (regClass != Operand::REG_DEBUG) return false;
            setRegField(compilerOperand, enc);
            return true;
        case ia32dis::OPND_MMX_MODRM:
            if (!isMmx) return false;
            setRegField(compilerOperand, enc);
            return true;
        case ia32dis::OPND_GP_32BIT_MODRM:
            // The register is 32 bit whatever the operand size is, and 64 bit
            // with REX.W
            // (pextrw takes rax as eax, nasm doesn't write REX.W for it)
            if (is64 && (enc.m_opcode->m_prefix != 0xC5))
            {
                if (!setOperandWidth(64, enc))
                    return false;
            } else if (!is32 && !is64)
                return false;
            setRegField(compilerOperand, enc);
            return true;
        case ia32dis::OPND_SIMD_MODRM:
            if (!isXmm) return false;
            setRegField(compilerOperand, enc);
            return true;

        // x87 ST(i) in the modrm r/m field (mod 11)
        case ia32dis::OPND_FPU_ST0:
            return isFpu && (index == 0);
        case ia32dis::OPND_FPU_ST_RM:
            if (!isFpu) return false;
            setRmRegister(compilerOperand, enc);
            return true;

        // MMX and SSE registers in the modrm r/m field (mod 11)
        case ia32dis::OPND_MMX_RM:
            if (!isMmx) return false;
            setRmRegister(compilerOperand, enc);
            return true;
        case ia32dis::OPND_SIMD_RM:
        case ia32dis::OPND_SIMD_RM_QWORD:
        case ia32dis::OPND_SIMD_RM_DWORD:
            if (!isXmm) return false;
            setRmRegister(compilerOperand, enc);
            return true;

        // The register is the modrm r/m (mod 3)
        case ia32dis::OPND_MODRM_R32_WORDPTR:
            // pinsrw takes ax and eax alike (and rax in 64 bit mode, which
            // nasm encodes as eax), and no operand size prefix
            if (!(is16 || is32 || is64)) return false;
            setRmRegister(compilerOperand, enc);
            return true;
        case ia32dis::OPND_MODRM_BYTEPTR:
            if (!isByte) return false;
            setRmRegister(compilerOperand, enc);
            return true;
        case ia32dis::OPND_MODRM_WORDPTR:
            if (enc.m_opcode->m_secondOperand == ia32dis::OPND_GP_SEGMENT_MODRM)
            {
                // mov ax, ds is 66 8C D8, mov eax, ds the whole register (and
                // nasm writes no REX.W for rax)
                if (!(is16 || is32 || is64) ||
                    !setOperandWidth(is16 ? 16 : 32, enc))
                    return false;
            } else if (enc.m_opcode->m_firstOperand == ia32dis::OPND_GP_SEGMENT_MODRM)
            {
                // mov ds, ax and mov ds, eax are 8E D8, without a prefix
                if (!(is16 || is32 || is64))
                    return false;
            } else if (!is16)
                return false;
            setRmRegister(compilerOperand, enc);
            return true;
        case ia32dis::OPND_MODRM_dWORDPTR:
            if (registerWidth == 0)
                return false;
            if (!setOperandWidth(registerWidth, enc))
                return false;
            setRmRegister(compilerOperand, enc);
            return true;

        // The 64 bit mode operands
        case ia32dis::OPND_MODRM_FIXED_DWORDPTR:
            // A dword register whatever the operand size is
            if (!is32) return false;
            setRmRegister(compilerOperand, enc);
            return true;
        case ia32dis::OPND_GP_NATIVE_RM:
            // The 64 bit register (mov cr0, rax)
            if (!is64) return false;
            setRmRegister(compilerOperand, enc);
            return true;

        default:
            return false;
        }
    }

    case Operand::IMMEDIATE:
    {
        // A size keyword sets the operand size: 'push word 0x12' is 66 6a 12
        uint size = compilerOperand.getSize();
        if ((size == 16) || (size == 32) || (size == 64))
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
                                         uint relaxed,
                                         const Encoding& enc) const
{
    int64 value = compilerOperand.getValue();
    switch (ia32disType)
    {
    case ia32dis::OPND_IMMEDIATE_8BIT:
        // Signed 8 bit, or the unsigned byte value in the first relaxed pass
        // In the second relaxed pass the value is truncated to the low byte,
        // as nasm does (psrld mm1, 0x12345678 is psrld mm1, 0x78)
        if (relaxed >= 2)
            return (value >= -0x80000000LL) && (value <= 0xFFFFFFFFLL);
        if (relaxed == 1)
            return (value >= -0x80) && (value <= 0xFF);
        return (value >= -0x80) && (value <= 0x7F);

    case ia32dis::OPND_IMMEDIATE_16BIT:
        return (value >= -0x8000) && (value <= 0xFFFF);

    case ia32dis::OPND_IMMEDIATE_DS:
        if (getEncodingWidth(enc) == 16)
            return (value >= -0x8000) && (value <= 0xFFFF);
        // The immediate of a 64 bit operand is a sign extended dword. nasm
        // takes the low dword of a bigger value, if the instruction has no
        // 64 bit immediate form (test rax, 0x1122334455667788)
        if (getEncodingWidth(enc) == 64)
            return (relaxed >= 1) || ((value >= -0x80000000LL) && (value <= 0x7FFFFFFFLL));
        return (value >= -0x80000000LL) && (value <= 0xFFFFFFFFLL);

    case ia32dis::OPND_IMMEDIATE_DQ:
        // mov r64, imm64: any value
        if (getEncodingWidth(enc) == 16)
            return (value >= -0x8000) && (value <= 0xFFFF);
        if (getEncodingWidth(enc) == 32)
            return (value >= -0x80000000LL) && (value <= 0xFFFFFFFFLL);
        return true;

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

    const bool mode64 = is64BitMode();
    uint defaultAddress = getDefaultAddressWidth();
    // The address size comes from the registers of the address or from the
    // a16 / a32 / a64 keyword, the default otherwise. 64 bit mode has no 16
    // bit addresses, the other modes no 64 bit addresses
    uint addressWidth = compilerOperand.getAddressWidth();
    const bool isExplicitWidth = (addressWidth != 0);
    if (addressWidth == 0)
        addressWidth = defaultAddress;
    if (mode64 ? (addressWidth == 16) : (addressWidth == 64))
        return false;
    // The rip relative addressing exists in 64 bit mode only
    if (!mode64 && compilerOperand.isRipRelative())
        return false;
    bool isDirect = (compilerOperand.getBase() < 0) &&
                    (compilerOperand.getIndex() < 0) &&
                    !compilerOperand.isRipRelative();

    // Direct address with the accumulator (mov eax, [0x1234] is A1 id)
    if (ia32disType == ia32dis::OPND_MEMREF_OFFSET_DS)
    {
        if (!isDirect)
            return false;
        // In 64 bit mode nasm uses the offset form (64 bit offset) for an
        // explicit address size only ([qword 0x1122334455667788] and
        // [a32 0x12345678]). The other absolute addresses are a SIB with
        // no base and no index
        if (mode64 && !isExplicitWidth)
            return false;
        if (addressWidth != defaultAddress)
            enc.m_addressSizePrefix = true;
        enc.m_displacementSize = addressWidth / 8;
        return true;
    }
    // The 64 bit offset is an offset form only
    if (mode64 && isDirect && isExplicitWidth && (addressWidth == 64))
        return false;

    switch (ia32disType)
    {
    case ia32dis::OPND_MODRM_MEM:
        // The 16 bit operand size of the descriptor tables (lgdt word [mem])
        if (mode64 && (enc.m_opcode->m_mode64 == ia32dis::MODE64_NO_REXW) &&
            (size == 16) && !setOperandWidth(16, enc))
            return false;
        break;
    case ia32dis::OPND_MODRM_FAR_OFFSET:
        // 64 bit mode: m16:32 for 'dword', m16:64 (REX.W) for 'qword' and
        // by default, m16:16 for 'word'
        if (mode64 && !setOperandWidth((size == 0) ? 64 : size, enc))
            return false;
        break;
    case ia32dis::OPND_MODRM_BYTEPTR:
        if ((size != 0) && (size != 8))
            return false;
        break;
    case ia32dis::OPND_MODRM_WORDPTR:
    case ia32dis::OPND_MODRM_R32_WORDPTR:
        if ((size != 0) && (size != 16))
            return false;
        break;
    case ia32dis::OPND_MODRM_QWORDPTR:
    case ia32dis::OPND_MMX_RM:
        if ((size != 0) && (size != 64))
            return false;
        break;
    // nasm takes any of the xmm sizes for most of the scalar instructions
    // (addsd xmm0, oword [eax])
    case ia32dis::OPND_SIMD_RM_QWORD:
        if ((size != 0) && (size != 64) && (size != 128))
            return false;
        break;
    case ia32dis::OPND_SIMD_RM_DWORD:
        if ((size != 0) && (size != 32) && (size != 128))
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
        if ((size == 8) || (size == 80) || (size == 128))
            return false;
        // The x87 memory size selects the opcode (fild word is df /0), it
        // doesn't change the operand size
        if (enc.m_table == 2)
        {
            if ((size == 16) || (size == 64))
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
    case ia32dis::OPND_MODRM_FIXED_DWORDPTR:
        if ((size != 0) && (size != 32))
            return false;
        break;
    case ia32dis::OPND_MODRM_DOUBLE_MEM:
        // cmpxchg8b m64, cmpxchg16b m128 (the operand size is 64 bit)
        if ((size != 0) && (size != ((enc.m_operandWidth == 64) ? 128 : 64)))
            return false;
        break;
    default:
        return false;
    }

    if (addressWidth != defaultAddress)
        enc.m_addressSizePrefix = true;

    if (mode64)
        encodeMemory64(compilerOperand, enc);
    else if (addressWidth == 16)
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

void IA32AssemblerStream::encodeMemory64(const Operand& compilerOperand,
                                         Encoding& enc) const
{
    int base = compilerOperand.getBase();
    int index = compilerOperand.getIndex();
    uint scale = compilerOperand.getScale();
    int64 displacement = compilerOperand.getDisplacement();

    enc.m_hasSib = false;

    // [rip + disp32]: mod 00, rm 101. The displacement is computed once the
    // length of the instruction is known
    if (compilerOperand.isRipRelative())
    {
        enc.m_modrm.m_bits.m_mod = 0;
        enc.m_modrm.m_bits.m_rm = 5;
        enc.m_displacementSize = 4;
        enc.m_isRipRelative = true;
        return;
    }

    // The displacement is a 32 bit value, sign extended. A bigger value is
    // truncated, as nasm does (with a warning)

    // The extension bits of the SIB index and base
    if (index >= 8)
    {
        enc.m_rex.m_bits.x = 1;
        enc.m_needsRex = true;
    }
    if (base >= 8)
    {
        enc.m_rex.m_bits.b = 1;
        enc.m_needsRex = true;
    }

    // [disp32]: SIB with no base (base 101) and no index (index 100), as
    // the mod 00 rm 101 form is rip relative
    if ((base < 0) && (index < 0))
    {
        enc.m_modrm.m_bits.m_mod = 0;
        enc.m_modrm.m_bits.m_rm = 4;
        enc.m_hasSib = true;
        enc.m_sib.m_bits.m_scale = 0;
        enc.m_sib.m_bits.m_index = 4;
        enc.m_sib.m_bits.m_base = 5;
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
        enc.m_sib.m_bits.m_index = index & 7;
        enc.m_sib.m_bits.m_base = 5;
        enc.m_displacementSize = 4;
        return;
    }

    // Displacement: none for 0 (except rbp and r13 as base, which need disp8
    // 0), disp8 for -128..127, disp32 otherwise
    const uint baseLow = base & 7;
    uint mod;
    uint displacementSize;
    if ((displacement == 0) && (baseLow != ia32dis::IA32_GP32_EBP))
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

    // rsp and r12 as base and any index need the SIB byte
    bool needsSib = (index >= 0) || (baseLow == ia32dis::IA32_GP32_ESP);
    enc.m_modrm.m_bits.m_mod = mod;
    enc.m_displacementSize = displacementSize;
    if (needsSib)
    {
        enc.m_modrm.m_bits.m_rm = 4;
        enc.m_hasSib = true;
        enc.m_sib.m_bits.m_scale = (index >= 0) ? scaleBits(scale) : 0;
        // The index 100 is no index (r12 is 100 with REX.X)
        enc.m_sib.m_bits.m_index = (index >= 0) ? (index & 7) : ia32dis::IA32_GP32_ESP;
        enc.m_sib.m_bits.m_base = baseLow;
    } else
        enc.m_modrm.m_bits.m_rm = baseLow;
}

bool IA32AssemblerStream::is64BitMode() const
{
    return (m_processorMode == IA32eInstructionSet::AMD_64);
}

uint IA32AssemblerStream::calculateLength(const Encoding& enc) const
{
    // The table entry gives the sizes of the immediates
    ia32dis::OperandType types[3] = { enc.m_opcode->m_firstOperand,
                                      enc.m_opcode->m_secondOperand,
                                      enc.m_opcode->m_thridOperand };

    uint length = 0;
    // Prefixes: segment, address size, lock, operand size, repeat, mandatory
    // and the REX prefix of 64 bit mode
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
    if (enc.m_needsRex)
        length++;

    // Opcode, modrm, sib and displacement
    if ((enc.m_table == 1) || (enc.m_table == 3) || (enc.m_table == 4))
        length++;
    if ((enc.m_table == 3) || (enc.m_table == 4))
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
        // The rip relative displacement is 32 bits
        if ((operands[i]->getKind() == Operand::MEMORY) && enc.m_isRipRelative)
        {
            int64 displacement = getRipDisplacement(*operands[i], enc);
            if ((displacement < -0x80000000LL) || (displacement > 0x7FFFFFFFLL))
                return false;
        }

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
            // Near offsets: 16 or 32 bit according to the operand size, 32 bit
            // in 64 bit mode
            if (!is64BitMode() && (getEncodingWidth(enc) == 16))
            {
                if ((relative < -0x8000) || (relative > 0x7FFF))
                    return false;
            } else if ((relative < -0x80000000LL) || (relative > 0x7FFFFFFFLL))
                return false;
        }
    }
    return true;
}

int64 IA32AssemblerStream::getRipDisplacement(const Operand& memory,
                                              const Encoding& enc) const
{
    // [rel $+N] is N bytes from the start of the instruction, [rip + N] is
    // N bytes from the end of the instruction
    if (memory.isRelativeToInstruction())
        return memory.getDisplacement() - (int64)enc.m_length;
    return memory.getDisplacement();
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
        // The immediate of a 64 bit operand is 32 bits, sign extended
        return (getEncodingWidth(enc) == 16) ? 2 : 4;
    case ia32dis::OPND_IMMEDIATE_DQ:
        return getEncodingWidth(enc) / 8;
    case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
        // The relative offset is 32 bits in 64 bit mode
        if (is64BitMode())
            return 4;
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
    // repeat, the mandatory prefix (66, F2, F3) and the REX prefix, which is
    // right before the opcode (before 0F)
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
    if (enc.m_needsRex)
        m_binary.appendUint8(enc.m_rex.m_packed);

    // The table index prefix. Used only for two bytes table (the x87 table
    // has its escape as the opcode prefix)
    if ((enc.m_table == 1) || (enc.m_table == 3) || (enc.m_table == 4))
        m_binary.appendUint8(ia32dis::gIa32TwoByteEscapeCharacter);
    if (enc.m_table == 3)
        m_binary.appendUint8(ia32dis::gIa32ThreeBytes38EscapeCharacter);
    if (enc.m_table == 4)
        m_binary.appendUint8(ia32dis::gIa32ThreeBytes3AEscapeCharacter);

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
                int64 displacement = operands[i]->getDisplacement();
                if (enc.m_isRipRelative)
                    displacement = getRipDisplacement(*operands[i], enc);
                writeValue(displacement, enc.m_displacementSize);
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
    uint size = getImmediateSize(ia32disType, enc);
    int64 value = compilerOperand.getValue();
    switch (ia32disType)
    {
    case ia32dis::OPND_IMMEDIATE_8BIT:
    case ia32dis::OPND_IMMEDIATE_16BIT:
    case ia32dis::OPND_IMMEDIATE_DS:
    case ia32dis::OPND_IMMEDIATE_DQ:
        writeValue(value, size);
        break;
    case ia32dis::OPND_IMMEDIATE_OFFSET_SHORT_8:
    case ia32dis::OPND_IMMEDIATE_OFFSET_LONG_32:
    case ia32dis::OPND_IMMEDIATE_OFFSET_DS:
        writeValue(value - (int64)enc.m_length, size);
        break;
    case ia32dis::OPND_IMMEDIATE_OFFSET_FAR:
        writeValue(value, size - 2);
        writeValue(compilerOperand.getSegmentValue(), 2);
        break;
    default:
        // Registers, memory displacement and the implicit operands
        break;
    }
}

void IA32AssemblerStream::writeValue(int64 value, uint bytes)
{
    uint8 buffer[sizeof(uint64)];
    for (uint i = 0; i < bytes; i++)
        buffer[i] = (uint8)(value >> (8 * i));
    m_binary.appendBuffer(buffer, bytes);
}

bool IA32AssemblerStream::needsOperandSizePrefix(const Encoding& enc) const
{
    // A mandatory 66 prefix is the operand size prefix too (movd xmm, eax)
    if (enc.m_opcode->m_mandatoryPrefix == ia32dis::MANDATORY_66)
        return false;
    // 64 bit mode: 66 is 16 bits, 32 or 64 bits are the default of the entry
    // or REX.W
    if (is64BitMode())
        return getEncodingWidth(enc) == 16;
    return getEncodingWidth(enc) != getDefaultOperandWidth();
}

uint IA32AssemblerStream::getDefaultOperandWidth() const
{
    return (m_processorMode == IA32eInstructionSet::INTEL_16) ? 16 : 32;
}

uint IA32AssemblerStream::getDefaultAddressWidth() const
{
    switch (m_processorMode)
    {
    case IA32eInstructionSet::INTEL_16:
        return 16;
    case IA32eInstructionSet::AMD_64:
        return 64;
    default:
        return 32;
    }
}

uint IA32AssemblerStream::getEncodingWidth(const Encoding& enc) const
{
    if (enc.m_operandWidth != 0)
        return enc.m_operandWidth;
    // The entries which default to the 64 bit operand size in 64 bit mode
    if (is64BitMode() &&
        ((enc.m_opcode->m_mode64 == ia32dis::MODE64_DEFAULT_64) ||
         (enc.m_opcode->m_mode64 == ia32dis::MODE64_FORCE_64)))
        return 64;
    return getDefaultOperandWidth();
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
    m_addressWidth(0),
    m_ripRelative(false),
    m_relativeToInstruction(false)
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
        // 'push qword fs': the size of a segment register is the operand size
        ASM_CHECK((m_size == 0) || (m_regClass == REG_SEGMENT),
                  "size keyword can't be used with a register");
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

    // The nasm keywords in front of the address: 'rel' (rip relative), 'abs'
    // (absolute, the default), the address size a16 / a32 / a64 and 'qword'
    // (a 64 bit offset)
    bool isRelative = false;
    for (;;)
    {
        cString word = peekWord(parser);
        if (word == "rel")
            isRelative = true;
        else if (word == "a16")
            m_addressWidth = 16;
        else if (word == "a32")
            m_addressWidth = 32;
        else if ((word == "a64") || (word == "qword"))
            m_addressWidth = 64;
        else
        {
            // Segment override inside the brackets: '[fs:eax]'. A segment
            // register which is not followed by ':' is rejected as an address
            // register below.
            int segment = segmentIndex(word);
            if (segment < 0)
            {
                if (word != "abs")
                    break;
                skipWord(parser);
                continue;
            }
            parser.readCString(false);
            ASM_CHECK(isNextChar(parser, ':'), "register was expected in address");
            parser.readChar();
            ASM_CHECK(m_segment < 0, "segment prefix used twice");
            m_segment = segment;
            parser.readBlanksUntilEOL();
            continue;
        }
        skipWord(parser);
    }

    // Terms of the form [+-]number, [+-]reg, [+-]number*reg, [+-]reg*number,
    // and '$' / 'rip' in a rip relative address
    bool hasRip = false;
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
        if (isNextChar(parser, '$'))
        {
            // [rel $+N]: N bytes from the start of the instruction
            ASM_CHECK(isRelative, "'$' can be used in a rip relative address only");
            ASM_CHECK(sign > 0, "'$' can't be subtracted");
            parser.readChar();
            m_hasDisplacement = true;
        } else if (isDecimalAhead(parser))
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
                          ((regClass == REG_32BIT) || (regClass == REG_16BIT) ||
                           (regClass == REG_64BIT)),
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
            if (name == "rip")
            {
                ASM_CHECK(!hasRip && (sign > 0), "invalid use of rip in address");
                hasRip = true;
                parser.readBlanksUntilEOL();
                ASM_CHECK(!isNextChar(parser, '*'), "rip can't be scaled");
            } else
            {
                RegisterClass regClass;
                uint index;
                ASM_CHECK(checkRegister(name, regClass, index) &&
                          ((regClass == REG_32BIT) || (regClass == REG_16BIT) ||
                           (regClass == REG_64BIT)),
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
            }
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

    // Rip relative addressing, [rip + disp] or [rel $+N]
    if (hasRip || isRelative)
    {
        ASM_CHECK((m_base < 0) && (m_index < 0),
                  "rip can't be combined with other registers");
        m_ripRelative = true;
        m_relativeToInstruction = !hasRip;
    }

    // Normalize the address: nasm encodes [eax*1] as [eax], and [eax*2] as
    // [eax+eax*1]. Without a base the index must be encodable as a base.
    if ((m_addressWidth == 32) || (m_addressWidth == 64))
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
        // esp can't be an index: [eax+esp] is [esp+eax*1]
        if ((m_index == (int)ia32dis::IA32_GP32_ESP) && (m_base >= 0) &&
            (m_scale == 1))
        {
            m_index = m_base;
            m_base = (int)ia32dis::IA32_GP32_ESP;
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
    ASM_CHECK((m_base >= 0) || (m_index >= 0) || m_hasDisplacement ||
              m_ripRelative, "empty address");
}

void IA32AssemblerStream::Operand::addAddressRegister(RegisterClass regClass,
                                                      uint index,
                                                      uint scale,
                                                      int sign)
{
    ASM_CHECK(sign > 0, "a register can't be subtracted");

    // The address size is defined by the first register
    uint addressWidth = (regClass == REG_16BIT) ? 16 :
                        ((regClass == REG_64BIT) ? 64 : 32);
    if (m_addressWidth == 0)
        m_addressWidth = addressWidth;
    ASM_CHECK(m_addressWidth == addressWidth,
              "16, 32 and 64 bit registers can't be mixed in an address");

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

bool IA32AssemblerStream::Operand::isRipRelative() const
{
    return m_ripRelative;
}

bool IA32AssemblerStream::Operand::isRelativeToInstruction() const
{
    return m_relativeToInstruction;
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

    // The registers of 64 bit mode (the first eight 32, 16 and 8 bit names
    // are the IA-32 names, which were checked above)
    for (uint i = 0; i < ia32dis::AMD64_NUMBER_OF_REGISTERS; i++)
    {
        if (regname == ia32dis::gAmd64Registers[i].m_name)
        {
            regClass = REG_64BIT;
            regIndex = i;
            return true;
        }
        if (i < ia32dis::NUMBER_OF_REGISTERS)
        {
            // spl, bpl, sil and dil
            if ((i >= ia32dis::IA32_GP8_AH) &&
                (regname == ia32dis::gAmd64Registers8[i].m_name))
            {
                regClass = REG_8BIT_REX;
                regIndex = i;
                return true;
            }
            continue;
        }
        if (regname == ia32dis::gAmd64Registers32[i].m_name)
        {
            regClass = REG_32BIT;
            regIndex = i;
            return true;
        }
        if (regname == ia32dis::gAmd64Registers16[i].m_name)
        {
            regClass = REG_16BIT;
            regIndex = i;
            return true;
        }
        if (regname == ia32dis::gAmd64Registers8[i].m_name)
        {
            regClass = REG_8BIT;
            regIndex = i;
            return true;
        }
        if (regname == ia32dis::gAmd64SIMDRegisters[i].m_name)
        {
            regClass = REG_XMM;
            regIndex = i;
            return true;
        }
    }
    if (regname == ia32dis::gAmd64ControlRegisters[ia32dis::AMD64_NUMBER_OF_CONTROL_REGISTERS - 1].m_name)
    {
        regClass = REG_CONTROL;
        regIndex = ia32dis::AMD64_NUMBER_OF_CONTROL_REGISTERS - 1;
        return true;
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
