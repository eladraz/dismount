#include "dismount/dismount.h"
/*
 * ArmOperandCodecA32.cpp
 *
 * Implementation file. The AArch32 (A32 and T32) kinds of ArmOperandCodec.
 *
 * TODO: Only the general purpose registers are implemented. The other
 * AArch32 kinds are reserved, so the A32 and T32 tables (which are empty)
 * decode nothing.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"

namespace armdis {

bool decodeA32(const ArmOperandSpec& spec,
               uint32 encoding,
               const ArmCodecContext& context,
               ArmDecodedOperand& operand)
{
    if (spec.m_kind != ARM_KIND_GPR)
        return false;
    uint number = ArmOperandCodec::getField(encoding, spec.m_position, spec.m_extra);
    if (number > 15)
        return false;
    operand.m_kind = ArmDecodedOperand::KIND_REGISTER;
    operand.m_register = ARM_REG_R0 + number;
    return true;
}

bool encodeA32(const ArmOperandSpec& spec,
               const ArmDecodedOperand& operand,
               const ArmCodecContext& context,
               uint32& encoding)
{
    if ((spec.m_kind != ARM_KIND_GPR) ||
        (operand.m_kind != ArmDecodedOperand::KIND_REGISTER) ||
        (operand.m_register > ARM_REG_PC))
        return false;
    ArmOperandCodec::setField(encoding, spec.m_position, spec.m_extra,
                              operand.m_register - ARM_REG_R0);
    return true;
}

} // namespace armdis
