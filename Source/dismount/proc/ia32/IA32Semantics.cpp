/*
 * IA32Semantics.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/proc/ia32/IA32Opcode.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/IA32OperandDecoder.h"
#include "dismount/proc/ia32/IA32Semantics.h"

using namespace ia32dis;

typedef SemanticOperation Op;

namespace {

// The prefix byte of lock
const uint8 PREFIX_LOCK = 0xF0;

// Return the size in bits of a register of the IA-32 description, or of the
// AMD64 one in 64 bit mode (where the segment bases and others are 64 bits)
uint getRegisterBits(uint registerId, bool is64)
{
    static IA32Architecture architecture(OpcodeSubsystems::DISASSEMBLER_INTEL_32);
    static AMD64Architecture architecture64;
    if (is64)
        return architecture64.getRegister(registerId).m_bits;
    return architecture.getRegister(registerId).m_bits;
}

// Return log2 of a power of two
uint log2Of(uint value)
{
    uint result = 0;
    while ((((uint)1 << result) < value) && (result < 31))
        result++;
    return result;
}

// Return true if the instruction has an mm register operand: it puts the x87
// unit in the MMX state
bool isMmxInstruction(const IA32Opcode& instruction)
{
    // cvtpi2ps and cvtpi2pd are MMX instructions with a memory operand too
    const char* name = instruction.getOpcodeEntry()->m_opcodeName;
    if ((strcmp(name, "cvtpi2ps") == 0) || (strcmp(name, "cvtpi2pd") == 0))
        return true;
    const uint count = IA32OperandDecoder::getOperandsCount(instruction);
    for (uint i = 0; i < count; i++)
    {
        const IA32DecodedOperand operand = IA32OperandDecoder::decode(instruction, i);
        if ((operand.m_kind == IA32DecodedOperand::KIND_REGISTER) &&
            (operand.m_register >= IA32_REG_MM0) &&
            (operand.m_register <= IA32_REG_MM7))
            return true;
    }
    return false;
}

// Return the data type of a vector or floating point entry, see
// IA32SemanticEntry::m_dataKind
SemanticType getEntryDataType(const IA32SemanticEntry& entry,
                              const IA32Opcode& instruction)
{
    uint lanes = entry.m_dataLanes;
    if (lanes == 0)
        lanes = (isMmxInstruction(instruction) ? 64 : 128) / entry.m_dataBits;
    return SemanticType((SemanticType::Kind)entry.m_dataKind, entry.m_dataBits,
                        lanes, (SemanticType::FloatFormat)entry.m_dataFormat);
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// IA32LiftContext
/////////////////////////////////////////////////////////////////////////////

IA32LiftContext::IA32LiftContext(const IA32Opcode& instruction,
                                 SemanticBlock& block,
                                 bool isFlat) :
    m_instruction(instruction),
    m_block(block),
    m_isFlat(isFlat || IA32OperandDecoder::is64BitMode(instruction)),
    m_operandsCount(IA32OperandDecoder::getOperandsCount(instruction))
{
    for (uint i = 0; i < m_operandsCount; i++)
        m_operands[i] = IA32OperandDecoder::decode(instruction, i);
}

const IA32Opcode& IA32LiftContext::getInstruction() const
{
    return m_instruction;
}

SemanticBlock& IA32LiftContext::getBlock()
{
    return m_block;
}

uint IA32LiftContext::getOperandsCount() const
{
    return m_operandsCount;
}

const IA32DecodedOperand& IA32LiftContext::getOperand(uint index) const
{
    CHECK(index < m_operandsCount);
    return m_operands[index];
}

SemanticType IA32LiftContext::getOperandType(uint index) const
{
    return SemanticType::integer(getOperand(index).m_bits);
}

SemanticType IA32LiftContext::getOperandSizeType() const
{
    return SemanticType::integer(IA32OperandDecoder::getOperandBits(m_instruction));
}

SemanticType IA32LiftContext::getAddressSizeType() const
{
    return SemanticType::integer(IA32OperandDecoder::getAddressBits(m_instruction));
}

SemanticType IA32LiftContext::getStackAddressType() const
{
    // The stack of protected mode is esp (the B bit of a flat ss), of real
    // mode sp, of 64 bit mode rsp
    if (is64BitMode())
        return SemanticType::integer(64);
    return SemanticType::integer(
        (m_instruction.getType() == OpcodeSubsystems::DISASSEMBLER_INTEL_32) ?
            32 : 16);
}

SemanticType IA32LiftContext::getNativeType() const
{
    return SemanticType::integer(is64BitMode() ? 64 : 32);
}

bool IA32LiftContext::is64BitMode() const
{
    return IA32OperandDecoder::is64BitMode(m_instruction);
}

SemanticOperand IA32LiftContext::read(uint index, const SemanticType& type)
{
    const IA32DecodedOperand& operand = getOperand(index);
    switch (operand.m_kind)
    {
    case IA32DecodedOperand::KIND_REGISTER:
        return getRegister(operand.m_register);
    case IA32DecodedOperand::KIND_MEMORY:
        return load(getAddress(index), type);
    case IA32DecodedOperand::KIND_IMMEDIATE:
        return getImmediate(operand.m_immediate, type);
    case IA32DecodedOperand::KIND_BRANCH_TARGET:
        return SemanticOperand::guestAddress(operand.m_immediate, type);
    default:
        // A far pointer isn't a value
        CHECK_FAIL();
    }
}

SemanticOperand IA32LiftContext::read(uint index)
{
    return read(index, getOperandType(index));
}

void IA32LiftContext::write(uint index, const SemanticOperand& value)
{
    const IA32DecodedOperand& operand = getOperand(index);
    switch (operand.m_kind)
    {
    case IA32DecodedOperand::KIND_REGISTER:
        move(getRegister(operand.m_register), value);
        break;
    case IA32DecodedOperand::KIND_MEMORY:
        store(getAddress(index), value);
        break;
    default:
        // The immediates and the branch targets are not destinations
        CHECK_FAIL();
    }
}

SemanticOperand IA32LiftContext::getOffset(uint index)
{
    const IA32DecodedOperand& operand = getOperand(index);
    CHECK(operand.m_kind == IA32DecodedOperand::KIND_MEMORY);
    const SemanticType addressType = SemanticType::integer(operand.m_addressBits);

    // [rip + disp32]: the address of the next instruction is known
    if (operand.m_isRipRelative)
    {
        const uint64 mask = (operand.m_addressBits == 64) ? ~(uint64)0 :
                            (((uint64)1 << operand.m_addressBits) - 1);
        return getImmediate((int64)((IA32OperandDecoder::getNextAddress(m_instruction) +
                                     (uint64)operand.m_displacement) & mask),
                            addressType);
    }

    // base + index * scale + displacement, in the address size: the offset
    // wraps at 16 bits for a 16 bit address size
    SemanticOperand offset;
    if (operand.m_base != IA32DecodedOperand::NO_REGISTER)
        offset = getRegister(operand.m_base);
    if (operand.m_index != IA32DecodedOperand::NO_REGISTER)
    {
        SemanticOperand scaled = getRegister(operand.m_index);
        if (operand.m_scale > 1)
            scaled = emit(Op::OP_SHIFT_LEFT, addressType, scaled,
                          getImmediate(log2Of(operand.m_scale), addressType));
        offset = offset.isValid() ? emit(Op::OP_ADD, addressType, offset, scaled)
                                  : scaled;
    }
    if ((operand.m_displacement != 0) || !offset.isValid())
    {
        SemanticOperand displacement = getImmediate(operand.m_displacement,
                                                    addressType);
        offset = offset.isValid() ?
                    emit(Op::OP_ADD, addressType, offset, displacement) :
                    displacement;
    }
    return offset;
}

SemanticOperand IA32LiftContext::getLinearAddress(uint segment,
                                                  const SemanticOperand& offset)
{
    const SemanticType linearType = getNativeType();
    SemanticOperand linear = offset;
    if (offset.getType().getTotalBits() < linearType.getTotalBits())
    {
        if (offset.getKind() == SemanticOperand::KIND_IMMEDIATE)
            linear = getImmediate(offset.getValue() &
                    (((int64)1 << offset.getType().getTotalBits()) - 1),
                    linearType);
        else
            linear = emit(Op::OP_ZERO_EXTEND, linearType, offset);
    }

    // The flat model adds the bases of fs and gs only. The real mode adds the
    // base of every segment
    if (!m_isFlat || (segment == IA32_REG_FS) || (segment == IA32_REG_GS))
        linear = emit(Op::OP_ADD, linearType, linear,
                      getRegister(IA32_REG_ES_BASE + (segment - IA32_REG_ES)));
    return linear;
}

SemanticOperand IA32LiftContext::getAddress(uint index)
{
    CHECK(index < 3);
    if (m_addresses[index].isValid())
        return m_addresses[index];

    SemanticOperand address = getLinearAddress(getOperand(index).m_segment,
                                               getOffset(index));
    // The instruction can change the registers of the address (xadd [eax],
    // eax) before it writes the memory: keep the value
    if (address.getKind() == SemanticOperand::KIND_REGISTER)
        address = emit(Op::OP_MOVE, address.getType(), address);
    m_addresses[index] = address;
    return address;
}

SemanticOperand IA32LiftContext::getRegister(uint registerId) const
{
    return SemanticOperand::registerOperand(registerId,
                    SemanticType::integer(getRegisterBits(registerId,
                                                          is64BitMode())));
}

SemanticOperand IA32LiftContext::getStackPointer() const
{
    if (is64BitMode())
        return getRegister(AMD64_REG_RSP);
    return getRegister((getStackAddressType().getTotalBits() == 32) ?
                       IA32_REG_ESP : IA32_REG_SP);
}

SemanticOperand IA32LiftContext::getInstructionPointer() const
{
    return getRegister(is64BitMode() ? AMD64_REG_RIP : IA32_REG_EIP);
}

SemanticOperand IA32LiftContext::getGeneralRegister(uint number, uint bits) const
{
    if (is64BitMode())
        return getRegister(getAmd64GeneralRegister(number, bits,
                                                   m_instruction.hasRex()));
    CHECK(number < NUMBER_OF_REGISTERS);
    switch (bits)
    {
    case 8:  return getRegister(IA32_REG_AL + number);
    case 16: return getRegister(IA32_REG_AX + number);
    case 32: return getRegister(IA32_REG_EAX + number);
    default:
        CHECK_FAIL();
    }
}

SemanticOperand IA32LiftContext::getImmediate(int64 value,
                                              const SemanticType& type) const
{
    return SemanticOperand::immediate(value, type);
}

SemanticOperand IA32LiftContext::getCurrentAddress() const
{
    return SemanticOperand::guestAddress(
                IA32OperandDecoder::getAddress(m_instruction), getNativeType());
}

SemanticOperand IA32LiftContext::getNextAddress() const
{
    return SemanticOperand::guestAddress(
                IA32OperandDecoder::getNextAddress(m_instruction),
                getNativeType());
}

SemanticOperation::Condition IA32LiftContext::getOpcodeCondition() const
{
    switch (IA32OperandDecoder::getLastOpcodeByte(m_instruction) & 0xF)
    {
    case 0x0: return Op::COND_OVERFLOW;
    case 0x1: return Op::COND_NOT_OVERFLOW;
    case 0x2: return Op::COND_UNSIGNED_LESS;
    case 0x3: return Op::COND_UNSIGNED_GREATER_EQUAL;
    case 0x4: return Op::COND_EQUAL;
    case 0x5: return Op::COND_NOT_EQUAL;
    case 0x6: return Op::COND_UNSIGNED_LESS_EQUAL;
    case 0x7: return Op::COND_UNSIGNED_GREATER;
    case 0x8: return Op::COND_NEGATIVE;
    case 0x9: return Op::COND_NOT_NEGATIVE;
    case 0xA: return Op::COND_PARITY_EVEN;
    case 0xB: return Op::COND_PARITY_ODD;
    case 0xC: return Op::COND_SIGNED_LESS;
    case 0xD: return Op::COND_SIGNED_GREATER_EQUAL;
    case 0xE: return Op::COND_SIGNED_LESS_EQUAL;
    default:  return Op::COND_SIGNED_GREATER;
    }
}

SemanticOperand IA32LiftContext::newTemporary(const SemanticType& type)
{
    return m_block.newTemporary(type);
}

SemanticOperand IA32LiftContext::newLabel()
{
    return m_block.newLabel();
}

SemanticOperand IA32LiftContext::emit(SemanticOperation::Operation operation,
                                      const SemanticType& type,
                                      const SemanticOperand& source0,
                                      const SemanticOperand& source1,
                                      const SemanticOperand& source2)
{
    SemanticOperand result = m_block.newTemporary(type);
    m_block.append(SemanticInstruction(operation, type, result,
                                       source0, source1, source2));
    return result;
}

void IA32LiftContext::emit(const SemanticInstruction& instruction)
{
    m_block.append(instruction);
}

void IA32LiftContext::move(const SemanticOperand& destination,
                           const SemanticOperand& source)
{
    m_block.append(SemanticInstruction(Op::OP_MOVE, destination.getType(),
                                       destination, source));
}

SemanticOperand IA32LiftContext::load(const SemanticOperand& address,
                                      const SemanticType& type)
{
    SemanticOperand result = m_block.newTemporary(type);
    SemanticInstruction instruction(Op::OP_LOAD, type, result, address);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    m_block.append(instruction);
    return result;
}

void IA32LiftContext::store(const SemanticOperand& address,
                            const SemanticOperand& value)
{
    SemanticInstruction instruction(Op::OP_STORE, value.getType(),
                                    SemanticOperand(), address, value);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    m_block.append(instruction);
}

void IA32LiftContext::setFlags(SemanticOperation::FlagRecipe recipe,
                               uint32 flagMask,
                               const SemanticType& type,
                               const SemanticOperand& source0,
                               const SemanticOperand& source1,
                               const SemanticOperand& result)
{
    SemanticInstruction instruction(Op::OP_SET_FLAGS, type, SemanticOperand(),
                                    source0, source1, result);
    instruction.setFlags(recipe, flagMask);
    m_block.append(instruction);
}

void IA32LiftContext::setFlags(uint32 flagMask, const SemanticOperand& value)
{
    setFlags(Op::FLAGS_EXPLICIT, flagMask, SemanticType::integer(1),
             value, SemanticOperand(), SemanticOperand());
}

SemanticOperand IA32LiftContext::testFlags(SemanticOperation::Condition condition)
{
    const SemanticType booleanType = SemanticType::integer(1);
    SemanticOperand result = m_block.newTemporary(booleanType);
    SemanticInstruction instruction(Op::OP_TEST_FLAGS, booleanType, result);
    instruction.setCondition(condition);
    m_block.append(instruction);
    return result;
}

void IA32LiftContext::placeLabel(const SemanticOperand& label)
{
    m_block.append(SemanticInstruction(Op::OP_LABEL, SemanticType(),
                                       SemanticOperand(), label));
}

void IA32LiftContext::branch(const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_BRANCH, SemanticType(),
                                       SemanticOperand(), target));
}

void IA32LiftContext::branchIf(const SemanticOperand& condition,
                               const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_BRANCH_CONDITIONAL,
                                       SemanticType(), SemanticOperand(),
                                       condition, target));
}

void IA32LiftContext::push(const SemanticOperand& value)
{
    const SemanticType stackType = getStackAddressType();
    const SemanticOperand stackPointer = getStackPointer();
    // push esp stores the value the register had before the push
    SemanticOperand data = value;
    if (value.getKind() == SemanticOperand::KIND_REGISTER)
        data = emit(Op::OP_MOVE, value.getType(), value);

    const uint bytes = value.getType().getTotalBits() / 8;
    SemanticOperand newPointer = emit(Op::OP_SUBTRACT, stackType, stackPointer,
                                      getImmediate(bytes, stackType));
    store(getLinearAddress(IA32_REG_SS, newPointer), data);
    move(stackPointer, newPointer);
}

SemanticOperand IA32LiftContext::pop(const SemanticType& type)
{
    const SemanticType stackType = getStackAddressType();
    const SemanticOperand stackPointer = getStackPointer();
    SemanticOperand value = load(getLinearAddress(IA32_REG_SS, stackPointer),
                                 type);
    // The destination of the pop is written after the increment (pop [esp])
    SemanticOperand newPointer = emit(Op::OP_ADD, stackType, stackPointer,
                    getImmediate(type.getTotalBits() / 8, stackType));
    move(stackPointer, newPointer);
    return value;
}

void IA32LiftContext::trap(SemanticOperation::TrapKind kind, uint number)
{
    SemanticInstruction instruction(Op::OP_TRAP, SemanticType(),
                                    SemanticOperand(),
                                    getImmediate(number, SemanticType::integer(32)));
    instruction.setKind(kind);
    m_block.append(instruction);
}

/////////////////////////////////////////////////////////////////////////////
// The instantiation of the template steps
/////////////////////////////////////////////////////////////////////////////

namespace {

// The most temporaries and labels a template uses
enum { MAX_TEMPLATE_VALUES = 8 };

/*
 * A lock prefixed read-modify-write of operand 0 (memory), which the steps of
 * a template express as a read of operand 0 and a write into it
 */
struct LockedAccess {
    // The value the steps see when they read operand 0: the old value of the
    // atomic operation
    SemanticOperand m_old;
    // The value the steps write into operand 0
    SemanticOperand m_new;
    // True if the steps write operand 0
    bool m_isWritten;
};

/*
 * The state of the instantiation of one template: its temporaries and labels
 * (created when they're first used), and the locked access.
 */
class TemplateInstance {
public:
    TemplateInstance(IA32LiftContext& context, LockedAccess* locked) :
        m_context(context),
        m_locked(locked)
    {
    }

    // Instantiate all the steps
    void run(const SemanticTemplate& semanticTemplate);

private:
    // Return the type of a step
    SemanticType getType(const SemanticTemplateType& type) const;
    // Return the operand a reference reads
    SemanticOperand readReference(const SemanticTemplateReference& reference,
                                  const SemanticType& type);
    // Write the result of a step into its destination
    void writeReference(const SemanticTemplateReference& reference,
                        const SemanticOperand& value);
    // Instantiate a single step
    void step(const SemanticTemplateStep& step);

    IA32LiftContext& m_context;
    LockedAccess* m_locked;
    SemanticOperand m_temporaries[MAX_TEMPLATE_VALUES];
    SemanticOperand m_labels[MAX_TEMPLATE_VALUES];
    // The values loaded from the memory operands: a memory operand is read
    // once, and the write goes to the same address
    SemanticOperand m_loaded[3];
};

SemanticType TemplateInstance::getType(const SemanticTemplateType& type) const
{
    switch (type.m_source)
    {
    case SemanticTemplateType::SOURCE_FIXED:
        return SemanticType((SemanticType::Kind)type.m_kind, type.m_bits,
                            type.m_lanes);
    case SemanticTemplateType::SOURCE_OPERAND_SIZE:
        return m_context.getOperandSizeType();
    case SemanticTemplateType::SOURCE_ADDRESS_SIZE:
        return m_context.getAddressSizeType();
    case SemanticTemplateType::SOURCE_OPERAND:
        return m_context.getOperandType(type.m_bits);
    case SemanticTemplateType::SOURCE_DOUBLE_OPERAND:
        return SemanticType::integer(
                    m_context.getOperandType(type.m_bits).getTotalBits() * 2);
    case SemanticTemplateType::SOURCE_BOOLEAN:
        return SemanticType::integer(1);
    default:
        // The vector types are not used by the IA-32 integer templates
        CHECK_FAIL();
    }
}

SemanticOperand TemplateInstance::readReference(
                                const SemanticTemplateReference& reference,
                                const SemanticType& type)
{
    switch (reference.m_kind)
    {
    case SemanticTemplateReference::KIND_NONE:
        return SemanticOperand();
    case SemanticTemplateReference::KIND_OPERAND:
        if ((m_locked != NULL) && (reference.m_index == 0))
            return m_locked->m_old;
        if (m_context.getOperand(reference.m_index).m_kind !=
            IA32DecodedOperand::KIND_MEMORY)
            return m_context.read(reference.m_index);
        if (!m_loaded[reference.m_index].isValid())
            m_loaded[reference.m_index] = m_context.read(reference.m_index);
        return m_loaded[reference.m_index];
    case SemanticTemplateReference::KIND_OPERAND_ADDRESS:
        return m_context.getAddress(reference.m_index);
    case SemanticTemplateReference::KIND_TEMPORARY:
        CHECK((reference.m_index < MAX_TEMPLATE_VALUES) &&
              m_temporaries[reference.m_index].isValid());
        return m_temporaries[reference.m_index];
    case SemanticTemplateReference::KIND_REGISTER:
        return m_context.getRegister(reference.m_value);
    case SemanticTemplateReference::KIND_IMMEDIATE:
        return m_context.getImmediate(reference.m_value, type);
    case SemanticTemplateReference::KIND_OPERATION_BYTES:
        return m_context.getImmediate(type.getTotalBits() / 8, type);
    case SemanticTemplateReference::KIND_NEXT_ADDRESS:
        return m_context.getNextAddress();
    case SemanticTemplateReference::KIND_CURRENT_ADDRESS:
        return m_context.getCurrentAddress();
    case SemanticTemplateReference::KIND_LABEL:
        CHECK(reference.m_index < MAX_TEMPLATE_VALUES);
        if (!m_labels[reference.m_index].isValid())
            m_labels[reference.m_index] = m_context.newLabel();
        return m_labels[reference.m_index];
    default:
        CHECK_FAIL();
    }
}

void TemplateInstance::writeReference(const SemanticTemplateReference& reference,
                                      const SemanticOperand& value)
{
    switch (reference.m_kind)
    {
    case SemanticTemplateReference::KIND_OPERAND:
        if ((m_locked != NULL) && (reference.m_index == 0))
        {
            // The atomic access writes the memory
            m_locked->m_new = value;
            m_locked->m_isWritten = true;
        } else
        {
            m_context.write(reference.m_index, value);
            m_loaded[reference.m_index] = SemanticOperand();
        }
        break;
    case SemanticTemplateReference::KIND_TEMPORARY:
        CHECK(reference.m_index < MAX_TEMPLATE_VALUES);
        m_temporaries[reference.m_index] = value;
        break;
    case SemanticTemplateReference::KIND_REGISTER:
        m_context.move(m_context.getRegister(reference.m_value), value);
        break;
    default:
        CHECK_FAIL();
    }
}

void TemplateInstance::step(const SemanticTemplateStep& step)
{
    const SemanticType type = getType(step.m_type);
    const SemanticOperation::Operation operation =
                        (SemanticOperation::Operation)step.m_operation;

    SemanticOperand sources[SemanticInstruction::MAX_SOURCES];
    for (uint i = 0; i < SemanticInstruction::MAX_SOURCES; i++)
        sources[i] = readReference(step.m_sources[i], type);

    SemanticOperation::Condition condition =
                        (SemanticOperation::Condition)step.m_condition;
    if (step.m_condition == SemanticTemplate::CONDITION_FROM_OPCODE)
        condition = m_context.getOpcodeCondition();

    switch (operation)
    {
    case SemanticOperation::OP_SET_FLAGS:
    {
        SemanticInstruction instruction(operation, type, SemanticOperand(),
                                        sources[0], sources[1], sources[2]);
        instruction.setFlags((SemanticOperation::FlagRecipe)step.m_flagRecipe,
                             step.m_flagMask);
        instruction.setCondition(condition);
        m_context.emit(instruction);
        return;
    }
    case SemanticOperation::OP_STORE:
    case SemanticOperation::OP_LABEL:
    case SemanticOperation::OP_BRANCH:
    case SemanticOperation::OP_BRANCH_CONDITIONAL:
    case SemanticOperation::OP_BRANCH_INDIRECT:
    case SemanticOperation::OP_CALL:
    case SemanticOperation::OP_RETURN:
    case SemanticOperation::OP_NOP:
        // The operations without a value
        m_context.emit(SemanticInstruction(operation, SemanticType(),
                                           SemanticOperand(), sources[0],
                                           sources[1], sources[2]));
        return;
    default:
        break;
    }

    // A move straight into an operand needs no temporary
    if ((operation == SemanticOperation::OP_MOVE) &&
        (step.m_destination.m_kind == SemanticTemplateReference::KIND_OPERAND))
    {
        writeReference(step.m_destination, sources[0]);
        return;
    }

    // The operations with a value. The result of a compare or a flags test is
    // a boolean
    const bool isBoolean = (operation == SemanticOperation::OP_COMPARE) ||
                           (operation == SemanticOperation::OP_TEST_FLAGS);
    SemanticOperand result = m_context.newTemporary(
                    isBoolean ? SemanticType::integer(1) : type);
    SemanticInstruction instruction(operation, type, result,
                                    sources[0], sources[1], sources[2]);
    instruction.setCondition(condition);
    m_context.emit(instruction);
    writeReference(step.m_destination, result);
}

void TemplateInstance::run(const SemanticTemplate& semanticTemplate)
{
    for (uint i = 0; i < semanticTemplate.m_stepsCount; i++)
        step(semanticTemplate.m_steps[i]);
}

/*
 * A locked template: the atomic read-modify-write of the memory operand 0,
 * and the steps of the template over the old value.
 *
 * add, sub, and, or, xor, inc and dec are a single atomic operation (sub and
 * dec add the negation). The others (adc, sbb, neg, not) read the memory,
 * compute the new value and compare-exchange it, and retry if the memory
 * changed in between.
 */
void instantiateLocked(const SemanticTemplate& semanticTemplate,
                       IA32LiftContext& context)
{
    const SemanticTemplateStep& first = semanticTemplate.m_steps[0];
    const SemanticType type = context.getOperandType(0);
    const SemanticOperand address = context.getAddress(0);
    LockedAccess locked;
    locked.m_isWritten = false;

    Op::Operation combine = Op::OP_NOP;
    bool isNegated = false;
    switch (first.m_operation)
    {
    case Op::OP_ADD:        combine = Op::OP_ADD; break;
    case Op::OP_SUBTRACT:   combine = Op::OP_ADD; isNegated = true; break;
    case Op::OP_AND:        combine = Op::OP_AND; break;
    case Op::OP_OR:         combine = Op::OP_OR; break;
    case Op::OP_XOR:        combine = Op::OP_XOR; break;
    default:
        break;
    }
    const bool isReadModifyWrite = (combine != Op::OP_NOP) &&
        (first.m_sources[0].m_kind == SemanticTemplateReference::KIND_OPERAND) &&
        (first.m_sources[0].m_index == 0);

    if (isReadModifyWrite)
    {
        // The operand of the operation: the other source of the first step
        SemanticOperand operand;
        switch (first.m_sources[1].m_kind)
        {
        case SemanticTemplateReference::KIND_OPERAND:
            operand = context.read(first.m_sources[1].m_index);
            break;
        case SemanticTemplateReference::KIND_IMMEDIATE:
            operand = context.getImmediate(first.m_sources[1].m_value, type);
            break;
        default:
            CHECK_FAIL();
        }
        if (isNegated)
            operand = context.emit(Op::OP_NEGATE, type, operand);

        locked.m_old = context.newTemporary(type);
        SemanticInstruction atomic(Op::OP_ATOMIC_RMW, type, locked.m_old,
                                   address, operand);
        atomic.setCombineOperation(combine);
        atomic.setMemoryOrder(Op::ORDER_SEQUENTIAL);
        atomic.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        context.emit(atomic);

        // The steps compute the flags from the old value. The memory is
        // already written
        TemplateInstance instance(context, &locked);
        instance.run(semanticTemplate);
        return;
    }

    // A compare-exchange loop
    SemanticOperand retry = context.newLabel();
    context.placeLabel(retry);
    locked.m_old = context.load(address, type);
    TemplateInstance instance(context, &locked);
    instance.run(semanticTemplate);
    CHECK(locked.m_isWritten);

    SemanticOperand previous = context.newTemporary(type);
    SemanticInstruction exchange(Op::OP_ATOMIC_COMPARE_EXCHANGE, type, previous,
                                 address, locked.m_old, locked.m_new);
    exchange.setMemoryOrder(Op::ORDER_SEQUENTIAL);
    exchange.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    context.emit(exchange);

    SemanticOperand isChanged = context.newTemporary(SemanticType::integer(1));
    SemanticInstruction compare(Op::OP_COMPARE, type, isChanged, previous,
                                locked.m_old);
    compare.setCondition(Op::COND_NOT_EQUAL);
    context.emit(compare);
    context.branchIf(isChanged, retry);
}

// Return true if the mnemonic can be locked when its memory operand is the
// destination (operand 0, or either operand of xchg)
bool isLockable(const char* name, const IA32LiftContext& context)
{
    static const char* names[] = {
        "add", "or", "adc", "sbb", "and", "sub", "xor", "inc", "dec", "not",
        "neg", "bts", "btr", "btc", "xadd", "cmpxchg", "cmpxchg8b",
        "cmpxchg8b/cmpxchg8b/cmpxchg16b"
    };
    for (uint i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0)
            return (context.getOperandsCount() > 0) &&
                   (context.getOperand(0).m_kind ==
                    IA32DecodedOperand::KIND_MEMORY);
    if (strcmp(name, "xchg") == 0)
    {
        for (uint i = 0; i < context.getOperandsCount(); i++)
            if (context.getOperand(i).m_kind == IA32DecodedOperand::KIND_MEMORY)
                return true;
    }
    return false;
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// IA32SemanticLifter
/////////////////////////////////////////////////////////////////////////////

IA32SemanticLifter::IA32SemanticLifter(OpcodeSubsystems::DisassemblerType type,
                                       SegmentModel segmentModel) :
    m_type(type),
    m_isFlat((segmentModel == SEGMENTS_FLAT) ||
             ((segmentModel == SEGMENTS_DEFAULT) &&
              (type != OpcodeSubsystems::DISASSEMBLER_INTEL_16))),
    m_architecture(ArchitectureDescriptionFactory::getArchitecture(type))
{
}

ArchitectureDescriptionPtr IA32SemanticLifter::getArchitecture() const
{
    return m_architecture;
}

namespace {

// Return true for the forms of mov, pop, jmp and call which are described in
// the segments table: they load or store a segment register, or are far
bool isSegmentForm(const OpcodeEntry& opcode)
{
    const char* name = opcode.m_opcodeName;
    if (strcmp(name, "mov") == 0)
        return (opcode.m_firstOperand == OPND_GP_SEGMENT_MODRM) ||
               (opcode.m_secondOperand == OPND_GP_SEGMENT_MODRM);
    if (strcmp(name, "pop") == 0)
        return (opcode.m_firstOperand == OPND_ES) ||
               (opcode.m_firstOperand == OPND_SS) ||
               (opcode.m_firstOperand == OPND_DS) ||
               (opcode.m_firstOperand == OPND_FS) ||
               (opcode.m_firstOperand == OPND_GS);
    if ((strcmp(name, "jmp") == 0) || (strcmp(name, "call") == 0))
        return (opcode.m_firstOperand == OPND_IMMEDIATE_OFFSET_FAR) ||
               (opcode.m_firstOperand == OPND_MODRM_FAR_OFFSET);
    return false;
}

} // namespace

const IA32SemanticEntry* IA32SemanticLifter::findEntry(const IA32Opcode& instruction)
{
    const OpcodeEntry* opcode = instruction.getOpcodeEntry();

    // The moves to the control and debug registers change the processor
    // state (paging, debugging): they have no semantics here
    if ((strcmp(opcode->m_opcodeName, "mov") == 0) &&
        ((opcode->m_firstOperand == OPND_CTRL_MODRM) ||
         (opcode->m_secondOperand == OPND_CTRL_MODRM) ||
         (opcode->m_firstOperand == OPND_DBG_MODRM) ||
         (opcode->m_secondOperand == OPND_DBG_MODRM)))
        return NULL;
    // The segment register numbers 6 and 7 don't exist
    if ((strcmp(opcode->m_opcodeName, "mov") == 0) &&
        ((opcode->m_firstOperand == OPND_GP_SEGMENT_MODRM) ||
         (opcode->m_secondOperand == OPND_GP_SEGMENT_MODRM)) &&
        (instruction.getModrm().m_bits.m_regOpcode >= NUMBER_OF_SEGMENTS_REGISTERS))
        return NULL;

    // An operand which is a memory reference only (lea, bound, les) encoded
    // as a register is an invalid opcode
    const IA32OpcodeDatastruct::MODRM modrm = instruction.getModrm();
    if ((modrm.m_bits.m_mod == 3) &&
        (instruction.getOpcodeEntry()->m_modrm != MODRM_NO_MODRM))
    {
        const OperandType operands[] = { opcode->m_firstOperand,
                                         opcode->m_secondOperand,
                                         opcode->m_thridOperand };
        for (uint i = 0; i < 3; i++)
            if ((operands[i] == OPND_MODRM_MEM) ||
                (operands[i] == OPND_MODRM_FAR_OFFSET))
                return NULL;
    }

    // The tables of the areas, see IA32Semantics.h. The forms of a mnemonic
    // of the integer table which touch a segment register (mov sreg, pop
    // sreg, the far jmp and call) are in the segments table only
    static const IA32SemanticEntry* const tables[] = {
        gAmd64SemanticTable,
        gIa32SemanticTable,
        gIa32SegmentsSemanticTable,
        gIa32X87SemanticTable,
        gIa32SimdSemanticTable
    };
    const uint operandsCount = IA32OperandDecoder::getOperandsCount(instruction);
    const bool isSegments = isSegmentForm(*opcode);
    // The table of 64 bit mode is searched first in 64 bit mode only
    const uint firstTable = isSegments ? 2 :
                            (IA32OperandDecoder::is64BitMode(instruction) ? 0 : 1);
    const uint lastTable = isSegments ? 3 :
                           (uint)(sizeof(tables) / sizeof(tables[0]));
    // The names of the 64 bit mode entries with a 64 bit form (movd/movd/movq,
    // cbw/cwde/cdqe) fall back to the name without the last form, which the
    // IA-32 tables have (movd/movd, cbw/cwde)
    char name[128];
    strncpy(name, opcode->m_opcodeName, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    for (;;)
    {
        for (uint i = firstTable; i < lastTable; i++)
        {
            for (const IA32SemanticEntry* entry = tables[i];
                 entry->m_template.m_name != NULL; entry++)
            {
                if ((entry->m_operandsCount != IA32SemanticEntry::ANY_OPERANDS_COUNT) &&
                    (entry->m_operandsCount != operandsCount))
                    continue;
                if (strcmp(entry->m_template.m_name, name) == 0)
                    return entry;
            }
        }
        char* slash = strrchr(name, '/');
        if (!IA32OperandDecoder::is64BitMode(instruction) || (slash == NULL))
            return NULL;
        *slash = 0;
    }
}

OpcodeClassification IA32SemanticLifter::classify(const OpcodePtr& instruction) const
{
    if (instruction->getType() != m_type)
        return OpcodeClassification();
    const IA32Opcode& ia32 = *(const IA32Opcode*)instruction.getPointer();
    const IA32SemanticEntry* entry = findEntry(ia32);
    if (entry == NULL)
        return OpcodeClassification();

    const SemanticTemplate& semanticTemplate = entry->m_template;
    const uint count = IA32OperandDecoder::getOperandsCount(ia32);
    uint32 properties = semanticTemplate.m_properties;
    uint32 flagsUndefined = semanticTemplate.m_flagsUndefined;

    // The memory the operands access
    bool hasMemoryOperand = false;
    for (uint i = 0; i < count; i++)
        if (IA32OperandDecoder::decode(ia32, i).m_kind ==
            IA32DecodedOperand::KIND_MEMORY)
            hasMemoryOperand = true;
    const bool isImplicitMemory =
        (semanticTemplate.m_category == OpcodeClassification::CATEGORY_STACK) ||
        (semanticTemplate.m_category == OpcodeClassification::CATEGORY_STRING) ||
        // maskmovq, maskmovdqu: the store to ds:edi
        (semanticTemplate.m_category == OpcodeClassification::CATEGORY_STORE) ||
        (semanticTemplate.m_category == OpcodeClassification::CATEGORY_CALL) ||
        (semanticTemplate.m_category == OpcodeClassification::CATEGORY_RETURN);
    uint32 units = semanticTemplate.m_units;
    if (!hasMemoryOperand && !isImplicitMemory)
    {
        properties&= ~(OpcodeClassification::PROPERTY_READS_MEMORY |
                       OpcodeClassification::PROPERTY_WRITES_MEMORY |
                       OpcodeClassification::PROPERTY_MAY_FAULT);
        units&= ~(OpcodeClassification::UNIT_LOAD |
                  OpcodeClassification::UNIT_STORE);
    } else
    {
        properties|= OpcodeClassification::PROPERTY_MAY_FAULT;
    }

    // The flags properties follow the flags sets
    if (semanticTemplate.m_flagsRead != 0)
        properties|= OpcodeClassification::PROPERTY_READS_FLAGS;
    if (semanticTemplate.m_flagsWritten != 0)
        properties|= OpcodeClassification::PROPERTY_WRITES_FLAGS;
    if (semanticTemplate.m_flagsUndefined != 0)
        properties|= OpcodeClassification::PROPERTY_UNDEFINED_FLAGS;

    // The prefixes
    if (IA32OperandDecoder::hasPrefix(ia32, PREFIX_LOCK))
        properties|= OpcodeClassification::PROPERTY_ATOMIC |
                     OpcodeClassification::PROPERTY_SERIALIZING;
    if ((properties & OpcodeClassification::PROPERTY_REPEATED) != 0)
    {
        if (!IA32OperandDecoder::hasPrefix(ia32, 0xF2) &&
            !IA32OperandDecoder::hasPrefix(ia32, 0xF3))
            properties&= ~OpcodeClassification::PROPERTY_REPEATED;
    }

    // The overflow flag of a shift or a rotate is defined for a count of 1
    // only
    if ((semanticTemplate.m_category == OpcodeClassification::CATEGORY_SHIFT) &&
        (count > 1))
    {
        const IA32DecodedOperand countOperand =
                                IA32OperandDecoder::decode(ia32, count - 1);
        const int64 countMask =
                (IA32OperandDecoder::decode(ia32, 0).m_bits == 64) ? 0x3F : 0x1F;
        if ((countOperand.m_kind == IA32DecodedOperand::KIND_IMMEDIATE) &&
            ((countOperand.m_immediate & countMask) == 1))
            flagsUndefined&= ~(uint32)SemanticFlags::FLAG_OVERFLOW;
    }

    OpcodeClassification result((OpcodeClassification::Category)semanticTemplate.m_category,
                                units, properties);
    result.setFlags(semanticTemplate.m_flagsRead,
                    semanticTemplate.m_flagsWritten, flagsUndefined);
    result.setFloatStatusUndefined(semanticTemplate.m_floatStatusUndefined);
    // The size of the data: the first operand, or the operand size. The
    // vector and the floating point entries give the element
    if (entry->m_dataKind != SemanticType::KIND_VOID)
        result.setDataType(getEntryDataType(*entry, ia32));
    else if (count > 0)
        result.setDataType(SemanticType::integer(
                    IA32OperandDecoder::decode(ia32, 0).m_bits));
    else
        result.setDataType(SemanticType::integer(
                    IA32OperandDecoder::getOperandBits(ia32)));

    // The x87 instructions work on extended precision numbers, except for
    // the memory operands of the converting loads and stores
    if ((semanticTemplate.m_category == OpcodeClassification::CATEGORY_FLOAT) &&
        (semanticTemplate.m_name[0] == 'f'))
    {
        SemanticType dataType = SemanticType::floating(
                                        SemanticType::FLOAT_EXTENDED);
        if ((count > 0) && (IA32OperandDecoder::decode(ia32, 0).m_kind ==
                            IA32DecodedOperand::KIND_MEMORY))
        {
            uint bits = IA32OperandDecoder::decode(ia32, 0).m_bits;
            // The "dword" operands have the operand size in the decoder
            if (bits < 64)
            {
                const uint8 opcode = IA32OperandDecoder::getLastOpcodeByte(ia32);
                bits = ((opcode == 0xDE) || (opcode == 0xDF)) ? 16 : 32;
            }
            const char* mnemonic = semanticTemplate.m_name;
            if ((mnemonic[1] == 'i') && (mnemonic[2] != 'n'))
                dataType = SemanticType::integer(bits);
            else if (bits == 32)
                dataType = SemanticType::floating(SemanticType::FLOAT_SINGLE);
            else if (bits == 64)
                dataType = SemanticType::floating(SemanticType::FLOAT_DOUBLE);
        }
        result.setDataType(dataType);
    }

    // The change of the stack pointer
    const int bytes = (int)IA32OperandDecoder::getOperandBits(ia32) / 8;
    const char* name = ia32.getOpcodeEntry()->m_opcodeName;
    if ((strcmp(name, "push") == 0) || (strcmp(name, "pushf#d") == 0) ||
        (strcmp(name, "call") == 0))
        result.setStackDelta(bytes);
    else if ((strcmp(name, "pop") == 0) || (strcmp(name, "popf#d") == 0))
        result.setStackDelta(-bytes);
    else if (strcmp(name, "pusha#d") == 0)
        result.setStackDelta(8 * bytes);
    else if (strcmp(name, "popa#d") == 0)
        result.setStackDelta(-8 * bytes);
    else if ((strcmp(name, "ret") == 0) && (count == 0))
        result.setStackDelta(-bytes);
    return result;
}

bool IA32SemanticLifter::lift(const OpcodePtr& instruction,
                              SemanticBlock& block) const
{
    CHECK(block.getArchitecture() == m_type);

    ProcessorAddress address(gNullPointerProcessorAddress);
    instruction->getOpcodeAddress(address);
    block.beginGuestInstruction(address, instruction->getOpcodeSize());

    // An invalid opcode has no semantics
    const IA32SemanticEntry* entry = NULL;
    if (instruction->getType() == m_type)
        entry = findEntry(*(const IA32Opcode*)instruction.getPointer());
    if (entry == NULL)
    {
        block.append(SemanticInstruction(Op::OP_UNDEFINED, SemanticType(),
                                         SemanticOperand()));
        return false;
    }

    const IA32Opcode& ia32 = *(const IA32Opcode*)instruction.getPointer();
    IA32LiftContext context(ia32, block, m_isFlat);

    // The lock prefix of an instruction which can't be locked is an invalid
    // opcode
    const bool isLocked = IA32OperandDecoder::hasPrefix(ia32, PREFIX_LOCK);
    if (isLocked && !isLockable(entry->m_template.m_name, context))
    {
        context.trap(Op::TRAP_FAULT, 6);
        return true;
    }

    if (entry->m_lift != NULL)
        entry->m_lift(context);
    else if (isLocked)
        instantiateLocked(entry->m_template, context);
    else
        instantiate(entry->m_template, context);

    // Every MMX instruction, even if it only reads an mm register, sets the
    // top of the x87 stack to 0 and marks all the registers valid
    if (isMmxInstruction(ia32))
    {
        context.move(context.getRegister(IA32_REG_FPU_TOP),
                     context.getImmediate(0, SemanticType::integer(3)));
        context.move(context.getRegister(IA32_REG_FPU_TAG),
                     context.getImmediate(0xFF, SemanticType::integer(8)));
    }
    return true;
}

void IA32SemanticLifter::instantiate(const SemanticTemplate& semanticTemplate,
                                     IA32LiftContext& context) const
{
    TemplateInstance instance(context, NULL);
    instance.run(semanticTemplate);
}
