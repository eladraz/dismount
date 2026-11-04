#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICOPERATION_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICOPERATION_H

/*
 * SemanticOperation.h
 *
 * The operations of the semantic IR, the conditions, the flags recipes and the
 * attributes of the operations.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"

/*
 * The operations of the semantic IR.
 *
 * An operation works on all the sizes and on vectors: for a vector type the
 * operation is done on each lane. The operand order is the C order:
 *   OP_SUBTRACT     d = s0 - s1
 *   OP_SHIFT_LEFT   d = s0 << s1
 *   OP_SELECT       d = s0 ? s1 : s2
 *
 * The integer operations wrap around (modulo 2^bits). Where processors
 * disagree on an edge case (a division by zero, a float to integer overflow,
 * an out of range shuffle index) the behavior of the guest is a part of the
 * instruction, see Behavior.
 *
 * The reference semantics of every operation is SemanticInterpreter. The
 * operations which are documented as "not interpreted yet" are reserved for
 * the processors which aren't lifted yet.
 */
class SemanticOperation {
public:
    // The operations
    enum Operation {
        // No operation
        OP_NOP,

        //////////////////////////////////////////////////////////////////
        // Data movement
        //////////////////////////////////////////////////////////////////

        // d = s0
        OP_MOVE,
        // d = zero extend s0 into the size of d
        OP_ZERO_EXTEND,
        // d = sign extend s0 into the size of d
        OP_SIGN_EXTEND,
        // d = the low bits of s0 (the size of d)
        OP_TRUNCATE,
        // d = the bits of s0 starting at bit s1 (the size of d). For a vector
        // s0 and a scalar d, s1 is the lane index
        OP_EXTRACT,
        // d = s0 with s1 written at bit s2 (a lane index for vectors)
        OP_INSERT,
        // d = s1:s0 (s0 is the low part)
        OP_CONCAT,
        // d = the bits of s0 as the type of d (same total size)
        OP_BITCAST,

        //////////////////////////////////////////////////////////////////
        // Memory
        //////////////////////////////////////////////////////////////////

        // d = memory[s0]. The type of d is the size of the access
        OP_LOAD,
        // memory[s0] = s1
        OP_STORE,
        // d = memory[s0] for the lanes of the mask s1, 0 for the others. The
        // inactive lanes don't fault (vmaskmov, AVX-512, SVE, RVV v0.t).
        // Not interpreted yet
        OP_LOAD_MASKED,
        // memory[s0] = s1 for the lanes of the mask s2. Not interpreted yet
        OP_STORE_MASKED,
        // d[i] = memory[s0 + s1[i]] for the lanes of the mask s2, 0 for the
        // others. s1 is a vector of byte offsets. Not interpreted yet
        OP_GATHER,
        // memory[s0 + s1[i]] = s2[i] for all the lanes. Masked scatters are
        // a scatter of a OP_COMPRESS. Not interpreted yet
        OP_SCATTER,
        // d = the structures at memory[s0] de-interleaved: s1 (an immediate)
        // structure members, d holds the members one after the other (ARM
        // LD2-LD4, RVV vlseg). Not interpreted yet
        OP_LOAD_INTERLEAVED,
        // memory[s0] = s1 interleaved into structures of s2 members (ARM
        // ST2-ST4, RVV vsseg). Not interpreted yet
        OP_STORE_INTERLEAVED,
        // d = memory[s0]; memory[s0] = s1 (atomic)
        OP_ATOMIC_EXCHANGE,
        // d = memory[s0]; if (d == s1) memory[s0] = s2 (atomic)
        OP_ATOMIC_COMPARE_EXCHANGE,
        // d = memory[s0]; memory[s0] = d <op> s1 (atomic). The operation
        // is one of OP_ADD, OP_AND, OP_AND_NOT (d & ~s1, ARM ldclr), OP_OR,
        // OP_XOR,
        // OP_MINIMUM_SIGNED/UNSIGNED, OP_MAXIMUM_SIGNED/UNSIGNED, see
        // SemanticInstruction::getCombineOperation()
        OP_ATOMIC_RMW,
        // d = memory[s0], and start monitoring the address (ARM LDXR,
        // RISC-V LR). Not interpreted yet
        OP_LOAD_EXCLUSIVE,
        // memory[s0] = s1 if the address is still monitored. d (1 bit) is 1
        // on success (note: ARM STXR and RISC-V SC write 0 on success, their
        // lifters invert it). Not interpreted yet
        OP_STORE_EXCLUSIVE,
        // Order the memory accesses, see MemoryOrder and FenceAccess
        OP_FENCE,
        // Make the stores to code visible to the instruction fetch of this
        // processor (RISC-V fence.i, ARM ic ivau + isb). A translator throws
        // away the translations of the code which was written
        OP_FENCE_INSTRUCTION,
        // A hint that memory[s0] will be accessed. May be removed
        OP_PREFETCH,
        // Cache maintenance of the line of memory[s0], see CacheOperation
        OP_CACHE_MAINTENANCE,
        // d (1 bit) = 1 if the memory at s0 can be read without a fault, the
        // size of the access is the size of the type. Never faults and has no
        // effect on the guest (SVE first fault and non fault loads: the
        // elements that fault are not loaded)
        OP_PROBE_ACCESS,

        //////////////////////////////////////////////////////////////////
        // Integer arithmetic
        //////////////////////////////////////////////////////////////////

        // d = s0 + s1
        OP_ADD,
        // d = s0 + s1 + s2 (s2 is a 1 bit carry). Used for adc and for
        // splitting wide additions
        OP_ADD_CARRY,
        // d = s0 - s1
        OP_SUBTRACT,
        // d = s0 - s1 - s2 (s2 is a 1 bit borrow)
        OP_SUBTRACT_BORROW,
        // d = -s0
        OP_NEGATE,
        // d = the low half of s0 * s1
        OP_MULTIPLY,
        // d = the high half of the signed s0 * s1
        OP_MULTIPLY_HIGH_SIGNED,
        // d = the high half of the unsigned s0 * s1
        OP_MULTIPLY_HIGH_UNSIGNED,
        // d = the high half of the signed s0 * the unsigned s1 (RISC-V
        // mulhsu)
        OP_MULTIPLY_HIGH_SIGNED_UNSIGNED,
        // d = s0 / s1, signed, rounded toward zero. A division by zero or
        // an overflow follows the Behavior
        OP_DIVIDE_SIGNED,
        // d = s0 / s1, unsigned
        OP_DIVIDE_UNSIGNED,
        // d = s0 % s1, signed (the sign of s0)
        OP_REMAINDER_SIGNED,
        // d = s0 % s1, unsigned
        OP_REMAINDER_UNSIGNED,
        // d = |s0 - s1|, signed (ARM SABD)
        OP_ABSOLUTE_DIFFERENCE_SIGNED,
        // d = |s0 - s1|, unsigned (ARM UABD, psadbw)
        OP_ABSOLUTE_DIFFERENCE_UNSIGNED,
        // d = the low half of the carry-less product of s0 and s1
        // (pclmulqdq, ARM PMULL, RISC-V clmul)
        OP_CARRYLESS_MULTIPLY,
        // d = the high half of the carry-less product (RISC-V clmulh)
        OP_CARRYLESS_MULTIPLY_HIGH,

        //////////////////////////////////////////////////////////////////
        // Logic, shifts and bits
        //////////////////////////////////////////////////////////////////

        // d = s0 & s1
        OP_AND,
        // d = s0 | s1
        OP_OR,
        // d = s0 ^ s1
        OP_XOR,
        // d = ~s0
        OP_NOT,
        // d = ~s0 & s1
        OP_AND_NOT,
        // d = (s0 & s1) | (~s0 & s2), bit by bit (ARM BSL/BIT/BIF)
        OP_BITWISE_SELECT,
        // d = s0 << s1. A count of the size or more gives 0
        OP_SHIFT_LEFT,
        // d = s0 >> s1, unsigned. A count of the size or more gives 0
        OP_SHIFT_RIGHT_LOGICAL,
        // d = s0 >> s1, signed. A count of the size or more gives the sign
        // of s0 in all the bits
        OP_SHIFT_RIGHT_ARITHMETIC,
        // d = s0 rotated left by s1 (modulo the size)
        OP_ROTATE_LEFT,
        // d = s0 rotated right by s1 (modulo the size)
        OP_ROTATE_RIGHT,
        // d = the number of leading zero bits of s0 (the size for 0)
        OP_COUNT_LEADING_ZEROS,
        // d = the number of trailing zero bits of s0 (the size for 0)
        OP_COUNT_TRAILING_ZEROS,
        // d = the number of leading bits of s0 equal to its sign bit, not
        // counting the sign bit (ARM CLS)
        OP_COUNT_LEADING_SIGN_BITS,
        // d = the number of one bits of s0
        OP_POPULATION_COUNT,
        // d = s0 with its bytes reversed
        OP_BYTE_SWAP,
        // d = s0 with its bits reversed
        OP_BIT_REVERSE,
        // d = the low bits of s0 scattered to the positions of the one bits
        // of the mask s1 (pdep, SVE2 BDEP)
        OP_BIT_DEPOSIT,
        // d = the bits of s0 at the positions of the one bits of the mask
        // s1, packed into the low bits (pext, SVE2 BEXT)
        OP_BIT_GATHER,

        //////////////////////////////////////////////////////////////////
        // Compare, select and flags
        //////////////////////////////////////////////////////////////////

        // d (1 bit, or a mask for vectors) = s0 <condition> s1
        OP_COMPARE,
        // d = s0 ? s1 : s2. For vectors s0 is a mask, chosen per lane
        OP_SELECT,
        // Set the flags in the flag mask from the operands s0, s1 and the
        // result s2, see FlagRecipe. With a condition other than COND_NONE
        // the recipe is used only when the condition (on the flags before)
        // is true; otherwise the flags of the mask are set to the
        // SemanticInstruction::getFalseFlags() bits (ARM ccmp, APX ccmp)
        OP_SET_FLAGS,
        // d (1 bit) = the condition evaluated from the current flags
        OP_TEST_FLAGS,

        //////////////////////////////////////////////////////////////////
        // Floating point (the rounding mode is an attribute)
        //////////////////////////////////////////////////////////////////

        // d = s0 + s1
        OP_FLOAT_ADD,
        // d = s0 - s1
        OP_FLOAT_SUBTRACT,
        // d = s0 * s1
        OP_FLOAT_MULTIPLY,
        // d = s0 / s1
        OP_FLOAT_DIVIDE,
        // d = s0 * s1 + s2 with a single rounding
        OP_FLOAT_MULTIPLY_ADD,
        // d = square root of s0
        OP_FLOAT_SQUARE_ROOT,
        // d = the minimum of s0 and s1 (the NaN rules are the Behavior)
        OP_FLOAT_MINIMUM,
        // d = the maximum of s0 and s1
        OP_FLOAT_MAXIMUM,
        // d = |s0|
        OP_FLOAT_ABSOLUTE,
        // d = -s0
        OP_FLOAT_NEGATE,
        // d = s0 with the sign of s1 (RISC-V fsgnj)
        OP_FLOAT_COPY_SIGN,
        // d = s0 rounded to an integral value (in the floating point format).
        // The kind (SemanticInstruction::getKind()) is a FloatRoundKind
        OP_FLOAT_ROUND,
        // d = s0 converted to the floating point format of d
        OP_FLOAT_CONVERT,
        // d (float) = s0 (signed integer)
        OP_SIGNED_TO_FLOAT,
        // d (float) = s0 (unsigned integer)
        OP_UNSIGNED_TO_FLOAT,
        // d (signed integer) = s0 (float). Out of range values follow the
        // Behavior
        OP_FLOAT_TO_SIGNED,
        // d (unsigned integer) = s0 (float)
        OP_FLOAT_TO_UNSIGNED,
        // d (integer) = the class of s0 as the bits of RISC-V fclass
        // (negative infinity = bit 0 ... quiet NaN = bit 9)
        OP_FLOAT_CLASSIFY,
        // d = s0 * 2^floor(s1) (vscalefps, SVE FSCALE, x87 fscale)
        OP_FLOAT_SCALE,
        // d = the unbiased exponent of s0, as a float (vgetexpps)
        OP_FLOAT_GET_EXPONENT,
        // d = an estimate of 1 / s0. The bits of the estimate are those of
        // the processor in the Behavior (each vendor has its own table)
        OP_FLOAT_RECIPROCAL_ESTIMATE,
        // d = an estimate of 1 / sqrt(s0), see OP_FLOAT_RECIPROCAL_ESTIMATE
        OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE,

        //////////////////////////////////////////////////////////////////
        // Vectors (all the operations above work on vectors too)
        //////////////////////////////////////////////////////////////////

        // d = s0 + s1, saturated as signed
        OP_ADD_SATURATE_SIGNED,
        // d = s0 + s1, saturated as unsigned
        OP_ADD_SATURATE_UNSIGNED,
        // d = s0 - s1, saturated as signed
        OP_SUBTRACT_SATURATE_SIGNED,
        // d = s0 - s1, saturated as unsigned
        OP_SUBTRACT_SATURATE_UNSIGNED,
        // d = (s0 + s1 + 1) >> 1, unsigned
        OP_AVERAGE_UNSIGNED,
        // d = the signed minimum of s0 and s1
        OP_MINIMUM_SIGNED,
        // d = the unsigned minimum of s0 and s1
        OP_MINIMUM_UNSIGNED,
        // d = the signed maximum of s0 and s1
        OP_MAXIMUM_SIGNED,
        // d = the unsigned maximum of s0 and s1
        OP_MAXIMUM_UNSIGNED,
        // d = |s0|
        OP_ABSOLUTE,
        // d = (s0 * s1 + 2^(bits-2)) >> (bits-1), signed, saturated
        // (pmulhrsw, ARM SQRDMULH)
        OP_MULTIPLY_HIGH_ROUNDING,
        // d = (s0 + 2^(s1-1)) >> s1, the rounding shift of the Behavior
        // signedness (ARM SRSHR/URSHR, RVV vssrl)
        OP_SHIFT_RIGHT_ROUNDING,
        // d[i] = s2[i] + the sum of the products of the element pairs of
        // s0 and s1 which make lane i (pmaddwd, vpdpbusd, ARM SDOT/UDOT).
        // The type is the type of d, the signedness is the Behavior
        OP_DOT_PRODUCT,
        // d = the sums of the adjacent lanes of s0 then of s1 (phaddw, ARM
        // ADDP, haddps)
        OP_PAIRWISE_ADD,
        // d = every lane set to the scalar s0
        OP_BROADCAST,
        // d[i] = (s0:s1)[s2[i]]. s2 is a vector of lane indexes. An out of
        // range index follows the Behavior
        OP_SHUFFLE,
        // d = the lanes of s0 and s1 interleaved, starting with lane s2 (0
        // for the low halves, half the lanes for the high halves)
        OP_INTERLEAVE,
        // d = the lanes of s1:s0 starting at lane s2 (palignr, ARM EXT, RVV
        // vslidedown)
        OP_SLIDE,
        // d = the lanes of s0 then s1, each narrowed to the element of d.
        // The source lanes are signed; the saturation attribute is the range
        // of the result (packsswb: signed, packuswb: unsigned)
        OP_NARROW,
        // d = the active lanes of s0 (mask s1) packed into the low lanes
        // (vpcompress, SVE COMPACT, RVV vcompress)
        OP_COMPRESS,
        // d = the low lanes of s0 spread into the active lanes of the mask
        // s1 (vpexpand)
        OP_EXPAND,
        // d = the lane numbers 0, 1, 2... times s1, plus s0 (SVE INDEX, RVV
        // vid)
        OP_LANE_INDEX,
        // d (mask) = the lanes i for which s0 + i < s1, unsigned (SVE
        // whilelo, the tail of RVV vl)
        OP_ACTIVE_LANE_MASK,
        // d (scalar) = the number of active lanes of the mask s0 (vcpop,
        // SVE CNTP)
        OP_MASK_COUNT,
        // d (scalar) = the index of the first active lane of the mask s0,
        // -1 if none (vfirst)
        OP_MASK_FIRST,
        // d (scalar) = the lanes of s0 combined with the combine operation
        // (OP_ADD, OP_AND, OP_OR, OP_XOR, OP_MINIMUM_*, OP_MAXIMUM_*,
        // OP_FLOAT_ADD, OP_FLOAT_MINIMUM, OP_FLOAT_MAXIMUM). The order of a
        // float reduction is the Behavior
        OP_REDUCE,
        // d (integer) = the sign bits of the lanes of s0 (pmovmskb, movmskps)
        OP_SIGN_MASK,

        //////////////////////////////////////////////////////////////////
        // Cryptography and checksums. AES is split into its steps, so the
        // different processors compose the same steps:
        //   x86 aesenc  = AES_SUB_SHIFT, AES_MIX_COLUMNS, xor key
        //   ARM AESE    = xor key, AES_SUB_SHIFT;  ARM AESMC = AES_MIX_COLUMNS
        //////////////////////////////////////////////////////////////////

        // d = ShiftRows(SubBytes(s0))
        OP_AES_SUB_SHIFT,
        // d = InvShiftRows(InvSubBytes(s0))
        OP_AES_INVERSE_SUB_SHIFT,
        // d = MixColumns(s0)
        OP_AES_MIX_COLUMNS,
        // d = InvMixColumns(s0)
        OP_AES_INVERSE_MIX_COLUMNS,
        // d = the CRC of s0 (the data, the size of the type) continued from
        // the CRC s1, reflected. The polynomial is the Behavior (x86 crc32
        // is CRC-32C, ARM has both)
        OP_CRC32,

        //////////////////////////////////////////////////////////////////
        // Control flow
        //
        // A branch target is a guest address, a register / temporary which
        // holds a guest address, or a label (KIND_LABEL) of the block. A
        // branch to a label stays in the block.
        //////////////////////////////////////////////////////////////////

        // Define the label s0 at this point of the block
        OP_LABEL,
        // Continue at s0
        OP_BRANCH,
        // Continue at s1 if s0 (1 bit) is set
        OP_BRANCH_CONDITIONAL,
        // Continue at the guest address in s0 (a register or a temporary).
        // On processors with more than one instruction set (ARM / Thumb)
        // the target may switch it, see the mode register of the
        // ArchitectureDescription
        OP_BRANCH_INDIRECT,
        // Continue at s0 and remember that this is a call, for the return
        // address prediction of the translator. The guest stack or link
        // register is written explicitly
        OP_CALL,
        // Continue at s0 and remember that this is a return
        OP_RETURN,
        // Raise a guest trap, see TrapKind. s0 is the number of the trap
        // (the interrupt vector, the system call immediate)
        OP_TRAP,
        // A hint to the processor, see HintKind. May be removed
        OP_HINT,

        //////////////////////////////////////////////////////////////////
        // Processor state
        //////////////////////////////////////////////////////////////////

        // d = system register s0 (an immediate register id of the
        // architecture). Unlike OP_MOVE the read has side effects and is
        // never removed or moved (rdtsc, mrs cntvct_el0, csrr cycle)
        OP_READ_SYSTEM_REGISTER,
        // system register s0 = s1, see OP_READ_SYSTEM_REGISTER
        OP_WRITE_SYSTEM_REGISTER,
        // d = register (s0 + (s1 mod s2)): s0 is a register operand (the
        // first register of a group with consecutive ids), s1 an index
        // value and s2 an immediate count. The type is the register's. For
        // register files indexed at runtime: the x87 ST(i) is
        // FPR[(TOP + i) mod 8]
        OP_READ_REGISTER_INDEXED,
        // register (d + (s0 mod s1)) = s2: d is the first register of the
        // group, see OP_READ_REGISTER_INDEXED
        OP_WRITE_REGISTER_INDEXED,

        //////////////////////////////////////////////////////////////////
        // Escape hatch
        //////////////////////////////////////////////////////////////////

        // d = runtime helper number s0 (s1, s2). For operations that are
        // not worth translating inline: cpuid, in/out, the x87
        // transcendental functions, matrix instructions, SHA rounds,
        // privileged instructions. The helper numbers belong to the
        // architecture (see IA32Semantics.h)
        OP_HELPER,
        // The guest instruction has no description. The translator falls
        // back to an interpreter or stops
        OP_UNDEFINED,

        // The number of operations. Must be last
        OP_COUNT
    };

    /*
     * The conditions of OP_COMPARE, OP_TEST_FLAGS and of the conditional
     * OP_SET_FLAGS.
     *
     * OP_TEST_FLAGS evaluates them on the semantic flags (SemanticFlags),
     * whose carry is the IA-32 one: after a subtraction it is the borrow.
     * A processor whose carry is the inverted borrow (ARM) converts it when
     * it's lifted or lowered, see ArchitectureDescription.
     *
     *   COND_EQUAL                  ZF            COND_NOT_EQUAL        !ZF
     *   COND_UNSIGNED_LESS          CF            ..._GREATER_EQUAL     !CF
     *   COND_UNSIGNED_LESS_EQUAL    CF | ZF       COND_UNSIGNED_GREATER !CF & !ZF
     *   COND_SIGNED_LESS            SF != OF      ..._GREATER_EQUAL     SF == OF
     *   COND_SIGNED_LESS_EQUAL      ZF | SF != OF COND_SIGNED_GREATER   !ZF & SF == OF
     *   COND_NEGATIVE               SF            COND_NOT_NEGATIVE     !SF
     *   COND_OVERFLOW               OF            COND_NOT_OVERFLOW     !OF
     *   COND_CARRY                  CF            COND_NOT_CARRY        !CF
     *   COND_PARITY_EVEN            PF            COND_PARITY_ODD       !PF
     *
     * The float conditions are for OP_COMPARE of floats only.
     */
    enum Condition {
        COND_NONE,
        COND_ALWAYS,
        COND_NEVER,
        COND_EQUAL,
        COND_NOT_EQUAL,
        COND_SIGNED_LESS,
        COND_SIGNED_LESS_EQUAL,
        COND_SIGNED_GREATER,
        COND_SIGNED_GREATER_EQUAL,
        COND_UNSIGNED_LESS,
        COND_UNSIGNED_LESS_EQUAL,
        COND_UNSIGNED_GREATER,
        COND_UNSIGNED_GREATER_EQUAL,
        COND_NEGATIVE,
        COND_NOT_NEGATIVE,
        COND_OVERFLOW,
        COND_NOT_OVERFLOW,
        COND_CARRY,
        COND_NOT_CARRY,
        COND_PARITY_EVEN,
        COND_PARITY_ODD,
        // Floating point compares. An unordered compare has a NaN operand
        COND_FLOAT_ORDERED,
        COND_FLOAT_UNORDERED,
        COND_FLOAT_EQUAL,
        COND_FLOAT_NOT_EQUAL,
        COND_FLOAT_LESS,
        COND_FLOAT_LESS_EQUAL,
        COND_FLOAT_GREATER,
        COND_FLOAT_GREATER_EQUAL,
        COND_FLOAT_UNORDERED_OR_EQUAL,
        COND_FLOAT_UNORDERED_OR_LESS,
        COND_FLOAT_UNORDERED_OR_LESS_EQUAL
    };

    /*
     * How OP_SET_FLAGS computes the flags from its operands s0, s1 and the
     * result s2. The flags are SemanticFlags::Flag bits (see
     * ArchitectureDescription.h); only the flags in the flag mask of the
     * instruction are written. N is the size of the type of the instruction
     * and MSB(x) is bit N-1 of x.
     *
     * Every recipe which has a result sets, when they are in the mask:
     *   ZERO    s2 == 0
     *   SIGN    MSB(s2)
     *   PARITY  the low byte of s2 has an even number of one bits
     *
     * Keeping the recipe instead of the flags values lets the translator:
     *   - Skip the flags which are never read (most of them).
     *   - Evaluate a condition directly from the operands: a FLAGS_SUBTRACT
     *     followed by COND_SIGNED_LESS is s0 < s1.
     *   - Use the flags of the target when they match (ARM ADDS/SUBS, with
     *     the inverted carry of a subtraction).
     */
    enum FlagRecipe {
        // No recipe
        FLAGS_NONE,
        // s2 = s0 + s1, or s0 + s1 + carry (adc, inc with a mask without
        // CARRY). The formulas don't depend on the carry in:
        //   CARRY     MSB((s0 & s1) | ((s0 | s1) & ~s2))
        //   OVERFLOW  MSB((s0 ^ s2) & (s1 ^ s2))
        //   ADJUST    bit 4 of (s0 ^ s1 ^ s2)
        FLAGS_ADD,
        // s2 = s0 - s1, or s0 - s1 - borrow (sub, cmp, sbb, dec, and neg as
        // 0 - s1). The formulas don't depend on the borrow in:
        //   CARRY     MSB((~s0 & s1) | ((~s0 | s1) & s2))   (the borrow)
        //   OVERFLOW  MSB((s0 ^ s1) & (s0 ^ s2))
        //   ADJUST    bit 4 of (s0 ^ s1 ^ s2)
        FLAGS_SUBTRACT,
        // s2 = a logic operation (and, or, xor, test):
        //   CARRY = OVERFLOW = ADJUST = 0
        FLAGS_LOGIC,
        // s2 = s0 << s1, with 0 < s1 (the lifter skips the flags of a 0
        // count):
        //   CARRY     s1 <= N ? bit (N - s1) of s0 : 0
        //   OVERFLOW  MSB(s2) ^ CARRY
        //   ADJUST    0
        FLAGS_SHIFT_LEFT,
        // s2 = s0 >> s1, unsigned, with 0 < s1:
        //   CARRY     s1 <= N ? bit (s1 - 1) of s0 : 0
        //   OVERFLOW  MSB(s0)
        //   ADJUST    0
        FLAGS_SHIFT_RIGHT_LOGICAL,
        // s2 = s0 >> s1, signed, with 0 < s1:
        //   CARRY     bit (min(s1, N) - 1) of s0
        //   OVERFLOW  0
        //   ADJUST    0
        FLAGS_SHIFT_RIGHT_ARITHMETIC,
        // s2 = s0 rotated left (rol, rcl), with a non zero count. Only CARRY
        // and OVERFLOW:
        //   CARRY     bit 0 of s2
        //   OVERFLOW  MSB(s2) ^ CARRY
        FLAGS_ROTATE_LEFT,
        // s2 = s0 rotated right (ror, rcr), with a non zero count:
        //   CARRY     MSB(s2)
        //   OVERFLOW  MSB(s2) ^ bit N-2 of s2
        FLAGS_ROTATE_RIGHT,
        // s0 = the low half and s1 = the high half of an unsigned product:
        //   CARRY = OVERFLOW = (s1 != 0)
        FLAGS_MULTIPLY_UNSIGNED,
        // s0 = the low half and s1 = the high half of a signed product:
        //   CARRY = OVERFLOW = (s1 != the sign extension of s0)
        FLAGS_MULTIPLY_SIGNED,
        // The flags of a floating point compare of s0 and s1 (comisd,
        // fcomi):
        //   unordered ZERO = PARITY = CARRY = 1;  less CARRY = 1;
        //   equal ZERO = 1;  greater all 0.  OVERFLOW = SIGN = ADJUST = 0
        // The kind of the instruction is a FloatCompareKind (comisd and
        // fcomi signal on a quiet NaN, ucomisd and fucomi don't)
        FLAGS_FLOAT_COMPARE,
        // The flags of a test of the predicate s1 governed by s0 (SVE
        // PTEST): SIGN = the first active lane is set, ZERO = no active lane
        // is set, CARRY = the last active lane is not set, OVERFLOW = 0
        FLAGS_PREDICATE_TEST,
        // Each flag in the mask is set to the 1 bit value s0 (stc, clc, cld,
        // bt, the flags of popf and sahf)
        FLAGS_EXPLICIT
    };

    /*
     * Where processors disagree on the result of an operation, the lifter
     * writes the behavior of the guest into the instruction (see
     * SemanticInstruction::getBehavior()). BEHAVIOR_DEFAULT is the first
     * documented behavior of each group.
     */
    enum Behavior {
        BEHAVIOR_DEFAULT,

        // OP_DIVIDE_* and OP_REMAINDER_*, by zero and the signed overflow
        // (the minimum / -1):
        //   Raise the divide error trap (IA-32 #DE). The default
        BEHAVIOR_DIVIDE_FAULT = BEHAVIOR_DEFAULT,
        //   By zero: 0, remainder the dividend. Overflow: the minimum,
        //   remainder 0 (ARM)
        BEHAVIOR_DIVIDE_ZERO,
        //   By zero: all ones, remainder the dividend. Overflow: the
        //   minimum, remainder 0 (RISC-V)
        BEHAVIOR_DIVIDE_ALL_ONES,

        // OP_FLOAT_TO_SIGNED/UNSIGNED of a NaN or an out of range value:
        //   The "integer indefinite", only the sign bit set (IA-32). The
        //   default
        BEHAVIOR_CONVERT_INDEFINITE = BEHAVIOR_DEFAULT,
        //   Saturate, a NaN is 0 (ARM)
        BEHAVIOR_CONVERT_SATURATE_NAN_ZERO,
        //   Saturate, a NaN is the maximum (RISC-V)
        BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM,

        // OP_FLOAT_MINIMUM/MAXIMUM:
        //   s1 if either is a NaN or both are zeros (SSE minps). The
        //   default
        BEHAVIOR_MINMAX_SECOND = BEHAVIOR_DEFAULT,
        //   A NaN if either is a NaN (ARM FMIN, IEEE-754 minimum)
        BEHAVIOR_MINMAX_PROPAGATE_NAN,
        //   The number if only one is a NaN (ARM FMINNM, RISC-V fmin,
        //   IEEE-754 minimumNumber)
        BEHAVIOR_MINMAX_NUMBER,

        // OP_SHUFFLE with an index which is out of range:
        //   The index is taken modulo the lanes count. The default
        BEHAVIOR_SHUFFLE_MODULO = BEHAVIOR_DEFAULT,
        //   The lane is 0 (pshufb with bit 7 set, ARM TBL, RVV vrgather).
        //   ARM TBX, which keeps the lane, is this and an OP_SELECT
        BEHAVIOR_SHUFFLE_ZERO,

        // OP_REDUCE of floats:
        //   Lane order, one rounding per lane (RVV vfredosum). The default
        BEHAVIOR_REDUCE_ORDERED = BEHAVIOR_DEFAULT,
        //   Pairwise tree (ARM FADDV)
        BEHAVIOR_REDUCE_PAIRWISE,

        // OP_FLOAT_RECIPROCAL_*_ESTIMATE, the table of the processor:
        BEHAVIOR_ESTIMATE_IA32 = BEHAVIOR_DEFAULT,
        BEHAVIOR_ESTIMATE_ARM,
        BEHAVIOR_ESTIMATE_RISCV,

        // OP_DOT_PRODUCT and OP_SHIFT_RIGHT_ROUNDING, the signedness of
        // s0 and s1:
        BEHAVIOR_SIGNED = BEHAVIOR_DEFAULT,
        BEHAVIOR_UNSIGNED,
        BEHAVIOR_UNSIGNED_SIGNED,

        // OP_CRC32, the polynomial:
        BEHAVIOR_CRC32C = BEHAVIOR_DEFAULT,
        BEHAVIOR_CRC32_IEEE,

        // OP_FLOAT_MULTIPLY: 0 * infinity is 2.0 with the sign of the product
        // instead of an invalid operation (ARM FMULX)
        BEHAVIOR_MULTIPLY_EXTENDED,

        // OP_FLOAT_MULTIPLY_ADD as the Newton-Raphson step of ARM: s0 is
        // negated first (a NaN too), the result is 2 + s0 * s1 (A64 FRECPS)
        // or (3 + s0 * s1) / 2 (FRSQRTS), fused, s2 is ignored, and
        // 0 * infinity gives 2.0 / 1.5
        BEHAVIOR_STEP_RECIPROCAL,
        BEHAVIOR_STEP_RECIPROCAL_SQRT,
        // The AArch32 steps (VRECPS, VRSQRTS): 2 - s0 * s1 and
        // (3 - s0 * s1) / 2 with the product rounded
        BEHAVIOR_STEP_RECIPROCAL_UNFUSED,
        BEHAVIOR_STEP_RECIPROCAL_SQRT_UNFUSED,

        // OP_FLOAT_RECIPROCAL_ESTIMATE of ARM FRECPX: the reciprocal
        // exponent. (On an integer type the ARM URECPE / URSQRTE estimate of
        // an unsigned fixed point number is BEHAVIOR_ESTIMATE_ARM)
        BEHAVIOR_ESTIMATE_ARM_EXPONENT,

        // OP_FLOAT_ROUND to the integers of 32 or 64 bits: a value which
        // doesn't fit, an infinity and a NaN give -2^31 / -2^63 and an
        // invalid operation (ARM FRINT32Z/X, FRINT64Z/X)
        BEHAVIOR_ROUND_INTEGER_32,
        BEHAVIOR_ROUND_INTEGER_64,

        // OP_FLOAT_SCALE where s1 is a signed integer lane (ARM FSCALE)
        BEHAVIOR_SCALE_INTEGER,

        // OP_COMPARE of floats: no NaN signals invalid except a signaling
        // one / every NaN signals invalid, whatever the condition
        BEHAVIOR_COMPARE_QUIET,
        BEHAVIOR_COMPARE_SIGNALING
    };

    // The kind of OP_TRAP
    enum TrapKind {
        // A processor exception, s0 is its vector (IA-32 #DE is 0, #UD 6)
        TRAP_FAULT,
        // A software interrupt, s0 is its vector (int n, into)
        TRAP_INTERRUPT,
        // A system call (syscall, sysenter, svc, ecall). s0 is the
        // immediate of the instruction, if any
        TRAP_SYSTEM_CALL,
        // A breakpoint (int3, brk, ebreak)
        TRAP_BREAKPOINT,
        // An undefined instruction on purpose (ud2, udf, unimp)
        TRAP_UNDEFINED
    };

    // The kind of OP_HINT
    enum HintKind {
        // A spin loop (pause, yield)
        HINT_SPIN,
        // Wait for an event or an interrupt (wfe, wfi, hlt)
        HINT_WAIT,
        // Send an event (sev)
        HINT_SEND_EVENT,
        // A branch target, for control flow integrity (endbr32, bti)
        HINT_LANDING_PAD
    };

    // The kind of FLAGS_FLOAT_COMPARE
    enum FloatCompareKind {
        // Only a signaling NaN raises invalid (ucomisd, fucomi)
        FLOAT_COMPARE_QUIET,
        // Any NaN raises invalid (comisd, fcomi)
        FLOAT_COMPARE_SIGNALING
    };

    // The kind of OP_FLOAT_ROUND
    enum FloatRoundKind {
        // Raise inexact when the value changes (frndint, ARM frintx)
        FLOAT_ROUND_SIGNAL_INEXACT,
        // Never raise inexact (roundss with imm8 bit 3, ARM frintz)
        FLOAT_ROUND_SUPPRESS_INEXACT
    };

    // The memory ordering of OP_FENCE and of the memory operations
    enum MemoryOrder {
        // No ordering
        ORDER_RELAXED,
        // Later accesses stay after this one
        ORDER_ACQUIRE,
        // Like ORDER_ACQUIRE, but only for the stores of other processors
        // (RCpc, ARM LDAPR)
        ORDER_ACQUIRE_PROCESSOR_CONSISTENT,
        // Earlier accesses stay before this one
        ORDER_RELEASE,
        // Both of the above
        ORDER_ACQUIRE_RELEASE,
        // A total order (mfence, lock prefix)
        ORDER_SEQUENTIAL
    };

    /*
     * The accesses which OP_FENCE orders (bit mask): the accesses before the
     * fence (FENCE_BEFORE_*) are ordered against the ones after it
     * (FENCE_AFTER_*). mfence is all, lfence and RISC-V 'fence r,r' are
     * FENCE_BEFORE_LOADS | FENCE_AFTER_LOADS, ARM 'dmb ishst' is
     * FENCE_BEFORE_STORES | FENCE_AFTER_STORES.
     */
    enum FenceAccess {
        FENCE_BEFORE_LOADS  = 0x01,
        FENCE_BEFORE_STORES = 0x02,
        FENCE_AFTER_LOADS   = 0x04,
        FENCE_AFTER_STORES  = 0x08,
        FENCE_ALL           = 0x0F
    };

    // The operation of OP_CACHE_MAINTENANCE
    enum CacheOperation {
        // Write the line back to memory and invalidate it (clflush, DC
        // CIVAC, cbo.flush)
        CACHE_FLUSH,
        // Write the line back to memory, keep it (clwb, DC CVAC, cbo.clean)
        CACHE_CLEAN,
        // Invalidate the line without writing it (DC IVAC, cbo.inval)
        CACHE_INVALIDATE,
        // Zero the whole line, it's a store (DC ZVA, cbo.zero)
        CACHE_ZERO
    };

    // The rounding mode of the floating point and conversion operations
    enum RoundingMode {
        // The current mode of the guest (MXCSR, x87 control word, FPCR)
        ROUND_DYNAMIC,
        ROUND_NEAREST_EVEN,
        // To nearest, ties away from zero (ARM frinta, RISC-V RMM)
        ROUND_NEAREST_AWAY,
        ROUND_TOWARD_ZERO,
        ROUND_TOWARD_POSITIVE,
        ROUND_TOWARD_NEGATIVE,
        // Round to odd (ARM FCVTXN)
        ROUND_TO_ODD
    };

    /*
     * Return the name of an operation, for traces ("add", "load"...)
     */
    static const char* getName(Operation operation);

    /*
     * Return the number of sources the operation reads (0 to 3)
     */
    static uint getSourcesCount(Operation operation);

    /*
     * Return true if the operation has a destination value
     */
    static bool hasDestination(Operation operation);

    /*
     * Return true if the sources s0 and s1 of the operation can be swapped
     */
    static bool isCommutative(Operation operation);

    /*
     * Return true if the operation reads or writes memory
     */
    static bool isMemoryAccess(Operation operation);

    /*
     * Return true if the operation changes the flow of the guest program
     * (the branches, OP_TRAP; not OP_LABEL)
     */
    static bool isControlFlow(Operation operation);

    /*
     * Return true if the operation has effects beyond its destination, so it
     * can't be removed when its destination isn't read (stores, branches,
     * OP_SET_FLAGS, system registers, helpers)
     */
    static bool hasSideEffects(Operation operation);

    /*
     * Return the condition which is true when 'condition' is false
     */
    static Condition invertCondition(Condition condition);
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICOPERATION_H
