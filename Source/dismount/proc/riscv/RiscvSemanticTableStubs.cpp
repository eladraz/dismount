/*
 * RiscvSemanticTableStubs.cpp
 *
 * Implementation file
 *
 * The empty semantic table of the vector RISC-V instructions (see
 * RiscvSemantics.h): their mnemonics are lifted to OP_UNDEFINED until the
 * table is written. The scalar table is in RiscvSemanticTable.cpp and the
 * privileged one in RiscvSemanticTablePrivileged.cpp.
 *
 * The author of RiscvSemanticTableVector.cpp removes the table from this
 * file (and the file).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/proc/riscv/RiscvSemantics.h"

namespace riscvdis {

const RiscvSemanticEntry gRiscvVectorSemanticTable[] =
{
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0, 0 }
};

} // namespace riscvdis
