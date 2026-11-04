/*
 * RiscvSemanticTablePrivileged.cpp
 *
 * Implementation file
 *
 * The semantic table of the privileged RISC-V instructions (see
 * RiscvSemantics.h): mret, sret, wfi, sfence.vma, the Svinval and the
 * hypervisor extension instructions. They work on the supervisor and machine
 * state, which the description doesn't have: each is lifted to the helper
 * RISCV_HELPER_PRIVILEGED with its encoding (RiscvSemanticInternal.h,
 * liftPrivileged), which the embedder implements. They can't run in user
 * mode, so no reference processor verifies them (tests/riscv/semantics
 * TEST_PLAN.md).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"

namespace riscvdis {

namespace {

#define P_(x) OpcodeClassification::PROPERTY_##x

// An entry: the mnemonic, the category and the memory property
#define RVP(name, category, memory) \
    { RiscvSemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, OpcodeClassification::CATEGORY_##category, \
        OpcodeClassification::UNIT_SYSTEM, \
        P_(PRIVILEGED) | P_(NEEDS_HELPER) | (memory), 0, 0, 0, 0 }, \
      liftPrivileged, 0, 0, 0, 0, 0 }

} // namespace

const RiscvSemanticEntry gRiscvPrivilegedSemanticTable[] =
{
    RVP("mret", SYSTEM, 0),
    RVP("sret", SYSTEM, 0),
    RVP("wfi", SYSTEM, 0),
    RVP("sfence.vma", SYSTEM, 0),
    RVP("sinval.vma", SYSTEM, 0),
    RVP("sfence.w.inval", SYSTEM, 0),
    RVP("sfence.inval.ir", SYSTEM, 0),
    RVP("hfence.vvma", SYSTEM, 0),
    RVP("hfence.gvma", SYSTEM, 0),
    RVP("hinval.vvma", SYSTEM, 0),
    RVP("hinval.gvma", SYSTEM, 0),
    RVP("hlv.b", LOAD, P_(READS_MEMORY)),
    RVP("hlv.bu", LOAD, P_(READS_MEMORY)),
    RVP("hlv.h", LOAD, P_(READS_MEMORY)),
    RVP("hlv.hu", LOAD, P_(READS_MEMORY)),
    RVP("hlvx.hu", LOAD, P_(READS_MEMORY)),
    RVP("hlv.w", LOAD, P_(READS_MEMORY)),
    RVP("hlvx.wu", LOAD, P_(READS_MEMORY)),
    RVP("hlv.wu", LOAD, P_(READS_MEMORY)),
    RVP("hlv.d", LOAD, P_(READS_MEMORY)),
    RVP("hsv.b", STORE, P_(WRITES_MEMORY)),
    RVP("hsv.h", STORE, P_(WRITES_MEMORY)),
    RVP("hsv.w", STORE, P_(WRITES_MEMORY)),
    RVP("hsv.d", STORE, P_(WRITES_MEMORY)),
    // The end of the table
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0, 0 }
};

} // namespace riscvdis
