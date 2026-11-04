/*
 * Arm64SemanticLiftSme.cpp
 *
 * Implementation file
 *
 * The dispatcher and the helpers of the A64 SME / SME2 lifter, and the moves between za and
 * the vectors, the loads and stores of the slices, ldr / str of za and zt0, zero (see
 * Arm64SemanticSme.h).
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
#include "dismount/proc/arm/Arm64SemanticSme.h"

using namespace armdis;

typedef SemanticOperation Op;
typedef SmeLift::Val Val;

namespace {

uint log2Of(uint value)
{
    uint r = 0;
    while ((1u << r) < value)
        r++;
    return r;
}

uint elementBitsOf(uint arrangement)
{
    switch (arrangement)
    {
    case ArmDecodedOperand::ARRANGEMENT_B: return 8;
    case ArmDecodedOperand::ARRANGEMENT_H: return 16;
    case ArmDecodedOperand::ARRANGEMENT_S: return 32;
    case ArmDecodedOperand::ARRANGEMENT_D: return 64;
    case ArmDecodedOperand::ARRANGEMENT_Q: return 128;
    default: return 0;
    }
}

uint getStreamingBits(ArmLiftContext& context)
{
    const ArchitectureDescriptionPtr& architecture = context.getArchitecture();
    CHECK(architecture->getType() == OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
    return static_cast<const Arm64Architecture*>(
                    architecture.getPointer())->getStreamingVectorBits();
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// SmeLift
/////////////////////////////////////////////////////////////////////////////

SmeLift::SmeLift(ArmLiftContext& context) :
    m_context(context),
    m_simd(context),
    m_svl(getStreamingBits(context)),
    m_registerBits(context.getArchitecture()->getRegister(ARM64_REG_Z0).m_bits),
    m_hasChecks(false)
{
}

uint SmeLift::elementBits(uint index) const
{
    return elementBitsOf(operand(index).m_arrangement);
}

uint SmeLift::groupCount(uint index) const
{
    const ArmDecodedOperand& o = operand(index);
    return (o.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ? o.m_count : 1;
}

uint SmeLift::groupRegister(uint index, uint r) const
{
    const ArmDecodedOperand& o = operand(index);
    const uint first = o.m_register - ARM64_REG_Z0;
    return (o.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ?
        ((first + r * o.m_stride) & 31) : first;
}

uint SmeLift::pNumber(uint index) const
{
    return operand(index).m_register - ARM64_REG_P0;
}

void SmeLift::begin(bool isStreaming, bool isZa)
{
    m_hasChecks = isStreaming || isZa;
    if (!m_hasChecks)
        return;
    m_trapLabel = m_context.newLabel();
    m_endLabel = m_context.newLabel();
    const SemanticType type = SemanticType::integer(64);
    const uint mask = (isStreaming ? 1u : 0u) | (isZa ? 2u : 0u);
    Val svcr = m_context.getRegister(ARM64_REG_SVCR);
    Val bits = m_context.emit(Op::OP_AND, type, svcr, imm(mask, 64));
    Val isBad = m_simd.compare(Op::COND_NOT_EQUAL, type, bits, imm(mask, 64));
    m_context.branchIf(isBad, m_trapLabel);
}

void SmeLift::end()
{
    if (!m_hasChecks)
        return;
    m_context.branch(m_endLabel);
    m_context.placeLabel(m_trapLabel);
    m_context.trap(Op::TRAP_UNDEFINED, 0);
    m_context.placeLabel(m_endLabel);
}

SemanticType SmeLift::vecType(uint elementBits) const
{
    return SimdLift::V(elementBits, lanes(elementBits));
}

SemanticType SmeLift::floatVecType(uint elementBits) const
{
    return SimdLift::FV(elementBits, lanes(elementBits));
}

Val SmeLift::imm(int64 value, uint bits)
{
    return m_context.getImmediate(value, SemanticType::integer(bits));
}

Val SmeLift::z(uint number)
{
    Val r = m_context.getRegister(ARM64_REG_Z0 + number);
    if (m_registerBits > m_svl)
        r = m_context.emit(Op::OP_TRUNCATE, SemanticType::integer(m_svl), r);
    return r;
}

Val SmeLift::zv(uint number, uint elementBits)
{
    return asVec(z(number), elementBits);
}

void SmeLift::setZ(uint number, const Val& value)
{
    Val v = asBits(value);
    if (m_registerBits > m_svl)
        v = m_context.emit(Op::OP_ZERO_EXTEND,
                           SemanticType::integer(m_registerBits), v);
    m_context.move(m_context.getRegister(ARM64_REG_Z0 + number), v);
}

Val SmeLift::asVec(const Val& value, uint elementBits)
{
    return m_simd.cast(vecType(elementBits), value);
}

Val SmeLift::asBits(const Val& value)
{
    return m_simd.cast(SemanticType::integer(m_svl), value);
}

Val SmeLift::lane(const Val& vector, uint index)
{
    // (a vector of one lane is a plain integer: the 128 bit elements of a 128 bit SVL)
    if (!vector.getType().isVector())
    {
        CHECK(index == 0);
        return vector;
    }
    return m_simd.lane(vector, index);
}

Val SmeLift::setLane(const Val& vector, const Val& scalar, uint index)
{
    if (!vector.getType().isVector())
    {
        CHECK(index == 0);
        return scalar;
    }
    return m_simd.setLane(vector, scalar, index);
}

Val SmeLift::splat(const Val& scalar, uint elementBits)
{
    return m_simd.splat(vecType(elementBits), scalar);
}

Val SmeLift::zeroVec(uint elementBits)
{
    return m_simd.splat(vecType(elementBits), 0);
}

Val SmeLift::wRegister(uint registerId)
{
    return m_context.getRegister(registerId);
}

Val SmeLift::laneMask(uint predicateNumber, uint elementBits)
{
    const uint n = lanes(elementBits);
    const uint pbits = m_svl / 8;
    Val p = m_context.getRegister(ARM64_REG_P0 + predicateNumber);
    if (p.getType().getTotalBits() > pbits)
        p = m_context.emit(Op::OP_TRUNCATE, SemanticType::integer(pbits), p);
    if (elementBits == 8)
        return m_simd.cast(SimdLift::M(n), p);
    // pext with the bits at the first byte of every element
    uint64 pattern = 0;
    for (uint i = 0; i < n; i++)
        pattern |= (uint64)1 << (i * (elementBits / 8));
    Val gathered = m_context.emit(Op::OP_BIT_GATHER, SemanticType::integer(pbits),
                                  p, imm((int64)pattern, pbits));
    if (pbits != n)
        gathered = m_context.emit(Op::OP_TRUNCATE, SemanticType::integer(n),
                                  gathered);
    return m_simd.cast(SimdLift::M(n), gathered);
}

Val SmeLift::andMask(const Val& a, const Val& b)
{
    return m_context.emit(Op::OP_AND, a.getType(), a, b);
}

/////////////////////////////////////////////////////////////////////////////
// za
/////////////////////////////////////////////////////////////////////////////

SmeZa SmeLift::za(uint index) const
{
    const ArmDecodedOperand& o = operand(index);
    CHECK(o.m_kind == ArmDecodedOperand::KIND_MATRIX);
    SmeZa r;
    r.m_elementBits = elementBitsOf(o.m_arrangement);
    r.m_tile = o.m_tile;
    r.m_direction = (o.m_sliceDirection == ArmDecodedOperand::SLICE_HORIZONTAL) ? 1 :
                    (o.m_sliceDirection == ArmDecodedOperand::SLICE_VERTICAL) ? 2 : 0;
    r.m_isArray = (r.m_direction == 0) && (o.m_sliceRegister != (uint)ARM_NO_REGISTER);
    r.m_selectRegister = o.m_sliceRegister;
    r.m_offset = o.m_sliceOffset;
    r.m_offsetLast = o.m_sliceOffsetLast;
    r.m_group = o.m_vectorGroup;
    return r;
}

Val SmeLift::zaRow(uint row)
{
    return m_context.getRegister(ARM64_REG_ZA_ROW0 + row);
}

void SmeLift::setZaRow(uint row, const Val& value)
{
    m_context.move(m_context.getRegister(ARM64_REG_ZA_ROW0 + row),
                   asBits(value));
}

Val SmeLift::zaRowAt(const Val& row)
{
    SemanticType type = SemanticType::integer(m_svl);
    Val result = m_context.newTemporary(type);
    m_context.emit(SemanticInstruction(Op::OP_READ_REGISTER_INDEXED, type, result,
                        m_context.getRegister(ARM64_REG_ZA_ROW0), row,
                        imm(rows(), 32)));
    return result;
}

void SmeLift::setZaRowAt(const Val& row, const Val& value)
{
    m_context.emit(SemanticInstruction(Op::OP_WRITE_REGISTER_INDEXED,
                        SemanticType::integer(m_svl),
                        m_context.getRegister(ARM64_REG_ZA_ROW0), row,
                        imm(rows(), 32), asBits(value)));
}

Val SmeLift::sliceIndex(const SmeZa& zaOperand, uint extra, uint groupRound)
{
    const SemanticType t32 = SemanticType::integer(32);
    Val w = wRegister(zaOperand.m_selectRegister);
    if (groupRound > 1)
        w = m_context.emit(Op::OP_AND, t32, w,
                           imm(~(int64)(groupRound - 1) & 0xFFFFFFFF, 32));
    Val sum = m_context.emit(Op::OP_ADD, t32, w,
                             imm(zaOperand.m_offset + extra, 32));
    const uint dim = zaOperand.m_isArray ? rows() : lanes(zaOperand.m_elementBits);
    return m_context.emit(Op::OP_AND, t32, sum, imm(dim - 1, 32));
}

Val SmeLift::arraySelect(const SmeZa& zaOperand, uint plus, uint groupRound)
{
    const SemanticType t32 = SemanticType::integer(32);
    const uint group = zaOperand.m_group ? zaOperand.m_group : 1;
    const uint stride = rows() / group;
    Val w = wRegister(zaOperand.m_selectRegister);
    if (groupRound > 1)
        w = m_context.emit(Op::OP_AND, t32, w,
                           imm(~(int64)(groupRound - 1) & 0xFFFFFFFF, 32));
    Val sum = m_context.emit(Op::OP_ADD, t32, w,
                             imm(zaOperand.m_offset + plus, 32));
    return m_context.emit(Op::OP_AND, t32, sum, imm(stride - 1, 32));
}

Val SmeLift::tileRowOfSlice(const SmeZa& zaOperand, const Val& sliceIndexValue)
{
    const SemanticType t32 = SemanticType::integer(32);
    const uint ntiles = zaOperand.m_elementBits / 8;
    Val row = m_context.emit(Op::OP_SHIFT_LEFT, t32, sliceIndexValue,
                             imm(log2Of(ntiles), 32));
    return m_context.emit(Op::OP_ADD, t32, row, imm(zaOperand.m_tile, 32));
}

Val SmeLift::tileRow(const SmeZa& zaOperand, uint row)
{
    const uint ntiles = zaOperand.m_elementBits / 8;
    return asVec(zaRow(row * ntiles + zaOperand.m_tile), zaOperand.m_elementBits);
}

void SmeLift::setTileRow(const SmeZa& zaOperand, uint row, const Val& vector)
{
    const uint ntiles = zaOperand.m_elementBits / 8;
    setZaRow(row * ntiles + zaOperand.m_tile, vector);
}

Val SmeLift::readSlice(const SmeZa& zaOperand, const Val& sliceIndexValue)
{
    const uint esize = zaOperand.m_elementBits;
    if (zaOperand.m_direction == 1)
        return asVec(zaRowAt(tileRowOfSlice(zaOperand, sliceIndexValue)), esize);
    // vertical: element 'index' of every row of the tile
    const SemanticType t32 = SemanticType::integer(32);
    Val offset = m_context.emit(Op::OP_MULTIPLY, t32, sliceIndexValue, imm(esize, 32));
    Val result = zeroVec(esize);
    for (uint j = 0; j < lanes(esize); j++)
    {
        Val row = zaRow(j * (esize / 8) + zaOperand.m_tile);
        Val element = m_context.emit(Op::OP_EXTRACT, SemanticType::integer(esize),
                                     row, offset);
        result = setLane(result, element, j);
    }
    return result;
}

void SmeLift::writeSlice(const SmeZa& zaOperand, const Val& sliceIndexValue,
                         const Val& vector)
{
    const uint esize = zaOperand.m_elementBits;
    if (zaOperand.m_direction == 1)
    {
        setZaRowAt(tileRowOfSlice(zaOperand, sliceIndexValue), vector);
        return;
    }
    const SemanticType t32 = SemanticType::integer(32);
    const SemanticType rowType = SemanticType::integer(m_svl);
    Val offset = m_context.emit(Op::OP_MULTIPLY, t32, sliceIndexValue, imm(esize, 32));
    Val v = asVec(vector, esize);
    for (uint j = 0; j < lanes(esize); j++)
    {
        const uint row = j * (esize / 8) + zaOperand.m_tile;
        Val updated = m_context.emit(Op::OP_INSERT, rowType, zaRow(row),
                                     lane(v, j), offset);
        setZaRow(row, updated);
    }
}

Val SmeLift::deinterleave(const Val& vector, uint k, uint step)
{
    const SemanticType type = vector.getType();
    const uint n = type.getLanes();
    const uint bits = type.getElementBits();
    const SemanticType indexType = SimdLift::V(bits, n);
    Val indexes = m_context.emit(Op::OP_LANE_INDEX, indexType, imm(k, bits), imm(step, bits));
    Val shuffled = m_context.emit(Op::OP_SHUFFLE, type, vector, vector, indexes);
    Val part = m_context.emit(Op::OP_TRUNCATE, SemanticType::integer(n * bits / step),
                              m_simd.cast(SemanticType::integer(n * bits), shuffled));
    return m_simd.cast(SimdLift::V(bits, n / step), part);
}

Val SmeLift::indexedVector(const Val& vector, uint groupBits, uint index)
{
    const uint n = lanes(groupBits);
    const uint per = 128 / groupBits;
    const SemanticType type = vecType(groupBits);
    Val v = asVec(vector, groupBits);
    Val j = m_context.emit(Op::OP_LANE_INDEX, type, imm(0, groupBits), imm(1, groupBits));
    uint shift = 0;
    while ((1u << shift) < per)
        shift++;
    Val base = j;
    if (shift != 0)
    {
        Val count = m_simd.splat(type, shift);
        base = m_context.emit(Op::OP_SHIFT_LEFT, type,
                              m_context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, j, count), count);
    }
    Val picked = m_context.emit(Op::OP_ADD, type, base, m_simd.splat(type, index));
    (void)n;
    return m_context.emit(Op::OP_SHUFFLE, type, v, v, picked);
}

Val SmeLift::extendLanes(const Val& vector, uint toBits, bool isSigned)
{
    const SemanticType type = vector.getType();
    return m_context.emit(isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                          SimdLift::V(toBits, type.getLanes()), vector);
}

Val SmeLift::deinterleaveBits(const Val& bits, uint k)
{
    const uint n = bits.getType().getTotalBits();
    uint64 pattern = 0;
    for (uint i = k; i < n; i += 2)
        pattern |= (uint64)1 << i;
    return m_context.emit(Op::OP_BIT_GATHER, bits.getType(), bits, imm((int64)pattern, n));
}

Val SmeLift::bitOf(const Val& bits, uint index)
{
    return m_context.emit(Op::OP_EXTRACT, SemanticType::integer(1), bits, imm(index, 32));
}

Val SmeLift::maskToBits(const Val& mask)
{
    return m_simd.cast(SemanticType::integer(mask.getType().getTotalBits()), mask);
}

Val SmeLift::bitsToMask(const Val& bits)
{
    return m_simd.cast(SimdLift::M(bits.getType().getTotalBits()), bits);
}

Val SmeLift::gateMask(const Val& condition, const Val& mask)
{
    const uint n = mask.getType().getTotalBits();
    Val gated = m_context.emit(Op::OP_SELECT, SemanticType::integer(n), condition,
                               maskToBits(mask), imm(0, n));
    return bitsToMask(gated);
}

Val SmeLift::flushHalf(const Val& vector)
{
    const SemanticType type = vector.getType();
    const uint n = type.getLanes();
    const SemanticType t32 = SemanticType::integer(32);
    Val fpcr = m_context.getRegister(ARM64_REG_FPCR);
    Val shifted = m_context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, SemanticType::integer(64),
                                 fpcr, imm(19, 32));
    Val flag = m_context.emit(Op::OP_TRUNCATE, SemanticType::integer(1), shifted);
    Val exponent = m_context.emit(Op::OP_AND, type, vector, m_simd.splat(type, 0x7c00));
    Val fraction = m_context.emit(Op::OP_AND, type, vector, m_simd.splat(type, 0x03ff));
    Val zeroExp = m_simd.compare(Op::COND_EQUAL, type, exponent, m_simd.splat(type, 0));
    Val hasFraction = m_simd.compare(Op::COND_NOT_EQUAL, type, fraction, m_simd.splat(type, 0));
    Val denormal = m_context.emit(Op::OP_AND, SimdLift::M(n), zeroExp, hasFraction);
    Val flushed = m_context.emit(Op::OP_AND, type, vector, m_simd.splat(type, 0x8000));
    (void)t32;
    return m_simd.select(gateMask(flag, denormal), flushed, vector);
}

Val SmeLift::flipSign(const Val& value, uint elementBits)
{
    const int64 sign = (elementBits == 64) ? (int64)0x8000000000000000ULL :
                       ((int64)1 << (elementBits - 1));
    if (value.getType().isVector())
        return m_context.emit(Op::OP_XOR, value.getType(), value,
                              m_simd.splat(value.getType(), imm(sign, elementBits)));
    return m_context.emit(Op::OP_XOR, value.getType(), value, imm(sign, elementBits));
}

/////////////////////////////////////////////////////////////////////////////
// Floating point
/////////////////////////////////////////////////////////////////////////////

Val SmeLift::fp(Op::Operation operation, uint elementBits, const Val& a,
                const Val& b, const Val& c, bool isZa, Op::Behavior behavior,
                bool isBFloat)
{
    const uint lanesCount = a.getType().isVector() ? a.getType().getLanes() : 1;
    const SemanticType type = isBFloat ?
        SemanticType::vector(SemanticType::floating(SemanticType::FLOAT_BFLOAT16), lanesCount) :
        SimdLift::FV(elementBits, lanesCount);
    Val fa = m_simd.cast(type, a);
    Val fb = b.isValid() ? m_simd.cast(type, b) : Val();
    Val fc = c.isValid() ? m_simd.cast(type, c) : Val();
    Val result = m_context.newTemporary(type);
    SemanticInstruction instruction(operation, type, result, fa, fb, fc);
    instruction.setBehavior(behavior);
    instruction.setAttributes(isZa ?
        SemanticInstruction::ATTRIBUTE_FLOAT_DEFAULT_NAN :
        SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    m_context.emit(instruction);
    return m_simd.cast(a.getType(), result);
}

Val SmeLift::fpConvert(const Val& a, SemanticType::FloatFormat from,
                       SemanticType::FloatFormat to, uint lanesCount, bool isZa)
{
    const uint fromBits = (from == SemanticType::FLOAT_DOUBLE) ? 64 :
                          (from == SemanticType::FLOAT_SINGLE) ? 32 : 16;
    const uint toBits = (to == SemanticType::FLOAT_DOUBLE) ? 64 :
                        (to == SemanticType::FLOAT_SINGLE) ? 32 : 16;
    const SemanticType fromType = lanesCount == 1 ?
        SemanticType::floating(from) :
        SemanticType::vector(SemanticType::floating(from), lanesCount);
    const SemanticType toType = lanesCount == 1 ?
        SemanticType::floating(to) :
        SemanticType::vector(SemanticType::floating(to), lanesCount);
    Val result = m_context.newTemporary(toType);
    SemanticInstruction instruction(Op::OP_FLOAT_CONVERT, toType, result,
                                    m_simd.cast(fromType, a));
    // (the half precision of the SME instructions is the IEEE one, whatever FPCR.AHP is)
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF |
        (isZa ? SemanticInstruction::ATTRIBUTE_FLOAT_DEFAULT_NAN :
                SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS));
    m_context.emit(instruction);
    (void)fromBits;
    return m_simd.cast(SimdLift::V(toBits, lanesCount), result);
}

/////////////////////////////////////////////////////////////////////////////
// The dispatcher
/////////////////////////////////////////////////////////////////////////////

void armdis::liftArm64Sme(ArmLiftContext& context)
{
    SmeLift m(context);
    if (liftSmeMove(m) || liftSmeOuter(m) || liftSmeArray(m) || liftSmeVector(m) ||
        liftSmeConvert(m))
        return;
    a64::undefined(context);
}

/////////////////////////////////////////////////////////////////////////////
// The moves, the loads and the stores
/////////////////////////////////////////////////////////////////////////////

namespace {

// The z register numbers of a register or a list operand
uint listCount(SmeLift& m, uint index)
{
    const ArmDecodedOperand& o = m.operand(index);
    return (o.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ? o.m_count : 1;
}

uint listRegister(SmeLift& m, uint index, uint r)
{
    const ArmDecodedOperand& o = m.operand(index);
    uint first = o.m_register - ARM64_REG_Z0;
    if (o.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST)
        return (first + r * o.m_stride) & 31;
    return first;
}

bool isZaOperand(SmeLift& m, uint index)
{
    return m.operand(index).m_kind == ArmDecodedOperand::KIND_MATRIX;
}

// mova between a z register (merging) and a slice
void liftMovaSingle(SmeLift& m, bool isToZ)
{
    ArmLiftContext& c = m.c();
    const SmeZa za = m.za(isToZ ? 2 : 0);
    const uint zIndex = isToZ ? 0 : 2;
    const uint zNumber = m.operand(zIndex).m_register - ARM64_REG_Z0;
    const uint predicate = m.operand(1).m_register - ARM64_REG_P0;
    const uint esize = za.m_elementBits;
    m.begin(true, true);
    Val index = m.sliceIndex(za, 0, 1);
    Val mask = m.laneMask(predicate, esize);
    Val slice = m.readSlice(za, index);
    if (isToZ)
    {
        Val merged = m.s().select(mask, slice, m.zv(zNumber, esize));
        m.setZ(zNumber, merged);
    } else
    {
        Val merged = m.s().select(mask, m.zv(zNumber, esize), slice);
        m.writeSlice(za, index, merged);
    }
    m.end();
    (void)c;
}

// mova of a vector group and slices / array vectors, movaz (zeroing)
void liftMovaGroup(SmeLift& m, bool isToZ, bool isZeroing)
{
    const uint zaIndex = isToZ ? 1 : 0;
    const uint zIndex = isToZ ? 0 : 1;
    const SmeZa za = m.za(zaIndex);
    const uint nreg = listCount(m, zIndex);
    m.begin(true, true);
    // all the sources are read before the destinations are written
    if (isToZ)
    {
        Val read[4];
        for (uint r = 0; r < nreg; r++)
        {
            if (za.m_isArray)
            {
                const uint stride = m.rows() / nreg;
                Val base = m.arraySelect(za, 0, 1);
                Val row = m.c().emit(Op::OP_ADD, SemanticType::integer(32), base,
                                     m.imm(r * stride, 32));
                read[r] = m.zaRowAt(row);
            } else
            {
                Val index = m.sliceIndex(za, r, nreg);
                read[r] = m.asBits(m.readSlice(za, index));
            }
        }
        for (uint r = 0; r < nreg; r++)
            m.setZ(listRegister(m, zIndex, r), read[r]);
        if (isZeroing)
        {
            Val zero = m.zeroVec(za.m_elementBits);
            for (uint r = 0; r < nreg; r++)
            {
                if (za.m_isArray)
                {
                    const uint stride = m.rows() / nreg;
                    Val base = m.arraySelect(za, 0, 1);
                    Val row = m.c().emit(Op::OP_ADD, SemanticType::integer(32),
                                         base, m.imm(r * stride, 32));
                    m.setZaRowAt(row, zero);
                } else
                {
                    m.writeSlice(za, m.sliceIndex(za, r, nreg), zero);
                }
            }
        }
    } else
    {
        Val source[4];
        for (uint r = 0; r < nreg; r++)
            source[r] = m.z(listRegister(m, zIndex, r));
        for (uint r = 0; r < nreg; r++)
        {
            if (za.m_isArray)
            {
                const uint stride = m.rows() / nreg;
                Val base = m.arraySelect(za, 0, 1);
                Val row = m.c().emit(Op::OP_ADD, SemanticType::integer(32), base,
                                     m.imm(r * stride, 32));
                m.setZaRowAt(row, source[r]);
            } else
            {
                m.writeSlice(za, m.sliceIndex(za, r, nreg), source[r]);
            }
        }
    }
    m.end();
}

// ld1b..ld1q / st1b..st1q of a slice
void liftSliceMemory(SmeLift& m, bool isLoad)
{
    ArmLiftContext& c = m.c();
    const SmeZa za = m.za(0);
    const uint predicate = m.operand(1).m_register - ARM64_REG_P0;
    const uint esize = za.m_elementBits;
    const uint n = m.lanes(esize);
    m.begin(true, true);
    Val index = m.sliceIndex(za, 0, 1);
    Val address = c.getAddress(2);
    const SemanticType type = SemanticType::integer(64);
    Val mask = m.laneMask(predicate, esize);
    Val maskBits = m.s().cast(SemanticType::integer(n), mask);
    Val vector;
    if (isLoad)
        vector = c.newTemporary(m.vecType(esize));
    else
        vector = m.readSlice(za, index);
    if (isLoad)
        c.move(vector, m.zeroVec(esize));
    for (uint e = 0; e < n; e++)
    {
        Val skip = c.newLabel();
        Val bit = c.emit(Op::OP_EXTRACT, SemanticType::integer(1), maskBits,
                         m.imm(e, 32));
        c.branchIf(m.s().compare(Op::COND_EQUAL, SemanticType::integer(1), bit,
                                 m.imm(0, 1)), skip);
        Val where = c.emit(Op::OP_ADD, type, address, m.imm((int64)e * (esize / 8), 64));
        if (isLoad)
        {
            Val element = c.load(where, SemanticType::integer(esize));
            c.move(vector, m.setLane(vector, element, e));
        } else
        {
            c.store(where, m.lane(vector, e));
        }
        c.placeLabel(skip);
    }
    if (isLoad)
        m.writeSlice(za, index, vector);
    m.end();
}

uint gprNumber(uint id)
{
    return ((id == (uint)ARM64_REG_XZR) || (id == (uint)ARM64_REG_SP)) ? 31 : (id - ARM64_REG_X0);
}

Val zaBaseAddress(SmeLift& m)
{
    // ldr / str: the base register (x or sp)
    return m.c().getRegister(m.operand(1).m_base);
}

} // namespace

bool armdis::liftSmeMove(SmeLift& m)
{
    ArmLiftContext& c = m.c();
    const bool isMova = m.is("mova") || m.is("mov");
    if (isMova)
    {
        const ArmDecodedOperand& o0 = m.operand(0);
        const ArmDecodedOperand& o1 = m.operand(1);
        if (o0.m_kind == ArmDecodedOperand::KIND_REGISTER && o1.m_kind == ArmDecodedOperand::KIND_REGISTER)
        {
            // mova z0.q, p0/m, za0h.q[w12, 0]
            liftMovaSingle(m, true);
            return true;
        }
        if (o0.m_kind == ArmDecodedOperand::KIND_MATRIX &&
            o1.m_kind == ArmDecodedOperand::KIND_REGISTER)
        {
            liftMovaSingle(m, false);
            return true;
        }
        if (o0.m_kind == ArmDecodedOperand::KIND_MATRIX)
        {
            liftMovaGroup(m, false, false);
            return true;
        }
        liftMovaGroup(m, true, false);
        return true;
    }
    if (m.is("movaz"))
    {
        liftMovaGroup(m, true, true);
        return true;
    }
    if (m.is("ld1b") || m.is("ld1h") || m.is("ld1w") || m.is("ld1d") || m.is("ld1q"))
    {
        liftSliceMemory(m, true);
        return true;
    }
    if (m.is("st1b") || m.is("st1h") || m.is("st1w") || m.is("st1d") || m.is("st1q"))
    {
        liftSliceMemory(m, false);
        return true;
    }
    if (m.is("ldr") || m.is("str"))
    {
        const bool isLoad = m.is("ldr");
        const ArmDecodedOperand& o0 = m.operand(0);
        m.begin(false, true);
        Val base = zaBaseAddress(m);
        if (o0.m_kind == ArmDecodedOperand::KIND_REGISTER)
        {
            // zt0
            if (isLoad)
                c.move(c.getRegister(ARM64_REG_ZT0),
                       c.load(base, SemanticType::integer(512)));
            else
                c.store(base, c.getRegister(ARM64_REG_ZT0));
        } else
        {
            const SmeZa za = m.za(0);
            const uint imm = (uint)m.operand(1).m_offset;
            const SemanticType t32 = SemanticType::integer(32);
            Val sum = c.emit(Op::OP_ADD, t32, m.wRegister(za.m_selectRegister),
                             m.imm(za.m_offset, 32));
            Val row = c.emit(Op::OP_AND, t32, sum, m.imm(m.rows() - 1, 32));
            Val address = c.emit(Op::OP_ADD, SemanticType::integer(64), base,
                                 m.imm((int64)imm * m.rows(), 64));
            if (isLoad)
                m.setZaRowAt(row, c.load(address, SemanticType::integer(m.svl())));
            else
                c.store(address, m.zaRowAt(row));
        }
        m.end();
        return true;
    }
    if (m.is("zero"))
    {
        const ArmDecodedOperand& o0 = m.operand(0);
        m.begin(false, true);
        if (o0.m_kind == ArmDecodedOperand::KIND_REGISTER)
        {
            c.move(c.getRegister(ARM64_REG_ZT0),
                   m.c().getImmediate(0, SemanticType::integer(512)));
        } else if (o0.m_vectorGroup != 0 || o0.m_sliceRegister != (uint)ARM_NO_REGISTER)
        {
            // the array vectors of SME2.1 (not lifted)
            m.end();
            return false;
        } else
        {
            const uint mask = o0.m_registerMask;
            for (uint r = 0; r < m.rows(); r++)
                if (((mask >> (r % 8)) & 1) != 0)
                    m.setZaRow(r, m.c().getImmediate(0, SemanticType::integer(m.svl())));
        }
        m.end();
        return true;
    }
    if (m.is("rdsvl") || m.is("addsvl") || m.is("addspl"))
    {
        // the multiples of the streaming vector length (SVL / 8 bytes) and of the predicate length;
        // not streaming instructions, they don't read SVCR
        const bool isRead = m.is("rdsvl");
        const int64 unit = m.is("addspl") ? (m.svl() / 64) : (m.svl() / 8);
        const int64 offset = m.operand(m.count() - 1).m_immediate * unit;
        const SemanticType t64 = SemanticType::integer(64);
        Val result;
        if (isRead)
        {
            result = m.imm(offset, 64);
        } else
        {
            Val base = a64::readGpr(c, gprNumber(m.operand(1).m_register), true, true);
            result = c.emit(Op::OP_ADD, t64, base, m.imm(offset, 64));
        }
        a64::writeGpr(c, gprNumber(m.operand(0).m_register), true, !isRead, result);
        return true;
    }
    if (m.is("movt"))
    {
        const ArmDecodedOperand& o0 = m.operand(0);
        const ArmDecodedOperand& o1 = m.operand(1);
        m.begin(false, true);
        if (o0.m_kind == ArmDecodedOperand::KIND_REGISTER && o0.m_register != ARM64_REG_ZT0)
        {
            // movt x0, zt0[off]
            Val zt = c.getRegister(ARM64_REG_ZT0);
            Val v = c.emit(Op::OP_EXTRACT, SemanticType::integer(64), zt,
                           m.imm(o1.m_elementIndex * 8, 32));
            a64::writeGpr(c, gprNumber(o0.m_register), true, false, v);
        } else if (o0.m_register == ARM64_REG_ZT0 && o1.m_kind == ArmDecodedOperand::KIND_REGISTER &&
                   o1.m_register >= ARM64_REG_Z0 && o1.m_register <= ARM64_REG_Z31)
        {
            m.end();
            return false;
        } else
        {
            Val v = a64::readGpr(c, gprNumber(o1.m_register), true, false);
            Val zt = c.getRegister(ARM64_REG_ZT0);
            Val updated = c.emit(Op::OP_INSERT, SemanticType::integer(512), zt, v,
                                 m.imm(o0.m_elementIndex * 8, 32));
            c.move(zt, updated);
        }
        m.end();
        return true;
    }
    (void)isZaOperand;
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// The mode changes: smstart, smstop, msr svcr
/////////////////////////////////////////////////////////////////////////////

namespace {

// Zero the register 'registerId'
void zeroRegister(ArmLiftContext& c, uint registerId)
{
    SemanticOperand reg = c.getRegister(registerId);
    c.move(reg, c.getImmediate(0, reg.getType()));
}

// Change the bit 'bit' of SVCR to the 1 bit value 'newValue'; when it changes 'body' runs (the
// registers the change resets)
void changeBit(ArmLiftContext& c, uint bit, const SemanticOperand& newValue,
               void (*body)(ArmLiftContext&))
{
    const SemanticType t64 = SemanticType::integer(64);
    SemanticOperand svcr = c.getRegister(ARM64_REG_SVCR);
    SemanticOperand wanted = c.emit(Op::OP_ZERO_EXTEND, t64, newValue);
    SemanticOperand old = c.emit(Op::OP_AND, t64,
                                 c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t64, svcr,
                                        c.getImmediate(bit, t64)),
                                 c.getImmediate(1, t64));
    SemanticOperand skip = c.newLabel();
    SemanticInstruction compare(Op::OP_COMPARE, t64, c.newTemporary(SemanticType::integer(1)),
                                old, wanted);
    compare.setCondition(Op::COND_EQUAL);
    c.emit(compare);
    c.branchIf(compare.getDestination(), skip);
    body(c);
    SemanticOperand cleared = c.emit(Op::OP_AND, t64, c.getRegister(ARM64_REG_SVCR),
                                     c.getImmediate(~((int64)1 << bit), t64));
    c.move(c.getRegister(ARM64_REG_SVCR),
           c.emit(Op::OP_OR, t64, cleared,
                  c.emit(Op::OP_SHIFT_LEFT, t64, wanted, c.getImmediate(bit, t64))));
    c.placeLabel(skip);
}

// A change of the streaming mode zeroes z, p, ffr (so v) and sets FPSR to 0x0800009F
void resetStreaming(ArmLiftContext& c)
{
    for (uint i = 0; i < 32; i++)
        zeroRegister(c, ARM64_REG_Z0 + i);
    for (uint i = 0; i < 16; i++)
        zeroRegister(c, ARM64_REG_P0 + i);
    zeroRegister(c, ARM64_REG_FFR);
    SemanticOperand fpsr = c.getRegister(ARM64_REG_FPSR);
    c.move(fpsr, c.getImmediate(0x0800009F, fpsr.getType()));
}

// A change of the ZA enable zeroes za and zt0
void resetZa(ArmLiftContext& c)
{
    const uint svl = getStreamingBits(c);
    for (uint i = 0; i < svl / 8; i++)
        zeroRegister(c, ARM64_REG_ZA_ROW0 + i);
    zeroRegister(c, ARM64_REG_ZT0);
}

} // namespace

void armdis::liftSmeModeChange(ArmLiftContext& context, uint crm)
{
    const SemanticType bit = SemanticType::integer(1);
    SemanticOperand value = context.getImmediate(crm & 1, bit);
    if ((crm & 2) != 0)
        changeBit(context, 0, value, resetStreaming);
    if ((crm & 4) != 0)
        changeBit(context, 1, value, resetZa);
}

void armdis::liftSmeWriteSvcr(ArmLiftContext& context, const SemanticOperand& value)
{
    const SemanticType bit = SemanticType::integer(1);
    SemanticOperand streaming = context.emit(Op::OP_TRUNCATE, bit, value);
    SemanticOperand za = context.emit(Op::OP_TRUNCATE, bit,
        context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, value.getType(), value,
                     context.getImmediate(1, value.getType())));
    changeBit(context, 0, streaming, resetStreaming);
    changeBit(context, 1, za, resetZa);
}
