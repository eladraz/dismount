/*
 * RiscvSemanticTable.cpp
 *
 * Implementation file
 *
 * The semantic table of the scalar unprivileged RISC-V instructions (see
 * RiscvSemantics.h): the mnemonics of gRiscvBaseEncodingTable with their
 * classification and the hand written lifter which lifts them
 * (RiscvSemanticInternal.h). One lifter serves a family of mnemonics; the
 * last field of an entry, the parameter, tells the members apart. The
 * compressed instructions are lifted by their expansion and the aliases by
 * the general encoding, so they have no entries but for the few aliases
 * whose semantics are their own (lpad, sspush, sspopchk, ssrdp, pause and
 * fence.tso).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include <string.h>

#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"

namespace riscvdis {

namespace {

#define U_(x) OpcodeClassification::UNIT_##x
#define P_(x) OpcodeClassification::PROPERTY_##x

// An entry: the mnemonic, the lifter, the category, the units, the
// properties and the parameter
#define RV(name, lift, category, units, properties, parameter) \
    { RiscvSemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, OpcodeClassification::CATEGORY_##category, units, \
        properties, 0, 0, 0, 0 }, \
      lift, 0, 0, 0, 0, (uint64)(parameter) }
// The same for the floating point: the data type is the format
#define RVF(name, lift, category, units, properties, format, bits, parameter) \
    { RiscvSemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, OpcodeClassification::CATEGORY_##category, units, \
        properties, 0, 0, 0, 0 }, \
      lift, SemanticType::KIND_FLOAT, SemanticType::FLOAT_##format, bits, 1, \
      (uint64)(parameter) }

// The common shapes
#define RVE_ALU(name, lift, parameter) \
    RV(name, lift, ARITHMETIC, U_(INTEGER_ALU), 0, parameter)
#define RVE_LOGIC(name, lift, parameter) \
    RV(name, lift, LOGIC, U_(INTEGER_ALU), 0, parameter)
#define RVE_SHIFT(name, lift, parameter) \
    RV(name, lift, SHIFT, U_(SHIFTER), 0, parameter)
#define RVE_BITS(name, lift, parameter) \
    RV(name, lift, BIT_MANIPULATION, U_(INTEGER_ALU), 0, parameter)
#define RVE_MULT(name, lift, parameter) \
    RV(name, lift, MULTIPLY, U_(INTEGER_MULTIPLY), 0, parameter)
#define RVE_DIV(name, lift, parameter) \
    RV(name, lift, DIVIDE, U_(INTEGER_DIVIDE), 0, parameter)
#define RVE_CRYPTO(name, lift, parameter) \
    RV(name, lift, CRYPTO, U_(CRYPTO), 0, parameter)
#define RVE_LOAD(name, bits) \
    RV(name, liftLoad, LOAD, U_(LOAD), P_(READS_MEMORY), bits)
#define RVE_STORE(name, bits) \
    RV(name, liftStore, STORE, U_(STORE), P_(WRITES_MEMORY), bits)
#define RVE_BRANCH(name, condition) \
    RV(name, liftBranch, BRANCH, U_(BRANCH), P_(CONDITIONAL), \
       SemanticOperation::condition)
#define RVE_MOP(name) RV(name, liftMop, NOP, 0, 0, 0)
#define RVE_AMO(name, bits, operation) \
    RV(name, liftAtomicOperation, ATOMIC, U_(LOAD) | U_(STORE), \
       P_(ATOMIC) | P_(READS_MEMORY) | P_(WRITES_MEMORY), \
       (bits) | ((uint64)SemanticOperation::operation << 8))
#define RVE_AMOS(suffix, bits) \
    RVE_AMO("amoswap." suffix, bits, OP_ATOMIC_EXCHANGE), \
    RVE_AMO("amoadd." suffix, bits, OP_ADD), \
    RVE_AMO("amoxor." suffix, bits, OP_XOR), \
    RVE_AMO("amoand." suffix, bits, OP_AND), \
    RVE_AMO("amoor." suffix, bits, OP_OR), \
    RVE_AMO("amomin." suffix, bits, OP_MINIMUM_SIGNED), \
    RVE_AMO("amomax." suffix, bits, OP_MAXIMUM_SIGNED), \
    RVE_AMO("amominu." suffix, bits, OP_MINIMUM_UNSIGNED), \
    RVE_AMO("amomaxu." suffix, bits, OP_MAXIMUM_UNSIGNED)
#define RVE_CSR_ENTRY(name, lift, parameter) \
    RV(name, lift, SYSTEM, U_(SYSTEM), P_(NEEDS_HELPER), parameter)

// The formats
#define F_S SemanticType::FLOAT_SINGLE
#define F_D SemanticType::FLOAT_DOUBLE
#define F_H SemanticType::FLOAT_HALF
#define F_B SemanticType::FLOAT_BFLOAT16
#define OPF(operation) ((uint64)SemanticOperation::operation << 8)
#define FLAGS_FROM_BITS(bits, isUnsigned) \
    (((uint64)(bits) << 16) | ((uint64)(isUnsigned) << 8))

// The arithmetic of one floating point format
#define RVE_FLOAT_FORMAT(suffix, format, bits, formatName) \
    RVF("fmadd." suffix, liftFloatFused, FLOAT, U_(FLOAT_MULTIPLY), 0, formatName, bits, format | (0 << 8)), \
    RVF("fmsub." suffix, liftFloatFused, FLOAT, U_(FLOAT_MULTIPLY), 0, formatName, bits, format | (1 << 8)), \
    RVF("fnmsub." suffix, liftFloatFused, FLOAT, U_(FLOAT_MULTIPLY), 0, formatName, bits, format | (2 << 8)), \
    RVF("fnmadd." suffix, liftFloatFused, FLOAT, U_(FLOAT_MULTIPLY), 0, formatName, bits, format | (3 << 8)), \
    RVF("fadd." suffix, liftFloatArithmetic, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format | OPF(OP_FLOAT_ADD)), \
    RVF("fsub." suffix, liftFloatArithmetic, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format | OPF(OP_FLOAT_SUBTRACT)), \
    RVF("fmul." suffix, liftFloatArithmetic, FLOAT, U_(FLOAT_MULTIPLY), 0, formatName, bits, format | OPF(OP_FLOAT_MULTIPLY)), \
    RVF("fdiv." suffix, liftFloatArithmetic, FLOAT, U_(FLOAT_DIVIDE), 0, formatName, bits, format | OPF(OP_FLOAT_DIVIDE)), \
    RVF("fsqrt." suffix, liftFloatSquareRoot, FLOAT, U_(FLOAT_DIVIDE), 0, formatName, bits, format), \
    RVF("fsgnj." suffix, liftFloatSign, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format | (0 << 8)), \
    RVF("fsgnjn." suffix, liftFloatSign, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format | (1 << 8)), \
    RVF("fsgnjx." suffix, liftFloatSign, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format | (2 << 8)), \
    RVF("fmin." suffix, liftFloatMinMax, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format | OPF(OP_FLOAT_MINIMUM)), \
    RVF("fmax." suffix, liftFloatMinMax, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format | OPF(OP_FLOAT_MAXIMUM)), \
    RVF("feq." suffix, liftFloatCompare, COMPARE, U_(FLOAT_ADD), 0, formatName, bits, format | (0 << 8)), \
    RVF("flt." suffix, liftFloatCompare, COMPARE, U_(FLOAT_ADD), 0, formatName, bits, format | (1 << 8)), \
    RVF("fle." suffix, liftFloatCompare, COMPARE, U_(FLOAT_ADD), 0, formatName, bits, format | (2 << 8)), \
    RVF("fclass." suffix, liftFloatClassify, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format), \
    RVF("fcvt.w." suffix, liftFloatToInteger, CONVERT, U_(FLOAT_CONVERT), 0, formatName, bits, format | FLAGS_FROM_BITS(32, 0)), \
    RVF("fcvt.wu." suffix, liftFloatToInteger, CONVERT, U_(FLOAT_CONVERT), 0, formatName, bits, format | FLAGS_FROM_BITS(32, 1)), \
    RVF("fcvt.l." suffix, liftFloatToInteger, CONVERT, U_(FLOAT_CONVERT), 0, formatName, bits, format | FLAGS_FROM_BITS(64, 0)), \
    RVF("fcvt.lu." suffix, liftFloatToInteger, CONVERT, U_(FLOAT_CONVERT), 0, formatName, bits, format | FLAGS_FROM_BITS(64, 1)), \
    RVF("fcvt." suffix ".w", liftIntegerToFloat, CONVERT, U_(FLOAT_CONVERT), 0, formatName, bits, format | FLAGS_FROM_BITS(32, 0) ), \
    RVF("fcvt." suffix ".wu", liftIntegerToFloat, CONVERT, U_(FLOAT_CONVERT), 0, formatName, bits, format | FLAGS_FROM_BITS(32, 1) ), \
    RVF("fcvt." suffix ".l", liftIntegerToFloat, CONVERT, U_(FLOAT_CONVERT), 0, formatName, bits, format | FLAGS_FROM_BITS(64, 0)), \
    RVF("fcvt." suffix ".lu", liftIntegerToFloat, CONVERT, U_(FLOAT_CONVERT), 0, formatName, bits, format | FLAGS_FROM_BITS(64, 1))

// Zfa for one format
#define RVE_ZFA_FORMAT(suffix, format, bits, formatName) \
    RVF("fli." suffix, liftFloatLoadImmediate, MOVE, U_(FLOAT_ADD), 0, formatName, bits, format), \
    RVF("fminm." suffix, liftFloatMinMax, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format | OPF(OP_FLOAT_MINIMUM) | (1 << 16)), \
    RVF("fmaxm." suffix, liftFloatMinMax, FLOAT, U_(FLOAT_ADD), 0, formatName, bits, format | OPF(OP_FLOAT_MAXIMUM) | (1 << 16)), \
    RVF("fround." suffix, liftFloatRound, FLOAT, U_(FLOAT_CONVERT), 0, formatName, bits, format), \
    RVF("froundnx." suffix, liftFloatRound, FLOAT, U_(FLOAT_CONVERT), 0, formatName, bits, format | (1 << 8)), \
    RVF("fleq." suffix, liftFloatCompare, COMPARE, U_(FLOAT_ADD), 0, formatName, bits, format | (4 << 8)), \
    RVF("fltq." suffix, liftFloatCompare, COMPARE, U_(FLOAT_ADD), 0, formatName, bits, format | (3 << 8))

// A conversion between two floating point formats: the destination format
// | (the source format << 8)
#define RVE_CONVERT(name, to, toName, bits, from, flags) \
    RVF(name, liftFloatConvert, CONVERT, U_(FLOAT_CONVERT), 0, toName, bits, \
        (to) | ((uint64)(from) << 8) | (flags))

// The moves between the integer and the floating point registers
#define RVE_MOVE_FORMAT(suffix, format, bits, formatName, integerSuffix) \
    RVF("fmv.x." integerSuffix, liftFloatMoveToInteger, MOVE, U_(FLOAT_CONVERT), 0, formatName, bits, format), \
    RVF("fmv." integerSuffix ".x", liftFloatMoveFromInteger, MOVE, U_(FLOAT_CONVERT), 0, formatName, bits, format)

} // namespace

const RiscvSemanticEntry gRiscvSemanticTable[] =
{
    // ---- RV32I / RV64I ----
    RVE_ALU("add", liftBinary, SemanticOperation::OP_ADD),
    RVE_ALU("sub", liftBinary, SemanticOperation::OP_SUBTRACT),
    RVE_LOGIC("and", liftBinary, SemanticOperation::OP_AND),
    RVE_LOGIC("or", liftBinary, SemanticOperation::OP_OR),
    RVE_LOGIC("xor", liftBinary, SemanticOperation::OP_XOR),
    RVE_ALU("addi", liftBinary, SemanticOperation::OP_ADD),
    RVE_LOGIC("andi", liftBinary, SemanticOperation::OP_AND),
    RVE_LOGIC("ori", liftBinary, SemanticOperation::OP_OR),
    RVE_LOGIC("xori", liftBinary, SemanticOperation::OP_XOR),
    RVE_ALU("addw", liftBinaryWord, SemanticOperation::OP_ADD),
    RVE_ALU("subw", liftBinaryWord, SemanticOperation::OP_SUBTRACT),
    RVE_ALU("addiw", liftBinaryWord, SemanticOperation::OP_ADD),
    RVE_SHIFT("sll", liftShift, SemanticOperation::OP_SHIFT_LEFT),
    RVE_SHIFT("srl", liftShift, SemanticOperation::OP_SHIFT_RIGHT_LOGICAL),
    RVE_SHIFT("sra", liftShift, SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC),
    RVE_SHIFT("slli", liftShift, SemanticOperation::OP_SHIFT_LEFT),
    RVE_SHIFT("srli", liftShift, SemanticOperation::OP_SHIFT_RIGHT_LOGICAL),
    RVE_SHIFT("srai", liftShift, SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC),
    RVE_SHIFT("sllw", liftShiftWord, SemanticOperation::OP_SHIFT_LEFT),
    RVE_SHIFT("srlw", liftShiftWord, SemanticOperation::OP_SHIFT_RIGHT_LOGICAL),
    RVE_SHIFT("sraw", liftShiftWord, SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC),
    RVE_SHIFT("slliw", liftShiftWord, SemanticOperation::OP_SHIFT_LEFT),
    RVE_SHIFT("srliw", liftShiftWord, SemanticOperation::OP_SHIFT_RIGHT_LOGICAL),
    RVE_SHIFT("sraiw", liftShiftWord, SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC),
    RV("slt", liftSetLess, COMPARE, U_(INTEGER_ALU), 0, SemanticOperation::COND_SIGNED_LESS),
    RV("sltu", liftSetLess, COMPARE, U_(INTEGER_ALU), 0, SemanticOperation::COND_UNSIGNED_LESS),
    RV("slti", liftSetLess, COMPARE, U_(INTEGER_ALU), 0, SemanticOperation::COND_SIGNED_LESS),
    RV("sltiu", liftSetLess, COMPARE, U_(INTEGER_ALU), 0, SemanticOperation::COND_UNSIGNED_LESS),
    RV("lui", liftUpper, MOVE, U_(INTEGER_ALU), 0, 0),
    RV("auipc", liftUpper, ARITHMETIC, U_(INTEGER_ALU), 0, 1),
    RVE_LOAD("lb", 8),
    RVE_LOAD("lbu", 8 | LOAD_UNSIGNED),
    RVE_LOAD("lh", 16),
    RVE_LOAD("lhu", 16 | LOAD_UNSIGNED),
    RVE_LOAD("lw", 32),
    RVE_LOAD("lwu", 32 | LOAD_UNSIGNED),
    RVE_LOAD("ld", 64),
    RVE_STORE("sb", 8),
    RVE_STORE("sh", 16),
    RVE_STORE("sw", 32),
    RVE_STORE("sd", 64),
    RVE_BRANCH("beq", COND_EQUAL),
    RVE_BRANCH("bne", COND_NOT_EQUAL),
    RVE_BRANCH("blt", COND_SIGNED_LESS),
    RVE_BRANCH("bge", COND_SIGNED_GREATER_EQUAL),
    RVE_BRANCH("bltu", COND_UNSIGNED_LESS),
    RVE_BRANCH("bgeu", COND_UNSIGNED_GREATER_EQUAL),
    RV("jal", liftJal, BRANCH, U_(BRANCH), 0, 0),
    RV("jalr", liftJalr, BRANCH, U_(BRANCH), 0, 0),
    RV("fence", liftFence, ATOMIC, U_(SYSTEM), P_(SERIALIZING), 0),
    RV("fence.tso", liftFenceTso, ATOMIC, U_(SYSTEM), P_(SERIALIZING), 0),
    RV("fence.i", liftFenceInstruction, ATOMIC, U_(SYSTEM), P_(SERIALIZING), 0),
    RV("pause", liftPause, NOP, 0, 0, 0),
    RV("ecall", liftEcall, INTERRUPT, U_(SYSTEM), 0, 0),
    RV("ebreak", liftEbreak, INTERRUPT, U_(SYSTEM), 0, 0),
    RV("unimp", liftUnimp, INTERRUPT, U_(SYSTEM), 0, 0),

    // ---- M, Zmmul ----
    RVE_MULT("mul", liftBinary, SemanticOperation::OP_MULTIPLY),
    RVE_MULT("mulh", liftBinary, SemanticOperation::OP_MULTIPLY_HIGH_SIGNED),
    RVE_MULT("mulhsu", liftBinary, SemanticOperation::OP_MULTIPLY_HIGH_SIGNED_UNSIGNED),
    RVE_MULT("mulhu", liftBinary, SemanticOperation::OP_MULTIPLY_HIGH_UNSIGNED),
    RVE_MULT("mulw", liftBinaryWord, SemanticOperation::OP_MULTIPLY),
    RVE_DIV("div", liftDivide, SemanticOperation::OP_DIVIDE_SIGNED),
    RVE_DIV("divu", liftDivide, SemanticOperation::OP_DIVIDE_UNSIGNED),
    RVE_DIV("rem", liftDivide, SemanticOperation::OP_REMAINDER_SIGNED),
    RVE_DIV("remu", liftDivide, SemanticOperation::OP_REMAINDER_UNSIGNED),
    RVE_DIV("divw", liftDivideWord, SemanticOperation::OP_DIVIDE_SIGNED),
    RVE_DIV("divuw", liftDivideWord, SemanticOperation::OP_DIVIDE_UNSIGNED),
    RVE_DIV("remw", liftDivideWord, SemanticOperation::OP_REMAINDER_SIGNED),
    RVE_DIV("remuw", liftDivideWord, SemanticOperation::OP_REMAINDER_UNSIGNED),

    // ---- Zba ----
    RVE_ALU("sh1add", liftShiftAdd, 1),
    RVE_ALU("sh2add", liftShiftAdd, 2),
    RVE_ALU("sh3add", liftShiftAdd, 3),
    RVE_ALU("add.uw", liftShiftAdd, SHIFT_ADD_WORD),
    RVE_ALU("sh1add.uw", liftShiftAdd, 1 | SHIFT_ADD_WORD),
    RVE_ALU("sh2add.uw", liftShiftAdd, 2 | SHIFT_ADD_WORD),
    RVE_ALU("sh3add.uw", liftShiftAdd, 3 | SHIFT_ADD_WORD),
    RVE_SHIFT("slli.uw", liftSlliUw, 0),

    // ---- Zbb, Zbkb ----
    RVE_LOGIC("andn", liftLogicNegated, 0),
    RVE_LOGIC("orn", liftLogicNegated, 1),
    RVE_LOGIC("xnor", liftLogicNegated, 2),
    RVE_SHIFT("rol", liftRotate, SemanticOperation::OP_ROTATE_LEFT),
    RVE_SHIFT("ror", liftRotate, SemanticOperation::OP_ROTATE_RIGHT),
    RVE_SHIFT("rori", liftRotate, SemanticOperation::OP_ROTATE_RIGHT),
    RVE_SHIFT("rolw", liftRotateWord, SemanticOperation::OP_ROTATE_LEFT),
    RVE_SHIFT("rorw", liftRotateWord, SemanticOperation::OP_ROTATE_RIGHT),
    RVE_SHIFT("roriw", liftRotateWord, SemanticOperation::OP_ROTATE_RIGHT),
    RVE_BITS("rev8", liftByteSwap, 0),
    RVE_BITS("clz", liftBitCount, SemanticOperation::OP_COUNT_LEADING_ZEROS),
    RVE_BITS("ctz", liftBitCount, SemanticOperation::OP_COUNT_TRAILING_ZEROS),
    RVE_BITS("cpop", liftBitCount, SemanticOperation::OP_POPULATION_COUNT),
    RVE_BITS("clzw", liftBitCountWord, SemanticOperation::OP_COUNT_LEADING_ZEROS),
    RVE_BITS("ctzw", liftBitCountWord, SemanticOperation::OP_COUNT_TRAILING_ZEROS),
    RVE_BITS("cpopw", liftBitCountWord, SemanticOperation::OP_POPULATION_COUNT),
    RVE_BITS("sext.b", liftSignExtendBits, 8),
    RVE_BITS("sext.h", liftSignExtendBits, 16),
    RVE_BITS("zext.h", liftZeroExtendHalf, 0),
    RVE_BITS("orc.b", liftOrCombineBytes, 0),
    RVE_ALU("max", liftBinary, SemanticOperation::OP_MAXIMUM_SIGNED),
    RVE_ALU("maxu", liftBinary, SemanticOperation::OP_MAXIMUM_UNSIGNED),
    RVE_ALU("min", liftBinary, SemanticOperation::OP_MINIMUM_SIGNED),
    RVE_ALU("minu", liftBinary, SemanticOperation::OP_MINIMUM_UNSIGNED),
    RVE_BITS("pack", liftPack, 0),
    RVE_BITS("packh", liftPack, 1),
    RVE_BITS("packw", liftPack, 2),
    RVE_BITS("brev8", liftReverseBitsInBytes, 0),
    RVE_BITS("zip", liftZip, 0),
    RVE_BITS("unzip", liftZip, 1),
    RVE_BITS("xperm4", liftCrossbar, 4),
    RVE_BITS("xperm8", liftCrossbar, 8),

    // ---- Zbs ----
    RVE_BITS("bclr", liftSingleBit, 0),
    RVE_BITS("bext", liftSingleBit, 1),
    RVE_BITS("binv", liftSingleBit, 2),
    RVE_BITS("bset", liftSingleBit, 3),
    RVE_BITS("bclri", liftSingleBit, 0),
    RVE_BITS("bexti", liftSingleBit, 1),
    RVE_BITS("binvi", liftSingleBit, 2),
    RVE_BITS("bseti", liftSingleBit, 3),

    // ---- Zbc, Zbkc ----
    RVE_MULT("clmul", liftCarryless, 0),
    RVE_MULT("clmulh", liftCarryless, 1),
    RVE_MULT("clmulr", liftCarryless, 2),

    // ---- Zicond, Zimop ----
    RV("czero.eqz", liftConditionalZero, CONDITIONAL_MOVE, U_(INTEGER_ALU), 0, 0),
    RV("czero.nez", liftConditionalZero, CONDITIONAL_MOVE, U_(INTEGER_ALU), 0, 1),
    RV("sspush", liftShadowStackPush, SECURITY, U_(STORE), P_(WRITES_MEMORY), 0),
    RV("sspopchk", liftShadowStackPopCheck, SECURITY, U_(LOAD), P_(READS_MEMORY), 0),
    RV("ssrdp", liftShadowStackRead, SECURITY, U_(INTEGER_ALU), 0, 0),
    RVE_MOP("mop.r.0"), RVE_MOP("mop.r.1"), RVE_MOP("mop.r.2"), RVE_MOP("mop.r.3"),
    RVE_MOP("mop.r.4"), RVE_MOP("mop.r.5"), RVE_MOP("mop.r.6"), RVE_MOP("mop.r.7"),
    RVE_MOP("mop.r.8"), RVE_MOP("mop.r.9"), RVE_MOP("mop.r.10"), RVE_MOP("mop.r.11"),
    RVE_MOP("mop.r.12"), RVE_MOP("mop.r.13"), RVE_MOP("mop.r.14"), RVE_MOP("mop.r.15"),
    RVE_MOP("mop.r.16"), RVE_MOP("mop.r.17"), RVE_MOP("mop.r.18"), RVE_MOP("mop.r.19"),
    RVE_MOP("mop.r.20"), RVE_MOP("mop.r.21"), RVE_MOP("mop.r.22"), RVE_MOP("mop.r.23"),
    RVE_MOP("mop.r.24"), RVE_MOP("mop.r.25"), RVE_MOP("mop.r.26"), RVE_MOP("mop.r.27"),
    RVE_MOP("mop.r.28"), RVE_MOP("mop.r.29"), RVE_MOP("mop.r.30"), RVE_MOP("mop.r.31"),
    RVE_MOP("mop.rr.0"), RVE_MOP("mop.rr.1"), RVE_MOP("mop.rr.2"), RVE_MOP("mop.rr.3"),
    RVE_MOP("mop.rr.4"), RVE_MOP("mop.rr.5"), RVE_MOP("mop.rr.6"), RVE_MOP("mop.rr.7"),

    // ---- Zicbo*, Zawrs, Zicfilp ----
    RV("prefetch.i", liftPrefetch, CACHE, 0, 0, 0),
    RV("prefetch.r", liftPrefetch, CACHE, 0, 0, 0),
    RV("prefetch.w", liftPrefetch, CACHE, 0, 0, 0),
    RV("cbo.clean", liftCacheBlock, CACHE, U_(LOAD), P_(READS_MEMORY), SemanticOperation::CACHE_CLEAN),
    RV("cbo.flush", liftCacheBlock, CACHE, U_(LOAD), P_(READS_MEMORY), SemanticOperation::CACHE_FLUSH),
    RV("cbo.inval", liftCacheBlock, CACHE, U_(LOAD), P_(READS_MEMORY), SemanticOperation::CACHE_INVALIDATE),
    RV("cbo.zero", liftCacheBlockZero, CACHE, U_(STORE), P_(WRITES_MEMORY), 0),
    RV("wrs.nto", liftWaitReservation, NOP, 0, 0, 0),
    RV("wrs.sto", liftWaitReservation, NOP, 0, 0, 0),
    RV("lpad", liftLandingPad, SECURITY, 0, P_(LANDING_PAD), 0),

    // ---- Zicsr ----
    RVE_CSR_ENTRY("csrrw", liftCsr, 0),
    RVE_CSR_ENTRY("csrrs", liftCsr, 1),
    RVE_CSR_ENTRY("csrrc", liftCsr, 2),
    RVE_CSR_ENTRY("csrrwi", liftCsr, 0 | CSR_IMMEDIATE),
    RVE_CSR_ENTRY("csrrsi", liftCsr, 1 | CSR_IMMEDIATE),
    RVE_CSR_ENTRY("csrrci", liftCsr, 2 | CSR_IMMEDIATE),

    // ---- Zalrsc, Zaamo, Zabha, Zacas, Zicfiss ----
    RV("lr.w", liftLoadReserved, ATOMIC, U_(LOAD), P_(EXCLUSIVE) | P_(READS_MEMORY), 32),
    RV("lr.d", liftLoadReserved, ATOMIC, U_(LOAD), P_(EXCLUSIVE) | P_(READS_MEMORY), 64),
    RV("sc.w", liftStoreConditional, ATOMIC, U_(STORE), P_(EXCLUSIVE) | P_(WRITES_MEMORY), 32),
    RV("sc.d", liftStoreConditional, ATOMIC, U_(STORE), P_(EXCLUSIVE) | P_(WRITES_MEMORY), 64),
    RVE_AMOS("w", 32),
    RVE_AMOS("d", 64),
    RVE_AMOS("b", 8),
    RVE_AMOS("h", 16),
    RV("amocas.w", liftCompareAndSwap, ATOMIC, U_(LOAD) | U_(STORE), P_(ATOMIC) | P_(READS_MEMORY) | P_(WRITES_MEMORY), 32),
    RV("amocas.d", liftCompareAndSwap, ATOMIC, U_(LOAD) | U_(STORE), P_(ATOMIC) | P_(READS_MEMORY) | P_(WRITES_MEMORY), 64),
    RV("amocas.b", liftCompareAndSwap, ATOMIC, U_(LOAD) | U_(STORE), P_(ATOMIC) | P_(READS_MEMORY) | P_(WRITES_MEMORY), 8),
    RV("amocas.h", liftCompareAndSwap, ATOMIC, U_(LOAD) | U_(STORE), P_(ATOMIC) | P_(READS_MEMORY) | P_(WRITES_MEMORY), 16),
    RV("amocas.q", liftCompareAndSwap, ATOMIC, U_(LOAD) | U_(STORE), P_(ATOMIC) | P_(READS_MEMORY) | P_(WRITES_MEMORY), 128),
    RV("ssamoswap.w", liftShadowStackSwap, SECURITY, U_(LOAD) | U_(STORE), P_(ATOMIC) | P_(READS_MEMORY) | P_(WRITES_MEMORY), 32 | ((uint64)SemanticOperation::OP_ATOMIC_EXCHANGE << 8)),
    RV("ssamoswap.d", liftShadowStackSwap, SECURITY, U_(LOAD) | U_(STORE), P_(ATOMIC) | P_(READS_MEMORY) | P_(WRITES_MEMORY), 64 | ((uint64)SemanticOperation::OP_ATOMIC_EXCHANGE << 8)),

    // ---- F, D, Zfh, Zfhmin, Zfbfmin ----
    RVF("flh", liftFloatLoad, LOAD, U_(LOAD), P_(READS_MEMORY), HALF, 16, F_H),
    RVF("flw", liftFloatLoad, LOAD, U_(LOAD), P_(READS_MEMORY), SINGLE, 32, F_S),
    RVF("fld", liftFloatLoad, LOAD, U_(LOAD), P_(READS_MEMORY), DOUBLE, 64, F_D),
    RVF("fsh", liftFloatStore, STORE, U_(STORE), P_(WRITES_MEMORY), HALF, 16, F_H),
    RVF("fsw", liftFloatStore, STORE, U_(STORE), P_(WRITES_MEMORY), SINGLE, 32, F_S),
    RVF("fsd", liftFloatStore, STORE, U_(STORE), P_(WRITES_MEMORY), DOUBLE, 64, F_D),
    RVE_FLOAT_FORMAT("s", F_S, 32, SINGLE),
    RVE_FLOAT_FORMAT("d", F_D, 64, DOUBLE),
    RVE_FLOAT_FORMAT("h", F_H, 16, HALF),
    RVE_MOVE_FORMAT("s", F_S, 32, SINGLE, "w"),
    RVE_MOVE_FORMAT("d", F_D, 64, DOUBLE, "d"),
    RVE_MOVE_FORMAT("h", F_H, 16, HALF, "h"),
    RVE_CONVERT("fcvt.s.d", F_S, SINGLE, 32, F_D, 0),
    RVE_CONVERT("fcvt.d.s", F_D, DOUBLE, 64, F_S, 0),
    RVE_CONVERT("fcvt.s.h", F_S, SINGLE, 32, F_H, 0),
    RVE_CONVERT("fcvt.h.s", F_H, HALF, 16, F_S, 0),
    RVE_CONVERT("fcvt.d.h", F_D, DOUBLE, 64, F_H, 0),
    RVE_CONVERT("fcvt.h.d", F_H, HALF, 16, F_D, 0),
    RVE_CONVERT("fcvt.s.bf16", F_S, SINGLE, 32, F_B, 0),
    RVE_CONVERT("fcvt.bf16.s", F_B, BFLOAT16, 16, F_S, 0),

    // ---- Zfa ----
    RVE_ZFA_FORMAT("s", F_S, 32, SINGLE),
    RVE_ZFA_FORMAT("d", F_D, 64, DOUBLE),
    RVE_ZFA_FORMAT("h", F_H, 16, HALF),
    RVF("fcvtmod.w.d", liftFloatConvertModular, CONVERT, U_(FLOAT_CONVERT), 0, DOUBLE, 64, 0),
    RV("fmvh.x.d", liftFloatMovePair, MOVE, U_(FLOAT_CONVERT), 0, 0),
    RV("fmvp.d.x", liftFloatMovePair, MOVE, U_(FLOAT_CONVERT), 0, 1),

    // ---- Zknd, Zkne, Zknh, Zksed, Zksh ----
    RVE_CRYPTO("aes32esi", liftAes32, 0),
    RVE_CRYPTO("aes32dsi", liftAes32, 1),
    RVE_CRYPTO("aes32esmi", liftAes32, 2),
    RVE_CRYPTO("aes32dsmi", liftAes32, 3),
    RVE_CRYPTO("aes64es", liftAes64, 0),
    RVE_CRYPTO("aes64esm", liftAes64, 1),
    RVE_CRYPTO("aes64ds", liftAes64, 2),
    RVE_CRYPTO("aes64dsm", liftAes64, 3),
    RVE_CRYPTO("aes64im", liftAes64, 4),
    RVE_CRYPTO("aes64ks1i", liftAes64KeySchedule, 0),
    RVE_CRYPTO("aes64ks2", liftAes64KeySchedule, 1),
    RVE_CRYPTO("sha256sig0", liftSha256, 0),
    RVE_CRYPTO("sha256sig1", liftSha256, 1),
    RVE_CRYPTO("sha256sum0", liftSha256, 2),
    RVE_CRYPTO("sha256sum1", liftSha256, 3),
    RVE_CRYPTO("sha512sig0", liftSha512, 0),
    RVE_CRYPTO("sha512sig1", liftSha512, 1),
    RVE_CRYPTO("sha512sum0", liftSha512, 2),
    RVE_CRYPTO("sha512sum1", liftSha512, 3),
    RVE_CRYPTO("sha512sig0l", liftSha512Rv32, 0),
    RVE_CRYPTO("sha512sig0h", liftSha512Rv32, 1),
    RVE_CRYPTO("sha512sig1l", liftSha512Rv32, 2),
    RVE_CRYPTO("sha512sig1h", liftSha512Rv32, 3),
    RVE_CRYPTO("sha512sum0r", liftSha512Rv32, 4),
    RVE_CRYPTO("sha512sum1r", liftSha512Rv32, 5),
    RVE_CRYPTO("sm3p0", liftSm3, 0),
    RVE_CRYPTO("sm3p1", liftSm3, 1),
    RV("sm4ed", liftSm4, CRYPTO, U_(CRYPTO), P_(NEEDS_HELPER), 0),
    RV("sm4ks", liftSm4, CRYPTO, U_(CRYPTO), P_(NEEDS_HELPER), 1),

    // The end of the table
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0, 0 }
};

} // namespace riscvdis
