#include "dismount/dismount.h"
/*
 * ArmAssemblerA32.cpp
 *
 * Implementation file. The AArch32 part of the ARM assembler, see
 * ArmAssemblerA32.h.
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
#include "dismount/assembler/proc/arm/ArmAssemblerA32.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"
#include "dismount/proc/arm/ArmOperandCodecA32.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"

namespace armdis {

// Throw an exception with a message describing the assembler failure
#define ASM_CHECK(cond, msg) if (!(cond)) { XSTL_THROW(cException, XSTL_STRING(msg), EXCEPTION_FAILED); }

namespace {

typedef ArmDecodedOperand Operand;
typedef ArmInstructionSet::DisassemblerTypes InstructionSet;

// The m_elementIndex of "d0[]": all the lanes
enum { ALL_LANES = 255 };

//////////////////////////////////////////////////////////////////////////
// Text helpers

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

// The fraction and the exponent of a floating point number
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

// Read the number at the parser: "-0x10", "5", "1.5e0". Sets the immediate
// (or the float immediate) of the operand; 'negative' tells if it had a minus
void readNumber(Parser& parser, Operand& operand, bool& negative)
{
    negative = false;
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

// The shift keyword at the start of a text, SHIFT_NONE if there is none
uint findShiftName(const cString& name)
{
    if (name == "lsl") return Operand::SHIFT_LSL;
    if (name == "lsr") return Operand::SHIFT_LSR;
    if (name == "asr") return Operand::SHIFT_ASR;
    if (name == "ror") return Operand::SHIFT_ROR;
    if (name == "rrx") return Operand::SHIFT_RRX;
    return Operand::SHIFT_NONE;
}

// The first word of a text
cString firstWord(const cString& text)
{
    uint length = 0;
    while ((length < text.length()) && isIdentifierChar(text[length]))
        length++;
    return text.left(length);
}

bool isShiftPiece(const cString& text)
{
    cString word = firstWord(text);
    if (findShiftName(word) == Operand::SHIFT_NONE)
        return false;
    // "lsl" alone, "lsl #2", "lsl r2" (a register named lsl... doesn't exist)
    return true;
}

// Parse the register at the parser (the name 'word' was read). Return false if
// it isn't a register. The suffix [n] / [] and the '!' are read
bool parseRegister(Parser& parser, const cString& word, InstructionSet type, Operand& operand)
{
    uint registerId;
    if (!findRegister(type, word, registerId))
        return false;
    operand.m_kind = Operand::KIND_REGISTER;
    operand.m_register = registerId;
    if (isNextChar(parser, '['))
    {
        parser.readChar();
        skipBlanks(parser);
        if (isNextChar(parser, ']'))
        {
            operand.m_elementIndex = ALL_LANES;
        } else
        {
            ASM_CHECK(isDigitAhead(parser), "element index was expected");
            uint64 index = 0;
            ASM_CHECK(parser.isUnsignedInteger64(&index, true), "invalid element index");
            operand.m_elementIndex = (int)index;
            skipBlanks(parser);
        }
        ASM_CHECK(isNextChar(parser, ']'), "']' was expected");
        parser.readChar();
    }
    if (isNextChar(parser, '!'))
    {
        parser.readChar();
        operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
    }
    return true;
}

// Parse "lsl #3", "ror r2", "rrx" (the keyword was read) into the shift
// fields of 'operand'
void parseShift(Parser& parser, const cString& word, Operand& operand, InstructionSet type)
{
    operand.m_shift = findShiftName(word);
    ASM_CHECK(operand.m_shift != Operand::SHIFT_NONE, "unknown shift");
    skipBlanks(parser);
    if (operand.m_shift == Operand::SHIFT_RRX)
        return;
    if (isNextChar(parser, '#'))
    {
        parser.readChar();
        skipBlanks(parser);
        ASM_CHECK(isDigitAhead(parser), "shift amount was expected");
        uint64 amount = 0;
        ASM_CHECK(parser.isUnsignedInteger64(&amount, true), "invalid shift amount");
        operand.m_shiftAmount = (uint)amount;
    } else if (isDigitAhead(parser))
    {
        uint64 amount = 0;
        ASM_CHECK(parser.isUnsignedInteger64(&amount, true), "invalid shift amount");
        operand.m_shiftAmount = (uint)amount;
    } else
    {
        cString registerName = readWord(parser);
        uint registerId;
        ASM_CHECK(findRegister(type, registerName, registerId), "shift amount was expected");
        operand.m_shiftRegister = registerId;
    }
    operand.m_isExplicit = true;
    // (a zero amount of lsl is no shift: it is kept as written)
}

// Parse "[r0, #4]!", "[r0], ...", "[r0:128]", "[r0, -r1, lsl #2]" (the '[' is next)
void parseMemory(Parser& parser, InstructionSet type, Operand& operand)
{
    parser.readChar();
    skipBlanks(parser);
    operand.m_kind = Operand::KIND_MEMORY;
    uint registerId;
    cString word = readWord(parser);
    ASM_CHECK(findRegister(type, word, registerId), "base register was expected");
    operand.m_base = registerId;
    skipBlanks(parser);
    if (isNextChar(parser, ':'))
    {
        // The alignment in bits
        parser.readChar();
        skipBlanks(parser);
        uint64 alignment = 0;
        ASM_CHECK(isDigitAhead(parser) && parser.isUnsignedInteger64(&alignment, true),
                  "alignment was expected");
        operand.m_alignment = (uint)alignment;
        skipBlanks(parser);
    }
    while (isNextChar(parser, ','))
    {
        parser.readChar();
        skipBlanks(parser);
        bool isSign = isNextChar(parser, '-') || isNextChar(parser, '+');
        bool isNegative = isNextChar(parser, '-');
        if (isNextChar(parser, '#') || isDigitAhead(parser) ||
            (isSign && !parser.isEOS()))
        {
            if (isNextChar(parser, '#'))
            {
                parser.readChar();
                skipBlanks(parser);
                isNegative = isNextChar(parser, '-');
                isSign = isNextChar(parser, '-') || isNextChar(parser, '+');
            }
            if (isSign)
            {
                // "-r1" is a register, "-4" and "#-4" a number
                parser.readChar();
                skipBlanks(parser);
            }
            if (isDigitAhead(parser))
            {
                Operand number;
                bool negative;
                readNumber(parser, number, negative);
                ASM_CHECK(number.m_kind == Operand::KIND_IMMEDIATE, "invalid offset");
                operand.m_offset = number.m_immediate;
                operand.m_isSubtract = isNegative;
                operand.m_hasHash = true;
            } else
            {
                ASM_CHECK(isSign, "invalid offset");
                cString part = readWord(parser);
                ASM_CHECK(findRegister(type, part, registerId), "index register was expected");
                operand.m_index = registerId;
                operand.m_isSubtract = isNegative;
            }
        } else
        {
            cString part = readWord(parser);
            uint shift = findShiftName(part);
            if (shift != Operand::SHIFT_NONE)
            {
                Operand holder;
                parseShift(parser, part, holder, type);
                operand.m_shift = holder.m_shift;
                operand.m_shiftAmount = holder.m_shiftAmount;
            } else
            {
                ASM_CHECK(findRegister(type, part, registerId), "index register was expected");
                operand.m_index = registerId;
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

// Parse a list: "{r0, r4-r6, lr}", "{d0, d2}", "{d0[], d1[]}", "{d0[1]}" and
// the option "{5}" of ldc. A general purpose list is a register mask, the
// others a first register, a count and a stride
void parseList(Parser& parser, InstructionSet type, Operand& operand)
{
    parser.readChar();
    skipBlanks(parser);
    if (isDigitAhead(parser))
    {
        // The coprocessor option of ldc / stc
        uint64 value = 0;
        ASM_CHECK(parser.isUnsignedInteger64(&value, true), "invalid option");
        skipBlanks(parser);
        ASM_CHECK(isNextChar(parser, '}'), "'}' was expected");
        parser.readChar();
        operand.m_kind = Operand::KIND_IMMEDIATE;
        operand.m_immediate = (int64)value;
        operand.m_optionScheme = ARM_A32_OPT_COPROC_OPTION;
        return;
    }
    uint32 mask = 0;
    uint count = 0;
    Operand first;
    Operand previous;
    uint stride = 0;
    bool isGeneral = false;
    for (;;)
    {
        cString word = readWord(parser);
        Operand item;
        // The registers that follow d31 in LLVM's enumeration, which it prints in
        // the lists that run past d31 (a Neon list of four registers from d30)
        static const char* const pastD31[] = { "fpinst2", "mvfr0", "mvfr1", "mvfr2" };
        uint pastIndex = 4;
        for (uint k = 0; k < 4; k++)
            if (word == pastD31[k])
                pastIndex = k;
        if (pastIndex < 4)
        {
            item.m_kind = Operand::KIND_REGISTER;
            item.m_register = ARM_REG_D0 + 32 + pastIndex;
        } else
        {
            ASM_CHECK(parseRegister(parser, word, type, item), "register was expected");
        }
        isGeneral = (item.m_register >= ARM_REG_R0) && (item.m_register <= ARM_REG_PC);
        uint last = item.m_register;
        skipBlanks(parser);
        if (isNextChar(parser, '-'))
        {
            parser.readChar();
            skipBlanks(parser);
            cString endWord = readWord(parser);
            Operand end;
            ASM_CHECK(parseRegister(parser, endWord, type, end), "register was expected");
            ASM_CHECK(end.m_register >= item.m_register, "invalid register range");
            last = end.m_register;
            skipBlanks(parser);
        }
        if (isGeneral)
        {
            for (uint r = item.m_register; r <= last; r++)
                mask |= 1u << (r - ARM_REG_R0);
        } else
        {
            if (count == 0)
                first = item;
            else
            {
                uint distance = (item.m_register - previous.m_register) & 31;
                ASM_CHECK((distance == 1) || (distance == 2), "invalid register list");
                if (stride == 0)
                    stride = distance;
                ASM_CHECK(stride == distance, "invalid register list");
            }
            if (last != item.m_register)
            {
                uint rangeStride = (stride == 0) ? 1 : stride;
                count += (last - item.m_register) / rangeStride;
                if (stride == 0)
                    stride = 1;
            }
            count++;
            previous = item;
            previous.m_register = last;
        }
        if (!isNextChar(parser, ','))
            break;
        parser.readChar();
        skipBlanks(parser);
    }
    ASM_CHECK(isNextChar(parser, '}'), "'}' was expected");
    parser.readChar();
    if (isGeneral)
    {
        operand.m_kind = Operand::KIND_REGISTER_LIST;
        operand.m_registerMask = mask;
        skipBlanks(parser);
        if (isNextChar(parser, '^'))
        {
            parser.readChar();
            operand.m_isExplicit = true;
        }
        return;
    }
    operand = first;
    operand.m_kind = Operand::KIND_REGISTER_LIST;
    operand.m_count = count;
    operand.m_stride = (stride == 0) ? 1 : stride;
}

// Parse the text of one operand
void parseOperand(const cString& text, InstructionSet type, uint64 address, Operand& operand)
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
        bool negative;
        readNumber(parser, operand, negative);
        operand.m_hasHash = true;
        if (isNextChar(parser, '!'))
        {
            parser.readChar();
            operand.m_addressing = Operand::ADDRESSING_PRE_INDEX;
        }
    } else if (cChar::isDigit(c) || (c == '-') || (c == '+'))
    {
        bool negative;
        // "-r1" is a negative register (a post index), not a number
        if ((c == '-') || (c == '+'))
        {
            parser.readChar();
            skipBlanks(parser);
            if (!isDigitAhead(parser))
            {
                cString word = readWord(parser);
                ASM_CHECK(parseRegister(parser, word, type, operand), "invalid operand");
                operand.m_isSubtract = (c == '-');
                skipBlanks(parser);
                ASM_CHECK(parser.isEOS(), "unexpected text after the operand");
                operand.m_name = text;
                operand.m_name.trim();
                return;
            }
            readNumber(parser, operand, negative);
            if (c == '-')
                operand.m_immediate = -operand.m_immediate;
        } else
        {
            readNumber(parser, operand, negative);
        }
    } else if (c == '.')
    {
        // The location counter: ".", ".+8", ". - 4"
        parser.readChar();
        skipBlanks(parser);
        int64 offset = 0;
        if (isNextChar(parser, '+') || isNextChar(parser, '-'))
        {
            bool isMinus = (parser.readChar() == '-');
            skipBlanks(parser);
            Operand number;
            bool negative;
            readNumber(parser, number, negative);
            ASM_CHECK(number.m_kind == Operand::KIND_IMMEDIATE, "invalid offset");
            offset = isMinus ? -number.m_immediate : number.m_immediate;
        }
        operand.m_kind = Operand::KIND_BRANCH_TARGET;
        operand.m_immediate = (int64)(address + (uint64)offset);
    } else
    {
        cString word = readWord(parser);
        ASM_CHECK(word.length() != 0, "invalid operand");
        if (!parseRegister(parser, word, type, operand))
        {
            operand.m_kind = Operand::KIND_NAME;
            operand.m_name = word;
        }
    }
    skipBlanks(parser);
    ASM_CHECK(parser.isEOS(), "unexpected text after the operand");
    if (operand.m_kind != Operand::KIND_NAME)
    {
        operand.m_name = text;
        operand.m_name.trim();
    }
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

// Return the text of a line without its comment: ';', '@' and '//' start one
cString stripComment(const cString& line)
{
    cString result(line);
    // The end of the line: the lines of the stream end with "\r\n" where endl
    // writes it (xStl on Windows)
    const character* buffer = result.getBuffer();
    for (uint i = 0; i < result.length(); i++)
        if (cChar::isNewLine(buffer[i]))
        {
            result = result.left(i);
            break;
        }
    uint position = result.find(XSTL_STRING(";"));
    if (position < result.length())
        result = result.left(position);
    position = result.find(XSTL_STRING("@"));
    if (position < result.length())
        result = result.left(position);
    position = result.find(XSTL_STRING("//"));
    if (position < result.length())
        result = result.left(position);
    result.trim();
    return result;
}

//////////////////////////////////////////////////////////////////////////
// The candidates

struct Candidate {
    const ArmEncodingEntry* m_entry;
    // The position in the search order
    uint m_order;
    // The size of the instruction in bytes
    uint m_size;
    // The entry is one of the floating point / Advanced SIMD table
    bool m_isSimd;
};

// The name of an entry: its mnemonic up to the first '.'
cString entryName(const ArmEncodingEntry* entry)
{
    cString name(entry->m_mnemonic);
    uint dot = name.find(XSTL_STRING("."));
    return (dot < name.length()) ? name.left(dot) : name;
}

// The fixed suffix of an entry: its mnemonic from the first '.'
cString entrySuffix(const ArmEncodingEntry* entry)
{
    cString name(entry->m_mnemonic);
    uint dot = name.find(XSTL_STRING("."));
    return (dot < name.length()) ? name.part(dot, name.length()) : cString();
}

class CandidateIndex {
public:
    explicit CandidateIndex(InstructionSet type)
    {
        uint order = 0;
        // T32: the 16 bit tables first, as clang prefers the narrow forms
        for (uint pass = 0; pass < 2; pass++)
        {
            uint size = (type == ArmInstructionSet::ARM_T32) ? (pass == 0 ? 2 : 4) : 4;
            if ((type != ArmInstructionSet::ARM_T32) && (pass == 1))
                break;
            const ArmEncodingEntry* const* tables = ArmStreamDisassembler::getTables(type, size);
            for (; *tables != NULL; tables++)
            {
                bool isSimd = (*tables == gArmA32SimdEncodingTable);
                for (const ArmEncodingEntry* entry = *tables; entry->m_mnemonic != NULL; entry++)
                {
                    if ((entry->m_flags & (ARM_ENTRY_DECODE_ONLY)) != 0)
                        continue;
                    Candidate candidate;
                    candidate.m_entry = entry;
                    candidate.m_order = order++;
                    candidate.m_size = size;
                    candidate.m_isSimd = isSimd;
                    cString name = entryName(entry);
                    if (!m_entries.hasKey(name))
                        m_entries.append(name, cArray<Candidate>());
                    m_entries[name].append(candidate);
                }
            }
        }
    }

    // Collect the candidates whose name starts the head of the mnemonic
    void find(const cString& head, cArray<Candidate>& result) const
    {
        for (uint length = head.length(); length >= 1; length--)
        {
            cString name = head.left(length);
            if (!m_entries.hasKey(name))
                continue;
            const cArray<Candidate>& found = m_entries[name];
            for (uint i = 0; i < found.getSize(); i++)
            {
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

const CandidateIndex& getIndex(InstructionSet type)
{
    if (type == ArmInstructionSet::ARM_T32)
    {
        static const CandidateIndex index(ArmInstructionSet::ARM_T32);
        return index;
    }
    static const CandidateIndex index(ArmInstructionSet::ARM_A32);
    return index;
}

// The T32 form of an A32 floating point / Advanced SIMD encoding
uint32 translateA32ToT32(uint32 encoding)
{
    switch (encoding >> 24)
    {
    case 0xF2: return (encoding & 0x00FFFFFF) | 0xEF000000;
    case 0xF3: return (encoding & 0x00FFFFFF) | 0xFF000000;
    case 0xF4: return (encoding & 0x00FFFFFF) | 0xF9000000;
    default:
        break;
    }
    // The floating point: the condition 1110 stays
    return encoding;
}

// True if the registers of an operand which was written are the registers which
// the bits decode to (a field which is encoded twice, a tied register, must
// have the same register in both places)
bool isSameRegisters(const Operand& written, const Operand& decoded)
{
    bool isRegisterWritten = (written.m_kind == Operand::KIND_REGISTER) ||
                             (written.m_kind == Operand::KIND_SHIFTED_REGISTER) ||
                             (written.m_kind == Operand::KIND_REGISTER_LIST);
    bool isRegisterDecoded = (decoded.m_kind == Operand::KIND_REGISTER) ||
                             (decoded.m_kind == Operand::KIND_SHIFTED_REGISTER) ||
                             (decoded.m_kind == Operand::KIND_REGISTER_LIST);
    if (isRegisterWritten && isRegisterDecoded)
    {
        if (written.m_register != decoded.m_register)
            return false;
        if ((written.m_kind == Operand::KIND_REGISTER_LIST) && (written.m_registerMask != decoded.m_registerMask))
            return false;
        return true;
    }
    if (written.m_kind == Operand::KIND_MEMORY)
        return (decoded.m_kind == Operand::KIND_MEMORY) && (written.m_base == decoded.m_base) &&
               (written.m_index == decoded.m_index) &&
               (written.m_postIndexRegister == decoded.m_postIndexRegister);
    return true;
}

// Match the operands of an entry from the spec 'index' on. 'used' collects
// the operands that were encoded, in the order of the specs
bool matchSpecs(const ArmEncodingEntry* entry,
                uint index,
                const cArray<Operand>& parsed,
                uint next,
                const ArmCodecContext& context,
                uint64 address,
                uint32 encoding,
                uint32& result,
                cString& label,
                Operand* slots)
{
    // Skip the parts
    while ((index < ARM_MAX_OPERAND_SPECS) && (entry->m_operands[index].m_kind != ARM_KIND_NONE) &&
           ArmOperandCodec::isMnemonicPart(entry->m_operands[index]))
        index++;
    if ((index >= ARM_MAX_OPERAND_SPECS) || (entry->m_operands[index].m_kind == ARM_KIND_NONE))
    {
        result = encoding;
        return next == parsed.getSize();
    }

    const ArmOperandSpec& spec = entry->m_operands[index];
    if (ArmOperandCodec::isHidden(spec))
    {
        uint32 copy = encoding;
        if (!ArmOperandCodec::encode(spec, Operand(), context, copy))
            return false;
        return matchSpecs(entry, index + 1, parsed, next, context, address, copy, result, label, slots);
    }
    if (next >= parsed.getSize())
        return false;

    // The alternatives of consuming the operands: one, or two for a pair of
    // registers and for the "#bits, #rot" modified immediate
    for (uint count = 1; count <= 2; count++)
    {
        if (next + count > parsed.getSize())
            break;
        Operand operand = parsed[next];
        bool isPair = (spec.m_kind == ARM_KIND_GPR) &&
                      (((spec.m_flags & ARM_A32_GPR_CLASS_MASK) == ARM_A32_GPR_PAIR_NEXT) ||
                       ((spec.m_flags & ARM_A32_GPR_CLASS_MASK) == ARM_A32_GPR_PAIR_CLASS));
        bool isModifiedImmediate = (spec.m_kind == ARM_KIND_A32_MODIFIED_IMMEDIATE);
        if (count == 2)
        {
            const Operand& second = parsed[next + 1];
            if (isPair)
            {
                if ((operand.m_kind != Operand::KIND_REGISTER) || (second.m_kind != Operand::KIND_REGISTER) ||
                    (second.m_register != operand.m_register + 1))
                    continue;
                operand.m_kind = Operand::KIND_REGISTER_LIST;
                operand.m_count = 2;
                operand.m_stride = 1;
            } else if (isModifiedImmediate)
            {
                if ((operand.m_kind != Operand::KIND_IMMEDIATE) || (second.m_kind != Operand::KIND_IMMEDIATE))
                    continue;
                operand.m_shiftAmount = (uint)second.m_immediate;
                operand.m_isExplicit = true;
            } else
            {
                continue;
            }
        } else if (isPair)
        {
            continue;
        }
        // A name where an address is: a label, left unresolved
        Operand used = operand;
        cString localLabel;
        if ((used.m_kind == Operand::KIND_NAME) && (spec.m_kind == ARM_KIND_BRANCH_TARGET))
        {
            // The offset field is left 0: the target is the pc of the instruction
            localLabel = used.m_name;
            used.m_kind = Operand::KIND_BRANCH_TARGET;
            used.m_immediate = (int64)(address + ((context.m_type == ArmInstructionSet::ARM_A32) ? 8 : 4));
        }
        uint32 copy = encoding;
        if (!ArmOperandCodec::encode(spec, used, context, copy))
            continue;
        if (matchSpecs(entry, index + 1, parsed, next + count, context, address, copy, result, label,
                       slots))
        {
            if (localLabel.length() != 0)
                label = localLabel;
            slots[index] = used;
            return true;
        }
    }
    return false;
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////

void assembleA32(ArmInstructionSet::DisassemblerTypes type,
                 const ArmFeatureSet& features,
                 const cString& line,
                 uint64 address,
                 uint itState,
                 ArmAssembledA32& result)
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

    // The operands. A shift belongs to the operand before it, and an operand
    // after a memory operand without an offset is its post index
    cArray<cString> parts;
    splitOperands(operandsText, parts);
    cArray<Operand> parsed;
    for (uint i = 0; i < parts.getSize(); i++)
    {
        if (isShiftPiece(parts[i]))
        {
            ASM_CHECK(parsed.getSize() > 0, "unexpected shift");
            Operand& previous = parsed[parsed.getSize() - 1];
            cSArray<char> ascii = parts[i].getASCIIstring();
            Parser parser(ascii.getBuffer(), ascii.getBuffer(), ascii.getSize() - 1, 1);
            Operand shift;
            parseShift(parser, readWord(parser), shift, type);
            if (previous.m_kind == Operand::KIND_REGISTER)
                previous.m_kind = Operand::KIND_SHIFTED_REGISTER;
            else
                ASM_CHECK(previous.m_kind == Operand::KIND_MEMORY, "this operand has no shift");
            previous.m_shift = shift.m_shift;
            previous.m_shiftAmount = shift.m_shiftAmount;
            previous.m_shiftRegister = shift.m_shiftRegister;
            previous.m_isExplicit = shift.m_isExplicit;
            continue;
        }
        Operand operand;
        parseOperand(parts[i], type, address, operand);
        if ((parsed.getSize() > 0) &&
            (parsed[parsed.getSize() - 1].m_kind == Operand::KIND_MEMORY) &&
            (parsed[parsed.getSize() - 1].m_addressing == Operand::ADDRESSING_OFFSET) &&
            !parsed[parsed.getSize() - 1].m_hasHash &&
            (operand.m_kind == Operand::KIND_IMMEDIATE) &&
            (operand.m_optionScheme == ARM_A32_OPT_COPROC_OPTION))
        {
            // The option of ldc: "[r1], {5}"
            Operand& memory = parsed[parsed.getSize() - 1];
            memory.m_addressing = Operand::ADDRESSING_POST_INDEX;
            memory.m_optionScheme = ARM_A32_OPT_COPROC_OPTION;
            memory.m_offset = operand.m_immediate;
            continue;
        }
        if ((parsed.getSize() > 0) &&
            (parsed[parsed.getSize() - 1].m_kind == Operand::KIND_MEMORY) &&
            (parsed[parsed.getSize() - 1].m_addressing == Operand::ADDRESSING_OFFSET) &&
            !parsed[parsed.getSize() - 1].m_hasHash &&
            (parsed[parsed.getSize() - 1].m_index == ARM_NO_REGISTER) &&
            (parsed[parsed.getSize() - 1].m_postIndexRegister == ARM_NO_REGISTER) &&
            ((operand.m_kind == Operand::KIND_IMMEDIATE) || (operand.m_kind == Operand::KIND_REGISTER)))
        {
            // [r1], #8 and [r1], r2
            Operand& memory = parsed[parsed.getSize() - 1];
            memory.m_addressing = Operand::ADDRESSING_POST_INDEX;
            if (operand.m_kind == Operand::KIND_IMMEDIATE)
            {
                memory.m_offset = operand.m_immediate < 0 ? -operand.m_immediate : operand.m_immediate;
                memory.m_isSubtract = (operand.m_immediate < 0) || operand.m_name.find(XSTL_STRING("-")) < operand.m_name.length();
                memory.m_hasHash = true;
            } else
            {
                memory.m_postIndexRegister = operand.m_register;
                memory.m_isSubtract = operand.m_isSubtract;
            }
            continue;
        }
        parsed.append(operand);
    }
    // "[r1], {5}": the option was read as a memory without the post index
    // (handled above); "ldc p14, c5, [r1], {5}"

    // The mnemonic: the head and the qualifier suffix after its first '.'
    uint dot = mnemonic.find(XSTL_STRING("."));
    cString head = (dot < mnemonic.length()) ? mnemonic.left(dot) : mnemonic;
    cString tail = (dot < mnemonic.length()) ? mnemonic.part(dot, mnemonic.length()) : cString();

    ArmCodecContext context;
    context.m_type = type;
    context.m_address = address;
    context.m_features = features;
    context.m_itState = itState;

    cArray<Candidate> candidates;
    getIndex(type).find(head, candidates);
    ASM_CHECK(candidates.getSize() != 0, "unknown instruction");

    int missingFeature = -1;
    for (uint i = 0; i < candidates.getSize(); i++)
    {
        const ArmEncodingEntry* entry = candidates[i].m_entry;
        context.m_entry = entry;
        uint32 encoding = entry->m_value;

        // The qualifiers of the size: .w / .n
        cString wanted = tail;
        bool isNarrowQualifier = false;
        if (type == ArmInstructionSet::ARM_T32)
        {
            if ((wanted.length() >= 2) && (wanted.right(2) == ".n"))
            {
                isNarrowQualifier = true;
                wanted = wanted.left(wanted.length() - 2);
            }
        }
        if (isNarrowQualifier && (candidates[i].m_size != 2))
            continue;

        // The parts of the mnemonic consume the text after the name
        cString suffix = head.part(entryName(entry).length(), head.length());
        bool isValid = true;
        for (uint j = 0; isValid && (j < ARM_MAX_OPERAND_SPECS); j++)
        {
            const ArmOperandSpec& spec = entry->m_operands[j];
            if (spec.m_kind == ARM_KIND_NONE)
                break;
            if (ArmOperandCodec::isMnemonicPart(spec))
                isValid = ArmOperandCodec::encodeMnemonicPart(spec, suffix, context, encoding);
        }
        if (!isValid || (suffix.length() != 0))
            continue;
        // The fixed suffix of the entry (the data type, ".w")
        cString fixed = entrySuffix(entry);
        if ((entry->m_flags & ARM_ENTRY_T32_WIDE) != 0)
            fixed += ".w";
        if (!(wanted == fixed))
        {
            // The wide qualifier is optional in the text
            bool isWideOptional = (type == ArmInstructionSet::ARM_T32) && (wanted.length() == 0) &&
                                  (fixed == ".w");
            if (!isWideOptional)
                continue;
        }

        cString label;
        uint32 bits = 0;
        Operand slots[ARM_MAX_OPERAND_SPECS];
        if (!matchSpecs(entry, 0, parsed, 0, context, address, encoding, bits, label, slots))
            continue;
        // The bits of the instruction: the floating point and Advanced SIMD
        // encodings of T32 are the A32 ones with another top byte
        uint size = candidates[i].m_size;
        if (candidates[i].m_isSimd && (type == ArmInstructionSet::ARM_T32))
            bits = translateA32ToT32(bits);

        // The bits have to decode to this instruction, or to an alias of it:
        // the constraints of the entries and the priority of the aliases are
        // applied by the decoder
        ArmOpcodePtr opcode = ArmStreamDisassembler::decode(type, bits, size, address,
                                                            ArmFeatureSet::all(), itState);
        if (opcode.isEmpty())
            continue;
        if ((opcode->getEntry() != entry) &&
            ((opcode->getEntry()->m_flags & (ARM_ENTRY_ALIAS | ARM_ENTRY_ASSEMBLE_ONLY)) == 0) &&
            ((entry->m_flags & ARM_ENTRY_ASSEMBLE_ONLY) == 0))
            continue;

        // The registers of the text are the registers of the bits
        if (opcode->getEntry() == entry)
        {
            uint k = 0;
            bool isSame = true;
            for (uint j = 0; isSame && (j < ARM_MAX_OPERAND_SPECS); j++)
            {
                const ArmOperandSpec& spec = entry->m_operands[j];
                if (spec.m_kind == ARM_KIND_NONE)
                    break;
                if (ArmOperandCodec::isMnemonicPart(spec) || ArmOperandCodec::isHidden(spec))
                    continue;
                if (k >= opcode->getOperandsCount())
                {
                    isSame = false;
                    break;
                }
                isSame = isSameRegisters(slots[j], opcode->getOperand(k));
                k++;
            }
            if (!isSame)
                continue;
        }

        // The instruction is valid, if its feature is on
        if (!features.has((ArmFeatures::Feature)entry->m_feature) ||
            (((entry->m_flags & ARM_ENTRY_PRE_V8) != 0) && features.has(ArmFeatures::FEAT_V8_AA32)))
        {
            if (missingFeature < 0)
                missingFeature = entry->m_feature;
            continue;
        }
        result.m_encoding = bits;
        result.m_size = size;
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

uint advanceItState(ArmInstructionSet::DisassemblerTypes type,
                    uint itState,
                    const ArmAssembledA32& assembled,
                    uint64 address,
                    const ArmFeatureSet& features)
{
    if (type != ArmInstructionSet::ARM_T32)
        return 0;
    ArmOpcodePtr opcode = ArmStreamDisassembler::decode(type, assembled.m_encoding, assembled.m_size,
                                                        address, ArmFeatureSet::all(), itState);
    if (!opcode.isEmpty())
    {
        const ArmEncodingEntry* entry = opcode->getEntry();
        for (uint i = 0; i < ARM_MAX_OPERAND_SPECS; i++)
        {
            if (entry->m_operands[i].m_kind == ARM_KIND_NONE)
                break;
            // An it instruction starts a block with its firstcond:mask
            if (entry->m_operands[i].m_kind == ARM_KIND_T16_IT_MASK)
                return assembled.m_encoding & 0xFF;
        }
    }
    if (itState == 0)
        return 0;
    return ((itState & 7) == 0) ? 0 : ((itState & 0xE0) | ((itState << 1) & 0x1F));
}

} // namespace armdis
