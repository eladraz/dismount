/*
 * TestRiscvArchitecture.cpp
 *
 * Tests the RISC-V (RV32 and RV64) description of the semantic layer: the
 * register tables for the XLEN and VLEN, the ABI and numeric names, the
 * aliasing of the sub-registers of fcsr and vcsr (through the interpreter),
 * the vector lengths and the floating point environment.
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
#include "dismount/proc/riscv/RiscvArchitecture.h"

using namespace riscvdis;

// TESTS_ASSERT which tells the line of the condition which failed
#define RISCV_ASSERT(condition) \
    do { \
        if (!(condition)) \
            cout << "FAILED line " << __LINE__ << ": " << #condition << endl; \
        TESTS_ASSERT(condition); \
    } while (0)

// The registers tests don't touch the memory
class TestRiscvMemory : public SemanticMemory {
public:
    virtual bool read(uint64, uint8*, uint) { return false; }
    virtual bool write(uint64, const uint8*, uint) { return false; }
};

class TestObjectTestRiscvArchitecture : public cTestObject {
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
            RISCV_ASSERT(false);
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
            RISCV_ASSERT(arch.findRegister(cString(r.m_name), found));
            check(r.m_name, found, i);
            RISCV_ASSERT(r.m_bits >= 1);
            if (r.m_parent != ArchitectureRegister::NO_PARENT)
            {
                const ArchitectureRegister& parent = arch.getRegister(r.m_parent);
                check("parent is full", parent.m_parent,
                      (uint)ArchitectureRegister::NO_PARENT);
                RISCV_ASSERT(r.m_bitOffset + r.m_bits <= parent.m_bits);
            }
        }
        uint unused;
        RISCV_ASSERT(!arch.findRegister(cString("notaregister"), unused));
        RISCV_ASSERT(!arch.findRegister(cString("x32"), unused));
        RISCV_ASSERT(!arch.findRegister(cString("x01"), unused));
        RISCV_ASSERT(!arch.findRegister(cString("f"), unused));
        RISCV_ASSERT(!arch.findRegister(cString(""), unused));
    }

    static uint64 get(SemanticInterpreter& interpreter, uint registerId,
                      uint word = 0)
    {
        return interpreter.getRegister(registerId).getWord(word);
    }

    void checkArchitecture(RiscvInstructionSet::DisassemblerTypes type,
                           uint vlen)
    {
        const bool is64 = type == RiscvInstructionSet::RISCV_RV64;
        const uint xlen = is64 ? 64 : 32;
        RiscvArchitecture arch(type, vlen);
        check("registers count", arch.getRegistersCount(), RISCV_REGISTERS_COUNT);
        check("type", arch.getType(), is64 ?
                OpcodeSubsystems::DISASSEMBLER_RISCV_64 :
                OpcodeSubsystems::DISASSEMBLER_RISCV_32);
        check("instruction set", arch.getInstructionSet(), type);
        check("xlen", arch.getXlen(), xlen);
        check("vlen", arch.getVectorBits(), vlen);
        check("address bits", arch.getAddressBits(), xlen);
        check("native bits", arch.getNativeIntegerBits(), xlen);
        check("max vector bits", arch.getMaximumVectorBits(), vlen);
        RISCV_ASSERT(arch.isVectorLengthScalable());
        RISCV_ASSERT(arch.isLittleEndian());
        RISCV_ASSERT(!arch.isCarryInvertedOnSubtract());
        check("memory model", arch.getMemoryModel(),
              ArchitectureDescription::MEMORY_WEAK);
        check("sp", arch.getStackPointer(), RISCV_REG_SP);
        check("pc", arch.getProgramCounter(), RISCV_REG_PC);
        uint reg = 0;
        RISCV_ASSERT(!arch.getModeRegister(reg));
        for (uint flag = 1; flag <= 0x40; flag <<= 1)
            RISCV_ASSERT(!arch.getFlagRegister(flag, reg));
        checkTable(arch);

        // The sizes
        check("x5 bits", arch.getRegister(RISCV_REG_X0 + 5).m_bits, xlen);
        check("pc bits", arch.getRegister(RISCV_REG_PC).m_bits, xlen);
        check("f9 bits", arch.getRegister(RISCV_REG_F0 + 9).m_bits, 64);
        check("v31 bits", arch.getRegister(RISCV_REG_V31).m_bits, vlen);
        check("fcsr bits", arch.getRegister(RISCV_REG_FCSR).m_bits, 32);
        check("vl bits", arch.getRegister(RISCV_REG_VL).m_bits, xlen);
        check("vtype bits", arch.getRegister(RISCV_REG_VTYPE).m_bits, xlen);
        check("vstart bits", arch.getRegister(RISCV_REG_VSTART).m_bits, xlen);
        check("ssp bits", arch.getRegister(RISCV_REG_SSP).m_bits, xlen);
        check("elp bits", arch.getRegister(RISCV_REG_ELP).m_bits, 1);
        check("reservation bits",
              arch.getRegister(RISCV_REG_RESERVATION_ADDRESS).m_bits, xlen);
        RISCV_ASSERT(arch.getRegister(RISCV_REG_X0).m_isZero);
        for (uint i = 1; i < 32; i++)
            RISCV_ASSERT(!arch.getRegister(RISCV_REG_X0 + i).m_isZero);
        check("x file", arch.getRegister(RISCV_REG_X0).m_file,
              ArchitectureRegister::FILE_GENERAL);
        check("pc file", arch.getRegister(RISCV_REG_PC).m_file,
              ArchitectureRegister::FILE_PROGRAM_COUNTER);
        check("v file", arch.getRegister(RISCV_REG_V0).m_file,
              ArchitectureRegister::FILE_VECTOR);
        check("fcsr file", arch.getRegister(RISCV_REG_FCSR).m_file,
              ArchitectureRegister::FILE_CONTROL);

        // The sub-registers of fcsr and vcsr
        check("fflags parent", arch.getRegister(RISCV_REG_FFLAGS).m_parent,
              RISCV_REG_FCSR);
        check("fflags offset", arch.getRegister(RISCV_REG_FFLAGS).m_bitOffset, 0);
        check("fflags bits", arch.getRegister(RISCV_REG_FFLAGS).m_bits, 5);
        check("frm parent", arch.getRegister(RISCV_REG_FRM).m_parent,
              RISCV_REG_FCSR);
        check("frm offset", arch.getRegister(RISCV_REG_FRM).m_bitOffset, 5);
        check("frm bits", arch.getRegister(RISCV_REG_FRM).m_bits, 3);
        check("vxsat parent", arch.getRegister(RISCV_REG_VXSAT).m_parent,
              RISCV_REG_VCSR);
        check("vxsat offset", arch.getRegister(RISCV_REG_VXSAT).m_bitOffset, 0);
        check("vxsat bits", arch.getRegister(RISCV_REG_VXSAT).m_bits, 1);
        check("vxrm parent", arch.getRegister(RISCV_REG_VXRM).m_parent,
              RISCV_REG_VCSR);
        check("vxrm offset", arch.getRegister(RISCV_REG_VXRM).m_bitOffset, 1);
        check("vxrm bits", arch.getRegister(RISCV_REG_VXRM).m_bits, 2);

        // The numeric and the ABI names
        check("x0", find(arch, "x0"), RISCV_REG_X0);
        check("x31", find(arch, "x31"), RISCV_REG_X0 + 31);
        check("f0", find(arch, "f0"), RISCV_REG_F0);
        check("f31", find(arch, "f31"), RISCV_REG_F0 + 31);
        check("v17", find(arch, "v17"), RISCV_REG_V0 + 17);
        check("zero", find(arch, "zero"), RISCV_REG_X0);
        check("ra", find(arch, "ra"), RISCV_REG_RA);
        check("sp", find(arch, "sp"), RISCV_REG_SP);
        check("gp", find(arch, "gp"), RISCV_REG_GP);
        check("tp", find(arch, "tp"), RISCV_REG_TP);
        check("t0", find(arch, "t0"), RISCV_REG_X0 + 5);
        check("t1", find(arch, "t1"), RISCV_REG_X0 + 6);
        check("t2", find(arch, "t2"), RISCV_REG_X0 + 7);
        check("s0", find(arch, "s0"), RISCV_REG_X0 + 8);
        check("fp", find(arch, "fp"), RISCV_REG_X0 + 8);
        check("s1", find(arch, "s1"), RISCV_REG_X0 + 9);
        check("a0", find(arch, "a0"), RISCV_REG_X0 + 10);
        check("a7", find(arch, "a7"), RISCV_REG_X0 + 17);
        check("s2", find(arch, "s2"), RISCV_REG_X0 + 18);
        check("s11", find(arch, "s11"), RISCV_REG_X0 + 27);
        check("t3", find(arch, "t3"), RISCV_REG_X0 + 28);
        check("t6", find(arch, "t6"), RISCV_REG_X0 + 31);
        check("ft0", find(arch, "ft0"), RISCV_REG_F0);
        check("ft7", find(arch, "ft7"), RISCV_REG_F0 + 7);
        check("fs0", find(arch, "fs0"), RISCV_REG_F0 + 8);
        check("fs1", find(arch, "fs1"), RISCV_REG_F0 + 9);
        check("fa0", find(arch, "fa0"), RISCV_REG_F0 + 10);
        check("fa7", find(arch, "fa7"), RISCV_REG_F0 + 17);
        check("fs2", find(arch, "fs2"), RISCV_REG_F0 + 18);
        check("fs11", find(arch, "fs11"), RISCV_REG_F0 + 27);
        check("ft8", find(arch, "ft8"), RISCV_REG_F0 + 28);
        check("ft11", find(arch, "ft11"), RISCV_REG_F0 + 31);
        check("fcsr", find(arch, "fcsr"), RISCV_REG_FCSR);
        check("fflags", find(arch, "fflags"), RISCV_REG_FFLAGS);
        check("frm", find(arch, "frm"), RISCV_REG_FRM);
        check("vstart", find(arch, "vstart"), RISCV_REG_VSTART);
        check("vcsr", find(arch, "vcsr"), RISCV_REG_VCSR);
        check("vxsat", find(arch, "vxsat"), RISCV_REG_VXSAT);
        check("vxrm", find(arch, "vxrm"), RISCV_REG_VXRM);
        check("vl", find(arch, "vl"), RISCV_REG_VL);
        check("vtype", find(arch, "vtype"), RISCV_REG_VTYPE);
        check("ssp", find(arch, "ssp"), RISCV_REG_SSP);
        check("elp", find(arch, "elp"), RISCV_REG_ELP);
        check("pc", find(arch, "pc"), RISCV_REG_PC);
        check("reservation_address", find(arch, "reservation_address"),
              RISCV_REG_RESERVATION_ADDRESS);
        check("reservation_size", find(arch, "reservation_size"),
              RISCV_REG_RESERVATION_SIZE);

        // Supported operations
        RISCV_ASSERT(arch.isSupported(SemanticOperation::OP_ADD,
                                      SemanticType::integer(xlen)));
        RISCV_ASSERT(arch.isSupported(SemanticOperation::OP_MULTIPLY_HIGH_UNSIGNED,
                                      SemanticType::integer(xlen)));
        RISCV_ASSERT(arch.isSupported(SemanticOperation::OP_DIVIDE_SIGNED,
                                      SemanticType::integer(xlen)));
        RISCV_ASSERT(arch.isSupported(SemanticOperation::OP_COUNT_LEADING_ZEROS,
                                      SemanticType::integer(xlen)));
        RISCV_ASSERT(!arch.isSupported(SemanticOperation::OP_ADD_CARRY,
                                       SemanticType::integer(xlen)));
        RISCV_ASSERT(!arch.isSupported(SemanticOperation::OP_ADD,
                                       SemanticType::integer(2 * xlen)));
        RISCV_ASSERT(arch.isSupported(SemanticOperation::OP_TRAP,
                                      SemanticType()));
        RISCV_ASSERT(arch.isSupported(SemanticOperation::OP_FLOAT_DIVIDE,
                SemanticType::floating(SemanticType::FLOAT_DOUBLE)));
        RISCV_ASSERT(arch.isSupported(SemanticOperation::OP_FLOAT_MULTIPLY_ADD,
                SemanticType::floating(SemanticType::FLOAT_HALF)));
        RISCV_ASSERT(!arch.isSupported(SemanticOperation::OP_FLOAT_ADD,
                SemanticType::floating(SemanticType::FLOAT_EXTENDED)));
        SemanticType v4i32 = SemanticType::vector(SemanticType::integer(32), 4);
        RISCV_ASSERT(arch.isSupported(SemanticOperation::OP_ADD,
                                      SemanticType::scalable(v4i32)));
    }

    void checkFloatEnvironment()
    {
        RiscvArchitecture arch;
        FloatEnvironment env;
        RISCV_ASSERT(arch.getFloatEnvironment(0, env));
        check("control", env.m_controlRegister, RISCV_REG_FCSR);
        check("status", env.m_statusRegister, RISCV_REG_FCSR);
        check("rounding offset", env.m_roundingOffset, 5);
        check("rounding bits", env.m_roundingBits, 3);
        check("rne", env.m_roundingModes[0], SemanticOperation::ROUND_NEAREST_EVEN);
        check("rtz", env.m_roundingModes[1], SemanticOperation::ROUND_TOWARD_ZERO);
        check("rdn", env.m_roundingModes[2], SemanticOperation::ROUND_TOWARD_NEGATIVE);
        check("rup", env.m_roundingModes[3], SemanticOperation::ROUND_TOWARD_POSITIVE);
        check("rmm", env.m_roundingModes[4], SemanticOperation::ROUND_NEAREST_AWAY);
        check("flush", env.m_flushToZeroOffset, FloatEnvironment::NO_FIELD);
        check("daz", env.m_denormalsAreZeroOffset, FloatEnvironment::NO_FIELD);
        check("precision", env.m_precisionOffset, FloatEnvironment::NO_FIELD);
        // fflags: nv dz of uf nx are bits 4 3 2 1 0, the denormal operand has
        // no flag
        check("nv", env.m_flagOffsets[0], 4);
        check("denormal", env.m_flagOffsets[1], FloatEnvironment::NO_FIELD);
        check("dz", env.m_flagOffsets[2], 3);
        check("of", env.m_flagOffsets[3], 2);
        check("uf", env.m_flagOffsets[4], 1);
        check("nx", env.m_flagOffsets[5], 0);
        for (uint i = 0; i < SemanticFloatFlags::FLOAT_FLAGS_COUNT; i++)
            check("no mask", env.m_maskOffsets[i], FloatEnvironment::NO_FIELD);
        check("trap", env.m_trapMode, FloatEnvironment::TRAP_NONE);
        check("nan rule", env.m_nanRule, FloatEnvironment::NAN_DEFAULT);
        RISCV_ASSERT(!env.m_isDefaultNanNegative);
        RISCV_ASSERT(!env.m_isTininessBeforeRounding);
        RISCV_ASSERT(!env.m_isInputFlushSignaled);
        RISCV_ASSERT(!env.m_isMinMaxNanSignaling);
        check("default nan offset", env.m_defaultNanOffset,
              FloatEnvironment::NO_FIELD);

        // The environments of the other processors keep their 2 bit field
        FloatEnvironment defaults;
        check("default rounding bits", defaults.m_roundingBits, 2);
        ArchitectureDescriptionPtr arm = ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        FloatEnvironment armEnv;
        RISCV_ASSERT(arm->getFloatEnvironment(0, armEnv));
        check("arm rounding bits", armEnv.m_roundingBits, 2);
        ArchitectureDescriptionPtr ia32 = ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        FloatEnvironment ia32Env;
        RISCV_ASSERT(ia32->getFloatEnvironment(0, ia32Env));
        check("ia32 rounding bits", ia32Env.m_roundingBits, 2);
    }

    void checkAliasing()
    {
        ArchitectureDescriptionPtr arch(new RiscvArchitecture(
                RiscvInstructionSet::RISCV_RV64, 128));
        TestRiscvMemory memory;
        SemanticInterpreter interpreter(arch, memory);

        // fcsr = frm 5 (rup... reserved value but a value), fflags 0x15
        interpreter.setRegister(RISCV_REG_FCSR, SemanticValue(0xB5));
        check("frm", get(interpreter, RISCV_REG_FRM), 5);
        check("fflags", get(interpreter, RISCV_REG_FFLAGS), 0x15);
        // Writing frm changes only its bits
        interpreter.setRegister(RISCV_REG_FRM, SemanticValue(2));
        check("fcsr after frm", get(interpreter, RISCV_REG_FCSR), 0x55);
        interpreter.setRegister(RISCV_REG_FFLAGS, SemanticValue(0x0A));
        check("fcsr after fflags", get(interpreter, RISCV_REG_FCSR), 0x4A);
        check("fflags again", get(interpreter, RISCV_REG_FFLAGS), 0x0A);

        // vcsr: vxsat is bit 0 and vxrm bits 1-2
        interpreter.setRegister(RISCV_REG_VCSR, SemanticValue(0x5));
        check("vxsat", get(interpreter, RISCV_REG_VXSAT), 1);
        check("vxrm", get(interpreter, RISCV_REG_VXRM), 2);
        interpreter.setRegister(RISCV_REG_VXRM, SemanticValue(3));
        check("vcsr after vxrm", get(interpreter, RISCV_REG_VCSR), 0x7);
        interpreter.setRegister(RISCV_REG_VXSAT, SemanticValue(0));
        check("vcsr after vxsat", get(interpreter, RISCV_REG_VCSR), 0x6);

        // The registers of the integer file don't alias
        interpreter.setRegister(RISCV_REG_X0 + 5, SemanticValue(0xFFFFFFFFFFFFFFFFULL));
        check("x5", get(interpreter, RISCV_REG_X0 + 5), 0xFFFFFFFFFFFFFFFFULL);
        check("x6", get(interpreter, RISCV_REG_X0 + 6), 0);
    }

    virtual void test()
    {
        checkArchitecture(RiscvInstructionSet::RISCV_RV64, 128);
        checkArchitecture(RiscvInstructionSet::RISCV_RV64, 256);
        checkArchitecture(RiscvInstructionSet::RISCV_RV64, 512);
        checkArchitecture(RiscvInstructionSet::RISCV_RV32, 128);
        checkArchitecture(RiscvInstructionSet::RISCV_RV32, 512);
        checkFloatEnvironment();
        checkAliasing();

        // The factory
        ArchitectureDescriptionPtr rv64 = ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        check("factory rv64", rv64->getNativeIntegerBits(), 64);
        check("factory rv64 vlen", rv64->getMaximumVectorBits(), 128);
        ArchitectureDescriptionPtr rv32 = ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_RISCV_32);
        check("factory rv32", rv32->getNativeIntegerBits(), 32);
        check("factory rv32 type", rv32->getType(),
              OpcodeSubsystems::DISASSEMBLER_RISCV_32);
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestRiscvArchitecture g_globalTestRiscvArchitecture;
