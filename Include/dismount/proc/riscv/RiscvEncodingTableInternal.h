#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVENCODINGTABLEINTERNAL_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVENCODINGTABLEINTERNAL_H

/*
 * RiscvEncodingTableInternal.h
 *
 * The macros of the generated encoding tables (RiscvEncodingTable*.cpp and
 * the .inc files next to them). Not part of the interface.
 *
 * Author: Elad Raz <e@eladraz.com>
 */

// An operand spec: the kind, the position of the field, the flags, the extra
#define S(kind, position, flags, extra) { (uint8)(kind), (uint8)(position), (uint8)(flags), (uint8)(extra) }
// An entry: the mnemonic, the mask and the value, the feature, the flags, the
// flow, the condition and the operands
#define E(mnemonic, mask, value, feature, flags, flow, condition, ...) \
    { mnemonic, mask, value, { __VA_ARGS__ }, RiscvFeatures::feature, flags, flow, condition, \
      RiscvFeatures::FEAT_I }
// ... with a second feature
#define E2(mnemonic, mask, value, feature, feature2, flags, flow, condition, ...) \
    { mnemonic, mask, value, { __VA_ARGS__ }, RiscvFeatures::feature, flags, flow, condition, \
      RiscvFeatures::feature2 }
// The common flows
#define FLOW_JUMP (Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_RELATIVE_JMP)
#define FLOW_CALL (Opcode::FLOW_STACK_CHANGE | Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_RELATIVE_JMP)
#define FLOW_COND (Opcode::FLOW_COND_ZERO | Opcode::FLOW_RELATIVE_JMP)
#define FLOW_JUMP_REGISTER (Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_ACTION)
#define FLOW_CALL_REGISTER (Opcode::FLOW_STACK_CHANGE | Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_ACTION)
#define FLOW_RETURN (Opcode::FLOW_RET | Opcode::FLOW_ACTION)
#define FLOW_TRAP (Opcode::FLOW_DEBUG)

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVENCODINGTABLEINTERNAL_H
