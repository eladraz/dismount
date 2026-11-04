/*
 * ArmSemanticLiftNeonMemory.cpp
 *
 * Implementation file
 *
 * The lifters of the Advanced SIMD structure loads and stores of AArch32:
 * vld1 - vld4 and vst1 - vst4, the multiple structures, the single element
 * to or from one lane, and (loads) the single element to all lanes, with the
 * writeback of the base by the transfer size or by a register. The elements
 * are accessed one by one, in the order of the pseudocode of the Arm ARM.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include <string.h>
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/ArmSemanticInternal.h"
#include "dismount/proc/arm/ArmSemanticNeon.h"

using namespace armdis;

typedef SemanticOperation Op;
typedef SemanticOperand Val;

namespace armdis {
namespace a32 {

void liftNeonStructure(ArmLiftContext& c)
{
    NeonBuilder b(c);
    NeonName n(c);
    const bool isLoad = n.m_base[1] == 'l';
    const uint structure = (uint)(n.m_base[3] - '0');
    const uint e = n.m_bits;
    const uint bytes = e / 8;
    const ArmDecodedOperand& list = c.getOperand(0);
    const ArmDecodedOperand& memory = c.getOperand(1);
    const uint first = list.m_register - ARM_REG_D0;
    const uint count = list.m_count;
    const uint stride = list.m_stride;
    const bool isLane = list.m_elementIndex >= 0;
    const bool isAll = !isLane && list.m_isExplicit;
    const SemanticType addressType = SemanticType::integer(32);

    const Val base = b.copy(c.getRegister(memory.m_base));
    uint transfer = 0;

    if (isLane)
    {
        const uint index = (uint)list.m_elementIndex;
        for (uint j = 0; j < structure; j++)
        {
            const Val address = (j == 0) ? base :
                c.emit(Op::OP_ADD, addressType, base,
                       c.getImmediate((int64)j * bytes, addressType));
            const Val reg = c.getRegister(ARM_REG_D0 + (first + j * stride) % 32);
            if (isLoad)
            {
                const Val element = c.load(address, SemanticType::integer(e));
                c.move(reg, b.insert(reg, index, e, element));
            } else
            {
                c.store(address, b.lane(reg, index, e));
            }
        }
        transfer = structure * bytes;
    } else if (isAll)
    {
        // Loads only: the element j of the structure to all the lanes of
        // register j
        const uint lanes = 64 / e;
        for (uint j = 0; j < count; j++)
        {
            const uint k = (structure == 1) ? 0 : j;
            const Val address = (k == 0) ? base :
                c.emit(Op::OP_ADD, addressType, base,
                       c.getImmediate((int64)k * bytes, addressType));
            const Val element = c.load(address, SemanticType::integer(e));
            Val lane[8];
            for (uint i = 0; i < lanes; i++)
                lane[i] = element;
            c.move(c.getRegister(ARM_REG_D0 + (first + j * stride) % 32),
                   b.pack(lane, lanes, e));
        }
        transfer = ((structure == 1) ? 1 : structure) * bytes;
    } else
    {
        // The multiple structures
        const uint lanes = 64 / e;
        if (structure == 1)
        {
            for (uint r = 0; r < count; r++)
            {
                const Val address = (r == 0) ? base :
                    c.emit(Op::OP_ADD, addressType, base,
                           c.getImmediate((int64)r * 8, addressType));
                const Val reg = c.getRegister(ARM_REG_D0 + (first + r) % 32);
                if (isLoad)
                    c.move(reg, c.load(address, SemanticType::integer(64)));
                else
                    c.store(address, reg);
            }
        } else
        {
            const uint groups = (structure == 2) ? count / 2 : 1;
            const uint increment = (structure == 2 && count == 4) ? 2 : stride;
            uint offset = 0;
            // The elements of each register are collected before the write
            for (uint r = 0; r < groups; r++)
            {
                Val elements[4][8];
                for (uint el = 0; el < lanes; el++)
                {
                    for (uint j = 0; j < structure; j++)
                    {
                        const Val address = (offset == 0) ? base :
                            c.emit(Op::OP_ADD, addressType, base,
                                   c.getImmediate((int64)offset, addressType));
                        const Val reg = c.getRegister(
                            ARM_REG_D0 + (first + r + j * increment) % 32);
                        if (isLoad)
                            elements[j][el] = c.load(address,
                                                     SemanticType::integer(e));
                        else
                            c.store(address, b.lane(reg, el, e));
                        offset += bytes;
                    }
                }
                if (isLoad)
                    for (uint j = 0; j < structure; j++)
                        c.move(c.getRegister(
                                   ARM_REG_D0 + (first + r + j * increment) % 32),
                               b.pack(elements[j], lanes, e));
            }
        }
        transfer = count * 8;
    }

    // The writeback: "!" adds the size of the transfer, ", Rm" the register
    if (memory.m_addressing == ArmDecodedOperand::ADDRESSING_PRE_INDEX)
    {
        c.move(c.getRegister(memory.m_base),
               c.emit(Op::OP_ADD, addressType, base,
                      c.getImmediate((int64)transfer, addressType)));
    } else if (memory.m_addressing == ArmDecodedOperand::ADDRESSING_POST_INDEX)
    {
        c.move(c.getRegister(memory.m_base),
               c.emit(Op::OP_ADD, addressType, base,
                      c.getRegister(memory.m_postIndexRegister)));
    }
}

} // namespace a32
} // namespace armdis
