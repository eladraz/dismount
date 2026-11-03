/*
 * SemanticOperation.cpp
 *
 * The static properties of the operations of the semantic IR.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "dismount/semantics/SemanticOperation.h"

namespace {

// The bits of OperationInfo::m_flags
enum {
    // The operation has a destination
    F_DEST         = 0x01,
    F_COMMUTATIVE  = 0x02,
    F_MEMORY       = 0x04,
    F_CONTROL      = 0x08,
    F_SIDE_EFFECTS = 0x10
};

struct OperationInfo {
    // The name, for traces
    const char* m_name;
    // The number of sources
    uint m_sources;
    // The F_* bits
    uint m_flags;
};

// The operations, in the order of SemanticOperation::Operation
const OperationInfo gOperations[] =
{
    /* OP_NOP                           */ { "nop", 0, 0 },
    /* OP_MOVE                          */ { "move", 1, F_DEST },
    /* OP_ZERO_EXTEND                   */ { "zero_extend", 1, F_DEST },
    /* OP_SIGN_EXTEND                   */ { "sign_extend", 1, F_DEST },
    /* OP_TRUNCATE                      */ { "truncate", 1, F_DEST },
    /* OP_EXTRACT                       */ { "extract", 2, F_DEST },
    /* OP_INSERT                        */ { "insert", 3, F_DEST },
    /* OP_CONCAT                        */ { "concat", 2, F_DEST },
    /* OP_BITCAST                       */ { "bitcast", 1, F_DEST },
    /* OP_LOAD                          */ { "load", 1, F_DEST | F_MEMORY },
    /* OP_STORE                         */ { "store", 2, F_MEMORY | F_SIDE_EFFECTS },
    /* OP_LOAD_MASKED                   */ { "load_masked", 2, F_DEST | F_MEMORY },
    /* OP_STORE_MASKED                  */ { "store_masked", 3, F_MEMORY | F_SIDE_EFFECTS },
    /* OP_GATHER                        */ { "gather", 3, F_DEST | F_MEMORY },
    /* OP_SCATTER                       */ { "scatter", 3, F_MEMORY | F_SIDE_EFFECTS },
    /* OP_LOAD_INTERLEAVED              */ { "load_interleaved", 2, F_DEST | F_MEMORY },
    /* OP_STORE_INTERLEAVED             */ { "store_interleaved", 3, F_MEMORY | F_SIDE_EFFECTS },
    /* OP_ATOMIC_EXCHANGE               */ { "atomic_exchange", 2, F_DEST | F_MEMORY | F_SIDE_EFFECTS },
    /* OP_ATOMIC_COMPARE_EXCHANGE       */ { "atomic_compare_exchange", 3, F_DEST | F_MEMORY | F_SIDE_EFFECTS },
    /* OP_ATOMIC_RMW                    */ { "atomic_rmw", 2, F_DEST | F_MEMORY | F_SIDE_EFFECTS },
    /* OP_LOAD_EXCLUSIVE                */ { "load_exclusive", 1, F_DEST | F_MEMORY | F_SIDE_EFFECTS },
    /* OP_STORE_EXCLUSIVE               */ { "store_exclusive", 2, F_DEST | F_MEMORY | F_SIDE_EFFECTS },
    /* OP_FENCE                         */ { "fence", 0, F_SIDE_EFFECTS },
    /* OP_FENCE_INSTRUCTION             */ { "fence_instruction", 0, F_SIDE_EFFECTS },
    /* OP_PREFETCH                      */ { "prefetch", 1, F_MEMORY },
    /* OP_CACHE_MAINTENANCE             */ { "cache_maintenance", 1, F_MEMORY | F_SIDE_EFFECTS },
    /* OP_ADD                           */ { "add", 2, F_DEST | F_COMMUTATIVE },
    /* OP_ADD_CARRY                     */ { "add_carry", 3, F_DEST },
    /* OP_SUBTRACT                      */ { "subtract", 2, F_DEST },
    /* OP_SUBTRACT_BORROW               */ { "subtract_borrow", 3, F_DEST },
    /* OP_NEGATE                        */ { "negate", 1, F_DEST },
    /* OP_MULTIPLY                      */ { "multiply", 2, F_DEST | F_COMMUTATIVE },
    /* OP_MULTIPLY_HIGH_SIGNED          */ { "multiply_high_signed", 2, F_DEST | F_COMMUTATIVE },
    /* OP_MULTIPLY_HIGH_UNSIGNED        */ { "multiply_high_unsigned", 2, F_DEST | F_COMMUTATIVE },
    /* OP_MULTIPLY_HIGH_SIGNED_UNSIGNED */ { "multiply_high_signed_unsigned", 2, F_DEST },
    /* OP_DIVIDE_SIGNED                 */ { "divide_signed", 2, F_DEST },
    /* OP_DIVIDE_UNSIGNED               */ { "divide_unsigned", 2, F_DEST },
    /* OP_REMAINDER_SIGNED              */ { "remainder_signed", 2, F_DEST },
    /* OP_REMAINDER_UNSIGNED            */ { "remainder_unsigned", 2, F_DEST },
    /* OP_ABSOLUTE_DIFFERENCE_SIGNED    */ { "absolute_difference_signed", 2, F_DEST | F_COMMUTATIVE },
    /* OP_ABSOLUTE_DIFFERENCE_UNSIGNED  */ { "absolute_difference_unsigned", 2, F_DEST | F_COMMUTATIVE },
    /* OP_CARRYLESS_MULTIPLY            */ { "carryless_multiply", 2, F_DEST | F_COMMUTATIVE },
    /* OP_CARRYLESS_MULTIPLY_HIGH       */ { "carryless_multiply_high", 2, F_DEST | F_COMMUTATIVE },
    /* OP_AND                           */ { "and", 2, F_DEST | F_COMMUTATIVE },
    /* OP_OR                            */ { "or", 2, F_DEST | F_COMMUTATIVE },
    /* OP_XOR                           */ { "xor", 2, F_DEST | F_COMMUTATIVE },
    /* OP_NOT                           */ { "not", 1, F_DEST },
    /* OP_AND_NOT                       */ { "and_not", 2, F_DEST },
    /* OP_BITWISE_SELECT                */ { "bitwise_select", 3, F_DEST },
    /* OP_SHIFT_LEFT                    */ { "shift_left", 2, F_DEST },
    /* OP_SHIFT_RIGHT_LOGICAL           */ { "shift_right_logical", 2, F_DEST },
    /* OP_SHIFT_RIGHT_ARITHMETIC        */ { "shift_right_arithmetic", 2, F_DEST },
    /* OP_ROTATE_LEFT                   */ { "rotate_left", 2, F_DEST },
    /* OP_ROTATE_RIGHT                  */ { "rotate_right", 2, F_DEST },
    /* OP_COUNT_LEADING_ZEROS           */ { "count_leading_zeros", 1, F_DEST },
    /* OP_COUNT_TRAILING_ZEROS          */ { "count_trailing_zeros", 1, F_DEST },
    /* OP_COUNT_LEADING_SIGN_BITS       */ { "count_leading_sign_bits", 1, F_DEST },
    /* OP_POPULATION_COUNT              */ { "population_count", 1, F_DEST },
    /* OP_BYTE_SWAP                     */ { "byte_swap", 1, F_DEST },
    /* OP_BIT_REVERSE                   */ { "bit_reverse", 1, F_DEST },
    /* OP_BIT_DEPOSIT                   */ { "bit_deposit", 2, F_DEST },
    /* OP_BIT_GATHER                    */ { "bit_gather", 2, F_DEST },
    /* OP_COMPARE                       */ { "compare", 2, F_DEST },
    /* OP_SELECT                        */ { "select", 3, F_DEST },
    /* OP_SET_FLAGS                     */ { "set_flags", 3, F_SIDE_EFFECTS },
    /* OP_TEST_FLAGS                    */ { "test_flags", 0, F_DEST },
    /* OP_FLOAT_ADD                     */ { "float_add", 2, F_DEST | F_COMMUTATIVE },
    /* OP_FLOAT_SUBTRACT                */ { "float_subtract", 2, F_DEST },
    /* OP_FLOAT_MULTIPLY                */ { "float_multiply", 2, F_DEST | F_COMMUTATIVE },
    /* OP_FLOAT_DIVIDE                  */ { "float_divide", 2, F_DEST },
    /* OP_FLOAT_MULTIPLY_ADD            */ { "float_multiply_add", 3, F_DEST },
    /* OP_FLOAT_SQUARE_ROOT             */ { "float_square_root", 1, F_DEST },
    /* OP_FLOAT_MINIMUM                 */ { "float_minimum", 2, F_DEST },
    /* OP_FLOAT_MAXIMUM                 */ { "float_maximum", 2, F_DEST },
    /* OP_FLOAT_ABSOLUTE                */ { "float_absolute", 1, F_DEST },
    /* OP_FLOAT_NEGATE                  */ { "float_negate", 1, F_DEST },
    /* OP_FLOAT_COPY_SIGN               */ { "float_copy_sign", 2, F_DEST },
    /* OP_FLOAT_ROUND                   */ { "float_round", 1, F_DEST },
    /* OP_FLOAT_CONVERT                 */ { "float_convert", 1, F_DEST },
    /* OP_SIGNED_TO_FLOAT               */ { "signed_to_float", 1, F_DEST },
    /* OP_UNSIGNED_TO_FLOAT             */ { "unsigned_to_float", 1, F_DEST },
    /* OP_FLOAT_TO_SIGNED               */ { "float_to_signed", 1, F_DEST },
    /* OP_FLOAT_TO_UNSIGNED             */ { "float_to_unsigned", 1, F_DEST },
    /* OP_FLOAT_CLASSIFY                */ { "float_classify", 1, F_DEST },
    /* OP_FLOAT_SCALE                   */ { "float_scale", 2, F_DEST },
    /* OP_FLOAT_GET_EXPONENT            */ { "float_get_exponent", 1, F_DEST },
    /* OP_FLOAT_RECIPROCAL_ESTIMATE     */ { "float_reciprocal_estimate", 1, F_DEST },
    /* OP_FLOAT_RECIPROCAL_SQRT_ESTIMATE*/ { "float_reciprocal_sqrt_estimate", 1, F_DEST },
    /* OP_ADD_SATURATE_SIGNED           */ { "add_saturate_signed", 2, F_DEST | F_COMMUTATIVE },
    /* OP_ADD_SATURATE_UNSIGNED         */ { "add_saturate_unsigned", 2, F_DEST | F_COMMUTATIVE },
    /* OP_SUBTRACT_SATURATE_SIGNED      */ { "subtract_saturate_signed", 2, F_DEST },
    /* OP_SUBTRACT_SATURATE_UNSIGNED    */ { "subtract_saturate_unsigned", 2, F_DEST },
    /* OP_AVERAGE_UNSIGNED              */ { "average_unsigned", 2, F_DEST | F_COMMUTATIVE },
    /* OP_MINIMUM_SIGNED                */ { "minimum_signed", 2, F_DEST | F_COMMUTATIVE },
    /* OP_MINIMUM_UNSIGNED              */ { "minimum_unsigned", 2, F_DEST | F_COMMUTATIVE },
    /* OP_MAXIMUM_SIGNED                */ { "maximum_signed", 2, F_DEST | F_COMMUTATIVE },
    /* OP_MAXIMUM_UNSIGNED              */ { "maximum_unsigned", 2, F_DEST | F_COMMUTATIVE },
    /* OP_ABSOLUTE                      */ { "absolute", 1, F_DEST },
    /* OP_MULTIPLY_HIGH_ROUNDING        */ { "multiply_high_rounding", 2, F_DEST | F_COMMUTATIVE },
    /* OP_SHIFT_RIGHT_ROUNDING          */ { "shift_right_rounding", 2, F_DEST },
    /* OP_DOT_PRODUCT                   */ { "dot_product", 3, F_DEST },
    /* OP_PAIRWISE_ADD                  */ { "pairwise_add", 2, F_DEST },
    /* OP_BROADCAST                     */ { "broadcast", 1, F_DEST },
    /* OP_SHUFFLE                       */ { "shuffle", 3, F_DEST },
    /* OP_INTERLEAVE                    */ { "interleave", 3, F_DEST },
    /* OP_SLIDE                         */ { "slide", 3, F_DEST },
    /* OP_NARROW                        */ { "narrow", 2, F_DEST },
    /* OP_COMPRESS                      */ { "compress", 2, F_DEST },
    /* OP_EXPAND                        */ { "expand", 2, F_DEST },
    /* OP_LANE_INDEX                    */ { "lane_index", 2, F_DEST },
    /* OP_ACTIVE_LANE_MASK              */ { "active_lane_mask", 2, F_DEST },
    /* OP_MASK_COUNT                    */ { "mask_count", 1, F_DEST },
    /* OP_MASK_FIRST                    */ { "mask_first", 1, F_DEST },
    /* OP_REDUCE                        */ { "reduce", 1, F_DEST },
    /* OP_SIGN_MASK                     */ { "sign_mask", 1, F_DEST },
    /* OP_AES_SUB_SHIFT                 */ { "aes_sub_shift", 1, F_DEST },
    /* OP_AES_INVERSE_SUB_SHIFT         */ { "aes_inverse_sub_shift", 1, F_DEST },
    /* OP_AES_MIX_COLUMNS               */ { "aes_mix_columns", 1, F_DEST },
    /* OP_AES_INVERSE_MIX_COLUMNS       */ { "aes_inverse_mix_columns", 1, F_DEST },
    /* OP_CRC32                         */ { "crc32", 2, F_DEST },
    /* OP_LABEL                         */ { "label", 1, F_SIDE_EFFECTS },
    /* OP_BRANCH                        */ { "branch", 1, F_CONTROL | F_SIDE_EFFECTS },
    /* OP_BRANCH_CONDITIONAL            */ { "branch_conditional", 2, F_CONTROL | F_SIDE_EFFECTS },
    /* OP_BRANCH_INDIRECT               */ { "branch_indirect", 1, F_CONTROL | F_SIDE_EFFECTS },
    /* OP_CALL                          */ { "call", 1, F_CONTROL | F_SIDE_EFFECTS },
    /* OP_RETURN                        */ { "return", 1, F_CONTROL | F_SIDE_EFFECTS },
    /* OP_TRAP                          */ { "trap", 1, F_CONTROL | F_SIDE_EFFECTS },
    /* OP_HINT                          */ { "hint", 0, 0 },
    /* OP_READ_SYSTEM_REGISTER          */ { "read_system_register", 1, F_DEST | F_SIDE_EFFECTS },
    /* OP_WRITE_SYSTEM_REGISTER         */ { "write_system_register", 2, F_SIDE_EFFECTS },
    /* OP_HELPER                        */ { "helper", 3, F_DEST | F_SIDE_EFFECTS },
    /* OP_UNDEFINED                     */ { "undefined", 0, F_SIDE_EFFECTS },
};

// The table must have an entry for every operation. A wrong size makes the
// array type below negative
typedef char gOperationsTableMatchesOperationsCount[
    (sizeof(gOperations) / sizeof(gOperations[0]) ==
     SemanticOperation::OP_COUNT) ? 1 : -1];

// Return the entry of an operation
const OperationInfo& getInfo(SemanticOperation::Operation operation)
{
    CHECK((uint)operation < (uint)SemanticOperation::OP_COUNT);
    return gOperations[operation];
}

} // namespace

const char* SemanticOperation::getName(Operation operation)
{
    return getInfo(operation).m_name;
}

uint SemanticOperation::getSourcesCount(Operation operation)
{
    return getInfo(operation).m_sources;
}

bool SemanticOperation::hasDestination(Operation operation)
{
    return (getInfo(operation).m_flags & F_DEST) != 0;
}

bool SemanticOperation::isCommutative(Operation operation)
{
    return (getInfo(operation).m_flags & F_COMMUTATIVE) != 0;
}

bool SemanticOperation::isMemoryAccess(Operation operation)
{
    return (getInfo(operation).m_flags & F_MEMORY) != 0;
}

bool SemanticOperation::isControlFlow(Operation operation)
{
    return (getInfo(operation).m_flags & F_CONTROL) != 0;
}

bool SemanticOperation::hasSideEffects(Operation operation)
{
    return (getInfo(operation).m_flags & F_SIDE_EFFECTS) != 0;
}

SemanticOperation::Condition SemanticOperation::invertCondition(
                                                        Condition condition)
{
    switch (condition)
    {
    case COND_NONE:                 return COND_NONE;
    case COND_ALWAYS:               return COND_NEVER;
    case COND_NEVER:                return COND_ALWAYS;
    case COND_EQUAL:                return COND_NOT_EQUAL;
    case COND_NOT_EQUAL:            return COND_EQUAL;
    case COND_SIGNED_LESS:          return COND_SIGNED_GREATER_EQUAL;
    case COND_SIGNED_GREATER_EQUAL: return COND_SIGNED_LESS;
    case COND_SIGNED_LESS_EQUAL:    return COND_SIGNED_GREATER;
    case COND_SIGNED_GREATER:       return COND_SIGNED_LESS_EQUAL;
    case COND_UNSIGNED_LESS:        return COND_UNSIGNED_GREATER_EQUAL;
    case COND_UNSIGNED_GREATER_EQUAL: return COND_UNSIGNED_LESS;
    case COND_UNSIGNED_LESS_EQUAL:  return COND_UNSIGNED_GREATER;
    case COND_UNSIGNED_GREATER:     return COND_UNSIGNED_LESS_EQUAL;
    case COND_NEGATIVE:             return COND_NOT_NEGATIVE;
    case COND_NOT_NEGATIVE:         return COND_NEGATIVE;
    case COND_OVERFLOW:             return COND_NOT_OVERFLOW;
    case COND_NOT_OVERFLOW:         return COND_OVERFLOW;
    case COND_CARRY:                return COND_NOT_CARRY;
    case COND_NOT_CARRY:            return COND_CARRY;
    case COND_PARITY_EVEN:          return COND_PARITY_ODD;
    case COND_PARITY_ODD:           return COND_PARITY_EVEN;
    case COND_FLOAT_ORDERED:        return COND_FLOAT_UNORDERED;
    case COND_FLOAT_UNORDERED:      return COND_FLOAT_ORDERED;
    case COND_FLOAT_EQUAL:          return COND_FLOAT_NOT_EQUAL;
    case COND_FLOAT_NOT_EQUAL:      return COND_FLOAT_EQUAL;
    case COND_FLOAT_GREATER_EQUAL:  return COND_FLOAT_UNORDERED_OR_LESS;
    case COND_FLOAT_UNORDERED_OR_LESS: return COND_FLOAT_GREATER_EQUAL;
    case COND_FLOAT_GREATER:        return COND_FLOAT_UNORDERED_OR_LESS_EQUAL;
    case COND_FLOAT_UNORDERED_OR_LESS_EQUAL: return COND_FLOAT_GREATER;
    default:
        // COND_FLOAT_LESS, COND_FLOAT_LESS_EQUAL and
        // COND_FLOAT_UNORDERED_OR_EQUAL: the inverse is not a condition
        CHECK_FAIL();
    }
}
