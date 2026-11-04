#include "dismount/dismount.h"
/*
 * ArmAssembler.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/char.h"
#include "xStl/data/array.h"
#include "xStl/data/hash.h"
#include "xStl/data/sarray.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/exceptions.h"
#include "xStl/parser/parser.h"
#include "dismount/assembler/proc/arm/ArmAssembler.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"

using namespace armdis;

// Throw an exception with a message describing the assembler failure
#define ASM_CHECK(cond, msg) if (!(cond)) { XSTL_THROW(cException, XSTL_STRING(msg), EXCEPTION_FAILED); }

ArmAssembler::ArmAssembler(FirstPassBinaryPtr binary,
                           ArmInstructionSet::DisassemblerTypes type,
                           const ArmFeatureSet& features) :
    m_binary(binary),
    m_type(type),
    m_features(features)
{
}

cStringerStreamPtr ArmAssembler::getAssembler()
{
    return cStringerStreamPtr(new ArmAssemblerStream(*m_binary, m_type, m_features));
}

FirstPassBinaryPtr ArmAssembler::getFirstPass()
{
    return m_binary;
}

//////////////////////////////////////////////////////////////////////////
// Text helpers. The text is read through the xStl Parser. The operands are
// parsed by their syntax only, into ArmDecodedOperand.

namespace {

typedef ArmDecodedOperand Operand;
typedef ArmInstructionSet::DisassemblerTypes InstructionSet;

// True if a letter, a digit or '_' (a word)
bool isIdentifierChar(character c)
{
    return cChar::isLetter(c) || cChar::isDigit(c) || (c == XSTL_CHAR('_'));
}

bool isNextChar(Parser& parser, char c)
{
    return !parser.isEOS() && (parser.peekChar() == c);
}

bool isDigitAhead(Parser& parser)
{
    return !parser.isEOS() && cChar::isDigit(parser.peekChar());
}

// Read the word (letters, digits and '_') at the parser
cString readWord(Parser& parser)
{
    cString word;
    while (!parser.isEOS() && isIdentifierChar(parser.peekChar()))
        word += cString(parser.readChar());
    return word;
}

void skipBlanks(Parser& parser)
{
    while (!parser.isEOS() && ((parser.peekChar() == ' ') || (parser.peekChar() == '\t')))
        parser.readChar();
}

// The text of a floating point number: digits '.' digits {e {+-} digits}
double readFloat(Parser& parser, uint64 whole)
{
    double value = (double)whole;
    if (isNextChar(parser, '.'))
    {
        parser.readChar();
        double scale = 0.1;
        while (isDigitAhead(parser))
        {
            value += (parser.readChar() - '0') * scale;
            scale /= 10;
        }
    }
    if (isNextChar(parser, 'e'))
    {
        parser.readChar();
        bool negative = false;
        if (isNextChar(parser, '-') || isNextChar(parser, '+'))
            negative = (parser.readChar() == '-');
        ASM_CHECK(isDigitAhead(parser), "invalid exponent");
        uint exponent = 0;
        while (isDigitAhead(parser))
            exponent = exponent * 10 + (parser.readChar() - '0');
        for (uint i = 0; i < exponent; i++)
            value = negative ? (value / 10) : (value * 10);
    }
    return value;
}

// Read the number at the parser: "#-0x10", "5", "1.5". Sets the immediate
// (or the float immediate) of the operand
void readNumber(Parser& parser, Operand& operand)
{
    bool negative = false;
    if (isNextChar(parser, '-') || isNextChar(parser, '+'))
        negative = (parser.readChar() == '-');
    ASM_CHECK(isDigitAhead(parser), "number was expected");
    uint64 value = 0;
    ASM_CHECK(parser.isUnsignedInteger64(&value, true), "invalid number");
    if (isNextChar(parser, '.') || isNextChar(parser, 'e'))
    {
        double number = readFloat(parser, value);
        operand.m_kind = Operand::KIND_FLOAT_IMMEDIATE;
        operand.m_floatImmediate = negative ? -number : number;
        return;
    }
    operand.m_kind = Operand::KIND_IMMEDIATE;
    operand.m_immediate = negative ? (int64)((uint64)0 - value) : (int64)value;
}

// The names of the arrangements and element sizes, after the '.' of a vector
// register
bool findArrangementName(const cString& name, uint& arrangement)
{
    static const struct { const char* m_name; uint m_arrangement; } names[] = {
        { "8b", Operand::ARRANGEMENT_8B }, { "16b", Operand::ARRANGEMENT_16B },
        { "4h", Operand::ARRANGEMENT_4H }, { "8h", Operand::ARRANGEMENT_8H },
        { "2s", Operand::ARRANGEMENT_2S }, { "4s", Operand::ARRANGEMENT_4S },
        { "1d", Operand::ARRANGEMENT_1D }, { "2d", Operand::ARRANGEMENT_2D },
        { "1q", Operand::ARRANGEMENT_1Q }, { "b", Operand::ARRANGEMENT_B },
        { "h", Operand::ARRANGEMENT_H }, { "s", Operand::ARRANGEMENT_S },
        { "d", Operand::ARRANGEMENT_D }, { "q", Operand::ARRANGEMENT_Q },
        { "2h", Operand::ARRANGEMENT_2H }, { "4b", Operand::ARRANGEMENT_4B }
    };
    for (uint i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (name == names[i].m_name)
        {
            arrangement = names[i].m_arrangement;
            return true;
        }
    return false;
}

// Return the shift or extend type of a keyword, SHIFT_NONE if it isn't one
uint findShiftName(const cString& name)
{
    static const struct { const char* m_name; uint m_shift; } names[] = {
        { "lsl", Operand::SHIFT_LSL }, { "lsr", Operand::SHIFT_LSR },
        { "asr", Operand::SHIFT_ASR }, { "ror", Operand::SHIFT_ROR },
        { "rrx", Operand::SHIFT_RRX }, { "msl", Operand::SHIFT_MSL },
        { "uxtb", Operand::EXTEND_UXTB }, { "uxth", Operand::EXTEND_UXTH },
        { "uxtw", Operand::EXTEND_UXTW }, { "uxtx", Operand::EXTEND_UXTX },
        { "sxtb", Operand::EXTEND_SXTB }, { "sxth", Operand::EXTEND_SXTH },
        { "sxtw", Operand::EXTEND_SXTW }, { "sxtx", Operand::EXTEND_SXTX }
    };
    for (uint i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (name == names[i].m_name)
            return names[i].m_shift;
    return Operand::SHIFT_NONE;
}

// Parse a register with its qualifiers at the parser: v0.4s, v0.s[1], z0.s,
// p0/m. The name is 'word', already read. Return false if it isn't a register
bool parseRegister(Parser& parser, const cString& word, InstructionSet type,
                   Operand& operand)
{
    uint registerId;
    if (!findRegister(type, word, registerId))
        return false;
    operand.m_kind = Operand::KIND_REGISTER;
    operand.m_register = registerId;
    operand.m_isPredicateCounter = (word.length() > 2) && (word.left(2) == "pn");
    if (isNextChar(parser, '.'))
    {
        parser.readChar();
        ASM_CHECK(findArrangementName(readWord(parser), operand.m_arrangement),
                  "unknown arrangement");
    }
    if (isNextChar(parser, '['))
    {
        // v0.s[1]
        parser.readChar();
        skipBlanks(parser);
        ASM_CHECK(isDigitAhead(parser), "element index was expected");
        uint64 index = 0;
        ASM_CHECK(parser.isUnsignedInteger64(&index, true), "invalid element index");
        skipBlanks(parser);
        ASM_CHECK(isNextChar(parser, ']'), "']' was expected");
        parser.readChar();
        operand.m_elementIndex = (int)index;
    }
    if (isNextChar(parser, '!'))
    {
        // The writeback register of the memory copy and set: x2!
        parser.readChar();
        operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
    }
    if (isNextChar(parser, '/'))
    {
        parser.readChar();
        char qualifier = parser.readChar();
        ASM_CHECK((qualifier == 'm') || (qualifier == 'z'), "'/m' or '/z' was expected");
        operand.m_predicateQualifier = (qualifier == 'm') ? Operand::PREDICATE_MERGING :
                                                            Operand::PREDICATE_ZEROING;
    }
    return true;
}

// Parse the shift or extend at the parser: "lsl #3", "uxtw", "sxtx #2".
// 'word' is the keyword (already read)
void parseShift(Parser& parser, const cString& word, Operand& shift)
{
    shift.m_shift = findShiftName(word);
    ASM_CHECK(shift.m_shift != Operand::SHIFT_NONE, "unknown shift");
    skipBlanks(parser);
    // The amount is optional for the extends ("uxtw", "sxtx")
    if (parser.isEOS() || (!isNextChar(parser, '#') && !isDigitAhead(parser)))
        return;
    if (isNextChar(parser, '#'))
        parser.readChar();
    ASM_CHECK(isDigitAhead(parser), "shift amount was expected");
    uint64 amount = 0;
    ASM_CHECK(parser.isUnsignedInteger64(&amount, true), "invalid shift amount");
    shift.m_shiftAmount = (uint)amount;
    shift.m_isExplicit = true;
}

// Parse "[x1, #8]!", "[x1, x2, lsl #3]" at the parser (the '[' is next)
void parseMemory(Parser& parser, InstructionSet type, Operand& operand)
{
    parser.readChar();
    skipBlanks(parser);
    operand.m_kind = Operand::KIND_MEMORY;

    // The base
    cString word = readWord(parser);
    uint registerId;
    ASM_CHECK(findRegister(type, word, registerId), "base register was expected");
    operand.m_base = registerId;
    skipBlanks(parser);

    while (isNextChar(parser, ','))
    {
        parser.readChar();
        skipBlanks(parser);
        if (isNextChar(parser, '#') || isDigitAhead(parser) ||
            isNextChar(parser, '-') || isNextChar(parser, '+'))
        {
            // The offset
            if (isNextChar(parser, '#'))
                parser.readChar();
            Operand number;
            readNumber(parser, number);
            ASM_CHECK(number.m_kind == Operand::KIND_IMMEDIATE, "invalid offset");
            operand.m_offset = number.m_immediate;
        } else
        {
            cString part = readWord(parser);
            uint partShift = findShiftName(part);
            if (partShift != Operand::SHIFT_NONE)
            {
                // The shift or extend of the index
                Operand shift;
                parseShift(parser, part, shift);
                operand.m_shift = shift.m_shift;
                operand.m_shiftAmount = shift.m_shiftAmount;
                operand.m_isExplicit = shift.m_isExplicit;
            } else
            {
                ASM_CHECK(findRegister(type, part, registerId), "index register was expected");
                operand.m_index = registerId;
                if (isNextChar(parser, '.'))
                {
                    parser.readChar();
                    ASM_CHECK(findArrangementName(readWord(parser), operand.m_indexArrangement),
                              "unknown arrangement");
                }
            }
        }
        skipBlanks(parser);
    }
    ASM_CHECK(isNextChar(parser, ']'), "']' was expected");
    parser.readChar();
    if (isNextChar(parser, '!'))
    {
        parser.readChar();
        operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
    }
}

// Parse a list of registers: "{ v0.16b, v1.16b }", "{ v0.16b - v3.16b }"
void parseList(Parser& parser, InstructionSet type, Operand& operand)
{
    parser.readChar();
    skipBlanks(parser);
    uint count = 0;
    Operand first;
    Operand last;
    for (;;)
    {
        cString word = readWord(parser);
        Operand item;
        ASM_CHECK(parseRegister(parser, word, type, item), "register was expected");
        if (count == 0)
            first = item;
        last = item;
        count++;
        skipBlanks(parser);
        if (isNextChar(parser, '-'))
        {
            // A range: the registers between
            parser.readChar();
            skipBlanks(parser);
            cString endWord = readWord(parser);
            Operand end;
            ASM_CHECK(parseRegister(parser, endWord, type, end), "register was expected");
            ASM_CHECK(end.m_register >= item.m_register, "invalid register range");
            count += end.m_register - item.m_register;
            last = end;
            skipBlanks(parser);
        }
        if (!isNextChar(parser, ','))
            break;
        parser.readChar();
        skipBlanks(parser);
    }
    ASM_CHECK(isNextChar(parser, '}'), "'}' was expected");
    parser.readChar();
    operand = first;
    operand.m_kind = Operand::KIND_REGISTER_LIST;
    operand.m_count = count;
    operand.m_stride = 1;
    if (isNextChar(parser, '['))
    {
        parser.readChar();
        uint64 index = 0;
        ASM_CHECK(isDigitAhead(parser) && parser.isUnsignedInteger64(&index, true),
                  "element index was expected");
        ASM_CHECK(isNextChar(parser, ']'), "']' was expected");
        parser.readChar();
        operand.m_elementIndex = (int)index;
    }
}

// Parse the text of one operand
void parseOperand(const cString& text, InstructionSet type, uint64 address,
                  Operand& operand)
{
    operand = Operand();
    cSArray<char> ascii = text.getASCIIstring();
    Parser parser(ascii.getBuffer(), ascii.getBuffer(), ascii.getSize() - 1, 1);
    skipBlanks(parser);
    ASM_CHECK(!parser.isEOS(), "operand was expected");

    char c = parser.peekChar();
    if (c == '[')
    {
        parseMemory(parser, type, operand);
    } else if (c == '{')
    {
        parseList(parser, type, operand);
    } else if (c == '#')
    {
        parser.readChar();
        skipBlanks(parser);
        readNumber(parser, operand);
        operand.m_hasHash = true;
    } else if (cChar::isDigit(c) || (c == '-') || (c == '+'))
    {
        readNumber(parser, operand);
    } else if (c == '.')
    {
        // The location counter: ".", ".+8", ". - 4"
        parser.readChar();
        skipBlanks(parser);
        int64 offset = 0;
        if (isNextChar(parser, '+') || isNextChar(parser, '-'))
        {
            bool negative = (parser.readChar() == '-');
            skipBlanks(parser);
            Operand number;
            readNumber(parser, number);
            ASM_CHECK(number.m_kind == Operand::KIND_IMMEDIATE, "invalid offset");
            offset = negative ? -number.m_immediate : number.m_immediate;
        }
        operand.m_kind = Operand::KIND_BRANCH_TARGET;
        operand.m_immediate = (int64)(address + (uint64)offset);
    } else
    {
        cString word = readWord(parser);
        ASM_CHECK(word.length() != 0, "invalid operand");
        if (!parseRegister(parser, word, type, operand))
        {
            // A condition, an option, a system register or a label
            operand.m_kind = Operand::KIND_NAME;
            operand.m_name = word;
        }
    }
    skipBlanks(parser);
    ASM_CHECK(parser.isEOS(), "unexpected text after the operand");
}

// Split the operands of the line by the commas outside of the brackets
void splitOperands(const cString& text, cArray<cString>& parts)
{
    uint depth = 0;
    cString current;
    for (uint i = 0; i < text.length(); i++)
    {
        character c = text[i];
        if ((c == XSTL_CHAR('[')) || (c == XSTL_CHAR('{')))
            depth++;
        else if ((c == XSTL_CHAR(']')) || (c == XSTL_CHAR('}')))
        {
            ASM_CHECK(depth > 0, "unbalanced bracket");
            depth--;
        }
        if ((c == XSTL_CHAR(',')) && (depth == 0))
        {
            current.trim();
            parts.append(current);
            current = cString();
        } else
        {
            current += cString(c);
        }
    }
    ASM_CHECK(depth == 0, "unbalanced bracket");
    current.trim();
    if ((current.length() != 0) || (parts.getSize() != 0))
        parts.append(current);
}

// Return the text of a line without its comment: ';' and '//' start one
cString stripComment(const cString& line)
{
    cString result(line);
    uint position = result.find(XSTL_STRING(";"));
    if (position < result.length())
        result = result.left(position);
    position = result.find(XSTL_STRING("//"));
    if (position < result.length())
        result = result.left(position);
    result.trim();
    return result;
}

// True if an operand begins with a shift or extend keyword and so belongs to
// the operand before it
bool isShiftOperand(const cString& text)
{
    uint length = 0;
    while ((length < text.length()) && cChar::isLetter(text[length]))
        length++;
    return (length > 0) && (findShiftName(text.left(length)) != Operand::SHIFT_NONE);
}

/*
 * The index of the tables by mnemonic. An entry is a candidate of the text
 * 'mnemonic' if its mnemonic is a prefix of the text.
 */
struct Candidate {
    const ArmEncodingEntry* m_entry;
    // The position in the tables, to keep their order
    uint m_order;
    // The size of the instruction in bytes
    uint m_size;
};

class MnemonicIndex {
public:
    MnemonicIndex(InstructionSet type)
    {
        uint order = 0;
        for (uint size = 4; size >= 2; size -= 2)
        {
            if ((size == 2) && (type != ArmInstructionSet::ARM_T32))
                continue;
            for (const ArmEncodingEntry* const* table = ArmStreamDisassembler::getTables(type, size);
                 *table != NULL; table++)
                for (const ArmEncodingEntry* entry = *table; entry->m_mnemonic != NULL; entry++)
                {
                    Candidate candidate;
                    candidate.m_entry = entry;
                    candidate.m_order = order++;
                    candidate.m_size = size;
                    cString name(entry->m_mnemonic);
                    if (!m_entries.hasKey(name))
                        m_entries.append(name, cArray<Candidate>());
                    m_entries[name].append(candidate);
                }
            // T16 comes before T32 in the search order
            if (type == ArmInstructionSet::ARM_T32)
                break;
        }
    }

    // Collect the candidates of a mnemonic text in the order of the tables
    void find(const cString& mnemonic, cArray<Candidate>& result) const
    {
        for (uint length = mnemonic.length(); length >= 1; length--)
        {
            cString name = mnemonic.left(length);
            if (!m_entries.hasKey(name))
                continue;
            const cArray<Candidate>& found = m_entries[name];
            for (uint i = 0; i < found.getSize(); i++)
            {
                // Insert sorted by the order
                uint position = result.getSize();
                result.append(found[i]);
                while ((position > 0) && (result[position - 1].m_order > found[i].m_order))
                {
                    result[position] = result[position - 1];
                    position--;
                }
                result[position] = found[i];
            }
        }
    }

private:
    cHash<cString, cArray<Candidate> > m_entries;
};

const MnemonicIndex& getMnemonicIndex(InstructionSet type)
{
    switch (type)
    {
    case ArmInstructionSet::ARM_A64:
    {
        static const MnemonicIndex index(ArmInstructionSet::ARM_A64);
        return index;
    }
    case ArmInstructionSet::ARM_A32:
    {
        static const MnemonicIndex index(ArmInstructionSet::ARM_A32);
        return index;
    }
    case ArmInstructionSet::ARM_T32:
    {
        static const MnemonicIndex index(ArmInstructionSet::ARM_T32);
        return index;
    }
    }
    CHECK_FAIL();
}

// True if two decoded operands name the same registers: the check that the
// register sizes of the text and of the decoded bits agree
bool isSameRegisters(const Operand& parsed, const Operand& decoded)
{
    switch (parsed.m_kind)
    {
    case Operand::KIND_REGISTER:
    case Operand::KIND_SHIFTED_REGISTER:
    case Operand::KIND_REGISTER_LIST:
        // (a word which is a register name too, "za", can be an option)
        if (decoded.m_kind == Operand::KIND_OPTION)
            return true;
        return ((decoded.m_kind == Operand::KIND_REGISTER) ||
                (decoded.m_kind == Operand::KIND_SHIFTED_REGISTER) ||
                (decoded.m_kind == Operand::KIND_REGISTER_LIST)) &&
               (parsed.m_register == decoded.m_register);
    case Operand::KIND_MEMORY:
        return (decoded.m_kind == Operand::KIND_MEMORY) &&
               (parsed.m_base == decoded.m_base) &&
               (parsed.m_index == decoded.m_index);
    default:
        return true;
    }
}

// Classify an A64 general purpose register id: its size and number
bool classifyRegister(uint id, bool& is64, uint& number, bool& isStackPointer)
{
    isStackPointer = false;
    if (id <= ARM64_REG_X30)
    {
        is64 = true;
        number = id;
        return true;
    }
    if ((id >= ARM64_REG_W0) && (id <= ARM64_REG_W30))
    {
        is64 = false;
        number = id - ARM64_REG_W0;
        return true;
    }
    if ((id == ARM64_REG_SP) || (id == ARM64_REG_XZR) || (id == ARM64_REG_WSP) ||
        (id == ARM64_REG_WZR))
    {
        is64 = (id == ARM64_REG_SP) || (id == ARM64_REG_XZR);
        number = 31;
        isStackPointer = (id == ARM64_REG_SP) || (id == ARM64_REG_WSP);
        return true;
    }
    return false;
}

/*
 * The result of assembling a line
 */
struct Assembled {
    uint32 m_encoding;
    uint m_size;
    // The label of a branch to a name, empty if none
    cString m_label;
};

// Assemble one instruction, see ArmAssemblerStream::assembleInstruction()
void assembleText(InstructionSet type,
                  const ArmFeatureSet& features,
                  const cString& line,
                  uint64 address,
                  Assembled& result)
{
    cString text = stripComment(line);
    text.makeLower();
    ASM_CHECK(text.length() != 0, "empty instruction");

    // The mnemonic is the first word
    uint split = 0;
    while ((split < text.length()) && (text[split] != XSTL_CHAR(' ')) &&
           (text[split] != XSTL_CHAR('\t')))
        split++;
    cString mnemonic = text.left(split);
    cString operandsText = text.part(split, text.length());
    operandsText.trim();

    // Parse the operands. A shift or an extend belongs to the operand
    // before it, and an operand after a memory operand is its post index
    cArray<cString> parts;
    splitOperands(operandsText, parts);
    cArray<Operand> parsed;
    for (uint i = 0; i < parts.getSize(); i++)
    {
        if (isShiftOperand(parts[i]))
        {
            ASM_CHECK(parsed.getSize() > 0, "unexpected shift");
            Operand& previous = parsed[parsed.getSize() - 1];
            cSArray<char> ascii = parts[i].getASCIIstring();
            Parser parser(ascii.getBuffer(), ascii.getBuffer(), ascii.getSize() - 1, 1);
            Operand shift;
            parseShift(parser, readWord(parser), shift);
            if (previous.m_kind == Operand::KIND_REGISTER)
                previous.m_kind = Operand::KIND_SHIFTED_REGISTER;
            else
                ASM_CHECK(previous.m_kind == Operand::KIND_IMMEDIATE,
                          "this operand has no shift");
            previous.m_shift = shift.m_shift;
            previous.m_shiftAmount = shift.m_shiftAmount;
            previous.m_isExplicit = shift.m_isExplicit;
            continue;
        }
        Operand operand;
        parseOperand(parts[i], type, address, operand);
        if ((parsed.getSize() > 0) &&
            (parsed[parsed.getSize() - 1].m_kind == Operand::KIND_MEMORY) &&
            (parsed[parsed.getSize() - 1].m_addressing == Operand::ADDRESSING_OFFSET) &&
            (parsed[parsed.getSize() - 1].m_offset == 0) &&
            (parsed[parsed.getSize() - 1].m_index == ARM_NO_REGISTER))
        {
            // [x1], #8 and [x1], x2
            Operand& memory = parsed[parsed.getSize() - 1];
            memory.m_addressing = Operand::ADDRESSING_POST_INDEX;
            if (operand.m_kind == Operand::KIND_IMMEDIATE)
                memory.m_offset = operand.m_immediate;
            else if (operand.m_kind == Operand::KIND_REGISTER)
                memory.m_postIndexRegister = operand.m_register;
            else
                ASM_CHECK(false, "invalid post index");
            continue;
        }
        parsed.append(operand);
    }

    ArmCodecContext context;
    context.m_type = type;
    context.m_address = address;
    context.m_features = ArmFeatureSet::all();

    cArray<Candidate> candidates;
    getMnemonicIndex(type).find(mnemonic, candidates);
    ASM_CHECK(candidates.getSize() != 0, "unknown instruction");

    // The feature which a valid encoding needs, for the message
    int missingFeature = -1;
    for (uint i = 0; i < candidates.getSize(); i++)
    {
        const ArmEncodingEntry* entry = candidates[i].m_entry;
        if ((entry->m_flags & ARM_ENTRY_DECODE_ONLY) != 0)
            continue;
        context.m_entry = entry;
        uint32 encoding = entry->m_value;

        // The parts of the mnemonic consume the text after the name
        cString suffix = mnemonic.part(cString(entry->m_mnemonic).length(), mnemonic.length());
        bool isValid = true;
        uint operandsCount = 0;
        for (uint j = 0; j < ARM_MAX_OPERAND_SPECS; j++)
        {
            const ArmOperandSpec& spec = entry->m_operands[j];
            if (spec.m_kind == ARM_KIND_NONE)
                break;
            if (ArmOperandCodec::isMnemonicPart(spec))
            {
                if (!ArmOperandCodec::encodeMnemonicPart(spec, suffix, context, encoding))
                    isValid = false;
            } else if (!ArmOperandCodec::isHidden(spec))
            {
                // A pair of registers is two operands of the text
                operandsCount += (spec.m_kind == ARM_KIND_GPR) &&
                                 ((spec.m_flags & ARM_GPR_PAIR) != 0) ? 2 : 1;
            }
        }
        if (!isValid || (suffix.length() != 0) || (operandsCount != parsed.getSize()))
            continue;

        // The operands
        cString label;
        uint next = 0;
        cArray<Operand> used;
        // The size of the registers which share the sf bit has to agree
        uint sfBits = 0;
        for (uint j = 0; isValid && (j < ARM_MAX_OPERAND_SPECS); j++)
        {
            const ArmOperandSpec& spec = entry->m_operands[j];
            if (spec.m_kind == ARM_KIND_NONE)
                break;
            if (ArmOperandCodec::isMnemonicPart(spec) || ArmOperandCodec::isHidden(spec))
                continue;
            Operand operand = parsed[next++];
            if ((spec.m_kind == ARM_KIND_GPR) && ((spec.m_flags & ARM_GPR_PAIR) != 0))
            {
                // x0, x1: the register and the next one
                const Operand& second = parsed[next++];
                bool isPair = (operand.m_kind == Operand::KIND_REGISTER) &&
                              (second.m_kind == Operand::KIND_REGISTER);
                if (isPair)
                {
                    bool isFirst64, isFirstSp, isSecond64, isSecondSp;
                    uint firstNumber, secondNumber;
                    isPair = classifyRegister(operand.m_register, isFirst64, firstNumber, isFirstSp) &&
                             classifyRegister(second.m_register, isSecond64, secondNumber, isSecondSp) &&
                             (isFirst64 == isSecond64) &&
                             ((secondNumber == firstNumber + 1) ||
                              ((firstNumber == 31) && (secondNumber == 31)));
                }
                if (!isPair)
                {
                    isValid = false;
                    break;
                }
                operand.m_kind = Operand::KIND_REGISTER_LIST;
                operand.m_count = 2;
                operand.m_stride = 1;
            }
            used.append(operand);
            if (((spec.m_kind == ARM_KIND_GPR) || (spec.m_kind == ARM_KIND_SHIFTED_REGISTER)) &&
                ((spec.m_flags & ARM_GPR_SF) != 0) &&
                ((operand.m_kind == Operand::KIND_REGISTER) ||
                 (operand.m_kind == Operand::KIND_SHIFTED_REGISTER)))
            {
                bool is64, isSp;
                uint number;
                if (classifyRegister(operand.m_register, is64, number, isSp))
                {
                    uint bits = is64 ? 64 : 32;
                    if ((sfBits != 0) && (sfBits != bits))
                    {
                        isValid = false;
                        break;
                    }
                    sfBits = bits;
                }
            }
            // A name where an address is: a label, left unresolved
            if ((operand.m_kind == Operand::KIND_NAME) &&
                ((spec.m_kind == ARM_KIND_BRANCH_TARGET) ||
                 (spec.m_kind == ARM_KIND_A64_PC_RELATIVE)))
            {
                label = operand.m_name;
                operand.m_kind = Operand::KIND_BRANCH_TARGET;
                operand.m_immediate = (int64)address;
            }
            isValid = ArmOperandCodec::encode(spec, operand, context, encoding);
        }
        // The hidden copies come after the registers they copy
        for (uint j = 0; isValid && (j < ARM_MAX_OPERAND_SPECS); j++)
        {
            const ArmOperandSpec& spec = entry->m_operands[j];
            if (spec.m_kind == ARM_KIND_NONE)
                break;
            if (ArmOperandCodec::isHidden(spec))
                isValid = ArmOperandCodec::encode(spec, Operand(), context, encoding);
        }
        if (!isValid || ((encoding & entry->m_mask) != (entry->m_value & entry->m_mask)))
            continue;

        // The bits have to decode to this instruction, or to an alias of it:
        // the constraints of the entries and the priority of the aliases are
        // applied by the decoder
        ArmOpcodePtr opcode = ArmStreamDisassembler::decode(type, encoding,
                                                            candidates[i].m_size, address,
                                                            ArmFeatureSet::all());
        if (opcode.isEmpty())
            continue;
        if ((opcode->getEntry() != entry) &&
            ((opcode->getEntry()->m_flags & ARM_ENTRY_ALIAS) == 0))
            continue;
        // The registers of the text are the registers of the bits (an alias
        // has other operands: its bits were checked by the entry)
        if (opcode->getEntry() == entry)
        {
            if (opcode->getOperandsCount() != used.getSize())
                continue;
            for (uint j = 0; isValid && (j < used.getSize()); j++)
                isValid = isSameRegisters(used[j], opcode->getOperand(j));
        }
        if (!isValid)
            continue;

        // The instruction is valid, if its feature is on
        if (!features.has((ArmFeatures::Feature)entry->m_feature))
        {
            if (missingFeature < 0)
                missingFeature = entry->m_feature;
            continue;
        }
        result.m_encoding = encoding;
        result.m_size = candidates[i].m_size;
        result.m_label = label;
        return;
    }

    if (missingFeature >= 0)
        XSTL_THROW(cException,
                   (cString("instruction requires: ") +
                    ArmFeatures::getName((ArmFeatures::Feature)missingFeature)).getBuffer(),
                   EXCEPTION_FAILED);
    ASM_CHECK(false, "invalid operands for the instruction");
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////
// ArmAssemblerStream

ArmAssemblerStream::ArmAssemblerStream(FirstPassBinary& binary,
                                       ArmInstructionSet::DisassemblerTypes type,
                                       const ArmFeatureSet& features) :
    m_binary(binary),
    m_type(type),
    m_features(features),
    m_itState(0)
{
}

ArmAssemblerStream::~ArmAssemblerStream()
{
    // Destructors must not throw
    XSTL_TRY
    {
        if (getData().length() != 0)
            assemble();
    }
    XSTL_CATCH_ALL
    {
    }
}

void ArmAssemblerStream::assemble()
{
    if (getData().length() == 0)
        return;

    // Each line holds a single instruction
    cString text = getData();
    // The buffered text has been consumed (even if a line is invalid)
    clean();
    cList<cString> lines = text.split(XSTL_STRING("\n"));
    for (cList<cString>::iterator line = lines.begin(); line != lines.end(); line++)
    {
        cString current(*line);
        current = stripComment(current);
        if (current.length() != 0)
            assembleLine(current);
    }
}

void ArmAssemblerStream::assembleLine(const cString& line)
{
    Assembled assembled;
    // TODO: T32 tracks the it block of the previous lines in m_itState
    (void)m_itState;
    // The address of an instruction in a binary is the offset in its block
    uint64 address = m_binary.getCurrentBlockData().getSize();
    assembleText(m_type, m_features, line, address, assembled);

    uint position = m_binary.getCurrentBlockData().getSize();
    if (assembled.m_size == 2)
    {
        m_binary.appendUint8((uint8)(assembled.m_encoding));
        m_binary.appendUint8((uint8)(assembled.m_encoding >> 8));
    } else if (m_type == ArmInstructionSet::ARM_T32)
    {
        m_binary.appendUint8((uint8)(assembled.m_encoding >> 16));
        m_binary.appendUint8((uint8)(assembled.m_encoding >> 24));
        m_binary.appendUint8((uint8)(assembled.m_encoding));
        m_binary.appendUint8((uint8)(assembled.m_encoding >> 8));
    } else
    {
        m_binary.appendUint8((uint8)(assembled.m_encoding));
        m_binary.appendUint8((uint8)(assembled.m_encoding >> 8));
        m_binary.appendUint8((uint8)(assembled.m_encoding >> 16));
        m_binary.appendUint8((uint8)(assembled.m_encoding >> 24));
    }

    // A branch to a name: the offset is left 0 and the name is a dependency
    // of the binary
    // TODO: SecondPassBinary doesn't know how to patch the A64 fields
    if (assembled.m_label.length() != 0)
        m_binary.getCurrentDependecies().addDependency(
            assembled.m_label, position, assembled.m_size,
            BinaryDependencies::DEP_RELATIVE, 2);
}

void ArmAssemblerStream::assembleInstruction(ArmInstructionSet::DisassemblerTypes type,
                                             const ArmFeatureSet& features,
                                             const cString& line,
                                             uint64 address,
                                             uint itState,
                                             uint32& encoding,
                                             uint& size)
{
    // TODO: T32 it blocks
    Assembled assembled;
    assembleText(type, features, line, address, assembled);
    encoding = assembled.m_encoding;
    size = assembled.m_size;
}
