#include "dismount/dismount.h"
/*
 * MangledNames.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/datastream.h"
#include "xStl/except/trace.h"
#include "xStl/parser/parser.h"
#include "dismount/assembler/MangledNames.h"

const char MangledNames::gManglingPrefix[] = "?.tba";
const char MangledNames::gMangledSeparator = '?';
const char MangledNames::gHexConcator[] = "0x";
const char MangledNames::gBasicBlockPrefix[] = "BLCK";
const char MangledNames::gRelativeRelocation[] = "REL";
const char MangledNames::gAbsoluteRelocation[] = "ABS";
const char MangledNames::gArgumentRelocation[] = "ARG";
const char MangledNames::gLocalRelocation[] = "LOC";
const char MangledNames::gTempRelocation[] = "TMP";
const char MangledNames::gLabel[] = "LBL";

cString MangledNames::getMangleLabel(const cString& name)
{
    // Return a string with the following form:
    //       ?.tba?LBL?name
    cString ret(gManglingPrefix); ret+= gMangledSeparator;
    ret+= gLabel; ret+= gMangledSeparator;
    ret+= name;
    return ret;
}

cString MangledNames::getMangleBlock(int blockID,
                                     uint length,
                                     BinaryDependencies::DependencyType type)
{
    // Return a string with the following form:
    //       ?.tba?BLCK?0x000000ID?0x00LENGTH?REL
    //       ?.tba?BLCK?0x000000ID?0x00LENGTH?ABS
    cString ret(getBlockSymbolPrefix());
    ret+= gHexConcator; ret+= HEXDWORD(blockID); ret+= gMangledSeparator;
    ret+= gHexConcator; ret+= HEXDWORD(length); ret+= gMangledSeparator;
    switch (type)
    {
    case BinaryDependencies::DEP_ABSOLUTE: ret+= gAbsoluteRelocation; break;
    case BinaryDependencies::DEP_RELATIVE: ret+= gRelativeRelocation; break;
    case BinaryDependencies::DEP_STACK_ARG: ret+= gArgumentRelocation; break;
    case BinaryDependencies::DEP_STACK_LOCAL: ret+= gLocalRelocation; break;
    case BinaryDependencies::DEP_STACK_TEMP: ret+= gTempRelocation; break;
    case BinaryDependencies::DEP_LABEL:    ret+= gLabel; break;
    default:
        // Unknown type!
        CHECK_FAIL();
    }

    return ret;
}

bool MangledNames::isBasicBlockSymbol(const cString& name)
{
    // Get fix block symbol prefix
    return name.left(getBlockSymbolPrefix().length()) == getBlockSymbolPrefix();
}

void MangledNames::readBasicBlockSymbol(const cString& name,
                                        int& blockID,
                                        uint& length,
                                        BinaryDependencies::DependencyType& type)
{
    CHECK(isBasicBlockSymbol(name));
    cSArray<char> ascii = name.part(getBlockSymbolPrefix().length(),
                                    MAX_UINT).getASCIIstring();
    Parser parser(ascii.getBuffer(), ascii.getBuffer(), ascii.getSize() - 1, 0);
    // Read the block id
    blockID = (int)(parser.readCUnsignedInteger());
    // Read separator
    CHECK(parser.readChar() == gMangledSeparator);
    // Read length
    length = parser.readCUnsignedInteger();
    // Read separator
    CHECK(parser.readChar() == gMangledSeparator);
    // Read relative
    cString data = parser.readWord();
    CHECK(parser.isEOS());
    if (data == gLabel)
    {
        type = BinaryDependencies::DEP_LABEL; return;
    } else if (data == gRelativeRelocation)
    {
        type = BinaryDependencies::DEP_RELATIVE; return;
    } else if (data == gAbsoluteRelocation)
    {
        type = BinaryDependencies::DEP_ABSOLUTE; return;
    } else if (data == gArgumentRelocation)
    {
        type = BinaryDependencies::DEP_STACK_ARG; return;
    } else if (data == gLocalRelocation)
    {
        type = BinaryDependencies::DEP_STACK_LOCAL; return;
    } else if (data == gTempRelocation)
    {
        type = BinaryDependencies::DEP_STACK_TEMP; return;
    } else
        CHECK_FAIL();
}

const cString& MangledNames::getBlockSymbolPrefix()
{
    // ?.tba?BLCK?
    static cString gBlockSymbolPrefix(cString(gManglingPrefix) +
                                      gMangledSeparator +
                                      gBasicBlockPrefix + gMangledSeparator );
    return gBlockSymbolPrefix;
}
