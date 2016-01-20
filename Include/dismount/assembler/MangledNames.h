#ifndef __TBA_DISMOUNT_ASSEMBLER_MANGLEDNAMES_H
#define __TBA_DISMOUNT_ASSEMBLER_MANGLEDNAMES_H

/*
 * MangledNames.h
 *
 * Contains helper routines which help encode and decode mangling names
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "dismount/assembler/BinaryDependencies.h"

/*
 * Contains helper routines which help encode and decode mangling names
 */
class MangledNames {
public:
    /*
     * Generate new labeling symbol
     */
    static cString getMangleLabel(const cString& name);

    /*
     * Encode a dependency of basic-block. This dependency is used it two ways:
     * 1. A symbol which links a block to another, comes with DEP_RELATIVE or
     *    DEP_ABSOULTE types.
     * 2. A label which describes a basic block. Comes with DEP_LABEL
     */
    static cString getMangleBlock(int blockID,
                                  uint length,
                                  BinaryDependencies::DependencyType type);

    /*
     * Return true if 'name' is encoded basic-block symbol
     */
    static bool isBasicBlockSymbol(const cString& name);

    /*
     * Parse a symbol according to the basic-block symbol name
     *
     * name    - The name of the symbol
     * blockID - Will be filled with the block index
     * length  - Will be filled with the length of the symbol
     * type    - Will be filled with the type of the symbol
     *
     * Throw exception if the symbol is invalid or doesn't match basic block.
     * See isBasicBlockSymbol, getMangleBlock
     */
    static void readBasicBlockSymbol(const cString& name,
                                     int& blockID,
                                     uint& length,
                                     BinaryDependencies::DependencyType& type);

private:
    /*
     * Return the basic-block prefix symbol '?.tba?BLCK?;
     */
    static const cString& getBlockSymbolPrefix();

    // The prefix for all mangling name. Stand of ?.tba, TBA mangeled names
    static const char gManglingPrefix[];
    // The separator token. Stand on ?
    static const char gMangledSeparator;
    // Hexadecimal prefix
    static const char gHexConcator[];
    // The prefix which indicate that this symbol is local and refered to the
    // current method basic-block signature
    static const char gBasicBlockPrefix[];
    // Indicate that the relocation is relative.
    static const char gRelativeRelocation[];
    // Indicate that the relocation is absolute.
    static const char gAbsoluteRelocation[];
    // Indicate that the relocation is a function argument
    static const char gArgumentRelocation[];
    // Indicate that the relocation is a local variable
    static const char gLocalRelocation[];
    // Indicate that the relocation is a temporary variable
    static const char gTempRelocation[];
    // Indicate that the symbol is a label
    static const char gLabel[];
};

#endif // __TBA_DISMOUNT_ASSEMBLER_MANGLEDNAMES_H
