/*
 * RiscvSemanticTableStubs.cpp
 *
 * Implementation file
 *
 * The empty semantic tables of the vector and the privileged RISC-V
 * instructions (see RiscvSemantics.h): their mnemonics are lifted to
 * OP_UNDEFINED until the tables are written. The scalar table is in
 * RiscvSemanticTable.cpp.
 *
 * The agent which writes RiscvSemanticTableVector.cpp or
 * RiscvSemanticTablePrivileged.cpp removes the table from this file.
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
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0 }
};

const RiscvSemanticEntry gRiscvPrivilegedSemanticTable[] =
{
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0 }
};

} // namespace riscvdis
