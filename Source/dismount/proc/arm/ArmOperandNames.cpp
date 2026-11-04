#include "dismount/dismount.h"
/*
 * ArmOperandNames.cpp
 *
 * Implementation file. The names of the registers, the conditions, the
 * options and the system registers (see ArmOperandCodec.h).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"

namespace armdis {

namespace {

// "p0", "p1"... for a prefix
#define NAMES_8(p)  p"0", p"1", p"2", p"3", p"4", p"5", p"6", p"7"
#define NAMES_16(p) NAMES_8(p), p"8", p"9", p"10", p"11", p"12", p"13", p"14", p"15"
#define NAMES_32(p) NAMES_16(p), p"16", p"17", p"18", p"19", p"20", p"21", p"22", \
                    p"23", p"24", p"25", p"26", p"27", p"28", p"29", p"30", p"31"

const char* const gX[] = { NAMES_32("x") };
const char* const gW[] = { NAMES_32("w") };
const char* const gZ[] = { NAMES_32("z") };
const char* const gV[] = { NAMES_32("v") };
const char* const gQ[] = { NAMES_32("q") };
const char* const gD[] = { NAMES_32("d") };
const char* const gS[] = { NAMES_32("s") };
const char* const gH[] = { NAMES_32("h") };
const char* const gB[] = { NAMES_32("b") };
const char* const gP[] = { NAMES_16("p") };
const char* const gR[] = { NAMES_8("r"), "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc" };

const char* const gConditions[] = {
    "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"
};

// A range of ids with a table of names
struct NameRange {
    uint m_first;
    uint m_count;
    const char* const* m_names;
};

const NameRange gA64Ranges[] = {
    { ARM64_REG_X0,  31, gX },
    { ARM64_REG_W0,  31, gW },
    { ARM64_REG_Z0,  32, gZ },
    { ARM64_REG_V0,  32, gV },
    { ARM64_REG_Q0,  32, gQ },
    { ARM64_REG_D0,  32, gD },
    { ARM64_REG_S0,  32, gS },
    { ARM64_REG_H0,  32, gH },
    { ARM64_REG_B0,  32, gB },
    { ARM64_REG_P0,  16, gP }
};

const NameRange gA32Ranges[] = {
    { ARM_REG_R0,  16, gR },
    { ARM_REG_Q0,  16, gQ },
    { ARM_REG_D0,  32, gD },
    { ARM_REG_S0,  32, gS }
};

// The registers with a single name
struct SingleName {
    uint m_id;
    const char* m_name;
};

const SingleName gA64Singles[] = {
    { ARM64_REG_SP, "sp" }, { ARM64_REG_XZR, "xzr" }, { ARM64_REG_WSP, "wsp" },
    { ARM64_REG_WZR, "wzr" }, { ARM64_REG_PC, "pc" },
    { ARM64_REG_FFR, "ffr" }, { ARM64_REG_ZA_ROW0, "za" }, { ARM64_REG_ZT0, "zt0" }
    // (the system registers fpcr, fpsr, tpidr_el0 and svcr are names of mrs /
    // msr, not registers of the syntax)
};

const SingleName gA32Singles[] = {
    { ARM_REG_FPSCR, "fpscr" }, { ARM_REG_FPEXC, "fpexc" },
    { ARM_REG_FPSID, "fpsid" }, { ARM_REG_MVFR0, "mvfr0" }, { ARM_REG_MVFR1, "mvfr1" },
    { ARM_REG_MVFR2, "mvfr2" }
};

// Compare two strings without the case. 'lower' is a lowercase C string
bool equalsLower(const cString& text, const char* lower)
{
    cString other(lower);
    return text.icompare(other) == cString::EqualTo;
}

} // anonymous namespace

const char* getRegisterName(ArmInstructionSet::DisassemblerTypes type, uint registerId)
{
    bool isA64 = (type == ArmInstructionSet::ARM_A64);
    const NameRange* ranges = isA64 ? gA64Ranges : gA32Ranges;
    uint rangesCount = isA64 ? (sizeof(gA64Ranges) / sizeof(gA64Ranges[0])) :
                               (sizeof(gA32Ranges) / sizeof(gA32Ranges[0]));
    for (uint i = 0; i < rangesCount; i++)
        if ((registerId >= ranges[i].m_first) &&
            (registerId < ranges[i].m_first + ranges[i].m_count))
            return ranges[i].m_names[registerId - ranges[i].m_first];

    const SingleName* singles = isA64 ? gA64Singles : gA32Singles;
    uint singlesCount = isA64 ? (sizeof(gA64Singles) / sizeof(gA64Singles[0])) :
                                (sizeof(gA32Singles) / sizeof(gA32Singles[0]));
    for (uint i = 0; i < singlesCount; i++)
        if (singles[i].m_id == registerId)
            return singles[i].m_name;
    return NULL;
}

bool findRegister(ArmInstructionSet::DisassemblerTypes type,
                  const cString& name,
                  uint& registerId)
{
    cString text(name);
    text.makeLower();
    bool isA64 = (type == ArmInstructionSet::ARM_A64);

    // The single names, and the aliases
    const SingleName* singles = isA64 ? gA64Singles : gA32Singles;
    uint singlesCount = isA64 ? (sizeof(gA64Singles) / sizeof(gA64Singles[0])) :
                                (sizeof(gA32Singles) / sizeof(gA32Singles[0]));
    for (uint i = 0; i < singlesCount; i++)
        if (text == singles[i].m_name)
        {
            registerId = singles[i].m_id;
            return true;
        }
    if (isA64)
    {
        if (text == "fp")
        {
            registerId = ARM64_REG_X0 + 29;
            return true;
        }
        if (text == "lr")
        {
            registerId = ARM64_REG_X0 + 30;
            return true;
        }
        uint number;
        // The predicate as counter is the predicate register
        if (parseNumberedName(text, "pn", 15, number))
        {
            registerId = ARM64_REG_P0 + number;
            return true;
        }
    } else
    {
        static const struct { const char* m_name; uint m_number; } aliases[] = {
            { "sp", 13 }, { "lr", 14 }, { "pc", 15 }, { "ip", 12 }, { "fp", 11 },
            { "sl", 10 }, { "sb", 9 }
        };
        for (uint i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
            if (text == aliases[i].m_name)
            {
                registerId = ARM_REG_R0 + aliases[i].m_number;
                return true;
            }
    }

    // The numbered registers
    const NameRange* ranges = isA64 ? gA64Ranges : gA32Ranges;
    uint rangesCount = isA64 ? (sizeof(gA64Ranges) / sizeof(gA64Ranges[0])) :
                               (sizeof(gA32Ranges) / sizeof(gA32Ranges[0]));
    for (uint i = 0; i < rangesCount; i++)
    {
        // The prefix of the table is the first character of its first name
        const char prefix[2] = { ranges[i].m_names[0][0], 0 };
        uint number;
        if (parseNumberedName(text, prefix, ranges[i].m_count - 1, number))
        {
            // x31 and w31 are not registers: sp / xzr
            if (isA64 && (number == 31) &&
                ((ranges[i].m_first == ARM64_REG_X0) || (ranges[i].m_first == ARM64_REG_W0)))
                continue;
            registerId = ranges[i].m_first + number;
            return true;
        }
    }
    return false;
}

const char* getConditionName(uint condition)
{
    CHECK(condition < 16);
    return gConditions[condition];
}

bool findCondition(const cString& name, uint& condition)
{
    cString text(name);
    text.makeLower();
    for (uint i = 0; i < 16; i++)
        if (text == gConditions[i])
        {
            condition = i;
            return true;
        }
    if (text == "cs")
    {
        condition = 2;
        return true;
    }
    if (text == "cc")
    {
        condition = 3;
        return true;
    }
    return false;
}

const char* getOptionName(uint scheme, uint value)
{
    if (scheme >= ARM_OPT_SCHEMES_COUNT)
        return NULL;
    const ArmNamedValue* names = gArmOptionNames[scheme];
    if (names == NULL)
        return NULL;
    for (; names->m_name != NULL; names++)
        if (names->m_value == value)
            return names->m_name;
    return NULL;
}

bool optionTakesRegister(uint scheme, uint value)
{
    if (scheme >= ARM_OPT_SCHEMES_COUNT)
        return true;
    const ArmNamedValue* names = gArmOptionNames[scheme];
    if (names == NULL)
        return true;
    for (; names->m_name != NULL; names++)
        if (names->m_value == value)
            return (names->m_flags & ARM_NAME_NO_REGISTER) == 0;
    return true;
}

bool findOption(uint scheme, const cString& name, uint& value)
{
    if (scheme >= ARM_OPT_SCHEMES_COUNT)
        return false;
    const ArmNamedValue* names = gArmOptionNames[scheme];
    if (names == NULL)
        return false;
    for (; names->m_name != NULL; names++)
        if (equalsLower(name, names->m_name))
        {
            value = names->m_value;
            return true;
        }
    return false;
}

const char* getSystemRegisterName(uint32 packed, bool isWrite)
{
    // Binary search, the table is sorted by the packed value
    uint low = 0;
    uint high = gArmSystemRegistersCount;
    while (low < high)
    {
        uint middle = (low + high) / 2;
        if (gArmSystemRegisters[middle].m_packed < packed)
            low = middle + 1;
        else
            high = middle;
    }
    // The read and the write of a register can have different names
    for (; (low < gArmSystemRegistersCount) && (gArmSystemRegisters[low].m_packed == packed); low++)
        if ((gArmSystemRegisters[low].m_access & (isWrite ? ARM_SYSREG_WRITE : ARM_SYSREG_READ)) != 0)
            return gArmSystemRegisters[low].m_name;
    return NULL;
}

bool findSystemRegister(const cString& name, bool isWrite, uint32& packed)
{
    for (uint i = 0; i < gArmSystemRegistersCount; i++)
        if (equalsLower(name, gArmSystemRegisters[i].m_name) &&
            ((gArmSystemRegisters[i].m_access & (isWrite ? ARM_SYSREG_WRITE : ARM_SYSREG_READ)) != 0))
        {
            packed = gArmSystemRegisters[i].m_packed;
            return true;
        }

    // The generic name s<op0>_<op1>_c<n>_c<m>_<op2>
    cString text(name);
    text.makeLower();
    if ((text.length() < 10) || (text[0] != XSTL_CHAR('s')))
        return false;
    uint values[5];
    uint position = 1;
    for (uint field = 0; field < 5; field++)
    {
        // The "c" before CRn and CRm
        if ((field == 2) || (field == 3))
        {
            if ((position >= text.length()) || (text[position] != XSTL_CHAR('c')))
                return false;
            position++;
        }
        uint digits = 0;
        uint value = 0;
        while ((position < text.length()) && (text[position] >= XSTL_CHAR('0')) &&
               (text[position] <= XSTL_CHAR('9')) && (digits < 3))
        {
            value = value * 10 + (text[position] - XSTL_CHAR('0'));
            position++;
            digits++;
        }
        if (digits == 0)
            return false;
        values[field] = value;
        if (field < 4)
        {
            if ((position >= text.length()) || (text[position] != XSTL_CHAR('_')))
                return false;
            position++;
        }
    }
    if ((position != text.length()) || (values[0] > 3) || (values[1] > 7) ||
        (values[2] > 15) || (values[3] > 15) || (values[4] > 7))
        return false;
    packed = (values[0] << 14) | (values[1] << 11) | (values[2] << 7) |
             (values[3] << 3) | values[4];
    return true;
}

} // namespace armdis
