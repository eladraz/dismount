#include "dismount/dismount.h"
/*
 * Arm64EncodingTableSimd.cpp
 *
 * The encoding table of the A64 scalar floating point, Advanced SIMD and cryptographic instructions
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

const ArmEncodingEntry gArm64SimdEncodingTable[] = {
    // The end of the table
    { NULL, 0, 0, {{ ARM_KIND_NONE, 0, 0, 0 }}, 0, 0, 0, NULL }
};

} // namespace armdis
