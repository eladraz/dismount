#ifndef __TBA_DISMOUNT_PROC_IA32_AMD64LOWERING_H
#define __TBA_DISMOUNT_PROC_IA32_AMD64LOWERING_H

/*
 * AMD64Lowering.h
 *
 * The lowering of the semantic IR onto the AMD64 processor: every IR
 * instruction of a legalized block is written as AMD64 instructions through
 * the IA-32 assembler.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/sarray.h"
#include "xStl/data/list.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticLowering.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/assembler/AssemblerInterface.h"
#include "dismount/assembler/BinaryDependencies.h"

/*
 * The AMD64 lowering. The code generated for a block follows these rules (the
 * "register map contract"), which the translator which links the blocks and
 * the runtime which runs them rely on:
 *
 *  Registers
 *    state register  (SemanticRegisterMap::getStateRegister(), r15 by the
 *                    convention): points to the guest state. Every guest
 *                    register lives in the state at getStateOffset(id), and
 *                    every IR temporary at getTemporaryOffset(n), an 8 byte
 *                    slot. A value of N bits occupies the low N/8 bytes of
 *                    its slot (an i1 is a byte holding 0 or 1); the other
 *                    bytes of the slot are not defined, so a value is always
 *                    read with its own width.
 *    memory base     (SemanticRegisterMap::getMemoryBaseRegister(), r14 by
 *                    the convention): the bias of the guest memory, host
 *                    address = guest address + r14. Loads and stores are
 *                    [r14 + rax], the guest address is in rax.
 *    scratch         rax, rcx and rdx. They are free at the start of every IR
 *                    instruction and nothing is kept in them between two IR
 *                    instructions. Some operations use more: the minimum and
 *                    maximum (ARM NaN rule) clobber r8 and r9, and the float
 *                    code uses xmm0 to xmm3 (the rounding helper uses xmm2).
 *                    None of them is kept between IR instructions either.
 *                    The state and memory base registers, rsp and the other
 *                    general purpose registers are never touched: the code
 *                    doesn't use the host stack.
 *    The state and the memory base register must be 64 bit general purpose
 *    registers other than rax, rcx, rdx, r8, r9 and rsp.
 *
 *  Control flow
 *    The code of a block is a sequence of instructions which falls through
 *    to its end or leaves with a jump. A jump to a guest address is
 *        jmp / jcc  B_<lowercase hex guest address, no 0x prefix>
 *    (see getBlockLabel()), an assembler label dependency (rel32, DEP_RELATIVE
 *    4 bytes) which the translator resolves. The lowering never defines a B_
 *    label: the translator places label B_<va> at the start of the code it
 *    generated for the block (or the guest instruction) at <va>.
 *    A block which doesn't end with a branch, a call, a return or a trap ends
 *    with a jump to its fall-through address (SemanticBlock::
 *    getFallThroughAddress(); when the lifter left it unset, the address after
 *    the last guest instruction of the block).
 *    A branch or a call to a constant address is a plain jump (the return
 *    address of a call is written to the link register by the IR itself).
 *    A branch, a call or a return to a register or temporary value leaves the
 *    translated code:
 *        mov qword [state + getProgramCounterOffset()], <target>
 *        mov qword [state + getExitReasonOffset()], <exit reason>
 *        jmp __reforge_exit
 *    where __reforge_exit (getExitLabel()) is a dependency too: the code
 *    which restores the host state and returns to the runtime. The exit
 *    reason is the ExitReason, see below.
 *    OP_TRAP leaves the same way, with the program counter slot holding the
 *    guest address of the trapping instruction.
 *
 *  Pattern text form
 *    The instructions of an IR operation are a text, given line by line to
 *    the IA-32 assembler. Placeholders:
 *        $d          the destination, '<size> [state + offset]'
 *        $0 $1 $2    the sources, the same form, or the immediate; a 64 bit
 *                    immediate which doesn't fit 32 bits is first loaded into
 *                    rcx (rdx for the second one) when the line isn't a mov
 *        %a %c %x    rax, rcx, rdx, in the size of the operation (al, ax,
 *                    eax, rax)
 *        %a32 %c32 %x32 %a64 %c64 %x64   in a fixed size
 *    The size is the width of the type of the instruction. Lines are
 *    separated by '\n'. One text covers one IR operation (the multiple IR
 *    instruction patterns of SemanticLoweringPattern are not used yet).
 *
 *  Labels of the block
 *    OP_LABEL defines the local label L_<n> (n is the label number of
 *    SemanticBlock::newLabel()) at the current offset and a branch to a label
 *    of the block is 'jmp / jcc L_<n>'. They exist only for lowerBlock(),
 *    which resolves them itself: lower() refuses a block that has them.
 *
 * How Reforge calls the lowering: lowerBlock()
 *    LoweredBlock result;
 *    SemanticLoweringFactory::getLowering(DISASSEMBLER_AMD_64) ... or directly
 *    AMD64Lowering lowering; lowering.lowerBlock(block, registerMap, result);
 *    - result.m_code     the encoded bytes of the block (position independent
 *                        but for the rel32 of the dependencies, zero filled)
 *    - result.m_labels   the definitions B_<va>: for each guest instruction of
 *                        the block, the offset in m_code where its code starts
 *    - result.m_dependencies  the references that are not resolved:
 *                        (name, offset of the rel32 field, 4 bytes, DEP_RELATIVE).
 *                        The names are B_<va> (a guest address; defined by
 *                        this block, by another one, or nowhere: the
 *                        translator decides) and __reforge_exit.
 *    To link, the translator places the code at some position P, and for each
 *    dependency d whose name is at position T (an offset in the final image):
 *        rel32 at P + d.m_offset = T - (P + d.m_offset + 4)
 *    (patchRelative32() stores the value into the buffer).
 *    The block must be legalized for AMD64 first (SemanticLegalizer).
 *    Any IR operation, width, operand or register map the lowering can't do
 *    throws cException naming the operation and the guest address: nothing is
 *    skipped or approximated.
 *
 * The coverage of the IR operations is listed at the top of AMD64Lowering.cpp.
 *
 * NOTE: This class is not thread-safe. Only one thread can work on a single
 *       instance.
 */
class AMD64Lowering : public SemanticLowering {
public:
    /*
     * Why the translated code returned to the runtime, written to the exit
     * reason slot of the guest state (getExitReasonOffset()) before the jump
     * to __reforge_exit
     */
    enum ExitReason {
        // Not an exit
        EXIT_NONE = 0,
        // A branch to the guest address in the program counter slot
        EXIT_BRANCH = 1,
        // A call to the guest address in the program counter slot (the
        // return address is already in the link register)
        EXIT_CALL = 2,
        // A return to the guest address in the program counter slot
        EXIT_RETURN = 3,
        // A trap. The program counter slot holds the address of the
        // instruction; bits 8-15 of the reason are the
        // SemanticOperation::TrapKind and bits 16-63 the trap number (s0)
        EXIT_TRAP = 4
    };

    // The shift of the trap kind and of the trap number in EXIT_TRAP
    enum {
        EXIT_TRAP_KIND_SHIFT = 8,
        EXIT_TRAP_NUMBER_SHIFT = 16
    };

    // A label defined by the lowered code: B_<guest address> of a guest
    // instruction of the block
    struct LabelDefinition {
        LabelDefinition() : m_offset(0) {}
        // The name of the label
        cString m_name;
        // The offset in the code of the block where the label is
        uint m_offset;
    };

    // A reference of the lowered code to a label which isn't resolved
    struct LabelDependency {
        LabelDependency() : m_offset(0), m_size(4),
                            m_kind(BinaryDependencies::DEP_RELATIVE) {}
        // The name of the label (B_<guest address> or __reforge_exit)
        cString m_name;
        // The offset in the code of the field to patch
        uint m_offset;
        // The size of the field in bytes (4)
        uint m_size;
        // DEP_RELATIVE: the field is target - (address of the field + size)
        BinaryDependencies::DependencyType m_kind;
    };

    // The result of lowerBlock()
    struct LoweredBlock {
        // The encoded bytes
        cBuffer m_code;
        // The labels the block defines, by offset order
        cList<LabelDefinition> m_labels;
        // The references to resolve, by offset order
        cList<LabelDependency> m_dependencies;
    };

    // Constructor
    AMD64Lowering();

    /*
     * Lower a legalized block into AMD64 bytes. See "How Reforge calls the
     * lowering" above
     *
     * block       - The block, legalized for AMD64
     * registerMap - Where the guest registers and the temporaries live
     * result      - Replaced with the encoded block
     *
     * Throw exception if an operation can't be lowered
     */
    void lowerBlock(const SemanticBlock& block,
                    const SemanticRegisterMap& registerMap,
                    LoweredBlock& result);

    /*
     * Write a relative dependency into the code: the little endian 32 bit
     * field at 'fieldOffset' of 'code' becomes 'relative', which is
     * target - (address of the field + 4) in one address space. Throw
     * exception if it doesn't fit 32 bits or the field is out of the code
     */
    static void patchRelative32(cBuffer& code, uint fieldOffset,
                                int64 relative);

    // See SemanticLowering::getTarget()
    virtual ArchitectureDescriptionPtr getTarget() const;

    // See SemanticLowering::lower()
    virtual void lower(const SemanticBlock& block,
                       const SemanticRegisterMap& registerMap,
                       AssemblerInterface& assembler);

    /*
     * Return the name of the label of a guest address: "B_" and the address
     * as lowercase hexadecimal without 0x ("B_140001000")
     */
    static cString getBlockLabel(uint64 guestAddress);

    // Return the name of the label of the exit code ("__reforge_exit")
    static cString getExitLabel();

private:
    // The description of the target
    ArchitectureDescriptionPtr m_target;
};

#endif // __TBA_DISMOUNT_PROC_IA32_AMD64LOWERING_H
