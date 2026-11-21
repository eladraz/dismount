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
// (an i1 is a byte holding 0 or 1). WIDTH_128 is a 16 byte slot (a wide value,
// see AMD64LoweringWide.cpp): it is not part of WIDTH_ALL, the operations
// of the narrow values refuse it
enum {
    WIDTH_1   = 0x01,
    WIDTH_8   = 0x02,
    WIDTH_16  = 0x04,
    WIDTH_32  = 0x08,
    WIDTH_64  = 0x10,
    WIDTH_128 = 0x20,
    // The integers of the machine
    WIDTH_INTEGERS = WIDTH_8 | WIDTH_16 | WIDTH_32 | WIDTH_64,
    // The integers and the boolean
    WIDTH_ALL = WIDTH_1 | WIDTH_INTEGERS
};

// Return true if the operand is a constant
inline bool isConstant(const SemanticOperand& operand)
{
    return (operand.getKind() == SemanticOperand::KIND_IMMEDIATE) ||
           (operand.getKind() == SemanticOperand::KIND_GUEST_ADDRESS);
}

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

    // The wide values (AMD64LoweringWide.cpp). A wide value is a slot of its
    // size: 8 to 64 bits, or 128 bits (a vector or a 128 bit integer), the
    // low bytes first. Return the number of bits of a value, a boolean is 1
    uint getWideBits(const SemanticType& type) const;
    // Return the memory operand of the slot of an operand, 'extra' bytes
    // after its start: 'oword [r15+0x210]' for 128 bits
    cString getSlotMemory(const SemanticOperand& operand, uint extra,
                          uint bits) const;
    // Load 'bits' of the operand, 'extra' bytes after its start, zero extended
    // into the scratch register (a constant is loaded whole)
    void loadPiece(Scratch scratch, const SemanticOperand& operand,
                   uint extra, uint bits);
    // Store the low 'bits' of the scratch register at 'extra' bytes of the
    // slot of the destination
    void storePiece(const SemanticOperand& destination, uint extra,
                    Scratch scratch, uint bits);
    // Copy 'bits' bits of a value to the destination: through xmm0 for 128
    // bits, through rax for the others. A constant has no 128 bit form: it
    // is refused for 128 bits, so a BITCAST of a constant is up to 64 bits
    void copySlot(const SemanticOperand& destination,
                  const SemanticOperand& source, uint bits);
    void lowerWideLoad(const SemanticInstruction& instruction);
    void lowerWideStore(const SemanticInstruction& instruction);
    void lowerWideConcat(const SemanticInstruction& instruction);
    // Store zero at 'extra' bytes of the destination: a qword
    void zeroQword(const SemanticOperand& destination, uint extra);
    // Return the constant bit position of an EXTRACT or INSERT operand, a
    // lane index scaled by 'scale' (1 for a bit offset). Throw if the operand
    // is not a constant
    uint64 getWidePosition(const SemanticOperand& position, uint scale) const;
    void lowerWideExtract(const SemanticInstruction& instruction);
    void lowerWideInsert(const SemanticInstruction& instruction);
    void lowerWideBroadcast(const SemanticInstruction& instruction);
    // AND, OR, XOR, NOT and AND_NOT of 128 bits, one qword at a time
    void lowerWideLogic(const SemanticInstruction& instruction);
    // The float values of 8 to 64 bits: their bits are moved as an integer
    void lowerWideFloatLoad(const SemanticInstruction& instruction);
    void lowerWideFloatStore(const SemanticInstruction& instruction);

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

    // The scalar floating point lowering (AMD64LoweringFloat.cpp): the
    // width in bits (32 or 64) of a scalar float type, throws for the others
    uint getFloatWidth(const SemanticType& type) const;
    // Throw for the behavior and the attributes the lowering doesn't do
    void checkFloatBehavior(const SemanticInstruction& instruction) const;
    // Throw for the behavior and the attributes the lowering doesn't do
    void checkFloatInstruction(const SemanticInstruction& instruction) const;
    // Throw for a rounding mode the lowering of OP_FLOAT_ROUND and of the
    // float to integer conversions doesn't do (round to odd)
    void checkFloatRounding(const SemanticInstruction& instruction) const;
    // Throw for a rounding mode the integer to float conversions don't do
    // (nearest away: no directed form in MXCSR)
    void checkIntegerToFloatRounding(const SemanticInstruction& instruction) const;
    // Emit the integer to float conversion 'convert' (cvtsi2sd/ss) in 'mode'.
    // A directed mode sets the rounding control of MXCSR for that conversion
    // only, through an 8 byte stack slot below rsp. Clobbers rcx and the flags
    void lineConvertToFloat(const char* convert, SemanticOperation::RoundingMode mode);
    // The rounding control of MXCSR for the directed modes, around one
    // instruction: lineRoundingBegin(mode) saves MXCSR and loads the control
    // of 'mode' (nothing for nearest even and dynamic), lineRoundingEnd(mode)
    // restores it. Both clobber rcx and use an 8 byte slot below rsp
    void lineRoundingBegin(SemanticOperation::RoundingMode mode);
    void lineRoundingEnd(SemanticOperation::RoundingMode mode);
    // Round xmm0 (a float of 'bits') to an integral value in the mode (SSE4.1
    // roundsd/roundss; nearest away by a sequence). Clobbers rax, rcx, rdx,
    // xmm1 and xmm2
    void roundXmm0(SemanticOperation::RoundingMode mode, uint bits);
    // Load a float operand of 'bits' into xmm0 or xmm1 (rax for a constant)
    void loadXmm(uint xmm, const SemanticOperand& operand, uint bits);
    // Store xmm0 into a float destination of 'bits'
    void storeXmm(const SemanticOperand& destination, uint bits);
    // Load the bits of an operand into a scratch register (zero extended)
    void loadBits(Scratch scratch, const SemanticOperand& operand, uint bits);
    // Store the low bits of a scratch register into a destination
    void storeBits(const SemanticOperand& destination, Scratch scratch,
                   uint bits);
    void lowerFloatArithmetic(const SemanticInstruction& instruction);
    void lowerFloatCompare(const SemanticInstruction& instruction);
    void lowerFloatSelect(const SemanticInstruction& instruction);
    // OP_FLOAT_ROUND of a scalar f32/f64 to an integral value (section 4)
    void lowerFloatRound(const SemanticInstruction& instruction);
    // Load the bit image 'pattern' of a float of 'bits' into an xmm register
    // (through rcx)
    void loadPatternXmm(uint xmm, uint64 pattern, uint bits);
    // The fixed point count of a conversion (source 1, zero when absent)
    uint getFractionBits(const SemanticInstruction& instruction) const;
    // Load a float operand into xmm0 times 2^fraction (a float to integer
    // conversion of a fixed point count), then round it to an integral value
    // in 'mode' unless the mode is toward zero (the conversion truncates)
    void loadScaledXmm(const SemanticOperand& source, uint sourceBits,
                       uint fraction, SemanticOperation::RoundingMode mode);
    // The conversions between the floats and the integers (section 4 of
    // AMD64LoweringFloat.cpp)
    void lowerIntegerToFloat(const SemanticInstruction& instruction);
    void lowerFloatToInteger(const SemanticInstruction& instruction);
    void lowerFloatConvert(const SemanticInstruction& instruction);
    // OP_FLOAT_CLASSIFY of a scalar f32/f64: the fclass mask (section 5 of
    // AMD64LoweringFloat.cpp)
    void lowerFloatClassify(const SemanticInstruction& instruction);
    // OP_FLOAT_MINIMUM / OP_FLOAT_MAXIMUM of a scalar f32/f64 with
    // BEHAVIOR_MINMAX_NUMBER (IEEE 754-2019 minimumNumber / maximumNumber, the
    // RISC-V fmin and fmax; section 6 of AMD64LoweringFloat.cpp)
    void lowerFloatMinMax(const SemanticInstruction& instruction);
    void lowerRiscvNaNMinMax(const SemanticOperand& s0, const SemanticOperand& s1, uint bits);
    void lowerArmNaNMinMax(const SemanticOperand& s0, const SemanticOperand& s1, uint bits);
    // OP_FLOAT_MULTIPLY_ADD of a scalar f32/f64 (section 8): d = s0 * s1 + s2,
    // one rounding (vfmadd231sd/ss, FMA3), and the NaN rule of the source ISA
    void lowerFloatMultiplyAdd(const SemanticInstruction& instruction);
    void emitFusedMultiplyAdd231(uint bits);
    void loadFusedOperand(uint reg, const SemanticOperand& operand, uint bits);
    void fusedNaNTest(uint reg, const SemanticOperand& operand, bool signaling, uint bits);
    void fusedNaNChain(bool signaling, const SemanticOperand& c,
                       const SemanticOperand& n, const SemanticOperand& m, uint bits);
    void lowerArmFusedNaN(const SemanticOperand& c, const SemanticOperand& n,
                          const SemanticOperand& m, uint bits);
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
