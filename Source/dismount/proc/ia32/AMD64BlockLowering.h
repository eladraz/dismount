#ifndef __TBA_DISMOUNT_PROC_IA32_AMD64BLOCKLOWERING_H
#define __TBA_DISMOUNT_PROC_IA32_AMD64BLOCKLOWERING_H

/*
 * AMD64BlockLowering.h
 *
 * The private declarations of the AMD64 lowering of one block, shared by
 * AMD64Lowering.cpp and the files which lower the groups of IR operations
 * (AMD64LoweringWide.cpp, AMD64LoweringInteger.cpp, AMD64LoweringFloat.cpp).
 * Not a public interface: use AMD64Lowering (AMD64Lowering.h).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/proc/ia32/AMD64Lowering.h"

namespace amd64lowering {

// The widths a value can have in the state, as a bit mask of the widths
// (an i1 is a byte holding 0 or 1)
enum {
    WIDTH_1  = 0x01,
    WIDTH_8  = 0x02,
    WIDTH_16 = 0x04,
    WIDTH_32 = 0x08,
    WIDTH_64 = 0x10,
    // The integers of the machine
    WIDTH_INTEGERS = WIDTH_8 | WIDTH_16 | WIDTH_32 | WIDTH_64,
    // The integers and the boolean
    WIDTH_ALL = WIDTH_1 | WIDTH_INTEGERS
};

// How a narrow value is widened into a register
enum Extension {
    EXTEND_ZERO,
    EXTEND_SIGN
};

// The scratch registers
enum Scratch {
    SCRATCH_A = 0,
    SCRATCH_C = 1,
    SCRATCH_D = 2
};

// The lowering of a single block
class BlockLowering {
public:
    BlockLowering(const ArchitectureDescriptionPtr& target,
                  const SemanticBlock& block,
                  const SemanticRegisterMap& registerMap,
                  AssemblerInterface& assembler,
                  bool allowLocalLabels);

    // Lower the block
    void lower();

    // The B_ labels the code defines, and the L_ labels
    const cList<AMD64Lowering::LabelDefinition>& getGuestLabels() const
    {
        return m_guestLabels;
    }
    const cList<AMD64Lowering::LabelDefinition>& getLocalLabels() const
    {
        return m_localLabels;
    }

private:
    // Throw an error naming the current operation and guest address
    void fail(const char* reason) const;
    void failText(const cString& reason) const;

    // The guest address of the IR instruction 'index'
    uint64 getGuestAddress(uint index) const;
    // The address where the block continues when it doesn't branch: the one
    // set in the block or, when none was set, the address after its last
    // guest instruction
    uint64 getFallThroughAddress() const;

    // Return the width of the type in bits (1 for a boolean); throw if the
    // type isn't one of the 'widths'
    uint getWidth(const SemanticType& type, uint widths) const;
    // Return the mask bit of WIDTH_* of a width in bits
    static uint getWidthMask(uint bits);
    // Return the register of the scratch, in 'bits' (1 is a byte)
    static const char* getRegister(Scratch scratch, uint bits);
    // Return the size keyword of an access of 'bits'
    static const char* getSizeKeyword(uint bits);

    // Return the offset of a register or a temporary in the state
    uint getSlotOffset(const SemanticOperand& operand) const;
    // Return the state memory operand: 'qword [r15+0x108]'
    cString getMemory(const SemanticOperand& operand, uint bits) const;
    // Return the value of a constant operand, truncated to 'bits'
    uint64 getConstant(const SemanticOperand& operand, uint bits) const;
    // Return the text of a constant read as the signed value of 'bits'
    cString getImmediate(const SemanticOperand& operand, uint bits) const;
    // Return true if the constant can be the immediate of a 'bits' operation
    bool isImmediate(const SemanticOperand& operand, uint bits) const;

    // Write a line of assembly (the lines of an IR instruction are assembled
    // together, see flush())
    void line(const cString& text);
    void line(const char* text);
    // Assemble the lines written
    void flush();

    // Load the operand into the scratch register, as a 64 bit value
    // extended from the width of the operand
    void load(Scratch scratch, const SemanticOperand& operand,
              Extension extension);
    // Store the low bits of the scratch register into the destination
    void store(const SemanticOperand& destination, Scratch scratch);
    // Expand and write a pattern text
    void expand(const char* text, uint bits,
                const SemanticOperand& destination,
                const SemanticOperand* sources);
    // Find and write the pattern of an operation
    bool applyPattern(const SemanticInstruction& instruction);

    // The IR operations
    void lowerMove(const SemanticInstruction& instruction);
    void lowerExtend(const SemanticInstruction& instruction,
                     Extension extension);
    void lowerTruncate(const SemanticInstruction& instruction);
    void lowerLoad(const SemanticInstruction& instruction);
    void lowerStore(const SemanticInstruction& instruction);
    void lowerFence(const SemanticInstruction& instruction);
    void lowerShift(const SemanticInstruction& instruction);
    void lowerRotate(const SemanticInstruction& instruction);
    void lowerCompare(const SemanticInstruction& instruction);
    void lowerSelect(const SemanticInstruction& instruction);
    void lowerBranch(const SemanticInstruction& instruction);
    void lowerBranchConditional(const SemanticInstruction& instruction);
    void lowerTrap(const SemanticInstruction& instruction);
    void lowerLabel(const SemanticInstruction& instruction);

    // The offset in the code of the next instruction written
    uint getOffset() const;
    // Return the name of the local label 'index', throw if not allowed
    cString getLocalLabel(const SemanticOperand& label) const;

    // Leave the translated code: the target (a guest address in a register,
    // a temporary or a constant) goes to the program counter slot, the reason
    // to the exit reason slot, and the code jumps to the exit label
    void writeExit(const SemanticOperand& target, uint64 reason);
    // Jump to the label of a constant guest address
    void jumpToGuest(const char* mnemonic, uint64 address);

    // Return the exit reason write: 'mov qword [state+reason], value'
    cString getExitReasonStore(uint64 reason) const;

    // The target description
    ArchitectureDescriptionPtr m_target;
    // The block
    const SemanticBlock& m_block;
    // The registers of the guest
    const SemanticRegisterMap& m_map;
    // The assembler
    AssemblerInterface& m_assembler;
    // The name of the register which points to the state
    cString m_state;
    // The name of the register which holds the bias of the guest memory
    cString m_base;
    // Local labels are allowed (lowerBlock())
    bool m_allowLocalLabels;
    // The labels defined by the code
    cList<AMD64Lowering::LabelDefinition> m_guestLabels;
    cList<AMD64Lowering::LabelDefinition> m_localLabels;
    // The lines which wait for the assembler
    cString m_pending;
    // The IR instruction which is lowered
    uint m_index;
    // Its operation
    SemanticOperation::Operation m_operation;
    // Its type, in bits
    uint m_typeBits;

    // The operations of the other files of the lowering (AMD64LoweringWide.cpp,
    // AMD64LoweringInteger.cpp, AMD64LoweringFloat.cpp). Each one lowers the
    // IR operations of its group and returns true; it returns false, leaving
    // the instruction alone, for any other operation.
    bool lowerWide(const SemanticInstruction& instruction);
    bool lowerIntegerExtra(const SemanticInstruction& instruction);
    bool lowerFloat(const SemanticInstruction& instruction);
    // The operations of AMD64LoweringInteger.cpp. A division uses the local
    // labels L_d<index>_<tag> of its IR instruction, so it is only lowered by
    // lowerBlock() (see defineLocalLabel())
    void lowerDivide(const SemanticInstruction& instruction);
    void lowerCountZeros(const SemanticInstruction& instruction);
    void lowerPopulationCount(const SemanticInstruction& instruction);
    void lowerByteSwap(const SemanticInstruction& instruction);
    void lowerBitReverse(const SemanticInstruction& instruction);
    void lowerHighSignedUnsigned(const SemanticInstruction& instruction);
    void lowerAddCarry(const SemanticInstruction& instruction);
    // Define the local label 'name' at the current offset of the code
    void defineLocalLabel(const cString& name);
    // Leave the code with the divide error of the current IR instruction:
    // the guest instruction is the program counter, then the exit
    void writeDivideFault();
};

} // namespace amd64lowering

#endif // __TBA_DISMOUNT_PROC_IA32_AMD64BLOCKLOWERING_H
