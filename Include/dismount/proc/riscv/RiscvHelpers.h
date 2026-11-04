#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVHELPERS_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVHELPERS_H

/*
 * RiscvHelpers.h
 *
 * The runtime helpers of RISC-V (riscvdis::RISCV_HELPER_*, RiscvSemantics.h)
 * on the state of a SemanticInterpreter. The helper is what a translator
 * calls into the runtime for; this is the reference implementation, which
 * the tests run (tests/riscv/semantics/TEST_PLAN.md) to verify the
 * instructions which the lifter doesn't expand into IR.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticInterpreter.h"

class RiscvHelpers {
public:
    /*
     * Execute the helper the interpreter stopped at (EXIT_HELPER) on its
     * registers, and give the value the OP_HELPER returns. The caller then
     * continues with SemanticInterpreter::resume()
     *
     * interpreter - Stopped at an OP_HELPER of RISC-V
     * xlen        - 32 or 64
     * result      - Will be filled with the value of the helper
     *
     * Implemented:
     *   - RISCV_HELPER_INSTRUCTION: sm4ed and sm4ks (the S-box).
     *   - RISCV_HELPER_READ_CSR / WRITE_CSR: the counters cycle, time and
     *     instret (and their high halves, hpmcounter3-31) read as 0, seed
     *     reads as an ES16 status without entropy; there is no other CSR in
     *     user mode.
     * Not implemented (return false): the privileged instructions
     * (RISCV_HELPER_PRIVILEGED), which work on the machine and supervisor
     * state a user mode run doesn't have, and the other CSRs.
     */
    static bool execute(SemanticInterpreter& interpreter, uint xlen,
                        SemanticValue& result);

    // Return the SM4 S-box of 'input'
    static uint8 getSm4Sbox(uint8 input);
};

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVHELPERS_H
