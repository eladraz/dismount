/*
 * TestSemanticLoweringRiscv.cpp
 *
 * The differential test of the RV64GC lowering (the RV64GC counterpart
 * of TestSemanticLoweringArm64). Each row is one instruction (or the two instructions of
 * call and tail) of the mnemonic set of the RV64GC guests: the 95 assembler
 * mnemonics of Lua 5.4.7 for RV64GC (the census of the -O2 assembly) and the
 * 4 mnemonics of a private test program that the Lua census doesn't have (fcvt.lu.d fcvt.w.d mulh
 * mulw), 99 in all, plus the base instructions of their pseudo-instructions
 * (fsgnj.d for fmv.d, fsgnjn.d for fneg.d). The rows are the rows of
 * tests/riscv/RiscvSemanticRows64.inc whose mnemonic (a compressed row counts
 * under its expansion) is in the set, plus two rows for call and tail, which
 * the row table has no single instruction for. For each row:
 *
 *   1. the words are decoded and lifted for the RV64GC guest
 *      (RiscvArchitecture(RV64, 128, no CFI));
 *   2. the lifted block is run by the SemanticInterpreter, unlegalized;
 *   3. a copy is legalized for AMD64 (SemanticLegalizer), lowered with
 *      AMD64Lowering::lowerBlock() onto a state layout that mirrors the one
 *      of reforge (x1-x31 and f0-f31 at 8 bytes, the temporaries at 16 bytes),
 *      linked into executable memory with a stub for each guest label, and run
 *      natively;
 *   4. the state the two runs leave is compared: x1-x31, f0-f31, the guest
 *      memory window, and the exit (a jump to a guest label, a branch, call or
 *      return to a register). A floating point NaN compares by class: the sign
 *      and payload of a NaN are the documented deviation of the lowering.
 *
 * The inputs of a row are the setup of the corpus (when it has one) and
 * random inputs with edge values (zeros, the extremes, the NaNs and the
 * infinities). The memory rows keep their base register in the guest memory
 * window (0x80100000, 16 KiB).
 *
 * Floating point: fcsr is 0 for every case (round to nearest for the dynamic
 * rounding mode, the rounding the lowering uses), and the setup's fcsr is
 * ignored. fflags is not maintained by the lowering, so fcsr is not compared.
 *
 * A floating point row with the dynamic rounding mode (rm = 7) runs as the
 * original (the lift reads frm through fcsr) and as a copy with rm = 0 (RNE):
 * fcsr is 0, so the interpreter rounds to nearest too.
 *
 * The skipped rows, each with its reason in the output:
 *   - a CSR instruction (csrr of fcsr): fflags is not maintained by the
 *     lowering, which is documented (as FPSR on A64);
 *   - a static rounding mode the lowering refuses ("only the rounding to
 *     nearest even is implemented"; the float to integer conversions,
 *     OP_FLOAT_ROUND and the integer to float conversions take the directed
 *     modes, so those fcvt rows run), for the rows and for the RNE
 *     copies;
 *   - a setup memory byte outside the guest memory window
 *     (the lowered code has no bounds check; the window is the only memory);
 *   - a CFI row (ssp/elp in the setup): the test runs the guest without CFI.
 *
 * Output: "riscv rows: <N> run, <F> failed, <K> skipped", then one coverage
 * line per mnemonic (rows passed and skipped). Every mnemonic must have at
 * least one passing row.
 *
 * The native runs need an executable mapping and an x86-64 Linux host; the
 * other hosts only print that the rows did not run.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <stdint.h>
#define LOWERING_RISCV_NATIVE 1
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/sarray.h"
#include "xStl/data/list.h"
#include "xStl/exceptions.h"
#include "xStl/stream/ioStream.h"
#include "xStl/data/datastream.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/../../tests/tests.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticLowering.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/semantics/SemanticInterpreter.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"

using namespace riscvdis;

// The rows: { code bytes (hex, memory order), text, setup, expected }. The
// expected text is not used: the oracle is the interpreter
struct RiscvRow {
    const char* m_code;
    const char* m_text;
    const char* m_setup;
    const char* m_expected;
};
#include "riscv/RiscvSemanticRows64.inc"

namespace {

// The rows of call and tail: two instructions each (auipc and jalr through
// the same register), which the row table doesn't have as one row
const RiscvRow CALL_ROWS[] = {
    // call symbol: auipc ra,0x1; jalr ra,0x10(ra)
    { "97100000e7800001", "call symbol", "", "" },
    // tail symbol: auipc t1,0x1; jalr zero,0x10(t1)
    { "1713000067000301", "tail symbol", "", "" },
};

// The load-reserved and store-conditional rows with every aq/rl suffix.
// The rows of the table have their bases outside the window's safe range
// (the setup base must be 0x800 bytes inside the window), so these use a base
// of 0x80102400. A pair is an lr then an sc to the same address: the sc
// stores and writes 0; a lone sc has no reservation and writes 1. The
// expected text is empty (the interpreter is the oracle)
// The fused multiply-add rows: fmadd, fmsub, fnmsub and fnmadd of
// double and single, with the dynamic and the nearest even rounding; the
// setups are the cancellation (the fused product differs from the unfused),
// the NaN and the signed zeros. The expected text is empty (the interpreter)
const RiscvRow FUSED_ROWS[] = {
    { "43f5201a", "fmadd.d fa0, ft1, ft2, ft3", "f1=3ff0000000000001 f2=3fefffffffffffff f3=bff0000000000000", "" },
    { "43f5201a", "fmadd.d fa0, ft1, ft2, ft3", "f1=7ff0000000000001 f2=3ff0000000000000 f3=7ff8000000000002", "" },
    { "43f5201a", "fmadd.d fa0, ft1, ft2, ft3", "f1=0000000000000000 f2=7ff0000000000000 f3=7ff8000000000005", "" },
    { "43f5201a", "fmadd.d fa0, ft1, ft2, ft3", "f1=0000000000000000 f2=7ff0000000000000 f3=7ff0000000000001", "" },
    { "4385201a", "fmadd.d fa0, ft1, ft2, ft3, rne", "f1=3ff0000000000001 f2=3fefffffffffffff f3=bff0000000000000", "" },
    { "47f5201a", "fmsub.d fa0, ft1, ft2, ft3", "f1=7ff8000000000001 f2=3ff0000000000000 f3=7ff0000000000001", "" },
    { "4bf5201a", "fnmsub.d fa0, ft1, ft2, ft3", "f1=fff8000000000001 f2=0000000000000000 f3=3ff0000000000000", "" },
    { "4ff5201a", "fnmadd.d fa0, ft1, ft2, ft3", "f1=7ff8000000000001 f2=7ff0000000000002 f3=3ff0000000000000", "" },
    { "43f52018", "fmadd.s fa0, ft1, ft2, ft3", "f1=7f800001 f2=3f800000 f3=7fc00002", "" },
    { "43f52018", "fmadd.s fa0, ft1, ft2, ft3", "f1=3f800001 f2=3f7fffff f3=bf800000", "" },
    { "43f52018", "fmadd.s fa0, ft1, ft2, ft3", "f1=00000000 f2=7f800000 f3=7f800001", "" },
};

const RiscvRow ATOMIC_ROWS[] = {
    { "2fa50510", "lr.w a0, (a1)", "x11=80102400 x10=68d874e7cf418bc3", "" },
    { "2fa5c518", "sc.w a0, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=90ba", "" },
    { "2fa50510afa6c518", "lr.w a0, (a1) ; sc.w a3, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=dd8fd7d584e129cd x13=2a", "" },
    { "2fa50510afa6c5182fa7c518", "lr.w a0, (a1) ; sc.w a3, a2, (a1) ; sc.w a4, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=ef7c x13=fffffffffffffffe x14=2a", "" },
    { "2fa50514", "lr.w.aq a0, (a1)", "x11=80102400 x10=68d874e7cf418bc3", "" },
    { "2fa5c51c", "sc.w.aq a0, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=90ba", "" },
    { "2fa50514afa6c51c", "lr.w.aq a0, (a1) ; sc.w.aq a3, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=dd8fd7d584e129cd x13=2a", "" },
    { "2fa50514afa6c51c2fa7c51c", "lr.w.aq a0, (a1) ; sc.w.aq a3, a2, (a1) ; sc.w.aq a4, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=ef7c x13=fffffffffffffffe x14=2a", "" },
    { "2fa50512", "lr.w.rl a0, (a1)", "x11=80102400 x10=68d874e7cf418bc3", "" },
    { "2fa5c51a", "sc.w.rl a0, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=90ba", "" },
    { "2fa50512afa6c51a", "lr.w.rl a0, (a1) ; sc.w.rl a3, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=dd8fd7d584e129cd x13=2a", "" },
    { "2fa50512afa6c51a2fa7c51a", "lr.w.rl a0, (a1) ; sc.w.rl a3, a2, (a1) ; sc.w.rl a4, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=ef7c x13=fffffffffffffffe x14=2a", "" },
    { "2fa50516", "lr.w.aqrl a0, (a1)", "x11=80102400 x10=68d874e7cf418bc3", "" },
    { "2fa5c51e", "sc.w.aqrl a0, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=90ba", "" },
    { "2fa50516afa6c51e", "lr.w.aqrl a0, (a1) ; sc.w.aqrl a3, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=dd8fd7d584e129cd x13=2a", "" },
    { "2fa50516afa6c51e2fa7c51e", "lr.w.aqrl a0, (a1) ; sc.w.aqrl a3, a2, (a1) ; sc.w.aqrl a4, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=ef7c x13=fffffffffffffffe x14=2a", "" },
    { "2fb50510", "lr.d a0, (a1)", "x11=80102400 x10=68d874e7cf418bc3", "" },
    { "2fb5c518", "sc.d a0, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=90ba", "" },
    { "2fb50510afb6c518", "lr.d a0, (a1) ; sc.d a3, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=dd8fd7d584e129cd x13=2a", "" },
    { "2fb50510afb6c5182fb7c518", "lr.d a0, (a1) ; sc.d a3, a2, (a1) ; sc.d a4, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=ef7c x13=fffffffffffffffe x14=2a", "" },
    { "2fb50514", "lr.d.aq a0, (a1)", "x11=80102400 x10=68d874e7cf418bc3", "" },
    { "2fb5c51c", "sc.d.aq a0, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=90ba", "" },
    { "2fb50514afb6c51c", "lr.d.aq a0, (a1) ; sc.d.aq a3, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=dd8fd7d584e129cd x13=2a", "" },
    { "2fb50514afb6c51c2fb7c51c", "lr.d.aq a0, (a1) ; sc.d.aq a3, a2, (a1) ; sc.d.aq a4, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=ef7c x13=fffffffffffffffe x14=2a", "" },
    { "2fb50512", "lr.d.rl a0, (a1)", "x11=80102400 x10=68d874e7cf418bc3", "" },
    { "2fb5c51a", "sc.d.rl a0, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=90ba", "" },
    { "2fb50512afb6c51a", "lr.d.rl a0, (a1) ; sc.d.rl a3, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=dd8fd7d584e129cd x13=2a", "" },
    { "2fb50512afb6c51a2fb7c51a", "lr.d.rl a0, (a1) ; sc.d.rl a3, a2, (a1) ; sc.d.rl a4, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=ef7c x13=fffffffffffffffe x14=2a", "" },
    { "2fb50516", "lr.d.aqrl a0, (a1)", "x11=80102400 x10=68d874e7cf418bc3", "" },
    { "2fb5c51e", "sc.d.aqrl a0, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=90ba", "" },
    { "2fb50516afb6c51e", "lr.d.aqrl a0, (a1) ; sc.d.aqrl a3, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=dd8fd7d584e129cd x13=2a", "" },
    { "2fb50516afb6c51e2fb7c51e", "lr.d.aqrl a0, (a1) ; sc.d.aqrl a3, a2, (a1) ; sc.d.aqrl a4, a2, (a1)", "x11=80102400 x12=5e526f88c3a1d20b x10=ef7c x13=fffffffffffffffe x14=2a", "" },
};

// The mnemonics of the test: the 95 of the Lua 5.4.7 RV64GC census (the
// assembly mnemonics, call and tail included), the 4 mnemonics of
// a private test program, and the base instructions of pseudo-instructions
const char* const MNEMONICS[] = {
    // Lua 5.4.7 (95)
    "add", "addi", "addiw", "addw", "and", "andi", "auipc", "beq", "beqz",
    "bge", "bgeu", "bgez", "bgtz", "blez", "blt", "bltu", "bltz", "bne",
    "bnez", "call", "div", "divu", "fadd.d", "fclass.d", "fcvt.d.l",
    "fcvt.d.lu", "fcvt.d.s", "fcvt.d.w", "fcvt.l.d", "fcvt.s.d", "fdiv.d",
    "feq.d", "fld", "fle.d", "flt.d", "flw", "fmul.d", "fmv.d", "fmv.d.x",
    "fmv.x.d", "fneg.d", "fsd", "fsub.d", "fsw", "j", "jalr", "jr", "lb",
    "lbu", "ld", "lh", "lhu", "li", "lui", "lw", "lwu", "mul", "mulhu", "mv",
    "neg", "negw", "not", "or", "ori", "rem", "remu", "remuw", "remw", "ret",
    "sb", "sd", "seqz", "sext.w", "sgtz", "sh", "sll", "slli", "slliw",
    "sllw", "slt", "slti", "sltiu", "sltu", "snez", "sra", "srai", "sraiw",
    "srl", "srli", "srliw", "sub", "subw", "sw", "tail", "xor", "xori",
    // A private test program (4)
    "fcvt.lu.d", "fcvt.w.d", "mulh", "mulw",
    // The base instructions of the pseudo-instructions (fmv.d, fneg.d)
    "fsgnj.d", "fsgnjn.d",
    // The load-reserved and store-conditional forms with every aq/rl
    // suffix, the RV64 floating point minimum and maximum (IEEE 754-2019
    // minimumNumber/maximumNumber), the sign injection of the abs pseudo-
    // instruction and the fences
    "lr.w", "lr.w.aq", "lr.w.rl", "lr.w.aqrl",
    "lr.d", "lr.d.aq", "lr.d.rl", "lr.d.aqrl",
    "sc.w", "sc.w.aq", "sc.w.rl", "sc.w.aqrl",
    "sc.d", "sc.d.aq", "sc.d.rl", "sc.d.aqrl",
    "fmin.s", "fmin.d", "fmax.s", "fmax.d",
    "fsgnjx.d", "fence",
    // The fused multiply-add and its negated forms (FMA3 on the host)
    "fmadd.d", "fmadd.s", "fmsub.d", "fnmadd.d", "fnmsub.d",
    // The atomic memory operations of 8, 16, 32 and 64 bits with every
    // aq/rl suffix (the amo* read-modify-write and the Zacas amocas, which
    // are lowered by AMD64LoweringAtomic.cpp). amocas.q (128 bits) is not
    // in the set: the lowering refuses a 128 bit compare-exchange (rbx)
    "amoswap.w", "amoswap.w.aq", "amoswap.w.rl", "amoswap.w.aqrl",
    "amoswap.d", "amoswap.d.aq", "amoswap.d.rl", "amoswap.d.aqrl",
    "amoswap.b", "amoswap.b.aq", "amoswap.b.rl", "amoswap.b.aqrl",
    "amoswap.h", "amoswap.h.aq", "amoswap.h.rl", "amoswap.h.aqrl", "amoadd.w",
    "amoadd.w.aq", "amoadd.w.rl", "amoadd.w.aqrl", "amoadd.d", "amoadd.d.aq",
    "amoadd.d.rl", "amoadd.d.aqrl", "amoadd.b", "amoadd.b.aq", "amoadd.b.rl",
    "amoadd.b.aqrl", "amoadd.h", "amoadd.h.aq", "amoadd.h.rl",
    "amoadd.h.aqrl", "amoxor.w", "amoxor.w.aq", "amoxor.w.rl",
    "amoxor.w.aqrl", "amoxor.d", "amoxor.d.aq", "amoxor.d.rl",
    "amoxor.d.aqrl", "amoxor.b", "amoxor.b.aq", "amoxor.b.rl",
    "amoxor.b.aqrl", "amoxor.h", "amoxor.h.aq", "amoxor.h.rl",
    "amoxor.h.aqrl", "amoand.w", "amoand.w.aq", "amoand.w.rl",
    "amoand.w.aqrl", "amoand.d", "amoand.d.aq", "amoand.d.rl",
    "amoand.d.aqrl", "amoand.b", "amoand.b.aq", "amoand.b.rl",
    "amoand.b.aqrl", "amoand.h", "amoand.h.aq", "amoand.h.rl",
    "amoand.h.aqrl", "amoor.w", "amoor.w.aq", "amoor.w.rl", "amoor.w.aqrl",
    "amoor.d", "amoor.d.aq", "amoor.d.rl", "amoor.d.aqrl", "amoor.b",
    "amoor.b.aq", "amoor.b.rl", "amoor.b.aqrl", "amoor.h", "amoor.h.aq",
    "amoor.h.rl", "amoor.h.aqrl", "amomin.w", "amomin.w.aq", "amomin.w.rl",
    "amomin.w.aqrl", "amomin.d", "amomin.d.aq", "amomin.d.rl",
    "amomin.d.aqrl", "amomin.b", "amomin.b.aq", "amomin.b.rl",
    "amomin.b.aqrl", "amomin.h", "amomin.h.aq", "amomin.h.rl",
    "amomin.h.aqrl", "amomax.w", "amomax.w.aq", "amomax.w.rl",
    "amomax.w.aqrl", "amomax.d", "amomax.d.aq", "amomax.d.rl",
    "amomax.d.aqrl", "amomax.b", "amomax.b.aq", "amomax.b.rl",
    "amomax.b.aqrl", "amomax.h", "amomax.h.aq", "amomax.h.rl",
    "amomax.h.aqrl", "amominu.w", "amominu.w.aq", "amominu.w.rl",
    "amominu.w.aqrl", "amominu.d", "amominu.d.aq", "amominu.d.rl",
    "amominu.d.aqrl", "amominu.b", "amominu.b.aq", "amominu.b.rl",
    "amominu.b.aqrl", "amominu.h", "amominu.h.aq", "amominu.h.rl",
    "amominu.h.aqrl", "amomaxu.w", "amomaxu.w.aq", "amomaxu.w.rl",
    "amomaxu.w.aqrl", "amomaxu.d", "amomaxu.d.aq", "amomaxu.d.rl",
    "amomaxu.d.aqrl", "amomaxu.b", "amomaxu.b.aq", "amomaxu.b.rl",
    "amomaxu.b.aqrl", "amomaxu.h", "amomaxu.h.aq", "amomaxu.h.rl",
    "amomaxu.h.aqrl", "amocas.w", "amocas.d", "amocas.b", "amocas.b.aq",
    "amocas.b.rl", "amocas.b.aqrl", "amocas.h", "amocas.h.aq", "amocas.h.rl",
    "amocas.h.aqrl"
};
const uint MNEMONIC_COUNT = sizeof(MNEMONICS) / sizeof(MNEMONICS[0]);

// The guest state: the layout of the tests of TestSemanticLowering (x at 0,
// the program counter at 0x100, the exit reason at 0x110, the temporaries at
// 0x200, the other registers at 0x1000 + 8 * id)
class RiscvStateMap : public SemanticRegisterMap {
public:
    virtual bool getTargetRegister(uint, uint&) const { return false; }
    virtual uint getStateOffset(uint guestRegister) const
    {
        if ((guestRegister >= RISCV_REG_X0) && (guestRegister <= RISCV_REG_X31))
            return (guestRegister - RISCV_REG_X0) * 8;
        return 0x1000 + guestRegister * 8;
    }
    virtual uint getStateRegister() const { return ia32dis::AMD64_REG_R15; }
    virtual uint getTemporaryOffset(uint temporary) const
    {
        return 0x200 + temporary * 16;
    }
    virtual uint getMemoryBaseRegister() const { return ia32dis::AMD64_REG_R14; }
    virtual uint getProgramCounterOffset() const { return 0x100; }
    virtual uint getExitReasonOffset() const { return 0x110; }
};

// The guest memory window of the rows, and the base the memory rows use
const uint64 WINDOW_BASE = 0x80100000ULL;
const uint WINDOW_SIZE = 0x4000;
const uint64 WINDOW_DEFAULT_BASE = WINDOW_BASE + 0x2000;

// The guest address of the rows
const uint64 ROW_ADDRESS = 0x80001000ULL;

// The number of cases of a row: the setup (or zeros) and the random inputs
const uint CASES_PER_ROW = 8;

// The label stubs of the native code store this reason (not an exit reason of
// the lowering, which are 1-4)
const uint64 REASON_LABEL = 0x1000ULL;

// The pseudo random numbers of the tests (xorshift64, fixed seed)
uint64 gRandom = 0x9E3779B97F4A7C15ULL;
uint64 nextRandom()
{
    gRandom ^= gRandom << 13;
    gRandom ^= gRandom >> 7;
    gRandom ^= gRandom << 17;
    return gRandom;
}

// The values the random inputs take one time in four
const uint64 EDGE_VALUES[] = {
    0, 1, 2, 3, ~(uint64)0, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
    0x80000000ULL, 0x7FFFFFFFULL, 0xFFFFFFFFULL, 0x100000000ULL,
    0xFFFFFFFF80000000ULL, 0x7FF8000000000000ULL, 0xFFF0000000000000ULL,
    0x7FF0000000000001ULL, 0x3FF0000000000000ULL, 0x7FC00000ULL,
    0x7F800000ULL, 0xFF800000ULL, 0x3F800000ULL, 0xFFFFFFFFFFFF8000ULL,
    0x7FFFFFFFFFFFFFFEULL, 0xFFFFFFFF7FC00000ULL, 0xFFFFFFFFFF800000ULL
};
const uint EDGE_COUNT = sizeof(EDGE_VALUES) / sizeof(EDGE_VALUES[0]);

uint64 randomWord()
{
    const uint64 r = nextRandom();
    if ((r & 3) == 0)
        return EDGE_VALUES[(r >> 8) % EDGE_COUNT];
    return nextRandom();
}

// The value of a hex digit, or -1
int hexDigit(char c)
{
    if ((c >= '0') && (c <= '9'))
        return c - '0';
    if ((c >= 'a') && (c <= 'f'))
        return c - 'a' + 10;
    if ((c >= 'A') && (c <= 'F'))
        return c - 'A' + 10;
    return -1;
}

// The value of the hex digits of [text, end) (up to 16 digits)
uint64 parseHex64(const char* text, const char* end)
{
    uint64 value = 0;
    for (const char* p = text; p < end; p++)
        value = (value << 4) | (uint64)hexDigit(*p);
    return value;
}

// The inputs of one case: x1-x31, f0-f31 and the window
struct CaseState
{
    uint64 m_x[32];
    uint64 m_f[32];
    uint8 m_window[WINDOW_SIZE];
};

// The setup of a row, parsed from its text ("x10=ffff mem=80100100:0a0b")
struct RowSetup
{
    bool m_hasX[32];
    uint64 m_x[32];
    bool m_hasF[32];
    uint64 m_f[32];
    bool m_hasMem[WINDOW_SIZE];
    uint8 m_mem[WINDOW_SIZE];
    // A setup memory byte is outside the guest memory window
    bool m_memOutside;
    // The setup has a key of the CFI guest (ssp, elp, cfi)
    bool m_cfi;
    // The name of a key the parser doesn't know ("" when all are known)
    const char* m_unknown;
};

// Parse the setup text of a row. Return false on a key the parser doesn't know
bool parseSetup(const char* text, RowSetup& setup, int rebase, uint64 delta)
{
    memset(&setup, 0, sizeof(setup));
    setup.m_unknown = "";
    const char* p = text;
    while (*p != 0)
    {
        while (*p == ' ')
            p++;
        if (*p == 0)
            break;
        const char* end = p;
        while ((*end != 0) && (*end != ' '))
            end++;
        const char* equals = p;
        while ((equals < end) && (*equals != '='))
            equals++;
        const uint nameLength = (uint)(equals - p);
        const char* value = (equals < end) ? equals + 1 : end;
        unsigned int index = 0;
        if ((equals == end) && (nameLength == 3) && (strncmp(p, "cfi", 3) == 0))
            setup.m_cfi = true;
        else if ((equals == end))
        {
            setup.m_unknown = "a token without '='";
            return false;
        }
        else if ((nameLength == 3) && (strncmp(p, "mem", 3) == 0))
        {
            // addr:bytes[,addr:bytes...]
            const char* q = value;
            while (q < end)
            {
                uint64 address = 0;
                while ((q < end) && (*q != ':'))
                {
                    address = (address << 4) | (uint64)hexDigit(*q);
                    q++;
                }
                if (q == end)
                    break;
                q++;
                address += delta;
                while ((q < end) && (*q != ','))
                {
                    const uint64 byte = ((uint64)hexDigit(q[0]) << 4) |
                                        (uint64)hexDigit(q[1]);
                    if ((address < WINDOW_BASE) ||
                        (address - WINDOW_BASE >= WINDOW_SIZE))
                        setup.m_memOutside = true;
                    else
                    {
                        setup.m_mem[address - WINDOW_BASE] = (uint8)byte;
                        setup.m_hasMem[address - WINDOW_BASE] = true;
                    }
                    address++;
                    q += 2;
                }
                if (q < end)
                    q++;
            }
        }
        else if ((nameLength >= 2) && (p[0] == 'x') &&
                 (sscanf(p + 1, "%u", &index) == 1) && (index >= 1) && (index < 32))
        {
            setup.m_hasX[index] = true;
            setup.m_x[index] = parseHex64(value, end) +
                               ((int)index == rebase ? delta : 0);
        }
        else if ((nameLength >= 2) && (p[0] == 'f') &&
                 (sscanf(p + 1, "%u", &index) == 1) && (index < 32))
        {
            setup.m_hasF[index] = true;
            setup.m_f[index] = parseHex64(value, end);
        }
        else if (((nameLength == 4) && (strncmp(p, "fcsr", 4) == 0)) ||
                 ((nameLength == 2) && (strncmp(p, "pc", 2) == 0)))
        {
            // fcsr is 0 for every case (see the header); pc is ROW_ADDRESS
        }
        else if (((nameLength == 3) && (strncmp(p, "ssp", 3) == 0)) ||
                 ((nameLength == 3) && (strncmp(p, "elp", 3) == 0)) ||
                 ((nameLength == 3) && (strncmp(p, "cfi", 3) == 0)))
        {
            setup.m_cfi = true;
        }
        else
        {
            setup.m_unknown = "an unknown setup key";
            return false;
        }
        p = end;
    }
    return true;
}

// The ABI name of a register ("a0", "sp", "fp"...) or its number ("x10"), or -1
int abiRegister(const char* name, uint length)
{
    static const char* const ABI[32] = {
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
        "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
        "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
    };
    for (int i = 0; i < 32; i++)
    {
        if ((strlen(ABI[i]) == length) && (strncmp(ABI[i], name, length) == 0))
            return i;
    }
    if ((length == 2) && (strncmp(name, "fp", 2) == 0))
        return 8;
    unsigned int index = 0;
    if ((length >= 2) && (name[0] == 'x') && (sscanf(name + 1, "%u", &index) == 1) &&
        (index < 32))
        return (int)index;
    return -1;
}

// The base register of the memory operand of a text ("ld a0, 8(a1)" is a1),
// or -1 when the text has no memory operand
// The mask of the registers which address memory in the row text: the
// registers of the "(reg)" operands (an atomic row may address through a
// second register; the setup of that one is in the window, its random cases
// are not)
uint32 addressMask(const char* text)
{
    uint32 mask = 0;
    for (const char* open = strchr(text, '('); open != NULL; open = strchr(open + 1, '('))
    {
        const char* close = strchr(open, ')');
        if (close == NULL)
            break;
        const int index = abiRegister(open + 1, (uint)(close - open - 1));
        if (index > 0)
            mask |= (uint32)1 << index;
    }
    return mask;
}

int memoryBase(const char* text)
{
    const char* open = strchr(text, '(');
    const char* close = (open != NULL) ? strchr(open, ')') : NULL;
    if (close == NULL)
        return -1;
    return abiRegister(open + 1, (uint)(close - open - 1));
}

// The position of a name in MNEMONICS, or -1
int mnemonicIndex(const char* name, uint length)
{
    for (uint i = 0; i < MNEMONIC_COUNT; i++)
    {
        if ((strlen(MNEMONICS[i]) == length) && (strncmp(MNEMONICS[i], name, length) == 0))
            return (int)i;
    }
    return -1;
}

// The mnemonic a row is counted under, or -1. The text's mnemonic is taken
// as it is, or without its "c." (a compressed row counts under its expansion:
// c.ldsp and c.lwsp under ld and lw, c.addi16sp and c.addi4spn under addi)
int rowMnemonic(const char* text)
{
    uint length = 0;
    while ((text[length] != 0) && (text[length] != ' '))
        length++;
    int index = mnemonicIndex(text, length);
    if (index >= 0)
        return index;
    if ((length > 2) && (text[0] == 'c') && (text[1] == '.'))
    {
        const char* name = text + 2;
        uint stripped = length - 2;
        if ((stripped >= 4) && (strncmp(name, "addi", 4) == 0))
            return mnemonicIndex("addi", 4);
        index = mnemonicIndex(name, stripped);
        if ((index < 0) && (stripped > 2) && (strncmp(name + stripped - 2, "sp", 2) == 0))
            index = mnemonicIndex(name, stripped - 2);
        return index;
    }
    return -1;
}

// The exit a row takes at its last instruction
enum RowExit { EXIT_LABEL, EXIT_INDIRECT_BRANCH, EXIT_INDIRECT_CALL, EXIT_INDIRECT_RETURN };

// The instruction words of a row: the bytes of the row, read as instructions
// (a 2 byte instruction when the low bits are not 11)
struct RowWords
{
    uint32 m_words[4];
    uint m_sizes[4];
    uint m_count;
    uint m_total;
};

RowWords splitWords(const char* code)
{
    RowWords out;
    out.m_count = 0;
    out.m_total = 0;
    // (four words of at most four bytes: the rows of three words exist)
    uint8 bytes[16];
    const uint length = (uint)(strlen(code) / 2);
    for (uint i = 0; i < length && i < 16; i++)
        bytes[i] = (uint8)((hexDigit(code[2 * i]) << 4) | hexDigit(code[2 * i + 1]));
    uint offset = 0;
    while (offset < length && out.m_count < 4)
    {
        const uint size = ((bytes[offset] & 3) == 3) ? 4 : 2;
        uint32 word = 0;
        for (uint b = 0; b < size; b++)
            word |= (uint32)bytes[offset + b] << (8 * b);
        out.m_words[out.m_count] = word;
        out.m_sizes[out.m_count] = size;
        out.m_count++;
        offset += size;
    }
    out.m_total = offset;
    return out;
}

// The exit of the last instruction of a row: a jalr is a call when it links
// (rd is not zero), a return when it is jalr zero, 0(ra|t0), else a branch;
// a compressed c.jalr and c.jr are the same with rd = ra and rd = zero. Every
// other instruction exits to a label (a fall through or a constant target)
RowExit lastExit(const RowWords& words)
{
    const uint last = words.m_count - 1;
    const uint32 w = words.m_words[last];
    if ((words.m_sizes[last] == 4) && ((w & 0x7F) == 0x67))
    {
        const uint rd = (w >> 7) & 31;
        const uint rs1 = (w >> 15) & 31;
        const uint imm = w >> 20;
        if (rd != 0)
            return EXIT_INDIRECT_CALL;
        if (((rs1 == 1) || (rs1 == 5)) && (imm == 0))
            return EXIT_INDIRECT_RETURN;
        return EXIT_INDIRECT_BRANCH;
    }
    if ((words.m_sizes[last] == 2) && ((w & 3) == 2) && (((w >> 13) & 7) == 4))
    {
        const uint rs1 = (w >> 7) & 31;
        const uint rs2 = (w >> 2) & 31;
        if ((rs2 == 0) && (rs1 != 0))
        {
            if ((w >> 12) & 1)
                return EXIT_INDIRECT_CALL;
            if ((rs1 == 1) || (rs1 == 5))
                return EXIT_INDIRECT_RETURN;
            return EXIT_INDIRECT_BRANCH;
        }
    }
    return EXIT_LABEL;
}

// Is the row a memory access: a load or store (4 byte: opcodes 03, 07, 23,
// 27; 2 byte: the loads and stores of quadrants 0 and 2)
bool isMemoryRow(const RowWords& words)
{
    const uint last = words.m_count - 1;
    const uint32 w = words.m_words[last];
    if (words.m_sizes[last] == 4)
    {
        const uint op = w & 0x7F;
        // The AMO opcode (lr, sc and the amos) addresses rs1 as well
        return (op == 0x03) || (op == 0x07) || (op == 0x23) || (op == 0x27) || (op == 0x2F);
    }
    const uint quadrant = w & 3;
    const uint funct3 = (w >> 13) & 7;
    if (quadrant == 0)
        return (funct3 == 4) || ((funct3 >= 1) && (funct3 <= 3)) ||
               ((funct3 >= 5) && (funct3 <= 7));
    if (quadrant == 2)
        return ((funct3 >= 1) && (funct3 <= 3)) || ((funct3 >= 5) && (funct3 <= 7));
    return false;
}

// Is the instruction a floating point operation with the dynamic rounding
// mode (rm = 7 in the instruction): it reads frm, through fcsr in the lift
bool isDynamicRounding(const RowWords& words)
{
    for (uint i = 0; i < words.m_count; i++)
    {
        const uint32 w = words.m_words[i];
        if ((words.m_sizes[i] == 4) && ((w & 0x7F) == 0x53) && (((w >> 12) & 7) == 7))
            return true;
    }
    return false;
}

// Is the row a floating point operation with a static rounding mode (a rm
// field other than the dynamic 7): the rounding of the instruction is fixed
bool hasStaticRounding(const RowWords& words)
{
    for (uint i = 0; i < words.m_count; i++)
    {
        const uint32 w = words.m_words[i];
        // OP-FP (0x53) and the fused multiply-adds (R4 type: fmadd 0x43,
        // fmsub 0x47, fnmsub 0x4B, fnmadd 0x4F), rm in bits 14:12
        const uint opcode = w & 0x7F;
        const bool isFloatOp = (opcode == 0x53) || (opcode == 0x43) ||
                               (opcode == 0x47) || (opcode == 0x4B) ||
                               (opcode == 0x4F);
        if ((words.m_sizes[i] == 4) && isFloatOp && (((w >> 12) & 7) != 7))
            return true;
    }
    return false;
}

// Is a 64 bit value a NaN (double precision)
bool isNaN64(uint64 v)
{
    return ((v & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) &&
           ((v & 0x000FFFFFFFFFFFFFULL) != 0);
}

// Is a 64 bit value a single precision NaN in its NaN box (upper word all ones)
bool isBoxedNaN32(uint64 v)
{
    const uint32 low = (uint32)v;
    return ((v >> 32) == 0xFFFFFFFFULL) &&
           ((low & 0x7F800000U) == 0x7F800000U) && ((low & 0x7FFFFFU) != 0);
}

// Two floating point register values agree when they are equal, or both NaNs
// of the same format (the NaN sign and payload are the documented deviation)
bool agreeFloat(uint64 actual, uint64 expected)
{
    if (actual == expected)
        return true;
    return (isNaN64(actual) && isNaN64(expected)) ||
           (isBoxedNaN32(actual) && isBoxedNaN32(expected));
}

#ifdef LOWERING_RISCV_NATIVE
// ----------------------------------------------------------------------
// The native runs: the state in r15, the guest memory window through the bias
// in r14, and the exit reason in the state slot of the lowering contract
// ----------------------------------------------------------------------
const uint STATE_SIZE = 0x20000;
const uint STATE_CLEAR = 0x10000;

// The guest state of the native runs (f0 is 8 byte aligned, the map is 8 bytes)
uint64 gState[STATE_SIZE / 8] __attribute__((aligned(16)));
uint8 gHostWindow[WINDOW_SIZE] __attribute__((aligned(16)));

// The stack of the first pass binary of the assembler of the tests
class RiscvLoweringStack : public StackInterface {
public:
    RiscvLoweringStack(FirstPassBinaryPtr& binary) :
        StackInterface(*binary, true, 4) {}
    virtual uint getStackType() { return 4; }
    virtual uint getRegisterSize() { return 4; }
    virtual RegisterAllocationTable& getRegistersTable() { return m_registers; }
    virtual const RegisterAllocationTable& getRegistersTable() const { return m_registers; }
    virtual StackLocation replaceRegisterToStackVariable(StackLocation source)
    {
        return source;
    }
    RegisterAllocationTable m_registers;
};

// The native function: the state in rdi, the memory bias in rsi
typedef void (*NativeFunction)(void* state, void* bias);

// The prologue and the epilogue of the native function (the exit label)
const char* const PROLOGUE =
    "push rbx\npush rbp\npush r12\npush r13\npush r14\npush r15\n"
    "mov r15, rdi\nmov r14, rsi\n";
const char* const EPILOGUE =
    "pop r15\npop r14\npop r13\npop r12\npop rbp\npop rbx\nret\n";

// Assemble a text into bytes. Return false on an assembler error
bool assembleBytes(const char* text, cBuffer& code)
{
    FirstPassBinaryPtr binary(new FirstPassBinary(
                            OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
    binary->createNewBlockWithoutChange(0,
            StackInterfacePtr(new RiscvLoweringStack(binary)));
    binary->changeBasicBlock(0);
    AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                            binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);
    cStringerStreamPtr stream = assembler->getAssembler();
    *stream << cString(text);
    IA32AssemblerStream* assembly =
                dynamic_cast<IA32AssemblerStream*>(stream.getPointer());
    if (assembly == NULL)
        return false;
    XSTL_TRY
    {
        assembly->assemble();
    }
    XSTL_CATCH(cException&)
    {
        return false;
    }
    const cBuffer& data = binary->getCurrentBlockData();
    code = cBuffer(data.getSize());
    for (uint i = 0; i < data.getSize(); i++)
        code[i] = data[i];
    return true;
}

// Write a 32 bit little endian value at 'offset'
void patch32(cBuffer& code, uint offset, int32 value)
{
    for (uint b = 0; b < 4; b++)
        code[offset + b] = (uint8)((uint32)value >> (8 * b));
}

// Lower a legalized block and link it into executable memory: the prologue,
// the lowered code, a stub for each guest label the block leaves to (the next
// program counter, the reason REASON_LABEL, then a jump to the epilogue) and
// the epilogue. Return false when the host won't map executable memory
bool buildNative(const SemanticBlock& block, const SemanticRegisterMap& registerMap,
                 NativeFunction& function, void*& memory, uint& memorySize)
{
    AMD64Lowering lowering;
    AMD64Lowering::LoweredBlock lowered;
    lowering.lowerBlock(block, registerMap, lowered);

    cBuffer prologue, epilogue;
    if (!assembleBytes(PROLOGUE, prologue) || !assembleBytes(EPILOGUE, epilogue))
        return false;

    // The stubs, one for each guest label the block refers to
    cList<cString> names;
    for (cList<AMD64Lowering::LabelDependency>::iterator d =
            lowered.m_dependencies.begin();
         d != lowered.m_dependencies.end(); d++)
    {
        bool isKnown = (*d).m_name == AMD64Lowering::getExitLabel();
        for (cList<cString>::iterator n = names.begin(); n != names.end(); n++)
            isKnown = isKnown || (*n == (*d).m_name);
        if (!isKnown)
            names.append((*d).m_name);
    }
    const uint codeOffset = prologue.getSize();
    uint offset = codeOffset + lowered.m_code.getSize();
    cList<uint> stubOffsets;
    cList<cBuffer> stubs;
    for (cList<cString>::iterator n = names.begin(); n != names.end(); n++)
    {
        // The name is B_<hex>: the next program counter, then the label reason
        cSArray<char> label = (*n).getASCIIstring();
        char text[300];
        snprintf(text, sizeof(text),
                 "mov rax, 0x%s\nmov qword [r15+0x100], rax\n"
                 "mov qword [r15+0x110], 0x%llx\n", label.getBuffer() + 2,
                 (unsigned long long)REASON_LABEL);
        cBuffer stub;
        if (!assembleBytes(text, stub))
            return false;
        stubOffsets.append(offset);
        stubs.append(stub);
        offset += stub.getSize() + 5;
    }
    const uint epilogueOffset = offset;

    const uint size = epilogueOffset + epilogue.getSize();
    cBuffer code(size);
    for (uint i = 0; i < codeOffset; i++)
        code[i] = prologue[i];
    for (uint i = 0; i < lowered.m_code.getSize(); i++)
        code[codeOffset + i] = lowered.m_code[i];
    cList<uint>::iterator so = stubOffsets.begin();
    for (cList<cBuffer>::iterator s = stubs.begin(); s != stubs.end(); s++, so++)
    {
        for (uint i = 0; i < (*s).getSize(); i++)
            code[*so + i] = (*s)[i];
        // jmp epilogue
        const uint jump = *so + (*s).getSize();
        code[jump] = 0xE9;
        patch32(code, jump + 1, (int32)(epilogueOffset - (jump + 5)));
    }
    for (uint i = 0; i < epilogue.getSize(); i++)
        code[epilogueOffset + i] = epilogue[i];

    // Resolve the references of the lowered code
    for (cList<AMD64Lowering::LabelDependency>::iterator d =
            lowered.m_dependencies.begin();
         d != lowered.m_dependencies.end(); d++)
    {
        uint target = epilogueOffset;
        if (!((*d).m_name == AMD64Lowering::getExitLabel()))
        {
            target = 0;
            cList<uint>::iterator so2 = stubOffsets.begin();
            for (cList<cString>::iterator n = names.begin(); n != names.end();
                 n++, so2++)
            {
                if (*n == (*d).m_name)
                    target = *so2;
            }
        }
        const uint field = codeOffset + (*d).m_offset;
        patch32(code, field, (int32)(target - (field + 4)));
    }

    memory = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
    {
        memory = NULL;
        return false;
    }
    memorySize = size;
    for (uint i = 0; i < size; i++)
        ((uint8*)memory)[i] = code[i];
    function = reinterpret_cast<NativeFunction>(memory);
    return true;
}

// The guest memory of the interpreter: the window only (any other address
// is a memory fault, which the rows must not reach)
class WindowMemory : public SemanticMemory {
public:
    WindowMemory(const uint8* window)
    {
        memcpy(m_bytes, window, WINDOW_SIZE);
    }
    virtual bool read(uint64 address, uint8* buffer, uint size)
    {
        if (!inWindow(address, size))
            return false;
        memcpy(buffer, m_bytes + (address - WINDOW_BASE), size);
        return true;
    }
    virtual bool write(uint64 address, const uint8* buffer, uint size)
    {
        if (!inWindow(address, size))
            return false;
        memcpy(m_bytes + (address - WINDOW_BASE), buffer, size);
        return true;
    }
    uint8 m_bytes[WINDOW_SIZE];
private:
    static bool inWindow(uint64 address, uint size)
    {
        return (address >= WINDOW_BASE) &&
               (address - WINDOW_BASE + size <= WINDOW_SIZE);
    }
};

// The outcome of one row
enum RowStatus { ROW_PASSED, ROW_FAILED, ROW_SKIPPED };

class TestObjectTestSemanticLoweringRiscv : public cTestObject {
public:
    TestObjectTestSemanticLoweringRiscv() : m_run(0), m_failed(0), m_skipped(0)
    {
        for (uint i = 0; i < MNEMONIC_COUNT; i++)
        {
            m_passed[i] = 0;
            m_rowSkipped[i] = 0;
            m_rowRoundSkipped[i] = 0;
            m_rows[i] = 0;
        }
    }

    virtual void test()
    {
        m_guest = ArchitectureDescriptionPtr(new RiscvArchitecture(
                RiscvInstructionSet::RISCV_RV64, 128, false));
        m_target = ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);

        // The rows of the table of the mnemonic set
        // (the table ends with a { NULL, NULL, NULL, NULL } sentinel)
        static const RiscvRow rows[] = { RISCV64_SEMANTIC_ROWS };
        for (uint i = 0; (i < sizeof(rows) / sizeof(rows[0])) && (rows[i].m_text != NULL); i++)
            considerRow(rows[i]);
        for (uint i = 0; i < sizeof(CALL_ROWS) / sizeof(CALL_ROWS[0]); i++)
            considerRow(CALL_ROWS[i]);
        for (uint i = 0; i < sizeof(ATOMIC_ROWS) / sizeof(ATOMIC_ROWS[0]); i++)
            considerRow(ATOMIC_ROWS[i]);
        for (uint i = 0; i < sizeof(FUSED_ROWS) / sizeof(FUSED_ROWS[0]); i++)
            considerRow(FUSED_ROWS[i]);

        cout << "riscv rows: " << m_run << " run, " << m_failed << " failed, "
             << m_skipped << " skipped" << endl;
        for (uint i = 0; i < MNEMONIC_COUNT; i++)
        {
            cout << "riscv coverage: " << MNEMONICS[i] << " " << m_passed[i]
                 << " passed, " << m_rowSkipped[i] << " skipped";
            if (m_rowRoundSkipped[i] != 0)
                cout << " (" << m_rowRoundSkipped[i] << " rounding refused)";
            cout << endl;
        }
        TESTS_ASSERT(m_failed == 0);
        for (uint i = 0; i < MNEMONIC_COUNT; i++)
        {
            if (m_passed[i] == 0)
                fprintf(stderr, "TestSemanticLoweringRiscv: no passing row of %s\n",
                        MNEMONICS[i]);
            TESTS_ASSERT(m_passed[i] != 0);
        }
        cout << "Semantic lowering riscv tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    struct RowInfo
    {
        const RiscvRow* m_row;
        uint m_index;
        // The row with rm = 0 in place of the dynamic rm (7)
        bool m_rneCopy;
    };

    // A row of the table: when its mnemonic is in the set, run it
    void considerRow(const RiscvRow& row)
    {
        const int index = rowMnemonic(row.m_text);
        if (index < 0)
            return;
        RowInfo info;
        info.m_row = &row;
        info.m_index = (uint)index;
        info.m_rneCopy = false;
        m_rows[index]++;
        runRow(info);
        // A dynamic rounding row also runs as a copy with its rm field set to
        // 0 (RNE): the interpreter reads frm = 0 (fcsr is 0), so the copy is
        // the same instruction with the rounding the lowering implements
        if (isDynamicRounding(splitWords(row.m_code)))
        {
            info.m_rneCopy = true;
            runRow(info);
        }
    }

    // Lift, legalize, lower and run one row. Count the outcome
    void runRow(const RowInfo& info)
    {
        const char* skipReason = NULL;
        RowStatus status = checkRow(info, skipReason);
        if (status == ROW_SKIPPED)
        {
            m_skipped++;
            m_rowSkipped[info.m_index]++;
            if (strncmp(skipReason, "static rounding", 15) == 0)
                m_rowRoundSkipped[info.m_index]++;
            cout << "riscv skip: " << info.m_row->m_text << ": " << skipReason << endl;
            return;
        }
        m_run++;
        if (status == ROW_PASSED)
            m_passed[info.m_index]++;
        else
            m_failed++;
    }

    // The check of a row. A skipped row sets reason
    RowStatus checkRow(const RowInfo& info, const char*& skipReason)
    {
        const RiscvRow& row = *info.m_row;
        RowWords words = splitWords(row.m_code);
        if (words.m_count == 0)
            return reportFailure(row, "the row has no instruction");
        // A CSR instruction (SYSTEM with funct3 != 0) reads or writes fcsr. The
        // lowering doesn't maintain fflags (documented, as FPSR on A64)
        for (uint i = 0; i < words.m_count; i++)
        {
            if ((words.m_sizes[i] == 4) && ((words.m_words[i] & 0x7F) == 0x73) &&
                (((words.m_words[i] >> 12) & 7) != 0))
            {
                skipReason = "fflags: a CSR read of fcsr, not maintained by the lowering (documented)";
                return ROW_SKIPPED;
            }
        }
        if (info.m_rneCopy)
        {
            // rm = 7 (dynamic) becomes rm = 0 (RNE) in every FP instruction
            for (uint i = 0; i < words.m_count; i++)
            {
                if ((words.m_sizes[i] == 4) && ((words.m_words[i] & 0x7F) == 0x53) &&
                    (((words.m_words[i] >> 12) & 7) == 7))
                    words.m_words[i] &= ~(uint32)(7u << 12);
            }
        }

        // The memory rows: the base of the setup is moved into the window
        // (keeping its low 12 bits), with the memory of the setup, when the
        // setup's base is outside it. The instruction only uses base + offset
        RowSetup setup;
        const bool memoryRow = isMemoryRow(words);
        int base = memoryRow ? memoryBase(row.m_text) : -1;
        if (memoryRow && (base < 0))
            return reportFailure(row, "a memory row without a base register");
        if (!parseSetup(row.m_setup, setup, -1, 0))
        {
            fprintf(stderr, "TestSemanticLoweringRiscv: %s: %s\n", row.m_text,
                    setup.m_unknown);
            return ROW_FAILED;
        }
        // A base outside the safe range of the window (0x800 bytes from each
        // end, see below) is moved to the default base, with the memory of
        // the setup: the access is the same relative to the base
        if ((base >= 0) && setup.m_hasX[base] &&
            ((setup.m_x[base] < WINDOW_BASE + 0x800) ||
             (setup.m_x[base] >= WINDOW_BASE + WINDOW_SIZE - 0x800)))
        {
            const uint64 newBase = WINDOW_DEFAULT_BASE + (setup.m_x[base] & 0xFFF);
            const uint64 delta = newBase - setup.m_x[base];
            parseSetup(row.m_setup, setup, base, delta);
        }
        if (setup.m_cfi && (strstr(row.m_expected, "trap:") != NULL))
        {
            skipReason = "a CFI trap row (shadow stack): the test runs the guest without CFI";
            return ROW_SKIPPED;
        }
        // (a CFI row on the non-CFI guest doesn't touch its shadow stack, the
        // memory outside the window, so that memory is not needed)
        if (setup.m_memOutside && !setup.m_cfi)
        {
            skipReason = "the setup memory is outside the guest memory window";
            return ROW_SKIPPED;
        }
        if ((base >= 0) && setup.m_hasX[base] &&
            ((setup.m_x[base] < WINDOW_BASE + 0x800) ||
             (setup.m_x[base] >= WINDOW_BASE + WINDOW_SIZE - 0x800)))
        {
            skipReason = "the setup base register is outside the guest memory window";
            return ROW_SKIPPED;
        }

        // The lift of the words (at the guest address, the next instruction
        // after each one)
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        RiscvSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_RISCV_64, m_guest);
        uint64 pc = ROW_ADDRESS;
        for (uint i = 0; i < words.m_count; i++)
        {
            RiscvOpcodePtr decoded = RiscvStreamDisassembler::decode(
                    RiscvInstructionSet::RISCV_RV64, words.m_words[i],
                    words.m_sizes[i], pc, RiscvFeatureSet::all());
            if (decoded.getPointer() == NULL)
                return reportFailure(row, "the word doesn't decode");
            OpcodePtr opcode((Opcode*)decoded.getPointer(), SMARTPTR_DESTRUCT_NONE);
            if (!lifter.lift(opcode, block))
                return reportFailure(row, "the word doesn't lift");
            pc += words.m_sizes[i];
        }
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ROW_ADDRESS + words.m_total));

        // The legalized copy, lowered and linked. A static rounding mode the
        // lowering refuses (the arithmetic and the float to float conversions
        // implement the nearest even rounding only) is a skip, with the refusal
        // as its reason; any other refusal is a failure
        SemanticBlock legalized(block);
        RiscvStateMap registerMap;
        NativeFunction function = NULL;
        void* memory = NULL;
        uint memorySize = 0;
        try
        {
            SemanticLegalizer legalizer(m_guest, m_target);
            legalizer.legalize(legalized);
            if (!buildNative(legalized, registerMap, function, memory, memorySize))
            {
                skipReason = "no executable memory (the host won't map it)";
                return ROW_SKIPPED;
            }
        } catch (cException& exception)
        {
            cSArray<char> message = cString(exception.getMessage()).getASCIIstring();
            if (hasStaticRounding(words) && (strstr(message.getBuffer(), "is implemented") != NULL))
            {
                snprintf(m_reason, sizeof(m_reason), "static rounding mode refused by the lowering%s: %s",
                         info.m_rneCopy ? " (RNE copy of a dyn row)" : "", message.getBuffer());
                skipReason = m_reason;
                return ROW_SKIPPED;
            }
            return reportFailure(row, message.getBuffer());
        }

        // The cases: the setup first, then the random inputs
        const RowExit exit = lastExit(words);
        bool isPassed = true;
        for (uint c = 0; (c < CASES_PER_ROW) && isPassed; c++)
        {
            CaseState state;
            makeCase(setup, base, addressMask(row.m_text), c, state);
            if (!compareCase(row, block, function, exit, words.m_total, state))
                isPassed = false;
        }
        munmap(memory, memorySize);
        return isPassed ? ROW_PASSED : ROW_FAILED;
    }

    RowStatus reportFailure(const RiscvRow& row, const char* message)
    {
        fprintf(stderr, "TestSemanticLoweringRiscv: %s: %s\n", row.m_text, message);
        return ROW_FAILED;
    }

    // The inputs of one case. Case 0 is the setup (zeros where it has no
    // value). The others are random; the base register of a memory row is
    // the window base, and any other register that addresses memory is a
    // window address (the 'addresses' mask of addressMask())
    void makeCase(const RowSetup& setup, int base, uint32 addresses, uint c,
                  CaseState& state)
    {
        const bool random = (c != 0);
        for (uint i = 1; i < 32; i++)
        {
            if (!random && setup.m_hasX[i])
                state.m_x[i] = setup.m_x[i];
            else if ((int)i == base)
                state.m_x[i] = WINDOW_DEFAULT_BASE;
            else if (random && ((addresses >> i) & 1))
                state.m_x[i] = WINDOW_DEFAULT_BASE + (randomWord() & 0xFF8);
            else
                state.m_x[i] = random ? randomWord() : 0;
        }
        state.m_x[0] = 0;
        for (uint i = 0; i < 32; i++)
        {
            if (!random && setup.m_hasF[i])
                state.m_f[i] = setup.m_f[i];
            else
                state.m_f[i] = random ? randomWord() : 0;
        }
        for (uint i = 0; i < WINDOW_SIZE; i++)
        {
            if (random)
                state.m_window[i] = (uint8)nextRandom();
            else
                state.m_window[i] = setup.m_hasMem[i] ? setup.m_mem[i] : 0;
        }
    }

    // Run one case in both: the interpreter on the lifted block, the native
    // code on the lowered one. Compare the outcomes
    bool compareCase(const RiscvRow& row, const SemanticBlock& block,
                     NativeFunction function, RowExit exit, uint totalSize,
                     const CaseState& state)
    {
        char message[512];
        RiscvStateMap registerMap;
        WindowMemory memory(state.m_window);
        SemanticInterpreter interpreter(m_guest, memory);
        for (uint i = 1; i < 32; i++)
            interpreter.setRegister(RISCV_REG_X0 + i, SemanticValue(state.m_x[i]));
        for (uint i = 0; i < 32; i++)
            interpreter.setRegister(RISCV_REG_F0 + i, SemanticValue(state.m_f[i]));
        interpreter.setRegister(RISCV_REG_FCSR, SemanticValue((uint64)0));
        const SemanticInterpreter::ExitReason reason = interpreter.run(block);

        // The native state
        memset(gState, 0, STATE_CLEAR);
        uint8* bytes = reinterpret_cast<uint8*>(gState);
        for (uint i = 1; i < 32; i++)
            memcpy(bytes + registerMap.getStateOffset(RISCV_REG_X0 + i), &state.m_x[i], 8);
        for (uint i = 0; i < 32; i++)
            memcpy(bytes + registerMap.getStateOffset(RISCV_REG_F0 + i), &state.m_f[i], 8);
        memcpy(gHostWindow, state.m_window, WINDOW_SIZE);
        const void* bias = reinterpret_cast<const void*>(
                (uintptr_t)gHostWindow - (uintptr_t)WINDOW_BASE);
        function(gState, const_cast<void*>(bias));

        // The exit of the native run
        const uint64 exitReason = *(uint64*)(bytes + registerMap.getExitReasonOffset());
        const uint64 exitPc = *(uint64*)(bytes + registerMap.getProgramCounterOffset());
        if (!compareExit(row, exit, reason, interpreter, exitReason, exitPc))
            return false;
        (void)totalSize;

        // The integer registers
        for (uint i = 1; i < 32; i++)
        {
            const uint64 actual = *(uint64*)(bytes + registerMap.getStateOffset(RISCV_REG_X0 + i));
            const uint64 expected = interpreter.getRegister(RISCV_REG_X0 + i).getWord(0);
            if (actual != expected)
            {
                snprintf(message, sizeof(message), "x%u differs: native 0x%llx interpreter 0x%llx",
                         i, (unsigned long long)actual, (unsigned long long)expected);
                return reportCase(row, message);
            }
        }

        // The floating point registers: the NaN class of a result
        for (uint i = 0; i < 32; i++)
        {
            const uint64 actual = *(uint64*)(bytes + registerMap.getStateOffset(RISCV_REG_F0 + i));
            const uint64 expected = interpreter.getRegister(RISCV_REG_F0 + i).getWord(0);
            if (!agreeFloat(actual, expected))
            {
                snprintf(message, sizeof(message),
                         "f%u differs: native 0x%016llx interpreter 0x%016llx"
                         " (inputs x1 0x%llx x2 0x%llx f1 0x%016llx f2 0x%016llx)",
                         i, (unsigned long long)actual, (unsigned long long)expected,
                         (unsigned long long)state.m_x[1], (unsigned long long)state.m_x[2],
                         (unsigned long long)state.m_f[1], (unsigned long long)state.m_f[2]);
                return reportCase(row, message);
            }
        }

        // The memory window
        if (memcmp(gHostWindow, memory.m_bytes, WINDOW_SIZE) != 0)
            return reportCase(row, "the memory window differs");
        return true;
    }

    // The exits of the two runs. A jump to a label (LABEL) is the label stub's
    // reason, with the interpreter's next address. An indirect exit is the
    // lowering's call, return or branch reason, with the interpreter's branch
    bool compareExit(const RiscvRow& row, RowExit exit,
                     SemanticInterpreter::ExitReason reason,
                     const SemanticInterpreter& interpreter, uint64 exitReason,
                     uint64 exitPc)
    {
        const uint64 interpreterNext = interpreter.getNextAddress();
        // A trap (an OP_TRAP of the lifter: a misaligned atomic or sc.d, for
        // one): the lowering exits with EXIT_TRAP, the kind and the number in
        // the reason, and the trapping instruction in the pc
        if (reason == SemanticInterpreter::EXIT_TRAP)
        {
            const uint64 expectedReason = AMD64Lowering::EXIT_TRAP |
                ((uint64)interpreter.getTrapKind() << AMD64Lowering::EXIT_TRAP_KIND_SHIFT) |
                (interpreter.getTrapNumber() << AMD64Lowering::EXIT_TRAP_NUMBER_SHIFT);
            if ((exitReason != expectedReason) || (exitPc != interpreterNext))
            {
                fprintf(stderr, "TestSemanticLoweringRiscv: %s: the trap exit (reason %llu, "
                        "pc 0x%llx) differs from the interpreter (reason %llu, next 0x%llx)\n",
                        row.m_text, (unsigned long long)exitReason,
                        (unsigned long long)exitPc, (unsigned long long)expectedReason,
                        (unsigned long long)interpreterNext);
                return false;
            }
            return true;
        }
        if (exit == EXIT_LABEL)
        {
            if ((exitReason != REASON_LABEL) ||
                (((reason != SemanticInterpreter::EXIT_FALL_THROUGH) &&
                  (reason != SemanticInterpreter::EXIT_BRANCH)) ||
                 (exitPc != interpreterNext)))
            {
                fprintf(stderr, "TestSemanticLoweringRiscv: %s: the label exit (reason %llu, "
                        "pc 0x%llx) differs from the interpreter (reason %d, next 0x%llx)\n",
                        row.m_text, (unsigned long long)exitReason,
                        (unsigned long long)exitPc, (int)reason,
                        (unsigned long long)interpreterNext);
                return false;
            }
            return true;
        }
        const uint64 expected = (exit == EXIT_INDIRECT_CALL) ? AMD64Lowering::EXIT_CALL :
            (exit == EXIT_INDIRECT_RETURN) ? AMD64Lowering::EXIT_RETURN :
            AMD64Lowering::EXIT_BRANCH;
        if ((exitReason != expected) || (reason != SemanticInterpreter::EXIT_BRANCH) ||
            (exitPc != interpreterNext))
        {
            fprintf(stderr, "TestSemanticLoweringRiscv: %s: the indirect exit (reason %llu, "
                    "expected %llu, pc 0x%llx) differs from the interpreter (reason %d, "
                    "next 0x%llx)\n", row.m_text, (unsigned long long)exitReason,
                    (unsigned long long)expected, (unsigned long long)exitPc, (int)reason,
                    (unsigned long long)interpreterNext);
            return false;
        }
        return true;
    }

    // Report a difference of a case
    bool reportCase(const RiscvRow& row, const char* message)
    {
        fprintf(stderr, "TestSemanticLoweringRiscv: %s: %s\n", row.m_text, message);
        return false;
    }

    // The guest and the target of the rows
    ArchitectureDescriptionPtr m_guest;
    ArchitectureDescriptionPtr m_target;
    // The rows that ran, failed and were skipped
    uint m_run;
    uint m_failed;
    uint m_skipped;
    // The rows of each mnemonic, its passing rows, its skipped rows and the
    // rows skipped for a rounding mode the lowering refuses
    uint m_rows[MNEMONIC_COUNT];
    uint m_passed[MNEMONIC_COUNT];
    uint m_rowSkipped[MNEMONIC_COUNT];
    uint m_rowRoundSkipped[MNEMONIC_COUNT];
    // The reason of a skip that is built from a lowering message
    char m_reason[320];
};

#else // !LOWERING_RISCV_NATIVE

class TestObjectTestSemanticLoweringRiscv : public cTestObject {
public:
    virtual void test()
    {
        cout << "riscv rows: 0 run, 0 failed, 0 skipped (the native runs need an "
                "x86-64 Linux host)" << endl;
    }
    virtual cString getName() { return __FILE__; }
};

#endif

} // namespace

// Instance test object
TestObjectTestSemanticLoweringRiscv g_globalTestSemanticLoweringRiscv;
