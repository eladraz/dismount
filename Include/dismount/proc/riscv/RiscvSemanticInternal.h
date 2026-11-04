#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVSEMANTICINTERNAL_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVSEMANTICINTERNAL_H

/*
 * RiscvSemanticInternal.h
 *
 * The hand written lifters of the scalar RISC-V instructions, which are split
 * per area (RiscvSemanticLiftInteger.cpp: the base integer instructions, M and
 * the bit manipulation; RiscvSemanticLiftFloat.cpp: F, D, Zfh, Zfa and
 * Zfbfmin; RiscvSemanticLiftAtomic.cpp: A, Zabha and Zacas;
 * RiscvSemanticLiftSystem.cpp: Zicsr, the fences, the cache block
 * operations, the control flow integrity and the privileged instructions;
 * RiscvSemanticLiftCrypto.cpp: the scalar cryptography), and the helpers they
 * share. Not a public header: the lifter is RiscvSemanticLifter
 * (RiscvSemantics.h).
 *
 * One lifter serves a family of mnemonics, and the table entry gives the
 * family member in RiscvSemanticEntry::m_parameter (RiscvLiftContext::
 * getParameter()). The meaning of the parameter is documented at each
 * lifter.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/proc/riscv/RiscvSemantics.h"

namespace riscvdis {

typedef SemanticOperation Op;

/*
 * The helpers
 */

// Return the integer type of 'bits' bits
inline SemanticType integerType(uint bits)
{
    return SemanticType::integer(bits);
}

// Return the entry mnemonic of the lifted instruction
const char* getMnemonic(RiscvLiftContext& context);

// Return the mask of the low 'bits' bits (1 to 64)
uint64 maskOf(uint bits);

// Return an immediate of 'type'
SemanticOperand makeConstant(RiscvLiftContext& context, int64 value,
                             const SemanticType& type);

// Return the 1 bit result of comparing 'a' and 'b' (of 'type')
SemanticOperand makeCompare(RiscvLiftContext& context,
                            SemanticOperation::Condition condition,
                            const SemanticType& type,
                            const SemanticOperand& a,
                            const SemanticOperand& b);

// Return 'condition ? a : b' (a 1 bit condition)
SemanticOperand makeSelect(RiscvLiftContext& context, const SemanticType& type,
                           const SemanticOperand& condition,
                           const SemanticOperand& a,
                           const SemanticOperand& b);

// Return 'value' converted to 'bits' bits: zero or sign extended, or
// truncated
SemanticOperand makeZeroExtend(RiscvLiftContext& context,
                               const SemanticOperand& value, uint bits);
SemanticOperand makeSignExtend(RiscvLiftContext& context,
                               const SemanticOperand& value, uint bits);
SemanticOperand makeTruncate(RiscvLiftContext& context,
                             const SemanticOperand& value, uint bits);

// Add an unconditional OP_TRAP of a number, and one taken when the 1 bit
// 'condition' is set
void makeTrap(RiscvLiftContext& context, SemanticOperation::TrapKind kind,
              uint number);
void makeTrapIf(RiscvLiftContext& context, const SemanticOperand& condition,
                SemanticOperation::TrapKind kind, uint number);

// Add an OP_BITCAST of 'value' to 'type'
SemanticOperand makeBitcast(RiscvLiftContext& context,
                            const SemanticOperand& value,
                            const SemanticType& type);

// Add a load / a store with a memory order
SemanticOperand makeLoad(RiscvLiftContext& context,
                         const SemanticOperand& address,
                         const SemanticType& type,
                         SemanticOperation::MemoryOrder order);
void makeStore(RiscvLiftContext& context, const SemanticOperand& address,
               const SemanticOperand& value,
               SemanticOperation::MemoryOrder order);

// Return the memory order of the aq / rl bits of an atomic instruction
SemanticOperation::MemoryOrder getAtomicOrder(RiscvLiftContext& context);

/*
 * Common lifters
 */

// A hint, and the compressed hints: no effect
void liftHint(RiscvLiftContext& context);

// The check every instruction but lpad begins with when Zicfilp is enabled:
// a software check exception while a landing pad is expected
void liftLandingPadCheck(RiscvLiftContext& context);

/*
 * RiscvSemanticLiftInteger.cpp. The parameter is a SemanticOperation::
 * Operation unless it says otherwise
 */
// rd = rs1 <op> rs2 / the immediate, XLEN bits
void liftBinary(RiscvLiftContext& context);
// The same on 32 bits, sign extended (addw, subw, addiw, mulw)
void liftBinaryWord(RiscvLiftContext& context);
// The shifts: the count is rs2 / the shamt modulo XLEN (the shift ops)
void liftShift(RiscvLiftContext& context);
// The shifts of 32 bits, sign extended
void liftShiftWord(RiscvLiftContext& context);
// slt, sltu, slti, sltiu: the parameter is a SemanticOperation::Condition
void liftSetLess(RiscvLiftContext& context);
// lui (parameter 0), auipc (1)
void liftUpper(RiscvLiftContext& context);
// lb, lbu, lh, lhu, lw, lwu, ld: the parameter is the bits of the access
// (| LOAD_UNSIGNED)
void liftLoad(RiscvLiftContext& context);
// sb, sh, sw, sd: the parameter is the bits of the access
void liftStore(RiscvLiftContext& context);
// beq...: the parameter is a SemanticOperation::Condition
void liftBranch(RiscvLiftContext& context);
void liftJal(RiscvLiftContext& context);
void liftJalr(RiscvLiftContext& context);
// mul / div / rem family: the high multiplies and the divisions
void liftDivide(RiscvLiftContext& context);
void liftDivideWord(RiscvLiftContext& context);
// Zba: sh1add, sh2add, sh3add (the parameter is the shift | SHIFT_ADD_WORD
// for the .uw forms)
void liftShiftAdd(RiscvLiftContext& context);
void liftSlliUw(RiscvLiftContext& context);
// Zbb / Zbkb
// andn (parameter 0), orn (1), xnor (2)
void liftLogicNegated(RiscvLiftContext& context);
// rol / ror, the register and immediate forms: the parameter is OP_ROTATE_*
void liftRotate(RiscvLiftContext& context);
void liftRotateWord(RiscvLiftContext& context);
void liftByteSwap(RiscvLiftContext& context);
// clz, ctz, cpop: the parameter is the operation; the w forms are 32 bits
void liftBitCount(RiscvLiftContext& context);
void liftBitCountWord(RiscvLiftContext& context);
// sext.b (8), sext.h (16)
void liftSignExtendBits(RiscvLiftContext& context);
// zext.h
void liftZeroExtendHalf(RiscvLiftContext& context);
void liftOrCombineBytes(RiscvLiftContext& context);
// Zbs: bclr (0), bext (1), binv (2), bset (3), the register and immediate
// forms
void liftSingleBit(RiscvLiftContext& context);
// Zbc: clmul (0), clmulh (1), clmulr (2)
void liftCarryless(RiscvLiftContext& context);
// Zbkb: pack (0), packh (1), packw (2)
void liftPack(RiscvLiftContext& context);
void liftReverseBitsInBytes(RiscvLiftContext& context);
// zip (0), unzip (1)
void liftZip(RiscvLiftContext& context);
// Zbkx: xperm4 (4), xperm8 (8)
void liftCrossbar(RiscvLiftContext& context);
// Zicond: czero.eqz (0), czero.nez (1)
void liftConditionalZero(RiscvLiftContext& context);
// Zimop, Zcmop (mop.r.N, mop.rr.N, c.mop.N): rd = 0
void liftMop(RiscvLiftContext& context);

// The bits of the parameter of liftLoad
enum { LOAD_UNSIGNED = 0x100 };
// The bit of the parameter of liftShiftAdd
enum { SHIFT_ADD_WORD = 0x100 };

/*
 * RiscvSemanticLiftFloat.cpp. The parameter is the SemanticType::FloatFormat
 * of the instruction (the destination for the conversions) unless it says
 * otherwise
 */
// flh, flw, fld / fsh, fsw, fsd
void liftFloatLoad(RiscvLiftContext& context);
void liftFloatStore(RiscvLiftContext& context);
// fadd, fsub, fmul, fdiv: the parameter is format | (operation << 8)
void liftFloatArithmetic(RiscvLiftContext& context);
void liftFloatSquareRoot(RiscvLiftContext& context);
// fmadd, fmsub, fnmsub, fnmadd: format | (kind << 8), kind 0 to 3
void liftFloatFused(RiscvLiftContext& context);
// fsgnj, fsgnjn, fsgnjx: format | (kind << 8), kind 0 to 2
void liftFloatSign(RiscvLiftContext& context);
// fmin, fmax: format | (OP_FLOAT_MINIMUM / MAXIMUM << 8); fminm, fmaxm (Zfa):
// the same | (1 << 16), the NaN propagates
void liftFloatMinMax(RiscvLiftContext& context);
// feq (kind 0), flt (1), fle (2), fltq (3), fleq (4)
void liftFloatCompare(RiscvLiftContext& context);
void liftFloatClassify(RiscvLiftContext& context);
// fcvt.w / wu / l / lu from a float: format | (unsigned << 8) | (bits << 16)
void liftFloatToInteger(RiscvLiftContext& context);
// fcvt from an integer: format | (unsigned << 8) | (bits << 16)
void liftIntegerToFloat(RiscvLiftContext& context);
// fcvt between the formats: the parameter is the destination format |
// (source format << 8)
void liftFloatConvert(RiscvLiftContext& context);
// fmv.x.w / fmv.x.h / fmv.x.d (the format) and fmv.w.x / fmv.h.x / fmv.d.x
void liftFloatMoveToInteger(RiscvLiftContext& context);
void liftFloatMoveFromInteger(RiscvLiftContext& context);
// Zfa
void liftFloatLoadImmediate(RiscvLiftContext& context);
// fround (kind 0) / froundnx (1)
void liftFloatRound(RiscvLiftContext& context);
void liftFloatConvertModular(RiscvLiftContext& context);
// fmvh.x.d (0), fmvp.d.x (1)
void liftFloatMovePair(RiscvLiftContext& context);

/*
 * RiscvSemanticLiftAtomic.cpp
 */
// lr (the parameter is the bits)
void liftLoadReserved(RiscvLiftContext& context);
// sc
void liftStoreConditional(RiscvLiftContext& context);
// The amos: the parameter is the bits | (operation << 8), with
// OP_ATOMIC_EXCHANGE for the swap
void liftAtomicOperation(RiscvLiftContext& context);
// amocas.w / d / q: the parameter is the bits
void liftCompareAndSwap(RiscvLiftContext& context);

/*
 * RiscvSemanticLiftSystem.cpp
 */
// csrrw (0), csrrs (1), csrrc (2), and the immediate forms (| CSR_IMMEDIATE)
void liftCsr(RiscvLiftContext& context);
enum { CSR_IMMEDIATE = 0x100 };
void liftFence(RiscvLiftContext& context);
void liftFenceTso(RiscvLiftContext& context);
void liftFenceInstruction(RiscvLiftContext& context);
// pause
void liftPause(RiscvLiftContext& context);
// wrs.nto, wrs.sto
void liftWaitReservation(RiscvLiftContext& context);
// prefetch.i / r / w
void liftPrefetch(RiscvLiftContext& context);
// cbo.clean, cbo.flush, cbo.inval (the parameter is the CacheOperation)
void liftCacheBlock(RiscvLiftContext& context);
void liftCacheBlockZero(RiscvLiftContext& context);
void liftEcall(RiscvLiftContext& context);
void liftEbreak(RiscvLiftContext& context);
void liftUnimp(RiscvLiftContext& context);
// Zicfilp / Zicfiss
void liftLandingPad(RiscvLiftContext& context);
void liftShadowStackPush(RiscvLiftContext& context);
void liftShadowStackPopCheck(RiscvLiftContext& context);
void liftShadowStackRead(RiscvLiftContext& context);
void liftShadowStackSwap(RiscvLiftContext& context);
// The privileged instructions: the helper RISCV_HELPER_PRIVILEGED
void liftPrivileged(RiscvLiftContext& context);

/*
 * RiscvSemanticLiftCrypto.cpp. The parameter is the sub-operation
 */
void liftAes32(RiscvLiftContext& context);
void liftAes64(RiscvLiftContext& context);
void liftAes64KeySchedule(RiscvLiftContext& context);
void liftSha256(RiscvLiftContext& context);
void liftSha512(RiscvLiftContext& context);
void liftSha512Rv32(RiscvLiftContext& context);
void liftSm3(RiscvLiftContext& context);
void liftSm4(RiscvLiftContext& context);

} // namespace riscvdis

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVSEMANTICINTERNAL_H
