/*
 * TestSemantics.cpp
 *
 * Tests the semantic IR: the types and values, the evaluation of every
 * implemented operation and flags recipe, and the execution of blocks over
 * the IA-32 registers.
 *
 * The expected values were computed by hand. The IA-32 flags results follow
 * the processor.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/datastream.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/sarray.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/stream/ioStream.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/semantics/Semantics.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/IA32Semantics.h"

using namespace ia32dis;

typedef SemanticOperation Op;

// A memory of SIZE bytes at BASE. The accesses outside of it, or to the
// refused address, fail
class TestSemanticMemory : public SemanticMemory {
public:
    enum { BASE = 0x1000, SIZE = 0x1000 };

    TestSemanticMemory() :
        m_refused(0xFFFFFFFFFFFFFFFFULL),
        m_data(SIZE)
    {
        for (uint i = 0; i < SIZE; i++)
            m_data[i] = 0;
    }

    virtual bool read(uint64 address, uint8* buffer, uint size)
    {
        if (!isAccessible(address, size))
            return false;
        for (uint i = 0; i < size; i++)
            buffer[i] = m_data[(uint)(address - BASE) + i];
        return true;
    }

    virtual bool write(uint64 address, const uint8* buffer, uint size)
    {
        if (!isAccessible(address, size))
            return false;
        for (uint i = 0; i < size; i++)
            m_data[(uint)(address - BASE) + i] = buffer[i];
        return true;
    }

    uint8 getByte(uint64 address) const
    {
        return m_data[(uint)(address - BASE)];
    }

    // The address whose access fails
    uint64 m_refused;

private:
    bool isAccessible(uint64 address, uint size) const
    {
        if ((address < BASE) || (address + size > BASE + SIZE))
            return false;
        return !((m_refused >= address) && (m_refused < address + size));
    }

    cBuffer m_data;
};

class TestObjectTestSemantics : public cTestObject {
public:
    // Fail the test, with the values, if they are different
    void check(const char* what, uint64 actual, uint64 expected)
    {
        if (actual != expected)
        {
            cout << "FAILED " << what << ": got " << HEXQWORD(actual)
                 << " expected " << HEXQWORD(expected) << endl;
        }
        TESTS_ASSERT_EQUAL(actual, expected);
    }

    //////////////////////////////////////////////////////////////////////
    // Types and values
    //////////////////////////////////////////////////////////////////////

    void testTypes()
    {
        SemanticType none;
        TESTS_ASSERT(none.isVoid());
        TESTS_ASSERT_EQUAL(none.getTotalBits(), 0);

        SemanticType i32 = SemanticType::integer(32);
        check("i32 kind", i32.getKind(), SemanticType::KIND_INTEGER);
        check("i32 bits", i32.getTotalBits(), 32);
        check("i32 lanes", i32.getLanes(), 1);
        TESTS_ASSERT(!i32.isVector());
        TESTS_ASSERT(!i32.isVoid());
        TESTS_ASSERT(i32 == SemanticType::integer(32));
        TESTS_ASSERT(i32 != SemanticType::integer(33));

        SemanticType f64 = SemanticType::floating(SemanticType::FLOAT_DOUBLE);
        check("f64 kind", f64.getKind(), SemanticType::KIND_FLOAT);
        check("f64 bits", f64.getTotalBits(), 64);
        check("f80 bits", SemanticType::floating(
                    SemanticType::FLOAT_EXTENDED).getTotalBits(), 80);
        check("f64 format", f64.getFloatFormat(), SemanticType::FLOAT_DOUBLE);
        TESTS_ASSERT(f64 != SemanticType::integer(64));

        SemanticType v16i8 = SemanticType::vector(SemanticType::integer(8), 16);
        TESTS_ASSERT(v16i8.isVector());
        check("v16i8 bits", v16i8.getTotalBits(), 128);
        check("v16i8 element bits", v16i8.getElementBits(), 8);
        check("v16i8 lanes", v16i8.getLanes(), 16);
        TESTS_ASSERT(v16i8.getElementType() == SemanticType::integer(8));
        SemanticType v4f32 = SemanticType::vector(
                    SemanticType::floating(SemanticType::FLOAT_SINGLE), 4);
        check("v4f32 bits", v4f32.getTotalBits(), 128);
        check("v4f32 format", v4f32.getFloatFormat(),
              SemanticType::FLOAT_SINGLE);

        SemanticType k16 = SemanticType::mask(16);
        check("k16 kind", k16.getKind(), SemanticType::KIND_MASK);
        check("k16 bits", k16.getTotalBits(), 16);
        check("k16 lanes", k16.getLanes(), 16);

        SemanticType scalable = SemanticType::scalable(v4f32);
        TESTS_ASSERT(scalable.isScalable());
        TESTS_ASSERT(!v4f32.isScalable());
        TESTS_ASSERT(scalable != v4f32);
        check("scalable lanes", scalable.getLanes(), 4);

        SemanticOperand reg = SemanticOperand::registerOperand(IA32_REG_EAX,
                                                               i32);
        TESTS_ASSERT(reg.isValid());
        check("register index", reg.getIndex(), IA32_REG_EAX);
        TESTS_ASSERT(reg == SemanticOperand::registerOperand(IA32_REG_EAX, i32));
        TESTS_ASSERT(reg != SemanticOperand::registerOperand(IA32_REG_ECX, i32));
        TESTS_ASSERT(!SemanticOperand().isValid());
        check("immediate", (uint64)SemanticOperand::immediate(-2, i32).getValue(),
              0xFFFFFFFFFFFFFFFEULL);

        TESTS_ASSERT_EQUAL(Op::getSourcesCount(Op::OP_ADD), 2);
        TESTS_ASSERT_EQUAL(Op::getSourcesCount(Op::OP_SELECT), 3);
        TESTS_ASSERT(Op::hasDestination(Op::OP_ADD));
        TESTS_ASSERT(!Op::hasDestination(Op::OP_STORE));
        TESTS_ASSERT(Op::isCommutative(Op::OP_ADD));
        TESTS_ASSERT(!Op::isCommutative(Op::OP_SUBTRACT));
        TESTS_ASSERT(Op::isMemoryAccess(Op::OP_LOAD));
        TESTS_ASSERT(Op::isControlFlow(Op::OP_BRANCH));
        TESTS_ASSERT(!Op::isControlFlow(Op::OP_LABEL));
        TESTS_ASSERT(Op::hasSideEffects(Op::OP_SET_FLAGS));
        TESTS_ASSERT(!Op::hasSideEffects(Op::OP_ADD));
        TESTS_ASSERT_EQUAL(Op::invertCondition(Op::COND_SIGNED_LESS),
                           Op::COND_SIGNED_GREATER_EQUAL);
        TESTS_ASSERT_EQUAL(Op::invertCondition(Op::COND_EQUAL),
                           Op::COND_NOT_EQUAL);
        TESTS_ASSERT(cString(Op::getName(Op::OP_ADD)) == cString("add"));
    }

    void testValues()
    {
        // A field across the word boundary
        SemanticValue v;
        v.setBits(60, 8, 0xAB);
        check("cross word 0", v.getWord(0), 0xB000000000000000ULL);
        check("cross word 1", v.getWord(1), 0xA);
        check("cross get", v.getBits(60, 8), 0xAB);
        check("get high", v.getBits(64, 4), 0xA);
        check("get 64", v.getBits(0, 64), 0xB000000000000000ULL);
        // Overwriting keeps the neighbors
        v.setBits(62, 4, 0x0);
        check("overwrite word 0", v.getWord(0), 0x3000000000000000ULL);
        check("overwrite word 1", v.getWord(1), 0x8);
        // A 64 bit field at an unaligned offset
        SemanticValue w;
        w.setBits(32, 64, 0x1122334455667788ULL);
        check("unaligned word 0", w.getWord(0), 0x5566778800000000ULL);
        check("unaligned word 1", w.getWord(1), 0x11223344);
        check("unaligned get", w.getBits(32, 64), 0x1122334455667788ULL);
        // The top of the value
        SemanticValue top;
        top.setBits(SemanticValue::MAX_BITS - 8, 8, 0xFE);
        check("top", top.getWord(SemanticValue::WORDS_COUNT - 1),
              0xFE00000000000000ULL);

        // Truncate
        SemanticValue t;
        for (uint i = 0; i < SemanticValue::WORDS_COUNT; i++)
            t.setWord(i, ~(uint64)0);
        t.truncate(65);
        check("truncate 65 word 0", t.getWord(0), 0xFFFFFFFFFFFFFFFFULL);
        check("truncate 65 word 1", t.getWord(1), 1);
        check("truncate 65 word 2", t.getWord(2), 0);
        t.truncate(33);
        check("truncate 33 word 0", t.getWord(0), 0x1FFFFFFFFULL);
        check("truncate 33 word 1", t.getWord(1), 0);
        t.truncate(0);
        TESTS_ASSERT(t == SemanticValue());
        SemanticValue u(5);
        u.truncate(64);
        check("truncate 64", u.getWord(0), 5);
        TESTS_ASSERT(u != SemanticValue(6));
    }

    //////////////////////////////////////////////////////////////////////
    // evaluate()
    //////////////////////////////////////////////////////////////////////

    // An instruction whose operands have the types 't0'..'t2' (the type of
    // the operation when not given)
    static SemanticInstruction makeInstruction(Op::Operation operation,
                                    const SemanticType& type,
                                    SemanticType t0 = SemanticType(),
                                    SemanticType t1 = SemanticType(),
                                    SemanticType t2 = SemanticType())
    {
        if (t0.isVoid()) t0 = type;
        if (t1.isVoid()) t1 = type;
        if (t2.isVoid()) t2 = type;
        return SemanticInstruction(operation, type, SemanticOperand(),
                                   SemanticOperand::immediate(0, t0),
                                   SemanticOperand::immediate(0, t1),
                                   SemanticOperand::immediate(0, t2));
    }

    // Evaluate an integer operation. Return false if it fails
    static bool tryEval(const SemanticInstruction& instruction,
                        uint64 a, uint64 b, uint64 c, uint64& result)
    {
        SemanticValue sources[3] = { SemanticValue(a), SemanticValue(b),
                                     SemanticValue(c) };
        SemanticValue value;
        if (!SemanticInterpreter::evaluate(instruction, sources, value))
            return false;
        result = value.getWord(0);
        return true;
    }

    // Evaluate an operation on 'bits' bit integers
    uint64 eval(const char* what, Op::Operation operation, uint bits,
                uint64 a, uint64 b = 0, uint64 c = 0)
    {
        uint64 result = 0;
        if (!tryEval(makeInstruction(operation, SemanticType::integer(bits)),
                     a, b, c, result))
        {
            cout << "FAILED " << what << ": evaluate failed" << endl;
            TESTS_ASSERT(false);
        }
        return result;
    }

    void expect(const char* what, Op::Operation operation, uint bits,
                uint64 a, uint64 b, uint64 c, uint64 expected)
    {
        check(what, eval(what, operation, bits, a, b, c), expected);
    }

    // Division with a behavior. Return false if it faults
    static bool divide(Op::Operation operation, Op::Behavior behavior,
                       uint bits, uint64 a, uint64 b, uint64& result)
    {
        SemanticInstruction instruction = makeInstruction(operation,
                                                SemanticType::integer(bits));
        instruction.setBehavior(behavior);
        return tryEval(instruction, a, b, 0, result);
    }

    void testEvaluateInteger()
    {
        // Arithmetic, with the wrap around
        expect("add i8", Op::OP_ADD, 8, 0xFF, 0x01, 0, 0x00);
        expect("sub i8", Op::OP_SUBTRACT, 8, 0x00, 0x01, 0, 0xFF);
        expect("neg i8", Op::OP_NEGATE, 8, 0x01, 0, 0, 0xFF);
        expect("neg i32 0", Op::OP_NEGATE, 32, 0, 0, 0, 0);
        expect("mul i8 wrap", Op::OP_MULTIPLY, 8, 0x10, 0x10, 0, 0x00);
        expect("mul i8", Op::OP_MULTIPLY, 8, 0x0F, 0x11, 0, 0xFF);
        expect("mul i64", Op::OP_MULTIPLY, 64, 0xFFFFFFFFFFFFFFFFULL, 2, 0,
               0xFFFFFFFFFFFFFFFEULL);
        expect("adc i8", Op::OP_ADD_CARRY, 8, 0xFF, 0x00, 1, 0x00);
        expect("adc i8 no carry", Op::OP_ADD_CARRY, 8, 0xFF, 0x00, 0, 0xFF);
        expect("sbb i8", Op::OP_SUBTRACT_BORROW, 8, 0x00, 0x00, 1, 0xFF);
        expect("add i64", Op::OP_ADD, 64, 0xFFFFFFFFFFFFFFFFULL, 1, 0, 0);

        // Odd widths
        expect("add i9", Op::OP_ADD, 9, 0x1FF, 1, 0, 0);
        expect("not i9", Op::OP_NOT, 9, 0, 0, 0, 0x1FF);
        expect("add i33", Op::OP_ADD, 33, 0x1FFFFFFFFULL, 2, 0, 1);
        expect("add i1", Op::OP_ADD, 1, 1, 1, 0, 0);
        expect("rol i9", Op::OP_ROTATE_LEFT, 9, 0x100, 1, 0, 0x001);
        expect("rol i9 modulo", Op::OP_ROTATE_LEFT, 9, 0x100, 10, 0, 0x001);
        expect("ror i9", Op::OP_ROTATE_RIGHT, 9, 0x001, 1, 0, 0x100);
        expect("rol i33", Op::OP_ROTATE_LEFT, 33, 0x100000000ULL, 1, 0, 1);
        expect("shl i33", Op::OP_SHIFT_LEFT, 33, 0x100000000ULL, 1, 0, 0);
        expect("sar i33", Op::OP_SHIFT_RIGHT_ARITHMETIC, 33, 0x100000000ULL,
               32, 0, 0x1FFFFFFFFULL);
        expect("clz i9", Op::OP_COUNT_LEADING_ZEROS, 9, 0, 0, 0, 9);

        // Shifts: the counts 0, size - 1, size, size + 1
        expect("shl 0", Op::OP_SHIFT_LEFT, 32, 1, 0, 0, 1);
        expect("shl 31", Op::OP_SHIFT_LEFT, 32, 1, 31, 0, 0x80000000ULL);
        expect("shl 32", Op::OP_SHIFT_LEFT, 32, 1, 32, 0, 0);
        expect("shl 33", Op::OP_SHIFT_LEFT, 32, 1, 33, 0, 0);
        expect("shl 63", Op::OP_SHIFT_LEFT, 64, 1, 63, 0, 0x8000000000000000ULL);
        expect("shl 64", Op::OP_SHIFT_LEFT, 64, 1, 64, 0, 0);
        expect("shr 31", Op::OP_SHIFT_RIGHT_LOGICAL, 32, 0x80000000ULL, 31, 0, 1);
        expect("shr 32", Op::OP_SHIFT_RIGHT_LOGICAL, 32, 0x80000000ULL, 32, 0, 0);
        expect("sar 31", Op::OP_SHIFT_RIGHT_ARITHMETIC, 32, 0x80000000ULL, 31,
               0, 0xFFFFFFFFULL);
        expect("sar 32", Op::OP_SHIFT_RIGHT_ARITHMETIC, 32, 0x80000000ULL, 32,
               0, 0xFFFFFFFFULL);
        expect("sar 33", Op::OP_SHIFT_RIGHT_ARITHMETIC, 32, 0x80000000ULL, 33,
               0, 0xFFFFFFFFULL);
        expect("sar positive 32", Op::OP_SHIFT_RIGHT_ARITHMETIC, 32,
               0x7FFFFFFFULL, 32, 0, 0);
        expect("sar i8 negative", Op::OP_SHIFT_RIGHT_ARITHMETIC, 8, 0xF0, 4, 0,
               0xFF);
        expect("sar i8 positive", Op::OP_SHIFT_RIGHT_ARITHMETIC, 8, 0x70, 4, 0,
               0x07);
        expect("sar 64", Op::OP_SHIFT_RIGHT_ARITHMETIC, 64,
               0x8000000000000000ULL, 64, 0, 0xFFFFFFFFFFFFFFFFULL);
        expect("sar 63", Op::OP_SHIFT_RIGHT_ARITHMETIC, 64,
               0x8000000000000000ULL, 63, 0, 0xFFFFFFFFFFFFFFFFULL);

        // Rotates
        expect("rol 1", Op::OP_ROTATE_LEFT, 32, 0x80000001ULL, 1, 0, 3);
        expect("ror 1", Op::OP_ROTATE_RIGHT, 32, 0x80000001ULL, 1, 0,
               0xC0000000ULL);
        expect("rol 32", Op::OP_ROTATE_LEFT, 32, 0x80000001ULL, 32, 0,
               0x80000001ULL);
        expect("rol 33", Op::OP_ROTATE_LEFT, 32, 0x80000001ULL, 33, 0, 3);
        expect("rol i8 4", Op::OP_ROTATE_LEFT, 8, 0x81, 4, 0, 0x18);
        expect("ror 0", Op::OP_ROTATE_RIGHT, 32, 0x12345678, 0, 0, 0x12345678);
        expect("rol i64", Op::OP_ROTATE_LEFT, 64, 0x8000000000000001ULL, 1, 0, 3);

        // Bit counts
        expect("clz 0", Op::OP_COUNT_LEADING_ZEROS, 32, 0, 0, 0, 32);
        expect("clz 1", Op::OP_COUNT_LEADING_ZEROS, 32, 1, 0, 0, 31);
        expect("clz msb", Op::OP_COUNT_LEADING_ZEROS, 32, 0x80000000ULL, 0, 0, 0);
        expect("clz i8", Op::OP_COUNT_LEADING_ZEROS, 8, 0x10, 0, 0, 3);
        expect("clz i64 0", Op::OP_COUNT_LEADING_ZEROS, 64, 0, 0, 0, 64);
        expect("ctz 0", Op::OP_COUNT_TRAILING_ZEROS, 32, 0, 0, 0, 32);
        expect("ctz 8", Op::OP_COUNT_TRAILING_ZEROS, 32, 8, 0, 0, 3);
        expect("cls 0", Op::OP_COUNT_LEADING_SIGN_BITS, 8, 0x00, 0, 0, 7);
        expect("cls -1", Op::OP_COUNT_LEADING_SIGN_BITS, 8, 0xFF, 0, 0, 7);
        expect("cls 1", Op::OP_COUNT_LEADING_SIGN_BITS, 8, 0x01, 0, 0, 6);
        expect("cls min", Op::OP_COUNT_LEADING_SIGN_BITS, 8, 0x80, 0, 0, 0);
        expect("cls 0xC0", Op::OP_COUNT_LEADING_SIGN_BITS, 8, 0xC0, 0, 0, 1);
        expect("cls 0x40", Op::OP_COUNT_LEADING_SIGN_BITS, 8, 0x40, 0, 0, 0);
        expect("popcnt", Op::OP_POPULATION_COUNT, 32, 0xF0F0F0F0ULL, 0, 0, 16);
        expect("popcnt i64", Op::OP_POPULATION_COUNT, 64,
               0xFFFFFFFFFFFFFFFFULL, 0, 0, 64);
        expect("bswap 32", Op::OP_BYTE_SWAP, 32, 0x12345678, 0, 0, 0x78563412);
        expect("bswap 16", Op::OP_BYTE_SWAP, 16, 0x1234, 0, 0, 0x3412);
        expect("bswap 64", Op::OP_BYTE_SWAP, 64, 0x0102030405060708ULL, 0, 0,
               0x0807060504030201ULL);
        expect("bitrev 1", Op::OP_BIT_REVERSE, 8, 0x01, 0, 0, 0x80);
        expect("bitrev 0x0B", Op::OP_BIT_REVERSE, 8, 0x0B, 0, 0, 0xD0);
        expect("pdep", Op::OP_BIT_DEPOSIT, 32, 0x05, 0x1A, 0, 0x12);
        expect("pext", Op::OP_BIT_GATHER, 32, 0x16, 0x1A, 0, 0x05);

        // Logic
        expect("and_not", Op::OP_AND_NOT, 8, 0x0F, 0xFF, 0, 0xF0);
        expect("bitwise select", Op::OP_BITWISE_SELECT, 8, 0xF0, 0xAA, 0x55,
               0xA5);
        expect("xor", Op::OP_XOR, 16, 0xFF00, 0x0FF0, 0, 0xF0F0);

        // The high half of a product
        expect("mulh s32", Op::OP_MULTIPLY_HIGH_SIGNED, 32, 0xFFFFFFFFULL,
               0xFFFFFFFFULL, 0, 0);
        expect("mulh u32", Op::OP_MULTIPLY_HIGH_UNSIGNED, 32, 0xFFFFFFFFULL,
               0xFFFFFFFFULL, 0, 0xFFFFFFFEULL);
        expect("mulh su32", Op::OP_MULTIPLY_HIGH_SIGNED_UNSIGNED, 32,
               0xFFFFFFFFULL, 0xFFFFFFFFULL, 0, 0xFFFFFFFFULL);
        expect("mulh s32 min", Op::OP_MULTIPLY_HIGH_SIGNED, 32, 0x80000000ULL,
               0x80000000ULL, 0, 0x40000000ULL);
        expect("mulh s64", Op::OP_MULTIPLY_HIGH_SIGNED, 64,
               0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0, 0);
        expect("mulh u64", Op::OP_MULTIPLY_HIGH_UNSIGNED, 64,
               0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0,
               0xFFFFFFFFFFFFFFFEULL);
        expect("mulh su64", Op::OP_MULTIPLY_HIGH_SIGNED_UNSIGNED, 64,
               0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0,
               0xFFFFFFFFFFFFFFFFULL);
        expect("mulh s64 min", Op::OP_MULTIPLY_HIGH_SIGNED, 64,
               0x8000000000000000ULL, 0x8000000000000000ULL, 0,
               0x4000000000000000ULL);
        expect("mulh u64 big", Op::OP_MULTIPLY_HIGH_UNSIGNED, 64,
               0x100000000ULL, 0x100000000ULL, 0, 1);
        expect("mulh s8", Op::OP_MULTIPLY_HIGH_SIGNED, 8, 0x80, 0x7F, 0, 0xC0);

        // Division
        expect("sdiv", Op::OP_DIVIDE_SIGNED, 32, 0xFFFFFFF9ULL, 2, 0,
               0xFFFFFFFDULL);
        expect("srem", Op::OP_REMAINDER_SIGNED, 32, 0xFFFFFFF9ULL, 2, 0,
               0xFFFFFFFFULL);
        expect("udiv", Op::OP_DIVIDE_UNSIGNED, 32, 7, 2, 0, 3);
        expect("urem", Op::OP_REMAINDER_UNSIGNED, 32, 7, 2, 0, 1);
        expect("udiv big", Op::OP_DIVIDE_UNSIGNED, 64, 0xFFFFFFFFFFFFFFFFULL,
               2, 0, 0x7FFFFFFFFFFFFFFFULL);

        uint64 r;
        TESTS_ASSERT(!divide(Op::OP_DIVIDE_UNSIGNED, Op::BEHAVIOR_DIVIDE_FAULT,
                             32, 7, 0, r));
        TESTS_ASSERT(!divide(Op::OP_REMAINDER_SIGNED, Op::BEHAVIOR_DIVIDE_FAULT,
                             32, 7, 0, r));
        TESTS_ASSERT(!divide(Op::OP_DIVIDE_SIGNED, Op::BEHAVIOR_DIVIDE_FAULT,
                             8, 0x80, 0xFF, r));
        TESTS_ASSERT(!divide(Op::OP_DIVIDE_SIGNED, Op::BEHAVIOR_DIVIDE_FAULT,
                             64, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
                             r));
        TESTS_ASSERT(divide(Op::OP_DIVIDE_SIGNED, Op::BEHAVIOR_DIVIDE_FAULT,
                            8, 0x80, 0x01, r));
        check("min / 1", r, 0x80);
        TESTS_ASSERT(divide(Op::OP_DIVIDE_UNSIGNED, Op::BEHAVIOR_DIVIDE_ZERO,
                            32, 7, 0, r));
        check("zero: udiv by 0", r, 0);
        TESTS_ASSERT(divide(Op::OP_REMAINDER_UNSIGNED, Op::BEHAVIOR_DIVIDE_ZERO,
                            32, 7, 0, r));
        check("zero: urem by 0", r, 7);
        TESTS_ASSERT(divide(Op::OP_DIVIDE_SIGNED, Op::BEHAVIOR_DIVIDE_ZERO,
                            8, 0x80, 0xFF, r));
        check("zero: sdiv overflow", r, 0x80);
        TESTS_ASSERT(divide(Op::OP_REMAINDER_SIGNED, Op::BEHAVIOR_DIVIDE_ZERO,
                            8, 0x80, 0xFF, r));
        check("zero: srem overflow", r, 0);
        TESTS_ASSERT(divide(Op::OP_DIVIDE_UNSIGNED,
                            Op::BEHAVIOR_DIVIDE_ALL_ONES, 32, 7, 0, r));
        check("ones: udiv by 0", r, 0xFFFFFFFFULL);
        TESTS_ASSERT(divide(Op::OP_DIVIDE_SIGNED, Op::BEHAVIOR_DIVIDE_ALL_ONES,
                            32, 7, 0, r));
        check("ones: sdiv by 0", r, 0xFFFFFFFFULL);
        TESTS_ASSERT(divide(Op::OP_REMAINDER_SIGNED,
                            Op::BEHAVIOR_DIVIDE_ALL_ONES, 32, 7, 0, r));
        check("ones: srem by 0", r, 7);
        TESTS_ASSERT(divide(Op::OP_DIVIDE_SIGNED, Op::BEHAVIOR_DIVIDE_ALL_ONES,
                            8, 0x80, 0xFF, r));
        check("ones: sdiv overflow", r, 0x80);
        TESTS_ASSERT(divide(Op::OP_REMAINDER_SIGNED,
                            Op::BEHAVIOR_DIVIDE_ALL_ONES, 8, 0x80, 0xFF, r));
        check("ones: srem overflow", r, 0);

        // Absolute difference, carry-less multiply
        expect("absdiff s", Op::OP_ABSOLUTE_DIFFERENCE_SIGNED, 8, 0x80, 0x7F, 0,
               0xFF);
        expect("absdiff u", Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, 8, 0x05, 0x0A,
               0, 0x05);
        expect("clmul", Op::OP_CARRYLESS_MULTIPLY, 8, 0x03, 0x03, 0, 0x05);
        expect("clmul low", Op::OP_CARRYLESS_MULTIPLY, 8, 0xFF, 0xFF, 0, 0x55);
        expect("clmul high", Op::OP_CARRYLESS_MULTIPLY_HIGH, 8, 0xFF, 0xFF, 0,
               0x55);
        expect("clmul32 low", Op::OP_CARRYLESS_MULTIPLY, 32, 0x80000000ULL, 2,
               0, 0);
        expect("clmul32 high", Op::OP_CARRYLESS_MULTIPLY_HIGH, 32, 0x80000000ULL,
               2, 0, 1);
        expect("clmul64 high", Op::OP_CARRYLESS_MULTIPLY_HIGH, 64,
               0x8000000000000000ULL, 2, 0, 1);

        // Saturating, averages, min / max
        expect("adds 127+1", Op::OP_ADD_SATURATE_SIGNED, 8, 0x7F, 0x01, 0, 0x7F);
        expect("adds -128-1", Op::OP_ADD_SATURATE_SIGNED, 8, 0x80, 0xFF, 0, 0x80);
        expect("adds ok", Op::OP_ADD_SATURATE_SIGNED, 8, 0x10, 0x20, 0, 0x30);
        expect("addu sat", Op::OP_ADD_SATURATE_UNSIGNED, 8, 0xF0, 0x20, 0, 0xFF);
        expect("addu i64", Op::OP_ADD_SATURATE_UNSIGNED, 64,
               0xFFFFFFFFFFFFFFFFULL, 1, 0, 0xFFFFFFFFFFFFFFFFULL);
        expect("subs -128-1", Op::OP_SUBTRACT_SATURATE_SIGNED, 8, 0x80, 0x01, 0,
               0x80);
        expect("subs 127+1", Op::OP_SUBTRACT_SATURATE_SIGNED, 8, 0x7F, 0xFF, 0,
               0x7F);
        expect("subu sat", Op::OP_SUBTRACT_SATURATE_UNSIGNED, 8, 0x10, 0x20, 0,
               0);
        expect("avg max", Op::OP_AVERAGE_UNSIGNED, 8, 0xFF, 0xFE, 0, 0xFF);
        expect("avg", Op::OP_AVERAGE_UNSIGNED, 8, 0x01, 0x02, 0, 0x02);
        expect("min s", Op::OP_MINIMUM_SIGNED, 8, 0x80, 0x7F, 0, 0x80);
        expect("max s", Op::OP_MAXIMUM_SIGNED, 8, 0x80, 0x7F, 0, 0x7F);
        expect("min u", Op::OP_MINIMUM_UNSIGNED, 8, 0x80, 0x7F, 0, 0x7F);
        expect("max u", Op::OP_MAXIMUM_UNSIGNED, 8, 0x80, 0x7F, 0, 0x80);
        expect("abs min", Op::OP_ABSOLUTE, 8, 0x80, 0, 0, 0x80);
        expect("abs -1", Op::OP_ABSOLUTE, 8, 0xFF, 0, 0, 0x01);
        expect("mulhrs min", Op::OP_MULTIPLY_HIGH_ROUNDING, 16, 0x8000, 0x8000,
               0, 0x7FFF);
        expect("mulhrs 0x4000", Op::OP_MULTIPLY_HIGH_ROUNDING, 16, 0x4000,
               0x4000, 0, 0x2000);
        expect("mulhrs 1", Op::OP_MULTIPLY_HIGH_ROUNDING, 16, 1, 1, 0, 0);
        expect("mulhrs max", Op::OP_MULTIPLY_HIGH_ROUNDING, 16, 0x7FFF, 0x7FFF,
               0, 0x7FFE);
        expect("mulhrs negative", Op::OP_MULTIPLY_HIGH_ROUNDING, 16, 0x8000,
               0x4000, 0, 0xC000);

        // The rounding shift: signed by default
        expect("rshr 5>>1", Op::OP_SHIFT_RIGHT_ROUNDING, 8, 0x05, 1, 0, 0x03);
        expect("rshr -5>>1", Op::OP_SHIFT_RIGHT_ROUNDING, 8, 0xFB, 1, 0, 0xFE);
        expect("rshr 3>>2", Op::OP_SHIFT_RIGHT_ROUNDING, 8, 0x03, 2, 0, 0x01);
        expect("rshr 0", Op::OP_SHIFT_RIGHT_ROUNDING, 8, 0x05, 0, 0, 0x05);
        SemanticInstruction unsignedShift = makeInstruction(
                    Op::OP_SHIFT_RIGHT_ROUNDING, SemanticType::integer(8));
        unsignedShift.setBehavior(Op::BEHAVIOR_UNSIGNED);
        TESTS_ASSERT(tryEval(unsignedShift, 0xFB, 1, 0, r));
        check("rshr unsigned", r, 0x7E);
    }

    void testEvaluateMove()
    {
        const SemanticType i8 = SemanticType::integer(8);
        const SemanticType i16 = SemanticType::integer(16);
        const SemanticType i32 = SemanticType::integer(32);
        const SemanticType i64 = SemanticType::integer(64);
        const SemanticType i128 = SemanticType::integer(128);
        uint64 r;

        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_MOVE, i32),
                             0x12345678, 0, 0, r));
        check("move", r, 0x12345678);
        // The result is cut to the type
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_MOVE, i8), 0x1FF, 0, 0, r));
        check("move cut", r, 0xFF);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_TRUNCATE, i16, i32),
                             0x12345678, 0, 0, r));
        check("truncate", r, 0x5678);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_TRUNCATE, i8,
                                             SemanticType::integer(9)),
                             0x1FF, 0, 0, r));
        check("truncate i9", r, 0xFF);

        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_ZERO_EXTEND, i32, i8),
                             0xFF, 0, 0, r));
        check("zext", r, 0xFF);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_SIGN_EXTEND, i32, i8),
                             0x80, 0, 0, r));
        check("sext negative", r, 0xFFFFFF80ULL);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_SIGN_EXTEND, i32, i8),
                             0x7F, 0, 0, r));
        check("sext positive", r, 0x7F);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_SIGN_EXTEND, i16,
                                             SemanticType::integer(9)),
                             0x100, 0, 0, r));
        check("sext i9", r, 0xFF00);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_SIGN_EXTEND, i64, i32),
                             0x80000000ULL, 0, 0, r));
        check("sext 64", r, 0xFFFFFFFF80000000ULL);

        // Wide values
        SemanticValue sources[3];
        SemanticValue value;
        sources[0] = SemanticValue(0x8000000000000000ULL);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_SIGN_EXTEND, i128, i64), sources, value));
        check("sext 128 word 0", value.getWord(0), 0x8000000000000000ULL);
        check("sext 128 word 1", value.getWord(1), 0xFFFFFFFFFFFFFFFFULL);
        check("sext 128 word 2", value.getWord(2), 0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_ZERO_EXTEND, i128, i64), sources, value));
        check("zext 128 word 1", value.getWord(1), 0);

        // extract / insert / concat
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_EXTRACT, i16, i64, i32),
                             0x1122334455667788ULL, 16, 0, r));
        check("extract", r, 0x5566);
        const SemanticType v4i16 = SemanticType::vector(i16, 4);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_EXTRACT, i16, v4i16, i32),
                             0x0004000300020001ULL, 2, 0, r));
        check("extract lane", r, 3);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_INSERT, i32, i32, i8, i32),
                             0xAABBCCDDULL, 0x11, 8, r));
        check("insert", r, 0xAABB11DDULL);
        const SemanticType v4i8 = SemanticType::vector(i8, 4);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_INSERT, v4i8, v4i8, i8, i32),
                             0x44332211ULL, 0xFF, 2, r));
        check("insert lane", r, 0x44FF2211ULL);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_CONCAT, i64, i32, i32),
                             0xAABBCCDDULL, 0x11223344ULL, 0, r));
        check("concat", r, 0x11223344AABBCCDDULL);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_CONCAT, i16, i8, i8),
                             0x12, 0x34, 0, r));
        check("concat i8", r, 0x3412);
        sources[0] = SemanticValue(0x1111111111111111ULL);
        sources[1] = SemanticValue(0x2222222222222222ULL);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_CONCAT, i128, i64, i64), sources, value));
        check("concat 128 word 0", value.getWord(0), 0x1111111111111111ULL);
        check("concat 128 word 1", value.getWord(1), 0x2222222222222222ULL);

        // The bit operations work on wide values
        sources[0] = SemanticValue(0xF0F0);
        sources[0].setWord(1, 0xFF);
        sources[1] = SemanticValue(0x00FF);
        sources[1].setWord(1, 0x0F);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_XOR, i128), sources, value));
        check("xor 128 word 0", value.getWord(0), 0xF00F);
        check("xor 128 word 1", value.getWord(1), 0xF0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_NOT, SemanticType::integer(65)),
                sources, value));
        check("not 65 word 0", value.getWord(0), ~(uint64)0xF0F0);
        check("not 65 word 1", value.getWord(1), 0);
        sources[0] = SemanticValue(0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_NOT, SemanticType::integer(65)),
                sources, value));
        check("not 65 zero word 0", value.getWord(0), ~(uint64)0);
        check("not 65 zero word 1", value.getWord(1), 1);

        // Scalars wider than 64 bits support only the bit operations
        TESTS_ASSERT(!SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_ADD, i128), sources, value));
        TESTS_ASSERT(!SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_SHIFT_LEFT, i128), sources, value));

        // Not yet implemented operations fail
        TESTS_ASSERT(!SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_FLOAT_RECIPROCAL_ESTIMATE,
                    SemanticType::floating(SemanticType::FLOAT_DOUBLE)),
                sources, value));
        TESTS_ASSERT(!SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_CRC32, SemanticType::integer(32)),
                sources, value));
    }

    // The steps of AES on the 128 bit state (the test vectors of FIPS-197:
    // the MixColumns examples, and the S-box and ShiftRows of the bytes 0..15)
    void testEvaluateAes()
    {
        const SemanticType v16i8 = SemanticType::vector(SemanticType::integer(8), 16);
        SemanticValue sources[3];
        SemanticValue value;

        // Columns db135345 f20a225c 01010101 c6c6c6c6
        sources[0].setWord(0, 0x5c220af2455313dbULL);
        sources[0].setWord(1, 0xc6c6c6c601010101ULL);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_AES_MIX_COLUMNS, v16i8), sources, value));
        check("mix columns word 0", value.getWord(0), 0x9d58dc9fbca14d8eULL);
        check("mix columns word 1", value.getWord(1), 0xc6c6c6c601010101ULL);
        sources[0] = value;
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_AES_INVERSE_MIX_COLUMNS, v16i8),
                sources, value));
        check("inverse mix columns word 0", value.getWord(0), 0x5c220af2455313dbULL);
        check("inverse mix columns word 1", value.getWord(1), 0xc6c6c6c601010101ULL);

        // The bytes 0, 1, ..., 15
        sources[0].setWord(0, 0x0706050403020100ULL);
        sources[0].setWord(1, 0x0f0e0d0c0b0a0908ULL);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_AES_SUB_SHIFT, v16i8), sources, value));
        check("sub shift word 0", value.getWord(0), 0x7bab01f276676b63ULL);
        check("sub shift word 1", value.getWord(1), 0x2b6f7cfec577d730ULL);
        sources[0] = value;
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_AES_INVERSE_SUB_SHIFT, v16i8),
                sources, value));
        check("inverse sub shift word 0", value.getWord(0), 0x0706050403020100ULL);
        check("inverse sub shift word 1", value.getWord(1), 0x0f0e0d0c0b0a0908ULL);

        // Only the 128 bit state
        TESTS_ASSERT(!SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_AES_SUB_SHIFT, SemanticType::integer(64)),
                sources, value));
    }

    void testEvaluateVector()
    {
        const SemanticType i8 = SemanticType::integer(8);
        const SemanticType i16 = SemanticType::integer(16);
        const SemanticType v4i8 = SemanticType::vector(i8, 4);
        const SemanticType v16i8 = SemanticType::vector(i8, 16);
        uint64 r;

        // Each lane wraps on its own
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_ADD, v4i8),
                             0x01FF7F80ULL, 0x01010101ULL, 0, r));
        check("v4i8 add", r, 0x02008081ULL);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_SHIFT_LEFT, v4i8),
                             0x01010101ULL, 0x00010808ULL, 0, r));
        check("v4i8 shl", r, 0x01020000ULL);

        // 128 bit vectors
        SemanticValue sources[3];
        SemanticValue value;
        sources[0].setWord(0, 0xF0F0F0F0F0F0F0F0ULL);
        sources[0].setWord(1, 0x0101010101010101ULL);
        sources[1].setWord(0, 0x2020202020202020ULL);
        sources[1].setWord(1, 0x2020202020202020ULL);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_ADD_SATURATE_UNSIGNED, v16i8), sources,
                value));
        check("v16i8 addus word 0", value.getWord(0), 0xFFFFFFFFFFFFFFFFULL);
        check("v16i8 addus word 1", value.getWord(1), 0x2121212121212121ULL);

        // The sign bits of the lanes
        sources[0] = SemanticValue(0x80000000000000FFULL);
        sources[0].setWord(1, 0x8000);
        TESTS_ASSERT(SemanticInterpreter::evaluate(
                makeInstruction(Op::OP_SIGN_MASK, i16, v16i8), sources, value));
        check("sign mask", value.getWord(0), 0x281);

        // Compare gives a bit for each lane
        // A compare without a condition is not interpreted
        TESTS_ASSERT(!tryEval(makeInstruction(Op::OP_COMPARE,
                                              SemanticType::mask(4), v4i8, v4i8),
                              0x04030201ULL, 0x04F30201ULL, 0, r));
        SemanticInstruction compare = makeInstruction(Op::OP_COMPARE,
                                                      SemanticType::mask(4),
                                                      v4i8, v4i8);
        compare.setCondition(Op::COND_EQUAL);
        TESTS_ASSERT(tryEval(compare, 0x04030201ULL, 0x04F30201ULL, 0, r));
        check("v4i8 equal", r, 0xB);
        compare.setCondition(Op::COND_SIGNED_LESS);
        // 0x04F3 vs 0x0403: lane 2 only, 0xF3 is negative
        TESTS_ASSERT(tryEval(compare, 0x04F30201ULL, 0x04030201ULL, 0, r));
        check("v4i8 signed less", r, 0x4);
        compare.setCondition(Op::COND_UNSIGNED_LESS);
        TESTS_ASSERT(tryEval(compare, 0x04F30201ULL, 0x04030201ULL, 0, r));
        check("v4i8 unsigned less", r, 0x0);

        // Select by a mask
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_SELECT, v4i8,
                                             SemanticType::mask(4)),
                             0x5, 0x44332211ULL, 0xAABBCCDDULL, r));
        check("v4i8 select", r, 0xAA33CC11ULL);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_SELECT,
                                             SemanticType::integer(32),
                                             SemanticType::integer(1)),
                             1, 0x1111, 0x2222, r));
        check("select true", r, 0x1111);
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_SELECT,
                                             SemanticType::integer(32),
                                             SemanticType::integer(1)),
                             0, 0x1111, 0x2222, r));
        check("select false", r, 0x2222);

        // Broadcast
        TESTS_ASSERT(tryEval(makeInstruction(Op::OP_BROADCAST, v4i8, i8),
                             0x7F, 0, 0, r));
        check("broadcast", r, 0x7F7F7F7FULL);

        // Reduce
        SemanticInstruction reduce = makeInstruction(Op::OP_REDUCE, i8, v4i8);
        reduce.setCombineOperation(Op::OP_ADD);
        TESTS_ASSERT(tryEval(reduce, 0x01020304ULL, 0, 0, r));
        check("reduce add", r, 0x0A);
        reduce.setCombineOperation(Op::OP_MAXIMUM_UNSIGNED);
        TESTS_ASSERT(tryEval(reduce, 0x01020304ULL, 0, 0, r));
        check("reduce max", r, 0x04);
        reduce.setCombineOperation(Op::OP_XOR);
        TESTS_ASSERT(tryEval(reduce, 0x01020304ULL, 0, 0, r));
        check("reduce xor", r, 0x04);
        // A reduce without a combine operation is not interpreted
        TESTS_ASSERT(!tryEval(makeInstruction(Op::OP_REDUCE, i8, v4i8),
                              0x01020304ULL, 0, 0, r));
        SemanticInstruction wrap = makeInstruction(Op::OP_REDUCE, i8,
                                    SemanticType::vector(i8, 2));
        wrap.setCombineOperation(Op::OP_ADD);
        TESTS_ASSERT(tryEval(wrap, 0xFF02, 0, 0, r));
        check("reduce add wrap", r, 0x01);
    }

    //////////////////////////////////////////////////////////////////////
    // Flags
    //////////////////////////////////////////////////////////////////////

    uint32 flagsOf(Op::FlagRecipe recipe, uint bits, uint64 s0, uint64 s1,
                   uint64 s2)
    {
        return SemanticInterpreter::evaluateFlags(recipe,
                    SemanticType::integer(bits), SemanticValue(s0),
                    SemanticValue(s1), SemanticValue(s2));
    }

    void testFlags()
    {
        const uint32 CF = SemanticFlags::FLAG_CARRY;
        const uint32 ZF = SemanticFlags::FLAG_ZERO;
        const uint32 SF = SemanticFlags::FLAG_SIGN;
        const uint32 OF = SemanticFlags::FLAG_OVERFLOW;
        const uint32 PF = SemanticFlags::FLAG_PARITY;
        const uint32 AF = SemanticFlags::FLAG_ADJUST;

        // add
        check("add 7F+1", flagsOf(Op::FLAGS_ADD, 8, 0x7F, 0x01, 0x80),
              OF | SF | AF);
        check("add FF+1", flagsOf(Op::FLAGS_ADD, 8, 0xFF, 0x01, 0x00),
              CF | ZF | PF | AF);
        check("adc FF+0+1", flagsOf(Op::FLAGS_ADD, 8, 0xFF, 0x00, 0x00),
              CF | ZF | PF | AF);
        check("add 80+80", flagsOf(Op::FLAGS_ADD, 8, 0x80, 0x80, 0x00),
              CF | OF | ZF | PF);
        check("add i32", flagsOf(Op::FLAGS_ADD, 32, 0x7FFFFFFF, 1, 0x80000000ULL),
              OF | SF | PF | AF);
        // sub
        check("sub 0-1", flagsOf(Op::FLAGS_SUBTRACT, 32, 0, 1, 0xFFFFFFFFULL),
              CF | SF | AF | PF);
        check("sub 80-1", flagsOf(Op::FLAGS_SUBTRACT, 8, 0x80, 0x01, 0x7F),
              OF | AF);
        check("sub 5-5", flagsOf(Op::FLAGS_SUBTRACT, 8, 5, 5, 0), ZF | PF);
        check("sub 3-5", flagsOf(Op::FLAGS_SUBTRACT, 32, 3, 5, 0xFFFFFFFEULL),
              CF | SF | AF);
        // logic
        check("logic sign", flagsOf(Op::FLAGS_LOGIC, 32, 0xFFFF, 0xFFFFFFFFULL,
                                    0x80000000ULL), SF | PF);
        check("logic zero", flagsOf(Op::FLAGS_LOGIC, 32, 0, 0, 0), ZF | PF);
        check("logic odd", flagsOf(Op::FLAGS_LOGIC, 8, 0, 0, 0x01), 0);
        // shl
        check("shl 1", flagsOf(Op::FLAGS_SHIFT_LEFT, 8, 0x81, 1, 0x02), CF | OF);
        check("shl 8", flagsOf(Op::FLAGS_SHIFT_LEFT, 8, 0x01, 8, 0x00),
              CF | OF | ZF | PF);
        check("shl 9", flagsOf(Op::FLAGS_SHIFT_LEFT, 8, 0x01, 9, 0x00),
              ZF | PF);
        check("shl msb", flagsOf(Op::FLAGS_SHIFT_LEFT, 8, 0x40, 1, 0x80), OF | SF);
        // shr
        check("shr 1", flagsOf(Op::FLAGS_SHIFT_RIGHT_LOGICAL, 8, 0x81, 1, 0x40),
              CF | OF);
        check("shr 9", flagsOf(Op::FLAGS_SHIFT_RIGHT_LOGICAL, 8, 0x81, 9, 0x00),
              OF | ZF | PF);
        check("shr 8", flagsOf(Op::FLAGS_SHIFT_RIGHT_LOGICAL, 8, 0x81, 8, 0x00),
              CF | OF | ZF | PF);
        // sar
        check("sar 1", flagsOf(Op::FLAGS_SHIFT_RIGHT_ARITHMETIC, 8, 0x81, 1,
                               0xC0), CF | SF | PF);
        check("sar 20", flagsOf(Op::FLAGS_SHIFT_RIGHT_ARITHMETIC, 8, 0x81, 20,
                                0xFF), CF | SF | PF);
        check("sar 3", flagsOf(Op::FLAGS_SHIFT_RIGHT_ARITHMETIC, 8, 0x04, 3,
                               0x00), CF | ZF | PF);
        // rotates
        check("rol", flagsOf(Op::FLAGS_ROTATE_LEFT, 8, 0x81, 1, 0x03), CF | OF);
        check("rol no carry", flagsOf(Op::FLAGS_ROTATE_LEFT, 8, 0x40, 1, 0x80),
              OF);
        check("ror", flagsOf(Op::FLAGS_ROTATE_RIGHT, 8, 0x81, 1, 0xC0), CF);
        check("ror msb", flagsOf(Op::FLAGS_ROTATE_RIGHT, 8, 0x01, 1, 0x80),
              CF | OF);
        // multiply
        check("mul unsigned", flagsOf(Op::FLAGS_MULTIPLY_UNSIGNED, 32, 5, 1, 0),
              CF | OF);
        check("mul unsigned small", flagsOf(Op::FLAGS_MULTIPLY_UNSIGNED, 32, 5,
                                            0, 0), 0);
        check("mul signed -1", flagsOf(Op::FLAGS_MULTIPLY_SIGNED, 32,
                    0xFFFFFFFFULL, 0xFFFFFFFFULL, 0), 0);
        check("mul signed min", flagsOf(Op::FLAGS_MULTIPLY_SIGNED, 32,
                    0x80000000ULL, 0, 0), CF | OF);
        check("mul signed max", flagsOf(Op::FLAGS_MULTIPLY_SIGNED, 32,
                    0x7FFFFFFFULL, 0, 0), 0);
        // explicit: every flag of the mask gets bit 0
        check("explicit 1", flagsOf(Op::FLAGS_EXPLICIT, 1, 1, 0, 0) & CF, CF);
        check("explicit 0", flagsOf(Op::FLAGS_EXPLICIT, 1, 0, 0, 0), 0);
        // not interpreted
        check("float compare", flagsOf(Op::FLAGS_FLOAT_COMPARE, 32, 1, 2, 0), 0);

        // The conditions
        const uint32 DF = SemanticFlags::FLAG_DIRECTION;
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(Op::COND_ALWAYS, 0));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(Op::COND_NONE, 0));
        TESTS_ASSERT(!SemanticInterpreter::evaluateCondition(Op::COND_NEVER, DF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(Op::COND_EQUAL, ZF));
        TESTS_ASSERT(!SemanticInterpreter::evaluateCondition(Op::COND_EQUAL, 0));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(Op::COND_NOT_EQUAL, CF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                                Op::COND_UNSIGNED_LESS, CF));
        TESTS_ASSERT(!SemanticInterpreter::evaluateCondition(
                                        Op::COND_UNSIGNED_GREATER_EQUAL, CF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_UNSIGNED_LESS_EQUAL, ZF));
        TESTS_ASSERT(!SemanticInterpreter::evaluateCondition(
                                        Op::COND_UNSIGNED_GREATER, ZF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_UNSIGNED_GREATER, 0));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_SIGNED_LESS, SF));
        TESTS_ASSERT(!SemanticInterpreter::evaluateCondition(
                                        Op::COND_SIGNED_LESS, SF | OF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_SIGNED_GREATER_EQUAL, SF | OF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_SIGNED_LESS_EQUAL, ZF));
        TESTS_ASSERT(!SemanticInterpreter::evaluateCondition(
                                        Op::COND_SIGNED_GREATER, ZF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_SIGNED_GREATER, 0));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_NEGATIVE, SF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_NOT_OVERFLOW, SF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_OVERFLOW, OF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_NOT_CARRY, OF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_PARITY_EVEN, PF));
        TESTS_ASSERT(SemanticInterpreter::evaluateCondition(
                                        Op::COND_PARITY_ODD, AF));
    }

    //////////////////////////////////////////////////////////////////////
    // run()
    //////////////////////////////////////////////////////////////////////

    static SemanticOperand reg(uint id, uint bits)
    {
        return SemanticOperand::registerOperand(id, SemanticType::integer(bits));
    }

    static SemanticOperand imm(int64 value, uint bits)
    {
        return SemanticOperand::immediate(value, SemanticType::integer(bits));
    }

    static ProcessorAddress addr(uint32 address)
    {
        return ProcessorAddress(ProcessorAddress::PROCESSOR_32, address);
    }

    // Append "destination = operation<bits> s0, s1, s2"
    static void emit(SemanticBlock& block, Op::Operation operation, uint bits,
                     const SemanticOperand& destination,
                     const SemanticOperand& s0 = SemanticOperand(),
                     const SemanticOperand& s1 = SemanticOperand(),
                     const SemanticOperand& s2 = SemanticOperand())
    {
        block.append(SemanticInstruction(operation, SemanticType::integer(bits),
                                         destination, s0, s1, s2));
    }

    static void emitFlags(SemanticBlock& block, Op::FlagRecipe recipe,
                          uint bits, uint32 mask, const SemanticOperand& s0,
                          const SemanticOperand& s1, const SemanticOperand& s2,
                          Op::Condition condition = Op::COND_NONE,
                          uint32 falseFlags = 0)
    {
        SemanticInstruction instruction(Op::OP_SET_FLAGS,
                    SemanticType::integer(bits), SemanticOperand(), s0, s1, s2);
        instruction.setFlags(recipe, mask);
        instruction.setCondition(condition);
        instruction.setFalseFlags(falseFlags);
        block.append(instruction);
    }

    static ArchitectureDescriptionPtr getArchitecture()
    {
        return ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_INTEL_32);
    }

    void testArchitecture()
    {
        ArchitectureDescriptionPtr arch = getArchitecture();
        check("registers count", arch->getRegistersCount(), IA32_REGISTERS_COUNT);
        check("address bits", arch->getAddressBits(), 32);
        check("address bits 16", ArchitectureDescriptionFactory::getArchitecture(
                OpcodeSubsystems::DISASSEMBLER_INTEL_16)->getAddressBits(), 16);
        check("sp", arch->getStackPointer(), IA32_REG_ESP);
        check("pc", arch->getProgramCounter(), IA32_REG_EIP);
        TESTS_ASSERT(arch->isLittleEndian());
        TESTS_ASSERT(!arch->isCarryInvertedOnSubtract());
        uint modeRegister;
        TESTS_ASSERT(!arch->getModeRegister(modeRegister));
        TESTS_EXCEPTION(ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_ARM_32_LE));

        // Every name is found at its own id, and the sub-registers point to
        // full registers
        for (uint i = 0; i < arch->getRegistersCount(); i++)
        {
            const ArchitectureRegister& reg = arch->getRegister(i);
            uint found = 0xFFFFFFFF;
            TESTS_ASSERT(arch->findRegister(cString(reg.m_name), found));
            check(reg.m_name, found, i);
            if (reg.m_parent != ArchitectureRegister::NO_PARENT)
            {
                TESTS_ASSERT(reg.m_parent < arch->getRegistersCount());
                TESTS_ASSERT(arch->getRegister(reg.m_parent).m_parent ==
                             ArchitectureRegister::NO_PARENT);
                TESTS_ASSERT(reg.m_bitOffset + reg.m_bits <=
                             arch->getRegister(reg.m_parent).m_bits);
            }
        }
        uint id;
        TESTS_ASSERT(arch->findRegister(cString("ah"), id));
        check("ah id", id, IA32_REG_AH);
        check("ah parent", arch->getRegister(id).m_parent, IA32_REG_EAX);
        check("ah offset", arch->getRegister(id).m_bitOffset, 8);
        check("ah bits", arch->getRegister(id).m_bits, 8);
        TESTS_ASSERT(arch->findRegister(cString("ax"), id));
        check("ax parent", arch->getRegister(id).m_parent, IA32_REG_EAX);
        TESTS_ASSERT(arch->findRegister(cString("mm3"), id));
        check("mm3 parent", arch->getRegister(id).m_parent, IA32_REG_FPR3);
        check("mm3 bits", arch->getRegister(id).m_bits, 64);
        TESTS_ASSERT(!arch->findRegister(cString("rax"), id));
        check("xmm bits", arch->getRegister(IA32_REG_XMM7).m_bits, 128);
        check("fpr bits", arch->getRegister(IA32_REG_FPR0).m_bits, 80);
        check("fpu top bits", arch->getRegister(IA32_REG_FPU_TOP).m_bits, 3);
        check("cf flag", arch->getRegister(IA32_REG_CF).m_flag,
              SemanticFlags::FLAG_CARRY);
        check("if flag", arch->getRegister(IA32_REG_IF).m_flag, 0);

        const uint32 flags[] = { SemanticFlags::FLAG_CARRY,
            SemanticFlags::FLAG_ZERO, SemanticFlags::FLAG_SIGN,
            SemanticFlags::FLAG_OVERFLOW, SemanticFlags::FLAG_PARITY,
            SemanticFlags::FLAG_ADJUST, SemanticFlags::FLAG_DIRECTION };
        for (uint i = 0; i < 7; i++)
        {
            TESTS_ASSERT(arch->getFlagRegister(flags[i], id));
            check("flag register", arch->getRegister(id).m_flag, flags[i]);
        }

        const SemanticType i32 = SemanticType::integer(32);
        const SemanticType i64 = SemanticType::integer(64);
        TESTS_ASSERT(arch->isSupported(Op::OP_ADD, i32));
        TESTS_ASSERT(!arch->isSupported(Op::OP_ADD, i64));
        TESTS_ASSERT(arch->isSupported(Op::OP_MOVE, i64));
        TESTS_ASSERT(arch->isSupported(Op::OP_ADD,
                SemanticType::vector(SemanticType::integer(8), 16)));
        TESTS_ASSERT(!arch->isSupported(Op::OP_ADD,
                SemanticType::vector(SemanticType::integer(8), 32)));
        TESTS_ASSERT(arch->isSupported(Op::OP_FLOAT_ADD,
                SemanticType::floating(SemanticType::FLOAT_DOUBLE)));
        TESTS_ASSERT(!arch->isSupported(Op::OP_FLOAT_ADD,
                SemanticType::floating(SemanticType::FLOAT_HALF)));
    }

    void testRunRegisters()
    {
        TestSemanticMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);

        // al, then ah, then read ax and eax
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        block.beginGuestInstruction(addr(0x1000), 2);
        emit(block, Op::OP_MOVE, 8, reg(IA32_REG_AL, 8), imm(0x11, 8));
        emit(block, Op::OP_MOVE, 8, reg(IA32_REG_AH, 8), imm(0x22, 8));
        block.setFallThroughAddress(addr(0x1002));
        check("exit", interpreter.run(block),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("ax", interpreter.getRegister(IA32_REG_AX).getWord(0), 0x2211);
        check("eax", interpreter.getRegister(IA32_REG_EAX).getWord(0), 0x2211);
        check("next address", interpreter.getNextAddress(), 0x1002);
        check("eip", interpreter.getRegister(IA32_REG_EIP).getWord(0), 0x1002);

        // A sub-register keeps the rest of the parent
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(0xAABBCCDDULL));
        interpreter.setRegister(IA32_REG_AX, SemanticValue(0x1234));
        check("eax after ax", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0xAABB1234ULL);
        interpreter.setRegister(IA32_REG_AH, SemanticValue(0x99));
        check("eax after ah", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0xAABB9934ULL);
        check("al", interpreter.getRegister(IA32_REG_AL).getWord(0), 0x34);
        check("ah", interpreter.getRegister(IA32_REG_AH).getWord(0), 0x99);
        // The value is cut to the register
        interpreter.setRegister(IA32_REG_AL, SemanticValue(0x1FF));
        check("eax after al", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0xAABB99FFULL);
        interpreter.setRegister(IA32_REG_ECX, SemanticValue(0x1FFFFFFFFULL));
        check("ecx cut", interpreter.getRegister(IA32_REG_ECX).getWord(0),
              0xFFFFFFFFULL);

        // ATTRIBUTE_ZERO_PARENT zeroes the rest of eax
        SemanticBlock zero(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        SemanticInstruction move(Op::OP_MOVE, SemanticType::integer(16),
                                 reg(IA32_REG_AX, 16), imm(0x4321, 16));
        move.setAttributes(SemanticInstruction::ATTRIBUTE_ZERO_PARENT);
        zero.append(move);
        interpreter.run(zero);
        check("eax zero parent", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0x4321);

        // An operand is cut to its type when written, and read
        SemanticBlock cut(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        interpreter.setRegister(IA32_REG_EBX, SemanticValue(0x12345678));
        emit(cut, Op::OP_MOVE, 16, reg(IA32_REG_EDX, 32), reg(IA32_REG_EBX, 16));
        interpreter.run(cut);
        check("read cut", interpreter.getRegister(IA32_REG_EDX).getWord(0),
              0x5678);

        // MMX is the low part of the x87 register
        SemanticValue st;
        st.setWord(0, 0x1122334455667788ULL);
        st.setWord(1, 0x7FFF);
        interpreter.setRegister(IA32_REG_FPR0, st);
        check("mm0", interpreter.getRegister(IA32_REG_MM0).getWord(0),
              0x1122334455667788ULL);
        interpreter.setRegister(IA32_REG_MM0, SemanticValue(0x99));
        check("fpr0 low", interpreter.getRegister(IA32_REG_FPR0).getWord(0), 0x99);
        check("fpr0 high", interpreter.getRegister(IA32_REG_FPR0).getWord(1),
              0x7FFF);
        // The fields of 80 bits are not wider
        SemanticValue ones(~(uint64)0);
        ones.setWord(1, ~(uint64)0);
        interpreter.setRegister(IA32_REG_FPR1, ones);
        check("fpr1 cut", interpreter.getRegister(IA32_REG_FPR1).getWord(1),
              0xFFFF);
    }

    void testRunLoop()
    {
        TestSemanticMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);

        // eax = 5 + 4 + 3 + 2 + 1:
        //     ecx = 5
        //   L:
        //     eax = eax + ecx
        //     ecx = ecx - 1 (ZF)
        //     branch_conditional (ecx != 0) L
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        block.beginGuestInstruction(addr(0x1000), 5);
        emit(block, Op::OP_MOVE, 32, reg(IA32_REG_ECX, 32), imm(5, 32));
        SemanticOperand label = block.newLabel();
        emit(block, Op::OP_LABEL, 32, SemanticOperand(), label);
        SemanticOperand t0 = block.newTemporary(SemanticType::integer(32));
        emit(block, Op::OP_ADD, 32, t0, reg(IA32_REG_EAX, 32),
             reg(IA32_REG_ECX, 32));
        emit(block, Op::OP_MOVE, 32, reg(IA32_REG_EAX, 32), t0);
        SemanticOperand t1 = block.newTemporary(SemanticType::integer(32));
        emit(block, Op::OP_SUBTRACT, 32, t1, reg(IA32_REG_ECX, 32), imm(1, 32));
        emitFlags(block, Op::FLAGS_SUBTRACT, 32, SemanticFlags::FLAG_ZERO,
                  reg(IA32_REG_ECX, 32), imm(1, 32), t1);
        emit(block, Op::OP_MOVE, 32, reg(IA32_REG_ECX, 32), t1);
        SemanticOperand t2 = block.newTemporary(SemanticType::integer(1));
        SemanticInstruction test(Op::OP_TEST_FLAGS, SemanticType::integer(1), t2);
        test.setCondition(Op::COND_NOT_EQUAL);
        block.append(test);
        emit(block, Op::OP_BRANCH_CONDITIONAL, 1, SemanticOperand(), t2, label);
        block.setFallThroughAddress(addr(0x1005));

        check("loop exit", interpreter.run(block),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("loop eax", interpreter.getRegister(IA32_REG_EAX).getWord(0), 15);
        check("loop ecx", interpreter.getRegister(IA32_REG_ECX).getWord(0), 0);
        check("loop zf", interpreter.getRegister(IA32_REG_ZF).getWord(0), 1);
        check("loop eip", interpreter.getRegister(IA32_REG_EIP).getWord(0),
              0x1005);

        // Branches out of the block
        SemanticBlock branch(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        branch.beginGuestInstruction(addr(0x2000), 5);
        emit(branch, Op::OP_BRANCH, 32, SemanticOperand(),
             SemanticOperand::guestAddress(0x5000, SemanticType::integer(32)));
        emit(branch, Op::OP_MOVE, 32, reg(IA32_REG_EAX, 32), imm(77, 32));
        check("branch exit", interpreter.run(branch),
              SemanticInterpreter::EXIT_BRANCH);
        check("branch address", interpreter.getNextAddress(), 0x5000);
        check("branch eip", interpreter.getRegister(IA32_REG_EIP).getWord(0),
              0x5000);
        // Nothing after the branch is executed
        check("branch skipped", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              15);

        // An indirect branch, and a not taken conditional branch
        SemanticBlock indirect(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        interpreter.setRegister(IA32_REG_EBX, SemanticValue(0x6000));
        emit(indirect, Op::OP_BRANCH_CONDITIONAL, 1, SemanticOperand(), imm(0, 1),
             SemanticOperand::guestAddress(0x7000, SemanticType::integer(32)));
        emit(indirect, Op::OP_BRANCH_INDIRECT, 32, SemanticOperand(),
             reg(IA32_REG_EBX, 32));
        check("indirect exit", interpreter.run(indirect),
              SemanticInterpreter::EXIT_BRANCH);
        check("indirect address", interpreter.getNextAddress(), 0x6000);

        // call and ret are branches
        SemanticBlock call(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        emit(call, Op::OP_CALL, 32, SemanticOperand(), imm(0x8000, 32));
        check("call exit", interpreter.run(call),
              SemanticInterpreter::EXIT_BRANCH);
        check("call address", interpreter.getNextAddress(), 0x8000);
        SemanticBlock ret(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        emit(ret, Op::OP_RETURN, 32, SemanticOperand(), reg(IA32_REG_EBX, 32));
        check("ret exit", interpreter.run(ret), SemanticInterpreter::EXIT_BRANCH);
        check("ret address", interpreter.getNextAddress(), 0x6000);

        // The steps limit
        SemanticBlock endless(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        endless.beginGuestInstruction(addr(0x3000), 2);
        SemanticOperand spin = endless.newLabel();
        emit(endless, Op::OP_LABEL, 32, SemanticOperand(), spin);
        emit(endless, Op::OP_BRANCH, 32, SemanticOperand(), spin);
        check("steps limit", interpreter.run(endless, 10),
              SemanticInterpreter::EXIT_STEPS_LIMIT);
        check("steps limit address", interpreter.getNextAddress(), 0x3000);
        check("steps limit eip", interpreter.getRegister(IA32_REG_EIP).getWord(0),
              0x3000);
        // Exactly enough steps
        SemanticBlock two(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        emit(two, Op::OP_NOP, 32, SemanticOperand());
        emit(two, Op::OP_NOP, 32, SemanticOperand());
        two.setFallThroughAddress(addr(0x4000));
        check("exact steps", interpreter.run(two, 2),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("one step less", interpreter.run(two, 1),
              SemanticInterpreter::EXIT_STEPS_LIMIT);
    }

    void testRunFlags()
    {
        TestSemanticMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);
        const uint32 CF = SemanticFlags::FLAG_CARRY;
        const uint32 ZF = SemanticFlags::FLAG_ZERO;
        const uint32 SF = SemanticFlags::FLAG_SIGN;
        const uint32 OF = SemanticFlags::FLAG_OVERFLOW;
        const uint32 PF = SemanticFlags::FLAG_PARITY;
        const uint32 AF = SemanticFlags::FLAG_ADJUST;
        check("initial flags", interpreter.getFlags(), 0);

        // 0x7F + 1 = 0x80
        SemanticBlock add(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        emitFlags(add, Op::FLAGS_ADD, 8, SemanticFlags::FLAGS_ARITHMETIC,
                  imm(0x7F, 8), imm(1, 8), imm(0x80, 8));
        check("flags exit", interpreter.run(add),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("flags", interpreter.getFlags(), OF | SF | AF);
        check("cf", interpreter.getRegister(IA32_REG_CF).getWord(0), 0);
        check("zf", interpreter.getRegister(IA32_REG_ZF).getWord(0), 0);
        check("sf", interpreter.getRegister(IA32_REG_SF).getWord(0), 1);
        check("of", interpreter.getRegister(IA32_REG_OF).getWord(0), 1);
        check("pf", interpreter.getRegister(IA32_REG_PF).getWord(0), 0);
        check("af", interpreter.getRegister(IA32_REG_AF).getWord(0), 1);

        // Only the flags of the mask change: and r, 0 clears CF, sets ZF
        SemanticBlock logic(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        emitFlags(logic, Op::FLAGS_LOGIC, 8, CF | ZF, imm(0, 8), imm(0, 8),
                  imm(0, 8));
        interpreter.run(logic);
        check("masked flags", interpreter.getFlags(), ZF | OF | SF | AF);

        // The explicit flags: stc, std
        SemanticBlock stc(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        emitFlags(stc, Op::FLAGS_EXPLICIT, 1, CF, imm(1, 1), SemanticOperand(),
                  SemanticOperand());
        emitFlags(stc, Op::FLAGS_EXPLICIT, 1, SemanticFlags::FLAG_DIRECTION,
                  imm(1, 1), SemanticOperand(), SemanticOperand());
        emitFlags(stc, Op::FLAGS_EXPLICIT, 1, ZF | OF, imm(0, 1),
                  SemanticOperand(), SemanticOperand());
        interpreter.run(stc);
        check("explicit flags", interpreter.getFlags(),
              CF | SemanticFlags::FLAG_DIRECTION | SF | AF);
        check("df", interpreter.getRegister(IA32_REG_DF).getWord(0), 1);

        // test_flags
        SemanticBlock sub(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        SemanticOperand t0 = sub.newTemporary(SemanticType::integer(32));
        emit(sub, Op::OP_SUBTRACT, 32, t0, imm(3, 32), imm(5, 32));
        emitFlags(sub, Op::FLAGS_SUBTRACT, 32, SemanticFlags::FLAGS_ARITHMETIC,
                  imm(3, 32), imm(5, 32), t0);
        SemanticOperand less = sub.newTemporary(SemanticType::integer(1));
        SemanticOperand below = sub.newTemporary(SemanticType::integer(1));
        SemanticOperand equal = sub.newTemporary(SemanticType::integer(1));
        SemanticInstruction test(Op::OP_TEST_FLAGS, SemanticType::integer(1), less);
        test.setCondition(Op::COND_SIGNED_LESS);
        sub.append(test);
        test.setDestination(below);
        test.setCondition(Op::COND_UNSIGNED_LESS);
        sub.append(test);
        test.setDestination(equal);
        test.setCondition(Op::COND_EQUAL);
        sub.append(test);
        emit(sub, Op::OP_MOVE, 32, reg(IA32_REG_EAX, 32), less);
        emit(sub, Op::OP_MOVE, 32, reg(IA32_REG_ECX, 32), below);
        emit(sub, Op::OP_MOVE, 32, reg(IA32_REG_EDX, 32), equal);
        interpreter.run(sub);
        // The direction flag is still set by the std above
        const uint32 DF = SemanticFlags::FLAG_DIRECTION;
        check("3-5 flags", interpreter.getFlags(), CF | SF | AF | DF);
        check("3 < 5 signed", interpreter.getRegister(IA32_REG_EAX).getWord(0), 1);
        check("3 < 5 unsigned", interpreter.getRegister(IA32_REG_ECX).getWord(0),
              1);
        check("3 == 5", interpreter.getRegister(IA32_REG_EDX).getWord(0), 0);

        // A conditional set_flags: the recipe when the condition is true,
        // the false flags (and 0 for the rest of the mask) when it's false
        const uint32 mask = CF | ZF | SF;
        // ZF is clear, so COND_EQUAL is false
        interpreter.setRegister(IA32_REG_ZF, SemanticValue(0));
        interpreter.setRegister(IA32_REG_SF, SemanticValue(1));
        SemanticBlock ccmp(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        emitFlags(ccmp, Op::FLAGS_LOGIC, 8, mask, imm(0, 8), imm(0, 8),
                  imm(0x80, 8), Op::COND_EQUAL, CF);
        interpreter.run(ccmp);
        check("false flags", interpreter.getFlags(), CF | AF | DF);
        // ZF is set, so the recipe: SF from the result 0x80
        interpreter.setRegister(IA32_REG_ZF, SemanticValue(1));
        interpreter.run(ccmp);
        check("true flags", interpreter.getFlags(), SF | AF | DF);
        check("true flags pf", interpreter.getRegister(IA32_REG_PF).getWord(0), 0);

        // The recipes which aren't interpreted
        SemanticBlock fcom(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        fcom.beginGuestInstruction(addr(0x1234), 2);
        emitFlags(fcom, Op::FLAGS_FLOAT_COMPARE, 32, ZF | CF | PF, imm(0, 32),
                  imm(0, 32), SemanticOperand());
        check("float compare exit", interpreter.run(fcom),
              SemanticInterpreter::EXIT_UNDEFINED);
        check("float compare address", interpreter.getNextAddress(), 0x1234);
    }

    void testRunExits()
    {
        TestSemanticMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);

        // A trap: the address is the guest instruction of the trap
        SemanticBlock trap(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        trap.beginGuestInstruction(addr(0x2000), 1);
        emit(trap, Op::OP_MOVE, 32, reg(IA32_REG_EAX, 32), imm(1, 32));
        trap.beginGuestInstruction(addr(0x2001), 2);
        SemanticInstruction interrupt(Op::OP_TRAP, SemanticType(),
                                      SemanticOperand(), imm(0x80, 8));
        interrupt.setKind(Op::TRAP_INTERRUPT);
        trap.append(interrupt);
        emit(trap, Op::OP_MOVE, 32, reg(IA32_REG_EAX, 32), imm(2, 32));
        trap.setFallThroughAddress(addr(0x2003));
        check("trap exit", interpreter.run(trap), SemanticInterpreter::EXIT_TRAP);
        check("trap kind", interpreter.getTrapKind(), Op::TRAP_INTERRUPT);
        check("trap number", interpreter.getTrapNumber(), 0x80);
        check("trap address", interpreter.getNextAddress(), 0x2001);
        check("trap eip", interpreter.getRegister(IA32_REG_EIP).getWord(0), 0x2001);
        check("trap eax", interpreter.getRegister(IA32_REG_EAX).getWord(0), 1);

        // A division by zero is a fault of the guest instruction
        SemanticBlock divide(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        divide.beginGuestInstruction(addr(0x3000), 2);
        emit(divide, Op::OP_MOVE, 32, reg(IA32_REG_EBX, 32), imm(9, 32));
        divide.beginGuestInstruction(addr(0x3002), 2);
        SemanticOperand t0 = divide.newTemporary(SemanticType::integer(32));
        emit(divide, Op::OP_DIVIDE_UNSIGNED, 32, t0, reg(IA32_REG_EAX, 32),
             reg(IA32_REG_ECX, 32));
        emit(divide, Op::OP_MOVE, 32, reg(IA32_REG_EBX, 32), t0);
        divide.setFallThroughAddress(addr(0x3004));
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(100));
        interpreter.setRegister(IA32_REG_ECX, SemanticValue(0));
        check("divide exit", interpreter.run(divide),
              SemanticInterpreter::EXIT_TRAP);
        check("divide kind", interpreter.getTrapKind(), Op::TRAP_FAULT);
        check("divide number", interpreter.getTrapNumber(), 0);
        check("divide address", interpreter.getNextAddress(), 0x3002);
        check("divide ebx", interpreter.getRegister(IA32_REG_EBX).getWord(0), 9);
        interpreter.setRegister(IA32_REG_ECX, SemanticValue(7));
        check("divide ok", interpreter.run(divide),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("divide result", interpreter.getRegister(IA32_REG_EBX).getWord(0),
              14);
        // The behavior of the guest: ARM gives 0
        SemanticBlock divideZero(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        SemanticOperand t1 = divideZero.newTemporary(SemanticType::integer(32));
        SemanticInstruction udiv(Op::OP_DIVIDE_UNSIGNED, SemanticType::integer(32),
                                 t1, reg(IA32_REG_EAX, 32),
                                 imm(0, 32));
        udiv.setBehavior(Op::BEHAVIOR_DIVIDE_ZERO);
        divideZero.append(udiv);
        emit(divideZero, Op::OP_MOVE, 32, reg(IA32_REG_EDX, 32), t1);
        interpreter.setRegister(IA32_REG_EDX, SemanticValue(0x55));
        check("divide zero exit", interpreter.run(divideZero),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("divide zero result",
              interpreter.getRegister(IA32_REG_EDX).getWord(0), 0);

        // A helper: the caller does the work, and resumes
        SemanticBlock helper(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        helper.beginGuestInstruction(addr(0x4000), 2);
        emit(helper, Op::OP_HELPER, 32, reg(IA32_REG_EAX, 32), imm(7, 32),
             imm(0x55, 32), imm(0x66, 32));
        SemanticOperand t2 = helper.newTemporary(SemanticType::integer(32));
        emit(helper, Op::OP_ADD, 32, t2, reg(IA32_REG_EAX, 32), imm(1, 32));
        emit(helper, Op::OP_MOVE, 32, reg(IA32_REG_EBX, 32), t2);
        helper.setFallThroughAddress(addr(0x4002));
        check("helper exit", interpreter.run(helper),
              SemanticInterpreter::EXIT_HELPER);
        check("helper number", interpreter.getHelperNumber(), 7);
        check("helper argument 0", interpreter.getHelperArgument(0).getWord(0),
              0x55);
        check("helper argument 1", interpreter.getHelperArgument(1).getWord(0),
              0x66);
        check("helper address", interpreter.getNextAddress(), 0x4000);
        check("helper resume", interpreter.resume(SemanticValue(0x100)),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("helper eax", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0x100);
        check("helper ebx", interpreter.getRegister(IA32_REG_EBX).getWord(0),
              0x101);
        check("helper resume address", interpreter.getNextAddress(), 0x4002);

        // Undefined operations
        SemanticBlock undefined(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        undefined.beginGuestInstruction(addr(0x5000), 2);
        emit(undefined, Op::OP_UNDEFINED, 32, SemanticOperand());
        check("undefined exit", interpreter.run(undefined),
              SemanticInterpreter::EXIT_UNDEFINED);
        check("undefined address", interpreter.getNextAddress(), 0x5000);
        SemanticBlock floating(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        floating.beginGuestInstruction(addr(0x5100), 2);
        const SemanticType f64 = SemanticType::floating(SemanticType::FLOAT_DOUBLE);
        floating.append(SemanticInstruction(Op::OP_FLOAT_RECIPROCAL_ESTIMATE, f64,
                SemanticOperand::registerOperand(IA32_REG_XMM0, f64),
                SemanticOperand::registerOperand(IA32_REG_XMM1, f64),
                SemanticOperand::registerOperand(IA32_REG_XMM2, f64)));
        check("float exit", interpreter.run(floating),
              SemanticInterpreter::EXIT_UNDEFINED);
        check("float address", interpreter.getNextAddress(), 0x5100);
    }

    void testRunMemory()
    {
        TestSemanticMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);

        // Store and load, little endian
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        block.beginGuestInstruction(addr(0x6000), 3);
        emit(block, Op::OP_STORE, 32, SemanticOperand(), imm(0x1000, 32),
             imm(0xAABBCCDDLL, 32));
        SemanticOperand t0 = block.newTemporary(SemanticType::integer(16));
        emit(block, Op::OP_LOAD, 16, t0, imm(0x1000, 32));
        emit(block, Op::OP_MOVE, 16, reg(IA32_REG_AX, 16), t0);
        SemanticOperand t1 = block.newTemporary(SemanticType::integer(8));
        emit(block, Op::OP_LOAD, 8, t1, imm(0x1003, 32));
        emit(block, Op::OP_MOVE, 8, reg(IA32_REG_BL, 8), t1);
        check("memory exit", interpreter.run(block),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("byte 0", memory.getByte(0x1000), 0xDD);
        check("byte 1", memory.getByte(0x1001), 0xCC);
        check("byte 3", memory.getByte(0x1003), 0xAA);
        check("load 16", interpreter.getRegister(IA32_REG_EAX).getWord(0), 0xCCDD);
        check("load 8", interpreter.getRegister(IA32_REG_EBX).getWord(0), 0xAA);

        // A 128 bit store
        SemanticBlock wide(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        SemanticValue xmm;
        xmm.setWord(0, 0x0807060504030201ULL);
        xmm.setWord(1, 0x100F0E0D0C0B0A09ULL);
        interpreter.setRegister(IA32_REG_XMM3, xmm);
        const SemanticType i128 = SemanticType::integer(128);
        wide.append(SemanticInstruction(Op::OP_STORE, i128, SemanticOperand(),
                imm(0x1100, 32), SemanticOperand::registerOperand(
                                                    IA32_REG_XMM3, i128)));
        SemanticOperand wideTemp = wide.newTemporary(i128);
        wide.append(SemanticInstruction(Op::OP_LOAD, i128, wideTemp,
                                        imm(0x1100, 32)));
        wide.append(SemanticInstruction(Op::OP_MOVE, i128,
                SemanticOperand::registerOperand(IA32_REG_XMM4, i128), wideTemp));
        interpreter.run(wide);
        check("wide byte 0", memory.getByte(0x1100), 0x01);
        check("wide byte 15", memory.getByte(0x110F), 0x10);
        TESTS_ASSERT(interpreter.getRegister(IA32_REG_XMM4) == xmm);

        // An exchange, a compare exchange and a read-modify-write
        SemanticBlock atomic(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        SemanticOperand old0 = atomic.newTemporary(SemanticType::integer(32));
        emit(atomic, Op::OP_ATOMIC_EXCHANGE, 32, old0, imm(0x1000, 32),
             imm(0x11111111, 32));
        emit(atomic, Op::OP_MOVE, 32, reg(IA32_REG_ESI, 32), old0);
        // memory == 0x11111111: written
        SemanticOperand old1 = atomic.newTemporary(SemanticType::integer(32));
        emit(atomic, Op::OP_ATOMIC_COMPARE_EXCHANGE, 32, old1, imm(0x1000, 32),
             imm(0x11111111, 32), imm(0x22222222, 32));
        emit(atomic, Op::OP_MOVE, 32, reg(IA32_REG_EDI, 32), old1);
        // memory == 0x22222222, expected 0x11111111: not written
        SemanticOperand old2 = atomic.newTemporary(SemanticType::integer(32));
        emit(atomic, Op::OP_ATOMIC_COMPARE_EXCHANGE, 32, old2, imm(0x1000, 32),
             imm(0x11111111, 32), imm(0x33333333, 32));
        emit(atomic, Op::OP_MOVE, 32, reg(IA32_REG_EBP, 32), old2);
        SemanticOperand old3 = atomic.newTemporary(SemanticType::integer(32));
        SemanticInstruction rmw(Op::OP_ATOMIC_RMW, SemanticType::integer(32),
                                old3, imm(0x1000, 32), imm(0x10, 32));
        rmw.setCombineOperation(Op::OP_ADD);
        atomic.append(rmw);
        emit(atomic, Op::OP_MOVE, 32, reg(IA32_REG_EDX, 32), old3);
        check("atomic exit", interpreter.run(atomic),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("exchange old", interpreter.getRegister(IA32_REG_ESI).getWord(0),
              0xAABBCCDDULL);
        check("cmpxchg equal old", interpreter.getRegister(IA32_REG_EDI).getWord(0),
              0x11111111);
        check("cmpxchg different old", interpreter.getRegister(IA32_REG_EBP).getWord(0),
              0x22222222);
        check("rmw old", interpreter.getRegister(IA32_REG_EDX).getWord(0),
              0x22222222);
        check("rmw memory 0", memory.getByte(0x1000), 0x32);
        check("rmw memory 1", memory.getByte(0x1001), 0x22);

        // A memory fault: the guest instruction which accessed the memory
        memory.m_refused = 0x1800;
        SemanticBlock fault(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        fault.beginGuestInstruction(addr(0x7000), 2);
        emit(fault, Op::OP_MOVE, 32, reg(IA32_REG_ECX, 32), imm(1, 32));
        fault.beginGuestInstruction(addr(0x7002), 3);
        SemanticOperand t2 = fault.newTemporary(SemanticType::integer(32));
        emit(fault, Op::OP_LOAD, 32, t2, imm(0x17FE, 32));
        emit(fault, Op::OP_MOVE, 32, reg(IA32_REG_ECX, 32), t2);
        fault.setFallThroughAddress(addr(0x7005));
        check("fault exit", interpreter.run(fault),
              SemanticInterpreter::EXIT_MEMORY_FAULT);
        check("fault address", interpreter.getFaultAddress(), 0x17FE);
        check("fault next address", interpreter.getNextAddress(), 0x7002);
        check("fault eip", interpreter.getRegister(IA32_REG_EIP).getWord(0),
              0x7002);
        check("fault ecx", interpreter.getRegister(IA32_REG_ECX).getWord(0), 1);
        // A store which faults, and an access outside of the memory
        SemanticBlock storeFault(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        storeFault.beginGuestInstruction(addr(0x7100), 2);
        emit(storeFault, Op::OP_STORE, 16, SemanticOperand(), imm(0x1800, 32),
             imm(1, 16));
        check("store fault exit", interpreter.run(storeFault),
              SemanticInterpreter::EXIT_MEMORY_FAULT);
        check("store fault address", interpreter.getFaultAddress(), 0x1800);
        SemanticBlock outside(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        outside.beginGuestInstruction(addr(0x7200), 2);
        SemanticOperand t3 = outside.newTemporary(SemanticType::integer(8));
        emit(outside, Op::OP_LOAD, 8, t3, imm(0x10, 32));
        check("outside exit", interpreter.run(outside),
              SemanticInterpreter::EXIT_MEMORY_FAULT);
        check("outside address", interpreter.getFaultAddress(), 0x10);
    }

    void testRunMisc()
    {
        TestSemanticMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);

        // The system registers, the select, the compare, and the no-ops
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        emit(block, Op::OP_WRITE_SYSTEM_REGISTER, 32, SemanticOperand(),
             imm(IA32_REG_CR0, 32), imm(0x80000001LL, 32));
        emit(block, Op::OP_READ_SYSTEM_REGISTER, 32, reg(IA32_REG_EAX, 32),
             imm(IA32_REG_CR0, 32));
        emit(block, Op::OP_FENCE, 32, SemanticOperand());
        SemanticInstruction hint(Op::OP_HINT, SemanticType(), SemanticOperand());
        hint.setKind(Op::HINT_SPIN);
        block.append(hint);
        SemanticOperand below = block.newTemporary(SemanticType::integer(1));
        SemanticInstruction compare(Op::OP_COMPARE, SemanticType::integer(32),
                                    below, reg(IA32_REG_EAX, 32),
                                    imm(0x7FFFFFFF, 32));
        compare.setCondition(Op::COND_SIGNED_LESS);
        block.append(compare);
        SemanticOperand selected = block.newTemporary(SemanticType::integer(32));
        emit(block, Op::OP_SELECT, 32, selected, below, imm(111, 32), imm(222, 32));
        emit(block, Op::OP_MOVE, 32, reg(IA32_REG_EBX, 32), selected);
        // Immediates are sign extended into wide values
        SemanticOperand wide = block.newTemporary(SemanticType::integer(128));
        emit(block, Op::OP_MOVE, 128, wide, imm(-1, 128));
        emit(block, Op::OP_MOVE, 128,
             SemanticOperand::registerOperand(IA32_REG_XMM0,
                                              SemanticType::integer(128)), wide);
        check("misc exit", interpreter.run(block),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("cr0", interpreter.getRegister(IA32_REG_CR0).getWord(0), 0x80000001ULL);
        check("system read", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0x80000001ULL);
        // 0x80000001 is negative, so it is less than 0x7FFFFFFF
        check("select", interpreter.getRegister(IA32_REG_EBX).getWord(0), 111);
        check("wide immediate", interpreter.getRegister(IA32_REG_XMM0).getWord(1),
              ~(uint64)0);

        // run() twice: the temporaries start again, the registers stay
        SemanticBlock twice(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        SemanticOperand t0 = twice.newTemporary(SemanticType::integer(32));
        emit(twice, Op::OP_ADD, 32, t0, reg(IA32_REG_ESI, 32), imm(1, 32));
        emit(twice, Op::OP_MOVE, 32, reg(IA32_REG_ESI, 32), t0);
        interpreter.run(twice);
        interpreter.run(twice);
        check("twice", interpreter.getRegister(IA32_REG_ESI).getWord(0), 2);
    }

    //////////////////////////////////////////////////////////////////////
    // The IA-32 lifter
    //////////////////////////////////////////////////////////////////////

    // Decode the instruction 'code' at 0x1000, lift it and run it over
    // 'interpreter'. 'opcode' will hold the decoded instruction. Return the
    // exit of the block (EXIT_UNDEFINED if the instruction isn't lifted)
    SemanticInterpreter::ExitReason liftAndRun(const uint8* code, uint size,
                                               SemanticInterpreter& interpreter,
                                               const SemanticLifterPtr& lifter,
                                               OpcodePtr& opcode)
    {
        cBuffer bytes(size);
        for (uint i = 0; i < size; i++)
            bytes[i] = code[i];
        cMemoryStream stream(bytes);
        StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
            OpcodeSubsystems::DISASSEMBLER_INTEL_32,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0x1000));
        opcode = disassembler->next();
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        lifter->lift(opcode, block);
        block.setFallThroughAddress(addr(0x1000 + size));
        return interpreter.run(block);
    }

    void testLifter()
    {
        ArchitectureDescriptionPtr architecture =
            ArchitectureDescriptionFactory::getArchitecture(
                OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        SemanticLifterPtr lifter = SemanticLifterFactory::getLifter(
                OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        TestSemanticMemory memory;
        SemanticInterpreter interpreter(architecture, memory);
        OpcodePtr opcode;

        // add eax, ebx
        const uint8 add[] = { 0x01, 0xD8 };
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(0x7FFFFFFF));
        interpreter.setRegister(IA32_REG_EBX, SemanticValue(1));
        check("add exit", liftAndRun(add, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("add result", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0x80000000);
        check("add overflow", interpreter.getRegister(IA32_REG_OF).getWord(0), 1);
        check("add sign", interpreter.getRegister(IA32_REG_SF).getWord(0), 1);
        check("add carry", interpreter.getRegister(IA32_REG_CF).getWord(0), 0);
        OpcodeClassification addClass = lifter->classify(opcode);
        check("add category", addClass.getCategory(),
              OpcodeClassification::CATEGORY_ARITHMETIC);
        check("add flags written", addClass.getFlagsWritten(),
              SemanticFlags::FLAGS_ARITHMETIC);
        check("add flags undefined", addClass.getFlagsUndefined(), 0);
        TESTS_ASSERT(!addClass.hasProperty(OpcodeClassification::PROPERTY_WRITES_MEMORY));

        // add [esi], eax: reads and writes memory, once
        const uint8 addMemory[] = { 0x01, 0x06 };
        interpreter.setRegister(IA32_REG_ESI, SemanticValue(0x1100));
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(5));
        check("add memory exit", liftAndRun(addMemory, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("add memory", memory.getByte(0x1100), 5);
        TESTS_ASSERT(lifter->classify(opcode).hasProperty(
                OpcodeClassification::PROPERTY_READS_MEMORY |
                OpcodeClassification::PROPERTY_WRITES_MEMORY));

        // The overflow flag of a shift is undefined, except for a count of 1
        const uint8 shiftOne[] = { 0xD1, 0xE0 };
        const uint8 shiftCl[] = { 0xD3, 0xE0 };
        liftAndRun(shiftOne, 2, interpreter, lifter, opcode);
        check("shift one undefined", lifter->classify(opcode).getFlagsUndefined(),
              SemanticFlags::FLAG_ADJUST);
        liftAndRun(shiftCl, 2, interpreter, lifter, opcode);
        check("shift cl undefined", lifter->classify(opcode).getFlagsUndefined(),
              SemanticFlags::FLAG_ADJUST | SemanticFlags::FLAG_OVERFLOW);

        // div ecx by zero is a divide error and changes nothing
        const uint8 divide[] = { 0xF7, 0xF1 };
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(100));
        interpreter.setRegister(IA32_REG_EDX, SemanticValue(0));
        interpreter.setRegister(IA32_REG_ECX, SemanticValue(0));
        check("div exit", liftAndRun(divide, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_TRAP);
        check("div trap", interpreter.getTrapNumber(), 0);
        check("div eax", interpreter.getRegister(IA32_REG_EAX).getWord(0), 100);
        interpreter.setRegister(IA32_REG_ECX, SemanticValue(7));
        check("div ok exit", liftAndRun(divide, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("div quotient", interpreter.getRegister(IA32_REG_EAX).getWord(0), 14);
        check("div remainder", interpreter.getRegister(IA32_REG_EDX).getWord(0), 2);

        // push eax: the stack pointer, the memory and the classification
        const uint8 push[] = { 0x50 };
        interpreter.setRegister(IA32_REG_ESP, SemanticValue(0x1800));
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(0xAB));
        liftAndRun(push, 1, interpreter, lifter, opcode);
        check("push esp", interpreter.getRegister(IA32_REG_ESP).getWord(0), 0x17FC);
        check("push memory", memory.getByte(0x17FC), 0xAB);
        check("push delta", lifter->classify(opcode).getStackDelta(), 4);

        // jz rel8 taken: the target is relative to the next instruction
        const uint8 jump[] = { 0x74, 0x10 };
        interpreter.setRegister(IA32_REG_ZF, SemanticValue(1));
        check("jz exit", liftAndRun(jump, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_BRANCH);
        check("jz target", interpreter.getNextAddress(), 0x1012);
        check("jz category", lifter->classify(opcode).getCategory(),
              OpcodeClassification::CATEGORY_BRANCH);
        check("jz flags read", lifter->classify(opcode).getFlagsRead(),
              SemanticFlags::FLAG_ZERO);

        // An instruction without semantics (rdmsr) is not lifted
        const uint8 rdmsr[] = { 0x0F, 0x32 };
        check("unlifted exit", liftAndRun(rdmsr, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_UNDEFINED);
        check("unlifted category", lifter->classify(opcode).getCategory(),
              OpcodeClassification::CATEGORY_UNKNOWN);

        // cpuid is a helper
        const uint8 cpuid[] = { 0x0F, 0xA2 };
        check("cpuid exit", liftAndRun(cpuid, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_HELPER);
        check("cpuid helper", interpreter.getHelperNumber(), IA32_HELPER_CPUID);

        // mov ds, ax loads the descriptor from the GDT (at 0x1800, 3 entries):
        // a data segment at 0x100000, DPL 0, present, not yet accessed
        const uint8 data[] = { 0xFF, 0xFF, 0x00, 0x00, 0x10, 0x92, 0xCF, 0x00 };
        for (uint i = 0; i < 8; i++)
            memory.write(0x1810 + i, &data[i], 1);
        interpreter.setRegister(IA32_REG_GDTR_BASE, SemanticValue(0x1800));
        interpreter.setRegister(IA32_REG_GDTR_LIMIT, SemanticValue(0x17));
        const uint8 moveDs[] = { 0x8E, 0xD8 };
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(0x10));
        check("mov ds exit", liftAndRun(moveDs, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("mov ds selector", interpreter.getRegister(IA32_REG_DS).getWord(0), 0x10);
        check("mov ds base", interpreter.getRegister(IA32_REG_DS_BASE).getWord(0), 0x100000);
        check("mov ds accessed", memory.getByte(0x1815), 0x93);

        // A selector beyond the limit is #GP and doesn't change ds; a null
        // selector loads with base 0
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(0x18));
        check("mov ds limit exit", liftAndRun(moveDs, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_TRAP);
        check("mov ds limit trap", interpreter.getTrapNumber(), 13);
        check("mov ds limit selector", interpreter.getRegister(IA32_REG_DS).getWord(0), 0x10);
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(0));
        check("mov ds null exit", liftAndRun(moveDs, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("mov ds null base", interpreter.getRegister(IA32_REG_DS_BASE).getWord(0), 0);

        // mov cs, ax is an invalid opcode; mov ax, ds reads the selector
        const uint8 moveCs[] = { 0x8E, 0xC8 };
        check("mov cs exit", liftAndRun(moveCs, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_TRAP);
        check("mov cs trap", interpreter.getTrapNumber(), 6);
    }


    // MMX, SSE and SSE2 through the lifter, see IA32SemanticTableSimd.cpp
    void testSimdLifter()
    {
        ArchitectureDescriptionPtr architecture =
            ArchitectureDescriptionFactory::getArchitecture(
                OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        SemanticLifterPtr lifter = SemanticLifterFactory::getLifter(
                OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        TestSemanticMemory memory;
        SemanticInterpreter interpreter(architecture, memory);
        OpcodePtr opcode;
        SemanticValue value;

        // paddb xmm1, xmm2: every byte wraps on its own
        const uint8 paddb[] = { 0x66, 0x0F, 0xFC, 0xCA };
        value.setWord(0, 0x80FF7F0180FF7F01ULL);
        value.setWord(1, 0x0000000100000001ULL);
        interpreter.setRegister(IA32_REG_XMM1, value);
        value.setWord(0, 0x0101010101010101ULL);
        value.setWord(1, 0xFFFFFFFFFFFFFFFFULL);
        interpreter.setRegister(IA32_REG_XMM2, value);
        check("paddb exit", liftAndRun(paddb, 4, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("paddb low", interpreter.getRegister(IA32_REG_XMM1).getWord(0),
              0x8100800281008002ULL);
        check("paddb high", interpreter.getRegister(IA32_REG_XMM1).getWord(1),
              0xFFFFFF00FFFFFF00ULL);
        OpcodeClassification paddbClass = lifter->classify(opcode);
        check("paddb category", paddbClass.getCategory(),
              OpcodeClassification::CATEGORY_VECTOR_INTEGER);
        TESTS_ASSERT(paddbClass.getDataType() ==
                     SemanticType(SemanticType::KIND_INTEGER, 8, 16));

        // movaps xmm1, [esi]: a memory operand must be aligned to 16 bytes
        const uint8 movaps[] = { 0x0F, 0x28, 0x0E };
        for (uint i = 0; i < 16; i++)
        {
            const uint8 byte = (uint8)(0x10 + i);
            memory.write(0x1100 + i, &byte, 1);
        }
        interpreter.setRegister(IA32_REG_ESI, SemanticValue(0x1101));
        check("movaps misaligned exit", liftAndRun(movaps, 3, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_TRAP);
        check("movaps misaligned trap", interpreter.getTrapNumber(), 13);
        check("movaps misaligned xmm1", interpreter.getRegister(IA32_REG_XMM1).getWord(0),
              0x8100800281008002ULL);
        interpreter.setRegister(IA32_REG_ESI, SemanticValue(0x1100));
        check("movaps exit", liftAndRun(movaps, 3, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("movaps xmm1", interpreter.getRegister(IA32_REG_XMM1).getWord(0),
              0x1716151413121110ULL);
        // movups xmm1, [esi] has no such rule
        const uint8 movups[] = { 0x0F, 0x10, 0x0E };
        interpreter.setRegister(IA32_REG_ESI, SemanticValue(0x1101));
        check("movups exit", liftAndRun(movups, 3, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("movups xmm1", interpreter.getRegister(IA32_REG_XMM1).getWord(0),
              0x1817161514131211ULL);

        // paddb mm1, mm2: the x87 stack is in the MMX state (TOP = 0, all
        // the registers valid), and the register is written with the
        // exponent bits set. Reading mm2 is part of it
        const uint8 paddbMmx[] = { 0x0F, 0xFC, 0xCA };
        interpreter.setRegister(IA32_REG_FPU_STATUS, SemanticValue(0x2800 | 0x0100));
        interpreter.setRegister(IA32_REG_FPU_TAG, SemanticValue(0x24));
        interpreter.setRegister(IA32_REG_FPR1, SemanticValue(0x0102030405060708ULL));
        interpreter.setRegister(IA32_REG_FPR2, SemanticValue(0x0101010101010101ULL));
        check("paddb mm exit", liftAndRun(paddbMmx, 3, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("paddb mm result", interpreter.getRegister(IA32_REG_MM1).getWord(0),
              0x0203040506070809ULL);
        check("paddb mm exponent", interpreter.getRegister(IA32_REG_FPR1).getWord(1),
              0xFFFF);
        check("paddb mm status", interpreter.getRegister(IA32_REG_FPU_STATUS).getWord(0),
              0x0100);
        check("paddb mm tags", interpreter.getRegister(IA32_REG_FPU_TAG).getWord(0), 0xFF);
        // emms empties the registers
        const uint8 emms[] = { 0x0F, 0x77 };
        check("emms exit", liftAndRun(emms, 2, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("emms tags", interpreter.getRegister(IA32_REG_FPU_TAG).getWord(0), 0);

        // addss xmm1, xmm2: the low lane, the others are kept. 1.0 + 2^-25
        // is inexact (PE), the default rounding gives 1.0
        const uint8 addss[] = { 0xF3, 0x0F, 0x58, 0xCA };
        value = SemanticValue();
        value.setWord(0, 0x1111111100000000ULL | 0x3F800000);
        value.setWord(1, 0x2222222233333333ULL);
        interpreter.setRegister(IA32_REG_XMM1, value);
        value = SemanticValue();
        value.setWord(0, 0x33000000);
        interpreter.setRegister(IA32_REG_XMM2, value);
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80));
        check("addss exit", liftAndRun(addss, 4, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("addss low", interpreter.getRegister(IA32_REG_XMM1).getWord(0),
              0x111111113F800000ULL);
        check("addss high", interpreter.getRegister(IA32_REG_XMM1).getWord(1),
              0x2222222233333333ULL);
        check("addss mxcsr", interpreter.getRegister(IA32_REG_MXCSR).getWord(0), 0x1FA0);
        check("addss category", lifter->classify(opcode).getCategory(),
              OpcodeClassification::CATEGORY_FLOAT);
        TESTS_ASSERT(lifter->classify(opcode).getDataType() ==
                     SemanticType::floating(SemanticType::FLOAT_SINGLE));
        // An unmasked overflow is #XM (19), before the register is written
        value = SemanticValue();
        value.setWord(0, 0x7F7FFFFF);
        interpreter.setRegister(IA32_REG_XMM1, value);
        interpreter.setRegister(IA32_REG_XMM2, value);
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80 & ~0x400));
        check("addss overflow exit", liftAndRun(addss, 4, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_TRAP);
        check("addss overflow trap", interpreter.getTrapNumber(), 19);
        check("addss overflow xmm1", interpreter.getRegister(IA32_REG_XMM1).getWord(0),
              0x7F7FFFFF);

        // cvttss2si eax, xmm2: a NaN is the integer indefinite and invalid
        const uint8 cvttss2si[] = { 0xF3, 0x0F, 0x2C, 0xC2 };
        value = SemanticValue();
        value.setWord(0, 0x7FC00000);
        interpreter.setRegister(IA32_REG_XMM2, value);
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80));
        check("cvttss2si exit", liftAndRun(cvttss2si, 4, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("cvttss2si eax", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0x80000000);
        check("cvttss2si mxcsr", interpreter.getRegister(IA32_REG_MXCSR).getWord(0), 0x1F81);

        // comiss xmm1, xmm2: less sets only the carry flag
        const uint8 comiss[] = { 0x0F, 0x2F, 0xCA };
        value = SemanticValue();
        value.setWord(0, 0x3F800000);
        interpreter.setRegister(IA32_REG_XMM1, value);
        value.setWord(0, 0x40000000);
        interpreter.setRegister(IA32_REG_XMM2, value);
        interpreter.setRegister(IA32_REG_ZF, SemanticValue(1));
        interpreter.setRegister(IA32_REG_OF, SemanticValue(1));
        check("comiss exit", liftAndRun(comiss, 3, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("comiss carry", interpreter.getRegister(IA32_REG_CF).getWord(0), 1);
        check("comiss zero", interpreter.getRegister(IA32_REG_ZF).getWord(0), 0);
        check("comiss overflow", interpreter.getRegister(IA32_REG_OF).getWord(0), 0);

        // rcpps is a helper: the table of the estimate isn't public
        const uint8 rcpps[] = { 0x0F, 0x53, 0xCA };
        check("rcpps exit", liftAndRun(rcpps, 3, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_HELPER);
        check("rcpps helper", interpreter.getHelperNumber(),
              IA32_HELPER_RECIPROCAL_ESTIMATE);

        // ldmxcsr [esi]: a reserved bit is #GP
        const uint8 ldmxcsr[] = { 0x0F, 0xAE, 0x16 };
        interpreter.setRegister(IA32_REG_ESI, SemanticValue(0x1200));
        const uint8 good[] = { 0xC0, 0x9F, 0x00, 0x00 };
        for (uint i = 0; i < 4; i++)
            memory.write(0x1200 + i, &good[i], 1);
        check("ldmxcsr exit", liftAndRun(ldmxcsr, 3, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("ldmxcsr value", interpreter.getRegister(IA32_REG_MXCSR).getWord(0), 0x9FC0);
        const uint8 bad = 0x01;
        memory.write(0x1202, &bad, 1);
        check("ldmxcsr reserved exit", liftAndRun(ldmxcsr, 3, interpreter, lifter, opcode),
              SemanticInterpreter::EXIT_TRAP);
        check("ldmxcsr reserved trap", interpreter.getTrapNumber(), 13);
        check("ldmxcsr reserved value", interpreter.getRegister(IA32_REG_MXCSR).getWord(0),
              0x9FC0);
    }

    virtual void test()
    {
        testTypes();
        testValues();
        testEvaluateInteger();
        testEvaluateMove();
        testEvaluateVector();
        testEvaluateAes();
        testFlags();
        testArchitecture();
        testRunRegisters();
        testRunLoop();
        testRunFlags();
        testRunExits();
        testRunMemory();
        testRunMisc();
        testLifter();
        testSimdLifter();
        cout << "Semantics tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestSemantics g_globalTestSemantics;
