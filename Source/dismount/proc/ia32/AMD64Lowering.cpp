/*
 * AMD64Lowering.cpp
 *
 * The lowering of the semantic IR onto AMD64. The conventions of the generated
 * code (registers, labels, exits, pattern text) are documented in
 * AMD64Lowering.h.
 *
 * Coverage of the IR operations (one IR instruction at a time; an operation
 * which is not DONE throws "AMD64 lowering: operation <name><bits> at guest
 * 0x<address>: ..."). DONE means lowered AND run natively against the
 * SemanticInterpreter: tests/TestSemanticLowering.cpp runs the 230 RV64I rows
 * (lowered bytes decoded and lifted back, and run in the interpreter, against
 * the row expectations), and, on an x86-64 Linux host, the lowered code natively
 * against the interpreter on random inputs, RISC-V rows and synthetic IR of
 * every width 8/16/32/64.
 *
 *   WP8a  data movement: MOVE ZERO_EXTEND SIGN_EXTEND TRUNCATE LOAD STORE
 *         FENCE NOP HINT
 *   WP8b  ALU / compare: ADD SUBTRACT AND OR XOR NOT NEGATE AND_NOT MULTIPLY
 *         MULTIPLY_HIGH_* SHIFT_* ROTATE_* COMPARE SELECT
 *   WP8c  control flow: BRANCH BRANCH_CONDITIONAL CALL RETURN BRANCH_INDIRECT
 *         TRAP and the fall-through jump of a block
 *
 *   Operation                          State  Note
 *   ---------------------------------  -----  -------------------------------
 *   OP_NOP                             DONE   WP8a nothing
 *   OP_MOVE                            DONE   WP8a
 *   OP_ZERO_EXTEND                     DONE   WP8a
 *   OP_SIGN_EXTEND                     DONE   WP8a
 *   OP_TRUNCATE                        DONE   WP8a
 *   OP_EXTRACT                         DONE   constant position, 1..64 bits (AMD64LoweringWide.cpp)
 *   OP_INSERT                          DONE   constant position, into 1..64 bits and 128 bits (bytes)
 *   OP_CONCAT                          DONE   2x64->128, 2x8..32->8..64 (AMD64LoweringWide.cpp)
 *   OP_BITCAST                         DONE   any width, int<->float<->vector (AMD64LoweringWide.cpp)
 *   OP_LOAD                            DONE   WP8a 8/16/32/64, [base+rax]
 *   OP_STORE                           DONE   WP8a 8/16/32/64
 *   OP_LOAD_MASKED                     TODO   later
 *   OP_STORE_MASKED                    TODO   later
 *   OP_GATHER                          TODO   later
 *   OP_SCATTER                         TODO   later
 *   OP_LOAD_INTERLEAVED                TODO   later
 *   OP_STORE_INTERLEAVED               TODO   later
 *   OP_ATOMIC_EXCHANGE                 DONE   xchg, 8-64 (AMD64LoweringAtomic.cpp)
 *   OP_ATOMIC_COMPARE_EXCHANGE         DONE   lock cmpxchg, 8-64; 128 bits refused (rbx)
 *   OP_ATOMIC_RMW                      DONE   add/sub lock xadd; and/or/xor/and_not/min/max lock cmpxchg loop, 8-64 (lowerBlock)
 *   OP_LOAD_EXCLUSIVE                  TODO   later
 *   OP_STORE_EXCLUSIVE                 TODO   later
 *   OP_FENCE                           DONE   WP8a mfence only for store->load or sequential order (TSO target)
 *   OP_FENCE_INSTRUCTION               TODO   later (the translator drops translations)
 *   OP_PREFETCH                        TODO   later (may be removed: nothing)
 *   OP_CACHE_MAINTENANCE               TODO   later
 *   OP_PROBE_ACCESS                    TODO   later
 *   OP_ADD                             DONE   WP8b
 *   OP_ADD_CARRY                       DONE   d = s0 + s1 + (s2 & 1), 8-64 (AMD64LoweringInteger.cpp)
 *   OP_SUBTRACT                        DONE   WP8b
 *   OP_SUBTRACT_BORROW                 TODO   WP8b
 *   OP_NEGATE                          DONE   WP8b
 *   OP_MULTIPLY                        DONE   WP8b low half, 8/16/32/64
 *   OP_MULTIPLY_HIGH_SIGNED            DONE   WP8b 16/32/64
 *   OP_MULTIPLY_HIGH_UNSIGNED          DONE   WP8b 16/32/64
 *   OP_MULTIPLY_HIGH_SIGNED_UNSIGNED   DONE   16/32/64: mul, minus s1 when s0 < 0 (8 bits refused)
 *   OP_DIVIDE_SIGNED                   DONE   idiv guarded; behaviours fault/zero/all ones (lowerBlock only)
 *   OP_DIVIDE_UNSIGNED                 DONE   div guarded; behaviours fault/zero/all ones (lowerBlock only)
 *   OP_REMAINDER_SIGNED                DONE   as OP_DIVIDE_SIGNED, the remainder
 *   OP_REMAINDER_UNSIGNED              DONE   as OP_DIVIDE_UNSIGNED, the remainder
 *   OP_ABSOLUTE_DIFFERENCE_SIGNED      TODO   later
 *   OP_ABSOLUTE_DIFFERENCE_UNSIGNED    TODO   later
 *   OP_CARRYLESS_MULTIPLY              TODO   later
 *   OP_CARRYLESS_MULTIPLY_HIGH         TODO   later
 *   OP_AND                             DONE   WP8b; 128 bits (two qwords)
 *   OP_OR                              DONE   WP8b; 128 bits
 *   OP_XOR                             DONE   WP8b; 128 bits
 *   OP_NOT                             DONE   WP8b; 128 bits
 *   OP_AND_NOT                         DONE   WP8b; 128 bits
 *   OP_BITWISE_SELECT                  TODO   later
 *   OP_SHIFT_LEFT                      DONE   WP8b count >= width clamped
 *   OP_SHIFT_RIGHT_LOGICAL             DONE   WP8b
 *   OP_SHIFT_RIGHT_ARITHMETIC          DONE   WP8b
 *   OP_ROTATE_LEFT                     DONE   WP8b
 *   OP_ROTATE_RIGHT                    DONE   WP8b
 *   OP_COUNT_LEADING_ZEROS             DONE   bsr + cmov for zero, 8-64
 *   OP_COUNT_TRAILING_ZEROS            DONE   bsf + cmov for zero, 8-64
 *   OP_COUNT_LEADING_SIGN_BITS         TODO   later
 *   OP_POPULATION_COUNT                DONE   SWAR, 8-64 (no popcnt)
 *   OP_BYTE_SWAP                       DONE   rol / bswap, 8-64
 *   OP_BIT_REVERSE                     DONE   SWAR + bswap, 8-64
 *   OP_BIT_DEPOSIT                     TODO   later
 *   OP_BIT_GATHER                      TODO   later
 *   OP_COMPARE                         DONE   WP8b all 10 integer conditions + ALWAYS/NEVER; floats
 *   OP_SELECT                          DONE   WP8b scalar integer; floats
 *   OP_SET_FLAGS                       TODO   later
 *   OP_TEST_FLAGS                      TODO   later
 *   OP_FLOAT_ADD                       DONE   addsd/addss
 *   OP_FLOAT_SUBTRACT                  DONE   subsd/subss
 *   OP_FLOAT_MULTIPLY                  DONE   mulsd/mulss (default behavior)
 *   OP_FLOAT_DIVIDE                    DONE   divsd/divss
 *   OP_FLOAT_MULTIPLY_ADD              DONE   vfmadd231sd/ss (FMA3, one rounding, NaN rule of the source ISA; AMD64LoweringFloat.cpp section 8)
 *   OP_FLOAT_SQUARE_ROOT               DONE   sqrtsd/sqrtss
 *   OP_FLOAT_MINIMUM                   TODO   later
 *   OP_FLOAT_MAXIMUM                   TODO   later
 *   OP_FLOAT_ABSOLUTE                  DONE   sign bit cleared (btr)
 *   OP_FLOAT_NEGATE                    DONE   sign bit toggled (btc)
 *   OP_FLOAT_COPY_SIGN                 TODO   later
 *   OP_FLOAT_ROUND                     DONE   roundsd/roundss (SSE4.1), all modes (AMD64LoweringFloat.cpp section 6)
 *   OP_FLOAT_CONVERT                   DONE   f32<->f64 (cvtss2sd/cvtsd2ss)
 *   OP_SIGNED_TO_FLOAT                 DONE   8-64 to f32/f64 (cvtsi2ss/sd)
 *   OP_UNSIGNED_TO_FLOAT               DONE   8-64 to f32/f64, 64 bit halve-and-double (sticky bit)
 *   OP_FLOAT_TO_SIGNED                 DONE   32/64 toward zero, indefinite/NaN-zero/NaN-max (cvtt + cmov); every rounding mode but odd (round first)
 *   OP_FLOAT_TO_UNSIGNED               DONE   32/64 toward zero, indefinite/NaN-zero/NaN-max (cmov); every rounding mode but odd (round first)
 *   OP_FLOAT_CLASSIFY                  DONE   f32/f64 fclass mask, branch free (AMD64LoweringFloat.cpp)
 *   OP_FLOAT_SCALE                     TODO   later
 *   OP_FLOAT_GET_EXPONENT              TODO   later
 *   OP_FLOAT_RECIPROCAL_ESTIMATE       TODO   later
 *   OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE  TODO   later
 *   OP_ADD_SATURATE_SIGNED             TODO   later
 *   OP_ADD_SATURATE_UNSIGNED           TODO   later
 *   OP_SUBTRACT_SATURATE_SIGNED        TODO   later
 *   OP_SUBTRACT_SATURATE_UNSIGNED      TODO   later
 *   OP_AVERAGE_UNSIGNED                TODO   later
 *   OP_MINIMUM_SIGNED                  TODO   later
 *   OP_MINIMUM_UNSIGNED                TODO   later
 *   OP_MAXIMUM_SIGNED                  TODO   later
 *   OP_MAXIMUM_UNSIGNED                TODO   later
 *   OP_ABSOLUTE                        TODO   later
 *   OP_MULTIPLY_HIGH_ROUNDING          TODO   later
 *   OP_SHIFT_RIGHT_ROUNDING            TODO   later
 *   OP_DOT_PRODUCT                     TODO   later
 *   OP_PAIRWISE_ADD                    TODO   later
 *   OP_BROADCAST                       DONE   lanes of 8..64 bits, up to 128 bits (AMD64LoweringWide.cpp)
 *   OP_SHUFFLE                         TODO   later
 *   OP_INTERLEAVE                      TODO   later
 *   OP_SLIDE                           TODO   later
 *   OP_NARROW                          TODO   later
 *   OP_COMPRESS                        TODO   later
 *   OP_EXPAND                          TODO   later
 *   OP_LANE_INDEX                      TODO   later
 *   OP_ACTIVE_LANE_MASK                TODO   later
 *   OP_MASK_COUNT                      TODO   later
 *   OP_MASK_FIRST                      TODO   later
 *   OP_REDUCE                          TODO   later
 *   OP_SIGN_MASK                       TODO   later
 *   OP_AES_SUB_SHIFT                   TODO   later
 *   OP_AES_INVERSE_SUB_SHIFT           TODO   later
 *   OP_AES_MIX_COLUMNS                 TODO   later
 *   OP_AES_INVERSE_MIX_COLUMNS         TODO   later
 *   OP_CRC32                           TODO   later
 *   OP_LABEL                           DONE   WP8c local label L_<n>, resolved by lowerBlock()
 *   OP_BRANCH                          DONE   WP8c constant -> jmp B_x; register -> exit EXIT_BRANCH; label -> jmp L_n
 *   OP_BRANCH_CONDITIONAL              DONE   WP8c constant target (cmp byte,0; jne B_x / L_n); register target throws
 *   OP_BRANCH_INDIRECT                 DONE   WP8c exit EXIT_BRANCH
 *   OP_CALL                            DONE   WP8c constant -> jmp B_x; register -> exit EXIT_CALL
 *   OP_RETURN                          DONE   WP8c constant -> jmp; register -> exit EXIT_RETURN
 *   OP_TRAP                            DONE   WP8c exit EXIT_TRAP, constant trap number
 *   OP_HINT                            DONE   WP8a nothing
 *   OP_READ_SYSTEM_REGISTER            TODO   later
 *   OP_WRITE_SYSTEM_REGISTER           TODO   later
 *   OP_READ_REGISTER_INDEXED           TODO   later
 *   OP_WRITE_REGISTER_INDEXED          TODO   later
 *   OP_HELPER                          TODO   later (runtime helper exit)
 *   OP_UNDEFINED                       TODO   never: refused by the legalizer
 *
 * Not covered inside the DONE rows: a guest register which lives in a target
 * register (SemanticRegisterMap::getTargetRegister() true) throws, widths
 * other than 1 (boolean), 8, 16, 32 and 64 bits throw (128 bits only in the
 * operations of AMD64LoweringWide.cpp: MOVE LOAD STORE ZERO_EXTEND TRUNCATE
 * BITCAST CONCAT EXTRACT INSERT BROADCAST AND OR XOR NOT AND_NOT), a conditional branch
 * to a register throws, a block with local labels throws in lower() (use
 * lowerBlock()).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include <stdio.h>
#include <string.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/sarray.h"
#include "xStl/exceptions.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
#include "AMD64BlockLowering.h"

// The enums and the class BlockLowering are in AMD64BlockLowering.h
using namespace amd64lowering;

namespace {

// The names of the scratch registers: 64, 32, 16 and 8 bits
const char* const gScratchNames[3][4] = {
    { "rax", "eax", "ax", "al" },
    { "rcx", "ecx", "cx", "cl" },
    { "rdx", "edx", "dx", "dl" }
};

// A pattern: the text of an IR operation, for the widths of the mask. The
// first pattern of an operation which has the width wins, see
// AMD64Lowering.h for the text form
struct AMD64Pattern {
    SemanticOperation::Operation m_operation;
    uint m_widths;
    const char* m_assembly;
};

const AMD64Pattern gPatterns[] = {
    // Arithmetic and logic: d = s0 <op> s1
    { SemanticOperation::OP_ADD, WIDTH_INTEGERS,
      "mov %a, $0\nadd %a, $1\nmov $d, %a" },
    { SemanticOperation::OP_SUBTRACT, WIDTH_INTEGERS,
      "mov %a, $0\nsub %a, $1\nmov $d, %a" },
    { SemanticOperation::OP_AND, WIDTH_ALL,
      "mov %a, $0\nand %a, $1\nmov $d, %a" },
    { SemanticOperation::OP_OR, WIDTH_ALL,
      "mov %a, $0\nor %a, $1\nmov $d, %a" },
    { SemanticOperation::OP_XOR, WIDTH_ALL,
      "mov %a, $0\nxor %a, $1\nmov $d, %a" },
    // d = ~s0 & s1
    { SemanticOperation::OP_AND_NOT, WIDTH_INTEGERS,
      "mov %a, $0\nnot %a\nand %a, $1\nmov $d, %a" },
    // d = ~s0 (a boolean flips its only bit)
    { SemanticOperation::OP_NOT, WIDTH_1,
      "mov %a, $0\nxor %a, 1\nmov $d, %a" },
    { SemanticOperation::OP_NOT, WIDTH_INTEGERS,
      "mov %a, $0\nnot %a\nmov $d, %a" },
    // d = -s0
    { SemanticOperation::OP_NEGATE, WIDTH_INTEGERS,
      "mov %a, $0\nneg %a\nmov $d, %a" },
    // d = the low half of s0 * s1 (the low byte of a product of the low
    // bytes is the product of the low bytes: imul has no 8 bit form, so the
    // 32 bit one is used)
    { SemanticOperation::OP_MULTIPLY, WIDTH_8,
      "mov %a, $0\nmov %c, $1\nimul %a32, %c32\nmov $d, %a" },
    { SemanticOperation::OP_MULTIPLY, WIDTH_16 | WIDTH_32 | WIDTH_64,
      "mov %a, $0\nmov %c, $1\nimul %a, %c\nmov $d, %a" },
    // d = the high half of s0 * s1
    { SemanticOperation::OP_MULTIPLY_HIGH_UNSIGNED,
      WIDTH_16 | WIDTH_32 | WIDTH_64,
      "mov %a, $0\nmov %c, $1\nmul %c\nmov $d, %x" },
    { SemanticOperation::OP_MULTIPLY_HIGH_SIGNED,
      WIDTH_16 | WIDTH_32 | WIDTH_64,
      "mov %a, $0\nmov %c, $1\nimul %c\nmov $d, %x" }
};

// The condition code (the cc of jcc, setcc) of an IR condition, NULL if the
// condition has no integer compare
const char* getConditionCode(SemanticOperation::Condition condition)
{
    switch (condition)
    {
    case SemanticOperation::COND_EQUAL:                 return "e";
    case SemanticOperation::COND_NOT_EQUAL:             return "ne";
    case SemanticOperation::COND_SIGNED_LESS:           return "l";
    case SemanticOperation::COND_SIGNED_LESS_EQUAL:     return "le";
    case SemanticOperation::COND_SIGNED_GREATER:        return "g";
    case SemanticOperation::COND_SIGNED_GREATER_EQUAL:  return "ge";
    case SemanticOperation::COND_UNSIGNED_LESS:         return "b";
    case SemanticOperation::COND_UNSIGNED_LESS_EQUAL:   return "be";
    case SemanticOperation::COND_UNSIGNED_GREATER:      return "a";
    case SemanticOperation::COND_UNSIGNED_GREATER_EQUAL:return "ae";
    default:                                            return NULL;
    }
}

// Return true if the value is a signed 32 bit number
bool fitsImmediate32(int64 value)
{
    return (value >= -(int64)0x80000000LL) && (value <= (int64)0x7FFFFFFFLL);
}

// Return the number as the assembler reads it: decimal when it is a signed
// 32 bit number, hexadecimal otherwise (the assembler chooses the shortest
// 'mov r, imm' for the 64 bit value)
cString formatNumber(int64 value)
{
    char text[40];
    if (fitsImmediate32(value))
        snprintf(text, sizeof(text), "%lld", (long long)value);
    else
        snprintf(text, sizeof(text), "0x%llx", (unsigned long long)value);
    return cString(text);
}

// Return the operand with another type: the same register, temporary or
// constant read in another size (the low bytes of a slot)
SemanticOperand retype(const SemanticOperand& operand, const SemanticType& type)
{
    switch (operand.getKind())
    {
    case SemanticOperand::KIND_REGISTER:
        return SemanticOperand::registerOperand(operand.getIndex(), type);
    case SemanticOperand::KIND_TEMPORARY:
        return SemanticOperand::temporary(operand.getIndex(), type);
    case SemanticOperand::KIND_IMMEDIATE:
        return SemanticOperand::immediate(operand.getValue(), type);
    case SemanticOperand::KIND_GUEST_ADDRESS:
        return SemanticOperand::guestAddress((uint64)operand.getValue(), type);
    default:
        return operand;
    }
}

// The stack of the first pass binary: the lowering doesn't use it, the
// assembler wants one
class NullStack : public StackInterface {
public:
    NullStack(FirstPassBinaryPtr& binary) :
        StackInterface(*binary, true, 4) {}
    virtual uint getStackType() { return 4; }
    virtual uint getRegisterSize() { return 4; }
    virtual RegisterAllocationTable& getRegistersTable() { return m_registers; }
    virtual const RegisterAllocationTable& getRegistersTable() const { return m_registers; }
    virtual StackLocation replaceRegisterToStackVariable(StackLocation source)
    {
        return source;
    }
private:
    RegisterAllocationTable m_registers;
};
} // namespace

BlockLowering::BlockLowering(const ArchitectureDescriptionPtr& target,
                             const SemanticBlock& block,
                             const SemanticRegisterMap& registerMap,
                             AssemblerInterface& assembler,
                  bool allowLocalLabels) :
    m_target(target),
    m_block(block),
    m_map(registerMap),
    m_assembler(assembler),
    m_allowLocalLabels(allowLocalLabels),
    m_index(0),
    m_operation(SemanticOperation::OP_NOP),
    m_typeBits(0)
{
    // The contract of the registers: 64 bit general purpose registers, not
    // the scratch registers or the stack pointer, not the same register
    const uint stateRegister = m_map.getStateRegister();
    const uint baseRegister = m_map.getMemoryBaseRegister();
    const uint registers[2] = { stateRegister, baseRegister };
    cString names[2];
    for (uint i = 0; i < 2; i++)
    {
        const uint id = registers[i];
        bool isValid = (id < m_target->getRegistersCount());
        if (isValid)
        {
            const ArchitectureRegister& reg = m_target->getRegister(id);
            isValid = (reg.m_file == ArchitectureRegister::FILE_GENERAL) &&
                      (reg.m_bits == 64) &&
                      (reg.m_parent == ArchitectureRegister::NO_PARENT) &&
                      (id != (uint)ia32dis::AMD64_REG_RAX) &&
                      (id != (uint)ia32dis::AMD64_REG_RCX) &&
                      (id != (uint)ia32dis::AMD64_REG_RDX) &&
                      (id != (uint)ia32dis::AMD64_REG_RSP);
            if (isValid)
                names[i] = cString(reg.m_name);
        }
        if (!isValid)
        {
            XSTL_THROW(cException,
                       cString("AMD64 lowering: the state and the memory base "
                               "registers must be 64 bit general purpose "
                               "registers other than rax, rcx, rdx and rsp").getBuffer(),
                       EXCEPTION_FAILED);
        }
    }
    if (stateRegister == baseRegister)
    {
        XSTL_THROW(cException,
                   cString("AMD64 lowering: the state and the memory base "
                           "registers are the same register").getBuffer(),
                   EXCEPTION_FAILED);
    }
    m_state = names[0];
    m_base = names[1];
}

void BlockLowering::fail(const char* reason) const
{
    failText(cString(reason));
}

void BlockLowering::failText(const cString& reason) const
{
    char text[200];
    snprintf(text, sizeof(text),
             "AMD64 lowering: operation %s<%u bits> at guest 0x%llx: ",
             SemanticOperation::getName(m_operation), m_typeBits,
             (unsigned long long)getGuestAddress(m_index));
    cString message(text);
    message += reason;
    XSTL_THROW(cException, message.getBuffer(), EXCEPTION_FAILED);
}

uint64 BlockLowering::getGuestAddress(uint index) const
{
    uint64 address = 0;
    const cList<SemanticBlock::GuestInstruction>& guests =
                                            m_block.getGuestInstructions();
    for (cList<SemanticBlock::GuestInstruction>::iterator i = guests.begin();
         i != guests.end(); i++)
    {
        if ((*i).m_firstInstruction <= index)
            address = (*i).m_address.getAddress();
    }
    return address;
}

uint64 BlockLowering::getFallThroughAddress() const
{
    if (!(m_block.getFallThroughAddress() == gNullPointerProcessorAddress))
        return m_block.getFallThroughAddress().getAddress();

    const cList<SemanticBlock::GuestInstruction>& guests =
                                            m_block.getGuestInstructions();
    if (guests.begin() == guests.end())
        fail("the block has no fall-through address and no guest instruction");
    uint64 address = 0;
    for (cList<SemanticBlock::GuestInstruction>::iterator i = guests.begin();
         i != guests.end(); i++)
    {
        address = (*i).m_address.getAddress() + (*i).m_size;
    }
    return address;
}

uint BlockLowering::getWidthMask(uint bits)
{
    switch (bits)
    {
    case 1:  return WIDTH_1;
    case 8:  return WIDTH_8;
    case 16: return WIDTH_16;
    case 32: return WIDTH_32;
    case 64: return WIDTH_64;
    case 128: return WIDTH_128;
    default: return 0;
    }
}

uint BlockLowering::getWidth(const SemanticType& type, uint widths) const
{
    const uint bits = type.getTotalBits();
    if ((type.getKind() != SemanticType::KIND_INTEGER) || type.isVector() ||
        type.isScalable() || ((getWidthMask(bits) & widths) == 0))
    {
        char text[100];
        snprintf(text, sizeof(text), "the type with %u bits (kind %u) is not "
                 "supported", bits, (uint)type.getKind());
        fail(text);
    }
    return bits;
}

const char* BlockLowering::getRegister(Scratch scratch, uint bits)
{
    switch (bits)
    {
    case 64: return gScratchNames[scratch][0];
    case 32: return gScratchNames[scratch][1];
    case 16: return gScratchNames[scratch][2];
    default: return gScratchNames[scratch][3];
    }
}

const char* BlockLowering::getSizeKeyword(uint bits)
{
    switch (bits)
    {
    case 64: return "qword";
    case 32: return "dword";
    case 16: return "word";
    default: return "byte";
    }
}

uint BlockLowering::getSlotOffset(const SemanticOperand& operand) const
{
    switch (operand.getKind())
    {
    case SemanticOperand::KIND_REGISTER:
        {
            uint targetRegister = 0;
            if (m_map.getTargetRegister(operand.getIndex(), targetRegister))
            {
                fail("a guest register which lives in a target register is "
                     "not implemented");
            }
            return m_map.getStateOffset(operand.getIndex());
        }
    case SemanticOperand::KIND_TEMPORARY:
        return m_map.getTemporaryOffset(operand.getIndex());
    default:
        fail("the operand is not a register or a temporary");
        return 0;
    }
}

cString BlockLowering::getMemory(const SemanticOperand& operand,
                                 uint bits) const
{
    char text[100];
    snprintf(text, sizeof(text), "%s [%s+0x%x]", getSizeKeyword(bits),
             m_state.getASCIIstring().getBuffer(), getSlotOffset(operand));
    return cString(text);
}

uint64 BlockLowering::getConstant(const SemanticOperand& operand,
                                  uint bits) const
{
    uint64 value = (uint64)operand.getValue();
    if (bits == 1)
        return value & 1;
    if (bits < 64)
        return value & (((uint64)1 << bits) - 1);
    return value;
}

cString BlockLowering::getImmediate(const SemanticOperand& operand,
                                    uint bits) const
{
    uint64 value = getConstant(operand, bits);
    int64 signedValue = (int64)value;
    if (bits == 8)
        signedValue = (int64)(int8)value;
    else if (bits == 16)
        signedValue = (int64)(int16)value;
    else if (bits == 32)
        signedValue = (int64)(int32)value;
    return formatNumber(signedValue);
}

bool BlockLowering::isImmediate(const SemanticOperand& operand,
                                uint bits) const
{
    return (bits < 64) || fitsImmediate32((int64)getConstant(operand, bits));
}

void BlockLowering::line(const cString& text)
{
    m_pending += text;
    m_pending += cString("\n");
}

void BlockLowering::line(const char* text)
{
    line(cString(text));
}

void BlockLowering::flush()
{
    if (m_pending.length() == 0)
        return;
    cString text = m_pending;
    m_pending = cString();

    cStringerStreamPtr stream = m_assembler.getAssembler();
    *stream << text;
    // The stream assembles when it's destroyed, where an error is swallowed:
    // assemble now to see it
    IA32AssemblerStream* assembly =
                        dynamic_cast<IA32AssemblerStream*>(stream.getPointer());
    if (assembly == NULL)
        fail("the assembler is not the IA-32 assembler");
    XSTL_TRY
    {
        assembly->assemble();
    }
    XSTL_CATCH(cException& e)
    {
        cString message("the assembler refused [");
        message += text;
        message += cString("]: ");
        message += cString(e.getMessage());
        failText(message);
    }
}

void BlockLowering::load(Scratch scratch, const SemanticOperand& operand,
                         Extension extension)
{
    char text[160];
    const uint bits = getWidth(operand.getType(), WIDTH_ALL);
    const char* reg64 = getRegister(scratch, 64);
    const char* reg32 = getRegister(scratch, 32);

    if (isConstant(operand))
    {
        uint64 value = getConstant(operand, bits);
        if (extension == EXTEND_SIGN)
        {
            if (bits == 1)
                value = (value != 0) ? ~(uint64)0 : 0;
            else if (bits == 8)
                value = (uint64)(int64)(int8)value;
            else if (bits == 16)
                value = (uint64)(int64)(int16)value;
            else if (bits == 32)
                value = (uint64)(int64)(int32)value;
        }
        // A value below 2^32 is written in hexadecimal: the assembler then
        // writes the 32 bit move, which zeroes the upper half
        cString number;
        if (!fitsImmediate32((int64)value) && (value <= 0xFFFFFFFFULL))
        {
            snprintf(text, sizeof(text), "0x%llx", (unsigned long long)value);
            number = cString(text);
        } else
        {
            number = formatNumber((int64)value);
        }
        snprintf(text, sizeof(text), "mov %s, %s", reg64,
                 number.getASCIIstring().getBuffer());
        line(text);
        return;
    }

    const cString memory = getMemory(operand, (bits == 1) ? 8 : bits);
    const cSArray<char> memoryAscii = memory.getASCIIstring();
    const char* memoryText = memoryAscii.getBuffer();
    if (bits <= 8)
    {
        if ((extension == EXTEND_SIGN) && (bits == 8))
            snprintf(text, sizeof(text), "movsx %s, %s", reg64, memoryText);
        else
            snprintf(text, sizeof(text), "movzx %s, %s", reg32, memoryText);
        line(text);
        if ((extension == EXTEND_SIGN) && (bits == 1))
        {
            // The boolean 1 is -1 when it is a signed 1 bit number
            snprintf(text, sizeof(text), "neg %s", reg64);
            line(text);
        }
    } else if (bits == 16)
    {
        if (extension == EXTEND_SIGN)
            snprintf(text, sizeof(text), "movsx %s, %s", reg64, memoryText);
        else
            snprintf(text, sizeof(text), "movzx %s, %s", reg32, memoryText);
        line(text);
    } else if (bits == 32)
    {
        if (extension == EXTEND_SIGN)
            snprintf(text, sizeof(text), "movsxd %s, %s", reg64, memoryText);
        else
            snprintf(text, sizeof(text), "mov %s, %s", reg32, memoryText);
        line(text);
    } else
    {
        snprintf(text, sizeof(text), "mov %s, %s", reg64, memoryText);
        line(text);
    }
}

void BlockLowering::store(const SemanticOperand& destination, Scratch scratch)
{
    const uint bits = getWidth(destination.getType(), WIDTH_ALL);
    const uint storeBits = (bits == 1) ? 8 : bits;
    cString text("mov ");
    text += getMemory(destination, storeBits);
    text += cString(", ");
    text += cString(getRegister(scratch, storeBits));
    line(text);
}

void BlockLowering::expand(const char* text, uint bits,
                           const SemanticOperand& destination,
                           const SemanticOperand* sources)
{
    // The line is built in 'current'. A constant which has to be in a
    // register is loaded into rcx (rdx for the second) on a line before
    cString current;
    uint preloads = 0;
    bool isMove = false;
    const uint registerBits = (bits == 1) ? 8 : bits;

    for (const char* c = text;; c++)
    {
        if ((*c == '\n') || (*c == 0))
        {
            line(current);
            current = cString();
            isMove = false;
            preloads = 0;
            if (*c == 0)
                break;
            continue;
        }
        if (current.length() == 0)
        {
            // "mov %": a move into a register, which has a 64 bit immediate
            isMove = (strncmp(c, "mov %", 5) == 0);
        }
        if (*c == '$')
        {
            c++;
            if (*c == 'd')
            {
                const uint destinationBits = getWidth(destination.getType(),
                                                      WIDTH_ALL);
                current += getMemory(destination,
                                (destinationBits == 1) ? 8 : destinationBits);
            } else
            {
                const SemanticOperand& source = sources[*c - '0'];
                const SemanticOperand narrow =
                                    retype(source, SemanticType::integer(bits));
                if (!isConstant(narrow))
                {
                    current += getMemory(narrow, registerBits);
                } else if (isImmediate(narrow, bits) || isMove)
                {
                    current += (bits == 64) ?
                        formatNumber((int64)getConstant(narrow, bits)) :
                        getImmediate(narrow, bits);
                } else
                {
                    const Scratch preload = (preloads++ == 0) ? SCRATCH_C :
                                                                SCRATCH_D;
                    char move[80];
                    snprintf(move, sizeof(move), "mov %s, 0x%llx",
                             getRegister(preload, 64),
                             (unsigned long long)getConstant(narrow, bits));
                    line(move);
                    current += cString(getRegister(preload, 64));
                }
            }
        } else if (*c == '%')
        {
            c++;
            Scratch scratch = SCRATCH_A;
            if (*c == 'c')
                scratch = SCRATCH_C;
            else if (*c == 'x')
                scratch = SCRATCH_D;
            uint size = registerBits;
            if ((c[1] == '3') && (c[2] == '2'))
            {
                size = 32;
                c += 2;
            } else if ((c[1] == '6') && (c[2] == '4'))
            {
                size = 64;
                c += 2;
            }
            current += cString(getRegister(scratch, size));
        } else
        {
            current += cString(*c);
        }
    }
}

bool BlockLowering::applyPattern(const SemanticInstruction& instruction)
{
    const uint count = sizeof(gPatterns) / sizeof(gPatterns[0]);
    const SemanticOperation::Operation operation = instruction.getOperation();
    bool isKnown = false;
    for (uint i = 0; i < count; i++)
    {
        if (gPatterns[i].m_operation != operation)
            continue;
        isKnown = true;
        // The width of the operation is the width of its destination
        const SemanticType& type = instruction.getDestination().getType();
        const uint bits = getWidth(type, WIDTH_ALL);
        if ((getWidthMask(bits) & gPatterns[i].m_widths) == 0)
            continue;
        SemanticOperand sources[SemanticInstruction::MAX_SOURCES];
        for (uint s = 0; s < SemanticInstruction::MAX_SOURCES; s++)
            sources[s] = instruction.getSource(s);
        expand(gPatterns[i].m_assembly, bits, instruction.getDestination(),
               sources);
        return true;
    }
    if (isKnown)
        fail("this width is not implemented");
    return false;
}

void BlockLowering::lowerMove(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_ALL);
    const uint storeBits = (bits == 1) ? 8 : bits;
    const SemanticOperand source =
                        retype(instruction.getSource(0), destination.getType());
    if (isConstant(source) && isImmediate(source, bits))
    {
        // (the constant of a boolean is 0 or 1)
        cString text("mov ");
        text += getMemory(destination, storeBits);
        text += cString(", ");
        text += getImmediate(source, bits);
        line(text);
        return;
    }
    load(SCRATCH_A, source, EXTEND_ZERO);
    // A wider value moved into a boolean keeps only its low bit
    if ((bits == 1) && (instruction.getSource(0).getType().getTotalBits() != 1))
        line("and eax, 1");
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerExtend(const SemanticInstruction& instruction,
                                Extension extension)
{
    const SemanticOperand& source = instruction.getSource(0);
    const uint sourceBits = getWidth(source.getType(), WIDTH_ALL);
    const uint destinationBits = getWidth(instruction.getDestination().getType(),
                                          WIDTH_INTEGERS);
    if (destinationBits < sourceBits)
        fail("the destination is narrower than the source");
    load(SCRATCH_A, source, extension);
    store(instruction.getDestination(), SCRATCH_A);
}

void BlockLowering::lowerTruncate(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_ALL);
    const uint sourceBits = getWidth(instruction.getSource(0).getType(),
                                     WIDTH_ALL);
    if (bits > sourceBits)
        fail("the destination is wider than the source");
    // The low bits of the value are at the start of its slot
    load(SCRATCH_A, retype(instruction.getSource(0), destination.getType()),
         EXTEND_ZERO);
    if (bits == 1)
        line("and eax, 1");
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerLoad(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    // The guest address of the access
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    char text[100];
    snprintf(text, sizeof(text), "mov %s, %s [%s+rax]", getRegister(SCRATCH_A, bits),
             getSizeKeyword(bits), m_base.getASCIIstring().getBuffer());
    line(text);
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerStore(const SemanticInstruction& instruction)
{
    // The size of the access is the type of the instruction
    const uint bits = getWidth(instruction.getType(), WIDTH_INTEGERS);
    const SemanticOperand value =
                    retype(instruction.getSource(1), instruction.getType());
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    char text[160];
    if (isConstant(value) && isImmediate(value, bits))
    {
        snprintf(text, sizeof(text), "mov %s [%s+rax], %s", getSizeKeyword(bits),
                 m_base.getASCIIstring().getBuffer(),
                 getImmediate(value, bits).getASCIIstring().getBuffer());
    } else
    {
        load(SCRATCH_C, value, EXTEND_ZERO);
        snprintf(text, sizeof(text), "mov %s [%s+rax], %s", getSizeKeyword(bits),
                 m_base.getASCIIstring().getBuffer(), getRegister(SCRATCH_C, bits));
    }
    line(text);
}

void BlockLowering::lowerFence(const SemanticInstruction& instruction)
{
    // The target is total store order: only a store followed by a load may
    // be reordered, which takes a full fence
    const uint access = instruction.getFenceAccess();
    const bool isStoreLoad = ((access & SemanticOperation::FENCE_BEFORE_STORES) != 0) &&
                             ((access & SemanticOperation::FENCE_AFTER_LOADS) != 0);
    if (isStoreLoad ||
        (instruction.getMemoryOrder() == SemanticOperation::ORDER_SEQUENTIAL))
    {
        line("mfence");
    }
}

void BlockLowering::lowerShift(const SemanticInstruction& instruction)
{
    // The count of a shift which is the width or more gives 0 (the sign for
    // an arithmetic shift), where the processor takes the count modulo 32 or
    // 64: the count is clamped
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    const SemanticOperation::Operation operation = instruction.getOperation();
    const char* mnemonic = "shl";
    if (operation == SemanticOperation::OP_SHIFT_RIGHT_LOGICAL)
        mnemonic = "shr";
    else if (operation == SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC)
        mnemonic = "sar";
    const bool isArithmetic = (operation ==
                               SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC);
    const SemanticOperand value =
                    retype(instruction.getSource(0), destination.getType());
    const SemanticOperand& count = instruction.getSource(1);
    const char* reg = getRegister(SCRATCH_A, bits);
    char text[100];

    load(SCRATCH_A, value, EXTEND_ZERO);
    if (isConstant(count))
    {
        const uint64 amount = getConstant(count,
                                getWidth(count.getType(), WIDTH_ALL));
        if (amount >= bits)
        {
            if (isArithmetic)
                snprintf(text, sizeof(text), "sar %s, %u", reg, bits - 1);
            else
                snprintf(text, sizeof(text), "xor eax, eax");
            line(text);
        } else if (amount != 0)
        {
            snprintf(text, sizeof(text), "%s %s, %u", mnemonic, reg,
                     (uint)amount);
            line(text);
        }
    } else
    {
        // The count as an unsigned number in rcx
        load(SCRATCH_C, count, EXTEND_ZERO);
        if (isArithmetic)
        {
            snprintf(text, sizeof(text), "cmp rcx, %u", bits);
            line(text);
            snprintf(text, sizeof(text), "mov edx, %u", bits - 1);
            line(text);
            line("cmovae rcx, rdx");
            snprintf(text, sizeof(text), "sar %s, cl", reg);
            line(text);
        } else
        {
            snprintf(text, sizeof(text), "%s %s, cl", mnemonic, reg);
            line(text);
            line("xor edx, edx");
            snprintf(text, sizeof(text), "cmp rcx, %u", bits);
            line(text);
            line("cmovae rax, rdx");
        }
    }
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerRotate(const SemanticInstruction& instruction)
{
    // The count is taken modulo the width, like the processor does
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWidth(destination.getType(), WIDTH_INTEGERS);
    const char* mnemonic =
        (instruction.getOperation() == SemanticOperation::OP_ROTATE_LEFT) ?
                                                                "rol" : "ror";
    const SemanticOperand& count = instruction.getSource(1);
    const char* reg = getRegister(SCRATCH_A, bits);
    char text[100];

    load(SCRATCH_A, retype(instruction.getSource(0), destination.getType()),
         EXTEND_ZERO);
    if (isConstant(count))
    {
        const uint amount = (uint)(getConstant(count,
                            getWidth(count.getType(), WIDTH_ALL)) % bits);
        if (amount != 0)
        {
            snprintf(text, sizeof(text), "%s %s, %u", mnemonic, reg, amount);
            line(text);
        }
    } else
    {
        load(SCRATCH_C, count, EXTEND_ZERO);
        snprintf(text, sizeof(text), "%s %s, cl", mnemonic, reg);
        line(text);
    }
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerCompare(const SemanticInstruction& instruction)
{
    const char* code = getConditionCode(instruction.getCondition());
    const SemanticOperand& destination = instruction.getDestination();
    const uint destinationBits = getWidth(destination.getType(), WIDTH_ALL);
    if (code == NULL)
    {
        const SemanticOperation::Condition condition = instruction.getCondition();
        if ((condition != SemanticOperation::COND_ALWAYS) &&
            (condition != SemanticOperation::COND_NEVER))
        {
            fail("this condition is not an integer compare");
        }
        // A constant result
        cString text("mov ");
        text += getMemory(destination, (destinationBits == 1) ? 8 : destinationBits);
        text += cString((condition == SemanticOperation::COND_ALWAYS) ? ", 1" : ", 0");
        line(text);
        return;
    }
    // The compare is as wide as its operands, not as its boolean result
    const SemanticOperand& s0 = instruction.getSource(0);
    const SemanticOperand& s1 = instruction.getSource(1);
    const SemanticOperand& reference = (!isConstant(s0) || isConstant(s1)) ?
                                                                    s0 : s1;
    const uint bits = getWidth(reference.getType(), WIDTH_ALL);
    SemanticOperand sources[SemanticInstruction::MAX_SOURCES];
    sources[0] = s0;
    sources[1] = s1;

    char text[100];
    snprintf(text, sizeof(text), "mov %%a, $0\ncmp %%a, $1\nset%s al", code);
    expand(text, bits, destination, sources);
    if (destinationBits > 8)
        line("movzx eax, al");
    store(destination, SCRATCH_A);
}

void BlockLowering::lowerSelect(const SemanticInstruction& instruction)
{
    const SemanticOperand& destination = instruction.getDestination();
    // A scalar is selected as its bits: an integer, or a float which is not
    // f32/f64 (half precision, see AMD64LoweringFloat.cpp). A vector select
    // takes one condition bit per lane, which the single byte tested here
    // doesn't give, so it is refused
    if (destination.getType().isVector() || destination.getType().isScalable())
        fail("a vector select is not implemented");
    const uint destinationBits = destination.getType().getTotalBits();
    const SemanticType bitType = SemanticType::integer(destinationBits);
    // (a boolean is one byte in the state: its compare writes the byte and
    // every reader tests the byte, so the other bytes of its slot are never read)
    const uint storedBits = (destinationBits == 1) ? 8 : destinationBits;
    const SemanticOperand& condition = instruction.getSource(0);
    const SemanticOperand ifTrue = retype(instruction.getSource(1), bitType);
    const SemanticOperand ifFalse = retype(instruction.getSource(2), bitType);
    if (isConstant(condition))
    {
        load(SCRATCH_A, (getConstant(condition, 1) != 0) ? ifTrue : ifFalse,
             EXTEND_ZERO);
        storeBits(destination, SCRATCH_A, storedBits);
        return;
    }
    // (the conditional move has no 8 bit form: the values are zero extended
    // to 64 bits, and only the low bits are stored)
    load(SCRATCH_A, ifFalse, EXTEND_ZERO);
    load(SCRATCH_C, ifTrue, EXTEND_ZERO);
    cString text("cmp ");
    text += getMemory(retype(condition, SemanticType::integer(1)), 8);
    text += cString(", 0");
    line(text);
    line("cmovne rax, rcx");
    storeBits(destination, SCRATCH_A, storedBits);
}

cString BlockLowering::getExitReasonStore(uint64 reason) const
{
    char text[120];
    snprintf(text, sizeof(text), "mov qword [%s+0x%x], 0x%llx",
             m_state.getASCIIstring().getBuffer(), m_map.getExitReasonOffset(),
             (unsigned long long)reason);
    return cString(text);
}

void BlockLowering::jumpToGuest(const char* mnemonic, uint64 address)
{
    cString text(mnemonic);
    text += cString(" ");
    text += AMD64Lowering::getBlockLabel(address);
    line(text);
}

void BlockLowering::writeExit(const SemanticOperand& target, uint64 reason)
{
    char text[120];
    load(SCRATCH_A, target, EXTEND_ZERO);
    snprintf(text, sizeof(text), "mov qword [%s+0x%x], rax",
             m_state.getASCIIstring().getBuffer(), m_map.getProgramCounterOffset());
    line(text);
    line(getExitReasonStore(reason));
    cString jump("jmp ");
    jump += AMD64Lowering::getExitLabel();
    line(jump);
}

void BlockLowering::lowerBranch(const SemanticInstruction& instruction)
{
    const SemanticOperand& target = instruction.getSource(0);
    const SemanticOperation::Operation operation = instruction.getOperation();
    if (target.getKind() == SemanticOperand::KIND_LABEL)
    {
        if (operation != SemanticOperation::OP_BRANCH)
            fail("only a branch can go to a label of the block");
        cString jump("jmp ");
        jump += getLocalLabel(target);
        line(jump);
        return;
    }
    if (isConstant(target))
    {
        // A constant target is a plain jump (a call writes its return
        // address to the link register in the IR)
        jumpToGuest("jmp", (uint64)target.getValue());
        return;
    }
    uint64 reason = AMD64Lowering::EXIT_BRANCH;
    if (operation == SemanticOperation::OP_CALL)
        reason = AMD64Lowering::EXIT_CALL;
    else if (operation == SemanticOperation::OP_RETURN)
        reason = AMD64Lowering::EXIT_RETURN;
    writeExit(target, reason);
}

void BlockLowering::lowerBranchConditional(const SemanticInstruction& instruction)
{
    const SemanticOperand& condition = instruction.getSource(0);
    const SemanticOperand& target = instruction.getSource(1);
    if (target.getKind() == SemanticOperand::KIND_LABEL)
    {
        const cString label = getLocalLabel(target);
        if (isConstant(condition))
        {
            if (getConstant(condition, 1) != 0)
                line(cString("jmp ") + label);
            return;
        }
        cString compare("cmp ");
        compare += getMemory(retype(condition, SemanticType::integer(1)), 8);
        compare += cString(", 0");
        line(compare);
        line(cString("jne ") + label);
        return;
    }
    if (!isConstant(target))
        fail("a conditional branch to a register is not implemented");
    if (isConstant(condition))
    {
        if (getConstant(condition, 1) != 0)
            jumpToGuest("jmp", (uint64)target.getValue());
        return;
    }
    cString compare("cmp ");
    compare += getMemory(retype(condition, SemanticType::integer(1)), 8);
    compare += cString(", 0");
    line(compare);
    jumpToGuest("jne", (uint64)target.getValue());
}

void BlockLowering::lowerTrap(const SemanticInstruction& instruction)
{
    const SemanticOperand& number = instruction.getSource(0);
    if (!isConstant(number))
        fail("the trap number is not a constant");
    // The program counter of a trap is the trapping instruction
    char text[120];
    snprintf(text, sizeof(text), "mov rax, 0x%llx",
             (unsigned long long)getGuestAddress(m_index));
    line(text);
    snprintf(text, sizeof(text), "mov qword [%s+0x%x], rax",
             m_state.getASCIIstring().getBuffer(), m_map.getProgramCounterOffset());
    line(text);
    const uint64 reason = (uint64)AMD64Lowering::EXIT_TRAP |
        ((uint64)instruction.getKind() << AMD64Lowering::EXIT_TRAP_KIND_SHIFT) |
        ((uint64)number.getValue() << AMD64Lowering::EXIT_TRAP_NUMBER_SHIFT);
    line(getExitReasonStore(reason));
    cString jump("jmp ");
    jump += AMD64Lowering::getExitLabel();
    line(jump);
}

uint BlockLowering::getOffset() const
{
    return (uint)m_assembler.getFirstPass()->getCurrentBlockData().getSize();
}

cString BlockLowering::getLocalLabel(const SemanticOperand& label) const
{
    if (!m_allowLocalLabels)
        fail("a block with labels must be lowered with lowerBlock()");
    char text[40];
    snprintf(text, sizeof(text), "L_%u", label.getIndex());
    return cString(text);
}

void BlockLowering::lowerLabel(const SemanticInstruction& instruction)
{
    AMD64Lowering::LabelDefinition definition;
    definition.m_name = getLocalLabel(instruction.getSource(0));
    definition.m_offset = getOffset();
    m_localLabels.append(definition);
}

void BlockLowering::lower()
{
    const cList<SemanticInstruction>& instructions = m_block.getInstructions();
    bool endsTheBlock = false;
    m_index = 0;
    for (cList<SemanticInstruction>::iterator i = instructions.begin();
         i != instructions.end(); i++, m_index++)
    {
        const SemanticInstruction& instruction = *i;
        // The code of a guest instruction starts here
        {
            const cList<SemanticBlock::GuestInstruction>& guests =
                                            m_block.getGuestInstructions();
            for (cList<SemanticBlock::GuestInstruction>::iterator g =
                    guests.begin(); g != guests.end(); g++)
            {
                if ((*g).m_firstInstruction == m_index)
                {
                    AMD64Lowering::LabelDefinition definition;
                    definition.m_name = AMD64Lowering::getBlockLabel(
                                                (*g).m_address.getAddress());
                    definition.m_offset = getOffset();
                    m_guestLabels.append(definition);
                }
            }
        }
        m_operation = instruction.getOperation();
        m_typeBits = instruction.getType().getTotalBits();
        if (m_operation != SemanticOperation::OP_NOP)
            endsTheBlock = false;

        // The wide values and the bit moves first: they own the 128 bit
        // moves (AMD64LoweringWide.cpp), before the narrow cases below
        if (lowerWide(instruction))
        {
            flush();
            continue;
        }

        switch (m_operation)
        {
        case SemanticOperation::OP_NOP:
        case SemanticOperation::OP_HINT:
            break;
        case SemanticOperation::OP_MOVE:
            lowerMove(instruction);
            break;
        case SemanticOperation::OP_ZERO_EXTEND:
            lowerExtend(instruction, EXTEND_ZERO);
            break;
        case SemanticOperation::OP_SIGN_EXTEND:
            lowerExtend(instruction, EXTEND_SIGN);
            break;
        case SemanticOperation::OP_TRUNCATE:
            lowerTruncate(instruction);
            break;
        case SemanticOperation::OP_LOAD:
            lowerLoad(instruction);
            break;
        case SemanticOperation::OP_STORE:
            lowerStore(instruction);
            break;
        case SemanticOperation::OP_FENCE:
            lowerFence(instruction);
            break;
        case SemanticOperation::OP_SHIFT_LEFT:
        case SemanticOperation::OP_SHIFT_RIGHT_LOGICAL:
        case SemanticOperation::OP_SHIFT_RIGHT_ARITHMETIC:
            lowerShift(instruction);
            break;
        case SemanticOperation::OP_ROTATE_LEFT:
        case SemanticOperation::OP_ROTATE_RIGHT:
            lowerRotate(instruction);
            break;
        case SemanticOperation::OP_COMPARE:
            // (a compare of floats is in AMD64LoweringFloat.cpp)
            if (!lowerFloat(instruction))
                lowerCompare(instruction);
            break;
        case SemanticOperation::OP_SELECT:
            // (a select of floats is in AMD64LoweringFloat.cpp)
            if (!lowerFloat(instruction))
                lowerSelect(instruction);
            break;
        case SemanticOperation::OP_BRANCH:
        case SemanticOperation::OP_CALL:
        case SemanticOperation::OP_RETURN:
        case SemanticOperation::OP_BRANCH_INDIRECT:
            lowerBranch(instruction);
            endsTheBlock = (instruction.getSource(0).getKind() !=
                            SemanticOperand::KIND_LABEL);
            break;
        case SemanticOperation::OP_BRANCH_CONDITIONAL:
            lowerBranchConditional(instruction);
            break;
        case SemanticOperation::OP_TRAP:
            lowerTrap(instruction);
            endsTheBlock = true;
            break;
        case SemanticOperation::OP_LABEL:
            lowerLabel(instruction);
            break;
        case SemanticOperation::OP_ATOMIC_EXCHANGE:
        case SemanticOperation::OP_ATOMIC_COMPARE_EXCHANGE:
        case SemanticOperation::OP_ATOMIC_RMW:
            // (AMD64LoweringAtomic.cpp)
            lowerAtomic(instruction);
            break;
        default:
            // The groups of operations of the other files, then the patterns
            if (!lowerIntegerExtra(instruction) &&
                !lowerFloat(instruction) && !applyPattern(instruction))
            {
                fail("the operation is not implemented");
            }
            break;
        }
        flush();
    }

    // The guest instructions which produced no IR at the end of the block
    {
        const cList<SemanticBlock::GuestInstruction>& guests =
                                        m_block.getGuestInstructions();
        for (cList<SemanticBlock::GuestInstruction>::iterator g =
                guests.begin(); g != guests.end(); g++)
        {
            if ((*g).m_firstInstruction == m_index)
            {
                AMD64Lowering::LabelDefinition definition;
                definition.m_name = AMD64Lowering::getBlockLabel(
                                            (*g).m_address.getAddress());
                definition.m_offset = getOffset();
                m_guestLabels.append(definition);
            }
        }
    }

    // The block continues in the next guest instruction
    if (!endsTheBlock)
    {
        m_operation = SemanticOperation::OP_NOP;
        m_typeBits = 0;
        jumpToGuest("jmp", getFallThroughAddress());
        flush();
    }
}

AMD64Lowering::AMD64Lowering() :
    m_target(ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64))
{
}

ArchitectureDescriptionPtr AMD64Lowering::getTarget() const
{
    return m_target;
}

void AMD64Lowering::lower(const SemanticBlock& block,
                          const SemanticRegisterMap& registerMap,
                          AssemblerInterface& assembler)
{
    BlockLowering lowering(m_target, block, registerMap, assembler, false);
    lowering.lower();
}

void AMD64Lowering::lowerBlock(const SemanticBlock& block,
                               const SemanticRegisterMap& registerMap,
                               LoweredBlock& result)
{
    FirstPassBinaryPtr binary(new FirstPassBinary(
                            OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
    binary->createNewBlockWithoutChange(0,
                                StackInterfacePtr(new NullStack(binary)));
    binary->changeBasicBlock(0);
    AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                            binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);

    BlockLowering lowering(m_target, block, registerMap, *assembler, true);
    lowering.lower();

    const cBuffer& data = binary->getCurrentBlockData();
    LoweredBlock lowered;
    lowered.m_code = cBuffer(data.getSize());
    for (uint i = 0; i < data.getSize(); i++)
        lowered.m_code[i] = data[i];
    lowered.m_labels = lowering.getGuestLabels();

    const BinaryDependencies::DependencyObjectList& list =
                                    binary->getCurrentDependecies().getList();
    for (BinaryDependencies::DependencyObjectList::iterator i = list.begin();
         i != list.end(); i++)
    {
        const BinaryDependencies::DependencyObject& dependency = *i;
        if ((dependency.m_type != BinaryDependencies::DEP_RELATIVE) ||
            (dependency.m_length != (uint)BinaryDependencies::DEP_32BIT))
        {
            XSTL_THROW(cException,
                       cString("AMD64 lowering: the assembler wrote a "
                               "dependency which is not a rel32").getBuffer(),
                       EXCEPTION_FAILED);
        }
        if (dependency.m_name.length() > 2 &&
            dependency.m_name.left(2) == cString("L_"))
        {
            // A label of the block
            bool isFound = false;
            const cList<LabelDefinition>& locals = lowering.getLocalLabels();
            for (cList<LabelDefinition>::iterator l = locals.begin();
                 l != locals.end(); l++)
            {
                if ((*l).m_name == dependency.m_name)
                {
                    patchRelative32(lowered.m_code, dependency.m_position,
                        (int64)(*l).m_offset -
                        (int64)(dependency.m_position + 4));
                    isFound = true;
                }
            }
            if (!isFound)
            {
                XSTL_THROW(cException,
                           cString("AMD64 lowering: a branch to a label which "
                                   "the block doesn't define").getBuffer(),
                           EXCEPTION_FAILED);
            }
            continue;
        }
        LabelDependency out;
        out.m_name = dependency.m_name;
        out.m_offset = dependency.m_position;
        out.m_size = dependency.m_length;
        out.m_kind = dependency.m_type;
        lowered.m_dependencies.append(out);
    }
    result = lowered;
}

void AMD64Lowering::patchRelative32(cBuffer& code, uint fieldOffset,
                                    int64 relative)
{
    if ((fieldOffset + 4 > code.getSize()) ||
        (relative < -(int64)0x80000000LL) || (relative > (int64)0x7FFFFFFFLL))
    {
        XSTL_THROW(cException,
                   cString("AMD64 lowering: the relative field is out of the "
                           "code or the distance doesn't fit 32 bits").getBuffer(),
                   EXCEPTION_FAILED);
    }
    for (uint i = 0; i < 4; i++)
        code[fieldOffset + i] = (uint8)((uint64)relative >> (8 * i));
}

cString AMD64Lowering::getBlockLabel(uint64 guestAddress)
{
    char text[40];
    snprintf(text, sizeof(text), "B_%llx", (unsigned long long)guestAddress);
    return cString(text);
}

cString AMD64Lowering::getExitLabel()
{
    return cString("__reforge_exit");
}
