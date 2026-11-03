/*
 * IA32SemanticTableSimd.cpp
 *
 * The semantics of the IA-32 MMX, SSE and SSE2 instructions, see IA32Semantics.h.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "dismount/proc/ia32/IA32Semantics.h"

namespace ia32dis {

const IA32SemanticEntry gIa32SimdSemanticTable[] = {
    // End marker
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }, NULL }
};

} // namespace ia32dis
