/*
 * AMD64LoweringWide.cpp
 *
 * The lowering of the wide values: the 128 bit values (a vector register, a
 * 128 bit temporary: a 16 byte slot in the state) and the bit moves between
 * values of the same size (BITCAST). The scratch registers are rax, rcx and
 * xmm0.
 *
 * Widths: a value is 1 to 64 bits (in a slot of the smallest of 8/16/32/64
 * bits which holds it: the bits above are masked when loaded) or 128 bits (a
 * 16 byte slot). A vector is refused by the zero extension (its lanes are
 * extended one by one by the interpreter).
 *
 * Coverage. The operations are DONE in the table of AMD64Lowering.cpp:
 *   OP_MOVE           128 <- 128 (xmm0); a float of 8..64 bits (its bits)
 *   OP_ZERO_EXTEND    128 <- 1..64 scalars (the high qword is zeroed)
 *   OP_TRUNCATE       1..64 <- 128 (the low bytes)
 *   OP_LOAD           128, [base+rax]; a float of 8..64 bits
 *   OP_STORE          128; a float of 8..64 bits
 *   OP_BITCAST        any size of 1..128 bits, the bits are copied
 *   OP_CONCAT         the parts of 64 + 64 -> 128, and the sums up to 64 bits
 *   OP_EXTRACT        a constant position: 1..64 bits of 1..128 bits (a lane
 *                     index for a vector source); 128 bits at position 0
 *   OP_INSERT         a constant position: into 1..64 bits; into 128 bits at
 *                     a byte position, of 8..64 bits
 *   OP_BROADCAST      a scalar into a vector of 8..64 bit lanes, up to 128 bits
 *   OP_AND OP_OR OP_XOR OP_NOT OP_AND_NOT   of 128 bits (two qwords)
 * Not implemented (the lowering throws): a constant of 128 bits, EXTRACT of
 * 128 bits at a position other than 0, INSERT into 128 bits at a bit position
 * which is not a byte position.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
// After the precompiled header: MSVC ignores everything above it
#include <stdio.h>
#include "AMD64BlockLowering.h"

// The bits a value of 'bits' occupies in its slot: the smallest of 8, 16,
// 32, 64 or 128 bits which holds it. A value narrower than its storage is
// masked when it is loaded (see loadPiece())
static uint getStorageBits(uint bits)
{
    if (bits <= 8)
        return 8;
    if (bits <= 16)
        return 16;
    if (bits <= 32)
        return 32;
    return (bits <= 64) ? 64 : 128;
}

uint amd64lowering::BlockLowering::getWideBits(const SemanticType& type) const
{
    const uint bits = type.getTotalBits();
    const bool isValue = (type.getKind() == SemanticType::KIND_INTEGER) ||
                         (type.getKind() == SemanticType::KIND_FLOAT);
    // The sizes of the wide values: 1 to 64 bits (a narrow value: the bits of
    // its slot are masked) and 128 bits
    if (!isValue || type.isScalable() ||
        ((bits == 0) || ((bits > 64) && (bits != 128))))
    {
        char text[100];
        snprintf(text, sizeof(text), "the type with %u bits (kind %u) is not "
                 "supported", (unsigned)bits, (unsigned)type.getKind());
        fail(text);
    }
    return bits;
}

cString amd64lowering::BlockLowering::getSlotMemory(
                                const SemanticOperand& operand, uint extra,
                                uint bits) const
{
    char text[100];
    snprintf(text, sizeof(text), "%s [%s+0x%x]",
             (bits == 128) ? "oword" : getSizeKeyword(bits),
             m_state.getASCIIstring().getBuffer(),
             (unsigned)(getSlotOffset(operand) + extra));
    return cString(text);
}

// The low 'bits' bits of a value (all ones from 64)
static uint64 getMaskOfBits(uint bits)
{
    return (bits >= 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
}

// The bits [shift, shift + bits) of the 128 bit value high:low
static uint64 getPiece128(uint64 low, uint64 high, uint shift, uint bits)
{
    uint64 value;
    if (shift >= 128)
        value = 0;
    else if (shift == 0)
        value = low;
    else if (shift < 64)
        value = (low >> shift) | (high << (64 - shift));
    else if (shift == 64)
        value = high;
    else
        value = high >> (shift - 64);
    return value & getMaskOfBits(bits);
}

void amd64lowering::BlockLowering::loadPiece(Scratch scratch,
                                             const SemanticOperand& operand,
                                             uint extra, uint bits)
{
    char text[160];
    if (isConstant(operand))
    {
        // A 128 bit constant is the immediate sign extended to 128 bits (the
        // interpreter's read()): its high qword is all ones when it is negative.
        // A narrower constant is zero above its width
        const uint total = operand.getType().getTotalBits();
        uint64 value;
        if ((total == 128) || (extra != 0))
        {
            const uint64 low = (total >= 64) ? (uint64)operand.getValue() :
                                               getConstant(operand, total);
            const uint64 high = ((total == 128) && (operand.getValue() < 0)) ?
                                ~(uint64)0 : 0;
            value = getPiece128(low, high, extra * 8, bits);
        } else
        {
            value = getConstant(operand, bits);
        }
        snprintf(text, sizeof(text), "mov %s, 0x%llx", getRegister(scratch, 64),
                 (unsigned long long)value);
        line(text);
        return;
    }
    const uint storage = getStorageBits(bits);
    const cString memory = getSlotMemory(operand, extra, storage);
    const cSArray<char> memoryAscii = memory.getASCIIstring();
    if (storage == 64)
        snprintf(text, sizeof(text), "mov %s, %s", getRegister(scratch, 64),
                 memoryAscii.getBuffer());
    else if (storage == 32)
        snprintf(text, sizeof(text), "mov %s, %s", getRegister(scratch, 32),
                 memoryAscii.getBuffer());
    else
        snprintf(text, sizeof(text), "movzx %s, %s", getRegister(scratch, 32),
                 memoryAscii.getBuffer());
    line(text);
    // A value narrower than its storage: the bits above it are cleared
    if (bits == 1)
    {
        snprintf(text, sizeof(text), "and %s, 1", getRegister(scratch, 32));
        line(text);
    } else if (bits < storage)
    {
        // (rdx is not a scratch of the loads of the callers)
        snprintf(text, sizeof(text), "mov rdx, 0x%llx",
                 (unsigned long long)(((uint64)1 << bits) - 1));
        line(text);
        snprintf(text, sizeof(text), "and %s, rdx", getRegister(scratch, 64));
        line(text);
    }
}

void amd64lowering::BlockLowering::storePiece(const SemanticOperand& destination,
                                              uint extra, Scratch scratch,
                                              uint bits)
{
    const uint storage = getStorageBits(bits);
    cString text("mov ");
    text += getSlotMemory(destination, extra, storage);
    text += cString(", ");
    text += cString(getRegister(scratch, storage));
    line(text);
}

void amd64lowering::BlockLowering::copySlot(const SemanticOperand& destination,
                                            const SemanticOperand& source,
                                            uint bits)
{
    if (bits != 128)
    {
        loadPiece(SCRATCH_A, source, 0, bits);
        storePiece(destination, 0, SCRATCH_A, bits);
        return;
    }
    if (isConstant(source))
    {
        // The halves go through rcx (a caller may hold a value in rax)
        for (uint extra = 0; extra < 16; extra += 8)
        {
            loadPiece(SCRATCH_C, source, extra, 64);
            storePiece(destination, extra, SCRATCH_C, 64);
        }
        return;
    }
    cString fetch("movdqu xmm0, ");
    fetch += getSlotMemory(source, 0, 128);
    line(fetch);
    cString spill("movdqu ");
    spill += getSlotMemory(destination, 0, 128);
    spill += cString(", xmm0");
    line(spill);
}

void amd64lowering::BlockLowering::lowerWideLoad(
                                    const SemanticInstruction& instruction)
{
    // The guest address of the access, then the 16 bytes of the memory
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    char text[100];
    snprintf(text, sizeof(text), "movdqu xmm0, oword [%s+rax]",
             m_base.getASCIIstring().getBuffer());
    line(text);
    cString spill("movdqu ");
    spill += getSlotMemory(instruction.getDestination(), 0, 128);
    spill += cString(", xmm0");
    line(spill);
}

void amd64lowering::BlockLowering::lowerWideStore(
                                    const SemanticInstruction& instruction)
{
    const SemanticOperand& value = instruction.getSource(1);
    if (getWideBits(value.getType()) != 128)
        fail("the stored value is not 128 bits");
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    if (isConstant(value))
    {
        // The two qwords through rcx, the second one after the first
        for (uint extra = 0; extra < 16; extra += 8)
        {
            if (extra != 0)
                line("add rax, 8");
            loadPiece(SCRATCH_C, value, extra, 64);
            char text[100];
            snprintf(text, sizeof(text), "mov %s [%s+rax], rcx",
                     getSizeKeyword(64), m_base.getASCIIstring().getBuffer());
            line(text);
        }
        return;
    }
    cString fetch("movdqu xmm0, ");
    fetch += getSlotMemory(value, 0, 128);
    line(fetch);
    char text[100];
    snprintf(text, sizeof(text), "movdqu oword [%s+rax], xmm0",
             m_base.getASCIIstring().getBuffer());
    line(text);
}

void amd64lowering::BlockLowering::lowerWideConcat(
                                    const SemanticInstruction& instruction)
{
    // d = high:low (the low part is the first source)
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& low = instruction.getSource(0);
    const SemanticOperand& high = instruction.getSource(1);
    const uint lowBits = getWideBits(low.getType());
    const uint highBits = getWideBits(high.getType());
    const uint bits = getWideBits(destination.getType());
    if (lowBits + highBits != bits)
        fail("the parts of the concatenation are not its size");
    // Both parts are read before the destination is written: it may be one
    // of the sources
    if (bits == 128)
    {
        if ((lowBits != 64) || (highBits != 64))
            fail("a concatenation of 128 bits is two parts of 64 bits");
        loadPiece(SCRATCH_A, low, 0, 64);
        loadPiece(SCRATCH_C, high, 0, 64);
        storePiece(destination, 0, SCRATCH_A, 64);
        storePiece(destination, 8, SCRATCH_C, 64);
        return;
    }
    loadPiece(SCRATCH_A, low, 0, lowBits);
    loadPiece(SCRATCH_C, high, 0, highBits);
    char text[40];
    snprintf(text, sizeof(text), "shl rcx, %u", (unsigned)lowBits);
    line(text);
    line("or rax, rcx");
    storePiece(destination, 0, SCRATCH_A, bits);
}

void amd64lowering::BlockLowering::zeroQword(const SemanticOperand& destination,
                                             uint extra)
{
    cString text("mov ");
    text += getSlotMemory(destination, extra, 64);
    text += cString(", 0");
    line(text);
}

uint64 amd64lowering::BlockLowering::getWidePosition(
                                    const SemanticOperand& position,
                                    uint scale) const
{
    if (!isConstant(position))
        fail("the position is not a constant");
    // (the interpreter reads the position as a 32 bit number)
    return (uint64)(uint32)getConstant(position, 64) * scale;
}

void amd64lowering::BlockLowering::lowerWideExtract(
                                    const SemanticInstruction& instruction)
{
    // d = the bits of s0 starting at bit s1. A vector s0 and a scalar d: s1 is
    // the lane index (see SemanticOperation.h)
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& source = instruction.getSource(0);
    const uint destBits = getWideBits(destination.getType());
    const uint sourceBits = getWideBits(source.getType());
    const uint scale = (source.getType().isVector() &&
                        !destination.getType().isVector()) ?
                        source.getType().getElementBits() : 1;
    const uint64 position = getWidePosition(instruction.getSource(1), scale);
    if (destBits == 128)
    {
        // A 128 bit value from the bit 0 of its source (zero extended), or
        // all zeros past the source
        if (position >= sourceBits)
        {
            zeroQword(destination, 0);
            zeroQword(destination, 8);
            return;
        }
        if (position != 0)
            fail("an extraction of 128 bits at a position other than 0 is not implemented");
        if (sourceBits == 128)
        {
            copySlot(destination, source, 128);
            return;
        }
        loadPiece(SCRATCH_A, source, 0, sourceBits);
        storePiece(destination, 0, SCRATCH_A, 64);
        zeroQword(destination, 8);
        return;
    }
    if (position >= sourceBits)
    {
        line("xor eax, eax");
        storePiece(destination, 0, SCRATCH_A, destBits);
        return;
    }
    if (isConstant(source) && (sourceBits <= 64))
    {
        char text[80];
        const uint64 value = (getConstant(source, sourceBits) >> position) &
                             getMaskOfBits(destBits);
        snprintf(text, sizeof(text), "mov rax, 0x%llx", (unsigned long long)value);
        line(text);
        storePiece(destination, 0, SCRATCH_A, destBits);
        return;
    }
    // A byte aligned field of a whole storage: read from the memory of the
    // source, the bytes of the field
    const bool isWhole = (destBits == 8) || (destBits == 16) ||
                         (destBits == 32) || (destBits == 64);
    if (isWhole && ((position % 8) == 0) && (position + destBits <= sourceBits))
    {
        loadPiece(SCRATCH_A, source, (uint)(position / 8), destBits);
        storePiece(destination, 0, SCRATCH_A, destBits);
        return;
    }
    // Otherwise the bits are shifted down from the qword(s) of the source,
    // then masked
    char text[80];
    if (sourceBits <= 64)
    {
        loadPiece(SCRATCH_A, source, 0, sourceBits);
        if (position > 0)
        {
            snprintf(text, sizeof(text), "shr rax, %u", (unsigned)position);
            line(text);
        }
    } else if (position < 64)
    {
        // The low qword, and the bits of the high qword that the field needs
        loadPiece(SCRATCH_A, source, 0, 64);
        if (position > 0)
        {
            snprintf(text, sizeof(text), "shr rax, %u", (unsigned)position);
            line(text);
        }
        if (position + destBits > 64)
        {
            loadPiece(SCRATCH_C, source, 8, 64);
            snprintf(text, sizeof(text), "shl rcx, %u", (unsigned)(64 - position));
            line(text);
            line("or rax, rcx");
        }
    } else
    {
        loadPiece(SCRATCH_A, source, 8, 64);
        if (position > 64)
        {
            snprintf(text, sizeof(text), "shr rax, %u", (unsigned)(position - 64));
            line(text);
        }
    }
    if (destBits < 64)
    {
        snprintf(text, sizeof(text), "mov rdx, 0x%llx",
                 (unsigned long long)getMaskOfBits(destBits));
        line(text);
        line("and rax, rdx");
    }
    storePiece(destination, 0, SCRATCH_A, destBits);
}

void amd64lowering::BlockLowering::lowerWideInsert(
                                    const SemanticInstruction& instruction)
{
    // d = s0 with s1 written at bit s2. A vector d and a scalar s1: s2 is the
    // lane index (see SemanticOperation.h)
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& base = instruction.getSource(0);
    const SemanticOperand& inserted = instruction.getSource(1);
    const uint destBits = getWideBits(destination.getType());
    const uint insertedBits = getWideBits(inserted.getType());
    if (getWideBits(base.getType()) != destBits)
        fail("the value and the result are not the same size");
    const uint scale = (destination.getType().isVector() &&
                        !inserted.getType().isVector()) ?
                        destination.getType().getElementBits() : 1;
    const uint64 position = getWidePosition(instruction.getSource(2), scale);
    if (destBits == 128)
    {
        if (position >= 128)
        {
            copySlot(destination, base, 128);
            return;
        }
        if ((insertedBits == 128) && (position == 0))
        {
            copySlot(destination, inserted, 128);
            return;
        }
        // The inserted value is read first: it may be the destination
        const bool isWhole = (insertedBits == 8) || (insertedBits == 16) ||
                             (insertedBits == 32) || (insertedBits == 64);
        if (!isWhole || ((position % 8) != 0) || (position + insertedBits > 128))
            fail("an insertion into 128 bits is implemented for the whole bytes only");
        loadPiece(SCRATCH_A, inserted, 0, insertedBits);
        copySlot(destination, base, 128);
        storePiece(destination, (uint)(position / 8), SCRATCH_A, insertedBits);
        return;
    }
    if (position >= destBits)
    {
        copySlot(destination, base, destBits);
        return;
    }
    // The bits of the inserted value which land in the destination
    const uint loadedBits = (insertedBits > 64) ? 64 : insertedBits;
    const uint count = (position + loadedBits > destBits) ?
                       (uint)(destBits - position) : loadedBits;
    loadPiece(SCRATCH_A, base, 0, destBits);
    loadPiece(SCRATCH_C, inserted, 0, loadedBits);
    char text[80];
    if (count < loadedBits)
    {
        snprintf(text, sizeof(text), "mov rdx, 0x%llx",
                 (unsigned long long)getMaskOfBits(count));
        line(text);
        line("and rcx, rdx");
    }
    if (position > 0)
    {
        snprintf(text, sizeof(text), "shl rcx, %u", (unsigned)position);
        line(text);
    }
    // The field of the destination is cleared, then the bits are or-ed in
    const uint64 field = getMaskOfBits(count) << position;
    const uint64 keep = ~field;
    if (keep == 0)
    {
        line("xor eax, eax");
    } else
    {
        snprintf(text, sizeof(text), "mov rdx, 0x%llx", (unsigned long long)keep);
        line(text);
        line("and rax, rdx");
    }
    line("or rax, rcx");
    storePiece(destination, 0, SCRATCH_A, destBits);
}

void amd64lowering::BlockLowering::lowerWideBroadcast(
                                    const SemanticInstruction& instruction)
{
    // d = every lane set to the low bits of the scalar s0
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& scalar = instruction.getSource(0);
    const SemanticType& type = destination.getType();
    const uint destBits = getWideBits(type);
    const uint elementBits = type.getElementBits();
    const bool isElement = (elementBits == 8) || (elementBits == 16) ||
                           (elementBits == 32) || (elementBits == 64);
    if (!isElement || (type.getLanes() * elementBits != destBits))
        fail("the broadcast of these lanes is not implemented");
    // The low lane is loaded (zero extended when the scalar is narrower)
    const uint scalarBits = getWideBits(scalar.getType());
    loadPiece(SCRATCH_A, scalar, 0, (scalarBits < elementBits) ? scalarBits :
                                                                 elementBits);
    if (elementBits < 64)
    {
        // The lane repeated in the qword: a multiplication by 0x..0101 of the
        // lane (no carries between the copies)
        uint64 repeat = 0;
        for (uint shift = 0; shift < 64; shift += elementBits)
            repeat |= (uint64)1 << shift;
        char text[80];
        snprintf(text, sizeof(text), "mov rdx, 0x%llx", (unsigned long long)repeat);
        line(text);
        line("imul rax, rdx");
    }
    if (destBits == 128)
    {
        storePiece(destination, 0, SCRATCH_A, 64);
        storePiece(destination, 8, SCRATCH_A, 64);
        return;
    }
    if (destBits < 64)
    {
        char text[80];
        snprintf(text, sizeof(text), "mov rdx, 0x%llx",
                 (unsigned long long)getMaskOfBits(destBits));
        line(text);
        line("and rax, rdx");
    }
    storePiece(destination, 0, SCRATCH_A, destBits);
}

void amd64lowering::BlockLowering::lowerWideLogic(
                                    const SemanticInstruction& instruction)
{
    // d = s0 <op> s1 (NOT: d = ~s0). A vector of up to 64 bits is one piece;
    // a 128 bit value is done one qword at a time: the qword of d is written
    // after the qwords of the sources it reads
    const SemanticOperation::Operation operation = instruction.getOperation();
    const bool isUnary = (operation == SemanticOperation::OP_NOT);
    const SemanticOperand& destination = instruction.getDestination();
    const SemanticOperand& first = instruction.getSource(0);
    const SemanticOperand* second = isUnary ? NULL : &instruction.getSource(1);
    const uint bits = getWideBits(destination.getType());
    if (getWideBits(first.getType()) != bits)
        fail("the logic of values of other sizes is not implemented");
    if ((second != NULL) && (getWideBits(second->getType()) != bits))
        fail("the logic of values of other sizes is not implemented");
    if (bits <= 64)
    {
        loadPiece(SCRATCH_A, first, 0, bits);
        if (second != NULL)
            loadPiece(SCRATCH_C, *second, 0, bits);
        switch (operation)
        {
        case SemanticOperation::OP_AND:
            line("and rax, rcx");
            break;
        case SemanticOperation::OP_OR:
            line("or rax, rcx");
            break;
        case SemanticOperation::OP_XOR:
            line("xor rax, rcx");
            break;
        case SemanticOperation::OP_NOT:
            line("not rax");
            break;
        default:
            line("not rax");
            line("and rax, rcx");
            break;
        }
        // (the complement has the bits above the size: they are cleared)
        if (bits < 64)
        {
            char text[80];
            snprintf(text, sizeof(text), "mov rdx, 0x%llx",
                     (unsigned long long)getMaskOfBits(bits));
            line(text);
            line("and rax, rdx");
        }
        storePiece(destination, 0, SCRATCH_A, bits);
        return;
    }
    if (bits != 128)
        fail("the logic of values of other sizes is not implemented");
    for (uint extra = 0; extra < 16; extra += 8)
    {
        loadPiece(SCRATCH_A, first, extra, 64);
        if (second != NULL)
            loadPiece(SCRATCH_C, *second, extra, 64);
        switch (operation)
        {
        case SemanticOperation::OP_AND:
            line("and rax, rcx");
            break;
        case SemanticOperation::OP_OR:
            line("or rax, rcx");
            break;
        case SemanticOperation::OP_XOR:
            line("xor rax, rcx");
            break;
        case SemanticOperation::OP_NOT:
            line("not rax");
            break;
        default:
            // d = ~s0 & s1
            line("not rax");
            line("and rax, rcx");
            break;
        }
        storePiece(destination, extra, SCRATCH_A, 64);
    }
}

void amd64lowering::BlockLowering::lowerWideFloatLoad(
                                    const SemanticInstruction& instruction)
{
    // The bits of the float are loaded as the integer of its size
    const SemanticOperand& destination = instruction.getDestination();
    const uint bits = getWideBits(destination.getType());
    if ((bits != 8) && (bits != 16) && (bits != 32) && (bits != 64))
        fail("the float load of this size is not implemented");
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    char text[100];
    snprintf(text, sizeof(text), "mov %s, %s [%s+rax]", getRegister(SCRATCH_A, bits),
             getSizeKeyword(bits), m_base.getASCIIstring().getBuffer());
    line(text);
    storePiece(destination, 0, SCRATCH_A, bits);
}

void amd64lowering::BlockLowering::lowerWideFloatStore(
                                    const SemanticInstruction& instruction)
{
    // The bits of the float are stored as the integer of its size
    const SemanticOperand& value = instruction.getSource(1);
    const uint bits = getWideBits(instruction.getType());
    if ((bits != 8) && (bits != 16) && (bits != 32) && (bits != 64))
        fail("the float store of this size is not implemented");
    if (getWideBits(value.getType()) != bits)
        fail("the stored value is not the size of the store");
    load(SCRATCH_A, instruction.getSource(0), EXTEND_ZERO);
    loadPiece(SCRATCH_C, value, 0, bits);
    char text[100];
    snprintf(text, sizeof(text), "mov %s [%s+rax], %s", getSizeKeyword(bits),
             m_base.getASCIIstring().getBuffer(), getRegister(SCRATCH_C, bits));
    line(text);
}

bool amd64lowering::BlockLowering::lowerWide(const SemanticInstruction& instruction)
{
    const SemanticOperation::Operation operation = instruction.getOperation();
    const SemanticOperand& destination = instruction.getDestination();
    switch (operation)
    {
    case SemanticOperation::OP_MOVE:
    case SemanticOperation::OP_ZERO_EXTEND:
    case SemanticOperation::OP_TRUNCATE:
    {
        const SemanticOperand& source = instruction.getSource(0);
        const bool isWideDestination = (destination.getType().getTotalBits() == 128);
        const bool isWideSource = (source.getType().getTotalBits() == 128);
        // A narrow float is moved as its bits (the integer of its size)
        if ((operation == SemanticOperation::OP_MOVE) && !isWideDestination &&
            !isWideSource &&
            ((destination.getType().getKind() == SemanticType::KIND_FLOAT) ||
             (source.getType().getKind() == SemanticType::KIND_FLOAT)))
        {
            const uint bits = getWideBits(destination.getType());
            if (getWideBits(source.getType()) != bits)
                fail("a move between values of other sizes");
            copySlot(destination, source, bits);
            return true;
        }
        // The narrow moves are the other lowerings'
        if (!isWideDestination && !isWideSource)
            return false;
        if (operation == SemanticOperation::OP_ZERO_EXTEND &&
            (destination.getType().isVector() || source.getType().isVector()))
        {
            // The interpreter extends the lanes of a vector one by one (v2i32
            // to v2i64 is not the whole value): not implemented
            fail("the zero extension of vectors is not implemented");
        }
        if (isWideDestination && isWideSource &&
            (operation != SemanticOperation::OP_MOVE))
        {
            // A truncation or an extension of 128 bits to 128 bits is the
            // value itself
            copySlot(destination, source, 128);
        } else if (operation == SemanticOperation::OP_MOVE)
        {
            if (!isWideDestination || !isWideSource)
                fail("a move between a wide value and a narrow one");
            copySlot(destination, source, 128);
        } else if (operation == SemanticOperation::OP_ZERO_EXTEND)
        {
            if (!isWideDestination)
                fail("a zero extension from 128 bits to a narrow value is not valid");
            // The value, then the zero high qword
            loadPiece(SCRATCH_A, source, 0, getWideBits(source.getType()));
            storePiece(destination, 0, SCRATCH_A, 64);
            cString zero("mov ");
            zero += getSlotMemory(destination, 8, 64);
            zero += cString(", 0");
            line(zero);
        } else
        {
            if (isWideDestination || !isWideSource)
                fail("a truncation from a narrow value to 128 bits is not valid");
            // The low bits of the value are at the start of its slot
            const uint bits = getWideBits(destination.getType());
            loadPiece(SCRATCH_A, source, 0, bits);
            storePiece(destination, 0, SCRATCH_A, bits);
        }
        return true;
    }
    case SemanticOperation::OP_LOAD:
        if (destination.getType().getTotalBits() == 128)
        {
            lowerWideLoad(instruction);
            return true;
        }
        if (destination.getType().getKind() == SemanticType::KIND_FLOAT)
        {
            lowerWideFloatLoad(instruction);
            return true;
        }
        return false;
    case SemanticOperation::OP_STORE:
        if (instruction.getType().getTotalBits() == 128)
        {
            lowerWideStore(instruction);
            return true;
        }
        if (instruction.getType().getKind() == SemanticType::KIND_FLOAT)
        {
            lowerWideFloatStore(instruction);
            return true;
        }
        return false;
    case SemanticOperation::OP_BITCAST:
    {
        // The same number of bits, the bits are copied as they are
        const uint bits = getWideBits(destination.getType());
        if (bits != getWideBits(instruction.getSource(0).getType()))
            fail("the bit cast of values of other sizes");
        copySlot(destination, instruction.getSource(0), bits);
        return true;
    }
    case SemanticOperation::OP_CONCAT:
        lowerWideConcat(instruction);
        return true;
    case SemanticOperation::OP_EXTRACT:
        lowerWideExtract(instruction);
        return true;
    case SemanticOperation::OP_INSERT:
        lowerWideInsert(instruction);
        return true;
    case SemanticOperation::OP_BROADCAST:
        lowerWideBroadcast(instruction);
        return true;
    case SemanticOperation::OP_AND:
    case SemanticOperation::OP_OR:
    case SemanticOperation::OP_XOR:
    case SemanticOperation::OP_NOT:
    case SemanticOperation::OP_AND_NOT:
        // The narrow scalar logic is the other lowerings'; the vectors are
        // done here (their bits, in one piece or by qwords)
        if ((destination.getType().getTotalBits() != 128) &&
            !destination.getType().isVector())
            return false;
        lowerWideLogic(instruction);
        return true;
    default:
        return false;
    }
}
