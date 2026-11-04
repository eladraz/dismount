/*
 * RiscvSemanticLiftVectorMask.cpp
 *
 * Implementation file
 *
 * The vector mask instructions: the mask logical instructions (vmand.mm...),
 * vcpop.m, vfirst.m, vmsbf.m, vmsif.m, vmsof.m and viota.m (vid.v is an
 * arithmetic instruction of the chunk engine). A mask register holds element
 * i in bit i (MLEN = 1), so the instructions work on the whole register as a
 * VLEN bit mask: the elements vstart .. vl - 1 are the active lanes, the
 * rest of the destination stays as it was.
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

// A mask register as a mask of VLEN lanes
SemanticOperand readMask(RvvLift& lift, uint number)
{
    return lift.bitcast(lift.readRegister(number), lift.maskType(lift.vlen()));
}

// The lanes that are vstart <= element < vl
SemanticOperand activeLanes(RvvLift& lift)
{
    const SemanticType mask = lift.maskType(lift.vlen());
    SemanticOperand below = lift.lanesBelow(lift.vlen(), lift.immX(0), lift.vl());
    SemanticOperand prestart = lift.lanesBelow(lift.vlen(), lift.immX(0),
                                               lift.vstart());
    return lift.emit(Op::OP_AND_NOT, mask, prestart, below);
}

// Lanes the instruction works on: the ones of activeLanes() selected by v0
// when the instruction is masked
SemanticOperand selectedLanes(RvvLift& lift)
{
    const SemanticType mask = lift.maskType(lift.vlen());
    SemanticOperand lanes = activeLanes(lift);
    if (lift.isMasked())
        lanes = lift.emit(Op::OP_AND, mask, lanes, readMask(lift, 0));
    return lanes;
}

// Write a mask register: 'result' in the lanes 'lanes', the old value in the
// others
void writeMask(RvvLift& lift, uint number, const SemanticOperand& result,
               const SemanticOperand& lanes)
{
    const SemanticType mask = lift.maskType(lift.vlen());
    SemanticOperand old = readMask(lift, number);
    SemanticOperand kept = lift.emit(Op::OP_AND_NOT, mask, lanes, old);
    SemanticOperand taken = lift.emit(Op::OP_AND, mask, result, lanes);
    lift.writeRegister(number, lift.emit(Op::OP_OR, mask, taken, kept));
}

void maskLogical(RvvLift& lift, uint operation)
{
    const SemanticType mask = lift.maskType(lift.vlen());
    SemanticOperand a = readMask(lift, lift.vs2());
    SemanticOperand b = readMask(lift, lift.vs1());
    SemanticOperand result;
    switch (operation)
    {
    case RVV_MASK_AND:
        result = lift.emit(Op::OP_AND, mask, a, b);
        break;
    case RVV_MASK_NAND:
        result = lift.emit(Op::OP_NOT, mask, lift.emit(Op::OP_AND, mask, a, b));
        break;
    case RVV_MASK_ANDN:
        result = lift.emit(Op::OP_AND_NOT, mask, b, a);
        break;
    case RVV_MASK_XOR:
        result = lift.emit(Op::OP_XOR, mask, a, b);
        break;
    case RVV_MASK_OR:
        result = lift.emit(Op::OP_OR, mask, a, b);
        break;
    case RVV_MASK_NOR:
        result = lift.emit(Op::OP_NOT, mask, lift.emit(Op::OP_OR, mask, a, b));
        break;
    case RVV_MASK_ORN:
        result = lift.emit(Op::OP_OR, mask, a, lift.emit(Op::OP_NOT, mask, b));
        break;
    default:
        result = lift.emit(Op::OP_NOT, mask, lift.emit(Op::OP_XOR, mask, a, b));
        break;
    }
    writeMask(lift, lift.vd(), result, activeLanes(lift));
}

// The lanes of vs2 the instruction counts: vs2 & the selected lanes (the
// instruction doesn't work with a nonzero vstart)
SemanticOperand sourceLanes(RvvLift& lift)
{
    return lift.emit(Op::OP_AND, lift.maskType(lift.vlen()),
                     readMask(lift, lift.vs2()), selectedLanes(lift));
}

void trapIfVstart(RvvLift& lift)
{
    lift.trapIllegalIf(lift.compare(Op::COND_NOT_EQUAL, lift.xlenType(),
                                    lift.vstart(), lift.immX(0)));
}

void maskCount(RvvLift& lift, uint operation)
{
    const SemanticType x = lift.xlenType();
    trapIfVstart(lift);
    SemanticOperand lanes = sourceLanes(lift);
    SemanticOperand result = lift.emit((operation == RVV_MASK_CPOP) ?
                    Op::OP_MASK_COUNT : Op::OP_MASK_FIRST, x, lanes);
    lift.writeX(lift.vd(), result);
}

// vmsbf.m, vmsif.m, vmsof.m
void maskSetFirst(RvvLift& lift, uint operation)
{
    const SemanticType x = lift.xlenType();
    const uint vlen = lift.vlen();
    const SemanticType mask = lift.maskType(vlen);
    trapIfVstart(lift);
    // vm = 0: the destination can't be v0; the source is not the
    // destination
    if (lift.isMasked() && (lift.vd() == 0))
    {
        lift.trapIllegal();
        return;
    }
    if (lift.vd() == lift.vs2())
    {
        lift.trapIllegal();
        return;
    }
    SemanticOperand lanes = selectedLanes(lift);
    SemanticOperand source = lift.emit(Op::OP_AND, mask,
                                       readMask(lift, lift.vs2()), lanes);
    // The index of the first set lane, -1 if none
    SemanticOperand first = lift.emit(Op::OP_MASK_FIRST, x, source);
    SemanticOperand result;
    if (operation == RVV_MASK_SOF)
    {
        // Only the lane 'first'
        SemanticOperand upTo = lift.lanesBelow(vlen, lift.immX(0),
                lift.emit(Op::OP_ADD, x, first, lift.immX(1)));
        SemanticOperand before = lift.lanesBelow(vlen, lift.immX(0), first);
        result = lift.emit(Op::OP_AND_NOT, mask, before, upTo);
    } else
    {
        // The lanes before it (sbf) / up to it (sif); all of them if none
        SemanticOperand isNone = lift.compare(Op::COND_SIGNED_LESS, x, first,
                                              lift.immX(0));
        SemanticOperand limit = (operation == RVV_MASK_SIF) ?
                lift.emit(Op::OP_ADD, x, first, lift.immX(1)) : first;
        limit = lift.emit(Op::OP_SELECT, x, isNone, lift.vl(), limit);
        result = lift.lanesBelow(vlen, lift.immX(0), limit);
    }
    writeMask(lift, lift.vd(), result, lanes);
}

// viota.m body, per (SEW, LMUL)
void iotaBody(RvvLift& lift, const RvvType& type)
{
    const SemanticType x = lift.xlenType();
    const uint vd = lift.vd();
    trapIfVstart(lift);
    const RvvGroup group = RvvLift::makeGroupLmul(type, vd, type.m_sew,
                                                  type.m_lmulLog2);
    RvvGroup source;
    source.m_register = lift.vs2();
    source.m_eew = 1;
    source.m_emulLog2 = 0;
    bool isLegal = RvvLift::isGroupLegal(group) &&
                   !RvvLift::isOverlapped(group, source) &&
                   !(lift.isMasked() && (vd == 0));
    if (!isLegal)
    {
        lift.trapIllegal();
        return;
    }
    const SemanticType mask = lift.maskType(lift.vlen());
    SemanticOperand lanes = selectedLanes(lift);
    SemanticOperand ones = lift.emit(Op::OP_AND, mask,
                                     readMask(lift, lift.vs2()), lanes);
    SemanticOperand i = lift.beginLoop(lift.vl());
    if (lift.isMasked())
    {
        SemanticOperand bit = lift.maskBit(i);
        lift.skipElementIf(lift.compare(Op::COND_EQUAL, lift.integerType(1), bit,
                                        lift.imm(0, lift.integerType(1))));
    }
    // The number of the lanes before this element
    SemanticOperand before = lift.lanesBelow(lift.vlen(), lift.immX(0), i);
    SemanticOperand count = lift.emit(Op::OP_MASK_COUNT, x,
            lift.emit(Op::OP_AND, mask, ones, before));
    lift.writeElement(vd, type.m_sew, i, lift.truncate(count, type.m_sew));
    lift.endLoop();
}

} // namespace

void liftVectorMask(RiscvLiftContext& context)
{
    RvvLift lift(context);
    const uint operation = (uint)context.getParameter();
    if (operation == RVV_MASK_IOTA)
    {
        lift.dispatch(iotaBody);
        lift.finish();
        return;
    }
    lift.requireValid();
    switch (operation)
    {
    case RVV_MASK_CPOP:
    case RVV_MASK_FIRST:
        maskCount(lift, operation);
        break;
    case RVV_MASK_SBF:
    case RVV_MASK_SIF:
    case RVV_MASK_SOF:
        maskSetFirst(lift, operation);
        break;
    default:
        maskLogical(lift, operation);
        break;
    }
    lift.placeLabel(lift.endLabel());
    lift.finish();
}

} // namespace riscvdis
