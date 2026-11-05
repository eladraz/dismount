#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICFLOAT_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICFLOAT_H

/*
 * SemanticFloat.h
 *
 * A software floating point library: bit exact IEEE-754 (and x87) arithmetic
 * on integers only, so the results are the same on every host. It is the
 * arithmetic of SemanticInterpreter for the OP_FLOAT_* operations.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/ArchitectureDescription.h"

/*
 * The floating point operations. A number is held as its raw bits (Bits) in
 * one of the formats of SemanticType::FloatFormat:
 *
 *   FLOAT_HALF, FLOAT_BFLOAT16, FLOAT_SINGLE, FLOAT_DOUBLE, FLOAT_QUAD
 *       IEEE-754 interchange formats (bfloat16 is a truncated single).
 *   FLOAT_EXTENDED
 *       The x87 80 bit format: sign and 15 bit exponent in m_high, a 64 bit
 *       significand with an explicit integer bit in m_low. The pseudo
 *       denormals, unnormals, pseudo infinities and pseudo NaNs are invalid
 *       encodings: an operation on them is an invalid operation, like on the
 *       real x87.
 *
 * FLOAT_E4M3 and FLOAT_E5M2 are not supported.
 *
 * Every operation takes an Environment (the rounding mode, the x87 precision
 * control, flush to zero, the NaN rules) and ORs the SemanticFloatFlags it
 * raises into 'flags'. The flags are the exceptions of the operation, whether
 * the guest masks them or not: a masked exception gives the IEEE default
 * result, which is the returned value.
 *
 * Tininess is detected after rounding (x86, and ARM as the IR defines it), see
 * Environment::m_isTininessBeforeRounding.
 *
 * NOTE: No host floating point is used.
 */
class SemanticFloat {
public:
    typedef SemanticType::FloatFormat Format;

    // The raw bits of a number of up to 128 bits
    struct Bits {
        // Bits 0..63
        uint64 m_low;
        // Bits 64..127
        uint64 m_high;

        Bits(uint64 low = 0, uint64 high = 0) :
            m_low(low), m_high(high) {};

        bool operator == (const Bits& other) const
        {
            return (m_low == other.m_low) && (m_high == other.m_high);
        }
        bool operator != (const Bits& other) const
        {
            return !(*this == other);
        }
    };

    // The result of a compare
    enum Relation {
        RELATION_LESS,
        RELATION_EQUAL,
        RELATION_GREATER,
        // At least one of the operands is a NaN
        RELATION_UNORDERED
    };

    // The bits of classify(): the class of the RISC-V fclass instruction
    enum Class {
        CLASS_NEGATIVE_INFINITY = 0x001,
        CLASS_NEGATIVE_NORMAL   = 0x002,
        CLASS_NEGATIVE_DENORMAL = 0x004,
        CLASS_NEGATIVE_ZERO     = 0x008,
        CLASS_POSITIVE_ZERO     = 0x010,
        CLASS_POSITIVE_DENORMAL = 0x020,
        CLASS_POSITIVE_NORMAL   = 0x040,
        CLASS_POSITIVE_INFINITY = 0x080,
        CLASS_SIGNALING_NAN     = 0x100,
        CLASS_QUIET_NAN         = 0x200
    };

    /*
     * The environment of an operation. The default is the IEEE-754 default
     * environment of x86 SSE: round to nearest even, no flushing, the first
     * NaN operand wins and the default NaN is negative.
     */
    struct Environment {
        Environment();

        // The rounding mode. ROUND_DYNAMIC is the same as nearest even (the
        // caller reads the guest register)
        SemanticOperation::RoundingMode m_rounding;
        // x87 precision control: the number of significand bits (24, 53 or
        // 64) of the add, subtract, multiply, divide, square root and
        // multiply-add with FLOAT_EXTENDED results. The exponent range stays
        // the extended one. 0 is the whole significand of the format
        uint m_precision;
        // Tiny results become zero (SSE FTZ, ARM FZ), with the underflow and
        // inexact flags
        bool m_flushToZero;
        // Denormal operands are treated as zeros (SSE DAZ), without the
        // denormal flag
        bool m_denormalsAreZero;
        // The guest doesn't mask the underflow exception: a tiny result raises
        // FLOAT_UNDERFLOW even if it's exact, and isn't flushed to zero
        bool m_isUnderflowUnmasked;
        // Detect tininess before rounding (the result of the exact operation
        // is smaller than the smallest normal number) instead of after
        // rounding to the precision of the format (x86)
        bool m_isTininessBeforeRounding;
        // See FloatEnvironment::NanRule
        uint8 m_nanRule;
        // The sign of the default NaN
        bool m_isDefaultNanNegative;
        // The ARM rule of the denormal operands: only a denormal operand
        // flushed to zero (m_denormalsAreZero) raises FLOAT_DENORMAL (ARM FZ:
        // IDC); the other denormal operands are silent. Otherwise (SSE) a
        // flushed operand is silent (DAZ) and the others raise the flag
        bool m_isInputFlushSignaled;
        // A result flushed to zero (m_flushToZero) raises FLOAT_INEXACT with
        // FLOAT_UNDERFLOW (SSE FTZ); ARM raises FLOAT_UNDERFLOW only
        bool m_isFlushInexact;
        // BEHAVIOR_MINMAX_NUMBER gives a NaN for a signaling NaN operand
        bool m_isMinMaxNanSignaling;
        // The half precision format has its own flush to zero control
        // (m_flushHalfToZero, ARM FZ16) instead of m_flushToZero /
        // m_denormalsAreZero
        bool m_isHalfFlushSeparate;
        bool m_flushHalfToZero;
        // The conversions from and to the half precision format use the
        // alternative format (ARM AHP): no infinities and NaNs
        bool m_isAlternativeHalf;
        // The NaN operand of a multiply-add is chosen in the order addend,
        // first and second multiplicand (ARM)
        bool m_isAddendNanFirst;
        // Rounding to odd turns an overflow into an infinity instead of the
        // largest finite number (the ARM BFloat16 dot product)
        bool m_isOddOverflowInfinity;
    };

    //////////////////////////////////////////////////////////////////////
    // Formats and encodings
    //////////////////////////////////////////////////////////////////////

    // Return true if the format is supported
    static bool isSupported(Format format);
    // Return the size of a number in bits (16, 32, 64, 80, 128)
    static uint getTotalBits(Format format);
    // Return the bits of the significand, with the integer bit (24 for
    // single, 64 for extended)
    static uint getPrecision(Format format);

    // Return true if the bits are a valid encoding (every IEEE-754 encoding is)
    static bool isValidEncoding(Format format, const Bits& value);
    static bool isNan(Format format, const Bits& value);
    static bool isSignalingNan(Format format, const Bits& value);
    static bool isInfinity(Format format, const Bits& value);
    static bool isZero(Format format, const Bits& value);
    static bool isDenormal(Format format, const Bits& value);
    static bool isNegative(Format format, const Bits& value);

    // Return a zero
    static Bits makeZero(Format format, bool isNegative);
    // Return an infinity
    static Bits makeInfinity(Format format, bool isNegative);
    // Return the default NaN of the environment
    static Bits makeDefaultNan(Format format, const Environment& environment);

    // Return the class of the number, the bits of Class (0 for an invalid
    // encoding). Raises no exception
    static uint classify(Format format, const Bits& value);

    //////////////////////////////////////////////////////////////////////
    // Arithmetic
    //////////////////////////////////////////////////////////////////////

    // a + b
    static Bits add(Format format, const Environment& environment,
                    const Bits& a, const Bits& b, uint32& flags);
    // a - b
    static Bits subtract(Format format, const Environment& environment,
                         const Bits& a, const Bits& b, uint32& flags);
    // a * b
    static Bits multiply(Format format, const Environment& environment,
                         const Bits& a, const Bits& b, uint32& flags);
    // a / b
    static Bits divide(Format format, const Environment& environment,
                       const Bits& a, const Bits& b, uint32& flags);
    // The square root of a
    static Bits squareRoot(Format format, const Environment& environment,
                           const Bits& a, uint32& flags);
    // a * b + c with a single rounding
    static Bits multiplyAdd(Format format, const Environment& environment,
                            const Bits& a, const Bits& b, const Bits& c,
                            uint32& flags);
    /*
     * a * b + c with a single rounding where a and b are numbers of the
     * format 'narrow' (unpacked with narrowEnvironment) and c and the result
     * are of the format 'wide' (ARM FMLAL: half precision products added to a
     * single precision accumulator)
     */
    static Bits multiplyAddWidening(Format narrow, Format wide,
                                    const Environment& narrowEnvironment,
                                    const Environment& wideEnvironment,
                                    const Bits& a, const Bits& b, const Bits& c,
                                    uint32& flags);
    /*
     * Round a to an integral value, in the same format
     *
     * isInexactSignaled - Raise FLOAT_INEXACT when the value changes
     *                     (x87 frndint, SSE roundsd without the suppress bit)
     */
    static Bits roundToIntegral(Format format, const Environment& environment,
                                const Bits& a, bool isInexactSignaled,
                                uint32& flags);
    // a * 2^floor(b), b is a number of the same format (vscalefps)
    static Bits scale(Format format, const Environment& environment,
                      const Bits& a, const Bits& b, uint32& flags);
    // The unbiased exponent of a as a number (vgetexpps): zero gives -infinity
    static Bits getExponent(Format format, const Environment& environment,
                            const Bits& a, uint32& flags);

    /*
     * The minimum or the maximum of a and b
     *
     * behavior - SemanticOperation::BEHAVIOR_MINMAX_*. The signed zeros order
     *            as -0 < +0 except for BEHAVIOR_MINMAX_SECOND, which returns b
     *            for any two equal values. BEHAVIOR_MINMAX_NUMBER returns
     *            the number of a number and a NaN (with FLOAT_INVALID for a
     *            signaling NaN) except under NAN_SIGNALING_FIRST (ARM
     *            FMINNM) where a signaling NaN gives a NaN
     */
    static Bits minimumMaximum(Format format, const Environment& environment,
                               bool isMaximum, SemanticOperation::Behavior behavior,
                               const Bits& a, const Bits& b, uint32& flags);

    //////////////////////////////////////////////////////////////////////
    // Sign operations (no exceptions, a NaN is not changed)
    //////////////////////////////////////////////////////////////////////

    static Bits absolute(Format format, const Bits& a);
    static Bits negate(Format format, const Bits& a);
    // a with the sign of b
    static Bits copySign(Format format, const Bits& a, const Bits& b);

    //////////////////////////////////////////////////////////////////////
    // Compare
    //////////////////////////////////////////////////////////////////////

    /*
     * Compare two numbers
     *
     * isSignaling - FLOAT_INVALID for a quiet NaN too (comisd, ordered
     *               relations). Otherwise only for a signaling NaN (ucomisd)
     */
    static Relation compare(Format format, const Environment& environment,
                            const Bits& a, const Bits& b, bool isSignaling,
                            uint32& flags);

    //////////////////////////////////////////////////////////////////////
    // Conversions
    //////////////////////////////////////////////////////////////////////

    // Convert a number of the format 'from' to the format 'to'
    static Bits convert(Format from, Format to, const Environment& environment,
                        const Bits& a, uint32& flags);

    /*
     * The estimates of the ARM reciprocal and reciprocal square root
     * instructions (FPRecipEstimate and FPRSqrtEstimate of the Arm ARM,
     * for the 16, 32 and 64 bit formats)
     */
    static Bits reciprocalEstimate(Format format, const Environment& environment,
                                   const Bits& a, uint32& flags);
    static Bits reciprocalSqrtEstimate(Format format,
                                       const Environment& environment,
                                       const Bits& a, uint32& flags);
    /*
     * The reciprocal step 2 - a * b (FPRecipStep) or, with isSqrt, the
     * reciprocal square root step (3 - a * b) / 2 (FPRSqrtStep). The AArch32
     * steps round the product (not fused). The A64 steps (isFused) are
     * fused: the first operand is negated before everything (a NaN too), and
     * 0 * infinity gives 2.0 (1.5)
     */
    static Bits reciprocalStep(Format format, const Environment& environment,
                               const Bits& a, const Bits& b, bool isSqrt,
                               bool isFused, uint32& flags);
    // a * b where 0 * infinity is 2.0 with the sign of the product (FMULX)
    static Bits multiplyExtended(Format format, const Environment& environment,
                                 const Bits& a, const Bits& b, uint32& flags);
    // FRECPX: the reciprocal exponent of a
    static Bits reciprocalExponent(Format format, const Environment& environment,
                                   const Bits& a, uint32& flags);
    /*
     * FRINT32Z/X and FRINT64Z/X: round to an integral value of the rounding
     * mode of the environment, and an infinity, a NaN or a value which
     * doesn't fit a signed integer of 'integerBits' (32 or 64) gives
     * -2^(integerBits - 1) with FLOAT_INVALID
     */
    static Bits roundToIntegerBounded(Format format,
                                      const Environment& environment,
                                      const Bits& a, uint integerBits,
                                      uint32& flags);
    /*
     * The BFloat16 dot product step of ARM (BFDOT, BFMMLA) with the standard
     * behaviors (FPCR.EBF = 0): addend + a0 * b0 + a1 * b1 where the addend
     * and the result are single precision and the other operands BFloat16.
     * The denormals are flushed, a NaN is the default NaN, no exception is
     * raised, FPCR is ignored; the sum of the products and the sum with the
     * addend are rounded to odd
     */
    static Bits bfloatDotAdd(const Bits& addend, const Bits& a0, const Bits& a1,
                             const Bits& b0, const Bits& b1);
    // URECPE / URSQRTE: the estimate of an unsigned 32 bit fixed point number
    static uint32 unsignedEstimate(uint32 value, bool isSqrt);
    /*
     * The estimates of the RISC-V vfrec7 and vfrsqrt7 (the V specification,
     * 13.9 and 13.10: 7 bits of accuracy by the tables of the specification,
     * for the 16, 32 and 64 bit formats). The rounding mode of the
     * environment decides the overflow of the reciprocal of a subnormal
     * number
     */
    static Bits riscvEstimate(Format format, const Environment& environment,
                              const Bits& a, bool isSqrt, uint32& flags);
    // a * 2^b where b is a signed integer of 'integerBits' bits (FSCALE)
    static Bits scaleByInteger(Format format, const Environment& environment,
                               const Bits& a, uint64 b, uint integerBits,
                               uint32& flags);

    /*
     * Convert an integer to a number
     *
     * integerBits - The size of the integer, 1 to 64 bits
     * value       - The integer, in its low bits
     * fractionBits - The number is value / 2^fractionBits (fixed point)
     */
    static Bits fromInteger(Format format, const Environment& environment,
                            uint64 value, uint integerBits, bool isSigned,
                            uint32& flags, uint fractionBits = 0);

    /*
     * Convert a number to an integer, rounded by the rounding mode of the
     * environment
     *
     * integerBits - The size of the integer, 1 to 64 bits
     * behavior    - SemanticOperation::BEHAVIOR_CONVERT_*, for a NaN and for
     *               the values out of range (FLOAT_INVALID). The indefinite
     *               value is the minimum for a signed integer and all ones for
     *               an unsigned one
     *
     * fractionBits - The integer is the number times 2^fractionBits (fixed
     *                 point)
     *
     * Return the integer in the low bits of the result
     */
    static uint64 toInteger(Format format, const Environment& environment,
                            const Bits& a, uint integerBits, bool isSigned,
                            SemanticOperation::Behavior behavior,
                            uint32& flags, uint fractionBits = 0);
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICFLOAT_H
