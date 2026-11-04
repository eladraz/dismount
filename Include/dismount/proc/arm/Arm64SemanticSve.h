#ifndef __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICSVE_H
#define __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICSVE_H

/*
 * Arm64SemanticSve.h
 *
 * The parts of the A64 lifter of SVE, SVE2 and SVE2.1 (Arm64SemanticLiftSve*.cpp)
 * and the helpers they share. Not a public header: the lifter is
 * ArmSemanticLifter (ArmSemantics.h).
 *
 * The vector length is known when the instruction is lifted (the VL of the
 * Arm64Architecture), so the IR holds concrete types: a z register is read as
 * a vector of VL / e lanes of e bits and a predicate as an integer of VL / 8
 * bits (one bit for each byte of the vector). A lane is active when the bit
 * of the predicate at its first byte is set; the other bits are ignored.
 * A write of a z register or a predicate zeroes the bits above VL, as the
 * registers of the description are as wide as the larger of VL and SVL.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64SemanticInternal.h"

namespace armdis {
namespace a64 {

/*
 * The helper of the SVE lifters: the vector length, the types, and the reads
 * and writes of the z and p registers
 */
class SveLift {
public:
    // Constructor
    explicit SveLift(ArmLiftContext& context);

    // The context of the instruction
    ArmLiftContext& context() { return m_context; }
    // VL in bits, and the number of bits of a predicate
    uint vectorBits() const { return m_vectorBits; }
    uint predicateBits() const { return m_vectorBits / 8; }
    // The number of lanes of 'elementBits' bit elements
    uint lanes(uint elementBits) const { return m_vectorBits / elementBits; }

    /*
     * The types: the integer vector, the floating point vector (16, 32 and 64
     * bits; the bfloat16 vector with 'isBrain'), the lane mask, and the
     * integer vector of VL bits
     */
    SemanticType integerType(uint elementBits) const;
    SemanticType floatType(uint elementBits, bool isBrain = false) const;
    SemanticType maskType(uint elementBits) const;
    SemanticType wholeType() const;
    SemanticType predicateType() const;

    // The element bits of a decoded operand's arrangement (b, h, s, d, q), 0
    // if none
    static uint elementBitsOf(const ArmDecodedOperand& operand);
    // The z / p register number (0-31, 0-15) of a decoded operand
    static uint registerNumber(const ArmDecodedOperand& operand);
    // True if the operand is a z / a p register
    static bool isZ(const ArmDecodedOperand& operand);
    static bool isP(const ArmDecodedOperand& operand);

    /*
     * Registers. readZ() gives the register as a vector of 'elementBits'
     * lanes, readZWhole() as an integer of VL bits. writeZ() takes a value
     * of VL bits of any type
     */
    SemanticOperand readZ(uint number, uint elementBits);
    SemanticOperand readZWhole(uint number);
    void writeZ(uint number, const SemanticOperand& value);
    // The predicate register as an integer of VL / 8 bits, and written
    SemanticOperand readPredicateBits(uint number);
    void writePredicateBits(uint number, const SemanticOperand& value);
    SemanticOperand readFfr();
    void writeFfr(const SemanticOperand& value);
    // The predicate as a mask of the lanes of 'elementBits' bit elements
    SemanticOperand readLaneMask(uint number, uint elementBits);
    // Spread a lane mask into a predicate of VL / 8 bits (the bit of the
    // first byte of each lane) and write it
    SemanticOperand maskToPredicate(const SemanticOperand& mask,
                                    uint elementBits);
    void writeLaneMask(uint number, const SemanticOperand& mask,
                       uint elementBits);
    // The bits of the predicate at the first byte of each element, as an
    // integer of VL / 8 bits
    SemanticOperand elementPredicate(const SemanticOperand& predicate,
                                     uint elementBits);
    // The bit positions of the first byte of each element, as a mask
    uint64 elementBitsMask(uint elementBits) const;

    /*
     * Predicate as counter (SVE2.1, SME2: pn8-pn15, the p registers): the
     * low bit set among bits 3-0 of the register is the element size
     * marker (sz: b, h, s, d), the bits 8 .. sz + 1 the count of the first
     * elements of the sequence of the registers of the instruction that are
     * active (inactive if bit 15, the invert bit, is set; the width of the
     * count is log2(VL / 8) + 2 - sz bits). counterPredicate() is the
     * predicate of the vector 'vectorIndex' of the sequence (VL / 8 bits,
     * the bit of the first byte of each element of the size of the
     * counter); an instruction reads it as a predicate of its element size
     * (laneMaskOfBits). counterActive() counts the active elements of
     * 'elementBits' elements in 'vectors' vectors. encodeCounter() is the
     * register value of 'active' elements out of 'total' (a 64 bit integer):
     * a descending count has its active elements at the end of the sequence
     */
    SemanticOperand counterPredicate(uint number, uint vectorIndex);
    SemanticOperand laneMaskOfBits(const SemanticOperand& predicateBits,
                                   uint elementBits);
    SemanticOperand counterActive(uint number, uint elementBits, uint vectors);
    SemanticOperand encodeCounter(const SemanticOperand& active, uint total,
                                  uint elementBits, bool isDescending);

    // The operands of the decoded instruction
    SemanticOperand readZOperand(uint index);
    SemanticOperand readZOperand(uint index, uint elementBits);
    // The lane mask of predicate operand 'index', for 'elementBits' bit
    // elements
    SemanticOperand readMaskOperand(uint index, uint elementBits);
    // The element 'z1.s[2]' of the operand 'index', as a scalar
    SemanticOperand readZElement(uint index);
    // A general purpose register operand, as 'bits' (32 or 64) integer
    SemanticOperand readGprOperand(uint index, uint bits);
    void writeGprOperand(uint index, const SemanticOperand& value);

    // Value helpers
    SemanticOperand integer(int64 value, uint bits);
    SemanticOperand zero(const SemanticType& type);
    // Every lane = the scalar 'value'
    SemanticOperand splat(const SemanticOperand& value, const SemanticType& type);
    SemanticOperand splatInteger(int64 value, uint elementBits);
    // The inactive lanes of the mask: 'inactive'
    SemanticOperand select(const SemanticOperand& mask,
                           const SemanticOperand& active,
                           const SemanticOperand& inactive);
    // Reinterpret the bits as 'type' (same size)
    SemanticOperand bitcast(const SemanticOperand& value, const SemanticType& type);
    SemanticOperand zeroExtend(const SemanticOperand& value, uint bits);
    SemanticOperand truncate(const SemanticOperand& value, uint bits);
    // A scalar lane 'lane' of the integer vector, and the vector with the
    // lane replaced
    SemanticOperand extractLane(const SemanticOperand& vector, uint lane);
    SemanticOperand insertLane(const SemanticOperand& vector,
                               const SemanticOperand& value, uint lane);
    // Generic emit. emitBehavior() sets the Behavior of the operation
    // (the division by zero...)
    SemanticOperand emitBehavior(SemanticOperation::Operation operation,
                                 const SemanticType& type,
                                 const SemanticOperand& a,
                                 const SemanticOperand& b,
                                 SemanticOperation::Behavior behavior,
                                 const SemanticOperand& c = SemanticOperand());
    SemanticOperand emit(SemanticOperation::Operation operation,
                         const SemanticType& type,
                         const SemanticOperand& a = SemanticOperand(),
                         const SemanticOperand& b = SemanticOperand(),
                         const SemanticOperand& c = SemanticOperand());
    // A lane by lane compare of vectors of 'type' (integer or floating
    // point): a mask with a bit for each lane
    SemanticOperand compareLanes(SemanticOperation::Condition condition,
                                 const SemanticType& type,
                                 const SemanticOperand& a,
                                 const SemanticOperand& b);
    // A scalar compare, one bit
    SemanticOperand compareScalar(SemanticOperation::Condition condition,
                                  const SemanticOperand& a,
                                  const SemanticOperand& b);
    // The lane mask as a vector of 1 and 0 elements and back
    SemanticOperand maskToVector(const SemanticOperand& mask, uint elementBits);
    SemanticOperand vectorToMask(const SemanticOperand& vector, uint elementBits);
    // The lane numbers 0, 1, 2... of the vector, plus 'start', times 'step'
    SemanticOperand laneIndex(int64 start, int64 step, uint elementBits);
    // The same for a vector of 'lanes' lanes of 'elementBits' bits
    SemanticOperand laneIndexWide(int64 start, int64 step, uint lanes,
                                  uint elementBits);
    // A floating point operation: the attributes of the exceptions
    SemanticOperand emitFloat(SemanticOperation::Operation operation,
                              const SemanticType& type,
                              const SemanticOperand& a = SemanticOperand(),
                              const SemanticOperand& b = SemanticOperand(),
                              const SemanticOperand& c = SemanticOperand(),
                              SemanticOperation::Behavior behavior =
                                SemanticOperation::BEHAVIOR_DEFAULT);

    /*
     * The result of a predicated instruction: the active lanes of 'mask'
     * get 'computed', the others the old value of the destination (merging)
     * or zero (zeroing)
     */
    SemanticOperand predicate(const SemanticOperand& mask,
                              const SemanticOperand& computed,
                              const SemanticOperand& old, bool isZeroing);

    /*
     * Set N Z C V from the result of a predicate-setting instruction (the
     * Arm ARM PredTest): 'governing' selects the elements, 'result' is the
     * predicate, both integers of VL / 8 bits
     */
    void setPredicateFlags(const SemanticOperand& governing,
                           const SemanticOperand& result, uint elementBits);

    // Return the integer maximum / minimum of the element
    static int64 maximumSigned(uint bits);
    static int64 minimumSigned(uint bits);

    // The architecture's floating point control for the DN rule
    SemanticOperand applyDefaultNan(const SemanticOperand& result,
                                    const SemanticType& type);

    // The dismount lifter says the instruction can't be lifted
    void undefined();

private:
    ArmLiftContext& m_context;
    uint m_vectorBits;
};

/*
 * A handler of a mnemonic and its parameters (m_a, m_b: the operation, the
 * flags...), and the tables of them. runSveTable() runs the handler of the
 * mnemonic of the instruction, or lifts it to OP_UNDEFINED if the table has
 * none
 */
struct SveDescriptor;
typedef void (*SveHandler)(SveLift& lift, const SveDescriptor& descriptor);
struct SveDescriptor {
    const char* m_name;
    SveHandler m_handler;
    int m_a;
    int m_b;
};
void runSveTable(ArmLiftContext& context, const SveDescriptor* table);

/*
 * The lifters of the mnemonics, per file. Each is the m_lift of the entries
 * of gArm64SveSemanticTable
 */
// Arm64SemanticLiftSve.cpp: integer arithmetic, logic, shifts, compares, moves
void liftSveInteger(ArmLiftContext& context);
// Arm64SemanticLiftSvePredicate.cpp: predicates, counts, while, reductions,
// permutes
void liftSvePredicate(ArmLiftContext& context);
// Arm64SemanticLiftSveFloat.cpp: floating point
void liftSveFloat(ArmLiftContext& context);
// Arm64SemanticLiftSveMemory.cpp: loads, stores, prefetches, first fault
void liftSveMemory(ArmLiftContext& context);
// Arm64SemanticLiftSve2.cpp: SVE2 integer
void liftSve2(ArmLiftContext& context);
// Arm64SemanticLiftSve2Wide.cpp: SVE2 widening, narrowing, dot products
void liftSve2Wide(ArmLiftContext& context);
// Arm64SemanticLiftSveFloat2.cpp: the remaining floating point
void liftSveFloat2(ArmLiftContext& context);
// Arm64SemanticLiftSveQuad.cpp: SVE2.1 quadword
void liftSveQuad(ArmLiftContext& context);
// Arm64SemanticLiftSveCrypto.cpp: AES, SM4, rax1, lookup tables
void liftSveCrypto(ArmLiftContext& context);
// Arm64SemanticLiftSveMisc.cpp: predicate as counter, SVE2.1 / 2.2 / 2.3 rest
void liftSveMisc(ArmLiftContext& context);

} // namespace a64
} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICSVE_H
