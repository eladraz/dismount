/*
 * RiscvSemanticLiftVectorPermute.cpp
 *
 * Implementation file
 *
 * The vector permutation instructions (vmv.x.s, vmv.s.x, the slides,
 * vrgather, vrgatherei16, vcompress, vmv<n>r.v) and the integer
 * reductions (vredsum.vs ..., vwredsum.vs). The permutations move elements
 * to places that depend on the data or on a runtime offset, so they are
 * element loops (RvvLift::beginLoop()); the reductions are chunk based: the
 * inactive lanes of a chunk take the identity of the operation and
 * OP_REDUCE combines the lanes.
 *
 * The parameter of the entries: a RvvPermuteOp, and in bits 8 and up the
 * number of registers of vmv<n>r.v or the description of a reduction
 * (RVV_REDUCE_* in bits 8-10, wide in bit 16, signed in bit 17).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"
#include "dismount/proc/riscv/RiscvSemanticVector.h"

namespace riscvdis {

typedef SemanticOperation Op;

namespace {

uint permuteOperation(RvvLift& lift)
{
    return (uint)(lift.context().getParameter() & 0xFF);
}

uint permuteArgument(RvvLift& lift)
{
    return (uint)(lift.context().getParameter() >> 8);
}

// Skip the element if it is masked off (vm = 0 and the bit of v0 is clear)
void skipMaskedOff(RvvLift& lift, const SemanticOperand& i)
{
    if (!lift.isMasked())
        return;
    SemanticOperand bit = lift.maskBit(i);
    lift.skipElementIf(lift.compare(Op::COND_EQUAL, lift.integerType(1), bit,
                                    lift.imm(0, lift.integerType(1))));
}

void trapIfVstart(RvvLift& lift)
{
    lift.trapIllegalIf(lift.compare(Op::COND_NOT_EQUAL, lift.xlenType(),
                                    lift.vstart(), lift.immX(0)));
}

// The f register rs1 as a scalar of 'bits' bits (a bad NaN box reads as the
// canonical NaN)
SemanticOperand readBoxedFloat(RvvLift& lift, uint bits)
{
    uint64 canonical = 0x7FF8000000000000ULL;
    if (bits == 16)
        canonical = 0x7E00;
    else if (bits == 32)
        canonical = 0x7FC00000;
    return lift.readFloatScalar(bits, canonical);
}

/*
 * vmv.x.s and vmv.s.x (and vfmv.f.s, vfmv.s.f)
 */
void moveScalarBody(RvvLift& lift, const RvvType& type)
{
    const uint sew = type.m_sew;
    const bool isFloat = (permuteArgument(lift) & 1) != 0;
    if (isFloat && (sew < 16))
    {
        lift.trapIllegal();
        return;
    }
    if (isFloat)
        lift.context().getRoundingMode(0);
    if (isFloat && (permuteOperation(lift) == RVV_PERMUTE_MV_X_S))
    {
        // fd = vs2[0], NaN boxed (vl and vstart don't matter)
        lift.writeFloatRegister(lift.vd(),
                                lift.readElement(lift.vs2(), sew, lift.immX(0)));
        return;
    }
    if (permuteOperation(lift) == RVV_PERMUTE_MV_X_S)
    {
        // rd = vs2[0], sign extended (vl and vstart don't matter)
        SemanticOperand value = lift.readElement(lift.vs2(), sew, lift.immX(0));
        if (sew < lift.xlen())
            value = lift.signExtend(value, lift.xlen());
        else if (sew > lift.xlen())
            value = lift.truncate(value, lift.xlen());
        lift.writeX(lift.vd(), value);
        return;
    }
    // vd[0] = x[rs1] if vstart < vl
    SemanticOperand skip = lift.newLabel();
    lift.branchIf(lift.compare(Op::COND_UNSIGNED_GREATER_EQUAL, lift.xlenType(),
                               lift.vstart(), lift.vl()), skip);
    SemanticOperand value = isFloat ? readBoxedFloat(lift, sew) :
                                      lift.readScalar(lift.rs1(), sew);
    SemanticOperand reg = lift.readRegister(lift.vd());
    lift.writeRegister(lift.vd(), lift.insertBits(reg, value, lift.immX(0)));
    lift.placeLabel(skip);
}

/*
 * The slides
 */
void slideBody(RvvLift& lift, const RvvType& type)
{
    const uint operation = permuteOperation(lift);
    const SemanticType x = lift.xlenType();
    const SemanticType booleanType = lift.integerType(1);
    const uint sew = type.m_sew;
    const RvvGroup d = RvvLift::makeGroupLmul(type, lift.vd(), sew, type.m_lmulLog2);
    const RvvGroup s = RvvLift::makeGroupLmul(type, lift.vs2(), sew, type.m_lmulLog2);
    const bool isUp = (operation == RVV_PERMUTE_SLIDEUP) ||
                      (operation == RVV_PERMUTE_SLIDE1UP);
    // vfslide1up.vf, vfslide1down.vf: the scalar is in an f register
    const bool isFloat = (permuteArgument(lift) & 1) != 0;
    bool isLegal = RvvLift::isGroupLegal(d) && RvvLift::isGroupLegal(s) &&
                   !(lift.isMasked() && (lift.vd() == 0));
    if (isUp && (lift.vd() == lift.vs2()))
        isLegal = false;
    if (isFloat && (sew < 16))
        isLegal = false;
    if (!isLegal)
    {
        lift.trapIllegal();
        return;
    }
    if (isFloat)
        lift.context().getRoundingMode(0);
    const bool isVector = (operation == RVV_PERMUTE_SLIDEUP) ||
                          (operation == RVV_PERMUTE_SLIDEDOWN);
    // The offset: x[rs1] (unsigned, XLEN bits) or the immediate; the scalar
    // of the slide1 forms
    SemanticOperand offset;
    SemanticOperand scalar;
    if (isVector)
        offset = (lift.funct3() == 3) ? lift.immX(lift.uimm5())
                                       : lift.readX(lift.rs1());
    else
        scalar = isFloat ? readBoxedFloat(lift, sew) :
                           lift.readScalar(lift.rs1(), sew);
    const uint64 vlmax = type.vlmax(lift.vlen());

    SemanticOperand i = lift.beginLoop(lift.vl());
    skipMaskedOff(lift, i);
    SemanticOperand value;
    switch (operation)
    {
    case RVV_PERMUTE_SLIDEUP:
    {
        // The elements below the offset are not written
        lift.skipElementIf(lift.compare(Op::COND_UNSIGNED_LESS, x, i, offset));
        SemanticOperand source = lift.emit(Op::OP_SUBTRACT, x, i, offset);
        value = lift.readElement(lift.vs2(), sew, source);
        break;
    }
    case RVV_PERMUTE_SLIDEDOWN:
    {
        SemanticOperand source = lift.emit(Op::OP_ADD, x, i, offset);
        SemanticOperand isValid = lift.emit(Op::OP_AND, booleanType,
                lift.compare(Op::COND_UNSIGNED_LESS, x, offset, lift.immX(vlmax)),
                lift.compare(Op::COND_UNSIGNED_LESS, x, source, lift.immX(vlmax)));
        SemanticOperand element = lift.readElement(lift.vs2(), sew, source);
        value = lift.select(isValid, element,
                            lift.imm(0, lift.integerType(sew)));
        break;
    }
    case RVV_PERMUTE_SLIDE1UP:
    {
        SemanticOperand source = lift.emit(Op::OP_SUBTRACT, x, i, lift.immX(1));
        SemanticOperand element = lift.readElement(lift.vs2(), sew, source);
        value = lift.select(lift.compare(Op::COND_EQUAL, x, i, lift.immX(0)),
                            scalar, element);
        break;
    }
    default:
    {
        SemanticOperand source = lift.emit(Op::OP_ADD, x, i, lift.immX(1));
        SemanticOperand element = lift.readElement(lift.vs2(), sew, source);
        SemanticOperand last = lift.emit(Op::OP_SUBTRACT, x, lift.vl(),
                                         lift.immX(1));
        value = lift.select(lift.compare(Op::COND_EQUAL, x, i, last),
                            scalar, element);
        break;
    }
    }
    lift.writeElement(lift.vd(), sew, i, value);
    lift.endLoop();
}

/*
 * vrgather.vv / .vx / .vi and vrgatherei16.vv
 */
void gatherBody(RvvLift& lift, const RvvType& type)
{
    const uint sew = type.m_sew;
    const bool isEi16 = permuteOperation(lift) == RVV_PERMUTE_GATHER_EI16;
    const uint f3 = lift.funct3();
    const bool isVector = (f3 == 0);
    const SemanticType x = lift.xlenType();
    const SemanticType wide = lift.integerType(64);
    const RvvGroup d = RvvLift::makeGroupLmul(type, lift.vd(), sew, type.m_lmulLog2);
    const RvvGroup s = RvvLift::makeGroupLmul(type, lift.vs2(), sew, type.m_lmulLog2);
    const uint indexEew = isEi16 ? 16 : sew;
    const RvvGroup index = RvvLift::makeGroup(type, lift.vs1(), indexEew);
    bool isLegal = RvvLift::isGroupLegal(d) && RvvLift::isGroupLegal(s) &&
                   !(lift.isMasked() && (lift.vd() == 0)) &&
                   (lift.vd() != lift.vs2());
    if (isVector)
    {
        isLegal = isLegal && RvvLift::isGroupLegal(index);
        if (isEi16)
            isLegal = isLegal && !RvvLift::isOverlapped(d, index);
        else
            isLegal = isLegal && (lift.vd() != lift.vs1());
    }
    if (!isLegal)
    {
        lift.trapIllegal();
        return;
    }
    const uint64 vlmax = type.vlmax(lift.vlen());
    SemanticOperand fixedIndex;
    if (f3 == 3)
        fixedIndex = lift.imm(lift.uimm5(), wide);
    else if (!isVector)
        fixedIndex = lift.zeroExtend(lift.readX(lift.rs1()), 64);

    SemanticOperand i = lift.beginLoop(lift.vl());
    skipMaskedOff(lift, i);
    SemanticOperand position = fixedIndex;
    if (isVector)
        position = lift.zeroExtend(lift.readElement(lift.vs1(), indexEew, i), 64);
    SemanticOperand isValid = lift.compare(Op::COND_UNSIGNED_LESS, wide,
                                           position, lift.imm(vlmax, wide));
    SemanticOperand element = lift.readElement(lift.vs2(), sew,
                                               lift.truncate(position, lift.xlen()));
    SemanticOperand value = lift.select(isValid, element,
                                        lift.imm(0, lift.integerType(sew)));
    lift.writeElement(lift.vd(), sew, i, value);
    lift.endLoop();
    (void)x;
}

/*
 * vcompress.vm
 */
void compressBody(RvvLift& lift, const RvvType& type)
{
    const uint sew = type.m_sew;
    const SemanticType x = lift.xlenType();
    trapIfVstart(lift);
    const RvvGroup d = RvvLift::makeGroupLmul(type, lift.vd(), sew, type.m_lmulLog2);
    const RvvGroup s = RvvLift::makeGroupLmul(type, lift.vs2(), sew, type.m_lmulLog2);
    RvvGroup mask;
    mask.m_register = lift.vs1();
    mask.m_eew = 1;
    mask.m_emulLog2 = 0;
    bool isLegal = RvvLift::isGroupLegal(d) && RvvLift::isGroupLegal(s) &&
                   (lift.vd() != lift.vs2()) && !RvvLift::isOverlapped(d, mask);
    if (!isLegal)
    {
        lift.trapIllegal();
        return;
    }
    const SemanticType maskType = lift.maskType(lift.vlen());
    SemanticOperand selected = lift.bitcast(lift.readRegister(lift.vs1()), maskType);
    SemanticOperand i = lift.beginLoop(lift.vl());
    SemanticOperand bit = lift.readMaskBit(lift.vs1(), i);
    lift.skipElementIf(lift.compare(Op::COND_EQUAL, lift.integerType(1), bit,
                                    lift.imm(0, lift.integerType(1))));
    // The position: the number of selected elements before this one
    SemanticOperand before = lift.lanesBelow(lift.vlen(), lift.immX(0), i);
    SemanticOperand position = lift.emit(Op::OP_MASK_COUNT, x,
            lift.emit(Op::OP_AND, maskType, selected, before));
    SemanticOperand value = lift.readElement(lift.vs2(), sew, i);
    lift.writeElement(lift.vd(), sew, position, value);
    lift.endLoop();
}

/*
 * vmv<n>r.v: copy n registers (the elements from vstart on)
 */
void moveRegistersBody(RvvLift& lift, const RvvType& type)
{
    const uint registers = permuteArgument(lift);
    const uint sew = type.m_sew;
    if (((lift.vd() % registers) != 0) || ((lift.vs2() % registers) != 0))
    {
        lift.trapIllegal();
        return;
    }
    const uint lanes = lift.vlen() / sew;
    const SemanticType vector = lift.vectorType(sew, lanes);
    SemanticOperand results[8];
    for (uint r = 0; r < registers; r++)
    {
        SemanticOperand source = lift.bitcast(lift.readRegister(lift.vs2() + r),
                                              vector);
        SemanticOperand old = lift.bitcast(lift.readRegister(lift.vd() + r),
                                           vector);
        SemanticOperand before = lift.lanesBelow(lanes, lift.immX(r * lanes),
                                                 lift.vstart());
        results[r] = lift.select(before, old, source);
    }
    for (uint r = 0; r < registers; r++)
        lift.writeRegister(lift.vd() + r, results[r]);
}

/*
 * The reductions
 */
void reduceBody(RvvLift& lift, const RvvType& type)
{
    typedef SemanticInstruction Instruction;
    const uint argument = permuteArgument(lift);
    const uint kind = argument & 7;
    const bool isWide = ((argument >> 8) & 1) != 0;
    const bool isSigned = ((argument >> 9) & 1) != 0;
    const uint sew = type.m_sew;
    const uint bits = isWide ? sew * 2 : sew;
    const SemanticType x = lift.xlenType();
    trapIfVstart(lift);
    const RvvGroup s = RvvLift::makeGroupLmul(type, lift.vs2(), sew, type.m_lmulLog2);
    if (!RvvLift::isGroupLegal(s) || (bits > 64))
    {
        lift.trapIllegal();
        return;
    }

    Op::Operation combine = Op::OP_ADD;
    int64 identity = 0;
    const int64 allOnes = -1;
    const int64 maximum = (bits >= 64) ? (int64)0x7FFFFFFFFFFFFFFFLL :
                                (int64)(((uint64)1 << (bits - 1)) - 1);
    const int64 minimum = -maximum - 1;
    switch (kind)
    {
    case RVV_REDUCE_SUM:
        break;
    case RVV_REDUCE_AND:
        combine = Op::OP_AND;
        identity = allOnes;
        break;
    case RVV_REDUCE_OR:
        combine = Op::OP_OR;
        break;
    case RVV_REDUCE_XOR:
        combine = Op::OP_XOR;
        break;
    case RVV_REDUCE_MIN:
        combine = isSigned ? Op::OP_MINIMUM_SIGNED : Op::OP_MINIMUM_UNSIGNED;
        identity = isSigned ? maximum : allOnes;
        break;
    default:
        combine = isSigned ? Op::OP_MAXIMUM_SIGNED : Op::OP_MAXIMUM_UNSIGNED;
        identity = isSigned ? minimum : 0;
        break;
    }

    const uint lanes = lift.vlen() / bits;
    uint chunks = type.vlmax(lift.vlen()) / lanes;
    if (chunks == 0)
        chunks = 1;
    const SemanticType integerType = lift.integerType(bits);
    const SemanticType vectorType = lift.vectorType(bits, lanes);
    const SemanticType maskType = lift.maskType(lanes);
    SemanticOperand v0;
    if (lift.isMasked())
        v0 = lift.readRegister(0);

    SemanticOperand total = lift.readElement(lift.vs1(), bits, lift.immX(0));
    for (uint c = 0; c < chunks; c++)
    {
        SemanticOperand active = lift.lanesBelow(lanes, lift.immX(c * lanes),
                                                 lift.vl());
        if (lift.isMasked())
            active = lift.emit(Op::OP_AND, maskType, active,
                    lift.bitcast(lift.extractBits(v0, lift.immX(c * lanes),
                                                  lift.integerType(lanes)),
                                 maskType));
        SemanticOperand piece = lift.readPiece(s, c, lanes);
        if (isWide)
            piece = lift.extendLanes(piece, bits, isSigned);
        piece = lift.select(active, piece, lift.splat(identity, vectorType));
        SemanticOperand partial = lift.context().newTemporary(integerType);
        Instruction reduce(Op::OP_REDUCE, integerType, partial, piece);
        reduce.setCombineOperation(combine);
        lift.context().emit(reduce);
        total = lift.emit(combine, integerType, total, partial);
    }
    // vd[0] = the result, if vl > 0
    SemanticOperand skip = lift.newLabel();
    lift.branchIf(lift.compare(Op::COND_EQUAL, x, lift.vl(), lift.immX(0)), skip);
    SemanticOperand reg = lift.readRegister(lift.vd());
    lift.writeRegister(lift.vd(), lift.insertBits(reg, total, lift.immX(0)));
    lift.placeLabel(skip);
}

void permuteBody(RvvLift& lift, const RvvType& type)
{
    switch (permuteOperation(lift))
    {
    case RVV_PERMUTE_MV_X_S:
    case RVV_PERMUTE_MV_S_X:
        moveScalarBody(lift, type);
        break;
    case RVV_PERMUTE_SLIDEUP:
    case RVV_PERMUTE_SLIDEDOWN:
    case RVV_PERMUTE_SLIDE1UP:
    case RVV_PERMUTE_SLIDE1DOWN:
        slideBody(lift, type);
        break;
    case RVV_PERMUTE_GATHER:
    case RVV_PERMUTE_GATHER_EI16:
        gatherBody(lift, type);
        break;
    case RVV_PERMUTE_COMPRESS:
        compressBody(lift, type);
        break;
    case RVV_PERMUTE_MOVE:
        moveRegistersBody(lift, type);
        break;
    default:
        reduceBody(lift, type);
        break;
    }
}

} // namespace

void liftVectorPermute(RiscvLiftContext& context)
{
    RvvLift lift(context);
    lift.dispatch(permuteBody);
    lift.finish();
}

} // namespace riscvdis
