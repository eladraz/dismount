/*
 * Arm64SemanticLiftSimdMemory.cpp
 *
 * Implementation file
 *
 * The A64 lifter of the Advanced SIMD structure loads and stores: ld1-ld4 and
 * st1-st4 (multiple structures, interleaved for 2-4), the single structure
 * forms (one lane of each register), ld1r-ld4r (replicate to all the lanes)
 * and the FEAT_LRCPC3 ldap1 / stl1. See Arm64SemanticSimd.h.
 *
 * The pseudocode of the Arm ARM (LDn / STn multiple structures, single
 * structure): the elements are accessed in order of the memory, all the
 * loads happen before any register is written, and the base register is
 * written back (post index by an immediate or by a register) at the end.
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

// The register number of the 'index'th register of the list
uint listRegister(const ArmDecodedOperand& list, uint index)
{
    return (uint)((list.m_register - ARM64_REG_V0 + index) & 31);
}

// The element size and the total size of an arrangement of a list
void arrangementSizes(uint arrangement, uint& elementBits, uint& totalBits)
{
    switch (arrangement)
    {
    case ArmDecodedOperand::ARRANGEMENT_8B:  elementBits = 8;  totalBits = 64;  break;
    case ArmDecodedOperand::ARRANGEMENT_16B: elementBits = 8;  totalBits = 128; break;
    case ArmDecodedOperand::ARRANGEMENT_4H:  elementBits = 16; totalBits = 64;  break;
    case ArmDecodedOperand::ARRANGEMENT_8H:  elementBits = 16; totalBits = 128; break;
    case ArmDecodedOperand::ARRANGEMENT_2S:  elementBits = 32; totalBits = 64;  break;
    case ArmDecodedOperand::ARRANGEMENT_4S:  elementBits = 32; totalBits = 128; break;
    case ArmDecodedOperand::ARRANGEMENT_1D:  elementBits = 64; totalBits = 64;  break;
    case ArmDecodedOperand::ARRANGEMENT_2D:  elementBits = 64; totalBits = 128; break;
    case ArmDecodedOperand::ARRANGEMENT_B:   elementBits = 8;  totalBits = 8;   break;
    case ArmDecodedOperand::ARRANGEMENT_H:   elementBits = 16; totalBits = 16;  break;
    case ArmDecodedOperand::ARRANGEMENT_S:   elementBits = 32; totalBits = 32;  break;
    default:                                 elementBits = 64; totalBits = 64;  break;
    }
}

// Return the address 'base' + 'offset'
Val addOffset(SimdLift& s, const Val& base, uint offset)
{
    if (offset == 0)
        return base;
    return s.op(Op::OP_ADD, SimdLift::I(64), base, s.imm(offset, SimdLift::I(64)));
}

// The access with an ordering (ldap1 / stl1)
Val loadOrdered(SimdLift& s, const Val& address, const SemanticType& type,
                Op::MemoryOrder order)
{
    Val result = s.c().newTemporary(type);
    SemanticInstruction instruction(Op::OP_LOAD, type, result, address);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    instruction.setMemoryOrder(order);
    s.c().emit(instruction);
    return result;
}

void storeOrdered(SimdLift& s, const Val& address, const Val& value,
                  Op::MemoryOrder order)
{
    SemanticInstruction instruction(Op::OP_STORE, value.getType(),
                                    SemanticOperand(), address, value);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    instruction.setMemoryOrder(order);
    s.c().emit(instruction);
}

} // namespace

bool armdis::liftSimdMemory(SimdLift& s)
{
    const char* name = s.name();
    const size_t length = strlen(name);
    const bool isLoad = (strncmp(name, "ld", 2) == 0);
    const bool isStore = (strncmp(name, "st", 2) == 0);
    if (!isLoad && !isStore)
        return false;
    const bool isOrderedLoad = s.is("ldap1");
    const bool isOrderedStore = s.is("stl1");
    uint structures;
    bool isReplicate = false;
    if (isOrderedLoad || isOrderedStore)
    {
        structures = 1;
    } else
    {
        if ((name[2] < '1') || (name[2] > '4'))
            return false;
        structures = (uint)(name[2] - '0');
        isReplicate = (length == 4) && (name[3] == 'r');
        if (!((length == 3) || isReplicate))
            return false;
    }
    if (s.count() != 2)
        return false;

    ArmLiftContext& c = s.c();
    const ArmDecodedOperand& list = s.operand(0);
    if (list.m_kind != ArmDecodedOperand::KIND_REGISTER_LIST)
        return false;
    uint elementBits, totalBits;
    arrangementSizes(list.m_arrangement, elementBits, totalBits);
    const bool isSingle = list.m_elementIndex >= 0;
    const uint elementBytes = elementBits / 8;
    // The lanes of the access: the arrangement, or the element of a single
    // structure
    const uint lanes = isSingle ? 1 : (totalBits / elementBits);
    const SemanticType elementType = SimdLift::I(elementBits);
    const Val address = c.getAddress(1);
    const Op::MemoryOrder loadOrder = isOrderedLoad ?
                    Op::ORDER_ACQUIRE_PROCESSOR_CONSISTENT : Op::ORDER_RELAXED;
    const Op::MemoryOrder storeOrder = isOrderedStore ? Op::ORDER_RELEASE :
                                                         Op::ORDER_RELAXED;
    const uint registers = list.m_count;

    if (isSingle || isReplicate)
    {
        // One element of each of the registers, consecutive in memory
        const uint total = isReplicate ? totalBits : 0;
        if (isLoad)
        {
            cArray<Val> values(structures);
            for (uint r = 0; r < structures; r++)
                values[r] = loadOrdered(s, addOffset(s, address, r * elementBytes),
                                        elementType, loadOrder);
            for (uint r = 0; r < structures; r++)
            {
                const uint number = listRegister(list, r);
                if (isReplicate)
                {
                    if (total == elementBits)
                        s.writeV(number, values[r]);
                    else
                        s.writeV(number, s.splat(SimdLift::V(elementBits,
                                    total / elementBits), values[r]));
                } else
                {
                    Val old = s.cast(SimdLift::V(elementBits, 128 / elementBits),
                                     s.readV(number));
                    s.writeV(number, s.setLane(old, values[r],
                                               (uint)list.m_elementIndex));
                }
            }
        } else
        {
            for (uint r = 0; r < structures; r++)
            {
                Val v = s.cast(SimdLift::V(elementBits, 128 / elementBits),
                               s.readV(listRegister(list, r)));
                storeOrdered(s, addOffset(s, address, r * elementBytes),
                             s.lane(v, (uint)list.m_elementIndex), storeOrder);
            }
        }
        c.writeBack(1);
        return true;
    }

    // Multiple structures. ld1 / st1 with 1-4 registers are sequential,
    // ld2-ld4 / st2-st4 interleave the elements of the registers
    const bool isInterleaved = structures > 1;
    const uint vectorLanes = lanes;
    if (isLoad)
    {
        cArray<Val> loaded(registers * vectorLanes);
        for (uint r = 0; r < registers; r++)
        {
            for (uint e = 0; e < vectorLanes; e++)
            {
                const uint index = isInterleaved ? (e * structures + r)
                                                 : (r * vectorLanes + e);
                loaded[r * vectorLanes + e] = c.load(
                        addOffset(s, address, index * elementBytes), elementType);
            }
        }
        for (uint r = 0; r < registers; r++)
        {
            Val v;
            if (vectorLanes == 1)
                v = loaded[r];
            else
            {
                v = s.splat(SimdLift::V(elementBits, vectorLanes), 0);
                for (uint e = 0; e < vectorLanes; e++)
                    v = s.setLane(v, loaded[r * vectorLanes + e], e);
            }
            s.writeV(listRegister(list, r), v);
        }
    } else
    {
        for (uint r = 0; r < registers; r++)
        {
            Val v = s.readV(listRegister(list, r));
            v = s.cast(SimdLift::V(elementBits, 128 / elementBits), v);
            for (uint e = 0; e < vectorLanes; e++)
            {
                const uint index = isInterleaved ? (e * structures + r)
                                                 : (r * vectorLanes + e);
                c.store(addOffset(s, address, index * elementBytes), s.lane(v, e));
            }
        }
    }
    c.writeBack(1);
    return true;
}
