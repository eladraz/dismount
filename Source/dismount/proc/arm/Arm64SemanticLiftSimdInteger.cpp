/*
 * Arm64SemanticLiftSimdInteger.cpp
 *
 * Implementation file
 *
 * The A64 lifter of the Advanced SIMD integer, logic, shift, permute, narrow
 * and widen instructions, see Arm64SemanticSimd.h. The semantics are the ones
 * of the pseudocode of the Arm ARM.
 *
 * The values are integers: a register is read whole (an integer of 128 bits for
 * a vector) and a lane is extracted with a bit offset, the result is built by
 * inserting lanes into an integer of 128 bits. Every lane is computed on
 * scalars of up to 64 bits (the interpreter's lane operations work up to 64
 * bits), so the saturation, the rounding and the widening are written per lane.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64SemanticInternal.h"
#include "dismount/proc/arm/Arm64SemanticSimd.h"

using namespace armdis;

typedef SemanticOperation Op;
typedef SimdLift::Val Val;

namespace {

/////////////////////////////////////////////////////////////////////////////
// The scalar toolbox
/////////////////////////////////////////////////////////////////////////////

// The largest / smallest signed and the largest unsigned value of 'w' bits
int64 signedMaximum(uint w)
{
    return (w >= 64) ? (int64)0x7FFFFFFFFFFFFFFFLL
                     : (int64)(((uint64)1 << (w - 1)) - 1);
}

int64 signedMinimum(uint w)
{
    return -signedMaximum(w) - 1;
}

int64 unsignedMaximum(uint w)
{
    return (w >= 64) ? (int64)-1 : (int64)(((uint64)1 << w) - 1);
}

// The emitter of the scalar operations, and the accumulated saturation flag
class X {
public:
    explicit X(SimdLift& l) : s(l), c(l.c()) {}

    SimdLift& s;
    ArmLiftContext& c;
    // The OR of the saturation flags of the lanes (a 1 bit), or invalid
    Val qc;

    static SemanticType I(uint bits) { return SemanticType::integer(bits); }

    Val k(int64 value, uint bits) { return s.imm(value, I(bits)); }

    // An operation with the type of its first source
    Val o(Op::Operation operation, const Val& a, const Val& b = Val(),
          const Val& d = Val())
    {
        return s.op(operation, a.getType(), a, b, d);
    }

    Val zx(const Val& a, uint bits) { return s.zext(bits, a); }
    Val sx(const Val& a, uint bits) { return s.sext(bits, a); }
    Val tr(const Val& a, uint bits) { return s.trunc(bits, a); }
    Val ext(const Val& a, uint bits, bool isSigned)
    {
        return isSigned ? sx(a, bits) : zx(a, bits);
    }

    Val cmp(Op::Condition condition, const Val& a, const Val& b)
    {
        return s.compare(condition, a.getType(), a, b);
    }

    Val sel(const Val& condition, const Val& a, const Val& b)
    {
        return s.select(condition, a, b);
    }

    // All ones if the condition holds, zero otherwise
    Val mask(const Val& condition, uint bits)
    {
        return sel(condition, k(-1, bits), k(0, bits));
    }

    Val orFlag(const Val& a, const Val& b)
    {
        return s.op(Op::OP_OR, I(1), a, b);
    }

    // Remember that a lane saturated
    void sat(const Val& flag)
    {
        qc = qc.isValid() ? orFlag(qc, flag) : flag;
    }

    // Set FPSR.QC if a lane saturated
    void finish()
    {
        if (qc.isValid())
            s.setSaturation(qc);
    }

    Val lane(const Val& reg, uint index, uint bits)
    {
        return c.emit(Op::OP_EXTRACT, I(bits), reg, s.imm((int64)index * bits, I(32)));
    }

    Val put(const Val& accumulator, const Val& value, uint index, uint bits)
    {
        return c.emit(Op::OP_INSERT, I(128), accumulator, value,
                      s.imm((int64)index * bits, I(32)));
    }

    // The right shift of the signed or unsigned value by 'count' (the same
    // type); 'isRounding' adds the rounding bit
    Val shiftRight(const Val& a, const Val& count, bool isSigned,
                   bool isRounding)
    {
        if (!isRounding)
            return o(isSigned ? Op::OP_SHIFT_RIGHT_ARITHMETIC :
                                Op::OP_SHIFT_RIGHT_LOGICAL, a, count);
        Val result = c.newTemporary(a.getType());
        SemanticInstruction instruction(Op::OP_SHIFT_RIGHT_ROUNDING,
                                        a.getType(), result, a, count);
        instruction.setBehavior(isSigned ? Op::BEHAVIOR_SIGNED :
                                           Op::BEHAVIOR_UNSIGNED);
        c.emit(instruction);
        return result;
    }

    // Clamp the signed 64 bit value to the signed range of 'w' bits
    Val clampSigned(const Val& v, uint w)
    {
        Val high = k(signedMaximum(w), 64);
        Val low = k(signedMinimum(w), 64);
        Val isHigh = cmp(Op::COND_SIGNED_GREATER, v, high);
        Val isLow = cmp(Op::COND_SIGNED_LESS, v, low);
        sat(orFlag(isHigh, isLow));
        return tr(sel(isHigh, high, sel(isLow, low, v)), w);
    }

    // Clamp the signed 64 bit value to the unsigned range of 'w' bits
    Val clampSignedToUnsigned(const Val& v, uint w)
    {
        Val high = k(unsignedMaximum(w), 64);
        Val zero = k(0, 64);
        Val isHigh = cmp(Op::COND_SIGNED_GREATER, v, high);
        Val isLow = cmp(Op::COND_SIGNED_LESS, v, zero);
        sat(orFlag(isHigh, isLow));
        return tr(sel(isHigh, high, sel(isLow, zero, v)), w);
    }

    // Clamp the unsigned 64 bit value to the unsigned range of 'w' bits
    Val clampUnsigned(const Val& v, uint w)
    {
        Val high = k(unsignedMaximum(w), 64);
        Val isHigh = cmp(Op::COND_UNSIGNED_GREATER, v, high);
        sat(isHigh);
        return tr(sel(isHigh, high, v), w);
    }
};

/////////////////////////////////////////////////////////////////////////////
// The operations
/////////////////////////////////////////////////////////////////////////////

enum {
    // Same size lanes: a = Vn, b = Vm (or the element), acc = Vd
    ID_ADD, ID_SUB, ID_MUL, ID_MLA, ID_MLS,
    ID_CMEQ, ID_CMGE, ID_CMGT, ID_CMHI, ID_CMHS, ID_CMTST, ID_CMLE, ID_CMLT,
    ID_SMAX, ID_SMIN, ID_UMAX, ID_UMIN,
    ID_SABD, ID_UABD, ID_SABA, ID_UABA,
    ID_SHADD, ID_UHADD, ID_SRHADD, ID_URHADD, ID_SHSUB, ID_UHSUB,
    ID_SQADD, ID_UQADD, ID_SQSUB, ID_UQSUB,
    ID_SSHL, ID_USHL, ID_SRSHL, ID_URSHL, ID_SQSHL, ID_UQSHL, ID_SQRSHL,
    ID_UQRSHL,
    ID_SQDMULH, ID_SQRDMULH, ID_SQRDMLAH, ID_SQRDMLSH, ID_PMUL,
    ID_SAME_END,
    // One source
    ID_ABS, ID_NEG, ID_SQABS, ID_SQNEG, ID_CLS, ID_CLZ, ID_CNT, ID_RBIT,
    ID_NOT, ID_SUQADD, ID_USQADD,
    ID_UNARY_END,
    // The long operations (the destination is twice the source)
    ID_SADDL, ID_UADDL, ID_SSUBL, ID_USUBL, ID_SABDL, ID_UABDL,
    ID_SABAL, ID_UABAL, ID_SMULL, ID_UMULL, ID_SMLAL, ID_UMLAL, ID_SMLSL,
    ID_UMLSL, ID_SQDMULL, ID_SQDMLAL, ID_SQDMLSL,
    ID_SADDW, ID_UADDW, ID_SSUBW, ID_USUBW,
    ID_LONG_END
};

struct Name {
    const char* m_name;
    int m_id;
};

const Name gNames[] = {
    { "add", ID_ADD }, { "sub", ID_SUB }, { "mul", ID_MUL }, { "mla", ID_MLA },
    { "mls", ID_MLS }, { "cmeq", ID_CMEQ }, { "cmge", ID_CMGE },
    { "cmgt", ID_CMGT }, { "cmhi", ID_CMHI }, { "cmhs", ID_CMHS },
    { "cmtst", ID_CMTST }, { "cmle", ID_CMLE }, { "cmlt", ID_CMLT },
    { "smax", ID_SMAX }, { "smin", ID_SMIN }, { "umax", ID_UMAX },
    { "umin", ID_UMIN }, { "sabd", ID_SABD }, { "uabd", ID_UABD },
    { "saba", ID_SABA }, { "uaba", ID_UABA }, { "shadd", ID_SHADD },
    { "uhadd", ID_UHADD }, { "srhadd", ID_SRHADD }, { "urhadd", ID_URHADD },
    { "shsub", ID_SHSUB }, { "uhsub", ID_UHSUB }, { "sqadd", ID_SQADD },
    { "uqadd", ID_UQADD }, { "sqsub", ID_SQSUB }, { "uqsub", ID_UQSUB },
    { "sshl", ID_SSHL }, { "ushl", ID_USHL }, { "srshl", ID_SRSHL },
    { "urshl", ID_URSHL }, { "sqshl", ID_SQSHL }, { "uqshl", ID_UQSHL },
    { "sqrshl", ID_SQRSHL }, { "uqrshl", ID_UQRSHL },
    { "sqdmulh", ID_SQDMULH }, { "sqrdmulh", ID_SQRDMULH },
    { "sqrdmlah", ID_SQRDMLAH }, { "sqrdmlsh", ID_SQRDMLSH },
    { "pmul", ID_PMUL },
    { "abs", ID_ABS }, { "neg", ID_NEG }, { "sqabs", ID_SQABS },
    { "sqneg", ID_SQNEG }, { "cls", ID_CLS }, { "clz", ID_CLZ },
    { "cnt", ID_CNT }, { "rbit", ID_RBIT }, { "not", ID_NOT },
    { "mvn", ID_NOT }, { "suqadd", ID_SUQADD }, { "usqadd", ID_USQADD },
    { "saddl", ID_SADDL }, { "uaddl", ID_UADDL }, { "ssubl", ID_SSUBL },
    { "usubl", ID_USUBL }, { "sabdl", ID_SABDL }, { "uabdl", ID_UABDL },
    { "sabal", ID_SABAL }, { "uabal", ID_UABAL }, { "smull", ID_SMULL },
    { "umull", ID_UMULL }, { "smlal", ID_SMLAL }, { "umlal", ID_UMLAL },
    { "smlsl", ID_SMLSL }, { "umlsl", ID_UMLSL }, { "sqdmull", ID_SQDMULL },
    { "sqdmlal", ID_SQDMLAL }, { "sqdmlsl", ID_SQDMLSL },
    { "saddw", ID_SADDW }, { "uaddw", ID_UADDW }, { "ssubw", ID_SSUBW },
    { "usubw", ID_USUBW },
    { NULL, 0 }
};

// Return the id of the mnemonic, and set 'isUpper' for the "2" forms
int findId(const char* name, bool& isUpper)
{
    isUpper = false;
    for (const Name* n = gNames; n->m_name != NULL; n++)
    {
        if (strcmp(n->m_name, name) == 0)
            return n->m_id;
    }
    // The upper half forms: smull2, saddw2 ...
    const size_t length = strlen(name);
    if ((length > 1) && (name[length - 1] == '2'))
    {
        for (const Name* n = gNames; n->m_name != NULL; n++)
        {
            if ((n->m_id > ID_UNARY_END) && (strlen(n->m_name) == length - 1) &&
                (strncmp(n->m_name, name, length - 1) == 0))
            {
                isUpper = true;
                return n->m_id;
            }
        }
    }
    return -1;
}

/////////////////////////////////////////////////////////////////////////////
// The operations on one lane of up to 64 bits
/////////////////////////////////////////////////////////////////////////////

// The shift by a register (sshl, ushl, srshl, urshl, sqshl, uqshl, sqrshl,
// uqrshl): the count is the signed low byte of 'b'
Val registerShift(X& x, const Val& a, const Val& b, uint e, bool isSigned,
                  bool isRounding, bool isSaturating)
{
    Val sh = x.sx(x.tr(b, 8), 64);
    Val isNegative = x.cmp(Op::COND_SIGNED_LESS, sh, x.k(0, 64));
    Val count = x.o(Op::OP_SUBTRACT, x.k(0, 64), sh);
    Val wide = x.ext(a, 64, isSigned);
    Val right = x.tr(x.shiftRight(wide, count, isSigned, isRounding), e);
    Val left64 = x.o(Op::OP_SHIFT_LEFT, wide, sh);
    Val left = x.tr(left64, e);
    if (isSaturating)
    {
        Val back = x.shiftRight(left64, sh, isSigned, false);
        Val isRoundTrip = x.cmp(Op::COND_EQUAL, back, wide);
        Val isFit = x.cmp(Op::COND_EQUAL, x.ext(left, 64, isSigned), left64);
        Val isOverflow = x.s.op(Op::OP_AND, SemanticType::integer(1),
            x.s.op(Op::OP_NOT, SemanticType::integer(1),
                   x.s.op(Op::OP_AND, SemanticType::integer(1), isRoundTrip,
                          isFit)),
            x.s.op(Op::OP_NOT, SemanticType::integer(1), isNegative));
        Val saturated;
        if (isSigned)
        {
            Val isLow = x.cmp(Op::COND_SIGNED_LESS, a, x.k(0, e));
            saturated = x.sel(isLow, x.k(signedMinimum(e), e),
                              x.k(signedMaximum(e), e));
        } else
        {
            saturated = x.k(unsignedMaximum(e), e);
        }
        x.sat(isOverflow);
        left = x.sel(isOverflow, saturated, left);
    }
    return x.sel(isNegative, right, left);
}

// The signed halving add / subtract and the unsigned ones, rounded or not
Val halving(X& x, const Val& a, const Val& b, uint e, bool isSigned,
            bool isRounding, bool isSubtract)
{
    Val one = x.k(1, e);
    Val ha = x.shiftRight(a, one, isSigned, false);
    Val hb = x.shiftRight(b, one, isSigned, false);
    if (isSubtract)
    {
        // floor((a - b) / 2) = a/2 - b/2 - (a even and b odd)
        Val borrow = x.o(Op::OP_AND, x.o(Op::OP_AND_NOT, a, b), one);
        return x.o(Op::OP_SUBTRACT, x.o(Op::OP_SUBTRACT, ha, hb), borrow);
    }
    Val carry = isRounding ? x.o(Op::OP_AND, x.o(Op::OP_OR, a, b), one)
                           : x.o(Op::OP_AND, x.o(Op::OP_AND, a, b), one);
    return x.o(Op::OP_ADD, x.o(Op::OP_ADD, ha, hb), carry);
}

// sqdmulh / sqrdmulh, and the product of sqrdmlah / sqrdmlsh: the doubled
// product, rounded, shifted, as the high half
Val doublingHigh(X& x, const Val& a, const Val& b, uint e, bool isRounding,
                 bool isNegated)
{
    Val product = x.o(Op::OP_MULTIPLY, x.sx(a, 64), x.sx(b, 64));
    if (isNegated)
        product = x.o(Op::OP_NEGATE, product);
    if (isRounding)
        product = x.o(Op::OP_ADD, product, x.k((int64)1 << (e - 2), 64));
    return x.shiftRight(product, x.k(e - 1, 64), true, false);
}

Val calcSame(X& x, int id, const Val& a, const Val& b, const Val& acc, uint e)
{
    typedef SemanticOperation O;
    switch (id)
    {
    case ID_ADD: return x.o(O::OP_ADD, a, b);
    case ID_SUB: return x.o(O::OP_SUBTRACT, a, b);
    case ID_MUL: return x.o(O::OP_MULTIPLY, a, b);
    case ID_MLA: return x.o(O::OP_ADD, acc, x.o(O::OP_MULTIPLY, a, b));
    case ID_MLS: return x.o(O::OP_SUBTRACT, acc, x.o(O::OP_MULTIPLY, a, b));
    case ID_CMEQ: return x.mask(x.cmp(O::COND_EQUAL, a, b), e);
    case ID_CMGE: return x.mask(x.cmp(O::COND_SIGNED_GREATER_EQUAL, a, b), e);
    case ID_CMGT: return x.mask(x.cmp(O::COND_SIGNED_GREATER, a, b), e);
    case ID_CMHI: return x.mask(x.cmp(O::COND_UNSIGNED_GREATER, a, b), e);
    case ID_CMHS: return x.mask(x.cmp(O::COND_UNSIGNED_GREATER_EQUAL, a, b), e);
    case ID_CMLE: return x.mask(x.cmp(O::COND_SIGNED_LESS_EQUAL, a, b), e);
    case ID_CMLT: return x.mask(x.cmp(O::COND_SIGNED_LESS, a, b), e);
    case ID_CMTST:
        return x.mask(x.cmp(O::COND_NOT_EQUAL, x.o(O::OP_AND, a, b), x.k(0, e)), e);
    case ID_SMAX: return x.o(O::OP_MAXIMUM_SIGNED, a, b);
    case ID_SMIN: return x.o(O::OP_MINIMUM_SIGNED, a, b);
    case ID_UMAX: return x.o(O::OP_MAXIMUM_UNSIGNED, a, b);
    case ID_UMIN: return x.o(O::OP_MINIMUM_UNSIGNED, a, b);
    case ID_SABD: return x.o(O::OP_ABSOLUTE_DIFFERENCE_SIGNED, a, b);
    case ID_UABD: return x.o(O::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, a, b);
    case ID_SABA:
        return x.o(O::OP_ADD, acc, x.o(O::OP_ABSOLUTE_DIFFERENCE_SIGNED, a, b));
    case ID_UABA:
        return x.o(O::OP_ADD, acc, x.o(O::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, a, b));
    case ID_SHADD: return halving(x, a, b, e, true, false, false);
    case ID_UHADD: return halving(x, a, b, e, false, false, false);
    case ID_SRHADD: return halving(x, a, b, e, true, true, false);
    case ID_URHADD: return halving(x, a, b, e, false, true, false);
    case ID_SHSUB: return halving(x, a, b, e, true, false, true);
    case ID_UHSUB: return halving(x, a, b, e, false, false, true);
    case ID_SQADD:
    {
        Val r = x.o(O::OP_ADD_SATURATE_SIGNED, a, b);
        x.sat(x.cmp(O::COND_NOT_EQUAL, r, x.o(O::OP_ADD, a, b)));
        return r;
    }
    case ID_UQADD:
    {
        Val r = x.o(O::OP_ADD_SATURATE_UNSIGNED, a, b);
        x.sat(x.cmp(O::COND_NOT_EQUAL, r, x.o(O::OP_ADD, a, b)));
        return r;
    }
    case ID_SQSUB:
    {
        Val r = x.o(O::OP_SUBTRACT_SATURATE_SIGNED, a, b);
        x.sat(x.cmp(O::COND_NOT_EQUAL, r, x.o(O::OP_SUBTRACT, a, b)));
        return r;
    }
    case ID_UQSUB:
    {
        Val r = x.o(O::OP_SUBTRACT_SATURATE_UNSIGNED, a, b);
        x.sat(x.cmp(O::COND_NOT_EQUAL, r, x.o(O::OP_SUBTRACT, a, b)));
        return r;
    }
    case ID_SSHL: return registerShift(x, a, b, e, true, false, false);
    case ID_USHL: return registerShift(x, a, b, e, false, false, false);
    case ID_SRSHL: return registerShift(x, a, b, e, true, true, false);
    case ID_URSHL: return registerShift(x, a, b, e, false, true, false);
    case ID_SQSHL: return registerShift(x, a, b, e, true, false, true);
    case ID_UQSHL: return registerShift(x, a, b, e, false, false, true);
    case ID_SQRSHL: return registerShift(x, a, b, e, true, true, true);
    case ID_UQRSHL: return registerShift(x, a, b, e, false, true, true);
    case ID_SQDMULH:
    case ID_SQRDMULH:
    {
        Val r = doublingHigh(x, a, b, e, id == ID_SQRDMULH, false);
        // Only the minimum * the minimum overflows
        Val high = x.k((int64)1 << (e - 1), 64);
        Val isOverflow = x.cmp(O::COND_EQUAL, r, high);
        x.sat(isOverflow);
        return x.sel(isOverflow, x.k(signedMaximum(e), e), x.tr(r, e));
    }
    case ID_SQRDMLAH:
    case ID_SQRDMLSH:
    {
        Val r = doublingHigh(x, a, b, e, true, id == ID_SQRDMLSH);
        return x.clampSigned(x.o(O::OP_ADD, x.sx(acc, 64), r), e);
    }
    case ID_PMUL: return x.o(O::OP_CARRYLESS_MULTIPLY, a, b);
    default:
        CHECK_FAIL();
    }
}

// The operations on one source (and the accumulator of suqadd, usqadd)
Val calcUnary(X& x, int id, const Val& a, const Val& acc, uint e)
{
    typedef SemanticOperation O;
    switch (id)
    {
    case ID_ABS: return x.o(O::OP_ABSOLUTE, a);
    case ID_NEG: return x.o(O::OP_NEGATE, a);
    case ID_SQABS:
    case ID_SQNEG:
    {
        Val isMinimum = x.cmp(O::COND_EQUAL, a, x.k(signedMinimum(e), e));
        x.sat(isMinimum);
        Val r = (id == ID_SQABS) ? x.o(O::OP_ABSOLUTE, a) : x.o(O::OP_NEGATE, a);
        return x.sel(isMinimum, x.k(signedMaximum(e), e), r);
    }
    case ID_CLS: return x.o(O::OP_COUNT_LEADING_SIGN_BITS, a);
    case ID_CLZ: return x.o(O::OP_COUNT_LEADING_ZEROS, a);
    case ID_CNT: return x.o(O::OP_POPULATION_COUNT, a);
    case ID_RBIT: return x.o(O::OP_BIT_REVERSE, a);
    case ID_NOT: return x.o(O::OP_NOT, a);
    case ID_SUQADD:
    case ID_USQADD:
    {
        // suqadd: the signed accumulator plus the unsigned source, saturated
        // as signed; usqadd: the unsigned accumulator plus the signed source,
        // saturated as unsigned
        const bool isSigned = id == ID_SUQADD;
        if (e < 64)
        {
            Val sum = x.o(O::OP_ADD, x.ext(acc, 64, isSigned), x.ext(a, 64, !isSigned));
            return isSigned ? x.clampSigned(sum, e) : x.clampSignedToUnsigned(sum, e);
        }
        Val wrapped = x.o(O::OP_ADD, acc, a);
        Val zero = x.k(0, 64);
        if (isSigned)
        {
            // a is unsigned
            Val aNegative = x.cmp(O::COND_SIGNED_LESS, a, zero);
            Val accNegative = x.cmp(O::COND_SIGNED_LESS, acc, zero);
            Val wrappedNegative = x.cmp(O::COND_SIGNED_LESS, wrapped, zero);
            Val ifNegative = x.orFlag(x.s.op(O::OP_NOT, SemanticType::integer(1), accNegative),
                                      wrappedNegative);
            Val ifPositive = x.s.op(O::OP_AND, SemanticType::integer(1),
                x.s.op(O::OP_NOT, SemanticType::integer(1), accNegative),
                wrappedNegative);
            Val isOverflow = x.sel(aNegative, ifNegative, ifPositive);
            x.sat(isOverflow);
            return x.sel(isOverflow, x.k(signedMaximum(64), 64), wrapped);
        }
        // acc is unsigned, a is signed
        Val aNegative = x.cmp(O::COND_SIGNED_LESS, a, zero);
        Val carry = x.cmp(O::COND_UNSIGNED_LESS, wrapped, acc);
        Val borrow = x.cmp(O::COND_UNSIGNED_GREATER, wrapped, acc);
        Val isOverflow = x.sel(aNegative, borrow, carry);
        x.sat(isOverflow);
        return x.sel(isOverflow, x.sel(aNegative, zero, x.k(-1, 64)), wrapped);
    }
    default:
        CHECK_FAIL();
    }
}

/////////////////////////////////////////////////////////////////////////////
// The register operands
/////////////////////////////////////////////////////////////////////////////

// The register value of operand 'index' (an integer of the register size)
Val regOf(X& x, uint index)
{
    return x.c.getRegister(x.s.operand(index).m_register);
}

// A lane of the source operand 'index': the element of an element operand,
// the lane 0 of a scalar, otherwise lane 'lane'
Val srcLane(X& x, uint index, uint lane, uint e)
{
    const ArmDecodedOperand& o = x.s.operand(index);
    if (o.m_elementIndex >= 0)
        lane = (uint)o.m_elementIndex;
    else if (x.s.isScalar(index))
        lane = 0;
    return x.lane(regOf(x, index), lane, e);
}

// The destination register to build: zero (or the old register for the
// upper half forms)
Val zeroResult(X& x)
{
    return x.k(0, 128);
}

void writeResult(X& x, const Val& result)
{
    x.s.writeV(x.s.registerNumber(0), result);
    x.finish();
}

// The shape of the operand and the lanes the instruction computes
struct Layout {
    // The size of a destination element and the number of lanes it computes
    uint m_e;
    uint m_n;
};

Layout destinationLayout(SimdLift& s)
{
    const SimdShape d = s.shape(0);
    Layout l;
    l.m_e = d.m_elementBits;
    l.m_n = d.m_lanes;
    return l;
}

/////////////////////////////////////////////////////////////////////////////
// Same size lanes
/////////////////////////////////////////////////////////////////////////////

bool liftSame(SimdLift& s, int id)
{
    X x(s);
    const Layout l = destinationLayout(s);
    const bool isZeroForm = (id >= ID_CMEQ && id <= ID_CMLT && id != ID_CMTST &&
                             id != ID_CMHI && id != ID_CMHS) &&
                            (s.operand(2).m_kind != ArmDecodedOperand::KIND_REGISTER);
    Val dest = regOf(x, 0);
    Val result = zeroResult(x);
    for (uint i = 0; i < l.m_n; i++)
    {
        Val a = srcLane(x, 1, i, l.m_e);
        Val b = isZeroForm ? x.k(0, l.m_e) : srcLane(x, 2, i, l.m_e);
        Val acc = x.lane(dest, i, l.m_e);
        result = x.put(result, calcSame(x, id, a, b, acc, l.m_e), i, l.m_e);
    }
    writeResult(x, result);
    return true;
}

bool liftUnary(SimdLift& s, int id)
{
    X x(s);
    const Layout l = destinationLayout(s);
    Val dest = regOf(x, 0);
    Val result = zeroResult(x);
    for (uint i = 0; i < l.m_n; i++)
    {
        Val a = srcLane(x, 1, i, l.m_e);
        Val acc = x.lane(dest, i, l.m_e);
        result = x.put(result, calcUnary(x, id, a, acc, l.m_e), i, l.m_e);
    }
    writeResult(x, result);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Long and wide operations
/////////////////////////////////////////////////////////////////////////////

bool liftLong(SimdLift& s, int id, bool isUpper)
{
    X x(s);
    const SimdShape d = s.shape(0);
    const uint wideBits = d.m_elementBits;
    const uint e = wideBits / 2;
    const uint n = d.m_lanes;
    // The lanes of the narrow sources: the upper half for the "2" forms
    const uint base = isUpper ? n : 0;
    const bool isWide = (id >= ID_SADDW);
    const bool isUnsigned = (id == ID_UADDL) || (id == ID_USUBL) || (id == ID_UABDL) ||
        (id == ID_UABAL) || (id == ID_UMULL) || (id == ID_UMLAL) ||
        (id == ID_UMLSL) || (id == ID_UADDW) || (id == ID_USUBW);
    const bool isSigned = !isUnsigned;
    Val dest = regOf(x, 0);
    Val result = zeroResult(x);
    for (uint i = 0; i < n; i++)
    {
        Val accumulator = x.lane(dest, i, wideBits);
        Val a, b;
        if (isWide)
            a = srcLane(x, 1, i, wideBits);
        else
            a = x.ext(srcLane(x, 1, base + i, e), wideBits, isSigned);
        b = x.ext(srcLane(x, 2, base + i, e), wideBits, isSigned);
        Val r;
        typedef SemanticOperation O;
        switch (id)
        {
        case ID_SADDL: case ID_UADDL: case ID_SADDW: case ID_UADDW:
            r = x.o(O::OP_ADD, a, b); break;
        case ID_SSUBL: case ID_USUBL: case ID_SSUBW: case ID_USUBW:
            r = x.o(O::OP_SUBTRACT, a, b); break;
        case ID_SABDL:
            r = x.o(O::OP_ABSOLUTE_DIFFERENCE_SIGNED, a, b); break;
        case ID_UABDL:
            r = x.o(O::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, a, b); break;
        case ID_SABAL:
            r = x.o(O::OP_ADD, accumulator, x.o(O::OP_ABSOLUTE_DIFFERENCE_SIGNED, a, b)); break;
        case ID_UABAL:
            r = x.o(O::OP_ADD, accumulator, x.o(O::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, a, b)); break;
        case ID_SMULL: case ID_UMULL:
            r = x.o(O::OP_MULTIPLY, a, b); break;
        case ID_SMLAL: case ID_UMLAL:
            r = x.o(O::OP_ADD, accumulator, x.o(O::OP_MULTIPLY, a, b)); break;
        case ID_SMLSL: case ID_UMLSL:
            r = x.o(O::OP_SUBTRACT, accumulator, x.o(O::OP_MULTIPLY, a, b)); break;
        case ID_SQDMULL: case ID_SQDMLAL: case ID_SQDMLSL:
        {
            Val product = x.o(O::OP_MULTIPLY, a, b);
            Val doubled = x.o(O::OP_ADD_SATURATE_SIGNED, product, product);
            x.sat(x.cmp(O::COND_NOT_EQUAL, doubled, x.o(O::OP_ADD, product, product)));
            if (id == ID_SQDMULL)
            {
                r = doubled;
            } else
            {
                const bool isAdd = id == ID_SQDMLAL;
                r = x.o(isAdd ? O::OP_ADD_SATURATE_SIGNED : O::OP_SUBTRACT_SATURATE_SIGNED,
                        accumulator, doubled);
                x.sat(x.cmp(O::COND_NOT_EQUAL, r,
                            x.o(isAdd ? O::OP_ADD : O::OP_SUBTRACT, accumulator, doubled)));
            }
            break;
        }
        default:
            CHECK_FAIL();
        }
        result = x.put(result, r, i, wideBits);
    }
    writeResult(x, result);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Narrowing and the shifts
/////////////////////////////////////////////////////////////////////////////

// addhn, raddhn, subhn, rsubhn (and the "2" forms)
bool liftHighNarrow(SimdLift& s, bool isUpper, bool isSubtract, bool isRounding)
{
    X x(s);
    const SimdShape src = s.shape(1);
    const uint wide = src.m_elementBits;
    const uint e = wide / 2;
    const uint n = src.m_lanes;
    Val result = isUpper ? x.lane(regOf(x, 0), 0, 64) : x.k(0, 64);
    Val full = isUpper ? x.zx(result, 128) : x.k(0, 128);
    const uint base = isUpper ? n : 0;
    for (uint i = 0; i < n; i++)
    {
        Val a = srcLane(x, 1, i, wide);
        Val b = srcLane(x, 2, i, wide);
        Val r = x.o(isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD, a, b);
        if (isRounding)
            r = x.o(Op::OP_ADD, r, x.k((int64)1 << (e - 1), wide));
        r = x.tr(x.shiftRight(r, x.k(e, wide), false, false), e);
        full = x.put(full, r, base + i, e);
    }
    writeResult(x, full);
    return true;
}

// The destination of a narrowing instruction: the old register for the
// upper half forms
Val narrowDestination(X& x, bool isUpper)
{
    if (isUpper)
        return x.lane(regOf(x, 0), 0, 64);
    return x.k(0, 64);
}

// xtn, sqxtn, uqxtn, sqxtun, and the shifts right with a narrowing
// (shrn, rshrn, sqshrn, sqrshrn, uqshrn, uqrshrn, sqshrun, sqrshrun)
enum { NARROW_PLAIN, NARROW_SIGNED, NARROW_UNSIGNED, NARROW_SIGNED_TO_UNSIGNED };

bool liftNarrow(SimdLift& s, bool isUpper, int kind, bool hasShift,
                bool isRounding)
{
    X x(s);
    const SimdShape src = s.shape(1);
    const uint wide = src.m_elementBits;
    const uint e = wide / 2;
    const uint n = src.m_isScalar ? 1 : src.m_lanes;
    const uint shift = hasShift ? (uint)s.operand(2).m_immediate : 0;
    const uint base = isUpper ? n : 0;
    Val full = isUpper ? x.zx(narrowDestination(x, true), 128) : x.k(0, 128);
    // The "2" forms keep the lower 64 bits of the destination, the others
    // write them
    for (uint i = 0; i < n; i++)
    {
        Val a = srcLane(x, 1, i, wide);
        // The shifted value, in 64 bits (a narrow source of up to 64 bits)
        const bool isSignedSource = (kind == NARROW_SIGNED) ||
                                    (kind == NARROW_SIGNED_TO_UNSIGNED);
        Val v = a;
        if (hasShift)
        {
            v = x.shiftRight(a, x.k(shift, wide), isSignedSource, isRounding);
        }
        Val r;
        switch (kind)
        {
        case NARROW_PLAIN:
            r = x.tr(v, e);
            break;
        case NARROW_SIGNED:
            r = x.clampSigned(x.sx(v, 64), e);
            break;
        case NARROW_UNSIGNED:
            r = x.clampUnsigned(x.zx(v, 64), e);
            break;
        default:
            r = x.clampSignedToUnsigned(x.sx(v, 64), e);
            break;
        }
        full = x.put(full, r, base + i, e);
    }
    writeResult(x, full);
    return true;
}

// The shifts by an immediate on lanes of the same size
enum {
    SHIFT_SHL, SHIFT_SSHR, SHIFT_USHR, SHIFT_SRSHR, SHIFT_URSHR, SHIFT_SSRA,
    SHIFT_USRA, SHIFT_SRSRA, SHIFT_URSRA, SHIFT_SRI, SHIFT_SLI,
    SHIFT_SQSHL, SHIFT_UQSHL, SHIFT_SQSHLU
};

bool liftShiftImmediate(SimdLift& s, int kind)
{
    X x(s);
    const Layout l = destinationLayout(s);
    const uint e = l.m_e;
    const uint shift = (uint)s.operand(2).m_immediate;
    Val dest = regOf(x, 0);
    Val result = zeroResult(x);
    for (uint i = 0; i < l.m_n; i++)
    {
        Val a = srcLane(x, 1, i, e);
        Val acc = x.lane(dest, i, e);
        Val amount = x.k(shift, e);
        Val r;
        typedef SemanticOperation O;
        switch (kind)
        {
        case SHIFT_SHL: r = x.o(O::OP_SHIFT_LEFT, a, amount); break;
        case SHIFT_SSHR: r = x.shiftRight(a, amount, true, false); break;
        case SHIFT_USHR: r = x.shiftRight(a, amount, false, false); break;
        case SHIFT_SRSHR: r = x.shiftRight(a, amount, true, true); break;
        case SHIFT_URSHR: r = x.shiftRight(a, amount, false, true); break;
        case SHIFT_SSRA: r = x.o(O::OP_ADD, acc, x.shiftRight(a, amount, true, false)); break;
        case SHIFT_USRA: r = x.o(O::OP_ADD, acc, x.shiftRight(a, amount, false, false)); break;
        case SHIFT_SRSRA: r = x.o(O::OP_ADD, acc, x.shiftRight(a, amount, true, true)); break;
        case SHIFT_URSRA: r = x.o(O::OP_ADD, acc, x.shiftRight(a, amount, false, true)); break;
        case SHIFT_SLI:
        {
            // Keep the low 'shift' bits of the destination
            Val keep = x.k(shift == 0 ? 0 : (int64)(((uint64)1 << shift) - 1), e);
            r = x.o(O::OP_OR, x.o(O::OP_SHIFT_LEFT, a, amount), x.o(O::OP_AND, acc, keep));
            break;
        }
        case SHIFT_SRI:
        {
            // Keep the high 'shift' bits of the destination
            Val moved = x.shiftRight(a, amount, false, false);
            Val shifted = x.shiftRight(x.k(-1, e), amount, false, false);
            r = x.o(O::OP_OR, moved, x.o(O::OP_AND_NOT, shifted, acc));
            break;
        }
        default:
        {
            // sqshl, uqshl, sqshlu: a left shift with saturation
            const bool isSigned = (kind == SHIFT_SQSHL) || (kind == SHIFT_SQSHLU);
            Val wide = x.ext(a, 64, isSigned);
            Val left64 = x.o(O::OP_SHIFT_LEFT, wide, x.k(shift, 64));
            Val back = x.shiftRight(left64, x.k(shift, 64),
                                    isSigned && (kind != SHIFT_SQSHLU), false);
            Val isRoundTrip = x.cmp(O::COND_EQUAL, back, wide);
            Val limited;
            if (kind == SHIFT_SQSHL)
            {
                Val fits = x.cmp(O::COND_EQUAL, x.sx(x.tr(left64, e), 64), left64);
                Val ok = x.s.op(O::OP_AND, SemanticType::integer(1), isRoundTrip, fits);
                Val isNegative = x.cmp(O::COND_SIGNED_LESS, a, x.k(0, e));
                Val saturated = x.sel(isNegative, x.k(signedMinimum(e), e),
                                      x.k(signedMaximum(e), e));
                x.sat(x.s.op(O::OP_NOT, SemanticType::integer(1), ok));
                r = x.sel(ok, x.tr(left64, e), saturated);
            } else if (kind == SHIFT_UQSHL)
            {
                Val fits = x.cmp(O::COND_EQUAL, x.zx(x.tr(left64, e), 64), left64);
                Val ok = x.s.op(O::OP_AND, SemanticType::integer(1), isRoundTrip, fits);
                x.sat(x.s.op(O::OP_NOT, SemanticType::integer(1), ok));
                r = x.sel(ok, x.tr(left64, e), x.k(unsignedMaximum(e), e));
            } else
            {
                // sqshlu: a signed source, an unsigned result
                Val isNegative = x.cmp(O::COND_SIGNED_LESS, a, x.k(0, e));
                Val fits = x.cmp(O::COND_EQUAL, x.zx(x.tr(left64, e), 64), left64);
                Val ok = x.s.op(O::OP_AND, SemanticType::integer(1),
                    x.s.op(O::OP_AND, SemanticType::integer(1), isRoundTrip, fits),
                    x.s.op(O::OP_NOT, SemanticType::integer(1), isNegative));
                // A negative value gives 0 (saturated, unless it is 0)
                Val isZero = x.cmp(O::COND_EQUAL, a, x.k(0, e));
                Val saturates = x.s.op(O::OP_NOT, SemanticType::integer(1),
                    x.orFlag(ok, isZero));
                x.sat(saturates);
                limited = x.sel(isNegative, x.k(0, e), x.k(unsignedMaximum(e), e));
                r = x.sel(ok, x.tr(left64, e), limited);
            }
            break;
        }
        }
        result = x.put(result, r, i, e);
    }
    writeResult(x, result);
    return true;
}

// sshll, ushll, shll, sxtl, uxtl (and the "2" forms): widen with a left shift
bool liftShiftLong(SimdLift& s, bool isUpper, bool isSigned, bool isShll)
{
    X x(s);
    const SimdShape d = s.shape(0);
    const uint wide = d.m_elementBits;
    const uint e = wide / 2;
    const uint n = d.m_lanes;
    uint shift = 0;
    if (isShll)
        shift = e;
    else if (s.count() > 2)
        shift = (uint)s.operand(2).m_immediate;
    const uint base = isUpper ? n : 0;
    Val result = zeroResult(x);
    for (uint i = 0; i < n; i++)
    {
        Val a = x.ext(srcLane(x, 1, base + i, e), wide, isSigned);
        Val r = shift == 0 ? a : x.o(Op::OP_SHIFT_LEFT, a, x.k(shift, wide));
        result = x.put(result, r, i, wide);
    }
    writeResult(x, result);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Pairwise, across lanes, and the pairs
/////////////////////////////////////////////////////////////////////////////

// addp, smaxp, sminp, umaxp, uminp
bool liftPairwise(SimdLift& s, Op::Operation operation)
{
    X x(s);
    const SimdShape d = s.shape(0);
    if (s.count() == 2)
    {
        // addp d0, v1.2d
        const uint e = d.m_elementBits;
        Val v = regOf(x, 1);
        Val r = x.o(operation, x.lane(v, 0, e), x.lane(v, 1, e));
        writeResult(x, x.zx(r, 128));
        return true;
    }
    const uint e = d.m_elementBits;
    const uint n = d.m_lanes;
    Val result = zeroResult(x);
    Val vn = regOf(x, 1);
    Val vm = regOf(x, 2);
    for (uint i = 0; i < n; i++)
    {
        const Val& source = (i < n / 2) ? vn : vm;
        const uint j = (i < n / 2) ? i : (i - n / 2);
        Val r = x.o(operation, x.lane(source, 2 * j, e), x.lane(source, 2 * j + 1, e));
        result = x.put(result, r, i, e);
    }
    writeResult(x, result);
    return true;
}

// saddlp, uaddlp, sadalp, uadalp
bool liftPairLong(SimdLift& s, bool isSigned, bool isAccumulate)
{
    X x(s);
    const SimdShape d = s.shape(0);
    const uint wide = d.m_elementBits;
    const uint e = wide / 2;
    const uint n = d.m_lanes;
    Val vn = regOf(x, 1);
    Val dest = regOf(x, 0);
    Val result = zeroResult(x);
    for (uint i = 0; i < n; i++)
    {
        Val a = x.ext(x.lane(vn, 2 * i, e), wide, isSigned);
        Val b = x.ext(x.lane(vn, 2 * i + 1, e), wide, isSigned);
        Val r = x.o(Op::OP_ADD, a, b);
        if (isAccumulate)
            r = x.o(Op::OP_ADD, r, x.lane(dest, i, wide));
        result = x.put(result, r, i, wide);
    }
    writeResult(x, result);
    return true;
}

// addv, saddlv, uaddlv, smaxv, sminv, umaxv, uminv
bool liftAcross(SimdLift& s, Op::Operation operation, bool isLong, bool isSigned)
{
    X x(s);
    const SimdShape src = s.shape(1);
    const uint e = src.m_elementBits;
    const uint outBits = isLong ? 2 * e : e;
    Val vn = regOf(x, 1);
    Val total;
    for (uint i = 0; i < src.m_lanes; i++)
    {
        Val a = x.lane(vn, i, e);
        if (isLong)
            a = x.ext(a, outBits, isSigned);
        total = total.isValid() ? x.o(operation, total, a) : a;
    }
    writeResult(x, x.zx(total, 128));
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Permutes and moves
/////////////////////////////////////////////////////////////////////////////

// zip1, zip2, uzp1, uzp2, trn1, trn2
bool liftPermute(SimdLift& s)
{
    X x(s);
    const char* name = s.name();
    const SimdShape d = s.shape(0);
    const uint e = d.m_elementBits;
    const uint n = d.m_lanes;
    const bool isSecond = name[strlen(name) - 1] == '2';
    Val vn = regOf(x, 1);
    Val vm = regOf(x, 2);
    Val result = zeroResult(x);
    for (uint i = 0; i < n; i++)
    {
        Val r;
        if (strncmp(name, "zip", 3) == 0)
        {
            const uint half = isSecond ? n / 2 : 0;
            const uint j = half + i / 2;
            r = x.lane((i % 2) == 0 ? vn : vm, j, e);
        } else if (strncmp(name, "uzp", 3) == 0)
        {
            const uint pos = (isSecond ? 1 : 0);
            if (i < n / 2)
                r = x.lane(vn, 2 * i + pos, e);
            else
                r = x.lane(vm, 2 * (i - n / 2) + pos, e);
        } else
        {
            const uint pos = (isSecond ? 1 : 0);
            r = x.lane(((i % 2) == 0) ? vn : vm, (i & ~1u) + pos, e);
        }
        result = x.put(result, r, i, e);
    }
    writeResult(x, result);
    return true;
}

// ext v0.16b, v1.16b, v2.16b, #index
bool liftExtract(SimdLift& s)
{
    X x(s);
    const SimdShape d = s.shape(0);
    const uint n = d.m_lanes;
    const uint index = (uint)s.operand(3).m_immediate;
    Val vn = regOf(x, 1);
    Val vm = regOf(x, 2);
    Val result = zeroResult(x);
    for (uint i = 0; i < n; i++)
    {
        const uint j = index + i;
        Val r = (j < n) ? x.lane(vn, j, 8) : x.lane(vm, j - n, 8);
        result = x.put(result, r, i, 8);
    }
    writeResult(x, result);
    return true;
}

// rev16, rev32, rev64
bool liftReverse(SimdLift& s)
{
    X x(s);
    const char* name = s.name();
    const SimdShape d = s.shape(0);
    const uint e = d.m_elementBits;
    const uint n = d.m_lanes;
    const uint container = (name[3] == '1') ? 16 : ((name[3] == '3') ? 32 : 64);
    const uint perContainer = container / e;
    Val vn = regOf(x, 1);
    Val result = zeroResult(x);
    for (uint i = 0; i < n; i++)
    {
        const uint start = (i / perContainer) * perContainer;
        const uint j = start + (perContainer - 1 - (i % perContainer));
        result = x.put(result, x.lane(vn, j, e), i, e);
    }
    writeResult(x, result);
    return true;
}

// dup, ins, mov, umov, smov in their forms
bool liftCopy(SimdLift& s)
{
    X x(s);
    ArmLiftContext& c = s.c();
    const ArmDecodedOperand& d = s.operand(0);
    const ArmDecodedOperand& n = s.operand(1);
    const ArchitectureDescriptionPtr& arch = c.getArchitecture();
    const bool isDestinationGeneral =
        arch->getRegister(d.m_register).m_file == ArchitectureRegister::FILE_GENERAL;
    const bool isSourceGeneral = (n.m_kind == ArmDecodedOperand::KIND_REGISTER) &&
        arch->getRegister(n.m_register).m_file == ArchitectureRegister::FILE_GENERAL;

    if (isDestinationGeneral)
    {
        // umov w0, v1.b[3] / smov x0, v1.h[2] / mov w0, v1.s[1]
        const SimdShape src = s.shape(1);
        const uint e = src.m_elementBits;
        const uint destBits = arch->getRegister(d.m_register).m_bits;
        Val v = srcLane(x, 1, 0, e);
        const bool isSigned = s.is("smov");
        c.write(0, x.ext(v, destBits, isSigned));
        return true;
    }
    if (d.m_elementIndex >= 0)
    {
        // ins v0.s[1], w1 / ins v0.s[1], v1.s[2] / mov aliases
        const SimdShape ds = s.shape(0);
        const uint e = ds.m_elementBits;
        Val value;
        if (isSourceGeneral)
            value = x.tr(c.read(1), e);
        else
            value = srcLane(x, 1, 0, e);
        Val updated = x.put(regOf(x, 0), value, (uint)d.m_elementIndex, e);
        s.writeV(s.registerNumber(0), updated);
        return true;
    }
    // dup v0.4s, w1 / dup v0.4s, v1.s[2] / dup d0, v1.d[1] (mov b0, v1.b[1])
    const SimdShape ds = s.shape(0);
    const uint e = ds.m_elementBits;
    Val value;
    if (isSourceGeneral)
        value = x.tr(c.read(1), e);
    else
        value = srcLane(x, 1, 0, e);
    Val result = zeroResult(x);
    for (uint i = 0; i < ds.m_lanes; i++)
        result = x.put(result, value, i, e);
    writeResult(x, result);
    return true;
}

// tbl, tbx
bool liftTable(SimdLift& s)
{
    X x(s);
    const bool isExtension = s.is("tbx");
    const SimdShape d = s.shape(0);
    const ArmDecodedOperand& list = s.operand(1);
    const uint count = list.m_count;
    const uint stride = list.m_stride ? list.m_stride : 1;
    // The registers of the table, in order
    cArray<Val> table(count);
    for (uint i = 0; i < count; i++)
        table[i] = x.c.getRegister(ARM64_REG_V0 + ((list.m_register - ARM64_REG_V0 + i * stride) & 31));
    Val indexes = regOf(x, 2);
    Val old = regOf(x, 0);
    Val result = zeroResult(x);
    const uint bytes = count * 16;
    for (uint i = 0; i < d.m_lanes; i++)
    {
        Val index = x.lane(indexes, i, 8);
        Val value = isExtension ? x.lane(old, i, 8) : x.k(0, 8);
        // value = table[index] when index < bytes
        for (uint r = 0; r < count; r++)
        {
            Val chosen = x.k(0, 8);
            // Select the byte of register r: a chain over the 16 bytes
            for (uint b = 0; b < 16; b++)
            {
                Val hit = x.cmp(Op::COND_EQUAL, index, x.k(r * 16 + b, 8));
                chosen = x.sel(hit, x.lane(table[r], b, 8), chosen);
            }
            Val inRegister = x.s.op(Op::OP_AND, SemanticType::integer(1),
                x.cmp(Op::COND_UNSIGNED_GREATER_EQUAL, index, x.k(r * 16, 8)),
                x.cmp(Op::COND_UNSIGNED_LESS, index, x.k(r * 16 + 16, 8)));
            if (bytes <= 255 + 1)
                value = x.sel(inRegister, chosen, value);
        }
        result = x.put(result, value, i, 8);
    }
    writeResult(x, result);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Logic and the modified immediates
/////////////////////////////////////////////////////////////////////////////

// The 64 bit value of the modified immediate (AdvSIMDExpandImm)
uint64 expandImmediate64(uint op, uint cmode, uint imm8)
{
    uint64 imm64 = 0;
    switch (cmode >> 1)
    {
    case 0: imm64 = ((uint64)imm8 << 32) | imm8; break;
    case 1: imm64 = (((uint64)imm8 << 8) << 32) | ((uint64)imm8 << 8); break;
    case 2: imm64 = (((uint64)imm8 << 16) << 32) | ((uint64)imm8 << 16); break;
    case 3: imm64 = (((uint64)imm8 << 24) << 32) | ((uint64)imm8 << 24); break;
    case 4: imm64 = 0; for (uint i = 0; i < 4; i++) imm64 |= (uint64)imm8 << (16 * i); break;
    case 5: imm64 = 0; for (uint i = 0; i < 4; i++) imm64 |= ((uint64)imm8 << 8) << (16 * i); break;
    case 6:
        if ((cmode & 1) == 0)
            imm64 = (((uint64)imm8 << 8) | 0xFF);
        else
            imm64 = (((uint64)imm8 << 16) | 0xFFFF);
        imm64 = (imm64 << 32) | imm64;
        break;
    default:
        if ((cmode & 1) == 0)
        {
            if (op == 0)
            {
                imm64 = 0;
                for (uint i = 0; i < 8; i++)
                    imm64 |= (uint64)imm8 << (8 * i);
            } else
            {
                for (uint i = 0; i < 8; i++)
                    if (((imm8 >> i) & 1) != 0)
                        imm64 |= (uint64)0xFF << (8 * i);
            }
        }
        break;
    }
    return imm64;
}

bool liftModifiedImmediate(SimdLift& s)
{
    X x(s);
    const uint imm8 = (s.bits(18, 16) << 5) | s.bits(9, 5);
    const uint cmode = s.bits(15, 12);
    const uint op = s.bits(29, 29);
    const bool isNegated = s.is("mvni");
    uint64 imm64 = expandImmediate64(op, cmode, imm8);
    const SimdShape d = s.shape(0);
    if (s.is("movi") || s.is("mvni"))
    {
        // The 64 bit movi with the op bit expands bytes; mvni inverts
        if (isNegated)
            imm64 = ~imm64;
        const uint total = d.m_isScalar ? 64 : d.m_totalBits;
        Val low = x.k((int64)imm64, 64);
        Val result = (total == 64) ? x.zx(low, 128) : x.s.join64(low, low);
        writeResult(x, result);
        return true;
    }
    // orr / bic with an immediate: Vd = Vd OR imm / Vd AND NOT imm
    const uint total = d.m_totalBits;
    Val low = x.k((int64)imm64, 64);
    Val immediate = (total == 64) ? x.zx(low, 128) : x.s.join64(low, low);
    Val old = regOf(x, 0);
    Val r = s.is("orr") ? x.o(Op::OP_OR, old, immediate)
                        : x.o(Op::OP_AND_NOT, immediate, old);
    if (total == 64)
        r = x.zx(x.tr(r, 64), 128);
    writeResult(x, r);
    return true;
}

// and, orr, eor, bic, orn, bsl, bit, bif (registers) and mov v0.16b, v1.16b
bool liftLogic(SimdLift& s)
{
    X x(s);
    const SimdShape d = s.shape(0);
    Val a = regOf(x, 1);
    Val r;
    if (s.is("bsl") || s.is("bit") || s.is("bif"))
    {
        Val m = regOf(x, 2);
        Val old = regOf(x, 0);
        if (s.is("bsl"))
            r = x.o(Op::OP_BITWISE_SELECT, old, a, m);
        else if (s.is("bit"))
            r = x.o(Op::OP_BITWISE_SELECT, m, a, old);
        else
            r = x.o(Op::OP_BITWISE_SELECT, m, old, a);
    } else if (s.is("mov"))
    {
        r = a;
    } else
    {
        Val b = regOf(x, 2);
        if (s.is("and")) r = x.o(Op::OP_AND, a, b);
        else if (s.is("orr")) r = x.o(Op::OP_OR, a, b);
        else if (s.is("eor")) r = x.o(Op::OP_XOR, a, b);
        else if (s.is("bic")) r = x.o(Op::OP_AND_NOT, b, a);
        else r = x.o(Op::OP_OR, a, x.o(Op::OP_NOT, b));
    }
    if (d.m_totalBits == 64)
        r = x.zx(x.tr(r, 64), 128);
    writeResult(x, r);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Dot products and the matrix multiplies
/////////////////////////////////////////////////////////////////////////////

// sdot, udot, usdot, sudot (vector and by element)
bool liftDot(SimdLift& s, bool isASigned, bool isBSigned)
{
    X x(s);
    const SimdShape d = s.shape(0);
    const uint n = d.m_lanes;
    Val vn = regOf(x, 1);
    Val dest = regOf(x, 0);
    const bool isElement = s.operand(2).m_elementIndex >= 0;
    Val vm = regOf(x, 2);
    Val result = zeroResult(x);
    for (uint i = 0; i < n; i++)
    {
        Val sum = x.lane(dest, i, 32);
        const uint group = isElement ? (uint)s.operand(2).m_elementIndex : i;
        for (uint j = 0; j < 4; j++)
        {
            Val a = x.ext(x.lane(vn, 4 * i + j, 8), 32, isASigned);
            Val b = x.ext(x.lane(vm, 4 * group + j, 8), 32, isBSigned);
            sum = x.o(Op::OP_ADD, sum, x.o(Op::OP_MULTIPLY, a, b));
        }
        result = x.put(result, sum, i, 32);
    }
    writeResult(x, result);
    return true;
}

// smmla, ummla, usmmla: 2x8 by 8x2 matrices into 2x2 of 32 bits
bool liftMatrixMultiply(SimdLift& s, bool isASigned, bool isBSigned)
{
    X x(s);
    Val vn = regOf(x, 1);
    Val vm = regOf(x, 2);
    Val dest = regOf(x, 0);
    Val result = zeroResult(x);
    for (uint i = 0; i < 2; i++)
    {
        for (uint j = 0; j < 2; j++)
        {
            Val sum = x.lane(dest, 2 * i + j, 32);
            for (uint k = 0; k < 8; k++)
            {
                Val a = x.ext(x.lane(vn, 8 * i + k, 8), 32, isASigned);
                Val b = x.ext(x.lane(vm, 8 * j + k, 8), 32, isBSigned);
                sum = x.o(Op::OP_ADD, sum, x.o(Op::OP_MULTIPLY, a, b));
            }
            result = x.put(result, sum, 2 * i + j, 32);
        }
    }
    writeResult(x, result);
    return true;
}

// urecpe, ursqrte
bool liftReciprocalEstimate(SimdLift& s, bool isSqrt)
{
    X x(s);
    const SimdShape d = s.shape(0);
    Val vn = regOf(x, 1);
    Val result = zeroResult(x);
    for (uint i = 0; i < d.m_lanes; i++)
    {
        Val a = x.lane(vn, i, 32);
        Val r = x.c.newTemporary(SemanticType::integer(32));
        SemanticInstruction instruction(isSqrt ? Op::OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE :
                                                 Op::OP_FLOAT_RECIPROCAL_ESTIMATE,
                                        SemanticType::integer(32), r, a);
        instruction.setBehavior(Op::BEHAVIOR_ESTIMATE_ARM);
        x.c.emit(instruction);
        result = x.put(result, r, i, 32);
    }
    writeResult(x, result);
    return true;
}

} // namespace

bool armdis::liftSimdInteger(SimdLift& s)
{
    const char* name = s.name();
    bool isUpper = false;
    const int id = findId(name, isUpper);

    // The logic: registers or modified immediates
    if (s.is("and") || s.is("eor") || s.is("orn") || s.is("bsl") ||
        s.is("bit") || s.is("bif"))
        return liftLogic(s);
    if (s.is("orr") || s.is("bic"))
    {
        if (s.operand(1).m_kind == ArmDecodedOperand::KIND_IMMEDIATE)
            return liftModifiedImmediate(s);
        return liftLogic(s);
    }
    if (s.is("movi") || s.is("mvni"))
        return liftModifiedImmediate(s);
    if (s.is("mov"))
    {
        if ((s.operand(1).m_kind == ArmDecodedOperand::KIND_REGISTER) &&
            (s.operand(0).m_elementIndex < 0) &&
            (s.operand(1).m_elementIndex < 0) &&
            (s.shape(0).m_isVector))
            return liftLogic(s);
        return liftCopy(s);
    }
    if (s.is("dup") || s.is("ins") || s.is("umov") || s.is("smov"))
        return liftCopy(s);
    if (s.is("not") || s.is("mvn"))
        return liftUnary(s, ID_NOT);

    // sqshl, uqshl by an immediate
    if ((s.is("sqshl") || s.is("uqshl")) && (s.count() > 2) &&
        (s.operand(2).m_kind == ArmDecodedOperand::KIND_IMMEDIATE))
        return liftShiftImmediate(s, s.is("sqshl") ? SHIFT_SQSHL : SHIFT_UQSHL);
    if ((id >= 0) && (id < ID_SAME_END) && !isUpper)
        return liftSame(s, id);
    if ((id > ID_SAME_END) && (id < ID_UNARY_END) && !isUpper)
        return liftUnary(s, id);
    if ((id > ID_UNARY_END) && (id < ID_LONG_END))
        return liftLong(s, id, isUpper);

    // The remaining names are decoded here
    const size_t length = strlen(name);
    const bool hasTwo = (length > 1) && (name[length - 1] == '2') &&
                        (strcmp(name, "sha512h2") != 0);
    char base[32];
    strncpy(base, name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    if (hasTwo)
        base[length - 1] = 0;

    if (strcmp(base, "addhn") == 0) return liftHighNarrow(s, hasTwo, false, false);
    if (strcmp(base, "raddhn") == 0) return liftHighNarrow(s, hasTwo, false, true);
    if (strcmp(base, "subhn") == 0) return liftHighNarrow(s, hasTwo, true, false);
    if (strcmp(base, "rsubhn") == 0) return liftHighNarrow(s, hasTwo, true, true);

    if (strcmp(base, "xtn") == 0) return liftNarrow(s, hasTwo, NARROW_PLAIN, false, false);
    if (strcmp(base, "sqxtn") == 0) return liftNarrow(s, hasTwo, NARROW_SIGNED, false, false);
    if (strcmp(base, "uqxtn") == 0) return liftNarrow(s, hasTwo, NARROW_UNSIGNED, false, false);
    if (strcmp(base, "sqxtun") == 0) return liftNarrow(s, hasTwo, NARROW_SIGNED_TO_UNSIGNED, false, false);
    if (strcmp(base, "shrn") == 0) return liftNarrow(s, hasTwo, NARROW_PLAIN, true, false);
    if (strcmp(base, "rshrn") == 0) return liftNarrow(s, hasTwo, NARROW_PLAIN, true, true);
    if (strcmp(base, "sqshrn") == 0) return liftNarrow(s, hasTwo, NARROW_SIGNED, true, false);
    if (strcmp(base, "sqrshrn") == 0) return liftNarrow(s, hasTwo, NARROW_SIGNED, true, true);
    if (strcmp(base, "uqshrn") == 0) return liftNarrow(s, hasTwo, NARROW_UNSIGNED, true, false);
    if (strcmp(base, "uqrshrn") == 0) return liftNarrow(s, hasTwo, NARROW_UNSIGNED, true, true);
    if (strcmp(base, "sqshrun") == 0) return liftNarrow(s, hasTwo, NARROW_SIGNED_TO_UNSIGNED, true, false);
    if (strcmp(base, "sqrshrun") == 0) return liftNarrow(s, hasTwo, NARROW_SIGNED_TO_UNSIGNED, true, true);

    if (s.is("shl") || s.is("sli")) return liftShiftImmediate(s, s.is("shl") ? SHIFT_SHL : SHIFT_SLI);
    if (s.is("sshr")) return liftShiftImmediate(s, SHIFT_SSHR);
    if (s.is("ushr")) return liftShiftImmediate(s, SHIFT_USHR);
    if (s.is("srshr")) return liftShiftImmediate(s, SHIFT_SRSHR);
    if (s.is("urshr")) return liftShiftImmediate(s, SHIFT_URSHR);
    if (s.is("ssra")) return liftShiftImmediate(s, SHIFT_SSRA);
    if (s.is("usra")) return liftShiftImmediate(s, SHIFT_USRA);
    if (s.is("srsra")) return liftShiftImmediate(s, SHIFT_SRSRA);
    if (s.is("ursra")) return liftShiftImmediate(s, SHIFT_URSRA);
    if (s.is("sri")) return liftShiftImmediate(s, SHIFT_SRI);
    if (s.is("sqshlu")) return liftShiftImmediate(s, SHIFT_SQSHLU);
    if ((s.is("sqshl") || s.is("uqshl")) &&
        (s.operand(2).m_kind == ArmDecodedOperand::KIND_IMMEDIATE))
        return liftShiftImmediate(s, s.is("sqshl") ? SHIFT_SQSHL : SHIFT_UQSHL);

    if (strcmp(base, "sshll") == 0) return liftShiftLong(s, hasTwo, true, false);
    if (strcmp(base, "ushll") == 0) return liftShiftLong(s, hasTwo, false, false);
    if (strcmp(base, "shll") == 0) return liftShiftLong(s, hasTwo, false, true);
    if (strcmp(base, "sxtl") == 0) return liftShiftLong(s, hasTwo, true, false);
    if (strcmp(base, "uxtl") == 0) return liftShiftLong(s, hasTwo, false, false);

    if (s.is("addp")) return liftPairwise(s, Op::OP_ADD);
    if (s.is("smaxp")) return liftPairwise(s, Op::OP_MAXIMUM_SIGNED);
    if (s.is("sminp")) return liftPairwise(s, Op::OP_MINIMUM_SIGNED);
    if (s.is("umaxp")) return liftPairwise(s, Op::OP_MAXIMUM_UNSIGNED);
    if (s.is("uminp")) return liftPairwise(s, Op::OP_MINIMUM_UNSIGNED);
    if (s.is("saddlp")) return liftPairLong(s, true, false);
    if (s.is("uaddlp")) return liftPairLong(s, false, false);
    if (s.is("sadalp")) return liftPairLong(s, true, true);
    if (s.is("uadalp")) return liftPairLong(s, false, true);

    if (s.is("addv")) return liftAcross(s, Op::OP_ADD, false, false);
    if (s.is("saddlv")) return liftAcross(s, Op::OP_ADD, true, true);
    if (s.is("uaddlv")) return liftAcross(s, Op::OP_ADD, true, false);
    if (s.is("smaxv")) return liftAcross(s, Op::OP_MAXIMUM_SIGNED, false, true);
    if (s.is("sminv")) return liftAcross(s, Op::OP_MINIMUM_SIGNED, false, true);
    if (s.is("umaxv")) return liftAcross(s, Op::OP_MAXIMUM_UNSIGNED, false, false);
    if (s.is("uminv")) return liftAcross(s, Op::OP_MINIMUM_UNSIGNED, false, false);

    if (s.is("zip1") || s.is("zip2") || s.is("uzp1") || s.is("uzp2") ||
        s.is("trn1") || s.is("trn2"))
        return liftPermute(s);
    if (s.is("ext")) return liftExtract(s);
    if (s.is("rev16") || s.is("rev32") || s.is("rev64")) return liftReverse(s);
    if (s.is("tbl") || s.is("tbx")) return liftTable(s);

    if (s.is("sdot")) return liftDot(s, true, true);
    if (s.is("udot")) return liftDot(s, false, false);
    if (s.is("usdot")) return liftDot(s, false, true);
    if (s.is("sudot")) return liftDot(s, true, false);
    if (s.is("smmla")) return liftMatrixMultiply(s, true, true);
    if (s.is("ummla")) return liftMatrixMultiply(s, false, false);
    if (s.is("usmmla")) return liftMatrixMultiply(s, false, true);

    if (s.is("urecpe")) return liftReciprocalEstimate(s, false);
    if (s.is("ursqrte")) return liftReciprocalEstimate(s, true);
    return false;
}
