/*
 * Arm64SemanticLiftSveQuad.cpp
 *
 * Implementation file
 *
 * The A64 SVE lifters of the SVE2.1 quadword instructions: the reductions of
 * the elements of every 128 bit segment into a SIMD register (addqv...), the
 * permutes within the segments (dupq, extq, tblq, tbxq, zipq, uzpq) and the
 * structure loads and stores of 128 bit elements (ld2q...st4q).
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
#include "dismount/proc/arm/Arm64SemanticSve.h"

using namespace armdis;
using namespace armdis::a64;

typedef SemanticOperation Op;

namespace {

const ArmDecodedOperand& operandOf(SveLift& s, uint index)
{
    return s.context().getOperand(index);
}

// The 128 bit segment 'segment' of the integer of VL bits
SemanticOperand segmentOf(SveLift& s, const SemanticOperand& whole, uint segment)
{
    return s.emit(Op::OP_EXTRACT, SemanticType::integer(128), whole,
                  s.integer((int64)segment * 128, 32));
}

// 'whole' with the segment 'segment' replaced
SemanticOperand placeSegment(SveLift& s, const SemanticOperand& whole,
                             const SemanticOperand& value, uint segment)
{
    return s.emit(Op::OP_INSERT, s.wholeType(), whole, value,
                  s.integer((int64)segment * 128, 32));
}

SemanticType segmentType(uint elementBits)
{
    return SemanticType::vector(SemanticType::integer(elementBits), 128 / elementBits);
}

/////////////////////////////////////////////////////////////////////////////
// The reductions of every segment: addqv, smaxqv, ... faddqv...
/////////////////////////////////////////////////////////////////////////////

// m_a: the combine operation, m_b: 1 floating point; for the floating point
// the behavior of the minimum / maximum is in the high bits (m_b >> 8)
void liftReduceSegments(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 2));
    const bool isFloat = (d.m_b & 1) != 0;
    const uint segmentLanes = 128 / e;
    const uint segments = s.vectorBits() / 128;
    const uint count = s.lanes(e);
    SemanticOperand maskBits = s.bitcast(s.readMaskOperand(1, e), SemanticType::integer(count));
    SemanticOperand z = s.readZOperand(2, e);
    const Op::Operation operation = (Op::Operation)d.m_a;
    const SemanticType scalar = SemanticType::integer(e);
    SemanticType::FloatFormat format = (e == 16) ? SemanticType::FLOAT_HALF :
        ((e == 32) ? SemanticType::FLOAT_SINGLE : SemanticType::FLOAT_DOUBLE);
    const SemanticType floatScalar = SemanticType::floating(format);
    // The identity of the inactive elements
    uint64 identity = 0;
    const uint64 ones = (e == 64) ? ~(uint64)0 : (((uint64)1 << e) - 1);
    const uint64 sign = (uint64)1 << (e - 1);
    if (isFloat)
    {
        const uint64 infinity = (e == 16) ? 0x7C00ULL : ((e == 32) ? 0x7F800000ULL : 0x7FF0000000000000ULL);
        const uint64 quietNan = (e == 16) ? 0x7E00ULL : ((e == 32) ? 0x7FC00000ULL : 0x7FF8000000000000ULL);
        const uint behavior = d.m_b >> 8;
        if (operation == Op::OP_FLOAT_ADD)
            identity = 0;
        else if (behavior == (uint)Op::BEHAVIOR_MINMAX_NUMBER)
            identity = quietNan;
        else if (operation == Op::OP_FLOAT_MAXIMUM)
            identity = infinity | sign;
        else
            identity = infinity;
    } else
    {
        switch (operation)
        {
        case Op::OP_MAXIMUM_SIGNED: identity = sign; break;
        case Op::OP_MINIMUM_SIGNED: identity = sign - 1; break;
        case Op::OP_MINIMUM_UNSIGNED:
        case Op::OP_AND: identity = ones; break;
        default: identity = 0; break;
        }
    }
    SemanticOperand result = s.zero(segmentType(e));
    for (uint j = 0; j < segmentLanes; j++)
    {
        SemanticOperand values[8];
        for (uint seg = 0; seg < segments; seg++)
        {
            const uint lane = seg * segmentLanes + j;
            SemanticOperand active = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, SemanticType::integer(count), maskBits,
                       s.integer(lane, count)));
            values[seg] = s.select(active, s.extractLane(z, lane), s.integer((int64)identity, e));
        }
        // The tree of the adjacent pairs
        uint n = segments;
        while (n > 1)
        {
            uint next = 0;
            for (uint i = 0; i + 1 < n; i += 2)
            {
                if (isFloat)
                {
                    SemanticOperand r = s.emitFloat(operation, floatScalar,
                        s.bitcast(values[i], floatScalar), s.bitcast(values[i + 1], floatScalar),
                        SemanticOperand(), (Op::Behavior)(d.m_b >> 8));
                    values[next++] = s.bitcast(r, scalar);
                } else
                {
                    values[next++] = s.emit(operation, scalar, values[i], values[i + 1]);
                }
            }
            if ((n % 2) != 0)
                values[next++] = values[n - 1];
            n = next;
        }
        SemanticOperand lane = values[0];
        if (isFloat)
            lane = s.bitcast(s.applyDefaultNan(s.bitcast(lane, floatScalar), floatScalar), scalar);
        result = s.insertLane(result, lane, j);
    }
    s.context().move(s.context().getRegister(operandOf(s, 0).m_register),
                     s.bitcast(result, SemanticType::integer(128)));
}

/////////////////////////////////////////////////////////////////////////////
// The permutes of every segment
/////////////////////////////////////////////////////////////////////////////

// dupq zd.t, zn.t[i]: the element i of every segment in the segment
void liftDupq(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint index = (uint)operandOf(s, 1).m_elementIndex;
    const SemanticType type = s.integerType(e);
    const uint segmentLanes = 128 / e;
    SemanticOperand z = s.readZOperand(1, e);
    if (s.lanes(e) <= segmentLanes)
    {
        s.writeZ(SveLift::registerNumber(operandOf(s, 0)),
                 s.emit(Op::OP_SHUFFLE, type, z, z, s.splatInteger(index, e)));
        return;
    }
    SemanticOperand base = s.emit(Op::OP_AND, type, s.laneIndex(0, 1, e),
                                  s.splatInteger(~(int64)(segmentLanes - 1), e));
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)),
             s.emit(Op::OP_SHUFFLE, type, z, z,
                    s.emit(Op::OP_ADD, type, base, s.splatInteger(index, e))));
}

// The permutes with two sources and one segment result: extq (m_a 0), tblq
// (1), tbxq (2), zipq1 (3), zipq2 (4), uzpq1 (5), uzpq2 (6)
void liftSegmentPermute(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint segmentLanes = 128 / e;
    const uint segments = s.vectorBits() / 128;
    const SemanticType type = segmentType(e);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand first, second;
    first = s.readZWhole(SveLift::registerNumber(operandOf(s, 1)));
    int64 immediate = 0;
    uint secondIndex = 2;
    if (d.m_a == 0)
        immediate = operandOf(s, 3).m_immediate;
    second = s.readZWhole(SveLift::registerNumber(operandOf(s, secondIndex)));
    SemanticOperand old = s.readZWhole(reg);
    SemanticOperand result = s.zero(s.wholeType());
    for (uint seg = 0; seg < segments; seg++)
    {
        SemanticOperand a = s.bitcast(segmentOf(s, first, seg), type);
        SemanticOperand b = s.bitcast(segmentOf(s, second, seg), type);
        SemanticOperand r;
        switch (d.m_a)
        {
        case 0:
            r = s.emit(Op::OP_SLIDE, segmentType(8), s.bitcast(a, segmentType(8)),
                       s.bitcast(b, segmentType(8)), s.integer(immediate, 32));
            r = s.bitcast(r, type);
            break;
        case 1:
        case 2:
        {
            // tblq zd, {zn}, zm: the table is zn, the indexes zm; tbxq zd, zn,
            // zm: the table is zn and the out of range keep the destination
            SemanticOperand looked = s.emitBehavior(Op::OP_SHUFFLE, type, a, s.zero(type),
                                                    Op::BEHAVIOR_SHUFFLE_ZERO, b);
            if (d.m_a == 2)
            {
                SemanticOperand inRange = s.compareLanes(Op::COND_UNSIGNED_LESS, type, b,
                    s.splat(s.integer(segmentLanes, e), type));
                looked = s.select(inRange, looked, s.bitcast(segmentOf(s, old, seg), type));
            }
            r = looked;
            break;
        }
        case 3:
            r = s.emit(Op::OP_INTERLEAVE, type, a, b, s.integer(0, 32));
            break;
        case 4:
            r = s.emit(Op::OP_INTERLEAVE, type, a, b, s.integer(segmentLanes / 2, 32));
            break;
        default:
            r = s.emit(Op::OP_SHUFFLE, type, a, b,
                       s.laneIndexWide(d.m_a == 5 ? 0 : 1, 2, segmentLanes, 16));
            break;
        }
        result = placeSegment(s, result, s.bitcast(r, SemanticType::integer(128)), seg);
    }
    s.writeZ(reg, result);
}

/////////////////////////////////////////////////////////////////////////////
// The structures of 128 bit elements
/////////////////////////////////////////////////////////////////////////////

// ld2q, ld3q, ld4q (m_a 0), st2q, st3q, st4q (m_a 1); m_b the members
void liftStructureQuad(SveLift& s, const SveDescriptor& d)
{
    const uint members = (uint)d.m_b;
    const bool isStore = d.m_a != 0;
    const ArmDecodedOperand& list = operandOf(s, 0);
    const uint first = SveLift::registerNumber(list);
    const uint stride = (list.m_stride == 0) ? 1 : list.m_stride;
    const ArmDecodedOperand& memory = operandOf(s, 2);
    const SemanticType i64 = SemanticType::integer(64);
    const uint count = s.lanes(128);
    // The address
    SemanticOperand base = s.context().getRegister(memory.m_base);
    if (memory.m_index != ARM_NO_REGISTER)
    {
        SemanticOperand offset = s.context().getRegister(memory.m_index);
        if (s.context().getArchitecture()->getRegister(memory.m_index).m_isZero)
            offset = s.integer(0, 64);
        if (memory.m_shiftAmount != 0)
            offset = s.emit(Op::OP_SHIFT_LEFT, i64, offset, s.integer(memory.m_shiftAmount, 64));
        base = s.emit(Op::OP_ADD, i64, base, offset);
    } else if (memory.m_offset != 0)
    {
        base = s.emit(Op::OP_ADD, i64, base,
                      s.integer(memory.m_offset * (int64)(s.vectorBits() / 8), 64));
    }
    SemanticOperand maskBits = s.bitcast(s.readMaskOperand(1, 128), SemanticType::integer(count));
    SemanticOperand vectors[4];
    for (uint member = 0; member < members; member++)
    {
        if (isStore)
            vectors[member] = s.readZ((first + member * stride) & 31, 128);
        else
            vectors[member] = s.zero(s.integerType(128));
    }
    for (uint lane = 0; lane < count; lane++)
    {
        SemanticOperand active = (count == 1) ? s.emit(Op::OP_TRUNCATE, SemanticType::integer(1), maskBits) :
            s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                   s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, SemanticType::integer(count), maskBits,
                          s.integer(lane, count)));
        for (uint member = 0; member < members; member++)
        {
            SemanticOperand address = s.emit(Op::OP_ADD, i64, base,
                s.integer((int64)(lane * members + member) * 16, 64));
            if (isStore)
            {
                SemanticInstruction store(Op::OP_STORE_MASKED, SemanticType(), SemanticOperand(),
                                          address, s.extractLane(vectors[member], lane), active);
                store.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
                s.context().emit(store);
            } else
            {
                SemanticOperand value = s.context().newTemporary(SemanticType::integer(128));
                SemanticInstruction load(Op::OP_LOAD_MASKED, SemanticType::integer(128), value,
                                         address, active);
                load.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
                s.context().emit(load);
                vectors[member] = s.insertLane(vectors[member], value, lane);
            }
        }
    }
    if (!isStore)
        for (uint member = 0; member < members; member++)
            s.writeZ((first + member * stride) & 31, vectors[member]);
}

#define RED(name, op, flags) { name, liftReduceSegments, Op::op, flags }

const SveDescriptor gTable[] =
{
    RED("addqv", OP_ADD, 0), RED("smaxqv", OP_MAXIMUM_SIGNED, 0), RED("umaxqv", OP_MAXIMUM_UNSIGNED, 0),
    RED("sminqv", OP_MINIMUM_SIGNED, 0), RED("uminqv", OP_MINIMUM_UNSIGNED, 0),
    RED("orqv", OP_OR, 0), RED("eorqv", OP_XOR, 0), RED("andqv", OP_AND, 0),
    RED("faddqv", OP_FLOAT_ADD, 1),
    RED("fmaxqv", OP_FLOAT_MAXIMUM, 1 | (Op::BEHAVIOR_MINMAX_PROPAGATE_NAN << 8)),
    RED("fminqv", OP_FLOAT_MINIMUM, 1 | (Op::BEHAVIOR_MINMAX_PROPAGATE_NAN << 8)),
    RED("fmaxnmqv", OP_FLOAT_MAXIMUM, 1 | (Op::BEHAVIOR_MINMAX_NUMBER << 8)),
    RED("fminnmqv", OP_FLOAT_MINIMUM, 1 | (Op::BEHAVIOR_MINMAX_NUMBER << 8)),
    { "dupq", liftDupq, 0, 0 },
    { "extq", liftSegmentPermute, 0, 0 },
    { "tblq", liftSegmentPermute, 1, 0 },
    { "tbxq", liftSegmentPermute, 2, 0 },
    { "zipq1", liftSegmentPermute, 3, 0 },
    { "zipq2", liftSegmentPermute, 4, 0 },
    { "uzpq1", liftSegmentPermute, 5, 0 },
    { "uzpq2", liftSegmentPermute, 6, 0 },
    { "ld2q", liftStructureQuad, 0, 2 }, { "ld3q", liftStructureQuad, 0, 3 },
    { "ld4q", liftStructureQuad, 0, 4 },
    { "st2q", liftStructureQuad, 1, 2 }, { "st3q", liftStructureQuad, 1, 3 },
    { "st4q", liftStructureQuad, 1, 4 },
    { NULL, NULL, 0, 0 }
};

} // namespace

void armdis::a64::liftSveQuad(ArmLiftContext& context)
{
    runSveTable(context, gTable);
}
