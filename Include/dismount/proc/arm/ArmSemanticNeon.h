#ifndef __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICNEON_H
#define __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICNEON_H

/*
 * ArmSemanticNeon.h
 *
 * The lifters of the AArch32 Advanced SIMD integer instructions, the
 * structure loads and stores and the cryptographic extensions, declared for
 * the semantic table ArmSemanticTableNeon.inc. Implementation:
 * ArmSemanticLiftNeon.cpp (arithmetic, shifts, permutes),
 * ArmSemanticLiftNeonMemory.cpp (vld1-4, vst1-4) and
 * ArmSemanticLiftCrypto.cpp (aes, sha1, sha256, the dot products and
 * matrix multiplies).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/proc/arm/ArmSemantics.h"

namespace armdis {
namespace a32 {

/*
 * The data type of the mnemonic of an Advanced SIMD instruction: the
 * "s16" of "vqmovn.s16", the "8" of "vcnt.8". kind is 's', 'u', 'i', 'p' or
 * 0 (no letter), bits the number after it; base is the mnemonic before the
 * first dot ("vqmovn")
 */
struct NeonName {
    explicit NeonName(const ArmLiftContext& context);

    char m_base[24];
    char m_kind;
    uint m_bits;

    // True for 's' (the signed types)
    bool isSigned() const { return m_kind == 's'; }
    // True if the base mnemonic is 'name'
    bool is(const char* name) const;
};

/*
 * The IR builder of the Advanced SIMD lifters: typed integer lanes of the
 * registers, and the saturation flag QC (bit 27 of fpscr) which the
 * saturating instructions accumulate.
 */
class NeonBuilder {
public:
    explicit NeonBuilder(ArmLiftContext& context);

    ArmLiftContext& m_context;
    // The 1 bit "some lane saturated" accumulated by orSaturation()
    SemanticOperand m_saturation;

    static SemanticType type(uint bits);
    SemanticOperand imm(int64 value, uint bits);
    // The operation in a new temporary of integer(bits)
    SemanticOperand op(SemanticOperation::Operation operation, uint bits,
                       const SemanticOperand& a,
                       const SemanticOperand& b = SemanticOperand(),
                       const SemanticOperand& c = SemanticOperand());
    // 'value' as an integer of 'bits': truncated, sign or zero extended
    SemanticOperand ext(const SemanticOperand& value, uint bits, bool isSigned);
    // A 1 bit temporary: a <condition> b (integers of the type of a)
    SemanticOperand compare(SemanticOperation::Condition condition,
                            const SemanticOperand& a, const SemanticOperand& b);
    // A copy of the value in a temporary (a source which the instruction
    // overwrites before it is read again)
    SemanticOperand copy(const SemanticOperand& value);

    // The lane 'index' of 'bits' bits of a register or a temporary value
    SemanticOperand lane(const SemanticOperand& value, uint index, uint bits);
    // The lanes (all of 'bits' bits) in one integer
    SemanticOperand pack(const SemanticOperand* lanes, uint count, uint bits);
    // 'value' with the lane 'index' replaced
    SemanticOperand insert(const SemanticOperand& value, uint index,
                           uint bits, const SemanticOperand& lane);

    // Return the value of the register operand 'index' of the instruction
    SemanticOperand reg(uint index);
    // The size of the register operand in bits (64: d, 128: q)
    uint regBits(uint index);

    // The saturation: the bounds are in the type of 'value' (an integer of
    // 64 bits holding the exact value). Return the clamped value
    SemanticOperand clampSigned(const SemanticOperand& value, int64 low,
                                int64 high);
    SemanticOperand clampUnsigned(const SemanticOperand& value, uint64 high);
    // QC |= bit
    void orSaturation(const SemanticOperand& bit);
    // Write the accumulated saturation into fpscr
    void commitSaturation();
};

// Return the signed range of an integer of 'bits' bits
int64 signedMaximum(uint bits);
int64 signedMinimum(uint bits);
uint64 unsignedMaximum(uint bits);

// The arithmetic and compare lifters (ArmSemanticLiftNeon.cpp)
void liftNeonBinary(ArmLiftContext& context);
void liftNeonLogic(ArmLiftContext& context);
void liftNeonUnary(ArmLiftContext& context);
void liftNeonLong(ArmLiftContext& context);
void liftNeonNarrowHigh(ArmLiftContext& context);
void liftNeonSaturatingDouble(ArmLiftContext& context);
void liftNeonShiftImmediate(ArmLiftContext& context);
void liftNeonShiftRegister(ArmLiftContext& context);
void liftNeonNarrowMove(ArmLiftContext& context);
void liftNeonPairwise(ArmLiftContext& context);
void liftNeonPermute(ArmLiftContext& context);
void liftNeonTable(ArmLiftContext& context);
void liftNeonDuplicate(ArmLiftContext& context);
void liftNeonImmediateLogic(ArmLiftContext& context);
void liftNeonEstimate(ArmLiftContext& context);

// The structure loads and stores (ArmSemanticLiftNeonMemory.cpp)
void liftNeonStructure(ArmLiftContext& context);

// The dot products, matrix multiplies and cryptography
// (ArmSemanticLiftCrypto.cpp)
void liftNeonDot(ArmLiftContext& context);
void liftNeonMatrixMultiply(ArmLiftContext& context);
void liftNeonCryptoAes(ArmLiftContext& context);
void liftNeonCryptoSha(ArmLiftContext& context);
void liftNeonPolynomial64(ArmLiftContext& context);

} // namespace a32
} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARMSEMANTICNEON_H
