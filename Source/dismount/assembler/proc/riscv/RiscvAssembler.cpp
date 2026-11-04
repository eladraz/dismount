#include "dismount/dismount.h"
/*
 * RiscvAssembler.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/char.h"
#include "xStl/data/array.h"
#include "xStl/data/list.h"
#include "xStl/data/sarray.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/exceptions.h"
#include "xStl/parser/parser.h"
#include "dismount/assembler/proc/riscv/RiscvAssembler.h"
#include "dismount/assembler/proc/riscv/RiscvAssemblerInternal.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvNotation.h"
#include "dismount/proc/riscv/RiscvOperandCodec.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"

using namespace riscvdis;
using namespace riscvasm;

// Throw an exception with a message describing the assembler failure
#define ASM_CHECK(cond, msg) if (!(cond)) { XSTL_THROW(cException, XSTL_STRING(msg), EXCEPTION_FAILED); }

RiscvAssembler::RiscvAssembler(FirstPassBinaryPtr binary,
                               RiscvInstructionSet::DisassemblerTypes type,
                               const RiscvFeatureSet& features) :
    m_binary(binary),
    m_type(type),
    m_features(features)
{
}

cStringerStreamPtr RiscvAssembler::getAssembler()
{
    return cStringerStreamPtr(new RiscvAssemblerStream(*m_binary, m_type, m_features));
}

FirstPassBinaryPtr RiscvAssembler::getFirstPass()
{
    return m_binary;
}

//////////////////////////////////////////////////////////////////////////
// Text helpers. The operands are parsed by their syntax only, into
// RiscvDecodedOperand; the codec resolves the names for the spec it encodes.

namespace {

// True if a letter, a digit or one of the characters of a symbol
bool isIdentifierChar(character c)
{
    return cChar::isLetter(c) || cChar::isDigit(c) || (c == XSTL_CHAR('_')) ||
           (c == XSTL_CHAR('.')) || (c == XSTL_CHAR('$'));
}

// Return the text without its comment ('#') and blanks
cString stripComment(const cString& line)
{
    cString result(line);
    uint position = result.find(XSTL_STRING("#"));
    if (position < result.length())
        result = result.left(position);
    result.trim();
    return result;
}

// Remove the blanks of the ends
cString trimmed(const cString& text)
{
    cString result(text);
    result.trim();
    return result;
}

// Split the operands at the commas which are not inside parentheses
void splitOperands(const cString& text, cArray<cString>& parts)
{
    uint depth = 0;
    uint start = 0;
    for (uint i = 0; i <= text.length(); i++)
    {
        bool isEnd = (i == text.length());
        if (!isEnd && (text[i] == XSTL_CHAR('(')))
            depth++;
        else if (!isEnd && (text[i] == XSTL_CHAR(')')) && (depth > 0))
            depth--;
        if (isEnd || ((text[i] == XSTL_CHAR(',')) && (depth == 0)))
        {
            cString part = trimmed(text.part(start, i));
            if (part.length() != 0 || !isEnd || parts.getSize() != 0)
                parts.append(part);
            start = i + 1;
        }
    }
}

// Find a register by its name (ABI and numeric names). Return false if it
// isn't one
bool findRegister(const cString& name, uint& registerId)
{
    if (name == "fp")
    {
        registerId = RISCV_REG_X0 + 8;
        return true;
    }
    if (name.length() < 2)
        return false;
    for (uint id = RISCV_REG_X0; id <= RISCV_REG_V31; id++)
    {
        if (id == RISCV_REG_PC)
            continue;
        const char* abi = RiscvNotation::getRegisterName(id, 0);
        const char* numeric = RiscvNotation::getRegisterName(id, RiscvNotation::RISCV_FORMAT_NUMERIC);
        if ((abi != NULL && name == abi) || (numeric != NULL && name == numeric))
        {
            registerId = id;
            return true;
        }
    }
    return false;
}

// Read a number word: decimal, 0x hexadecimal, 0b binary, 0 octal (as llvm-mc)
bool parseNumberWord(const cString& word, uint64& value)
{
    uint length = word.length();
    if (length == 0 || !cChar::isDigit(word[0]))
        return false;
    uint base = 10;
    uint start = 0;
    if (length >= 2 && word[0] == XSTL_CHAR('0'))
    {
        if (word[1] == XSTL_CHAR('x'))
        {
            base = 16;
            start = 2;
        } else if (word[1] == XSTL_CHAR('b'))
        {
            base = 2;
            start = 2;
        } else
        {
            base = 8;
            start = 1;
        }
    }
    if (start >= length)
        return false;
    value = 0;
    for (uint i = start; i < length; i++)
    {
        character c = word[i];
        uint digit;
        if (c >= XSTL_CHAR('0') && c <= XSTL_CHAR('9'))
            digit = c - XSTL_CHAR('0');
        else if (c >= XSTL_CHAR('a') && c <= XSTL_CHAR('f'))
            digit = 10 + (c - XSTL_CHAR('a'));
        else
            return false;
        if (digit >= base)
            return false;
        uint64 next = value * base + digit;
        if ((next - digit) / base != value)
            return false;
        value = next;
    }
    return true;
}

/*
 * A constant expression, as llvm-mc reads them: numbers, '.' (the address of the
 * instruction: the value is the offset 0 from it), the operators + - * / % << >>
 * & | ^ ~ ! and parentheses
 */
class Expression {
public:
    Expression(const cString& text) :
        m_text(text), m_position(0), m_usesDot(false)
    {
    }

    bool evaluate(int64& value)
    {
        uint64 result;
        if (!parseOr(result))
            return false;
        skipBlanks();
        if (m_position != m_text.length())
            return false;
        value = (int64)result;
        return true;
    }

    // True if '.' is in the expression
    bool usesDot() const
    {
        return m_usesDot;
    }

private:
    void skipBlanks()
    {
        while (m_position < m_text.length() && m_text[m_position] == XSTL_CHAR(' '))
            m_position++;
    }

    // True (and consumed) if the next characters are 'token'
    bool accept(const char* token)
    {
        skipBlanks();
        uint length = 0;
        while (token[length] != 0)
            length++;
        if (m_position + length > m_text.length())
            return false;
        for (uint i = 0; i < length; i++)
            if (m_text[m_position + i] != (character)token[i])
                return false;
        // (a single '<' or '>' is not a shift)
        m_position += length;
        return true;
    }

    bool peekIs(char c)
    {
        skipBlanks();
        return m_position < m_text.length() && m_text[m_position] == (character)c;
    }

    bool parseOr(uint64& value)
    {
        if (!parseXor(value))
            return false;
        while (peekIs('|'))
        {
            m_position++;
            uint64 right;
            if (!parseXor(right))
                return false;
            value |= right;
        }
        return true;
    }

    bool parseXor(uint64& value)
    {
        if (!parseAnd(value))
            return false;
        while (peekIs('^'))
        {
            m_position++;
            uint64 right;
            if (!parseAnd(right))
                return false;
            value ^= right;
        }
        return true;
    }

    bool parseAnd(uint64& value)
    {
        if (!parseShift(value))
            return false;
        while (peekIs('&'))
        {
            m_position++;
            uint64 right;
            if (!parseShift(right))
                return false;
            value &= right;
        }
        return true;
    }

    bool parseShift(uint64& value)
    {
        if (!parseAdd(value))
            return false;
        for (;;)
        {
            uint64 right;
            if (accept("<<"))
            {
                if (!parseAdd(right) || right >= 64)
                    return false;
                value <<= right;
            } else if (accept(">>"))
            {
                if (!parseAdd(right) || right >= 64)
                    return false;
                value = (uint64)((int64)value >> right);
            } else
                return true;
        }
    }

    bool parseAdd(uint64& value)
    {
        if (!parseMul(value))
            return false;
        for (;;)
        {
            if (peekIs('+'))
            {
                m_position++;
                uint64 right;
                if (!parseMul(right))
                    return false;
                value += right;
            } else if (peekIs('-'))
            {
                m_position++;
                uint64 right;
                if (!parseMul(right))
                    return false;
                value -= right;
            } else
                return true;
        }
    }

    bool parseMul(uint64& value)
    {
        if (!parseUnary(value))
            return false;
        for (;;)
        {
            char op = 0;
            if (peekIs('*'))
                op = '*';
            else if (peekIs('/'))
                op = '/';
            else if (peekIs('%'))
                op = '%';
            else
                return true;
            m_position++;
            uint64 right;
            if (!parseUnary(right))
                return false;
            if (op == '*')
                value *= right;
            else
            {
                if (right == 0)
                    return false;
                if (op == '/')
                    value = (uint64)((int64)value / (int64)right);
                else
                    value = (uint64)((int64)value % (int64)right);
            }
        }
    }

    bool parseUnary(uint64& value)
    {
        if (peekIs('-'))
        {
            m_position++;
            if (!parseUnary(value))
                return false;
            value = (uint64)0 - value;
            return true;
        }
        if (peekIs('+'))
        {
            m_position++;
            return parseUnary(value);
        }
        if (peekIs('~'))
        {
            m_position++;
            if (!parseUnary(value))
                return false;
            value = ~value;
            return true;
        }
        if (peekIs('!'))
        {
            m_position++;
            if (!parseUnary(value))
                return false;
            value = (value == 0) ? 1 : 0;
            return true;
        }
        return parsePrimary(value);
    }

    bool parsePrimary(uint64& value)
    {
        skipBlanks();
        if (m_position >= m_text.length())
            return false;
        if (m_text[m_position] == XSTL_CHAR('('))
        {
            m_position++;
            if (!parseOr(value))
                return false;
            if (!peekIs(')'))
                return false;
            m_position++;
            return true;
        }
        if (m_text[m_position] == XSTL_CHAR('.'))
        {
            // '.' alone
            if (m_position + 1 < m_text.length() && isIdentifierChar(m_text[m_position + 1]))
                return false;
            m_position++;
            m_usesDot = true;
            value = 0;
            return true;
        }
        uint start = m_position;
        while (m_position < m_text.length() &&
               (cChar::isDigit(m_text[m_position]) || cChar::isLetter(m_text[m_position]) ||
                m_text[m_position] == XSTL_CHAR('_')))
            m_position++;
        return parseNumberWord(m_text.part(start, m_position), value);
    }

    cString m_text;
    uint m_position;
    bool m_usesDot;
};

// Try to read the whole text as a constant expression
bool parseNumber(const cString& text, int64& value, bool* usesDot = NULL)
{
    Expression expression(text);
    if (!expression.evaluate(value))
        return false;
    if (usesDot != NULL)
        *usesDot = expression.usesDot();
    return true;
}

// Parse the text of one operand into an Operand
void parseOperand(const cString& text, Operand& operand)
{
    operand = Operand();
    ASM_CHECK(text.length() != 0, "empty operand");

    // offset(base)
    if (text[text.length() - 1] == XSTL_CHAR(')'))
    {
        uint open = text.length();
        while (open > 0 && text[open - 1] != XSTL_CHAR('('))
            open--;
        uint base;
        if (open > 0 && findRegister(trimmed(text.part(open, text.length() - 1)), base) &&
            base <= RISCV_REG_X31)
        {
            cString offsetText = trimmed(text.left(open - 1));
            operand.m_kind = Operand::KIND_MEMORY;
            operand.m_base = base;
            if (offsetText.length() != 0)
            {
                int64 offset;
                ASM_CHECK(parseNumber(offsetText, offset), "invalid offset");
                operand.m_hasOffset = true;
                operand.m_offset = offset;
            }
            return;
        }
        // (an expression with parentheses: "-(1)")
    }

    uint registerId;
    if (findRegister(text, registerId))
    {
        operand.m_kind = Operand::KIND_REGISTER;
        operand.m_register = registerId;
        return;
    }
    if (text == "v0.t")
    {
        operand.m_kind = Operand::KIND_VECTOR_MASK;
        return;
    }
    int64 value;
    bool usesDot = false;
    if (parseNumber(text, value, &usesDot))
    {
        operand.m_kind = Operand::KIND_IMMEDIATE;
        operand.m_immediate = value;
        // (the offsets from '.' may be relaxed, see expandPseudo)
        if (usesDot)
            operand.m_name = cString(".");
        return;
    }
    // An identifier, or a float: the codec and the pseudo instructions
    // resolve it
    for (uint i = 0; i < text.length(); i++)
        ASM_CHECK(isIdentifierChar(text[i]) || text[i] == XSTL_CHAR('-') ||
                  text[i] == XSTL_CHAR('+') || text[i] == XSTL_CHAR(' ') ||
                  text[i] == XSTL_CHAR('%'), "invalid operand");
    operand.m_kind = Operand::KIND_NAME;
    operand.m_name = text;
}

// True if the operand is a name which can be a label
bool isLabelName(const Operand& operand)
{
    if (operand.m_kind != Operand::KIND_NAME || operand.m_name.length() == 0)
        return false;
    character first = operand.m_name[0];
    return cChar::isLetter(first) || first == XSTL_CHAR('_') || first == XSTL_CHAR('.') ||
           first == XSTL_CHAR('$');
}

RiscvCodecContext makeContext(const Target& target)
{
    RiscvCodecContext context;
    context.m_type = target.m_type;
    context.m_features = target.m_features;
    context.m_address = target.m_address;
    return context;
}

/*
 * Try to encode a mnemonic and its operands by one entry. Return false if the
 * entry is not this instruction.
 */
bool tryEntry(const Target& target, const RiscvEncodingEntry* entry, const cString& mnemonic,
              const cArray<Operand>& operands, uint size, bool skipHints,
              uint32& encoding, cString& label)
{
    if ((entry->m_flags & RISCV_ENTRY_DECODE_ONLY) != 0)
        return false;
    if (((entry->m_flags & RISCV_ENTRY_RV32_ONLY) != 0) &&
        (target.m_type != RiscvInstructionSet::RISCV_RV32))
        return false;
    if (((entry->m_flags & RISCV_ENTRY_RV64_ONLY) != 0) &&
        (target.m_type != RiscvInstructionSet::RISCV_RV64))
        return false;
    if (skipHints && ((entry->m_flags & RISCV_ENTRY_HINT) != 0))
        return false;
    // (llvm-mc reads the hint aliases and the Zicsr instructions whichever
    // features are enabled, as LLVM's decoder does)
    if ((((entry->m_flags & RISCV_ENTRY_DECODE_UNGATED) == 0) &&
         !target.m_features.has((RiscvFeatures::Feature)entry->m_feature)) ||
        !target.m_features.has((RiscvFeatures::Feature)entry->m_feature2))
        return false;

    // The base mnemonic
    cString base(entry->m_mnemonic);
    if (mnemonic.length() < base.length() || mnemonic.left(base.length()) != base)
        return false;
    cString suffix = mnemonic.part(base.length(), mnemonic.length());

    RiscvCodecContext context = makeContext(target);
    uint32 bits = entry->m_value;
    uint next = 0;
    label = cString();
    for (uint i = 0; i < RISCV_MAX_OPERAND_SPECS; i++)
    {
        const RiscvOperandSpec& spec = entry->m_operands[i];
        if (spec.m_kind == RISCV_KIND_NONE)
            break;
        if (RiscvOperandCodec::isMnemonicPart(spec))
        {
            if (!RiscvOperandCodec::encodeMnemonicPart(spec, suffix, bits))
                return false;
            continue;
        }
        Operand operand;
        if (RiscvOperandCodec::isHidden(spec))
        {
            // (no text: the codec copies the field)
        } else if (spec.m_kind == RISCV_KIND_VTYPE)
        {
            // The words of vtype are separate operands: "e32, m1, ta, ma"
            if (next >= operands.getSize())
                return false;
            if (operands[next].m_kind == Operand::KIND_NAME)
            {
                cString joined = operands[next].m_name;
                next++;
                while (next < operands.getSize() && operands[next].m_kind == Operand::KIND_NAME)
                {
                    joined += cString(",") + operands[next].m_name;
                    next++;
                }
                operand.m_kind = Operand::KIND_NAME;
                operand.m_name = joined;
            } else
            {
                operand = operands[next++];
            }
        } else if (next < operands.getSize())
        {
            operand = operands[next++];
            if ((spec.m_kind == RISCV_KIND_BRANCH_TARGET) && isLabelName(operand))
            {
                // A label: the offset is left 0 for the link
                label = operand.m_name;
                operand = Operand();
                operand.m_kind = Operand::KIND_IMMEDIATE;
            }
        } else if (!RiscvOperandCodec::isOptional(spec))
        {
            return false;
        }
        if (!RiscvOperandCodec::encode(spec, operand, context, bits))
            return false;
    }
    if (next != operands.getSize() || suffix.length() != 0)
        return false;

    // The bits must be this entry (the reserved values of the operands, the
    // features, the instruction set); the entries before it in the tables only
    // decide which of the encodings is printed
    if (!RiscvStreamDisassembler::matches(entry, target.m_type, bits, target.m_address,
                                          target.m_features))
        return false;
    encoding = bits;
    return true;
}

// Find the encoding of a mnemonic and its operands in the tables
bool findEncoding(const Target& target, const cString& mnemonic, const cArray<Operand>& operands,
                  bool isCompressed, bool skipHints, uint32& encoding, cString& label)
{
    const RiscvEncodingEntry* const* tables = RiscvStreamDisassembler::getTables(isCompressed ? 2 : 4);
    for (; *tables != NULL; tables++)
    {
        for (const RiscvEncodingEntry* entry = *tables; entry->m_mnemonic != NULL; entry++)
        {
            if (tryEntry(target, entry, mnemonic, operands, isCompressed ? 2 : 4, skipHints,
                         encoding, label))
                return true;
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////
// The compression, as llvm-mc does when a compressed feature is enabled: a
// 32 bit instruction with a compressed form with the same operands is
// emitted as the 16 bit instruction.

Operand reg(uint number)
{
    Operand operand;
    operand.m_kind = Operand::KIND_REGISTER;
    operand.m_register = RISCV_REG_X0 + number;
    return operand;
}

Operand freg(uint number)
{
    Operand operand;
    operand.m_kind = Operand::KIND_REGISTER;
    operand.m_register = RISCV_REG_F0 + number;
    return operand;
}

Operand imm(int64 value)
{
    Operand operand;
    operand.m_kind = Operand::KIND_IMMEDIATE;
    operand.m_immediate = value;
    return operand;
}

Operand mem(uint base, int64 offset)
{
    Operand operand;
    operand.m_kind = Operand::KIND_MEMORY;
    operand.m_base = RISCV_REG_X0 + base;
    operand.m_hasOffset = true;
    operand.m_offset = offset;
    return operand;
}

// The register number of a register operand (x or f), or 0xff
uint number(const Operand& operand)
{
    if (operand.m_kind != Operand::KIND_REGISTER)
        return 0xff;
    if (operand.m_register <= RISCV_REG_X31)
        return operand.m_register - RISCV_REG_X0;
    if (operand.m_register >= RISCV_REG_F0 && operand.m_register <= RISCV_REG_F31)
        return operand.m_register - RISCV_REG_F0;
    return 0xff;
}

bool isCompressedRegister(uint n)
{
    return n >= 8 && n <= 15;
}

// The offset from an instruction to a target (the absolute address of the
// decoder, wrapped to 32 bits in RV32)
int64 offsetOf(int64 target, uint64 address, bool is64)
{
    int64 offset = (int64)((uint64)target - address);
    return is64 ? offset : (int64)(int32)(uint32)offset;
}

struct Candidate {
    cString m_mnemonic;
    cArray<Operand> m_operands;
};

void addCandidate(cArray<Candidate>& list, const char* mnemonic,
                  const Operand& a, const Operand& b, const Operand& c, uint count)
{
    Candidate candidate;
    candidate.m_mnemonic = cString(mnemonic);
    if (count > 0) candidate.m_operands.append(a);
    if (count > 1) candidate.m_operands.append(b);
    if (count > 2) candidate.m_operands.append(c);
    list.append(candidate);
}

// Build the compressed forms of a 32 bit instruction (decoded without
// aliases), in the order llvm-mc tries them
void buildCompressions(const Target& target, const RiscvOpcode& w, cArray<Candidate>& list)
{
    const bool is64 = (target.m_type == RiscvInstructionSet::RISCV_RV64);
    const cString& mn = w.getMnemonic();
    const uint count = w.getOperandsCount();
    Operand none;
    if (mn == "addi" && count == 3)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1));
        int64 value = w.getOperand(2).m_immediate;
        if (rd == 0 && rs1 == 0 && value == 0)
            addCandidate(list, "c.nop", none, none, none, 0);
        if (rs1 == 2 && isCompressedRegister(rd) && value > 0)
            addCandidate(list, "c.addi4spn", reg(rd), reg(2), imm(value), 3);
        if (rd == rs1 && rd != 0 && value != 0)
            addCandidate(list, "c.addi", reg(rd), imm(value), none, 2);
        if (rd == 2 && rs1 == 2 && value != 0)
            addCandidate(list, "c.addi16sp", reg(2), imm(value), none, 2);
        if (rs1 == 0 && rd != 0)
            addCandidate(list, "c.li", reg(rd), imm(value), none, 2);
        if (value == 0 && rd != 0 && rs1 != 0)
            addCandidate(list, "c.mv", reg(rd), reg(rs1), none, 2);
        if (rd == rs1 && value == 255 && isCompressedRegister(rd))
            { /* (andi, not addi) */ }
    } else if (mn == "addiw" && count == 3)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1));
        if (rd == rs1 && rd != 0)
            addCandidate(list, "c.addiw", reg(rd), imm(w.getOperand(2).m_immediate), none, 2);
        // (addiw from zero is addi from zero)
        if (rs1 == 0 && rd != 0)
            addCandidate(list, "c.li", reg(rd), imm(w.getOperand(2).m_immediate), none, 2);
    } else if (mn == "lui" && count == 2)
    {
        uint rd = number(w.getOperand(0));
        if (rd != 0 && rd != 2)
            addCandidate(list, "c.lui", reg(rd), imm(w.getOperand(1).m_immediate), none, 2);
    } else if ((mn == "srli" || mn == "srai") && count == 3)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1));
        if (rd == rs1)
            addCandidate(list, mn == "srli" ? "c.srli" : "c.srai", reg(rd),
                         imm(w.getOperand(2).m_immediate), none, 2);
    } else if (mn == "andi" && count == 3)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1));
        if (rd == rs1)
        {
            addCandidate(list, "c.andi", reg(rd), imm(w.getOperand(2).m_immediate), none, 2);
            if (w.getOperand(2).m_immediate == 255)
                addCandidate(list, "c.zext.b", reg(rd), none, none, 1);
        }
    } else if (mn == "xori" && count == 3)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1));
        if (rd == rs1 && w.getOperand(2).m_immediate == -1)
            addCandidate(list, "c.not", reg(rd), none, none, 1);
    } else if (mn == "slli" && count == 3)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1));
        if (rd == rs1)
            addCandidate(list, "c.slli", reg(rd), imm(w.getOperand(2).m_immediate), none, 2);
    } else if ((mn == "sub" || mn == "xor" || mn == "or" || mn == "and" || mn == "subw" ||
                mn == "addw" || mn == "mul") && count == 3)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1)), rs2 = number(w.getOperand(2));
        cString name = cString("c.") + mn;
        bool commutative = !(mn == "sub" || mn == "subw");
        if (rd == rs1)
            addCandidate(list, name.getASCIIstring().getBuffer(), reg(rd), reg(rs2), none, 2);
        else if (rd == rs2 && commutative)
            addCandidate(list, name.getASCIIstring().getBuffer(), reg(rd), reg(rs1), none, 2);
    } else if (mn == "add" && count == 3)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1)), rs2 = number(w.getOperand(2));
        if (rd == 0 && rs1 == 0 && rs2 >= 2 && rs2 <= 5)
        {
            static const char* const ntl[] = { "c.ntl.p1", "c.ntl.pall", "c.ntl.s1", "c.ntl.all" };
            addCandidate(list, ntl[rs2 - 2], none, none, none, 0);
        }
        if (rs1 == 0)
            addCandidate(list, "c.mv", reg(rd), reg(rs2), none, 2);
        else if (rs2 == 0)
            addCandidate(list, "c.mv", reg(rd), reg(rs1), none, 2);
        if (rd == rs1)
            addCandidate(list, "c.add", reg(rd), reg(rs2), none, 2);
        else if (rd == rs2)
            addCandidate(list, "c.add", reg(rd), reg(rs1), none, 2);
    } else if (mn == "jal" && count == 2)
    {
        uint rd = number(w.getOperand(0));
        Operand target0 = w.getOperand(1);
        // (the offset: the branch target is the absolute address, which the
        // decoder computed with the address of the instruction)
        target0 = imm(offsetOf(target0.m_immediate, w.getAddress(), is64));
        if (rd == 0)
            addCandidate(list, "c.j", target0, none, none, 1);
        else if (rd == 1 && !is64)
            addCandidate(list, "c.jal", target0, none, none, 1);
    } else if ((mn == "beq" || mn == "bne") && count == 3)
    {
        uint rs1 = number(w.getOperand(0)), rs2 = number(w.getOperand(1));
        Operand target0 = imm(offsetOf(w.getOperand(2).m_immediate, w.getAddress(), is64));
        if (rs2 == 0)
            addCandidate(list, mn == "beq" ? "c.beqz" : "c.bnez", reg(rs1), target0, none, 2);
        else if (rs1 == 0)
            addCandidate(list, mn == "beq" ? "c.beqz" : "c.bnez", reg(rs2), target0, none, 2);
    } else if (mn == "jalr" && count == 2)
    {
        uint rd = number(w.getOperand(0));
        const Operand& m = w.getOperand(1);
        uint rs1 = m.m_base - RISCV_REG_X0;
        if (m.m_offset == 0 && rs1 != 0)
        {
            if (rd == 0)
                addCandidate(list, "c.jr", reg(rs1), none, none, 1);
            else if (rd == 1)
                addCandidate(list, "c.jalr", reg(rs1), none, none, 1);
        }
    } else if (mn == "sspush" && count == 1 && number(w.getOperand(0)) == 1)
    {
        addCandidate(list, "c.sspush", reg(1), none, none, 1);
    } else if (mn == "sspopchk" && count == 1 && number(w.getOperand(0)) == 5)
    {
        addCandidate(list, "c.sspopchk", reg(5), none, none, 1);
    } else if (mn == "unimp")
    {
        addCandidate(list, "c.unimp", none, none, none, 0);
    } else if (mn == "ebreak")
    {
        addCandidate(list, "c.ebreak", none, none, none, 0);
    } else if ((mn == "lw" || mn == "ld" || mn == "flw" || mn == "fld" ||
                mn == "lbu" || mn == "lhu" || mn == "lh") && count == 2)
    {
        uint rd = number(w.getOperand(0));
        const Operand& m = w.getOperand(1);
        uint base = m.m_base - RISCV_REG_X0;
        bool isFloat = (mn == "flw" || mn == "fld");
        Operand dest = isFloat ? freg(rd) : reg(rd);
        if (base == 2 && (isFloat || rd != 0) && (mn == "lw" || mn == "ld" || isFloat))
        {
            cString name = cString("c.") + mn + "sp";
            addCandidate(list, name.getASCIIstring().getBuffer(), dest, mem(2, m.m_offset), none, 2);
        }
        if (isCompressedRegister(base) && isCompressedRegister(rd))
        {
            cString name = cString("c.") + mn;
            addCandidate(list, name.getASCIIstring().getBuffer(), dest, mem(base, m.m_offset), none, 2);
        }
    } else if ((mn == "sw" || mn == "sd" || mn == "fsw" || mn == "fsd" ||
                mn == "sb" || mn == "sh") && count == 2)
    {
        uint rs2 = number(w.getOperand(0));
        const Operand& m = w.getOperand(1);
        uint base = m.m_base - RISCV_REG_X0;
        bool isFloat = (mn == "fsw" || mn == "fsd");
        Operand source = isFloat ? freg(rs2) : reg(rs2);
        if (base == 2 && (mn == "sw" || mn == "sd" || isFloat))
        {
            cString name = cString("c.") + mn + "sp";
            addCandidate(list, name.getASCIIstring().getBuffer(), source, mem(2, m.m_offset), none, 2);
        }
        if (isCompressedRegister(base) && isCompressedRegister(rs2))
        {
            cString name = cString("c.") + mn;
            addCandidate(list, name.getASCIIstring().getBuffer(), source, mem(base, m.m_offset), none, 2);
        }
    } else if ((mn == "sext.b" || mn == "sext.h" || mn == "zext.h" || mn == "zext.w") && count == 2)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1));
        if (rd == rs1)
        {
            cString name = cString("c.") + mn;
            addCandidate(list, name.getASCIIstring().getBuffer(), reg(rd), none, none, 1);
        }
    } else if ((mn == "pack" || mn == "packw" || mn == "add.uw") && count == 3)
    {
        uint rd = number(w.getOperand(0)), rs1 = number(w.getOperand(1)), rs2 = number(w.getOperand(2));
        if (rs2 == 0 && rd == rs1)
        {
            if (mn == "add.uw")
                addCandidate(list, "c.zext.w", reg(rd), none, none, 1);
            else if ((mn == "pack") != is64)
                // (zext.h is pack in RV32 and packw in RV64)
                addCandidate(list, "c.zext.h", reg(rd), none, none, 1);
        }
    }
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////

namespace riscvasm {

bool assembleOperands(const Target& target, const cString& mnemonic,
                      const cArray<Operand>& operands, bool allowCompression, Unit& unit)
{
    bool isCompressed = (mnemonic.length() > 2) && (mnemonic.left(2) == "c.");
    uint32 encoding = 0;
    cString label;
    if (!findEncoding(target, mnemonic, operands, isCompressed, false, encoding, label))
        return false;
    unit.m_encoding = encoding;
    unit.m_size = isCompressed ? 2 : 4;
    unit.m_label = label;
    unit.m_labelSpan = unit.m_size;
    if (isCompressed || !allowCompression || !target.m_isCompressing ||
        !target.m_features.has(RiscvFeatures::FEAT_ZCA) || label.length() != 0)
        return true;

    // The compression
    RiscvOpcodePtr wide = RiscvStreamDisassembler::decode(target.m_type, encoding, 4,
                                                          target.m_address, target.m_features,
                                                          false);
    if (wide.isEmpty())
        return true;
    cArray<Candidate> candidates;
    buildCompressions(target, *wide, candidates);
    for (uint i = 0; i < candidates.getSize(); i++)
    {
        uint32 compressed;
        cString ignored;
        if (findEncoding(target, candidates[i].m_mnemonic, candidates[i].m_operands, true, true,
                         compressed, ignored))
        {
            unit.m_encoding = compressed;
            unit.m_size = 2;
            unit.m_labelSpan = 2;
            return true;
        }
    }
    return true;
}

} // namespace riscvasm

//////////////////////////////////////////////////////////////////////////
// RiscvAssemblerStream

namespace {

// True if the text has an uppercase letter
bool hasUppercase(const cString& text)
{
    for (uint i = 0; i < text.length(); i++)
        if (text[i] >= XSTL_CHAR('A') && text[i] <= XSTL_CHAR('Z'))
            return true;
    return false;
}

// Assemble one statement (no ';') into units
void assembleSingle(const Target& target, const cString& line, cArray<Unit>& units)
{
    cString original = trimmed(line);
    ASM_CHECK(original.length() != 0, "empty instruction");
    // (llvm-mc reads 0X10 and the uppercase compressed instructions with their
    // registers as errors; the rounding modes and the fence sets in lowercase only)
    ASM_CHECK(original.find(XSTL_STRING("0X")) >= original.length(), "invalid number");
    cString text(original);
    text.makeLower();

    // The mnemonic is the first word
    uint split = 0;
    while ((split < text.length()) && (text[split] != XSTL_CHAR(' ')) &&
           (text[split] != XSTL_CHAR('\t')))
        split++;
    cString mnemonic = text.left(split);
    cString operandsText = trimmed(text.part(split, text.length()));
    ASM_CHECK(!(mnemonic.length() > 2 && mnemonic.left(2) == "c." &&
                hasUppercase(trimmed(original.part(split, original.length())))),
              "invalid instruction");

    cArray<cString> parts;
    splitOperands(operandsText, parts);
    cArray<cString> originalParts;
    splitOperands(trimmed(original.part(split, original.length())), originalParts);
    cArray<Operand> operands;
    for (uint i = 0; i < parts.getSize(); i++)
    {
        Operand operand;
        parseOperand(parts[i], operand);
        // (an identifier written with uppercase letters: see the codec)
        if (operand.m_kind == Operand::KIND_NAME && i < originalParts.getSize() &&
            hasUppercase(originalParts[i]))
            operand.m_immediate = 1;
        // RV32: 0x80000000-0xffffffff are the negative values
        if (target.m_type == RiscvInstructionSet::RISCV_RV32)
        {
            if (operand.m_kind == Operand::KIND_IMMEDIATE && operand.m_immediate >= ((int64)1 << 31) &&
                operand.m_immediate < ((int64)1 << 32))
                operand.m_immediate -= (int64)1 << 32;
            if (operand.m_kind == Operand::KIND_MEMORY && operand.m_offset >= ((int64)1 << 31) &&
                operand.m_offset < ((int64)1 << 32))
                operand.m_offset -= (int64)1 << 32;
        }
        operands.append(operand);
    }

    if (expandPseudo(target, mnemonic, operands, units))
        return;
    Unit unit;
    ASM_CHECK(assembleOperands(target, mnemonic, operands, true, unit),
              "invalid instruction or operands");
    units.append(unit);
}

// Assemble one line: statements separated by ';'
void assembleStatement(const Target& target, const cString& line, cArray<Unit>& units)
{
    cString text = stripComment(line);
    ASM_CHECK(text.length() != 0, "empty instruction");
    uint start = 0;
    for (uint i = 0; i <= text.length(); i++)
    {
        if (i != text.length() && text[i] != XSTL_CHAR(';'))
            continue;
        cString part = trimmed(text.part(start, i));
        start = i + 1;
        // (an empty statement is fine)
        if (part.length() == 0)
            continue;
        Target current = target;
        for (uint k = 0; k < units.getSize(); k++)
            current.m_address += units[k].m_size;
        assembleSingle(current, part, units);
    }
}

} // anonymous namespace

RiscvAssemblerStream::RiscvAssemblerStream(FirstPassBinary& binary,
                                           RiscvInstructionSet::DisassemblerTypes type,
                                           const RiscvFeatureSet& features) :
    m_binary(binary),
    m_type(type),
    m_features(features),
    m_isCompressing(true)
{
}

RiscvAssemblerStream::~RiscvAssemblerStream()
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

void RiscvAssemblerStream::setCompression(bool isCompressing)
{
    m_isCompressing = isCompressing;
}

void RiscvAssemblerStream::assemble()
{
    if (getData().length() == 0)
        return;

    // Each line holds a single statement
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

void RiscvAssemblerStream::assembleLine(const cString& line)
{
    Target target;
    target.m_type = m_type;
    target.m_features = m_features;
    target.m_isCompressing = m_isCompressing;
    // The address of an instruction in a binary is the offset in its block
    target.m_address = m_binary.getCurrentBlockData().getSize();

    cArray<Unit> units;
    assembleStatement(target, line, units);
    // (the pseudo instructions of more than one instruction are assembled at
    // the addresses which follow)
    for (uint i = 0; i < units.getSize(); i++)
    {
        const Unit& unit = units[i];
        uint position = m_binary.getCurrentBlockData().getSize();
        for (uint j = 0; j < unit.m_size; j++)
            m_binary.appendUint8((uint8)(unit.m_encoding >> (8 * j)));
        // A reference to a name: the offset is left 0 and the name is a
        // dependency of the binary
        if (unit.m_label.length() != 0)
            m_binary.getCurrentDependecies().addDependency(
                unit.m_label, position, unit.m_labelSpan, BinaryDependencies::DEP_RELATIVE, 0);
    }
}

void RiscvAssemblerStream::assembleInstruction(RiscvInstructionSet::DisassemblerTypes type,
                                               const RiscvFeatureSet& features,
                                               const cString& line,
                                               uint64 address,
                                               bool isCompressing,
                                               uint32& encoding,
                                               uint& size)
{
    Target target;
    target.m_type = type;
    target.m_features = features;
    target.m_address = address;
    target.m_isCompressing = isCompressing;
    cArray<Unit> units;
    assembleStatement(target, line, units);
    ASM_CHECK(units.getSize() == 1, "a pseudo instruction of more than one instruction");
    encoding = units[0].m_encoding;
    size = units[0].m_size;
}
