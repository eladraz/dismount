/*
 * ArmSemanticTable.cpp
 *
 * Implementation file
 *
 * The semantic table of the AArch32 integer, branch, load / store and system
 * instructions (A32 and T32 share it, see ArmSemantics.h). It holds the
 * instructions which define the AArch32 specific rules of the lifter: the
 * pc reads as the address + 8 / + 4, a write of pc branches and (for bx, blx,
 * the loads of pc and the A32 data processing) interworks, the condition
 * wraps the instruction. The rest of the instruction set comes with the A32 /
 * T32 decoders.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/ArmSemantics.h"

using namespace armdis;

typedef SemanticOperation Op;

namespace {

// The NZCV flags of the AArch32 APSR
const uint32 FLAGS_NZCV = SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO |
                          SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW;

// The link register value of a call: the address of the next instruction,
// with bit 0 set in T32 (the return interworks back)
SemanticOperand getReturnAddress(ArmLiftContext& c)
{
    const uint64 next = c.getInstruction().getAddress() +
                        c.getInstruction().getOpcodeSize();
    const bool isThumb = c.getInstruction().getInstructionSet() ==
                         ArmInstructionSet::ARM_T32;
    return c.getImmediate((int64)((next | (isThumb ? 1 : 0)) & 0xFFFFFFFFULL),
                          SemanticType::integer(32));
}

// b
void liftBranch(ArmLiftContext& c)
{
    c.branch(SemanticOperand::guestAddress(c.getOperand(0).m_immediate,
                                           SemanticType::integer(32)));
}

// bl
void liftBranchLink(ArmLiftContext& c)
{
    c.move(c.getRegister(ARM_REG_LR), getReturnAddress(c));
    c.call(SemanticOperand::guestAddress(c.getOperand(0).m_immediate,
                                         SemanticType::integer(32)));
}

// bx Rm
void liftBranchExchange(ArmLiftContext& c)
{
    c.branchWritePc(c.read(0), true);
}

// blx Rm: the target is read before the link register is written
void liftBranchLinkExchange(ArmLiftContext& c)
{
    const SemanticOperand target = c.emit(Op::OP_MOVE, SemanticType::integer(32),
                                          c.read(0));
    c.move(c.getRegister(ARM_REG_LR), getReturnAddress(c));
    c.branchWritePc(target, true);
}

// mov Rd, Rm / #imm (movs sets N and Z, and C from the shifter: not modeled
// for the shifted forms)
void liftMove(ArmLiftContext& c)
{
    const SemanticType type = SemanticType::integer(32);
    const SemanticOperand value = c.read(1, type);
    if (c.getInstruction().isSettingFlags())
        c.setFlags(Op::FLAGS_LOGIC, SemanticFlags::FLAG_SIGN |
                   SemanticFlags::FLAG_ZERO, type, value, value, value);
    c.write(0, value);
}

// add, adds, sub, subs: a subtraction is the addition of the inverted
// operand, which is the carry of ARM (see Arm64SemanticLift.cpp)
void liftAddSub(ArmLiftContext& c)
{
    const SemanticType type = SemanticType::integer(32);
    const bool isSub = c.getInstruction().getEntry()->m_mnemonic[0] == 's';
    const SemanticOperand a = c.read(1, type);
    SemanticOperand b = c.read(2, type);
    SemanticOperand result = c.emit(isSub ? Op::OP_SUBTRACT : Op::OP_ADD, type,
                                    a, b);
    if (c.getInstruction().isSettingFlags())
    {
        if (isSub)
            b = c.emit(Op::OP_NOT, type, b);
        c.setFlags(Op::FLAGS_ADD, FLAGS_NZCV, type, a, b, result);
    }
    c.write(0, result);
}

// ldr Rt, [Rn, ...]: the load of pc interworks
void liftLoad(ArmLiftContext& c)
{
    const SemanticType type = SemanticType::integer(32);
    const SemanticOperand value = c.read(1, type);
    c.write(0, value);
    c.writeBack(1);
}

#define ARM32_ENTRY(name, handler, category, units, properties) \
    { ArmSemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, category, units, properties, 0, 0, 0, 0 }, \
      handler, 0, 0, 0, 0 }

} // namespace

namespace armdis {

const ArmSemanticEntry gArmSemanticTable[] =
{
    ARM32_ENTRY("b", liftBranch, OpcodeClassification::CATEGORY_BRANCH,
                OpcodeClassification::UNIT_BRANCH, 0),
    ARM32_ENTRY("bl", liftBranchLink, OpcodeClassification::CATEGORY_CALL,
                OpcodeClassification::UNIT_BRANCH, 0),
    ARM32_ENTRY("bx", liftBranchExchange, OpcodeClassification::CATEGORY_BRANCH,
                OpcodeClassification::UNIT_BRANCH,
                OpcodeClassification::PROPERTY_CHANGES_MODE),
    ARM32_ENTRY("blx", liftBranchLinkExchange, OpcodeClassification::CATEGORY_CALL,
                OpcodeClassification::UNIT_BRANCH,
                OpcodeClassification::PROPERTY_CHANGES_MODE),
    ARM32_ENTRY("mov", liftMove, OpcodeClassification::CATEGORY_MOVE,
                OpcodeClassification::UNIT_INTEGER_ALU, 0),
    ARM32_ENTRY("add", liftAddSub, OpcodeClassification::CATEGORY_ARITHMETIC,
                OpcodeClassification::UNIT_INTEGER_ALU, 0),
    ARM32_ENTRY("sub", liftAddSub, OpcodeClassification::CATEGORY_ARITHMETIC,
                OpcodeClassification::UNIT_INTEGER_ALU, 0),
    ARM32_ENTRY("ldr", liftLoad, OpcodeClassification::CATEGORY_LOAD,
                OpcodeClassification::UNIT_LOAD,
                OpcodeClassification::PROPERTY_READS_MEMORY),
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0 }
};

} // namespace armdis
