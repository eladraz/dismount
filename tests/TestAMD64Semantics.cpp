/*
 * TestAMD64Semantics.cpp
 *
 * Tests the AMD64 description of the semantic layer: the register table, how
 * the IA-32 registers alias the 64 bit ones, and blocks over 64 bit
 * registers.
 *
 * The expected values were computed by hand.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/data/datastream.h"
#include "xStl/data/smartptr.h"
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
#include "dismount/proc/ia32/AMD64Architecture.h"

using namespace ia32dis;

typedef SemanticOperation Op;

// A memory of SIZE bytes at BASE, the accesses outside of it fail
class TestAmd64Memory : public SemanticMemory {
public:
    enum { SIZE = 0x1000 };

    TestAmd64Memory() :
        m_base(0x7FFF12340000ULL),
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
            buffer[i] = m_data[(uint)(address - m_base) + i];
        return true;
    }

    virtual bool write(uint64 address, const uint8* buffer, uint size)
    {
        if (!isAccessible(address, size))
            return false;
        for (uint i = 0; i < size; i++)
            m_data[(uint)(address - m_base) + i] = buffer[i];
        return true;
    }

    uint8 getByte(uint64 address) const
    {
        return m_data[(uint)(address - m_base)];
    }

    // The address of the first byte, above 4GB
    const uint64 m_base;

private:
    bool isAccessible(uint64 address, uint size) const
    {
        return (address >= m_base) && (address + size <= m_base + SIZE);
    }

    cBuffer m_data;
};

class TestObjectTestAMD64Semantics : public cTestObject {
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

    static SemanticOperand reg(uint id, uint bits)
    {
        return SemanticOperand::registerOperand(id, SemanticType::integer(bits));
    }

    static SemanticOperand imm(int64 value, uint bits)
    {
        return SemanticOperand::immediate(value, SemanticType::integer(bits));
    }

    static ProcessorAddress addr(uint64 address)
    {
        return ProcessorAddress(ProcessorAddress::PROCESSOR_64, address);
    }

    static void emit(SemanticBlock& block, Op::Operation operation, uint bits,
                     const SemanticOperand& destination,
                     const SemanticOperand& s0 = SemanticOperand(),
                     const SemanticOperand& s1 = SemanticOperand(),
                     const SemanticOperand& s2 = SemanticOperand())
    {
        block.append(SemanticInstruction(operation, SemanticType::integer(bits),
                                         destination, s0, s1, s2));
    }

    static ArchitectureDescriptionPtr getArchitecture()
    {
        return ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_AMD_64);
    }

    // Return the value of a register
    static uint64 get(SemanticInterpreter& interpreter, uint registerId)
    {
        return interpreter.getRegister(registerId).getWord(0);
    }

    void testArchitecture()
    {
        ArchitectureDescriptionPtr arch = getArchitecture();
        check("registers count", arch->getRegistersCount(),
              AMD64_REGISTERS_COUNT);
        check("type", arch->getType(), OpcodeSubsystems::DISASSEMBLER_AMD_64);
        check("address bits", arch->getAddressBits(), 64);
        check("native bits", arch->getNativeIntegerBits(), 64);
        check("sp", arch->getStackPointer(), AMD64_REG_RSP);
        check("pc", arch->getProgramCounter(), AMD64_REG_RIP);
        TESTS_ASSERT(arch->isLittleEndian());
        TESTS_ASSERT(!arch->isCarryInvertedOnSubtract());
        uint modeRegister;
        TESTS_ASSERT(!arch->getModeRegister(modeRegister));

        // The IA-32 ids keep their names, the flags are the IA-32 ones
        ArchitectureDescriptionPtr ia32 =
            ArchitectureDescriptionFactory::getArchitecture(
                                OpcodeSubsystems::DISASSEMBLER_INTEL_32);
        for (uint i = 0; i < ia32->getRegistersCount(); i++)
        {
            check(ia32->getRegister(i).m_name,
                  cString(arch->getRegister(i).m_name) ==
                  cString(ia32->getRegister(i).m_name), 1);
        }
        uint flag;
        TESTS_ASSERT(arch->getFlagRegister(SemanticFlags::FLAG_CARRY, flag));
        check("carry", flag, IA32_REG_CF);
        FloatEnvironment environment;
        TESTS_ASSERT(arch->getFloatEnvironment(0, environment));
        check("mxcsr", environment.m_controlRegister, IA32_REG_MXCSR);

        // Every name is found at its own id, and the sub-registers point to
        // full registers
        for (uint i = 0; i < arch->getRegistersCount(); i++)
        {
            const ArchitectureRegister& r = arch->getRegister(i);
            uint found = 0xFFFFFFFF;
            TESTS_ASSERT(arch->findRegister(cString(r.m_name), found));
            check(r.m_name, found, i);
            if (r.m_parent != ArchitectureRegister::NO_PARENT)
            {
                TESTS_ASSERT(r.m_parent < arch->getRegistersCount());
                TESTS_ASSERT(arch->getRegister(r.m_parent).m_parent ==
                             ArchitectureRegister::NO_PARENT);
                TESTS_ASSERT(r.m_bitOffset + r.m_bits <=
                             arch->getRegister(r.m_parent).m_bits);
            }
        }
        uint id;
        TESTS_ASSERT(arch->findRegister(cString("r12"), id));
        check("r12 id", id, AMD64_REG_R12);
        check("r12 bits", arch->getRegister(id).m_bits, 64);
        TESTS_ASSERT(arch->findRegister(cString("sil"), id));
        check("sil id", id, AMD64_REG_SIL);
        check("sil parent", arch->getRegister(id).m_parent, AMD64_REG_RSI);
        TESTS_ASSERT(arch->findRegister(cString("xmm15"), id));
        check("xmm15 id", id, AMD64_REG_XMM15);
        TESTS_ASSERT(!arch->findRegister(cString("r16"), id));
        check("fs_base bits", arch->getRegister(IA32_REG_FS_BASE).m_bits, 64);

        // The 64 bit integer operations are supported, the vectors as in
        // IA-32
        const SemanticType i64 = SemanticType::integer(64);
        TESTS_ASSERT(arch->isSupported(Op::OP_ADD, i64));
        TESTS_ASSERT(arch->isSupported(Op::OP_MULTIPLY_HIGH_UNSIGNED, i64));
        TESTS_ASSERT(!arch->isSupported(Op::OP_FLOAT_ADD, i64));
        TESTS_ASSERT(!ia32->isSupported(Op::OP_ADD, i64));
        TESTS_ASSERT(arch->isSupported(Op::OP_ADD, SemanticType::vector(
                                    SemanticType::integer(32), 4)));
    }

    void testAliasing()
    {
        TestAmd64Memory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);
        const uint64 full = 0x1122334455667788ULL;

        // Writing eax zero extends rax
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(full));
        interpreter.setRegister(IA32_REG_EAX, SemanticValue(0xAABBCCDDULL));
        check("rax after eax", get(interpreter, AMD64_REG_RAX), 0xAABBCCDDULL);
        check("eax", get(interpreter, IA32_REG_EAX), 0xAABBCCDDULL);

        // Writing ax, al and ah keeps the rest
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(full));
        interpreter.setRegister(IA32_REG_AX, SemanticValue(0xFFFF9999ULL));
        check("rax after ax", get(interpreter, AMD64_REG_RAX),
              0x1122334455669999ULL);
        interpreter.setRegister(IA32_REG_AL, SemanticValue(0x01));
        check("rax after al", get(interpreter, AMD64_REG_RAX),
              0x1122334455669901ULL);
        interpreter.setRegister(IA32_REG_AH, SemanticValue(0x02));
        check("rax after ah", get(interpreter, AMD64_REG_RAX),
              0x1122334455660201ULL);
        check("ah", get(interpreter, IA32_REG_AH), 0x02);
        check("ax", get(interpreter, IA32_REG_AX), 0x0201);
        check("eax read", get(interpreter, IA32_REG_EAX), 0x55660201ULL);
        // The other legacy registers
        interpreter.setRegister(AMD64_REG_RBX, SemanticValue(full));
        interpreter.setRegister(IA32_REG_BH, SemanticValue(0xEE));
        check("rbx after bh", get(interpreter, AMD64_REG_RBX),
              0x112233445566EE88ULL);
        interpreter.setRegister(AMD64_REG_RSP, SemanticValue(full));
        interpreter.setRegister(IA32_REG_SP, SemanticValue(0));
        check("rsp after sp", get(interpreter, AMD64_REG_RSP),
              0x1122334455660000ULL);
        interpreter.setRegister(IA32_REG_ESP, SemanticValue(0x10));
        check("rsp after esp", get(interpreter, AMD64_REG_RSP), 0x10);

        // r8d zero extends r8, r8w and r8b don't
        interpreter.setRegister(AMD64_REG_R8, SemanticValue(full));
        interpreter.setRegister(AMD64_REG_R8W, SemanticValue(0xAAAA));
        check("r8 after r8w", get(interpreter, AMD64_REG_R8),
              0x112233445566AAAAULL);
        interpreter.setRegister(AMD64_REG_R8B, SemanticValue(0xBB));
        check("r8 after r8b", get(interpreter, AMD64_REG_R8),
              0x112233445566AABBULL);
        check("r8d", get(interpreter, AMD64_REG_R8D), 0x5566AABBULL);
        interpreter.setRegister(AMD64_REG_R8D, SemanticValue(0x80000001ULL));
        check("r8 after r8d", get(interpreter, AMD64_REG_R8), 0x80000001ULL);
        interpreter.setRegister(AMD64_REG_R15, SemanticValue(full));
        interpreter.setRegister(AMD64_REG_R15D, SemanticValue(7));
        check("r15 after r15d", get(interpreter, AMD64_REG_R15), 7);

        // spl, bpl, sil and dil are bits 0..7 of rsp, rbp, rsi and rdi
        interpreter.setRegister(AMD64_REG_RSP, SemanticValue(full));
        interpreter.setRegister(AMD64_REG_SPL, SemanticValue(0xFE));
        check("rsp after spl", get(interpreter, AMD64_REG_RSP),
              0x11223344556677FEULL);
        check("spl", get(interpreter, AMD64_REG_SPL), 0xFE);
        interpreter.setRegister(AMD64_REG_RDI, SemanticValue(full));
        interpreter.setRegister(AMD64_REG_DIL, SemanticValue(0x01));
        check("rdi after dil", get(interpreter, AMD64_REG_RDI),
              0x1122334455667701ULL);
        interpreter.setRegister(AMD64_REG_RBP, SemanticValue(full));
        check("bpl", get(interpreter, AMD64_REG_BPL), 0x88);
        interpreter.setRegister(AMD64_REG_RSI, SemanticValue(full));
        check("sil", get(interpreter, AMD64_REG_SIL), 0x88);
        // ah is bits 8..15 of rax, not rsp
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(full));
        check("ah of rax", get(interpreter, IA32_REG_AH), 0x77);
        // dh, ch
        interpreter.setRegister(AMD64_REG_RDX, SemanticValue(full));
        check("dh", get(interpreter, IA32_REG_DH), 0x77);

        // eip is bits 0..31 of rip
        interpreter.setRegister(AMD64_REG_RIP, SemanticValue(0x123456789ABCULL));
        check("eip", get(interpreter, IA32_REG_EIP), 0x56789ABCULL);
        interpreter.setRegister(IA32_REG_EIP, SemanticValue(0x10));
        check("rip after eip", get(interpreter, AMD64_REG_RIP), 0x10);

        // The segment bases and the control registers are 64 bits
        interpreter.setRegister(IA32_REG_FS_BASE, SemanticValue(full));
        check("fs_base", get(interpreter, IA32_REG_FS_BASE), full);
        interpreter.setRegister(AMD64_REG_CR8, SemanticValue(0xF));
        check("cr8", get(interpreter, AMD64_REG_CR8), 0xF);
        interpreter.setRegister(AMD64_REG_XMM15, SemanticValue(5));
        check("xmm15", get(interpreter, AMD64_REG_XMM15), 5);
        check("xmm8", get(interpreter, AMD64_REG_XMM8), 0);
    }

    void testRunBlocks()
    {
        TestAmd64Memory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);
        const uint64 start = 0x7FFF12340000ULL;

        // The 64 bit adds wrap, a 32 bit write zero extends, and the
        // fall-through address is a 64 bit rip
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_AMD_64);
        block.beginGuestInstruction(addr(start + 0x100), 4);
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0xFFFFFFFFFFFFFFFFULL));
        interpreter.setRegister(AMD64_REG_RCX, SemanticValue(2));
        emit(block, Op::OP_ADD, 64, reg(AMD64_REG_RAX, 64),
             reg(AMD64_REG_RAX, 64), reg(AMD64_REG_RCX, 64));
        // r9d = 0x80000000 * 2 in 32 bits: zero, and the upper bits of r9
        // are cleared
        interpreter.setRegister(AMD64_REG_R9, SemanticValue(0xDEADBEEFDEADBEEFULL));
        emit(block, Op::OP_ADD, 32, reg(AMD64_REG_R9D, 32),
             imm(0x80000000LL, 32), imm(0x80000000LL, 32));
        // rdx:rax style high multiply
        emit(block, Op::OP_MULTIPLY_HIGH_UNSIGNED, 64, reg(AMD64_REG_RDX, 64),
             imm(-1, 64), imm(-1, 64));
        // A 64 bit store and a 32 bit load at an address above 4GB
        emit(block, Op::OP_STORE, 64, SemanticOperand(),
             imm((int64)(start + 0x10), 64), imm(0x0102030405060708LL, 64));
        SemanticOperand t0 = block.newTemporary(SemanticType::integer(32));
        emit(block, Op::OP_LOAD, 32, t0, imm((int64)(start + 0x14), 64));
        emit(block, Op::OP_MOVE, 32, reg(IA32_REG_ESI, 32), t0);
        interpreter.setRegister(AMD64_REG_RSI, SemanticValue(0xFFFFFFFFFFFFFFFFULL));
        block.setFallThroughAddress(addr(start + 0x104));
        check("exit", interpreter.run(block),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("rax", get(interpreter, AMD64_REG_RAX), 1);
        check("r9", get(interpreter, AMD64_REG_R9), 0);
        check("rdx", get(interpreter, AMD64_REG_RDX), 0xFFFFFFFFFFFFFFFEULL);
        check("byte 0", memory.getByte(start + 0x10), 0x08);
        check("byte 7", memory.getByte(start + 0x17), 0x01);
        check("rsi", get(interpreter, AMD64_REG_RSI), 0x01020304ULL);
        check("next address", interpreter.getNextAddress(), start + 0x104);
        check("rip", get(interpreter, AMD64_REG_RIP), start + 0x104);
        check("eip", get(interpreter, IA32_REG_EIP), (start + 0x104) & 0xFFFFFFFFULL);

        // A load outside of the memory is a memory fault
        SemanticBlock fault(OpcodeSubsystems::DISASSEMBLER_AMD_64);
        fault.beginGuestInstruction(addr(start), 3);
        SemanticOperand t1 = fault.newTemporary(SemanticType::integer(64));
        emit(fault, Op::OP_LOAD, 64, t1, imm((int64)(start - 8), 64));
        emit(fault, Op::OP_MOVE, 64, reg(AMD64_REG_R10, 64), t1);
        check("fault exit", interpreter.run(fault),
              SemanticInterpreter::EXIT_MEMORY_FAULT);
        check("fault address", interpreter.getFaultAddress(), start - 8);
    }

    // Decode and lift the instruction 'code' at 'address' and run it on
    // 'interpreter'. Return the exit of the block
    SemanticInterpreter::ExitReason liftAndRun(const uint8* code, uint size,
                                               uint64 address,
                                               SemanticInterpreter& interpreter,
                                               const SemanticLifterPtr& lifter)
    {
        cBuffer bytes(size);
        for (uint i = 0; i < size; i++)
            bytes[i] = code[i];
        cMemoryStream stream(bytes);
        StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
            OpcodeSubsystems::DISASSEMBLER_AMD_64,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true, addr(address));
        OpcodePtr opcode = disassembler->next();
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_AMD_64);
        lifter->lift(opcode, block);
        block.setFallThroughAddress(addr(address + size));
        return interpreter.run(block);
    }

    // The lifter in 64 bit mode
    void testLifter()
    {
        TestAmd64Memory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);
        SemanticLifterPtr lifter = SemanticLifterFactory::getLifter(
                                OpcodeSubsystems::DISASSEMBLER_AMD_64);
        const uint64 start = memory.m_base + 0x100;

        // add rax, rbx: 64 bit flags
        const uint8 add[] = { 0x48, 0x01, 0xD8 };
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0x7FFFFFFFFFFFFFFFULL));
        interpreter.setRegister(AMD64_REG_RBX, SemanticValue(1));
        check("add exit", liftAndRun(add, 3, start, interpreter, lifter),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("add rax", get(interpreter, AMD64_REG_RAX), 0x8000000000000000ULL);
        check("add of", get(interpreter, IA32_REG_OF), 1);
        check("add sf", get(interpreter, IA32_REG_SF), 1);
        check("add cf", get(interpreter, IA32_REG_CF), 0);
        check("add rip", get(interpreter, AMD64_REG_RIP), start + 3);

        // mov eax, ecx: a 32 bit write clears the upper half of rax
        const uint8 mov32[] = { 0x89, 0xC8 };
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0xFFFFFFFFFFFFFFFFULL));
        interpreter.setRegister(AMD64_REG_RCX, SemanticValue(0x123456789ULL));
        liftAndRun(mov32, 2, start, interpreter, lifter);
        check("mov eax", get(interpreter, AMD64_REG_RAX), 0x23456789ULL);
        // mov al, cl keeps the rest
        const uint8 mov8[] = { 0x88, 0xC8 };
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0xFFFFFFFFFFFFFFFFULL));
        liftAndRun(mov8, 2, start, interpreter, lifter);
        check("mov al", get(interpreter, AMD64_REG_RAX), 0xFFFFFFFFFFFFFF89ULL);
        // mov spl, 0x12 (REX) is bits 0..7 of rsp
        const uint8 movSpl[] = { 0x40, 0xB4, 0x12 };
        interpreter.setRegister(AMD64_REG_RSP, SemanticValue(0x1122334455667788ULL));
        liftAndRun(movSpl, 3, start, interpreter, lifter);
        check("mov spl", get(interpreter, AMD64_REG_RSP), 0x1122334455667712ULL);

        // cmovz eax, ecx with the condition false: the destination is written
        // anyway, so the upper half of rax is cleared
        const uint8 cmov32[] = { 0x0F, 0x44, 0xC1 };
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0xFFFFFFFFFFFFFFFFULL));
        interpreter.setRegister(AMD64_REG_RCX, SemanticValue(0x123456789ULL));
        interpreter.setRegister(IA32_REG_ZF, SemanticValue(0));
        liftAndRun(cmov32, 3, start, interpreter, lifter);
        check("cmovz false", get(interpreter, AMD64_REG_RAX), 0xFFFFFFFFULL);
        interpreter.setRegister(IA32_REG_ZF, SemanticValue(1));
        liftAndRun(cmov32, 3, start, interpreter, lifter);
        check("cmovz true", get(interpreter, AMD64_REG_RAX), 0x23456789ULL);
        // cmovz ax, cx with the condition false keeps the rest of rax
        const uint8 cmov16[] = { 0x66, 0x0F, 0x44, 0xC1 };
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0xFFFFFFFFFFFFFFFFULL));
        interpreter.setRegister(IA32_REG_ZF, SemanticValue(0));
        liftAndRun(cmov16, 4, start, interpreter, lifter);
        check("cmovz ax", get(interpreter, AMD64_REG_RAX), 0xFFFFFFFFFFFFFFFFULL);
        // popcnt rax, rcx: the zero flag tells a zero source, the rest are cleared
        const uint8 popcnt64[] = { 0xF3, 0x48, 0x0F, 0xB8, 0xC1 };
        interpreter.setRegister(AMD64_REG_RCX, SemanticValue(0xF0F0F0F0F0F0F0F0ULL));
        interpreter.setRegister(IA32_REG_CF, SemanticValue(1));
        liftAndRun(popcnt64, 5, start, interpreter, lifter);
        check("popcnt rax", get(interpreter, AMD64_REG_RAX), 32);
        check("popcnt zf", get(interpreter, IA32_REG_ZF), 0);
        check("popcnt cf", get(interpreter, IA32_REG_CF), 0);
        interpreter.setRegister(AMD64_REG_RCX, SemanticValue(0));
        liftAndRun(popcnt64, 5, start, interpreter, lifter);
        check("popcnt zero zf", get(interpreter, IA32_REG_ZF), 1);
        // tzcnt rax, rcx and lzcnt of zero: the operand size, the carry flag
        const uint8 tzcnt64[] = { 0xF3, 0x48, 0x0F, 0xBC, 0xC1 };
        liftAndRun(tzcnt64, 5, start, interpreter, lifter);
        check("tzcnt zero", get(interpreter, AMD64_REG_RAX), 64);
        check("tzcnt zero cf", get(interpreter, IA32_REG_CF), 1);
        check("tzcnt zero zf", get(interpreter, IA32_REG_ZF), 0);
        // ud2 is the invalid opcode exception
        const uint8 ud2[] = { 0x0F, 0x0B };
        check("ud2 exit", liftAndRun(ud2, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_TRAP);
        check("ud2 vector", interpreter.getTrapNumber(), 6);

        // push rax: 8 bytes
        const uint8 push[] = { 0x50 };
        interpreter.setRegister(AMD64_REG_RSP, SemanticValue(memory.m_base + 0x800));
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0x0102030405060708ULL));
        check("push exit", liftAndRun(push, 1, start, interpreter, lifter),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("push rsp", get(interpreter, AMD64_REG_RSP), memory.m_base + 0x7F8);
        check("push byte 0", memory.getByte(memory.m_base + 0x7F8), 0x08);
        check("push byte 7", memory.getByte(memory.m_base + 0x7FF), 0x01);
        // 66 50 pushes 2 bytes
        const uint8 push16[] = { 0x66, 0x50 };
        liftAndRun(push16, 2, start, interpreter, lifter);
        check("push16 rsp", get(interpreter, AMD64_REG_RSP), memory.m_base + 0x7F6);

        // push qword [rip + 0x10]: relative to the next instruction
        const uint8 pushRip[] = { 0xFF, 0x35, 0x10, 0x00, 0x00, 0x00 };
        const uint8 value = 0x5A;
        const uint64 target = start + 6 + 0x10;
        memory.write(target, &value, 1);
        liftAndRun(pushRip, 6, start, interpreter, lifter);
        check("push [rip]", memory.getByte(get(interpreter, AMD64_REG_RSP)), 0x5A);

        // movsxd rax, ecx
        const uint8 movsxd[] = { 0x48, 0x63, 0xC1 };
        interpreter.setRegister(AMD64_REG_RCX, SemanticValue(0x80000000ULL));
        liftAndRun(movsxd, 3, start, interpreter, lifter);
        check("movsxd", get(interpreter, AMD64_REG_RAX), 0xFFFFFFFF80000000ULL);

        // div rbx: rdx:rax / rbx, and the divide error
        const uint8 div[] = { 0x48, 0xF7, 0xF3 };
        interpreter.setRegister(AMD64_REG_RDX, SemanticValue(1));
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(5));
        interpreter.setRegister(AMD64_REG_RBX, SemanticValue(0x10));
        check("div exit", liftAndRun(div, 3, start, interpreter, lifter),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("div quotient", get(interpreter, AMD64_REG_RAX), 0x1000000000000000ULL);
        check("div remainder", get(interpreter, AMD64_REG_RDX), 5);
        interpreter.setRegister(AMD64_REG_RBX, SemanticValue(1));
        check("div overflow", liftAndRun(div, 3, start, interpreter, lifter),
              SemanticInterpreter::EXIT_TRAP);
        check("div overflow vector", interpreter.getTrapNumber(), 0);

        // shl rax, cl: the count is masked to 6 bits
        const uint8 shl[] = { 0x48, 0xD3, 0xE0 };
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(3));
        interpreter.setRegister(AMD64_REG_RCX, SemanticValue(65));
        liftAndRun(shl, 3, start, interpreter, lifter);
        check("shl 65", get(interpreter, AMD64_REG_RAX), 6);
        // shl eax, cl with a count of 32: a zero count, but eax is written
        const uint8 shl32[] = { 0xD3, 0xE0 };
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0xFFFFFFFF00000005ULL));
        interpreter.setRegister(AMD64_REG_RCX, SemanticValue(32));
        liftAndRun(shl32, 2, start, interpreter, lifter);
        check("shl eax 32", get(interpreter, AMD64_REG_RAX), 5);

        // syscall is a trap
        const uint8 syscall[] = { 0x0F, 0x05 };
        check("syscall exit", liftAndRun(syscall, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_TRAP);
        check("syscall kind", interpreter.getTrapKind(), Op::TRAP_SYSTEM_CALL);

        // swapgs exchanges the gs base
        const uint8 swapgs[] = { 0x0F, 0x01, 0xF8 };
        interpreter.setRegister(IA32_REG_GS_BASE, SemanticValue(0x1111));
        interpreter.setRegister(AMD64_REG_KERNEL_GS_BASE, SemanticValue(0x2222));
        liftAndRun(swapgs, 3, start, interpreter, lifter);
        check("swapgs gs", get(interpreter, IA32_REG_GS_BASE), 0x2222);
        check("swapgs kernel", get(interpreter, AMD64_REG_KERNEL_GS_BASE), 0x1111);
    }

    // Write the 8 bytes of a descriptor of the GDT at 'index'
    static void setDescriptor(TestAmd64Memory& memory, uint64 gdt, uint index,
                              uint64 descriptor)
    {
        uint8 bytes[8];
        for (uint i = 0; i < 8; i++)
            bytes[i] = (uint8)(descriptor >> (8 * i));
        memory.write(gdt + index * 8, bytes, 8);
    }

    // The segment registers, far branches and returns of 64 bit mode
    void testSegments()
    {
        TestAmd64Memory memory;
        SemanticInterpreter interpreter(getArchitecture(), memory);
        SemanticLifterPtr lifter = SemanticLifterFactory::getLifter(
                                OpcodeSubsystems::DISASSEMBLER_AMD_64);
        const uint64 start = memory.m_base + 0x100;
        const uint64 gdt = memory.m_base + 0x400;
        // 1 data, 2 base 0x87654321, 3 code 64 bit (L), 4 code 32 bit, 5 not
        // present, 6 code 16 bit, 7 L and D
        setDescriptor(memory, gdt, 1, 0x00CF92000000FFFFULL);
        setDescriptor(memory, gdt, 2, 0x87CF92654321FFFFULL);
        setDescriptor(memory, gdt, 3, 0x00AF9A000000FFFFULL);
        setDescriptor(memory, gdt, 4, 0x00CF9A000000FFFFULL);
        setDescriptor(memory, gdt, 5, 0x00CF12000000FFFFULL);
        setDescriptor(memory, gdt, 6, 0x00009A000000FFFFULL);
        setDescriptor(memory, gdt, 7, 0x00EF9A000000FFFFULL);
        interpreter.setRegister(IA32_REG_GDTR_BASE, SemanticValue(gdt));
        interpreter.setRegister(IA32_REG_GDTR_LIMIT, SemanticValue(0x3F));

        // mov fs, ax: the base of fs is the base of the descriptor
        const uint8 movFs[] = { 0x8E, 0xE0 };
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0x10));
        check("mov fs exit", liftAndRun(movFs, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        check("mov fs", get(interpreter, IA32_REG_FS), 0x10);
        check("fs base", get(interpreter, IA32_REG_FS_BASE), 0x87654321ULL);
        check("fs accessed", memory.getByte(gdt + 2 * 8 + 5) & 1, 1);
        // the base of ds is ignored
        const uint8 movDs[] = { 0x8E, 0xD8 };
        liftAndRun(movDs, 2, start, interpreter, lifter);
        check("ds base", get(interpreter, IA32_REG_DS_BASE), 0);
        // a null selector: allowed, the base is 0
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0));
        liftAndRun(movFs, 2, start, interpreter, lifter);
        check("null fs", get(interpreter, IA32_REG_FS), 0);
        check("null fs base", get(interpreter, IA32_REG_FS_BASE), 0);
        // a null ss: allowed at CPL 0, #GP at CPL 3
        const uint8 movSs[] = { 0x8E, 0xD0 };
        check("null ss exit", liftAndRun(movSs, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_FALL_THROUGH);
        interpreter.setRegister(IA32_REG_CS, SemanticValue(3));
        check("null ss cpl 3", liftAndRun(movSs, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_TRAP);
        check("null ss vector", interpreter.getTrapNumber(), 13);
        interpreter.setRegister(IA32_REG_CS, SemanticValue(0));
        // not present: #NP, past the limit and a system... #GP
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0x28));
        check("np exit", liftAndRun(movDs, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_TRAP);
        check("np vector", interpreter.getTrapNumber(), 11);
        interpreter.setRegister(AMD64_REG_RAX, SemanticValue(0x48));
        liftAndRun(movDs, 2, start, interpreter, lifter);
        check("limit vector", interpreter.getTrapNumber(), 13);

        // jmp far [rbx] (m16:32) to the 64 bit code segment
        const uint8 jmpFar[] = { 0xFF, 0x2B };
        const uint64 pointer = memory.m_base + 0x800;
        const uint8 target[] = { 0x78, 0x56, 0x34, 0x12, 0x18, 0x00 };
        memory.write(pointer, target, 6);
        interpreter.setRegister(AMD64_REG_RBX, SemanticValue(pointer));
        check("jmp far exit", liftAndRun(jmpFar, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_BRANCH);
        check("jmp far cs", get(interpreter, IA32_REG_CS), 0x18);
        check("jmp far rip", get(interpreter, AMD64_REG_RIP), 0x12345678);
        // to the data segment: #GP, to the 16 bit code segment and with L and D
        const uint8 dataTarget[] = { 0x78, 0x56, 0x34, 0x12, 0x08, 0x00 };
        memory.write(pointer, dataTarget, 6);
        check("jmp far data", liftAndRun(jmpFar, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_TRAP);
        const uint8 code16Target[] = { 0x78, 0x56, 0x34, 0x12, 0x30, 0x00 };
        memory.write(pointer, code16Target, 6);
        check("jmp far code16", liftAndRun(jmpFar, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_UNDEFINED);
        const uint8 longDefault[] = { 0x78, 0x56, 0x34, 0x12, 0x38, 0x00 };
        memory.write(pointer, longDefault, 6);
        check("jmp far L and D", liftAndRun(jmpFar, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_TRAP);

        // call far [rbx]: pushes cs and the return address, 4 bytes each
        const uint8 callFar[] = { 0xFF, 0x1B };
        memory.write(pointer, target, 6);
        interpreter.setRegister(IA32_REG_CS, SemanticValue(0));
        interpreter.setRegister(AMD64_REG_RSP, SemanticValue(memory.m_base + 0xF00));
        check("call far exit", liftAndRun(callFar, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_BRANCH);
        check("call far rsp", get(interpreter, AMD64_REG_RSP), memory.m_base + 0xEF8);
        check("call far return", memory.getByte(memory.m_base + 0xEF8), (start + 2) & 0xFF);
        check("call far cs", memory.getByte(memory.m_base + 0xEFC), 0);

        // iretq: rip, cs, rflags, rsp, ss
        const uint8 iretq[] = { 0x48, 0xCF };
        const uint64 frame = memory.m_base + 0xC00;
        const uint64 values[] = { 0x4000, 0x18, 0x246, memory.m_base + 0x700, 0x8 };
        for (uint i = 0; i < 5; i++)
        {
            uint8 bytes[8];
            for (uint k = 0; k < 8; k++)
                bytes[k] = (uint8)(values[i] >> (8 * k));
            memory.write(frame + 8 * i, bytes, 8);
        }
        interpreter.setRegister(AMD64_REG_RSP, SemanticValue(frame));
        interpreter.setRegister(IA32_REG_ZF, SemanticValue(0));
        check("iretq exit", liftAndRun(iretq, 2, start, interpreter, lifter),
              SemanticInterpreter::EXIT_BRANCH);
        check("iretq rip", get(interpreter, AMD64_REG_RIP), 0x4000);
        check("iretq rsp", get(interpreter, AMD64_REG_RSP), memory.m_base + 0x700);
        check("iretq ss", get(interpreter, IA32_REG_SS), 0x8);
        check("iretq zf", get(interpreter, IA32_REG_ZF), 1);
    }

    virtual void test()
    {
        testArchitecture();
        testAliasing();
        testRunBlocks();
        testLifter();
        testSegments();
        cout << "AMD64 semantics tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestAMD64Semantics g_globalTestAMD64Semantics;
