#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICTYPE_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICTYPE_H

/*
 * SemanticType.h
 *
 * The type of a value in the semantic IR: an integer, a floating point number
 * or a mask, as a scalar or as a vector of lanes.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"

/*
 * The type of a value in the semantic IR.
 *
 * The size of the value is a part of the type, not of the operation, so the
 * same operation works on all sizes:
 *   i8, i16, i32, i64, i128   - integer(bits)
 *   f32, f64, f80             - floating(FLOAT_SINGLE), ...
 *   v16i8, v4f32, v8f64       - vector(integer(8), 16), ...
 *   k16                       - mask(16), one bit per lane
 *   nxv4i32                   - scalable(vector(integer(32), 4)): 4 lanes
 *                               times the vector length multiple of the
 *                               processor (SVE, RVV), known only at runtime
 *
 * An integer has no signedness. The operation decides how its bits are used
 * (OP_DIVIDE_SIGNED, OP_SHIFT_RIGHT_ARITHMETIC, COND_SIGNED_LESS, ...).
 *
 * NOTE: This class is a value, it can be copied freely.
 */
class SemanticType {
public:
    // The kind of the value
    enum Kind {
        // No value. The type of the operations which don't return a value
        // (OP_STORE, OP_BRANCH...)
        KIND_VOID,
        // Two's complement integer of any size (a 1 bit integer is a boolean)
        KIND_INTEGER,
        // Floating point number, see FloatFormat
        KIND_FLOAT,
        // Predicate of a vector, one bit per lane (AVX-512 k registers, SVE
        // predicates)
        KIND_MASK
    };

    // The format of a floating point number
    enum FloatFormat {
        // Not a floating point number
        FLOAT_NONE,
        // 16 bit brain floating point
        FLOAT_BFLOAT16,
        // IEEE-754 binary16
        FLOAT_HALF,
        // IEEE-754 binary32
        FLOAT_SINGLE,
        // IEEE-754 binary64
        FLOAT_DOUBLE,
        // x87 80 bit extended precision
        FLOAT_EXTENDED,
        // IEEE-754 binary128
        FLOAT_QUAD,
        // 8 bit floating point, 4 bits exponent and 3 bits mantissa (OCP
        // FP8 E4M3: AVX10.2, ARM FP8)
        FLOAT_E4M3,
        // 8 bit floating point, 5 bits exponent and 2 bits mantissa (OCP
        // FP8 E5M2)
        FLOAT_E5M2
    };

    /*
     * Default constructor. Construct a KIND_VOID type
     */
    SemanticType();

    /*
     * Construct a type
     *
     * kind        - The kind of the value
     * elementBits - The size of a single lane in bits
     * lanes       - The number of lanes, 1 for a scalar
     * format      - The format of a KIND_FLOAT value
     */
    SemanticType(Kind kind,
                 uint elementBits,
                 uint lanes = 1,
                 FloatFormat format = FLOAT_NONE);

    // Return an integer scalar type of 'bits' size
    static SemanticType integer(uint bits);
    // Return a floating point scalar type
    static SemanticType floating(FloatFormat format);
    // Return a vector of 'lanes' elements of 'element' type
    static SemanticType vector(const SemanticType& element, uint lanes);
    // Return a mask of 'lanes' bits
    static SemanticType mask(uint lanes);
    // Return 'type' with a lanes count which is a multiple of the runtime
    // vector length (see isScalable())
    static SemanticType scalable(const SemanticType& type);

    // Return the kind of the type
    Kind getKind() const;
    // Return the floating point format, FLOAT_NONE for other kinds
    FloatFormat getFloatFormat() const;
    // Return the size of a single lane in bits
    uint getElementBits() const;
    // Return the number of lanes, 1 for a scalar
    uint getLanes() const;
    // Return the size of the whole value in bits (element bits * lanes)
    uint getTotalBits() const;
    // Return the type of a single lane
    SemanticType getElementType() const;

    // Return true for a value with more than one lane
    bool isVector() const;
    // Return true for KIND_VOID
    bool isVoid() const;
    // Return true if the lanes count is the minimum count, multiplied at
    // runtime by the vector length of the processor (SVE, RVV). getLanes()
    // and getTotalBits() then return the minimum
    bool isScalable() const;

    // Compare two types
    bool operator == (const SemanticType& other) const;
    bool operator != (const SemanticType& other) const;

private:
    // See Kind
    uint8 m_kind;
    // See FloatFormat
    uint8 m_floatFormat;
    // See isScalable()
    bool m_isScalable;
    // The size of a single lane in bits
    uint16 m_elementBits;
    // The number of lanes
    uint16 m_lanes;
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICTYPE_H
