/*
 * RiscvSemanticVector.cpp
 *
 * Implementation file
 *
 * The infrastructure of the vector lifters of RISC-V: the frame (the vill
 * check and the dispatch on SEW and LMUL), the register group helpers, the
 * chunk engine for the instructions which work on each element on its own,
 * the element loop and the fixed point rounding. See RiscvSemanticVector.h.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"
#include "dismount/proc/riscv/RiscvSemanticVector.h"

namespace riscvdis {

namespace {

// Return log2 of a power of two
uint log2Of(uint value)
{
    uint result = 0;
    while (((uint)1 << result) < value)
        result++;
    return result;
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// RvvType, RvvGroup, RvvPlan
/////////////////////////////////////////////////////////////////////////////

uint RvvType::registers() const
{
    return (m_lmulLog2 > 0) ? ((uint)1 << m_lmulLog2) : 1;
}

uint RvvType::vlmax(uint vlen) const
{
    const uint perRegister = vlen / m_sew;
    return (m_lmulLog2 >= 0) ? (perRegister << m_lmulLog2)
                             : (perRegister >> (-m_lmulLog2));
}

uint RvvGroup::registers() const
{
    return (m_emulLog2 > 0) ? ((uint)1 << m_emulLog2) : 1;
}

RvvPlan::RvvPlan() :
    m_shape(SHAPE_SAME),
    m_factor(1),
    m_extendA(EXTEND_ZERO),
    m_extendB(EXTEND_ZERO),
    m_isImmediateUnsigned(false),
    m_mask(MASK_BY_V0),
    m_readsDestination(false),
    m_setsSaturation(false),
    m_hasNoB(false),
    m_sanitizeA(-1),
    m_sanitizeB(-1),
    m_readScalar(NULL),
    m_op(NULL),
    m_operation(0),
    m_argument(0)
{
}

/////////////////////////////////////////////////////////////////////////////
// RvvChunk
/////////////////////////////////////////////////////////////////////////////

RvvChunk::RvvChunk(RvvLift& lift, const RvvType& type, const RvvPlan& plan) :
    m_lift(lift),
    m_type(type),
    m_plan(plan),
    m_index(0),
    m_lanes(0),
    m_firstElement(0),
    m_bits(0)
{
}

SemanticOperand RvvChunk::sanitize(const SemanticOperand& value, int64 neutral)
{
    const SemanticType type = value.getType();
    return m_lift.select(m_active, value, m_lift.splat(neutral, type));
}

/////////////////////////////////////////////////////////////////////////////
// RvvLift: types and values
/////////////////////////////////////////////////////////////////////////////

RvvLift::RvvLift(RiscvLiftContext& context) :
    m_context(context),
    m_xlen(context.getXlen()),
    m_vlen(context.getVectorBits())
{
}

uint32 RvvLift::encoding() const
{
    return m_context.getInstruction().getEncoding();
}

uint RvvLift::field(uint position, uint width) const
{
    return (uint)((encoding() >> position) & (((uint32)1 << width) - 1));
}

int64 RvvLift::simm5() const
{
    const uint value = field(15, 5);
    return (value >= 16) ? ((int64)value - 32) : (int64)value;
}

SemanticType RvvLift::integerType(uint bits) const
{
    return SemanticType::integer(bits);
}

SemanticType RvvLift::vectorType(uint elementBits, uint lanes) const
{
    return SemanticType::vector(SemanticType::integer(elementBits), lanes);
}

SemanticType RvvLift::maskType(uint lanes) const
{
    return SemanticType::mask(lanes);
}

SemanticType RvvLift::xlenType() const
{
    return SemanticType::integer(m_xlen);
}

SemanticOperand RvvLift::imm(int64 value, const SemanticType& type) const
{
    return SemanticOperand::immediate(value, type);
}

SemanticOperand RvvLift::immX(int64 value) const
{
    return SemanticOperand::immediate(value, xlenType());
}

SemanticOperand RvvLift::emit(SemanticOperation::Operation operation,
                              const SemanticType& type,
                              const SemanticOperand& a,
                              const SemanticOperand& b,
                              const SemanticOperand& c)
{
    return m_context.emit(operation, type, a, b, c);
}

SemanticOperand RvvLift::emitBehavior(SemanticOperation::Operation operation,
                                      const SemanticType& type,
                                      const SemanticOperand& a,
                                      const SemanticOperand& b,
                                      SemanticOperation::Behavior behavior)
{
    SemanticOperand result = m_context.newTemporary(type);
    SemanticInstruction instruction(operation, type, result, a, b);
    instruction.setBehavior(behavior);
    m_context.emit(instruction);
    return result;
}

void RvvLift::move(const SemanticOperand& destination,
                   const SemanticOperand& source)
{
    m_context.move(destination, source);
}

SemanticOperand RvvLift::compare(SemanticOperation::Condition condition,
                                 const SemanticType& type,
                                 const SemanticOperand& a,
                                 const SemanticOperand& b)
{
    SemanticOperand result = m_context.newTemporary(
                type.isVector() ? SemanticType::mask(type.getLanes())
                                : SemanticType::integer(1));
    SemanticInstruction instruction(SemanticOperation::OP_COMPARE, type,
                                    result, a, b);
    instruction.setCondition(condition);
    m_context.emit(instruction);
    return result;
}

SemanticOperand RvvLift::select(const SemanticOperand& condition,
                                const SemanticOperand& a,
                                const SemanticOperand& b)
{
    return emit(SemanticOperation::OP_SELECT, a.getType(), condition, a, b);
}

SemanticOperand RvvLift::bitcast(const SemanticOperand& value,
                                 const SemanticType& type)
{
    if (value.getType() == type)
        return value;
    return emit(SemanticOperation::OP_BITCAST, type, value);
}

SemanticOperand RvvLift::zeroExtend(const SemanticOperand& value, uint bits)
{
    const uint valueBits = value.getType().getTotalBits();
    if (valueBits == bits)
        return value;
    if (valueBits > bits)
        return emit(SemanticOperation::OP_TRUNCATE, integerType(bits), value);
    return emit(SemanticOperation::OP_ZERO_EXTEND, integerType(bits), value);
}

SemanticOperand RvvLift::signExtend(const SemanticOperand& value, uint bits)
{
    const uint valueBits = value.getType().getTotalBits();
    if (valueBits == bits)
        return value;
    if (valueBits > bits)
        return emit(SemanticOperation::OP_TRUNCATE, integerType(bits), value);
    return emit(SemanticOperation::OP_SIGN_EXTEND, integerType(bits), value);
}

SemanticOperand RvvLift::truncate(const SemanticOperand& value, uint bits)
{
    if (value.getType().getTotalBits() == bits)
        return value;
    return emit(SemanticOperation::OP_TRUNCATE, integerType(bits), value);
}

SemanticOperand RvvLift::extendLanes(const SemanticOperand& value, uint bits,
                                     bool isSigned)
{
    const SemanticType type = value.getType();
    if (type.getElementBits() == bits)
        return value;
    CHECK(type.getElementBits() < bits);
    return emit(isSigned ? SemanticOperation::OP_SIGN_EXTEND :
                           SemanticOperation::OP_ZERO_EXTEND,
                vectorType(bits, type.getLanes()), value);
}

SemanticOperand RvvLift::truncateLanes(const SemanticOperand& value, uint bits)
{
    const SemanticType type = value.getType();
    if (type.getElementBits() == bits)
        return value;
    CHECK(type.getElementBits() > bits);
    // OP_NARROW of the vector with itself: the low half is the narrowed lanes
    const uint lanes = type.getLanes();
    SemanticOperand both = emit(SemanticOperation::OP_NARROW,
                                vectorType(bits, lanes * 2), value, value);
    return extractBits(both, immX(0), vectorType(bits, lanes));
}

SemanticOperand RvvLift::broadcast(const SemanticOperand& value,
                                   const SemanticType& vectorType)
{
    return emit(SemanticOperation::OP_BROADCAST, vectorType, value);
}

SemanticOperand RvvLift::splat(int64 value, const SemanticType& vectorType)
{
    return broadcast(imm(value, vectorType.getElementType()), vectorType);
}

SemanticOperand RvvLift::extractBits(const SemanticOperand& value,
                                     const SemanticOperand& offset,
                                     const SemanticType& type)
{
    return emit(SemanticOperation::OP_EXTRACT, type, value, offset);
}

SemanticOperand RvvLift::insertBits(const SemanticOperand& value,
                                    const SemanticOperand& piece,
                                    const SemanticOperand& offset)
{
    return emit(SemanticOperation::OP_INSERT, value.getType(), value, piece,
                offset);
}

SemanticOperand RvvLift::allLanes(uint lanes)
{
    return emit(SemanticOperation::OP_ACTIVE_LANE_MASK, maskType(lanes),
                immX(0), immX(lanes));
}

SemanticOperand RvvLift::lanesBelow(uint lanes, const SemanticOperand& first,
                                    const SemanticOperand& limit)
{
    return emit(SemanticOperation::OP_ACTIVE_LANE_MASK, maskType(lanes),
                first, limit);
}

SemanticOperand RvvLift::conditionMask(const SemanticOperand& condition,
                                       uint lanes)
{
    CHECK(lanes <= 64);
    const SemanticType type = integerType(lanes);
    const int64 ones = (lanes >= 64) ? (int64)-1 :
                                (int64)(((uint64)1 << lanes) - 1);
    SemanticOperand chosen = emit(SemanticOperation::OP_SELECT, type,
                                  condition, imm(ones, type), imm(0, type));
    return bitcast(chosen, maskType(lanes));
}

SemanticOperand RvvLift::isNonZero(const SemanticOperand& value)
{
    return compare(SemanticOperation::COND_NOT_EQUAL, value.getType(), value,
                   imm(0, value.getType()));
}

/////////////////////////////////////////////////////////////////////////////
// RvvLift: registers
/////////////////////////////////////////////////////////////////////////////

SemanticOperand RvvLift::readRegister(uint number)
{
    CHECK(number < 32);
    return emit(SemanticOperation::OP_MOVE, integerType(m_vlen),
                m_context.getRegister(RISCV_REG_V0 + number));
}

void RvvLift::writeRegister(uint number, const SemanticOperand& value)
{
    CHECK(number < 32);
    m_context.move(m_context.getRegister(RISCV_REG_V0 + number),
                   bitcast(value, integerType(m_vlen)));
}

SemanticOperand RvvLift::readRegisterIndexed(const SemanticOperand& number)
{
    SemanticOperand result = m_context.newTemporary(integerType(m_vlen));
    m_context.emit(SemanticInstruction(
            SemanticOperation::OP_READ_REGISTER_INDEXED, integerType(m_vlen),
            result, m_context.getRegister(RISCV_REG_V0), number,
            immX(32)));
    return result;
}

void RvvLift::writeRegisterIndexed(const SemanticOperand& number,
                                   const SemanticOperand& value)
{
    m_context.emit(SemanticInstruction(
            SemanticOperation::OP_WRITE_REGISTER_INDEXED,
            integerType(m_vlen), m_context.getRegister(RISCV_REG_V0),
            number, immX(32), bitcast(value, integerType(m_vlen))));
}

SemanticOperand RvvLift::readX(uint number)
{
    if (number == 0)
        return immX(0);
    return emit(SemanticOperation::OP_MOVE, xlenType(),
                m_context.getRegister(RISCV_REG_X0 + number));
}

void RvvLift::writeX(uint number, const SemanticOperand& value)
{
    if (number == 0)
        return;
    SemanticOperand source = value;
    if (source.getType().getTotalBits() < m_xlen)
        source = zeroExtend(source, m_xlen);
    else if (source.getType().getTotalBits() > m_xlen)
        source = truncate(source, m_xlen);
    m_context.move(m_context.getRegister(RISCV_REG_X0 + number), source);
}

SemanticOperand RvvLift::readScalar(uint number, uint bits)
{
    SemanticOperand x = readX(number);
    if (m_xlen > bits)
        return truncate(x, bits);
    if (m_xlen < bits)
        return signExtend(x, bits);
    return x;
}

SemanticOperand RvvLift::readFloatScalar(uint bits, uint64 canonicalNan)
{
    CHECK((bits == 16) || (bits == 32) || (bits == 64));
    const SemanticType registerType = integerType(64);
    SemanticOperand value = emit(SemanticOperation::OP_MOVE, registerType,
                                 m_context.getRegister(RISCV_REG_F0 + rs1()));
    if (bits == 64)
        return value;
    // The value is boxed when the bits above it are all ones
    SemanticOperand upper = emit(SemanticOperation::OP_SHIFT_RIGHT_LOGICAL,
                                 registerType, value, imm(bits, registerType));
    const uint64 ones = ((uint64)1 << (64 - bits)) - 1;
    SemanticOperand isBoxed = compare(SemanticOperation::COND_EQUAL,
                                      registerType, upper,
                                      imm((int64)ones, registerType));
    return select(isBoxed, truncate(value, bits),
                  imm((int64)canonicalNan, integerType(bits)));
}

void RvvLift::writeFloatRegister(uint number, const SemanticOperand& value)
{
    const uint bits = value.getType().getTotalBits();
    CHECK((bits == 16) || (bits == 32) || (bits == 64));
    SemanticOperand pattern = zeroExtend(value, 64);
    if (bits < 64)
        pattern = emit(SemanticOperation::OP_OR, integerType(64), pattern,
                       imm((int64)~(((uint64)1 << bits) - 1), integerType(64)));
    move(m_context.getRegister(RISCV_REG_F0 + number), pattern);
}

SemanticOperand RvvLift::vlRegister()
{
    return m_context.getRegister(RISCV_REG_VL);
}

SemanticOperand RvvLift::vstartRegister()
{
    return m_context.getRegister(RISCV_REG_VSTART);
}

SemanticOperand RvvLift::vtypeRegister()
{
    return m_context.getRegister(RISCV_REG_VTYPE);
}

void RvvLift::readState()
{
    m_vl = emit(SemanticOperation::OP_MOVE, xlenType(), vlRegister());
    m_vstart = emit(SemanticOperation::OP_MOVE, xlenType(), vstartRegister());
}

/////////////////////////////////////////////////////////////////////////////
// RvvLift: traps, labels and the frame
/////////////////////////////////////////////////////////////////////////////

void RvvLift::trapIllegal()
{
    m_context.trap(SemanticOperation::TRAP_UNDEFINED,
                   RISCV_TRAP_ILLEGAL_INSTRUCTION);
}

void RvvLift::trapIllegalIf(const SemanticOperand& condition)
{
    SemanticOperand skip = newLabel();
    SemanticOperand inverted = emit(SemanticOperation::OP_XOR, integerType(1),
                                    condition, imm(1, integerType(1)));
    branchIf(inverted, skip);
    trapIllegal();
    placeLabel(skip);
}

SemanticOperand RvvLift::newLabel()
{
    return m_context.newLabel();
}

void RvvLift::placeLabel(const SemanticOperand& label)
{
    m_context.placeLabel(label);
}

void RvvLift::branch(const SemanticOperand& label)
{
    m_context.branch(label);
}

void RvvLift::branchIf(const SemanticOperand& condition,
                       const SemanticOperand& label)
{
    m_context.branchIf(condition, label);
}

void RvvLift::begin(bool checkVill)
{
    readState();
    m_endLabel = newLabel();
    m_saturation = SemanticOperand();
    if (checkVill)
    {
        // vill is the top bit of vtype
        SemanticOperand isIllegal = compare(SemanticOperation::COND_SIGNED_LESS,
                                            xlenType(), vtypeRegister(),
                                            immX(0));
        trapIllegalIf(isIllegal);
    }
}

void RvvLift::requireValid()
{
    begin(true);
}

void RvvLift::finish()
{
    move(vstartRegister(), immX(0));
}

void RvvLift::dispatch(RvvBody body)
{
    begin(true);

    SemanticOperand key = emit(SemanticOperation::OP_AND, xlenType(),
                               vtypeRegister(), immX(0x3F));
    // The legal combinations: LMUL 1/8 .. 8 (code 5, 6, 7, 0, 1, 2, 3), and
    // SEW <= ELEN * min(LMUL, 1)
    RvvType types[28];
    SemanticOperand labels[28];
    uint count = 0;
    static const int lmulCodes[7] = { 5, 6, 7, 0, 1, 2, 3 };
    for (uint s = 0; s < 4; s++)
    {
        for (uint l = 0; l < 7; l++)
        {
            const int lmulLog2 = (lmulCodes[l] < 4) ? lmulCodes[l]
                                                    : (lmulCodes[l] - 8);
            const int limit = 3 + ((lmulLog2 < 0) ? lmulLog2 : 0);
            if ((int)s > limit)
                continue;
            RvvType type;
            type.m_sew = 8u << s;
            type.m_lmulLog2 = lmulLog2;
            type.m_key = (s << 3) | (uint)lmulCodes[l];
            types[count] = type;
            labels[count] = newLabel();
            SemanticOperand isThis = compare(SemanticOperation::COND_EQUAL,
                                             xlenType(), key,
                                             immX(type.m_key));
            branchIf(isThis, labels[count]);
            count++;
        }
    }
    // The reserved combinations
    trapIllegal();
    for (uint i = 0; i < count; i++)
    {
        placeLabel(labels[i]);
        body(*this, types[i]);
        branch(m_endLabel);
    }
    placeLabel(m_endLabel);
}

/////////////////////////////////////////////////////////////////////////////
// RvvLift: register groups
/////////////////////////////////////////////////////////////////////////////

bool RvvLift::isGroupLegal(const RvvGroup& group)
{
    if ((group.m_emulLog2 < -3) || (group.m_emulLog2 > 3))
        return false;
    const uint registers = group.registers();
    return ((group.m_register % registers) == 0) &&
           (group.m_register + registers <= 32);
}

bool RvvLift::isOverlapped(const RvvGroup& a, const RvvGroup& b)
{
    const int aStart = (int)a.m_register;
    const int bStart = (int)b.m_register;
    const int aSize = (int)a.registers();
    const int bSize = (int)b.registers();
    const int aEnd = aStart + aSize;
    const int bEnd = bStart + bSize;
    const int last = (aEnd > bEnd) ? aEnd : bEnd;
    const int first = (aStart < bStart) ? aStart : bStart;
    return (last - first) < (aSize + bSize);
}

bool RvvLift::isOverlappedWiden(const RvvGroup& a, const RvvGroup& b)
{
    const int aStart = (int)a.m_register;
    const int bStart = (int)b.m_register;
    const int bSize = (int)b.registers();
    // The source is in the highest numbered part of the destination: the
    // registers after it are outside of the destination
    if ((aStart < bStart) && isOverlapped(a, b))
    {
        RvvGroup after = b;
        after.m_register = (uint)(bStart + bSize);
        if (!isOverlapped(a, after))
            return false;
    }
    return isOverlapped(a, b);
}

RvvGroup RvvLift::makeGroup(const RvvType& type, uint registerNumber, uint eew)
{
    RvvGroup group;
    group.m_register = registerNumber;
    group.m_eew = eew;
    group.m_emulLog2 = (int)log2Of(eew) - (int)log2Of(type.m_sew) +
                       type.m_lmulLog2;
    return group;
}

RvvGroup RvvLift::makeGroupLmul(const RvvType&, uint registerNumber,
                                uint eew, int emulLog2)
{
    RvvGroup group;
    group.m_register = registerNumber;
    group.m_eew = eew;
    group.m_emulLog2 = emulLog2;
    return group;
}

bool RvvLift::isElementGroupLegal(uint egw) const
{
    return egw <= m_vlen;
}

/////////////////////////////////////////////////////////////////////////////
// RvvLift: pieces and elements of groups
/////////////////////////////////////////////////////////////////////////////

SemanticOperand RvvLift::readPiece(const RvvGroup& group, uint chunk,
                                   uint lanes)
{
    const uint pieceBits = lanes * group.m_eew;
    const uint absolute = chunk * pieceBits;
    const uint number = group.m_register + absolute / m_vlen;
    const uint offset = absolute % m_vlen;
    CHECK(number < 32);
    const SemanticType type = vectorType(group.m_eew, lanes);
    SemanticOperand reg = m_context.getRegister(RISCV_REG_V0 + number);
    if (pieceBits == m_vlen)
        return bitcast(emit(SemanticOperation::OP_MOVE, integerType(m_vlen), reg),
                       type);
    SemanticOperand piece = extractBits(reg, immX(offset),
                                        integerType(pieceBits));
    return bitcast(piece, type);
}

void RvvLift::writePiece(const RvvGroup& group, uint chunk, uint lanes,
                         const SemanticOperand& value)
{
    const uint pieceBits = lanes * group.m_eew;
    const uint absolute = chunk * pieceBits;
    const uint number = group.m_register + absolute / m_vlen;
    const uint offset = absolute % m_vlen;
    CHECK(number < 32);
    if (pieceBits == m_vlen)
    {
        writeRegister(number, value);
        return;
    }
    SemanticOperand reg = readRegister(number);
    SemanticOperand piece = bitcast(value, integerType(pieceBits));
    writeRegister(number, insertBits(reg, piece, immX(offset)));
}

void RvvLift::readGroup(const RvvGroup& group, SemanticOperand* registers)
{
    for (uint i = 0; i < group.registers(); i++)
        registers[i] = readRegister(group.m_register + i);
}

void RvvLift::writeGroup(const RvvGroup& group, const SemanticOperand* registers)
{
    for (uint i = 0; i < group.registers(); i++)
        writeRegister(group.m_register + i, registers[i]);
}

SemanticOperand RvvLift::elementRegister(uint eew, const SemanticOperand& index)
{
    // index * eew / VLEN = index >> log2(VLEN / eew)
    return emit(SemanticOperation::OP_SHIFT_RIGHT_LOGICAL, xlenType(), index,
                immX(log2Of(m_vlen / eew)));
}

SemanticOperand RvvLift::elementOffset(uint eew, const SemanticOperand& index)
{
    // (index mod (VLEN / eew)) * eew
    SemanticOperand lane = emit(SemanticOperation::OP_AND, xlenType(), index,
                                immX(m_vlen / eew - 1));
    return emit(SemanticOperation::OP_SHIFT_LEFT, xlenType(), lane,
                immX(log2Of(eew)));
}

SemanticOperand RvvLift::readElement(uint registerNumber, uint eew,
                                     const SemanticOperand& index)
{
    SemanticOperand number = emit(SemanticOperation::OP_ADD, xlenType(),
                                  elementRegister(eew, index),
                                  immX(registerNumber));
    SemanticOperand reg = readRegisterIndexed(number);
    return extractBits(reg, elementOffset(eew, index), integerType(eew));
}

void RvvLift::writeElement(uint registerNumber, uint eew,
                           const SemanticOperand& index,
                           const SemanticOperand& value)
{
    SemanticOperand number = emit(SemanticOperation::OP_ADD, xlenType(),
                                  elementRegister(eew, index),
                                  immX(registerNumber));
    SemanticOperand reg = readRegisterIndexed(number);
    SemanticOperand updated = insertBits(reg, value, elementOffset(eew, index));
    writeRegisterIndexed(number, updated);
}

SemanticOperand RvvLift::maskBit(const SemanticOperand& index)
{
    return extractBits(m_context.getRegister(RISCV_REG_V0), index,
                       integerType(1));
}

SemanticOperand RvvLift::readMaskBit(uint number, const SemanticOperand& index)
{
    return extractBits(m_context.getRegister(RISCV_REG_V0 + number), index,
                       integerType(1));
}

void RvvLift::writeMaskBit(uint number, const SemanticOperand& index,
                           const SemanticOperand& bit)
{
    SemanticOperand reg = readRegister(number);
    writeRegister(number, insertBits(reg, bit, index));
}

/////////////////////////////////////////////////////////////////////////////
// RvvLift: the element loop
/////////////////////////////////////////////////////////////////////////////

SemanticOperand RvvLift::beginLoop(const SemanticOperand& limit)
{
    m_loopLabel = newLabel();
    m_nextLabel = newLabel();
    m_doneLabel = newLabel();
    placeLabel(m_loopLabel);
    m_loopIndex = emit(SemanticOperation::OP_MOVE, xlenType(),
                       vstartRegister());
    SemanticOperand isDone = compare(SemanticOperation::COND_UNSIGNED_GREATER_EQUAL,
                                     xlenType(), m_loopIndex, limit);
    branchIf(isDone, m_doneLabel);
    return m_loopIndex;
}

void RvvLift::skipElement()
{
    branch(m_nextLabel);
}

void RvvLift::skipElementIf(const SemanticOperand& condition)
{
    branchIf(condition, m_nextLabel);
}

void RvvLift::leaveLoop()
{
    branch(m_doneLabel);
}

void RvvLift::endLoop()
{
    placeLabel(m_nextLabel);
    move(vstartRegister(), emit(SemanticOperation::OP_ADD, xlenType(),
                                m_loopIndex, immX(1)));
    branch(m_loopLabel);
    placeLabel(m_doneLabel);
}

/////////////////////////////////////////////////////////////////////////////
// RvvLift: fixed point
/////////////////////////////////////////////////////////////////////////////

SemanticOperand RvvLift::readRoundingMode()
{
    return m_context.getRegister(RISCV_REG_VXRM);
}

SemanticOperand RvvLift::roundingIncrement(RvvChunk& chunk,
                                           const SemanticOperand& value,
                                           const SemanticOperand& shift,
                                           const SemanticOperand& shifted)
{
    typedef SemanticOperation Op;
    const SemanticType type = chunk.m_vectorType;
    const uint lanes = chunk.m_lanes;
    SemanticOperand one = splat(1, type);
    SemanticOperand zero = splat(0, type);

    // The bits shifted out: lost = value & ((1 << shift) - 1); half is the
    // highest of them
    SemanticOperand pow = emit(Op::OP_SHIFT_LEFT, type, one, shift);
    SemanticOperand lowMask = emit(Op::OP_SUBTRACT, type, pow, one);
    SemanticOperand lost = emit(Op::OP_AND, type, value, lowMask);
    SemanticOperand half = emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, pow, one);
    SemanticOperand roundBit = compare(Op::COND_NOT_EQUAL, type,
            emit(Op::OP_AND, type, lost, half), zero);
    SemanticOperand halfBelow = emit(Op::OP_SUBTRACT, type, half, one);
    SemanticOperand sticky = compare(Op::COND_NOT_EQUAL, type,
            emit(Op::OP_AND, type, lost, halfBelow), zero);
    SemanticOperand lsb = compare(Op::COND_NOT_EQUAL, type,
            emit(Op::OP_AND, type, shifted, one), zero);

    const SemanticType mask = maskType(lanes);
    // rnu: the round bit; rne: round bit and (sticky or lsb); rod: not lsb
    // and (round bit or sticky); rdn: nothing
    SemanticOperand rne = emit(Op::OP_AND, mask, roundBit,
                               emit(Op::OP_OR, mask, sticky, lsb));
    SemanticOperand rod = emit(Op::OP_AND_NOT, mask, lsb,
                               emit(Op::OP_OR, mask, roundBit, sticky));

    SemanticOperand mode = readRoundingMode();
    const SemanticType modeType = integerType(2);
    SemanticOperand isRnu = conditionMask(compare(Op::COND_EQUAL, modeType,
                                    mode, imm(0, modeType)), lanes);
    SemanticOperand isRne = conditionMask(compare(Op::COND_EQUAL, modeType,
                                    mode, imm(1, modeType)), lanes);
    SemanticOperand isRod = conditionMask(compare(Op::COND_EQUAL, modeType,
                                    mode, imm(3, modeType)), lanes);
    SemanticOperand increment = emit(Op::OP_OR, mask,
            emit(Op::OP_OR, mask,
                 emit(Op::OP_AND, mask, isRnu, roundBit),
                 emit(Op::OP_AND, mask, isRne, rne)),
            emit(Op::OP_AND, mask, isRod, rod));
    return select(increment, one, zero);
}

void RvvLift::setSaturation(const SemanticOperand& anyBoolean)
{
    if (!m_saturation.isValid())
        m_saturation = anyBoolean;
    else
        m_saturation = emit(SemanticOperation::OP_OR, integerType(1),
                            m_saturation, anyBoolean);
}

/////////////////////////////////////////////////////////////////////////////
// RvvLift: the chunk engine
/////////////////////////////////////////////////////////////////////////////

namespace {

// The shape of the operands of a plan at (SEW, LMUL)
struct Shape {
    RvvGroup m_d;
    RvvGroup m_a;
    RvvGroup m_b;
    // The width of the lanes the callback computes on
    uint m_bits;
    // false when the shape is reserved for the combination
    bool m_isPossible;
};

Shape makeShape(const RvvPlan& plan, const RvvType& type, uint vd, uint vs2,
                uint vs1)
{
    Shape shape;
    shape.m_isPossible = true;
    const uint sew = type.m_sew;
    const int lmul = type.m_lmulLog2;
    shape.m_d = RvvLift::makeGroupLmul(type, vd, sew, lmul);
    shape.m_a = RvvLift::makeGroupLmul(type, vs2, sew, lmul);
    shape.m_b = RvvLift::makeGroupLmul(type, vs1, sew, lmul);
    shape.m_bits = sew;
    switch (plan.m_shape)
    {
    case RvvPlan::SHAPE_SAME:
        break;
    case RvvPlan::SHAPE_WIDEN:
        shape.m_d = RvvLift::makeGroupLmul(type, vd, sew * 2, lmul + 1);
        shape.m_bits = sew * 2;
        break;
    case RvvPlan::SHAPE_WIDEN_W:
        shape.m_d = RvvLift::makeGroupLmul(type, vd, sew * 2, lmul + 1);
        shape.m_a = RvvLift::makeGroupLmul(type, vs2, sew * 2, lmul + 1);
        shape.m_bits = sew * 2;
        break;
    case RvvPlan::SHAPE_NARROW:
        shape.m_a = RvvLift::makeGroupLmul(type, vs2, sew * 2, lmul + 1);
        shape.m_bits = sew * 2;
        break;
    case RvvPlan::SHAPE_EXTEND:
    {
        // The source element is sew / factor
        uint factorLog2 = 0;
        while (((uint)1 << factorLog2) < plan.m_factor)
            factorLog2++;
        shape.m_a = RvvLift::makeGroupLmul(type, vs2, sew / plan.m_factor,
                                           lmul - (int)factorLog2);
        break;
    }
    case RvvPlan::SHAPE_MASK_DEST:
        // The destination is a single mask register
        shape.m_d = RvvLift::makeGroupLmul(type, vd, 1, 0);
        break;
    }
    if ((plan.m_shape == RvvPlan::SHAPE_WIDEN) ||
        (plan.m_shape == RvvPlan::SHAPE_WIDEN_W) ||
        (plan.m_shape == RvvPlan::SHAPE_NARROW))
        shape.m_isPossible = (sew * 2 <= 64) && (lmul + 1 <= 3);
    if (plan.m_shape == RvvPlan::SHAPE_EXTEND)
        shape.m_isPossible = (sew / plan.m_factor >= 8);
    return shape;
}

} // namespace

void RvvLift::elementwise(const RvvPlan& plan, const RvvType& type)
{
    typedef SemanticOperation Op;
    const uint f3 = funct3();
    // The vs1 operand: a vector (OPIVV, OPMVV, OPFVV), a scalar (OPIVX,
    // OPMVX, OPFVF) or an immediate (OPIVI)
    const bool isVectorB = !plan.m_hasNoB && ((f3 == 0) || (f3 == 1) || (f3 == 2));
    const bool isMasked = (plan.m_mask == RvvPlan::MASK_BY_V0) &&
                          (field(25, 1) == 0);

    Shape shape = makeShape(plan, type, vd(), vs2(), vs1());

    // The reserved combinations and register groups
    bool isLegal = shape.m_isPossible &&
                   isGroupLegal(shape.m_d) && isGroupLegal(shape.m_a) &&
                   (!isVectorB || isGroupLegal(shape.m_b));
    // A masked instruction (and vadc, vsbc, vmerge, whose vm bit is 0 too) can't
    // write v0, unless the destination is a mask
    if (isLegal && (plan.m_shape != RvvPlan::SHAPE_MASK_DEST) &&
        (field(25, 1) == 0) && (vd() == 0))
        isLegal = false;
    if (isLegal)
    {
        switch (plan.m_shape)
        {
        case RvvPlan::SHAPE_SAME:
            break;
        case RvvPlan::SHAPE_WIDEN:
            if (type.m_lmulLog2 < 0)
            {
                isLegal = !isOverlapped(shape.m_d, shape.m_a) &&
                          (!isVectorB || !isOverlapped(shape.m_d, shape.m_b));
            } else
            {
                isLegal = !isOverlappedWiden(shape.m_d, shape.m_a) &&
                          (!isVectorB || !isOverlappedWiden(shape.m_d, shape.m_b));
            }
            break;
        case RvvPlan::SHAPE_WIDEN_W:
            if (isVectorB)
            {
                if (type.m_lmulLog2 < 0)
                    isLegal = !isOverlapped(shape.m_d, shape.m_b);
                else
                    isLegal = !isOverlappedWiden(shape.m_d, shape.m_b);
            }
            break;
        case RvvPlan::SHAPE_NARROW:
            if (vd() != vs2())
                isLegal = !isOverlapped(shape.m_d, shape.m_a);
            break;
        case RvvPlan::SHAPE_EXTEND:
            // (the source is never the destination, and overlaps it only at
            // the top of the destination, as the specification says)
            if (vd() == vs2())
                isLegal = false;
            else if (shape.m_a.m_emulLog2 < 0)
                isLegal = !isOverlapped(shape.m_d, shape.m_a);
            else
                isLegal = !isOverlappedWiden(shape.m_d, shape.m_a);
            break;
        case RvvPlan::SHAPE_MASK_DEST:
            if (vd() != vs2())
                isLegal = !isOverlapped(shape.m_d, shape.m_a);
            if (isLegal && isVectorB && (vd() != vs1()))
                isLegal = !isOverlapped(shape.m_d, shape.m_b);
            break;
        }
    }
    if (!isLegal)
    {
        trapIllegal();
        return;
    }

    // The chunks
    const uint bits = shape.m_bits;
    const uint lanes = m_vlen / bits;
    const uint vlmax = type.vlmax(m_vlen);
    uint chunks = vlmax / lanes;
    if (chunks == 0)
        chunks = 1;
    const SemanticType vectorT = vectorType(bits, lanes);
    const SemanticType maskT = maskType(lanes);
    const bool isMaskDest = plan.m_shape == RvvPlan::SHAPE_MASK_DEST;

    SemanticOperand v0Register = isMasked || (plan.m_mask == RvvPlan::MASK_DATA) ?
                                 readRegister(0) : SemanticOperand();

    // The scalar operand
    SemanticOperand scalar;
    SemanticOperand scalarBroadcast;
    if (!plan.m_hasNoB && !isVectorB)
    {
        const uint eewB = shape.m_b.m_eew;
        const SemanticType narrow = integerType(eewB);
        SemanticOperand value;
        if (f3 == 3)
        {
            int64 immediate = plan.m_isImmediateUnsigned ? (int64)uimm5() : simm5();
            // The immediate at the width of the operand
            const uint64 patternMask = (eewB >= 64) ? ~(uint64)0 :
                                       (((uint64)1 << eewB) - 1);
            int64 pattern = (int64)((uint64)immediate & patternMask);
            // Extended to the compute width
            if (eewB < bits)
            {
                if (plan.m_extendB == RvvPlan::EXTEND_SIGN)
                {
                    if (eewB < 64 && ((uint64)pattern >> (eewB - 1)) & 1)
                        pattern = (int64)((uint64)pattern | ~patternMask);
                }
            }
            const uint64 computeMask = (bits >= 64) ? ~(uint64)0 :
                                       (((uint64)1 << bits) - 1);
            pattern = (int64)((uint64)pattern & computeMask);
            scalar = immX(immediate);
            if (plan.m_extendB == RvvPlan::EXTEND_NONE)
                scalarBroadcast = broadcast(imm(pattern & (int64)patternMask, narrow),
                                            vectorType(eewB, lanes));
            else
                scalarBroadcast = splat(pattern, vectorT);
        } else if (plan.m_readScalar != NULL)
        {
            value = plan.m_readScalar(*this, eewB);
            scalar = value;
            if (plan.m_extendB == RvvPlan::EXTEND_NONE)
                scalarBroadcast = broadcast(value, vectorType(eewB, lanes));
            else
            {
                if (eewB < bits)
                    value = emit((plan.m_extendB == RvvPlan::EXTEND_SIGN) ?
                                    Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                                 integerType(bits), value);
                scalarBroadcast = broadcast(value, vectorT);
            }
        } else
        {
            scalar = readX(rs1());
            // sign extended / truncated to the operand width (SEW)
            SemanticOperand x = scalar;
            if (m_xlen > eewB)
                x = truncate(x, eewB);
            else if (m_xlen < eewB)
                x = signExtend(x, eewB);
            if (plan.m_extendB == RvvPlan::EXTEND_NONE)
            {
                scalarBroadcast = broadcast(x, vectorType(eewB, lanes));
            } else
            {
                if (eewB < bits)
                    x = emit((plan.m_extendB == RvvPlan::EXTEND_SIGN) ?
                                Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                             integerType(bits), x);
                scalarBroadcast = broadcast(x, vectorT);
            }
        }
    }

    // Phase 1: the results of all the chunks
    cArray<SemanticOperand> results(chunks);
    cArray<SemanticOperand> bitsResults(chunks);
    for (uint c = 0; c < chunks; c++)
    {
        RvvChunk chunk(*this, type, plan);
        chunk.m_index = c;
        chunk.m_lanes = lanes;
        chunk.m_firstElement = c * lanes;
        chunk.m_bits = bits;
        chunk.m_vectorType = vectorT;
        chunk.m_maskType = maskT;
        chunk.m_scalar = scalar;

        // The active lanes: vstart <= element < vl, and v0
        SemanticOperand first = immX(c * lanes);
        SemanticOperand active = lanesBelow(lanes, first, m_vl);
        SemanticOperand prestart = lanesBelow(lanes, first, m_vstart);
        active = emit(Op::OP_AND_NOT, maskT, prestart, active);
        SemanticOperand v0Bits;
        if (v0Register.isValid())
            v0Bits = bitcast(extractBits(v0Register, immX(c * lanes),
                                         integerType(lanes)), maskT);
        if (isMasked)
            active = emit(Op::OP_AND, maskT, active, v0Bits);
        chunk.m_active = active;
        if (plan.m_mask == RvvPlan::MASK_DATA)
            chunk.m_carry = v0Bits;

        // The operands
        if (shape.m_a.m_eew < bits)
        {
            SemanticOperand narrow = readPiece(shape.m_a, c, lanes);
            chunk.m_a = (plan.m_extendA == RvvPlan::EXTEND_NONE) ? narrow :
                    extendLanes(narrow, bits,
                                plan.m_extendA == RvvPlan::EXTEND_SIGN);
        } else
        {
            chunk.m_a = readPiece(shape.m_a, c, lanes);
        }
        if (plan.m_hasNoB)
        {
            // none
        } else if (isVectorB)
        {
            if (shape.m_b.m_eew < bits)
            {
                SemanticOperand narrow = readPiece(shape.m_b, c, lanes);
                chunk.m_b = (plan.m_extendB == RvvPlan::EXTEND_NONE) ? narrow :
                        extendLanes(narrow, bits,
                                    plan.m_extendB == RvvPlan::EXTEND_SIGN);
            } else
            {
                chunk.m_b = readPiece(shape.m_b, c, lanes);
            }
        } else
        {
            chunk.m_b = scalarBroadcast;
        }
        // The old value of the destination
        SemanticOperand old;
        if (isMaskDest)
        {
            SemanticOperand reg = m_context.getRegister(RISCV_REG_V0 + vd());
            old = bitcast(extractBits(reg, immX(c * lanes), integerType(lanes)),
                          maskT);
        } else
        {
            old = readPiece(shape.m_d, c, lanes);
            if (plan.m_readsDestination)
                chunk.m_old = old;
        }
        if (plan.m_sanitizeA >= 0)
            chunk.m_a = chunk.sanitize(chunk.m_a, plan.m_sanitizeA);
        if ((plan.m_sanitizeB >= 0) && chunk.m_b.isValid())
            chunk.m_b = chunk.sanitize(chunk.m_b, plan.m_sanitizeB);

        SemanticOperand result = plan.m_op(chunk);
        if (isMaskDest)
        {
            // new = (result & active) | (old & ~active)
            SemanticOperand kept = emit(Op::OP_AND_NOT, maskT, active, old);
            SemanticOperand taken = emit(Op::OP_AND, maskT, result, active);
            results[c] = emit(Op::OP_OR, maskT, taken, kept);
        } else
        {
            if (shape.m_d.m_eew < bits)
                result = truncateLanes(result, shape.m_d.m_eew);
            results[c] = select(active, result, old);
        }
        if (plan.m_setsSaturation && chunk.m_saturated.isValid())
        {
            SemanticOperand satActive = emit(Op::OP_AND, maskT,
                                             chunk.m_saturated, active);
            SemanticOperand count = emit(Op::OP_MASK_COUNT, xlenType(),
                                         satActive);
            setSaturation(compare(Op::COND_NOT_EQUAL, xlenType(), count,
                                  immX(0)));
        }
    }

    // Phase 2: the destination
    for (uint c = 0; c < chunks; c++)
    {
        if (isMaskDest)
        {
            SemanticOperand reg = readRegister(vd());
            SemanticOperand piece = bitcast(results[c], integerType(lanes));
            writeRegister(vd(), insertBits(reg, piece, immX(c * lanes)));
        } else
        {
            writePiece(shape.m_d, c, lanes, results[c]);
        }
    }
    if (m_saturation.isValid())
    {
        SemanticOperand vxsat = m_context.getRegister(RISCV_REG_VXSAT);
        move(vxsat, emit(Op::OP_OR, integerType(1), vxsat, m_saturation));
        m_saturation = SemanticOperand();
    }
}

} // namespace riscvdis
