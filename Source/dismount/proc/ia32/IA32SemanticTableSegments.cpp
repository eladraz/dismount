/*
 * IA32SemanticTableSegments.cpp
 *
 * The semantics of the IA-32 segment registers, far branches and descriptor
 * tables, see IA32Semantics.h.
 *
 * Real mode: a segment load sets base = selector * 16, nothing is checked.
 * Protected mode: the descriptor is read from the GDT / LDT (the registers
 * IA32_REG_GDTR_BASE...) and checked as the Intel SDM describes (MOV to Sreg,
 * JMP / CALL / RET far, IRET), raising #GP (13), #SS (12) or #NP (11). The CPL
 * is the RPL of cs. The accessed bit of the descriptor is set when it is
 * loaded. The error codes of the faults aren't given (OP_TRAP has the vector
 * only), and the traps push no frame.
 *
 * Not lifted (OP_UNDEFINED): the far branches to a gate or a task segment, to
 * a 16 bit code segment (the mode changes), a return to an outer privilege
 * level, a return to virtual 8086 mode.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/ia32/IA32Opcode.h"
#include "dismount/proc/ia32/IA32Architecture.h"
#include "dismount/proc/ia32/IA32OperandDecoder.h"
#include "dismount/proc/ia32/IA32Semantics.h"

using namespace ia32dis;

typedef SemanticOperation Op;
typedef OpcodeClassification Cl;
typedef IA32LiftContext Context;

namespace {

// The exception vectors
const uint VECTOR_INVALID_OPCODE = 6;
const uint VECTOR_NOT_PRESENT = 11;
const uint VECTOR_STACK_FAULT = 12;
const uint VECTOR_GENERAL_PROTECTION = 13;

// The bits of the second dword of a descriptor
const uint32 DESCRIPTOR_ACCESSED   = 1 << 8;
const uint32 DESCRIPTOR_WRITABLE   = 1 << 9;   // data segments
const uint32 DESCRIPTOR_READABLE   = 1 << 9;   // code segments
const uint32 DESCRIPTOR_CONFORMING = 1 << 10;
const uint32 DESCRIPTOR_CODE       = 1 << 11;
const uint32 DESCRIPTOR_NOT_SYSTEM = 1 << 12;
const uint32 DESCRIPTOR_PRESENT    = 1 << 15;
const uint32 DESCRIPTOR_LONG       = 1 << 21;   // code segments, 64 bit mode
const uint32 DESCRIPTOR_DEFAULT_32 = 1 << 22;
const uint32 DESCRIPTOR_GRANULAR   = 1 << 23;

SemanticType integerType(uint bits)
{
    return SemanticType::integer(bits);
}

SemanticOperand immediate(Context& c, int64 value, uint bits)
{
    return c.getImmediate(value, integerType(bits));
}

// Return true for the protected mode (the 32 bit processor, and 64 bit mode)
bool isProtected(Context& c)
{
    return c.getInstruction().getType() != OpcodeSubsystems::DISASSEMBLER_INTEL_16;
}

uint operandBitsOf(Context& c)
{
    return IA32OperandDecoder::getOperandBits(c.getInstruction());
}

// Return the stack pointer register: rsp in 64 bit mode, esp in protected
// mode, sp in real mode
SemanticOperand stackPointerOf(Context& c)
{
    return c.getStackPointer();
}

// Return the integer 'value' in 'bits' bits: truncated or zero extended
SemanticOperand resize(Context& c, const SemanticOperand& value, uint bits)
{
    const uint valueBits = value.getType().getTotalBits();
    if (valueBits == bits)
        return value;
    return c.emit(valueBits > bits ? Op::OP_TRUNCATE : Op::OP_ZERO_EXTEND,
                  integerType(bits), value);
}

// Return the value in a temporary, so a later write to the register doesn't
// change it
SemanticOperand snapshot(Context& c, const SemanticOperand& value)
{
    if (value.getKind() != SemanticOperand::KIND_REGISTER)
        return value;
    return c.emit(Op::OP_MOVE, value.getType(), value);
}

// Return a 1 bit temporary: 'first' <condition> 'second'
SemanticOperand compare(Context& c, Op::Condition condition,
                        const SemanticOperand& first,
                        const SemanticOperand& second)
{
    SemanticOperand result = c.newTemporary(integerType(1));
    SemanticInstruction instruction(Op::OP_COMPARE, first.getType(), result,
                                    first, second);
    instruction.setCondition(condition);
    c.emit(instruction);
    return result;
}

// Return the boolean 'first' <operation> 'second' (OP_AND / OP_OR)
SemanticOperand logic(Context& c, Op::Operation operation,
                      const SemanticOperand& first, const SemanticOperand& second)
{
    return c.emit(operation, integerType(1), first, second);
}

SemanticOperand notOf(Context& c, const SemanticOperand& value)
{
    return c.emit(Op::OP_XOR, integerType(1), value, immediate(c, 1, 1));
}

// Return true if any of the 'mask' bits of the 32 bit 'value' is set
SemanticOperand isSet(Context& c, const SemanticOperand& value, uint32 mask)
{
    return compare(c, Op::COND_NOT_EQUAL,
                   c.emit(Op::OP_AND, integerType(32), value,
                          immediate(c, mask, 32)),
                   immediate(c, 0, 32));
}

// Raise the exception 'number' if 'condition' is true
void trapIf(Context& c, const SemanticOperand& condition, uint number)
{
    SemanticOperand skip = c.newLabel();
    c.branchIf(notOf(c, condition), skip);
    c.trap(Op::TRAP_FAULT, number);
    c.placeLabel(skip);
}

void emitUndefined(Context& c)
{
    c.emit(SemanticInstruction(Op::OP_UNDEFINED, SemanticType(),
                               SemanticOperand()));
}

// Return the segment base register of a segment register
uint baseRegisterOf(uint segment)
{
    return IA32_REG_ES_BASE + (segment - IA32_REG_ES);
}

// The CPL: the RPL of cs (32 bit)
SemanticOperand getCpl(Context& c)
{
    return c.emit(Op::OP_AND, integerType(32),
                  resize(c, c.getRegister(IA32_REG_CS), 32), immediate(c, 3, 32));
}

/////////////////////////////////////////////////////////////////////////////
// Descriptors
/////////////////////////////////////////////////////////////////////////////

// A descriptor of the GDT / LDT: its address and its two dwords
struct Descriptor {
    SemanticOperand m_address;
    SemanticOperand m_low;
    SemanticOperand m_high;
};

// Read the descriptor of 'selector' (32 bit). A selector beyond the limit of
// its table is #GP. The tables are addressed in the native size
Descriptor fetchDescriptor(Context& c, const SemanticOperand& selector)
{
    const SemanticType type = integerType(32);
    const SemanticType native = c.getNativeType();
    const uint nativeBits = native.getTotalBits();
    SemanticOperand index = c.emit(Op::OP_AND, type, selector,
                                   immediate(c, 0xFFF8, 32));
    SemanticOperand isLocal = isSet(c, selector, 4);
    SemanticOperand tableBase = c.emit(Op::OP_SELECT, native, isLocal,
                    c.getRegister(IA32_REG_LDTR_BASE),
                    c.getRegister(IA32_REG_GDTR_BASE));
    SemanticOperand limit = c.emit(Op::OP_SELECT, type, isLocal,
                    resize(c, c.getRegister(IA32_REG_LDTR_LIMIT), 32),
                    resize(c, c.getRegister(IA32_REG_GDTR_LIMIT), 32));
    trapIf(c, compare(c, Op::COND_UNSIGNED_GREATER,
                      c.emit(Op::OP_ADD, type, index, immediate(c, 7, 32)),
                      limit), VECTOR_GENERAL_PROTECTION);

    Descriptor result;
    result.m_address = c.emit(Op::OP_ADD, native, tableBase,
                              resize(c, index, nativeBits));
    result.m_low = c.load(result.m_address, type);
    result.m_high = c.load(c.emit(Op::OP_ADD, native, result.m_address,
                                  immediate(c, 4, nativeBits)), type);
    return result;
}

// Return the base address of a descriptor
SemanticOperand getDescriptorBase(Context& c, const Descriptor& descriptor)
{
    const SemanticType type = integerType(32);
    SemanticOperand low = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type,
                                 descriptor.m_low, immediate(c, 16, 32));
    SemanticOperand middle = c.emit(Op::OP_SHIFT_LEFT, type,
            c.emit(Op::OP_AND, type, descriptor.m_high, immediate(c, 0xFF, 32)),
            immediate(c, 16, 32));
    SemanticOperand high = c.emit(Op::OP_AND, type, descriptor.m_high,
                                  immediate(c, 0xFF000000u, 32));
    return c.emit(Op::OP_OR, type, c.emit(Op::OP_OR, type, low, middle), high);
}

// Return the limit of a descriptor in bytes
SemanticOperand getDescriptorLimit(Context& c, const Descriptor& descriptor)
{
    const SemanticType type = integerType(32);
    SemanticOperand raw = c.emit(Op::OP_OR, type,
            c.emit(Op::OP_AND, type, descriptor.m_low, immediate(c, 0xFFFF, 32)),
            c.emit(Op::OP_AND, type, descriptor.m_high, immediate(c, 0xF0000, 32)));
    SemanticOperand pages = c.emit(Op::OP_OR, type,
            c.emit(Op::OP_SHIFT_LEFT, type, raw, immediate(c, 12, 32)),
            immediate(c, 0xFFF, 32));
    return c.emit(Op::OP_SELECT, type,
                  isSet(c, descriptor.m_high, DESCRIPTOR_GRANULAR), pages, raw);
}

// Return the DPL of a descriptor (32 bit)
SemanticOperand getDescriptorDpl(Context& c, const Descriptor& descriptor)
{
    const SemanticType type = integerType(32);
    return c.emit(Op::OP_AND, type,
                  c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, descriptor.m_high,
                         immediate(c, 13, 32)),
                  immediate(c, 3, 32));
}

// Set the accessed bit of a descriptor in memory, if it isn't set
void setAccessed(Context& c, const Descriptor& descriptor)
{
    const SemanticType type = integerType(32);
    const SemanticType native = descriptor.m_address.getType();
    SemanticOperand done = c.newLabel();
    c.branchIf(isSet(c, descriptor.m_high, DESCRIPTOR_ACCESSED), done);
    c.store(c.emit(Op::OP_ADD, native, descriptor.m_address,
                   immediate(c, 4, native.getTotalBits())),
            c.emit(Op::OP_OR, type, descriptor.m_high,
                   immediate(c, DESCRIPTOR_ACCESSED, 32)));
    c.placeLabel(done);
}

/////////////////////////////////////////////////////////////////////////////
// Loading the segment registers
/////////////////////////////////////////////////////////////////////////////

// Load segment register 'segment' (IA32_REG_ES..GS, not cs) from the 16 bit
// selector. In 64 bit mode the bases of es, ss and ds are ignored (0), fs and
// gs get the 32 bit base of the descriptor (a null selector: 0), and a null
// ss is allowed at CPL 0 to 2
void loadSegmentRegister(Context& c, uint segment, const SemanticOperand& selector16)
{
    const SemanticType type = integerType(32);
    const bool is64 = c.is64BitMode();
    const uint baseRegister = baseRegisterOf(segment);
    const bool hasBase = !is64 || (segment == IA32_REG_FS) || (segment == IA32_REG_GS);
    const uint baseBits = c.getRegister(baseRegister).getType().getTotalBits();
    SemanticOperand selector = snapshot(c, selector16);

    if (!isProtected(c))
    {
        // Real mode: base = selector * 16
        c.move(c.getRegister(baseRegister),
               c.emit(Op::OP_SHIFT_LEFT, type, resize(c, selector, 32),
                      immediate(c, 4, 32)));
        c.move(c.getRegister(segment), selector);
        return;
    }

    const bool isStack = (segment == IA32_REG_SS);
    SemanticOperand selector32 = resize(c, selector, 32);
    SemanticOperand isNull = compare(c, Op::COND_EQUAL,
            c.emit(Op::OP_AND, type, selector32, immediate(c, 0xFFFC, 32)),
            immediate(c, 0, 32));
    SemanticOperand nullLabel = c.newLabel();
    SemanticOperand doneLabel = c.newLabel();
    if (isStack && is64)
    {
        SemanticOperand notNull = c.newLabel();
        c.branchIf(notOf(c, isNull), notNull);
        trapIf(c, compare(c, Op::COND_EQUAL, getCpl(c), immediate(c, 3, 32)),
               VECTOR_GENERAL_PROTECTION);
        c.move(c.getRegister(segment), selector);
        c.branch(doneLabel);
        c.placeLabel(notNull);
    } else if (isStack)
    {
        trapIf(c, isNull, VECTOR_GENERAL_PROTECTION);
    } else
    {
        c.branchIf(isNull, nullLabel);
    }

    Descriptor descriptor = fetchDescriptor(c, selector32);
    SemanticOperand rpl = c.emit(Op::OP_AND, type, selector32, immediate(c, 3, 32));
    SemanticOperand cpl = getCpl(c);
    SemanticOperand dpl = getDescriptorDpl(c, descriptor);
    SemanticOperand isCode = isSet(c, descriptor.m_high, DESCRIPTOR_CODE);

    // A system descriptor can't be loaded
    trapIf(c, notOf(c, isSet(c, descriptor.m_high, DESCRIPTOR_NOT_SYSTEM)),
           VECTOR_GENERAL_PROTECTION);
    if (isStack)
    {
        // A writable data segment of the CPL
        SemanticOperand notWritable = notOf(c,
                    isSet(c, descriptor.m_high, DESCRIPTOR_WRITABLE));
        trapIf(c, logic(c, Op::OP_OR, isCode, notWritable),
               VECTOR_GENERAL_PROTECTION);
        trapIf(c, compare(c, Op::COND_NOT_EQUAL, rpl, cpl),
               VECTOR_GENERAL_PROTECTION);
        trapIf(c, compare(c, Op::COND_NOT_EQUAL, dpl, cpl),
               VECTOR_GENERAL_PROTECTION);
    } else
    {
        // A data segment or a readable code segment
        SemanticOperand notReadable = notOf(c,
                    isSet(c, descriptor.m_high, DESCRIPTOR_READABLE));
        trapIf(c, logic(c, Op::OP_AND, isCode, notReadable),
               VECTOR_GENERAL_PROTECTION);
        // The privilege is checked, unless it's a conforming code segment
        SemanticOperand isConformingCode = logic(c, Op::OP_AND, isCode,
                    isSet(c, descriptor.m_high, DESCRIPTOR_CONFORMING));
        SemanticOperand isDenied = logic(c, Op::OP_OR,
                    compare(c, Op::COND_UNSIGNED_LESS, dpl, cpl),
                    compare(c, Op::COND_UNSIGNED_LESS, dpl, rpl));
        trapIf(c, logic(c, Op::OP_AND, notOf(c, isConformingCode), isDenied),
               VECTOR_GENERAL_PROTECTION);
    }
    trapIf(c, notOf(c, isSet(c, descriptor.m_high, DESCRIPTOR_PRESENT)),
           isStack ? VECTOR_STACK_FAULT : VECTOR_NOT_PRESENT);

    setAccessed(c, descriptor);
    if (hasBase)
        c.move(c.getRegister(baseRegister),
               resize(c, getDescriptorBase(c, descriptor), baseBits));
    c.move(c.getRegister(segment), selector);
    if (isStack)
    {
        if (is64)
            c.placeLabel(doneLabel);
        return;
    }
    c.branch(doneLabel);

    // A null selector: loaded, the accesses through it would fault
    c.placeLabel(nullLabel);
    if (hasBase)
        c.move(c.getRegister(baseRegister), immediate(c, 0, baseBits));
    c.move(c.getRegister(segment), selector);
    c.placeLabel(doneLabel);
}

// Check the code segment 'selector32' for a far transfer to it (jmp, call,
// ret, iret) and load cs and its base. The privilege rules:
//   isReturn - the selector comes from a return: the RPL gives the new CPL,
//              and a return to an outer level isn't lifted
//   otherwise- the code segment of the same privilege (conforming: DPL <= CPL;
//              non conforming: DPL == CPL and RPL <= CPL)
// The system descriptors (gates, task segments) and the 16 bit code segments
// aren't lifted: they go to 'notLifted'. The traps come before any change of
// the state. 'beforeLoad' is called after the checks, to push the return
// frame; the offset is checked against the limit of the segment.
struct FarTarget {
    Descriptor m_descriptor;
    SemanticOperand m_selector;
};

FarTarget checkFarTarget(Context& c, const SemanticOperand& selector32,
                         const SemanticOperand& offset, bool isReturn,
                         bool checkLimit, const SemanticOperand& notLifted)
{
    const SemanticType type = integerType(32);
    const bool is64 = c.is64BitMode();
    trapIf(c, compare(c, Op::COND_EQUAL,
                c.emit(Op::OP_AND, type, selector32, immediate(c, 0xFFFC, 32)),
                immediate(c, 0, 32)), VECTOR_GENERAL_PROTECTION);

    FarTarget target;
    target.m_selector = selector32;
    target.m_descriptor = fetchDescriptor(c, selector32);
    const Descriptor& descriptor = target.m_descriptor;
    SemanticOperand cpl = getCpl(c);
    SemanticOperand rpl = c.emit(Op::OP_AND, type, selector32, immediate(c, 3, 32));
    SemanticOperand dpl = getDescriptorDpl(c, descriptor);

    SemanticOperand isSystem = notOf(c,
                    isSet(c, descriptor.m_high, DESCRIPTOR_NOT_SYSTEM));
    if (!isReturn)
        c.branchIf(isSystem, notLifted);
    else
        trapIf(c, isSystem, VECTOR_GENERAL_PROTECTION);
    trapIf(c, notOf(c, isSet(c, descriptor.m_high, DESCRIPTOR_CODE)),
           VECTOR_GENERAL_PROTECTION);

    SemanticOperand isConforming = isSet(c, descriptor.m_high, DESCRIPTOR_CONFORMING);
    SemanticOperand isDenied;
    if (isReturn)
    {
        // RPL >= CPL, conforming: DPL <= RPL, otherwise DPL == RPL
        SemanticOperand conformingDenied = compare(c, Op::COND_UNSIGNED_GREATER,
                                                   dpl, rpl);
        SemanticOperand otherDenied = compare(c, Op::COND_NOT_EQUAL, dpl, rpl);
        isDenied = logic(c, Op::OP_OR,
                    compare(c, Op::COND_UNSIGNED_LESS, rpl, cpl),
                    c.emit(Op::OP_SELECT, integerType(1), isConforming,
                           conformingDenied, otherDenied));
    } else
    {
        SemanticOperand conformingDenied = compare(c, Op::COND_UNSIGNED_GREATER,
                                                   dpl, cpl);
        SemanticOperand otherDenied = logic(c, Op::OP_OR,
                    compare(c, Op::COND_UNSIGNED_GREATER, rpl, cpl),
                    compare(c, Op::COND_NOT_EQUAL, dpl, cpl));
        isDenied = c.emit(Op::OP_SELECT, integerType(1), isConforming,
                          conformingDenied, otherDenied);
    }
    trapIf(c, isDenied, VECTOR_GENERAL_PROTECTION);
    trapIf(c, notOf(c, isSet(c, descriptor.m_high, DESCRIPTOR_PRESENT)),
           VECTOR_NOT_PRESENT);

    if (isReturn)
    {
        // A return to an outer privilege level switches the stack
        c.branchIf(compare(c, Op::COND_UNSIGNED_GREATER, rpl, cpl), notLifted);
    }
    SemanticOperand isDefault32 = isSet(c, descriptor.m_high, DESCRIPTOR_DEFAULT_32);
    SemanticOperand isLong;
    if (is64)
    {
        // A 64 bit code segment has no default size (L and D together are
        // #GP). A 16 bit one changes the mode
        isLong = isSet(c, descriptor.m_high, DESCRIPTOR_LONG);
        trapIf(c, logic(c, Op::OP_AND, isLong, isDefault32),
               VECTOR_GENERAL_PROTECTION);
        c.branchIf(logic(c, Op::OP_AND, notOf(c, isLong), notOf(c, isDefault32)),
                   notLifted);
    } else
    {
        // A 16 bit code segment changes the mode
        c.branchIf(notOf(c, isDefault32), notLifted);
    }
    if (checkLimit)
    {
        // The limit of a 64 bit code segment is not checked
        SemanticOperand isOver = compare(c, Op::COND_UNSIGNED_GREATER, offset,
                resize(c, getDescriptorLimit(c, descriptor),
                       offset.getType().getTotalBits()));
        if (is64)
            isOver = logic(c, Op::OP_AND, notOf(c, isLong), isOver);
        trapIf(c, isOver, VECTOR_GENERAL_PROTECTION);
    }
    return target;
}

// Load cs from a checked far target. The RPL of cs is the CPL for a branch,
// and the RPL of the selector for a return. In 64 bit mode the base of cs
// is ignored
void loadCodeSegment(Context& c, const FarTarget& target, bool isReturn)
{
    const SemanticType type = integerType(32);
    setAccessed(c, target.m_descriptor);
    SemanticOperand selector;
    if (isReturn)
        selector = resize(c, target.m_selector, 16);
    else
        selector = resize(c, c.emit(Op::OP_OR, type,
                    c.emit(Op::OP_AND, type, target.m_selector,
                           immediate(c, 0xFFFC, 32)), getCpl(c)), 16);
    if (!c.is64BitMode())
        c.move(c.getRegister(IA32_REG_CS_BASE),
               getDescriptorBase(c, target.m_descriptor));
    c.move(c.getRegister(IA32_REG_CS), selector);
}

// Load cs in real mode
void loadCodeSegmentReal(Context& c, const SemanticOperand& selector16)
{
    SemanticOperand selector = snapshot(c, selector16);
    c.move(c.getRegister(IA32_REG_CS_BASE),
           c.emit(Op::OP_SHIFT_LEFT, integerType(32), resize(c, selector, 32),
                  immediate(c, 4, 32)));
    c.move(c.getRegister(IA32_REG_CS), selector);
}

/////////////////////////////////////////////////////////////////////////////
// The instructions
/////////////////////////////////////////////////////////////////////////////

// mov sreg, r/m16 and mov r/m, sreg
void liftMove(Context& c)
{
    const IA32DecodedOperand& destination = c.getOperand(0);
    if ((destination.m_kind == IA32DecodedOperand::KIND_REGISTER) &&
        (destination.m_register >= IA32_REG_ES) &&
        (destination.m_register <= IA32_REG_GS))
    {
        // Loading cs is invalid
        if (destination.m_register == IA32_REG_CS)
        {
            c.trap(Op::TRAP_FAULT, VECTOR_INVALID_OPCODE);
            return;
        }
        loadSegmentRegister(c, destination.m_register,
                            c.read(1, integerType(16)));
        return;
    }

    // The selector is zero extended into a register of the operand size
    SemanticOperand value = c.read(1, integerType(16));
    c.write(0, resize(c, value, destination.m_bits));
}

// pop es, ss, ds, fs, gs
void liftPopSegment(Context& c)
{
    const SemanticType type = c.getOperandSizeType();
    const SemanticType stackType = c.getStackAddressType();
    SemanticOperand stackPointer = stackPointerOf(c);
    SemanticOperand value = c.load(c.getLinearAddress(IA32_REG_SS, stackPointer),
                                   type);
    loadSegmentRegister(c, c.getOperand(0).m_register, resize(c, value, 16));
    c.move(stackPointer, c.emit(Op::OP_ADD, stackType, stackPointer,
                    immediate(c, type.getTotalBits() / 8, stackType.getTotalBits())));
}

// lds, les, lss, lfs, lgs: the offset and then the selector are read from
// memory, the segment register is loaded, and then the register
template <int SEGMENT>
void liftLoadFarPointer(Context& c)
{
    // The register form is invalid
    if (c.getOperand(1).m_kind != IA32DecodedOperand::KIND_MEMORY)
    {
        c.trap(Op::TRAP_FAULT, VECTOR_INVALID_OPCODE);
        return;
    }
    const uint bits = c.getOperand(0).m_bits;
    SemanticOperand address = c.getAddress(1);
    const uint addressBits = address.getType().getTotalBits();
    SemanticOperand offset = c.load(address, integerType(bits));
    SemanticOperand selector = c.load(c.emit(Op::OP_ADD, address.getType(), address,
                                             immediate(c, bits / 8, addressBits)),
                                      integerType(16));
    loadSegmentRegister(c, SEGMENT, selector);
    c.write(0, offset);
}

// Push the return address (cs and the address of the next instruction) of a
// far call, in the operand size
void pushReturnFrame(Context& c)
{
    const uint bits = operandBitsOf(c);
    c.push(resize(c, c.getRegister(IA32_REG_CS), bits));
    c.push(SemanticOperand::guestAddress(
                IA32OperandDecoder::getNextAddress(c.getInstruction()),
                integerType(bits)));
}

// jmp / call with a far pointer: ptr16:16/32 immediate, or m16:16/32/64
template <bool IS_CALL>
void liftFarBranch(Context& c)
{
    const IA32DecodedOperand& operand = c.getOperand(0);
    const uint bits = operandBitsOf(c);
    const uint nativeBits = c.getNativeType().getTotalBits();
    SemanticOperand offset;
    SemanticOperand selector;
    if (operand.m_kind == IA32DecodedOperand::KIND_FAR_POINTER)
    {
        offset = immediate(c, operand.m_immediate & ((((int64)1) << bits) - 1), 32);
        selector = immediate(c, operand.m_farSegment, 16);
    } else
    {
        CHECK(operand.m_kind == IA32DecodedOperand::KIND_MEMORY);
        SemanticOperand address = c.getAddress(0);
        offset = resize(c, c.load(address, integerType(bits)), nativeBits);
        selector = c.load(c.emit(Op::OP_ADD, address.getType(), address,
                                 immediate(c, bits / 8,
                                           address.getType().getTotalBits())),
                          integerType(16));
    }
    offset = snapshot(c, offset);

    SemanticOperand notLifted = c.newLabel();
    if (isProtected(c))
    {
        FarTarget target = checkFarTarget(c, resize(c, snapshot(c, selector), 32),
                                          offset, false, true, notLifted);
        if (IS_CALL)
            pushReturnFrame(c);
        loadCodeSegment(c, target, false);
    } else
    {
        if (IS_CALL)
            pushReturnFrame(c);
        loadCodeSegmentReal(c, selector);
    }
    c.emit(SemanticInstruction(IS_CALL ? Op::OP_CALL : Op::OP_BRANCH_INDIRECT,
                               SemanticType(), SemanticOperand(), offset));
    c.placeLabel(notLifted);
    emitUndefined(c);
}

// Restore the modeled bits of EFLAGS from the 'bits' wide image (iret)
void restoreEflags(Context& c, const SemanticOperand& image, uint bits)
{
    static const struct {
        uint m_register;
        uint32 m_flag;
        uint m_bit;
    } flags[] = {
        { IA32_REG_CF, SemanticFlags::FLAG_CARRY,     0 },
        { IA32_REG_PF, SemanticFlags::FLAG_PARITY,    2 },
        { IA32_REG_AF, SemanticFlags::FLAG_ADJUST,    4 },
        { IA32_REG_ZF, SemanticFlags::FLAG_ZERO,      6 },
        { IA32_REG_SF, SemanticFlags::FLAG_SIGN,      7 },
        { IA32_REG_IF, 0,                             9 },
        { IA32_REG_DF, SemanticFlags::FLAG_DIRECTION, 10 },
        { IA32_REG_OF, SemanticFlags::FLAG_OVERFLOW,  11 },
        { IA32_REG_AC, 0,                             18 },
        { IA32_REG_ID, 0,                             21 }
    };
    for (uint i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
    {
        // A 16 bit iret doesn't change the AC and ID bits
        if (flags[i].m_bit >= bits)
            continue;
        SemanticOperand shifted = image;
        if (flags[i].m_bit != 0)
            shifted = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, image.getType(), image,
                             immediate(c, flags[i].m_bit, bits));
        SemanticOperand bit = c.emit(Op::OP_TRUNCATE, integerType(1), shifted);
        if (flags[i].m_flag != 0)
            c.setFlags(flags[i].m_flag, bit);
        else
            c.move(c.getRegister(flags[i].m_register), bit);
    }
}

// The checks of the stack segment that iret loads in 64 bit mode, from the
// frame: a null ss is allowed if the new RPL isn't 3, otherwise the segment
// is a present writable data segment of the new privilege level. Nothing
// changes: the load is done by loadReturnStackSegment. Returns the descriptor
// (valid only if ss isn't null)
Descriptor checkReturnStackSegment(Context& c, const SemanticOperand& ss32,
                                   const SemanticOperand& isNull,
                                   const SemanticOperand& rpl)
{
    const SemanticType type = integerType(32);
    SemanticOperand skip = c.newLabel();
    Descriptor descriptor;
    descriptor.m_address = c.newTemporary(c.getNativeType());
    descriptor.m_low = c.newTemporary(type);
    descriptor.m_high = c.newTemporary(type);
    // A null selector with an RPL of 3 is #GP
    trapIf(c, logic(c, Op::OP_AND, isNull,
                    compare(c, Op::COND_EQUAL, rpl, immediate(c, 3, 32))),
           VECTOR_GENERAL_PROTECTION);
    c.branchIf(isNull, skip);
    trapIf(c, compare(c, Op::COND_NOT_EQUAL,
                      c.emit(Op::OP_AND, type, ss32, immediate(c, 3, 32)), rpl),
           VECTOR_GENERAL_PROTECTION);
    Descriptor fetched = fetchDescriptor(c, ss32);
    c.move(descriptor.m_address, fetched.m_address);
    c.move(descriptor.m_low, fetched.m_low);
    c.move(descriptor.m_high, fetched.m_high);
    trapIf(c, notOf(c, isSet(c, fetched.m_high, DESCRIPTOR_NOT_SYSTEM)),
           VECTOR_GENERAL_PROTECTION);
    trapIf(c, logic(c, Op::OP_OR, isSet(c, fetched.m_high, DESCRIPTOR_CODE),
                    notOf(c, isSet(c, fetched.m_high, DESCRIPTOR_WRITABLE))),
           VECTOR_GENERAL_PROTECTION);
    trapIf(c, compare(c, Op::COND_NOT_EQUAL, getDescriptorDpl(c, fetched), rpl),
           VECTOR_GENERAL_PROTECTION);
    trapIf(c, notOf(c, isSet(c, fetched.m_high, DESCRIPTOR_PRESENT)),
           VECTOR_NOT_PRESENT);
    c.placeLabel(skip);
    return descriptor;
}

// retf, retf imm16 and iret: pop the offset, cs (and the flags), and
// discard the immediate. In 64 bit mode iret pops rip, cs, rflags, rsp and ss
// in any case; the target is a 64 bit code segment or a 32 bit one, a
// return to a 16 bit one, to an outer privilege level or with a 16 bit operand
// size is not lifted
template <bool IS_IRET>
void liftFarReturn(Context& c)
{
    const bool is64 = c.is64BitMode();
    const uint bits = operandBitsOf(c);
    if (is64 && (bits == 16))
    {
        emitUndefined(c);
        return;
    }
    const uint bytes = bits / 8;
    const SemanticType type = integerType(bits);
    const SemanticType stackType = c.getStackAddressType();
    const uint nativeBits = c.getNativeType().getTotalBits();
    SemanticOperand stackPointer = stackPointerOf(c);

    // The frame: offset, cs, (flags, rsp, ss)
    SemanticOperand offset = c.load(c.getLinearAddress(IA32_REG_SS, stackPointer), type);
    SemanticOperand selectorAddress = c.getLinearAddress(IA32_REG_SS,
            c.emit(Op::OP_ADD, stackType, stackPointer,
                   immediate(c, bytes, stackType.getTotalBits())));
    SemanticOperand selector = c.load(selectorAddress, type);
    SemanticOperand flags;
    SemanticOperand newStack;
    SemanticOperand newSs;
    if (IS_IRET)
        flags = c.load(c.getLinearAddress(IA32_REG_SS,
                c.emit(Op::OP_ADD, stackType, stackPointer,
                       immediate(c, 2 * bytes, stackType.getTotalBits()))), type);
    if (IS_IRET && is64)
    {
        newStack = c.load(c.getLinearAddress(IA32_REG_SS,
                c.emit(Op::OP_ADD, stackType, stackPointer,
                       immediate(c, 3 * bytes, stackType.getTotalBits()))), type);
        newSs = c.load(c.getLinearAddress(IA32_REG_SS,
                c.emit(Op::OP_ADD, stackType, stackPointer,
                       immediate(c, 4 * bytes, stackType.getTotalBits()))), type);
    }

    // The bytes the instruction takes off the stack
    uint64 total = (IS_IRET ? (is64 ? 5 : 3) : 2) * bytes;
    if (!IS_IRET && (c.getOperandsCount() == 1))
        total += (uint64)(c.getOperand(0).m_immediate & 0xFFFF);
    SemanticOperand newPointer = c.emit(Op::OP_ADD, stackType, stackPointer,
                                        immediate(c, (int64)total,
                                                  stackType.getTotalBits()));
    SemanticOperand offsetNative = snapshot(c, resize(c, offset, nativeBits));
    SemanticOperand selector16 = snapshot(c, resize(c, selector, 16));

    SemanticOperand notLifted = c.newLabel();
    if (isProtected(c))
    {
        // A return to virtual 8086 mode isn't lifted
        if (IS_IRET && (bits == 32) && !is64)
            c.branchIf(isSet(c, snapshot(c, flags), (uint32)1 << 17), notLifted);
        FarTarget target = checkFarTarget(c, resize(c, selector16, 32), offsetNative,
                                          true, false, notLifted);
        if (IS_IRET && is64)
        {
            SemanticOperand ss16 = snapshot(c, resize(c, newSs, 16));
            SemanticOperand ss32 = resize(c, ss16, 32);
            SemanticOperand rpl = c.emit(Op::OP_AND, integerType(32),
                                         resize(c, selector16, 32), immediate(c, 3, 32));
            SemanticOperand isNull = compare(c, Op::COND_EQUAL,
                    c.emit(Op::OP_AND, integerType(32), ss32, immediate(c, 0xFFFC, 32)),
                    immediate(c, 0, 32));
            Descriptor ssDescriptor = checkReturnStackSegment(c, ss32, isNull, rpl);
            loadCodeSegment(c, target, true);
            SemanticOperand skip = c.newLabel();
            c.branchIf(isNull, skip);
            setAccessed(c, ssDescriptor);
            c.placeLabel(skip);
            c.move(c.getRegister(IA32_REG_SS), ss16);
            newPointer = resize(c, snapshot(c, newStack), nativeBits);
        } else
        {
            loadCodeSegment(c, target, true);
        }
    } else
    {
        loadCodeSegmentReal(c, selector16);
    }
    c.move(stackPointer, newPointer);
    if (IS_IRET)
        restoreEflags(c, snapshot(c, flags), bits);
    c.emit(SemanticInstruction(Op::OP_RETURN, SemanticType(), SemanticOperand(),
                               offsetNative));
    c.placeLabel(notLifted);
    emitUndefined(c);
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// The table
/////////////////////////////////////////////////////////////////////////////

namespace ia32dis {

namespace {

const uint8 CAT_MOVE      = Cl::CATEGORY_MOVE;
const uint8 CAT_LOAD      = Cl::CATEGORY_LOAD;
const uint8 CAT_STACK     = Cl::CATEGORY_STACK;
const uint8 CAT_BRANCH    = Cl::CATEGORY_BRANCH;
const uint8 CAT_CALL      = Cl::CATEGORY_CALL;
const uint8 CAT_RETURN    = Cl::CATEGORY_RETURN;
const uint32 U_ALU        = Cl::UNIT_INTEGER_ALU;
const uint32 U_BRANCH     = Cl::UNIT_BRANCH;
const uint32 U_MEMORY     = Cl::UNIT_LOAD | Cl::UNIT_STORE;
const uint32 P_READ       = Cl::PROPERTY_READS_MEMORY;
const uint32 P_WRITE      = Cl::PROPERTY_WRITES_MEMORY;
const uint32 P_STACK      = Cl::PROPERTY_CHANGES_STACK;
const uint32 P_IMPLICIT   = Cl::PROPERTY_IMPLICIT_OPERANDS;
const uint32 P_MODE       = Cl::PROPERTY_CHANGES_MODE;
const uint32 FD           = SemanticFlags::FLAG_DIRECTION;
const uint32 FALL         = SemanticFlags::FLAGS_ARITHMETIC;

} // namespace

// An entry of a hand written lifter
#define LIFTER(name, lift, category, units, properties, read, written, undefined) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, category, units, properties, read, written, undefined }, \
      lift }

const IA32SemanticEntry gIa32SegmentsSemanticTable[] = {
    // Name   Lifter                              Category   Units               Properties                 Flags read, written, undefined
    LIFTER("mov",  liftMove,                      CAT_MOVE,   U_ALU | U_MEMORY,  P_READ | P_WRITE,          0, 0, 0),
    LIFTER("pop",  liftPopSegment,                CAT_STACK,  U_ALU | U_MEMORY,  P_STACK | P_READ,          0, 0, 0),
    LIFTER("les",  liftLoadFarPointer<IA32_REG_ES>, CAT_LOAD, U_ALU | U_MEMORY,  P_READ,                    0, 0, 0),
    LIFTER("lds",  liftLoadFarPointer<IA32_REG_DS>, CAT_LOAD, U_ALU | U_MEMORY,  P_READ,                    0, 0, 0),
    LIFTER("lss",  liftLoadFarPointer<IA32_REG_SS>, CAT_LOAD, U_ALU | U_MEMORY,  P_READ,                    0, 0, 0),
    LIFTER("lfs",  liftLoadFarPointer<IA32_REG_FS>, CAT_LOAD, U_ALU | U_MEMORY,  P_READ,                    0, 0, 0),
    LIFTER("lgs",  liftLoadFarPointer<IA32_REG_GS>, CAT_LOAD, U_ALU | U_MEMORY,  P_READ,                    0, 0, 0),
    LIFTER("jmp",  liftFarBranch<false>,          CAT_BRANCH, U_BRANCH | U_MEMORY, P_READ | P_MODE,         0, 0, 0),
    LIFTER("call", liftFarBranch<true>,           CAT_CALL,   U_BRANCH | U_MEMORY, P_STACK | P_WRITE | P_READ | P_IMPLICIT | P_MODE, 0, 0, 0),
    LIFTER("retf", liftFarReturn<false>,          CAT_RETURN, U_BRANCH | U_MEMORY, P_STACK | P_READ | P_IMPLICIT | P_MODE, 0, 0, 0),
    LIFTER("iret", liftFarReturn<true>,           CAT_RETURN, U_BRANCH | U_MEMORY, P_STACK | P_READ | P_IMPLICIT | P_MODE, 0, FALL | FD, 0),

    // End marker
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }, NULL }
};

#undef LIFTER

} // namespace ia32dis
