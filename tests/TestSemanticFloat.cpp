/*
 * TestSemanticFloat.cpp
 *
 * Tests the software floating point library (SemanticFloat) and the floating
 * point and vector operations of SemanticInterpreter:
 *   - hand written vectors for each operation, format, rounding mode and edge
 *     case,
 *   - the generated reference vectors of tests/semantics/float_vectors.inc
 *     (computed by tests/semantics/tools/gen_float_vectors.py with exact
 *     rational arithmetic),
 *   - the interpreter: the environment of the guest (MXCSR, the x87 control
 *     word), the exception flags and traps, and the vector permutations.
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
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/semantics/Semantics.h"
#include "dismount/semantics/SemanticFloat.h"
#include "dismount/proc/ia32/IA32Architecture.h"

using namespace ia32dis;

typedef SemanticOperation Op;
typedef SemanticFloat SF;
typedef SemanticFloat::Bits FB;
typedef SemanticType::FloatFormat Format;

// The generated vectors
struct FloatVector {
    uint8 m_operation;
    uint8 m_format;
    // The second format of a conversion or the bits of an integer
    uint8 m_format2;
    uint8 m_mode;
    uint8 m_precision;
    uint8 m_flags;
    uint64 m_a[2];
    uint64 m_b[2];
    uint64 m_c[2];
    uint64 m_result[2];
};

// The operation codes of the vectors, see gen_float_vectors.py
enum {
    V_ADD, V_SUB, V_MUL, V_DIV, V_SQRT, V_FMA, V_CVT, V_FROM_SIGNED,
    V_FROM_UNSIGNED, V_TO_SIGNED, V_TO_UNSIGNED, V_ROUND, V_ROUND_EXACT,
    V_COUNT
};

static const FloatVector gVectors[] = {
#include "semantics/float_vectors.inc"
};

static const Format gFormats[5] = {
    SemanticType::FLOAT_SINGLE, SemanticType::FLOAT_DOUBLE,
    SemanticType::FLOAT_EXTENDED, SemanticType::FLOAT_HALF,
    SemanticType::FLOAT_BFLOAT16 };

static const Op::RoundingMode gModes[6] = {
    Op::ROUND_NEAREST_EVEN, Op::ROUND_NEAREST_AWAY, Op::ROUND_TOWARD_ZERO,
    Op::ROUND_TOWARD_POSITIVE, Op::ROUND_TOWARD_NEGATIVE, Op::ROUND_TO_ODD };

// The memory of the interpreter tests: BASE .. BASE + SIZE
class FloatTestMemory : public SemanticMemory {
public:
    enum { BASE = 0x1000, SIZE = 0x100 };

    FloatTestMemory() : m_refused(0xFFFFFFFFFFFFFFFFULL), m_data(SIZE)
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

    void setByte(uint64 address, uint8 value)
    {
        m_data[(uint)(address - BASE)] = value;
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

class TestObjectTestSemanticFloat : public cTestObject {
public:
    //////////////////////////////////////////////////////////////////////
    // Checks
    //////////////////////////////////////////////////////////////////////

    void check(const char* what, uint64 actual, uint64 expected)
    {
        if (actual != expected)
        {
            cout << "FAILED " << what << ": got " << HEXQWORD(actual)
                 << " expected " << HEXQWORD(expected) << endl;
        }
        TESTS_ASSERT_EQUAL(actual, expected);
    }

    void checkBits(const char* what, const FB& actual, uint64 low,
                   uint64 high = 0)
    {
        if ((actual.m_low != low) || (actual.m_high != high))
        {
            cout << "FAILED " << what << ": got " << HEXQWORD(actual.m_high)
                 << ":" << HEXQWORD(actual.m_low) << " expected "
                 << HEXQWORD(high) << ":" << HEXQWORD(low) << endl;
        }
        TESTS_ASSERT_EQUAL(actual.m_low, low);
        TESTS_ASSERT_EQUAL(actual.m_high, high);
    }

    // Check a result and its flags
    void checkResult(const char* what, const FB& actual, uint32 flags,
                     uint64 low, uint64 high, uint32 expectedFlags)
    {
        checkBits(what, actual, low, high);
        check(what, flags, expectedFlags);
    }

    //////////////////////////////////////////////////////////////////////
    // The generated vectors
    //////////////////////////////////////////////////////////////////////

    void testGeneratedVectors()
    {
        const uint count = sizeof(gVectors) / sizeof(gVectors[0]);
        uint counts[V_COUNT][5];
        for (uint i = 0; i < V_COUNT; i++)
            for (uint j = 0; j < 5; j++)
                counts[i][j] = 0;
        uint failures = 0;
        for (uint i = 0; i < count; i++)
        {
            const FloatVector& v = gVectors[i];
            const Format format = gFormats[v.m_format];
            SF::Environment env;
            env.m_rounding = gModes[v.m_mode];
            env.m_precision = v.m_precision;
            const FB a(v.m_a[0], v.m_a[1]);
            const FB b(v.m_b[0], v.m_b[1]);
            const FB c(v.m_c[0], v.m_c[1]);
            uint32 flags = 0;
            FB result;
            switch (v.m_operation)
            {
            case V_ADD: result = SF::add(format, env, a, b, flags); break;
            case V_SUB: result = SF::subtract(format, env, a, b, flags); break;
            case V_MUL: result = SF::multiply(format, env, a, b, flags); break;
            case V_DIV: result = SF::divide(format, env, a, b, flags); break;
            case V_SQRT: result = SF::squareRoot(format, env, a, flags); break;
            case V_FMA:
                result = SF::multiplyAdd(format, env, a, b, c, flags); break;
            case V_CVT:
                result = SF::convert(format, gFormats[v.m_format2], env, a,
                                     flags);
                break;
            case V_FROM_SIGNED:
            case V_FROM_UNSIGNED:
                result = SF::fromInteger(format, env, v.m_a[0], v.m_format2,
                                         v.m_operation == V_FROM_SIGNED,
                                         flags);
                break;
            case V_TO_SIGNED:
            case V_TO_UNSIGNED:
                result = FB(SF::toInteger(format, env, a, v.m_format2,
                                          v.m_operation == V_TO_SIGNED,
                                          Op::BEHAVIOR_CONVERT_INDEFINITE,
                                          flags), 0);
                break;
            case V_ROUND:
            case V_ROUND_EXACT:
                result = SF::roundToIntegral(format, env, a,
                                             v.m_operation == V_ROUND_EXACT,
                                             flags);
                break;
            default:
                CHECK_FAIL();
            }
            counts[v.m_operation][v.m_format]++;
            if ((result.m_low != v.m_result[0]) ||
                (result.m_high != v.m_result[1]) || (flags != v.m_flags))
            {
                if (failures < 20)
                {
                    cout << "FAILED vector " << i << " op "
                         << (uint)v.m_operation << " format "
                         << (uint)v.m_format << "/" << (uint)v.m_format2
                         << " mode " << (uint)v.m_mode << " pc "
                         << (uint)v.m_precision << " a=" << HEXQWORD(v.m_a[1])
                         << ":" << HEXQWORD(v.m_a[0]) << " b="
                         << HEXQWORD(v.m_b[1]) << ":" << HEXQWORD(v.m_b[0])
                         << " c=" << HEXQWORD(v.m_c[1]) << ":"
                         << HEXQWORD(v.m_c[0]) << " got "
                         << HEXQWORD(result.m_high) << ":"
                         << HEXQWORD(result.m_low) << "/" << flags
                         << " expected " << HEXQWORD(v.m_result[1]) << ":"
                         << HEXQWORD(v.m_result[0]) << "/" << (uint)v.m_flags
                         << endl;
                }
                failures++;
            }
        }
        static const char* gNames[V_COUNT] = { "add", "sub", "mul", "div",
            "sqrt", "fma", "convert", "from signed", "from unsigned",
            "to signed", "to unsigned", "round", "round exact" };
        cout << "Generated float vectors: " << count << " (f32 f64 f80 f16 "
             << "bf16 per operation)" << endl;
        for (uint i = 0; i < V_COUNT; i++)
        {
            cout << "  " << gNames[i] << ": " << counts[i][0] << " "
                 << counts[i][1] << " " << counts[i][2] << " " << counts[i][3]
                 << " " << counts[i][4] << endl;
        }
        check("generated vector failures", failures, 0);
    }

    //////////////////////////////////////////////////////////////////////
    // Hand vectors
    //////////////////////////////////////////////////////////////////////

    static FB s(uint32 v) { return FB(v, 0); }
    static FB d(uint64 v) { return FB(v, 0); }

    enum { INV = 1, DEN = 2, DZ = 4, OVF = 8, UNF = 0x10, INX = 0x20 };

    void testSingleBasic()
    {
        const Format f = SemanticType::FLOAT_SINGLE;
        SF::Environment env;
        uint32 fl = 0;
        checkBits("1+2", SF::add(f, env, s(0x3F800000), s(0x40000000), fl),
                  0x40400000);
        check("1+2 flags", fl, 0);
        fl = 0;
        // 0.1f + 0.2f = 0.3f (0x3E99999A), inexact
        checkBits("0.1+0.2", SF::add(f, env, s(0x3DCCCCCD), s(0x3E4CCCCD), fl),
                  0x3E99999A);
        check("0.1+0.2 flags", fl, INX);
        fl = 0;
        checkBits("sub", SF::subtract(f, env, s(0x40400000), s(0x3F800000), fl),
                  0x40000000);
        checkBits("mul", SF::multiply(f, env, s(0x40400000), s(0x40000000), fl),
                  0x40C00000);
        checkBits("div", SF::divide(f, env, s(0x3F800000), s(0x40400000), fl),
                  0x3EAAAAAB);
        check("div flags", fl, INX);
        fl = 0;
        checkBits("sqrt 4", SF::squareRoot(f, env, s(0x40800000), fl),
                  0x40000000);
        check("sqrt 4 flags", fl, 0);
        checkBits("sqrt 2", SF::squareRoot(f, env, s(0x40000000), fl),
                  0x3FB504F3);
        check("sqrt 2 flags", fl, INX);
        fl = 0;
        // fma(2, 3, 1) = 7 ; with a single rounding
        checkBits("fma", SF::multiplyAdd(f, env, s(0x40000000), s(0x40400000),
                                         s(0x3F800000), fl), 0x40E00000);
        check("fma flags", fl, 0);
        // fma keeps the low bits of the product: (1+2^-12)^2 - 1 - 2^-11
        // = 2^-24 exactly
        fl = 0;
        checkBits("fma single rounding", SF::multiplyAdd(f, env,
                    s(0x3F800800), s(0x3F800800), s(0xBF801000), fl),
                  0x33800000);
        check("fma single rounding flags", fl, 0);
    }

    void testSpecials()
    {
        const Format f = SemanticType::FLOAT_SINGLE;
        SF::Environment env;
        uint32 fl;
        // Signed zeros
        fl = 0;
        checkBits("+0 + -0", SF::add(f, env, s(0), s(0x80000000), fl), 0);
        checkBits("-0 + -0", SF::add(f, env, s(0x80000000), s(0x80000000), fl),
                  0x80000000);
        checkBits("-0 - +0", SF::subtract(f, env, s(0x80000000), s(0), fl),
                  0x80000000);
        checkBits("x - x", SF::subtract(f, env, s(0x40000000), s(0x40000000),
                                        fl), 0);
        SF::Environment down = env;
        down.m_rounding = Op::ROUND_TOWARD_NEGATIVE;
        checkBits("x - x down", SF::subtract(f, down, s(0x40000000),
                                             s(0x40000000), fl), 0x80000000);
        checkBits("+0 + -0 down", SF::add(f, down, s(0), s(0x80000000), fl),
                  0x80000000);
        checkBits("-1 * +0", SF::multiply(f, env, s(0xBF800000), s(0), fl),
                  0x80000000);
        checkBits("sqrt -0", SF::squareRoot(f, env, s(0x80000000), fl),
                  0x80000000);
        check("zero flags", fl, 0);
        // Infinities
        checkBits("inf + 1", SF::add(f, env, s(0x7F800000), s(0x3F800000), fl),
                  0x7F800000);
        checkBits("-inf * -2", SF::multiply(f, env, s(0xFF800000),
                                            s(0xC0000000), fl), 0x7F800000);
        checkBits("1 / inf", SF::divide(f, env, s(0x3F800000), s(0x7F800000),
                                        fl), 0);
        checkBits("-1 / inf", SF::divide(f, env, s(0xBF800000), s(0x7F800000),
                                         fl), 0x80000000);
        check("infinity flags", fl, 0);
        // Invalid operations give the default NaN (negative on x86)
        fl = 0;
        checkBits("inf - inf", SF::subtract(f, env, s(0x7F800000),
                                s(0x7F800000), fl), 0xFFC00000);
        check("inf - inf flags", fl, INV);
        fl = 0;
        checkBits("0 * inf", SF::multiply(f, env, s(0), s(0x7F800000), fl),
                  0xFFC00000);
        check("0 * inf flags", fl, INV);
        fl = 0;
        checkBits("0 / 0", SF::divide(f, env, s(0), s(0), fl), 0xFFC00000);
        check("0 / 0 flags", fl, INV);
        fl = 0;
        checkBits("inf / inf", SF::divide(f, env, s(0x7F800000), s(0x7F800000),
                                          fl), 0xFFC00000);
        check("inf / inf flags", fl, INV);
        fl = 0;
        checkBits("sqrt -1", SF::squareRoot(f, env, s(0xBF800000), fl),
                  0xFFC00000);
        check("sqrt -1 flags", fl, INV);
        fl = 0;
        checkBits("fma 0 * inf + 1", SF::multiplyAdd(f, env, s(0),
                    s(0x7F800000), s(0x3F800000), fl), 0xFFC00000);
        check("fma flags", fl, INV);
        fl = 0;
        checkBits("fma inf - inf", SF::multiplyAdd(f, env, s(0x7F800000),
                    s(0x3F800000), s(0xFF800000), fl), 0xFFC00000);
        check("fma inf - inf flags", fl, INV);
        // Division by zero
        fl = 0;
        checkBits("1 / 0", SF::divide(f, env, s(0x3F800000), s(0), fl),
                  0x7F800000);
        check("1 / 0 flags", fl, DZ);
        fl = 0;
        checkBits("-1 / 0", SF::divide(f, env, s(0xBF800000), s(0), fl),
                  0xFF800000);
        checkBits("1 / -0", SF::divide(f, env, s(0x3F800000), s(0x80000000),
                                       fl), 0xFF800000);
        check("1 / -0 flags", fl, DZ);
        fl = 0;
        checkBits("inf / 0", SF::divide(f, env, s(0x7F800000), s(0), fl),
                  0x7F800000);
        check("inf / 0 flags", fl, 0);
        // The default NaN of ARM is positive
        SF::Environment arm = env;
        arm.m_isDefaultNanNegative = false;
        checkBits("positive default NaN", SF::subtract(f, arm, s(0x7F800000),
                                    s(0x7F800000), fl), 0x7FC00000);
    }

    void testOverflowAndRounding()
    {
        const Format f = SemanticType::FLOAT_SINGLE;
        uint32 fl;
        const uint32 maxFinite = 0x7F7FFFFF;
        for (uint m = 0; m < 6; m++)
        {
            SF::Environment env;
            env.m_rounding = gModes[m];
            // max + max overflows
            fl = 0;
            FB r = SF::add(f, env, s(maxFinite), s(maxFinite), fl);
            const bool toInfinity = (m == 0) || (m == 1) || (m == 3);
            checkBits("overflow +", r, toInfinity ? 0x7F800000 : maxFinite);
            check("overflow flags", fl, OVF | INX);
            fl = 0;
            r = SF::add(f, env, s(0x80000000 | maxFinite),
                        s(0x80000000 | maxFinite), fl);
            const bool toNegInfinity = (m == 0) || (m == 1) || (m == 4);
            checkBits("overflow -", r, toNegInfinity ? 0xFF800000
                                                     : (0x80000000 | maxFinite));
        }
        // 1 + 2^-24 is a tie between 1 and 1 + 2^-23
        SF::Environment env;
        const uint32 one = 0x3F800000, tie = 0x33800000;
        const uint32 expected[6] = { one, 0x3F800001, one, 0x3F800001, one,
                                     0x3F800001 };
        for (uint m = 0; m < 6; m++)
        {
            env.m_rounding = gModes[m];
            fl = 0;
            checkBits("tie", SF::add(f, env, s(one), s(tie), fl), expected[m]);
            check("tie flags", fl, INX);
        }
        // 1 + 3 * 2^-25 is above the tie: nearest even rounds up
        env.m_rounding = Op::ROUND_NEAREST_EVEN;
        fl = 0;
        checkBits("above tie", SF::add(f, env, s(one), s(0x33C00000), fl),
                  0x3F800001);
        // The sign matters for the directed modes
        env.m_rounding = Op::ROUND_TOWARD_POSITIVE;
        checkBits("neg tie up", SF::add(f, env, s(0xBF800000), s(0xB3800000),
                                        fl), 0xBF800000);
        env.m_rounding = Op::ROUND_TOWARD_NEGATIVE;
        checkBits("neg tie down", SF::add(f, env, s(0xBF800000), s(0xB3800000),
                                          fl), 0xBF800001);
    }

    void testDenormals()
    {
        const Format f = SemanticType::FLOAT_SINGLE;
        SF::Environment env;
        uint32 fl;
        // The smallest denormal / 2 is a tie: rounds to zero (even)
        fl = 0;
        checkBits("denormal underflow", SF::multiply(f, env, s(1),
                                        s(0x3F000000), fl), 0);
        check("denormal underflow flags", fl, DEN | UNF | INX);
        // Exact denormal results are not underflows
        fl = 0;
        checkBits("exact denormal", SF::multiply(f, env, s(0x00800000),
                                        s(0x3F000000), fl), 0x00400000);
        check("exact denormal flags", fl, 0);
        // A denormal operand raises the denormal flag
        fl = 0;
        checkBits("denormal operand", SF::add(f, env, s(1), s(1), fl), 2);
        check("denormal operand flags", fl, DEN);
        // A denormal result rounded up to the smallest normal
        SF::Environment up = env;
        up.m_rounding = Op::ROUND_TOWARD_POSITIVE;
        fl = 0;
        checkBits("denormal to normal", SF::add(f, up, s(0x007FFFFF), s(1),
                                                fl), 0x00800000);
        check("denormal to normal flags", fl, DEN);
        // Flush to zero
        SF::Environment ftz = env;
        ftz.m_flushToZero = true;
        fl = 0;
        checkBits("flush to zero", SF::multiply(f, ftz, s(0x00800000),
                                        s(0x3F000000), fl), 0);
        check("flush to zero flags", fl, UNF | INX);
        // 2^-126 * (1 - 2^-24) is exact with 24 bits, so it's tiny after
        // rounding too
        fl = 0;
        checkBits("ftz tiny", SF::multiply(f, ftz, s(0x00800000),
                    s(0x3F7FFFFF), fl), 0);
        check("ftz tiny flags", fl, UNF | INX);
        // Denormals are zeros
        SF::Environment daz = env;
        daz.m_denormalsAreZero = true;
        fl = 0;
        checkBits("daz", SF::add(f, daz, s(1), s(0x3F800000), fl), 0x3F800000);
        check("daz flags", fl, 0);
        fl = 0;
        checkBits("daz zero", SF::multiply(f, daz, s(0x007FFFFF),
                                           s(0x40000000), fl), 0);
        // An unmasked underflow raises the flag even if it's exact
        SF::Environment unmasked = env;
        unmasked.m_isUnderflowUnmasked = true;
        fl = 0;
        checkBits("unmasked underflow", SF::multiply(f, unmasked,
                    s(0x00800000), s(0x3F000000), fl), 0x00400000);
        check("unmasked underflow flags", fl, UNF);
        // 2^-127 * (2 - 2^-23) needs 24 bits: a tie in the denormal range
        // which rounds up to the smallest normal. It's tiny (and inexact)
        // both before and after the rounding with an unbounded exponent
        SF::Environment before = env;
        before.m_isTininessBeforeRounding = true;
        fl = 0;
        checkBits("tie to min normal", SF::multiply(f, env, s(0x00FFFFFF),
                    s(0x3F000000), fl), 0x00800000);
        check("tie to min normal flags", fl, UNF | INX);
        fl = 0;
        SF::multiply(f, before, s(0x00FFFFFF), s(0x3F000000), fl);
        check("before rounding flags", fl, UNF | INX);
        // The same for double
        const Format fd = SemanticType::FLOAT_DOUBLE;
        fl = 0;
        checkBits("double denormal", SF::multiply(fd, env,
                    d(0x0010000000000000ULL), d(0x3FE0000000000000ULL), fl),
                  0x0008000000000000ULL);
        check("double denormal flags", fl, 0);
    }

    void testNanRules()
    {
        const Format f = SemanticType::FLOAT_SINGLE;
        const FB q1 = s(0x7FC00001), q2 = s(0x7FC00002), n5 = s(0x7F800005),
                 n2 = s(0x7F800002), one = s(0x3F800000);
        uint32 fl;

        // SSE: the first NaN operand, quieted
        SF::Environment sse;
        fl = 0;
        checkBits("sse q1 q2", SF::add(f, sse, q1, q2, fl), 0x7FC00001);
        check("sse flags", fl, 0);
        checkBits("sse q2 q1", SF::add(f, sse, q2, q1, fl), 0x7FC00002);
        checkBits("sse one q2", SF::multiply(f, sse, one, q2, fl), 0x7FC00002);
        fl = 0;
        checkBits("sse q1 n5", SF::add(f, sse, q1, n5, fl), 0x7FC00001);
        check("sse q1 n5 flags", fl, INV);
        fl = 0;
        checkBits("sse n5 q1", SF::add(f, sse, n5, q1, fl), 0x7FC00005);
        check("sse n5 q1 flags", fl, INV);
        fl = 0;
        checkBits("sse sqrt n5", SF::squareRoot(f, sse, n5, fl), 0x7FC00005);
        checkBits("sse fma c", SF::multiplyAdd(f, sse, one, one, q2, fl),
                  0x7FC00002);

        // x87: the larger significand, a quiet NaN before a signaling one
        SF::Environment x87;
        x87.m_nanRule = FloatEnvironment::NAN_LARGER_SIGNIFICAND;
        fl = 0;
        checkBits("x87 q1 q2", SF::add(f, x87, q1, q2, fl), 0x7FC00002);
        checkBits("x87 q2 q1", SF::add(f, x87, q2, q1, fl), 0x7FC00002);
        check("x87 flags", fl, 0);
        fl = 0;
        checkBits("x87 n5 q1", SF::add(f, x87, n5, q1, fl), 0x7FC00001);
        check("x87 n5 q1 flags", fl, INV);
        fl = 0;
        checkBits("x87 n5 n2", SF::add(f, x87, n5, n2, fl), 0x7FC00005);
        // Equal significands: the positive one
        checkBits("x87 equal sign", SF::add(f, x87, s(0xFFC00003),
                                            s(0x7FC00003), fl), 0x7FC00003);

        // ARM: a signaling NaN first, quieted
        SF::Environment arm;
        arm.m_nanRule = FloatEnvironment::NAN_SIGNALING_FIRST;
        arm.m_isDefaultNanNegative = false;
        fl = 0;
        checkBits("arm q1 n5", SF::add(f, arm, q1, n5, fl), 0x7FC00005);
        check("arm flags", fl, INV);
        fl = 0;
        checkBits("arm q1 q2", SF::add(f, arm, q1, q2, fl), 0x7FC00001);
        check("arm q1 q2 flags", fl, 0);
        checkBits("arm n5 n2", SF::add(f, arm, n5, n2, fl), 0x7FC00005);

        // Always the default NaN
        SF::Environment defaultNan;
        defaultNan.m_nanRule = FloatEnvironment::NAN_DEFAULT;
        defaultNan.m_isDefaultNanNegative = false;
        fl = 0;
        checkBits("default q1", SF::add(f, defaultNan, q1, one, fl),
                  0x7FC00000);
        check("default flags", fl, 0);
        defaultNan.m_isDefaultNanNegative = true;
        checkBits("default negative", SF::add(f, defaultNan, q1, one, fl),
                  0xFFC00000);

        // The sign of a NaN is kept
        checkBits("negative NaN", SF::add(f, sse, s(0xFFC00007), one, fl),
                  0xFFC00007);
    }

    void testExtended()
    {
        const Format f = SemanticType::FLOAT_EXTENDED;
        const uint64 intBit = (uint64)1 << 63;
        const FB one(intBit, 0x3FFF), two(intBit, 0x4000),
                 three(intBit | ((uint64)1 << 62), 0x4000);
        SF::Environment env;
        uint32 fl = 0;
        checkBits("1+2", SF::add(f, env, one, two, fl), three.m_low, 0x4000);
        check("1+2 flags", fl, 0);
        checkBits("2*2", SF::multiply(f, env, two, two, fl), intBit, 0x4001);
        checkBits("3/3", SF::divide(f, env, three, three, fl), intBit, 0x3FFF);
        checkBits("sqrt 4", SF::squareRoot(f, env, FB(intBit, 0x4001), fl),
                  intBit, 0x4000);
        check("flags", fl, 0);
        checkBits("neg", SF::negate(f, one), intBit, 0xBFFF);
        checkBits("abs", SF::absolute(f, FB(intBit, 0xBFFF)), intBit, 0x3FFF);
        checkBits("copysign", SF::copySign(f, one, FB(0, 0x8000)), intBit,
                  0xBFFF);

        // The default NaN is the x87 "real indefinite"
        fl = 0;
        checkBits("indefinite", SF::subtract(f, env,
                    FB(intBit, 0x7FFF), FB(intBit, 0x7FFF), fl),
                  intBit | ((uint64)1 << 62), 0xFFFF);
        check("indefinite flags", fl, INV);

        // Invalid encodings are invalid operations
        const FB pseudoDenormal(intBit | 1, 0x0000);
        const FB unnormal(0x4000000000000000ULL, 0x3FFF);
        const FB pseudoInfinity(0, 0x7FFF);
        const FB pseudoNan(0x4000000000000001ULL, 0x7FFF);
        const FB bad[3] = { unnormal, pseudoInfinity, pseudoNan };
        for (uint i = 0; i < 3; i++)
        {
            fl = 0;
            checkBits("invalid encoding", SF::add(f, env, bad[i], one, fl),
                      intBit | ((uint64)1 << 62), 0xFFFF);
            check("invalid encoding flags", fl, INV);
            TESTS_ASSERT(!SF::isValidEncoding(f, bad[i]));
        }
        TESTS_ASSERT(SF::isValidEncoding(f, one));
        TESTS_ASSERT(SF::isValidEncoding(f, FB(1, 0)));
        // A pseudo-denormal (exponent 0, integer bit 1) is a valid operand:
        // the denormal 2^-16382 * (1 + 2^-63). The 387 and later use it as
        // such, and never produce it. 1 + it rounds to 1, inexact
        TESTS_ASSERT(SF::isValidEncoding(f, pseudoDenormal));
        fl = 0;
        checkBits("pseudo-denormal + 1", SF::add(f, env, pseudoDenormal, one, fl),
                  intBit, 0x3FFF);
        check("pseudo-denormal flags", fl, DEN | INX);
        // pseudo-denormal + pseudo-denormal: no carry is lost
        fl = 0;
        checkBits("pseudo-denormal doubled", SF::add(f, env, pseudoDenormal,
                  pseudoDenormal, fl), intBit | 1, 0x0002);
        check("pseudo-denormal doubled flags", fl, DEN);
        fl = 0;
        check("class of invalid", SF::classify(f, unnormal), 0);

        // Precision control: 1 + 2^-30
        const FB tiny(intBit, 0x3FFF - 30);
        SF::Environment pc;
        fl = 0;
        checkBits("pc 64", SF::add(f, pc, one, tiny, fl), intBit | ((uint64)1 << 33),
                  0x3FFF);
        check("pc 64 flags", fl, 0);
        pc.m_precision = 53;
        fl = 0;
        checkBits("pc 53", SF::add(f, pc, one, tiny, fl), intBit | ((uint64)1 << 33),
                  0x3FFF);
        pc.m_precision = 24;
        fl = 0;
        checkBits("pc 24", SF::add(f, pc, one, tiny, fl), intBit, 0x3FFF);
        check("pc 24 flags", fl, INX);
        // 1 / 3 with each precision
        pc.m_precision = 24;
        fl = 0;
        checkBits("1/3 pc24", SF::divide(f, pc, one, three, fl),
                  0xAAAAAB0000000000ULL, 0x3FFD);
        pc.m_precision = 53;
        checkBits("1/3 pc53", SF::divide(f, pc, one, three, fl),
                  0xAAAAAAAAAAAAA800ULL, 0x3FFD);
        pc.m_precision = 0;
        checkBits("1/3 pc64", SF::divide(f, pc, one, three, fl),
                  0xAAAAAAAAAAAAAAABULL, 0x3FFD);
        // The exponent range stays extended with a small precision: overflow
        // at the extended maximum, the largest finite number has 24 bits
        pc.m_precision = 24;
        pc.m_rounding = Op::ROUND_TOWARD_ZERO;
        fl = 0;
        checkBits("pc overflow", SF::multiply(f, pc, FB(~(uint64)0, 0x7FFE),
                    FB(~(uint64)0, 0x7FFE), fl), 0xFFFFFF0000000000ULL, 0x7FFE);
        check("pc overflow flags", fl, OVF | INX);
        // Extended denormals
        env = SF::Environment();
        fl = 0;
        checkBits("ext denormal", SF::multiply(f, env, FB(intBit, 1),
                    FB(intBit, 0x3FFE), fl), intBit >> 1, 0);
        check("ext denormal flags", fl, 0);
        fl = 0;
        checkBits("ext denormal operand", SF::add(f, env, FB(1, 0), FB(1, 0),
                                                  fl), 2, 0);
        check("ext denormal operand flags", fl, DEN);
    }

    void testMinMax()
    {
        const Format f = SemanticType::FLOAT_SINGLE;
        SF::Environment env;
        uint32 fl;
        const FB one = s(0x3F800000), two = s(0x40000000), pz = s(0),
                 nz = s(0x80000000), q = s(0x7FC00001), n = s(0x7F800001);
        // SSE min/max: the second operand for a NaN and for equal values
        fl = 0;
        checkBits("min", SF::minimumMaximum(f, env, false,
                    Op::BEHAVIOR_MINMAX_SECOND, one, two, fl), 0x3F800000);
        checkBits("max", SF::minimumMaximum(f, env, true,
                    Op::BEHAVIOR_MINMAX_SECOND, one, two, fl), 0x40000000);
        check("minmax flags", fl, 0);
        checkBits("min zeros", SF::minimumMaximum(f, env, false,
                    Op::BEHAVIOR_MINMAX_SECOND, pz, nz, fl), 0x80000000);
        checkBits("min zeros 2", SF::minimumMaximum(f, env, false,
                    Op::BEHAVIOR_MINMAX_SECOND, nz, pz, fl), 0);
        fl = 0;
        checkBits("min nan", SF::minimumMaximum(f, env, false,
                    Op::BEHAVIOR_MINMAX_SECOND, q, one, fl), 0x3F800000);
        check("min nan flags", fl, INV);
        fl = 0;
        checkBits("min second nan", SF::minimumMaximum(f, env, false,
                    Op::BEHAVIOR_MINMAX_SECOND, one, n, fl), 0x7F800001);
        check("min second nan flags", fl, INV);
        // Propagate the NaN, -0 < +0
        fl = 0;
        checkBits("propagate", SF::minimumMaximum(f, env, false,
                    Op::BEHAVIOR_MINMAX_PROPAGATE_NAN, one, q, fl),
                  0x7FC00001);
        check("propagate flags", fl, 0);
        checkBits("propagate zeros", SF::minimumMaximum(f, env, false,
                    Op::BEHAVIOR_MINMAX_PROPAGATE_NAN, pz, nz, fl), 0x80000000);
        checkBits("propagate zeros max", SF::minimumMaximum(f, env, true,
                    Op::BEHAVIOR_MINMAX_PROPAGATE_NAN, nz, pz, fl), 0);
        // The number
        fl = 0;
        checkBits("number", SF::minimumMaximum(f, env, false,
                    Op::BEHAVIOR_MINMAX_NUMBER, q, two, fl), 0x40000000);
        checkBits("number 2", SF::minimumMaximum(f, env, true,
                    Op::BEHAVIOR_MINMAX_NUMBER, two, q, fl), 0x40000000);
        check("number flags", fl, 0);
        checkBits("number both nan", SF::minimumMaximum(f, env, true,
                    Op::BEHAVIOR_MINMAX_NUMBER, q, q, fl), 0x7FC00001);
        fl = 0;
        checkBits("number snan", SF::minimumMaximum(f, env, true,
                    Op::BEHAVIOR_MINMAX_NUMBER, n, two, fl), 0x40000000);
        check("number snan flags", fl, INV);
        SF::Environment arm;
        arm.m_nanRule = FloatEnvironment::NAN_SIGNALING_FIRST;
        fl = 0;
        checkBits("arm number snan", SF::minimumMaximum(f, arm, true,
                    Op::BEHAVIOR_MINMAX_NUMBER, n, two, fl), 0x7FC00001);
        check("arm number snan flags", fl, INV);

        // DAZ: a denormal operand is a zero of its sign before the
        // computation, and so is the result (the SDM: "converts all denormal
        // source operands to a zero with the sign of the original operand")
        SF::Environment daz;
        daz.m_denormalsAreZero = true;
        fl = 0;
        checkBits("daz max zeros", SF::minimumMaximum(f, daz, true,
                    Op::BEHAVIOR_MINMAX_SECOND, pz, s(0x007FFFFF), fl), 0);
        checkBits("daz max negative", SF::minimumMaximum(f, daz, true,
                    Op::BEHAVIOR_MINMAX_SECOND, pz, s(0x80000001), fl),
                  0x80000000);
        checkBits("daz min first", SF::minimumMaximum(f, daz, false,
                    Op::BEHAVIOR_MINMAX_SECOND, s(0x80000001), one, fl),
                  0x80000000);
        checkBits("daz min nan", SF::minimumMaximum(f, daz, false,
                    Op::BEHAVIOR_MINMAX_SECOND, q, s(0x00000001), fl), 0);
        check("daz minmax flags", fl, INV);
    }

    void testCompareClassify()
    {
        const Format f = SemanticType::FLOAT_DOUBLE;
        SF::Environment env;
        uint32 fl = 0;
        const FB one = d(0x3FF0000000000000ULL), two = d(0x4000000000000000ULL),
                 q = d(0x7FF8000000000001ULL), n = d(0x7FF0000000000001ULL);
        check("less", SF::compare(f, env, one, two, false, fl),
              SF::RELATION_LESS);
        check("greater", SF::compare(f, env, two, one, false, fl),
              SF::RELATION_GREATER);
        check("equal", SF::compare(f, env, one, one, false, fl),
              SF::RELATION_EQUAL);
        check("zeros", SF::compare(f, env, d(0), d(0x8000000000000000ULL),
                                   false, fl), SF::RELATION_EQUAL);
        check("negative", SF::compare(f, env,
                    d(0xBFF0000000000000ULL), one, false, fl),
              SF::RELATION_LESS);
        check("infinity", SF::compare(f, env, d(0x7FF0000000000000ULL),
                    d(0x7FEFFFFFFFFFFFFFULL), false, fl), SF::RELATION_GREATER);
        check("compare flags", fl, 0);
        // A quiet NaN: unordered, invalid only for the signaling compare
        fl = 0;
        check("unordered", SF::compare(f, env, q, one, false, fl),
              SF::RELATION_UNORDERED);
        check("quiet flags", fl, 0);
        check("unordered signaling", SF::compare(f, env, one, q, true, fl),
              SF::RELATION_UNORDERED);
        check("signaling flags", fl, INV);
        fl = 0;
        SF::compare(f, env, n, one, false, fl);
        check("snan flags", fl, INV);
        fl = 0;
        SF::compare(f, env, d(1), one, false, fl);
        check("denormal compare flags", fl, DEN);

        // classify
        check("class -inf", SF::classify(f, d(0xFFF0000000000000ULL)), 0x001);
        check("class -normal", SF::classify(f, d(0xBFF0000000000000ULL)), 0x002);
        check("class -denormal", SF::classify(f, d(0x8000000000000001ULL)), 0x004);
        check("class -0", SF::classify(f, d(0x8000000000000000ULL)), 0x008);
        check("class +0", SF::classify(f, d(0)), 0x010);
        check("class +denormal", SF::classify(f, d(1)), 0x020);
        check("class +normal", SF::classify(f, one), 0x040);
        check("class +inf", SF::classify(f, d(0x7FF0000000000000ULL)), 0x080);
        check("class snan", SF::classify(f, n), 0x100);
        check("class qnan", SF::classify(f, q), 0x200);
        // Sign operations don't change a NaN
        checkBits("neg nan", SF::negate(f, n), 0xFFF0000000000001ULL);
        checkBits("abs", SF::absolute(f, d(0xBFF0000000000000ULL)),
                  0x3FF0000000000000ULL);
        checkBits("copysign", SF::copySign(f, one, d(0x8000000000000000ULL)),
                  0xBFF0000000000000ULL);
    }

    void testConversions()
    {
        SF::Environment env;
        uint32 fl;
        const Format single = SemanticType::FLOAT_SINGLE;
        const Format dbl = SemanticType::FLOAT_DOUBLE;
        const Format ext = SemanticType::FLOAT_EXTENDED;
        const Format half = SemanticType::FLOAT_HALF;
        const Format bf16 = SemanticType::FLOAT_BFLOAT16;
        fl = 0;
        checkBits("s->d", SF::convert(single, dbl, env, s(0x3FC00000), fl),
                  0x3FF8000000000000ULL);
        checkBits("d->s", SF::convert(dbl, single, env,
                    d(0x3FF8000000000000ULL), fl), 0x3FC00000);
        check("exact flags", fl, 0);
        // d -> s inexact, overflow, underflow
        fl = 0;
        checkBits("d->s inexact", SF::convert(dbl, single, env,
                    d(0x3FF0000000000001ULL), fl), 0x3F800000);
        check("d->s inexact flags", fl, INX);
        fl = 0;
        checkBits("d->s overflow", SF::convert(dbl, single, env,
                    d(0x47F0000000000000ULL), fl), 0x7F800000);
        check("d->s overflow flags", fl, OVF | INX);
        fl = 0;
        checkBits("d->s underflow", SF::convert(dbl, single, env,
                    d(0x36A0000000000000ULL), fl), 1);
        check("d->s exact denormal flags", fl, 0);
        fl = 0;
        checkBits("d->s tiny", SF::convert(dbl, single, env,
                    d(0x3690000000000000ULL), fl), 0);
        check("d->s tiny flags", fl, UNF | INX);
        // NaNs: the payload and the sign are kept, quieted
        fl = 0;
        checkBits("nan d->s", SF::convert(dbl, single, env,
                    d(0x7FF0000040000000ULL), fl), 0x7FC00002);
        check("snan convert flags", fl, INV);
        fl = 0;
        checkBits("nan s->d", SF::convert(single, dbl, env, s(0xFFC00001), fl),
                  0xFFF8000020000000ULL);
        check("qnan convert flags", fl, 0);
        // The extended format
        checkBits("s->e", SF::convert(single, ext, env, s(0x3F800000), fl),
                  (uint64)1 << 63, 0x3FFF);
        checkBits("e->d", SF::convert(ext, dbl, env,
                    FB((uint64)1 << 63, 0x3FFF), fl), 0x3FF0000000000000ULL);
        checkBits("d denormal -> e", SF::convert(dbl, ext, env, d(1), fl),
                  (uint64)1 << 63, 0x3FFF - 1074);
        // Half and bfloat16
        fl = 0;
        checkBits("s->h", SF::convert(single, half, env, s(0x3F800000), fl),
                  0x3C00);
        checkBits("h->s", SF::convert(half, single, env, FB(0x3C00, 0), fl),
                  0x3F800000);
        checkBits("s->bf16", SF::convert(single, bf16, env, s(0x3F800000), fl),
                  0x3F80);
        checkBits("bf16->s", SF::convert(bf16, single, env, FB(0x3F80, 0), fl),
                  0x3F800000);
        check("half flags", fl, 0);
        fl = 0;
        checkBits("half overflow", SF::convert(single, half, env,
                    s(0x477FF000), fl), 0x7C00);
        check("half overflow flags", fl, OVF | INX);
        fl = 0;
        checkBits("half max", SF::convert(single, half, env, s(0x477FE000), fl),
                  0x7BFF);
        checkBits("half denormal", SF::convert(single, half, env,
                    s(0x33800000), fl), 1);
        check("half denormal flags", fl, 0);
        // bfloat16 rounds nearest even
        checkBits("bf16 tie even", SF::convert(single, bf16, env,
                    s(0x3F808000), fl), 0x3F80);
        checkBits("bf16 tie odd", SF::convert(single, bf16, env,
                    s(0x3F818000), fl), 0x3F82);
        checkBits("half infinity", SF::convert(half, dbl, env, FB(0xFC00, 0),
                                               fl), 0xFFF0000000000000ULL);
    }

    void testIntegerConversions()
    {
        SF::Environment env;
        uint32 fl;
        const Format single = SemanticType::FLOAT_SINGLE;
        const Format dbl = SemanticType::FLOAT_DOUBLE;
        const Format ext = SemanticType::FLOAT_EXTENDED;
        // To a float
        fl = 0;
        checkBits("i32 -> s", SF::fromInteger(single, env, 0xFFFFFFFF, 32, true,
                                              fl), 0xBF800000);
        checkBits("u32 -> s", SF::fromInteger(single, env, 0xFFFFFFFF, 32,
                    false, fl), 0x4F800000);
        check("u32 -> s flags", fl, INX);
        fl = 0;
        checkBits("i64 -> d", SF::fromInteger(dbl, env, 0x8000000000000000ULL,
                    64, true, fl), 0xC3E0000000000000ULL);
        check("i64 -> d flags", fl, 0);
        checkBits("u64 -> e", SF::fromInteger(ext, env, 0xFFFFFFFFFFFFFFFFULL,
                    64, false, fl), 0xFFFFFFFFFFFFFFFFULL, 0x403E);
        check("u64 -> e flags", fl, 0);
        checkBits("i8 -> d", SF::fromInteger(dbl, env, 0x80, 8, true, fl),
                  0xC060000000000000ULL);
        checkBits("zero", SF::fromInteger(dbl, env, 0, 32, true, fl), 0);

        // From a float: the rounding mode
        const FB twoHalf = s(0x40200000), negTwoHalf = s(0xC0200000);
        const uint64 expected[6] = { 2, 3, 2, 3, 2, 3 };
        const uint64 negExpected[6] = { (uint64)-2, (uint64)-3, (uint64)-2,
                                        (uint64)-2, (uint64)-3, (uint64)-3 };
        for (uint m = 0; m < 6; m++)
        {
            env.m_rounding = gModes[m];
            fl = 0;
            check("2.5 -> i32", SF::toInteger(single, env, twoHalf, 32, true,
                    Op::BEHAVIOR_CONVERT_INDEFINITE, fl), expected[m]);
            check("2.5 flags", fl, INX);
            check("-2.5 -> i64", SF::toInteger(single, env, negTwoHalf, 64,
                    true, Op::BEHAVIOR_CONVERT_INDEFINITE, fl), negExpected[m]);
        }
        env.m_rounding = Op::ROUND_TOWARD_ZERO;
        // Out of range and NaN: the three behaviors
        const FB big = s(0x4F000000);       // 2^31
        const FB nan = s(0x7FC00000);
        const FB neg = s(0xBF800000);       // -1
        fl = 0;
        check("indefinite", SF::toInteger(single, env, big, 32, true,
                    Op::BEHAVIOR_CONVERT_INDEFINITE, fl), 0x80000000);
        check("indefinite flags", fl, INV);
        fl = 0;
        check("saturate", SF::toInteger(single, env, big, 32, true,
                    Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO, fl), 0x7FFFFFFF);
        check("saturate flags", fl, INV);
        check("saturate nan zero", SF::toInteger(single, env, nan, 32, true,
                    Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO, fl), 0);
        check("saturate nan max", SF::toInteger(single, env, nan, 32, true,
                    Op::BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM, fl), 0x7FFFFFFF);
        check("saturate nan max u", SF::toInteger(single, env, nan, 32, false,
                    Op::BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM, fl), 0xFFFFFFFF);
        check("indefinite nan", SF::toInteger(single, env, nan, 32, true,
                    Op::BEHAVIOR_CONVERT_INDEFINITE, fl), 0x80000000);
        check("indefinite unsigned", SF::toInteger(single, env, neg, 32, false,
                    Op::BEHAVIOR_CONVERT_INDEFINITE, fl), 0xFFFFFFFF);
        check("saturate unsigned", SF::toInteger(single, env, neg, 32, false,
                    Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO, fl), 0);
        check("saturate -inf", SF::toInteger(single, env, s(0xFF800000), 16,
                    true, Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO, fl), 0x8000);
        check("i16 saturate", SF::toInteger(single, env, s(0x47800000), 16, true,
                    Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO, fl), 0x7FFF);
        // -2^31 fits, 2^31 doesn't
        fl = 0;
        check("min i32", SF::toInteger(single, env, s(0xCF000000), 32, true,
                    Op::BEHAVIOR_CONVERT_INDEFINITE, fl), 0x80000000);
        check("min i32 flags", fl, 0);
        // -0.5 truncated to an unsigned is 0, inexact
        fl = 0;
        check("-0.5 unsigned", SF::toInteger(single, env, s(0xBF000000), 32,
                    false, Op::BEHAVIOR_CONVERT_INDEFINITE, fl), 0);
        check("-0.5 unsigned flags", fl, INX);
        // 64 bit
        fl = 0;
        check("u64 max", SF::toInteger(dbl, env, d(0x43EFFFFFFFFFFFFFULL), 64,
                    false, Op::BEHAVIOR_CONVERT_INDEFINITE, fl),
              0xFFFFFFFFFFFFF800ULL);
        check("u64 max flags", fl, 0);
        fl = 0;
        check("2^64", SF::toInteger(dbl, env, d(0x43F0000000000000ULL), 64,
                    false, Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO, fl),
              0xFFFFFFFFFFFFFFFFULL);
        check("2^64 flags", fl, INV);
        fl = 0;
        check("i64 min", SF::toInteger(dbl, env, d(0xC3E0000000000000ULL), 64,
                    true, Op::BEHAVIOR_CONVERT_INDEFINITE, fl),
              0x8000000000000000ULL);
        check("i64 min flags", fl, 0);
        // A pseudo denormal is a tiny number: 0, inexact (no denormal exception)
        fl = 0;
        check("pseudo-denormal", SF::toInteger(ext, env,
                    FB((uint64)1 << 63, 0), 32, true,
                    Op::BEHAVIOR_CONVERT_INDEFINITE, fl), 0);
        check("pseudo-denormal flags", fl, INX);
        // An unnormal is invalid
        fl = 0;
        check("invalid encoding", SF::toInteger(ext, env,
                    FB((uint64)1 << 62, 0x3FFF), 32, true,
                    Op::BEHAVIOR_CONVERT_INDEFINITE, fl), 0x80000000);
        check("invalid encoding flags", fl, INV);
    }

    void testRoundScale()
    {
        SF::Environment env;
        uint32 fl;
        const Format f = SemanticType::FLOAT_DOUBLE;
        const FB twoHalf = d(0x4004000000000000ULL);
        fl = 0;
        checkBits("round 2.5", SF::roundToIntegral(f, env, twoHalf, true, fl),
                  0x4000000000000000ULL);
        check("round inexact", fl, INX);
        fl = 0;
        checkBits("round 2.5 quiet", SF::roundToIntegral(f, env, twoHalf, false,
                                                         fl), 0x4000000000000000ULL);
        check("round suppressed", fl, 0);
        env.m_rounding = Op::ROUND_NEAREST_AWAY;
        checkBits("round away", SF::roundToIntegral(f, env, twoHalf, true, fl),
                  0x4008000000000000ULL);
        env.m_rounding = Op::ROUND_TOWARD_NEGATIVE;
        checkBits("round down -0.4", SF::roundToIntegral(f, env,
                    d(0xBFD999999999999AULL), true, fl), 0xBFF0000000000000ULL);
        env.m_rounding = Op::ROUND_TOWARD_ZERO;
        checkBits("round -0.4 to -0", SF::roundToIntegral(f, env,
                    d(0xBFD999999999999AULL), true, fl), 0x8000000000000000ULL);
        fl = 0;
        checkBits("round integral", SF::roundToIntegral(f, env,
                    d(0x4330000000000001ULL), true, fl), 0x4330000000000001ULL);
        check("round integral flags", fl, 0);
        checkBits("round inf", SF::roundToIntegral(f, env,
                    d(0x7FF0000000000000ULL), true, fl), 0x7FF0000000000000ULL);
        // scale
        env = SF::Environment();
        fl = 0;
        checkBits("scale", SF::scale(f, env, d(0x3FF8000000000000ULL),
                    d(0x4008000000000000ULL), fl), 0x4028000000000000ULL);
        checkBits("scale floor", SF::scale(f, env, d(0x3FF0000000000000ULL),
                    d(0xBFE0000000000000ULL), fl), 0x3FE0000000000000ULL);
        checkBits("scale huge", SF::scale(f, env, d(0x3FF0000000000000ULL),
                    d(0x4130000000000000ULL), fl), 0x7FF0000000000000ULL);
        check("scale huge flags", fl, OVF | INX);
        fl = 0;
        checkBits("scale zero inf", SF::scale(f, env, d(0),
                    d(0x7FF0000000000000ULL), fl), 0xFFF8000000000000ULL);
        check("scale zero inf flags", fl, INV);
        // exponent
        fl = 0;
        checkBits("exponent", SF::getExponent(f, env,
                    d(0x4020000000000000ULL), fl), 0x4008000000000000ULL);
        checkBits("exponent zero", SF::getExponent(f, env, d(0), fl),
                  0xFFF0000000000000ULL);
        checkBits("exponent denormal", SF::getExponent(f, env, d(1), fl),
                  0xC090C80000000000ULL);
    }

    //////////////////////////////////////////////////////////////////////
    // The interpreter
    //////////////////////////////////////////////////////////////////////

    static ProcessorAddress addr(uint32 address)
    {
        return ProcessorAddress(ProcessorAddress::PROCESSOR_32, address);
    }

    static ArchitectureDescriptionPtr getArchitecture()
    {
        return ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DISASSEMBLER_INTEL_32);
    }

    static SemanticOperand reg(uint id, const SemanticType& type)
    {
        return SemanticOperand::registerOperand(id, type);
    }

    static SemanticValue vec(uint64 low, uint64 high = 0)
    {
        SemanticValue v(low);
        v.setWord(1, high);
        return v;
    }

    // A block with the single instruction 'instruction'
    static SemanticInstruction::Attribute sse()
    {
        return (SemanticInstruction::Attribute)
                    SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS;
    }

    void runOne(SemanticInterpreter& interpreter, const SemanticInstruction& ins,
                SemanticInterpreter::ExitReason expected)
    {
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        block.beginGuestInstruction(addr(0x400), 3);
        block.append(ins);
        block.setFallThroughAddress(addr(0x403));
        check("exit", interpreter.run(block), expected);
    }

    void testInterpreterFloat()
    {
        FloatTestMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);
        const SemanticType f32 = SemanticType::floating(SemanticType::FLOAT_SINGLE);
        const SemanticType f64 = SemanticType::floating(SemanticType::FLOAT_DOUBLE);
        const SemanticType v4f32 = SemanticType::vector(f32, 4);
        const SemanticType v2f64 = SemanticType::vector(f64, 2);

        // addpd with the default MXCSR (round nearest, all masked)
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80));
        interpreter.setRegister(IA32_REG_XMM1, vec(0x3FF0000000000000ULL,
                                                   0x4000000000000000ULL));
        interpreter.setRegister(IA32_REG_XMM2, vec(0x3CA0000000000001ULL,
                                                   0x4008000000000000ULL));
        SemanticInstruction add(Op::OP_FLOAT_ADD, v2f64,
                reg(IA32_REG_XMM0, v2f64), reg(IA32_REG_XMM1, v2f64),
                reg(IA32_REG_XMM2, v2f64));
        add.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
        runOne(interpreter, add, SemanticInterpreter::EXIT_FALL_THROUGH);
        SemanticValue x0 = interpreter.getRegister(IA32_REG_XMM0);
        // 1 + (2^-54 + tiny) rounds up
        check("addpd lane 0", x0.getWord(0), 0x3FF0000000000001ULL);
        check("addpd lane 1", x0.getWord(1), 0x4014000000000000ULL);
        check("addpd inexact", interpreter.getRegister(IA32_REG_MXCSR).getWord(0),
              0x1F80 | INX);

        // The rounding mode from MXCSR: toward zero (bits 13-14 = 3)
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x7F80));
        runOne(interpreter, add, SemanticInterpreter::EXIT_FALL_THROUGH);
        x0 = interpreter.getRegister(IA32_REG_XMM0);
        check("addpd toward zero", x0.getWord(0), 0x3FF0000000000000ULL);
        check("sticky flags", interpreter.getRegister(IA32_REG_MXCSR).getWord(0),
              0x7F80 | INX);
        // An explicit rounding mode in the instruction wins
        SemanticInstruction addUp(add);
        addUp.setRoundingMode(Op::ROUND_TOWARD_POSITIVE);
        runOne(interpreter, addUp, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("addpd explicit up", interpreter.getRegister(IA32_REG_XMM0).getWord(0),
              0x3FF0000000000001ULL);
        // Without ATTRIBUTE_FLOAT_EXCEPTIONS the flags are not recorded
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80));
        SemanticInstruction noFlags(add);
        noFlags.setAttributes(0);
        runOne(interpreter, noFlags, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("no flags", interpreter.getRegister(IA32_REG_MXCSR).getWord(0), 0x1F80);

        // FTZ + DAZ: the denormal operand is a zero
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80 | 0x8040));
        interpreter.setRegister(IA32_REG_XMM1, vec(0x0000000100000000ULL, 0));
        interpreter.setRegister(IA32_REG_XMM2, vec(0x0000000100000000ULL, 0));
        SemanticInstruction addPs(Op::OP_FLOAT_ADD, v4f32,
                reg(IA32_REG_XMM0, v4f32), reg(IA32_REG_XMM1, v4f32),
                reg(IA32_REG_XMM2, v4f32));
        addPs.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
        interpreter.setRegister(IA32_REG_XMM1, vec(0x00000001, 0));
        interpreter.setRegister(IA32_REG_XMM2, vec(0x00000001, 0));
        runOne(interpreter, addPs, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("daz sum", interpreter.getRegister(IA32_REG_XMM0).getWord(0), 0);
        check("daz flags", interpreter.getRegister(IA32_REG_MXCSR).getWord(0),
              0x1F80 | 0x8040);

        // An unmasked invalid operation: #XM (19), nothing is written
        // (IM is bit 7)
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80 & ~0x80));
        // Lane 1 is inf + -inf: invalid
        interpreter.setRegister(IA32_REG_XMM1, vec(0x7F80000000000000ULL, 0));
        interpreter.setRegister(IA32_REG_XMM2, vec(0xFF80000000000000ULL, 0));
        interpreter.setRegister(IA32_REG_XMM0, vec(0x1111, 0x2222));
        runOne(interpreter, addPs, SemanticInterpreter::EXIT_TRAP);
        check("trap kind", interpreter.getTrapKind(), Op::TRAP_FAULT);
        check("trap number", interpreter.getTrapNumber(), 19);
        check("trap address", interpreter.getNextAddress(), 0x400);
        check("trap destination", interpreter.getRegister(IA32_REG_XMM0).getWord(0),
              0x1111);
        check("trap flags", interpreter.getRegister(IA32_REG_MXCSR).getWord(0) & 0x3F,
              INV);
        // The same operation with the exception masked gives the default NaN
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80));
        runOne(interpreter, addPs, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("masked default NaN",
              interpreter.getRegister(IA32_REG_XMM0).getWord(0),
              0xFFC0000000000000ULL);
    }

    void testInterpreterX87()
    {
        FloatTestMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);
        const SemanticType f80 = SemanticType::floating(
                                        SemanticType::FLOAT_EXTENDED);
        // fcw: all masked, extended precision, round to nearest = 0x037F
        interpreter.setRegister(IA32_REG_FPU_CONTROL, SemanticValue(0x037F));
        interpreter.setRegister(IA32_REG_FPU_STATUS, SemanticValue(0));
        const uint64 intBit = (uint64)1 << 63;
        // ST operands in the xmm registers (80 bits)
        interpreter.setRegister(IA32_REG_XMM1, vec(intBit, 0x3FFF));
        interpreter.setRegister(IA32_REG_XMM2, vec(intBit | ((uint64)1 << 33),
                                                   0x3FFF - 0));
        SemanticInstruction add(Op::OP_FLOAT_ADD, f80,
                reg(IA32_REG_XMM0, f80), reg(IA32_REG_XMM1, f80),
                reg(IA32_REG_XMM3, f80));
        add.setAttributes(SemanticInstruction::ATTRIBUTE_X87 |
                          SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
        // xmm3 = 2^-30
        interpreter.setRegister(IA32_REG_XMM3, vec(intBit, 0x3FFF - 30));
        runOne(interpreter, add, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("x87 add", interpreter.getRegister(IA32_REG_XMM0).getWord(0),
              intBit | ((uint64)1 << 33));
        check("x87 add high", interpreter.getRegister(IA32_REG_XMM0).getWord(1),
              0x3FFF);
        // Precision control 24 bits (fcw bits 8-9 = 0)
        interpreter.setRegister(IA32_REG_FPU_CONTROL, SemanticValue(0x007F));
        runOne(interpreter, add, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("x87 pc24", interpreter.getRegister(IA32_REG_XMM0).getWord(0),
              intBit);
        check("x87 pc24 flags", interpreter.getRegister(IA32_REG_FPU_STATUS).getWord(0),
              INX);
        // Round toward zero via the control word, 53 bits (pc = 2)
        interpreter.setRegister(IA32_REG_FPU_CONTROL, SemanticValue(0x0E7F));
        interpreter.setRegister(IA32_REG_FPU_STATUS, SemanticValue(0));

        // An unmasked precision exception is a trap: the result is written,
        // ES (bit 7) and B (bit 15) are set
        interpreter.setRegister(IA32_REG_FPU_CONTROL, SemanticValue(0x005F));
        interpreter.setRegister(IA32_REG_XMM0, vec(0x77, 0));
        runOne(interpreter, add, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("x87 trap destination",
              interpreter.getRegister(IA32_REG_XMM0).getWord(0), intBit);
        check("x87 trap status",
              interpreter.getRegister(IA32_REG_FPU_STATUS).getWord(0),
              INX | 0x80 | 0x8000);
        // An unmasked invalid operation is a fault: the result isn't written
        interpreter.setRegister(IA32_REG_FPU_CONTROL, SemanticValue(0x037E));
        interpreter.setRegister(IA32_REG_FPU_STATUS, SemanticValue(0));
        interpreter.setRegister(IA32_REG_XMM0, vec(0x77, 0));
        interpreter.setRegister(IA32_REG_XMM1, vec(intBit | 1, 0x7FFF));
        runOne(interpreter, add, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("x87 fault destination",
              interpreter.getRegister(IA32_REG_XMM0).getWord(0), 0x77);
        check("x87 fault status",
              interpreter.getRegister(IA32_REG_FPU_STATUS).getWord(0),
              INV | 0x80 | 0x8000);
        // x87 NaN rule: the larger significand
        interpreter.setRegister(IA32_REG_FPU_CONTROL, SemanticValue(0x037F));
        interpreter.setRegister(IA32_REG_XMM1, vec(intBit | (3ULL << 62) | 1,
                                                   0x7FFF));
        interpreter.setRegister(IA32_REG_XMM3, vec(intBit | (3ULL << 62) | 2,
                                                   0x7FFF));
        runOne(interpreter, add, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("x87 nan", interpreter.getRegister(IA32_REG_XMM0).getWord(0),
              intBit | (3ULL << 62) | 2);
    }

    void testInterpreterCompareConvert()
    {
        FloatTestMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);
        const SemanticType f32 = SemanticType::floating(SemanticType::FLOAT_SINGLE);
        const SemanticType f64 = SemanticType::floating(SemanticType::FLOAT_DOUBLE);
        const SemanticType i32 = SemanticType::integer(32);
        const SemanticType i64 = SemanticType::integer(64);
        const SemanticType v4f32 = SemanticType::vector(f32, 4);
        const SemanticType v4i32 = SemanticType::vector(i32, 4);
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80));

        // cvtss2sd
        interpreter.setRegister(IA32_REG_XMM1, vec(0x3FC00000, 0));
        SemanticInstruction convert(Op::OP_FLOAT_CONVERT, f64,
                reg(IA32_REG_XMM0, f64), reg(IA32_REG_XMM1, f32));
        runOne(interpreter, convert, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("convert", interpreter.getRegister(IA32_REG_XMM0).getWord(0),
              0x3FF8000000000000ULL);

        // cvtsi2sd and cvttsd2si
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(0xFFFFFFFE));
        SemanticInstruction fromInt(Op::OP_SIGNED_TO_FLOAT, f64,
                reg(IA32_REG_XMM0, f64), reg(IA32_REG_EAX, i32));
        runOne(interpreter, fromInt, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("signed to float", interpreter.getRegister(IA32_REG_XMM0).getWord(0),
              0xC000000000000000ULL);
        interpreter.setRegister(IA32_REG_XMM1, vec(0xC006000000000000ULL, 0));
        SemanticInstruction toInt(Op::OP_FLOAT_TO_SIGNED, i32,
                reg(IA32_REG_EAX, i32), reg(IA32_REG_XMM1, f64));
        toInt.setRoundingMode(Op::ROUND_TOWARD_ZERO);
        runOne(interpreter, toInt, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("float to signed", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0xFFFFFFFE);
        // The default rounding is dynamic: nearest even (-2.5 -> -2)
        toInt.setRoundingMode(Op::ROUND_DYNAMIC);
        runOne(interpreter, toInt, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("float to signed nearest", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0xFFFFFFFD);
        // A NaN: the integer indefinite
        interpreter.setRegister(IA32_REG_XMM1, vec(0x7FF8000000000000ULL, 0));
        toInt.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
        runOne(interpreter, toInt, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("indefinite", interpreter.getRegister(IA32_REG_EAX).getWord(0),
              0x80000000);
        check("indefinite flag", interpreter.getRegister(IA32_REG_MXCSR).getWord(0),
              0x1F80 | INV);
        toInt.setBehavior(Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO);
        runOne(interpreter, toInt, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("nan zero", interpreter.getRegister(IA32_REG_EAX).getWord(0), 0);

        // A float compare on vectors: cmpltps-like, a mask bit per lane
        interpreter.setRegister(IA32_REG_XMM1, vec(0x4000000040000000ULL,
                                                   0x7FC000003F800000ULL));
        interpreter.setRegister(IA32_REG_XMM2, vec(0x3F80000040400000ULL,
                                                   0x3F8000003F800000ULL));
        const SemanticType mask4 = SemanticType::mask(4);
        SemanticInstruction less(Op::OP_COMPARE, v4f32,
                SemanticOperand::temporary(0, mask4),
                reg(IA32_REG_XMM1, v4f32), reg(IA32_REG_XMM2, v4f32));
        less.setCondition(Op::COND_FLOAT_LESS);
        less.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        block.newTemporary(mask4);
        block.append(less);
        block.append(SemanticInstruction(Op::OP_ZERO_EXTEND, i32,
                reg(IA32_REG_EDX, i32), SemanticOperand::temporary(0, mask4)));
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80));
        interpreter.run(block);
        // lanes: 2<3 yes, 2<1 no, 1<1 no, nan<1 no (unordered)
        check("compare mask", interpreter.getRegister(IA32_REG_EDX).getWord(0),
              0x1);
        check("compare nan flag", interpreter.getRegister(IA32_REG_MXCSR).getWord(0),
              0x1F80 | INV);
        // The quiet conditions don't signal for a quiet NaN
        interpreter.setRegister(IA32_REG_MXCSR, SemanticValue(0x1F80));
        SemanticBlock quiet(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        quiet.newTemporary(mask4);
        SemanticInstruction equal(less);
        equal.setCondition(Op::COND_FLOAT_UNORDERED);
        quiet.append(equal);
        quiet.append(SemanticInstruction(Op::OP_ZERO_EXTEND, i32,
                reg(IA32_REG_EDX, i32), SemanticOperand::temporary(0, mask4)));
        interpreter.run(quiet);
        check("unordered mask", interpreter.getRegister(IA32_REG_EDX).getWord(0),
              0x8);
        check("quiet flags", interpreter.getRegister(IA32_REG_MXCSR).getWord(0),
              0x1F80);

        // comisd: FLAGS_FLOAT_COMPARE writes ZF PF CF
        interpreter.setRegister(IA32_REG_XMM1, vec(0x4000000000000000ULL, 0));
        interpreter.setRegister(IA32_REG_XMM2, vec(0x3FF0000000000000ULL, 0));
        SemanticInstruction comis(Op::OP_SET_FLAGS, f64, SemanticOperand(),
                reg(IA32_REG_XMM1, f64), reg(IA32_REG_XMM2, f64));
        comis.setFlags(Op::FLAGS_FLOAT_COMPARE,
                       SemanticFlags::FLAG_ZERO | SemanticFlags::FLAG_PARITY |
                       SemanticFlags::FLAG_CARRY);
        runOne(interpreter, comis, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("comis greater", interpreter.getFlags(), 0);
        interpreter.setRegister(IA32_REG_XMM2, vec(0x7FF8000000000000ULL, 0));
        runOne(interpreter, comis, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("comis unordered", interpreter.getFlags(),
              SemanticFlags::FLAG_ZERO | SemanticFlags::FLAG_PARITY |
              SemanticFlags::FLAG_CARRY);
        interpreter.setRegister(IA32_REG_XMM2, vec(0x4000000000000000ULL, 0));
        runOne(interpreter, comis, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("comis equal", interpreter.getFlags(), SemanticFlags::FLAG_ZERO);
        interpreter.setRegister(IA32_REG_XMM2, vec(0x4010000000000000ULL, 0));
        runOne(interpreter, comis, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("comis less", interpreter.getFlags(), SemanticFlags::FLAG_CARRY);

        // min / max / sqrt / fma / classify on a vector, through evaluate()
        SemanticValue sources[3];
        sources[0] = vec(0x40800000ULL | (0x41100000ULL << 32), 0);
        SemanticValue result;
        SemanticInstruction root(Op::OP_FLOAT_SQUARE_ROOT, v4f32,
                SemanticOperand());
        TESTS_ASSERT(SemanticInterpreter::evaluate(root, sources, result));
        check("sqrt lanes", result.getWord(0), 0x4040000040000000ULL);
        SemanticInstruction classify(Op::OP_FLOAT_CLASSIFY, v4i32,
                SemanticOperand(), reg(IA32_REG_XMM1, v4f32));
        sources[0] = vec(0x0000000180000000ULL, 0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(classify, sources, result));
        check("classify lanes", result.getWord(0), (uint64)0x20 << 32 | 0x8);
        (void)i64;
    }

    //////////////////////////////////////////////////////////////////////
    // Vector operations
    //////////////////////////////////////////////////////////////////////

    static SemanticValue bytes(const uint8* data, uint count)
    {
        SemanticValue v;
        for (uint i = 0; i < count; i++)
            v.setBits(i * 8, 8, data[i]);
        return v;
    }

    uint8 laneOf(const SemanticValue& v, uint lane, uint bits = 8)
    {
        return (uint8)v.getBits(lane * bits, bits);
    }

    void testVectorOperations()
    {
        const SemanticType i8 = SemanticType::integer(8);
        const SemanticType i16 = SemanticType::integer(16);
        const SemanticType i32 = SemanticType::integer(32);
        const SemanticType i64 = SemanticType::integer(64);
        const SemanticType v16i8 = SemanticType::vector(i8, 16);
        const SemanticType v8i16 = SemanticType::vector(i16, 8);
        const SemanticType v4i32 = SemanticType::vector(i32, 4);
        const SemanticType v2i64 = SemanticType::vector(i64, 2);
        SemanticValue sources[3];
        SemanticValue result;
        uint8 a[16], b[16], idx[16];
        for (uint i = 0; i < 16; i++)
        {
            a[i] = (uint8)(i + 1);
            b[i] = (uint8)(0x80 + i);
            idx[i] = (uint8)i;
        }
        sources[0] = bytes(a, 16);
        sources[1] = bytes(b, 16);

        // punpcklbw / punpckhbw: OP_INTERLEAVE
        SemanticInstruction interleave(Op::OP_INTERLEAVE, v16i8,
                SemanticOperand());
        sources[2] = SemanticValue(0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(interleave, sources, result));
        check("interleave low 0", laneOf(result, 0), 1);
        check("interleave low 1", laneOf(result, 1), 0x80);
        check("interleave low 14", laneOf(result, 14), 8);
        check("interleave low 15", laneOf(result, 15), 0x87);
        sources[2] = SemanticValue(8);
        TESTS_ASSERT(SemanticInterpreter::evaluate(interleave, sources, result));
        check("interleave high 0", laneOf(result, 0), 9);
        check("interleave high 15", laneOf(result, 15), 0x8F);

        // pshufb: OP_SHUFFLE with BEHAVIOR_SHUFFLE_ZERO, indexes 0..15 of
        // s0, an index with bit 7 gives zero
        SemanticInstruction shuffle(Op::OP_SHUFFLE, v16i8, SemanticOperand());
        shuffle.setBehavior(Op::BEHAVIOR_SHUFFLE_ZERO);
        uint8 pattern[16];
        for (uint i = 0; i < 16; i++)
            pattern[i] = (uint8)(15 - i);
        pattern[3] = 0x80;
        sources[0] = bytes(a, 16);
        sources[1] = bytes(a, 16);
        sources[2] = bytes(pattern, 16);
        TESTS_ASSERT(SemanticInterpreter::evaluate(shuffle, sources, result));
        check("pshufb 0", laneOf(result, 0), 16);
        check("pshufb zero", laneOf(result, 3), 0);
        check("pshufb 15", laneOf(result, 15), 1);
        // The default: modulo 2 * lanes, the second source above the lanes
        shuffle.setBehavior(Op::BEHAVIOR_SHUFFLE_MODULO);
        sources[1] = bytes(b, 16);
        pattern[0] = 16;    // b[0]
        pattern[1] = 35;    // 35 % 32 = 3 -> a[3]
        sources[2] = bytes(pattern, 16);
        TESTS_ASSERT(SemanticInterpreter::evaluate(shuffle, sources, result));
        check("shuffle second", laneOf(result, 0), 0x80);
        check("shuffle modulo", laneOf(result, 1), 4);
        // pshufd: 4 x 32 bit lanes, indexes from an immediate-built vector
        SemanticInstruction shuffleD(Op::OP_SHUFFLE, v4i32, SemanticOperand());
        sources[0] = vec(0x0000000200000001ULL, 0x0000000400000003ULL);
        sources[1] = sources[0];
        sources[2] = vec(0x0000000200000003ULL, 0x0000000000000001ULL);
        TESTS_ASSERT(SemanticInterpreter::evaluate(shuffleD, sources, result));
        check("pshufd low", result.getWord(0), 0x0000000300000004ULL);
        check("pshufd high", result.getWord(1), 0x0000000100000002ULL);

        // palignr: OP_SLIDE d = (s1:s0) >> s2 lanes
        SemanticInstruction slide(Op::OP_SLIDE, v16i8, SemanticOperand());
        sources[0] = bytes(a, 16);
        sources[1] = bytes(b, 16);
        sources[2] = SemanticValue(4);
        TESTS_ASSERT(SemanticInterpreter::evaluate(slide, sources, result));
        check("slide 0", laneOf(result, 0), 5);
        check("slide 11", laneOf(result, 11), 16);
        check("slide 12", laneOf(result, 12), 0x80);
        check("slide 15", laneOf(result, 15), 0x83);
        sources[2] = SemanticValue(33);
        TESTS_ASSERT(SemanticInterpreter::evaluate(slide, sources, result));
        check("slide out of range", result.getWord(0), 0);

        // packsswb / packuswb / packssdw: OP_NARROW with a typed operand
        const SemanticType v8i16type = v8i16;
        SemanticInstruction narrow(Op::OP_NARROW, v16i8, SemanticOperand(),
                SemanticOperand::immediate(0, v8i16type));
        narrow.setAttributes(SemanticInstruction::ATTRIBUTE_SATURATE_SIGNED);
        SemanticValue w0, w1;
        const int16 words0[8] = { 0, 1, -1, 127, 128, -128, -129, 32767 };
        const int16 words1[8] = { -32768, 100, 300, -300, 255, 256, 0, 5 };
        for (uint i = 0; i < 8; i++)
        {
            w0.setBits(i * 16, 16, (uint16)words0[i]);
            w1.setBits(i * 16, 16, (uint16)words1[i]);
        }
        sources[0] = w0;
        sources[1] = w1;
        TESTS_ASSERT(SemanticInterpreter::evaluate(narrow, sources, result));
        check("packsswb 4", laneOf(result, 4), 127);
        check("packsswb 5", laneOf(result, 5), 0x80);
        check("packsswb 6", laneOf(result, 6), 0x80);
        check("packsswb 7", laneOf(result, 7), 127);
        check("packsswb 8", laneOf(result, 8), 0x80);
        check("packsswb 10", laneOf(result, 10), 127);
        check("packsswb 11", laneOf(result, 11), 0x80);
        narrow.setAttributes(SemanticInstruction::ATTRIBUTE_SATURATE_UNSIGNED);
        TESTS_ASSERT(SemanticInterpreter::evaluate(narrow, sources, result));
        check("packuswb 2", laneOf(result, 2), 0);
        check("packuswb 4", laneOf(result, 4), 128);
        check("packuswb 7", laneOf(result, 7), 255);
        check("packuswb 10", laneOf(result, 10), 255);
        check("packuswb 11", laneOf(result, 11), 0);
        narrow.setAttributes(0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(narrow, sources, result));
        check("narrow truncate", laneOf(result, 4), 128);
        check("narrow truncate 7", laneOf(result, 7), 0xFF);

        // phaddw / phaddd: OP_PAIRWISE_ADD
        SemanticInstruction pairwise(Op::OP_PAIRWISE_ADD, v4i32,
                SemanticOperand());
        sources[0] = vec(0x0000000200000001ULL, 0x0000000400000003ULL);
        sources[1] = vec(0x0000001000000010ULL, 0x0000002000000020ULL);
        TESTS_ASSERT(SemanticInterpreter::evaluate(pairwise, sources, result));
        check("phaddd low", result.getWord(0), 0x0000000700000003ULL);
        check("phaddd high", result.getWord(1), 0x0000004000000020ULL);

        // pmaddwd: OP_DOT_PRODUCT of v8i16 into v4i32
        SemanticInstruction dot(Op::OP_DOT_PRODUCT, v4i32, SemanticOperand(),
                SemanticOperand::immediate(0, v8i16));
        for (uint i = 0; i < 8; i++)
        {
            w0.setBits(i * 16, 16, (uint16)(i + 1));
            w1.setBits(i * 16, 16, 0xFFFF);
        }
        sources[0] = w0;
        sources[1] = w1;
        sources[2] = SemanticValue();
        TESTS_ASSERT(SemanticInterpreter::evaluate(dot, sources, result));
        check("pmaddwd", result.getWord(0), 0xFFFFFFF9FFFFFFFDULL);
        // 0x8000 * 0x8000 * 2 wraps to 0x80000000
        sources[0] = vec(0x8000800080008000ULL, 0);
        sources[1] = vec(0x8000800080008000ULL, 0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(dot, sources, result));
        check("pmaddwd wrap", result.getWord(0), 0x8000000080000000ULL);
        // psadbw: the absolute differences summed by an unsigned dot product
        // with ones
        SemanticInstruction absoluteDifference(
                Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, v16i8, SemanticOperand());
        sources[0] = bytes(a, 16);
        sources[1] = bytes(b, 16);
        TESTS_ASSERT(SemanticInterpreter::evaluate(absoluteDifference, sources,
                                                   result));
        check("absdiff", laneOf(result, 0), 0x7F);
        SemanticInstruction sad(Op::OP_DOT_PRODUCT, v2i64, SemanticOperand(),
                SemanticOperand::immediate(0, v16i8));
        sad.setBehavior(Op::BEHAVIOR_UNSIGNED);
        sources[0] = result;
        uint8 ones[16];
        for (uint i = 0; i < 16; i++)
            ones[i] = 1;
        sources[1] = bytes(ones, 16);
        sources[2] = SemanticValue();
        TESTS_ASSERT(SemanticInterpreter::evaluate(sad, sources, result));
        check("psadbw low", result.getWord(0), 8 * 0x7F);
        check("psadbw high", result.getWord(1), 8 * 0x7F);

        // pmulhw: OP_MULTIPLY_HIGH_SIGNED lanes
        SemanticInstruction mulHigh(Op::OP_MULTIPLY_HIGH_SIGNED, v8i16,
                SemanticOperand());
        sources[0] = vec(0x8000800040000002ULL, 0);
        sources[1] = vec(0x7FFF800040000002ULL, 0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(mulHigh, sources, result));
        check("pmulhw", result.getWord(0), 0xC000400010000000ULL);
        (void)idx;

        // compress / expand, lane index, active lane mask, mask count / first
        SemanticInstruction compress(Op::OP_COMPRESS, v4i32, SemanticOperand());
        sources[0] = vec(0x0000000200000001ULL, 0x0000000400000003ULL);
        sources[1] = SemanticValue(0xA);
        TESTS_ASSERT(SemanticInterpreter::evaluate(compress, sources, result));
        check("compress", result.getWord(0), 0x0000000400000002ULL);
        SemanticInstruction expand(Op::OP_EXPAND, v4i32, SemanticOperand());
        TESTS_ASSERT(SemanticInterpreter::evaluate(expand, sources, result));
        check("expand low", result.getWord(0), 0x0000000100000000ULL);
        check("expand high", result.getWord(1), 0x0000000200000000ULL);
        SemanticInstruction laneIndex(Op::OP_LANE_INDEX, v4i32,
                SemanticOperand());
        sources[0] = SemanticValue(10);
        sources[1] = SemanticValue(3);
        TESTS_ASSERT(SemanticInterpreter::evaluate(laneIndex, sources, result));
        check("lane index low", result.getWord(0), 0x0000000D0000000AULL);
        check("lane index high", result.getWord(1), 0x0000001300000010ULL);
        const SemanticType mask8 = SemanticType::mask(8);
        SemanticInstruction whileLess(Op::OP_ACTIVE_LANE_MASK, mask8,
                SemanticOperand(), SemanticOperand::immediate(0, i64));
        sources[0] = SemanticValue(5);
        sources[1] = SemanticValue(9);
        TESTS_ASSERT(SemanticInterpreter::evaluate(whileLess, sources, result));
        check("active lanes", result.getWord(0), 0x0F);
        SemanticInstruction count(Op::OP_MASK_COUNT, i32,
                SemanticOperand(), SemanticOperand::immediate(0, mask8));
        sources[0] = SemanticValue(0x2C);
        TESTS_ASSERT(SemanticInterpreter::evaluate(count, sources, result));
        check("mask count", result.getWord(0), 3);
        SemanticInstruction first(Op::OP_MASK_FIRST, i32,
                SemanticOperand(), SemanticOperand::immediate(0, mask8));
        TESTS_ASSERT(SemanticInterpreter::evaluate(first, sources, result));
        check("mask first", result.getWord(0), 2);
        sources[0] = SemanticValue(0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(first, sources, result));
        check("mask first none", result.getWord(0) & 0xFFFFFFFF, 0xFFFFFFFF);

        // pmovzxbw / pmovsxbw: lane-wise extension
        const SemanticType v8i8 = SemanticType::vector(i8, 8);
        SemanticInstruction zeroExtend(Op::OP_ZERO_EXTEND, v8i16,
                SemanticOperand(), SemanticOperand::immediate(0, v8i8));
        sources[0] = vec(0x80FF7F0180FF7F01ULL, 0);
        TESTS_ASSERT(SemanticInterpreter::evaluate(zeroExtend, sources, result));
        check("pmovzx low", result.getWord(0), 0x008000FF007F0001ULL);
        SemanticInstruction signExtend(Op::OP_SIGN_EXTEND, v8i16,
                SemanticOperand(), SemanticOperand::immediate(0, v8i8));
        TESTS_ASSERT(SemanticInterpreter::evaluate(signExtend, sources, result));
        check("pmovsx lane 0", result.getBits(0, 16), 0x0001);
        check("pmovsx lane 1", result.getBits(16, 16), 0x007F);
        check("pmovsx lane 2", result.getBits(32, 16), 0xFFFF);
        check("pmovsx lane 3", result.getBits(48, 16), 0xFF80);
        (void)v2i64;
    }

    void testMaskedMemory()
    {
        FloatTestMemory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);
        const SemanticType i8 = SemanticType::integer(8);
        const SemanticType v8i8 = SemanticType::vector(i8, 8);
        const SemanticType v16i8 = SemanticType::vector(i8, 16);
        const SemanticType mask16 = SemanticType::mask(16);
        const SemanticType i32 = SemanticType::integer(32);
        for (uint i = 0; i < 16; i++)
            memory.setByte(0x1000 + i, 0xA0 + i);

        // maskmovdqu: store the bytes of the mask
        interpreter.setRegister(IA32_REG_XMM0, vec(0x0807060504030201ULL,
                                                   0x100F0E0D0C0B0A09ULL));
        interpreter.setRegister(IA32_REG_EDI, SemanticValue(0x1000));
        SemanticInstruction store(Op::OP_STORE_MASKED, v16i8, SemanticOperand(),
                reg(IA32_REG_EDI, i32), reg(IA32_REG_XMM0, v16i8),
                SemanticOperand::immediate(0x8005, mask16));
        runOne(interpreter, store, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("masked store 0", memory.getByte(0x1000), 0x01);
        check("masked store 1", memory.getByte(0x1001), 0xA1);
        check("masked store 2", memory.getByte(0x1002), 0x03);
        check("masked store 3", memory.getByte(0x1003), 0xA3);
        check("masked store 15", memory.getByte(0x100F), 0x10);

        // The masked-out lanes don't fault, an active one does and nothing
        // is written
        memory.m_refused = 0x1008;
        SemanticInstruction store2(Op::OP_STORE_MASKED, v16i8, SemanticOperand(),
                reg(IA32_REG_EDI, i32), reg(IA32_REG_XMM0, v16i8),
                SemanticOperand::immediate(0x0107, mask16));
        runOne(interpreter, store2, SemanticInterpreter::EXIT_MEMORY_FAULT);
        check("fault address", interpreter.getFaultAddress(), 0x1008);
        check("restored", memory.getByte(0x1001), 0xA1);
        check("restored 2", memory.getByte(0x1002), 0x03);
        SemanticInstruction store3(Op::OP_STORE_MASKED, v16i8, SemanticOperand(),
                reg(IA32_REG_EDI, i32), reg(IA32_REG_XMM0, v16i8),
                SemanticOperand::immediate(0x00F7, mask16));
        runOne(interpreter, store3, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("hole not accessed", memory.getByte(0x1004), 0x05);

        // A masked load: 0 for the inactive lanes
        memory.m_refused = 0xFFFFFFFFFFFFFFFFULL;
        SemanticInstruction load(Op::OP_LOAD_MASKED, v8i8,
                reg(IA32_REG_XMM1, v8i8), reg(IA32_REG_EDI, i32),
                SemanticOperand::immediate(0x05, SemanticType::mask(8)));
        runOne(interpreter, load, SemanticInterpreter::EXIT_FALL_THROUGH);
        check("masked load", interpreter.getRegister(IA32_REG_XMM1).getWord(0),
              0x0000000000030001ULL);
    }

    virtual void test()
    {
        testSingleBasic();
        testSpecials();
        testOverflowAndRounding();
        testDenormals();
        testNanRules();
        testExtended();
        testMinMax();
        testCompareClassify();
        testConversions();
        testIntegerConversions();
        testRoundScale();
        testGeneratedVectors();
        testInterpreterFloat();
        testInterpreterX87();
        testInterpreterCompareConvert();
        testVectorOperations();
        testMaskedMemory();
        cout << "Semantic float tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestSemanticFloat g_globalTestSemanticFloat;
