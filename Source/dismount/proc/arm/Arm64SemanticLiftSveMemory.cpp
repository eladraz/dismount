/*
 * Arm64SemanticLiftSveMemory.cpp
 *
 * Implementation file
 *
 * The A64 SVE lifters of the loads and stores: the contiguous, structure,
 * replicating, gather and scatter forms, the non temporal ones, the first
 * fault and non fault loads (with the first fault register), ldr / str of
 * the registers and the prefetches. The inactive elements are not accessed
 * (OP_LOAD_MASKED, OP_STORE_MASKED); a gather or a scatter accesses its
 * elements one at a time, with the predicate bit of the element as the mask.
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

// The largest number of lanes (512 bit vectors of bytes)
enum { MAX_LANES = 64 };

const ArmDecodedOperand& operandOf(SveLift& s, uint index)
{
    return s.context().getOperand(index);
}

bool isVectorRegister(uint registerId)
{
    return (registerId >= ARM64_REG_Z0) && (registerId <= ARM64_REG_Z31);
}

// What the mnemonic says about the access
struct Access {
    // The size of the memory element in bits
    uint m_memoryBits;
    // The extension of the loaded element is signed
    bool m_isSigned;
    // The number of vectors of a structure access (ld2, ld3, ld4)
    uint m_structure;
};

Access parseAccess(const char* name)
{
    Access access;
    access.m_isSigned = false;
    access.m_structure = 1;
    // ld1b, ld1sb, ldff1b, ldnf1sw, ldnt1b, ld1rb, ld1rsb, ld1rqb, ld1rob, ld2w,
    // st1b, stnt1b, st3d...
    const char* p = name;
    while ((*p != 0) && ((*p < '0') || (*p > '9')))
        p++;
    if ((*p >= '2') && (*p <= '4'))
        access.m_structure = *p - '0';
    p++;
    // The optional r (replicate), q / o (quadword / octaword replicate), s
    // (signed), then the size letter
    if ((*p == 'r') && (p[1] != 0))
    {
        p++;
        if ((*p == 'q') || (*p == 'o'))
            p++;
    }
    if ((*p == 's') && (p[1] != 0))
    {
        access.m_isSigned = true;
        p++;
    }
    switch (*p)
    {
    case 'b': access.m_memoryBits = 8; break;
    case 'h': access.m_memoryBits = 16; break;
    case 'w': access.m_memoryBits = 32; break;
    case 'd': access.m_memoryBits = 64; break;
    default: access.m_memoryBits = 128; break;
    }
    return access;
}

// The first register number and the count of the list operand 'index'
void listOf(SveLift& s, uint index, uint& first, uint& count, uint& stride,
            uint& elementBits)
{
    const ArmDecodedOperand& list = operandOf(s, index);
    first = SveLift::registerNumber(list);
    count = (list.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST) ?
                list.m_count : 1;
    stride = (list.m_stride == 0) ? 1 : list.m_stride;
    elementBits = SveLift::elementBitsOf(list);
}

// The scalar address of a [xn, ...] operand: the base plus the register
// offset (shifted) or the immediate (in bytes, or in units of 'vectorUnit'
// bytes for "mul vl")
SemanticOperand scalarAddress(SveLift& s, const ArmDecodedOperand& memory,
                              int64 vectorUnit)
{
    SemanticOperand address = s.context().getRegister(memory.m_base);
    if (memory.m_index != ARM_NO_REGISTER)
    {
        SemanticOperand offset = s.context().getRegister(memory.m_index);
        if (s.context().getArchitecture()->getRegister(memory.m_index).m_isZero)
            offset = s.integer(0, 64);
        if ((memory.m_shift == ArmDecodedOperand::SHIFT_LSL) && (memory.m_shiftAmount != 0))
            offset = s.emit(Op::OP_SHIFT_LEFT, SemanticType::integer(64), offset,
                            s.integer(memory.m_shiftAmount, 64));
        address = s.emit(Op::OP_ADD, SemanticType::integer(64), address, offset);
    } else if (memory.m_offset != 0)
    {
        int64 bytes = memory.m_offset;
        if (memory.m_shift == ArmDecodedOperand::SHIFT_MUL_VL)
            bytes *= vectorUnit;
        address = s.emit(Op::OP_ADD, SemanticType::integer(64), address,
                         s.integer(bytes, 64));
    }
    return address;
}

// The bit of the lane 'lane' of the integer 'bits' of 'count' bits, as 1 bit
SemanticOperand laneBit(SveLift& s, const SemanticOperand& bits, uint count,
                        uint lane)
{
    const SemanticType type = SemanticType::integer(count);
    if (count == 1)
        return bits;
    return s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                  s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, bits, s.integer(lane, count)));
}

// A masked load of one element: zero when the bit is clear, no fault
SemanticOperand loadElement(SveLift& s, const SemanticOperand& address,
                            const SemanticOperand& active, uint bits)
{
    const SemanticType type = SemanticType::integer(bits);
    SemanticOperand result = s.context().newTemporary(type);
    SemanticInstruction instruction(Op::OP_LOAD_MASKED, type, result, address, active);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    s.context().emit(instruction);
    return result;
}

void storeElement(SveLift& s, const SemanticOperand& address,
                  const SemanticOperand& value, const SemanticOperand& active)
{
    SemanticInstruction instruction(Op::OP_STORE_MASKED, SemanticType(),
                                    SemanticOperand(), address, value, active);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    s.context().emit(instruction);
}

// Extend the loaded memory element to the vector element
SemanticOperand extendElement(SveLift& s, const SemanticOperand& value,
                              uint memoryBits, uint elementBits, bool isSigned)
{
    if (memoryBits >= elementBits)
        return value;
    return s.emit(isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                  SemanticType::integer(elementBits), value);
}

/////////////////////////////////////////////////////////////////////////////
// The addresses of the elements
/////////////////////////////////////////////////////////////////////////////

// The address of every lane: a contiguous access (base + lane * stride), or
// the gather forms [xn, zm.t{, uxtw|sxtw|lsl #n}], [zn.t{, #imm}], [zn.t, xm]
struct Addresses {
    SemanticOperand m_lane[MAX_LANES];
    bool m_isGather;
};

void buildAddresses(SveLift& s, const ArmDecodedOperand& memory, uint count,
                    uint elementBits, uint stride, int64 vectorUnit,
                    Addresses& addresses)
{
    const SemanticType i64 = SemanticType::integer(64);
    addresses.m_isGather = isVectorRegister(memory.m_base) ||
        ((memory.m_index != ARM_NO_REGISTER) && isVectorRegister(memory.m_index));
    if (!addresses.m_isGather)
    {
        SemanticOperand base = scalarAddress(s, memory, vectorUnit);
        for (uint lane = 0; lane < count; lane++)
            addresses.m_lane[lane] = (lane == 0) ? base :
                s.emit(Op::OP_ADD, i64, base, s.integer((int64)lane * stride, 64));
        return;
    }
    if (isVectorRegister(memory.m_base))
    {
        // [zn.t, #imm] and [zn.t, xm]
        const uint bits = (memory.m_indexArrangement == ArmDecodedOperand::ARRANGEMENT_S) ? 32 : 64;
        SemanticOperand vector = s.readZ(memory.m_base - ARM64_REG_Z0, bits);
        SemanticOperand offset;
        if (memory.m_index != ARM_NO_REGISTER)
            offset = s.context().getRegister(memory.m_index);
        else
            offset = s.integer(memory.m_offset, 64);
        // The 128 bit elements of ld1q / st1q use the even 64 bit bases
        const uint step = (elementBits == 128) ? 2 : 1;
        for (uint lane = 0; lane < count; lane++)
        {
            SemanticOperand value = s.extractLane(vector, lane * step);
            if (bits < 64)
                value = s.emit(Op::OP_ZERO_EXTEND, i64, value);
            addresses.m_lane[lane] = s.emit(Op::OP_ADD, i64, value, offset);
        }
        return;
    }
    // [xn, zm.t{, extend #n}]
    const uint bits = (memory.m_indexArrangement == ArmDecodedOperand::ARRANGEMENT_S) ? 32 : 64;
    SemanticOperand vector = s.readZ(memory.m_index - ARM64_REG_Z0, bits);
    SemanticOperand base = s.context().getRegister(memory.m_base);
    for (uint lane = 0; lane < count; lane++)
    {
        SemanticOperand value = s.extractLane(vector, lane);
        if (memory.m_shift == ArmDecodedOperand::EXTEND_UXTW ||
            memory.m_shift == ArmDecodedOperand::EXTEND_SXTW)
        {
            if (bits == 64)
                value = s.emit(Op::OP_TRUNCATE, SemanticType::integer(32), value);
            value = s.emit(memory.m_shift == ArmDecodedOperand::EXTEND_SXTW ?
                           Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND, i64, value);
        } else if (bits < 64)
        {
            value = s.emit(Op::OP_ZERO_EXTEND, i64, value);
        }
        if (memory.m_shiftAmount != 0)
            value = s.emit(Op::OP_SHIFT_LEFT, i64, value, s.integer(memory.m_shiftAmount, 64));
        addresses.m_lane[lane] = s.emit(Op::OP_ADD, i64, base, value);
    }
}

/////////////////////////////////////////////////////////////////////////////
// Loads
/////////////////////////////////////////////////////////////////////////////

// The first fault and non fault loads: the elements which would fault are
// not loaded and the first fault register keeps the elements before them.
// 'values' gets the loaded elements (zero for the others)
void loadFaultTolerant(SveLift& s, const Addresses& addresses, uint count,
                       uint elementBits, uint memoryBits, bool isSigned,
                       bool isNonFault, SemanticOperand* values)
{
    const SemanticType flag = SemanticType::integer(1);
    SemanticOperand mask = s.bitcast(s.readLaneMask(
        SveLift::registerNumber(operandOf(s, 1)), elementBits), SemanticType::integer(count));
    SemanticOperand seenActive = s.integer(0, 1);
    SemanticOperand faulted = s.integer(0, 1);
    SemanticOperand faultedBits = s.integer(0, count);
    for (uint lane = 0; lane < count; lane++)
    {
        SemanticOperand active = laneBit(s, mask, count, lane);
        SemanticOperand isFirst = s.emit(Op::OP_AND_NOT, flag, seenActive, active);
        seenActive = s.emit(Op::OP_OR, flag, seenActive, active);
        SemanticOperand probe = s.context().newTemporary(flag);
        SemanticInstruction instruction(Op::OP_PROBE_ACCESS,
                                        SemanticType::integer(memoryBits),
                                        probe, addresses.m_lane[lane]);
        s.context().emit(instruction);
        SemanticOperand failed = s.emit(Op::OP_AND_NOT, flag, probe, active);
        if (!isNonFault)
            failed = s.emit(Op::OP_AND_NOT, flag, isFirst, failed);
        faulted = s.emit(Op::OP_OR, flag, faulted, failed);
        SemanticOperand load = s.emit(Op::OP_AND_NOT, flag, faulted, active);
        values[lane] = loadElement(s, addresses.m_lane[lane], load, memoryBits);
        SemanticOperand placed = s.emit(Op::OP_SHIFT_LEFT, SemanticType::integer(count),
            s.emit(Op::OP_ZERO_EXTEND, SemanticType::integer(count), faulted),
            s.integer(lane, count));
        faultedBits = s.emit(Op::OP_OR, SemanticType::integer(count), faultedBits, placed);
    }
    // The first fault register loses the elements from the first fault on
    SemanticOperand cleared = s.maskToPredicate(s.bitcast(faultedBits, SemanticType::mask(count)),
                                                elementBits);
    if (elementBits > 8)
    {
        // The whole predicate element (the bits of its bytes), as ElemP writes it
        cleared = s.truncate(s.emit(Op::OP_MULTIPLY, SemanticType::integer(64), s.zeroExtend(cleared, 64),
                                    s.integer((1 << (elementBits / 8)) - 1, 64)), s.predicateBits());
    }
    s.writeFfr(s.emit(Op::OP_AND_NOT, s.predicateType(), cleared, s.readFfr()));
}

// The multi-vector contiguous forms with a predicate as counter (SVE2.1,
// SME2): ld1 / ldnt1 { z0.t - z3.t }, pn8/z, [xn, ...] and the stores. The
// vectors are consecutive in memory; the mask of the vector r is the part
// of the counter for the elements r * lanes ...
void liftMultiVector(SveLift& s, const SveDescriptor& d, bool isStore)
{
    const Access access = parseAccess(d.m_name);
    uint first, listCount, stride, e;
    listOf(s, 0, first, listCount, stride, e);
    const ArmDecodedOperand& memory = operandOf(s, 2);
    const uint count = s.lanes(e);
    const int64 vectorUnit = (int64)s.vectorBits() / 8;
    SemanticOperand base = scalarAddress(s, memory, vectorUnit);
    const uint counterRegister = SveLift::registerNumber(operandOf(s, 1));
    for (uint r = 0; r < listCount; r++)
    {
        SemanticOperand mask = s.laneMaskOfBits(s.counterPredicate(counterRegister, r), e);
        SemanticOperand address = (r == 0) ? base :
            s.emit(Op::OP_ADD, SemanticType::integer(64), base, s.integer((int64)r * vectorUnit, 64));
        const uint reg = (first + r * stride) & 31;
        if (isStore)
        {
            SemanticInstruction instruction(Op::OP_STORE_MASKED, SemanticType(), SemanticOperand(),
                                            address, s.readZ(reg, e), mask);
            instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
            s.context().emit(instruction);
        } else
        {
            const SemanticType memoryType = SemanticType::vector(SemanticType::integer(access.m_memoryBits), count);
            SemanticOperand loaded = s.context().newTemporary(memoryType);
            SemanticInstruction instruction(Op::OP_LOAD_MASKED, memoryType, loaded, address, mask);
            instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
            s.context().emit(instruction);
            s.writeZ(reg, loaded);
        }
    }
}

// ld1, ldff1, ldnf1, ldnt1 (the contiguous, gather and structure forms),
// m_a: 1 first fault, 2 non fault
void liftLoad(SveLift& s, const SveDescriptor& d)
{
    if (operandOf(s, 1).m_isPredicateCounter)
    {
        liftMultiVector(s, d, false);
        return;
    }
    Access access = parseAccess(d.m_name);
    uint first, listCount, stride, e;
    listOf(s, 0, first, listCount, stride, e);
    const ArmDecodedOperand& memory = operandOf(s, 2);
    // ld1q, and ld1w / ld1d { z.q } (SVE2.1): quadword elements
    if (e == 128)
        access.m_memoryBits = 128;
    const uint count = s.lanes(e);
    const uint mbytes = access.m_memoryBits / 8;
    // The unit of "mul vl": the bytes the instruction loads for one vector
    const int64 vectorUnit = (int64)count * mbytes;
    Addresses addresses;
    buildAddresses(s, memory, count, e, mbytes * access.m_structure, vectorUnit,
                   addresses);
    SemanticOperand mask = s.readMaskOperand(1, e);
    const bool isFaultTolerant = d.m_a != 0;

    if (isFaultTolerant)
    {
        SemanticOperand values[MAX_LANES];
        loadFaultTolerant(s, addresses, count, e, access.m_memoryBits, access.m_isSigned,
                          d.m_a == 2, values);
        SemanticOperand vector = s.zero(s.integerType(e));
        for (uint lane = 0; lane < count; lane++)
            vector = s.insertLane(vector, extendElement(s, values[lane], access.m_memoryBits,
                                  e, access.m_isSigned), lane);
        s.writeZ(first, vector);
        return;
    }
    if (!addresses.m_isGather && (access.m_structure == 1))
    {
        // One masked load of the lanes, extended
        const SemanticType memoryType = SemanticType::vector(
            SemanticType::integer(access.m_memoryBits), count);
        SemanticOperand loaded = s.context().newTemporary(memoryType);
        SemanticInstruction instruction(Op::OP_LOAD_MASKED, memoryType, loaded,
                                        addresses.m_lane[0], mask);
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        s.context().emit(instruction);
        if (access.m_memoryBits < e)
            loaded = s.emit(access.m_isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                            s.integerType(e), loaded);
        s.writeZ(first, loaded);
        return;
    }
    // The elements one at a time (a gather, or the members of a structure)
    SemanticOperand maskBits = s.bitcast(mask, SemanticType::integer(count));
    SemanticOperand vectors[4];
    for (uint member = 0; member < access.m_structure; member++)
        vectors[member] = s.zero(s.integerType(e));
    for (uint lane = 0; lane < count; lane++)
    {
        SemanticOperand active = laneBit(s, maskBits, count, lane);
        for (uint member = 0; member < access.m_structure; member++)
        {
            SemanticOperand address = addresses.m_lane[lane];
            if (member != 0)
                address = s.emit(Op::OP_ADD, SemanticType::integer(64), address,
                                 s.integer((int64)member * mbytes, 64));
            SemanticOperand value = extendElement(s,
                loadElement(s, address, active, access.m_memoryBits),
                access.m_memoryBits, e, access.m_isSigned);
            vectors[member] = s.insertLane(vectors[member], value, lane);
        }
    }
    for (uint member = 0; member < access.m_structure; member++)
        s.writeZ((first + member * stride) & 31, vectors[member]);
}

// ld1r, ld1rs: one element replicated to the active lanes
void liftLoadReplicate(SveLift& s, const SveDescriptor& d)
{
    const Access access = parseAccess(d.m_name);
    uint first, listCount, stride, e;
    listOf(s, 0, first, listCount, stride, e);
    const SemanticType i64 = SemanticType::integer(64);
    SemanticOperand address = scalarAddress(s, operandOf(s, 2), 0);
    SemanticOperand mask = s.readMaskOperand(1, e);
    const uint count = s.lanes(e);
    SemanticOperand any = s.compareScalar(Op::COND_NOT_EQUAL,
        s.bitcast(mask, SemanticType::integer(count)), s.integer(0, count));
    SemanticOperand value = extendElement(s,
        loadElement(s, address, any, access.m_memoryBits),
        access.m_memoryBits, e, access.m_isSigned);
    (void)i64;
    SemanticOperand replicated = s.splat(value, s.integerType(e));
    s.writeZ(first, s.select(mask, replicated, s.zero(s.integerType(e))));
}

// ld1rq (a quadword), ld1ro (an octaword) replicated to every segment
void liftLoadReplicateBlock(SveLift& s, const SveDescriptor& d)
{
    uint first, listCount, stride, e;
    listOf(s, 0, first, listCount, stride, e);
    const uint blockBits = (d.m_a == 0) ? 128 : 256;
    if (s.vectorBits() < blockBits)
    {
        s.context().trap(Op::TRAP_UNDEFINED, ARM_TRAP_UNDEFINED);
        return;
    }
    const uint count = blockBits / e;
    SemanticOperand address = scalarAddress(s, operandOf(s, 2), 0);
    // The predicate of the first block
    SemanticOperand mask = s.readMaskOperand(1, e);
    SemanticOperand maskBits = s.truncate(s.bitcast(mask, SemanticType::integer(s.lanes(e))), count);
    SemanticOperand loaded;
    {
        const SemanticType memoryType = SemanticType::vector(SemanticType::integer(e), count);
        loaded = s.context().newTemporary(memoryType);
        SemanticInstruction instruction(Op::OP_LOAD_MASKED, memoryType, loaded, address,
                                        s.bitcast(maskBits, SemanticType::mask(count)));
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        s.context().emit(instruction);
    }
    SemanticOperand block = s.bitcast(loaded, SemanticType::integer(blockBits));
    s.writeZ(first, s.splat(block, SemanticType::vector(SemanticType::integer(blockBits),
                                                        s.vectorBits() / blockBits)));
}

// ldr zt, [xn, #imm, mul vl]; ldr pt, [xn, #imm, mul vl]
void liftLoadRegister(SveLift& s, const SveDescriptor& d)
{
    const ArmDecodedOperand& destination = operandOf(s, 0);
    const bool isPredicate = SveLift::isP(destination);
    const uint bytes = (isPredicate ? s.predicateBits() : s.vectorBits()) / 8;
    SemanticOperand address = scalarAddress(s, operandOf(s, 1), bytes);
    SemanticOperand value = s.context().load(address, SemanticType::integer(bytes * 8));
    if (isPredicate)
        s.writePredicateBits(SveLift::registerNumber(destination), value);
    else
        s.writeZ(SveLift::registerNumber(destination), value);
}

/////////////////////////////////////////////////////////////////////////////
// Stores
/////////////////////////////////////////////////////////////////////////////

// st1, stnt1, st2, st3, st4: the contiguous, scatter and structure forms
void liftStore(SveLift& s, const SveDescriptor& d)
{
    if (operandOf(s, 1).m_isPredicateCounter)
    {
        liftMultiVector(s, d, true);
        return;
    }
    Access access = parseAccess(d.m_name);
    uint first, listCount, stride, e;
    listOf(s, 0, first, listCount, stride, e);
    const ArmDecodedOperand& memory = operandOf(s, 2);
    if (e == 128)
        access.m_memoryBits = 128;
    const uint count = s.lanes(e);
    const uint mbytes = access.m_memoryBits / 8;
    const int64 vectorUnit = (int64)count * mbytes;
    Addresses addresses;
    buildAddresses(s, memory, count, e, mbytes * access.m_structure, vectorUnit, addresses);
    SemanticOperand mask = s.readMaskOperand(1, e);
    if (!addresses.m_isGather && (access.m_structure == 1))
    {
        SemanticOperand value = s.readZ(first, e);
        SemanticOperand storedMask = mask;
        if (access.m_memoryBits < e)
        {
            // The low bits of every element: narrow in steps of two (the
            // first lanes hold the values, the upper lanes are zero and not
            // stored)
            const uint narrowLanes = s.vectorBits() / access.m_memoryBits;
            SemanticOperand current = value;
            uint bits = e;
            while (bits > access.m_memoryBits)
            {
                const uint next = bits / 2;
                current = s.emit(Op::OP_NARROW,
                                 SemanticType::vector(SemanticType::integer(next), s.vectorBits() / next),
                                 current, s.zero(current.getType()));
                bits = next;
            }
            value = current;
            storedMask = s.bitcast(s.zeroExtend(s.bitcast(mask, SemanticType::integer(count)), narrowLanes),
                                   SemanticType::mask(narrowLanes));
        }
        SemanticInstruction instruction(Op::OP_STORE_MASKED, SemanticType(), SemanticOperand(),
                                        addresses.m_lane[0], value, storedMask);
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        s.context().emit(instruction);
        return;
    }
    SemanticOperand maskBits = s.bitcast(mask, SemanticType::integer(count));
    SemanticOperand vectors[4];
    for (uint member = 0; member < access.m_structure; member++)
        vectors[member] = s.readZ((first + member * stride) & 31, e);
    for (uint lane = 0; lane < count; lane++)
    {
        SemanticOperand active = laneBit(s, maskBits, count, lane);
        for (uint member = 0; member < access.m_structure; member++)
        {
            SemanticOperand address = addresses.m_lane[lane];
            if (member != 0)
                address = s.emit(Op::OP_ADD, SemanticType::integer(64), address,
                                 s.integer((int64)member * mbytes, 64));
            SemanticOperand value = s.extractLane(vectors[member], lane);
            if (access.m_memoryBits < e)
                value = s.emit(Op::OP_TRUNCATE, SemanticType::integer(access.m_memoryBits), value);
            storeElement(s, address, value, active);
        }
    }
}

// str zt, [xn, #imm, mul vl]; str pt, [xn, #imm, mul vl]
void liftStoreRegister(SveLift& s, const SveDescriptor& d)
{
    const ArmDecodedOperand& source = operandOf(s, 0);
    const bool isPredicate = SveLift::isP(source);
    const uint bytes = (isPredicate ? s.predicateBits() : s.vectorBits()) / 8;
    SemanticOperand address = scalarAddress(s, operandOf(s, 1), bytes);
    SemanticOperand value = isPredicate ?
        s.readPredicateBits(SveLift::registerNumber(source)) :
        s.readZWhole(SveLift::registerNumber(source));
    s.context().store(address, value);
}

// prfb, prfh, prfw, prfd: hints, no effect
void liftPrefetch(SveLift& s, const SveDescriptor& d)
{
    s.context().emit(SemanticInstruction(Op::OP_NOP, SemanticType(), SemanticOperand()));
}

/////////////////////////////////////////////////////////////////////////////
// The tables
/////////////////////////////////////////////////////////////////////////////

const SveDescriptor gMemoryTable[] =
{
    { "ld1b", liftLoad, 0, 0 }, { "ld1h", liftLoad, 0, 0 }, { "ld1w", liftLoad, 0, 0 },
    { "ld1d", liftLoad, 0, 0 }, { "ld1sb", liftLoad, 0, 0 }, { "ld1sh", liftLoad, 0, 0 },
    { "ld1sw", liftLoad, 0, 0 }, { "ld1q", liftLoad, 0, 0 },
    { "ld2b", liftLoad, 0, 0 }, { "ld2h", liftLoad, 0, 0 }, { "ld2w", liftLoad, 0, 0 },
    { "ld2d", liftLoad, 0, 0 }, { "ld3b", liftLoad, 0, 0 }, { "ld3h", liftLoad, 0, 0 },
    { "ld3w", liftLoad, 0, 0 }, { "ld3d", liftLoad, 0, 0 }, { "ld4b", liftLoad, 0, 0 },
    { "ld4h", liftLoad, 0, 0 }, { "ld4w", liftLoad, 0, 0 }, { "ld4d", liftLoad, 0, 0 },
    { "ldff1b", liftLoad, 1, 0 }, { "ldff1h", liftLoad, 1, 0 }, { "ldff1w", liftLoad, 1, 0 },
    { "ldff1d", liftLoad, 1, 0 }, { "ldff1sb", liftLoad, 1, 0 }, { "ldff1sh", liftLoad, 1, 0 },
    { "ldff1sw", liftLoad, 1, 0 },
    { "ldnf1b", liftLoad, 2, 0 }, { "ldnf1h", liftLoad, 2, 0 }, { "ldnf1w", liftLoad, 2, 0 },
    { "ldnf1d", liftLoad, 2, 0 }, { "ldnf1sb", liftLoad, 2, 0 }, { "ldnf1sh", liftLoad, 2, 0 },
    { "ldnf1sw", liftLoad, 2, 0 },
    { "ldnt1b", liftLoad, 0, 0 }, { "ldnt1h", liftLoad, 0, 0 }, { "ldnt1w", liftLoad, 0, 0 },
    { "ldnt1d", liftLoad, 0, 0 }, { "ldnt1sb", liftLoad, 0, 0 }, { "ldnt1sh", liftLoad, 0, 0 },
    { "ldnt1sw", liftLoad, 0, 0 },
    { "ld1rb", liftLoadReplicate, 0, 0 }, { "ld1rh", liftLoadReplicate, 0, 0 },
    { "ld1rw", liftLoadReplicate, 0, 0 }, { "ld1rd", liftLoadReplicate, 0, 0 },
    { "ld1rsb", liftLoadReplicate, 0, 0 }, { "ld1rsh", liftLoadReplicate, 0, 0 },
    { "ld1rsw", liftLoadReplicate, 0, 0 },
    { "ld1rqb", liftLoadReplicateBlock, 0, 0 }, { "ld1rqh", liftLoadReplicateBlock, 0, 0 },
    { "ld1rqw", liftLoadReplicateBlock, 0, 0 }, { "ld1rqd", liftLoadReplicateBlock, 0, 0 },
    { "ld1rob", liftLoadReplicateBlock, 1, 0 }, { "ld1roh", liftLoadReplicateBlock, 1, 0 },
    { "ld1row", liftLoadReplicateBlock, 1, 0 }, { "ld1rod", liftLoadReplicateBlock, 1, 0 },
    { "ldr", liftLoadRegister, 0, 0 },
    { "st1b", liftStore, 0, 0 }, { "st1h", liftStore, 0, 0 }, { "st1w", liftStore, 0, 0 },
    { "st1d", liftStore, 0, 0 }, { "st1q", liftStore, 0, 0 },
    { "st2b", liftStore, 0, 0 }, { "st2h", liftStore, 0, 0 }, { "st2w", liftStore, 0, 0 },
    { "st2d", liftStore, 0, 0 }, { "st3b", liftStore, 0, 0 }, { "st3h", liftStore, 0, 0 },
    { "st3w", liftStore, 0, 0 }, { "st3d", liftStore, 0, 0 }, { "st4b", liftStore, 0, 0 },
    { "st4h", liftStore, 0, 0 }, { "st4w", liftStore, 0, 0 }, { "st4d", liftStore, 0, 0 },
    { "stnt1b", liftStore, 0, 0 }, { "stnt1h", liftStore, 0, 0 }, { "stnt1w", liftStore, 0, 0 },
    { "stnt1d", liftStore, 0, 0 },
    { "str", liftStoreRegister, 0, 0 },
    { "prfb", liftPrefetch, 0, 0 }, { "prfh", liftPrefetch, 0, 0 },
    { "prfw", liftPrefetch, 0, 0 }, { "prfd", liftPrefetch, 0, 0 },
    { NULL, NULL, 0, 0 }
};

} // namespace

void armdis::a64::liftSveMemory(ArmLiftContext& context)
{
    runSveTable(context, gMemoryTable);
}
