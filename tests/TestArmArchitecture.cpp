/*
 * TestArmArchitecture.cpp
 *
 * Tests the AArch32 and AArch64 descriptions of the semantic layer: the
 * register tables, the aliasing of the sub-registers (through the
 * interpreter), the flags, the vector lengths and the floating point
 * environments.
 *
 * The expected values were computed by hand.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/string.h"
#include "xStl/data/datastream.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/stream/ioStream.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/Semantics.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/Arm64Architecture.h"

using namespace armdis;

// TESTS_ASSERT which tells the line of the condition which failed
#define ARM_ASSERT(condition) \
    do { \
        if (!(condition)) \
            cout << "FAILED line " << __LINE__ << ": " << #condition << endl; \
        TESTS_ASSERT(condition); \
    } while (0)

// The registers tests don't touch the memory
class TestArmMemory : public SemanticMemory {
public:
    virtual bool read(uint64, uint8*, uint) { return false; }
    virtual bool write(uint64, const uint8*, uint) { return false; }
};

class TestObjectTestArmArchitecture : public cTestObject {
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

    // Return the id of a register by name
    static uint find(const ArchitectureDescription& arch, const char* name)
    {
        uint id = 0xFFFFFFFF;
        if (!arch.findRegister(cString(name), id))
        {
            cout << "FAILED register " << name << " not found" << endl;
            ARM_ASSERT(false);
        }
        return id;
    }

    // Every name is found at its own id, and the sub-registers point to full
    // registers which contain them
    void checkTable(const ArchitectureDescription& arch)
    {
        for (uint i = 0; i < arch.getRegistersCount(); i++)
        {
            const ArchitectureRegister& r = arch.getRegister(i);
            uint found = 0xFFFFFFFF;
            ARM_ASSERT(arch.findRegister(cString(r.m_name), found));
            check(r.m_name, found, i);
            ARM_ASSERT(r.m_bits >= 1);
            if (r.m_parent != ArchitectureRegister::NO_PARENT)
            {
                const ArchitectureRegister& parent = arch.getRegister(r.m_parent);
                check("parent is full", parent.m_parent,
                      ArchitectureRegister::NO_PARENT);
                ARM_ASSERT(r.m_bitOffset + r.m_bits <= parent.m_bits);
            }
        }
        uint unused;
        ARM_ASSERT(!arch.findRegister(cString("notaregister"), unused));
    }

    static uint64 get(SemanticInterpreter& interpreter, uint registerId,
                      uint word = 0)
    {
        return interpreter.getRegister(registerId).getWord(word);
    }

    // Check the fields of the floating point environment which both
    // processors have in common
    void checkFloatEnvironment(const ArchitectureDescription& arch,
                               uint control, uint status)
    {
        FloatEnvironment env;
        ARM_ASSERT(arch.getFloatEnvironment(0, env));
        check("control", env.m_controlRegister, control);
        check("status", env.m_statusRegister, status);
        check("rounding offset", env.m_roundingOffset, 22);
        check("rn", env.m_roundingModes[0], SemanticOperation::ROUND_NEAREST_EVEN);
        check("rp", env.m_roundingModes[1], SemanticOperation::ROUND_TOWARD_POSITIVE);
        check("rm", env.m_roundingModes[2], SemanticOperation::ROUND_TOWARD_NEGATIVE);
        check("rz", env.m_roundingModes[3], SemanticOperation::ROUND_TOWARD_ZERO);
        check("fz", env.m_flushToZeroOffset, 24);
        // The flags: invalid, denormal, divide by zero, overflow, underflow,
        // inexact are IOC, IDC, DZC, OFC, UFC, IXC
        check("ioc", env.m_flagOffsets[0], 0);
        check("idc", env.m_flagOffsets[1], 7);
        check("dzc", env.m_flagOffsets[2], 1);
        check("ofc", env.m_flagOffsets[3], 2);
        check("ufc", env.m_flagOffsets[4], 3);
        check("ixc", env.m_flagOffsets[5], 4);
        check("ioe", env.m_maskOffsets[0], 8);
        check("ide", env.m_maskOffsets[1], 15);
        check("ixe", env.m_maskOffsets[5], 12);
        ARM_ASSERT(!env.m_isMaskSetDisabled);
        check("trap", env.m_trapMode, FloatEnvironment::TRAP_NONE);
        ARM_ASSERT(!env.m_isDefaultNanNegative);
    }

    void testArm32()
    {
        ArchitectureDescriptionPtr archPtr =
            ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_ARM_32_LE);
        const ArchitectureDescription& arch = *archPtr;
        ArchitectureDescriptionPtr thumb =
            ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_THUMB_LE);
        check("thumb count", thumb->getRegistersCount(),
              arch.getRegistersCount());

        check("registers count", arch.getRegistersCount(), ARM_REGISTERS_COUNT);
        check("address bits", arch.getAddressBits(), 32);
        check("native bits", arch.getNativeIntegerBits(), 32);
        check("vector bits", arch.getMaximumVectorBits(), 128);
        ARM_ASSERT(!arch.isVectorLengthScalable());
        ARM_ASSERT(arch.isLittleEndian());
        check("memory model", arch.getMemoryModel(),
              ArchitectureDescription::MEMORY_WEAK);
        ARM_ASSERT(arch.isCarryInvertedOnSubtract());
        check("sp", arch.getStackPointer(), ARM_REG_SP);
        check("pc", arch.getProgramCounter(), ARM_REG_PC);
        uint mode = 0;
        ARM_ASSERT(arch.getModeRegister(mode));
        check("mode", mode, ARM_REG_T);
        checkTable(arch);

        // The names, in the LLVM spelling, and the other spellings
        check("r0", find(arch, "r0"), ARM_REG_R0);
        check("r12", find(arch, "r12"), ARM_REG_R12);
        check("sp", find(arch, "sp"), ARM_REG_SP);
        check("r13", find(arch, "r13"), ARM_REG_SP);
        check("lr", find(arch, "lr"), ARM_REG_LR);
        check("r14", find(arch, "r14"), ARM_REG_LR);
        check("pc", find(arch, "pc"), ARM_REG_PC);
        check("r15", find(arch, "r15"), ARM_REG_PC);
        check("ip", find(arch, "ip"), ARM_REG_R12);
        check("fp", find(arch, "fp"), ARM_REG_R11);
        check("q15", find(arch, "q15"), ARM_REG_Q15);
        check("d31", find(arch, "d31"), ARM_REG_D31);
        check("s31", find(arch, "s31"), ARM_REG_S31);
        check("fpscr", find(arch, "fpscr"), ARM_REG_FPSCR);
        check("ge3", find(arch, "ge3"), ARM_REG_GE3);
        check("t", find(arch, "t"), ARM_REG_T);

        // The sizes and the aliasing of the vector registers
        check("q bits", arch.getRegister(ARM_REG_Q0 + 5).m_bits, 128);
        const ArchitectureRegister& d9 = arch.getRegister(ARM_REG_D0 + 9);
        check("d9 parent", d9.m_parent, ARM_REG_Q0 + 4);
        check("d9 offset", d9.m_bitOffset, 64);
        const ArchitectureRegister& d30 = arch.getRegister(ARM_REG_D0 + 30);
        check("d30 parent", d30.m_parent, ARM_REG_Q0 + 15);
        check("d30 offset", d30.m_bitOffset, 0);
        const ArchitectureRegister& s7 = arch.getRegister(ARM_REG_S0 + 7);
        check("s7 parent", s7.m_parent, ARM_REG_Q0 + 1);
        check("s7 offset", s7.m_bitOffset, 96);
        for (uint i = 0; i < 32; i++)
        {
            ARM_ASSERT(!arch.getRegister(ARM_REG_D0 + i).m_zeroExtendsParent);
            ARM_ASSERT(!arch.getRegister(ARM_REG_S0 + i).m_zeroExtendsParent);
        }

        // The flags
        uint flag = 0;
        ARM_ASSERT(arch.getFlagRegister(SemanticFlags::FLAG_CARRY, flag));
        check("carry", flag, ARM_REG_C);
        ARM_ASSERT(arch.getFlagRegister(SemanticFlags::FLAG_ZERO, flag));
        check("zero", flag, ARM_REG_Z);
        ARM_ASSERT(arch.getFlagRegister(SemanticFlags::FLAG_SIGN, flag));
        check("sign", flag, ARM_REG_N);
        ARM_ASSERT(arch.getFlagRegister(SemanticFlags::FLAG_OVERFLOW, flag));
        check("overflow", flag, ARM_REG_V);
        ARM_ASSERT(!arch.getFlagRegister(SemanticFlags::FLAG_PARITY, flag));
        check("n file", arch.getRegister(ARM_REG_N).m_file,
              ArchitectureRegister::FILE_FLAG);
        check("n flag", arch.getRegister(ARM_REG_N).m_flag, SemanticFlags::FLAG_SIGN);
        check("q file", arch.getRegister(ARM_REG_Q).m_file,
              ArchitectureRegister::FILE_FLAG);

        checkFloatEnvironment(arch, ARM_REG_FPSCR, ARM_REG_FPSCR);

        // Supported operations: no 64 bit arithmetic, no vector divide
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_ADD,
                                      SemanticType::integer(32)));
        ARM_ASSERT(!arch.isSupported(SemanticOperation::OP_ADD,
                                       SemanticType::integer(64)));
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_LOAD,
                                      SemanticType::integer(64)));
        ARM_ASSERT(!arch.isSupported(SemanticOperation::OP_MULTIPLY_HIGH_UNSIGNED,
                                       SemanticType::integer(64)));
        SemanticType v4i32 = SemanticType::vector(SemanticType::integer(32), 4);
        SemanticType v2i64 = SemanticType::vector(SemanticType::integer(64), 2);
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_ADD, v4i32));
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_ADD, v2i64));
        ARM_ASSERT(!arch.isSupported(SemanticOperation::OP_DIVIDE_SIGNED, v4i32));
        ARM_ASSERT(!arch.isSupported(SemanticOperation::OP_MULTIPLY, v2i64));
        SemanticType v4f32 = SemanticType::vector(
                SemanticType::floating(SemanticType::FLOAT_SINGLE), 4);
        SemanticType v2f64 = SemanticType::vector(
                SemanticType::floating(SemanticType::FLOAT_DOUBLE), 2);
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_FLOAT_ADD, v4f32));
        ARM_ASSERT(!arch.isSupported(SemanticOperation::OP_FLOAT_DIVIDE, v4f32));
        ARM_ASSERT(!arch.isSupported(SemanticOperation::OP_FLOAT_ADD, v2f64));
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_FLOAT_DIVIDE,
                SemanticType::floating(SemanticType::FLOAT_DOUBLE)));
    }

    void testArm32Aliasing()
    {
        ArchitectureDescriptionPtr arch =
            ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_ARM_32_LE);
        TestArmMemory memory;
        SemanticInterpreter interpreter(arch, memory);

        // Write the four words of q1 through q1, then rewrite s5 (= d2[32..63]
        // = q1[32..63])
        SemanticValue q1;
        q1.setWord(0, 0x1111111122222222ULL);
        q1.setWord(1, 0x3333333344444444ULL);
        interpreter.setRegister(ARM_REG_Q0 + 1, q1);
        check("d2", get(interpreter, ARM_REG_D0 + 2), 0x1111111122222222ULL);
        check("d3", get(interpreter, ARM_REG_D0 + 3), 0x3333333344444444ULL);
        check("s4", get(interpreter, ARM_REG_S0 + 4), 0x22222222);
        check("s5", get(interpreter, ARM_REG_S0 + 5), 0x11111111);
        check("s6", get(interpreter, ARM_REG_S0 + 6), 0x44444444);
        check("s7", get(interpreter, ARM_REG_S0 + 7), 0x33333333);

        // s5 changes only its 32 bits: no zero extension
        interpreter.setRegister(ARM_REG_S0 + 5, SemanticValue(0xAABBCCDD));
        check("q1 low", get(interpreter, ARM_REG_Q0 + 1, 0), 0xAABBCCDD22222222ULL);
        check("q1 high", get(interpreter, ARM_REG_Q0 + 1, 1), 0x3333333344444444ULL);
        check("d2 after s5", get(interpreter, ARM_REG_D0 + 2), 0xAABBCCDD22222222ULL);

        // d3 changes only the upper half of q1
        interpreter.setRegister(ARM_REG_D0 + 3, SemanticValue(0x0123456789ABCDEFULL));
        check("q1 low kept", get(interpreter, ARM_REG_Q0 + 1, 0), 0xAABBCCDD22222222ULL);
        check("q1 high new", get(interpreter, ARM_REG_Q0 + 1, 1), 0x0123456789ABCDEFULL);
        check("s7 after d3", get(interpreter, ARM_REG_S0 + 7), 0x01234567);

        // d0 and d1 are the halves of q0, d16 .. d31 are q8 .. q15
        interpreter.setRegister(ARM_REG_D0 + 16, SemanticValue(0x55));
        interpreter.setRegister(ARM_REG_D0 + 17, SemanticValue(0x66));
        check("q8 low", get(interpreter, ARM_REG_Q0 + 8, 0), 0x55);
        check("q8 high", get(interpreter, ARM_REG_Q0 + 8, 1), 0x66);

        // The general registers don't alias
        interpreter.setRegister(ARM_REG_R0, SemanticValue(0xFFFFFFFFULL));
        check("r0", get(interpreter, ARM_REG_R0), 0xFFFFFFFFULL);
        check("r1", get(interpreter, ARM_REG_R1), 0);
    }

    void testArm64()
    {
        ArchitectureDescriptionPtr archPtr =
            ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        const ArchitectureDescription& arch = *archPtr;
        check("registers count", arch.getRegistersCount(), ARM64_REGISTERS_COUNT);
        check("address bits", arch.getAddressBits(), 64);
        check("native bits", arch.getNativeIntegerBits(), 64);
        ARM_ASSERT(arch.isVectorLengthScalable());
        ARM_ASSERT(arch.isLittleEndian());
        ARM_ASSERT(arch.isCarryInvertedOnSubtract());
        check("sp", arch.getStackPointer(), ARM64_REG_SP);
        check("pc", arch.getProgramCounter(), ARM64_REG_PC);
        uint mode = 0;
        ARM_ASSERT(!arch.getModeRegister(mode));
        checkTable(arch);

        // The names, in the LLVM spelling, and the other spellings
        check("x0", find(arch, "x0"), ARM64_REG_X0);
        check("x30", find(arch, "x30"), ARM64_REG_X30);
        check("lr", find(arch, "lr"), ARM64_REG_X30);
        check("fp", find(arch, "fp"), ARM64_REG_X0 + 29);
        check("sp", find(arch, "sp"), ARM64_REG_SP);
        check("xzr", find(arch, "xzr"), ARM64_REG_XZR);
        check("w17", find(arch, "w17"), ARM64_REG_W0 + 17);
        check("wsp", find(arch, "wsp"), ARM64_REG_WSP);
        check("wzr", find(arch, "wzr"), ARM64_REG_WZR);
        check("z31", find(arch, "z31"), ARM64_REG_Z31);
        check("v7", find(arch, "v7"), ARM64_REG_V0 + 7);
        check("q8", find(arch, "q8"), ARM64_REG_Q0 + 8);
        check("d9", find(arch, "d9"), ARM64_REG_D0 + 9);
        check("s10", find(arch, "s10"), ARM64_REG_S0 + 10);
        check("h11", find(arch, "h11"), ARM64_REG_H0 + 11);
        check("b12", find(arch, "b12"), ARM64_REG_B0 + 12);
        check("p15", find(arch, "p15"), ARM64_REG_P15);
        check("pn3", find(arch, "pn3"), ARM64_REG_P0 + 3);
        check("ffr", find(arch, "ffr"), ARM64_REG_FFR);
        check("zt0", find(arch, "zt0"), ARM64_REG_ZT0);
        check("fpcr", find(arch, "fpcr"), ARM64_REG_FPCR);
        check("fpsr", find(arch, "fpsr"), ARM64_REG_FPSR);
        check("svcr", find(arch, "svcr"), ARM64_REG_SVCR);

        // The id helpers
        check("gp x", getArm64GeneralRegister(5, true, false), ARM64_REG_X0 + 5);
        check("gp w", getArm64GeneralRegister(5, false, true), ARM64_REG_W0 + 5);
        check("gp sp", getArm64GeneralRegister(31, true, true), ARM64_REG_SP);
        check("gp xzr", getArm64GeneralRegister(31, true, false), ARM64_REG_XZR);
        check("gp wsp", getArm64GeneralRegister(31, false, true), ARM64_REG_WSP);
        check("gp wzr", getArm64GeneralRegister(31, false, false), ARM64_REG_WZR);
        check("simd v", getArm64SimdRegister(3, 0), ARM64_REG_V0 + 3);
        check("simd b", getArm64SimdRegister(3, 8), ARM64_REG_B0 + 3);
        check("simd h", getArm64SimdRegister(3, 16), ARM64_REG_H0 + 3);
        check("simd s", getArm64SimdRegister(3, 32), ARM64_REG_S0 + 3);
        check("simd d", getArm64SimdRegister(3, 64), ARM64_REG_D0 + 3);
        check("simd q", getArm64SimdRegister(3, 128), ARM64_REG_Q0 + 3);

        // The aliasing flags
        const ArchitectureRegister& w3 = arch.getRegister(ARM64_REG_W0 + 3);
        check("w3 parent", w3.m_parent, ARM64_REG_X0 + 3);
        check("w3 bits", w3.m_bits, 32);
        ARM_ASSERT(w3.m_zeroExtendsParent);
        ARM_ASSERT(arch.getRegister(ARM64_REG_XZR).m_isZero);
        ARM_ASSERT(arch.getRegister(ARM64_REG_WZR).m_isZero);
        ARM_ASSERT(!arch.getRegister(ARM64_REG_SP).m_isZero);
        check("wsp parent", arch.getRegister(ARM64_REG_WSP).m_parent, ARM64_REG_SP);
        static const uint simdBits[6] = { 8, 16, 32, 64, 128, 128 };
        static const uint simdBase[6] = { ARM64_REG_B0, ARM64_REG_H0,
                ARM64_REG_S0, ARM64_REG_D0, ARM64_REG_Q0, ARM64_REG_V0 };
        for (uint k = 0; k < 6; k++)
        {
            const ArchitectureRegister& r = arch.getRegister(simdBase[k] + 20);
            check("simd bits", r.m_bits, simdBits[k]);
            check("simd parent", r.m_parent, ARM64_REG_Z0 + 20);
            check("simd offset", r.m_bitOffset, 0);
            ARM_ASSERT(r.m_zeroExtendsParent);
            check("simd file", r.m_file, ArchitectureRegister::FILE_VECTOR);
        }

        // The flags
        uint flag = 0;
        ARM_ASSERT(arch.getFlagRegister(SemanticFlags::FLAG_CARRY, flag));
        check("carry", flag, ARM64_REG_C);
        ARM_ASSERT(arch.getFlagRegister(SemanticFlags::FLAG_ZERO, flag));
        check("zero", flag, ARM64_REG_Z);
        ARM_ASSERT(arch.getFlagRegister(SemanticFlags::FLAG_SIGN, flag));
        check("sign", flag, ARM64_REG_N);
        ARM_ASSERT(arch.getFlagRegister(SemanticFlags::FLAG_OVERFLOW, flag));
        check("overflow", flag, ARM64_REG_V);
        ARM_ASSERT(!arch.getFlagRegister(SemanticFlags::FLAG_ADJUST, flag));
        check("c file", arch.getRegister(ARM64_REG_C).m_file,
              ArchitectureRegister::FILE_FLAG);

        checkFloatEnvironment(arch, ARM64_REG_FPCR, ARM64_REG_FPSR);

        // Supported operations: 64 bit arithmetic and multiply high, a
        // vector divide only for the scalable vectors
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_ADD,
                                      SemanticType::integer(64)));
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_MULTIPLY_HIGH_UNSIGNED,
                                      SemanticType::integer(64)));
        ARM_ASSERT(!arch.isSupported(SemanticOperation::OP_ADD,
                                       SemanticType::integer(128)));
        SemanticType v4i32 = SemanticType::vector(SemanticType::integer(32), 4);
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_ADD, v4i32));
        ARM_ASSERT(!arch.isSupported(SemanticOperation::OP_DIVIDE_SIGNED, v4i32));
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_DIVIDE_SIGNED,
                                      SemanticType::scalable(v4i32)));
        SemanticType v2f64 = SemanticType::vector(
                SemanticType::floating(SemanticType::FLOAT_DOUBLE), 2);
        ARM_ASSERT(arch.isSupported(SemanticOperation::OP_FLOAT_DIVIDE, v2f64));
    }

    void testVectorLengths()
    {
        // The default: the test processors, VL = 128 and SVL = 512
        Arm64Architecture defaults;
        check("default VL", defaults.getVectorBits(), 128);
        check("default SVL", defaults.getStreamingVectorBits(), 512);
        check("max vector bits", defaults.getMaximumVectorBits(), 128);
        // z and its predicates hold the larger vector, the rows of za are
        // SVL bits
        check("z0 bits", defaults.getRegister(ARM64_REG_Z0).m_bits, 512);
        check("p0 bits", defaults.getRegister(ARM64_REG_P0).m_bits, 64);
        check("ffr bits", defaults.getRegister(ARM64_REG_FFR).m_bits, 64);
        check("za row bits", defaults.getRegister(ARM64_REG_ZA_ROW0 + 63).m_bits, 512);
        check("zt0 bits", defaults.getRegister(ARM64_REG_ZT0).m_bits, 512);
        check("za file", defaults.getRegister(ARM64_REG_ZA_ROW0).m_file,
              ArchitectureRegister::FILE_MATRIX);
        check("p file", defaults.getRegister(ARM64_REG_P0).m_file,
              ArchitectureRegister::FILE_MASK);

        Arm64Architecture narrowVectors(256, 128);
        check("small VL", narrowVectors.getVectorBits(), 256);
        check("small SVL", narrowVectors.getStreamingVectorBits(), 128);
        check("small max vector bits", narrowVectors.getMaximumVectorBits(), 256);
        check("small z bits", narrowVectors.getRegister(ARM64_REG_Z0 + 31).m_bits, 256);
        check("small p bits", narrowVectors.getRegister(ARM64_REG_P0 + 15).m_bits, 32);
        check("small za bits", narrowVectors.getRegister(ARM64_REG_ZA_ROW0).m_bits, 128);

        Arm64Architecture large(512, 512);
        check("large z bits", large.getRegister(ARM64_REG_Z0).m_bits, 512);
        check("large p bits", large.getRegister(ARM64_REG_P0).m_bits, 64);
    }

    void testArm64Aliasing()
    {
        ArchitectureDescriptionPtr arch =
            ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        TestArmMemory memory;
        SemanticInterpreter interpreter(arch, memory);

        // Write x0, then write w0: the upper half is zeroed
        interpreter.setRegister(ARM64_REG_X0, SemanticValue(0xFFFFFFFFFFFFFFFFULL));
        check("w0 read", get(interpreter, ARM64_REG_W0), 0xFFFFFFFFULL);
        interpreter.setRegister(ARM64_REG_W0, SemanticValue(0x12345678));
        check("x0 after w0", get(interpreter, ARM64_REG_X0), 0x12345678ULL);
        interpreter.setRegister(ARM64_REG_X0, SemanticValue(0xAAAAAAAABBBBBBBBULL));
        check("w0 low", get(interpreter, ARM64_REG_W0), 0xBBBBBBBBULL);

        // The zero register reads 0 and drops the writes; wzr as well
        interpreter.setRegister(ARM64_REG_XZR, SemanticValue(5));
        check("xzr", get(interpreter, ARM64_REG_XZR), 0);
        interpreter.setRegister(ARM64_REG_WZR, SemanticValue(5));
        check("wzr", get(interpreter, ARM64_REG_WZR), 0);

        // wsp
        interpreter.setRegister(ARM64_REG_SP, SemanticValue(0x7FFFFFFFFFFF0000ULL));
        check("wsp read", get(interpreter, ARM64_REG_WSP), 0xFFFF0000ULL);
        interpreter.setRegister(ARM64_REG_WSP, SemanticValue(0x1000));
        check("sp after wsp", get(interpreter, ARM64_REG_SP), 0x1000);

        // Fill z3 (512 bits), then write d3: v3[64..127] and z3[128..511] are
        // zeroed
        SemanticValue z;
        for (uint i = 0; i < 8; i++)
            z.setWord(i, 0x1111111111111111ULL * (i + 1));
        interpreter.setRegister(ARM64_REG_Z0 + 3, z);
        check("v3 low", get(interpreter, ARM64_REG_V0 + 3, 0), 0x1111111111111111ULL);
        check("v3 high", get(interpreter, ARM64_REG_V0 + 3, 1), 0x2222222222222222ULL);
        check("q3 high", get(interpreter, ARM64_REG_Q0 + 3, 1), 0x2222222222222222ULL);
        check("s3", get(interpreter, ARM64_REG_S0 + 3), 0x11111111);
        check("h3", get(interpreter, ARM64_REG_H0 + 3), 0x1111);
        check("b3", get(interpreter, ARM64_REG_B0 + 3), 0x11);
        interpreter.setRegister(ARM64_REG_D0 + 3, SemanticValue(0xDEADBEEFULL));
        check("z3 word 0", get(interpreter, ARM64_REG_Z0 + 3, 0), 0xDEADBEEFULL);
        for (uint i = 1; i < 8; i++)
            check("z3 zeroed", get(interpreter, ARM64_REG_Z0 + 3, i), 0);
        check("v3 zeroed", get(interpreter, ARM64_REG_V0 + 3, 1), 0);

        // Writing b5 zeroes the rest
        interpreter.setRegister(ARM64_REG_Z0 + 5, z);
        interpreter.setRegister(ARM64_REG_B0 + 5, SemanticValue(0xAB));
        check("z5 word 0", get(interpreter, ARM64_REG_Z0 + 5, 0), 0xAB);
        check("z5 word 4", get(interpreter, ARM64_REG_Z0 + 5, 4), 0);

        // Writing v keeps nothing of the upper bits of z
        interpreter.setRegister(ARM64_REG_Z0 + 6, z);
        SemanticValue v;
        v.setWord(0, 7);
        v.setWord(1, 8);
        interpreter.setRegister(ARM64_REG_V0 + 6, v);
        check("z6 word 1", get(interpreter, ARM64_REG_Z0 + 6, 1), 8);
        check("z6 word 2", get(interpreter, ARM64_REG_Z0 + 6, 2), 0);

        // A 512 bit z value survives a roundtrip
        interpreter.setRegister(ARM64_REG_Z0 + 7, z);
        for (uint i = 0; i < 8; i++)
            check("z7", get(interpreter, ARM64_REG_Z0 + 7, i),
                  0x1111111111111111ULL * (i + 1));

        // Predicates are plain registers; the flags are single bits
        interpreter.setRegister(ARM64_REG_P0 + 2, SemanticValue(0x5555));
        check("p2", get(interpreter, ARM64_REG_P0 + 2), 0x5555);
        interpreter.setRegister(ARM64_REG_C, SemanticValue(1));
        check("c", get(interpreter, ARM64_REG_C), 1);
    }

    virtual void test()
    {
        testArm32();
        testArm32Aliasing();
        testArm64();
        testVectorLengths();
        testArm64Aliasing();
        cout << "ARM architecture tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestArmArchitecture g_globalTestArmArchitecture;
