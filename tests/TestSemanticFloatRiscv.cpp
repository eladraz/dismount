/*
 * TestSemanticFloatRiscv.cpp
 *
 * Tests the RISC-V rules of the floating point operations of the semantic
 * layer, through the interpreter and the environment of RiscvArchitecture:
 * the 3 bit rounding field of fcsr with its 5 modes (rne, rtz, rdn, rup,
 * rmm), the canonical positive NaN for every NaN result, the accumulated
 * flags of fflags (no flag for a denormal operand), the tininess detected
 * after rounding, the saturating conversions to integers (a NaN gives the
 * largest positive value), fmin / fmax (minimumNumber, -0 < +0) and fminm /
 * fmaxm (the NaN propagates), and the invalid operation of infinity * zero
 * + a quiet NaN.
 *
 * The expected values follow the RISC-V specification (the F and D chapters
 * and the Zfa chapter), they were computed by hand; the reference processors
 * check the same behavior in tests/riscv/semantics.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/stream/ioStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/semantics/Semantics.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"

using namespace riscvdis;

typedef SemanticOperation Op;

// The bits of fflags
enum {
    NX = 0x01, UF = 0x02, OF = 0x04, DZ = 0x08, NV = 0x10
};

// The values
static const uint64 D_ONE      = 0x3FF0000000000000ULL;
static const uint64 D_TWO      = 0x4000000000000000ULL;
static const uint64 D_NAN      = 0x7FF8000000000000ULL;
static const uint64 D_ULP_HALF = 0x3CA0000000000000ULL;   // 2^-53
static const uint64 D_ONE_UP   = 0x3FF0000000000001ULL;
static const uint64 D_INF      = 0x7FF0000000000000ULL;
static const uint64 D_MINUS_ZERO = 0x8000000000000000ULL;
static const uint64 S_ONE      = 0x3F800000ULL;
static const uint64 S_NAN      = 0x7FC00000ULL;

// The behavior of the vector is the one of the instruction
struct RiscvFloatVector {
    // The operation, the format (32 or 64 bits), the rounding mode (the
    // frm value of fcsr, the instruction uses the dynamic mode)
    Op::Operation m_operation;
    uint m_bits;
    uint m_frm;
    // For the conversions to integers: the integer size and signedness, and
    // the sources
    uint m_integerBits;
    bool m_isSigned;
    Op::Behavior m_behavior;
    uint64 m_a;
    uint64 m_b;
    uint64 m_c;
    // The result: the pattern of the format, or the integer (low bits)
    uint64 m_result;
    // The accumulated fflags
    uint m_flags;
};

#define ADD(bits, frm, a, b, result, flags) \
    { Op::OP_FLOAT_ADD, bits, frm, 0, false, Op::BEHAVIOR_DEFAULT, a, b, 0, result, flags }
#define MUL(bits, frm, a, b, result, flags) \
    { Op::OP_FLOAT_MULTIPLY, bits, frm, 0, false, Op::BEHAVIOR_DEFAULT, a, b, 0, result, flags }
#define DIV(bits, frm, a, b, result, flags) \
    { Op::OP_FLOAT_DIVIDE, bits, frm, 0, false, Op::BEHAVIOR_DEFAULT, a, b, 0, result, flags }
#define SQRT(bits, frm, a, result, flags) \
    { Op::OP_FLOAT_SQUARE_ROOT, bits, frm, 0, false, Op::BEHAVIOR_DEFAULT, a, 0, 0, result, flags }
#define FMA(bits, frm, a, b, c, result, flags) \
    { Op::OP_FLOAT_MULTIPLY_ADD, bits, frm, 0, false, Op::BEHAVIOR_DEFAULT, a, b, c, result, flags }
#define MIN_NUMBER(bits, a, b, result, flags) \
    { Op::OP_FLOAT_MINIMUM, bits, 0, 0, false, Op::BEHAVIOR_MINMAX_NUMBER, a, b, 0, result, flags }
#define MAX_NUMBER(bits, a, b, result, flags) \
    { Op::OP_FLOAT_MAXIMUM, bits, 0, 0, false, Op::BEHAVIOR_MINMAX_NUMBER, a, b, 0, result, flags }
#define MIN_NAN(bits, a, b, result, flags) \
    { Op::OP_FLOAT_MINIMUM, bits, 0, 0, false, Op::BEHAVIOR_MINMAX_PROPAGATE_NAN, a, b, 0, result, flags }
#define MAX_NAN(bits, a, b, result, flags) \
    { Op::OP_FLOAT_MAXIMUM, bits, 0, 0, false, Op::BEHAVIOR_MINMAX_PROPAGATE_NAN, a, b, 0, result, flags }
#define TO_INT(bits, frm, integerBits, isSigned, a, result, flags) \
    { isSigned ? Op::OP_FLOAT_TO_SIGNED : Op::OP_FLOAT_TO_UNSIGNED, bits, frm, integerBits, isSigned, \
      Op::BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM, a, 0, 0, result, flags }
#define FROM_INT(bits, frm, integerBits, isSigned, a, result, flags) \
    { isSigned ? Op::OP_SIGNED_TO_FLOAT : Op::OP_UNSIGNED_TO_FLOAT, bits, frm, integerBits, isSigned, \
      Op::BEHAVIOR_DEFAULT, a, 0, 0, result, flags }

static const RiscvFloatVector gVectors[] = {
    // The five rounding modes: 1 + 2^-53 is a tie between 1 and 1 + 2^-52
    ADD(64, 0, D_ONE, D_ULP_HALF, D_ONE, NX),
    ADD(64, 1, D_ONE, D_ULP_HALF, D_ONE, NX),
    ADD(64, 2, D_ONE, D_ULP_HALF, D_ONE, NX),
    ADD(64, 3, D_ONE, D_ULP_HALF, D_ONE_UP, NX),
    ADD(64, 4, D_ONE, D_ULP_HALF, D_ONE_UP, NX),
    // The same, negative
    ADD(64, 0, 0xBFF0000000000000ULL, 0xBCA0000000000000ULL, 0xBFF0000000000000ULL, NX),
    ADD(64, 1, 0xBFF0000000000000ULL, 0xBCA0000000000000ULL, 0xBFF0000000000000ULL, NX),
    ADD(64, 2, 0xBFF0000000000000ULL, 0xBCA0000000000000ULL, 0xBFF0000000000001ULL, NX),
    ADD(64, 3, 0xBFF0000000000000ULL, 0xBCA0000000000000ULL, 0xBFF0000000000000ULL, NX),
    ADD(64, 4, 0xBFF0000000000000ULL, 0xBCA0000000000000ULL, 0xBFF0000000000001ULL, NX),
    // Exact: no flags
    ADD(64, 0, D_ONE, D_ONE, D_TWO, 0),
    // The result of a NaN is the canonical NaN: the payload and the sign of
    // a quiet NaN are dropped, a signaling NaN raises the invalid flag
    ADD(64, 0, 0x7FF8000000000123ULL, D_ONE, D_NAN, 0),
    ADD(64, 0, D_ONE, 0xFFF8000000000123ULL, D_NAN, 0),
    ADD(64, 0, 0x7FF0000000000001ULL, D_ONE, D_NAN, NV),
    ADD(64, 0, 0xFFF4000000000000ULL, 0x7FF8000000000001ULL, D_NAN, NV),
    ADD(64, 0, D_INF, 0xFFF0000000000000ULL, D_NAN, NV),
    ADD(32, 0, 0x7FC00001ULL, S_ONE, S_NAN, 0),
    ADD(32, 0, 0xFFC00001ULL, S_ONE, S_NAN, 0),
    ADD(32, 0, 0x7F800001ULL, S_ONE, S_NAN, NV),
    ADD(32, 3, S_ONE, 0x33800000ULL, 0x3F800001ULL, NX),
    ADD(32, 0, S_ONE, 0x33800000ULL, S_ONE, NX),
    // A denormal operand raises no flag (and is not flushed)
    ADD(64, 0, 0x0000000000000001ULL, 0, 0x0000000000000001ULL, 0),
    ADD(64, 0, 0x000FFFFFFFFFFFFFULL, 0x0000000000000001ULL, 0x0010000000000000ULL, 0),
    // The tininess is detected after rounding: 2^-1022 (1 - 2^-104) rounds
    // to 2^-1022 with a 53 bit significand, so it is not tiny (only nx) in
    // the nearest mode; rounding toward zero is tiny and inexact
    MUL(64, 0, 0x2000000000000001ULL, 0x1FFFFFFFFFFFFFFEULL, 0x0010000000000000ULL, NX),
    MUL(64, 1, 0x2000000000000001ULL, 0x1FFFFFFFFFFFFFFEULL, 0x000FFFFFFFFFFFFFULL, UF | NX),
    // Overflow and underflow, exact results
    MUL(64, 0, 0x7FEFFFFFFFFFFFFFULL, D_TWO, D_INF, OF | NX),
    MUL(64, 1, 0x7FEFFFFFFFFFFFFFULL, D_TWO, 0x7FEFFFFFFFFFFFFFULL, OF | NX),
    MUL(64, 0, 0x0010000000000000ULL, 0x3FE0000000000000ULL, 0x0008000000000000ULL, 0),
    // Divide by zero, and 0 / 0
    DIV(64, 0, D_ONE, 0, D_INF, DZ),
    DIV(64, 0, 0xBFF0000000000000ULL, 0, 0xFFF0000000000000ULL, DZ),
    DIV(64, 0, 0, 0, D_NAN, NV),
    SQRT(64, 0, 0xBFF0000000000000ULL, D_NAN, NV),
    SQRT(64, 0, D_MINUS_ZERO, D_MINUS_ZERO, 0),
    SQRT(64, 0, 0x4010000000000000ULL, D_TWO, 0),
    // infinity * 0 + a quiet NaN is an invalid operation
    FMA(64, 0, D_INF, 0, 0x7FF8000000000123ULL, D_NAN, NV),
    FMA(64, 0, D_ONE, D_ONE, D_ONE, D_TWO, 0),
    FMA(64, 0, D_ONE, D_ONE, 0x7FF8000000000123ULL, D_NAN, 0),

    // fmin / fmax: the number of a NaN and a number, a signaling NaN is
    // invalid, -0 < +0, both NaN give the canonical NaN
    MIN_NUMBER(64, 0x7FF8000000000123ULL, D_ONE, D_ONE, 0),
    MIN_NUMBER(64, D_ONE, 0x7FF8000000000123ULL, D_ONE, 0),
    MIN_NUMBER(64, 0x7FF0000000000001ULL, D_ONE, D_ONE, NV),
    MAX_NUMBER(64, D_ONE, 0x7FF0000000000001ULL, D_ONE, NV),
    MIN_NUMBER(64, 0, D_MINUS_ZERO, D_MINUS_ZERO, 0),
    MIN_NUMBER(64, D_MINUS_ZERO, 0, D_MINUS_ZERO, 0),
    MAX_NUMBER(64, 0, D_MINUS_ZERO, 0, 0),
    MAX_NUMBER(64, D_MINUS_ZERO, 0, 0, 0),
    MIN_NUMBER(64, 0x7FF8000000000123ULL, 0xFFF8000000000456ULL, D_NAN, 0),
    MAX_NUMBER(64, 0x7FF0000000000001ULL, 0x7FF8000000000456ULL, D_NAN, NV),
    MIN_NUMBER(64, D_ONE, D_TWO, D_ONE, 0),
    MAX_NUMBER(64, D_ONE, D_TWO, D_TWO, 0),
    MIN_NUMBER(32, 0x7FC00001ULL, S_ONE, S_ONE, 0),
    MAX_NUMBER(32, 0x7F800001ULL, S_ONE, S_ONE, NV),
    // fminm / fmaxm (Zfa): the NaN propagates, canonical, a signaling NaN is
    // invalid
    MIN_NAN(64, 0x7FF8000000000123ULL, D_ONE, D_NAN, 0),
    MAX_NAN(64, D_ONE, 0xFFF8000000000123ULL, D_NAN, 0),
    MIN_NAN(64, 0x7FF0000000000001ULL, D_ONE, D_NAN, NV),
    MAX_NAN(64, D_ONE, 0x7FF0000000000001ULL, D_NAN, NV),
    MIN_NAN(64, 0, D_MINUS_ZERO, D_MINUS_ZERO, 0),
    MAX_NAN(64, D_MINUS_ZERO, 0, 0, 0),
    MIN_NAN(64, D_ONE, D_TWO, D_ONE, 0),
    MAX_NAN(32, S_ONE, 0x40000000ULL, 0x40000000ULL, 0),

    // The conversions to integers saturate; a NaN is the largest positive
    // value; the rounding is the instruction's
    TO_INT(64, 0, 32, true, 0x7FF8000000000000ULL, 0x7FFFFFFFULL, NV),
    TO_INT(64, 0, 32, true, 0xFFF8000000000000ULL, 0x7FFFFFFFULL, NV),
    TO_INT(64, 0, 32, true, 0x7FF0000000000001ULL, 0x7FFFFFFFULL, NV),
    TO_INT(64, 0, 32, true, 0x44B52D02C7E14AF6ULL, 0x7FFFFFFFULL, NV),
    TO_INT(64, 0, 32, true, 0xC4B52D02C7E14AF6ULL, 0x80000000ULL, NV),
    TO_INT(64, 0, 32, true, D_INF, 0x7FFFFFFFULL, NV),
    TO_INT(64, 0, 32, true, 0xFFF0000000000000ULL, 0x80000000ULL, NV),
    TO_INT(64, 0, 32, true, 0x4004000000000000ULL, 2, NX),
    TO_INT(64, 1, 32, true, 0x4004000000000000ULL, 2, NX),
    TO_INT(64, 2, 32, true, 0x4004000000000000ULL, 2, NX),
    TO_INT(64, 3, 32, true, 0x4004000000000000ULL, 3, NX),
    TO_INT(64, 4, 32, true, 0x4004000000000000ULL, 3, NX),
    TO_INT(64, 0, 32, true, 0xC004000000000000ULL, 0xFFFFFFFEULL, NX),
    TO_INT(64, 1, 32, true, 0xC004000000000000ULL, 0xFFFFFFFEULL, NX),
    TO_INT(64, 2, 32, true, 0xC004000000000000ULL, 0xFFFFFFFDULL, NX),
    TO_INT(64, 3, 32, true, 0xC004000000000000ULL, 0xFFFFFFFEULL, NX),
    TO_INT(64, 4, 32, true, 0xC004000000000000ULL, 0xFFFFFFFDULL, NX),
    TO_INT(64, 0, 32, true, 0x4000000000000000ULL, 2, 0),
    TO_INT(64, 0, 32, false, 0xBFF0000000000000ULL, 0, NV),
    TO_INT(64, 0, 32, false, 0x7FF8000000000000ULL, 0xFFFFFFFFULL, NV),
    TO_INT(64, 1, 32, false, 0xBFD3333333333333ULL, 0, NX),
    TO_INT(64, 0, 32, false, 0x41E65A0BC0000000ULL, 0xB2D05E00ULL, 0),
    TO_INT(64, 0, 32, true, 0x41E65A0BC0000000ULL, 0x7FFFFFFFULL, NV),
    TO_INT(64, 0, 64, true, 0x43E0000000000000ULL, 0x7FFFFFFFFFFFFFFFULL, NV),
    TO_INT(64, 0, 64, true, 0xC3E0000000000000ULL, 0x8000000000000000ULL, 0),
    TO_INT(64, 0, 64, false, 0x43F0000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, NV),
    TO_INT(32, 0, 32, true, S_NAN, 0x7FFFFFFFULL, NV),
    TO_INT(32, 4, 32, true, 0x40200000ULL, 3, NX),
    // The conversions from integers
    FROM_INT(64, 0, 64, true, 0x0020000000000001ULL, 0x4340000000000000ULL, NX),
    FROM_INT(64, 3, 64, true, 0x0020000000000001ULL, 0x4340000000000001ULL, NX),
    FROM_INT(64, 0, 32, true, 0xFFFFFFFFULL, 0xBFF0000000000000ULL, 0),
    FROM_INT(64, 0, 32, false, 0xFFFFFFFFULL, 0x41EFFFFFFFE00000ULL, 0),
    FROM_INT(32, 0, 32, true, 0x01000001ULL, 0x4B800000ULL, NX),
    FROM_INT(32, 3, 32, true, 0x01000001ULL, 0x4B800001ULL, NX)
};

class TestObjectTestSemanticFloatRiscv : public cTestObject {
public:
    TestObjectTestSemanticFloatRiscv() : m_failures(0) {}

    // Load the register as a float of the vector's format
    static SemanticOperand loadFloat(SemanticBlock& block, uint registerId,
                                     const SemanticType& type)
    {
        SemanticOperand value = SemanticOperand::registerOperand(
                                    registerId, SemanticType::integer(64));
        if (type.getTotalBits() == 32)
        {
            SemanticOperand low = block.newTemporary(SemanticType::integer(32));
            block.append(SemanticInstruction(Op::OP_TRUNCATE,
                    SemanticType::integer(32), low, value));
            value = low;
        }
        SemanticOperand result = block.newTemporary(type);
        block.append(SemanticInstruction(Op::OP_BITCAST, type, result, value));
        return result;
    }

    // Run a vector: return false and report on a difference
    void run(uint index, const RiscvFloatVector& v)
    {
        ArchitectureDescriptionPtr architecture(new RiscvArchitecture(
                RiscvInstructionSet::RISCV_RV64, 128));
        const SemanticType type = SemanticType::floating(
                (v.m_bits == 32) ? SemanticType::FLOAT_SINGLE :
                                   SemanticType::FLOAT_DOUBLE);
        const bool isToInteger = (v.m_operation == Op::OP_FLOAT_TO_SIGNED) ||
                                 (v.m_operation == Op::OP_FLOAT_TO_UNSIGNED);
        const bool isFromInteger = (v.m_operation == Op::OP_SIGNED_TO_FLOAT) ||
                                   (v.m_operation == Op::OP_UNSIGNED_TO_FLOAT);

        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        SemanticOperand a, b, c;
        if (isFromInteger)
        {
            // The integer operand is x5 (the integer type of the vector)
            a = SemanticOperand::registerOperand(RISCV_REG_X0 + 5,
                                                 SemanticType::integer(64));
            if (v.m_integerBits < 64)
            {
                SemanticOperand low = block.newTemporary(
                                    SemanticType::integer(v.m_integerBits));
                block.append(SemanticInstruction(Op::OP_TRUNCATE,
                        SemanticType::integer(v.m_integerBits), low, a));
                a = low;
            }
        } else
        {
            a = loadFloat(block, RISCV_REG_F0, type);
            b = loadFloat(block, RISCV_REG_F0 + 1, type);
            c = loadFloat(block, RISCV_REG_F0 + 2, type);
        }

        const SemanticType resultType = isToInteger ?
                SemanticType::integer(v.m_integerBits) : type;
        SemanticOperand result = block.newTemporary(resultType);
        SemanticInstruction instruction(v.m_operation, resultType, result,
                                        a, b, c);
        instruction.setRoundingMode(Op::ROUND_DYNAMIC);
        instruction.setBehavior(v.m_behavior);
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
        block.append(instruction);

        // The pattern, zero extended into x6
        SemanticOperand pattern = result;
        if (!isToInteger)
        {
            pattern = block.newTemporary(SemanticType::integer(v.m_bits));
            block.append(SemanticInstruction(Op::OP_BITCAST,
                    SemanticType::integer(v.m_bits), pattern, result));
        }
        if (pattern.getType().getTotalBits() < 64)
        {
            SemanticOperand wide = block.newTemporary(SemanticType::integer(64));
            block.append(SemanticInstruction(Op::OP_ZERO_EXTEND,
                    SemanticType::integer(64), wide, pattern));
            pattern = wide;
        }
        block.append(SemanticInstruction(Op::OP_MOVE, SemanticType::integer(64),
                SemanticOperand::registerOperand(RISCV_REG_X0 + 6,
                                                 SemanticType::integer(64)),
                pattern));
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, 0x1000));

        class NoMemory : public SemanticMemory {
        public:
            virtual bool read(uint64, uint8*, uint) { return false; }
            virtual bool write(uint64, const uint8*, uint) { return false; }
        } memory;
        SemanticInterpreter interpreter(architecture, memory);
        interpreter.setRegister(RISCV_REG_F0, SemanticValue(v.m_a));
        interpreter.setRegister(RISCV_REG_F0 + 1, SemanticValue(v.m_b));
        interpreter.setRegister(RISCV_REG_F0 + 2, SemanticValue(v.m_c));
        interpreter.setRegister(RISCV_REG_X0 + 5, SemanticValue(v.m_a));
        interpreter.setRegister(RISCV_REG_FCSR, SemanticValue((uint64)v.m_frm << 5));
        const SemanticInterpreter::ExitReason reason = interpreter.run(block);

        uint64 got = interpreter.getRegister(RISCV_REG_X0 + 6).getWord(0);
        const uint64 flags = interpreter.getRegister(RISCV_REG_FCSR).getWord(0);
        if (reason != SemanticInterpreter::EXIT_FALL_THROUGH)
        {
            fprintf(stderr, "TestSemanticFloatRiscv: vector %u: exit %u\n",
                    (unsigned int)index, (unsigned int)reason);
            m_failures++;
            return;
        }
        uint64 want = v.m_result;
        const uint bits = isToInteger ? v.m_integerBits : v.m_bits;
        if (bits < 64)
        {
            got &= (((uint64)1 << bits) - 1);
            want &= (((uint64)1 << bits) - 1);
        }
        if ((got != want) || ((flags & 0x1F) != v.m_flags) ||
            ((flags >> 5) != v.m_frm))
        {
            fprintf(stderr, "TestSemanticFloatRiscv: vector %u (op %u, %u bits, "
                    "frm %u, a=%llx b=%llx c=%llx): result %llx fflags %llx, "
                    "expected %llx fflags %x\n", (unsigned int)index, (unsigned int)v.m_operation,
                    (unsigned int)v.m_bits, (unsigned int)v.m_frm, (unsigned long long)v.m_a,
                    (unsigned long long)v.m_b, (unsigned long long)v.m_c,
                    (unsigned long long)got, (unsigned long long)(flags & 0x1F),
                    (unsigned long long)want, (unsigned int)v.m_flags);
            m_failures++;
        }
    }

    virtual void test()
    {
        const uint count = sizeof(gVectors) / sizeof(gVectors[0]);
        for (uint i = 0; i < count; i++)
            run(i, gVectors[i]);
        cout << count << " RISC-V floating point vectors run, "
             << m_failures << " failures" << endl;
        TESTS_ASSERT(m_failures == 0);
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    uint m_failures;
};

// Instance test object
TestObjectTestSemanticFloatRiscv g_globalTestSemanticFloatRiscv;
