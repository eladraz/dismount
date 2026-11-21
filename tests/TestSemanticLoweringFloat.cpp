/*
 * TestSemanticLoweringFloat.cpp
 *
 * Tests of the lowering of the scalar floating point operations onto AMD64
 * (AMD64LoweringFloat.cpp). The guest is RISC-V RV64: its floating point
 * registers f0-f31 are the inputs and outputs of the synthetic blocks.
 *
 *   Part F1  the legalizer accepts the operations, the lowering assembles
 *            them, and the operations it refuses are refused (fused
 *            multiply-add, minimum, non default behaviors, round to odd).
 *   Part F2  (x86-64 Linux hosts) each block is lowered, assembled and run
 *            natively on the same random and edge inputs as the
 *            SemanticInterpreter. The edge inputs are the signed zeros,
 *            the infinities, quiet and signaling NaNs (with both signs and
 *            payloads), the denormals and the extremes of f32 and f64. The
 *            results are compared bit for bit, except that two NaNs compare
 *            equal (sign and payload are the documented deviation of
 *            AMD64LoweringFloat.cpp).
 *
 * The harness is a copy of the one of TestSemanticLowering.cpp (its helpers
 * are not exported), kept in an anonymous namespace.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#define LOWERING_FLOAT_NATIVE 1
#endif
#include <stdio.h>
#include <string.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "xStl/exceptions.h"
#include "xStl/stream/ioStream.h"
#include "xStl/data/datastream.h"
#include "xStl/data/hash.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/../../tests/tests.h"

#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/semantics/SemanticLowering.h"
#include "dismount/semantics/SemanticInterpreter.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64Architecture.h"

using namespace riscvdis;

namespace {

// ----------------------------------------------------------------------
// The harness
// ----------------------------------------------------------------------

// The guest: RV64 with its floating point registers
ArchitectureDescriptionPtr getGuest()
{
    return ArchitectureDescriptionPtr(new RiscvArchitecture(
                RiscvInstructionSet::RISCV_RV64, 128, false));
}

// The target: the AMD64 processor
ArchitectureDescriptionPtr getTarget()
{
    return ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);
}

// The guest state of the tests. The integer registers are at 0 (x10 is at
// 0x50), the program counter at 0x100 and the exit reason at 0x110 (as in
// TestSemanticLowering.cpp), the floating point registers f0-f31 at 0x2000,
// the temporaries (16 bytes each) at 0x400
class FloatRegisterMap : public SemanticRegisterMap {
public:
    virtual bool getTargetRegister(uint, uint&) const { return false; }
    virtual uint getStateOffset(uint guestRegister) const
    {
        if ((guestRegister >= RISCV_REG_X0) && (guestRegister <= RISCV_REG_X31))
            return (guestRegister - RISCV_REG_X0) * 8;
        if ((guestRegister >= RISCV_REG_F0) && (guestRegister <= RISCV_REG_F31))
            return 0x2000 + (guestRegister - RISCV_REG_F0) * 8;
        return 0x3000 + guestRegister * 8;
    }
    virtual uint getStateRegister() const { return ia32dis::AMD64_REG_R15; }
    virtual uint getTemporaryOffset(uint temporary) const { return 0x400 + temporary * 16; }
    virtual uint getMemoryBaseRegister() const { return ia32dis::AMD64_REG_R14; }
    virtual uint getProgramCounterOffset() const { return 0x100; }
    virtual uint getExitReasonOffset() const { return 0x110; }
};

// The stack of the first pass binary, which the lowering doesn't use
class FloatLoweringStack : public StackInterface {
public:
    FloatLoweringStack(FirstPassBinaryPtr& binary) :
        StackInterface(*binary, true, 4) {}
    virtual uint getStackType() { return 4; }
    virtual uint getRegisterSize() { return 4; }
    virtual RegisterAllocationTable& getRegistersTable() { return m_registers; }
    virtual const RegisterAllocationTable& getRegistersTable() const { return m_registers; }
    virtual StackLocation replaceRegisterToStackVariable(StackLocation source)
    {
        return source;
    }
private:
    RegisterAllocationTable m_registers;
};

// The guest address of the blocks
const uint64 BLOCK_ADDRESS = 0x80001000ULL;

// Lower a block into a new first pass binary. The block falls through to
// BLOCK_ADDRESS + 4 (the lowering needs the address after the block)
FirstPassBinaryPtr lowerBlock(SemanticBlock& block)
{
    block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, BLOCK_ADDRESS + 4));
    FirstPassBinaryPtr binary(new FirstPassBinary(
                            OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
    binary->createNewBlockWithoutChange(0,
            StackInterfacePtr(new FloatLoweringStack(binary)));
    binary->changeBasicBlock(0);
    AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                            binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);
    SemanticLoweringPtr lowering = SemanticLoweringFactory::getLowering(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64);
    FloatRegisterMap registerMap;
    lowering->lower(block, registerMap, *assembler);
    return binary;
}


// The integer register which receives the boolean results
const uint X_RESULT = 10;
// The floating point registers: two inputs, and the results
const uint F_IN_A = 1;
const uint F_IN_B = 2;
const uint F_OUT = 10;
const uint F_SELECT = 11;

// The float type of a width
SemanticType floatType(uint bits)
{
    return SemanticType::floating((bits == 64) ? SemanticType::FLOAT_DOUBLE :
                                                 SemanticType::FLOAT_SINGLE);
}

// A floating point register of the guest
SemanticOperand floatRegister(uint index, uint bits)
{
    return SemanticOperand::registerOperand(RISCV_REG_F0 + index, floatType(bits));
}

// A floating point constant (1.5, of the width): the bits of the value
SemanticOperand floatConstant(uint bits)
{
    const int64 pattern = (bits == 64) ? (int64)0x3FF8000000000000ULL : 0x3FC00000;
    return SemanticOperand::immediate(pattern, floatType(bits));
}

// The pseudo random numbers of the native tests (xorshift64, fixed seed)
uint64 gFloatRandom = 0x9E3779B97F4A7C15ULL;
uint64 floatRandom()
{
    gFloatRandom ^= gFloatRandom << 13;
    gFloatRandom ^= gFloatRandom >> 7;
    gFloatRandom ^= gFloatRandom << 17;
    return gFloatRandom;
}

// The memory of the guest: the blocks don't access it
class FloatTestMemory : public SemanticMemory {
public:
    virtual bool read(uint64, uint8*, uint) { return false; }
    virtual bool write(uint64, const uint8*, uint) { return false; }
};

// Return true if a bit pattern of 'bits' is a NaN (class only, the payload
// and the sign are not compared)
bool isNanBits(uint64 value, uint bits)
{
    if (bits == 64)
        return (((value >> 52) & 0x7FF) == 0x7FF) &&
               ((value & (((uint64)1 << 52) - 1)) != 0);
    return (((value >> 23) & 0xFF) == 0xFF) && ((value & 0x7FFFFF) != 0);
}

// The mask of the low 'bits' bits of a value
uint64 bitsMask(uint bits)
{
    return (bits == 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
}

// Return true if the two float results agree on the low 'bits' bits. With
// 'exact' the bits must be equal; otherwise two NaNs agree whatever their
// sign and payload (the x86 default NaN of the arithmetic)
bool sameFloat(uint64 native, uint64 interpreted, uint bits, bool exact)
{
    const uint64 mask = bitsMask(bits);
    native &= mask;
    interpreted &= mask;
    if (!exact && isNanBits(native, bits) && isNanBits(interpreted, bits))
        return true;
    return native == interpreted;
}

// The blocks. Each one reads f1 and f2 (the width of the block) and writes
// f10 (a float) and, for the compares, x10 (the boolean zero extended) and
// f11 (the select of the boolean)
SemanticBlock arithmeticBlock(SemanticOperation::Operation operation,
                              uint bits, uint form)
{
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    const SemanticType type = floatType(bits);
    const SemanticOperand a = floatRegister(F_IN_A, bits);
    const SemanticOperand b = floatRegister(F_IN_B, bits);
    const SemanticOperand c = floatConstant(bits);
    const SemanticOperand out = floatRegister(F_OUT, bits);
    const bool isBinary = (operation == SemanticOperation::OP_FLOAT_ADD) ||
                          (operation == SemanticOperation::OP_FLOAT_SUBTRACT) ||
                          (operation == SemanticOperation::OP_FLOAT_MULTIPLY) ||
                          (operation == SemanticOperation::OP_FLOAT_DIVIDE);
    if (!isBinary)
    {
        // (the unary forms: a register, a constant, a chain)
        if (form == 0)
            block.append(SemanticInstruction(operation, type, out, a));
        else if (form == 1)
            block.append(SemanticInstruction(operation, type, out, c));
        else
        {
            const SemanticOperand t = block.newTemporary(type);
            block.append(SemanticInstruction(operation, type, t, a));
            block.append(SemanticInstruction(operation, type, out, t));
        }
        return block;
    }
    if (form == 0)
        block.append(SemanticInstruction(operation, type, out, a, b));
    else if (form == 1)
    {
        // a chain through a temporary: the temporary is a slot of the state
        const SemanticOperand t = block.newTemporary(type);
        block.append(SemanticInstruction(operation, type, t, a, b));
        block.append(SemanticInstruction(operation, type, out, t, b));
    } else if (form == 2)
        block.append(SemanticInstruction(operation, type, out, c, b));
    else
        block.append(SemanticInstruction(operation, type, out, a, c));
    return block;
}

// d (x10) = the boolean of the compare, f11 = the select of 'a' or 'b' by it
SemanticBlock compareBlock(SemanticOperation::Condition condition,
                           SemanticOperation::Behavior behavior,
                           uint bits, uint form)
{
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    const SemanticType type = floatType(bits);
    const SemanticType boolean = SemanticType::integer(1);
    const SemanticType wide = SemanticType::integer(64);
    const SemanticOperand a = floatRegister(F_IN_A, bits);
    const SemanticOperand b = floatRegister(F_IN_B, bits);
    const SemanticOperand flag = block.newTemporary(boolean);
    SemanticInstruction compare(SemanticOperation::OP_COMPARE, type, flag, a,
                                (form == 0) ? b : floatConstant(bits));
    compare.setCondition(condition);
    compare.setBehavior(behavior);
    block.append(compare);
    // x10 = the boolean, zero extended
    const SemanticOperand zero = block.newTemporary(wide);
    block.append(SemanticInstruction(SemanticOperation::OP_ZERO_EXTEND, wide, zero, flag));
    block.append(SemanticInstruction(SemanticOperation::OP_MOVE, wide,
                 SemanticOperand::registerOperand(RISCV_REG_X0 + X_RESULT, wide), zero));
    // f11 = flag ? a : b
    block.append(SemanticInstruction(SemanticOperation::OP_SELECT, type,
                 floatRegister(F_SELECT, bits), flag, a, b));
    return block;
}

// f11 = the select of a constant condition
SemanticBlock constantSelectBlock(bool isTrue, uint bits)
{
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    const SemanticType type = floatType(bits);
    block.append(SemanticInstruction(SemanticOperation::OP_SELECT, type,
                 floatRegister(F_SELECT, bits),
                 SemanticOperand::immediate(isTrue ? 1 : 0, SemanticType::integer(1)),
                 floatRegister(F_IN_A, bits), floatRegister(F_IN_B, bits)));
    return block;
}

// The select of two values of 'type' (not a scalar f32/f64: a half precision
// float here) by a scalar compare of f1 and f2, into f11
SemanticBlock typedSelectBlock(const SemanticType& type)
{
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    const SemanticOperand flag = block.newTemporary(SemanticType::integer(1));
    SemanticInstruction compare(SemanticOperation::OP_COMPARE, floatType(64), flag,
                                floatRegister(F_IN_A, 64), floatRegister(F_IN_B, 64));
    compare.setCondition(SemanticOperation::COND_FLOAT_LESS);
    block.append(compare);
    block.append(SemanticInstruction(SemanticOperation::OP_SELECT, type,
                 SemanticOperand::registerOperand(RISCV_REG_F0 + F_SELECT, type), flag,
                 SemanticOperand::registerOperand(RISCV_REG_F0 + F_IN_A, type),
                 SemanticOperand::registerOperand(RISCV_REG_F0 + F_IN_B, type)));
    return block;
}

// The integer register which is the input of the integer conversions (x1)
const uint X_IN = 1;

// An integer register of the guest, of 'bits' (its low bits)
SemanticOperand integerRegister(uint index, uint bits)
{
    return SemanticOperand::registerOperand(RISCV_REG_X0 + index,
                                            SemanticType::integer(bits));
}

// The fixed point count operand (source 1), as the A64 lifter writes it
SemanticOperand fractionOperand(uint fraction)
{
    return SemanticOperand::immediate((int64)fraction, SemanticType::integer(32));
}

// f10 = the integer of 'sourceBits' in x1, converted to a float of 'bits'
// with 'fraction' fixed point bits (0: none)
// (rounded in 'mode'; dynamic: the environment's nearest even)
SemanticBlock integerToFloatBlock(bool isSigned, uint sourceBits, uint bits,
                                  uint fraction = 0,
                                  SemanticOperation::RoundingMode mode =
                                        SemanticOperation::ROUND_DYNAMIC)
{
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    const SemanticOperation::Operation operation = isSigned ?
                SemanticOperation::OP_SIGNED_TO_FLOAT :
                SemanticOperation::OP_UNSIGNED_TO_FLOAT;
    SemanticInstruction convert = (fraction == 0) ?
        SemanticInstruction(operation, floatType(bits),
                     floatRegister(F_OUT, bits), integerRegister(X_IN, sourceBits)) :
        SemanticInstruction(operation, floatType(bits),
                     floatRegister(F_OUT, bits), integerRegister(X_IN, sourceBits),
                     fractionOperand(fraction));
    convert.setRoundingMode(mode);
    block.append(convert);
    return block;
}

// x10 = the integer of 'bits' (32 or 64) converted from f1 of 'sourceBits'
// with 'behavior', in 'mode' (toward zero by default). A 32 bit result is zero
// extended. A block of another width is built to be refused by the lowering
SemanticBlock floatToIntegerBlock(bool isSigned,
                                  SemanticOperation::Behavior behavior,
                                  uint sourceBits, uint bits, uint fraction = 0,
                                  SemanticOperation::RoundingMode mode =
                                        SemanticOperation::ROUND_TOWARD_ZERO)
{
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    const SemanticType result = SemanticType::integer(bits);
    const SemanticOperand wide = integerRegister(X_RESULT, 64);
    const SemanticOperand target = (bits == 64) ? wide : block.newTemporary(result);
    const SemanticOperation::Operation operation = isSigned ?
                SemanticOperation::OP_FLOAT_TO_SIGNED :
                SemanticOperation::OP_FLOAT_TO_UNSIGNED;
    SemanticInstruction convert = (fraction == 0) ?
        SemanticInstruction(operation, result, target, floatRegister(F_IN_A, sourceBits)) :
        SemanticInstruction(operation, result, target, floatRegister(F_IN_A, sourceBits),
                            fractionOperand(fraction));
    convert.setRoundingMode(mode);
    convert.setBehavior(behavior);
    block.append(convert);
    if (bits == 32)
        block.append(SemanticInstruction(SemanticOperation::OP_ZERO_EXTEND,
                     SemanticType::integer(64), wide, target));
    return block;
}

// x10 = the fclass mask of f1 of 'bits' (an integer of 32 bits, zero
// extended to 64 as the RISC-V fclass writes it)
SemanticBlock classifyBlock(uint bits)
{
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    const SemanticOperand mask = block.newTemporary(SemanticType::integer(32));
    block.append(SemanticInstruction(SemanticOperation::OP_FLOAT_CLASSIFY,
                 SemanticType::integer(32), mask, floatRegister(F_IN_A, bits)));
    block.append(SemanticInstruction(SemanticOperation::OP_ZERO_EXTEND,
                 SemanticType::integer(64), integerRegister(X_RESULT, 64), mask));
    return block;
}

// f10 = f1 converted from f(sourceBits) to f(bits)
SemanticBlock floatConvertBlock(uint sourceBits, uint bits)
{
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    block.append(SemanticInstruction(SemanticOperation::OP_FLOAT_CONVERT,
                 floatType(bits), floatRegister(F_OUT, bits),
                 floatRegister(F_IN_A, sourceBits)));
    return block;
}

// f10 = f1 rounded to an integral value in 'mode' (OP_FLOAT_ROUND: the frint
// of A64, and the modes of the RISC-V float to integer conversions)
SemanticBlock floatRoundBlock(SemanticOperation::RoundingMode mode, uint bits)
{
    SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    SemanticInstruction round(SemanticOperation::OP_FLOAT_ROUND,
                 floatType(bits), floatRegister(F_OUT, bits), floatRegister(F_IN_A, bits));
    round.setRoundingMode(mode);
    block.append(round);
    return block;
}

// The A64 word of the Lua guest: ucvtf d0, x9, #fbits (math.random's I2d)
// with 53 fraction bits, lifted at ARM64_PC
const uint32 ARM64_UCVTF_WORD = 0x9e432d20;
const uint64 ARM64_PC = 0x1000;

// Decode and lift an A64 word into 'block', with the guest 'architecture'
bool liftArm64Word(uint32 word, const ArchitectureDescriptionPtr& architecture,
                   SemanticBlock& block)
{
    ArmSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE,
                                     architecture);
    ArmOpcodePtr decoded = ArmStreamDisassembler::decode(
            ArmInstructionSet::ARM_A64, word, 4, ARM64_PC,
            ArmFeatureSet::all());
    if (decoded.getPointer() == NULL)
        return false;
    OpcodePtr opcode((Opcode*)decoded.getPointer(), SMARTPTR_DESTRUCT_NONE);
    if (!lifter.lift(opcode, block))
        return false;
    block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ARM64_PC + 4));
    return true;
}

// The state of the A64 guest of the native test: x0-x30 at 8 bytes, the
// vector registers z0-z31 at 16 bytes from 0x2000 (d0 is the low half of z0)
class Arm64FixedRegisterMap : public SemanticRegisterMap {
public:
    virtual bool getTargetRegister(uint, uint&) const { return false; }
    virtual uint getStateOffset(uint guestRegister) const
    {
        if ((guestRegister >= armdis::ARM64_REG_X0) &&
            (guestRegister <= armdis::ARM64_REG_X30))
            return (guestRegister - armdis::ARM64_REG_X0) * 8;
        if ((guestRegister >= armdis::ARM64_REG_Z0) &&
            (guestRegister <= armdis::ARM64_REG_Z31))
            return 0x2000 + (guestRegister - armdis::ARM64_REG_Z0) * 16;
        if ((guestRegister >= armdis::ARM64_REG_D0) &&
            (guestRegister <= armdis::ARM64_REG_D31))
            return 0x2000 + (guestRegister - armdis::ARM64_REG_D0) * 16;
        return 0x3000 + guestRegister * 8;
    }
    virtual uint getStateRegister() const { return ia32dis::AMD64_REG_R15; }
    virtual uint getTemporaryOffset(uint temporary) const { return 0x400 + temporary * 16; }
    virtual uint getMemoryBaseRegister() const { return ia32dis::AMD64_REG_R14; }
    virtual uint getProgramCounterOffset() const { return 0x100; }
    virtual uint getExitReasonOffset() const { return 0x110; }
};

// The behaviors of the float to integer conversions
const SemanticOperation::Behavior gConvertBehaviors[] = {
    SemanticOperation::BEHAVIOR_CONVERT_INDEFINITE,
    SemanticOperation::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO,
    SemanticOperation::BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM };
const uint CONVERT_BEHAVIORS_COUNT = sizeof(gConvertBehaviors) / sizeof(gConvertBehaviors[0]);

// The operations of the arithmetic: the forms of each one
const SemanticOperation::Operation gArithmetic[] = {
    SemanticOperation::OP_FLOAT_ADD,
    SemanticOperation::OP_FLOAT_SUBTRACT,
    SemanticOperation::OP_FLOAT_MULTIPLY,
    SemanticOperation::OP_FLOAT_DIVIDE,
    SemanticOperation::OP_FLOAT_SQUARE_ROOT,
    SemanticOperation::OP_FLOAT_NEGATE,
    SemanticOperation::OP_FLOAT_ABSOLUTE };
const uint ARITHMETIC_COUNT = sizeof(gArithmetic) / sizeof(gArithmetic[0]);

// The float conditions of OP_COMPARE (the ones which are not ALWAYS/NEVER)
const SemanticOperation::Condition gFloatConditions[] = {
    SemanticOperation::COND_FLOAT_ORDERED,
    SemanticOperation::COND_FLOAT_UNORDERED,
    SemanticOperation::COND_FLOAT_EQUAL,
    SemanticOperation::COND_FLOAT_NOT_EQUAL,
    SemanticOperation::COND_FLOAT_LESS,
    SemanticOperation::COND_FLOAT_LESS_EQUAL,
    SemanticOperation::COND_FLOAT_GREATER,
    SemanticOperation::COND_FLOAT_GREATER_EQUAL,
    SemanticOperation::COND_FLOAT_UNORDERED_OR_EQUAL,
    SemanticOperation::COND_FLOAT_UNORDERED_OR_LESS,
    SemanticOperation::COND_FLOAT_UNORDERED_OR_LESS_EQUAL };
const uint CONDITIONS_COUNT = sizeof(gFloatConditions) / sizeof(gFloatConditions[0]);

// The edge inputs of each width: signed zeros, infinities, NaNs (quiet and
// signaling, both signs, payloads), denormals, the extremes and some
// ordinary values
const uint64 gEdges64[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL, 0x3FF0000000000000ULL,
    0xBFF0000000000000ULL, 0x4000000000000000ULL, 0x4008000000000000ULL,
    0x3FE0000000000000ULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
    0x7FF8000000000000ULL, 0xFFF8000000000000ULL, 0x7FF0000000000001ULL,
    0xFFF0000000000001ULL, 0x7FF800000000DEADULL, 0x0000000000000001ULL,
    0x8000000000000001ULL, 0x000FFFFFFFFFFFFFULL, 0x0010000000000000ULL,
    0x7FEFFFFFFFFFFFFFULL, 0x3FF0000000000001ULL, 0x7FE0000000000000ULL,
    0x0008000000000000ULL, 0x3CB0000000000000ULL, 0x4330000000000000ULL };
const uint64 gEdges32[] = {
    0x00000000ULL, 0x80000000ULL, 0x3F800000ULL, 0xBF800000ULL,
    0x40000000ULL, 0x40400000ULL, 0x3F000000ULL, 0x7F800000ULL,
    0xFF800000ULL, 0x7FC00000ULL, 0xFFC00000ULL, 0x7F800001ULL,
    0xFF800001ULL, 0x7FC0DEADULL, 0x00000001ULL, 0x80000001ULL,
    0x007FFFFFULL, 0x00800000ULL, 0x7F7FFFFFULL, 0x3F800001ULL,
    0x7E800000ULL, 0x00400000ULL, 0x34000000ULL, 0x4B000000ULL };

// The inputs of the round to an integral value: the halfway cases of every
// mode (x.5 and the largest value below one half, both signs), the values
// around 2^(p-1) where the fraction runs out, and some ordinary values. The
// infinities, the NaNs and the denormals are bit patterns (gRoundBits64/32)
const double gRoundDoubles[] = {
    0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 3.5, -3.5, 4.5, -4.5, 0.25,
    -0.75, 0.49999999999999994, -0.49999999999999994, 0.7, -0.7, 2.7, -2.7,
    1.0, -1.0, 4503599627370495.5, -4503599627370495.5, 4503599627370496.0,
    -4503599627370496.0, 9007199254740992.0, -9007199254740992.0,
    9223372036854775808.0, -9223372036854775808.0, 1e300, -1e300,
    1e-10, -1e-10, 2.2250738585072014e-308 };
const uint64 gRoundBits64[] = {
    0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 0x7FF8000000000000ULL,
    0xFFF8000000000000ULL, 0x7FF0000000000001ULL, 0x0000000000000001ULL,
    0x8000000000000001ULL, 0x0010000000000000ULL };
const float gRoundSingles[] = {
    0.0f, -0.0f, 0.5f, -0.5f, 1.5f, -1.5f, 2.5f, -2.5f, 0.49999997f,
    -0.49999997f, 0.7f, -2.7f, 1.0f, -1.0f, 8388607.5f, -8388607.5f,
    8388608.0f, -8388608.0f, 16777216.0f, -16777216.0f, 3.4028235e38f,
    -3.4028235e38f, 1e-10f, -1e-10f };
const uint64 gRoundBits32[] = {
    0x7F800000ULL, 0xFF800000ULL, 0x7FC00000ULL, 0xFFC00000ULL,
    0x7F800001ULL, 0x00000001ULL, 0x80000001ULL, 0x00800000ULL };

// The rounding modes of OP_FLOAT_ROUND and of the float to integer conversions
// (the nearest away is the frinta and fcvta[ns] mode; dynamic is nearest even)
const SemanticOperation::RoundingMode gRoundModes[] = {
    SemanticOperation::ROUND_DYNAMIC,
    SemanticOperation::ROUND_NEAREST_EVEN,
    SemanticOperation::ROUND_NEAREST_AWAY,
    SemanticOperation::ROUND_TOWARD_NEGATIVE,
    SemanticOperation::ROUND_TOWARD_POSITIVE,
    SemanticOperation::ROUND_TOWARD_ZERO };
const uint ROUND_MODES_COUNT = sizeof(gRoundModes) / sizeof(gRoundModes[0]);

// The number of random trials of each block, after the edge pairs
const uint RANDOM_TRIALS = 200;


#ifdef LOWERING_FLOAT_NATIVE
// ----------------------------------------------------------------------
// The native run: the lowered block between an entry and an exit stub
// ----------------------------------------------------------------------

typedef void (*NativeFunction)(void* state, void* memoryBias);

// The state index of a floating point register, and of x10 and the pc
const uint FLOAT_STATE_INDEX = 0x2000 / 8;
const uint X10_STATE_INDEX = 0x50 / 8;
const uint PC_STATE_INDEX = 0x100 / 8;

// Assemble a text into the binary of the assembler, false on an error
bool assembleText(AssemblerInterfacePtr& assembler, const char* text)
{
    cStringerStreamPtr stream = assembler->getAssembler();
    *stream << cString(text);
    IA32AssemblerStream* assembly =
                dynamic_cast<IA32AssemblerStream*>(stream.getPointer());
    if (assembly == NULL)
        return false;
    XSTL_TRY
    {
        assembly->assemble();
    }
    XSTL_CATCH(cException&)
    {
        return false;
    }
    return true;
}

// Lower a block with the entry and exit stubs around it, resolve the labels
// (a B_ label stores its address as the next program counter and exits), and
// return the code
bool buildNative(const SemanticBlock& block, const SemanticRegisterMap& registerMap,
                 cBuffer& code)
{
    FirstPassBinaryPtr binary(new FirstPassBinary(
                        OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
    binary->createNewBlockWithoutChange(0,
            StackInterfacePtr(new FloatLoweringStack(binary)));
    binary->changeBasicBlock(0);
    AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                        binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);
    if (!assembleText(assembler,
        "push rbx\npush rbp\npush r12\npush r13\npush r14\npush r15\n"
        "mov r15, rdi\nmov r14, rsi\n"))
        return false;

    SemanticLoweringPtr lowering = SemanticLoweringFactory::getLowering(
                                OpcodeSubsystems::DISASSEMBLER_AMD_64);
    lowering->lower(block, registerMap, *assembler);

    // A stub for each label the block refers to, then the exit
    cList<cString> names;
    cList<uint> positions;
    const BinaryDependencies::DependencyObjectList& list =
                                binary->getCurrentDependecies().getList();
    cList<cString> wanted;
    for (BinaryDependencies::DependencyObjectList::iterator i = list.begin();
         i != list.end(); i++)
    {
        bool isKnown = false;
        for (cList<cString>::iterator w = wanted.begin(); w != wanted.end(); w++)
            isKnown = isKnown || (*w == (*i).m_name);
        if (!isKnown && !((*i).m_name == AMD64Lowering::getExitLabel()))
            wanted.append((*i).m_name);
    }
    for (cList<cString>::iterator w = wanted.begin(); w != wanted.end(); w++)
    {
        names.append(*w);
        positions.append(binary->getCurrentBlockData().getSize());
        cSArray<char> label = (*w).getASCIIstring();
        // (the name is B_<hex>)
        char text[300];
        snprintf(text, sizeof(text),
                 "mov rax, 0x%s\nmov qword [r15+0x100], rax\n"
                 "mov qword [r15+0x110], 0x1000\njmp __reforge_exit\n",
                 label.getBuffer() + 2);
        if (!assembleText(assembler, text))
            return false;
    }
    names.append(AMD64Lowering::getExitLabel());
    positions.append(binary->getCurrentBlockData().getSize());
    if (!assembleText(assembler,
        "pop r15\npop r14\npop r13\npop r12\npop rbp\npop rbx\nret\n"))
        return false;

    // Resolve the relative 32 bit dependencies
    const cBuffer& data = binary->getCurrentBlockData();
    code = cBuffer(data.getSize());
    for (uint i = 0; i < data.getSize(); i++)
        code[i] = data[i];
    for (BinaryDependencies::DependencyObjectList::iterator i = list.begin();
         i != list.end(); i++)
    {
        bool isFound = false;
        cList<uint>::iterator p = positions.begin();
        for (cList<cString>::iterator n = names.begin(); n != names.end(); n++, p++)
        {
            if (!(*n == (*i).m_name))
                continue;
            const int32 relative = (int32)((*p) - ((*i).m_position + 4));
            for (uint b = 0; b < 4; b++)
                code[(*i).m_position + b] = (uint8)(relative >> (8 * b));
            isFound = true;
        }
        if (!isFound)
            return false;
    }
    return true;
}

// The outputs a block writes, and how they are compared
struct FloatOutputs {
    // f10 is written (the float result)
    bool m_result;
    // f11 is written (the select of the boolean)
    bool m_select;
    // x10 is written (the boolean, zero extended)
    bool m_boolean;
    // f10 and f11 are determined bit for bit (no NaN class rule)
    bool m_exact;
};

// The value of every state slot before a run. It is not zero, so a slot which
// the code doesn't write fully is seen: the upper bytes of a float and of a
// boolean (one byte) keep this pattern
const uint64 STATE_FILL = 0xA5A5A5A5A5A5A5A5ULL;

// Compare one float output of the native code and the interpreter. Add one
// to the differences when they disagree, or when the code wrote the bits
// above 'bits' of the output slot
uint compareFloatOutput(const char* name, uint bits, uint index,
                        uint64 nativeValue, uint64 interpretedValue,
                        bool exact, uint64 a, uint64 b)
{
    uint differences = 0;
    if (bits < 64 && ((nativeValue & ~bitsMask(bits)) !=
                      (STATE_FILL & ~bitsMask(bits))))
    {
        fprintf(stderr, "TestSemanticLoweringFloat: %s (%llu bits): f%llu: the "
                "upper bits of the slot were written: %016llx\n", name,
                (unsigned long long)bits, (unsigned long long)index,
                (unsigned long long)nativeValue);
        differences++;
    }
    if (!sameFloat(nativeValue, interpretedValue, bits, exact))
    {
        fprintf(stderr, "TestSemanticLoweringFloat: %s (%llu bits): f%llu native "
                "%016llx interpreter %016llx for inputs %016llx %016llx\n",
                name, (unsigned long long)bits, (unsigned long long)index,
                (unsigned long long)nativeValue,
                (unsigned long long)interpretedValue,
                (unsigned long long)a, (unsigned long long)b);
        differences++;
    }
    return differences;
}

// Run the block natively and in the SemanticInterpreter on the pairs of
// edge inputs and then on random inputs of 'bits' (the width of the values:
// 16, 32 or 64), and compare the outputs. Add the comparisons to 'cases'
// and return the number of differences
uint runFloatBlock(const char* name, SemanticBlock& block, uint bits,
                   FloatOutputs outputs, uint& cases)
{
    block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, BLOCK_ADDRESS + 4));
    cBuffer code;
    if (!buildNative(block, FloatRegisterMap(), code))
    {
        fprintf(stderr, "TestSemanticLoweringFloat: %s: the native code is "
                "not assembled\n", name);
        return 1;
    }
    void* memory = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
        return 0;     // (a host without executable memory: nothing to run)
    for (uint i = 0; i < code.getSize(); i++)
        ((uint8*)memory)[i] = code[i];
    NativeFunction function = reinterpret_cast<NativeFunction>(memory);

    // The edge inputs of the width: the 32 bit ones for the half precision
    const bool isWide = (bits == 64);
    const uint64* edges = isWide ? gEdges64 : gEdges32;
    const uint edgeCount = isWide ?
                (uint)(sizeof(gEdges64) / sizeof(gEdges64[0])) :
                (uint)(sizeof(gEdges32) / sizeof(gEdges32[0]));
    const uint64 mask = bitsMask(bits);
    const uint trials = edgeCount * edgeCount + RANDOM_TRIALS;
    ArchitectureDescriptionPtr guest = getGuest();
    uint differences = 0;
    for (uint trial = 0; (trial < trials) && (differences == 0); trial++)
    {
        uint64 a, b;
        if (trial < edgeCount * edgeCount)
        {
            a = edges[trial / edgeCount] & mask;
            b = edges[trial % edgeCount] & mask;
        } else
        {
            a = floatRandom() & mask;
            b = floatRandom() & mask;
        }

        // The native state: every slot is dirty, then the inputs, then the
        // code runs
        static uint64 state[0x800];
        for (uint i = 0; i < 0x800; i++)
            state[i] = STATE_FILL;
        state[FLOAT_STATE_INDEX + F_IN_A] = a;
        state[FLOAT_STATE_INDEX + F_IN_B] = b;
        function(state, NULL);

        // The interpreter on the same inputs
        FloatTestMemory guestMemory;
        SemanticInterpreter interpreter(guest, guestMemory);
        interpreter.setRegister(RISCV_REG_F0 + F_IN_A, SemanticValue(a));
        interpreter.setRegister(RISCV_REG_F0 + F_IN_B, SemanticValue(b));
        const SemanticInterpreter::ExitReason reason = interpreter.run(block);
        if (reason != SemanticInterpreter::EXIT_FALL_THROUGH)
        {
            fprintf(stderr, "TestSemanticLoweringFloat: %s: the interpreter "
                    "does not run the block\n", name);
            differences++;
            break;
        }
        cases++;

        if (outputs.m_result)
        {
            differences += compareFloatOutput(name, bits, F_OUT,
                    state[FLOAT_STATE_INDEX + F_OUT],
                    interpreter.getRegister(RISCV_REG_F0 + F_OUT).getWord(0),
                    outputs.m_exact, a, b);
        }
        if (outputs.m_select)
        {
            differences += compareFloatOutput(name, bits, F_SELECT,
                    state[FLOAT_STATE_INDEX + F_SELECT],
                    interpreter.getRegister(RISCV_REG_F0 + F_SELECT).getWord(0),
                    outputs.m_exact, a, b);
        }
        if (outputs.m_boolean)
        {
            const uint64 nativeFlag = state[X10_STATE_INDEX];
            const uint64 interpretedFlag =
                    interpreter.getRegister(RISCV_REG_X0 + X_RESULT).getWord(0);
            if (nativeFlag != interpretedFlag)
            {
                fprintf(stderr, "TestSemanticLoweringFloat: %s (%llu bits): "
                        "x10 native %llu interpreter %llu\n", name,
                        (unsigned long long)bits,
                        (unsigned long long)nativeFlag,
                        (unsigned long long)interpretedFlag);
                differences++;
            }
        }
        if (state[PC_STATE_INDEX] != interpreter.getNextAddress())
        {
            fprintf(stderr, "TestSemanticLoweringFloat: %s: the next address "
                    "differs\n", name);
            differences++;
        }
    }
    munmap(memory, 0x10000);
    return differences;
}

// The shape of a conversion block: the input is an integer of 'inputBits'
// in x1 or a float in f1, the output is an integer in x10 (zero extended to
// 64 bits) or a float of 'outputBits' in f10. 'exact': the float output is
// determined bit for bit (no NaN class rule)
struct ConvertShape {
    bool m_inputInteger;
    uint m_inputBits;
    bool m_outputInteger;
    uint m_outputBits;
    bool m_exact;
};

// The integers of the integer conversions: the rounding ties of 32 and 64
// bits (round to nearest even at 24, 53 and 64 significant bits), the signs,
// the extremes of each width and the ordinary values
const uint64 gIntegerEdges[] = {
    0x0000000000000000ULL, 0x0000000000000001ULL, 0x0000000000000002ULL,
    0x0000000000000003ULL, 0x000000000000007FULL, 0x0000000000000080ULL,
    0x00000000000000FFULL, 0x0000000000007FFFULL, 0x0000000000008000ULL,
    0x000000000000FFFFULL, 0x000000007FFFFFFFULL, 0x0000000080000000ULL,
    0x00000000FFFFFFFFULL, 0x0000000080000001ULL, 0x0000000001000001ULL,
    0x0000000001000003ULL, 0x0000000000FFFFFFULL, 0x000000007FFFFF80ULL,
    0x0000000100000001ULL, 0xFFFFFFFF80000000ULL, 0x0020000000000001ULL,
    0x0020000000000003ULL, 0x8000000000000000ULL, 0x8000000000000001ULL,
    0x7FFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL,
    0xFFFFFFFFFFFFFFFEULL, 0x8000000000000400ULL, 0x8000000000000C00ULL,
    0x8000000000000800ULL, 0x7FFFFFFFFFFFFC00ULL, 0x7FFFFFFFFFFFF800ULL,
    0x123456789ABCDEF0ULL, 0xFEDCBA9876543211ULL, 0x3FFFFFFFFFFFFF01ULL };

// The float inputs of the float to integer conversions, as doubles: the
// bounds of 32 and 64 bit integers (2^31, 2^32, 2^63, 2^64 and their
// neighbours), the halves, the signs and the extremes
const double gDoubleEdges[] = {
    0.0, -0.0, 0.5, -0.5, 0.9999999999999999, -0.9999999999999999, 1.0, -1.0,
    1.5, -1.5, 2.5, -2.5, 2147483647.0, 2147483647.5, 2147483648.0,
    2147483648.5, -2147483648.0, -2147483648.5, -2147483649.0, 4294967295.0,
    4294967295.5, 4294967296.0, -4294967296.0, 9223372036854774784.0,
    9223372036854775808.0, 9223372036854777856.0, -9223372036854775808.0,
    -9223372036854777856.0, -9223372036854776832.0, 18446744073709549568.0,
    18446744073709551616.0, -18446744073709551616.0, 1e19, -1e19, 1e300,
    -1e300, 1.7976931348623157e308, -1.7976931348623157e308,
    2.2250738585072014e-308, 4.9e-324, -4.9e-324 };

// The float inputs of the f32 conversions: the bounds of the 32 and 64 bit
// integers in single precision, and the same extremes
const float gSingleEdges[] = {
    0.0f, -0.0f, 0.5f, -0.5f, 0.99999994f, 1.0f, -1.0f, 2147483520.0f,
    2147483648.0f, -2147483648.0f, -2147483904.0f, 4294967040.0f,
    4294967296.0f, 9223371487098961920.0f, 9223372036854775808.0f,
    -9223372036854775808.0f, -9223373136366403584.0f, 18446742974197923840.0f,
    18446744073709551616.0f, 1e30f, -1e30f, 3.4028235e38f, -3.4028235e38f,
    1.4e-45f, -1.4e-45f };

// The bit image of a double, or of a float (as a single) of 'bits'
uint64 patternOf(double value, uint bits)
{
    if (bits == 64)
    {
        union { double d; uint64 u; } image;
        image.d = value;
        return image.u;
    }
    union { float f; uint32 u; } image;
    image.f = (float)value;
    return (uint64)image.u;
}

// Run a conversion block natively and in the SemanticInterpreter on the
// edge inputs then on random ones (of the width of the input), and compare
// the output. Add the comparisons to 'cases' and return the differences
uint runConvertBlock(const char* name, SemanticBlock& block,
                     const ConvertShape& shape, const uint64* edges,
                     uint edgeCount, uint& cases)
{
    block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, BLOCK_ADDRESS + 4));
    cBuffer code;
    if (!buildNative(block, FloatRegisterMap(), code))
    {
        fprintf(stderr, "TestSemanticLoweringFloat: %s: the native code is "
                "not assembled\n", name);
        return 1;
    }
    void* memory = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
        return 0;     // (a host without executable memory: nothing to run)
    for (uint i = 0; i < code.getSize(); i++)
        ((uint8*)memory)[i] = code[i];
    NativeFunction function = reinterpret_cast<NativeFunction>(memory);

    const uint64 inputMask = bitsMask(shape.m_inputBits);
    const uint trials = edgeCount + RANDOM_TRIALS;
    ArchitectureDescriptionPtr guest = getGuest();
    uint differences = 0;
    for (uint trial = 0; (trial < trials) && (differences == 0); trial++)
    {
        const uint64 a = ((trial < edgeCount) ? edges[trial] : floatRandom()) &
                         inputMask;

        // The native state: every slot is dirty, then the input, then the code
        static uint64 state[0x800];
        for (uint i = 0; i < 0x800; i++)
            state[i] = STATE_FILL;
        if (shape.m_inputInteger)
            state[X_IN] = a;
        else
            state[FLOAT_STATE_INDEX + F_IN_A] = a;
        function(state, NULL);

        // The interpreter on the same input
        FloatTestMemory guestMemory;
        SemanticInterpreter interpreter(guest, guestMemory);
        if (shape.m_inputInteger)
            interpreter.setRegister(RISCV_REG_X0 + X_IN, SemanticValue(a));
        else
            interpreter.setRegister(RISCV_REG_F0 + F_IN_A, SemanticValue(a));
        const SemanticInterpreter::ExitReason reason = interpreter.run(block);
        if (reason != SemanticInterpreter::EXIT_FALL_THROUGH)
        {
            fprintf(stderr, "TestSemanticLoweringFloat: %s: the interpreter "
                    "does not run the block\n", name);
            differences++;
            break;
        }
        cases++;

        if (shape.m_outputInteger)
        {
            const uint64 nativeValue = state[X10_STATE_INDEX];
            const uint64 interpretedValue =
                    interpreter.getRegister(RISCV_REG_X0 + X_RESULT).getWord(0);
            if (nativeValue != interpretedValue)
            {
                fprintf(stderr, "TestSemanticLoweringFloat: %s: x10 native "
                        "%016llx interpreter %016llx for input %016llx\n", name,
                        (unsigned long long)nativeValue,
                        (unsigned long long)interpretedValue,
                        (unsigned long long)a);
                differences++;
            }
        } else
        {
            differences += compareFloatOutput(name, shape.m_outputBits, F_OUT,
                    state[FLOAT_STATE_INDEX + F_OUT],
                    interpreter.getRegister(RISCV_REG_F0 + F_OUT).getWord(0),
                    shape.m_exact, a, 0);
        }
        if (state[PC_STATE_INDEX] != interpreter.getNextAddress())
        {
            fprintf(stderr, "TestSemanticLoweringFloat: %s: the next address "
                    "differs\n", name);
            differences++;
        }
    }
    munmap(memory, 0x10000);
    return differences;
}
#endif

} // namespace

// ----------------------------------------------------------------------
// The test object
// ----------------------------------------------------------------------

class TestObjectTestSemanticLoweringFloat : public cTestObject {
public:
    TestObjectTestSemanticLoweringFloat() : m_failures(0), m_cases(0),
                                            m_convertCases(0),
                                            m_classifyCases(0),
                                            m_roundCases(0) {}

    virtual void test()
    {
        testFloatLowering();
#ifdef LOWERING_FLOAT_NATIVE
        testFloatNative();
        cout << "float: " << m_cases << " cases native==interpreter" << endl;
        testConvertNative();
        testArm64FixedNative();
        cout << "convert: " << m_convertCases << " cases native==interpreter" << endl;
        testClassifyNative();
        cout << "classify: " << m_classifyCases << " cases native==interpreter" << endl;
        testRoundNative();
        cout << "round: " << m_roundCases << " cases native==interpreter" << endl;
#else
        cout << "float: native comparisons skipped (not an x86-64 Linux host)" << endl;
        cout << "convert: native comparisons skipped (not an x86-64 Linux host)" << endl;
        cout << "classify: native comparisons skipped (not an x86-64 Linux host)" << endl;
        cout << "round: native comparisons skipped (not an x86-64 Linux host)" << endl;
#endif
        TESTS_ASSERT(m_failures == 0);
        cout << "Semantic lowering float tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    // ------------------------------------------------------------------
    // Part F1: the legalizer accepts, the lowering assembles, and the
    // refused operations are refused
    // ------------------------------------------------------------------
    void testFloatLowering()
    {
        ArchitectureDescriptionPtr guest = getGuest();
        SemanticLegalizer legalizer(guest, getTarget());
        uint blocks = 0;
        for (uint w = 0; w < 2; w++)
        {
            const uint bits = (w == 0) ? 32 : 64;
            for (uint o = 0; o < ARITHMETIC_COUNT; o++)
            {
                const uint forms = (o < 4) ? 4 : 3;
                for (uint form = 0; form < forms; form++)
                {
                    SemanticBlock block = arithmeticBlock(gArithmetic[o], bits, form);
                    legalizer.legalize(block);
                    FirstPassBinaryPtr binary = lowerBlock(block);
                    TESTS_ASSERT(binary->getCurrentBlockData().getSize() > 0);
                    blocks++;
                }
            }
            for (uint k = 0; k < CONDITIONS_COUNT; k++)
            {
                for (uint form = 0; form < 2; form++)
                {
                    SemanticBlock block = compareBlock(gFloatConditions[k],
                                    SemanticOperation::BEHAVIOR_DEFAULT, bits, form);
                    legalizer.legalize(block);
                    lowerBlock(block);
                    blocks++;
                }
            }
            {
                SemanticBlock block = compareBlock(SemanticOperation::COND_ALWAYS,
                                    SemanticOperation::BEHAVIOR_DEFAULT, bits, 0);
                legalizer.legalize(block);
                lowerBlock(block);
                blocks++;
            }
            {
                SemanticBlock block = compareBlock(SemanticOperation::COND_NEVER,
                                    SemanticOperation::BEHAVIOR_DEFAULT, bits, 0);
                legalizer.legalize(block);
                lowerBlock(block);
                blocks++;
            }
            {
                SemanticBlock block = constantSelectBlock(true, bits);
                legalizer.legalize(block);
                lowerBlock(block);
                blocks++;
            }
        }

        // A select of a half precision value is lowered as its bits; a
        // vector select is refused (its condition is one bit per lane)
        {
            SemanticBlock block = typedSelectBlock(SemanticType::floating(SemanticType::FLOAT_HALF));
            SemanticLegalizer legalizer(getGuest(), getTarget());
            legalizer.legalize(block);
            lowerBlock(block);
            blocks++;
        }
        {
            SemanticBlock block = typedSelectBlock(
                    SemanticType::vector(floatType(32), 2));
            TESTS_EXCEPTION(lowerBlock(block));
        }

        // Fused multiply-add: the legalizer and the lowering refuse it
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticType type = floatType(64);
            block.append(SemanticInstruction(SemanticOperation::OP_FLOAT_MULTIPLY_ADD,
                         type, floatRegister(F_OUT, 64), floatRegister(F_IN_A, 64),
                         floatRegister(F_IN_B, 64), floatRegister(F_OUT, 64)));
            TESTS_EXCEPTION(legalizer.legalize(block));
            TESTS_EXCEPTION(lowerBlock(block));
        }
        // The minimum is not lowered yet
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticType type = floatType(64);
            block.append(SemanticInstruction(SemanticOperation::OP_FLOAT_MINIMUM,
                         type, floatRegister(F_OUT, 64), floatRegister(F_IN_A, 64),
                         floatRegister(F_IN_B, 64)));
            TESTS_EXCEPTION(lowerBlock(block));
        }
        // The behaviors and the rounding which are not lowered
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticType type = floatType(64);
            SemanticInstruction multiply(SemanticOperation::OP_FLOAT_MULTIPLY, type,
                         floatRegister(F_OUT, 64), floatRegister(F_IN_A, 64),
                         floatRegister(F_IN_B, 64));
            multiply.setBehavior(SemanticOperation::BEHAVIOR_MULTIPLY_EXTENDED);
            block.append(multiply);
            TESTS_EXCEPTION(lowerBlock(block));
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            const SemanticType type = floatType(64);
            SemanticInstruction add(SemanticOperation::OP_FLOAT_ADD, type,
                         floatRegister(F_OUT, 64), floatRegister(F_IN_A, 64),
                         floatRegister(F_IN_B, 64));
            add.setRoundingMode(SemanticOperation::ROUND_TOWARD_ZERO);
            block.append(add);
            TESTS_EXCEPTION(lowerBlock(block));
        }

        // Part F3, the conversions: every integer width (signed and
        // unsigned) to both floats; every float to the 32 and 64 bit integers
        // with the three behaviors; the float conversions
        {
            static const uint sourceWidths[] = { 8, 16, 32, 64 };
            for (uint s = 0; s < 4; s++)
            {
                for (uint sign = 0; sign < 2; sign++)
                {
                    for (uint w = 0; w < 2; w++)
                    {
                        SemanticLegalizer legalizer(getGuest(), getTarget());
                        SemanticBlock block = integerToFloatBlock(sign == 0,
                                        sourceWidths[s], (w == 0) ? 32 : 64);
                        legalizer.legalize(block);
                        lowerBlock(block);
                        blocks++;
                    }
                }
            }
            for (uint sign = 0; sign < 2; sign++)
            {
                for (uint b = 0; b < CONVERT_BEHAVIORS_COUNT; b++)
                {
                    for (uint f = 0; f < 2; f++)
                    {
                        for (uint w = 0; w < 2; w++)
                        {
                            SemanticLegalizer legalizer(getGuest(), getTarget());
                            SemanticBlock block = floatToIntegerBlock(sign == 0,
                                gConvertBehaviors[b], (f == 0) ? 32 : 64,
                                (w == 0) ? 32 : 64);
                            legalizer.legalize(block);
                            lowerBlock(block);
                            blocks++;
                        }
                    }
                }
            }
            for (uint f = 0; f < 2; f++)
            {
                SemanticLegalizer legalizer(getGuest(), getTarget());
                SemanticBlock block = floatConvertBlock((f == 0) ? 32 : 64,
                                                        (f == 0) ? 64 : 32);
                legalizer.legalize(block);
                lowerBlock(block);
                blocks++;
            }
        }
        // The integer to float conversion in nearest away (no directed form in MXCSR)
        {
            SemanticBlock block = integerToFloatBlock(true, 64, 64, 0,
                        SemanticOperation::ROUND_NEAREST_AWAY);
            TESTS_EXCEPTION(lowerBlock(block));
        }
        // The conversions which are refused: round to odd, a behavior of
        // another operation, a result of 8 or 16 bits, a fixed point count of
        // more than 64 bits, the float round with a behavior of the integer
        // round (frint32z), and a float conversion between the same width
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            SemanticInstruction convert(SemanticOperation::OP_FLOAT_TO_SIGNED,
                        SemanticType::integer(64), integerRegister(X_RESULT, 64),
                        floatRegister(F_IN_A, 64));
            convert.setRoundingMode(SemanticOperation::ROUND_TO_ODD);
            block.append(convert);
            TESTS_EXCEPTION(lowerBlock(block));
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            SemanticInstruction convert(SemanticOperation::OP_FLOAT_TO_UNSIGNED,
                        SemanticType::integer(64), integerRegister(X_RESULT, 64),
                        floatRegister(F_IN_A, 64));
            convert.setRoundingMode(SemanticOperation::ROUND_TO_ODD);
            block.append(convert);
            TESTS_EXCEPTION(lowerBlock(block));
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            SemanticInstruction convert(SemanticOperation::OP_FLOAT_TO_SIGNED,
                        SemanticType::integer(64), integerRegister(X_RESULT, 64),
                        floatRegister(F_IN_A, 64));
            convert.setRoundingMode(SemanticOperation::ROUND_TOWARD_ZERO);
            convert.setBehavior(SemanticOperation::BEHAVIOR_COMPARE_QUIET);
            block.append(convert);
            TESTS_EXCEPTION(lowerBlock(block));
        }
        {
            SemanticBlock block = floatToIntegerBlock(true,
                        SemanticOperation::BEHAVIOR_CONVERT_INDEFINITE, 64, 8);
            TESTS_EXCEPTION(lowerBlock(block));
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            SemanticInstruction convert(SemanticOperation::OP_SIGNED_TO_FLOAT,
                        floatType(64), floatRegister(F_OUT, 64),
                        integerRegister(X_IN, 64),
                        SemanticOperand::immediate(65, SemanticType::integer(64)));
            block.append(convert);
            TESTS_EXCEPTION(lowerBlock(block));
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            SemanticInstruction round(SemanticOperation::OP_FLOAT_ROUND,
                         floatType(64), floatRegister(F_OUT, 64),
                         floatRegister(F_IN_A, 64));
            round.setBehavior(SemanticOperation::BEHAVIOR_ROUND_INTEGER_32);
            block.append(round);
            TESTS_EXCEPTION(lowerBlock(block));
        }
        {
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_RISCV_64);
            SemanticInstruction round(SemanticOperation::OP_FLOAT_ROUND,
                         floatType(64), floatRegister(F_OUT, 64),
                         floatRegister(F_IN_A, 64));
            round.setRoundingMode(SemanticOperation::ROUND_TO_ODD);
            block.append(round);
            TESTS_EXCEPTION(lowerBlock(block));
        }
        {
            SemanticBlock block = floatConvertBlock(64, 64);
            TESTS_EXCEPTION(lowerBlock(block));
        }

        // Part F3, the fixed point counts (the interpreter's exponent shift
        // and the product with 2^count before the truncation)
        {
            static const uint fractions[] = { 1, 32, 53, 63 };
            for (uint k = 0; k < 4; k++)
            {
                for (uint sign = 0; sign < 2; sign++)
                {
                    SemanticLegalizer legalizer(getGuest(), getTarget());
                    SemanticBlock block = integerToFloatBlock(sign == 0, 64, 64, fractions[k]);
                    legalizer.legalize(block);
                    lowerBlock(block);
                    blocks++;
                }
                for (uint b = 0; b < CONVERT_BEHAVIORS_COUNT; b++)
                {
                    SemanticLegalizer legalizer(getGuest(), getTarget());
                    SemanticBlock block = floatToIntegerBlock(true, gConvertBehaviors[b],
                                                              32, 32, fractions[k]);
                    legalizer.legalize(block);
                    lowerBlock(block);
                    blocks++;
                }
            }
        }
        // The A64 word ucvtf d0, x9, #53 is lifted and legalized (its lowering
        // runs in the native part)
        {
            ArchitectureDescriptionPtr guest(new Arm64Architecture(128, 128));
            SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
            TESTS_ASSERT(liftArm64Word(ARM64_UCVTF_WORD, guest, block));
            SemanticLegalizer legalizer(guest, getTarget());
            legalizer.legalize(block);
            blocks++;
        }

        // The round to an integral value in every mode, and the float to
        // integer conversions in the modes other than toward zero (Part F5
        // runs them natively)
        for (uint w = 0; w < 2; w++)
        {
            for (uint m = 0; m < ROUND_MODES_COUNT; m++)
            {
                SemanticLegalizer legalizer(getGuest(), getTarget());
                SemanticBlock block = floatRoundBlock(gRoundModes[m], (w == 0) ? 32 : 64);
                legalizer.legalize(block);
                lowerBlock(block);
                blocks++;
            }
        }
        for (uint m = 0; m < ROUND_MODES_COUNT; m++)
        {
            if (gRoundModes[m] == SemanticOperation::ROUND_TOWARD_ZERO)
                continue;
            for (uint f = 0; f < 2; f++)
            {
                SemanticLegalizer legalizer(getGuest(), getTarget());
                SemanticBlock block = floatToIntegerBlock(true,
                    SemanticOperation::BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM,
                    (f == 0) ? 32 : 64, 64, 0, gRoundModes[m]);
                legalizer.legalize(block);
                lowerBlock(block);
                blocks++;
            }
        }

        // The classify of f32 and f64 (Part F4 runs it natively)
        for (uint w = 0; w < 2; w++)
        {
            SemanticBlock block = classifyBlock((w == 0) ? 32 : 64);
            FirstPassBinaryPtr binary = lowerBlock(block);
            TESTS_ASSERT(binary->getCurrentBlockData().getSize() > 0);
            blocks++;
        }

        cout << "float lowering: " << blocks << " blocks legalized and lowered" << endl;
    }


#ifdef LOWERING_FLOAT_NATIVE
    // ------------------------------------------------------------------
    // Part F2: the lowered blocks run natively, as the interpreter runs them
    // ------------------------------------------------------------------
    void testFloatNative()
    {
        const uint failuresBefore = m_failures;
        for (uint w = 0; w < 2; w++)
        {
            const uint bits = (w == 0) ? 32 : 64;
            // The arithmetic: four binary operations (four forms), three unary.
            // The results of the arithmetic are NaN by class; the negate and
            // the absolute value only flip or clear the sign bit, so they are
            // exact
            for (uint o = 0; o < ARITHMETIC_COUNT; o++)
            {
                const uint forms = (o < 4) ? 4 : 3;
                const bool isSignOnly =
                    (gArithmetic[o] == SemanticOperation::OP_FLOAT_NEGATE) ||
                    (gArithmetic[o] == SemanticOperation::OP_FLOAT_ABSOLUTE);
                for (uint form = 0; form < forms; form++)
                {
                    SemanticBlock block = arithmeticBlock(gArithmetic[o], bits, form);
                    SemanticLegalizer legalizer(getGuest(), getTarget());
                    legalizer.legalize(block);
                    FloatOutputs outputs = { true, false, false, isSignOnly };
                    m_failures += runFloatBlock(SemanticOperation::getName(gArithmetic[o]),
                                                block, bits, outputs, m_cases);
                }
            }
            // The compares: every float condition, with a register and a
            // constant second operand; the booleans are exact, and the select
            // of the boolean is exact too
            for (uint k = 0; k < CONDITIONS_COUNT; k++)
            {
                for (uint form = 0; form < 2; form++)
                {
                    SemanticBlock block = compareBlock(gFloatConditions[k],
                                    SemanticOperation::BEHAVIOR_DEFAULT, bits, form);
                    FloatOutputs outputs = { false, true, true, true };
                    m_failures += runFloatBlock("compare", block, bits, outputs, m_cases);
                }
            }
            static const SemanticOperation::Condition always[] = {
                SemanticOperation::COND_ALWAYS, SemanticOperation::COND_NEVER };
            for (uint k = 0; k < 2; k++)
            {
                SemanticBlock block = compareBlock(always[k],
                                    SemanticOperation::BEHAVIOR_DEFAULT, bits, 0);
                FloatOutputs outputs = { false, true, true, true };
                m_failures += runFloatBlock("compare constant", block, bits, outputs, m_cases);
            }
            {
                SemanticBlock block = compareBlock(SemanticOperation::COND_FLOAT_EQUAL,
                                    SemanticOperation::BEHAVIOR_COMPARE_SIGNALING, bits, 0);
                FloatOutputs outputs = { false, true, true, true };
                m_failures += runFloatBlock("compare signaling", block, bits, outputs, m_cases);
            }
            {
                SemanticBlock block = compareBlock(SemanticOperation::COND_FLOAT_LESS,
                                    SemanticOperation::BEHAVIOR_COMPARE_QUIET, bits, 0);
                FloatOutputs outputs = { false, true, true, true };
                m_failures += runFloatBlock("compare quiet", block, bits, outputs, m_cases);
            }
            // The select of a constant condition
            for (uint k = 0; k < 2; k++)
            {
                SemanticBlock block = constantSelectBlock(k == 0, bits);
                FloatOutputs outputs = { false, true, false, true };
                m_failures += runFloatBlock("select", block, bits, outputs, m_cases);
            }
        }
        // The select of a half precision value (the generic bit copy): the
        // 16 bits are selected and the other bits of the slot are kept
        {
            SemanticBlock block = typedSelectBlock(SemanticType::floating(SemanticType::FLOAT_HALF));
            SemanticLegalizer legalizer(getGuest(), getTarget());
            legalizer.legalize(block);
            FloatOutputs outputs = { false, true, false, true };
            m_failures += runFloatBlock("select half", block, 16, outputs, m_cases);
        }
        TESTS_ASSERT(m_failures == failuresBefore);
    }
    // ------------------------------------------------------------------
    // Part F4: the classify of f32 and f64 runs natively, as the
    // interpreter runs it. The edge inputs cover every class (both signs of
    // the zeros, the normals, the subnormals, the infinities, the quiet and
    // the signaling NaNs), and the random inputs add the normals
    // ------------------------------------------------------------------
    void testClassifyNative()
    {
        const uint failuresBefore = m_failures;
        for (uint w = 0; w < 2; w++)
        {
            const uint bits = (w == 0) ? 32 : 64;
            SemanticBlock block = classifyBlock(bits);
            const ConvertShape shape = { false, bits, true, 32, true };
            const uint64* edges = (bits == 64) ? gEdges64 : gEdges32;
            const uint edgeCount = (bits == 64) ?
                (uint)(sizeof(gEdges64) / sizeof(gEdges64[0])) :
                (uint)(sizeof(gEdges32) / sizeof(gEdges32[0]));
            m_failures += runConvertBlock("classify", block, shape, edges,
                                          edgeCount, m_classifyCases);
        }
        TESTS_ASSERT(m_failures == failuresBefore);
    }

    // ------------------------------------------------------------------
    // Part F5: the round to an integral value (OP_FLOAT_ROUND) runs natively
    // in every rounding mode, as the interpreter runs it. The halfway cases
    // are exact (a signed zero keeps its sign, an integral value is itself)
    // ------------------------------------------------------------------
    void testRoundNative()
    {
        const uint failuresBefore = m_failures;
        for (uint w = 0; w < 2; w++)
        {
            const uint bits = (w == 0) ? 32 : 64;
            uint64 edges[64];
            uint count = 0;
            if (bits == 64)
            {
                for (uint i = 0; i < sizeof(gRoundDoubles) / sizeof(gRoundDoubles[0]); i++)
                    edges[count++] = patternOf(gRoundDoubles[i], 64);
                for (uint i = 0; i < sizeof(gRoundBits64) / sizeof(gRoundBits64[0]); i++)
                    edges[count++] = gRoundBits64[i];
            } else
            {
                for (uint i = 0; i < sizeof(gRoundSingles) / sizeof(gRoundSingles[0]); i++)
                    edges[count++] = patternOf(gRoundSingles[i], 32);
                for (uint i = 0; i < sizeof(gRoundBits32) / sizeof(gRoundBits32[0]); i++)
                    edges[count++] = gRoundBits32[i];
            }
            for (uint m = 0; m < ROUND_MODES_COUNT; m++)
            {
                SemanticBlock block = floatRoundBlock(gRoundModes[m], bits);
                SemanticLegalizer legalizer(getGuest(), getTarget());
                legalizer.legalize(block);
                // (a NaN compares by class: the sign of the default NaN is the x86 one)
                const ConvertShape shape = { false, bits, false, bits, false };
                m_failures += runConvertBlock("round", block, shape, edges, count,
                                              m_roundCases);
            }
        }
        TESTS_ASSERT(m_failures == failuresBefore);
    }

    // ------------------------------------------------------------------
    // Part F3: the conversions run natively, as the interpreter runs them
    // ------------------------------------------------------------------
    void testConvertNative()
    {
        const uint failuresBefore = m_failures;
        static const uint widths[] = { 8, 16, 32, 64 };
        const uint integerCount = sizeof(gIntegerEdges) / sizeof(gIntegerEdges[0]);

        // The float inputs of the conversions to integers: the bounds of
        // each width, then the NaNs, the infinities, the zeros and the
        // denormals of the format
        uint64 floats64[64];
        uint floats64Count = 0;
        for (uint i = 0; i < sizeof(gDoubleEdges) / sizeof(gDoubleEdges[0]); i++)
            floats64[floats64Count++] = patternOf(gDoubleEdges[i], 64);
        for (uint i = 0; i < sizeof(gEdges64) / sizeof(gEdges64[0]); i++)
            floats64[floats64Count++] = gEdges64[i];
        uint64 floats32[64];
        uint floats32Count = 0;
        for (uint i = 0; i < sizeof(gSingleEdges) / sizeof(gSingleEdges[0]); i++)
            floats32[floats32Count++] = patternOf(gSingleEdges[i], 32);
        for (uint i = 0; i < sizeof(gEdges32) / sizeof(gEdges32[0]); i++)
            floats32[floats32Count++] = gEdges32[i];

        // The integers to the floats: signed and unsigned, every width
        for (uint s = 0; s < 4; s++)
        {
            for (uint sign = 0; sign < 2; sign++)
            {
                for (uint w = 0; w < 2; w++)
                {
                    const uint bits = (w == 0) ? 32 : 64;
                    const SemanticOperation::Operation operation = (sign == 0) ?
                        SemanticOperation::OP_SIGNED_TO_FLOAT :
                        SemanticOperation::OP_UNSIGNED_TO_FLOAT;
                    SemanticBlock block = integerToFloatBlock(sign == 0, widths[s], bits);
                    SemanticLegalizer legalizer(getGuest(), getTarget());
                    legalizer.legalize(block);
                    ConvertShape shape = { true, widths[s], false, bits, true };
                    m_failures += runConvertBlock(SemanticOperation::getName(operation),
                                block, shape, gIntegerEdges, integerCount, m_convertCases);
                }
            }
        }

        // The floats to the integers, with each behavior
        for (uint f = 0; f < 2; f++)
        {
            const uint sourceBits = (f == 0) ? 32 : 64;
            const uint64* edges = (f == 0) ? floats32 : floats64;
            const uint edgeCount = (f == 0) ? floats32Count : floats64Count;
            for (uint sign = 0; sign < 2; sign++)
            {
                for (uint b = 0; b < CONVERT_BEHAVIORS_COUNT; b++)
                {
                    for (uint w = 0; w < 2; w++)
                    {
                        const uint bits = (w == 0) ? 32 : 64;
                        const SemanticOperation::Operation operation = (sign == 0) ?
                            SemanticOperation::OP_FLOAT_TO_SIGNED :
                            SemanticOperation::OP_FLOAT_TO_UNSIGNED;
                        SemanticBlock block = floatToIntegerBlock(sign == 0,
                                    gConvertBehaviors[b], sourceBits, bits);
                        SemanticLegalizer legalizer(getGuest(), getTarget());
                        legalizer.legalize(block);
                        ConvertShape shape = { false, sourceBits, true, bits, true };
                        m_failures += runConvertBlock(SemanticOperation::getName(operation),
                                    block, shape, edges, edgeCount, m_convertCases);
                    }
                }
            }
        }

        // The floats to the integers in the other rounding modes: the
        // value is rounded to an integral value, then truncated
        for (uint f = 0; f < 2; f++)
        {
            const uint sourceBits = (f == 0) ? 32 : 64;
            const uint64* edges = (f == 0) ? floats32 : floats64;
            const uint edgeCount = (f == 0) ? floats32Count : floats64Count;
            for (uint m = 0; m < ROUND_MODES_COUNT; m++)
            {
                if (gRoundModes[m] == SemanticOperation::ROUND_TOWARD_ZERO)
                    continue;
                for (uint sign = 0; sign < 2; sign++)
                {
                    for (uint b = 0; b < CONVERT_BEHAVIORS_COUNT; b++)
                    {
                        for (uint w = 0; w < 2; w++)
                        {
                            const uint bits = (w == 0) ? 32 : 64;
                            const SemanticOperation::Operation operation = (sign == 0) ?
                                SemanticOperation::OP_FLOAT_TO_SIGNED :
                                SemanticOperation::OP_FLOAT_TO_UNSIGNED;
                            SemanticBlock block = floatToIntegerBlock(sign == 0,
                                        gConvertBehaviors[b], sourceBits, bits, 0,
                                        gRoundModes[m]);
                            SemanticLegalizer legalizer(getGuest(), getTarget());
                            legalizer.legalize(block);
                            ConvertShape shape = { false, sourceBits, true, bits, true };
                            m_failures += runConvertBlock(SemanticOperation::getName(operation),
                                        block, shape, edges, edgeCount, m_convertCases);
                        }
                    }
                }
            }
        }

        // The float conversions: the NaNs are compared by class (the sign and
        // the payload of the converted NaN are the documented deviation)
        for (uint f = 0; f < 2; f++)
        {
            const uint sourceBits = (f == 0) ? 32 : 64;
            const uint bits = (f == 0) ? 64 : 32;
            SemanticBlock block = floatConvertBlock(sourceBits, bits);
            SemanticLegalizer legalizer(getGuest(), getTarget());
            legalizer.legalize(block);
            ConvertShape shape = { false, sourceBits, false, bits, false };
            const uint64* edges = (f == 0) ? floats32 : floats64;
            const uint edgeCount = (f == 0) ? floats32Count : floats64Count;
            m_failures += runConvertBlock(SemanticOperation::getName(
                                SemanticOperation::OP_FLOAT_CONVERT),
                        block, shape, edges, edgeCount, m_convertCases);
        }

        // The integers to the floats in the directed rounding modes (the
        // RISC-V fcvt.{s,d}.{w,wu,l,lu} with rdn, rup and rtz): each mode,
        // signed and unsigned, every width, f32 and f64
        static const SemanticOperation::RoundingMode intModes[] = {
            SemanticOperation::ROUND_TOWARD_NEGATIVE,
            SemanticOperation::ROUND_TOWARD_POSITIVE,
            SemanticOperation::ROUND_TOWARD_ZERO,
            SemanticOperation::ROUND_NEAREST_EVEN };
        for (uint m = 0; m < 4; m++)
        {
            for (uint s = 0; s < 4; s++)
            {
                for (uint sign = 0; sign < 2; sign++)
                {
                    for (uint w = 0; w < 2; w++)
                    {
                        const uint bits = (w == 0) ? 32 : 64;
                        SemanticBlock block = integerToFloatBlock(sign == 0, widths[s],
                                                                  bits, 0, intModes[m]);
                        SemanticLegalizer legalizer(getGuest(), getTarget());
                        legalizer.legalize(block);
                        ConvertShape shape = { true, widths[s], false, bits, true };
                        m_failures += runConvertBlock("integer to float, rounded",
                                    block, shape, gIntegerEdges, integerCount, m_convertCases);
                    }
                }
            }
        }

        // The fixed point counts of 1, 32, 53 and 63 bits: the integers to the
        // floats, and the floats to the integers with each behavior
        static const uint fractions[] = { 1, 32, 53, 63 };
        for (uint k = 0; k < 4; k++)
        {
            const uint fraction = fractions[k];
            for (uint s = 0; s < 4; s++)
            {
                for (uint sign = 0; sign < 2; sign++)
                {
                    for (uint w = 0; w < 2; w++)
                    {
                        const uint bits = (w == 0) ? 32 : 64;
                        SemanticBlock block = integerToFloatBlock(sign == 0, widths[s],
                                                                  bits, fraction);
                        SemanticLegalizer legalizer(getGuest(), getTarget());
                        legalizer.legalize(block);
                        ConvertShape shape = { true, widths[s], false, bits, true };
                        m_failures += runConvertBlock("fixed integer to float", block,
                                    shape, gIntegerEdges, integerCount, m_convertCases);
                    }
                }
            }
            for (uint f = 0; f < 2; f++)
            {
                const uint sourceBits = (f == 0) ? 32 : 64;
                const uint64* edges = (f == 0) ? floats32 : floats64;
                const uint edgeCount = (f == 0) ? floats32Count : floats64Count;
                for (uint sign = 0; sign < 2; sign++)
                {
                    for (uint b = 0; b < CONVERT_BEHAVIORS_COUNT; b++)
                    {
                        for (uint w = 0; w < 2; w++)
                        {
                            const uint bits = (w == 0) ? 32 : 64;
                            SemanticBlock block = floatToIntegerBlock(sign == 0,
                                        gConvertBehaviors[b], sourceBits, bits, fraction);
                            SemanticLegalizer legalizer(getGuest(), getTarget());
                            legalizer.legalize(block);
                            ConvertShape shape = { false, sourceBits, true, bits, true };
                            m_failures += runConvertBlock("fixed float to integer", block,
                                        shape, edges, edgeCount, m_convertCases);
                        }
                    }
                }
            }
        }
        TESTS_ASSERT(m_failures == failuresBefore);
    }

    // ------------------------------------------------------------------
    // Part F4: the A64 word ucvtf d0, x9, #53 (lifted, legalized, lowered)
    // runs natively as the interpreter runs the lifted block
    // ------------------------------------------------------------------
    void testArm64FixedNative()
    {
        const uint failuresBefore = m_failures;
        ArchitectureDescriptionPtr guest(new Arm64Architecture(128, 128));
        SemanticBlock lifted(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        TESTS_ASSERT(liftArm64Word(ARM64_UCVTF_WORD, guest, lifted));
        SemanticBlock legalized(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        TESTS_ASSERT(liftArm64Word(ARM64_UCVTF_WORD, guest, legalized));
        SemanticLegalizer legalizer(guest, getTarget());
        legalizer.legalize(legalized);
        cBuffer code;
        if (!buildNative(legalized, Arm64FixedRegisterMap(), code))
        {
            fprintf(stderr, "TestSemanticLoweringFloat: ucvtf: the native code is "
                    "not assembled\n");
            m_failures++;
            return;
        }
        void* memory = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED)
            return;
        for (uint i = 0; i < code.getSize(); i++)
            ((uint8*)memory)[i] = code[i];
        NativeFunction function = reinterpret_cast<NativeFunction>(memory);

        const uint integerCount = sizeof(gIntegerEdges) / sizeof(gIntegerEdges[0]);
        const uint trials = integerCount + RANDOM_TRIALS;
        const uint X9_STATE_INDEX = 0x48 / 8;
        for (uint trial = 0; (trial < trials) && (m_failures == failuresBefore); trial++)
        {
            const uint64 a = (trial < integerCount) ? gIntegerEdges[trial] : floatRandom();
            static uint64 state[0x800];
            for (uint i = 0; i < 0x800; i++)
                state[i] = STATE_FILL;
            state[X9_STATE_INDEX] = a;
            function(state, NULL);

            FloatTestMemory guestMemory;
            SemanticInterpreter interpreter(guest, guestMemory);
            interpreter.setRegister(armdis::ARM64_REG_X0 + 9, SemanticValue(a));
            if (interpreter.run(lifted) != SemanticInterpreter::EXIT_FALL_THROUGH)
            {
                fprintf(stderr, "TestSemanticLoweringFloat: ucvtf: the interpreter "
                        "does not run the block\n");
                m_failures++;
                break;
            }
            m_convertCases++;
            // d0 is the low half of z0 (the upper half is cleared by the
            // interpreter and not compared)
            const uint64 nativeValue = state[FLOAT_STATE_INDEX];
            const uint64 interpretedValue =
                    interpreter.getRegister(armdis::ARM64_REG_D0).getWord(0);
            if (nativeValue != interpretedValue)
            {
                fprintf(stderr, "TestSemanticLoweringFloat: ucvtf: d0 native "
                        "%016llx interpreter %016llx for x9 %016llx\n",
                        (unsigned long long)nativeValue,
                        (unsigned long long)interpretedValue,
                        (unsigned long long)a);
                m_failures++;
            }
            if (state[PC_STATE_INDEX] != interpreter.getNextAddress())
            {
                fprintf(stderr, "TestSemanticLoweringFloat: ucvtf: the next address "
                        "differs\n");
                m_failures++;
            }
        }
        munmap(memory, 0x10000);
        TESTS_ASSERT(m_failures == failuresBefore);
    }
#endif

    // The failures of the comparisons
    uint m_failures;
    // The comparisons made natively against the interpreter
    uint m_cases;
    // The comparisons of the conversions made natively
    uint m_convertCases;
    // The comparisons of the classify made natively
    uint m_classifyCases;
    // The cases of the round to an integral value (Part F5)
    uint m_roundCases;
};

// Instance test object
TestObjectTestSemanticLoweringFloat g_globalTestSemanticLoweringFloat;
