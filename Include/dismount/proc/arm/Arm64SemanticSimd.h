#ifndef __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICSIMD_H
#define __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICSIMD_H

/*
 * Arm64SemanticSimd.h
 *
 * The parts of the A64 lifter of the scalar floating point, the Advanced SIMD
 * and the cryptographic instructions (gArm64SimdEncodingTable), and the helpers
 * they share. Not a public header: the lifter is ArmSemanticLifter
 * (ArmSemantics.h).
 *
 *   Arm64SemanticLiftSimd.cpp       the dispatcher, the Advanced SIMD integer
 *                                   instructions
 *   Arm64SemanticLiftFloat.cpp      the scalar and vector floating point, the
 *                                   conversions
 *   Arm64SemanticLiftSimdMemory.cpp the structure loads and stores
 *   Arm64SemanticLiftCrypto.cpp     AES, SHA, SM3, SM4, PMULL
 *
 * The lifters decode the operands of the instruction (ArmDecodedOperand: the
 * arrangement, the element index, the immediates) and the encoding bits where
 * the operands don't carry the information. The vector values of the IR are
 * integer vectors (the lanes of the arrangement); the floating point
 * operations bitcast them (SimdLift::fp). Complicated per lane semantics
 * (saturation, rounding shifts, permutes) are written per lane on scalars.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/ArmSemantics.h"

namespace armdis {

/*
 * The lifter of the mnemonics of gArm64SimdEncodingTable. Every entry of
 * gArm64SimdSemanticTable is this function. Each group lifter returns true if
 * it handled the instruction.
 */
void liftArm64Simd(ArmLiftContext& context);

class SimdLift;

// Arm64SemanticLiftSimd.cpp: the Advanced SIMD integer, logic, shifts,
// permutes, narrow and widen instructions
bool liftSimdInteger(SimdLift& s);
// Arm64SemanticLiftFloat.cpp: scalar and vector floating point, the
// conversions with the integers
bool liftSimdFloat(SimdLift& s);
// Arm64SemanticLiftSimdMemory.cpp: ld1-ld4, st1-st4 and the replicate and
// single structure forms
bool liftSimdMemory(SimdLift& s);
// Arm64SemanticLiftCrypto.cpp: AES, SHA1, SHA256, SHA512, SHA3, SM3, SM4
bool liftSimdCrypto(SimdLift& s);

/*
 * The shape of a SIMD and floating point operand: a vector register with an
 * arrangement (v0.4s), an element of a vector register (v0.s[1]) or a scalar
 * register (b0, h0, s0, d0, q0)
 */
struct SimdShape {
    // The size of an element (or of the scalar) in bits
    uint m_elementBits;
    // The number of lanes: 1 for a scalar and an element
    uint m_lanes;
    // The size of the value in bits (the lane count times the element size)
    uint m_totalBits;
    // The operand is a vector arrangement (v0.4s)
    bool m_isVector;
    // The operand is an element (v0.s[1])
    bool m_isElement;
    // The operand is a scalar register (s0, d0)
    bool m_isScalar;
};

/*
 * The helpers of the lifters: the types, the operations on integer vectors
 * and scalars, the floating point operations with the exception flags, the
 * saturation flag.
 */
class SimdLift {
public:
    typedef SemanticOperand Val;

    explicit SimdLift(ArmLiftContext& context);

    ArmLiftContext& c() { return m_context; }
    // The mnemonic of the encoding entry ("fadd", "sqdmulh")
    const char* name() const { return m_name; }
    // The encoding bits
    uint32 encoding() const { return m_encoding; }
    // Return true if the mnemonic is 'name'
    bool is(const char* name) const;
    // Return true if the mnemonic starts with 'prefix'
    bool startsWith(const char* prefix) const;
    // Return bits 'high'..'low' of the encoding
    uint32 bits(uint high, uint low) const;
    uint count() const { return m_context.getOperandsCount(); }
    const ArmDecodedOperand& operand(uint index) const
    {
        return m_context.getOperand(index);
    }

    // The shape of the register operand 'index'
    SimdShape shape(uint index) const;
    // Return true if the register operand 'index' is a scalar register
    bool isScalar(uint index) const;

    //////////////////////////////////////////////////////////////////////
    // Types
    //////////////////////////////////////////////////////////////////////

    static SemanticType I(uint bits);
    // Integer lanes of 'elementBits' bits: an integer for one lane
    static SemanticType V(uint elementBits, uint lanes);
    // The floating point format of 16 / 32 / 64 bits
    static SemanticType::FloatFormat format(uint bits);
    static SemanticType F(uint bits);
    static SemanticType FV(uint elementBits, uint lanes);
    static SemanticType M(uint lanes);

    //////////////////////////////////////////////////////////////////////
    // Values
    //////////////////////////////////////////////////////////////////////

    Val imm(int64 value, const SemanticType& type);
    // An operation on the types
    Val op(SemanticOperation::Operation operation, const SemanticType& type,
           const Val& a, const Val& b = Val(), const Val& c = Val());
    // Reinterpret 'value' as 'type' (the same size). Does nothing if it has
    // the type already
    Val cast(const SemanticType& type, const Val& value);
    // Resize an integer
    Val zext(uint bits, const Val& value);
    Val sext(uint bits, const Val& value);
    Val trunc(uint bits, const Val& value);
    // The lane 'index' of the vector (an integer of the lane size)
    Val lane(const Val& vector, uint index);
    // 'vector' with the lane 'index' replaced
    Val setLane(const Val& vector, const Val& scalar, uint index);
    // Every lane of a vector of 'type' is 'scalar'
    Val splat(const SemanticType& type, const Val& scalar);
    Val splat(const SemanticType& type, int64 value);
    // The lanes of the mask as all ones / zeros lanes
    Val maskToLanes(const Val& mask, uint elementBits, uint lanes);
    // A compare of two values of 'type' (vector or scalar) with a result
    // mask (a bit for each lane), or a lanes vector (maskToLanes)
    Val compare(SemanticOperation::Condition condition, const SemanticType& type,
                const Val& a, const Val& b);
    Val compareLanes(SemanticOperation::Condition condition, uint elementBits,
                     uint lanes, const Val& a, const Val& b);
    // 'condition' (a 1 bit) ? a : b
    Val select(const Val& condition, const Val& a, const Val& b);
    // The low / high 64 bits of a 128 bit value
    Val low64(const Val& value);
    Val high64(const Val& value);
    // The 128 bit value of two halves
    Val join64(const Val& low, const Val& high);
    // A 1 bit: the value (up to 128 bits) is not zero
    Val nonZero(const Val& value);

    //////////////////////////////////////////////////////////////////////
    // Registers
    //////////////////////////////////////////////////////////////////////

    // Read the whole 128 bit register v<number>
    Val readV(uint number);
    // Write the register v<number> (zeroes the rest of z). The value is an
    // integer or a vector of up to 128 bits
    void writeV(uint number, const Val& value);
    // The register number of the register operand 'index'
    uint registerNumber(uint index) const;
    // The register number (0-31) of a register id of the SIMD and floating
    // point registers (v, q, d, s, h, b, z)
    uint registerNumberOf(uint registerId) const;

    //////////////////////////////////////////////////////////////////////
    // Floating point and the saturation flag
    //////////////////////////////////////////////////////////////////////

    /*
     * A floating point operation on integer values (scalars or vectors of
     * the same lane size, the bits of floating point numbers). The result is
     * an integer value of the same type as 'a'. The exception flags of the
     * guest are updated (FPSR) and the rounding mode is the one of FPCR
     */
    Val fp(SemanticOperation::Operation operation, const Val& a,
           const Val& b = Val(), const Val& c = Val(),
           SemanticOperation::Behavior behavior = SemanticOperation::BEHAVIOR_DEFAULT,
           SemanticOperation::RoundingMode rounding = SemanticOperation::ROUND_DYNAMIC,
           uint kind = 0);
    // A floating point compare of integer values: a mask (a bit per lane)
    Val fpCompare(SemanticOperation::Condition condition, const Val& a,
                  const Val& b, SemanticOperation::Behavior behavior);
    // The immediate source of the fixed point conversions (invalid for none)
    Val fractionOperand(uint fractionBits);
    // A conversion from floating point lanes to integer lanes of
    // 'integerBits' bits
    Val fpToInteger(const Val& a, uint integerBits, bool isSigned,
                    SemanticOperation::RoundingMode rounding, uint fractionBits);
    // A conversion from integer lanes to floating point lanes of
    // 'floatBits' bits
    Val integerToFp(const Val& a, uint floatBits, bool isSigned,
                    uint fractionBits);
    // A conversion between the floating point precisions
    Val fpConvert(const Val& a, uint toBits,
                  SemanticOperation::RoundingMode rounding);
    // A conversion between two floating point formats (the BFloat16 ones
    // included) of integer lanes
    Val fpConvertFormat(const Val& a, SemanticType::FloatFormat fromFormat,
                        SemanticType::FloatFormat toFormat,
                        SemanticOperation::RoundingMode rounding);
    // The BFloat16 dot product step of ARM: the single precision addend (a
    // 32 bit integer) plus the dot product of the two BFloat16 pairs (32 bit
    // integers)
    Val bfloatDot(const Val& addend, const Val& a, const Val& b);
    // A fused multiply-add a * b + c of scalars where a and b are numbers of
    // 'narrowFormat' (16 bit integers) and c and the result are floating point
    // numbers of 'wideBits' (ARM FMLAL, BFMLALB...)
    Val fpMultiplyAddWidening(const Val& a, const Val& b, const Val& c,
                              SemanticType::FloatFormat narrowFormat,
                              uint wideBits);
    // Set the cumulative saturation flag FPSR.QC if the 1 bit is set
    void setSaturation(const Val& isSaturated);
    // Set the bit 'bit' of FPSR (IOC 0, DZC 1, OFC 2, UFC 3, IXC 4, IDC 7,
    // QC 27) if the 1 bit 'condition' is set
    void setFpsrBit(uint bit, const Val& condition);

private:
    ArmLiftContext& m_context;
    const char* m_name;
    uint32 m_encoding;
};

} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICSIMD_H
