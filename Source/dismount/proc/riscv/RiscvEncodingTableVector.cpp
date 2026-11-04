#include "dismount/dismount.h"
/*
 * RiscvEncodingTableVector.cpp
 *
 * The encoding table of the vector instructions (see RiscvEncodingTable.h).
 * EMPTY: the vector extension and its subsets (V, Zve*, Zvfh, Zvfbf*, Zvbb,
 * Zvbc, the vector cryptography) are filled here by their owner, with the
 * kinds RISCV_KIND_VR, RISCV_KIND_VR_V0, RISCV_KIND_VMASK and
 * RISCV_KIND_VTYPE (RiscvOperandCodecVector.cpp is a minimal codec of them:
 * extend it with the constraints of RiscvVectorRegisterFlags).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/riscv/RiscvEncodingTable.h"

namespace riscvdis {

const RiscvEncodingEntry gRiscvVectorEncodingTable[] = {
    { NULL, 0, 0, { }, 0, 0, 0, NULL, 0 }
};

} // namespace riscvdis
