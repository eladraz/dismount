/*
 * SemanticPrinter.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/list.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticPrinter.h"

namespace {

// Return the name of a condition of OP_COMPARE and OP_SET_FLAGS
const char* getConditionName(SemanticOperation::Condition condition)
{
    switch (condition)
    {
    case SemanticOperation::COND_NONE: return "COND_NONE";
    case SemanticOperation::COND_ALWAYS: return "COND_ALWAYS";
    case SemanticOperation::COND_NEVER: return "COND_NEVER";
    case SemanticOperation::COND_EQUAL: return "COND_EQUAL";
    case SemanticOperation::COND_NOT_EQUAL: return "COND_NOT_EQUAL";
    case SemanticOperation::COND_SIGNED_LESS: return "COND_SIGNED_LESS";
    case SemanticOperation::COND_SIGNED_LESS_EQUAL: return "COND_SIGNED_LESS_EQUAL";
    case SemanticOperation::COND_SIGNED_GREATER: return "COND_SIGNED_GREATER";
    case SemanticOperation::COND_SIGNED_GREATER_EQUAL: return "COND_SIGNED_GREATER_EQUAL";
    case SemanticOperation::COND_UNSIGNED_LESS: return "COND_UNSIGNED_LESS";
    case SemanticOperation::COND_UNSIGNED_LESS_EQUAL: return "COND_UNSIGNED_LESS_EQUAL";
    case SemanticOperation::COND_UNSIGNED_GREATER: return "COND_UNSIGNED_GREATER";
    case SemanticOperation::COND_UNSIGNED_GREATER_EQUAL: return "COND_UNSIGNED_GREATER_EQUAL";
    case SemanticOperation::COND_NEGATIVE: return "COND_NEGATIVE";
    case SemanticOperation::COND_NOT_NEGATIVE: return "COND_NOT_NEGATIVE";
    case SemanticOperation::COND_OVERFLOW: return "COND_OVERFLOW";
    case SemanticOperation::COND_NOT_OVERFLOW: return "COND_NOT_OVERFLOW";
    case SemanticOperation::COND_CARRY: return "COND_CARRY";
    case SemanticOperation::COND_NOT_CARRY: return "COND_NOT_CARRY";
    case SemanticOperation::COND_PARITY_EVEN: return "COND_PARITY_EVEN";
    case SemanticOperation::COND_PARITY_ODD: return "COND_PARITY_ODD";
    case SemanticOperation::COND_FLOAT_ORDERED: return "COND_FLOAT_ORDERED";
    case SemanticOperation::COND_FLOAT_UNORDERED: return "COND_FLOAT_UNORDERED";
    case SemanticOperation::COND_FLOAT_EQUAL: return "COND_FLOAT_EQUAL";
    case SemanticOperation::COND_FLOAT_NOT_EQUAL: return "COND_FLOAT_NOT_EQUAL";
    case SemanticOperation::COND_FLOAT_LESS: return "COND_FLOAT_LESS";
    case SemanticOperation::COND_FLOAT_LESS_EQUAL: return "COND_FLOAT_LESS_EQUAL";
    case SemanticOperation::COND_FLOAT_GREATER: return "COND_FLOAT_GREATER";
    case SemanticOperation::COND_FLOAT_GREATER_EQUAL: return "COND_FLOAT_GREATER_EQUAL";
    case SemanticOperation::COND_FLOAT_UNORDERED_OR_EQUAL: return "COND_FLOAT_UNORDERED_OR_EQUAL";
    case SemanticOperation::COND_FLOAT_UNORDERED_OR_LESS: return "COND_FLOAT_UNORDERED_OR_LESS";
    case SemanticOperation::COND_FLOAT_UNORDERED_OR_LESS_EQUAL: return "COND_FLOAT_UNORDERED_OR_LESS_EQUAL";
    }
    return "COND_UNKNOWN";
}

// Return the name of a floating point format, for the types
const char* getFloatName(SemanticType::FloatFormat format)
{
    switch (format)
    {
    case SemanticType::FLOAT_BFLOAT16: return "bf16";
    case SemanticType::FLOAT_HALF: return "f16";
    case SemanticType::FLOAT_SINGLE: return "f32";
    case SemanticType::FLOAT_DOUBLE: return "f64";
    case SemanticType::FLOAT_EXTENDED: return "f80";
    case SemanticType::FLOAT_QUAD: return "f128";
    case SemanticType::FLOAT_E4M3: return "e4m3";
    case SemanticType::FLOAT_E5M2: return "e5m2";
    case SemanticType::FLOAT_NONE: break;
    }
    return "f?";
}

// Return the text of a scalar type: i32, f64, void
cString formatScalarType(const SemanticType& type)
{
    switch (type.getKind())
    {
    case SemanticType::KIND_VOID:
        return cString("void");
    case SemanticType::KIND_FLOAT:
        return cString(getFloatName(type.getFloatFormat()));
    case SemanticType::KIND_MASK:
        return cString("k") + cString((int64)type.getLanes());
    case SemanticType::KIND_INTEGER:
        break;
    }
    return cString("i") + cString((int64)type.getElementBits());
}

// Return the text of a type: i32, v4f32, nxv4i32 (a scalable vector), k16
cString formatType(const SemanticType& type)
{
    if (type.isVector())
    {
        const cString prefix = type.isScalable() ? cString("nxv") : cString("v");
        return prefix + cString((int64)type.getLanes()) +
               formatScalarType(type.getElementType());
    }
    return formatScalarType(type);
}

// Return a number in hexadecimal, with 0x and no leading zeros
cString formatHex(uint64 value)
{
    const char digits[] = "0123456789abcdef";
    char text[17];
    uint count = 0;
    do
    {
        text[count++] = digits[value & 0xF];
        value >>= 4;
    } while (value != 0);
    cString result("0x");
    while (count > 0)
    {
        count--;
        result += cString(text[count]);
    }
    return result;
}

// Return the name of a register, by its id
cString formatRegister(uint registerId,
                       const ArchitectureDescription& architecture)
{
    if (registerId >= architecture.getRegistersCount())
        return cString("r") + cString((int64)registerId);
    return cString(architecture.getRegister(registerId).m_name);
}

// Return the text of an operand which is read (a source, or a destination
// without its type)
cString formatOperand(const SemanticOperand& operand,
                      const ArchitectureDescription& architecture)
{
    switch (operand.getKind())
    {
    case SemanticOperand::KIND_NONE:
        return cString("_");
    case SemanticOperand::KIND_REGISTER:
        return formatRegister(operand.getIndex(), architecture);
    case SemanticOperand::KIND_TEMPORARY:
        return cString("t") + cString((int64)operand.getIndex());
    case SemanticOperand::KIND_IMMEDIATE:
        return cString((int64)operand.getValue());
    case SemanticOperand::KIND_GUEST_ADDRESS:
        return formatHex((uint64)operand.getValue());
    case SemanticOperand::KIND_LABEL:
        return cString("L") + cString((int64)operand.getIndex());
    }
    return cString("?");
}

// Return the text of a destination: a register, or a temporary with its type
cString formatDestination(const SemanticInstruction& instruction,
                          const ArchitectureDescription& architecture)
{
    const SemanticOperand& destination = instruction.getDestination();
    if (destination.getKind() == SemanticOperand::KIND_TEMPORARY)
        return formatOperand(destination, architecture) + cString(":") +
               formatType(destination.getType());
    return formatOperand(destination, architecture);
}

// Return the text of one IR instruction, without its guest address
cString formatInstruction(const SemanticInstruction& instruction,
                          const ArchitectureDescription& architecture)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    const SemanticOperand& destination = instruction.getDestination();

    if (operation == SemanticOperation::OP_LABEL)
    {
        return formatOperand(instruction.getSource(0), architecture) + cString(":");
    }

    cString text;
    const bool hasDestination = (destination.getKind() != SemanticOperand::KIND_NONE);
    if (hasDestination)
        text = formatDestination(instruction, architecture) + cString(" = ");

    text += cString(SemanticOperation::getName(operation));
    if (!hasDestination && !instruction.getType().isVoid())
        text += cString(":") + formatType(instruction.getType());

    const SemanticOperation::Condition condition = instruction.getCondition();
    if (condition != SemanticOperation::COND_NONE)
        text += cString("[") + cString(getConditionName(condition)) + cString("]");

    const uint sources = SemanticOperation::getSourcesCount(operation);
    for (uint i = 0; i < sources; i++)
    {
        text += (i == 0) ? cString(" ") : cString(", ");
        text += formatOperand(instruction.getSource(i), architecture);
    }

    if (instruction.getAttributes() != SemanticInstruction::ATTRIBUTE_NONE)
        text += cString(" {attributes ") +
                formatHex(instruction.getAttributes()) + cString("}");
    return text;
}

} // namespace

void SemanticPrinter::print(const SemanticBlock& block,
                            const ArchitectureDescription& architecture,
                            cStringerStream& output)
{
    const cList<SemanticBlock::GuestInstruction>& guests =
                                        block.getGuestInstructions();
    cList<SemanticBlock::GuestInstruction>::iterator guest = guests.begin();

    // The guest instruction of the IR which is printed. Before the first
    // marker (or without markers) the IR has no guest address
    bool hasAddress = false;
    uint64 address = 0;
    uint index = 0;
    for (cList<SemanticInstruction>::iterator i =
                                    block.getInstructions().begin();
         i != block.getInstructions().end(); i++, index++)
    {
        // Print the headers of the guest instructions which start here (an
        // instruction without IR has a header too)
        while ((guest != guests.end()) && ((*guest).m_firstInstruction <= index))
        {
            address = (*guest).m_address.getAddress();
            hasAddress = true;
            output << "; guest " << formatHex(address) << " size "
                   << (int64)(*guest).m_size << endl;
            guest++;
        }

        const SemanticInstruction& instruction = *i;
        output << (hasAddress ? formatHex(address) : cString("")) << ": "
               << formatInstruction(instruction, architecture) << endl;
    }

    // The headers of the guest instructions after the last IR instruction
    while (guest != guests.end())
    {
        output << "; guest " << formatHex((*guest).m_address.getAddress())
               << " size " << (int64)(*guest).m_size << endl;
        guest++;
    }

    if (!(block.getFallThroughAddress() == gNullPointerProcessorAddress))
        output << "; fall-through "
               << formatHex(block.getFallThroughAddress().getAddress()) << endl;
}
