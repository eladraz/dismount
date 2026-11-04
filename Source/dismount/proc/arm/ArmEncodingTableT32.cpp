#include "dismount/dismount.h"
/*
 * ArmEncodingTableT32.cpp
 *
 * The encoding table of the T32 32 bit instructions
 * (see ArmEncodingTable.h).
 *
 * TODO: The table is empty, no instruction of this group is decoded or
 * assembled yet.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/arm/ArmEncodingTable.h"

namespace armdis {

const ArmEncodingEntry gArmT32EncodingTable[] = {
    // The end of the table
    { NULL, 0, 0, {{ ARM_KIND_NONE, 0, 0, 0 }}, 0, 0, 0, NULL }
};

} // namespace armdis
